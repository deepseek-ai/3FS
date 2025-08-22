import os
from pathlib import PosixPath

from hf3fs_py_usrbio import HF3FS_SUPER_MAGIC

HF3FS_IOC_GET_MOUNT_NAME = 2149607424
HF3FS_IOC_GET_PATH_OFFSET = 2147772417
HF3FS_IOC_GET_MAGIC_NUM = 2147772418

HF3FS_IOC_RECURSIVE_RM = 2147772426

def serverPath(p):
    '''
    从完整路径获取 client 接受的路径名

    Args:
        p: 待解析的路径名

    Examples:

    .. code-block:: python

        import hf3fs.fuse
        hf3fs.fuse.serverPath('/hf3fs-cluster/aaa/../cpu/abc/def')
    '''
    np = os.path.normpath(os.path.realpath(p))
    return os.path.join('/', *PosixPath(np).parts[3:])

def mountName(p):
    '''
    从完整路径获取 mount name

    Args:
        p: 待解析的路径名

    Examples:
    
    .. code-block:: python
    
        import hf3fs.fuse
        hf3fs.fuse.mountName('/hf3fs-cluster/aaa/../cpu/abc/def')
    '''
    np = os.path.normpath(os.path.realpath(p))
    return PosixPath(np).parts[2]
