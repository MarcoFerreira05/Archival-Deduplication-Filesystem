#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# tests/concurrent_roundtrip.sh — Teste de correctude sob carga concorrente.
#
# Lança N cópias paralelas, cada uma a escrever conteúdo aleatório próprio
# para um ficheiro distinto no FUSE, e a validar md5 round-trip.
#
# Se a concorrência introduzir corrupção (race entre threads, lock errado,
# torn read/write), as md5 vão divergir e o teste falha.
#
# Pré-requisito: FUSE montado em /mnt/fs (via fuse.sh em background ou
# noutro terminal).
#
# Uso:
#   ./tests/concurrent_roundtrip.sh                # 8 threads × 4 MiB
#   ./tests/concurrent_roundtrip.sh 16 8           # 16 threads × 8 MiB
# -----------------------------------------------------------------------------
set -euo pipefail

MOUNT="/mnt/fs"
N="${1:-8}"
SIZE_MB="${2:-4}"

if [[ ! -d "${MOUNT}" ]]; then
  echo "ERRO: ${MOUNT} não existe. Montar o FUSE primeiro com sudo ./fuse.sh." >&2
  exit 1
fi

cleanup() {
  for i in $(seq 1 "$N"); do
    rm -f "/tmp/conc.$i.src" "${MOUNT}/conc_$i.bin" 2>/dev/null || true
  done
}
trap cleanup EXIT

# Lança N cópias paralelas em background. Cada uma:
#   1. Gera SIZE_MB MiB de conteúdo aleatório distinto.
#   2. Copia para o FUSE.
#   3. Compara md5 do original com o do destino.
#
# Se qualquer thread falhar, set -e faz exit imediato.

declare -a pids
for i in $(seq 1 "$N"); do
  (
    src="/tmp/conc.$i.src"
    dst="${MOUNT}/conc_$i.bin"
    dd if=/dev/urandom of="$src" bs=1M count="$SIZE_MB" status=none
    cp "$src" "$dst"
    sync
    md5_src=$(md5sum "$src" | awk '{print $1}')
    md5_dst=$(md5sum "$dst" | awk '{print $1}')
    if [[ "$md5_src" != "$md5_dst" ]]; then
      echo "FAIL thread $i: src=${md5_src} dst=${md5_dst}" >&2
      exit 1
    fi
    echo "PASS thread $i (${SIZE_MB} MiB, md5=${md5_src})"
  ) &
  pids+=( "$!" )
done

# Aguarda por todos. Se algum falhar, exit não-zero.
fail=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    fail=1
  fi
done

if [[ $fail -eq 0 ]]; then
  echo "All $N threads passed (${SIZE_MB} MiB each)."
else
  echo "FAILED: at least one thread reported md5 mismatch." >&2
  exit 1
fi
