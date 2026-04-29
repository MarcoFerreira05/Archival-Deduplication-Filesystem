# Arquitectura — Dedup-IO-Lib

> Documento vivo da arquitectura da biblioteca. Descreve o estado actual,
> o problema que motivou o refactor de batching de escritas, a mudança
> proposta e a sua justificação.
>
> Documentos de design exploratório (mais longos, com pseudocódigo e debate
> de alternativas): [DESIGN_BATCHING_STORAGE_FIRST.md](../DESIGN_BATCHING_STORAGE_FIRST.md)
> e [DESIGN_BATCHING_ESCRITAS.md](../DESIGN_BATCHING_ESCRITAS.md).

---

## 1. Visão Geral

A **Dedup-IO-Lib** é uma biblioteca FUSE (em modo passthrough) que adiciona
**deduplicação a nível de bloco** sobre um sistema de ficheiros existente.

Componentes principais:

- **FUSE passthrough** ([src/passthrough.c](../src/passthrough.c)) — intercepta
  as syscalls do kernel (`read`, `write`, `unlink`, `truncate`, ...) e delega
  para a lib de dedup.
- **Camada de deduplicação** ([src/dedup.c](../src/dedup.c)) — para cada bloco
  lógico calcula o hash (SHA-512), faz lookup no índice e decide se reutiliza
  um bloco existente (HIT) ou escreve um novo (MISS) no master file.
- **Master file** (`/masterFILE`) — ficheiro físico onde **cada bloco único**
  do sistema é guardado uma vez. Os ficheiros lógicos vistos pelo utilizador
  são apenas referências para offsets dentro deste master.
- **Índice** ([src/metaindex.c](../src/metaindex.c)) — duas hash tables que
  permitem `O(1)` em ambas as direcções:
  - `hash_to_master`: hash → MasterInfo (quem é o bloco com este conteúdo?).
  - `file_to_master`: (path, blockIndex) → MasterInfo (que bloco lógico
    aponta para qual bloco físico?).
- **Free list** — lista de slots no master que ficaram livres após
  unlinks/truncates e podem ser reutilizados.
- **Persistência** ([src/persistence.c](../src/persistence.c)) — guarda e
  carrega todas as estruturas em ficheiros separados (`/table_path_*`).

Bloco físico: **4 KiB**. Hash: **SHA-512** (64 B).

---

## 2. Estado Actual (pré-refactor)

### 2.1 Estruturas

```c
typedef struct masterInfo {
  unsigned char hash[HASH_SIZE];    // SHA-512 do conteúdo
  uint64_t      masterBlockIndex;   // posição no master file
  uint32_t      refcount;           // quantas referências (file, block)
} MasterInfo;

typedef struct index {
  GHashTable      *hash_to_master;  // hash → MasterInfo*
  GHashTable      *file_to_master;  // BlockIndice* → MasterInfo*
  GSList          *free_block_list; // lista de uint64_t* (slots livres soltos)
  GHashTable      *file_to_sizes;   // path → size_t* (tamanho lógico)
  pthread_mutex_t  mutex;
} Index;
```

### 2.2 Fluxo de escrita actual

```
xmp_write(path, buf, size, offset)
   │
   ├─ lock(index->mutex)
   │
   └─ write_dedup(...)
        │
        └─ FOR i = 0 .. num_blocks-1:                  ← loop bloco-a-bloco
             ├─ hash(buf + i*4096)                     ← SHA-512
             ├─ MasterInfo *info = lookup_by_hash(...)
             │
             ├─ se HIT:
             │    └─ info->refcount++
             │
             └─ se MISS:
                  ├─ slot = pop GSList   OR   slot = nextBlockIndex++
                  ├─ pwrite(masterFd, block, 4096, slot * 4096)   ← 1 syscall por MISS
                  ├─ criar MasterInfo, inserir em hash_to_master
                  └─ insert_file_block(path, i, info)
```

Para um request FUSE de 64 KiB com todos MISSes: **16 syscalls `pwrite`**.

### 2.3 Fluxo de remoção actual

```
xmp_unlink(path)                                 ← ou xmp_truncate
   │
   └─ FOR i = 0 .. num_blocks-1:
        └─ remove_block_dedup(path, i)
             ├─ info->refcount--
             └─ se refcount == 0:
                  ├─ slot = malloc(uint64_t)
                  ├─ free_block_list = g_slist_prepend(slot)   ← LIFO, sem coalesce
                  └─ remove de hash_to_master, free(info)
```

Resultado: 100 unlinks → **100 nós soltos** numa GSList, sem ordem nem agrupamento.

---

## 3. Problema Identificado

Três problemas concretos, mensuráveis no benchmark actual:

