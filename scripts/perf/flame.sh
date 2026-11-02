#!/bin/bash
# Usage: flame.sh <name> <outdir> <perf_analysis args...>
# Records the harness with DWARF call graphs in a fresh isolated dir and
# writes <outdir>/<name>.svg (all threads) and <name>.folded.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
perf_dir="$repo/target/perf"; mkdir -p "$perf_dir/data"
bin="${FLAME_BIN:-$repo/target/fp/release/examples/perf_analysis}"
name="$1"; out="$2"; shift 2
iso="$perf_dir/runs/flame-$$"; rm -rf -- "$iso"
"$here/iso.sh" "$iso" perf record -e cycles:u -F 1999 --call-graph fp --user-callchains -o "$perf_dir/data/$name.perf.data" -- "$bin" "$@" > "$out/$name.result.json" 2> "$perf_dir/data/$name.perf.log"
rm -rf -- "$iso"
perf script -i "$perf_dir/data/$name.perf.data" 2>/dev/null | inferno-collapse-perf --all > "$out/$name.folded"
inferno-flamegraph --title "$name" --width 1800 < "$out/$name.folded" > "$out/$name.svg"
