import json
import random

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
    if config["num_ops"] < 0:
        print("Negative number of operation not allowed")
        return None
    return config


def write(config, dup: bool):
    """Execute a write operation."""
    print(f"WRITE operation (duplicate={dup})")


def read(config, dup: bool):
    """Execute a read operation."""
    print(f"READ operation (duplicate={dup})")


def unlink(config, dup: bool):
    """Execute an unlink operation."""
    print(f"UNLINK operation (duplicate={dup})")


def workload(config):
    random.seed(42)
    for i in range(0, config["num_ops"]):
        operation_rand = random.randint(0, 100)
        dup_rand = random.randint(0, 100)
        dup = dup_rand < config["dup_pcnt"]
        
        write_threshold = config["write_pcnt"]
        read_threshold = write_threshold + config["read_pcnt"]
        
        if operation_rand < write_threshold:
            write(config, dup)
        elif operation_rand < read_threshold:
            read(config, dup)
        else:
            unlink(config, dup)


def main():

    config = get_config()

    if config is None:
        print("Invalid configuration")
        quit

    print("Configuration is valid!\nProceeding with the benchmark...")

    workload(config)


if __name__ == '__main__':
    main()
