import os
import dataclasses
import pwd
import stat
import errno
import time
from typing import Optional
from datetime import datetime, timedelta, timezone
from . import fs

UTC8_TZ = timezone(timedelta(hours=8))
DATE_FORMAT = "%Y%m%d_%H%M"
BASE_TIMESTAMP = int(datetime(year=1980, month=1, day=1, tzinfo=UTC8_TZ).timestamp())


def format_date(t: datetime) -> str:
    assert t.tzinfo
    return t.astimezone(tz=UTC8_TZ).strftime(DATE_FORMAT)


def parse_date(t: str) -> datetime:
    return datetime.strptime(t, DATE_FORMAT).replace(tzinfo=UTC8_TZ)


def get_timestamp_us() -> int:
    timestamp_seconds = time.time()
    return int(timestamp_seconds * 1_000_000)


@dataclasses.dataclass
class TrashConfig:
    name: str
    expire: timedelta
    time_slice: timedelta

    def __post_init__(self):
        assert self.name and "-" not in self.name, f"invalid name {self.name}"
        assert self.expire >= timedelta(minutes=1), self.expire
        assert self.time_slice >= timedelta(minutes=1), self.time_slice
        assert self.time_slice < self.expire, (self.time_slice, self.expire)

    def current_dir(self) -> str:
        base_timestamp = BASE_TIMESTAMP
        current_timestamp = int(datetime.now(tz=UTC8_TZ).timestamp())
        assert current_timestamp > base_timestamp, current_timestamp

        time_slice_seconds = int(self.time_slice.total_seconds())
        expire_seconds = int(self.expire.total_seconds())
        assert time_slice_seconds and expire_seconds, repr(self)
        start_timestamp = (
            (current_timestamp - base_timestamp) // time_slice_seconds
        ) * time_slice_seconds + base_timestamp
        end_timestamp = start_timestamp + expire_seconds + time_slice_seconds
        start_datetime = datetime.fromtimestamp(start_timestamp, tz=UTC8_TZ)
        end_datetime = datetime.fromtimestamp(end_timestamp, tz=UTC8_TZ)

        return f"{self.name}-{format_date(start_datetime)}-{format_date(end_datetime)}"


TRASH_CONFIGS = {
    "1h": TrashConfig("1h", timedelta(hours=1), timedelta(minutes=10)),
    "3h": TrashConfig("3h", timedelta(hours=3), timedelta(minutes=30)),
    "8h": TrashConfig("8h", timedelta(hours=8), timedelta(minutes=30)),
    "1d": TrashConfig("1d", timedelta(days=1), timedelta(hours=1)),
    "3d": TrashConfig("3d", timedelta(days=3), timedelta(days=1)),
    "7d": TrashConfig("7d", timedelta(days=7), timedelta(days=1)),
}


class Trash:
    def __init__(
        self,
        filesystem: fs.FileSystem,
        user: Optional[int] = None,
        user_name: Optional[str] = None,
    ) -> None:
        if user is None:
            user = os.geteuid()
        assert isinstance(user, int), user
        if user_name is None:
            user_name = pwd.getpwuid(user).pw_name
        if user == 0:
            raise RuntimeError(f"hf3fs trash does not support root user")

        # Check if the trash directory is mounted
        trash = os.path.join(filesystem.mountpoint, "trash")
        if not os.path.exists(trash):
            raise FileNotFoundError(errno.ENOENT, os.strerror(errno.ENOENT), trash)

        # Check if the user's trash directory exists
        user_trash = os.path.join(filesystem.mountpoint, "trash", user_name)
        if not os.path.exists(user_trash):
            raise FileNotFoundError(errno.ENOENT, os.strerror(errno.ENOENT), user_trash)

        user_trash_fd, user_trash_st = filesystem.opendir(user_trash)
        os.close(user_trash_fd)
        assert stat.S_ISDIR(user_trash_st.st_mode)
        if user_trash_st.st_uid != user:
            raise RuntimeError(
                f"Trash directory {user_trash}, owner {user_trash_st.st_uid} != {user}"
            )

        self.filesystem = filesystem
        self.user = user
        self.user_name = user_name
        self.trash_path = trash
        self.user_trash_path = user_trash

    def _check_user(self):
        euid = os.geteuid()
        if euid != self.user:
            raise RuntimeError(f"euid {euid} != trash owner {self.user}")

    def move_to_trash(
        self,
        path: str,
        config: TrashConfig,
        trash_name: Optional[str] = None,
        append_timestamp_if_exists: bool = True,
    ) -> str:
        self._check_user()
        assert isinstance(config, TrashConfig), f"invalid trash config {config}"

        dir_fd = None
        trash_dir_fd = None
        try:
            dir_fd, dir_st, filename = self.filesystem.split_path(path)
            try:
                st = os.stat(filename, dir_fd=dir_fd, follow_symlinks=False)
            except OSError as ex:
                ex.filename = path
                raise

            if stat.S_ISDIR(st.st_mode):
                # The user must be the owner of the directory and have rwx permissions.
                imode = stat.S_IMODE(st.st_mode)
                if st.st_uid != os.geteuid() or (imode & 0o700) != 0o700:
                    raise PermissionError(errno.EPERM, os.strerror(errno.EPERM), path)

            trash_dir = os.path.join(self.user_trash_path, config.current_dir())
            try:
                os.mkdir(trash_dir, 0o755)
            except FileExistsError:
                pass

            trash_dir_fd, trash_dir_st = self.filesystem.opendir(trash_dir)

            trash_name = trash_name or filename
            current_trash_name = trash_name
            retry = 0
            while True:
                retry += 1
                try:
                    self.filesystem._rename_ioctl(
                        dir_fd,
                        dir_st.st_ino,
                        filename,
                        trash_dir_st.st_ino,
                        current_trash_name,
                        True,
                    )
                    return os.path.join(trash_dir, current_trash_name)
                except OSError as ex:
                    if (
                        ex.errno in (errno.ENOTDIR, errno.EEXIST, errno.ENOTEMPTY)
                        and append_timestamp_if_exists
                        and retry < 10
                    ):
                        current_trash_name = f"{trash_name[0:200]}.{get_timestamp_us()}"
                    else:
                        raise
        finally:
            if dir_fd is not None:
                os.close(dir_fd)
            if trash_dir_fd is not None:
                os.close(trash_dir_fd)