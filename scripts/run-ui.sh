#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
UI_DIR="${REPO_ROOT}/ui"
BUILD_DIR="${FERROUS_UI_BUILD_DIR:-${UI_DIR}/build}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"

# shellcheck disable=SC1091
source "${SCRIPT_DIR}/load-build-env.sh"
load_repo_build_env "${REPO_ROOT}"

DO_CONFIGURE=1
DO_BUILD=1
DO_RUN=1
NUKE_DB=0
NUKE_SESSION=0
NUKE_THUMBNAILS=0
CLEAR_DIAGNOSTICS_LOG=0
ASSUME_YES=0
ENABLE_COREDUMP=0
ENABLE_PROFILE_LOGS=0
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

CLEANUP_PATHS=()
CLEANUP_KINDS=()
CLEANUP_LABELS=()

# Usage: add_cleanup_target file|dir <path> <label>
add_cleanup_target() {
    CLEANUP_KINDS+=("$1")
    CLEANUP_PATHS+=("$2")
    CLEANUP_LABELS+=("$3")
}

cleanup_target_exists() {
    local index="$1"
    local path="${CLEANUP_PATHS[index]}"

    if [[ "${CLEANUP_KINDS[index]}" == dir ]]; then
        [[ -d "${path}" && ! -L "${path}" ]]
    else
        [[ -f "${path}" || -L "${path}" ]]
    fi
}

collect_cleanup_targets() {
    local data_home="${XDG_DATA_HOME:-${HOME}/.local/share}"
    local config_home="${XDG_CONFIG_HOME:-${HOME}/.config}"
    local cache_home="${XDG_CACHE_HOME:-${HOME}/.cache}"
    local db_path="${data_home}/ferrous/library.sqlite3"

    if [[ ${NUKE_DB} -eq 1 ]]; then
        add_cleanup_target file "${db_path}" "library database"
        add_cleanup_target file "${db_path}-wal" "library database WAL"
        add_cleanup_target file "${db_path}-shm" "library database SHM"
    fi
    if [[ ${NUKE_SESSION} -eq 1 ]]; then
        add_cleanup_target file "${config_home}/ferrous/session.json" "saved session"
    fi
    if [[ ${NUKE_THUMBNAILS} -eq 1 ]]; then
        # Only older versions kept library thumbnails. Their folder under /tmp
        # was shared by every user, so it is left alone.
        add_cleanup_target dir "${cache_home}/ferrous/thumbnails/library" "thumbnail cache"
    fi
    if [[ ${CLEAR_DIAGNOSTICS_LOG} -eq 1 ]]; then
        add_cleanup_target file "${data_home}/ferrous/diagnostics.log" "diagnostics log"
    fi
}

confirm_cleanup() {
    local reply=""

    if [[ ${ASSUME_YES} -eq 1 ]]; then
        return
    fi
    if [[ ! -t 0 ]]; then
        echo "Not deleting without confirmation; pass --yes to delete without asking." >&2
        exit 1
    fi
    read -r -p "Delete these? [y/N] " reply
    if [[ ! "${reply}" =~ ^[Yy]([Ee][Ss])?$ ]]; then
        echo "Nothing was deleted." >&2
        exit 1
    fi
}

