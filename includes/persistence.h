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

void ghash_save(const char *path, GHashTable *ht, EncodeFunc key_func,
                EncodeFunc val_func);

GHashTable *ghash_load(const char *path, GHashFunc hash_fn, GEqualFunc equal_fn,
                       DecodeFunc key_func, DecodeFunc val_func,
                       GDestroyNotify free_key, GDestroyNotify free_val);

void gslist_save(const char *path, GSList *list, EncodeFunc func);
GSList *gslist_load(const char *path, DecodeFunc func);

#endif
