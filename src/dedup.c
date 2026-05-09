// =============================================================================
// dedup.c — Camada de deduplicação a nível de bloco.
//
// `write_dedup` é a função central, refactorizada em três passes:
//
//   Passe 1 (decisão): para cada bloco lógico do request, calcula o hash e
//                       decide se é HIT (já existe) ou MISS (novo). Para os
//                       MISSes, cria um MasterInfo "pendente" sem o inserir
//                       ainda no índice oficial.
//
//   Passe 1.5 (alocação): allocate_batch_storage_first atribui master_blk a
//                       todos os MISSes — drena a free list LIFO antes de
//                       fazer append (storage-first).
//
//   Passe 2 (flush):    agrupa MISSes com master_blk consecutivos em runs e
//                       emite um único pwrite por run (pwrite individual
//                       quando run = 1). Reusos da free list resultam em
//                       runs de 1 (master_blk dispersos); appends agregam-se
//                       num único run contíguo.
//                       Em caso de falha, faz rollback completo.
//
//   Passe 3 (consolidação): só após o flush ter sucesso, insere os
//                       MasterInfos pendentes em hash_to_master e os pares
//                       (file, block) em file_to_master.
//
// Esta separação tem três objectivos:
//   1. Reduzir N pwrite a 1 pwrite no caso comum (master_blk contíguos).
//   2. Garantir que se o flush falhar, o índice oficial não fica com entradas
//      a apontar para blocos que nunca chegaram ao disco.
//   3. Permitir que o allocator decida globalmente para o batch (e não bloco
//      a bloco), o que é essencial para a política storage-first.
//
// `read_dedup` faz batching análogo no lado da leitura: ordena os blocos
// pelo master_block_index, agrupa em runs físicos consecutivos e faz
// 1 pread por run.
// =============================================================================

#include <stddef.h>
#define FUSE_USE_VERSION 31

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse3/fuse.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "dedup.h"
#include "hashing.h"
#include "metaindex.h"
#include "passthrough_helpers.h"

static int cmp_master_idx(const void *a, const void *b) {
  uint64_t ma = ((const BlockPair *)a)->master_block_index;
  uint64_t mb = ((const BlockPair *)b)->master_block_index;
  return (ma > mb) - (ma < mb);
}

