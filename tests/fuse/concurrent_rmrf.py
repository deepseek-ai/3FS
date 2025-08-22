import os
import argparse
import uuid
import time
from hf3fs_utils.fs import FileSystem
from functools import partial
from multiprocessing import Pool
from datetime import datetime

mnt = None
fs = None
repeat = 20

def task(fn):
    for _ in range(repeat):
        fn()

def rmrf_ln(dir):
    path = os.path.join(mnt, dir, str(uuid.uuid4()))
    os.makedirs(path)
    os.symlink(os.path.realpath(path), os.path.realpath(os.path.join(mnt, '3fs-virt', 'rm-rf', str(uuid.uuid4()))))

def rmrf_ioctl(dir):
    path = os.path.join(mnt, dir, str(uuid.uuid4()))
    os.makedirs(path)
    fs.remove(path, True)

def mv_ioctl(src, dst):
    src = os.path.join(mnt, src, str(uuid.uuid4()))
    dst = os.path.join(mnt, dst, str(uuid.uuid4()))
    os.makedirs(src)
    fs.rename(src, dst)

def parallel(tasks):
    with Pool(processes=len(tasks)) as pool:
        pool.map(task, tasks)

def main():
    parser = argparse.ArgumentParser(description="Process some paths.")
    parser.add_argument('--mnt_path', type=str, required=True, help='The mount path to process')
    args = parser.parse_args()

    global mnt, fs, repeat
    mnt = args.mnt_path
    fs = FileSystem(mnt)

    os.makedirs(os.path.join(mnt, "test", "src"), exist_ok=True)
    os.makedirs(os.path.join(mnt, "test", "dst"), exist_ok=True)

    for c in [32, 64, 128]:
        print(datetime.now().strftime("%Y-%m-%d %H:%M:%S"), f"3fs-virt/rm-rf {c}")
        parallel([partial(rmrf_ln, "test")] * c)

    print("=============")

    for c in [32, 64, 128]:
        print(datetime.now().strftime("%Y-%m-%d %H:%M:%S"), f"ioctl rmrf {c}")
        parallel([partial(rmrf_ioctl, "test")] * c)
    
    print("=============")
    for c in [32, 64, 128]:
        print(datetime.now().strftime("%Y-%m-%d %H:%M:%S"), f"ioctl mv {c}")
        parallel([partial(mv_ioctl, "test/src", "test/dst")] * c)

    print("=============")
    for c in [32, 64, 128]:
        print(datetime.now().strftime("%Y-%m-%d %H:%M:%S"), f"ioctl mv2 {c}")
        parallel([partial(mv_ioctl, "test/src", "test/dst")] * c + [partial(mv_ioctl, "test/dst", "test/src")] * c)
    
    print("=============")
    for c in [32, 64, 128]:
        print(datetime.now().strftime("%Y-%m-%d %H:%M:%S"), f"all {c}")
        parallel(
            (
                [partial(mv_ioctl, "test/src", "test/dst")] * c +
                [partial(mv_ioctl, "test/dst", "test/src")] * c +
                [partial(rmrf_ioctl, "test")] * c +
                [partial(rmrf_ioctl, "test")] * c
            )
        )

if __name__ == "__main__":
    main()
