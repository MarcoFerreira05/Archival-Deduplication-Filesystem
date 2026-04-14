import os
import random

class State():
    def __init__(self, config):
        """
        Extracts all the parameters from config and prepares the memory buffers for the blocks.
        """
        random.seed(2005)
        # extract config
        self.write_pcnt: int = config["write_pcnt"]
        self.read_pcnt: int = config["read_pcnt"]
        self.unlink_pcnt: int = config["unlink_pcnt"]
        self.dup_pcnt: int = config["dup_pcnt"]
        self.block_range: list[int] = config["block_range"]
        self.num_ops: int = config["num_ops"]
        self.num_files: int = config["num_files"]
        self.num_unique_dup_blocks: int = config["num_unique_dup_blocks"]

        # file tracking

        self.files = [
            {"fd": -1, "path": f"test_file{i}", "size": 0, "exists": False}
            for i in range(self.num_files)
        ]

        # prepare memory buffers

        self.dup_blocks: list[bytearray] = []
        for i in range(self.num_unique_dup_blocks):
            block = bytearray(4096)
            mv = memoryview(block)
            v = i + 1
            for i in range(4096):
                mv[i] = v
            self.dup_blocks.append(block)

        self.unique_block: bytearray = bytearray(4096)
        mv = memoryview(self.unique_block)
        self.unique_value: int = self.num_unique_dup_blocks + 1
        for i in range(4096):
            mv[i] = self.unique_value
        self.unique_block_index: int = 0

        # this buffer is reused everytime we want to build a write request, so it has the maximum capacity
        self.buffer: bytearray = bytearray(4096*self.block_range[1])
        self.buffer_mv: memoryview = memoryview(self.buffer)

    def get_num_blocks(self) -> int:
        """
        Returns a random number of blocks between the configured range.
        """
        return random.randint(self.block_range[0], self.block_range[1])

    def dup_or_not(self) -> bool:
        """
        Determines if a block should be duplicate or not, based on the configured percentage.
        """
        dup_rand = random.randint(0, 100)
        return dup_rand < self.dup_pcnt

    def get_random_file(self):
        """
        Returns the file dictionary of a random test file.
        """
        return random.choice(self.files)

    def append_dup_block(self, block_index: int):
        """
        Appends a duplicate block (from the set of generated ones on initialization) to the internal buffer
        """
        rand_block_index = random.randint(0, self.num_unique_dup_blocks-1)
        dup_block = self.dup_blocks[rand_block_index]
        start = block_index * 4096
        end = start + 4096
        self.buffer[start:end] = dup_block

    def append_unique_block(self, block_index: int):
        """
        Appends a unique block to the internal buffer and updates the unique block.
        """
        start = block_index * 4096
        end = start + 4096
        self.buffer[start:end] = self.unique_block

        # keep the unique block unique
        self.unique_block[self.unique_block_index] = self.unique_value
        self.unique_block_index += 1
        if self.unique_block_index == 4096:
            self.unique_block_index = 0
            self.unique_value += 1
            if self.unique_value == 256:
                self.unique_value = 1

    def get_buffer(self, size: int) -> memoryview:
        """
        Returns a memoryview of the reusable buffer, sliced to the according size.
        """
        return self.buffer_mv[:size]

    def free(self):
        for file in self.files:
            if file["fd"] != -1:
                os.close(file["fd"])