// Read file with batching optimization.
// Phase 1: Lookup all blocks to get their master positions
// Phase 2: Sort pairs by master block index
// Phase 3: Identify consecutive groups in physical space
// Phase 4: Read each group with a single pread
// Phase 5: Copy blocks to correct positions in output
int read_dedup(Index *index, const char *path, char *buf, size_t size,
               off_t offset, int masterFd) {
  size_t num_blocks = size / BLOCK_SIZE;
  uint64_t start_block = offset / BLOCK_SIZE;

  if (num_blocks == 0)
    return 0;

  // Phase 1: Lookup all blocks and create pairs.
  // Read lock no metadata_rwlock — múltiplos read_dedup correm em
  // paralelo. Copiamos master_block_index para o array local; NÃO
  // guardamos o ponteiro MasterInfo* (pode ser libertado por
  // remove_block_dedup assim que libertarmos o read lock).
  BlockPair pairs[num_blocks];
  pthread_rwlock_rdlock(&index->metadata_rwlock);
  for (size_t i = 0; i < num_blocks; i++) {
    MasterInfo *info = lookup_by_file_block(index, path, start_block + i);
    if (info == NULL) {
      pthread_rwlock_unlock(&index->metadata_rwlock);
      return -1;
    }
    pairs[i].logical_idx = i;
    pairs[i].master_block_index = info->masterBlockIndex;
  }
  pthread_rwlock_unlock(&index->metadata_rwlock);

  // Phases 2-5 correm SEM LOCK. Os pairs[] são locais, e os preads
  // a offsets disjuntos no master file são thread-safe por POSIX.

  // Phase 2: Sort pairs by master block index
  if (num_blocks <= INSERTION_SORT_THRESHOLD) {
    insertion_sort(pairs, num_blocks);
  } else {
    qsort(pairs, num_blocks, sizeof(BlockPair), cmp_master_idx);
  }

  // Allocate single buffer for all reads not one per group
  char *master_buf = malloc(num_blocks * BLOCK_SIZE);
  if (master_buf == NULL)
    return -ENOMEM;

  // Phase 3 & 4: Identify groups and read them
  size_t group_start = 0;
  for (size_t i = 1; i <= num_blocks; i++) {
    int is_last = (i == num_blocks);

    int is_consec = !is_last && (pairs[i].master_block_index ==
                                 pairs[i - 1].master_block_index + 1);
    if (!is_last && is_consec)
      continue;

    // End of current group - read it
    uint64_t min_master = pairs[group_start].master_block_index;
    size_t blocks_in_group = i - group_start;

    // Fast path: single block group
    if (blocks_in_group == 1) {
      size_t logical_idx = pairs[group_start].logical_idx;
      char *dst = buf + logical_idx * BLOCK_SIZE;
      ssize_t res = pread(masterFd, dst, BLOCK_SIZE, min_master * BLOCK_SIZE);
      if (res != BLOCK_SIZE) {
        free(master_buf);
        return -1;
      }
    } else {
      size_t read_size = blocks_in_group * BLOCK_SIZE;
      ssize_t res =
          pread(masterFd, master_buf, read_size, min_master * BLOCK_SIZE);
      if (res != (ssize_t)read_size) {
        free(master_buf);
        return -1;
      }
      // Phase 5: Copy each block to correct position in output
      for (size_t j = group_start; j < i; j++) {
        size_t logical_idx = pairs[j].logical_idx;
        uint64_t offset_in_range = pairs[j].master_block_index - min_master;
        char *src = master_buf + offset_in_range * BLOCK_SIZE;
        char *dst = buf + logical_idx * BLOCK_SIZE;
        memcpy(dst, src, BLOCK_SIZE);
      }
    }
    group_start = i;
  }
  free(master_buf);
  return size;
}

// -----------------------------------------------------------------------------
// Remove single-block reference. Usado por xmp_unlink/xmp_truncate em loop.
// -----------------------------------------------------------------------------

// PRECONDITION: caller deve segurar metadata_rwlock em write mode.
// O cleanup do MasterInfo (quando refcount cai a 0) tem de acontecer
// sob write lock para excluir leitores que ainda possam ter o ponteiro.
void remove_block_dedup(Index *index, const char *path, uint64_t blockIndex) {
  MasterInfo *info = lookup_by_file_block(index, path, blockIndex);
  if (info == NULL)
    return;

  remove_file_block(index, path, blockIndex);

  // fetch_sub atómico para coordenar com increments paralelos sob read
  // lock. ACQ_REL: RELEASE garante que escritas anteriores ao info
  // ficam visíveis se for cleanup; ACQUIRE garante que vemos estado
  // estabilizado se ganharmos a corrida ao 0.
  // Se fetch_sub retornar 1, o valor antes do dec era 1, agora é 0 —
  // somos o último a soltar a referência.
  if (__atomic_fetch_sub(&info->refcount, 1, __ATOMIC_ACQ_REL) == 1) {
    // Devolve o slot à free list. Pega freelist_mutex isoladamente
    // (já temos metadata_rwlock em wr; ordem hierárquica respeitada:
    // metadata > freelist).
    uint64_t *slot = malloc(sizeof(uint64_t));
    *slot = info->masterBlockIndex;
    pthread_mutex_lock(&index->freelist_mutex);
    index->free_block_list = g_slist_prepend(index->free_block_list, slot);
    pthread_mutex_unlock(&index->freelist_mutex);

    remove_hash(index, info->hash);
    free(info);
  }
}

// =============================================================================
// write_dedup — três passes (Passe 1: decisão; Passe 1.5: alocação;
// Passe 2: flush; Passe 3: consolidação).
// =============================================================================

// Estrutura efémera que representa cada bloco lógico do request durante
// o processamento. Vive na stack durante write_dedup e é descartada no fim.
typedef enum { PLAN_HIT, PLAN_MISS } PlanKind;

