#!/bin/bash
# Analysis of files read in place from the network share: the first run is
# cold (not in the client's page cache), the next ones warm.
# Usage: run_nfs.sh <out.jsonl> <nfs_selection.txt>
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
perf="$repo/target/perf"
bin="$repo/target/release/examples/perf_analysis"
out="$1"
while IFS=$'\t' read -r id desc dur path; do
  for rep in 1 2 3; do
    iso="$perf/runs/nfs-$$"; rm -rf -- "$iso"
    res=$("$here/iso.sh" "$iso" "$bin" analysis "$path" --view per-channel --mode centered --fft 2048 --timeout 120 2>>"$out.err" | sed "s|\"file\":\"[^\"]*\"|\"file\":\"$id\"|")
    echo "{\"config\":\"nfs_default\",\"rep\":$rep,\"id\":\"$id\",\"cold\":$([ $rep = 1 ] && echo true || echo false),\"r\":$res}" >> "$out"
    rm -rf -- "$iso"
  done
done < "$2"
