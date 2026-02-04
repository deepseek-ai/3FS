import errno
import click
import os
import sys
import stat
from typing import Optional, List
from hf3fs_utils.fs import is_relative_to, FileSystem
from hf3fs_utils.trash import TRASH_CONFIGS, Trash

MOUNTPOINT = os.environ.get("HF3FS_CLI_MOUNTPOINT", None)


def get_filesystem(path: str) -> FileSystem:
    mountpoint = None
    if MOUNTPOINT is not None:
        mountpoint = os.path.abspath(MOUNTPOINT)
    else:
        path = os.path.realpath(path)
        parts = path.split(os.sep)
        for i in range(1, 4):
            p = os.sep.join(parts[:i])
            if os.path.exists(os.path.join(p, "3fs-virt")):
                mountpoint = p
                break
    if not mountpoint:
        abort(f"{path} is not on 3FS")
    return FileSystem(mountpoint)


def abs_path(path: str) -> str:
    if ".." in path.split(os.path.sep):
        abort(f"Path {path} contains '..', which is not supported yet")
    normpath = os.path.normpath(path)
    # If the user calls rmtree path/symlink, it should delete the symlink instead of the path it points to
    # For dir paths, take the realpath, but keep the filename as is
    dir = os.path.dirname(normpath)
    filename = os.path.basename(normpath)
    return os.path.join(os.path.realpath(dir), filename)


def abort(msg):
    click.echo(click.style(msg, fg="red"), err=True)
    sys.exit(1)


@click.group()
def cli():
    """
    3FS command-line tool
    """


@cli.command()
@click.argument("old_path", type=click.Path(exists=True))
@click.argument("new_path", type=click.Path())
def mv(old_path: str, new_path: str):
    """
    Move files, supports moving files between different mount points within the same 3FS
    """
    try:
        old_path = abs_path(old_path)
        new_path = abs_path(new_path)

        try:
            new_st = os.stat(new_path, follow_symlinks=True)
            # new_path exists, should be a directory
            if not stat.S_ISDIR(new_st.st_mode):
                raise FileExistsError(errno.EEXIST, os.strerror(errno.EEXIST), new_path)
            # move to new_path/filename
            new_path = os.path.join(new_path, os.path.basename(old_path))
        except FileNotFoundError:
            pass

        fs = get_filesystem(old_path)
        fs.rename(old_path, new_path)
        click.echo(f"Move successful: {old_path} -> {new_path}")
    except AssertionError:
        raise
    except Exception as ex:
        abort(f"Move failed: {ex}")


class ExpireType(click.ParamType):
    def get_metavar(self, param) -> str:
        return "[1h|3h|8h|1d|3d|7d]"

    def convert(self, value, param, ctx):
        norm_value = value
        if norm_value.endswith("hour"):
            norm_value = norm_value.replace("hour", "h")
        elif norm_value.endswith("hours"):
            norm_value = norm_value.replace("hours", "h")
        elif norm_value.endswith("day"):
            norm_value = norm_value.replace("day", "d")
        elif norm_value.endswith("days"):
            norm_value = norm_value.replace("days", "d")

        if norm_value not in TRASH_CONFIGS.keys():
            self.fail(f"{value} is invalid, valid options are {self.get_metavar()}", param, ctx)
        else:
            return norm_value


@cli.command()
@click.argument("dir_paths", type=click.Path(exists=True), nargs=-1)
@click.option(
    "--expire",
    type=ExpireType(),
    help="Expiration time, contents in the trash will be automatically deleted after expiration",
)
@click.option("-y", "--yes", is_flag=True, default=False, help="Skip confirmation prompt and delete immediately")
def rmtree(dir_paths: List[str], expire: Optional[str], yes: bool):
    """
    Move a directory tree to the trash and set an expiration time, it will be automatically deleted after expiration

    \b
    Example:
    hf3fs_cli rmtree <path/to/remove> --expire <expire_time>

    \b
    - Use --expire [1h|3h|8h|1d|3d|7d] to specify the expiration time, the directory will be deleted after expiration.
    - Before expiration, you can restore the directory from the trash using `hf3fs_cli mv <trash_path> <target_path>`.
    - If you need to free up space immediately, you can use `hf3fs_cli rmtree <trash_path>` to delete the data in the trash immediately, this operation cannot be undone!
    - Use `ls /hf3fs/{cluster}/trash` to view the trash.
    """

    if not dir_paths:
        abort(f"Please provide the directory path to delete")

    first_path = abs_path(dir_paths[0])
    fs = get_filesystem(first_path)
    fs_trash = Trash(fs)

    clean_trash = is_relative_to(first_path, fs_trash.trash_path)
    if not clean_trash:
        if not expire:
            abort(f"Use --expire [1h|3h|8h|1d|3d|7d] to specify the expiration time")
    elif expire:
        abort(f"{first_path} is already in the trash")
    trash_cfg = TRASH_CONFIGS[expire] if not clean_trash else None

    dir_paths = [abs_path(p) for p in dir_paths]
    for dir_path in dir_paths:
        if is_relative_to(dir_path, fs_trash.trash_path) != clean_trash:
            if clean_trash:
                abort(f"{dir_path} is not in the trash")
            else:
                abort(f"{dir_path} is already in the trash")

    if clean_trash:
        if len(dir_paths) != 1:
            msg = (
                f"Immediately delete the following paths:\n"
                + "\n".join([f"- {p}" for p in dir_paths])
                + "\nThis operation cannot be undone"
            )
        else:
            msg = f"Immediately delete {dir_path}, this operation cannot be undone"
    else:
        if len(dir_paths) != 1:
            msg = (
                f"Move the following paths to the trash:\n"
                + "\n".join([f"- {p}" for p in dir_paths])
                + f"\nThey will be automatically deleted after {expire}"
            )
        else:
            msg = f"Move {dir_path} to the trash, it will be automatically deleted after {expire}"
    if not yes:
        assert click.confirm(msg, abort=True)

    for dir_path in dir_paths:
        try:
            if clean_trash:
                fs.remove(dir_path, recursive=True)
                click.echo(f"- Deleted {dir_path}")
            else:
                trash_path = fs_trash.move_to_trash(dir_path, trash_cfg)
                click.echo(f"- Trash path: {trash_path}")
        except AssertionError:
            raise
        except Exception as ex:
            abort(f"Failed to delete {dir_path}: {ex}")

    if not clean_trash:
        click.echo(
            "- Before expiration, you can use 'hf3fs_cli mv <trash_path> <target_path>' to restore, "
            "or use 'hf3fs_cli rmtree <trash_path>' to delete immediately and free up space"
        )


if __name__ == "__main__":
    cli()
