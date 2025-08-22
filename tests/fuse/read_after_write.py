import os
import random
import argparse
import time

def generate_random_data(length):
    # Generate random data of length 'length'
    data = bytearray(os.urandom(length))
    return data

def write_random_data(file_path, data):
    # Write data to file
    with open(file_path, 'wb') as file:
        file.write(data)

def validate_data(file_path, data):
    st = os.stat(file_path)
    assert st.st_size == len(data), f'{file_path} {st.st_size} != {len(data)}'

    # Open the file and validate if the data matches the original data
    with open(file_path, 'rb') as file:
        read_data = file.read()

    assert read_data == data, f"validation failed for {file_path}"
    print(f"validation success for {file_path}")

if __name__ == '__main__':
    # Use argparse to accept parameters
    parser = argparse.ArgumentParser(description='Generate specified number and size of random files and validate data correctness')
    parser.add_argument('--filesize', type=int, default=32 * 1024 * 1024, help='Maximum size of each file in bytes, default is 32MB')
    parser.add_argument('--filenum', type=int, default=10, help='Number of files to generate, default is 1000')
    parser.add_argument('--seconds', type=int, default=0, help='Run seconds')
    parser.add_argument('path', type=str, help='Path for test run')
    args = parser.parse_args()

    max_file_size = args.filesize
    file_num = args.filenum
    test_path = args.path

    start = time.time()
    
    i = 0
    while time.time() - start < args.seconds or i < file_num:
        file_size = random.randint(1, max_file_size)
        data = generate_random_data(file_size)

        file_name = f"file_{file_size}_{i}.bin"
        file_path = os.path.join(test_path, file_name)

        write_random_data(file_path, data)
        validate_data(file_path, data)

        i+= 1