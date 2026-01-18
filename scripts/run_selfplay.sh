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
base_seed=""
base_seed_supplied=0
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
      base_seed_supplied=1
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

if (( workers <= 0 )); then
  echo "error: number of workers must be positive" >&2
  exit 1
fi
if (( games_per_worker <= 0 )); then
  echo "error: number of games per worker must be positive" >&2
  exit 1
fi

total_games=$(( workers * games_per_worker ))
max_seed=$(( 0x7fffffff ))

if (( total_games <= 0 )); then
  echo "error: total games computed as zero or overflowed" >&2
  exit 1
fi

if (( total_games > max_seed + 1 )); then
  echo "error: requesting $total_games games exceeds unique seed capacity (max $(($max_seed + 1)))" >&2
  exit 1
fi

max_valid_base=$(( max_seed - total_games + 1 ))
if [[ -z "$base_seed" ]]; then
  random_limit=$(( max_valid_base + 1 ))
  if (( random_limit <= 0 )); then
    base_seed=0
  elif command -v python3 >/dev/null 2>&1; then
    base_seed=$(python3 - "$random_limit" <<'PY'
import secrets
import sys

limit = int(sys.argv[1])
print(secrets.randbelow(limit))
PY
    )
  elif command -v python >/dev/null 2>&1; then
    base_seed=$(python - "$random_limit" <<'PY'
import secrets
import sys

limit = int(sys.argv[1])
print(secrets.randbelow(limit))
PY
    )
  else
    rand_word=$(od -An -N4 -tu4 /dev/urandom | tr -d ' \n')
    if [[ -z "$rand_word" ]]; then
      echo "error: unable to obtain randomness for base seed" >&2
      exit 1
    fi
    if (( random_limit == 1 )); then
      base_seed=0
    else
      base_seed=$(( rand_word % random_limit ))
    fi
  fi
fi

if [[ ! "$base_seed" =~ ^-?[0-9]+$ ]]; then
  echo "error: base seed '$base_seed' is not an integer" >&2
  exit 1
fi

if (( base_seed < 0 )); then
  echo "error: base seed must be non-negative" >&2
  exit 1
fi

if (( base_seed > max_valid_base )); then
  echo "error: base seed $base_seed leaves insufficient headroom for $total_games unique seeds (max $max_valid_base)" >&2
  exit 1
fi

echo "Launching $workers worker(s) * $games_per_worker game(s) each = $total_games total games" >&2
if (( ! base_seed_supplied )); then
  echo "Auto-selected base seed $base_seed (randomized, reserving ${total_games} unique values)" >&2
fi

declare -a pids
for idx in $(seq 0 $(( workers - 1 ))); do
  seed=$(( base_seed + idx * games_per_worker ))
  if (( seed > max_seed - (games_per_worker - 1) )); then
    echo "error: internal seed calculation overflowed for worker $idx" >&2
    exit 1
  fi
  base_path=$(printf "%s/depth%02d_worker%02d_seed%010d" "$out_dir" "$depth" "$idx" "$seed")
  if (( compress )); then
    printf 'worker %02d -> seed %d -> %s_game*.jsonl.gz\n' "$idx" "$seed" "$base_path" >&2
  else
    printf 'worker %02d -> seed %d -> %s_game*.jsonl\n' "$idx" "$seed" "$base_path" >&2
  fi
  (
    set -e
    cmd=("$engine" -q -d "$depth" -i "$games_per_worker" -J "$base_path")
    if (( compress )); then
      cmd+=( -Z )
    fi
    cmd+=("$seed")
    "${cmd[@]}" >/dev/null 2>&1
    if (( compress )); then
      shopt -s nullglob
      for f in "${base_path}"_game*.jsonl; do
        gzip -fn "$f"
      done
      shopt -u nullglob
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
