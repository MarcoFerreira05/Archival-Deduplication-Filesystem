/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresponding user-space
 * libc functions. Its performance is terrible.
 *
 * Compile with
 *
 *     gcc -Wall passthrough.c `pkg-config fuse3 --cflags --libs` -o passthrough
 *
 * ## Source code ##
 * \include passthrough.c
 */

#define FUSE_USE_VERSION 31
#include "glib.h"
#include <stddef.h>

#define _GNU_SOURCE

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif
#include "dedup.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "metaindex.h"
#include "passthrough_helpers.h"

#define DEBUG "/tmp/debug.log"

#ifdef DEBUG
typedef struct context {
  uint64_t create;
  uint64_t open;
  uint64_t close;
  uint64_t write;
  uint64_t read;
  uint64_t getattr;
  uint64_t unlink;
  pthread_mutex_t mutex;
  FILE *fp;
  Index *index;
  int masterFd;
  uint64_t nextBlockIndex;
} Context;
#endif

static int fill_dir_plus = 0;

static void *xmp_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {

  (void)conn;
  cfg->use_ino = 1;

  /* Pick up changes from lower filesystem right away. This is
     also necessary for better hardlink support. When the kernel
     calls the unlink() handler, it does not know the inode of
     the to-be-removed entry and can therefore not invalidate
     the cache of the associated inode - resulting in an
     incorrect st_nlink value being reported for any remaining
     hardlinks to this inode. */
  cfg->entry_timeout = 0;
  cfg->attr_timeout = 0;
  cfg->negative_timeout = 0;

  // Initialize context
  Context *ctx = malloc(sizeof(Context));
  pthread_mutex_init(&ctx->mutex, NULL);

  // Initialize metaindex
  ctx->index = index_init();

  ctx->masterFd = open("/masterFILE", O_RDWR | O_CREAT, 0666);
  struct stat stbuf;
  lstat("/masterFILE", &stbuf);
  ctx->nextBlockIndex = stbuf.st_size / BLOCK_SIZE;

#ifdef DEBUG
  ctx->open = 0;
  ctx->close = 0;
  ctx->read = 0;
  ctx->write = 0;
  ctx->getattr = 0;
  ctx->unlink = 0;
  struct fuse_context *f_ctx = fuse_get_context();
  ctx->fp = fopen(DEBUG, "w");
  printf("[Thread %d] Init called, userid %d, pid %d\n", gettid(), f_ctx->uid,
         f_ctx->pid);
  fprintf(ctx->fp, "[Thread %d] Init called, userid %d, pid %d\n", gettid(),
          f_ctx->uid, f_ctx->pid);
#endif

  return ctx;
}

static void xmp_destroy(void *private_data) {

  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = (Context *)private_data;
  pthread_mutex_destroy(&p_ctx->mutex);
  index_destroy(p_ctx->index);
  close(p_ctx->masterFd);

#ifdef DEBUG
  printf("[Thread %d] Destroy called, userid %d, pid %d\n", gettid(),
         f_ctx->uid, f_ctx->pid);
  printf("[Thread %d] Open() - %lu, Read() - %lu, Write() - %lu, Close() - "
         "%lu, Getattr() - %lu, Unlink() - %lu\n",
         gettid(), p_ctx->open, p_ctx->read, p_ctx->write, p_ctx->close,
         p_ctx->getattr, p_ctx->unlink);
  fprintf(p_ctx->fp, "[Thread %d] Destroy called,userid %d,pid % d\n ",
          gettid(), f_ctx->uid, f_ctx->pid);
  // fprintf(
  //     p_ctx->fp,
  //     "[Thread %d] Open() - %lu, Read() - %lu, Write() - %lu, Close() -
  //     %lu\n", gettid(), p_ctx->open, p_ctx->read, p_ctx->write,
  //     p_ctx->close);
  fclose(p_ctx->fp);
#endif

  free(private_data);
}

