import argparse
import fcntl
import os
import struct

FS_IOC_GETFLAGS = 0x80086601
FS_IOC_SETFLAGS = 0x40086602
FS_HUGE_FILE_FL = 0x00040000
FS_UNSUPPORTED_FL = 0x00800000


def get_iflags(file_path):
    with open(file_path, "rb") as f:
        buf = struct.pack("I", 0)
        flags = fcntl.ioctl(f.fileno(), FS_IOC_GETFLAGS, buf)
        iflags = struct.unpack("I", flags)[0]
        return iflags


def set_iflags(file_path, iflags):
    with open(file_path, "rb") as f:
        buf = struct.pack("I", iflags)
        fcntl.ioctl(f.fileno(), FS_IOC_SETFLAGS, buf)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("file_path")
    args = parser.parse_args()

    current_iflags = get_iflags(args.file_path)
    print(f"Current iflags: {current_iflags:#010x}")

    new_iflags = current_iflags | FS_HUGE_FILE_FL
    set_iflags(args.file_path, new_iflags)

    updated_iflags = get_iflags(args.file_path)
    print(f"Updated iflags: {updated_iflags:#010x}")
    assert updated_iflags == new_iflags

    try:
        set_iflags(args.file_path, updated_iflags | FS_UNSUPPORTED_FL)
        assert False
    except OSError as ex:
        print("set FS_UNSUPPORTED_FL", ex)
        pass
