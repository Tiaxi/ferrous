#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs a command while it holds one of a fixed number of build slots that all
# of the user's checkouts share, so that builds and test runs started at the
# same time, for example by agents in several worktrees, queue for a slot
# instead of running all at once.
# Usage: scripts/build-slot.sh <command> [args...]
# See docs/DEVELOPMENT.md for the variables it reads.

set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $(basename "$0") <command> [args...]" >&2
    exit 2
fi

slots="${BUILD_SLOTS:-2}"
if ! [[ "${slots}" =~ ^[1-9][0-9]*$ ]]; then
    echo "build-slot: BUILD_SLOTS must be a positive whole number, not '${slots}'." >&2
    exit 2
fi
niceness="${BUILD_SLOT_NICE:-10}"
if ! [[ "${niceness}" =~ ^[0-9]+$ ]]; then
    echo "build-slot: BUILD_SLOT_NICE must be a whole number, not '${niceness}'." >&2
    exit 2
fi

# The home directory comes from the user's account rather than from HOME or
# XDG_CACHE_HOME: test and app runs often point those at temporary
# directories, which would give that run slots of its own.
account_home="$(getent passwd "$(id -u)" | cut -d: -f6)"
lock_dir="${BUILD_SLOT_DIR:-${account_home:-${HOME}}/.cache/build-slot}"
mkdir -p -- "${lock_dir}"

if [[ -n "${BUILD_SLOT_JOBS:-}" ]]; then
    export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-${BUILD_SLOT_JOBS}}"
    export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-${BUILD_SLOT_JOBS}}"
fi
# One compiler cache for every checkout. CMake reads the launchers when it
# first configures a build directory; set them empty to build without ccache.
if command -v ccache >/dev/null 2>&1; then
    export CMAKE_C_COMPILER_LAUNCHER="${CMAKE_C_COMPILER_LAUNCHER-ccache}"
    export CMAKE_CXX_COMPILER_LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER-ccache}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR-${HOME}}"
fi

told=0
while :; do
    for ((slot = 1; slot <= slots; slot++)); do
        exec {fd}>>"${lock_dir}/slot${slot}"
        if flock -n "${fd}"; then
            # A signal to this script alone waits until the command exits, so
            # the slot is not given back while the command still runs. Bash
            # runs the trap only after the command, which keeps the default
            # handlers of its own.
            trap ':' TERM HUP INT
            # The command does not inherit the lock, so a process it leaves
            # running does not keep the slot once the command has exited.
            status=0
            nice -n "${niceness}" "$@" {fd}>&- || status=$?
            exit "${status}"
        fi
        exec {fd}>&-
    done
    if [[ "${told}" == 0 ]]; then
        echo "build-slot: all ${slots} build slots are busy; waiting..." >&2
        told=1
    fi
    sleep 2
done
