import hf3fs_py_usrbio as h3fio
from hf3fs_py_usrbio import register_fd, deregister_fd, force_fsync, extract_mount_point, hardlink, punch_hole

import multiprocessing.shared_memory
import os
import os.path
from uuid import uuid4

class iovec:
    def __init__(self, iov, link):
        self.iov = iov
        self.link = link

    def __del__(self):
        os.unlink(self.link)

    def __getitem__(self, slice):
        return self.iov[slice]

    def __setitem__(self, slice, val):
         self.iov[slice] = val

class ioring:
    def __init__(self, ior):
        self.ior = ior

    @staticmethod
    def size_for_entries(entries):
        return h3fio.ioring.size_for_entries(entries)

    def prepare(self, iov, *args, **kwargs):
        if type(iov) == iovec:
            return self.ior.prepare(iov.iov, *args, **kwargs)
        else:
            return self.ior.prepare(iov, *args, **kwargs)

    def submit(self):
        return self.ior.submit()

    def wait(self, *args, **kwargs):
        return self.ior.wait(*args, **kwargs)

class IorPriority(object):
    HIGH = -1
    NORMAL = 0
    LOW = 1
    
def make_iovec(shm, hf3fs_mount_point, block_size=0, numa=-1):
    '''
    创建 iovec 对象

    Args:
        shm: Python multiprocessing.shared_memory.SharedMemory 对象
        hf3fs_mount_point: hf3fs 挂载点
        block_size: 默认为 0，代表整体视为一个 block，系统会按照 block_size 按块分配内存，防止触发 IB 注册驱动问题
        numa: 默认为 -1，代表不进行 numa 绑定，指定此参数可以指定内存绑定到固定 numa
    '''
    id = str(uuid4())
    target = os.path.normpath(f'/dev/shm/{shm.name}')
    link = f'{hf3fs_mount_point}/3fs-virt/iovs/{id}{f".b{block_size}" if block_size > 0 else ""}'

    os.symlink(target, link)

    return iovec(h3fio.iovec(shm.buf, id, hf3fs_mount_point, block_size, numa), link)

def make_ioring(hf3fs_mount_point, entries, for_read=True, io_depth=0, priority=None, timeout=None, numa=-1, flags=0):
    '''
    创建 ioring 对象
    可以用 io_depth 参数来控制读取策略，有以下三种情况：
    io_depth = 0 时，ioring 每次后台扫描任务或被通知有后台任务时，提交全部 io
    io_depth > 0 时，ioring 每次提交 io_depth 个 io，用户需保证最终有足量任务，否则 ioring 在 wait 时会卡住
    io_depth < 0 时，ioring 每次后台扫描任务或被通知有后台任务时，提交最多 -io_depth 个 io

    Args:
        hf3fs_mount_point: hf3fs 挂载点
        entries: ioring 最大存放 io 操作的个数
        for_read: 指定该 ioring 执行的操作，读为 True，写为 False
        io_depth: 指定读取策略
        numa: 默认为 -1，代表不进行 numa 绑定，指定此参数可以指定将 ioring 通信使用的内存绑定到固定 numa
        flags: 读写操作的一些额外选项，现在有用取值的主要是2可以在读到洞的时候报错而不是填0
    '''
    return ioring(h3fio.ioring(hf3fs_mount_point, entries, for_read, io_depth, priority, timeout, numa, flags))

# @param cb if set, will callback with read data and current offset, will return None
# suggested use is to use cb 
def read_file(fn, hf3fs_mount_point=None, block_size=1 << 30, off=0, priority=None, cb=None):
    if hf3fs_mount_point is None:
        hf3fs_mount_point = extract_mount_point(fn)

    bufs = []

    try:
        fd = os.open(fn, os.O_RDONLY)
        register_fd(fd)
        shm = multiprocessing.shared_memory.SharedMemory(size=block_size, create=True)
        iov = make_iovec(shm, hf3fs_mount_point)
        ior = make_ioring(hf3fs_mount_point, 1, priority=priority)

        i = 0
        roff = off
        while True:
            ior.prepare(iov[:], True, fd, roff)
            done = ior.submit().wait(min_results=1)[0]
            if done.result < 0:
                raise OSError(-done.result)

            if done.result == 0:
                break

            if cb is None:
                bufs.append(bytes(shm.buf[:done.result]))
            else:
                res = cb(shm.buf[:done.result], roff)
                if type(res) == int:
                    roff = res
                    continue
                elif res:
                    return

            if done.result < block_size:
                break

            i += 1
            roff += block_size

        if cb is not None:
            return
        
        if len(bufs) == 1:
            return bufs[0]
        else:
            return b''.join(bufs)
    finally:
        deregister_fd(fd)
        os.close(fd)
        del ior
        del iov
        shm.close()
        shm.unlink()
