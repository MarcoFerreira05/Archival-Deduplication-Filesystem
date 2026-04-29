#ifndef FREELIST_H
#define FREELIST_H

#include <glib.h>
#include <stdint.h>

// =============================================================================
// FreeList — lista de slots livres no master file, organizada como mapa
// de extents (intervalos contíguos).
//
// Cada Extent representa um intervalo de slots livres consecutivos:
//     [start, start+1, ..., start+length-1]
//
// Os extents são guardados num GTree ordenado por `start`, o que nos dá:
//   - Lookup O(log F) de um extent que comece numa posição concreta.
//   - Walk ordenado para iteração e persistência.
//   - Procura eficiente de vizinhos para fundir (coalescing on release).
//
// Invariantes globais do GTree `by_start`:
//   - Não há extents que se sobreponham.
//   - Não há extents adjacentes (`a.start + a.length == b.start` é proibido —
//     teriam de estar fundidos num só).
//   - Para todos os extents: `length >= 1`.
// =============================================================================

typedef struct extent {
  uint64_t start;   // primeiro masterBlk livre do intervalo
  uint64_t length;  // número de blocos consecutivos livres
} Extent;

typedef struct freelist {
  GTree *by_start;  // chave: uint64_t* (start, alocado), valor: Extent*

  /*
   * FUTURE WORK — Fase 3: índice secundário `by_length` para best-fit O(log F)
   *
   * Esta implementação usa best-fit por varrimento de `by_start` (O(F) por
   * alocação). Uma evolução possível é manter um segundo GTree indexado por
   * (length, start) para fazer best-fit em O(log F).
   *
   * PRÓS de adicionar `by_length`:
   *   - Alocação batch escala melhor com free lists grandes.
   *   - Permite estatísticas eficientes ("maior extent disponível", etc.).
   *   - Algoritmicamente mais elegante.
   *
   * CONTRAS:
   *   - Toda operação que altere length/start exige sincronizar dois índices
   *     (remove + insert em ambos, com chave composta no by_length). Os 4
   *     casos do coalescing passam de 2-3 operações cada para 6-8.
   *   - Chave composta (LengthKey { length, start }) precisa de comparator
   *     próprio.
   *   - Maior superfície para bugs de sincronização entre os dois índices.
   *   - O coalescing on release tende a manter F pequeno (dezenas), portanto
   *     O(F) ≈ O(log F) na prática.
   *
   * QUANDO ACTIVAR:
   *   Apenas se profiling mostrar que o varrimento de freelist consome > 1%
   *   do tempo de write, ou se workloads reais produzirem free lists com
   *   milhares de extents que o coalescing não consiga compactar.
   *
   * A API pública (freelist_take, freelist_release) ficaria inalterada —
   * toda a mudança é interna a freelist.c.
   */
} FreeList;

// -----------------------------------------------------------------------------
// Ciclo de vida
// -----------------------------------------------------------------------------

// Inicializa uma FreeList vazia. Não falha.
void freelist_init(FreeList *fl);

// Liberta toda a memória interna da FreeList (extents e GTree).
void freelist_destroy(FreeList *fl);

// -----------------------------------------------------------------------------
// Operações principais
// -----------------------------------------------------------------------------

// Tira até `k` blocos da freelist, preenchendo `out[0..taken-1]` com os
// master_blk escolhidos. Devolve quantos blocos foram efectivamente retirados
// (entre 0 e k). O caller é responsável por fazer append do remainder
// quando o retorno for < k.
//
// Estratégia: best-fit por varrimento. Procura o menor extent com
// length >= k; se existir, consome k blocos do início. Se nenhum extent
// chega para k, consome o maior extent disponível inteiro e recursa com
// k -= consumido até esgotar o pedido ou a freelist.
uint64_t freelist_take(FreeList *fl, uint64_t k, uint64_t *out);

// Devolve um bloco isolado à freelist, fundindo com vizinhos adjacentes
// se existirem (coalescing on release). Custo O(log F) — ver freelist.c
// para os 4 casos (sem vizinhos / só pred / só suc / ambos).
void freelist_release(FreeList *fl, uint64_t master_blk);

// Devolve um run inteiro de slots à freelist. Implementado como
// freelist_release em loop, beneficia automaticamente do coalescing.
void freelist_release_run(FreeList *fl, uint64_t start, uint64_t length);

// -----------------------------------------------------------------------------
// Inspecção
// -----------------------------------------------------------------------------

// Soma de `length` de todos os extents. Útil para o caller saber se a
// freelist tem o suficiente para um batch antes de chamar `freelist_take`.
uint64_t freelist_total_free(FreeList *fl);

// -----------------------------------------------------------------------------
// Persistência
// -----------------------------------------------------------------------------
//
// Formato em disco: header com nº de extents, seguido de pares
// (start, length) prefixados pelo tamanho do payload (ver freelist.c).
// O `freelist_load` detecta automaticamente o formato antigo (slots
// individuais como uint64_t) e migra-o para o formato novo via release
// + coalescing.
void freelist_save(const char *path, FreeList *fl);
void freelist_load(const char *path, FreeList *fl);

#endif // FREELIST_H
