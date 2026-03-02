#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NATIVE_UI_DIR="${REPO_ROOT}/native_ui"
BUILD_DIR="${FERROUS_NATIVE_BUILD_DIR:-${NATIVE_UI_DIR}/build}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"

RUN_RUST=1
RUN_UI=1
DO_CONFIGURE=1
DO_BUILD=1
RUN_CLIPPY="${FERROUS_RUN_CLIPPY:-1}"
RUN_AUDIT="${FERROUS_RUN_AUDIT:-1}"
RUST_FEATURES="${FERROUS_TEST_FEATURES:-gst}"

usage() {
    cat <<USAGE
Usage: $(basename "$0") [options]

Options:
  --rust-only       Run only Rust checks/tests
  --ui-only         Run only native UI smoke tests
  --no-clippy       Skip strict Clippy (`-D clippy::pedantic`)
  --no-audit        Skip cargo audit
  --no-configure    Skip CMake configure step for UI tests
  --no-build        Skip CMake build step for UI tests
  -h, --help        Show this help

Environment:
  FERROUS_TEST_FEATURES   Cargo feature set for checks/tests (default: gst)
  FERROUS_RUN_CLIPPY      Run strict Clippy in Rust checks (default: 1)
  FERROUS_RUN_AUDIT       Run cargo audit in Rust checks (default: 1)
  FERROUS_NATIVE_BUILD_DIR
                          Native UI build dir (default: native_ui/build)
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
    cargo test --features "${RUST_FEATURES}"
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
        cmake -S "${NATIVE_UI_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}"
    fi
    if [[ ${DO_BUILD} -eq 1 ]]; then
        cmake --build "${BUILD_DIR}"
    fi
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi
