#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
UI_DIR="${REPO_ROOT}/ui"
BUILD_DIR="${FERROUS_UI_BUILD_DIR:-${UI_DIR}/build}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"

# shellcheck disable=SC1091
source "${SCRIPT_DIR}/load-build-env.sh"
load_repo_build_env "${REPO_ROOT}"

RUN_RUST=1
RUN_UI=1
DO_CONFIGURE=1
DO_BUILD=1
RUN_CLIPPY="${FERROUS_RUN_CLIPPY:-1}"
RUN_AUDIT="${FERROUS_RUN_AUDIT:-1}"
RUN_COVERAGE="${FERROUS_RUN_COVERAGE:-0}"
COVERAGE_MIN="${FERROUS_COVERAGE_MIN:-35}"
RUST_FEATURES="${FERROUS_TEST_FEATURES:-gst}"

reset_stale_cmake_cache() {
    local cache_file="${BUILD_DIR}/CMakeCache.txt"
    local home_dir=""

    if [[ -f "${cache_file}" ]]; then
        home_dir="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cache_file}" | head -n 1)"
        if [[ -n "${home_dir}" && "${home_dir}" != "${UI_DIR}" ]]; then
            echo "Resetting stale CMake cache in ${BUILD_DIR} (was configured for ${home_dir})"
            rm -rf "${BUILD_DIR}/CMakeCache.txt" "${BUILD_DIR}/CMakeFiles"
        fi
    fi
}

usage() {
    cat <<USAGE
Usage: $(basename "$0") [options]

Options:
  --rust-only       Run only Rust checks/tests
  --ui-only         Run only UI smoke tests
  --no-clippy       Skip strict Clippy (-D clippy::pedantic)
  --no-audit        Skip cargo audit
  --coverage        Run Rust tests via cargo llvm-cov with line threshold gate
  --no-coverage     Skip coverage gate (cargo llvm-cov)
  --no-configure    Skip CMake configure step for UI tests
  --no-build        Skip CMake build step for UI tests
  -h, --help        Show this help

Environment:
  FERROUS_TEST_FEATURES   Cargo feature set for checks/tests (default: gst)
  FERROUS_RUN_CLIPPY      Run strict Clippy in Rust checks (default: 1)
  FERROUS_RUN_AUDIT       Run cargo audit in Rust checks (default: 1)
  FERROUS_RUN_COVERAGE    Run coverage gate via cargo-llvm-cov (default: 0)
  FERROUS_COVERAGE_MIN    Minimum line coverage percent for gate (default: 35)
  FERROUS_UI_BUILD_DIR
                          UI build dir (default: ui/build)
  CMAKE_GENERATOR         CMake generator (default: Ninja)
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rust-only)
            RUN_RUST=1
            RUN_UI=0
            ;;
        --ui-only)
            RUN_RUST=0
            RUN_UI=1
            ;;
        --no-clippy)
            RUN_CLIPPY=0
            ;;
        --no-audit)
            RUN_AUDIT=0
            ;;
        --coverage)
            RUN_COVERAGE=1
            ;;
        --no-coverage)
            RUN_COVERAGE=0
            ;;
        --no-configure)
            DO_CONFIGURE=0
            ;;
        --no-build)
            DO_BUILD=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

if ! command -v cargo >/dev/null 2>&1; then
    if [[ -f "$HOME/.cargo/env" ]]; then
        # shellcheck disable=SC1090
        source "$HOME/.cargo/env"
    fi
fi

if [[ ${RUN_RUST} -eq 1 ]]; then
    cargo fmt --check
    cargo check --features "${RUST_FEATURES}"
    if [[ ${RUN_CLIPPY} -eq 1 ]]; then
        cargo clippy --features "${RUST_FEATURES}" -- -D clippy::pedantic
    fi
    if [[ ${RUN_COVERAGE} -eq 1 ]]; then
        if ! grep -qE '^[[:space:]]+llvm-cov([[:space:]]|$)' <<<"$(cargo --list)"; then
            echo "cargo-llvm-cov is not installed." >&2
            echo "Install it with:" >&2
            echo "  rustup component add llvm-tools-preview" >&2
            echo "  cargo install cargo-llvm-cov" >&2
            exit 1
        fi
        if ! [[ "${COVERAGE_MIN}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
            echo "FERROUS_COVERAGE_MIN must be a number (got '${COVERAGE_MIN}')." >&2
            exit 1
        fi
        cargo llvm-cov --features "${RUST_FEATURES}" \
            --workspace --all-targets --summary-only \
            --fail-under-lines "${COVERAGE_MIN}"
    else
        cargo test --features "${RUST_FEATURES}"
    fi
    if [[ ${RUN_AUDIT} -eq 1 ]]; then
        if ! grep -qE '^[[:space:]]+audit([[:space:]]|$)' <<<"$(cargo --list)"; then
            echo "cargo-audit is not installed. Install it with: cargo install cargo-audit" >&2
            exit 1
        fi
        cargo audit
    fi
fi

if [[ ${RUN_UI} -eq 1 ]]; then
    if [[ ${DO_CONFIGURE} -eq 1 ]]; then
        reset_stale_cmake_cache
        cmake -S "${UI_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}"
    fi
    if [[ ${DO_BUILD} -eq 1 ]]; then
        cmake --build "${BUILD_DIR}"
    fi
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi
