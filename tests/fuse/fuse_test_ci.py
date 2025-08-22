import struct
import os
import mmap
import array
import time
import threading
try:
    import numpy as np
except ImportError:
    np = None


file_modes = ['r', 'rb', 'r+', 'rb+', 'w', 'wb', 'w+', 'a', 'ab', 'a+', 'ab+']
path = ""
text_path = ""
binary_path = ""

def check_read(fd, path, val, desc):
    if fd == None:
        fd = open(path, 'r')
    v = fd.read(len(val))
    if v != val:
        raise RuntimeError(v, val, 'Check Error: ', desc)
    fd.close()

def write_file(fd, val, cls = False):
    fd.write(val)
    fd.flush()
    if cls:
        fd.close()

def print_file_data(fd, path, num):
    if fd == None:
        fd = open(path, 'r')
    fd.read(num)
    print('print_file_data', num, fd.read(num))
    fd.close()

def open_test():
    fd = open(text_path+'1', 'w+')
    write_file(fd, 'abcdefghigk')
    check_read(None, text_path+'1', 'abc', 'open w+ mode')
    
    fd2 = open(text_path+'2', 'w+')
    fd2.close()
    
    fd = open(text_path+'1', 'r')
    check_read(fd, '', 'abc', 'open r mode')
    fd = open(text_path+'1', 'rb')
    check_read(fd, '', b'abc', 'open rb mode')
    fd = open(text_path+'1', 'r+')
    check_read(fd, '', 'abc', 'open r+ mode')
    fd = open(text_path+'1', 'rb+')
    check_read(fd, '', b'abc', 'open rb+ mode')

    #正常写文本文件
    fd = open(text_path+'2_w', 'w')
    write_file(fd, 'cba')
    check_read(None, text_path+'2_w', 'cba', 'open w mode')
    #将文本写入二进制文件
    fd = open(binary_path+'2_w', 'wb')
    #print_file_data(None, binary_path+'2', 6)
    write_file(fd, b'efg')
    check_read(None, binary_path+'2_w', 'efg', 'open wb mode')
    #读写打开，用同一个fd读写，校验
    fd = open(text_path+'2_w', 'w+')
    write_file(fd, 'etc', True)
    check_read(None, text_path+'2_w', 'etc', 'open w+ mode')

    #追加文本文件
    fd = open(text_path+'2_w', 'a')
    write_file(fd, 'zzz', True)
    check_read(None, text_path+'2_w', 'etczzz', 'open a mode')
    #追加二进制文件
    fd = open(binary_path+'2_w', 'ab')
    write_file(fd, b'zzz', True)
    check_read(None, binary_path+'2_w', 'efgzzz', 'open ab mode')
    #读写文本文件，追加后再读取
    fd = open(text_path+'2_w', 'a+')
    write_file(fd, 'yyy', True)
    check_read(None, text_path+'2_w', 'etczzzyyy', 'open a+ mode')
    #读写二进制文件，追加后再读取
    # fd = open(binary_path+'389', 'ab+')
    # write_file(fd, b'xxx', True)
    # check_read(None, binary_path+'389', 'xxx', 'open ab+ mode')
    # fd.close()
    #测试非法模式
    is_err = True
    try:
        fd = open(binary_path+'389', 'aa')
    except ValueError as err:
        is_err = False
        print('open_test 1: {0}'.format(err))
    if is_err:
        raise RuntimeError('open_test 1 Error')

    os.unlink(text_path+'2_w')
    print('open test over')

def seek_test():
    fd = open(text_path+'1', 'r+')
    #fd.seek(-1)
    #获取文件长度
    fd.seek(0, 2)
    len = fd.tell()
    print(text_path+'1', ' file_len = ', len)
    #seek到文件末尾之后,是成功的
    idx = fd.seek(len+2, 0)
    fd.read(2)
        #raise RuntimeError('seek_test len+1,', idx)
    #直接seek到最后+1，异常
    is_err = True
    try:
        fd.seek(1, 2)
    except ValueError as err:
        is_err = False
        print('seek_test 2: {0}'.format(err))
    if is_err:
        raise RuntimeError('seek_test 2 Error')
    fd.close()

    print('seek test over')

def flush_test():
    fd = open(text_path+'2', 'r')
    fd.flush()
    fd = open(text_path+'2_w', 'wb')
    fd.flush()

    print('flush test over')

def next_test():
    fd = open(text_path+'2_w', 'w+')
    #空文件获取行
    is_err = True
    try:
        val = next(fd)
    except StopIteration as err:
        is_err = False
        print('next_test 1: {0}'.format(err))
    if is_err:
        raise RuntimeError('next_test 1 Error')
    #写入不同数据，顺序读取
    fd.write('abc\nttt\nbbb\nccc\nggg')
    fd.flush()
    fd.seek(0)
    val = next(fd)
    if val.strip() != 'abc':
        raise RuntimeError('next_test 2 Error')
    val = next(fd)
    if val.strip() != 'ttt':
        raise RuntimeError('next_test 3 Error')
    val = next(fd)
    if val.strip() != 'bbb':
        raise RuntimeError('next_test 4 Error')

    fd.close()
    os.remove(text_path+'2_w')
    print('next test over')

