import contextlib
import io
import os
import pathlib
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from hf3fs_utils import cli


class MountpointDiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="3fs-mount-discovery-")
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name).resolve()
        self.mount = self.root / "nested" / "mount"
        (self.mount / "3fs-virt").mkdir(parents=True)
        self.file = self.mount / "data" / "file"
        self.file.parent.mkdir()
        self.file.touch()

    def discover(self, path, configured_mount=None):
        # Mount discovery uses the real filesystem. Only construction of the
        # ioctl-backed FileSystem object needs a substitute.
        with patch.object(cli, "MOUNTPOINT", configured_mount), patch.object(
            cli,
            "FileSystem",
            side_effect=lambda mountpoint: SimpleNamespace(mountpoint=mountpoint),
        ):
            return cli.get_filesystem(str(path))

    def test_nested_mount(self):
        filesystem = self.discover(self.file)
        self.assertEqual(filesystem.mountpoint, str(self.mount))

    def test_mount_directory_itself(self):
        filesystem = self.discover(self.mount)
        self.assertEqual(filesystem.mountpoint, str(self.mount))

    def test_missing_descendant(self):
        filesystem = self.discover(self.mount / "missing" / "file")
        self.assertEqual(filesystem.mountpoint, str(self.mount))

    def test_symlink_to_nested_mount(self):
        link = self.root / "link"
        link.symlink_to(self.mount, target_is_directory=True)
        filesystem = self.discover(link / "data" / "file")
        self.assertEqual(filesystem.mountpoint, str(self.mount))

    def test_path_outside_mount_is_rejected(self):
        error = io.StringIO()
        with contextlib.redirect_stderr(error), self.assertRaises(SystemExit) as caught:
            self.discover(self.root / "other")
        self.assertEqual(caught.exception.code, 1)
        self.assertIn("is not on 3FS", error.getvalue())

    def test_configured_mount_takes_precedence(self):
        configured = self.root / "configured"
        configured.mkdir()
        filesystem = self.discover(self.file, configured_mount=str(configured))
        self.assertEqual(filesystem.mountpoint, str(configured))

    def test_first_ancestor_marker_takes_precedence(self):
        (self.file.parent / "3fs-virt").mkdir()
        filesystem = self.discover(self.file)
        self.assertEqual(filesystem.mountpoint, str(self.mount))

    def test_current_directory_marker_is_not_a_root_marker(self):
        other = self.root / "unrelated"
        (other / "3fs-virt").mkdir(parents=True)
        previous = os.getcwd()
        try:
            os.chdir(other)
            filesystem = self.discover(self.file)
        finally:
            os.chdir(previous)
        self.assertEqual(filesystem.mountpoint, str(self.mount))
