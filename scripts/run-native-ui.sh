#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NATIVE_UI_DIR="${REPO_ROOT}/native_ui"
BUILD_DIR="${FERROUS_NATIVE_BUILD_DIR:-${NATIVE_UI_DIR}/build}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
DEFAULT_BRIDGE_CMD='cargo run --release --bin native_frontend --features gst -- --json-bridge'
DEFAULT_BRIDGE_BIN="${REPO_ROOT}/target/release/native_frontend"

DO_CONFIGURE=1
DO_BUILD=1
DO_RUN=1
APP_ARGS=()

usage() {
    cat <<USAGE
Usage: $(basename "$0") [options] [-- <native-ui-args...>]

Options:
  --no-configure    Skip cmake configure step
  --no-build        Skip cmake build step
  --no-run          Only configure/build; do not launch UI
  -h, --help        Show this help

Environment:
  FERROUS_BRIDGE_CMD       Override bridge command (default: ${DEFAULT_BRIDGE_CMD})
  FERROUS_NATIVE_BUILD_DIR Override build dir (default: ${NATIVE_UI_DIR}/build)
  CMAKE_GENERATOR          Override generator (default: Ninja)
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-configure)
            DO_CONFIGURE=0
            ;;
        --no-build)
            DO_BUILD=0
            ;;
        --no-run)
            DO_RUN=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            APP_ARGS=("$@")
            break
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

if ! command -v cargo >/dev/null 2>&1; then
    echo "cargo not found in PATH. Install Rust or source ~/.cargo/env" >&2
    exit 1
fi

BRIDGE_CMD="${FERROUS_BRIDGE_CMD:-$DEFAULT_BRIDGE_CMD}"

if [[ ${DO_CONFIGURE} -eq 1 ]]; then
    cmake -S "${NATIVE_UI_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}"
fi

if [[ ${DO_BUILD} -eq 1 ]]; then
    cargo build --release --bin native_frontend --features gst
    cmake --build "${BUILD_DIR}" -j
fi

if [[ -z "${FERROUS_BRIDGE_CMD:-}" ]]; then
    BRIDGE_CMD="${DEFAULT_BRIDGE_BIN} --json-bridge"
fi

if [[ ${DO_RUN} -eq 1 ]]; then
    export FERROUS_BRIDGE_CMD="${BRIDGE_CMD:-$DEFAULT_BRIDGE_CMD}"
    exec "${BUILD_DIR}/ferrous_kirigami_shell" "${APP_ARGS[@]}"
fi
