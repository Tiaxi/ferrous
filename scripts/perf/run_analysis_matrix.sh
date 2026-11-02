#!/bin/bash
# Runs the analysis mode over every sample and configuration, cold each time.
# Usage: run_analysis_matrix.sh <out.jsonl> <repeats> <sample-dir> [sample ids...]
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
perf="$repo/target/perf"
bin="$repo/target/release/examples/perf_analysis"
out="$1"; reps="$2"; dir="$3"; shift 3
ids=("$@")
[ ${#ids[@]} -eq 0 ] && ids=(S01 S02 S03 S04 S05 S06 S07 S08 S09 S10 S11 S12)
configs=(
  "default|10|--view per-channel --mode centered --fft 2048"
  "full_pc|100000|--view per-channel --mode centered --fft 2048"
  "full_dm|100000|--view downmix --mode centered --fft 2048"
  "full_pc8k|100000|--view per-channel --mode centered --fft 8192"
)
for rep in $(seq 1 "$reps"); do
  for id in "${ids[@]}"; do
    f=$(ls "$dir"/"$id".* | grep -v MAPPING | head -1)
    for cfg in "${configs[@]}"; do
      IFS='|' read -r name look args <<< "$cfg"
      iso="$perf/runs/matrix-$$"
      rm -rf -- "$iso"
      load=$(cut -d' ' -f1 /proc/loadavg)
      res=$(FERROUS_SPECTROGRAM_LOOKAHEAD_SECONDS=$look "$here/iso.sh" "$iso" "$bin" analysis "$f" $args --timeout 300 2>>"$out.err")
      echo "{\"config\":\"$name\",\"rep\":$rep,\"id\":\"$id\",\"load\":$load,\"r\":$res}" >> "$out"
      rm -rf -- "$iso"
    done
  done
done