typedef struct {
  PlanKind kind;
  uint64_t logical_blk;            // bloco lógico no ficheiro
  uint64_t master_blk;              // só MISS: slot atribuído no master
  const char *payload;              // só MISS: ptr para dentro de buf (BLOCK_SIZE bytes)
  unsigned char hash[HASH_SIZE];   // hash do conteúdo deste bloco
  MasterInfo *info;                 // partilhado: HIT → existente; MISS → pendente
} PlanEntry;

// -----------------------------------------------------------------------------
// Allocator storage-first
// -----------------------------------------------------------------------------
//
// Filosofia: usar SEMPRE a free list antes de fazer append. Storage-first
// — só fazemos crescer o master file quando a free list está vazia.
//
// Estratégia (LIFO O(1)):
//   - Drena slots da head do GSList enquanto houver e ainda faltarem
//     blocos no batch.
//   - Resto vai por append (incremento de nextBlockIndex).
//
// Sem coalescing nem best-fit. Os slots saem na ordem LIFO em que foram
// libertados, o que é suficiente: o caso comum no nosso workload é
// append puro (free list vazia), e os reusos individuais não beneficiariam
// de batching porque master_blk dispersos não cabem num único pwrite.
static void allocate_batch_storage_first(Index *idx, uint64_t miss_count,
                                          uint64_t *next_block_index,
                                          uint64_t *out) {
  uint64_t taken = 0;

  // Drena a free list pela head (LIFO). Lock isolado à parte que toca
  // o GSList — tirado mal acaba a drenagem.
  pthread_mutex_lock(&idx->freelist_mutex);
  while (taken < miss_count && idx->free_block_list != NULL) {
    GSList *head = idx->free_block_list;
    uint64_t *slot = head->data;
    out[taken++] = *slot;
    idx->free_block_list = g_slist_delete_link(idx->free_block_list, head);
    free(slot);
  }
  pthread_mutex_unlock(&idx->freelist_mutex);

  // Remainder por append: o master cresce só agora, e só o necessário.
  // Reserva atómica de range contíguo — múltiplas threads appendam em
  // paralelo SEM lock, em ranges disjuntos por construção.
  // RELAXED memory order: counter puro; o pwrite que se segue passa
  // por syscall do kernel, com as suas próprias barreiras.
  uint64_t remaining = miss_count - taken;
  if (remaining > 0) {
    uint64_t base = __atomic_fetch_add(next_block_index, remaining,
                                        __ATOMIC_RELAXED);
    for (uint64_t i = 0; i < remaining; i++) {
      out[taken + i] = base + i;
    }
  }
}

// -----------------------------------------------------------------------------
// flush_plan — agrupa MISSes em runs contíguos no master e emite I/O.
// -----------------------------------------------------------------------------
//
// Um "run" é uma sequência de PlanEntries MISS consecutivas no plan cujos
// master_blk são também consecutivos. Quando isso acontece, podemos emitir
// um único pwrite que cobre todos os blocos do run de uma vez — N syscalls
// a 1.
//
// Runs de tamanho 1 degeneram para 1 pwrite de 4 KiB.
// HITs no meio do plan INTERROMPEM runs: se houver um HIT entre dois MISSes,
// não é possível juntá-los num único pwrite porque os seus payloads não são
// contíguos na memória (o buffer de input tem os dados do HIT no meio).
// HITs só são "saltados" quando aparecem no início de uma janela não processada
// (o while externo), nunca dentro de um run activo.
//
// Como funciona sem pwritev: dentro de um run os payloads são FISICAMENTE
// contíguos no buffer de input do utilizador (a ordem do plan é a ordem
// dos blocos lógicos no buf). Logo o run inteiro pode ser escrito com um
// único pwrite que parte de plan[run_start].payload e cobre
// run_len * BLOCK_SIZE.
//
// Retorna 0 em sucesso, -errno em falha. O caller é responsável pelo
// rollback em caso de falha.
static int flush_plan(int masterFd, PlanEntry *plan, size_t n) {
  size_t i = 0;
  while (i < n) {
    // Saltar HITs — não geram I/O.
    while (i < n && plan[i].kind == PLAN_HIT) {
      i++;
    }
    if (i == n) break;

    // Construir um run a partir do MISS actual, juntando MISSes seguintes
    // cujo master_blk seja exactamente o anterior + 1. Como o plan é
    // percorrido pela ordem dos blocos lógicos, MISSes consecutivos no
    // plan estão também em posições consecutivas no buffer de input —
    // ou seja, o run é contíguo em memória e no master.
    size_t run_start = i;
    size_t run_len = 1;
    i++;
    while (i < n && plan[i].kind == PLAN_MISS &&
           plan[i].master_blk == plan[i - 1].master_blk + 1) {
      run_len++;
      i++;
    }

    off_t offset = (off_t)plan[run_start].master_blk * BLOCK_SIZE;
    size_t bytes = run_len * BLOCK_SIZE;
    // Um único pwrite cobre o run inteiro. Para run_len==1 fica a
    // semântica clássica de pwrite de um bloco isolado.
    ssize_t written = pwrite(masterFd, plan[run_start].payload, bytes, offset);

    if (written != (ssize_t)bytes) {
      // Falha total ou parcial: para simplificar, tratamos qualquer caso
      // como erro. O caller faz rollback.
      return (written < 0) ? -errno : -EIO;
    }
  }
  return 0;
}

