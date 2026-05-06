#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <fcntl.h>
#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
typedef struct {
  void *data;
  size_t size;
} Bytes;

typedef Bytes (*EncodeFunc)(void *elem);
typedef void *(*DecodeFunc)(void *data, int size);

typedef struct {
  GHashTable *table1;
  GHashTable *table2;
} IndexedPairTables;

typedef struct {
  GHashFunc hash_fn;
  GEqualFunc equal_fn;
  DecodeFunc decode_key;
  GDestroyNotify free_key;
} IndexedTableLoadConfig;

typedef struct {
  EncodeFunc encode_key;
  gboolean free_encoded_key;  // Set TRUE if encode_key allocates memory
} IndexedTableSaveConfig;

// Save a regular hash table. Set free_encoded_key/val TRUE if the corresponding
// encoder allocates memory (e.g., encode_block_indice). Allocated memory will be
// freed automatically after encoding each entry.
void ghash_save(const char *path, GHashTable *ht, EncodeFunc encode_key,
                EncodeFunc encode_val, gboolean free_encoded_key,
                gboolean free_encoded_val);

GHashTable *ghash_load(const char *path, GHashFunc hash_fn, GEqualFunc equal_fn,
                       DecodeFunc decode_key, DecodeFunc decode_val,
                       GDestroyNotify free_key, GDestroyNotify free_val);

// Save two indexed hash tables using a shared values file. Set config1/2's
// free_encoded_key TRUE if the corresponding encoder allocates memory.
void ghash_save_indexed_pair(const char *table1_path, const char *table2_path,
                             const char *values_path, GHashTable *table1,
                             GHashTable *table2,
                             IndexedTableSaveConfig config1,
                             IndexedTableSaveConfig config2,
                             EncodeFunc encode_value);

IndexedPairTables ghash_load_indexed_pair(
    const char *table1_path, const char *table2_path, const char *values_path,
    IndexedTableLoadConfig config1, IndexedTableLoadConfig config2,
    DecodeFunc decode_value, GDestroyNotify free_value);

// Persistência da free list: lista de slots livres como uint64_t individuais.
void gslist_save(const char *path, GSList *list, EncodeFunc encode_elem);
GSList *gslist_load(const char *path, DecodeFunc decode_elem);

// Helpers de baixo-nível para escrever/ler elementos prefixados por tamanho
// (size_t prefix + payload). Expostos para que módulos com formatos próprios
// possam reaproveitar o mesmo esquema sem duplicar código.
void write_elem(int fd, Bytes b, off_t *offset);
void *read_elem(int fd, DecodeFunc decode_func, off_t *offset);

#endif
