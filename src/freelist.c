// =============================================================================
// freelist.c — Implementação da FreeList do master file.
//
// Porquê GTree (em vez de uma simples GSList):
//   - GTree é uma árvore binária balanceada (red-black por baixo, em glib),
//     ordenada pela chave. As operações de inserção, remoção e lookup são
//     O(log F).
//   - Como queremos *fundir* slots adjacentes ao libertar, precisamos de
//     conseguir encontrar rapidamente o "extent que termina em X-1" e o
//     "extent que começa em X+1". Com GSList isto seria O(F); com GTree
//     ordenado por `start`, é O(log F).
//   - Walk ordenado também é O(F) directo, útil para alocar (best-fit por
//     varrimento) e para persistir.
//
// Porquê "coalescing on release":
//   - Sem coalescing, cada slot libertado é um nó isolado. Apagar um ficheiro
//     de 100 blocos cria 100 nós; futuras alocações batch não veriam um run
//     contíguo, e o flush precisaria de muitos pwrite separados.
//   - Com coalescing, slots adjacentes fundem-se à medida que são libertados.
//     Apagar um ficheiro de 100 blocos resulta em 1 extent de tamanho 100.
//     A próxima escrita batch consome esse extent num único pwritev contíguo.
//
// Porquê só `by_start` nesta iteração:
//   - Para alocação best-fit O(log F) seria preciso um segundo índice por
//     length (Fase 3 do roadmap). Isso duplica a complexidade de cada release
//     (sincronizar dois índices) por um ganho que, com coalescing, é marginal:
//     em estado estável o número de extents F é pequeno (dezenas), portanto
//     O(F) ≈ O(log F).
//   - Ver bloco "FUTURE WORK" em freelist.h para os prós e contras detalhados.
// =============================================================================

#include "freelist.h"
#include <glib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
// Helpers de chave/destruição para o GTree
// -----------------------------------------------------------------------------

// Comparator de chaves (uint64_t*). Retorna negativo/zero/positivo conforme
// `a < b`, `a == b`, `a > b`. Assinatura compatível com GCompareDataFunc.
static gint compare_uint64(gconstpointer a, gconstpointer b, gpointer userdata) {
  (void)userdata;
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  if (x < y) return -1;
  if (x > y) return 1;
  return 0;
}

// O GTree é dono das chaves (uint64_t* alocadas) e dos valores (Extent*
// alocados). Estas funções fazem o free correspondente quando uma entrada
// é removida ou quando o GTree é destruído.
static void key_destroy(gpointer key)   { g_free(key); }
static void value_destroy(gpointer val) { g_free(val); }

// Insere um Extent novo no GTree. Aloca a chave a partir de `extent->start`.
// O GTree fica dono da chave e do valor.
static void tree_insert(GTree *tree, Extent *extent) {
  uint64_t *key = g_new(uint64_t, 1);
  *key = extent->start;
  g_tree_insert(tree, key, extent);
}

// -----------------------------------------------------------------------------
// Ciclo de vida
// -----------------------------------------------------------------------------

void freelist_init(FreeList *fl) {
  // GTree ordenado pela chave (uint64_t* start), com destrutores que libertam
  // tanto a chave como o valor (Extent*) quando uma entrada sai do GTree.
  fl->by_start = g_tree_new_full(compare_uint64, NULL, key_destroy, value_destroy);
}

void freelist_destroy(FreeList *fl) {
  if (fl->by_start) {
    g_tree_destroy(fl->by_start);
    fl->by_start = NULL;
  }
}

// -----------------------------------------------------------------------------
// Inspecção
// -----------------------------------------------------------------------------

// Estado partilhado pelo callback de soma.
typedef struct {
  uint64_t total;
} sum_state_t;

static gboolean sum_lengths_cb(gpointer key, gpointer value, gpointer data) {
  (void)key;
  Extent *e = (Extent *)value;
  sum_state_t *s = (sum_state_t *)data;
  s->total += e->length;
  return FALSE;  // FALSE = continuar a iterar
}

uint64_t freelist_total_free(FreeList *fl) {
  sum_state_t s = { .total = 0 };
  g_tree_foreach(fl->by_start, sum_lengths_cb, &s);
  return s.total;
}

