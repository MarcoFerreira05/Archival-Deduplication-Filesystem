// =============================================================================
// alloc_policy.c — Carregamento da configuração de política de alocação a
// partir de variáveis de ambiente.
//
// Lemos as env vars uma única vez, no arranque do FUSE (xmp_init), e
// guardamos o resultado no Context. write_dedup recebe um const AllocConfig*
// para escolher entre as filosofias storage-first e syscall-first.
//
// Defaults:
//   DEDUP_ALLOC_POLICY    = storage_first
//   DEDUP_THRESHOLD       = 4
//   DEDUP_MAX_FRAGMENTS   = 0 (ilimitado, reservado)
// =============================================================================

#include "alloc_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper: lê uma env var como uint64_t. Se não existir ou for inválida,
// devolve `default_val` (e imprime warning para o segundo caso).
static uint64_t env_uint64(const char *name, uint64_t default_val) {
  const char *raw = getenv(name);
  if (raw == NULL || *raw == '\0')
    return default_val;

  char *end = NULL;
  unsigned long long parsed = strtoull(raw, &end, 10);
  if (end == raw || *end != '\0') {
    fprintf(stderr, "[alloc-policy] WARN: env var %s='%s' inválida, usando default %lu\n",
            name, raw, (unsigned long)default_val);
    return default_val;
  }
  return (uint64_t)parsed;
}

void alloc_config_load_from_env(AllocConfig *cfg) {
  // Política — default storage_first.
  const char *raw_policy = getenv("DEDUP_ALLOC_POLICY");
  if (raw_policy == NULL || *raw_policy == '\0') {
    cfg->policy = ALLOC_STORAGE_FIRST;
  } else if (strcmp(raw_policy, "storage_first") == 0) {
    cfg->policy = ALLOC_STORAGE_FIRST;
  } else if (strcmp(raw_policy, "syscall_first") == 0) {
    cfg->policy = ALLOC_SYSCALL_FIRST;
  } else {
    fprintf(stderr,
            "[alloc-policy] WARN: DEDUP_ALLOC_POLICY='%s' desconhecido, "
            "usando 'storage_first'\n",
            raw_policy);
    cfg->policy = ALLOC_STORAGE_FIRST;
  }

  cfg->threshold = env_uint64("DEDUP_THRESHOLD", 4);
  cfg->max_fragments = env_uint64("DEDUP_MAX_FRAGMENTS", 0);
}

void alloc_config_log(const AllocConfig *cfg) {
  const char *name = (cfg->policy == ALLOC_STORAGE_FIRST)
                         ? "storage_first"
                         : "syscall_first";
  printf("[alloc-policy] policy=%s threshold=%lu max_fragments=%lu\n", name,
         (unsigned long)cfg->threshold, (unsigned long)cfg->max_fragments);
}
