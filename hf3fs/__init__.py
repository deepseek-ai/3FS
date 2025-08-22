from hf3fs_py_usrbio import Client, iovec

#import attrs
from contextlib import contextmanager, AbstractContextManager
from dataclasses import dataclass
import functools
import os
from pathlib import PurePosixPath
import threading as th

from pkg_resources import get_distribution

try:
    __version__ = get_distribution('hf3fs').version
except:
    __version__ = "debug"

DEFAULT_CLIENT = th.local()
DEFAULT_CLIENT.client = None
DEFAULT_CLIENT.clients = {}

#@attrs.define
@dataclass
class MountInfo:
    token: str
    as_super: bool

MOUNT_INFO = {}

def _getDefaultClient(kwargs):
    if 'client' in kwargs and kwargs['client'] is not None:
        client = kwargs['client']
        del kwargs['client']
    elif 'mount_name' in kwargs and kwargs['mount_name'] is not None:
        mount_name = kwargs['mount_name']
        if mount_name not in DEFAULT_CLIENT.clients:
            setupDefaultClient(mount_name)
        client = DEFAULT_CLIENT.clients[mount_name]
        del kwargs['mount_name']
    elif DEFAULT_CLIENT.client is None:
        raise RuntimeError("default client not setup")
    else:
        client = DEFAULT_CLIENT.client

    return client, kwargs

def _setupH3Method(name):
    @functools.wraps(getattr(Client, name))
    def wrapper(*args, **kwargs):
        nonlocal name
        client, kwargs = _getDefaultClient(kwargs)
        return getattr(client, name)(*args, **kwargs)

    globals()[name] = wrapper

for _name in ['stat', 'fstat', 'mkdir', 'rmdir', 'unlink', 'remove', 'realpath', 'readlink', 'opendir', 'readdir',
              'creat', 'symlink', 'link', 'open', 'close', 'chmod', 'chown', 'chdir', 'ftruncate',
              'iovalloc', 'iovfree', 'preadv', 'pwritev']:
    _setupH3Method(_name)

def setMountInfo(mount_name, token, as_super=False):
    global MOUNT_INFO
    MOUNT_INFO[mount_name] = MountInfo(token, as_super)

def setupDefaultClient(mount_name):
    global MOUNT_INFO
    if mount_name not in MOUNT_INFO:
        raise ValueError(f"unknown mount name '{mount_name}'")

    mount = MOUNT_INFO[mount_name]
    
    global DEFAULT_CLIENT
    client = DEFAULT_CLIENT.clients[mount_name] = Client(mount_name, mount.token, as_super=mount.as_super)
    return client

@contextmanager
def defaultClient(mount_name, token, as_super=False):
    global DEFAULT_CLIENT
    lastClient = DEFAULT_CLIENT.client
    DEFAULT_CLIENT.client = Client(mount_name, token, as_super=as_super)

    try:
        yield DEFAULT_CLIENT.client
    finally:
        DEFAULT_CLIENT.client = lastClient

def withClient(f):
    @functools.wraps(f)
    def wrapper(*args, **kwargs):
        nonlocal f
        client, kwargs = _getDefaultClient(kwargs)
        return f(client=client, *args, **kwargs)

    return wrapper

@withClient
def listdir(path='.', client=None):
    if path is None:
        path = '.'

    dirp = client.opendir(path)
#    print(dirp, flush=True)
    fileList = []
    while True:
        dent = client.readdir(dirp)
#        print('dent', dent, flush=True)
        if dent is None:
            break
        fileList.append(dent.d_name)

    return fileList

class DirEntry(object):
    @withClient
    def __init__(self, parentPath, name, etype, parentFd, client=None):
        self._parentPath = PurePosixPath(parentPath)
        self._name = name
        self._etype = etype
        self._parentFd = parentFd
        self._client = client
        self._st = None

    @property
    def name(self):
        return self._name

    @property
    def path(self):
        return str(self._parentPath / self._name)

    _S_IFMT = 0o170000
    _S_IFLNK = 0o120000
    _S_IFREG = 0o100000
    _S_IFDIR = 0o040000

    _DT_DIR = 4
    _DT_REG = 8
    _DT_LNK = 10

    def _checkWFollow(self, against, follow_symlinks):
        if self._etype == against[0]:
            return True
        elif follow_symlinks and self.is_symlink():
            st = self.stat(True)
            return (st.st_mode & self._S_IFMT) == against[1]
        else:
            return False

    def is_dir(self, follow_symlinks=True):
        return self._checkWFollow((self._DT_DIR, self._S_IFDIR), follow_symlinks)

    def is_file(self, follow_symlinks=True):
        return self._checkWFollow((self._DT_REG, self._S_IFREG), follow_symlinks)

    def is_symlink(self):
        return self._etype == self._DT_LNK

    def stat(self, follow_symlinks=True):
        if self._st is None:
            self._st = self._client.stat(self._name, dir_fd=self._parentFd, follow_symlinks=follow_symlinks)
        return self._st

