#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# tests/run_all.sh — Orquestrador de testes para concorrência.
#
# Faz: clean → build → mount → 2 × roundtrip single-thread → 3 × roundtrip
# concorrente (carga crescente) → unmount. Falha logo no primeiro erro
# graças a `set -e`. Cleanup robusto via trap mesmo em interrupção.
#
# REQUER sudo (precisa de mount FUSE).
#
# Uso:
#   sudo ./tests/run_all.sh         # corre tudo
#   sudo ./tests/run_all.sh fast    # só roundtrip single-thread + 1 concorrente
# -----------------------------------------------------------------------------
set -euo pipefail

# Cores ANSI (graceful degradation se TERM não suporta).
if [[ -t 1 ]]; then
  GREEN=$'\033[0;32m'
  RED=$'\033[0;31m'
  YELLOW=$'\033[0;33m'
  BLUE=$'\033[0;34m'
  RESET=$'\033[0m'
else
  GREEN='' RED='' YELLOW='' BLUE='' RESET=''
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

MOUNT="/mnt/fs"
MODE="${1:-full}"

step() { echo "${BLUE}==> $1${RESET}"; }
ok()   { echo "${GREEN}    OK: $1${RESET}"; }
fail() { echo "${RED}    FAIL: $1${RESET}" >&2; }

# Cleanup: desmonta se ainda estiver montado. Corre em qualquer saída
# (sucesso, falha, Ctrl+C).
cleanup() {
  local rc=$?
  if mountpoint -q "$MOUNT" 2>/dev/null; then
    step "Cleanup — desmontando $MOUNT"
    sudo umount "$MOUNT" 2>/dev/null || sudo umount -l "$MOUNT" 2>/dev/null || true
  fi
  if [[ $rc -eq 0 ]]; then
    echo
    echo "${GREEN}╔════════════════════════════════╗${RESET}"
    echo "${GREEN}║  ✓  ALL TESTS PASSED          ║${RESET}"
    echo "${GREEN}╚════════════════════════════════╝${RESET}"
  else
    echo
    echo "${RED}╔════════════════════════════════╗${RESET}"
    echo "${RED}║  ✗  TESTS FAILED (rc=$rc)        ║${RESET}"
    echo "${RED}╚════════════════════════════════╝${RESET}"
  fi
}
trap cleanup EXIT INT TERM

# Verificações iniciais
[[ $EUID -eq 0 ]] || { fail "este script precisa de sudo (mount FUSE)"; exit 2; }
[[ -d "$MOUNT" ]] || { fail "$MOUNT não existe"; exit 2; }
[[ -d "/backend" ]] || { fail "/backend não existe (necessário para subdir module)"; exit 2; }

# Se já estiver montado de uma run anterior, desmonta.
if mountpoint -q "$MOUNT"; then
  step "$MOUNT já estava montado — desmontando"
  umount "$MOUNT" || umount -l "$MOUNT"
fi

step "Limpando dados de runs anteriores"
./clean_fuse_data.sh
ok "estado limpo"

step "Build"
make -s clean
make -s
ok "compilado"

step "Montando FUSE em background"
# Sem -f → daemoniza por defeito. Sai imediatamente quando o mount está pronto.
./passthrough "$MOUNT" -omodules="subdir,subdir=/backend" -oallow_other

# Espera pelo mount efectivo (fallback de 10 s).
for _ in {1..50}; do
  if mountpoint -q "$MOUNT"; then break; fi
  sleep 0.2
done
mountpoint -q "$MOUNT" || { fail "FUSE não montou em 10 s"; exit 3; }
ok "montado em $MOUNT"

# Garante que os scripts são chamados como o utilizador correcto, não root,
# para o md5sum não dar permission denied. Mas como estamos em sudo, OK.

# -----------------------------------------------------------------------------
# Single-thread round-trip
# -----------------------------------------------------------------------------
step "Round-trip single-thread (8 MiB)"
./tests/roundtrip.sh 8
ok "8 MiB single-thread"

step "Round-trip single-thread (64 MiB)"
./tests/roundtrip.sh 64
ok "64 MiB single-thread"

# -----------------------------------------------------------------------------
# Concorrente — carga crescente
# -----------------------------------------------------------------------------
if [[ "$MODE" == "fast" ]]; then
  step "Round-trip concorrente (4 threads × 4 MiB) — modo fast"
  ./tests/concurrent_roundtrip.sh 4 4
  ok "4×4 MiB concorrente"
else
  step "Round-trip concorrente (4 threads × 4 MiB)"
  ./tests/concurrent_roundtrip.sh 4 4
  ok "4×4 MiB concorrente"

  step "Round-trip concorrente (8 threads × 4 MiB)"
  ./tests/concurrent_roundtrip.sh 8 4
  ok "8×4 MiB concorrente"

  step "Round-trip concorrente (16 threads × 8 MiB)"
  ./tests/concurrent_roundtrip.sh 16 8
  ok "16×8 MiB concorrente"
fi

step "Edge cases (suite extensa de cenários)"
./tests/edge_cases.sh
ok "edge cases"

step "Stress: criar + apagar concorrente"
# Cria 4 ficheiros em paralelo, depois apaga em paralelo, várias vezes.
# Exercita o caminho remove_blocks_dedup_batch sob contenção.
for round in 1 2 3; do
  for i in 1 2 3 4; do
    dd if=/dev/urandom of="$MOUNT/stress_${round}_${i}.bin" bs=1M count=2 status=none &
  done
  wait
  for i in 1 2 3 4; do
    rm -f "$MOUNT/stress_${round}_${i}.bin" &
  done
  wait
done
ok "create+unlink concorrente"

# Verifica que não restam ficheiros stress no FS visível.
remaining=$(find "$MOUNT" -name 'stress_*.bin' 2>/dev/null | wc -l)
[[ $remaining -eq 0 ]] || { fail "$remaining ficheiros stress não foram apagados"; exit 4; }
ok "FS limpo após stress"

# -----------------------------------------------------------------------------
# Métricas finais (opcional, informativo)
# -----------------------------------------------------------------------------
step "Métricas finais"
master_size=$(stat -c %s /masterFILE 2>/dev/null || echo "?")
echo "    /masterFILE size: ${master_size} bytes"
freelist_size=$(stat -c %s /table_path_free_block_list 2>/dev/null || echo "?")
echo "    freelist persisted: ${freelist_size} bytes"

# trap EXIT trata do umount.
