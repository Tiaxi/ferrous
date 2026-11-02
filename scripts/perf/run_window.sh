#!/bin/bash
# Waveform detail windows through the FFI, a fresh process (cold service) each run.
# Usage: run_window.sh <out.jsonl> <selection.txt> <sample-dir> <reps> <ids...>
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
perf="$repo/target/perf"
bin="$repo/target/release/examples/perf_analysis"
out="$1"; sel="$2"; dir="$3"; reps="$4"; shift 4
for rep in $(seq 1 "$reps"); do
  for id in "$@"; do
    line=$(grep -P "^$id\t" "$sel"); dur=$(cut -f3 <<< "$line"); ext=$(cut -f6 <<< "$line")
    iso="$perf/runs/win-$$"; rm -rf -- "$iso"
    r=$("$here/iso.sh" "$iso" "$bin" window "$dir/$id.$ext" --window 10 --points 3440 --duration "$dur" 2>>"$out.err")
    echo "{\"id\":\"$id\",\"rep\":$rep,\"r\":$r}" >> "$out"
    rm -rf -- "$iso"
  done
done