// -----------------------------------------------------------------------------
// rollback — desfaz alocações e estado pendente quando o flush falha.
// -----------------------------------------------------------------------------
//
// Em caso de falha de I/O temos de garantir que:
//   - Os master_blk reservados voltam à free list (para futuras escritas).
//   - Os MasterInfos pendentes (criados nos MISSes deste batch) são libertos.
//   - Os HITs que tiveram refcount++ no Passe 1 são revertidos. Se um HIT
//     for para um MasterInfo PENDENTE deste mesmo batch (intra-batch dedup),
//     a libertação fica a cargo do MISS original.
//
// Nota sobre nextBlockIndex: NÃO restauramos nextBlockIndex porque os blocos
// alocados por append já foram adicionados à free list (via g_slist_prepend
// na secção dos MISSes abaixo). Restaurar nextBlockIndex criaria uma dupla
// alocação: o mesmo índice ficaria tanto na free list como no ponto de
// append, podendo ser atribuído duas vezes na próxima escrita.
static void rollback_allocations(Index *idx, PlanEntry *plan, size_t n) {
  // Reverter HITs: decrementa refcount; se for um MasterInfo já no índice
  // oficial e o refcount cair a 0, restaurá-lo seria muito invasivo, e não
  // acontece neste cenário de erro local (o MasterInfo só estava no índice
  // antes deste batch porque ALGUÉM ainda o referenciava).
  // Para MasterInfos pendentes (intra-batch), a libertação acontece abaixo,
  // no caminho dos MISSes.
  for (size_t i = 0; i < n; i++) {
    if (plan[i].kind == PLAN_HIT) {
      // Atomic decrement para reverter o increment do Passe 1.
      // RELAXED: counter puro, sem ordering com mais nada.
      __atomic_fetch_sub(&plan[i].info->refcount, 1, __ATOMIC_RELAXED);
    }
  }

  // Reverter MISSes: devolver master_blk à free list (prepend O(1)),
  // libertar MasterInfo pendente. Tanto slots retirados da free list como
  // slots alocados por append são devolvidos aqui — serão reutilizados nas
  // próximas escritas via pop da head do GSList (LIFO).
  //
  // É por isto que NÃO restauramos nextBlockIndex: os índices alocados por
  // append já estão na free list, restaurar nextBlockIndex criaria uma
  // dupla alocação (mesmo índice na free list E como próximo append).
  //
  // freelist_mutex pegue 1 vez para todos os prepends (em vez de N vezes).
  pthread_mutex_lock(&idx->freelist_mutex);
  for (size_t i = 0; i < n; i++) {
    if (plan[i].kind == PLAN_MISS) {
      uint64_t *slot = malloc(sizeof(uint64_t));
      *slot = plan[i].master_blk;
      idx->free_block_list = g_slist_prepend(idx->free_block_list, slot);
      free(plan[i].info);
    }
  }
  pthread_mutex_unlock(&idx->freelist_mutex);
}

