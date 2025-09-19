#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/run_selfplay.sh [options]

Options:
  -n <workers>    Number of parallel self-play processes to launch (default: auto-detect CPUs)
  -g <games>      Number of games per worker (default: 50)
  -d <depth>      Search depth to pass to the engine (default: 5)
  -o <dir>        Directory for JSONL logs (default: selfplay_logs)
  -e <path>       Engine executable to run (default: ./2048b if present, otherwise ./2048)
  -s <seed>       Base seed for reproducibility (default: current epoch seconds)
  -z              Compress logs with gzip (adds .gz suffix)
  -h              Show this help message

Each worker emits structured JSONL via the engine's -J flag. Seeds are offset
by the number of games per worker so workers do not overlap their sequences.
USAGE
}

if command -v getconf >/dev/null 2>&1; then
  default_workers=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
else
  default_workers=1
fi

games_per_worker=50
depth=5
out_dir="selfplay_logs"
compress=0
base_seed=$(date +%s)
engine=""
workers="$default_workers"

while getopts ":n:g:d:o:e:s:zh" opt; do
  case "$opt" in
    n)
      workers="$OPTARG"
      ;;
    g)
      games_per_worker="$OPTARG"
      ;;
    d)
      depth="$OPTARG"
      ;;
    o)
      out_dir="$OPTARG"
      ;;
    e)
      engine="$OPTARG"
      ;;
    s)
      base_seed="$OPTARG"
      ;;
    z)
      compress=1
      ;;
    h)
      usage
      exit 0
      ;;
    *)
      usage
      exit 1
      ;;
  esac
done

if [[ -z "$engine" ]]; then
  if [[ -x ./2048b ]]; then
    engine="./2048b"
  else
    engine="./2048"
  fi
fi

if [[ ! -x "$engine" ]]; then
  echo "error: engine executable '$engine' not found or not executable" >&2
  exit 1
fi

mkdir -p "$out_dir"

total_games=$(( workers * games_per_worker ))
echo "Launching $workers worker(s) * $games_per_worker game(s) each = $total_games total games" >&2

declare -a pids
for idx in $(seq 0 $(( workers - 1 ))); do
  seed=$(( base_seed + idx * games_per_worker ))
  base_path=$(printf "%s/depth%02d_worker%02d_seed%010d" "$out_dir" "$depth" "$idx" "$seed")
  if (( compress )); then
    printf 'worker %02d -> seed %d -> %s_game*.jsonl.gz\n' "$idx" "$seed" "$base_path" >&2
  else
    printf 'worker %02d -> seed %d -> %s_game*.jsonl\n' "$idx" "$seed" "$base_path" >&2
  fi
  (
    set -e
    cmd=("$engine" -q -d "$depth" -i "$games_per_worker" -J "$base_path" "$seed")
    "${cmd[@]}" >/dev/null 2>&1
    if (( compress )); then
      shopt -s nullglob
      for f in "${base_path}"_game*.jsonl "${base_path}"_game*.meta.json; do
        gzip -f "$f"
      done
    fi
  ) &
  pids+=($!)
done

failures=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    failures=$(( failures + 1 ))
  fi
done

if (( failures > 0 )); then
  echo "warning: $failures worker(s) exited with a non-zero status" >&2
  exit 1
fi

echo "All workers completed" >&2
