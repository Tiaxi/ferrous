#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Print the version in the [package] table of a Cargo.toml, the app version
# that ui/cmake/FerrousCargoVersion.cmake also reads. Other tables, such as a
# dependency's, have versions of their own. With --set, print the file with
# that version replaced instead, keeping its quotes, comment and line endings.
# Usage: cargo-package-version.sh [--set <version>] [path/to/Cargo.toml]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
new_version=""
if [[ "${1:-}" == "--set" ]]; then
    new_version="${2:?--set requires a version}"
    shift 2
fi
cargo_toml="${1:-${SCRIPT_DIR}/../Cargo.toml}"

# POSIX awk only: Ubuntu's awk is mawk. The value is the string right after
# `=`, in single or double quotes, so a comment after it cannot supply one.
status=0
awk -v new_version="${new_version}" '
    function trim(s) {
        sub(/^[ \t]+/, "", s)
        sub(/[ \t]+$/, "", s)
        return s
    }
    {
        had_cr = sub(/\r$/, "")
        line = $0
    }
    line ~ /^[ \t]*\[/ {
        header = line
        sub(/^[ \t]*/, "", header)
        array = substr(header, 1, 2) == "[["
        header = substr(header, array ? 3 : 2)
        close_at = index(header, array ? "]]" : "]")
        if (close_at > 0) {
            name = trim(substr(header, 1, close_at - 1))
            if (name ~ /^"[^"]*"$/ || name ~ /^\047[^\047]*\047$/) {
                name = substr(name, 2, length(name) - 2)
            }
            table = array ? "[[" name "]]" : name
        }
    }
    table == "package" && !found && line ~ /^[ \t]*version[ \t]*=/ {
        value = line
        sub(/^[ \t]*version[ \t]*=[ \t]*/, "", value)
        quote = substr(value, 1, 1)
        close_at = (quote == "\"" || quote == "\047") ? index(substr(value, 2), quote) : 0
        if (close_at == 0 || (quote == "\"" && index(substr(value, 2, close_at - 1), "\\") > 0)) {
            print "The [package] version in " FILENAME " is not a plain quoted string" > "/dev/stderr"
            failed = 1
            exit
        }
        found = 1
        if (new_version == "") {
            print substr(value, 2, close_at - 1)
            exit
        }
        start = length(line) - length(value)
        line = substr(line, 1, start) quote new_version quote substr(value, close_at + 2)
    }
    new_version != "" { printf "%s%s\n", line, had_cr ? "\r" : "" }
    END { exit failed ? 2 : (found ? 0 : 1) }
' "${cargo_toml}" || status=$?
if [[ ${status} -eq 1 ]]; then
    echo "No version in the [package] table of ${cargo_toml}" >&2
fi
exit "${status}"
