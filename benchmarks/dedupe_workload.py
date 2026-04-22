import json
import random
import os
from benchmark_state import State

total_non_dup = 0
total_dup = 0

def get_config():
    """
    Reads the configuration from config.json and validates it.
    """
    with open("config.json") as f:
        text = f.read()
        config = json.loads(text)
    total_pcnt = config["write_pcnt"] + config["read_pcnt"] + config["unlink_pcnt"]
    if total_pcnt != 100:
        print("Operation percentages don't add up to 100")
        return None
    if config["dup_pcnt"] > 100 or config["dup_pcnt"] < 0:
        print("Invalid duplicate percentage")
        return None
    block_range = config.get("block_range")
    if (
        not isinstance(block_range, list)
        or len(block_range) != 2
        or not all(isinstance(v, int) for v in block_range)
    ):
        print("Invalid block range, expected [min_blocks, max_blocks]")
        return None
    if block_range[0] < 1 or block_range[1] < block_range[0]:
        print("Invalid range limits, minimum must be at least 1 block (4096 bytes)")
        return None
    if config["num_ops"] <= 0:
        print("Number of operations must be greater than 0")
        return None
    if config["num_files"] <= 0:
        print("Number of files must be greater than 0")
        return None
    if config["num_unique_dup_blocks"] <= 0:
        print("Number of unique duplicate blocks must be greater than 0")
        return None
    test_file_path_format = config.get("test_file_path_format")
    if not isinstance(test_file_path_format, str) or test_file_path_format == "":
        print("Invalid test file path format")
        return None
    try:
        _ = test_file_path_format.format(index=0)
    except (KeyError, IndexError, ValueError):
        print("Invalid test file path format, expected a Python format string using {index}")
        return None
    return config


def write(state: State):
    """
    Executes a write operation in a random file.
    Appends a random number (between the configured range) of blocks, each of them with the
    configured probability of being duplicate.
    """
    global total_dup, total_non_dup

    # first build the whole request in a single buffer
    
    num_blocks = state.get_num_blocks()
    num_bytes = num_blocks * 4096

    for i in range(num_blocks):

        dup = state.dup_or_not()

        if dup:
            state.append_dup_block(i)
            total_dup += 1
        else:
            state.append_unique_block(i)
            total_non_dup += 1

    # now execute the write operation at once
    file = state.get_random_file()
    if file["fd"] == -1:
        file["fd"] = os.open(file["path"], os.O_RDWR | os.O_APPEND | os.O_CREAT, mode=0o666)
        file["exists"] = True
    buf = state.get_buffer(num_blocks * 4096)
    bytes_written = os.write(file["fd"], buf)
    if bytes_written != num_bytes:
        print("Number of bytes written doesn't match the expected!")

    # update the file info
    file["size"] += num_bytes


def read(state: State):
    """
    Executes a read operation on a random file.
    Reads a random number (between the configured range) of blocks at a random offset.
    """

    file = state.get_random_file()
    fd = file["fd"]
    if fd == -1 or not file["exists"]:
        return

    num_blocks = state.get_num_blocks()
    max_blocks = file["size"] // 4096

    num_blocks = min(num_blocks, max_blocks)
    max_offset_blocks = max_blocks - num_blocks
    block_offset = random.randint(0, max_offset_blocks)

    offset = block_offset * 4096
    size = num_blocks * 4096
    buf = state.get_buffer(size)

    os.lseek(fd, offset, os.SEEK_SET)
    result = os.read(fd, size)
    #result = os.pread(fd, size, offset)
    if len(result) != size:
        print("A read request returned a result with an incorrect length!!!")


def unlink(state: State):
    """
    Executes an unlink operation on a random file, closing the file descriptor.
    """

    file = state.get_random_file()
    fd = file["fd"]
    path = file["path"]

    if fd != -1:
        _ = os.close(file["fd"])
    if file["exists"]:
        _ = os.unlink(file["path"])
        file["exists"] = False

    file["fd"] = -1
    file["size"] = 0



def workload(state: State):
    print("Workload started...")
    random.seed(42)
    for _ in range(state.num_ops):
        operation_rand = random.randint(0, 99)
        
        write_threshold = state.write_pcnt
        read_threshold = write_threshold + state.read_pcnt
        
        if operation_rand < write_threshold:
            write(state)
        elif operation_rand < read_threshold:
            read(state)
        else:
            unlink(state)

    print("Workload done")


def main():

    config = get_config()
    if config is None:
        print("Invalid configuration")
        quit()
    print("Configuration is valid!\nProceeding with the benchmark...")

    state = State(config)
    workload(state)
    state.free()

    print("Benchmark done!")
    print(f"Blocks written with a repeated pattern (dedup candidates): {total_dup}")
    print(f"Blocks written with a unique pattern (non-dedup): {total_non_dup}")
    print(f"Total blocks written: {total_dup + total_non_dup}")


if __name__ == '__main__':
    main()
