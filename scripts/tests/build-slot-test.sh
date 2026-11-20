#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that scripts/build-slot.sh runs no more commands at once than it has
# slots, passes the command's exit status on, gives a slot back when the
# command exits even if it leaves a process running, and sets the job limits,
# compiler cache and niceness it documents.
# Usage: build-slot-test.sh <absolute scratch work dir>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_SLOT="${SCRIPT_DIR}/../build-slot.sh"
WORK_DIR="${1:?usage: $(basename "$0") <work dir>}"
if [[ "${WORK_DIR}" != /* ]]; then
    echo "The work dir must be an absolute path." >&2
    exit 1
fi

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

rm -rf -- "${WORK_DIR}"
mkdir -p "${WORK_DIR}/bin"
log="${WORK_DIR}/log"
: >"${log}"

# The script's settings come from each check, not from the caller.
unset BUILD_SLOTS BUILD_SLOT_JOBS BUILD_SLOT_NICE
# Every run uses locks of its own, never the user's shared ones.
export BUILD_SLOT_DIR="${WORK_DIR}/locks"
slot() {
    bash "${BUILD_SLOT}" "$@"
}

# Waits up to 30 s for a file, so that a broken script fails instead of
# hanging the test.
wait_for() {
    local i
    for ((i = 0; i < 300; i++)); do
        [[ -e "$1" ]] && return 0
        sleep 0.1
    done
    return 1
}

# A command that says it started, then holds its slot until it is released.
cat >"${WORK_DIR}/bin/hold" <<'SH'
#!/usr/bin/env bash
echo "$1 start" >>"${LOG}"
touch "${MARKS}/$1.started"
for ((i = 0; i < 300; i++)); do
    [[ -e "${MARKS}/$1.release" ]] && break
    sleep 0.1
done
echo "$1 end" >>"${LOG}"
SH
chmod +x "${WORK_DIR}/bin/hold"
export LOG="${log}" MARKS="${WORK_DIR}"

# No command: usage error.
status=0
slot 2>/dev/null || status=$?
[[ "${status}" == 2 ]] || fail "no command exited ${status}, not 2"
status=0
BUILD_SLOTS=0 slot true 2>/dev/null || status=$?
[[ "${status}" == 2 ]] || fail "BUILD_SLOTS=0 exited ${status}, not 2"

# The command's exit status comes through.
status=0
slot bash -c 'exit 7' || status=$?
[[ "${status}" == 7 ]] || fail "exit status ${status}, not 7"

# One slot: a second command, run with the environment in the array named
# by $2 (the first with the one named by $1), waits until the first has
# finished. With $3 set, the first script gets a TERM of its own while its
# command runs, and still keeps the slot until the command exits.
one_slot_waits() {
    local -n first_env="$1" second_env="$2"
    local term="${3:-}" first second
    : >"${log}"
    rm -f -- "${WORK_DIR}"/[ab].started "${WORK_DIR}"/[ab].release
    env "${first_env[@]}" BUILD_SLOTS=1 bash "${BUILD_SLOT}" "${WORK_DIR}/bin/hold" a &
    first=$!
    wait_for "${WORK_DIR}/a.started" || fail "the first command never started"
    if [[ -n "${term}" ]]; then
        kill -TERM "${first}"
    fi
    env "${second_env[@]}" BUILD_SLOTS=1 bash "${BUILD_SLOT}" "${WORK_DIR}/bin/hold" b 2>"${WORK_DIR}/b.err" &
    second=$!
    touch "${WORK_DIR}/b.release"
    # Longer than the script's poll interval, so a second command that does
    # not wait would have started by now.
    sleep 3
    [[ ! -e "${WORK_DIR}/b.started" ]] || fail "the second command ran beside the first in one slot ($*)"
    grep -q 'build slots are busy' "${WORK_DIR}/b.err" || fail "the waiting command did not say it waits ($*)"
    touch "${WORK_DIR}/a.release"
    wait "${first}" || true
    wait "${second}"
    local expected=$'a start\na end\nb start\nb end'
    [[ "$(cat "${log}")" == "${expected}" ]] || fail "one slot ran ($*): $(tr '\n' ',' <"${log}")"
}
same_env=()
one_slot_waits same_env same_env
one_slot_waits same_env same_env term

# The default lock directory is in the account's home directory, so runs
# that point HOME and XDG_CACHE_HOME at directories of their own share it.
cat >"${WORK_DIR}/bin/getent" <<SH
#!/bin/sh
echo "builder:x:\$(id -u):\$(id -g)::${WORK_DIR}/account:/bin/sh"
SH
chmod +x "${WORK_DIR}/bin/getent"
own_dirs_1=(-u BUILD_SLOT_DIR PATH="${WORK_DIR}/bin:${PATH}" HOME="${WORK_DIR}/home1" XDG_CACHE_HOME="${WORK_DIR}/cache1")
own_dirs_2=(-u BUILD_SLOT_DIR PATH="${WORK_DIR}/bin:${PATH}" HOME="${WORK_DIR}/home2" XDG_CACHE_HOME="${WORK_DIR}/cache2")
one_slot_waits own_dirs_1 own_dirs_2
[[ -e "${WORK_DIR}/account/.cache/build-slot/slot1" ]] || fail "the default slots are not in the account's home directory"

# Two slots: two commands run at once. Each waits for the other to start.
cat >"${WORK_DIR}/bin/meet" <<'SH'
#!/usr/bin/env bash
touch "${MARKS}/$1.here"
for ((i = 0; i < 300; i++)); do
    [[ -e "${MARKS}/$2.here" ]] && exit 0
    sleep 0.1
done
exit 1
SH
chmod +x "${WORK_DIR}/bin/meet"
BUILD_SLOTS=2 slot "${WORK_DIR}/bin/meet" c d &
first=$!
BUILD_SLOTS=2 slot "${WORK_DIR}/bin/meet" d c &
second=$!
wait "${first}" || fail "two slots did not run two commands at once"
wait "${second}" || fail "two slots did not run two commands at once"

# A process the command leaves running does not keep the slot.
BUILD_SLOTS=1 slot bash -c 'sleep 60 >/dev/null 2>&1 & echo $! >"${MARKS}/left.pid"'
left="$(cat "${WORK_DIR}/left.pid")"
BUILD_SLOTS=1 slot touch "${WORK_DIR}/after-left" &
after=$!
freed=0
wait_for "${WORK_DIR}/after-left" && freed=1
kill "${left}" 2>/dev/null || true
wait "${after}"
[[ "${freed}" == 1 ]] || fail "a process left running kept the slot"

# Job limits: set from BUILD_SLOT_JOBS, unless already set; untouched without it.
show_env='echo "${CMAKE_BUILD_PARALLEL_LEVEL-unset} ${CARGO_BUILD_JOBS-unset}"'
got="$(env -u CMAKE_BUILD_PARALLEL_LEVEL -u CARGO_BUILD_JOBS BUILD_SLOT_JOBS=3 bash "${BUILD_SLOT}" bash -c "${show_env}")"
[[ "${got}" == "3 3" ]] || fail "BUILD_SLOT_JOBS=3 gave '${got}'"
got="$(env -u CMAKE_BUILD_PARALLEL_LEVEL CARGO_BUILD_JOBS=5 BUILD_SLOT_JOBS=3 bash "${BUILD_SLOT}" bash -c "${show_env}")"
[[ "${got}" == "3 5" ]] || fail "an explicit CARGO_BUILD_JOBS was replaced: '${got}'"
got="$(env -u CMAKE_BUILD_PARALLEL_LEVEL -u CARGO_BUILD_JOBS -u BUILD_SLOT_JOBS bash "${BUILD_SLOT}" bash -c "${show_env}")"
[[ "${got}" == "unset unset" ]] || fail "job limits were set without BUILD_SLOT_JOBS: '${got}'"

# ccache, when installed: the launchers default to it, and empty ones stay empty.
printf '#!/bin/sh\n' >"${WORK_DIR}/bin/ccache"
chmod +x "${WORK_DIR}/bin/ccache"
show_launcher='echo "[${CMAKE_CXX_COMPILER_LAUNCHER-unset}]"'
got="$(env -u CMAKE_C_COMPILER_LAUNCHER -u CMAKE_CXX_COMPILER_LAUNCHER PATH="${WORK_DIR}/bin:${PATH}" \
    bash "${BUILD_SLOT}" bash -c "${show_launcher}")"
[[ "${got}" == "[ccache]" ]] || fail "the compiler launcher was '${got}', not ccache"
got="$(CMAKE_CXX_COMPILER_LAUNCHER='' PATH="${WORK_DIR}/bin:${PATH}" slot bash -c "${show_launcher}")"
[[ "${got}" == "[]" ]] || fail "an empty compiler launcher became '${got}'"

# Niceness: the command runs BUILD_SLOT_NICE (default 10) above the caller.
base="$(nice)"
want=$((base + 10 > 19 ? 19 : base + 10))
got="$(slot nice)"
[[ "${got}" == "${want}" ]] || fail "niceness ${got}, not ${want}"
want=$((base + 3 > 19 ? 19 : base + 3))
got="$(BUILD_SLOT_NICE=3 slot nice)"
[[ "${got}" == "${want}" ]] || fail "BUILD_SLOT_NICE=3 gave niceness ${got}, not ${want}"

rm -rf -- "${WORK_DIR}"
echo "build-slot: ok"
