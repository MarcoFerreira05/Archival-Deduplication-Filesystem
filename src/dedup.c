// =============================================================================
// dedup.c — Camada de deduplicação a nível de bloco.
//
// `write_dedup` é a função central, e foi refactorizada para funcionar em
// três passes:
//
//   Passe 1 (decisão): para cada bloco lógico do request, calcula o hash e
//                       decide se é HIT (já existe) ou MISS (novo). Para os
//                       MISSes, cria um MasterInfo "pendente" sem o inserir
//                       ainda no índice oficial. Aloca master_blk para todos
//                       os MISSes em conjunto, no fim deste passe.
//
//   Passe 2 (flush):    agrupa MISSes com master_blk consecutivos em runs e
//                       emite um único pwritev por run (pwrite quando run = 1).
//                       Em caso de falha, faz rollback completo das alocações.
//
//   Passe 3 (consolidação): só após o flush ter sucesso, insere os
//                       MasterInfos pendentes em hash_to_master e os pares
//                       (file, block) em file_to_master.
//
// Esta separação tem três objectivos:
//   1. Reduzir N pwrite a 1 pwritev no caso comum (master_blk contíguos).
//   2. Garantir que se o flush falhar, o índice oficial não fica com entradas
//      a apontar para blocos que nunca chegaram ao disco.
//   3. Permitir que o allocator decida globalmente para o batch (e não bloco
//      a bloco), o que é essencial para a política storage-first.
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
#include <sys/uio.h>   // pwritev / struct iovec
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "alloc_policy.h"
#include "dedup.h"
#include "freelist.h"
#include "hashing.h"
#include "metaindex.h"
#include "passthrough_helpers.h"

// -----------------------------------------------------------------------------
// Read path (inalterado conceptualmente — bloco a bloco com pread)
// -----------------------------------------------------------------------------

static int master_read(int fdMaster, off_t offset, char *buff) {
  return pread(fdMaster, buff, BLOCK_SIZE, offset);
}

// Single-lookup read: (file, blockIndex) -> MasterInfo -> master file
static int read_block(int fdMaster, const char *path, uint64_t block_index,
                      char *buff, Index *index) {
  MasterInfo *info = lookup_by_file_block(index, path, block_index);
  if (info == NULL)
    return -1;
  return master_read(fdMaster, info->masterBlockIndex * BLOCK_SIZE, buff);
}

// Read file content block by block from the master file.
// For each logical block, we look up the MasterInfo and read from the master.
int read_dedup(Index *index, const char *path, char *buf, size_t size,
               off_t offset, int masterFd) {
  ssize_t total_read = 0;
  size_t num_blocks = size / BLOCK_SIZE;
  uint64_t start_block = offset / BLOCK_SIZE;
  int res = 0;
  for (size_t i = 0; i < num_blocks; i++) {
    char buff[BLOCK_SIZE];
    res = read_block(masterFd, path, start_block + i, buff, index);
    if (res == -1) {
      return res;
    }
    memcpy(buf + i * BLOCK_SIZE, buff, BLOCK_SIZE);
    total_read += BLOCK_SIZE;
  }
  return total_read;
}

// -----------------------------------------------------------------------------
// Remove single-block reference. Usado por xmp_unlink/xmp_truncate em loop.
// -----------------------------------------------------------------------------

void remove_block_dedup(Index *index, const char *path, uint64_t blockIndex) {
  MasterInfo *info = lookup_by_file_block(index, path, blockIndex);
  if (info == NULL)
    return;

  remove_file_block(index, path, blockIndex);
  info->refcount--;

  // Se mais ninguém referencia este bloco, devolve o slot à free list.
  // O freelist_release faz coalescing com vizinhos adjacentes
  // automaticamente (ver freelist.c).
  if (info->refcount == 0) {
    freelist_release(&index->free_list, info->masterBlockIndex);
    remove_hash(index, info->hash);
    free(info);
  }
}

// =============================================================================
// write_dedup — Reformulado em dois passes (Passe 1: decisão; Passe 2: flush;
// Passe 3: consolidação).
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
// Allocators — uma função por filosofia + um switch unificado.
// -----------------------------------------------------------------------------

// Storage-first: drena sempre a free list antes de fazer append. O master
// nunca cresce enquanto houver slots livres.
//
// Estratégia:
//   - Se a free list tem >= miss_count slots livres, todos vêm de lá
//     (best-fit por varrimento, possivelmente repartido por vários extents).
//   - Caso contrário, drena tudo o que houver e completa por append.
static void allocate_storage_first(Index *idx, uint64_t miss_count,
                                    uint64_t *next_block_index,
                                    uint64_t *out) {
  uint64_t total = freelist_total_free(&idx->free_list);

  if (total >= miss_count) {
    // Caso ideal: a free list chega para tudo, o master não cresce.
    freelist_take(&idx->free_list, miss_count, out);
    return;
  }

  // Caso misto: drena a free list e completa com append.
  uint64_t taken = 0;
  if (total > 0) {
    taken = freelist_take(&idx->free_list, total, out);
  }
  for (uint64_t i = taken; i < miss_count; i++) {
    out[i] = (*next_block_index)++;
  }
}

