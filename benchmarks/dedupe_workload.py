import json
import random
import os

def get_config():
    """
    Reads the configuration from config.json and validates it.
    """
    with open("config.json") as f:
        text = f.read()
        config = json.loads(text)
    total_prob = config["write_pcnt"] + config["read_pcnt"] + config["unlink_pcnt"]
    if total_prob != 100:
        print("Operation percentages don't add up to 100")
        return None
    if config["dup_pcnt"] > 100 or config["dup_pcnt"] < 0:
        print("Invalid duplicate percentage")
        return None
    if config["block_range"][0] < 0 or config["block_range"][1] < config["block_range"][0]:
        print("Invalid range limits")
        return None
    if config["num_ops"] <= 0:
        print("Number of operations must be greater than 0")
        return None
    if config["num_files"] <= 0:
        print("Number of files must be greater than 0")
        return None
    return config


def init(num_files: int) -> list[int]:
    """
    Creates the list of file descriptors, initializing it with -1s.
    """
    files: list[int] = []
    for _ in range(0, num_files):
        files.append(-1)
    return files


def write(range_min: int, range_max: int, dup_pcnt: int, files: list[int]):
    """
    Executes a write operation in a random file.
    Appends a random number (between the configured range) of blocks, each of them with the
    configured probability of being duplicate.
    """

    num_blocks = random.randint(range_min, range_max)

    for _ in range(0, num_blocks):

        dup_rand = random.randint(0, 100)
        dup = dup_rand < dup_pcnt

        file_index = random.randint(0, len(files)-1)
        fd = files[file_index]

        if fd == -1:
            fd = os.open(f"test_file{file_index}", os.O_WRONLY | os.O_APPEND | os.O_CREAT, mode=666)
            files[file_index] = fd

        if dup:
            print(f"Appending duplicate content on fd {fd}")
        else:
            print(f"Appending non-duplicate content on fd {fd}")
        

def read(range_min: int, range_max: int):
    """Execute a read operation."""


def unlink():
    """Execute an unlink operation."""


def workload(config, files: list[int]):
    print("Workload started...")
    random.seed(42)
    for i in range(0, config["num_ops"]):
        operation_rand = random.randint(0, 100)
        
        write_threshold = config["write_pcnt"]
        read_threshold = write_threshold + config["read_pcnt"]
        
        if operation_rand < write_threshold:
            write(config["block_range"][0], config["block_range"][1], config["dup_pcnt"], files)
        elif operation_rand < read_threshold:
            read(config)
        else:
            unlink(config)

    print("Workload done")


def finish(files: list[int]):
    print("Closing all file descriptors...")
    for fd in files:
        if fd != -1:
            os.close(fd)
    print("Done")

def main():

    config = get_config()

    if config is None:
        print("Invalid configuration")
        quit()

    print("Configuration is valid!\nProceeding with the benchmark...")

    files = init(config["num_files"])

    workload(config, files)

    finish(files)


if __name__ == '__main__':
    main()
