#!/bin/bash
# build_probes.sh [OUT-DIR]: builds the harness's two probes against the system Qt, when missing, stale (the
# source is newer) or built for another Qt version. Not part of the project's build.
#
#   shotprobe.so  the LD_PRELOAD probe the driver talks to (eval, find, click, key, grab, recordings);
#   iconprobe     which hicolor folder Qt's and KDE's icon lookup take per size and device pixel ratio.
#
# OUT-DIR defaults to $SHOT_WORK/probe ($SHOT_WORK: $XDG_CACHE_HOME/ferrous-ui-capture). Both use public Qt API only.
# SHOT_BUILD_WRAPPER, when set, is a command the compiler runs under (e.g. a script that limits parallel builds).
set -euo pipefail

H=$(dirname "$(realpath "$0")")
WORK=${SHOT_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/ferrous-ui-capture}
OUT=${1:-$WORK/probe}
WRAP=()
if [ -n "${SHOT_BUILD_WRAPPER:-}" ]; then
    read -r -a WRAP <<< "$SHOT_BUILD_WRAPPER"
fi
mkdir -p "$OUT"
QTV=$(pkg-config --modversion Qt6Quick)

stale() {  # stale BINARY SOURCE: missing, older than its source, or built for another Qt
    [ ! -e "$1" ] || [ "$2" -nt "$1" ] || [ "$(cat "$1.qt" 2>/dev/null)" != "$QTV" ]
}

if stale "$OUT/shotprobe.so" "$H/shotprobe.cpp"; then
    # shellcheck disable=SC2046  # pkg-config's flags are meant to split
    "${WRAP[@]}" g++ -shared -fPIC -O2 -std=c++17 "$H/shotprobe.cpp" -o "$OUT/shotprobe.so.tmp" \
        $(pkg-config --cflags --libs Qt6Quick Qt6Qml Qt6Gui Qt6Core Qt6Concurrent Qt6Network)
    mv "$OUT/shotprobe.so.tmp" "$OUT/shotprobe.so"
    echo "$QTV" > "$OUT/shotprobe.so.qt"
    echo "built $OUT/shotprobe.so (Qt $QTV)"
fi
if stale "$OUT/iconprobe" "$H/iconprobe.cpp"; then
    # shellcheck disable=SC2046
    "${WRAP[@]}" g++ -fPIC -O2 -std=c++17 "$H/iconprobe.cpp" -o "$OUT/iconprobe.tmp" \
        $(pkg-config --cflags --libs Qt6Widgets Qt6Gui Qt6Core) -l:libKF6IconThemes.so.6
    mv "$OUT/iconprobe.tmp" "$OUT/iconprobe"
    echo "$QTV" > "$OUT/iconprobe.qt"
    echo "built $OUT/iconprobe (Qt $QTV)"
fi