1. **N syscalls por request com N MISSes**. Cada bloco MISS dispara um
   `pwrite` separado, mesmo quando os slots alocados são contíguos no master.
   Para um request de 64 KiB: 16 syscalls onde 1 chegaria.

2. **Free list fragmenta indefinidamente**. Cada bloco libertado vira um
   nó isolado na GSList. Não há lógica que junte slots adjacentes mesmo quando
   100 blocos consecutivos são libertados ao apagar um ficheiro inteiro.

3. **Master file pode crescer mesmo havendo slots livres**. A política
   actual consome a free list em LIFO, slot a slot. Se um batch precisa de
   `K` slots e a free list tem `K` slots livres mas dispersos, eles **são**
   consumidos — mas o `pwrite` é feito a offsets dispersos, e nada no
   sistema tenta arranjar runs contíguos para alocação eficiente.

---

## 4. Mudança Proposta

Quatro mudanças coordenadas, descritas em detalhe em
[DESIGN_BATCHING_STORAGE_FIRST.md](../DESIGN_BATCHING_STORAGE_FIRST.md).

### 4.1 `write_dedup` em dois passes

```
Passe 1 (decisão, sem I/O):
   - hash + lookup para todos os blocos
   - HITs incrementam refcount no plan local
   - MISSes recebem MasterInfo "pendente" (não inserida em hash_to_master)
   - alocação dos master_blk de TODOS os MISSes feita em conjunto

Passe 2 (flush):
   - agrupa runs contíguos no plan
   - emite UM pwritev por run (1 syscall ≥ 1 bloco)
   - falha → rollback completo

Passe 3 (consolidação, só após flush OK):
   - inserir em hash_to_master e file_to_master
   - actualizar file_to_sizes
```

### 4.2 Free list como mapa de extents

`GSList<uint64_t>` → `GTree<start → Extent>`, onde
`Extent = { uint64_t start, uint64_t length }`.

Permite lookup ordenado, walk eficiente para alocação batch e — sobretudo
— **coalescing on release**.

### 4.3 Coalescing on release

Cada bloco libertado procura vizinhos adjacentes na árvore de extents e
funde-se com eles:

| Caso | Vizinhos | Acção |
|---|---|---|
| A | nenhum | criar `Extent(X, 1)` |
| B | só predecessor (extent termina em `X-1`) | `pred->length++` |
| C | só sucessor (extent começa em `X+1`) | `suc->start--; suc->length++` |
| D | ambos | predecessor absorve `1 + suc.length`, sucessor é removido |

Resultado: 100 unlinks consecutivos → **1 extent de tamanho 100**.

### 4.4 Allocator storage-first

```
allocate_batch_storage_first(K):
   total = freelist_total_free()
   se total >= K:
      freelist_take(K)               ← reuso máximo, master não cresce
   senão:
      freelist_take(total)
      append remainder via nextBlockIndex
```

Sem `THRESHOLD`, sem fallback automático para append. Master só cresce
quando a free list está completamente esgotada.

---

## 5. Justificação da Escolha

### 5.1 Filosofia "storage-first"

> Cada slot livre é sagrado. Reutilizamos sempre antes de fazer append. Os
> syscalls que daí advêm são o preço a pagar.

A biblioteca **é uma biblioteca de deduplicação**. O seu propósito declarado
é poupar espaço em disco. Aceitar uma política que cresce o master file
quando há slots livres seria filosoficamente contraditório.

### 5.2 Trade-off explícito

Sob fragmentação severa (free list com muitos extents pequenos), a alocação
storage-first produz `master_blk` dispersos, e o flush acaba por emitir
mais que um syscall. **Aceitamos este custo** em troca de não desperdiçar
espaço.

A alternativa — política `syscall-first` com fallback para append quando
extents são pequenos demais — **fica disponível como opção configurável**
através da variável de ambiente `DEDUP_ALLOC_POLICY` (ver §7 deste
documento), permitindo comparações empíricas.

### 5.3 Por que NÃO `io_uring`

`io_uring` foi avaliado e excluído deliberadamente. Resumo:

- O ganho real de `io_uring` vem de **paralelismo** (write-back assíncrono,
  pipelining hash/I/O, múltiplos writers concorrentes).
- O nosso modelo é **estritamente síncrono**: `xmp_write` regressa só com
  os dados em disco; o `index->mutex` serializa todos os writes; não há
  pipeline entre hashing e I/O.
- Nestes cenários, `io_uring_submit_and_wait` ≈ `pwritev` em performance,
  com complexidade muito superior (dependência `liburing`, gestão de SQEs/CQEs,
  rollback per-CQE).
- Para extrair valor real seria preciso adoptar **write-back assíncrono**,
  o que abdica de durabilidade síncrona — redesign maior, fora do âmbito.

---

## 6. Roadmap

