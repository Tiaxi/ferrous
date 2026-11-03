#!/bin/bash
# The overview decode, then the first whole-track waveform window, a fresh
# process (cold service and empty waveform cache) each run.
# The window ends where the overview's audio does.
# Usage: run_overview_window.sh <out.jsonl> <selection.txt> <sample-dir> <reps> <ids...>
# PERF_BIN chooses the driver (default: this checkout's release build).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
perf="$repo/target/perf"
bin="${PERF_BIN:-$repo/target/release/examples/perf_analysis}"
out="$1"; sel="$2"; dir="$3"; reps="$4"; shift 4
for rep in $(seq 1 "$reps"); do
  for id in "$@"; do
    ext=$(grep -P "^$id\t" "$sel" | cut -f6)
    iso="$perf/runs/ovwin-$$"; rm -rf -- "$iso"
    r=$("$here/iso.sh" "$iso" "$bin" overview-window "$dir/$id.$ext" --points 3440 2>>"$out.err")
    echo "{\"id\":\"$id\",\"rep\":$rep,\"r\":$r}" >> "$out"
    rm -rf -- "$iso"
  done
done
