#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>

#include "common/utils/FdWrapper.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

TEST(TestLibUring, Normal) {
  constexpr auto N = 1024;
  FdWrapper fd{::open("/dev/zero", O_RDONLY)};
  ASSERT_TRUE(fd);

  struct io_uring ring;
  ASSERT_EQ(io_uring_queue_init(N, &ring, 0), 0);

  std::string buf(N, 'A');
  struct iovec iovec;
  iovec.iov_base = buf.data();
  iovec.iov_len = N;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  ASSERT_NE(sqe, nullptr);
  io_uring_prep_readv(sqe, fd, &iovec, 1, 0);
  io_uring_sqe_set_data(sqe, nullptr);

  ASSERT_EQ(io_uring_submit(&ring), 1);

  struct io_uring_cqe *cqe;
  ASSERT_EQ(::io_uring_wait_cqe(&ring, &cqe), 0);
  ASSERT_EQ(cqe->res, N);
  ::io_uring_cqe_seen(&ring, cqe);

  ASSERT_EQ(buf, std::string(N, '\0'));

  ::io_uring_queue_exit(&ring);
}

TEST(TestLibUring, SQPolling) {
  if (getuid()) {
    // skip if non-root.
    GTEST_SKIP_("Skip SQPolling if non-root");
  }

  constexpr auto N = 256;
  FdWrapper fd{::open("/dev/zero", O_RDONLY)};
  ASSERT_TRUE(fd);

  struct io_uring ring {};
  struct io_uring_params params {};
  params.flags = IORING_SETUP_SQPOLL;
  params.sq_thread_idle = 50;
  ASSERT_EQ(io_uring_queue_init_params(N, &ring, &params), 0);

  int files[1] = {fd};
  ASSERT_EQ(io_uring_register_files(&ring, files, 1), 0);

  std::string buf(N, 'A');
  struct iovec iovec;
  iovec.iov_base = buf.data();
  iovec.iov_len = N;
  ASSERT_EQ(io_uring_register_buffers(&ring, &iovec, 1), 0);

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  ASSERT_NE(sqe, nullptr);
  io_uring_prep_read_fixed(sqe, 0, iovec.iov_base, N, 0, 0);
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data(sqe, nullptr);

  ASSERT_EQ(io_uring_submit(&ring), 1);

  struct io_uring_cqe *cqe;
  ASSERT_EQ(::io_uring_wait_cqe(&ring, &cqe), 0);
  ASSERT_EQ(cqe->res, N);
  ::io_uring_cqe_seen(&ring, cqe);

  ASSERT_EQ(buf, std::string(N, '\0'));

  ::io_uring_queue_exit(&ring);
}

}  // namespace
}  // namespace hf3fs::test
