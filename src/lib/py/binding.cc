#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <stdexcept>

#include "lib/api/Client.h"

namespace hl = hf3fs::lib;
namespace py = pybind11;

using py_iovec = std::tuple<hl::iovec, int, off_t>;

struct OSException {
  int errcode;
  std::string filename;
};

template <typename T>
static std::conditional_t<std::is_same_v<T, hl::Empty>, void, T> unwrap(hl::Result<T> r) {
  if (!r) {
    // throw std::runtime_error(std::string("errno ") + std::to_string(r.error()));
    throw OSException{r.error().first, r.error().second};
    // PyErr_SetFromErrnoWithFilename(PyExc_OSError, r.error().second.c_str());
  } else if constexpr (std::is_same_v<T, hl::Empty>) {
    return;
  } else {
    return r.value();
  }
}

static int64_t makeTimeNs(const timespec &t) { return t.tv_sec * 1'000'000'000ll + t.tv_nsec; }

PYBIND11_MODULE(hf3fs_py_usrbio, m) {
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) std::rethrow_exception(p);
    } catch (const OSException &e) {
      errno = e.errcode;
      PyErr_SetFromErrnoWithFilename(PyExc_OSError, e.filename.c_str());
    }
  });

  m.attr("HF3FS_SUPER_MAGIC") = py::int_(uint32_t(HF3FS_SUPER_MAGIC));
  // m.attr("HF3FS_IOC_GET_MOUNT_NAME") = py::int_(uint32_t(hl::HF3FS_IOC_GET_MOUNT_NAME));
  // m.attr("HF3FS_IOC_GET_PATH_OFFSET") = py::int_(uint32_t(hl::HF3FS_IOC_GET_PATH_OFFSET));
  // m.attr("HF3FS_IOC_GET_MAGIC_NUM") = py::int_(uint32_t(hl::HF3FS_IOC_GET_MAGIC_NUM));

  //  m.def("init", &hl::initLib);

  py::class_<hl::DIR, std::shared_ptr<hl::DIR>>(m, "_DIR");
  py::class_<hl::dirent>(m, "dirent")
      .def_readonly("d_type", &hl::dirent::d_type)
      .def_readonly("d_name", &hl::dirent::d_name);
  py::class_<hl::stat>(m, "stat_result")
      .def_readonly("st_mode", &hl::stat::st_mode)
      .def_readonly("st_nlink", &hl::stat::st_nlink)
      .def_readonly("st_uid", &hl::stat::st_uid)
      .def_readonly("st_gid", &hl::stat::st_gid)
      .def_readonly("st_size", &hl::stat::st_size)
      .def_property_readonly("st_atime", [](const hl::stat &st) { return st.st_atim.tv_sec; })
      .def_property_readonly("st_atime_ns", [](const hl::stat &st) { return makeTimeNs(st.st_atim); })
      .def_property_readonly("st_mtime", [](const hl::stat &st) { return st.st_mtim.tv_sec; })
      .def_property_readonly("st_mtime_ns", [](const hl::stat &st) { return makeTimeNs(st.st_mtim); })
      .def_property_readonly("st_ctime", [](const hl::stat &st) { return st.st_ctim.tv_sec; })
      .def_property_readonly("st_ctime_ns", [](const hl::stat &st) { return makeTimeNs(st.st_ctim); })
      .def_readonly("st_ltarg", &hl::stat::st_ltarg)
      .def("is_file", [](const hl::stat &st) { return (st.st_mode & S_IFMT) == S_IFREG; })
      .def("is_dir", [](const hl::stat &st) { return (st.st_mode & S_IFMT) == S_IFDIR; })
      .def("is_link", [](const hl::stat &st) { return (st.st_mode & S_IFMT) == S_IFLNK; });

  py::class_<hl::iovec>(m, "iovec", py::buffer_protocol(), R"(
        用于批量读取和批量写入的共享内存，支持 buffer protocol

        可以通过 iovalloc 方法分配
      )")
      .def_buffer([](hl::iovec &iov) -> py::buffer_info {
        return py::buffer_info(iov.iov_base, 1, py::format_descriptor<uint8_t>::format(), 1, {iov.iov_len}, {1});
      })
      .def(py::init([](const hl::iovec &other, std::optional<py::buffer> buf) {
             auto bufInfo = buf->request(true);
             auto ptr = (uint8_t *)bufInfo.ptr;
             auto bs = bufInfo.itemsize * (uint64_t)bufInfo.size;

             if (ptr < other.iov_base) {
               throw std::out_of_range("given buf is out of range");
             }
             size_t off = (uint8_t *)ptr - (uint8_t *)other.iov_base;
             size_t bytes = bs;
             if (off + bytes > other.iov_len) {
               throw std::out_of_range("given buf is out of range");
             }
             return hl::iovec{(uint8_t *)other.iov_base + off, bytes, other.iov_handle, nullptr};
           }),
           py::arg("other"),
           py::arg("buf"),
           R"(
            从给定的 Python buffer 地址创建子 iovec

            Args:
                other: 父 iovec
                buf: Python buffer 对象，需要保证 buffer 完全在在父 iovec 地址区间内，且指向的内存是连续的
           )")
      .def(py::init([](const hl::iovec &other, std::optional<size_t> off, std::optional<size_t> bytes) {
             size_t off_value = off.value_or(0);
             size_t bytes_value = bytes.value_or(other.iov_len);
             if (off_value + bytes_value > other.iov_len) {
               throw std::out_of_range("given off and bytes is out of range");
             }
             return hl::iovec{(uint8_t *)other.iov_base + off_value, bytes_value, other.iov_handle, nullptr};
           }),
           py::arg("other"),
           py::arg("off") = std::nullopt,
           py::arg("bytes") = std::nullopt,
           R"(
            从父 iovec 的一段连续内存创建子 iovec

            Args:
                other: 父 iovec
                off: 子 iovec 在父 iovec 中的起始位置（以 byte 为单位）
                bytes: 子 iovec 的长度（以 byte 为单位）
           )");

