#include "metaindex.h"
#include "glib.h"
#include "persistence.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABLE_PATH_HASH_TO_MASTER "/table_path_hash_to_master"
#define TABLE_PATH_FILE_TO_MASTER "/table_path_file_to_master"
#define TABLE_PATH_MASTER_INFOS "/table_path_master_infos"
#define TABLE_PATH_FILE_TO_SIZES "/table_path_file_to_sizes"
#define TABLE_PATH_FREE_BLOCK_LIST "/table_path_free_block_list"

Bytes encode_master_info(void *elem) {
  return (Bytes){elem, sizeof(MasterInfo)};
}

void *decode_master_info(void *data, int size) {
  if (size != sizeof(MasterInfo))
    return NULL;

  MasterInfo *m = malloc(sizeof(MasterInfo));
  memcpy(m, data, sizeof(MasterInfo));
  return m;
}

Bytes encode_hash(void *elem) { return (Bytes){elem, HASH_SIZE}; }

void *decode_hash(void *data, int size) {
  if (size != HASH_SIZE)
    return NULL;

  char *hash = malloc(HASH_SIZE);
  memcpy(hash, data, HASH_SIZE);
  return hash;
}

Bytes encode_block_indice(void *elem) {
  const BlockIndice *b = elem;
  int path_len = strlen(b->path) + 1;
  int total = sizeof(path_len) + path_len + sizeof(size_t);

  void *buf = malloc(total);
  void *p = buf;

  memcpy(p, &path_len, sizeof(path_len));
  p += sizeof(path_len);
  memcpy(p, b->path, path_len);
  p += path_len;
  memcpy(p, &b->offset, sizeof(size_t));

  return (Bytes){buf, total};
}

void *decode_block_indice(void *data, int size) {
  (void)size;
  void *p = data;

  int path_len;
  memcpy(&path_len, p, sizeof(path_len));
  p += sizeof(path_len);

  char *path = malloc(path_len);
  memcpy(path, p, path_len);
  p += path_len;

  size_t offset;
  memcpy(&offset, p, sizeof(size_t));

  BlockIndice *b = malloc(sizeof(BlockIndice));
  b->path = path;
  b->offset = offset;
  return b;
}

Bytes encode_size(void *elem) { return (Bytes){elem, sizeof(size_t)}; }

void *decode_size(void *data, int size) {
  (void)size;
  size_t *value = malloc(sizeof(size_t));
  memcpy(value, data, sizeof(size_t));
  return value;
}

Bytes encode_str(void *elem) {
  char *s = elem;
  int len = strlen(s) + 1;
  return (Bytes){elem, len};
}

void *decode_str(void *data, int size) {
  char *s = malloc(size);
  memcpy(s, data, size);
  return s;
}

guint hash512_hash(const void *key) {
  const size_t *data = key;

  size_t folded = data[0] ^ data[1] ^ data[2] ^ data[3] ^ data[4] ^ data[5] ^
                  data[6] ^ data[7];

  return (guint)(folded ^ (folded >> 32));
}

gboolean hash512_equal(const void *a, gconstpointer b) {
  return memcmp(a, b, HASH_SIZE) == 0;
}

guint blockIndice_hash(const void *key) {
  const BlockIndice *b = key;

  guint h1 = g_str_hash(b->path);
  guint h2 = g_int64_hash(&b->offset);

  return h1 ^ (h2 << 1);
}

gboolean blockIndice_equal(const void *a, const void *b) {
  const BlockIndice *x = a;
  const BlockIndice *y = b;

  return (x->offset == y->offset) && (g_str_equal(x->path, y->path));
}

