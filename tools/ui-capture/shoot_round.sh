#!/bin/bash
# A screenshot pack of the Ferrous UI for one round, from a source tree, in one command.
#
#   shoot_round.sh <round> [<source-dir>] [--out DIR] [--only AREA[,AREA...]] [--themes dark,light]
#                  [--sessions S1,S2] [--workers N]   (parallel isolated app sessions; default 4, at most 4)
#                  [--pack-jobs N]   (pack.py's worker processes; default pack.py's own, 2 to 8 by the CPUs)
#                  [--focus SCOPES | --focus-only SCOPES | --census]   (<round>/focus/<scope>/: the small pack of each
#                  scope of focus.py in the comma-separated list, "census" for the census scopes; --focus-only shoots
#                  just the union of their sessions, once; --census is --focus-only census)
#
# <source-dir>: the tree under test (default: the checkout that holds this script). --out DIR: where rounds go
# (default $SHOT_OUT_ROOT, else $SHOT_WORK/rounds). $SHOT_WORK: the work folder (default
# $XDG_CACHE_HOME/ferrous-ui-capture) with the source mirror, build, probes, fixtures and app dirs.
#
# 1. Mirrors <source-dir> into $SHOT_WORK/src (rsync, mtimes kept) and builds it incrementally into
#    $SHOT_WORK/build (Release, no tests, target ferrous), under $SHOT_BUILD_WRAPPER when set.
# 2. Builds the probes (build_probes.sh) when stale; makes the fixtures if missing; rescans the fixture library
#    into the template DB with the new binary (schema-proof).
# 3. Runs every state of states.py (runner.py) under headless KWin in N parallel, fully isolated
#    workers (own dbus-run-session, KWin --virtual, XDG dirs, app), each at nice 10, in a private user and
#    mount namespace where the work folder is at /mnt/ferrous (neutral_ns.sh; SHOT_NEUTRAL=0 runs without).
# 4. Writes MOTION.md, CONTRAST.md, LAYOUT.md, then pack.py: review-sized contact sheets (cropped tiles, one theme
#    per sheet), key-frame motion strips and INDEX.md with the core set first.
# Output: <out>/<round>/, with the focus packs in <round>/focus/<scope>/. A full run wipes the round's previous
# images (BUGS.md is kept). --only/--sessions/--themes make a partial run: it re-shoots just those states into the
# existing round (wiping nothing) and rebuilds the sheets and INDEX.md. See README.md.
set -euo pipefail

