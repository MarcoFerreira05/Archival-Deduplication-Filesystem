#include "persistence.h"
#include <stddef.h>

typedef struct {
  int fd;
  EncodeFunc key_encoder;
  EncodeFunc val_encoder;
  off_t offset;
} SaveCtx;

void write_elem(int fd, Bytes b, off_t *offset) {
  if (pwrite(fd, &b.size, sizeof(b.size), *offset) != sizeof(b.size))
    return;
  *offset += sizeof(b.size);

  if (pwrite(fd, b.data, b.size, *offset) != b.size)
    return;
  *offset += b.size;
}

void *read_elem(int fd, DecodeFunc decode_func, off_t *offset) {
  size_t size = 0;
  if (pread(fd, &size, sizeof(size), *offset) != sizeof(size))
    return NULL;
  *offset += sizeof(size);

  void *buf = malloc(size);
  if (pread(fd, buf, size, *offset) != size) {
    free(buf);
    return NULL;
  }
  *offset += size;

  void *elem = decode_func(buf, (int)size);
  free(buf);
  return elem;
}

void save_entry(void *key, void *value, void *data) {
  SaveCtx *ctx = data;
  write_elem(ctx->fd, ctx->key_encoder(key), &ctx->offset);
  write_elem(ctx->fd, ctx->val_encoder(value), &ctx->offset);
}

void ghash_save(const char *path, GHashTable *ht, EncodeFunc key_func,
                EncodeFunc val_func) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return;

  int n = g_hash_table_size(ht);
  off_t offset = 0;
  if (pwrite(fd, &n, sizeof(n), offset) != sizeof(n)) {
    close(fd);
    return;
  }
  offset += sizeof(n);

  SaveCtx ctx = {fd, key_func, val_func, offset};
  g_hash_table_foreach(ht, save_entry, &ctx);

  close(fd);
}

GHashTable *ghash_load(const char *path, GHashFunc hash_fn, GEqualFunc equal_fn,
                       DecodeFunc key_func, DecodeFunc val_func,
                       GDestroyNotify free_key, GDestroyNotify free_val) {
  int fd = open(path, O_RDONLY | O_CREAT, 0644);
  GHashTable *ht = g_hash_table_new_full(hash_fn, equal_fn, free_key, free_val);
  if (fd < 0)
    return ht;

  int n = 0;
  off_t offset = 0;
  ssize_t n_read = pread(fd, &n, sizeof(n), offset);
  if (n_read == sizeof(n)) {
    offset += sizeof(n);
    for (int i = 0; i < n; i++) {
      void *key = read_elem(fd, key_func, &offset);
      void *val = read_elem(fd, val_func, &offset);
      if (key == NULL || val == NULL)
        break;
      g_hash_table_insert(ht, key, val);
    }
  }

  close(fd);
  return ht;
}

void gslist_save(const char *path, GSList *list, EncodeFunc func) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return;

  int n = g_slist_length(list);
  off_t offset = 0;
  if (pwrite(fd, &n, sizeof(n), offset) != sizeof(n)) {
    close(fd);
    return;
  }
  offset += sizeof(n);

  for (GSList *l = list; l; l = l->next) {
    write_elem(fd, func(l->data), &offset);
  }

  close(fd);
}

GSList *gslist_load(const char *path, DecodeFunc func) {
  int fd = open(path, O_RDONLY | O_CREAT, 0644);
  if (fd < 0)
    return NULL;

  int n = 0;
  off_t offset = 0;
  ssize_t n_read = pread(fd, &n, sizeof(n), offset);
  if (n_read != sizeof(n)) {
    close(fd);
    return NULL;
  }
  offset += sizeof(n);

  GSList *list = NULL;
  for (int i = 0; i < n; i++) {
    void *elem = read_elem(fd, func, &offset);
    if (elem == NULL)
      break;
    list = g_slist_append(list, elem);
  }

  close(fd);
  return list;
}
