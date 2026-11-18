import errno
import os
import pathlib
import pwd
import tempfile
import unittest
from unittest.mock import patch

from hf3fs_utils.fs import FileSystem
from hf3fs_utils.trash import Trash, TRASH_CONFIGS


class CollisionFileSystem(FileSystem):
    """Keep real path and filename checks, replacing only the kernel ioctl."""

    def __init__(self, root, error=errno.EEXIST):
        self.mountpoint = str(root)
        self.st_dev = os.stat(root).st_dev
        self.error = error
        self.calls = 0

    def _ioctl(self, fd, cmd, buffer):
        self.calls += 1
        if self.calls == 1 and self.error is not None:
            raise OSError(self.error, os.strerror(self.error))
        return 0


@unittest.skipIf(os.geteuid() == 0, "3FS trash does not support root users")
class TrashFilenameTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="3fs-trash-regression-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        user = pwd.getpwuid(os.geteuid()).pw_name
        (self.root / "trash" / user).mkdir(parents=True)

    def move(self, name, error=errno.EEXIST, append_timestamp_if_exists=True):
        source = self.root / name
        source.touch()
        filesystem = CollisionFileSystem(self.root, error)
        trash = Trash(filesystem)
        with patch("hf3fs_utils.trash.get_timestamp_us", return_value=1770000000000000):
            result = trash.move_to_trash(
                str(source),
                TRASH_CONFIGS["1h"],
                append_timestamp_if_exists=append_timestamp_if_exists,
            )
        return pathlib.Path(result).name, filesystem.calls

    def test_ascii_collision(self):
        name, calls = self.move("a" * 246)
        self.assertEqual(name, "a" * 200 + ".1770000000000000")
        self.assertEqual(calls, 2)

    def test_chinese_collision(self):
        name, calls = self.move("汉" * 82)
        self.assertEqual(name, "汉" * 79 + ".1770000000000000")
        self.assertEqual(calls, 2)

    def test_emoji_collision(self):
        name, calls = self.move("😀" * 60)
        self.assertEqual(name, "😀" * 59 + ".1770000000000000")
        self.assertEqual(calls, 2)

    def test_mixed_collision(self):
        name, calls = self.move("a" * 100 + "汉" * 50)
        self.assertEqual(name, "a" * 100 + "汉" * 46 + ".1770000000000000")
        self.assertEqual(calls, 2)

    def test_fitting_unicode_collision_preserves_prefix(self):
        name, calls = self.move("汉" * 70)
        self.assertEqual(name, "汉" * 70 + ".1770000000000000")
        self.assertEqual(calls, 2)

    def test_no_collision_preserves_original_name(self):
        original = "汉" * 82
        name, calls = self.move(original, error=None)
        self.assertEqual(name, original)
        self.assertEqual(calls, 1)

    def test_collision_retry_can_be_disabled(self):
        with self.assertRaises(OSError) as caught:
            self.move("汉" * 82, append_timestamp_if_exists=False)
        self.assertEqual(caught.exception.errno, errno.EEXIST)

    def test_unrelated_error_propagates(self):
        with self.assertRaises(OSError) as caught:
            self.move("汉" * 82, error=errno.EACCES)
        self.assertEqual(caught.exception.errno, errno.EACCES)
