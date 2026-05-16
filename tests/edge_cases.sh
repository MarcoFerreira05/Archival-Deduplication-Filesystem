#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# tests/edge_cases.sh — Suite extensa de testes de correctude.
#
# Cobre cenários que tests/concurrent_roundtrip.sh não toca:
#   - EOF e tamanhos não-alinhados a BLOCK_SIZE
#   - Ficheiros vazios e sub-bloco
#   - Dedup single-thread (mesmo conteúdo, master não cresce duas vezes)
#   - Dedup cross-thread (força double-check insert)
#   - Overwrite total e parcial
#   - Truncate shrink (xmp_truncate + remove_blocks_dedup_batch)
#   - Read concorrente com write no mesmo ficheiro
#   - Unlink durante write em loop
#   - Reuso de free list (master não cresce desnecessariamente)
#   - Muitos ficheiros pequenos em paralelo
#
# Uso (FUSE já montado em /mnt/fs):
#   ./tests/edge_cases.sh
# -----------------------------------------------------------------------------
set -uo pipefail   # NÃO -e: queremos somar falhas e reportar no fim

if [[ -t 1 ]]; then
  GREEN=$'\033[0;32m'; RED=$'\033[0;31m'; YELLOW=$'\033[0;33m'
  BLUE=$'\033[0;34m'; BOLD=$'\033[1m'; RESET=$'\033[0m'
else
  GREEN='' RED='' YELLOW='' BLUE='' BOLD='' RESET=''
fi

MOUNT="/mnt/fs"
[[ -d "$MOUNT" ]] || { echo "${RED}ERRO: $MOUNT não existe${RESET}" >&2; exit 2; }
mountpoint -q "$MOUNT" || { echo "${RED}ERRO: $MOUNT não está montado${RESET}" >&2; exit 2; }

TESTS_TOTAL=0
TESTS_PASSED=0
TESTS_FAILED=0
FAILED_NAMES=()

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

