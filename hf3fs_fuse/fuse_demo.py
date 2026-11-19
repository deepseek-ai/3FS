from hf3fs_fuse.io import make_iovec, make_ioring, register_fd, deregister_fd
from multiprocessing.shared_memory import SharedMemory
import os

# Create memory for IO
shm = SharedMemory(size=1024, create=True)
iov = make_iovec(shm, '/hf3fs-cluster', 0, -1) # shm, mountpoint, blocksize, numa
shm.unlink() # shm can be unlinked after make_iovec

# Create ioring for IO
ior = make_ioring('/hf3fs-cluster', 100, True, 0) # mountpoint, num_entries, for_read, io_depth

# Open file
fd = os.open('/hf3fs-cluster/testread', os.O_RDONLY)
register_fd(fd) # must register after open to use usrbio

# Read file
ios = [(iov[:512], fd, 512), (iov[512:], fd, 0)] # iov, fd, offset
for io in ios:
    expected_size = len(memoryview(io[0]))
    ior.prepare(io[0], True, io[1], io[2], userdata=expected_size) # iov, for_read, fd, offset, userdata
    # Only for_read == True is allowed, because ior has for_read == True
    # userdata is kept alive until completion; avoid referencing the iovec to prevent a cycle
resv = ior.submit().wait(min_results=2)
for res in resv:
    print(res.result)
    assert res.result == res.userdata # Check read length is correct

# Close file
deregister_fd(fd) # must deregister before close
os.close(fd)
