#!/bin/bash
# Launches the Ferrous desktop app in an isolated HOME/XDG tree and samples
# its per-thread CPU and wakeups in a sequence of playback states, driven
# over MPRIS.
#
# Usage: app_states.sh <out.jsonl> <rep> <stereo-file> <surround-file> <raw-surround-file>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
perf="$repo/target/perf"
app="${FERROUS_PERF_APP:-$perf/ui-build/ferrous}"
out="$1"; rep="$2"; stereo="$3"; surround="$4"; raw="$5"
sample_s="${SAMPLE_S:-30}"

if pgrep -x ferrous >/dev/null; then
    echo "A Ferrous instance is running already; not starting another." >&2
    pgrep -ax ferrous >&2
    exit 1
fi

iso="$perf/runs/app-$rep"
rm -rf -- "$iso"
mkdir -p "$iso/config/ferrous"
# Display settings like a real user's: centered, per channel, FFT 2048.
cp "$here/app-settings.txt" "$iso/config/ferrous/settings.txt"

"$here/iso.sh" "$iso" "$app" > "$iso/app.log" 2>&1 &
pid=$!
echo "app pid $pid" >&2
trap 'kill -TERM $pid 2>/dev/null || true' EXIT

mpris() {
    # Calls only the D-Bus connection of the app started here: the generic
    # MPRIS name may belong to another Ferrous, such as the user's own, and a
    # unique connection name is never reused, so a call fails once the app
    # is gone instead of reaching another instance.
    local conn
    conn=$(busctl --user list --no-legend | awk -v pid="$pid" \
        '!found && $1 ~ /^org\.mpris\.MediaPlayer2\.ferrous/ && $2 == pid { print $5; found = 1 }')
    if [ -z "$conn" ]; then
        echo "no Ferrous MPRIS name is owned by PID $pid; refusing to call another instance" >&2
        return 1
    fi
    busctl --user call "$conn" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player "$@"
}
sample() {
    python3 -I "$here/threadsample.py" "$pid" "$1" "$2" "$out" > /dev/null
}
wait_s() {
    # Waits without sampling, as the app settles.
    python3 -I -c "import time; time.sleep($1)"
}

wait_s 12
sample "$sample_s" "r$rep idle_empty"

for kind in stereo surround raw; do
    file="${!kind}"
    mpris OpenUri s "file://$file"
    sample 20 "r$rep ${kind}_play_first20s"
    wait_s 10
    sample "$sample_s" "r$rep ${kind}_play_steady"
    mpris Pause
    wait_s 3
    sample "$sample_s" "r$rep ${kind}_paused"
    mpris Play
    wait_s 2
done
mpris Stop
wait_s 3
sample "$sample_s" "r$rep stopped_with_queue"

kill -TERM "$pid"
wait "$pid" || true
trap - EXIT