# Cleanup de ficheiros num directório do FS após cada teste.
cleanup_fs() {
  rm -f "${MOUNT}"/* 2>/dev/null || true
}

# Lê ficheiro do FS via cat (evita mmap, que falha sob direct_io=1).
md5_via_cat() {
  cat "$1" | md5sum | awk '{print $1}'
}

md5_of() {
  md5sum "$1" | awk '{print $1}'
}

# Tamanho do master file em bytes (0 se não existir).
master_size() {
  stat -c '%s' /masterFILE 2>/dev/null || echo 0
}

# Helper de teste: regista resultado e imprime status.
test_start() {
  TESTS_TOTAL=$((TESTS_TOTAL + 1))
  echo "  ${BLUE}▶${RESET} $1"
}

pass() {
  TESTS_PASSED=$((TESTS_PASSED + 1))
  echo "    ${GREEN}✓ PASS${RESET}"
}

fail() {
  TESTS_FAILED=$((TESTS_FAILED + 1))
  FAILED_NAMES+=( "$1" )
  echo "    ${RED}✗ FAIL: $2${RESET}"
}

# Lança N comandos shell em paralelo. Retorna 0 se TODOS passarem.
# Uso: parallel_run "cmd1" "cmd2" "cmd3" ...
parallel_run() {
  local pids=()
  for cmd in "$@"; do
    bash -c "$cmd" &
    pids+=($!)
  done
  local rc=0
  for pid in "${pids[@]}"; do
    wait "$pid" || rc=1
  done
  return $rc
}

# =============================================================================
# Testes
# =============================================================================

# --- 1. EOF e tamanhos não-alinhados -----------------------------------------

test_empty_file() {
  test_start "ficheiro vazio (touch + md5)"
  local f="${MOUNT}/empty.bin"
  : > "$f"
  local md5
  md5=$(md5_via_cat "$f")
  if [[ "$md5" == "d41d8cd98f00b204e9800998ecf8427e" ]]; then
    pass
  else
    fail "test_empty_file" "md5 vazio esperado, got $md5"
  fi
  cleanup_fs
}

test_sub_block_file_rejected_gracefully() {
  test_start "write sub-bloco (100 bytes) rejeitado sem livelock"
  local f="${MOUNT}/tiny.bin"
  # O enunciado garante que todas as operações são alinhadas a BLOCK_SIZE.
  # Este teste valida ROBUSTEZ: se algo passar um write não-alinhado por
  # acidente, o sistema rejeita com erro em vez de livelockear. Timeout
  # de 5 s para apanhar regressão (livelock) caso o fix do EOPNOTSUPP
  # seja revertido.
  if timeout 5 bash -c "printf 'a%.0s' {1..100} > '$f'" 2>/dev/null; then
    # Write não devia ter sucesso. Limpa antes de falhar.
    rm -f "$f" 2>/dev/null
    fail "test_sub_block_file_rejected_gracefully" \
         "write sub-bloco devia ter falhado mas teve sucesso"
  else
    local rc=$?
    if [[ $rc -eq 124 ]]; then
      fail "test_sub_block_file_rejected_gracefully" \
           "TIMEOUT 5s — livelock detectado, regressão!"
    else
      # Falha normal (EOPNOTSUPP ou similar): comportamento esperado.
      pass
    fi
  fi
  rm -f "$f" 2>/dev/null
  cleanup_fs
}

test_exactly_one_block() {
  test_start "exactamente 1 bloco (4096 bytes)"
  local src=$(mktemp) f="${MOUNT}/onek.bin"
  dd if=/dev/urandom of="$src" bs=4096 count=1 status=none
  cp "$src" "$f"
  local md5_src=$(md5_of "$src") md5_dst=$(md5_via_cat "$f")
  rm -f "$src"
  if [[ "$md5_src" == "$md5_dst" ]]; then pass; else
    fail "test_exactly_one_block" "$md5_src != $md5_dst"
  fi
  cleanup_fs
}

test_unaligned_size_rejected_gracefully() {
  test_start "write não-alinhado (12345 bytes) rejeitado sem livelock"
  local src=$(mktemp) f="${MOUNT}/unaligned.bin"
  dd if=/dev/urandom of="$src" bs=1 count=12345 status=none
  # Robustez: o enunciado garante alinhamento, mas se um cp acidental
  # passar 12345 bytes (último chunk = 57 bytes não-alinhado), o sistema
  # rejeita com erro em vez de livelockear.
  if timeout 5 cp "$src" "$f" 2>/dev/null; then
    rm -f "$src" "$f" 2>/dev/null
    fail "test_unaligned_size_rejected_gracefully" \
         "cp com tamanho não-alinhado devia ter falhado mas teve sucesso"
  else
    local rc=$?
    if [[ $rc -eq 124 ]]; then
      fail "test_unaligned_size_rejected_gracefully" \
           "TIMEOUT 5s — livelock detectado, regressão!"
    else
      pass
    fi
  fi
  rm -f "$src" "$f" 2>/dev/null
  cleanup_fs
}

test_read_past_eof() {
  test_start "read past EOF retorna 0 (não erro)"
  local src=$(mktemp) f="${MOUNT}/eof.bin"
  dd if=/dev/urandom of="$src" bs=4096 count=1 status=none
  cp "$src" "$f"
  # tail -c +N começa na byte N. Se pedir além do tamanho, deve dar vazio.
  local out=$(tail -c +99999 "$f" 2>&1)
  local rc=$?
  rm -f "$src"
  if [[ $rc -eq 0 && -z "$out" ]]; then pass; else
    fail "test_read_past_eof" "rc=$rc out_len=${#out}"
  fi
  cleanup_fs
}

# --- 2. Dedup ---------------------------------------------------------------

test_dedup_single_thread_same_content() {
  test_start "dedup single-thread (2º cp do mesmo conteúdo não cresce master)"
  cleanup_fs
  local src=$(mktemp)
  dd if=/dev/urandom of="$src" bs=1M count=4 status=none

  cp "$src" "${MOUNT}/dup1.bin"
  sync
  local size_after_first=$(master_size)

  # Mesmo conteúdo: todos os blocos do 2º cp viram HIT no hash_to_master.
  # HITs não consomem free list nem incrementam nextBlockIndex — o
  # /masterFILE NÃO deve crescer entre o 1º e o 2º cp.
  cp "$src" "${MOUNT}/dup2.bin"
  sync
  local size_after_second=$(master_size)

  local growth=$((size_after_second - size_after_first))

  # Verifica também md5 round-trip dos dois.
  local md5_src=$(md5_of "$src")
  local md5_d1=$(md5_via_cat "${MOUNT}/dup1.bin")
  local md5_d2=$(md5_via_cat "${MOUNT}/dup2.bin")

  rm -f "$src"
  cleanup_fs

  # NOTA: não comparamos com size_before porque cleanup_fs não limpa o
  # /masterFILE (cresce monotonamente). O 1º cp pode reaproveitar slots
  # da free list de testes anteriores e não crescer o master — isso é
  # o reuso a funcionar, não bug.
  if [[ $growth -eq 0 && "$md5_src" == "$md5_d1" && "$md5_src" == "$md5_d2" ]]; then
    pass
  else
    fail "test_dedup_single_thread_same_content" \
         "growth=$growth (esperado 0) md5_src=$md5_src md5_d1=$md5_d1 md5_d2=$md5_d2"
  fi
}

test_dedup_cross_thread() {
  test_start "dedup cross-thread (8 threads escrevem mesmo conteúdo em paralelo)"
  cleanup_fs
  local shared=$(mktemp)
  dd if=/dev/urandom of="$shared" bs=1M count=4 status=none

  # Lança 8 threads, todas a escrever o MESMO conteúdo em ficheiros distintos.
  # Força race no caminho do double-check insert: dois threads em
  # write_dedup vêem o mesmo hash como MISS no Passe 1, ambos pwrite,
  # depois um deles no Passe 3 (write lock) descobre via re-lookup que
  # o outro já inseriu, descarta o seu MasterInfo pendente e devolve
  # o slot à free list.
  #
  # Esta validação foca-se em CORRECTUDE (md5 round-trip de cada um).
  # Não validamos growth do /masterFILE porque o estado da free list
  # entre testes é não-determinístico (reuso/append podem alternar).
  local pids=()
  for i in {1..8}; do
    cp "$shared" "${MOUNT}/cross_$i.bin" &
    pids+=($!)
  done
  local rc=0
  for pid in "${pids[@]}"; do wait "$pid" || rc=1; done
  sync

  # Verifica md5 round-trip de cada um.
  local md5_shared=$(md5_of "$shared")
  local all_ok=1
  for i in {1..8}; do
    local md5_dst=$(md5_via_cat "${MOUNT}/cross_$i.bin")
    if [[ "$md5_dst" != "$md5_shared" ]]; then all_ok=0; break; fi
  done

  rm -f "$shared"
  cleanup_fs

  if [[ $rc -eq 0 && $all_ok -eq 1 ]]; then
    pass
  else
    fail "test_dedup_cross_thread" "rc=$rc md5_ok=$all_ok"
  fi
}

# --- 3. Overwrite -----------------------------------------------------------

test_overwrite_full() {
  test_start "overwrite total (mesmo tamanho)"
  local f="${MOUNT}/over.bin"
  local src1=$(mktemp) src2=$(mktemp)
  dd if=/dev/urandom of="$src1" bs=1M count=2 status=none
  dd if=/dev/urandom of="$src2" bs=1M count=2 status=none

  cp "$src1" "$f"
  local md5_first=$(md5_via_cat "$f")
  cp "$src2" "$f"     # overwrite
  local md5_second=$(md5_via_cat "$f")

  local md5_src2=$(md5_of "$src2")
  rm -f "$src1" "$src2"
  cleanup_fs

  if [[ "$md5_second" == "$md5_src2" && "$md5_first" != "$md5_second" ]]; then
    pass
  else
    fail "test_overwrite_full" "md5_second=$md5_second md5_src2=$md5_src2"
  fi
}

test_overwrite_via_dd_seek() {
  test_start "overwrite parcial (dd seek a meio)"
  local f="${MOUNT}/seek.bin"
  # Usa mktemp para evitar colisão com runs paralelas ou ficheiros
  # pré-existentes em /tmp. Cleanup automático via trap RETURN.
  local src_a=$(mktemp) src_b=$(mktemp) expected=$(mktemp)
  trap 'rm -f "$src_a" "$src_b" "$expected"' RETURN

  # Cria ficheiro de 8 KiB (2 blocos) com pattern A.
  dd if=/dev/urandom of="$src_a" bs=4096 count=2 status=none
  cp "$src_a" "$f"

  # Sobrescreve apenas o 2º bloco com pattern B.
  dd if=/dev/urandom of="$src_b" bs=4096 count=1 status=none
  dd if="$src_b" of="$f" bs=4096 seek=1 conv=notrunc status=none

  # Resultado esperado: bloco 0 = primeiro 4K de A, bloco 1 = B.
  head -c 4096 "$src_a" > "$expected"
  cat "$src_b" >> "$expected"

  local md5_expected=$(md5_of "$expected")
  local md5_actual=$(md5_via_cat "$f")

  cleanup_fs

  if [[ "$md5_expected" == "$md5_actual" ]]; then pass; else
    fail "test_overwrite_via_dd_seek" "expected=$md5_expected actual=$md5_actual"
  fi
}

# --- 4. Truncate shrink -----------------------------------------------------

test_truncate_shrink() {
  test_start "truncate shrink (8 MiB → 2 MiB)"
  local src=$(mktemp) f="${MOUNT}/trunc.bin"
  dd if=/dev/urandom of="$src" bs=1M count=8 status=none
  cp "$src" "$f"

  truncate -s 2097152 "$f"     # 2 MiB

  local size_after=$(stat -c '%s' "$f")
  local md5_first2mb=$(head -c 2097152 "$src" | md5sum | awk '{print $1}')
  local md5_actual=$(md5_via_cat "$f")

  rm -f "$src"
  cleanup_fs

  if [[ "$size_after" == "2097152" && "$md5_first2mb" == "$md5_actual" ]]; then
    pass
  else
    fail "test_truncate_shrink" "size=$size_after md5_first2mb=$md5_first2mb md5_actual=$md5_actual"
  fi
}

test_truncate_to_zero() {
  test_start "truncate to 0 (apaga blocos, master reusa)"
  local src=$(mktemp) f="${MOUNT}/trunc0.bin"
  dd if=/dev/urandom of="$src" bs=1M count=4 status=none
  cp "$src" "$f"
  truncate -s 0 "$f"
  local size_after=$(stat -c '%s' "$f")
  local content=$(cat "$f")
  rm -f "$src"
  cleanup_fs
  if [[ "$size_after" == "0" && -z "$content" ]]; then pass; else
    fail "test_truncate_to_zero" "size=$size_after content_len=${#content}"
  fi
}

# --- 5. Concorrência avançada -----------------------------------------------

test_concurrent_read_during_write() {
  test_start "read concorrente em ficheiro estável durante writes noutros"
  cleanup_fs
  # Cria um ficheiro estável que será lido em loop.
  local stable_src=$(mktemp)
  dd if=/dev/urandom of="$stable_src" bs=1M count=4 status=none
  cp "$stable_src" "${MOUNT}/stable.bin"
  local md5_stable=$(md5_of "$stable_src")

  # Lança um leitor em loop e vários escritores.
  (
    for _ in {1..30}; do
      local md5_read=$(md5_via_cat "${MOUNT}/stable.bin")
      if [[ "$md5_read" != "$md5_stable" ]]; then
        echo "READER: md5 divergiu! got=$md5_read expected=$md5_stable" >&2
        exit 1
      fi
    done
  ) &
  local reader_pid=$!

  # 4 escritores em paralelo (ficheiros distintos).
  local writer_pids=()
  for i in {1..4}; do
    (
      local s=$(mktemp)
      dd if=/dev/urandom of="$s" bs=1M count=2 status=none
      cp "$s" "${MOUNT}/w_$i.bin"
      rm -f "$s"
    ) &
    writer_pids+=($!)
  done

  local rc=0
  for pid in "${writer_pids[@]}"; do wait "$pid" || rc=1; done
  wait $reader_pid || rc=1

  rm -f "$stable_src"
  cleanup_fs
  if [[ $rc -eq 0 ]]; then pass; else
    fail "test_concurrent_read_during_write" "leitor reportou divergência ou writer falhou"
  fi
}

test_concurrent_unlink_during_writes() {
  test_start "unlink concorrente com writes noutros ficheiros"
  cleanup_fs
  # Pré-cria 4 ficheiros que vão ser apagados.
  for i in {1..4}; do
    dd if=/dev/urandom of="${MOUNT}/del_$i.bin" bs=1M count=2 status=none
  done

  # Em paralelo: 4 unlinks + 4 writes em ficheiros novos.
  local pids=()
  for i in {1..4}; do
    rm -f "${MOUNT}/del_$i.bin" &
    pids+=($!)
  done
  for i in {1..4}; do
    (
      local s=$(mktemp)
      dd if=/dev/urandom of="$s" bs=1M count=2 status=none
      cp "$s" "${MOUNT}/new_$i.bin"
      rm -f "$s"
    ) &
    pids+=($!)
  done

  local rc=0
  for pid in "${pids[@]}"; do wait "$pid" || rc=1; done

  # Verifica estado final: del_*.bin não devem existir, new_*.bin sim.
  local del_remaining=$(ls "${MOUNT}"/del_*.bin 2>/dev/null | wc -l)
  local new_count=$(ls "${MOUNT}"/new_*.bin 2>/dev/null | wc -l)

  cleanup_fs

  if [[ $rc -eq 0 && $del_remaining -eq 0 && $new_count -eq 4 ]]; then pass; else
    fail "test_concurrent_unlink_during_writes" \
         "rc=$rc del_remaining=$del_remaining new_count=$new_count"
  fi
}

# --- 6. Reuso da free list --------------------------------------------------

test_freelist_reuse() {
  test_start "free list reuse (write→unlink→write não cresce master)"
  cleanup_fs
  local src=$(mktemp)
  dd if=/dev/urandom of="$src" bs=1M count=4 status=none

  cp "$src" "${MOUNT}/r1.bin"
  sync
  local size_after_first=$(master_size)

  rm -f "${MOUNT}/r1.bin"
  sync

  # Segundo write com conteúdo diferente — deve consumir slots libertados.
  local src2=$(mktemp)
  dd if=/dev/urandom of="$src2" bs=1M count=4 status=none
  cp "$src2" "${MOUNT}/r2.bin"
  sync
  local size_after_second=$(master_size)

  local growth=$((size_after_second - size_after_first))

  rm -f "$src" "$src2"
  cleanup_fs

  # Master cresceu na primeira escrita. Na segunda devia crescer 0
  # (todos os slots vieram da free list).
  if [[ $growth -eq 0 ]]; then pass; else
    fail "test_freelist_reuse" "master cresceu $growth bytes na 2ª escrita (esperado 0)"
  fi
}

# --- 7. Stress generalizado --------------------------------------------------

test_many_small_files_concurrent() {
  test_start "100 ficheiros de 1 bloco criados em paralelo"
  cleanup_fs
  # Usa dd bs=4096 count=1 (1 bloco exacto = 4 KiB) por causa da
  # constraint "operações alinhadas a BLOCK_SIZE" do enunciado.
  # Cada ficheiro contém o byte ASCII do dígito final do índice
  # repetido 4096 vezes (não é importante o conteúdo, só validar
  # que os 100 ficheiros se escrevem em paralelo sem perder).
  local pids=()
  for i in {1..100}; do
    (
      yes "$((i % 10))" 2>/dev/null | head -c 4096 > "${MOUNT}/many_$i.bin"
    ) &
    pids+=($!)
    if [[ ${#pids[@]} -ge 20 ]]; then
      for pid in "${pids[@]}"; do wait "$pid"; done
      pids=()
    fi
  done
  for pid in "${pids[@]}"; do wait "$pid"; done

  local count=$(ls "${MOUNT}"/many_*.bin 2>/dev/null | wc -l)
  # Valida tamanho de uma amostra (1, 50, 100): cada ficheiro deve ter
  # exactamente 4096 bytes.
  local sample_ok=1
  for i in 1 50 100; do
    local size=$(stat -c '%s' "${MOUNT}/many_$i.bin" 2>/dev/null || echo -1)
    if [[ "$size" != "4096" ]]; then sample_ok=0; break; fi
  done

  cleanup_fs

  if [[ $count -eq 100 && $sample_ok -eq 1 ]]; then pass; else
    fail "test_many_small_files_concurrent" "count=$count sample_ok=$sample_ok"
  fi
}

# =============================================================================
# Main
# =============================================================================

echo
echo "${BOLD}═══════════════════════════════════════════════════════════════${RESET}"
echo "${BOLD}  Edge cases suite — exercitando caminhos não cobertos pelo${RESET}"
echo "${BOLD}  concurrent_roundtrip.sh${RESET}"
echo "${BOLD}═══════════════════════════════════════════════════════════════${RESET}"
echo

echo "${BOLD}1. EOF e tamanhos não-alinhados${RESET}"
test_empty_file
test_sub_block_file_rejected_gracefully
test_exactly_one_block
test_unaligned_size_rejected_gracefully
test_read_past_eof

echo
echo "${BOLD}2. Dedup${RESET}"
test_dedup_single_thread_same_content
test_dedup_cross_thread

echo
echo "${BOLD}3. Overwrite${RESET}"
test_overwrite_full
test_overwrite_via_dd_seek

echo
echo "${BOLD}4. Truncate${RESET}"
test_truncate_shrink
test_truncate_to_zero

echo
echo "${BOLD}5. Concorrência avançada${RESET}"
test_concurrent_read_during_write
test_concurrent_unlink_during_writes

echo
echo "${BOLD}6. Reuso da free list${RESET}"
test_freelist_reuse

echo
echo "${BOLD}7. Stress generalizado${RESET}"
test_many_small_files_concurrent

# Resumo final
echo
echo "${BOLD}═══════════════════════════════════════════════════════════════${RESET}"
echo "Total: $TESTS_TOTAL  ${GREEN}Passed: $TESTS_PASSED${RESET}  ${RED}Failed: $TESTS_FAILED${RESET}"
if [[ $TESTS_FAILED -gt 0 ]]; then
  echo
  echo "${RED}Falharam:${RESET}"
  for n in "${FAILED_NAMES[@]}"; do echo "  - $n"; done
  exit 1
fi
echo "${GREEN}${BOLD}✓ Todos os edge cases passaram${RESET}"
exit 0
