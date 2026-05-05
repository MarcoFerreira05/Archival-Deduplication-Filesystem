#ifndef DEDUP_H
#define DEDUP_H

#include <stdint.h>
#include <sys/types.h>

typedef struct index Index;
#define INSERTION_SORT_THRESHOLD 32
typedef struct {
  size_t logical_idx;
  uint64_t master_block_index;
} BlockPair;

static inline void insertion_sort(BlockPair *arr, size_t n) {
  for (size_t i = 1; i < n; i++) {

    // Fast path: already in order
    if (arr[i - 1].master_block_index <= arr[i].master_block_index) {
      continue;
    }

    BlockPair tmp = arr[i];
    size_t j = i;

    while (j > 0 && arr[j - 1].master_block_index > tmp.master_block_index) {
      arr[j] = arr[j - 1];
      j--;
    }

    arr[j] = tmp;
  }
}

int read_dedup(Index *index, const char *path, char *buf, size_t size,
               off_t offset, int masterFd);

int write_dedup(Index *index, const char *path, const char *buf, size_t size,
                off_t offset, int masterFd, uint64_t *nextBlockIndex);

void remove_block_dedup(Index *index, const char *path, uint64_t blockIndex);

#endif
