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

### Constraint do enunciado

O trabalho prático garante que **todas as operações de leitura e escrita
são alinhadas a `BLOCK_SIZE`** (offset e size múltiplos de 4 KiB). A
biblioteca opera com base nesta assunção: writes não-alinhados são
rejeitados com `-EOPNOTSUPP` (em vez de provocarem livelock no kernel,
que era o comportamento pré-existente quando `write_dedup` retornava 0
sem ter feito nada).

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

## 4. Mudança Implementada

Três mudanças coordenadas. Detalhes históricos do design exploratório
(incluindo a tentativa de extent map que foi descartada) ficam em
[DESIGN_BATCHING_STORAGE_FIRST.md](../DESIGN_BATCHING_STORAGE_FIRST.md).

### 4.1 `write_dedup` em três passes

```
Passe 1 (decisão, sem I/O):
   - hash + lookup para todos os blocos do request
   - HITs incrementam refcount; MasterInfo partilhado (já no índice
     oficial, ou pending neste mesmo batch)
   - MISSes criam MasterInfo PENDENTE (NÃO inserido em hash_to_master)
     e registam-no em pending_hashes para tratar intra-batch dedup

Passe 1.5 (alocação batch):
   - allocate_batch_storage_first decide os master_blk de TODOS os
     MISSes em conjunto (drena free list, depois append)

Passe 2 (flush):
   - agrupa MISSes em runs contíguos no plan onde master_blk é
     sequencial; cada run dispara um único pwrite (não pwritev) que
     parte de plan[run_start].payload e cobre run_len * 4096 bytes
   - HITs no meio do plan são saltados, partindo o run
   - falha → rollback_allocations devolve todos os master_blk à free
     list, liberta MasterInfos pendentes e decrementa refcount dos
     HITs; não restaura nextBlockIndex, para evitar dupla alocação
     quando índices de append já foram devolvidos à free list

Passe 3 (consolidação, só após flush OK):
   - inserir MasterInfos pendentes em hash_to_master
   - inserir todos os pares (path, blk) em file_to_master
   - actualizar file_to_sizes
```

A separação Passe 1 (decisão) / Passe 2 (flush) / Passe 3 (consolidação)
garante que se o pwrite falhar, o índice oficial nunca fica com entradas
a apontar para blocos que nunca chegaram ao disco.

**Por que `pwrite` em vez de `pwritev`**: dentro de um run os payloads
são fisicamente contíguos no buffer de input (a ordem do plan reflecte
a ordem dos blocos lógicos), portanto um único `pwrite` cobre o run
inteiro sem precisar de iovec. Quando há HITs intercalados que partem
o run, paga-se a syscall extra — mas o profiling mostrou que esse
custo é <1% do tempo total. A constraint da equipa (manter código
observacional BPF calibrado para `pwrite`) selou a decisão.

### 4.2 Free list como `GSList<uint64_t*>` LIFO

A free list é uma lista ligada simples. O release faz `g_slist_prepend`
do slot (O(1)); o consumo faz pop da head (O(1)).

Esta forma é **deliberadamente simples**. Uma versão prévia usou um
mapa de extents (`GTree<start → Extent>`) com coalescing automático
nos 4 casos de vizinhança e best-fit por varrimento. Foi descartada
após benchmarks revelarem:

- **Caminho quente do nosso workload é dominado por appends contíguos**
  (free list quase sempre vazia ou pequena), portanto o ganho do
  best-fit/coalescing era marginal a zero.
- **Custo constante elevado**: alloc/free de `Extent*` por release,
  varrimento da árvore por allocate, três operações sincronizadas no
  caso D do coalescing. Estes custos pagam-se em **todas** as escritas
  e remoções, mesmo nas que não beneficiam.

A reintrodução do extent map fica como trabalho futuro **se** algum dia
um workload mostrar fragmentação severa persistente da free list.

### 4.3 Allocator storage-first (LIFO O(1))

```
allocate_batch_storage_first(miss_count):
   taken = 0
   enquanto taken < miss_count e free_block_list != NULL:
      slot = pop head (O(1))
      out[taken++] = slot
   para i em taken..miss_count-1:
      out[i] = (*nextBlockIndex)++
```

Sem `THRESHOLD`, sem fallback automático para append. **Master só cresce
quando a free list está completamente esgotada** — propriedade
storage-first preservada.

Perdeu-se "best-fit" e "coalescing" face à versão com extent map. Para
o nosso flush actual isso é irrelevante: como cada slot reusado vira um
run de tamanho 1 (master_blk dispersos), não havia batching possível
para reusos mesmo com coalescing.

---

## 5. Justificação da Escolha

### 5.1 Filosofia "storage-first"

> Cada slot livre é sagrado. Reutilizamos sempre antes de fazer append. Os
> syscalls que daí advêm são o preço a pagar.

