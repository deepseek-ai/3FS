import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path


def setup_version(source_dir, env=None):
  result = subprocess.run(
    [sys.executable, "setup.py", "--version"],
    cwd=source_dir,
    env=env,
    capture_output=True,
    text=True,
  )
  assert result.returncode == 0, result.stderr
  return result.stdout.strip().splitlines()[-1]


def test_sdist_version_without_git(tmp_path):
  source_dir = tmp_path / "source"
  source_dir.mkdir()
  shutil.copy(Path(__file__).parents[1] / "setup.py", source_dir)
  package_dir = source_dir / "hf3fs_fuse"
  package_dir.mkdir()
  (package_dir / "__init__.py").touch()

  subprocess.run(["git", "init", "-q"], cwd=source_dir, check=True)
  subprocess.run(["git", "add", "."], cwd=source_dir, check=True)
  subprocess.run(
    [
      "git", "-c", "user.name=3FS test", "-c", "user.email=test@example.com",
      "commit", "-qm", "test source",
    ],
    cwd=source_dir,
    check=True,
  )

  source_version = setup_version(source_dir)
  dist_dir = tmp_path / "dist"
  subprocess.run(
    [sys.executable, "setup.py", "sdist", "--dist-dir", str(dist_dir)],
    cwd=source_dir,
    check=True,
    capture_output=True,
    text=True,
  )

  unpack_dir = tmp_path / "unpacked"
  with tarfile.open(next(dist_dir.glob("*.tar.gz"))) as archive:
    archive.extractall(unpack_dir)
  unpacked_source = next(unpack_dir.iterdir())

  env = os.environ.copy()
  env["GIT_CEILING_DIRECTORIES"] = str(unpacked_source)
  unpacked_version = setup_version(unpacked_source, env)
  package_info = next(unpacked_source.glob("*.egg-info/PKG-INFO")).read_text()

  assert source_version == unpacked_version == "1.2.9"
  assert "Version: 1.2.9\n" in package_info
