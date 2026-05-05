#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Helper Functions VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV

log() {
  printf '[%s] %s\n' "$(date +'%H:%M:%S')" "$*" >&2
}

die() {
  log "ERROR: $*"
  exit 1
}

# Start a root command detached (via setsid) and print its PID.
# The returned PID is the PID of the detached command.
#
# Usage: sudo_detached_pid "<command-string>" "<working-dir>" "<log-file>"
sudo_detached_pid() {
  local cmd="$1" workdir="$2" logfile="$3"
  sudo env CMD="$cmd" WORKDIR="$workdir" LOGFILE="$logfile" bash -c '
    set -e
    cd "$WORKDIR"
    # Run detached so benchmark.sh can continue.
    # Note: $! is setsid PID; setsid execs the target, so PID is the target PID.
    setsid bash -c "$CMD" >"$LOGFILE" 2>&1 &
    echo $!
  '
}

wait_for_mountpoint() {
  local mp="$1" timeout_s="$2"
  local t_end=$((SECONDS + timeout_s))
  while (( SECONDS < t_end )); do
    if mountpoint -q "$mp"; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

wait_for_stopped() {
  local pid="$1" timeout_s="$2"
  local t_end=$((SECONDS + timeout_s))
  while (( SECONDS < t_end )); do
    # STAT contains T when stopped.
    local stat
    stat="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
    if [[ "$stat" == *T* ]]; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

wait_for_exit_root() {
  local pid="$1" timeout_s="$2"
  local t_end=$((SECONDS + timeout_s))
  while (( SECONDS < t_end )); do
    if sudo kill -0 "$pid" 2>/dev/null; then
      sleep 0.1
    else
      return 0
    fi
  done
  return 1
}

unmount_fs_if_mounted() {
  if mountpoint -q /mnt/fs; then
    log "Unmounting /mnt/fs"
    sudo umount /mnt/fs 2>/dev/null || true
  fi
}

chown_if_exists_root() {
  local path="$1"
  [[ -e "$path" ]] || return 0
  sudo chown "$(id -u):$(id -g)" "$path" 2>/dev/null || true
}

usage() {
  cat <<EOF
Usage: ./benchmark.sh [output_dir]

Runs the end-to-end benchmark and writes all artifacts under output_dir.
If omitted, defaults to: ./results/<timestamp>/
EOF
}

# Helper Functions ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

# Main Logic VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVV

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

timestamp="$(date +'%Y-%m-%d--%H-%M-%S')"
OUTDIR="${1:-$ROOT/results/$timestamp}"
mkdir -p "$OUTDIR"

PASSTHROUGH_PID=""
WORKLOAD_PID=""
SYSCOUNTER_PASSTHROUGH_PID=""
SYSCOUNTER_WORKLOAD_PID=""
CACHESTAT_PASSTHROUGH_PID=""

cleanup() {
  set +e

  if [[ -n "${SYSCOUNTER_WORKLOAD_PID}" ]]; then
    sudo kill -INT "$SYSCOUNTER_WORKLOAD_PID" 2>/dev/null || true
  fi
  if [[ -n "${SYSCOUNTER_PASSTHROUGH_PID}" ]]; then
    sudo kill -INT "$SYSCOUNTER_PASSTHROUGH_PID" 2>/dev/null || true
  fi
  if [[ -n "${CACHESTAT_PASSTHROUGH_PID}" ]]; then
    sudo kill -INT "$CACHESTAT_PASSTHROUGH_PID" 2>/dev/null || true
  fi

  if [[ -n "${WORKLOAD_PID}" ]]; then
    kill -CONT "$WORKLOAD_PID" 2>/dev/null || true
  fi

  unmount_fs_if_mounted
}
trap cleanup EXIT

log "Output dir: $OUTDIR"

# Ensure sudo credentials are available early (so we don't block mid-run).
sudo -v

log "Dropping page cache (including dentries and inodes)"
sudo sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches' || die "Failed to drop page cache"

log "Cleaning previous run data"
"$ROOT/clean_fuse_data.sh" >"$OUTDIR/clean.log" 2>&1 || die "clean_fuse_data.sh failed (see $OUTDIR/clean.log)"

log "Starting FUSE passthrough"
PASSTHROUGH_LOG="$OUTDIR/passthrough.log"
PASSTHROUGH_PID="$(sudo_detached_pid './passthrough /mnt/fs -omodules="subdir,subdir=/backend" -oallow_other -f' "$ROOT" "$PASSTHROUGH_LOG")"
[[ "$PASSTHROUGH_PID" =~ ^[0-9]+$ ]] || die "Failed to start passthrough (got PID: $PASSTHROUGH_PID)"
log "passthrough PID: $PASSTHROUGH_PID"

log "Waiting for /mnt/fs to become a mountpoint"
wait_for_mountpoint "/mnt/fs" 15 || die "/mnt/fs did not mount (see $PASSTHROUGH_LOG)"

# Workload: start a wrapper process that stops itself before exec().
# This gives us a stable PID that becomes the python PID after exec.
log "Starting workload in STOPPED state (so we can attach BPF first)"
WORKLOAD_LOG="$OUTDIR/workload.log"
(
  cd "$ROOT/benchmarks"
  # Stop this process immediately, then replace it with python.
  # PID stays the same across exec.
  exec bash -c 'kill -STOP $$; exec python3 dedupe_workload.py'
) >"$WORKLOAD_LOG" 2>&1 &
WORKLOAD_PID=$!

wait_for_stopped "$WORKLOAD_PID" 5 || die "Workload did not enter STOPPED state"
log "workload PID (will become python PID): $WORKLOAD_PID"

log "Starting syscounter for passthrough + workload"
PASSTHROUGH_CSV="$OUTDIR/passthrough_results.csv"
WORKLOAD_CSV="$OUTDIR/workload_results.csv"

SYSCOUNTER_PASSTHROUGH_PID="$(sudo_detached_pid "./bpf_programs/syscounter/syscounter $PASSTHROUGH_PID '$PASSTHROUGH_CSV'" "$ROOT" "$OUTDIR/syscounter_passthrough.log")"
SYSCOUNTER_WORKLOAD_PID="$(sudo_detached_pid "./bpf_programs/syscounter/syscounter $WORKLOAD_PID '$WORKLOAD_CSV'" "$ROOT" "$OUTDIR/syscounter_workload.log")"

log "Starting cachestat for passthrough"
CACHESTAT_PASSTHROUGH_JSON="$OUTDIR/cachestat_passthrough.json"
CACHESTAT_PASSTHROUGH_PID="$(sudo_detached_pid "./cachestat 99999 --pids $PASSTHROUGH_PID --output '$CACHESTAT_PASSTHROUGH_JSON'" "$ROOT/bpf_programs/cachestat" "$OUTDIR/cachestat_passthrough.log")"
[[ "$CACHESTAT_PASSTHROUGH_PID" =~ ^[0-9]+$ ]] || die "Failed to start cachestat for passthrough (got PID: $CACHESTAT_PASSTHROUGH_PID)"

sleep 1

log "Resuming workload"
run_start_ns="$(date +%s%N)"
kill -CONT "$WORKLOAD_PID"

set +e
wait "$WORKLOAD_PID"
workload_rc=$?
set -e
run_end_ns="$(date +%s%N)"

elapsed_ms=$(( (run_end_ns - run_start_ns) / 1000000 ))
printf 'workload_rc=%s\nelapsed_ms=%s\n' "$workload_rc" "$elapsed_ms" >"$OUTDIR/workload.meta"

log "Workload finished (rc=$workload_rc, elapsed_ms=$elapsed_ms)"

log "Stopping BPF programs (SIGINT)"
sudo kill -INT "$SYSCOUNTER_PASSTHROUGH_PID" 2>/dev/null || true
sudo kill -INT "$SYSCOUNTER_WORKLOAD_PID" 2>/dev/null || true
sudo kill -INT "$CACHESTAT_PASSTHROUGH_PID" 2>/dev/null || true

wait_for_exit_root "$SYSCOUNTER_PASSTHROUGH_PID" 10 || log "syscounter(passthrough) did not exit within timeout"
wait_for_exit_root "$SYSCOUNTER_WORKLOAD_PID" 10 || log "syscounter(workload) did not exit within timeout"
wait_for_exit_root "$CACHESTAT_PASSTHROUGH_PID" 10 || log "cachestat(passthrough) did not exit within timeout"

# Make sure artifacts are readable without sudo.
chown_if_exists_root "$PASSTHROUGH_CSV"
chown_if_exists_root "$WORKLOAD_CSV"
chown_if_exists_root "$OUTDIR/syscounter_passthrough.log"
chown_if_exists_root "$OUTDIR/syscounter_workload.log"
chown_if_exists_root "$OUTDIR/cachestat_passthrough.log"
chown_if_exists_root "$CACHESTAT_PASSTHROUGH_JSON"
chown_if_exists_root "$PASSTHROUGH_LOG"

unmount_fs_if_mounted
if [[ -n "${PASSTHROUGH_PID}" ]]; then
  wait_for_exit_root "$PASSTHROUGH_PID" 10 || log "passthrough did not exit within timeout"
fi

log "Measuring disk used"
{
  echo "# masterFILE"
  sudo du -m /masterFILE 2>&1 || true
  echo

  metaindex_paths=(
    /table_path_hash_to_master
    /table_path_file_to_master
    /table_path_master_infos
    /table_path_file_to_sizes
    /table_path_free_block_list
  )

  total_metaindex_mb=0
  for path in "${metaindex_paths[@]}"; do
    echo "# $path"
    if du_output="$(sudo du -m "$path" 2>&1)"; then
      echo "$du_output"
      used_mb="$(awk '{print $1}' <<<"$du_output")"
      if [[ "$used_mb" =~ ^[0-9]+$ ]]; then
        total_metaindex_mb=$((total_metaindex_mb + used_mb))
      fi
    else
      echo "$du_output"
    fi
    echo
  done

  echo "# total_metaindex_files_mb"
  echo "${total_metaindex_mb} MB"
} | tee "$OUTDIR/disk_usage.txt" >/dev/null
chown_if_exists_root "$OUTDIR/disk_usage.txt"

cat >"$OUTDIR/pids.env" <<EOF
PASSTHROUGH_PID=$PASSTHROUGH_PID
WORKLOAD_PID=$WORKLOAD_PID
SYSCOUNTER_PASSTHROUGH_PID=$SYSCOUNTER_PASSTHROUGH_PID
SYSCOUNTER_WORKLOAD_PID=$SYSCOUNTER_WORKLOAD_PID
CACHESTAT_PASSTHROUGH_PID=$CACHESTAT_PASSTHROUGH_PID
EOF

log "Done. Results in: $OUTDIR"

exit "$workload_rc"
