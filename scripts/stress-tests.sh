#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the test suites again and again, some runs under CPU load and with
# varied parallelism, to find timing-sensitive tests. See docs/DEVELOPMENT.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${FERROUS_UI_BUILD_DIR:-${REPO_ROOT}/ui/build}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
NPROC="$(nproc)"

# shellcheck disable=SC1091
source "${SCRIPT_DIR}/load-build-env.sh"
load_repo_build_env "${REPO_ROOT}"

SUITES=()
RUNS=30
FIRST=1
LOAD_EVERY=3
LOAD_WORKERS=$((NPROC * 2))
# Single-threaded, the GStreamer tests, which play their fixtures in real
# time, take about 20 minutes; with 4 threads about 5.
MOCK_THREADS=(1 default 32)
GST_THREADS=(4 default 32)
RUST_THREADS=()
CTEST_JOBS=(1 4 "${NPROC}")
DO_BUILD=1
OUT_DIR="${FERROUS_STRESS_DIR:-${REPO_ROOT}/target/stress}"

usage() {
    cat <<USAGE
Usage: $(basename "$0") [options]

Runs each chosen suite --runs times. Run i of a suite uses the i-th entry of
the thread or job list, cycling, and runs under load when i is a multiple of
--load-every. Logs go to ${OUT_DIR#"${REPO_ROOT}"/}/<suite>-<i>.log.

Options:
  --suite NAME        gst (Rust, GStreamer backend), mock (Rust, mock
                      backend) or ctest (Qt and script tests); repeatable,
                      default all three
  --runs N            Runs of each suite (default ${RUNS})
  --first I           Number the runs from I, so a long loop can be split
                      into several invocations that keep the cycle
                      (default 1)
  --load-every K      Load every K-th run; 0 for none (default ${LOAD_EVERY})
  --load-workers N    CPU-bound load processes (default ${LOAD_WORKERS},
                      twice the CPUs)
  --threads "LIST"    Rust --test-threads values to cycle through
                      (default "${MOCK_THREADS[*]}" for mock and
                      "${GST_THREADS[*]}" for gst; default = libtest's)
  --jobs "LIST"       ctest -j values to cycle through
                      (default "${CTEST_JOBS[*]}"; default = ctest's, serial)
  --no-build          Use the existing build
  -h, --help          Show this help

Environment:
  FERROUS_STRESS_DIR    Log directory (default target/stress)
  FERROUS_UI_BUILD_DIR  UI build dir (default ui/build)
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --suite) SUITES+=("${2:?}"); shift ;;
        --runs) RUNS="${2:?}"; shift ;;
        --first) FIRST="${2:?}"; shift ;;
        --load-every) LOAD_EVERY="${2:?}"; shift ;;
        --load-workers) LOAD_WORKERS="${2:?}"; shift ;;
        --threads) read -r -a RUST_THREADS <<<"${2:?}"; shift ;;
        --jobs) read -r -a CTEST_JOBS <<<"${2:?}"; shift ;;
        --no-build) DO_BUILD=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done
for number in "${RUNS}" "${FIRST}" "${LOAD_EVERY}" "${LOAD_WORKERS}"; do
    if ! [[ "${number}" =~ ^[0-9]+$ ]]; then
        echo "Not a number: ${number}" >&2
        exit 1
    fi
