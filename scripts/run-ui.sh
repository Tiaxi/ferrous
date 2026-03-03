#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
UI_DIR="${REPO_ROOT}/ui"
BUILD_DIR="${FERROUS_UI_BUILD_DIR:-${FERROUS_NATIVE_BUILD_DIR:-${UI_DIR}/build}}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
DEFAULT_BRIDGE_CMD='cargo run --release --bin native_frontend --features gst -- --json-bridge'
DEFAULT_BRIDGE_BIN="${REPO_ROOT}/target/release/native_frontend"

DO_CONFIGURE=1
DO_BUILD=1
DO_RUN=1
FORCE_PROCESS_BRIDGE=0
NUKE_DB=0
NUKE_THUMBNAILS=0
APP_ARGS=()

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

remove_file_target() {
    local target="$1"
    local label="$2"

    if [[ -f "${target}" ]]; then
        echo "Removing ${label}: ${target}"
        rm -f -- "${target}"
    else
        echo "No ${label} at ${target}"
    fi
}

remove_dir_target() {
    local target="$1"
    local label="$2"

    if [[ -d "${target}" ]]; then
        echo "Removing ${label}: ${target}"
        rm -rf -- "${target}"
    else
        echo "No ${label} at ${target}"
    fi
}

nuke_library_db() {
    local data_home="${XDG_DATA_HOME:-${HOME}/.local/share}"
    local db_path="${data_home}/ferrous/library.sqlite3"

    remove_file_target "${db_path}" "library database"
    remove_file_target "${db_path}-wal" "library database WAL"
    remove_file_target "${db_path}-shm" "library database SHM"
}

nuke_thumbnail_cache() {
    local cache_home="${XDG_CACHE_HOME:-${HOME}/.cache}"
    local primary_path="${cache_home}/ferrous/thumbnails/library"
    local fallback_path="/tmp/ferrous/thumbnails/library"

    remove_dir_target "${primary_path}" "thumbnail cache"
    if [[ "${fallback_path}" != "${primary_path}" ]]; then
        remove_dir_target "${fallback_path}" "thumbnail cache fallback"
    fi
}

run_requested_cleanup() {
    if [[ ${NUKE_DB} -eq 0 && ${NUKE_THUMBNAILS} -eq 0 ]]; then
        return
    fi

    echo "Running requested Ferrous cleanup..."
    if [[ ${NUKE_DB} -eq 1 ]]; then
        nuke_library_db
    fi
    if [[ ${NUKE_THUMBNAILS} -eq 1 ]]; then
        nuke_thumbnail_cache
    fi
}

usage() {
    cat <<USAGE
Usage: $(basename "$0") [options] [-- <ui-args...>]

Options:
  --no-configure    Skip cmake configure step
  --no-build        Skip cmake build step
  --no-run          Only configure/build; do not launch UI
  --process-bridge  Force legacy process/stdout bridge (default: in-process FFI bridge)
  --nuke-db         Delete Ferrous library DB (${XDG_DATA_HOME:-\$HOME/.local/share}/ferrous/library.sqlite3 + -wal/-shm)
  --nuke-thumbnails Delete Ferrous library thumbnail cache (${XDG_CACHE_HOME:-\$HOME/.cache}/ferrous/thumbnails/library)
  --nuke-all        Equivalent to --nuke-db --nuke-thumbnails
  -h, --help        Show this help

Environment:
  FERROUS_BRIDGE_CMD       Override bridge command (default: ${DEFAULT_BRIDGE_CMD})
  FERROUS_BRIDGE_MODE      Set to 'process' to force legacy process bridge
  FERROUS_UI_BUILD_DIR     Override build dir (default: ${UI_DIR}/build)
  FERROUS_NATIVE_BUILD_DIR Backward-compatible alias for FERROUS_UI_BUILD_DIR
  XDG_DATA_HOME            Base path for DB cleanup target (default: \$HOME/.local/share)
  XDG_CACHE_HOME           Base path for thumbnail cleanup target (default: \$HOME/.cache)
  CMAKE_BUILD_TYPE         Build type for single-config generators (default: RelWithDebInfo)
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
        --process-bridge)
            FORCE_PROCESS_BRIDGE=1
            ;;
        --nuke-db)
            NUKE_DB=1
            ;;
        --nuke-thumbnails)
            NUKE_THUMBNAILS=1
            ;;
        --nuke-all)
            NUKE_DB=1
            NUKE_THUMBNAILS=1
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

run_requested_cleanup

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

BRIDGE_MODE_RAW="${FERROUS_BRIDGE_MODE:-}"
if [[ ${FORCE_PROCESS_BRIDGE} -eq 1 ]]; then
    BRIDGE_MODE_RAW="process"
fi
BRIDGE_MODE="$(printf '%s' "${BRIDGE_MODE_RAW}" | tr '[:upper:]' '[:lower:]')"
USE_PROCESS_BRIDGE=0
if [[ "${BRIDGE_MODE}" == "process" ]]; then
    USE_PROCESS_BRIDGE=1
fi

if [[ ${DO_CONFIGURE} -eq 1 ]]; then
    reset_stale_cmake_cache
    cmake -S "${UI_DIR}" -B "${BUILD_DIR}" -G "${GENERATOR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
fi

if [[ ${DO_BUILD} -eq 1 ]]; then
    if [[ ${USE_PROCESS_BRIDGE} -eq 1 ]]; then
        cargo build --release --bin native_frontend --features gst
    fi
    cmake --build "${BUILD_DIR}" -j
fi

if [[ ${DO_RUN} -eq 1 ]]; then
    if [[ ${USE_PROCESS_BRIDGE} -eq 1 ]]; then
        BRIDGE_CMD="${FERROUS_BRIDGE_CMD:-}"
        if [[ -z "${BRIDGE_CMD}" ]]; then
            if [[ -x "${DEFAULT_BRIDGE_BIN}" ]]; then
                BRIDGE_CMD="${DEFAULT_BRIDGE_BIN} --json-bridge"
            else
                BRIDGE_CMD="${DEFAULT_BRIDGE_CMD}"
            fi
        fi
        export FERROUS_BRIDGE_MODE=process
        export FERROUS_BRIDGE_CMD="${BRIDGE_CMD}"
    else
        export FERROUS_BRIDGE_MODE=in-process
    fi
    exec "${BUILD_DIR}/ferrous" "${APP_ARGS[@]}"
fi