// Syscall-first: tenta minimizar o número de syscalls de escrita por
// request, mesmo crescendo o master quando a free list está fragmentada.
//
// Estratégia em três níveis:
//   1. Se existe um extent contínuo >= miss_count → 1 syscall garantido.
//   2. Senão, se o maior extent disponível >= threshold → consome-o e
//      completa por append. 2 syscalls.
//   3. Senão, append puro ignora a free list → 1 syscall, mas o master
//      cresce miss_count blocos.
static void allocate_syscall_first(Index *idx, uint64_t miss_count,
                                    uint64_t *next_block_index,
                                    uint64_t threshold, uint64_t *out) {
  // 1. Run contíguo grande o suficiente: caminho ideal.
  if (freelist_take_if_exists_run(&idx->free_list, miss_count, out))
    return;

  // 2. Maior extent é "razoável": consome-o e completa por append.
  Extent largest = freelist_largest(&idx->free_list);
  if (largest.length >= threshold) {
    uint64_t taken = freelist_take(&idx->free_list, largest.length, out);
    for (uint64_t i = taken; i < miss_count; i++) {
      out[i] = (*next_block_index)++;
    }
    return;
  }

  // 3. Free list demasiado fragmentada para esta filosofia: append puro.
  // (A free list fica intacta — slots órfãos, mas o request resolve-se
  //  numa única operação de pwritev.)
  for (uint64_t i = 0; i < miss_count; i++) {
    out[i] = (*next_block_index)++;
  }
}

// Switch unificado: escolhe a implementação consoante a política activa.
static void allocate_batch(Index *idx, const AllocConfig *cfg,
                            uint64_t miss_count, uint64_t *next_block_index,
                            uint64_t *out) {
  switch (cfg->policy) {
    case ALLOC_STORAGE_FIRST:
      allocate_storage_first(idx, miss_count, next_block_index, out);
      break;
    case ALLOC_SYSCALL_FIRST:
      allocate_syscall_first(idx, miss_count, next_block_index, cfg->threshold,
                             out);
      break;
  }
}