done
[[ ${#SUITES[@]} -gt 0 ]] || SUITES=(gst mock ctest)
for suite in "${SUITES[@]}"; do
    case "${suite}" in
        gst|mock|ctest) ;;
        *) echo "Unknown suite: ${suite}" >&2; exit 1 ;;
    esac
done

if ! command -v cargo >/dev/null 2>&1 && [[ -f "$HOME/.cargo/env" ]]; then
    # shellcheck disable=SC1090
    source "$HOME/.cargo/env"
fi

cargo_features() {
    if [[ "$1" == gst ]]; then
        echo "--no-default-features --features cli,gst"
    else
        echo "--no-default-features --features cli"
    fi
}

mkdir -p "${OUT_DIR}"
# Absolute, so the tests' temporary paths, and the links they make, are too.
OUT_DIR="$(cd "${OUT_DIR}" && pwd)"
# As in run-tests.sh: throwaway user and temporary directories, removed at
# the end, so no run sees the real settings, library or cache.
STATE_ROOT="$(mktemp -d "${OUT_DIR}/state.XXXXXX")"
LOAD_PIDS=()

stop_load() {
    local pid
    for pid in "${LOAD_PIDS[@]}"; do
        kill "${pid}" 2>/dev/null || true
    done
    for pid in "${LOAD_PIDS[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
    LOAD_PIDS=()
}

cleanup() {
    stop_load
    rm -rf -- "${STATE_ROOT}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

start_load() {
    local i
    if command -v stress-ng >/dev/null 2>&1; then
        stress-ng --quiet --cpu "${LOAD_WORKERS}" >/dev/null 2>&1 &
        LOAD_PIDS+=("$!")
    else
        for ((i = 0; i < LOAD_WORKERS; i++)); do
            bash -c 'while :; do :; done' &
            LOAD_PIDS+=("$!")
        done
    fi
}

with_test_dirs() {
    rm -rf -- "${STATE_ROOT:?}/run"
    mkdir -p "${STATE_ROOT}/run/tmp"
    TMPDIR="${STATE_ROOT}/run/tmp" \
        XDG_CONFIG_HOME="${STATE_ROOT}/run/config" \
        XDG_DATA_HOME="${STATE_ROOT}/run/data" \
        XDG_CACHE_HOME="${STATE_ROOT}/run/cache" \
        "$@"
}

if [[ ${DO_BUILD} -eq 1 ]]; then
    for suite in "${SUITES[@]}"; do
        if [[ "${suite}" == ctest ]]; then
            if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
                cmake -S "${REPO_ROOT}/ui" -B "${BUILD_DIR}" -G "${GENERATOR}" \
                    -DFERROUS_ENABLE_PROFILE_LOGS=OFF
            fi
            cmake --build "${BUILD_DIR}"
        else
            # shellcheck disable=SC2046
            (cd "${REPO_ROOT}" && cargo test $(cargo_features "${suite}") --no-run)
        fi
    done
fi

SUMMARY="${OUT_DIR}/summary.txt"
failed_runs=0
total_runs=0
for suite in "${SUITES[@]}"; do
    for ((i = FIRST; i < FIRST + RUNS; i++)); do
        loaded=0
        if [[ ${LOAD_EVERY} -gt 0 && $((i % LOAD_EVERY)) -eq 0 ]]; then
            loaded=1
        fi
        log="${OUT_DIR}/${suite}-${i}.log"
        if [[ "${suite}" == ctest ]]; then
            jobs="${CTEST_JOBS[$(((i - 1) % ${#CTEST_JOBS[@]}))]}"
            parallel="jobs=${jobs}"
            command=(ctest --test-dir "${BUILD_DIR}" --output-on-failure)
            [[ "${jobs}" == default ]] || command+=(-j "${jobs}")
        else
            threads_list=("${RUST_THREADS[@]}")
            if [[ ${#threads_list[@]} -eq 0 && "${suite}" == gst ]]; then
                threads_list=("${GST_THREADS[@]}")
            elif [[ ${#threads_list[@]} -eq 0 ]]; then
                threads_list=("${MOCK_THREADS[@]}")
            fi
            threads="${threads_list[$(((i - 1) % ${#threads_list[@]}))]}"
            parallel="threads=${threads}"
            # shellcheck disable=SC2207
            command=(cargo test --no-fail-fast $(cargo_features "${suite}") --)
            [[ "${threads}" == default ]] || command+=("--test-threads=${threads}")
        fi
        [[ ${loaded} -eq 1 ]] && start_load
        context="${suite} run ${i} ${parallel} load=${loaded} loadavg_start=$(cut -d' ' -f1 /proc/loadavg)"
        started=${SECONDS}
        status=0
        (cd "${REPO_ROOT}" && with_test_dirs "${command[@]}") >"${log}" 2>&1 || status=$?
        context+=" loadavg_end=$(cut -d' ' -f1 /proc/loadavg) seconds=$((SECONDS - started))"
        stop_load
        total_runs=$((total_runs + 1))
        if [[ ${status} -eq 0 ]]; then
            echo "PASS ${context}" | tee -a "${SUMMARY}"
            continue
        fi
        failed_runs=$((failed_runs + 1))
        echo "FAIL ${context} status=${status} log=${log}" | tee -a "${SUMMARY}"
        if [[ "${suite}" == ctest ]]; then
            sed -n '/The following tests FAILED/,$p' "${log}" | tail -n +2 | tee -a "${SUMMARY}"
        else
            grep -E '^test .* FAILED$|^error: test failed' "${log}" | tee -a "${SUMMARY}" || true
        fi
    done
done

echo "${failed_runs} of ${total_runs} runs failed; summary in ${SUMMARY}"
[[ ${failed_runs} -eq 0 ]]
