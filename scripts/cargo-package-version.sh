#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Print the version in the [package] table of a Cargo.toml, the app version
# that ui/CMakeLists.txt also reads and scripts/bump-version.sh changes. Other
# tables, such as a dependency's, have versions of their own.
# Usage: cargo-package-version.sh [path/to/Cargo.toml]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cargo_toml="${1:-${SCRIPT_DIR}/../Cargo.toml}"

if ! awk '
    /^[[:space:]]*\[/ { table = $0; gsub(/^[[:space:]]*\[|\][[:space:]]*(#.*)?$/, "", table) }
    table == "package" && /^version[[:space:]]*=/ {
        if (match($0, /"[^"]*"/)) {
            print substr($0, RSTART + 1, RLENGTH - 2)
            found = 1
            exit
        }
    }
    END { exit found ? 0 : 1 }
' "${cargo_toml}"; then
    echo "No version in the [package] table of ${cargo_toml}" >&2
    exit 1
fi