def read_test():
    fd = open(text_path+'1', 'r+')
    #读取长度为0的数据
    val = fd.read(0)
    if len(val) != 0:
        raise RuntimeError('read_test 1,', val)
    #在文件最后位置读取数据
    fd.seek(0, 2)
    val = fd.read(1)

    if len(val) != 0:
        raise RuntimeError('read_test 2 Error')

    fd.close()

    #在写权限的文件读取
    is_err = True
    fd = open(text_path+'2_w', 'w')
    try:
        fd.read(1)
    except IOError as err:
        is_err = False
        print('read_test 3: {0}'.format(err))

    if is_err:
        raise RuntimeError('read_test 3 Error')
    print('read test over')

def truncate_test():
    #读权限截取
    fd = open(text_path+'2_w', 'r')
    is_err = True
    try:
        fd.truncate()
        fd.close()
    except IOError as err:
        is_err = False
        print('truncate_test 1: {0}'.format(err))
    if is_err:
        raise RuntimeError('truncate_test 1 Error')

    #正常截取
    fd = open(text_path+'2_w', 'w+')
    fd.truncate()
    fd.write('abcd\nefg')
    #当前位置截取
    fd.seek(4, 0)
    fd.truncate()
    fd.seek(0, 0)
    val = fd.read(4)
    print('val = ', val)
    if val != 'abcd':
        raise RuntimeError('truncate_test 2 Error')
    #非法位置截取
    fd.truncate(10)

    fd.close()
    print('truncate test over')

def tell_test():
    #正常获取
    fd = open(text_path+'1', 'r')
    fd.seek(0, 2)
    len = fd.tell()
    fd.seek(1, 0)
    fd.read(5)
    fd.seek(0, 1)
    idx = fd.tell()
    if idx != 6:
        raise RuntimeError('tell_test 1 Error')
    #超出文件末尾获取
    fd.seek(len+1, 0)
    idx = fd.tell()
    if idx != len+1:
        raise RuntimeError('tell_test 2 Error')

    print('tell test over')

def readlines_test():
    fd = open(text_path+'2_w', 'w+')
    #空文件获取行
    val = fd.readline(-1)
    fd.write('abc\nttt\nbbb\nccc\nggg')
    fd.flush()
    fd.seek(0, 2)
    f_len = fd.tell()
    fd.seek(0)
    val = fd.readline(-1)
    if val.strip() != 'abc':
        raise RuntimeError('readlines_test 1 Error')
    val = fd.readline(0)
    if len(val) != 0:
        raise RuntimeError('readlines_test 2 Error')

    val = fd.readline(2)
    if val.strip() != 'tt':
        raise RuntimeError('readlines_test 3 Error')

    val = fd.readline(f_len + 5)
    if val.strip() != 't':
        raise RuntimeError('readlines_test 4 Error')

    print('readlines test over')

def mmap_test():
    fd = os.open(text_path+'1', os.O_RDWR | os.O_NONBLOCK)
    mm = mmap.mmap(fd, 0)
    if mm[:6] != b'abcdef':
        raise RuntimeError('mmap_test 1 Error')
    os.close(fd)
    print('mmap test over')

def os_open_test():
    fd = os.open(text_path+'2_rw', os.O_RDWR | os.O_CREAT | os.O_TRUNC)
    b1 = bytearray(b'123')
    b2 = bytearray(b'abc')
    b3 = bytearray(b'456')
    bufs = []
    bufs.append(b1)
    bufs.append(b2)
    bufs.append(b3)
    os.writev(fd, bufs)
    os.lseek(fd, 0, 0)
    val = os.read(fd, 5)
    if val != b'123ab':
        raise RuntimeError('os_open_test 1 Error')
    os.close(fd)

    #open文件，但bufsize超出长度
    fd = os.open(text_path+'2_rw', os.O_RDONLY, 0)
    val = os.read(fd, 20)
    if val != b'123abc456':
        raise RuntimeError('os_open_test 2 Error')
    os.close(fd)

    print('os_open test over')

def os_mkdir_test():
    #创建文件夹
    try:
        os.mkdir(text_path+'test_file')
        fd = open(text_path+'test_file/1_t', 'w')
        fd.close()
        #在文件夹里有文件的情况下删除
        os.rmdir(text_path+'test_file')
    except OSError as err:
        print('os_mkdir_test 1: {0}  预期的结果'.format(err))

    os.unlink(text_path+'test_file/1_t')
    os.rmdir(text_path+'test_file')
    if os.path.exists(text_path+'test_file2'):
        os.rmdir(text_path+'test_file2')

    print('os_mkdir test over')