@withClient
def scandir(path='.', dir_fd=None, client=None):
    class DirEntryIter(AbstractContextManager):
        def __init__(self, path, client, dir_fd):
            self._path = path
            self._client = client
            self._dir_fd = dir_fd
            self._fd = None
            self._dirp = None

        @property
        def dir_fd(self):
            return self._fd

        def close(self):
            if self._fd is not None:
                self._client.close(self._fd)
                self._fd = None
                
            self._dirp = None

        def __iter__(self):
            self._fd = self._client.open(self._path, os.O_DIRECTORY | os.O_PATH, dir_fd=self._dir_fd)
#            print('dirfd to scan', self._fd, flush=True)
            self._dirp = self._client.opendir(self._path, dir_fd=self._dir_fd)
            return self

        def __next__(self):
            dent = self._client.readdir(self._dirp)
            if dent is None:
                raise StopIteration()
            return DirEntry(self._path, dent.d_name, dent.d_type, self._fd, client=self._client)

        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc_value, traceback):
            self.close()
            return False
    
    if path is None:
        path = '.'

    return DirEntryIter(path, client, dir_fd)

@withClient
def walk2(top, topdown=True, onerror=None, followlinks=False, dir_fd=None, curr_dir=None, client=None):
    try:
        with scandir(curr_dir if curr_dir is not None else top, dir_fd, client=client) as sd:
            dirnames = []
            filenames = []
            for dent in sd:
                name = dent.name
                if dent.is_dir(followlinks):
                    dirnames.append(name)
                    if not topdown:
                        yield from walk2(dent.path, False, onerror, followlinks, sd.dir_fd, name, client=client)
                else:
                    filenames.append(name)

            yield (top, dirnames, filenames, sd.dir_fd)
            if topdown:
                topp = PurePosixPath(top)
                for dirname in dirnames:
                    yield from walk2(str(topp / dirname), True, onerror, followlinks, sd.dir_fd, dirname, client=client)
    except OSError as e:
        if onerror is not None:
            onerror(e)

@withClient
def walk(top, topdown=True, onerror=None, followlinks=False, dir_fd=None, client=None):
    for dp, dns, fns, dfd in walk2(top, topdown, onerror, followlinks, dir_fd, None, client=client):
        yield (dp, dns, fns)

class BinaryFile(AbstractContextManager):
    @withClient
    def __init__(self, path, mode, dir_fd=None, client=None, ignore_cache=False):
        self.client = client

        if mode == 'r':
            flags = os.O_RDONLY
        elif mode == 'r+':
            flags = os.O_RDWR
        elif mode == 'r+c':
            flags = os.O_RDWR | os.O_CREAT
        elif mode == 'w':
            flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
        elif mode == 'w+':
            flags = os.O_RDWR | os.O_CREAT | os.O_TRUNC
        else:
            raise ValueError(f'invalid mode {mode}')

        if ignore_cache:
            flags |= os.O_NONBLOCK

        self._fd = None
        self._off = 0
        
        self._fd = client.open(path, flags, 0o644, dir_fd=dir_fd)

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def close(self):
#        print('to close fd', self._fd, flush=True)
        if self._fd is not None:
            self.client.close(self._fd)
            self._fd = None

    def fileno(self):
        return self._fd

    def seek(self, pos, how=os.SEEK_SET, readahead=None):
        self._off = self.client.lseek(self._fd, pos, how, readahead=readahead)
        return self._off

    def tell(self):
        return self._off

    def _bytesLeft(self):
        off = self._off
        flen = self.seek(0, os.SEEK_END)
        self.seek(off)
        return flen - off

    def read(self, size=None, readahead=None):
        if size is None:
            size = self._bytesLeft()

        buf = memoryview(bytearray(size))
        red = self.readinto(buf, readahead=readahead)
        return buf[:red]

    def readinto(self, buf, readahead=None):
        red = self.client.read(self._fd, buf, readahead=readahead)
        self._off += red
        return red

    def write(self, buf, flush=False):
        writ = self.client.write(self._fd, buf, flush=flush)
        self._off += writ
        return writ