// -----------------------------------------------------------------------------
// Best-fit por varrimento (Commit 1: simples e suficiente)
// -----------------------------------------------------------------------------
//
// FUTURE WORK — Fase 3: índice secundário by_length para best-fit O(log F).
// Ver bloco em includes/freelist.h junto à definição da struct.
//
// A estratégia abaixo varre o GTree by_start em ordem ascendente. Mantém
// dois ponteiros: o "menor extent que ainda chega para K" (best-fit
// estrito) e o "maior extent disponível" (fallback se ninguém chega).
// Walk completo é O(F); como F tende a ser pequeno em estado estável
// graças ao coalescing, o custo absoluto é desprezível face ao próprio
// pwrite/pwritev que acontece em seguida.

typedef struct {
  uint64_t k;
  Extent *best_fit;   // menor extent com length >= k (NULL se nenhum chega)
  Extent *largest;    // maior extent (sempre o melhor parcial)
} pick_state_t;

static gboolean pick_extent_cb(gpointer key, gpointer value, gpointer data) {
  (void)key;
  Extent *e = (Extent *)value;
  pick_state_t *s = (pick_state_t *)data;

  // Maior extent disponível: comparação simples por length.
  if (s->largest == NULL || e->length > s->largest->length) {
    s->largest = e;
  }

  // Best-fit: menor extent que ainda chega para K.
  if (e->length >= s->k) {
    if (s->best_fit == NULL || e->length < s->best_fit->length) {
      s->best_fit = e;
    }
  }

  return FALSE;  // continuar a iterar
}

// Remove um Extent do GTree (libertando memória) ou encolhe-o consumindo
// os primeiros `consumed` blocos. Devolve o `start` original (antes do consumo).
//
// Nota: como a chave do GTree é o `start`, encolher implica remover a
// entrada actual e re-inserir com a nova chave. Não há forma mais leve com
// glib porque alterar a chave em sítio quebraria as invariantes da árvore.
static uint64_t consume_from_extent(GTree *tree, Extent *extent, uint64_t consumed) {
  uint64_t original_start = extent->start;
  if (consumed == extent->length) {
    // Consumir extent inteiro → remover do GTree (que liberta key e value).
    g_tree_remove(tree, &extent->start);
  } else {
    // Consumir do início → mudar start e length, mas o GTree foi indexado
    // pela start original, então temos de remover e re-inserir.
    Extent *replacement = g_new(Extent, 1);
    replacement->start  = extent->start + consumed;
    replacement->length = extent->length - consumed;
    g_tree_remove(tree, &extent->start);   // liberta extent original
    tree_insert(tree, replacement);
  }
  return original_start;
}

uint64_t freelist_take(FreeList *fl, uint64_t k, uint64_t *out) {
  uint64_t taken = 0;

  while (taken < k) {
    uint64_t remaining = k - taken;

    pick_state_t s = { .k = remaining, .best_fit = NULL, .largest = NULL };
    g_tree_foreach(fl->by_start, pick_extent_cb, &s);

    if (s.best_fit != NULL) {
      // Caminho feliz: existe um extent que serve para o remainder. Tira
      // exactamente `remaining` blocos do início. Termina o pedido.
      uint64_t start = consume_from_extent(fl->by_start, s.best_fit, remaining);
      for (uint64_t i = 0; i < remaining; i++) {
        out[taken + i] = start + i;
      }
      taken += remaining;
      break;
    }

    if (s.largest == NULL) {
      // Free list vazia. Devolvemos o que conseguimos tirar até agora; o
      // caller faz append do remainder.
      break;
    }

    // Nenhum extent chega para o remainder, mas há pelo menos um disponível.
    // Consumimos o maior inteiro e voltamos a iterar com o que sobra.
    uint64_t consumed_length = s.largest->length;
    uint64_t start = consume_from_extent(fl->by_start, s.largest, consumed_length);
    for (uint64_t i = 0; i < consumed_length; i++) {
      out[taken + i] = start + i;
    }
    taken += consumed_length;
  }

  return taken;
}

