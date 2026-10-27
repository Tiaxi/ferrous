#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that scripts/run-ui.sh deletes app state only when confirmed, and only
# Ferrous's own paths under the XDG directories it is given.
# Usage: run-ui-cleanup-test.sh <absolute scratch work dir>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_UI="${SCRIPT_DIR}/../run-ui.sh"
WORK_DIR="${1:?usage: $(basename "$0") <work dir>}"
if [[ "${WORK_DIR}" != /* ]]; then
    echo "The work dir must be an absolute path." >&2
    exit 1
fi

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

make_state() {
    rm -rf -- "${WORK_DIR}"
    mkdir -p "${WORK_DIR}/data/ferrous" "${WORK_DIR}/config/ferrous" \
        "${WORK_DIR}/cache/ferrous/thumbnails/library" "${WORK_DIR}/cache/ferrous/embedded_covers" \
        "${WORK_DIR}/home"
    touch "${WORK_DIR}/data/ferrous/library.sqlite3" "${WORK_DIR}/data/ferrous/library.sqlite3-wal" \
        "${WORK_DIR}/data/ferrous/diagnostics.log" "${WORK_DIR}/config/ferrous/session.json" \
        "${WORK_DIR}/config/ferrous/settings.txt" "${WORK_DIR}/cache/ferrous/thumbnails/library/a.png" \
        "${WORK_DIR}/cache/ferrous/embedded_covers/b.jpg"
}

run_ui() {
    HOME="${WORK_DIR}/home" \
        XDG_DATA_HOME="${WORK_DIR}/data" \
        XDG_CONFIG_HOME="${WORK_DIR}/config" \
        XDG_CACHE_HOME="${WORK_DIR}/cache" \
        bash "${RUN_UI}" --no-configure --no-build --no-run "$@"
}

state_files() {
    (cd "${WORK_DIR}" && find data config cache -type f | sort)
}

# Without a terminal or --yes, nothing is deleted.
make_state
before="$(state_files)"
if run_ui --nuke-all --clear-diagnostics-log </dev/null >"${WORK_DIR}/out.log" 2>&1; then
    fail "cleanup ran without confirmation"
fi
[[ "$(state_files)" == "${before}" ]] || fail "unconfirmed cleanup deleted files"
grep -q -- '--yes' "${WORK_DIR}/out.log" || fail "the refusal does not mention --yes"

# With --yes, exactly the named paths go.
run_ui --nuke-all --clear-diagnostics-log --yes </dev/null >"${WORK_DIR}/out.log" 2>&1 \
    || fail "confirmed cleanup failed: $(cat "${WORK_DIR}/out.log")"
expected="$(printf '%s\n' cache/ferrous/embedded_covers/b.jpg config/ferrous/settings.txt)"
[[ "$(state_files)" == "${expected}" ]] || fail "unexpected files after cleanup: $(state_files)"
[[ ! -e "${WORK_DIR}/cache/ferrous/thumbnails/library" ]] || fail "the thumbnail cache is left"
if grep -q '/tmp' "${WORK_DIR}/out.log"; then
    fail "cleanup touched /tmp: $(cat "${WORK_DIR}/out.log")"
fi

# Nothing to delete needs no confirmation.
run_ui --nuke-session </dev/null >/dev/null 2>&1 || fail "a cleanup with nothing to delete failed"

# A path of the wrong kind, which Ferrous did not create, stays.
make_state
rm -f -- "${WORK_DIR}/config/ferrous/session.json"
mkdir -p "${WORK_DIR}/config/ferrous/session.json"
touch "${WORK_DIR}/config/ferrous/session.json/keep"
run_ui --nuke-session --yes </dev/null >/dev/null 2>&1 || fail "cleanup of a missing session failed"
[[ -e "${WORK_DIR}/config/ferrous/session.json/keep" ]] || fail "a folder at the session path was deleted"

rm -rf -- "${WORK_DIR}"
echo "run-ui.sh cleanup asks first and deletes only Ferrous's paths"
