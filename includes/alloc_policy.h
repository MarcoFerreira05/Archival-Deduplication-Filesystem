#ifndef ALLOC_POLICY_H
#define ALLOC_POLICY_H

#include <stdint.h>

// =============================================================================
// AllocPolicy / AllocConfig — configuração da política de alocação de
// master_blk no batching de escritas.
//
// Existem duas filosofias suportadas:
//
//   ALLOC_STORAGE_FIRST (default):
//     "Cada slot livre é sagrado." Drena sempre a free list antes de
//     fazer append, mesmo quando os extents disponíveis são pequenos
//     e a alocação resulta em master_blk dispersos. Master file não
//     cresce enquanto houver slots livres.
//     Trade-off: mais syscalls de escrita sob fragmentação.
//
//   ALLOC_SYSCALL_FIRST:
//     "Mantém o pwrite count baixo." Tenta primeiro alocar K slots
//     contíguos a partir da free list (1 syscall garantido). Se não
//     existir extent grande o suficiente, mas o maior disponível for
//     >= threshold, consome-o e completa por append. Se até o maior
//     for pequeno demais, faz append puro e ignora a free list — o
//     master file cresce mas o request resolve-se em 1 syscall.
//
// Configurável em runtime via variáveis de ambiente:
//   DEDUP_ALLOC_POLICY    = "storage_first" | "syscall_first"   (default: storage_first)
//   DEDUP_THRESHOLD       = inteiro >= 0                         (default: 4)
//   DEDUP_MAX_FRAGMENTS   = inteiro >= 0                         (default: 0 = ilimitado)
// =============================================================================

typedef enum {
  ALLOC_STORAGE_FIRST = 0,   // default
  ALLOC_SYSCALL_FIRST = 1,
} AllocPolicy;

typedef struct {
  AllocPolicy policy;
  uint64_t threshold;        // só relevante em ALLOC_SYSCALL_FIRST
  uint64_t max_fragments;    // reservado para storage-first; 0 = ilimitado
} AllocConfig;

// Lê DEDUP_ALLOC_POLICY / DEDUP_THRESHOLD / DEDUP_MAX_FRAGMENTS do ambiente
// e popula `cfg`. Defaults sãos quando as variáveis não estão definidas.
// Não falha — valores inválidos caem para defaults com warning para stderr.
void alloc_config_load_from_env(AllocConfig *cfg);

// Imprime a configuração actual para stdout no formato:
//   [alloc-policy] policy=storage_first threshold=4 max_fragments=0
// Útil para registo no arranque do FUSE e correlação com benchmarks.
void alloc_config_log(const AllocConfig *cfg);

#endif // ALLOC_POLICY_H