// -----------------------------------------------------------------------------
// Release com coalescing — junta o slot libertado com vizinhos adjacentes.
// -----------------------------------------------------------------------------
//
// Quando um slot X é libertado, há quatro situações possíveis quanto aos
// extents existentes:
//
//   Caso A: nenhum vizinho adjacente
//           → criar um Extent novo (X, 1).
//
//   Caso B: existe um predecessor PRED com PRED.start + PRED.length == X
//           (extent que termina exactamente em X-1)
//           → estender PRED em 1 (length++); start não muda, sem re-insert.
//
//   Caso C: existe um sucessor SUC com SUC.start == X+1
//           (extent que começa exactamente em X+1)
//           → o slot novo vira o início do SUC: start--, length++.
//             Como a chave do GTree é o start, temos de remover e re-inserir.
//
//   Caso D: ambos existem
//           → PRED absorve o slot novo + o SUC inteiro:
//                PRED.length += 1 + SUC.length
//             SUC é removido do GTree.
//
// É este caso D que faz com que apagar um ficheiro de 100 blocos consecutivos
// produza 1 extent de tamanho 100 em vez de 100 extents soltos: cada release
// encontra o predecessor (que cresceu 1 no release anterior) e absorve.

// Estado do walk usado para encontrar o predecessor mais próximo de X.
// O predecessor é o extent com o maior `start` que ainda seja < X.
typedef struct {
  uint64_t x;
  Extent *candidate;     // melhor predecessor encontrado até agora
} pred_search_t;

static gboolean find_pred_cb(gpointer key, gpointer value, gpointer data) {
  Extent *e = (Extent *)value;
  pred_search_t *s = (pred_search_t *)data;
  uint64_t start = *(uint64_t *)key;

  if (start >= s->x) {
    // Já ultrapassámos X — o último candidato (se houver) é o predecessor.
    return TRUE;  // TRUE = parar a iteração
  }
  s->candidate = e;
  return FALSE;
}

// Procura o extent que termine adjacente a X (i.e., extent.start + extent.length == X).
// Retorna NULL se não houver.
static Extent *find_adjacent_predecessor(GTree *by_start, uint64_t x) {
  pred_search_t s = { .x = x, .candidate = NULL };
  g_tree_foreach(by_start, find_pred_cb, &s);
  if (s.candidate && s.candidate->start + s.candidate->length == x) {
    return s.candidate;
  }
  return NULL;
}

void freelist_release(FreeList *fl, uint64_t master_blk) {
  // Procurar o sucessor adjacente: extent que comece em X+1.
  uint64_t suc_key = master_blk + 1;
  Extent *suc = g_tree_lookup(fl->by_start, &suc_key);

  // Procurar o predecessor adjacente: extent que termine em X-1.
  Extent *pred = find_adjacent_predecessor(fl->by_start, master_blk);

  if (pred && suc) {
    // Caso D: pred absorve o slot novo + o sucessor inteiro.
    // pred.start não muda, pred.length cresce; sucessor é removido.
    pred->length += 1 + suc->length;
    g_tree_remove(fl->by_start, &suc->start);  // liberta key e Extent do suc
    return;
  }

  if (pred) {
    // Caso B: pred estende-se em 1. start não muda, sem re-insert.
    pred->length++;
    return;
  }

  if (suc) {
    // Caso C: o slot novo vira o início do sucessor. Como a chave do GTree é
    // o start, temos de criar um Extent substituto e re-inserir (não há
    // forma leve de mutar uma chave em sítio em glib).
    Extent *replacement = g_new(Extent, 1);
    replacement->start = master_blk;
    replacement->length = suc->length + 1;
    g_tree_remove(fl->by_start, &suc->start);  // liberta o suc antigo
    tree_insert(fl->by_start, replacement);
    return;
  }

  // Caso A: sem vizinhos. Cria-se um Extent novo de tamanho 1.
  Extent *e = g_new(Extent, 1);
  e->start = master_blk;
  e->length = 1;
  tree_insert(fl->by_start, e);
}

void freelist_release_run(FreeList *fl, uint64_t start, uint64_t length) {
  for (uint64_t i = 0; i < length; i++) {
    freelist_release(fl, start + i);
  }
}

// -----------------------------------------------------------------------------
// Persistência (stubs — implementação real no Commit 4)
// -----------------------------------------------------------------------------

void freelist_save(const char *path, FreeList *fl) {
  (void)path;
  (void)fl;
  // TODO: implementação real no Commit 4 (formato (start, length) com
  // auto-coalesce do formato antigo durante a transição).
}

void freelist_load(const char *path, FreeList *fl) {
  (void)path;
  // TODO: implementação real no Commit 4.
  // Por agora, partimos sempre de uma free list vazia — o utilizador
  // tem de correr clean_fuse_data.sh entre arranques durante os Commits 2-3.
  freelist_init(fl);
}