// -----------------------------------------------------------------------------
// flush_plan — agrupa MISSes em runs contíguos no master e emite I/O.
// -----------------------------------------------------------------------------
//
// Um "run" é uma sequência de PlanEntries MISS consecutivas no plan cujos
// master_blk são também consecutivos. Quando isso acontece, podemos emitir
// um único pwritev em vez de N pwrite — N syscalls a 1.
//
// Runs de tamanho 1 degeneram para pwrite (igualmente eficiente).
// HITs no meio do plan não impedem runs longos: simplesmente saltam-se,
// porque os HITs não geram I/O.
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
    // cujo master_blk seja exactamente o anterior + 1.
    size_t run_start = i;
    size_t run_len = 1;
    i++;
    while (i < n && plan[i].kind == PLAN_MISS &&
           plan[i].master_blk == plan[i - 1].master_blk + 1) {
      run_len++;
      i++;
    }

    // Construir o vector de iovecs (pode não ser preciso, mas mantém o
    // código simétrico entre run=1 e run>1).
    struct iovec iov[run_len];
    for (size_t j = 0; j < run_len; j++) {
      // pwritev/pwrite escrevem para offsets do ficheiro, não modificam
      // o buffer de origem. O cast para void* é seguro.
      iov[j].iov_base = (void *)plan[run_start + j].payload;
      iov[j].iov_len = BLOCK_SIZE;
    }

    off_t offset = (off_t)plan[run_start].master_blk * BLOCK_SIZE;
    ssize_t written = pwritev(masterFd, iov, (int)run_len, offset);

    if (written != (ssize_t)(run_len * BLOCK_SIZE)) {
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
//   - `*next_block_index` é restaurado ao valor pré-batch, o que é seguro
//     porque o mutex grosseiro impede que outra escrita o tenha tocado.
static void rollback_allocations(Index *idx, PlanEntry *plan, size_t n,
                                  uint64_t *next_block_index,
                                  uint64_t saved_next) {
  // Reverter HITs: decrementa refcount; se for um MasterInfo já no índice
  // oficial e o refcount cair a 0, restaurá-lo seria muito invasivo, e não
  // acontece neste cenário de erro local (o MasterInfo só estava no índice
  // antes deste batch porque ALGUÉM ainda o referenciava).
  // Para MasterInfos pendentes (intra-batch), a libertação acontece abaixo,
  // no caminho dos MISSes.
  for (size_t i = 0; i < n; i++) {
    if (plan[i].kind == PLAN_HIT) {
      plan[i].info->refcount--;
    }
  }

  // Reverter MISSes: devolver master_blk à free list, libertar MasterInfo
  // pendente. A free list aceita slots em qualquer ordem; o coalescing
  // funde-os automaticamente se forem adjacentes.
  for (size_t i = 0; i < n; i++) {
    if (plan[i].kind == PLAN_MISS) {
      freelist_release(&idx->free_list, plan[i].master_blk);
      free(plan[i].info);
    }
  }

  // Restaurar nextBlockIndex. Seguro sob mutex grosseiro.
  *next_block_index = saved_next;
}

// -----------------------------------------------------------------------------
// write_dedup — entrada pública.
// -----------------------------------------------------------------------------

int write_dedup(Index *index, const AllocConfig *cfg, const char *path,
                const char *buf, size_t size, off_t offset, int masterFd,
                uint64_t *nextBlockIndex) {
  // Caso degenerado: write de 0 bytes não faz nada.
  if (size == 0)
    return 0;

  size_t num_blocks = size / BLOCK_SIZE;
  if (num_blocks == 0)
    return 0;

  uint64_t start_block = offset / BLOCK_SIZE;
  uint64_t saved_next = *nextBlockIndex;

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
  // PASSE 1 — Decisão. Sem I/O, sem inserção definitiva.
  // ---------------------------------------------------------------------------
  size_t miss_count = 0;
  for (size_t i = 0; i < num_blocks; i++) {
    plan[i].logical_blk = start_block + i;
    plan[i].payload = buf + i * BLOCK_SIZE;

    // Calcular hash do bloco directamente a partir do buffer do utilizador
    // (evita uma cópia para um array intermediário, ao contrário do código
    // original).
    hash((const unsigned char *)plan[i].payload, plan[i].hash);

    // Procurar primeiro no índice oficial; depois nos pendentes deste batch.
    MasterInfo *existing = lookup_by_hash(index, plan[i].hash);
    if (existing == NULL) {
      existing = g_hash_table_lookup(pending_hashes, plan[i].hash);
    }

    if (existing != NULL) {
      // HIT: o conteúdo já existe (ou no índice ou neste mesmo batch).
      // Aumenta o refcount; o MasterInfo é partilhado.
      plan[i].kind = PLAN_HIT;
      plan[i].info = existing;
      existing->refcount++;
    } else {
      // MISS: novo conteúdo. Cria-se o MasterInfo mas NÃO se insere em
      // hash_to_master ainda — só após o flush bem-sucedido (Passe 3).
      MasterInfo *info = g_new(MasterInfo, 1);
      memcpy(info->hash, plan[i].hash, HASH_SIZE);
      info->refcount = 1;
      info->masterBlockIndex = 0;  // preenchido na fase de alocação abaixo

      plan[i].kind = PLAN_MISS;
      plan[i].info = info;

      // Regista no batch para que o próximo bloco com o mesmo hash vire HIT.
      g_hash_table_insert(pending_hashes, info->hash, info);
      miss_count++;
    }
  }

  // ---------------------------------------------------------------------------
  // PASSE 1.5 — Alocação dos master_blk para todos os MISSes em conjunto.
  // ---------------------------------------------------------------------------
  if (miss_count > 0) {
    uint64_t master_blks_stack[256];
    uint64_t *master_blks =
        miss_count <= 256 ? master_blks_stack : g_new(uint64_t, miss_count);

    allocate_batch(index, cfg, miss_count, nextBlockIndex, master_blks);

    // Preenche master_blk em cada PlanEntry MISS, na ordem em que apareceram.
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
  // PASSE 2 — Flush. Emite pwritev/pwrite por runs contíguos.
  // ---------------------------------------------------------------------------
  if (miss_count > 0) {
    int rc = flush_plan(masterFd, plan, num_blocks);
    if (rc < 0) {
      // Erro de I/O: rollback completo. Free list, MasterInfos pendentes,
      // refcounts dos HITs e nextBlockIndex são restaurados.
      rollback_allocations(index, plan, num_blocks, nextBlockIndex,
                           saved_next);
      g_hash_table_destroy(pending_hashes);
      if (plan_heap)
        g_free(plan_heap);
      return rc;
    }
  }

  // ---------------------------------------------------------------------------
  // PASSE 3 — Consolidação. Só corre depois do flush ter passado.
  // Inserir os MasterInfos pendentes no hash_to_master, e os pares
  // (file, block) no file_to_master.
  // ---------------------------------------------------------------------------
  for (size_t i = 0; i < num_blocks; i++) {
    if (plan[i].kind == PLAN_MISS) {
      insert_hash(index, plan[i].info->hash, plan[i].info);
    }
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

  g_hash_table_destroy(pending_hashes);
  if (plan_heap)
    g_free(plan_heap);

  return (int)(num_blocks * BLOCK_SIZE);
}
