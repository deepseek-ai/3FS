import os
import argparse
import errno
import shutil
from datetime import datetime, timedelta, timezone
from hf3fs_utils.fs import FileSystem
from hf3fs_utils import trash


def remove_trailing_separator(path: str):
    if path.endswith(os.path.sep):
        return os.path.dirname(path)
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("mount_point", type=str)
    parser.add_argument("test_path", type=str)
    args = parser.parse_args()

    shutil.rmtree(args.test_path, ignore_errors=True)

    fs = FileSystem(args.mount_point)
    os.makedirs(os.path.join(args.test_path, "dir", "subdir"), exist_ok=True)
    try:
        fs.remove(os.path.join(args.test_path, "dir"), False)
        assert False
    except OSError as ex:
        assert ex.errno == errno.ENOTEMPTY, ex
    fs.remove(os.path.join(args.test_path, "dir"), True)
    assert not os.path.exists(os.path.join(args.test_path, "dir"))

    try:
        fs.remove(os.path.join(args.mount_point, "../.."), True)
        assert False
    except RuntimeError as ex:
        print(ex)

    os.makedirs(os.path.join(args.test_path, "src/subdir"), exist_ok=True)
    fs.rename(os.path.join(args.test_path, "src"), os.path.join(args.test_path, "dst"))
    assert os.path.exists(os.path.join(args.test_path, "dst"))
    os.makedirs(os.path.join(args.test_path, "src"), exist_ok=True)
    try:
        fs.rename(
            os.path.join(args.test_path, "src"), os.path.join(args.test_path, "dst")
        )
        assert False
    except OSError as ex:
        assert ex.errno in [errno.EEXIST, errno.ENOTEMPTY], ex
    fs.rename(
        os.path.join(args.test_path, "src"), os.path.join(args.test_path, "dst", "src")
    )
    assert os.path.exists(os.path.join(args.test_path, "dst", "src"))

    os.makedirs(os.path.join(args.test_path, "src/subdir"), exist_ok=True)
    try:
        fs.rename(os.path.join(args.test_path, "src"), os.path.expanduser("~"))
        assert False
    except RuntimeError as ex:
        print(ex)

    # test trash
    # test trash timestamp
    raised = False
    try:
        trash.format_date(datetime.now())
    except:
        raised = True
    assert raised
    now = datetime.now(tz=timezone.utc).replace(second=0, microsecond=0)
    str = trash.format_date(now)
    assert trash.parse_date(str) == now
    for tz in range(-12, 12):
        tz_time = now.astimezone(timezone(timedelta(hours=tz)))
        tz_str = trash.format_date(tz_time)
        assert tz_str == str, (tz_str, str)
        assert tz_time == now, (tz_time, now)
        assert trash.parse_date(tz_str) == now, (trash.parse_date(tz_str), now)

    try:
        trash.TrashConfig("5s", timedelta(seconds=5), timedelta(seconds=5))
        assert False
    except AssertionError:
        pass
    for i in range(100):
        os.makedirs(os.path.join(args.test_path, f"dir-{i}", "data", "subdir"))
    trash_config = trash.TrashConfig("2m", timedelta(minutes=2), timedelta(minutes=1))
    my_trash = trash.Trash(fs)
    assert not os.listdir(my_trash.user_trash_path), os.listdir(
        my_trash.user_trash_path
    )

    trash_paths = set()
    for i in range(100):
        trash_path = my_trash.move_to_trash(
            os.path.join(args.test_path, f"dir-{i}", "data"), trash_config
        )
        assert trash_path not in trash_paths, trash_path
        trash_paths.add(trash_path)
        # time.sleep(1)
    assert len(trash_paths) == 100
    for t in trash_paths:
        assert os.path.exists(t)

    # test clean trash
    my_trash.move_to_trash(
        os.path.join(args.test_path, f"dir-0"),
        trash.TrashConfig("1h", timedelta(hours=1), timedelta(minutes=10)),
    )
    # print(f"before clean", os.listdir(my_trash.user_trash_path))
    # trash_cleaner = TrashCleaner(fs)
    # trash_cleaner.run(True)
    # print(f"after clean", os.listdir(my_trash.user_trash_path))
    # time.sleep(180)
    # # create some unknown type under
    # trash_cleaner.run(True)
    # print(f"after clean", os.listdir(my_trash.user_trash_path))
    # assert os.listdir(my_trash.user_trash_path)

    print("test finish")