usage() { sed -n '2,/^set -euo/{/^#/p}' "$0"; exit 2; }
[ $# -ge 1 ] || usage
case $1 in -h|--help|-*) usage ;; esac
H=$(dirname "$(realpath "$0")")
ROUND=$1
shift
case $ROUND in */*|.|..|"") echo "!!! the round is a name, not a path: $ROUND" >&2; exit 2 ;; esac
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
    SRC=$1
    shift
else
    SRC=$(git -C "$H" rev-parse --show-toplevel)
fi
SRC=$(realpath "$SRC")
[ -f "$SRC/ui/CMakeLists.txt" ] || { echo "!!! $SRC is not a Ferrous source tree (no ui/CMakeLists.txt)" >&2; exit 2; }
WORK=$(realpath -m "${SHOT_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/ferrous-ui-capture}")
OUTROOT=${SHOT_OUT_ROOT:-$WORK/rounds}
ONLY=""
PARTIAL=""
FOCUS_ONLY=""
WORKERS=4
RUNNER_ARGS=()
PACK_ARGS=()
while [ $# -gt 0 ]; do
    if [ "$1" = --census ]; then  # (the one option without a value)
        export SHOT_FOCUS=census SHOT_FOCUS_ONLY=1; FOCUS_ONLY=1; shift; continue
    fi
    [ $# -ge 2 ] || { echo "option $1 needs a value" >&2; exit 2; }
    case $1 in
        --out) OUTROOT=$2; shift 2 ;;
        --only) ONLY=$2; PARTIAL=1; RUNNER_ARGS+=(--only "$2"); shift 2 ;;
        --themes|--sessions) PARTIAL=1; RUNNER_ARGS+=("$1" "$2"); shift 2 ;;
        --workers) WORKERS=$(( $2 > 12 ? 12 : $2 )); shift 2 ;;
        # fewer pack workers use less memory (see README.md); without it, pack.py picks its own count
        --pack-jobs)
            [[ $2 =~ ^[1-9][0-9]*$ ]] || { echo "--pack-jobs needs a whole number of 1 or more (got '$2')" >&2; exit 2; }
            PACK_ARGS=(--jobs "$2"); shift 2 ;;
        # the focus packs (focus.py) after the whole pack; --focus-only also shoots only the scopes' sessions
        --focus) export SHOT_FOCUS=$2; shift 2 ;;
        --focus-only) export SHOT_FOCUS=$2 SHOT_FOCUS_ONLY=1; FOCUS_ONLY=1; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
OUTROOT=$(realpath -m "$OUTROOT")

if [ -n "${SHOT_FOCUS:-}" ]; then  # every scope must exist (loudly, before building)
    # focus.py SPEC prints the scopes (in order, each once; "census" expanded) and the union of their sessions
    FOCUS_RESOLVED=$(python3 "$H/focus.py" "$SHOT_FOCUS") || { echo "!!! bad focus scope list '$SHOT_FOCUS'" >&2; exit 2; }
    SHOT_FOCUS=$(sed -n 1p <<< "$FOCUS_RESOLVED")
    FOCUS_SESSIONS=$(sed -n 2p <<< "$FOCUS_RESOLVED")
    echo "focus scopes: $SHOT_FOCUS (sessions: $FOCUS_SESSIONS)"
    if [ -n "$FOCUS_ONLY" ]; then
        PARTIAL=1
        RUNNER_ARGS+=(--sessions "$FOCUS_SESSIONS" --focus "$SHOT_FOCUS")
    fi
fi
# The icon A/B (SHOT_ICON_AB=1) needs variant B's folder: fail before building, never a sheet without its B row.
if [ "${SHOT_ICON_AB:-}" = 1 ]; then
    [ -n "${SHOT_ICON_B:-}" ] || { echo "!!! SHOT_ICON_AB=1 needs SHOT_ICON_B (the icon tree to compare)" >&2; exit 2; }
    ICON_B=$(realpath -m "$SHOT_ICON_B")
    [ "$(basename "$ICON_B")" = hicolor ] || ICON_B=$ICON_B/hicolor
    if [ ! -d "$ICON_B" ]; then
        echo "!!! SHOT_ICON_AB=1 but there is no icon B tree at $ICON_B (set SHOT_ICON_B)" >&2
        exit 2
    fi
fi
# SHOT_BUILD_WRAPPER: an optional command the compiles run under (e.g. a script that holds a build slot, so a
# capture's build never overlaps another build on a machine short of memory).
WRAP=()
if [ -n "${SHOT_BUILD_WRAPPER:-}" ]; then
    read -r -a WRAP <<< "$SHOT_BUILD_WRAPPER"
fi
# a focus-only run works in its own folder (<round>-focus: shots, strips, timings, its app and TMP dirs); the full
# run of the same round packs <round>/. So the two can be started together (see the run lock below). Either builds
# the focus packs asked of it in its focus-build/ and, only when the whole run passed, publishes each to
# <round>/focus/<scope>/ (FOCUS.md, FOCUS-FACTS.md, the images).
OUTNAME=$ROUND
[ -n "$FOCUS_ONLY" ] && OUTNAME=$ROUND-focus
OUT=$OUTROOT/$OUTNAME
# TMPDIR on the resolved path (WORK is resolved): a temporary path the app compares with another must not differ
# by a symlink. Inside the work folder, so the app sees it at the neutral path too.
export TMPDIR=$WORK/tmp-$OUTNAME
export SHOT_WORK=$WORK
export LC_ALL=C.UTF-8
export SHOT_KWIN=1
# SHOT_BUILD_TAG: a separate source mirror and build dir ($SHOT_WORK/src$TAG, build$TAG), so a
# test of the harness never shares a binary with a round someone else is shooting.
TAG=${SHOT_BUILD_TAG:-}
# The tag becomes part of two folder names the mirror step deletes inside (rsync --delete), so it may only be a
# plain suffix: a "/" or ".." would point that deletion outside the work folder.
if [[ ! "$TAG" =~ ^[A-Za-z0-9_-]*$ ]]; then
    echo "!!! SHOT_BUILD_TAG must be letters, digits, '-' or '_' (got '$TAG')" >&2
    exit 2
fi
# One run at a time per work folder, whatever its build tag, from the mirror and build through the shooting and
# packing. Every tag shares the probes (probe/, and their build temporaries), the fixtures, the template DB and
# the setup app (run/template, run/setup), the GStreamer registries and the bus sockets, so a per-tag lock let
# two runs rebuild or rescan them under each other. One lock also keeps a second capture off the machine while
# this one records (latency, blink and "neither" are timing measurements). A second command waits here, in the
# order started, and needs no build once the first has built the same source; runs that must go side by side
# need separate work folders (SHOT_WORK).
mkdir -p "$WORK"
# Every app gets fakeaudiosink; the app falls back to autoaudiosink (a real device) when it cannot create it. So
# no capture starts unless GStreamer can create and run it (driver.py checks again before each app it starts).
mkdir -p "$WORK/run"
if ! GST_REGISTRY="$WORK/run/gst-registry-sinkcheck.bin" \
        gst-launch-1.0 -q audiotestsrc num-buffers=1 ! fakeaudiosink >/dev/null 2>&1; then
    echo "!!! GStreamer cannot create fakeaudiosink, so the app would play to a real audio device: install" \
        "gstreamer1-plugins-bad-free (Fedora) and gst-launch-1.0 (gstreamer1)" >&2
    exit 2
fi
exec 9>"$WORK/.run.lock"
if ! flock -n 9; then
    echo "another shoot_round holds the run lock of $WORK ($(cat "$WORK/.run.holder" 2>/dev/null || echo '?')): waiting"
    flock 9
fi
echo "$OUTNAME${TAG:+ (tag $TAG)} since $(date +%T) (pid $$)" > "$WORK/.run.holder"
CSRC=$WORK/src$TAG
CBUILD=$WORK/build$TAG
export SHOT_APP=$CBUILD/ferrous
export SHOT_OUT=$OUT/shots
export SHOT_SRC=$SRC
export FERROUS_GST_OUTPUT_SINK=fakeaudiosink
unset SHOT_SOFTWARE
mkdir -p "$TMPDIR" "$OUT"
START=$(date +%s.%N)
LOG=$OUT/run.log
mkdir -p "$OUT/timings"
PHASES=$OUT/timings/phases.tsv
: > "$PHASES"
T_PREV=$START
phase() {  # phase NAME: seconds since the previous phase
    local now; now=$(date +%s.%N)
    printf '%s\t%.1f\n' "$1" "$(echo "$now - $T_PREV" | bc)" | tee -a "$PHASES"
    T_PREV=$now
}
exec > >(tee -a "$LOG") 2>&1
echo "=== shoot_round $ROUND from $SRC ($(git -C "$SRC" log -1 --format=%h 2>/dev/null || echo '?')) at $(date)"

# 0. The source's commit and branch, recorded before mirroring (the mirror has no .git) for INDEX.md.
python3 - "$SRC" "$OUT/source.json" <<'PYEOF'
import json, subprocess, sys
src, out = sys.argv[1], sys.argv[2]
git = lambda *a: subprocess.run(["git", "-C", src, *a], capture_output=True, text=True).stdout.strip()
json.dump(dict(path=src, rev=git("rev-parse", "--short", "HEAD") or "?", branch=git("rev-parse", "--abbrev-ref", "HEAD") or "?",
               subject=git("log", "-1", "--format=%s"), dirty=bool(git("status", "--porcelain"))), open(out, "w"), indent=1)
PYEOF
echo "source: $(python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); print(d["rev"], d["branch"], "(dirty)" if d["dirty"] else "")' "$OUT/source.json")"

# 1. Build: always run the incremental build (a no-op when the binary is current). Gating it on rsync's
# change list let a sync whose build then failed leave a stale binary that the next run, seeing nothing
# new to sync, captured anyway (an older binary under a newer source).
mkdir -p "$CSRC" "$CBUILD"
# The mirror deletes inside $CSRC, so it must be the work folder's own directory, not a link out of it.
if [ -L "$CSRC" ] || [ "$(realpath "$CSRC")" != "$(realpath "$WORK")/src$TAG" ]; then
    echo "!!! $CSRC is not a plain folder inside $WORK; refusing to mirror into it" >&2
    exit 2
fi
rsync -a --delete --exclude .git --exclude /target --exclude '/ui/build*' --exclude /build "$SRC/" "$CSRC/"
phase sync
"${WRAP[@]}" bash -c '
    set -e
    if [ ! -f "$2/CMakeCache.txt" ]; then
        cmake -S "$1/ui" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    fi
    cmake --build "$2" --target ferrous' build "$CSRC" "$CBUILD"
phase build

# 2. Probe, fixtures, template DB.
# one probe folder for the build here and for driver.py (shotprobe.so) and states.py (iconprobe) in the runs
export SHOT_PROBE_DIR=$(realpath -m "${SHOT_PROBE_DIR:-$WORK/probe}")
"$H/build_probes.sh" "$SHOT_PROBE_DIR"
LIB=$WORK/fixtures/library
if [ ! -d "$LIB" ]; then  # made aside and renamed in, so an interrupted run never leaves half a library
    rm -rf "$LIB.partial"
    python3 "$H/make_fixtures.py" "$LIB.partial"
    mv "$LIB.partial" "$LIB"
fi
[ -f "$LIB/Orchid Static/2019 - Loose Ends/02 - Cut Tape.flac" ] || python3 "$H/make_extra_fixtures.py" "$LIB"
[ -f "$LIB/The Quiet Meridian/2005 - Small Hours/.cover-illustrated" ] || python3 "$H/make_cover_art.py" "$LIB"
[ -f "$(dirname "$LIB")/broadband/04 - Moving Levels.flac" ] || python3 "$H/make_broadband_fixtures.py" "$(dirname "$LIB")/broadband"
[ -f "$(dirname "$LIB")/standin/03 - Same Name.flac" ] || python3 "$H/make_standin_fixtures.py" "$(dirname "$LIB")/standin"
[ -f "$(dirname "$LIB")/library-large/.count" ] || python3 "$H/make_large_library.py" "$(dirname "$LIB")/library-large"
[ -d "$WORK/fixtures/library2/Pale Harbor" ] || python3 "$H/make_library2.py" "$WORK/fixtures/library2"
phase probe+fixtures
# The app runs in a private mount namespace where the work folder is at /mnt/ferrous (neutral_ns.sh), so the
# paths it shows name no home folder; SHOT_NEUTRAL=0 runs it at the real paths (the pack then paints them over).
NEUTRAL=${SHOT_NEUTRAL:-1}
NS=""
if [ "$NEUTRAL" = 1 ]; then
    NS="$H/neutral_ns.sh"
    "$NS" true || { echo "!!! neutral_ns.sh cannot make its namespace (user namespaces off?); SHOT_NEUTRAL=0 runs without" >&2; exit 6; }
fi
# The template library DB: rescanned only when the Rust sources (the schema) or the fixtures changed.
# (and the library's resolved path: the DB stores absolute paths, and the queue is loaded by that path)
KEY=$( (cd "$CSRC" && find src -name '*.rs' -type f -print0 | sort -z | xargs -0 sha1sum; sha1sum Cargo.lock;
        realpath "$WORK/fixtures/library"; echo "neutral=$NEUTRAL"; find "$WORK/fixtures/library" -type f -printf '%P %s\n' | sort) | sha1sum | cut -c1-16)
if [ "$(cat "$WORK/run/template/key" 2>/dev/null)" != "$KEY" ] || [ ! -f "$WORK/run/template/library.sqlite3" ]; then
    SHOT_RUN_PREFIX=setup nice -n 10 $NS python3 "$H/setup_db_round.py"
    echo "$KEY" > "$WORK/run/template/key"
else
    echo "template DB current (key $KEY)"
fi
phase template-db

# 3. Shoot.
if [ -z "$PARTIAL" ] || [ -n "$FOCUS_ONLY" ]; then  # (a focus-only run starts its own folder afresh)
    rm -rf "$WORK/run/$OUTNAME"-w[0-9]-*  # this run's previous per-worker app dirs
    rm -rf "$OUT/focus-build"  # (not <round>/focus/: the published packs stay until new ones pass)
    rm -rf "$OUT/shots" "$OUT/index" "$OUT/strips-raw" "$OUT/strips" "$OUT/sheets" "$OUT/icons-raw" \
        "$OUT/failures.txt" "$OUT"/focus_owners_*.txt "$OUT"/worker*.log "$OUT/contrast" "$OUT/CONTRAST.md" "$OUT/layout" "$OUT/LAYOUT.md" "$OUT/layout.json" \
        "$OUT/facts.jsonl" "$OUT/floors.jsonl" "$OUT/closeup-raw" "$OUT"/a11y*.jsonl \
        "$OUT"/popups_*.txt "$OUT"/rolling_*.txt "$OUT"/treekeys_*.txt  # (appended by states; a cut run left them)
fi
mkdir -p "$OUT/shots"
rm -f "$OUT/failures.txt"
rm -f "$OUT"/worker*.log
$NS python3 "$H/runner.py" --workers "$WORKERS" "${RUNNER_ARGS[@]}"
phase shooting
cat "$OUT"/worker*.log 2>/dev/null | grep -E "!!" || true

# 4. Package.
python3 "$H/motion_inventory.py" "$OUT" "$SRC"
phase motion-inventory
CONTRAST_STATUS=0
python3 "$H/contrast.py" "$OUT" || CONTRAST_STATUS=$?
phase contrast
nice -n 10 python3 "$H/layout.py" "$OUT"
phase layout
PACK_STATUS=0
SHOT_PARTIAL="$PARTIAL" nice -n 10 python3 "$H/pack.py" "$OUT" "$SRC" "${PACK_ARGS[@]}" || PACK_STATUS=$?
phase pack
# publish the focus packs to <round>/focus/<scope>/ when the whole run passed (no failed check, measurement or
# state): every pack copied aside first, then each renamed in. The round's packs of other scopes stay.
if [ -n "${SHOT_FOCUS:-}" ] && [ "$PACK_STATUS" -eq 0 ] && [ "$CONTRAST_STATUS" -eq 0 ] && [ ! -s "$OUT/failures.txt" ]; then
    IFS=, read -r -a SCOPE_LIST <<< "$SHOT_FOCUS"
    for SC in "${SCOPE_LIST[@]}"; do
        [ -f "$OUT/focus-build/$SC/FOCUS.md" ] || { echo "!!! focus pack $SC was not written" >&2; PACK_STATUS=3; }
    done
    if [ "$PACK_STATUS" -eq 0 ]; then
        FDEST=$OUTROOT/$ROUND/focus
        STAGED=$FDEST/.staged
        rm -rf "$STAGED"
        mkdir -p "$STAGED"
        for SC in "${SCOPE_LIST[@]}"; do  # (a failed copy stops the script here, with nothing replaced)
            cp -a "$OUT/focus-build/$SC" "$STAGED/$SC"
            cp -p "$OUT/source.json" "$STAGED/$SC/source.json"
        done
        # (a pack of the flat layout before per-scope folders: its files go, its scope is unknown)
        rm -f "$FDEST/FOCUS.md" "$FDEST/FOCUS-FACTS.md" "$FDEST/source.json" "$FDEST"/F[0-9][0-9]_*.png
        for SC in "${SCOPE_LIST[@]}"; do
            rm -rf "${FDEST:?}/$SC"
            mv "$STAGED/$SC" "$FDEST/$SC"
            echo "focus pack published: $FDEST/$SC/FOCUS.md ($(ls "$FDEST/$SC"/F[0-9][0-9]_*.png 2>/dev/null | wc -l) images)"
        done
        rmdir "$STAGED"
    fi
fi
if [ ! -f "$OUT/BUGS.md" ]; then
    printf '# Non-visual bugs noticed while driving the app (%s)\n\nNone recorded yet.\n' "$ROUND" > "$OUT/BUGS.md"
fi
# Every shot must come from the OpenGL renderer.
python3 - "$OUT" <<'EOF'
import glob, json, sys
bad = [e["file"] for p in glob.glob(sys.argv[1] + "/index/*.json") for e in json.load(open(p)) if e.get("renderer") != 3]
print("renderer check:", "all OpenGL (3)" if not bad else f"NOT OpenGL: {bad}")
EOF
echo "shots: $(ls "$OUT/shots" | wc -l), sheets: $(ls "$OUT/sheets" | wc -l), strips: $(ls "$OUT/strips"/*.png 2>/dev/null | wc -l)"
echo "failures: $(wc -l < "$OUT/failures.txt" 2>/dev/null || echo 0)"
printf 'total\t%.1f\n' "$(echo "$(date +%s.%N) - $START" | bc)" | tee -a "$PHASES"
echo "=== done: $OUT/INDEX.md"
if [ "$PACK_STATUS" -ne 0 ]; then  # e.g. 3: a dialog shot is not of the dialog its state names
    echo "!!! pack.py failed ($PACK_STATUS): CHECKS FAILED (a dialog shot, a missing core image) or another packaging error; see INDEX.md" >&2
    exit "$PACK_STATUS"
fi
if [ "$CONTRAST_STATUS" -ne 0 ]; then  # loudly: a measurement that cannot be trusted
    echo "!!! contrast.py failed ($CONTRAST_STATUS); CONTRAST.md is incomplete" >&2
    exit 5
fi
if [ -s "$OUT/failures.txt" ]; then  # loudly: a state that failed is missing from the pack
    echo "!!! $(wc -l < "$OUT/failures.txt") state(s) failed; see $OUT/failures.txt" >&2
    exit 4
fi
