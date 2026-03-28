#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

load_repo_build_env() {
    local repo_root="$1"
    local env_file="${repo_root}/build.env"

    if [[ ! -f "${env_file}" ]]; then
        return 0
    fi

    set -a
    # shellcheck disable=SC1090
    source "${env_file}"
    set +a
}
