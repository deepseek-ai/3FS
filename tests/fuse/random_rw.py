import os
import random
import argparse
import subprocess

def generate_random_data(length):
    data = bytearray(os.urandom(length))
    return data
def write_random_blocks(file_path, data, close):
    blocks = []
    offset = 0
    while offset < len(data):
        length = min(random.randint(1, 4 << 20), len(data) - offset)
        blocks.append((offset, length))
        offset += length
    random.shuffle(blocks)
    
    fd = os.open(file_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    for offset, length in blocks:
        print("{:10d} {:10d}".format(offset, offset + length))
        os.pwrite(fd, data[offset:(offset + length)], offset)
    if not close:
        return fd
    os.close(fd)

def run_command(command):
    try:
        output = subprocess.check_output(command, shell=True, text=True)
        print(f"{command} output:")
        print(output)
    except subprocess.CalledProcessError as e:
        print("Error occurred while running the external program:", e)

def dump_buf(buf, data):
    with open('random_rw.buf', 'wb') as f:
        f.write(buf)
    with open('random_rw.data', 'wb') as f:
        f.write(data)
    run_command("cmp -l random_rw.buf random_rw.data | head")
    run_command("cmp -l random_rw.buf random_rw.data | tail")

def validate_random_access(file_path, data):
    total = 0

    st = os.stat(file_path)
    assert st.st_size == len(data), f'{st.st_size} != {len(data)}'

    fd = os.open(file_path, os.O_RDONLY)
    while total < 2 * len(data):
        offset = random.randint(0, len(data))
        length = min(random.randint(1, 4 << 20), len(data) - offset)
        buf = os.pread(fd, length, offset)
        total += length
        if buf != data[offset:(offset + length)]:
            print(f"validation failed for {file_path}, offset {offset}, length {length}, buf {len(buf)}")
            dump_buf(buf, data[offset:(offset + length)])
            assert False

    buf = os.pread(fd, len(data), 0)
    if buf != data:
        print(f"validation failed for {file_path}, buf {len(buf)}, data {len(data)}")
        dump_buf(buf, data)
        assert False
    os.close(fd)

    print(f"validation success for {file_path}")

if __name__ == '__main__':
    # Use argparse to accept parameters
    parser = argparse.ArgumentParser(description='Generate specified number and size of random files and validate data correctness')
    parser.add_argument('--filesize', type=int, default=128 * 1024 * 1024, help='Maximum size of each file in bytes, default is 128MB')
    parser.add_argument('--filenum', type=int, default=10, help='Number of files to generate, default is 1000')
    parser.add_argument('--read_before_close', action='store_true', default=False, help='Enable the flag')
    parser.add_argument('path', type=str, help='Path for test run')
    args = parser.parse_args()

    max_file_size = args.filesize
    file_num = args.filenum
    test_path = args.path

    for i in range(file_num):
        file_size = random.randint(1, max_file_size)
        data = generate_random_data(file_size)

        file_name = f"file_{file_size}_{i}.bin"
        file_path = os.path.join(test_path, file_name)

        data = generate_random_data(file_size)
        fd = write_random_blocks(file_path, data, not args.read_before_close)
        validate_random_access(file_path, data)
        if args.read_before_close:
            os.close(fd)
