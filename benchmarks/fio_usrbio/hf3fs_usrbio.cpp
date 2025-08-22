#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <chrono>
#include <iomanip>
#include <ctime>
#include "hf3fs_usrbio.h"
#include <atomic>
#include <errno.h>
#include <vector>
#include <sys/stat.h>

extern "C" {
#include "fio.h"
#include "optgroup.h"
}

struct hf3fs_usrbio_options {
    int dummy;
    char *mountpoint;
    int ior_depth;
    int ior_timeout;
};

static struct fio_option options[] = {
    {
        .name = "mountpoint",
        .lname = "hf3fs mount point",
        .type = FIO_OPT_STR_STORE,
        .off1 = offsetof(struct hf3fs_usrbio_options, mountpoint),
        .help = "Mount point (e.g. /hf3fs/mount/point)",
        .def = "",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_NETIO,
    },
    {
        .name = "ior_depth",
        .lname = "hf3fs ior depth",
        .type = FIO_OPT_INT,
        .off1 = offsetof(struct hf3fs_usrbio_options, ior_depth),
        .help = "Ior depth",
        .def = "0",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_NETIO,
    },
    {
        .name = "ior_timeout",
        .lname = "hf3fs ior timeout (in ms)",
        .type = FIO_OPT_INT,
        .off1 = offsetof(struct hf3fs_usrbio_options, ior_timeout),
        .help = "Ior timeout",
        .def = "1",
        .category = FIO_OPT_C_ENGINE,
        .group = FIO_OPT_G_NETIO,
    },
    {
        .name = NULL,
    },
};

#define LAST_POS(f) ((f)->engine_pos)

struct hf3fs_usrbio_data {
    struct hf3fs_iov iov;
    struct hf3fs_ior ior_r;
    struct hf3fs_ior ior_w;
    std::vector<struct io_u *> io_us;
    int queued;
    int events;
    enum fio_ddir last_ddir;
};

static int hf3fs_usrbio_init(struct thread_data *td) {
    td->io_ops_data = static_cast<void *>(new hf3fs_usrbio_data);
    struct hf3fs_usrbio_options *options = td->eo;

    auto &ior_r = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->ior_r;
    auto res = hf3fs_iorcreate3(&ior_r, options->mountpoint, td->o.iodepth, true, options->ior_depth, 0, options->ior_timeout, -1);
    if (res < 0) {
        return res;
    }

    auto &ior_w = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->ior_w;
    res = hf3fs_iorcreate(&ior_w, options->mountpoint, td->o.iodepth, false, options->ior_depth, -1);
    if (res < 0) {
        return res;
    }

    auto *data = static_cast<hf3fs_usrbio_data *>(td->io_ops_data);
    data->io_us.resize(td->o.iodepth);
    data->queued = 0;
    data->events = 0;

    return 0;
}

static int fio_io_end(struct thread_data *td, struct io_u *io_u, int ret) {
    if (io_u->file && ret >= 0 && ddir_rw(io_u->ddir)) {
        LAST_POS(io_u->file) = io_u->offset + ret;
    }

    if (ret != (int) io_u->xfer_buflen) {
        if (ret >= 0) {
            io_u->resid = io_u->xfer_buflen - ret;
            io_u->error = 0;
            return FIO_Q_COMPLETED;
        } else {
            io_u->error = errno;
        }
    }

    if (io_u->error) {
        io_u_log_error(td, io_u);
        td_verror(td, io_u->error, "xfer");
    }

    return FIO_Q_COMPLETED;
}

static enum fio_q_status hf3fs_usrbio_queue(struct thread_data *td, struct io_u *io_u) {
    auto &vec = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->io_us;
    auto *sd = static_cast<hf3fs_usrbio_data *>(td->io_ops_data);

    if (io_u->ddir != sd->last_ddir) {
        if (sd->queued != 0) {
            return FIO_Q_BUSY;
        } else {
            vec[sd->queued++] = io_u;
            sd->last_ddir = io_u->ddir;
            return FIO_Q_QUEUED;
        }
    } else {
        if (sd->queued == td->o.iodepth) {
            return FIO_Q_BUSY;
        }
        vec[sd->queued++] = io_u;
        return FIO_Q_QUEUED;
    }
}

