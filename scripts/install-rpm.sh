#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RPM_ROOT="${REPO_ROOT}/dist/rpm/RPMS"

usage() {
    cat <<USAGE
Usage: $(basename "$0") [rpm-path]

Install a built Ferrous RPM via dnf. If no path is provided, the newest local
RPM under dist/rpm/RPMS is installed.

Environment:
  FERROUS_DNF_CMD   Override the install command prefix (default: "sudo dnf"
                    for non-root users, "dnf" for root)
USAGE
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if ! command -v dnf >/dev/null 2>&1; then
    echo "dnf is required to install the RPM." >&2
    exit 1
fi

rpm_path="${1:-}"
if [[ -z "${rpm_path}" ]]; then
    rpm_path="$(find "${RPM_ROOT}" -type f -name 'ferrous-*.rpm' -printf '%T@ %p\n' \
        | sort -n \
        | tail -1 \
        | cut -d' ' -f2-)"
fi

if [[ -z "${rpm_path}" ]]; then
    echo "No Ferrous RPM found under ${RPM_ROOT}. Build one first." >&2
    exit 1
fi

if [[ ! -f "${rpm_path}" ]]; then
    echo "RPM not found: ${rpm_path}" >&2
    exit 1
fi

default_dnf_cmd="dnf"
if [[ "${EUID}" -ne 0 ]]; then
    default_dnf_cmd="sudo dnf"
fi
dnf_cmd="${FERROUS_DNF_CMD:-${default_dnf_cmd}}"

echo "Installing ${rpm_path}"
/bin/sh -lc "${dnf_cmd} install -y \"${rpm_path}\""