### Já feito neste PR
- Refactor de `write_dedup` em dois passes com `pwritev`.
- Mapa de extents (`FreeList`) com coalescing on release.
- Allocator storage-first como default.
- Persistência migrável (auto-coalesce do formato antigo).
- Configurabilidade `storage-first` ↔ `syscall-first` via env var.
- Teste de round-trip (`tests/roundtrip.sh`).

### Trabalho futuro (intencionalmente adiado)

- **Fase 3 — Índice secundário `by_length`** para best-fit em O(log F) em
  vez de varrimento O(F). Adiado porque o coalescing mantém o número de
  extents pequeno (dezenas em estado estável); a complexidade de manter
  dois índices sincronizados não compensa enquanto não houver profiling
  que mostre o varrimento como hotspot. Documentação inline em
  [includes/freelist.h](../includes/freelist.h) e
  [src/freelist.c](../src/freelist.c) com prós/contras detalhados.

- **Encolher `nextBlockIndex` no release** quando o slot libertado toca
  a fronteira do master. Micro-optimização; deixada como TODO.

- **Mount option FUSE `-o alloc_policy=...`** em vez de variável de
  ambiente. Mais idiomático em FUSE, mas requer parsing extra em `main()`.

- **`max_fragments` em storage-first** como salvaguarda contra fragmentação
  patológica (cap no nº de runs por batch, depois fallback a append). Reservado
  no `AllocConfig` mas não implementado — activar apenas se medições
  mostrarem cenários patológicos persistentes.

---

## 7. Configuração da política de alocação

A política é seleccionável via variáveis de ambiente, lidas em `xmp_init`.

| Variável | Valores | Default | Aplicabilidade |
|---|---|---|---|
| `DEDUP_ALLOC_POLICY` | `storage_first`, `syscall_first` | `storage_first` | sempre |
| `DEDUP_THRESHOLD` | inteiro `≥ 0` | `4` | só `syscall_first` |
| `DEDUP_MAX_FRAGMENTS` | inteiro `≥ 0` | `0` (ilimitado) | reservado, sem efeito |

### Exemplos

```bash
# Storage-first (default): master nunca cresce com slots livres.
sudo ./fuse.sh

# Syscall-first com threshold 8: ignora extents < 8 blocos, faz append.
DEDUP_ALLOC_POLICY=syscall_first DEDUP_THRESHOLD=8 sudo -E ./fuse.sh
```

A política escolhida é registada em log no arranque do FS:
```
[alloc-policy] policy=storage_first threshold=4 max_fragments=0
```

### Comparação esperada das métricas

| Métrica | `storage_first` | `syscall_first` |
|---|---|---|
| Tamanho final do master | menor | maior sob fragmentação |
| Syscalls `pwrite/pwritev` por request | 1 quando contíguo, mais sob fragmentação | 1 ou 2 mesmo sob fragmentação |
| Comportamento em workloads write-puro | igual (free list vazia) | igual |
| Comportamento em workloads write+unlink | reuso máximo | reuso parcial |

---

## 8. Verificação

### Build e benchmark

```bash
make clean && make
sudo ./clean_fuse_data.sh
sudo ./fuse.sh                    # terminal A
./run_command.sh                   # terminal B
sudo umount /mnt/fs
```

### Teste de correctude (round-trip)

```bash
sudo ./fuse.sh &                   # background
sleep 1
./tests/roundtrip.sh 8             # 8 MiB
./tests/roundtrip.sh 64            # 64 MiB
sudo umount /mnt/fs
```

### Métricas a observar

| Métrica | Como medir |
|---|---|
| Tamanho `/masterFILE` | `stat -c %s /masterFILE` |
| Syscalls `pwrite`/`pwritev` | `bpf_programs/syscounter` durante benchmark |
| Tamanho da freelist | inspecção de `/table_path_free_block_list` |

---

## 9. Glossário

- **Bloco** — unidade de 4 KiB, alinhada.
- **Bloco lógico** — bloco indexado por `(path, offset)`, do ponto de vista
  do utilizador.
- **Bloco físico** — bloco no master file, indexado por `masterBlockIndex`.
- **Hash hit / miss** — quando o hash de um bloco lógico já existe (HIT) ou
  não (MISS) no índice `hash_to_master`.
- **Slot** — posição livre/ocupada no master file (offset = `slot * 4096`).
- **Free list** — estrutura que regista slots livres para reuso.
- **Extent** — par `(start, length)` representando `length` slots
  contíguos livres a partir de `start`.
- **Run** — sequência de blocos com `master_blk` consecutivos no plan,
  agrupada num único `pwritev` durante o flush.
- **Batch** — conjunto de blocos lógicos de um único request FUSE.
- **Coalesce** — fundir extents adjacentes num só.
- **Storage-first** — política que prioriza não crescer o master.
- **Syscall-first** — política que prioriza minimizar syscalls.
