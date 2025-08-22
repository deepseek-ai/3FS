import os
import fcntl
import errno
import struct
import stat
import sys
import pathlib
from typing import Tuple


def is_relative_to(path1, path2) -> bool:
    try:
        pathlib.PurePath(path1).relative_to(path2)
        return True
    except:
        return False


class FileSystem:
    HF3FS_IOCTL_MAGIC_CMD = 0x80046802
    HF3FS_IOCTL_MAGIC_NUM = 0x8F3F5FFF

    HF3FS_IOCTL_VERSION_CMD = 0x80046803

    HF3FS_IOCTL_RENAME_CMD = 0x4218680E
    HF3FS_IOCTL_RENAME_BUFFER_SIZE = 536

    HF3FS_IOCTL_REMOVE_CMD = 0x4110680F
    HF3FS_IOCTL_REMOVE_BUFFER_SIZE = 272

    def __init__(self, mountpoint: str) -> None:
        self.mountpoint = os.path.realpath(mountpoint)
        self.virt_path = os.path.join(self.mountpoint, "3fs-virt")

        # Check if the mount point is a directory
        if not os.path.exists(self.mountpoint):
            raise FileNotFoundError(
                errno.ENOENT, os.strerror(errno.ENOENT), self.mountpoint
            )
        if not os.path.isdir(self.mountpoint):
            raise NotADirectoryError(
                errno.ENOTDIR, os.strerror(errno.ENOTDIR), self.mountpoint
            )

        virt_fd = None
        try:
            # Check the 3fs-virt directory
            virt_fd = os.open(self.virt_path, os.O_RDONLY | os.O_DIRECTORY)
            virt_st = os.fstat(virt_fd)
            if not stat.S_ISDIR(virt_st.st_mode):
                raise NotADirectoryError(
                    errno.ENOTDIR, os.strerror(errno.ENOTDIR), self.virt_path
                )
            self.st_dev = virt_st.st_dev
            # Check the magic number
            buffer = bytearray(4)
            try:
                self._ioctl(virt_fd, FileSystem.HF3FS_IOCTL_MAGIC_CMD, buffer)
            except OSError:
                raise RuntimeError(f"{self.mountpoint} is not a 3FS mount point")
            magic_number = struct.unpack("I", buffer)[0]
            expected_magic_number = FileSystem.HF3FS_IOCTL_MAGIC_NUM
            if magic_number != expected_magic_number:
                raise RuntimeError(
                    f"{self.mountpoint} is not a 3FS mount point, "
                    f"magic number {magic_number:x} != {expected_magic_number:x}"
                )
            # Check if the required ioctl is supported
            ioctl_version = -1
            try:
                buffer = bytearray(4)
                self._ioctl(virt_fd, FileSystem.HF3FS_IOCTL_VERSION_CMD, buffer)
                ioctl_version = int.from_bytes(buffer, sys.byteorder, signed=False)
            except OSError:
                pass
            assert ioctl_version >= 1
        finally:
            if virt_fd is not None:
                os.close(virt_fd)

    def _check_user(self):
        if os.geteuid() == 0 or os.getegid() == 0:
            raise RuntimeError(f"root user not allowed")

    def _encode_filename(self, name: str) -> bytes:
        assert name and os.sep not in name, name
        name_bytes = name.encode("utf8")
        if len(name_bytes) > 255:
            raise OSError(errno.ENAMETOOLONG, os.strerror(errno.ENAMETOOLONG), name)
        return name_bytes

    def opendir(self, dir_path: str) -> Tuple[int, os.stat_result]:
        dir_fd = os.open(dir_path, os.O_DIRECTORY | os.O_RDONLY)
        try:
            try:
                dir_st = os.fstat(dir_fd)
            except OSError as ex:
                ex.filename = dir_path
                raise

            if not stat.S_ISDIR(dir_st.st_mode):
                raise NotADirectoryError(
                    errno.ENOTDIR, os.strerror(errno.ENOTDIR), dir_path
                )
            if dir_st.st_dev != self.st_dev:
                raise RuntimeError(f"{dir_path} is not under the 3FS mount point {self.mountpoint}")
            if dir_st.st_ino & 0xF000000000000000:
                raise RuntimeError(f"{dir_path} is a virtual path")

            return dir_fd, dir_st
        except:
            os.close(dir_fd)
            raise

    def split_path(self, path: str) -> Tuple[int, os.stat_result, str]:
        filename = os.path.basename(path)
        if not filename:
            raise RuntimeError(f"{path} has no filename")
        if filename in [".", ".."]:
            raise RuntimeError(f"{path} filename is {filename}")
        if len(filename.encode("utf8")) > 255:
            raise OSError(errno.ENAMETOOLONG, os.strerror(errno.ENAMETOOLONG), path)

        dir = os.path.dirname(path) or "."
        dir_fd, dir_st = self.opendir(dir)
        return dir_fd, dir_st, filename

    def rename(self, old_path: str, new_path: str) -> None:
        self._check_user()
        if is_relative_to(
            os.path.realpath(new_path), os.path.join(self.mountpoint, "trash")
        ):
            raise RuntimeError(f"{new_path} is in the trash")

        old_dir_fd = None
        new_dir_fd = None
        try:
            old_dir_fd, old_dir_st, old_filename = self.split_path(old_path)
            new_dir_fd, new_dir_st, new_filename = self.split_path(new_path)

            try:
                old_st = os.stat(old_filename, dir_fd=old_dir_fd, follow_symlinks=False)
                if stat.S_ISLNK(old_st.st_mode):
                    raise RuntimeError(f"{old_path} is symlink")
            except OSError as ex:
                ex.filename = old_path
                raise

            try:
                os.stat(new_filename, dir_fd=new_dir_fd, follow_symlinks=False)
                raise FileExistsError(errno.EEXIST, os.strerror(errno.EEXIST), new_path)
            except FileNotFoundError:
                pass
            except OSError as ex:
                ex.filename = new_path
                raise

            try:
                self._rename_ioctl(
                    old_dir_fd,
                    old_dir_st.st_ino,
                    old_filename,
                    new_dir_st.st_ino,
                    new_filename,
                    False,
                )
            except OSError as ex:
                ex.filename = old_path
                ex.filename2 = new_path
                raise
        finally:
            if old_dir_fd is not None:
                os.close(old_dir_fd)
            if new_dir_fd is not None:
                os.close(new_dir_fd)

    def _rename_ioctl(
        self,
        old_dir_fd: int,
        old_dir_ino: int,
        old_filename: str,
        new_dir_ino: int,
        new_filename: str,
        move_to_trash: bool,
    ) -> None:
        assert old_filename and not os.path.sep in old_filename, old_filename
        assert new_filename and not os.path.sep in new_filename, new_filename

        cmd = FileSystem.HF3FS_IOCTL_RENAME_CMD
        buffer = struct.pack(
            "N256sN256s?",
            old_dir_ino,
            self._encode_filename(old_filename),
            new_dir_ino,
            self._encode_filename(new_filename),
            move_to_trash,
        ).ljust(FileSystem.HF3FS_IOCTL_RENAME_BUFFER_SIZE)
        self._ioctl(old_dir_fd, cmd, buffer)

    def remove(self, path: str, recursive: bool) -> None:
        dir_fd = None
        try:
            dir_fd, dir_st, filename = self.split_path(path)
            st = os.stat(filename, dir_fd=dir_fd, follow_symlinks=False)
            if stat.S_ISLNK(st.st_mode):
                raise RuntimeError(f"{path} is symlink")
            if stat.S_ISDIR(st.st_mode) and recursive:
                # The user must be the owner of the directory and have rwx permissions
                imode = stat.S_IMODE(st.st_mode)
                if st.st_uid != os.geteuid() or (imode & 0o700) != 0o700:
                    raise PermissionError(errno.EPERM, os.strerror(errno.EPERM), path)

            try:
                self._remove_ioctl(dir_fd, dir_st.st_ino, filename, recursive)
            except OSError as ex:
                ex.filename = path
                raise ex
        finally:
            if dir_fd is not None:
                os.close(dir_fd)

    def _remove_ioctl(
        self, parent_fd: int, parent_ino: int, filename: str, recursive: bool
    ) -> None:
        assert filename and os.sep not in filename, filename
        cmd = FileSystem.HF3FS_IOCTL_REMOVE_CMD
        buffer = struct.pack(
            "N256s?", parent_ino, self._encode_filename(filename), recursive
        ).ljust(FileSystem.HF3FS_IOCTL_REMOVE_BUFFER_SIZE)
        self._ioctl(parent_fd, cmd, buffer)

    def _ioctl(self, fd: int, cmd: int, buffer):
        self._check_user()
        return fcntl.ioctl(fd, cmd, buffer)