#!/bin/bash
# Launches the app with a copy of the user's library database in an isolated
# data directory, with watching on, so that it rescans the roots at startup,
# and samples per-thread CPU through the scan, then while playing a track.
#
# Usage: app_library.sh <out.jsonl> <library.sqlite3> <file-to-play> [label]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
perf="$repo/target/perf"
app="${FERROUS_PERF_APP:-$perf/ui-build/ferrous}"
out="$1"; db="$2"; file="$3"; label="${4:-lib}"

if pgrep -x ferrous >/dev/null; then
    echo "A Ferrous instance is running already; not starting another." >&2
    exit 1
fi
iso="$perf/runs/app-library"
rm -rf -- "$iso"
mkdir -p "$iso/config/ferrous" "$iso/data/ferrous"
cp "$db" "$iso/data/ferrous/library.sqlite3"
cp "$here/app-settings.txt" "$iso/config/ferrous/settings.txt"

"$here/iso.sh" "$iso" "$app" > "$iso/app.log" 2>&1 &
pid=$!
trap 'kill -TERM $pid 2>/dev/null || true' EXIT
sample() { python3 -I "$here/threadsample.py" "$pid" "$1" "$2" "$out" > /dev/null; }
mpris() {
    busctl --user call org.mpris.MediaPlayer2.ferrous /org/mpris/MediaPlayer2 \
        org.mpris.MediaPlayer2.Player "$@"
}
for i in 1 2 3 4 5 6 7 8 9; do
    sample 10 "$label startup_${i}0s"
done
mpris OpenUri s "file://$file"
sample 20 "$label play_first20s"
sample 30 "$label play_steady"
kill -TERM "$pid"
wait "$pid" || true
trap - EXIT
