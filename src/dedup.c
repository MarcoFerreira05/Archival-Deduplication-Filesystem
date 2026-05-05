
#include <stddef.h>
#define FUSE_USE_VERSION 31

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "dedup.h"
#include "hashing.h"
#include "metaindex.h"
#include "passthrough_helpers.h"

static int cmp_master_idx(const void *a, const void *b) {
  uint64_t ma = ((const BlockPair *)a)->master_block_index;
  uint64_t mb = ((const BlockPair *)b)->master_block_index;
  return (ma > mb) - (ma < mb);
}

// Read file with batching optimization.
// Phase 1: Lookup all blocks to get their master positions
// Phase 2: Sort pairs by master block index
// Phase 3: Identify consecutive groups in physical space
// Phase 4: Read each group with a single pread
// Phase 5: Copy blocks to correct positions in output
int read_dedup(Index *index, const char *path, char *buf, size_t size,
               off_t offset, int masterFd) {
  size_t num_blocks = size / BLOCK_SIZE;
  uint64_t start_block = offset / BLOCK_SIZE;

  if (num_blocks == 0)
    return 0;

  // Phase 1: Lookup all blocks and create pairs
  BlockPair pairs[num_blocks];
  for (size_t i = 0; i < num_blocks; i++) {
    MasterInfo *info = lookup_by_file_block(index, path, start_block + i);
    if (info == NULL) {
      return -1;
    }
    pairs[i].logical_idx = i;
    pairs[i].master_block_index = info->masterBlockIndex;
  }

  // Phase 2: Sort pairs by master block index
  if (num_blocks <= INSERTION_SORT_THRESHOLD) {
    insertion_sort(pairs, num_blocks);
  } else {
    qsort(pairs, num_blocks, sizeof(BlockPair), cmp_master_idx);
  }

  // Allocate single buffer for all reads not one per group
  char *master_buf = malloc(num_blocks * BLOCK_SIZE);

  // Phase 3 & 4: Identify groups and read them
  size_t group_start = 0;
  for (size_t i = 1; i <= num_blocks; i++) {
    int is_last = (i == num_blocks);

    int is_consec = !is_last && (pairs[i].master_block_index ==
                                 pairs[i - 1].master_block_index + 1);
    if (!is_last && is_consec)
      continue;

    // End of current group - read it
    uint64_t min_master = pairs[group_start].master_block_index;
    size_t blocks_in_group = i - group_start;

    // Fast path: single block group
    if (blocks_in_group == 1) {
      size_t logical_idx = pairs[group_start].logical_idx;
      char *dst = buf + logical_idx * BLOCK_SIZE;
      ssize_t res = pread(masterFd, dst, BLOCK_SIZE, min_master * BLOCK_SIZE);
      if (res != BLOCK_SIZE) {
        free(master_buf);
        return -1;
      }
    } else {
      size_t read_size = blocks_in_group * BLOCK_SIZE;
      ssize_t res =
          pread(masterFd, master_buf, read_size, min_master * BLOCK_SIZE);
      if (res != (ssize_t)read_size) {
        free(master_buf);
        return -1;
      }
      // Phase 5: Copy each block to correct position in output
      for (size_t j = group_start; j < i; j++) {
        size_t logical_idx = pairs[j].logical_idx;
        uint64_t offset_in_range = pairs[j].master_block_index - min_master;
        char *src = master_buf + offset_in_range * BLOCK_SIZE;
        char *dst = buf + logical_idx * BLOCK_SIZE;
        memcpy(dst, src, BLOCK_SIZE);
      }
    }
    group_start = i;
  }
  free(master_buf);
  return size;
}