static int xmp_getattr(const char *path, struct stat *stbuf,
                       struct fuse_file_info *fi) {
  (void)fi;
  int res;

  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = (Context *)f_ctx->private_data;

#ifdef DEBUG
  p_ctx->getattr++;
  printf("[Thread %d] Getattr for path %s, userid %d, pid %d\n", gettid(), path,
         f_ctx->uid, f_ctx->pid);
  fprintf(p_ctx->fp, "[Thread %d] Getattr for path %s, userid %d, pid %d\n",
          gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

  res = lstat(path, stbuf);
  size_t *size_pointer = g_hash_table_lookup(p_ctx->index->file_to_sizes, path);
  if (size_pointer != NULL) {
    stbuf->st_size = *size_pointer;
  }

  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_access(const char *path, int mask) {
  int res;

  res = access(path, mask);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size) {
  int res;

  res = readlink(path, buf, size - 1);
  if (res == -1)
    return -errno;

  buf[res] = '\0';
  return 0;
}

static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
  DIR *dp;
  struct dirent *de;

  (void)offset;
  (void)fi;
  (void)flags;

  dp = opendir(path);
  if (dp == NULL)
    return -errno;

  while ((de = readdir(dp)) != NULL) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;
    if (filler(buf, de->d_name, &st, 0, fill_dir_plus))
      break;
  }

  closedir(dp);
  return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev) {
  int res;

  res = mknod_wrapper(AT_FDCWD, path, NULL, mode, rdev);
  if (res == -1)
    return -errno;

  return 0;
}