#define PY_METHOD_AT(method, ...) return unwrap(self.method##at(dir_fd.value_or(AT_FDCWD), __VA_ARGS__));

#define PY_F_METHOD_AT(method, ...) \
  unwrap(self.method##at(dir_fd.value_or(AT_FDCWD), __VA_ARGS__, follow_symlinks ? 0 : AT_SYMLINK_NOFOLLOW))

  py::class_<hl::IClient, std::shared_ptr<hl::IClient>>(m, "Client")
      .def(py::init([](std::string_view mountName, std::string_view token, bool as_super) {
             if (as_super) {
               return unwrap(hl::IClient::newSuperClient(mountName, token));
             } else {
               return unwrap(hl::IClient::newClient(mountName, token));
             }
           }),
           py::arg("mount_name"),
           py::arg("token"),
           py::kw_only(),
           py::arg("as_super") = false,
           R"(
            创建 client 对象

            Args:
                mount_name: 挂载名
                token: 访问 3FS 所需的认证信息，可联系运维获取
                as_super: 是否创建有 super 权限的 client（需 token 有 root 权限）
           )")
      .def(
          "opendir",
          [](hl::IClient &self, std::string name, std::optional<int> dir_fd) { PY_METHOD_AT(opendir, name); },
          py::arg("name"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt)
      .def("readdir", [](hl::IClient &self, const std::shared_ptr<hl::DIR> &dp) { return unwrap(self.readdir(dp)); })
      .def("rewinddir",
           [](hl::IClient &self, const std::shared_ptr<hl::DIR> &dp) { return unwrap(self.rewinddir(dp)); })
      .def(
          "mkdir",
          [](hl::IClient &self, std::string path, mode_t mode, std::optional<int> dir_fd, bool recursive) {
            PY_METHOD_AT(mkdir, path, mode, recursive);
          },
          py::arg("path"),
          py::arg("mode") = 0777,
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          py::arg("recursive") = false)
      .def(
          "rmdir",
          [](hl::IClient &self, std::string path, std::optional<int> dir_fd, bool recursive) {
            PY_METHOD_AT(rmdir, path, recursive);
          },
          py::arg("path"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          py::arg("recursive") = false)
      .def(
          "unlink",
          [](hl::IClient &self, std::string path, std::optional<int> dir_fd) { PY_METHOD_AT(unlink, path, 0); },
          py::arg("path"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt)
      .def(
          "remove",
          [](hl::IClient &self, std::string path, std::optional<int> dir_fd, bool recursive) {
            PY_METHOD_AT(remove, path, recursive);
          },
          py::arg("path"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          py::arg("recursive") = false)
      .def(
          "rename",
          [](hl::IClient &self,
             std::string src,
             std::string dst,
             std::optional<int> src_dir_fd,
             std::optional<int> dst_dir_fd) {
            unwrap(self.renameat(src_dir_fd.value_or(AT_FDCWD), src, dst_dir_fd.value_or(AT_FDCWD), dst));
          },
          py::arg("src"),
          py::arg("dst"),
          py::kw_only(),
          py::arg("src_dir_fd") = std::nullopt,
          py::arg("dst_dir_fd") = std::nullopt)
      .def(
          "creat",
          [](hl::IClient &self, std::string path, mode_t mode, std::optional<int> dir_fd, bool excl) {
            PY_METHOD_AT(creat, path, mode, excl);
          },
          py::arg("path"),
          py::arg("mode") = 0666,
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          py::arg("excl") = false)
      .def(
          "symlink",
          [](hl::IClient &self, std::string src, std::string dst, std::optional<int> dir_fd) {
            unwrap(self.symlinkat(src, dir_fd.value_or(AT_FDCWD), dst));
          },
          py::arg("src"),
          py::arg("dst"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt)
      .def(
          "link",
          [](hl::IClient &self,
             std::string src,
             std::string dst,
             std::optional<int> src_dir_fd,
             std::optional<int> dst_dir_fd,
             bool follow_symlinks) {
            unwrap(self.linkat(src_dir_fd.value_or(AT_FDCWD),
                               src,
                               dst_dir_fd.value_or(AT_FDCWD),
                               dst,
                               follow_symlinks ? AT_SYMLINK_NOFOLLOW : 0));
          },
          py::arg("src"),
          py::arg("dst"),
          py::kw_only(),
          py::arg("old_dir_fd") = std::nullopt,
          py::arg("new_dir_fd") = std::nullopt,
          py::arg("follow_symlinks") = true)
      .def(
          "stat",
          [](hl::IClient &self, std::string path, std::optional<int> dir_fd, bool follow_symlinks) {
            hl::stat st;
            PY_F_METHOD_AT(fstat, path, &st);
            return st;
          },
          py::arg("path"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          py::arg("follow_symlinks") = true)
      .def("fstat",
           [](hl::IClient &self, int fd) {
             hl::stat st;
             unwrap(self.fstat(fd, &st));
             return st;
           })
      .def(
          "readlink",
          [](hl::IClient &self, std::string path, std::optional<int> dir_fd) { PY_METHOD_AT(readlink, path); },
          py::arg("path"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt)
      .def(
          "realpath",
          [](hl::IClient &self, std::string path, std::optional<int> dir_fd, bool absolute) {
            PY_METHOD_AT(realpath, path, absolute);
          },
          py::arg("path"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          py::arg("absolute") = false)
      .def(
          "open",
          [](hl::IClient &self, std::string path, int flags, mode_t mode, std::optional<int> dir_fd) {
            PY_METHOD_AT(open, path, flags, mode);
          },
          py::arg("path"),
          py::arg("flags"),
          py::arg("mode") = 0666,
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          R"(
            使用 client 打开一个 3fs 上的文件，并返回 fd（与操作系统的 fd 不互通）
            路径需要使用服务端路径（详见 hf3fs.fuse.serverPath）
          )")
      .def("close", [](hl::IClient &self, int fd) { return unwrap(self.close(fd)); })
      .def("ftruncate", [](hl::IClient &self, int fd, off_t length) { return unwrap(self.ftruncate(fd, length)); })
      .def(
          "utime",
          [](hl::IClient &self,
             std::string path,
             std::optional<std::pair<float, float>> times,
             std::optional<std::pair<int64_t, int64_t>> ns,
             std::optional<int> dir_fd,
             bool follow_symlinks) {
            struct timespec ts[2];
            if (times.has_value() && ns.has_value()) {
              throw std::invalid_argument("times and ns are both specified");
            } else if (times.has_value()) {
              ts[0].tv_sec = (time_t)times->first;
              ts[0].tv_nsec = (long)times->first * 1'000'000'000;
              ts[1].tv_sec = (time_t)times->second;
              ts[1].tv_nsec = (long)times->second * 1'000'000'000;
            } else if (ns.has_value()) {
              ts[0].tv_sec = ns->first / 1'000'000'000;
              ts[0].tv_nsec = ns->first % 1'000'000'000;
              ts[1].tv_sec = ns->second / 1'000'000'000;
              ts[1].tv_nsec = ns->second % 1'000'000'000;
            } else {
              ts[0].tv_nsec = UTIME_NOW;
              ts[1].tv_nsec = UTIME_NOW;
            }
            PY_F_METHOD_AT(utimens, path, ts);
          },
          py::arg("path"),
          py::kw_only(),
          py::arg("times") = std::nullopt,
          py::arg("ns") = std::nullopt,
          py::arg("dir_fd") = std::nullopt,
          py::arg("follow_symlinks") = true)
      .def(
          "chmod",
          [](hl::IClient &self, std::string path, mode_t mode, std::optional<int> dir_fd, bool follow_symlinks) {
            PY_F_METHOD_AT(fchmod, path, mode);
          },
          py::arg("path"),
          py::arg("mode"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          py::arg("follow_symlinks") = true)
      .def(
          "chown",
          [](hl::IClient &self, std::string path, int uid, int gid, std::optional<int> dir_fd, bool follow_symlinks) {
            PY_F_METHOD_AT(fchown, path, uid, gid);
          },
          py::arg("path"),
          py::arg("uid"),
          py::arg("gid"),
          py::kw_only(),
          py::arg("dir_fd") = std::nullopt,
          py::arg("follow_symlinks") = true)
      .def("chdir", [](hl::IClient &self, std::string path) { unwrap(self.chdir(path)); })
      .def(
          "iovalloc",
          [](hl::IClient &self, size_t bytes, int numa, bool global, size_t block_size) {
            return unwrap(self.iovalloc(bytes, numa, global, block_size));
          },
          py::arg("bytes"),
          py::kw_only(),
          py::arg("numa") = -1,
          py::arg("global") = false,
          py::arg("block_size") = 0,
          R"(
            创建 iovec 对象在给定 numa 上

            Args:
                bytes: 分配的 iovec 大小（以 byte 为单位）
                numa: iovec 绑定的 numa 编号，默认为不绑定numa
                global: 是否是所有进程都能访问到的iovec，是的话可以share给其它进程，默认为false

            Examples:

            .. code-block:: python

                client.iovalloc(1 << 30)
                client.iovalloc(1 << 30, numa=1)
          )")
      .def("iovfree", [](hl::IClient &self, hl::iovec iov) { unwrap(self.iovfree(iov.iov_handle)); })
      .def(
          "preadv",
          [](hl::IClient &self, const std::vector<py_iovec> &piov) {
            std::vector<hl::iovec> iov(piov.size());
            std::vector<hl::ioseg> segv(piov.size());
            std::vector<ssize_t> resv(piov.size());
            for (size_t i = 0; i < piov.size(); ++i) {
              iov[i] = std::get<0>(piov[i]);
              segv[i] = hl::ioseg{std::get<1>(piov[i]), std::get<2>(piov[i])};
            }
            unwrap(self.preadv((int)piov.size(), iov.data(), segv.data(), resv.data()));
            return resv;
          },
          R"(
            批量读取数据到 iovec 上

            Args:
                piov: 一个列表，其中每一项是 (iovec, fd, offset) 的格式

            .. code-block:: python

                import hf3fs as h3
                iov = client.iovalloc(1 << 30)
                client.preadv([(h3.iovec(iov, 3072, 2048), 3, 1024)])
                # 从 3 号 fd 中的文件，从 1024 字节开始，读取 2048 字节到 iov 上从 3072 字节开始的内存
           )")
      .def(
          "pwritev",
          [](hl::IClient &self, const std::vector<py_iovec> &piov) {
            std::vector<hl::iovec> iov(piov.size());
            std::vector<hl::ioseg> segv(piov.size());
            std::vector<ssize_t> resv(piov.size());
            for (size_t i = 0; i < piov.size(); ++i) {
              iov[i] = std::get<0>(piov[i]);
              segv[i] = hl::ioseg{std::get<1>(piov[i]), std::get<2>(piov[i])};
            }
            unwrap(self.pwritev((int)piov.size(), iov.data(), segv.data(), resv.data()));
            return resv;
          },
          R"(
            批量写入 iovec 上的数据到文件系统

            Args:
                piov: 一个列表，其中每一项是 (iovec, fd, offset) 的格式

            .. code-block:: python

                import hf3fs as h3
                iov = client.iovalloc(1 << 30)
                memoryview(h3.iovec(iov, 3072, 2048))[:] = bytes([1] * 2048)
                client.pwrite([(h3.iovec(iov, 3072, 2048), 3, 1024)])
                # 将从 iov 中，3072 字节开始的连续 2048 个字节写入到 3 号 fd 中的文件 offset 为 1024 的位置
           )")
      .def(
          "lseek",
          [](hl::IClient &self, int fd, off_t pos, int how, std::optional<size_t> readahead) {
            return unwrap(self.lseek(fd, pos, how, readahead.value_or(0)));
          },
          py::arg("fd"),
          py::arg("pos"),
          py::arg("how"),
          py::kw_only(),
          py::arg("readahead") = std::nullopt)
      .def(
          "read",
          [](hl::IClient &self, int fd, py::buffer buf, std::optional<size_t> readahead) {
            auto binfo = buf.request();
            ssize_t stride = binfo.itemsize;
            for (size_t i = binfo.ndim; i--;) {
              if (stride != binfo.strides[i]) {
                throw std::invalid_argument("cannot read into incontiguous buffer");
              } else {
                stride *= binfo.shape[i];
              }
            }

            return unwrap(self.read(fd, binfo.ptr, stride, readahead.value_or(0)));
          },
          py::arg("fd"),
          py::arg("buf"),
          py::kw_only(),
          py::arg("readahead") = std::nullopt)
      .def(
          "write",
          [](hl::IClient &self, int fd, py::buffer buf, bool flush) {
            auto binfo = buf.request();
            ssize_t stride = binfo.itemsize;
            for (size_t i = binfo.ndim; i--;) {
              if (stride != binfo.strides[i]) {
                throw std::invalid_argument("cannot write from incontiguous buffer");
              } else {
                stride *= binfo.shape[i];
              }
            }

            return unwrap(self.write(fd, binfo.ptr, stride, flush));
          },
          py::arg("fd"),
          py::arg("buf"),
          py::kw_only(),
          py::arg("flush") = false)
      .def(
          "sharedFileHandles",
          [](hl::IClient &self, const std::vector<int> &fds) { return unwrap(self.sharedFileHandles(fds)); },
          py::arg("fds"))
      .def(
          "openWithFileHandles",
          [](hl::IClient &self, const std::vector<uint8_t> &fhs) { return unwrap(self.openWithFileHandles(fhs)); },
          py::arg("fhs"))
      .def(
          "sharedIovecHandle",
          [](hl::IClient &self, const hl::iovec &iov) { return unwrap(self.sharedIovecHandle(iov.iov_handle)); },
          py::arg("iov"))
      .def(
          "openIovecHandle",
          [](hl::IClient &self, const std::string &iovh) { return unwrap(self.openIovecHandle(iovh)); },
          py::arg("iovh"));

  // py::class_<hl::ClientPool>(m, "ClientPool")
  //     .def(py::init<int, std::string_view, uint32_t>())
  //     .def("acquire", &ClientPool::acquire)
  //     .def("release", &ClientPool::release);
#undef PY_F_METHOD_AT
#undef PY_METHOD_AT
}
