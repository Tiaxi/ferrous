#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Bump the app version in all locations that reference it.
# Usage: ./scripts/bump-version.sh 0.2.0

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if [[ $# -ne 1 ]]; then
    echo "Usage: $(basename "$0") <new-version>" >&2
    echo "Example: $(basename "$0") 0.2.0" >&2
    exit 1
fi

NEW_VERSION="$1"

if ! [[ "${NEW_VERSION}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: version must be in semver format (e.g. 0.2.0)" >&2
    exit 1
fi

# Cargo.toml — single source of truth
sed -i "s/^version = \".*\"/version = \"${NEW_VERSION}\"/" "${REPO_ROOT}/Cargo.toml"

# RPM spec fallback
sed -i "s/%{!?ferrous_version:%global ferrous_version .*}/%{!?ferrous_version:%global ferrous_version ${NEW_VERSION}}/" \
    "${REPO_ROOT}/packaging/rpm/ferrous.spec"

echo "Bumped version to ${NEW_VERSION} in:"
echo "  Cargo.toml"
echo "  packaging/rpm/ferrous.spec"
echo ""
echo "Next steps:"
echo "  cargo check          # regenerates Cargo.lock"
echo "  git add -p && git commit -m 'chore: bump version to ${NEW_VERSION}'"
echo "  git tag v${NEW_VERSION}"