A biblioteca **é uma biblioteca de deduplicação**. O seu propósito declarado
é poupar espaço em disco. Aceitar uma política que cresce o master file
quando há slots livres seria filosoficamente contraditório.

### 5.2 Trade-off explícito

Sob fragmentação da free list, a alocação storage-first produz
`master_blk` dispersos, e o flush acaba por emitir mais que um syscall
(um `pwrite` por slot reusado, não há batching de runs descontíguos).
**Aceitamos este custo** em troca de não desperdiçar espaço.

Duas alternativas foram prototipadas e descartadas após medições empíricas:

- Política `syscall-first` (fallback para append puro quando os extents
  são pequenos) — convergiu com `storage-first` no caminho quente e o
  syscall count ficou indistinguível.
- Free list como mapa de extents com coalescing — adicionava overhead
  constante (mallocs/frees, varrimento) por release sem ganho mensurável,
  porque o caminho quente é dominado por appends contíguos onde a
  estrutura simples chega.

Ambas podem voltar a ser exploradas se um workload futuro mostrar
divergência mensurável.

### 5.3 Por que NÃO `io_uring`

`io_uring` foi avaliado e excluído deliberadamente. Resumo:

- O ganho real de `io_uring` vem de **paralelismo** (write-back assíncrono,
  pipelining hash/I/O, múltiplos writers concorrentes).
- O nosso modelo é **estritamente síncrono**: `xmp_write` regressa só com
  os dados em disco; não há pipeline entre hashing e I/O.
- Nestes cenários, `io_uring_submit_and_wait` ≈ `pwritev` em performance,
  com complexidade muito superior (dependência `liburing`, gestão de SQEs/CQEs,
  rollback per-CQE).
- Para extrair valor real seria preciso adoptar **write-back assíncrono**,
  o que abdica de durabilidade síncrona — redesign maior, fora do âmbito.

---

## 5.4 Modelo de concorrência

O FUSE corre multi-threaded por defeito (sem `-s`). Múltiplas threads do
kernel podem chamar `xmp_read`/`xmp_write`/`xmp_unlink`/`xmp_truncate`/
`xmp_getattr` em paralelo. A versão original serializava tudo num único
mutex global; este refactor introduz paralelismo real.

### Locks e atomics

| Estrutura | Sincronização |
|---|---|
| `hash_to_master`, `file_to_master`, `file_to_sizes` | `pthread_rwlock_t metadata_rwlock` (rwlock — reads concorrentes) |
| `MasterInfo` (incl. cleanup) | `metadata_rwlock` em modo write |
| `MasterInfo.refcount` | `_Atomic uint32_t` (increment RELAXED, decrement+test ACQ_REL) |
| `free_block_list` | `pthread_mutex_t freelist_mutex` |
| `Context.nextBlockIndex` | `_Atomic uint64_t` (`__atomic_fetch_add` RELAXED) |

### Hierarquia de locks (para evitar deadlocks)

> Se for preciso pegar os dois, **`metadata_rwlock` ANTES de `freelist_mutex`**.
> Nunca o inverso.

### `write_dedup` por fases

```
Passe 1 (hash + lookup) ────► rdlock(metadata_rwlock)
                              hash, lookup_by_hash, refcount++ atómico em HITs
                              unlock

Passe 1.5 (alocação)    ────► dentro de allocate_batch_storage_first:
                              lock(freelist_mutex); drena GSList; unlock
                              __atomic_fetch_add(nextBlockIndex, K, RELAXED)

Passe 2 (flush)         ────► SEM LOCK
                              pwrite por run contíguo (offsets disjuntos
                              por construção; pwrite é thread-safe a nível
                              de fd)

Passe 3 (consolidar)    ────► wrlock(metadata_rwlock)
                              double-check: relookup antes de inserir,
                              para fechar race de cross-thread dedup
                              insert_hash, insert_file_block, update sizes
                              unlock

                              se houve slots descartados pelo double-check:
                              lock(freelist_mutex); concat; unlock
```

**Múltiplos `write_dedup` correm os Passes 1 e 2 em paralelo entre si**.
Apenas o Passe 3 é serializado pelo wrlock.

### `read_dedup` por fases

```
Phase 1 (lookups)       ────► rdlock(metadata_rwlock)
                              copia masterBlockIndex para pairs[] local
                              (NÃO guarda MasterInfo*)
                              unlock

Phases 2-5 (sort+pread) ────► SEM LOCK
                              pread por run físico contíguo
```

Múltiplos `read_dedup` em paralelo, e em paralelo com Passe 1 do
`write_dedup`.

### Double-check insert (cross-thread dedup)

No Passe 3 do `write_dedup`, antes de inserir um MasterInfo pendente em
`hash_to_master`, fazemos um segundo lookup. Se outro thread já inseriu
o mesmo conteúdo entretanto, descartamos o nosso pendente, devolvemos
o slot à free list e usamos o existente (incrementando refcount).

