#ifndef METAINDEX_H
#define METAINDEX_H

#include "persistence.h"
#include <glib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define HASH_SIZE 64

// MasterInfo represents a unique physical block in the Masterfile.
// Two indices point to MasterInfo:
//  - (file, logical block) -> MasterInfo*
//  - hash -> MasterInfo*
// This enables O(1) deduplication and avoids reading the Masterfile during
// removals.
//
// `refcount` é atómico porque os incrementos em HITs acontecem sob read
// lock do metadata_rwlock — múltiplos threads podem incrementar em
// paralelo. O decrement+cleanup acontece sob write lock, garantindo que
// só um thread liberta o MasterInfo.
typedef struct masterInfo {
  unsigned char hash[HASH_SIZE]; // SHA-512 hash of the block content
  uint64_t masterBlockIndex;     // block index in the Masterfile (byte offset =
                                 // masterBlockIndex * BLOCK_SIZE)
  _Atomic uint32_t refcount;     // number of logical (file, block) references
} MasterInfo;

typedef struct blockIndice {
  const char *path;
  size_t offset; // logical block index
} BlockIndice;

// Main index structure containing both indices and the free list.
//
// Modelo de concorrência:
//   - metadata_rwlock protege hash_to_master, file_to_master, file_to_sizes
//     e o cleanup do MasterInfo (decrement final do refcount). Reads
//     concorrentes em paralelo, writes exclusivos.
//   - freelist_mutex protege free_block_list isoladamente.
//   - Lock hierarchy: se forem precisos os dois, sempre metadata_rwlock
//     ANTES de freelist_mutex. Nunca o inverso.
typedef struct index {
  GHashTable *hash_to_master;  // hash (unsigned char[64]) -> MasterInfo*
  GHashTable *file_to_master;  // BlockIndice* -> MasterInfo*
  GSList *free_block_list;     // lista de uint64_t* (slots reutilizáveis, LIFO)
  GHashTable *file_to_sizes;   // path (char*) -> size_t* (logical file size)
  pthread_rwlock_t metadata_rwlock; // protege hash_to_master, file_to_master,
                                    // file_to_sizes, refcount cleanup
  pthread_mutex_t freelist_mutex;   // protege free_block_list
} Index;

Bytes encode_master_info(void *elem);
void *decode_master_info(void *data, int size);

Bytes encode_hash(void *elem);
void *decode_hash(void *data, int size);

Bytes encode_block_indice(void *elem);
void *decode_block_indice(void *data, int size);

Bytes encode_free_block(void *elem);
void *decode_free_block(void *data, int size);

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
