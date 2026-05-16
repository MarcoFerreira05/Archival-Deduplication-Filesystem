#!/usr/bin/env bash
set -Eeuo pipefail

print_help() {
    cat <<EOF
Usage: $0 [--jobs=N] [--dedup=PERCENT] [--runtime=SECONDS] [--size=SIZE] [--mode=writes|reads]

Description:
  Runs fio workloads for writes or reads.
  The chosen mode uses its own file set.
  Results are written to a PID-specific directory.
  Note: --runtime only affects reads mode; writes are size-based.
  BASE_DIR is not a CLI flag. To change target mount/path, edit BASE_DIR
  directly inside this script.

Flags:
  --jobs=N          Number of worker files. Default: 4
  --dedup=PERCENT   Write dedupe percentage per job (NOT split by jobs).
                    Example: --jobs=4 --dedup=50 => each job runs at 50%
                    dedupe_percentage. Default: 0
  --runtime=SECONDS Runtime in seconds for read fio jobs. Ignored for writes.
                    Default: 60
  --size=SIZE       Total size to split across jobs.
                    Each fio job gets SIZE / jobs.
                    Example: 50M, 2G. Default: 50M
  --mode=...        writes or reads. Default: writes
                    writes are size-based; reads honor --runtime
  -h, --help        Show this help

Examples:
  $0
  $0 --mode=reads --jobs=8 --runtime=120 --size=500M
  $0 --mode=writes --jobs=8 --dedup=25 --size=500M
EOF
}

JOBS=4
DEDUP=0
RUNTIME=60
SIZE=50M
MODE=writes

for arg in "$@"; do
    case "$arg" in
    -h | --help)
        print_help
        exit 0
        ;;
    --jobs=*)
        JOBS="${arg#*=}"
        ;;
    --dedup=*)
        DEDUP="${arg#*=}"
        ;;
    --runtime=*)
        RUNTIME="${arg#*=}"
        ;;
    --size=*)
        SIZE="${arg#*=}"
        ;;
    --mode=*)
        MODE="${arg#*=}"
        case "$MODE" in
        writes | reads) ;;
        *)
            echo "Unknown mode: $MODE (use writes or reads)" >&2
            print_help
            exit 1
            ;;
        esac
        ;;
    *)
        echo "Unknown option: $arg" >&2
        print_help
        exit 1
        ;;
    esac
done

size_to_bytes() {
    python3 - "$1" <<'PY'
import re
import sys

value = sys.argv[1].strip().upper()
match = re.fullmatch(r"(\d+)([KMG]?B?)?", value)
if not match:
    raise SystemExit(f"Invalid size: {value}")

number = int(match.group(1))
unit = (match.group(2) or "B").replace("B", "")
scale = {
    "": 1,
    "K": 1024,
    "M": 1024**2,
    "G": 1024**3,
}[unit]
print(number * scale)
PY
}

SIZE_BYTES="$(size_to_bytes "$SIZE")"
if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || [[ "$JOBS" -le 0 ]]; then
    echo "Invalid --jobs value: $JOBS (expected integer > 0)" >&2
    exit 1
fi
JOB_SIZE_BYTES=$((SIZE_BYTES / JOBS))
if [[ "$JOB_SIZE_BYTES" -le 0 ]]; then
    echo "Invalid per-job size computed from SIZE=$SIZE and JOBS=$JOBS" >&2
    exit 1
fi

if ! [[ "$DEDUP" =~ ^[0-9]+$ ]] || [[ "$DEDUP" -lt 0 || "$DEDUP" -gt 100 ]]; then
    echo "Invalid --dedup value: $DEDUP (expected integer 0..100)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPEND_FIO="${SCRIPT_DIR}/append-worker.fio"
RANDREAD_FIO="${SCRIPT_DIR}/randread-worker.fio"
ANALYZE_PY="${SCRIPT_DIR}/analyze_fios.py"

BASE_DIR="/mnt/newfs/fs/fio_concurrent-${MODE}-$$"
RESULTS_DIR="./results-${MODE}-$$"

mkdir -p "$BASE_DIR" "$RESULTS_DIR"

if [[ "$MODE" == "writes" ]]; then
    for ((i = 0; i < JOBS; i++)); do
        FILE="${BASE_DIR}/worker_${i}.dat"
        SEED=$((1000 + i))

        fio "$APPEND_FIO" \
            --filename="$FILE" \
            --output="${RESULTS_DIR}/write_${i}.json" \
            --output-format=json \
            --size="$JOB_SIZE_BYTES" \
            --dedupe_percentage="$DEDUP" \
            --randseed="$SEED" &
    done
else
    for ((i = 0; i < JOBS; i++)); do
        FILE="${BASE_DIR}/worker_${i}.dat"

        fio "$RANDREAD_FIO" \
            --filename="$FILE" \
            --output="${RESULTS_DIR}/read_${i}.json" \
            --output-format=json \
            --size="$JOB_SIZE_BYTES" \
            --runtime="$RUNTIME" &
    done
fi

wait

echo "fio run complete (MODE=$MODE, JOBS=$JOBS, DEDUP_PER_JOB=$DEDUP, RUNTIME=${RUNTIME}s, SIZE=$SIZE, JOB_SIZE_BYTES=$JOB_SIZE_BYTES, RESULTS_DIR=$RESULTS_DIR)"
python3 "$ANALYZE_PY" "$MODE" "$RESULTS_DIR"
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'
