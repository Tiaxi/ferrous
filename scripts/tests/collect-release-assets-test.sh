#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs scripts/collect-release-assets.sh on stand-in RPMs in the layouts that
# actions/download-artifact produces: a lone artifact extracted straight into
# the download directory, and one directory per artifact. Checks the assets
# and SHA256SUMS it writes, and that it fails for duplicate names, no packages
# and a missing required build without moving anything.
# Usage: collect-release-assets-test.sh <absolute scratch work dir>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COLLECT="${SCRIPT_DIR}/../collect-release-assets.sh"
WORK_DIR="${1:?usage: $(basename "$0") <work dir>}"
if [[ "${WORK_DIR}" != /* ]]; then
    echo "The work dir must be an absolute path." >&2
    exit 1
fi

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

rm -rf -- "${WORK_DIR}"
mkdir -p "${WORK_DIR}"
cd "${WORK_DIR}"

fc44_rpm="ferrous-1.2.3-1.fc44.x86_64.rpm"
fc45_rpm="ferrous-1.2.3-1.fc45.x86_64.rpm"

# Writes a stand-in RPM whose contents name it, so each checksum differs.
fake_rpm() {
    mkdir -p "$(dirname "$1")"
    printf 'stand-in for %s\n' "$(basename "$1")" >"$1"
}

# Runs the collect script in case directory $1 with the remaining arguments,
# keeping its output in $1/out and $1/err.
collect() {
    local case_dir="$1"
    shift
    bash "${COLLECT}" "$@" >"${case_dir}/out" 2>"${case_dir}/err"
}

# Checks that the assets in $1 are exactly the RPMs named after it, plus a
# SHA256SUMS that lists each of them and passes its own check.
expect_assets() {
    local assets="$1"
    shift
    local expected actual
    expected="$(printf '%s\n' "$@" SHA256SUMS | LC_ALL=C sort)"
    actual="$(cd "${assets}" && find . -mindepth 1 -printf '%P\n' | LC_ALL=C sort)"
    [[ "${actual}" == "${expected}" ]] \
        || fail "${assets} holds: ${actual//$'\n'/ }; expected: ${expected//$'\n'/ }"
    local name
    for name in "$@"; do
        grep -qE "^[0-9a-f]{64}  ${name//./\\.}\$" "${assets}/SHA256SUMS" \
            || fail "${assets}/SHA256SUMS does not list ${name}: $(cat "${assets}/SHA256SUMS")"
    done
    [[ "$(wc -l <"${assets}/SHA256SUMS")" -eq $# ]] \
        || fail "${assets}/SHA256SUMS lists more than the packages: $(cat "${assets}/SHA256SUMS")"
    (cd "${assets}" && sha256sum --check --strict --quiet SHA256SUMS) \
        || fail "${assets}/SHA256SUMS does not match the packages"
}

# The v0.3.0 release, when Fedora 45 was not yet required: its build
# failed, and download-artifact extracted the only artifact straight into
# packages/.
case_dir="${WORK_DIR}/lone-artifact"
fake_rpm "${case_dir}/packages/x86_64/${fc44_rpm}"
collect "${case_dir}" --require-dist fc44 "${case_dir}/packages" "${case_dir}/assets" \
    || fail "a lone artifact was not collected: $(cat "${case_dir}/err")"
expect_assets "${case_dir}/assets" "${fc44_rpm}"
grep -qF "${fc44_rpm}" "${case_dir}/out" || fail "the checksums were not printed"
[[ -z "$(find "${case_dir}/packages" -name '*.rpm')" ]] || fail "a package was left in packages/"

# Both builds passed: one directory per artifact.
case_dir="${WORK_DIR}/per-artifact"
fake_rpm "${case_dir}/packages/rpm-package-fc44/x86_64/${fc44_rpm}"
fake_rpm "${case_dir}/packages/rpm-package-fc45/x86_64/${fc45_rpm}"
collect "${case_dir}" --require-dist fc44 --require-dist fc45 "${case_dir}/packages" "${case_dir}/assets" \
    || fail "per-artifact directories were not collected: $(cat "${case_dir}/err")"
expect_assets "${case_dir}/assets" "${fc44_rpm}" "${fc45_rpm}"

# A lone artifact whose files sit at the top of the download directory.
case_dir="${WORK_DIR}/flat"
fake_rpm "${case_dir}/packages/${fc44_rpm}"
collect "${case_dir}" "${case_dir}/packages" "${case_dir}/assets" \
    || fail "an RPM at the top of the download directory was not collected: $(cat "${case_dir}/err")"
expect_assets "${case_dir}/assets" "${fc44_rpm}"

# Checks that a failed collection in case dir $1 left its downloads alone,
# made no assets, and named the reason $2.
expect_failure() {
    local case_dir="$1" reason="$2"
    [[ ! -e "${case_dir}/assets" ]] || fail "${case_dir}: assets were made"
    grep -qF "${reason}" "${case_dir}/err" \
        || fail "${case_dir}: the error does not say \"${reason}\": $(cat "${case_dir}/err")"
}

# Two artifacts with a package of the same name.
case_dir="${WORK_DIR}/duplicate"
fake_rpm "${case_dir}/packages/rpm-package-fc44/x86_64/${fc44_rpm}"
fake_rpm "${case_dir}/packages/rpm-package-fc44-again/x86_64/${fc44_rpm}"
if collect "${case_dir}" "${case_dir}/packages" "${case_dir}/assets"; then
    fail "two packages with the same name were collected"
fi
expect_failure "${case_dir}" "Two packages are named ${fc44_rpm}"
[[ "$(find "${case_dir}/packages" -name '*.rpm' | wc -l)" -eq 2 ]] \
    || fail "a failed collection moved packages"

# No packages at all, in an empty download directory and in empty artifact
# directories.
case_dir="${WORK_DIR}/none"
mkdir -p "${case_dir}/packages/rpm-package-fc44"
touch "${case_dir}/packages/rpm-package-fc44/notes.txt"
if collect "${case_dir}" "${case_dir}/packages" "${case_dir}/assets"; then
    fail "a release without packages was collected"
fi
expect_failure "${case_dir}" "No packages to release"

case_dir="${WORK_DIR}/missing-download-dir"
mkdir -p "${case_dir}"
if collect "${case_dir}" "${case_dir}/packages" "${case_dir}/assets"; then
    fail "a missing download directory was collected"
fi
expect_failure "${case_dir}" "is not a directory"

# Both builds are required and only Fedora 44 built: the lone artifact sits
# straight in packages/, and the missing Fedora 45 package fails the release.
case_dir="${WORK_DIR}/second-required-missing"
fake_rpm "${case_dir}/packages/x86_64/${fc44_rpm}"
if collect "${case_dir}" --require-dist fc44 --require-dist fc45 "${case_dir}/packages" "${case_dir}/assets"; then
    fail "a release without the required fc45 package was collected"
fi
expect_failure "${case_dir}" "No fc45 package to release"
[[ -f "${case_dir}/packages/x86_64/${fc44_rpm}" ]] || fail "a failed collection moved packages"

# Only the other build's package: the required build is missing.
case_dir="${WORK_DIR}/required-missing"
fake_rpm "${case_dir}/packages/x86_64/${fc45_rpm}"
if collect "${case_dir}" --require-dist fc44 "${case_dir}/packages" "${case_dir}/assets"; then
    fail "a release without the required fc44 package was collected"
fi
expect_failure "${case_dir}" "No fc44 package to release"
[[ -f "${case_dir}/packages/x86_64/${fc45_rpm}" ]] || fail "a failed collection moved packages"

# A distribution tag must match a whole component of the file name.
case_dir="${WORK_DIR}/required-prefix"
fake_rpm "${case_dir}/packages/x86_64/ferrous-1.2.3-1.fc440.x86_64.rpm"
if collect "${case_dir}" --require-dist fc44 "${case_dir}/packages" "${case_dir}/assets"; then
    fail "fc440 was taken for the required fc44"
fi
expect_failure "${case_dir}" "No fc44 package to release"

# An existing assets directory is not reused.
case_dir="${WORK_DIR}/assets-exist"
fake_rpm "${case_dir}/packages/x86_64/${fc44_rpm}"
mkdir -p "${case_dir}/assets"
if collect "${case_dir}" "${case_dir}/packages" "${case_dir}/assets"; then
    fail "an existing assets directory was reused"
fi
grep -qF "already exists" "${case_dir}/err" || fail "the error does not name the existing directory"
[[ -f "${case_dir}/packages/x86_64/${fc44_rpm}" ]] || fail "a failed collection moved packages"

echo "collect-release-assets tests passed"
