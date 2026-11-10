"""Exercise BinaryFile with real local files, without the native extension."""

import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import patch


def load_hf3fs():
    # The extension is required at import time, but all tested operations use
    # the explicit local-file client below instead of constructing a cluster.
    def unavailable(*args, **kwargs):
        raise AssertionError("Unexpected native client operation")

    native = types.ModuleType("hf3fs_py_usrbio")
    native.Client = types.SimpleNamespace(
        **dict.fromkeys(
            "stat fstat mkdir rmdir unlink remove realpath readlink opendir "
            "readdir creat symlink link open close chmod chown chdir ftruncate "
            "iovalloc iovfree preadv pwritev".split(),
            unavailable,
        )
    )
    native.iovec = unavailable
    metadata = types.ModuleType("pkg_resources")
    metadata.get_distribution = lambda name: types.SimpleNamespace(version="test")
    source = Path(__file__).resolve().parents[2] / "hf3fs" / "__init__.py"
    spec = importlib.util.spec_from_file_location("_hf3fs_binary_file_test", source)
    module = importlib.util.module_from_spec(spec)
    with patch.dict(
        sys.modules,
        {
            "hf3fs_py_usrbio": native,
            "pkg_resources": metadata,
            spec.name: module,
        },
    ):
        spec.loader.exec_module(module)
    return module


class LocalClient:
    """Implement the native read/seek buffer interface using real OS calls."""

    def open(self, path, flags, mode, *, dir_fd=None):
        return os.open(path, flags, mode, dir_fd=dir_fd)

    def close(self, fd):
        os.close(fd)

    def lseek(self, fd, pos, how, *, readahead=None):
        return os.lseek(fd, pos, how)

    def read(self, fd, buf, *, readahead=None):
        data = os.read(fd, len(buf))
        buf[: len(data)] = data
        return len(data)


class BinaryFileReadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.hf3fs = load_hf3fs()

    def test_read_all_preserves_position_at_and_beyond_eof(self):
        for content, offset in [(b"", 0), (b"", 3), (b"abc", 3), (b"abc", 8)]:
            with self.subTest(content=content, offset=offset):
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "data"
                    path.write_bytes(content)
                    with self.hf3fs.BinaryFile(path, "r", client=LocalClient()) as f:
                        f.seek(offset)
                        self.assertEqual(bytes(f.read()), b"")
                        self.assertEqual(f.tell(), offset)
                        self.assertEqual(bytes(f.read()), b"")
                        self.assertEqual(f.tell(), offset)
                        self.assertEqual(os.lseek(f.fileno(), 0, os.SEEK_CUR), offset)

    def test_read_all_returns_remaining_content(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "data"
            path.write_bytes(b"abcdef")
            with self.hf3fs.BinaryFile(path, "r", client=LocalClient()) as f:
                f.seek(2)
                self.assertEqual(bytes(f.read()), b"cdef")
                self.assertEqual(f.tell(), 6)

    def test_read_all_after_file_is_truncated(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "data"
            path.write_bytes(b"abcdef")
            with self.hf3fs.BinaryFile(path, "r", client=LocalClient()) as f:
                f.seek(5)
                with path.open("r+b") as writer:
                    writer.truncate(2)
                self.assertEqual(bytes(f.read()), b"")
                self.assertEqual(f.tell(), 5)

    def test_sized_reads_preserve_data_and_position(self):
        for offset, size, expected, final_offset in [
            (0, 2, b"ab", 2),
            (1, 5, b"bc", 3),
            (8, 2, b"", 8),
        ]:
            with self.subTest(offset=offset, size=size):
                with tempfile.TemporaryDirectory() as directory:
                    path = Path(directory) / "data"
                    path.write_bytes(b"abc")
                    with self.hf3fs.BinaryFile(path, "r", client=LocalClient()) as f:
                        f.seek(offset)
                        self.assertEqual(bytes(f.read(size)), expected)
                        self.assertEqual(f.tell(), final_offset)


if __name__ == "__main__":
    unittest.main()
