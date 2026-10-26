#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RPM_TOPDIR="${REPO_ROOT}/dist/rpm"
SPEC_FILE="${REPO_ROOT}/packaging/rpm/ferrous.spec"
DEFAULT_LICENSE="GPL-3.0-or-later"

# shellcheck disable=SC1091
source "${SCRIPT_DIR}/load-build-env.sh"
load_repo_build_env "${REPO_ROOT}"

DO_INSTALL=0
DO_CHECK=1
DO_CLEAN=0
RELEASE_OVERRIDE=""
LICENSE_VALUE="${DEFAULT_LICENSE}"

usage() {
    cat <<USAGE
Usage: $(basename "$0") [options]

Build a local Ferrous RPM from the current working tree. Crates are vendored
into a second source tarball so that rpmbuild builds offline. rpmbuild checks
the spec's BuildRequires; install them with:

  sudo dnf builddep packaging/rpm/ferrous.spec

Options:
  --install            Install the resulting RPM via scripts/install-rpm.sh
  --no-check           Skip the rpmbuild %check phase
  --clean              Remove dist/rpm before building
  --release VALUE      Override the computed RPM Release
  --license VALUE      Override the RPM License field
  -h, --help           Show this help

Environment:
  FERROUS_DNF_CMD      Forwarded to scripts/install-rpm.sh when --install is used
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install)
            DO_INSTALL=1
            ;;
        --no-check)
            DO_CHECK=0
            ;;
        --clean)
            DO_CLEAN=1
            ;;
        --release)
            shift
            RELEASE_OVERRIDE="${1:-}"
            if [[ -z "${RELEASE_OVERRIDE}" ]]; then
                echo "--release requires a value." >&2
                exit 1
            fi
            ;;
        --license)
            shift
            LICENSE_VALUE="${1:-}"
            if [[ -z "${LICENSE_VALUE}" ]]; then
                echo "--license requires a value." >&2
                exit 1
            fi
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

require_cmd() {
    local cmd="$1"
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "Required command not found: ${cmd}" >&2
        exit 1
    fi
}

require_cmd appstreamcli
require_cmd desktop-file-validate
require_cmd cargo
require_cmd cmake
require_cmd git
require_cmd ninja
require_cmd rpm
require_cmd rpmbuild
require_cmd rpmspec
require_cmd tar
if [[ ${DO_CHECK} -eq 1 ]]; then
    require_cmd ctest
fi

if [[ ${DO_CLEAN} -eq 1 ]]; then
    rm -rf "${RPM_TOPDIR}"
fi

mkdir -p "${RPM_TOPDIR}"/BUILD "${RPM_TOPDIR}"/BUILDROOT "${RPM_TOPDIR}"/RPMS \
    "${RPM_TOPDIR}"/SOURCES "${RPM_TOPDIR}"/SPECS "${RPM_TOPDIR}"/SRPMS

version="$(sed -n 's/^version = "\(.*\)"/\1/p' "${REPO_ROOT}/Cargo.toml" | head -n 1)"
if [[ -z "${version}" ]]; then
    echo "Failed to determine Cargo version from Cargo.toml" >&2
    exit 1
fi

if [[ -n "${RELEASE_OVERRIDE}" ]]; then
    release="${RELEASE_OVERRIDE}"
else
    timestamp="$(date -u +%Y%m%d%H%M%S)"
    short_sha="$(git -C "${REPO_ROOT}" rev-parse --short=12 HEAD 2>/dev/null || true)"
    if [[ -n "${short_sha}" ]]; then
        release="0.${timestamp}.git${short_sha}%{?dist}"
    else
        release="0.${timestamp}.local%{?dist}"
    fi
fi

# The newest commit's date, so rebuilding a commit gives the same changelog.
changelog_date="$(LC_ALL=C TZ=UTC git -C "${REPO_ROOT}" log -1 --date=format-local:'%a %b %d %Y' \
    --format=%cd 2>/dev/null || true)"
if [[ -z "${changelog_date}" ]]; then
    changelog_date="$(LC_ALL=C date -u +'%a %b %d %Y')"
fi

desktop-file-validate "${REPO_ROOT}/ui/ferrous.desktop"
appstreamcli validate --no-net "${REPO_ROOT}/ui/io.github.tiaxi.ferrous.metainfo.xml"

snapshot_root="${RPM_TOPDIR}/SOURCES/ferrous-${version}"
snapshot_tar="${RPM_TOPDIR}/SOURCES/ferrous-${version}.tar.gz"
rm -rf "${snapshot_root}" "${snapshot_tar}"
mkdir -p "${snapshot_root}"

mapfile -d '' -t repo_files < <(
    git -C "${REPO_ROOT}" ls-files -z --cached --modified --others --exclude-standard \
        | sort -zu
)

if [[ ${#repo_files[@]} -eq 0 ]]; then
    echo "No files selected for RPM source snapshot." >&2
    exit 1
fi

for relpath in "${repo_files[@]}"; do
    src_path="${REPO_ROOT}/${relpath}"
    if [[ ! -e "${src_path}" ]]; then
        continue
    fi
    dest_path="${snapshot_root}/${relpath}"
    mkdir -p "$(dirname "${dest_path}")"
    cp -a "${src_path}" "${dest_path}"
done

tar -C "${RPM_TOPDIR}/SOURCES" -czf "${snapshot_tar}" "ferrous-${version}"
rm -rf "${snapshot_root}"

vendor_stage="${RPM_TOPDIR}/SOURCES/ferrous-${version}-vendor"
vendor_tar="${RPM_TOPDIR}/SOURCES/ferrous-${version}-vendor.tar.gz"
rm -rf "${vendor_stage}" "${vendor_tar}"
mkdir -p "${vendor_stage}"
cargo vendor --quiet --locked --manifest-path "${REPO_ROOT}/Cargo.toml" "${vendor_stage}/vendor" >/dev/null
tar -C "${vendor_stage}" -czf "${vendor_tar}" vendor
rm -rf "${vendor_stage}"

rpm_defines=(
    --define "_topdir ${RPM_TOPDIR}"
    --define "ferrous_version ${version}"
    --define "ferrous_release ${release}"
    --define "ferrous_license ${LICENSE_VALUE}"
    --define "ferrous_changelog_date ${changelog_date}"
)

rpmspec -P "${SPEC_FILE}" "${rpm_defines[@]}" >/dev/null

rpmbuild_args=(
    -bb
    "${SPEC_FILE}"
    "${rpm_defines[@]}"
)

if [[ ${DO_CHECK} -eq 0 ]]; then
    rpmbuild_args+=(--nocheck)
fi

rpmbuild "${rpmbuild_args[@]}"

rpm_arch="$(rpm --eval '%{_arch}')"
rpm_path="${RPM_TOPDIR}/RPMS/${rpm_arch}/ferrous-${version}-${release}.${rpm_arch}.rpm"
if [[ ! -f "${rpm_path}" ]]; then
    rpm_path="$(find "${RPM_TOPDIR}/RPMS" -type f -name "ferrous-${version}-*.rpm" -print | sort | tail -1)"
fi

if [[ -z "${rpm_path}" || ! -f "${rpm_path}" ]]; then
    echo "RPM build completed but the output package could not be located." >&2
    exit 1
fi

echo "Built RPM: ${rpm_path}"

if [[ ${DO_INSTALL} -eq 1 ]]; then
    "${SCRIPT_DIR}/install-rpm.sh" "${rpm_path}"
fi
