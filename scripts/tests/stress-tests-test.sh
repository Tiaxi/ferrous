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

# Records each run's arguments and directories; fails when FAKE_CARGO_FAIL
# is set, as a test binary with a failed test does.
cat >"${bin}/cargo" <<'SH'
#!/usr/bin/env bash
echo "args=$* tmp=${TMPDIR} config=${XDG_CONFIG_HOME}" >>"${STUB_LOG}"
if [[ -n "${FAKE_CARGO_FAIL:-}" ]]; then
    echo "test some::module::a_test ... FAILED"
    exit 101
fi
echo "test result: ok."
SH
# Runs until it is stopped, and says so.
cat >"${bin}/stress-ng" <<'SH'
#!/usr/bin/env bash
echo "load started $*" >>"${STUB_LOG}"
trap 'echo "load stopped" >>"${STUB_LOG}"; exit 0' TERM
while :; do sleep 0.05; done
SH
chmod +x "${bin}/cargo" "${bin}/stress-ng"

stress() {
    PATH="${bin}:${PATH}" STUB_LOG="${WORK_DIR}/stub.log" \
        FERROUS_STRESS_DIR="${WORK_DIR}/out" \
        bash "${repo}/scripts/stress-tests.sh" --no-build "$@"
}

# Three runs of the mock suite: 1 thread, libtest's default, 32 threads,
# the third under load.
: >"${WORK_DIR}/stub.log"
stress --suite mock --runs 3 --load-workers 2 >"${WORK_DIR}/pass.out" 2>&1 \
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
# The load ran during the third run only, and was stopped with it.
[[ "$(grep -c '^load started --quiet --cpu 2$' "${WORK_DIR}/stub.log")" -eq 1 ]] \
    || fail "load: $(cat "${WORK_DIR}/stub.log")"
grep -q '^load stopped$' "${WORK_DIR}/stub.log" || fail "the load was not stopped"
[[ "$(grep -n '' "${WORK_DIR}/stub.log" | grep -m1 'load started' | cut -d: -f1)" -eq 3 ]] \
    || fail "the load did not start with the third run: $(cat "${WORK_DIR}/stub.log")"
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
