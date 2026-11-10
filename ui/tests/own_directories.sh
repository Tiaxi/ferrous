#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# CTest's launcher for the test programs: runs the command it is given with
# a temporary directory and XDG config, data and cache directories of its
# own, made in $TMPDIR, so that tests run at the same time (ctest -j) share
# no settings, library, caches or temporary files, and none reaches the
# real ones. scripts/run-tests.sh points $TMPDIR at a directory it removes
# when the run ends.
#
# The command replaces this script rather than running below it: CTest
# stops a test that times out by killing the process it started, which
# would leave a child of this script running.

set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "usage: $(basename "$0") <command> [arguments...]" >&2
    exit 2
fi

root="$(mktemp -d "${TMPDIR:-/tmp}/$(basename -- "$1").XXXXXX")"
mkdir "${root}/tmp" "${root}/config" "${root}/data" "${root}/cache"
export TMPDIR="${root}/tmp"
export XDG_CONFIG_HOME="${root}/config"
export XDG_DATA_HOME="${root}/data"
export XDG_CACHE_HOME="${root}/cache"
exec "$@"