static int hf3fs_usrbio_commit(struct thread_data *td) {
    auto &vec = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->io_us;
    auto *sd = static_cast<hf3fs_usrbio_data *>(td->io_ops_data);
    auto &ior_r = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->ior_r;
    auto &ior_w = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->ior_w;
    auto &iov = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->iov;

    if (sd->queued == 0) {
        return 0;
    }

    io_u_mark_submit(td, sd->queued);

    int res = 0;
    bool read = (sd->last_ddir == DDIR_READ);
    auto &ior = read ? ior_r : ior_w;
    for (int i = 0; i < sd->queued; i++) {
        res = hf3fs_prep_io(&ior, &iov, read, vec[i]->xfer_buf, vec[i]->file->fd, vec[i]->offset, vec[i]->xfer_buflen, nullptr);
        if (res < 0) {
            std::cout << "prep " << res << " " << vec[i]->file->fd << std::endl;
            return res;
        }
    }
    res = hf3fs_submit_ios(&ior);
    if (res < 0) {
        std::cout << "submit " << res << std::endl;
        return res;
    }

    std::vector<struct hf3fs_cqe> cqe(sd->queued);
    res = hf3fs_wait_for_ios(&ior, cqe.data(), sd->queued, sd->queued, nullptr);
    if (res < 0) {
        std::cout << "wait " << res << std::endl;
        return res;
    }

    for (int i = 0; i < sd->queued; i++) {
        if (cqe[i].result < 0) {
            std::cout << "cqe error " << res << std::endl;
            return res;
        }
    }

    sd->events = sd->queued;
    sd->queued = 0;

    return 0;
}

static int hf3fs_usrbio_getevents(struct thread_data *td, unsigned int min, unsigned int max, const struct timespec fio_unused *t) {
    auto &vec = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->io_us;
    auto *sd = static_cast<hf3fs_usrbio_data *>(td->io_ops_data);
    int ret = 0;
    if (min) {
        ret = sd->events;
        sd->events = 0;
    }
    return ret;
}

static struct io_u *hf3fs_usrbio_event(struct thread_data *td, int event) {
    auto &vec = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->io_us;
    return vec[event];
}

static void hf3fs_usrbio_cleanup(struct thread_data *td) {
    delete static_cast<hf3fs_usrbio_data *>(td->io_ops_data);
}

static int hf3fs_usrbio_open(struct thread_data *td, struct fio_file *f) {
    int flags = 0;
    if (td_write(td)) {
        if (!read_only) {
            flags = O_RDWR;
        }
    } else if (td_read(td)) {
        if (!read_only) {
            flags = O_RDWR;
        } else {
            flags = O_RDONLY;
        }
    }

    f->fd = open(f->file_name, flags);
    hf3fs_reg_fd(f->fd, 0);
    td->o.open_files++;
    return 0;
}

static int hf3fs_usrbio_close(struct thread_data *td, struct fio_file *f) {
    hf3fs_dereg_fd(f->fd);
    close(f->fd);
    f->fd = -1;
    return 0;
}

static int hf3fs_usrbio_alloc(struct thread_data *td, size_t total_mem) {
    struct hf3fs_usrbio_options *options = td->eo;

    auto &iov = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->iov;
    auto res = hf3fs_iovcreate(&iov, options->mountpoint, total_mem, 0, -1);
    if (res < 0) {
        return res;
    }

    td->orig_buffer = iov.base;
    return 0;
}

static void hf3fs_usrbio_free(struct thread_data *td) {
    auto &iov = static_cast<hf3fs_usrbio_data *>(td->io_ops_data)->iov;
    hf3fs_iovdestroy(&iov);
}

static int hf3fs_invalidate(struct thread_data *td, struct fio_file *f) {
    return 0;
}

extern "C" {

static struct ioengine_ops ioengine;
void get_ioengine(struct ioengine_ops **ioengine_ptr) {
    *ioengine_ptr = &ioengine;

    ioengine.name = "hf3fs_usrbio",
    ioengine.version = FIO_IOOPS_VERSION;
    ioengine.flags = FIO_NODISKUTIL;
    ioengine.init = hf3fs_usrbio_init;
    ioengine.queue = hf3fs_usrbio_queue;
    ioengine.commit = hf3fs_usrbio_commit;
    ioengine.getevents = hf3fs_usrbio_getevents;
    ioengine.event = hf3fs_usrbio_event;
    ioengine.cleanup = hf3fs_usrbio_cleanup;
    ioengine.open_file = hf3fs_usrbio_open;
    ioengine.close_file = hf3fs_usrbio_close;
    ioengine.invalidate = hf3fs_invalidate;
    ioengine.get_file_size = generic_get_file_size;
    ioengine.iomem_alloc = hf3fs_usrbio_alloc;
    ioengine.iomem_free = hf3fs_usrbio_free;
    ioengine.option_struct_size = sizeof(struct hf3fs_usrbio_options);
    ioengine.options = options;
}

}
