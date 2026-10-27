#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Check that a release tag matches the version in Cargo.toml, the RPM spec, and
# the newest AppStream release, so a tag on an un-bumped tree cannot ship
# packages of the previous version.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
METAINFO_CHECK="${SCRIPT_DIR}/../ui/tests/check_metainfo_release.cmake"

usage() {
    cat <<USAGE
Usage: $(basename "$0") [--source-dir DIR] <tag>

Fail unless <tag> is vX.Y.Z and X.Y.Z is the version in Cargo.toml, the RPM
spec, and the newest release in the AppStream metadata. Requires cmake.

Options:
  --source-dir DIR     Check the tree in DIR instead of this repository
  -h, --help           Show this help
USAGE
}

tag=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-dir)
            shift
            SOURCE_DIR="${1:-}"
            if [[ -z "${SOURCE_DIR}" ]]; then
                echo "--source-dir requires a value." >&2
                exit 1
            fi
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            if [[ -n "${tag}" ]]; then
                echo "Pass exactly one tag." >&2
                usage >&2
                exit 1
            fi
            tag="$1"
            ;;
    esac
    shift
done

if [[ -z "${tag}" ]]; then
    usage >&2
    exit 1
fi

if ! [[ "${tag}" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
    echo "Tag ${tag} is not of the form vX.Y.Z." >&2
    exit 1
fi
version="${BASH_REMATCH[1]}"

cargo_toml="${SOURCE_DIR}/Cargo.toml"
spec_file="${SOURCE_DIR}/packaging/rpm/ferrous.spec"
metainfo="${SOURCE_DIR}/ui/io.github.tiaxi.ferrous.metainfo.xml"
failed=0

# The [package] version, which scripts/build-rpm.sh and ui/CMakeLists.txt use.
cargo_version="$("${SCRIPT_DIR}/cargo-package-version.sh" "${cargo_toml}" 2>/dev/null || true)"
if [[ "${cargo_version}" != "${version}" ]]; then
    echo "Cargo.toml version '${cargo_version}' does not match tag ${tag}." >&2
    failed=1
fi

# scripts/build-rpm.sh defines ferrous_version from Cargo.toml; the spec's own
# fallback is what a plain rpmbuild of the spec uses.
spec_version="$(sed -n 's/^Version:[[:space:]]*\([^[:space:]]*\).*/\1/p' "${spec_file}" | head -n 1)"
if [[ "${spec_version}" == "%{ferrous_version}" ]]; then
    spec_version="$(sed -n 's/^%{!?ferrous_version:%global ferrous_version \([^}]*\)}$/\1/p' \
        "${spec_file}" | head -n 1)"
fi
if [[ "${spec_version}" != "${version}" ]]; then
    echo "RPM spec version '${spec_version}' does not match tag ${tag}." >&2
    failed=1
fi

if ! cmake -DMETAINFO_FILE="${metainfo}" -DEXPECTED_VERSION="${version}" \
    -P "${METAINFO_CHECK}"; then
    echo "AppStream metadata does not match tag ${tag}." >&2
    failed=1
fi

if [[ ${failed} -ne 0 ]]; then
    echo "Run scripts/bump-version.sh ${version}, commit, and tag that commit." >&2
    exit 1
fi

echo "Tag ${tag} matches Cargo.toml, the RPM spec, and the AppStream metadata."
