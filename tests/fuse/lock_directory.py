import sys
import os
import errno
import uuid

def random_fname():
    unique_id = uuid.uuid4()
    return str(unique_id).replace("-", "")

def assert_errno(expected, fn, *args):
    code = 0
    try:
        fn(*args)
    except Exception as e:
        code = getattr(e, "errno", errno.EINVAL)
    assert code == expected, f"{code} != {expected}"

# Note:    
# setxattr: hf3fs.lock -> [try_lock, unlock, preempt_lock, clear]
# getxattr: hf3fs.lock -> ENODATA if not locked else json string '{"client":{"uuid":"...","hostname":"..."}}'
# removexattr('f3fs.lock') == setxattr(path, 'hf3fs.lock', b'clear')

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Please provide exactly two mount points as arguments.")
        sys.exit(-1)

    # two clients mount same filesystem
    mnt1 = sys.argv[1]
    mnt2 = sys.argv[2]
    
    # return ENOTSUP should affect subsequent operations.
    assert_errno(errno.ENOTSUP, os.setxattr, mnt1, 'system.acl', b'soemthing')
    assert_errno(errno.ENOTSUP, os.setxattr, mnt1, 'hf3fs.invalid_key', b'soemthing')

    os.removexattr(mnt1, 'hf3fs.lock')
    os.removexattr(mnt2, 'hf3fs.lock')
    assert_errno(errno.ENODATA, os.getxattr, mnt1, 'hf3fs.lock')
    assert_errno(errno.ENODATA, os.getxattr, mnt2, 'hf3fs.lock')

    # directory not locked, both client1 & client2 can create
    os.open(os.path.join(mnt1, random_fname()), os.O_CREAT | os.O_RDONLY, 0o644)
    os.open(os.path.join(mnt2, random_fname()), os.O_CREAT | os.O_RDONLY, 0o644)

    # client1 try_lock
    os.setxattr(mnt1, 'hf3fs.lock', b'try_lock')
    os.open(os.path.join(mnt1, 'file'), os.O_CREAT | os.O_RDONLY, 0o644)
    attr = os.getxattr(mnt1, 'hf3fs.lock')
    assert attr != ''
    print(f'getxattr hf3fs.lock: {attr}')
    attrs = os.listxattr(mnt1)
    assert len(attrs) != 0
    print(f'listxattr {attrs}')

    # client2 can't try_lock or create
    assert_errno(errno.ENOLCK, os.setxattr, mnt2, 'hf3fs.lock', b'try_lock')
    assert_errno(errno.ENOLCK, os.open, os.path.join(mnt2, random_fname()), os.O_CREAT | os.O_RDONLY, 0o644)
    assert_errno(errno.ENOLCK, os.link, os.path.join(mnt2, 'file'), os.path.join(mnt2, random_fname()))
    assert_errno(errno.ENOLCK, os.remove, os.path.join(mnt2, 'file'))

    # client2 can preempt lock
    os.setxattr(mnt2, 'hf3fs.lock', b'preempt_lock')
    os.open(os.path.join(mnt2, random_fname()), os.O_CREAT | os.O_RDONLY, 0o644)
    os.link(os.path.join(mnt2, 'file'), os.path.join(mnt2, random_fname()))
    os.remove(os.path.join(mnt2, 'file'))
    assert_errno(errno.ENOLCK, os.setxattr, mnt1, 'hf3fs.lock', b'unlock')

    # client2 unlock, both client1 & client2 can create again
    os.setxattr(mnt2, 'hf3fs.lock', b'unlock')
    os.open(os.path.join(mnt1, random_fname()), os.O_CREAT | os.O_RDONLY, 0o644)
    os.open(os.path.join(mnt2, random_fname()), os.O_CREAT | os.O_RDONLY, 0o644)

    print(f'test passed')