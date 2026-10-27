#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Extracts the user interface's translatable strings into ferrous.pot, the
# template that translations start from: by default po/ferrous.pot, or in the
# directory given. Needs xgettext from GNU gettext.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    echo "Usage: $0 [output-directory]" >&2
}

if [[ $# -gt 1 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 2
fi

if ! command -v xgettext >/dev/null 2>&1; then
    echo "xgettext is not installed; install GNU gettext (the gettext package on Fedora)." >&2
    exit 1
fi

out_dir="${1:-${REPO_ROOT}/po}"
mkdir -p "${out_dir}"
out_dir="$(cd "${out_dir}" && pwd)"

# The keywords of KI18n's i18n() and ki18n() families, with their context
# (c), singular and plural arguments; --kde marks the strings' %1 format.
keywords=()
for prefix in i18n ki18n xi18n kxi18n; do
    keywords+=(
        "--keyword=${prefix}:1"
        "--keyword=${prefix}c:1c,2"
        "--keyword=${prefix}p:1,2"
        "--keyword=${prefix}cp:1c,2,3"
    )
done

XGETTEXT="xgettext --from-code=UTF-8 --kde --add-comments=i18n: --package-name=ferrous \
--msgid-bugs-address=https://github.com/Tiaxi/ferrous/issues ${keywords[*]}"
export XGETTEXT
export podir="${out_dir}"

cd "${REPO_ROOT}/ui"
./Messages.sh
echo "Wrote ${out_dir}/ferrous.pot"
