#!/bin/bash
# build_probes.sh [OUT-DIR]: builds the harness's two probes against the system Qt, when missing or stale: built
# from other source, other flags or for another Qt version (a key of those and of the binary itself beside each
# binary, so another checkout of the same source needs no rebuild, and a binary that another checkout's script
# replaced is not taken for this one's). Not part of the project's build.
#
#   shotprobe.so  the LD_PRELOAD probe the driver talks to (eval, find, click, key, grab, recordings);
#   iconprobe     which hicolor folder Qt's and KDE's icon lookup take per size and device pixel ratio.
#
# OUT-DIR defaults to $SHOT_PROBE_DIR, else $SHOT_WORK/probe ($SHOT_WORK: $XDG_CACHE_HOME/ferrous-ui-capture), the
# folder driver.py (PROBE_DIR) loads both from. Both use public Qt API only.
# SHOT_BUILD_WRAPPER, when set, is a command the compiler runs under (e.g. a script that limits parallel builds).
set -euo pipefail

H=$(dirname "$(realpath "$0")")
WORK=${SHOT_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/ferrous-ui-capture}
OUT=${1:-${SHOT_PROBE_DIR:-$WORK/probe}}
WRAP=()
if [ -n "${SHOT_BUILD_WRAPPER:-}" ]; then
    read -r -a WRAP <<< "$SHOT_BUILD_WRAPPER"
fi
mkdir -p "$OUT"
QTV=$(pkg-config --modversion Qt6Quick)

SHOT_FLAGS=$(pkg-config --cflags --libs Qt6Quick Qt6Qml Qt6Gui Qt6Core Qt6Concurrent Qt6Network)
ICON_FLAGS="$(pkg-config --cflags --libs Qt6Widgets Qt6Gui Qt6Core) -l:libKF6IconThemes.so.6"

key() {  # key SOURCE FLAGS: what a binary is built from (the source's content, the flags, Qt, the compiler)
    { sha256sum < "$1"; echo "$2"; echo "$QTV"; g++ --version | head -1; } | sha256sum | cut -c1-32
}

built() {  # built BINARY KEY: what is recorded beside a binary built with KEY
    echo "$2 $(sha256sum < "$1" | cut -c1-32)"
}

stale() {  # stale BINARY KEY: missing, built from anything else, or no longer the binary that was built
    [ ! -e "$1" ] || [ "$(cat "$1.key" 2>/dev/null)" != "$(built "$1" "$2")" ]
}

K=$(key "$H/shotprobe.cpp" "-shared -fPIC -O2 -std=c++17 $SHOT_FLAGS")
if stale "$OUT/shotprobe.so" "$K"; then
    # shellcheck disable=SC2086  # pkg-config's flags are meant to split
    "${WRAP[@]}" g++ -shared -fPIC -O2 -std=c++17 "$H/shotprobe.cpp" -o "$OUT/shotprobe.so.tmp" $SHOT_FLAGS
    mv "$OUT/shotprobe.so.tmp" "$OUT/shotprobe.so"
    built "$OUT/shotprobe.so" "$K" > "$OUT/shotprobe.so.key"
    echo "built $OUT/shotprobe.so (Qt $QTV)"
fi
K=$(key "$H/iconprobe.cpp" "-fPIC -O2 -std=c++17 $ICON_FLAGS")
if stale "$OUT/iconprobe" "$K"; then
    # shellcheck disable=SC2086
    "${WRAP[@]}" g++ -fPIC -O2 -std=c++17 "$H/iconprobe.cpp" -o "$OUT/iconprobe.tmp" $ICON_FLAGS
    mv "$OUT/iconprobe.tmp" "$OUT/iconprobe"
    built "$OUT/iconprobe" "$K" > "$OUT/iconprobe.key"
    echo "built $OUT/iconprobe (Qt $QTV)"
fi
