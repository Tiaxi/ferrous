#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks own_directories.sh, the test programs' launcher: each run has a
# temporary directory and XDG directories of its own, made empty in
# $TMPDIR; the command keeps its arguments and exit status; and CTest puts
# the launcher before the tests of the programs built in BUILD_DIR.

set -euo pipefail

USAGE="usage: $(basename "$0") <launcher> <work dir> <build dir> <ctest>"
LAUNCHER="${1:?${USAGE}}"
WORK_DIR="${2:?${USAGE}}"
BUILD_DIR="${3:?${USAGE}}"
CTEST="${4:?${USAGE}}"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

rm -rf -- "${WORK_DIR}"
mkdir -p "${WORK_DIR}/parent"

# Prints the directories it was given, one a line, and fails unless each
# is an empty directory.
cat >"${WORK_DIR}/report.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
for directory in "${TMPDIR}" "${XDG_CONFIG_HOME}" "${XDG_DATA_HOME}" "${XDG_CACHE_HOME}"; do
    [[ -d "${directory}" && -z "$(ls -A "${directory}")" ]] || exit 10
    echo "${directory}"
done
EOF

run_report() {
    TMPDIR="${WORK_DIR}/parent" \
        XDG_CONFIG_HOME=/nonexistent/config \
        XDG_DATA_HOME=/nonexistent/data \
        XDG_CACHE_HOME=/nonexistent/cache \
        bash "${LAUNCHER}" bash "${WORK_DIR}/report.sh"
}

first="$(run_report)" || fail "the first run's directories were not empty directories"
second="$(run_report)" || fail "the second run's directories were not empty directories"
mapfile -t first_dirs <<<"${first}"
mapfile -t second_dirs <<<"${second}"
[[ ${#first_dirs[@]} -eq 4 && ${#second_dirs[@]} -eq 4 ]] || fail "expected four directories: ${first} / ${second}"

declare -A seen=()
for directory in "${first_dirs[@]}" "${second_dirs[@]}"; do
    [[ "${directory}" == "${WORK_DIR}/parent/"* ]] || fail "${directory} is not in the parent \$TMPDIR"
    [[ -z "${seen[${directory}]:-}" ]] || fail "${directory} is shared"
    seen[${directory}]=1
done
[[ "$(dirname "${first_dirs[0]}")" != "$(dirname "${second_dirs[0]}")" ]] \
    || fail "two runs share a root"
# The root is named after the program, to find what a failed test left.
[[ "$(basename "$(dirname "${first_dirs[0]}")")" == bash.* ]] \
    || fail "the root is not named after the program: ${first_dirs[0]}"

# Arguments, the exit status and standard input reach the command.
status=0
output="$(printf 'in' | TMPDIR="${WORK_DIR}/parent" bash "${LAUNCHER}" \
    bash -c 'read -r line; printf "%s|%s|%s" "$1" "$2" "${line}"; exit 7' _ 'a b' '')" || status=$?
[[ ${status} -eq 7 ]] || fail "exit status ${status}, expected 7"
[[ "${output}" == "a b||in" ]] || fail "unexpected output: ${output}"

# Without a command, it fails rather than doing nothing.
if TMPDIR="${WORK_DIR}/parent" bash "${LAUNCHER}" 2>/dev/null; then
    fail "it succeeded without a command"
fi

# CTest runs the test programs through it.
if [[ ! -f "${BUILD_DIR}/CTestTestfile.cmake" ]]; then
    fail "no CTestTestfile.cmake in ${BUILD_DIR}"
fi
launched="$("${CTEST}" --test-dir "${BUILD_DIR}" --show-only=json-v1 -R '^ferrous_playback_clock_tests$')"
grep -q "own_directories.sh" <<<"${launched}" \
    || fail "ferrous_playback_clock_tests does not run through the launcher"

rm -rf -- "${WORK_DIR}"
echo "PASS"