static void block_indice_free(void *data) {
  BlockIndice *b = data;
  g_free((char *)b->path);
  g_free(b);
}
Index *index_init(void) {
  Index *index = malloc(sizeof(Index));

  IndexedTableLoadConfig config1 = {hash512_hash, hash512_equal, decode_hash,
                                    g_free};
  IndexedTableLoadConfig config2 = {blockIndice_hash, blockIndice_equal,
                                    decode_block_indice, block_indice_free};

  IndexedPairTables indexed_tables = ghash_load_indexed_pair(
      TABLE_PATH_HASH_TO_MASTER, TABLE_PATH_FILE_TO_MASTER,
      TABLE_PATH_MASTER_INFOS, config1, config2, decode_master_info, NULL);

  // hash_to_master: key = g_memdup'd hash (freed by g_free),
  //                 value = shared MasterInfo* (freed in index_destroy)
  index->hash_to_master = indexed_tables.table1;

  // file_to_master: key = BlockIndice* (freed by block_indice_free),
  //                 value = shared MasterInfo* (owned by hash_to_master set)
  index->file_to_master = indexed_tables.table2;

  index->file_to_sizes =
      ghash_load(TABLE_PATH_FILE_TO_SIZES, g_str_hash, g_str_equal, decode_str,
                 decode_size, g_free, g_free);

  // Carrega a free list em formato extent map. Detecta automaticamente o
  // formato antigo (slots individuais) e migra com coalescing — ver
  // freelist_load em src/freelist.c.
  freelist_load(TABLE_PATH_FREE_BLOCK_LIST, &index->free_list);

  pthread_mutex_init(&index->mutex, NULL);

  return index;
}

void index_destroy(Index *index) {
  pthread_mutex_lock(&index->mutex);

  // encode_hash doesn't allocate, so free_encoded_key=FALSE
  IndexedTableSaveConfig config1 = {encode_hash, FALSE};
  // encode_block_indice allocates memory, so free_encoded_key=TRUE
  IndexedTableSaveConfig config2 = {encode_block_indice, TRUE};

  ghash_save_indexed_pair(TABLE_PATH_HASH_TO_MASTER, TABLE_PATH_FILE_TO_MASTER,
                          TABLE_PATH_MASTER_INFOS, index->hash_to_master,
                          index->file_to_master, config1, config2,
                          encode_master_info);
  ghash_save(TABLE_PATH_FILE_TO_SIZES, index->file_to_sizes, encode_str,
             encode_size, FALSE, FALSE);
  freelist_save(TABLE_PATH_FREE_BLOCK_LIST, &index->free_list);

  pthread_mutex_unlock(&index->mutex);

  // Free all MasterInfo objects (one per unique block, stored in
  // hash_to_master)
  GHashTableIter iter;
  void *key;
  void *value;
  g_hash_table_iter_init(&iter, index->hash_to_master);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    free((MasterInfo *)value);
  }

  g_hash_table_destroy(index->file_to_master);
  g_hash_table_destroy(index->hash_to_master);
  g_hash_table_destroy(index->file_to_sizes);
  freelist_destroy(&index->free_list);

  pthread_mutex_destroy(&index->mutex);
  free(index);
}

// --- Logical index: (file, blockIndex) -> MasterInfo* ---

MasterInfo *lookup_by_file_block(Index *index, const char *file,
                                 uint64_t blockIndex) {
  BlockIndice key = {.path = file, .offset = blockIndex};
  return g_hash_table_lookup(index->file_to_master, &key);
}

void insert_file_block(Index *index, const char *file, uint64_t blockIndex,
                       MasterInfo *info) {
  BlockIndice *key = g_new(BlockIndice, 1);
  key->path = g_strdup(file);
  key->offset = blockIndex;
  g_hash_table_insert(index->file_to_master, key, info);
}

void remove_file_block(Index *index, const char *file, uint64_t blockIndex) {
  BlockIndice key = {.path = file, .offset = blockIndex};
  g_hash_table_remove(index->file_to_master, &key);
}

// --- Hash index: hash -> MasterInfo* ---

MasterInfo *lookup_by_hash(Index *index, const unsigned char *hash) {
  return g_hash_table_lookup(index->hash_to_master, hash);
}

void insert_hash(Index *index, const unsigned char *hash, MasterInfo *info) {
  unsigned char *key = g_memdup2(hash, HASH_SIZE);
  g_hash_table_insert(index->hash_to_master, key, info);
}

void remove_hash(Index *index, const unsigned char *hash) {
  g_hash_table_remove(index->hash_to_master, hash);
}
