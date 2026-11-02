#!/bin/bash
# Seek latency through the bridge with GStreamer playback, idle and busy.
# Usage: run_seek_matrix.sh <out.jsonl> <repeats> <sample-dir> <ids...>
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
perf="$repo/target/perf"
bin="$repo/target/release/examples/perf_analysis"
out="$1"; reps="$2"; dir="$3"; shift 3
configs=(
  "idle_centered|--mode centered --idle --warmup 2 --settle 3"
  "busy_centered|--mode centered --warmup 0.3 --settle 1.0"
  "idle_rolling|--mode rolling --idle --warmup 2 --settle 3"
  "busy_rolling|--mode rolling --warmup 0.3 --settle 1.0"
)
for rep in $(seq 1 "$reps"); do
  for id in "$@"; do
    f=$(ls "$dir"/"$id".* | head -1)
    for cfg in "${configs[@]}"; do
      IFS='|' read -r name args <<< "$cfg"
      if pgrep -x ferrous >/dev/null; then echo "ferrous running; stop" >&2; exit 1; fi
      iso="$perf/runs/seek-$$"; rm -rf -- "$iso"
      load=$(cut -d' ' -f1 /proc/loadavg)
      res=$("$here/iso.sh" "$iso" "$bin" seek "$f" --view per-channel --fft 2048 $args --timeout 120 2>>"$out.err")
      echo "{\"config\":\"$name\",\"rep\":$rep,\"id\":\"$id\",\"load\":$load,\"r\":$res}" >> "$out"
      rm -rf -- "$iso"
    done
  done
done