run_requested_cleanup() {
    local existing=()
    local index=0

    collect_cleanup_targets
    if [[ ${#CLEANUP_PATHS[@]} -eq 0 ]]; then
        return
    fi

    for index in "${!CLEANUP_PATHS[@]}"; do
        if cleanup_target_exists "${index}"; then
            existing+=("${index}")
        else
            echo "No ${CLEANUP_LABELS[index]} at ${CLEANUP_PATHS[index]}"
        fi
    done
    if [[ ${#existing[@]} -eq 0 ]]; then
        return
    fi

    echo "This deletes Ferrous data:"
    for index in "${existing[@]}"; do
        echo "  ${CLEANUP_LABELS[index]}: ${CLEANUP_PATHS[index]}"
    done
    confirm_cleanup
    for index in "${existing[@]}"; do
        echo "Removing ${CLEANUP_LABELS[index]}: ${CLEANUP_PATHS[index]}"
        if [[ "${CLEANUP_KINDS[index]}" == dir ]]; then
            rm -rf -- "${CLEANUP_PATHS[index]}"
        else
            rm -f -- "${CLEANUP_PATHS[index]}"
        fi
    done
}

usage() {
    cat <<USAGE
Usage: $(basename "$0") [options] [-- <ui-args...>]

Options:
  --no-configure    Skip cmake configure step
  --no-build        Skip cmake build step
  --no-run          Only configure/build; do not launch UI
  --spectrogram-instrumentation
                    Build with UI profiling logs and export FERROUS_PROFILE_UI=1 on launch
  --profile-logs    Alias for --spectrogram-instrumentation
  --nuke-db         Delete Ferrous library DB (${XDG_DATA_HOME:-\$HOME/.local/share}/ferrous/library.sqlite3 + -wal/-shm)
  --nuke-session    Delete saved playlist/session (${XDG_CONFIG_HOME:-\$HOME/.config}/ferrous/session.json)
  --nuke-thumbnails Delete the library thumbnail cache of older versions (${XDG_CACHE_HOME:-\$HOME/.cache}/ferrous/thumbnails/library)
  --clear-diagnostics-log
                    Delete Ferrous diagnostics log (${XDG_DATA_HOME:-\$HOME/.local/share}/ferrous/diagnostics.log)
  --nuke-all        Equivalent to --nuke-db --nuke-session --nuke-thumbnails
  -y, --yes         Delete what the options above name without asking; needed
                    when stdin is not a terminal
  --coredump        Enable unlimited core dump size and print coredumpctl hints
  -h, --help        Show this help

Environment:
  FERROUS_UI_BUILD_DIR     Override build dir (default: ${UI_DIR}/build)
  XDG_DATA_HOME            Base path for DB cleanup target (default: \$HOME/.local/share)
  XDG_CONFIG_HOME          Base path for session cleanup target (default: \$HOME/.config)
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
        --spectrogram-instrumentation|--profile-logs)
            ENABLE_PROFILE_LOGS=1
            ;;
        --nuke-db)
            NUKE_DB=1
            ;;
        --nuke-session)
            NUKE_SESSION=1
            ;;
        --nuke-thumbnails)
            NUKE_THUMBNAILS=1
            ;;
        --clear-diagnostics-log)
            CLEAR_DIAGNOSTICS_LOG=1
            ;;
        --nuke-all)
            NUKE_DB=1
            NUKE_SESSION=1
            NUKE_THUMBNAILS=1
            ;;
        -y|--yes)
            ASSUME_YES=1
            ;;
        --coredump)
            ENABLE_COREDUMP=1
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

if [[ ${DO_CONFIGURE} -eq 1 ]]; then
    reset_stale_cmake_cache
    CMAKE_ARGS=(
        -S "${UI_DIR}"
        -B "${BUILD_DIR}"
        -G "${GENERATOR}"
        -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
        -DFERROUS_ENABLE_PROFILE_LOGS=$([[ ${ENABLE_PROFILE_LOGS} -eq 1 ]] && echo ON || echo OFF)
    )
    cmake "${CMAKE_ARGS[@]}"
fi

if [[ ${DO_BUILD} -eq 1 ]]; then
    cmake --build "${BUILD_DIR}" -j
fi

if [[ ${DO_RUN} -eq 1 ]]; then
    if [[ ${ENABLE_PROFILE_LOGS} -eq 1 ]]; then
        export FERROUS_PROFILE_UI="${FERROUS_PROFILE_UI:-1}"
        echo "UI instrumentation/profiling enabled (FERROUS_PROFILE_UI=${FERROUS_PROFILE_UI})."
    fi
    if [[ ${ENABLE_COREDUMP} -eq 1 ]]; then
        ulimit -c unlimited || true
        echo "Core dumps enabled (ulimit -c unlimited)."
        echo "After a crash:"
        echo "  coredumpctl list ferrous"
        echo "  coredumpctl gdb -1 ${BUILD_DIR}/ferrous"
    fi
    exec "${BUILD_DIR}/ferrous" "${APP_ARGS[@]}"
fi
