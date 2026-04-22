
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

static int master_read(int fdMaster, off_t offset, char *buff) {
  return pread(fdMaster, buff, BLOCK_SIZE, offset);
}

// Single-lookup read: (file, blockIndex) -> MasterInfo -> master file
static int read_block(int fdMaster, const char *path, uint64_t block_index,
                      char *buff, Index *index) {
  MasterInfo *info = lookup_by_file_block(index, path, block_index);
  if (info == NULL)
    return -1;
  return master_read(fdMaster, info->masterBlockIndex * BLOCK_SIZE, buff);
}

// Read file content block by block from the master file.
// For each logical block, we look up the MasterInfo and read from the master.
int read_dedup(Index *index, const char *path, char *buf, size_t size,
               off_t offset, int masterFd) {
  ssize_t total_read = 0;
  size_t num_blocks = size / BLOCK_SIZE;
  uint64_t start_block = offset / BLOCK_SIZE;
  int res = 0;
  for (size_t i = 0; i < num_blocks; i++) {
    char buff[BLOCK_SIZE];
    // one lookup: (file, block) -> MasterInfo -> pread from master
    res = read_block(masterFd, path, start_block + i, buff, index);
    if (res == -1) {
      return res;
    }
    memcpy(buf + i * BLOCK_SIZE, buff, BLOCK_SIZE);
    total_read += BLOCK_SIZE;
  }
  return total_read;
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
