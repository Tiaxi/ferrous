#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs scripts/stress-tests.sh on a scratch copy with stand-in cargo and
# stress-ng, and checks its cycle of parallelism and load, its summary, its
# exit status, and that it stops its load and removes its directories.
# Usage: stress-tests-test.sh <absolute scratch work dir>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
repo="${WORK_DIR}/repo"
bin="${WORK_DIR}/bin"
mkdir -p "${repo}/scripts" "${bin}"
cp "${SCRIPT_DIR}/../stress-tests.sh" "${SCRIPT_DIR}/../load-build-env.sh" "${repo}/scripts/"

# The stand-in load says it runs, once its stop handler is in place, by
# writing to this pipe, which the stand-in cargo of the loaded run reads
# before it ends: the run then overlaps the load, and stopping the load
# finds the handler.
mkfifo "${WORK_DIR}/load-ready"

# Records each run's arguments and directories, and whether the load runs;
# waits for the load in run FAKE_LOADED_CALL; fails when FAKE_CARGO_FAIL is
# set, as a test binary with a failed test does.
cat >"${bin}/cargo" <<'SH'
#!/usr/bin/env bash
call=$(($(grep -c '^args=' "${STUB_LOG}") + 1))
if [[ "${call}" == "${FAKE_LOADED_CALL:-}" ]]; then
    # Bounded only so that a script that never starts the load fails.
    timeout 60 cat "${STUB_DIR}/load-ready" >/dev/null || exit 70
fi
load=off
[[ -e "${STUB_DIR}/load-running" ]] && load=on
echo "args=$* tmp=${TMPDIR} config=${XDG_CONFIG_HOME} load=${load}" >>"${STUB_LOG}"
if [[ -n "${FAKE_CARGO_FAIL:-}" ]]; then
    echo "test some::module::a_test ... FAILED"
    exit 101
fi
echo "test result: ok."
SH
# Runs until it is stopped, and says so.
cat >"${bin}/stress-ng" <<'SH'
#!/usr/bin/env bash
idle=""
stopped() {
    [[ -n "${idle}" ]] && kill "${idle}" 2>/dev/null
    rm -f "${STUB_DIR}/load-running"
    echo "load stopped $*" >>"${STUB_LOG}"
    exit 0
}
trap 'stopped' TERM
echo "${$}" >"${STUB_DIR}/load-running"
echo "load started $*" >>"${STUB_LOG}"
echo ready >"${STUB_DIR}/load-ready"
sleep infinity &
idle=$!
wait "${idle}"
SH
chmod +x "${bin}/cargo" "${bin}/stress-ng"

stress() {
    PATH="${bin}:${PATH}" STUB_LOG="${WORK_DIR}/stub.log" STUB_DIR="${WORK_DIR}" \
        FERROUS_STRESS_DIR="${WORK_DIR}/out" \
        bash "${repo}/scripts/stress-tests.sh" --no-build "$@"
}

# Three runs of the mock suite: 1 thread, libtest's default, 32 threads,
# the third under load.
: >"${WORK_DIR}/stub.log"
FAKE_LOADED_CALL=3 stress --suite mock --runs 3 --load-workers 2 >"${WORK_DIR}/pass.out" 2>&1 \
    || fail "passing runs failed: $(cat "${WORK_DIR}/pass.out")"
summary="${WORK_DIR}/out/summary.txt"
[[ "$(grep -c '^PASS mock run' "${summary}")" -eq 3 ]] || fail "summary: $(cat "${summary}")"
grep -q '^PASS mock run 1 threads=1 load=0 ' "${summary}" || fail "run 1: $(cat "${summary}")"
grep -q '^PASS mock run 2 threads=default load=0 ' "${summary}" || fail "run 2: $(cat "${summary}")"
grep -q '^PASS mock run 3 threads=32 load=1 ' "${summary}" || fail "run 3: $(cat "${summary}")"
mapfile -t calls < <(grep '^args=' "${WORK_DIR}/stub.log")
[[ ${#calls[@]} -eq 3 ]] || fail "cargo calls: ${calls[*]}"
[[ "${calls[0]}" == *"test --no-fail-fast --no-default-features --features cli -- --test-threads=1 "* ]] \
    || fail "run 1 arguments: ${calls[0]}"
[[ "${calls[1]}" == *"features cli -- tmp="* ]] || fail "run 2 arguments: ${calls[1]}"
[[ "${calls[2]}" == *"--test-threads=32 "* ]] || fail "run 3 arguments: ${calls[2]}"
# Throwaway directories under the log directory, removed afterwards.
[[ "${calls[0]}" == *"tmp=${WORK_DIR}/out/state."*"/run/tmp config=${WORK_DIR}/out/state."* ]] \
    || fail "run directories: ${calls[0]}"
compgen -G "${WORK_DIR}/out/state.*" >/dev/null && fail "the state directory was left behind"
# The load ran during the third run only, and was stopped with it: the
# third run saw it, after the handshake, and it was gone before the script
# ended.
[[ "${calls[0]}" == *" load=off" && "${calls[1]}" == *" load=off" ]] \
    || fail "load during the first runs: ${calls[*]}"
[[ "${calls[2]}" == *" load=on" ]] || fail "no load during the third run: ${calls[2]}"
[[ "$(grep -c '^load started --quiet --cpu 2$' "${WORK_DIR}/stub.log")" -eq 1 ]] \
    || fail "load: $(cat "${WORK_DIR}/stub.log")"
grep -q '^load stopped' "${WORK_DIR}/stub.log" || fail "the load was not stopped"
[[ ! -e "${WORK_DIR}/load-running" ]] || fail "the load still runs"
[[ -f "${WORK_DIR}/out/mock-3.log" ]] || fail "no log of run 3"

# A failed run fails the script and names the test; --first continues the
# cycle, so run 5 uses libtest's default.
rm -rf -- "${WORK_DIR}/out"
: >"${WORK_DIR}/stub.log"
if FAKE_CARGO_FAIL=1 stress --suite gst --runs 1 --first 5 >"${WORK_DIR}/fail.out" 2>&1; then
    fail "a failed run passed"
fi
summary="${WORK_DIR}/out/summary.txt"
grep -q '^FAIL gst run 5 threads=default load=0 .* status=101 ' "${summary}" \
    || fail "failure summary: $(cat "${summary}")"
grep -q '^test some::module::a_test \.\.\. FAILED$' "${summary}" \
    || fail "the failed test is not named: $(cat "${summary}")"
grep -q -- '--features cli,gst --' "${WORK_DIR}/stub.log" || fail "gst features: $(cat "${WORK_DIR}/stub.log")"

# Unknown suites and options are refused.
if stress --suite nope >/dev/null 2>&1; then
    fail "an unknown suite was accepted"
fi
if stress --runs x >/dev/null 2>&1; then
    fail "a run count that is not a number was accepted"
fi

echo "stress-tests.sh checks passed"