def os_link_test():
    try:
        os.mkdir(text_path+'test_file')
        os.mkdir(text_path+'test_file2')
        fd = open(text_path+'test_file/1_t', 'w')
        os.link(text_path+'test_file/1_t', text_path+'test_file2/1_t2')
    except PermissionError as err:
        print('os_link_test 1: {0}'.format(err))

    print('os_link test over')

def os_unlink_test():
    os.unlink(text_path+'test_file/1_t')
    os.unlink(text_path+'test_file2/1_t2')
    os.rmdir(text_path+'test_file')
    os.rmdir(text_path+'test_file2')

    print('os_unlink test over')

letter_ary = []
binary_ary = []
for ch in range(97, 123):
    letter_ary.append(chr(ch))
    binary_ary.append(ch)

def thread_check_text(start, offset, ary, text_data):
    for i in range(offset):
        t = (start + i) % 26
        #print(t, text_data, text_data[i])
        if ary[t] != text_data[i]:
            raise RuntimeError('text_data check error: {0}, {1}, {2} right: {3}, real:{4}'.format( start, offset, i, ary[t], text_data[i]))

def thread_check_binary(start, offset, binary_data):
    for i in range(offset):
        t = (start + i + 1) % 126
        if t == 0:
            t = 126
        if t != binary_data[i]:
            raise RuntimeError('binary_data check error: start {0} offset {1} index {2} expected: {3} actual: {4}'.format( start, offset, i, t, binary_data[i]))

def thread_check_text_np(start, offset, ary, text_data):
    expected = np.arange(start, start + offset, dtype=np.int32)
    expected %= 26
    expected += ary[0]
    # actual = np.array(text_data, dtype=np.int32)
    actual = np.frombuffer(text_data, dtype=np.uint8)
    if not np.array_equal(expected, actual):
        i = np.argmin(expected == actual)
        raise RuntimeError('text_data check error: start {0} offset {1} index {2} expected: {3} actual: {4}'.format( start, offset, i, expected[i:i+16], actual[i:i+16]))

def thread_check_binary_np(start, offset, binary_data):
    expected = np.arange(start + 1, start + offset + 1, dtype=np.int32)
    expected %= 126
    expected[expected == 0] = 126
    # actual = np.array(binary_data, dtype=np.int32)
    actual = np.frombuffer(binary_data, dtype=np.uint8)
    if not np.array_equal(expected, actual):
        i = np.argmin(expected == actual)
        raise RuntimeError('binary_data check error: start {0} offset {1} index {2} expected: {3} actual: {4}'.format( start, offset, i, expected[i:i+16], actual[i:i+16]))

#文本校验
def check_text_data(start, offset, text_data):
    if offset != len(text_data):
        raise RuntimeError('check_text_data parameter error: offset != len(text_data), {0} {1} {2}'.format(offset, len(text_data), start))

    return

    if type(text_data).__name__ == 'bytes':
        ary = binary_ary
    else:
        ary = letter_ary

    if len(text_data) <= 1024:
        thread_check_text(start, offset, ary, text_data)
    else:
        thread_check_text_np(start, offset, ary, text_data)

    # p = threading.Thread(target=thread_check_text_np, args=(start, offset, ary, text_data,))
    # p.start()

    # for i in range(offset):
    #     t = (start + i) % 26
    #     #print(t, text_data, text_data[i])
    #     if ary[t] != text_data[i]:
    #         raise RuntimeError('text_data check error: {0}, {1}, {2} right: {3}, real:{4}'.format( start, offset, i, ary[t], text_data[i]))

#二进制校验
def check_binary_data(start, offset, binary_data):
    if offset != len(binary_data):
        raise RuntimeError('check_binary_data parameter error: offset != len(binary_data), {0} {1}'.format(offset, len(binary_data)))

    if len(binary_data) <= 1024:
        thread_check_binary(start, offset, binary_data)
    else:
        thread_check_binary_np(start, offset, binary_data)

    # p = threading.Thread(target=thread_check_binary_np, args=(start, offset, binary_data,))
    # p.start()

    # for i in range(offset):
    #     t = (start + i + 1) % 126
    #     if t == 0:
    #         t = 126
    #     if t != binary_data[i]:
    #         raise RuntimeError('binary_data check error: {0}, {1}, {2} right: {3}, real:{4}'.format( start, offset, i, t, binary_data[i]))