Custo: 1 pwrite ocasional desperdiçado (slot recicla-se imediatamente
via free list). Garantia: **zero duplicados em hash_to_master**.

### Trade-offs aceites

- **Read concorrente com overwrite no mesmo `(file, offset)`** pode
  retornar torn data se o slot antigo for reciclado durante o pread.
  Workload FUSE realista raramente exibe este pattern.
- **Pwrite ocasional desperdiçado** sob colisão cross-thread (resolvido
  pelo double-check, sem corrupção).

---

## 6. Roadmap

### Já feito neste PR
- Refactor de `write_dedup` em três passes (decisão → flush → consolidação).
- Flush via `pwrite` agregando runs contíguos no buffer de input.
- Free list `GSList<uint64_t*>` LIFO simples.
- Allocator storage-first (drena free list antes de append).
- `pending_hashes` para intra-batch dedup.
- `rollback_allocations` em caso de falha de I/O.
- Teste de round-trip (`tests/roundtrip.sh`).

### Trabalho futuro (intencionalmente adiado)

- **Free list como mapa de extents + coalescing**. Foi prototipada e
  descartada porque o overhead constante não se justificava no nosso
  workload. Reintroduzir só se medições mostrarem fragmentação severa
  persistente ou se o batching de reusos passar a ser viável (ex.: com
  `pwritev` ou `io_uring`).

- **`pwritev` em vez de `pwrite`** para juntar runs descontíguos num só
  syscall. Custo actual estimado é <1% do wall-clock. Reabrir se o
  colega do BPF aceitar adaptar o código observacional para incluir
  `pwritev` na contagem.

- **Encolher `nextBlockIndex` no release** quando o slot libertado toca
  a fronteira do master. Micro-optimização TODO.

- **Reintroduzir política syscall-first configurável** se um workload
  futuro mostrar divergência mensurável. O ponto de variação está
  concentrado em `allocate_batch_storage_first` — fácil de substituir
  por switch.

---

## 7. Verificação

### Build e benchmark

```bash
make clean && make
sudo ./clean_fuse_data.sh
sudo ./fuse.sh                    # terminal A
./run_command.sh                   # terminal B
sudo umount /mnt/fs
```

### Teste de correctude single-thread (round-trip)

```bash
sudo ./fuse.sh &                   # background
sleep 1
./tests/roundtrip.sh 8             # 8 MiB
./tests/roundtrip.sh 64            # 64 MiB
sudo umount /mnt/fs
```

### Teste de correctude concorrente

Lança N cópias paralelas, cada uma a copiar conteúdo aleatório próprio
para um ficheiro distinto, e valida md5 round-trip. Se os locks/atomics
estiverem mal, as md5 vão divergir e o teste falha.

```bash
sudo ./fuse.sh &
sleep 1
./tests/concurrent_roundtrip.sh 8 4    # 8 threads × 4 MiB cada
./tests/concurrent_roundtrip.sh 16 8   # 16 threads × 8 MiB cada
sudo umount /mnt/fs
```

### Detectar races com ThreadSanitizer (recomendado)

Build de debug com TSan apanha races que escapam ao raciocínio:

```bash
make clean
CFLAGS="-fsanitize=thread -g" make
sudo ./fuse.sh &     # com TSan activo, output extra em stderr se houver race
./tests/concurrent_roundtrip.sh 16 8
sudo umount /mnt/fs
```

### Métricas a observar

| Métrica | Como medir |
|---|---|
| Tamanho `/masterFILE` | `stat -c %s /masterFILE` |
| Syscalls `pwrite` | `bpf_programs/syscounter` durante benchmark |
| Tamanho da freelist | inspecção de `/table_path_free_block_list` |
| Wall-clock concorrente | `time ./tests/concurrent_roundtrip.sh 16 8` |

---

## 8. Glossário

- **Bloco** — unidade de 4 KiB, alinhada.
- **Bloco lógico** — bloco indexado por `(path, offset)`, do ponto de vista
  do utilizador.
- **Bloco físico** — bloco no master file, indexado por `masterBlockIndex`.
- **Hash hit / miss** — quando o hash de um bloco lógico já existe (HIT) ou
  não (MISS) no índice `hash_to_master`.
- **Slot** — posição livre/ocupada no master file (offset = `slot * 4096`).
- **Free list** — `GSList<uint64_t*>` LIFO que regista slots livres para
  reuso.
- **Run** — sequência de blocos com `master_blk` consecutivos no plan,
  agrupada num único `pwrite` durante o flush.
- **Batch** — conjunto de blocos lógicos de um único request FUSE.
- **Storage-first** — política de alocação que prioriza não crescer o
  master, drenando sempre a free list antes de fazer append. É a
  política activa na biblioteca.
