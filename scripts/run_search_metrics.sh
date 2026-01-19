#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/run_search_metrics.sh [options]

Options:
  -t <total_games>   Total games to run (default: 100)
  -d <depth>         Search depth (default: 6)
  -n <workers>       Parallel workers (default: auto-detect CPUs)
  -s <seed>          Base seed (optional; random if omitted)
  -o <dir>           Output directory for JSONL logs (default: search_metrics_logs)
  -z                Compress JSONL logs with gzip (adds .gz suffix)
USAGE
}

if command -v getconf >/dev/null 2>&1; then
  default_workers=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
else
  default_workers=1
fi

total_games=100
depth=6
workers="$default_workers"
out_dir="search_metrics_logs"
compress=0
base_seed=""

while getopts ":t:d:n:s:o:zh" opt; do
  case "$opt" in
    t) total_games="$OPTARG" ;;
    d) depth="$OPTARG" ;;
    n) workers="$OPTARG" ;;
    s) base_seed="$OPTARG" ;;
    o) out_dir="$OPTARG" ;;
    z) compress=1 ;;
    h) usage; exit 0 ;;
    *) usage; exit 1 ;;
  esac
done

make -s

mkdir -p "$out_dir"

if [[ -z "$base_seed" ]]; then
  base_seed=$(date +%s)
fi

if (( total_games <= 0 || workers <= 0 )); then
  echo "error: total_games and workers must be positive" >&2
  exit 1
fi

games_per_worker=$(( (total_games + workers - 1) / workers ))

tmpdir=$(mktemp -d)
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

declare -a pids
offset=0
for idx in $(seq 0 $(( workers - 1 ))); do
  remaining=$(( total_games - offset ))
  if (( remaining <= 0 )); then
    break
  fi
  count=$games_per_worker
  if (( count > remaining )); then
    count=$remaining
  fi
  seed=$(( base_seed + offset ))
  out="$tmpdir/metrics_${idx}.log"
  (
    set -e
    base_path=$(printf "%s/depth%02d_worker%02d_seed%010d" "$out_dir" "$depth" "$idx" "$seed")
    cmd=(./2048 -T -M -q -i"${count}" -d"${depth}" -J "$base_path")
    if (( compress )); then
      cmd+=( -Z )
    fi
    cmd+=("${seed}")
    "${cmd[@]}" >"$out"
  ) &
  pids+=($!)
  offset=$(( offset + count ))
done

failures=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    failures=$(( failures + 1 ))
  fi
done

if (( failures > 0 )); then
  echo "error: $failures worker(s) failed" >&2
  exit 1
fi

cat "$tmpdir"/metrics_*.log | awk -v depth="${depth}" '
  $1 == "search_metrics" {
    for (i=1; i<=NF; i++) {
      split($i, kv, "=");
      key=kv[1]; val=kv[2];
      if (key=="moves") moves+=val;
      else if (key=="total_nodes") total_nodes+=val;
      else if (key=="nodes_per_move") nodes_per_move+=val;
      else if (key=="cache_lookups") lookups+=val;
      else if (key=="cache_hits") hits+=val;
      games+=1;
    }
  }
  END {
    if (games == 0) { print "no metrics"; exit 1; }
    avg_nodes_per_move = nodes_per_move / games;
    overall_nodes_per_move = (moves > 0) ? (total_nodes / moves) : 0;
    hit_rate = (lookups > 0) ? (hits * 100.0 / lookups) : 0;
    printf("games=%d depth=%d total_moves=%d total_nodes=%d avg_nodes_per_move=%.1f overall_nodes_per_move=%.1f cache_hit_rate=%.1f%%\\n",
           games, depth, moves, total_nodes, avg_nodes_per_move,
           overall_nodes_per_move, hit_rate);
  }
'