// When a file is deleted, we need to clean up all its dedup metadata:
// decrement refcounts, free blocks that are no longer referenced,
// and then actually delete the file from the filesystem.
static int xmp_unlink(const char *path) {
  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = (Context *)f_ctx->private_data;

#ifdef DEBUG
  p_ctx->unlink++;
  printf("[Thread %d] Unlink for path %s, userid %d, pid %d\n", gettid(), path,
         f_ctx->uid, f_ctx->pid);
  // fprintf(p_ctx->fp, "[Thread %d] Unlink for path %s, userid %d, pid %d\n",
  //         gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

  pthread_mutex_lock(&p_ctx->index->mutex);
  size_t *logical_size = g_hash_table_lookup(p_ctx->index->file_to_sizes, path);
  if (logical_size != NULL) {
    // walk through every block of the file and remove its reference
    uint64_t num_blocks = (*logical_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint64_t i = 0; i < num_blocks; i++) {
      remove_block_dedup(p_ctx->index, path, i);
    }
    // remove the file size entry itself
    g_hash_table_remove(p_ctx->index->file_to_sizes, path);
  }
  pthread_mutex_unlock(&p_ctx->index->mutex);

  // now actually delete the file
  int res = unlink(path);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_mkdir(const char *path, mode_t mode) {
  int res;

  res = mkdir(path, mode);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_rmdir(const char *path) {
  int res;

  res = rmdir(path);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_symlink(const char *from, const char *to) {
  int res;

  res = symlink(from, to);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags) {
  int res;

  if (flags)
    return -EINVAL;

  res = rename(from, to);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_link(const char *from, const char *to) {
  int res;

  res = link(from, to);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_chmod(const char *path, mode_t mode, struct fuse_file_info *fi) {
  (void)fi;
  int res;

  res = chmod(path, mode);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
                     struct fuse_file_info *fi) {
  (void)fi;
  int res;

  res = lchown(path, uid, gid);
  if (res == -1)
    return -errno;

  return 0;
}

// When a file is truncated, remove dedup references for the blocks
// that are now beyond the new size, then shrink the actual file.
static int xmp_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi) {
  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = (Context *)f_ctx->private_data;

  pthread_mutex_lock(&p_ctx->index->mutex);
  size_t *logical_size = g_hash_table_lookup(p_ctx->index->file_to_sizes, path);
  if (logical_size != NULL && (size_t)size < *logical_size) {
    // only the blocks past the new size need to be removed
    uint64_t new_block_count = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    uint64_t old_block_count = (*logical_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (uint64_t i = new_block_count; i < old_block_count; i++) {
      remove_block_dedup(p_ctx->index, path, i);
    }
    *logical_size = size;
  }
  pthread_mutex_unlock(&p_ctx->index->mutex);

  int res;
  if (fi != NULL)
    res = ftruncate(fi->fh, size);
  else
    res = truncate(path, size);
  if (res == -1)
    return -errno;

  return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2],
                       struct fuse_file_info *fi) {
  (void)fi;
  int res;

  /* don't use utime/utimes since they follow symlinks */
  res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
  if (res == -1)
    return -errno;

  return 0;
}
#endif

static int xmp_create(const char *path, mode_t mode,
                      struct fuse_file_info *fi) {
  int res;

#ifdef DEBUG
  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = f_ctx->private_data;
  pthread_mutex_lock(&p_ctx->mutex);
  p_ctx->create++;
  printf("[Thread %d] Create for path %s, userid %d, pid %d\n", gettid(), path,
         f_ctx->uid, f_ctx->pid);
  fprintf(p_ctx->fp, "[Thread %d] Create for path %s, userid %d, pid %d\n",
          gettid(), path, f_ctx->uid, f_ctx->pid);
  pthread_mutex_unlock(&p_ctx->mutex);
#endif

  res = open(path, fi->flags, mode);
  if (res == -1)
    return -errno;

  fi->fh = res;
  return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi) {
  int res;

  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = (Context *)f_ctx->private_data;

  res = open(path, fi->flags);

#ifdef DEBUG
  p_ctx->open++;
  printf("[Thread %d] Open for path %s, userid %d, pid %d\n", gettid(), path,
         f_ctx->uid, f_ctx->pid);
  fprintf(p_ctx->fp, "[Thread %d] Open for path %s, userid %d, pid %d\n",
          gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

  if (res == -1)
    return -errno;
  fi->fh = res;

  return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
  int fd;

  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = f_ctx->private_data;

#ifdef DEBUG
  f_ctx = fuse_get_context();
  p_ctx = f_ctx->private_data;
  pthread_mutex_lock(&p_ctx->mutex);
  p_ctx->read++;
  printf("[Thread %d] Read for path %s, userid %d, pid %d\n", gettid(), path,
         f_ctx->uid, f_ctx->pid);
  fprintf(p_ctx->fp, "[Thread %d] Read for path %s, userid %d, pid %d\n",
          gettid(), path, f_ctx->uid, f_ctx->pid);
  pthread_mutex_unlock(&p_ctx->mutex);
#endif
  // printf("[READ START] tid=%d path=%s uid=%d pid=%d size=%zu offset=%ld\n",
  //        gettid(), path, f_ctx->uid, f_ctx->pid, size, offset);

  if (fi == NULL)
    fd = open(path, O_RDONLY);
  else
    fd = fi->fh;

  if (fd == -1)
    return -errno;

  // read from the master file through the dedup layer
  pthread_mutex_lock(&p_ctx->index->mutex);
  int total_read =
      read_dedup(p_ctx->index, path, buf, size, offset, p_ctx->masterFd);
  pthread_mutex_unlock(&p_ctx->index->mutex);

  if (fi == NULL)
    close(fd);

  return (int)total_read;
}

static int xmp_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi) {
  int fd;
  int res = 0;

  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = f_ctx->private_data;

  // printf("[WRITE START] tid=%d path=%s uid=%d pid=%d size=%zu offset=%ld\n",
  //        gettid(), path, f_ctx->uid, f_ctx->pid, size, offset);

#ifdef DEBUG
  f_ctx = fuse_get_context();
  p_ctx = f_ctx->private_data;
  pthread_mutex_lock(&p_ctx->mutex);
  p_ctx->write++;
  printf("[Thread %d] Write for path %s, userid %d, pid %d\n", gettid(), path,
         f_ctx->uid, f_ctx->pid);
  fprintf(p_ctx->fp, "[Thread %d] Write for path %s, userid %d, pid %d\n",
          gettid(), path, f_ctx->uid, f_ctx->pid);
  pthread_mutex_unlock(&p_ctx->mutex);
#endif
  (void)fi;
  if (fi == NULL)
    fd = open(path, O_WRONLY);
  else
    fd = fi->fh;

  if (fd == -1)
    return -errno;

  // write through the dedup layer (handles dedup + overwrite)
  pthread_mutex_lock(&p_ctx->index->mutex);
  res = write_dedup(p_ctx->index, path, buf, size, offset, p_ctx->masterFd,
                    &p_ctx->nextBlockIndex);
  pthread_mutex_unlock(&p_ctx->index->mutex);

  if (fi == NULL)
    close(fd);

  return res;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf) {
  int res;
  res = statvfs(path, stbuf);
  if (res == -1)
    return -errno;

  return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi) {
  (void)path;

  struct fuse_context *f_ctx = fuse_get_context();
  Context *p_ctx = f_ctx->private_data;

#ifdef DEBUG
  p_ctx->close++;
  printf("[Thread %d] Close for path %s, userid %d, pid %d\n", gettid(), path,
         f_ctx->uid, f_ctx->pid);
  fprintf(p_ctx->fp, "[Thread %d] Close for path %s, userid %d, pid %d\n",
          gettid(), path, f_ctx->uid, f_ctx->pid);
#endif

  int res = close(fi->fh);
  return res;
}

static int xmp_fsync(const char *path, int isdatasync,
                     struct fuse_file_info *fi) {

  (void)path;

  // If the datasync parameter is non-zero, then only the user data should be
  // flushed, not the meta data.
  if (isdatasync == 0) {
    return fsync(fi->fh);
  }

  return fdatasync(fi->fh);
}

#ifdef HAVE_POSIX_FALLOCATE
static int xmp_fallocate(const char *path, int mode, off_t offset, off_t length,
                         struct fuse_file_info *fi) {
  int fd;
  int res;

  (void)fi;

  if (mode)
    return -EOPNOTSUPP;

  if (fi == NULL)
    fd = open(path, O_WRONLY);
  else
    fd = fi->fh;

  if (fd == -1)
    return -errno;

  res = -posix_fallocate(fd, offset, length);

  if (fi == NULL)
    close(fd);
  return res;
}
#endif

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
                        size_t size, int flags) {
  int res = lsetxattr(path, name, value, size, flags);
  if (res == -1)
    return -errno;
  return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
                        size_t size) {
  int res = lgetxattr(path, name, value, size);
  if (res == -1)
    return -errno;
  return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size) {
  int res = llistxattr(path, list, size);
  if (res == -1)
    return -errno;
  return res;
}

static int xmp_removexattr(const char *path, const char *name) {
  int res = lremovexattr(path, name);
  if (res == -1)
    return -errno;
  return 0;
}
#endif /* HAVE_SETXATTR */

#ifdef HAVE_COPY_FILE_RANGE
static ssize_t xmp_copy_file_range(const char *path_in,
                                   struct fuse_file_info *fi_in,
                                   off_t offset_in, const char *path_out,
                                   struct fuse_file_info *fi_out,
                                   off_t offset_out, size_t len, int flags) {
  int fd_in, fd_out;
  ssize_t res;

  if (fi_in == NULL)
    fd_in = open(path_in, O_RDONLY);
  else
    fd_in = fi_in->fh;

  if (fd_in == -1)
    return -errno;

  if (fi_out == NULL)
    fd_out = open(path_out, O_WRONLY);
  else
    fd_out = fi_out->fh;

  if (fd_out == -1) {
    close(fd_in);
    return -errno;
  }

  res = copy_file_range(fd_in, &offset_in, fd_out, &offset_out, len, flags);
  if (res == -1)
    res = -errno;

  if (fi_out == NULL)
    close(fd_out);
  if (fi_in == NULL)
    close(fd_in);

  return res;
}
#endif

static off_t xmp_lseek(const char *path, off_t off, int whence,
                       struct fuse_file_info *fi) {
  int fd;
  off_t res;

  if (fi == NULL)
    fd = open(path, O_RDONLY);
  else
    fd = fi->fh;

  if (fd == -1)
    return -errno;

  res = lseek(fd, off, whence);
  if (res == -1)
    res = -errno;

  if (fi == NULL)
    close(fd);
  return res;
}

static const struct fuse_operations xmp_oper = {
    .init = xmp_init,
    .destroy = xmp_destroy,
    .getattr = xmp_getattr,
    .access = xmp_access,
    .readlink = xmp_readlink,
    .readdir = xmp_readdir,
    .mknod = xmp_mknod,
    .mkdir = xmp_mkdir,
    .symlink = xmp_symlink,
    .unlink = xmp_unlink, // TODO (extra)
    .rmdir = xmp_rmdir,
    .rename = xmp_rename, // TODO (extra)
    .link = xmp_link,
    .chmod = xmp_chmod,
    .chown = xmp_chown,
    .truncate = xmp_truncate, // TODO (extra)
#ifdef HAVE_UTIMENSAT
    .utimens = xmp_utimens,
#endif
    .open = xmp_open,
    .create = xmp_create,
    .read = xmp_read, // TODO (decompress data on read)
    .write = xmp_write,
    .statfs = xmp_statfs,
    .release = xmp_release, // TODO (compress data on close)
    .fsync = xmp_fsync,
#ifdef HAVE_POSIX_FALLOCATE
    .fallocate = xmp_fallocate,
#endif
#ifdef HAVE_SETXATTR
    .setxattr = xmp_setxattr,
    .getxattr = xmp_getxattr,
    .listxattr = xmp_listxattr,
    .removexattr = xmp_removexattr,
#endif
#ifdef HAVE_COPY_FILE_RANGE
// .copy_file_range = xmp_copy_file_range, // TODO (extra)
#endif
    // .lseek = xmp_lseek, // TODO (extra)
};

int main(int argc, char *argv[]) {
  enum { MAX_ARGS = 10 };
  int i, new_argc;
  char *new_argv[MAX_ARGS];

  umask(0);
  /* Process the "--plus" option apart */
  for (i = 0, new_argc = 0; (i < argc) && (new_argc < MAX_ARGS); i++) {
    if (!strcmp(argv[i], "--plus")) {
      fill_dir_plus = FUSE_FILL_DIR_PLUS;
    } else {
      new_argv[new_argc++] = argv[i];
    }
  }

  return fuse_main(new_argc, new_argv, &xmp_oper, NULL);
}