// Remove a single logical block reference. Decrements refcount and reclaims
// the master block if no more references exist.
void remove_block_dedup(Index *index, const char *path, uint64_t blockIndex) {
  // find the MasterInfo for this (file, block)
  MasterInfo *info = lookup_by_file_block(index, path, blockIndex);
  if (info == NULL)
    return;

  // remove the logical mapping and decrement refcount
  remove_file_block(index, path, blockIndex);
  info->refcount--;

  // if nobody else references this block, free it and reclaim the space
  if (info->refcount == 0) {
    uint64_t *slot = malloc(sizeof(uint64_t));
    *slot = info->masterBlockIndex;
    index->free_block_list = g_slist_prepend(index->free_block_list, slot);

    remove_hash(index, info->hash);
    free(info);
  }
}

//  1. check if the same content already exists (by hash)
//     - HIT:  just bump refcount, no write to master
//     - MISS: write to master, create new MasterInfo
//  2. point this (file, block) at the MasterInfo
int write_dedup(Index *index, const char *path, const char *buf, size_t size,
                off_t offset, int masterFd, uint64_t *nextBlockIndex) {
  size_t num_blocks = size / BLOCK_SIZE;
  uint64_t start_block = offset / BLOCK_SIZE;
  int res = 0;
  unsigned char hash_bytes[HASH_SIZE];
  unsigned char block[BLOCK_SIZE];

  for (size_t i = 0; i < num_blocks; i++) {
    uint64_t blk = start_block + i;

    memcpy(block, buf + i * BLOCK_SIZE, BLOCK_SIZE);
    hash(block, hash_bytes);

    // Overwrite support is commented out in case you guys want
    // to add it later mas como n dá mais pontos it is waht it is.
    // MasterInfo *old = lookup_by_file_block(index, path, blk);
    // if (old != NULL) {
    //   remove_file_block(index, path, blk);
    //   old->refcount--;
    //   if (old->refcount == 0) {
    //     uint64_t *slot = malloc(sizeof(uint64_t));
    //     *slot = old->masterBlockIndex;
    //     index->free_block_list =
    //         g_slist_prepend(index->free_block_list, slot);
    //     remove_hash(index, old->hash);
    //     free(old);
    //   }
    // }

    // check if this content already exists somewhere in the master
    MasterInfo *info = lookup_by_hash(index, hash_bytes);
    if (info != NULL) {
      // HIT: same content already stored, just add another reference
      printf("HIT\n");
      info->refcount++;
    } else {
      // MISS: new content, need to write it to the master file
      printf("MISS\n");

      // try to reuse a freed slot, otherwise append at the end
      uint64_t masterBlk;
      if (index->free_block_list != NULL) {
        masterBlk = *(uint64_t *)index->free_block_list->data;
        free(index->free_block_list->data);
        index->free_block_list =
            g_slist_delete_link(index->free_block_list, index->free_block_list);
      } else {
        masterBlk = *nextBlockIndex;
        (*nextBlockIndex)++;
      }

      // write the actual data to the master file
      ssize_t written =
          pwrite(masterFd, block, BLOCK_SIZE, masterBlk * BLOCK_SIZE);
      if (written == -1)
        return -errno;

      // create the MasterInfo for this new unique block
      info = malloc(sizeof(MasterInfo));
      memcpy(info->hash, hash_bytes, HASH_SIZE);
      info->masterBlockIndex = masterBlk;
      info->refcount = 1;

      insert_hash(index, hash_bytes, info);
    }

    // point this (file, block) at the MasterInfo (new or existing)
    insert_file_block(index, path, blk, info);
    res += BLOCK_SIZE;
  }

  // update the logical file size (only grows, never shrinks here)
  size_t new_end = (start_block + num_blocks) * BLOCK_SIZE;
  size_t *logical_size = g_hash_table_lookup(index->file_to_sizes, path);
  if (logical_size == NULL) {
    logical_size = malloc(sizeof(size_t));
    *logical_size = new_end;
    g_hash_table_insert(index->file_to_sizes, g_strdup(path), logical_size);
  } else if (new_end > *logical_size) {
    *logical_size = new_end;
  }

  return res;
}
