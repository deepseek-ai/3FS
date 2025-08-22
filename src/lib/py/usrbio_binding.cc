#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fmt/format.h>
#include <pybind11/chrono.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/stl.h>
#include <stdexcept>

#include "common/utils/Uuid.h"
#include "lib/api/fuse.h"
#include "lib/api/hf3fs_usrbio.h"

namespace py = pybind11;

struct OSException {
  int errcode;
};

struct Hf3fsIovWithRes : public hf3fs_iov {
  std::shared_ptr<Hf3fsIovWithRes> base_iov;
  ssize_t result = 0;
  py::object userdata;
};

struct Hf3fsIorWithIovs : public hf3fs_ior {
  std::vector<std::shared_ptr<Hf3fsIovWithRes>> iovs;
};

PYBIND11_MODULE(hf3fs_py_usrbio, m) {
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) std::rethrow_exception(p);
    } catch (const OSException &e) {
      errno = e.errcode;
      PyErr_SetFromErrno(PyExc_OSError);
    }
  });

  m.def(
       "extract_mount_point",
       [](const std::string &path) -> std::optional<std::string> {
         char mp[4096];
         auto res = hf3fs_extract_mount_point(mp, sizeof(mp), path.c_str());
         if (res < 0) {
           return std::nullopt;
         } else if (res > (int)sizeof(mp)) {
           throw OSException{ENAMETOOLONG};
         } else {
           return std::string(mp);
         }
       },
       py::arg("path"),
       R"(
         从文件路径中获取hf3fs的挂载点，用于iov和ior的创建

         Args:
           path: hf3fs上的文件路径
        )")
      .def(
          "register_fd",
          [](int fd, uint64_t flags) {
            auto res = hf3fs_reg_fd(fd, flags);
            if (res > 0) {
              throw OSException{res};
            }
          },
          py::arg("fd"),
          py::arg("flags") = 0,
          R"(
        注册文件描述符，使得后续 usrbio 接口可以从该 fd 读取
        同一个 fd 在取消注册前不可重复注册

        Args:
          fd: 文件描述符
       )")
      .def("deregister_fd",
           &hf3fs_dereg_fd,
           py::arg("fd"),
           R"(
        取消注册文件描述符，使得后续 usrbio 接口不可从此 fd 读取
        文件如果已被注册，则在 close 前必须取消注册，否则可能导致后续其他文件读取错误

        Args:
          fd: 文件描述符
      )");

  m.def(
      "force_fsync",
      [](int fd) { return ioctl(fd, hf3fs::lib::fuse::HF3FS_IOC_FSYNC); },
      py::arg("fd"),
      R"(
        强制刷新文件长度，使得下次 stat 操作返回正确的文件长度

        Args:
          fd: 文件描述符，要求可读
      )");
  m.def(
      "hardlink",
      [](const std::string &target, const std::string &link_name) {
        int ret = hf3fs_hardlink(target.c_str(), link_name.c_str());
        if (ret != 0) {
          throw OSException{errno};
        }
      },
      py::arg("target"),
      py::arg("link_name"));

  py::class_<Hf3fsIovWithRes, std::shared_ptr<struct Hf3fsIovWithRes>>(m,
                                                                       "iovec",
                                                                       py::buffer_protocol(),
                                                                       R"(
    用于 usrbio 读写的共享内存，支持从给定的 Python SharedMemory 对象构建，支持 buffer protocol，支持绑定 numa
  )")
      .def_buffer([](const Hf3fsIovWithRes &iov) -> py::buffer_info {
        return py::buffer_info(iov.base, 1, py::format_descriptor<uint8_t>::format(), 1, {iov.size}, {1});
      })
      .def(py::init([](py::buffer base,
                       std::string_view id,
                       const char *hf3fs_mount_point,
                       size_t block_size = 0,
                       int numa = -1) {
             auto iov = std::make_shared<struct Hf3fsIovWithRes>();
             hf3fs::Uuid uuid;

             auto idRes = hf3fs::Uuid::fromHexString(id);
             if (!idRes) {
               throw std::invalid_argument(fmt::format("id '{}' is not a valid UUID ", *idRes));
             }
             uuid = *idRes;

             auto bufInfo = base.request(true);
             auto size = bufInfo.itemsize * (uint64_t)bufInfo.size;
             auto ptr = (uint8_t *)bufInfo.ptr;

             auto res = hf3fs_iovwrap(iov.get(), ptr, uuid.data, hf3fs_mount_point, size, block_size, numa);
             if (res < 0) {
               throw OSException{-res};
             }

             return iov;
           }),
           py::arg("base"),
           py::arg("id"),
           py::arg("hf3fs_mount_point"),
           py::arg("block_size") = 0,
           py::arg("numa") = -1)
      .def(
          "slice_by",
          [](const std::shared_ptr<Hf3fsIovWithRes> &self, py::buffer buf) {
            auto bufInfo = buf.request(true);
            auto ptr = (uint8_t *)bufInfo.ptr;
            auto bs = bufInfo.itemsize * (uint64_t)bufInfo.size;

            if (ptr < self->base) {
              throw std::out_of_range("given buf is out of range");
            }
            size_t off = (uint8_t *)ptr - (uint8_t *)self->base;
            size_t bytes = bs;
            if (off + bytes > self->size) {
              throw std::out_of_range("given buf is out of range");
            }

            auto iov = std::make_shared<struct Hf3fsIovWithRes>();
            iov->base = ptr;
            iov->size = bs;
            iov->block_size = self->block_size;
            iov->numa = self->numa;

            memcpy(iov->id, self->id, sizeof(self->id));
            strcpy(iov->mount_point, self->mount_point);

            iov->base_iov = self->base_iov ? self->base_iov : self;

            return iov;
          },
          py::arg("buf"),
          R"(
            将 iovec 切片到 buf 对应的地址区间
            用户需保证 buf 为一段连续内存，且 buf 的地址区间完全在 iovec 内
            
            Args:
              buf: Python buffer 对象
           )")
      .def("__getitem__",
           [](const std::shared_ptr<Hf3fsIovWithRes> &self, const py::slice &slice) {
             size_t start = 0, stop = 0, step = 0, slicelength = 0;
             if (!slice.compute(self->size, &start, &stop, &step, &slicelength)) {
               throw py::error_already_set();
             } else if (step != 1) {
               throw std::invalid_argument("step not 1 when slicing iovec");
             }

             auto iov = std::make_shared<struct Hf3fsIovWithRes>();
             iov->base = self->base + start;
             iov->size = slicelength;
             iov->block_size = self->block_size;
             iov->numa = self->numa;

             memcpy(iov->id, self->id, sizeof(self->id));
             strcpy(iov->mount_point, self->mount_point);

             iov->base_iov = self->base_iov ? self->base_iov : self;

             // XLOGF(DBG,
             //       "self {} iov {} base {} base off {} size {}",
             //       (void *)self.get(),
             //       (void *)iov.get(),
             //       (void *)iov->base,
             //       iov->base_off,
             //       iov->size);

             return iov;
           })
      .def_readonly("result", &Hf3fsIovWithRes::result, R"(
        操作结果返回值，如结果为正代表实际读写的字节数，如结果为负代表 -errno
        如读取字节数小于请求字节数，可能是文件已到末尾，或者读到文件中间空洞，这两种情况需用户自行区分
      )")
      .def_property_readonly("base_off",
                             [](const std::shared_ptr<Hf3fsIovWithRes> &self) {
                               return self->base_iov ? self->base - self->base_iov->base : 0;
                             })
      .def_property_readonly(
          "userdata",
          [](const std::shared_ptr<Hf3fsIovWithRes> &self) { return self->userdata; },
          R"(
        操作对应的 userdata，由 prepare 函数指定
      )");

  py::class_<Hf3fsIorWithIovs, std::shared_ptr<Hf3fsIorWithIovs>>(m, "ioring", R"(
    用于提交 usrbio 读写任务的队列，负责与 fuse 进程的通信
  )")
      .def(py::init([](const char *hf3fs_mount_point,
                       int entries,
                       bool for_read = true,
                       int io_depth = 0,
                       std::optional<int> priority = std::nullopt,
                       std::optional<int> timeout = std::nullopt,
                       int numa = -1,
                       int flags = 0) {
             std::shared_ptr<Hf3fsIorWithIovs> ior(new Hf3fsIorWithIovs{}, [](auto p) {
               hf3fs_iordestroy(p);
               delete p;
             });

             (void)priority;
             auto res = hf3fs_iorcreate4(ior.get(),
                                         hf3fs_mount_point,
                                         entries,
                                         for_read,
                                         io_depth,
                                         timeout.value_or(0),
                                         numa,
                                         flags);
             if (res < 0) {
               throw OSException{-res};
             }

             ior->iovs.resize(hf3fs_io_entries(ior.get()));
             return ior;
           }),
           py::arg("hf3fs_mount_point"),
           py::arg("entries"),
           py::arg("for_read") = true,
           py::arg("io_depth") = 0,
           py::arg("priority") = std::nullopt,
           py::arg("timeout") = std::nullopt,
           py::arg("numa") = -1,
           py::arg("flags") = 0)
      .def_static("size_for_entries", &hf3fs_ior_size)
      .def_property_readonly("entries",
                             [](const std::shared_ptr<Hf3fsIorWithIovs> &self) { return hf3fs_io_entries(self.get()); })
      .def(
          "prepare",
          [](const std::shared_ptr<Hf3fsIorWithIovs> &self,
             const std::shared_ptr<Hf3fsIovWithRes> &iov,
             bool read,
             int fd,
             size_t off,
             const py::object &userdata) {
            // XLOGF(DBG,
            //       "iov {} base {} base off {} size {}",
            //       (void *)iov.get(),
            //       (void *)iov->base,
            //       iov->base_off,
            //       iov->size);

            userdata.inc_ref();
            py::gil_scoped_release gr;

            auto res = hf3fs_prep_io(self.get(),
                                     (iov->base_iov ? iov->base_iov : iov).get(),
                                     read,
                                     iov->base,
                                     fd,
                                     off,
                                     iov->size,
                                     (void *)userdata.ptr());
            if (res < 0) {
              throw OSException{-res};
            }

            self->iovs[res] = iov;
            return self;
          },
          py::arg("iov"),
          py::arg("read"),
          py::arg("fd"),
          py::arg("off"),
          py::arg("userdata") = py::none(),
          R"(
            通过 ioring 对象请求后台读写任务
            prepare 调用过后，该读写会加入 usrbio 的后台队列，并随时可能被执行

            Args:
              iov: 读写使用的 iov slice，其长度为实际操作文件的长度
              read: True / False，需与 ioring 的参数一致
              fd: 文件描述符
              off: 文件读取或写入偏移量
              userdata: 默认为 None，可以指定一个 Python object，用户需确保该 object 不会被垃圾回收
          )")
      .def(
          "submit",
          [](const std::shared_ptr<Hf3fsIorWithIovs> &self) {
            py::gil_scoped_release gr;
            auto res = hf3fs_submit_ios(self.get());
            if (res < 0) {
              throw OSException{-res};
            }

            return self;
          },
          R"(
            通知 ioring 后台队列有新的读写任务
           )")
      .def(
          "wait",
          [](const std::shared_ptr<Hf3fsIorWithIovs> &self,
             int max_results = 0,
             int min_results = 0,
             std::optional<std::chrono::microseconds> timeout = std::nullopt) {
            std::vector<std::shared_ptr<Hf3fsIovWithRes>> out;
            {
              py::gil_scoped_release gr;

              if (max_results < min_results) {
                max_results = min_results;
              }

              struct timespec start, ts;
              struct timespec *tsp = nullptr;
              auto res = clock_gettime(CLOCK_REALTIME, &start);
              if (res < 0) {
                auto err = errno;
                throw OSException{-err};
              }

              if (timeout) {
                ts = start;

                XLOGF(DBG, "timeout {}us", timeout->count());

                ts.tv_sec += (uint64_t)timeout->count() / 1000000;
                ts.tv_nsec += (uint64_t)timeout->count() % 1000000 * 1000;
                ts.tv_sec += ts.tv_nsec / 1000000000;
                ts.tv_nsec %= 1000000000;

                tsp = &ts;
              }

              std::vector<struct hf3fs_cqe> cqes(max_results > 0 ? max_results : min_results > 0 ? min_results : 1);
              out.reserve(max_results > 0 ? max_results : min_results > 0 ? min_results : 1024);

              do {
                XLOGF(DBG, "to wait for ios");

                // if we got enough results, wait without timeout
                auto res = hf3fs_wait_for_ios(self.get(),
                                              cqes.data(),
                                              cqes.size(),
                                              min_results - (int)out.size(),
                                              (int)out.size() < min_results ? tsp : &start);

                XLOGF(DBG, "waited for ios with res {}", res);

                if (res < 0) {
                  if (out.empty()) {
                    throw OSException{-res};
                  } else {
                    // so the fetched results won't be wasted
                    return out;
                  }
                }

                for (int i = 0; i < res; ++i) {
                  auto idx = cqes[i].index;
                  XLOGF(DBG, "iov idx {}", idx);
                  auto &iov = self->iovs[idx];
                  if (!iov) {
                    XLOGF(FATAL, "same cqe {} fetched more than once", cqes[i].index);
                  }
                  iov->result = cqes[i].result;
                  iov->userdata = py::reinterpret_borrow<py::object>((PyObject *)cqes[i].userdata);
                  // iov->userdata.dec_ref();
                  out.emplace_back(std::move(iov));
                  iov.reset();
                }

                if (!res) {
                  break;
                }
              } while ((int)out.size() < max_results);
            }

            for (auto &iov : out) {
              iov->userdata.dec_ref();
            }

            return out;
          },
          py::kw_only(),
          py::arg("max_results") = 0,
          py::arg("min_results") = 0,
          py::arg("timeout") = std::nullopt,
          R"(
            等待 ioring 返回后台队列操作结果

            Args:
              max_results: 最大返回结果数
              min_results: 最小返回结果数
              timeout: 超时时间，默认为 None，代表无超时限制
          )")
      .def("destroy", [](const std::shared_ptr<Hf3fsIorWithIovs> &self) { hf3fs_iordestroy(self.get()); });

  m.def(
      "punch_hole",
      [](const std::string &filename, const std::vector<size_t> &start, const std::vector<size_t> &end, size_t flags) {
        if (start.size() != end.size() || start.size() > HF3FS_IOCTL_PUNCH_HOLE_MAX) {
          throw std::runtime_error("size not equal");
        }
        int fd = open(filename.c_str(), O_RDWR);
        auto res = hf3fs_punchhole(fd, start.size(), start.data(), end.data(), flags);
        close(fd);
        if (res != 0) {
          throw OSException{res};
        }
      },
      py::arg("filename"),
      py::arg("start"),
      py::arg("end"),
      py::arg("flags") = 0);

  m.def(
      "punch_hole",
      [](int fd, const std::vector<size_t> &start, const std::vector<size_t> &end, size_t flags) {
        if (start.size() != end.size() || start.size() > HF3FS_IOCTL_PUNCH_HOLE_MAX) {
          throw std::runtime_error("size not equal");
        }
        auto res = hf3fs_punchhole(fd, start.size(), start.data(), end.data(), flags);
        if (res != 0) {
          throw OSException{res};
        }
      },
      py::arg("fd"),
      py::arg("start"),
      py::arg("end"),
      py::arg("flags") = 0);
}
