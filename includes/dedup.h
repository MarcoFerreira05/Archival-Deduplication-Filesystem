#ifndef DEDUP_H
#define DEDUP_H

#include <stdint.h>
#include <sys/types.h>

#include "alloc_policy.h"

typedef struct index Index;

int read_dedup(Index *index, const char *path, char *buf, size_t size,
               off_t offset, int masterFd);

// `cfg` selecciona a política de alocação para os MISSes deste request.
// Tipicamente vem de Context.alloc_config (carregado de env vars no
// arranque). NUNCA NULL.
int write_dedup(Index *index, const AllocConfig *cfg, const char *path,
                const char *buf, size_t size, off_t offset, int masterFd,
                uint64_t *nextBlockIndex);

void remove_block_dedup(Index *index, const char *path, uint64_t blockIndex);

#endif