def check_readv_results(file_path, read_offset, data_bufs, data_len):
    index = read_offset

    readbufsize = sum(len(buf) for buf in data_bufs)
    filesize = os.path.getsize(file_path)
    if read_offset > filesize :
        assert data_len == 0, "read start exceeding file end, return size should be 0. now it's %d " % (data_len)
        return

    expected_return_size = readbufsize if (read_offset + readbufsize <= filesize) else (filesize - read_offset)
    assert  data_len == expected_return_size, "expected size %d, result return size %d" % (expected_return_size, data_len)

    #print('data_bufs size = ', len(data_bufs[0]))
    #print(data_bufs)
    for buf in data_bufs:
        l = len(buf)
        if l <= data_len:
            val = bytes(buf)
        else:
            val = bytes(buf[:data_len])
            l = data_len
        #print('val = ', val, data_len)
        data_len -= l
        #print(index, l, val[:128])
        if file_path.find('binary') != -1:
            check_binary_data(index, l, val)
        else:
            check_text_data(index, l, val)
        index += l
        if data_len <= 0:
            break

def os_read_test_base(file_path, index, size, file_len = -1):
    fd = os.open(file_path, os.O_RDONLY )
    os.lseek(fd, index, 0)
    val = os.read(fd, size)
    os.close(fd)

    readbufsize = size
    filesize = os.path.getsize(file_path)
    read_offset = index
    data_len = len(val)

    if read_offset > filesize :
        assert data_len == 0, "read start exceeding file end, return size should be 0. now it's %d " % (data_len)
        return

    expected_return_size = readbufsize if (read_offset + readbufsize <= filesize) else (filesize - read_offset)
    assert  data_len == expected_return_size, "expected size %d, result return size %d" % (expected_return_size, data_len)

    #print(index, l, val[:128])
    if file_path.find('binary') != -1:
        check_binary_data(index, data_len, val)
    else:
        check_text_data(index, data_len, val)

def os_readv_test_base(file_path, index, buffers_size, file_len = -1):
    buffers = []
    size = 0
    for s in buffers_size:
        buf = bytearray(s)
        buffers.append(buf)
        size += s
    print('bufs = ', buffers_size)
    fd = os.open(file_path, os.O_RDONLY  ) #| os.O_DIRECT
    os.lseek(fd, index, 0)
    num_bytes = os.readv(fd, buffers)
    os.close(fd)

    check_readv_results(file_path, index, buffers, num_bytes)

def test_continuous_readv(fd, index, buffers_size):
    buffers = []
    size = 0
    for s in buffers_size:
        buf = bytearray(s)
        buffers.append(buf)
        size += s
    print('bufs = ', buffers_size)
    os.lseek(fd, index, 0)
    num_bytes = os.readv(fd, buffers)
    #print('buffers size = ', len(buffers), num_bytes, size, buffers)
    if num_bytes == size:
        for buf in buffers:
            l = len(buf)
            val = bytes(buf)
            check_text_data(index, l, val)
            index += l
    else:
        raise RuntimeError('os_readv_test_base error: num_bytes != size {0}, {1}'.format( num_bytes, size))

def posix_io_text():
    print("=== [start test suite] posix_io_test basic funtions ===")
    open_test()
    seek_test()
    flush_test()
    next_test()
    read_test()
    truncate_test()
    tell_test()
    readlines_test()
    mmap_test()
    os_open_test()
    os_mkdir_test()
    os_link_test()
    os_unlink_test()
    print("=== [finish test suite] posix_io_test basic functions ===")

def os_write_run(path, open_mode, max_file_size, write_data):
    fd = os.open(path, open_mode)
    idx = 0
    print("write started", path, max_file_size)
    while idx < max_file_size:
        os.write(fd, write_data)
        idx += len(write_data)
    os.close(fd)
    print("write done", path, max_file_size)

def os_read_run(path, open_mode, max_file_size):
    fd = os.open(path, open_mode)
    idx = 0
    data = array.array('B')
    check_data = check_text_data if path.find('text') != -1 else check_binary_data

    print("read started", path, max_file_size, flush=True)
    while idx < max_file_size - 26:
        val = os.read(fd, 26)
        data.frombytes(val)
        l = len(val)
        idx += l
        if l == 0:
            print("wait for more data...")
            time.sleep(0.5)

        if idx % (max_file_size // 20) < 26:
            print(idx * 100 // max_file_size, 'percent done', 'idx', idx, 'max file size', max_file_size, flush=True)

    print("read done", path, flush=True)
    os.close(fd)
    print("check data", flush=True)
    check_data(0, len(data), data)

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("path", help="The path argument")
    args = parser.parse_args()
    path = args.path
    text_path = os.path.join(path, "text")
    binary_path = os.path.join(path, "binary")

    print('Path', path)
    print('posix test')
    print('text_path = ', text_path)
    print('binary_path = ', binary_path)
    print('letter_ary = ', letter_ary)
    print('binary_ary = ', binary_ary)

    posix_io_text()
    #
    # os_read_test()
    # os_readv_test()