// -----------------------------------------------------------------------------
// write_dedup — entrada pública.
// -----------------------------------------------------------------------------

int write_dedup(Index *index, const char *path, const char *buf, size_t size,
                off_t offset, int masterFd, uint64_t *nextBlockIndex) {
  // Caso degenerado: write de 0 bytes não faz nada.
  if (size == 0)
    return 0;

  size_t num_blocks = size / BLOCK_SIZE;
  if (num_blocks == 0)
    return 0;

  uint64_t start_block = offset / BLOCK_SIZE;

  // Plan vive na stack quando num_blocks é razoável. Para requests muito
  // grandes (muito raros — FUSE max_write costuma ser <= 1 MiB = 256 blocos),
  // cai para o heap.
  PlanEntry *plan;
  PlanEntry plan_stack[256];
  PlanEntry *plan_heap = NULL;
  if (num_blocks <= 256) {
    plan = plan_stack;
  } else {
    plan_heap = g_new(PlanEntry, num_blocks);
    plan = plan_heap;
  }

  // pending_hashes: tabela efémera que regista MISSes deste batch que ainda
  // não foram inseridos no hash_to_master oficial. Necessária para tratar
  // intra-batch dedup: se dois blocos do request têm o mesmo conteúdo, o
  // primeiro vira MISS e o segundo deve ser HIT, mesmo antes do flush.
  GHashTable *pending_hashes =
      g_hash_table_new(hash512_hash, hash512_equal);

  // ---------------------------------------------------------------------------
  // PASSE 1 — Decisão. Hash + lookup. Read lock no metadata_rwlock
  // permite múltiplos write_dedup correrem o Passe 1 em paralelo (e em
  // paralelo com read_dedup).
  // ---------------------------------------------------------------------------
  size_t miss_count = 0;
  pthread_rwlock_rdlock(&index->metadata_rwlock);
  for (size_t i = 0; i < num_blocks; i++) {
    plan[i].logical_blk = start_block + i;
    plan[i].payload = buf + i * BLOCK_SIZE;

    // Calcular hash directamente a partir do buf do utilizador.
    hash((const unsigned char *)plan[i].payload, plan[i].hash);

    // Procurar primeiro no índice oficial; depois nos pendentes deste batch.
    MasterInfo *existing = lookup_by_hash(index, plan[i].hash);
    if (existing == NULL) {
      existing = g_hash_table_lookup(pending_hashes, plan[i].hash);
    }

    if (existing != NULL) {
      // HIT: incrementa refcount atomicamente. RELAXED chega — o read
      // lock garante visibilidade do MasterInfo.
      plan[i].kind = PLAN_HIT;
      plan[i].info = existing;
      __atomic_fetch_add(&existing->refcount, 1, __ATOMIC_RELAXED);
    } else {
      // MISS: cria MasterInfo pendente. Ainda não publicado em
      // hash_to_master — só no Passe 3 após o pwrite ter sucesso.
      MasterInfo *info = g_new(MasterInfo, 1);
      memcpy(info->hash, plan[i].hash, HASH_SIZE);
      atomic_store_explicit(&info->refcount, 1, memory_order_relaxed);
      info->masterBlockIndex = 0;

      plan[i].kind = PLAN_MISS;
      plan[i].info = info;

      g_hash_table_insert(pending_hashes, info->hash, info);
      miss_count++;
    }
  }
  pthread_rwlock_unlock(&index->metadata_rwlock);

  // ---------------------------------------------------------------------------
  // PASSE 1.5 — Alocação. Pega freelist_mutex breve + atomic fetch_add.
  // ---------------------------------------------------------------------------
  if (miss_count > 0) {
    uint64_t master_blks_stack[256];
    uint64_t *master_blks =
        miss_count <= 256 ? master_blks_stack : g_new(uint64_t, miss_count);

    // allocate_batch_storage_first faz locking interno: pega
    // freelist_mutex breve durante a drenagem, depois usa
    // __atomic_fetch_add no nextBlockIndex sem qualquer lock.
    allocate_batch_storage_first(index, miss_count, nextBlockIndex,
                                  master_blks);

    // Preenche master_blk em cada PlanEntry MISS.
    size_t m = 0;
    for (size_t i = 0; i < num_blocks; i++) {
      if (plan[i].kind == PLAN_MISS) {
        plan[i].master_blk = master_blks[m];
        plan[i].info->masterBlockIndex = master_blks[m];
        m++;
      }
    }

    if (master_blks != master_blks_stack)
      g_free(master_blks);
  }

  // ---------------------------------------------------------------------------
  // PASSE 2 — Flush. SEM LOCK. Múltiplas threads emitem pwrite em paralelo
  // a offsets disjuntos no master file (POSIX garante atomicidade ao nível
  // do fd). Esta é a peça grande do paralelismo.
  // ---------------------------------------------------------------------------
  if (miss_count > 0) {
    int rc = flush_plan(masterFd, plan, num_blocks);
    if (rc < 0) {
      // Erro de I/O: rollback completo (pega locks internamente).
      rollback_allocations(index, plan, num_blocks);
      g_hash_table_destroy(pending_hashes);
      if (plan_heap)
        g_free(plan_heap);
      return rc;
    }
  }

  // ---------------------------------------------------------------------------
  // PASSE 3 — Consolidação. Write lock no metadata_rwlock. Inclui
  // double-check insert: relookup ao hash antes de inserir, para fechar
  // a race com outras threads que possam ter inserido o mesmo conteúdo
  // entretanto. Custo: pwrite ocasional desperdiçado, sem corrupção
  // nem duplicados em hash_to_master.
  // ---------------------------------------------------------------------------
  GSList *slots_to_release = NULL;

  pthread_rwlock_wrlock(&index->metadata_rwlock);

  for (size_t i = 0; i < num_blocks; i++) {
    if (plan[i].kind != PLAN_MISS)
      continue;

    // Double-check: outro thread pode ter inserido o mesmo hash entre
    // o nosso Passe 1 e este Passe 3.
    MasterInfo *winner = lookup_by_hash(index, plan[i].info->hash);
    if (winner != NULL) {
      // Outra thread venceu a race. O nosso pwrite no slot
      // plan[i].master_blk fica desperdiçado — devolve à free list.
      uint64_t *slot = malloc(sizeof(uint64_t));
      *slot = plan[i].master_blk;
      slots_to_release = g_slist_prepend(slots_to_release, slot);

      // Liberta o nosso MasterInfo pendente; usa o vencedor.
      g_free(plan[i].info);
      plan[i].info = winner;
      __atomic_fetch_add(&winner->refcount, 1, __ATOMIC_RELAXED);
    } else {
      // Nós ganhamos: inserimos o nosso MasterInfo no índice oficial.
      insert_hash(index, plan[i].info->hash, plan[i].info);
    }
  }

  // Insere os mapeamentos (file, block) → MasterInfo para todos.
  for (size_t i = 0; i < num_blocks; i++) {
    insert_file_block(index, path, plan[i].logical_blk, plan[i].info);
  }

  // Actualiza o tamanho lógico do ficheiro (cresce, nunca encolhe aqui).
  size_t new_end = (start_block + num_blocks) * BLOCK_SIZE;
  size_t *logical_size = g_hash_table_lookup(index->file_to_sizes, path);
  if (logical_size == NULL) {
    logical_size = malloc(sizeof(size_t));
    *logical_size = new_end;
    g_hash_table_insert(index->file_to_sizes, g_strdup(path), logical_size);
  } else if (new_end > *logical_size) {
    *logical_size = new_end;
  }

  pthread_rwlock_unlock(&index->metadata_rwlock);

  // Devolve à free list os slots descartados pelo double-check (raro).
  if (slots_to_release != NULL) {
    pthread_mutex_lock(&index->freelist_mutex);
    index->free_block_list =
        g_slist_concat(slots_to_release, index->free_block_list);
    pthread_mutex_unlock(&index->freelist_mutex);
  }

  g_hash_table_destroy(pending_hashes);
  if (plan_heap)
    g_free(plan_heap);

  return (int)(num_blocks * BLOCK_SIZE);
}
