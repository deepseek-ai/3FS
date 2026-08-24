import importlib
import sys
import types
from types import SimpleNamespace
from unittest.mock import Mock

import pytest


@pytest.fixture
def io_module(monkeypatch):
    native = types.ModuleType("hf3fs_py_usrbio")
    native.register_fd = Mock()
    native.deregister_fd = Mock()
    native.force_fsync = Mock()
    native.extract_mount_point = Mock(return_value="/3fs")
    native.hardlink = Mock()
    native.punch_hole = Mock()
    native.iovec = Mock()
    native.ioring = Mock()

    monkeypatch.setitem(sys.modules, "hf3fs_py_usrbio", native)
    sys.modules.pop("hf3fs_fuse.io", None)
    module = importlib.import_module("hf3fs_fuse.io")
    yield module, native
    sys.modules.pop("hf3fs_fuse.io", None)


def test_read_file_preserves_open_error(io_module, monkeypatch):
    module, native = io_module
    error = FileNotFoundError("missing input")
    monkeypatch.setattr(module.os, "open", Mock(side_effect=error))

    with pytest.raises(FileNotFoundError, match="missing input"):
        module.read_file("missing")

    native.register_fd.assert_not_called()
    native.deregister_fd.assert_not_called()


def test_read_file_does_not_deregister_failed_registration(io_module, monkeypatch):
    module, native = io_module
    error = RuntimeError("registration failed")
    native.register_fd.side_effect = error
    monkeypatch.setattr(module.os, "open", Mock(return_value=17))
    close = Mock()
    monkeypatch.setattr(module.os, "close", close)

    with pytest.raises(RuntimeError, match="registration failed"):
        module.read_file("input")

    native.deregister_fd.assert_not_called()
    close.assert_called_once_with(17)


def test_read_file_releases_resources_after_iovec_failure(io_module, monkeypatch):
    module, native = io_module
    native.register_fd.return_value = None
    monkeypatch.setattr(module.os, "open", Mock(return_value=23))
    close = Mock()
    monkeypatch.setattr(module.os, "close", close)

    shared_memory = Mock()
    shared_memory.buf = bytearray(8)
    monkeypatch.setattr(
        module.multiprocessing.shared_memory,
        "SharedMemory",
        Mock(return_value=shared_memory),
    )
    monkeypatch.setattr(module, "make_iovec", Mock(side_effect=RuntimeError("iovec failed")))

    with pytest.raises(RuntimeError, match="iovec failed"):
        module.read_file("input", hf3fs_mount_point="/3fs", block_size=8)

    native.deregister_fd.assert_called_once_with(23)
    close.assert_called_once_with(23)
    shared_memory.close.assert_called_once_with()
    shared_memory.unlink.assert_called_once_with()


def test_read_file_keeps_successful_reads_unchanged(io_module, monkeypatch):
    module, native = io_module
    monkeypatch.setattr(module.os, "open", Mock(return_value=29))
    close = Mock()
    monkeypatch.setattr(module.os, "close", close)

    shared_memory = Mock()
    shared_memory.buf = bytearray(b"data-rest")
    monkeypatch.setattr(
        module.multiprocessing.shared_memory,
        "SharedMemory",
        Mock(return_value=shared_memory),
    )
    monkeypatch.setattr(module, "make_iovec", Mock(return_value=shared_memory.buf))

    submission = Mock()
    submission.wait.return_value = [SimpleNamespace(result=4)]
    ring = Mock()
    ring.submit.return_value = submission
    monkeypatch.setattr(module, "make_ioring", Mock(return_value=ring))

    assert module.read_file("input", hf3fs_mount_point="/3fs", block_size=9) == b"data"
    ring.prepare.assert_called_once_with(shared_memory.buf[:], True, 29, 0)
    native.deregister_fd.assert_called_once_with(29)
    close.assert_called_once_with(29)
    shared_memory.close.assert_called_once_with()
    shared_memory.unlink.assert_called_once_with()
