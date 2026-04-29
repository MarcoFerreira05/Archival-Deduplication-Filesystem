#ifndef METAINDEX_H
#define METAINDEX_H

#include "freelist.h"
#include "persistence.h"
#include <glib.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#define HASH_SIZE 64

// MasterInfo represents a unique physical block in the Masterfile.
// Two indices point to MasterInfo:
//  - (file, logical block) -> MasterInfo*
//  - hash -> MasterInfo*
// This enables O(1) deduplication and avoids reading the Masterfile during
// removals.
typedef struct masterInfo {
  unsigned char hash[HASH_SIZE]; // SHA-512 hash of the block content
  uint64_t masterBlockIndex;     // block index in the Masterfile (byte offset =
                                 // masterBlockIndex * BLOCK_SIZE)
  uint32_t refcount;             // number of logical (file, block) references
} MasterInfo;

typedef struct blockIndice {
  const char *path;
  size_t offset; // logical block index
} BlockIndice;

// Main index structure containing both indices and the free list.
typedef struct index {
  GHashTable *hash_to_master; // hash (unsigned char[64]) -> MasterInfo*
  GHashTable *file_to_master; // BlockIndice* -> MasterInfo*
  FreeList free_list;         // mapa de extents (slots livres reutilizáveis)
  GHashTable *file_to_sizes;  // path (char*) -> size_t* (logical file size)
  pthread_mutex_t mutex;
} Index;

Bytes encode_master_info(void *elem);
void *decode_master_info(void *data, int size);

Bytes encode_hash(void *elem);
void *decode_hash(void *data, int size);

Bytes encode_block_indice(void *elem);
void *decode_block_indice(void *data, int size);

Bytes encode_size(void *elem);
void *decode_size(void *data, int size);

Bytes encode_str(void *elem);
void *decode_str(void *data, int size);

// Initializes the index structure.
// Returns NULL in case of failure and a pointer to the struct otherwise.
Index *index_init(void);

// Destroys the index structure and frees all MasterInfo objects.
void index_destroy(Index *index);

// Logical index: (file, blockIndex) -> MasterInfo*
MasterInfo *lookup_by_file_block(Index *index, const char *file,
                                 uint64_t blockIndex);
void insert_file_block(Index *index, const char *file, uint64_t blockIndex,
                       MasterInfo *info);
void remove_file_block(Index *index, const char *file, uint64_t blockIndex);

// Hash index: hash -> MasterInfo*
MasterInfo *lookup_by_hash(Index *index, const unsigned char *hash);
void insert_hash(Index *index, const unsigned char *hash, MasterInfo *info);
void remove_hash(Index *index, const unsigned char *hash);

// Funções hash/equal para usar com tabelas glib que indexem por SHA-512
// (chaves de HASH_SIZE bytes). Expostas para reuso por outros módulos.
guint hash512_hash(const void *key);
gboolean hash512_equal(const void *a, gconstpointer b);

#endif
