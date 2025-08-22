import os
from pathlib import PosixPath

def get_mount_point(p):
    np = os.path.realpath(p)
    parts = PosixPath(np).parts
    return os.path.join(*parts[:3])
