import hf3fs_fuse.io as h3io

import numpy as np
import os
import sys
import multiprocessing as mp
import multiprocessing.shared_memory
import time

fn = sys.argv[1] + '/file'
fd = os.open(fn, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)

data = np.random.randn(200 << 20).tobytes()
os.write(fd, data)
os.close(fd)

h3mp = h3io.extract_mount_point(sys.argv[1])

bs = h3io.read_file(fn, hf3fs_mount_point=h3mp, priority=h3io.IorPriority.HIGH)
assert data == bs, f'{fn} read out data not equal to those written in'

shm = mp.shared_memory.SharedMemory(size=10<<20, create=True)
iov = h3io.make_iovec(shm, h3mp, 0, -1)
shm.unlink()
ior = h3io.make_ioring(h3mp, 100, False, 0)

fn = sys.argv[1] + '/file2'
fd = os.open(fn, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
h3io.register_fd(fd)

shm.buf[:] = bytes(len(shm.buf))
ior.prepare(iov[:], False, fd, 0)
write_len = ior.submit().wait(min_results=1)[0].result
assert write_len == len(shm.buf)

time.sleep(6) # After linux inode cache invalidated
assert os.path.getsize(fn) == len(shm.buf)

ior.prepare(iov[:], False, fd, len(shm.buf))
write_len = ior.submit().wait(min_results=1)[0].result
assert write_len == len(shm.buf)

h3io.force_fsync(fd) # Force update length
assert os.path.getsize(fn) == 2 * len(shm.buf)

h3io.deregister_fd(fd)
os.close(fd)

print('usrbio read test succeeded', len(shm.buf))

fn = sys.argv[1] + '/file3'
fd = os.open(fn, os.O_RDONLY | os.O_CREAT | os.O_EXCL, 0o666)
os.close(fd)
fd = os.open(fn, os.O_RDONLY) # No write permission
h3io.register_fd(fd)
erred = False
try:
    ior.prepare(iov[:], False, fd, 0)
except OSError as e:
    erred = True
    assert e.errno == 13, "Errno not correct" # Should raise EPERM
assert erred, "Didn't raise EPERM"
h3io.deregister_fd(fd)
os.close(fd)
print('usrbio permission test succeeded')
