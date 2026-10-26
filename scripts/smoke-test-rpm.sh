#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RPM_ROOT="${REPO_ROOT}/dist/rpm/RPMS"
FEDORA_RELEASE=""

usage() {
    cat <<USAGE
Usage: $(basename "$0") [--fedora VERSION] [rpm-path]

Install a built Ferrous RPM in a fresh Fedora container with podman, then check
the installed files, validate the desktop entry and AppStream metadata, check
that the executable's libraries resolve, and start Ferrous offscreen. If no
path is provided, the newest local RPM under dist/rpm/RPMS is tested.

Options:
  --fedora VERSION     Fedora image to use (default: the RPM's fcNN dist tag)
  -h, --help           Show this help
USAGE
}

rpm_path=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fedora)
            shift
            FEDORA_RELEASE="${1:-}"
            if [[ -z "${FEDORA_RELEASE}" ]]; then
                echo "--fedora requires a value." >&2
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
            rpm_path="$1"
            ;;
    esac
    shift
done

if ! command -v podman >/dev/null 2>&1; then
    echo "podman is required for the RPM smoke test." >&2
    exit 1
fi

if [[ -z "${rpm_path}" && -d "${RPM_ROOT}" ]]; then
    rpm_path="$(find "${RPM_ROOT}" -type f -name 'ferrous-[0-9]*.rpm' -printf '%T@ %p\n' \
        | sort -n | tail -1 | cut -d' ' -f2-)"
fi
if [[ -z "${rpm_path}" || ! -f "${rpm_path}" ]]; then
    echo "No Ferrous RPM found; build one with scripts/build-rpm.sh." >&2
    exit 1
fi
rpm_path="$(realpath "${rpm_path}")"
rpm_name="$(basename "${rpm_path}")"

if [[ -z "${FEDORA_RELEASE}" ]]; then
    if [[ "${rpm_name}" =~ \.fc([0-9]+)\. ]]; then
        FEDORA_RELEASE="${BASH_REMATCH[1]}"
    else
        echo "Cannot tell the Fedora release from ${rpm_name}; pass --fedora." >&2
        exit 1
    fi
fi

echo "Testing ${rpm_name} in fedora:${FEDORA_RELEASE}"

# The container starts without a desktop, so this also catches runtime QML
# modules that only a Plasma session would provide.
podman run --rm \
    -v "${rpm_path}:/rpm/${rpm_name}:ro,Z" \
    "fedora:${FEDORA_RELEASE}" \
    bash -euo pipefail -c '
        # Container images skip documentation unless tsflags is cleared.
        dnf -y -q --setopt=tsflags= install "/rpm/$1" appstream desktop-file-utils dbus-daemon
        for path in /usr/bin/ferrous \
            /usr/share/applications/ferrous.desktop \
            /usr/share/icons/hicolor/scalable/apps/ferrous.svg \
            /usr/share/metainfo/io.github.tiaxi.ferrous.metainfo.xml \
            /usr/share/licenses/ferrous/LICENSE \
            /usr/share/doc/ferrous/README.md; do
            if [[ ! -f "${path}" ]]; then
                echo "Missing installed file: ${path}" >&2
                exit 1
            fi
        done
        desktop-file-validate /usr/share/applications/ferrous.desktop
        appstreamcli validate --no-net /usr/share/metainfo/io.github.tiaxi.ferrous.metainfo.xml
        if ldd /usr/bin/ferrous | grep "not found"; then
            echo "ferrous links against missing libraries." >&2
            exit 1
        fi
        export XDG_RUNTIME_DIR=/tmp/xdg-runtime
        mkdir -m 700 -p "${XDG_RUNTIME_DIR}"
        status=0
        QT_QPA_PLATFORM=offscreen FERROUS_GST_OUTPUT_SINK=fakesink \
            dbus-run-session -- timeout 15 ferrous >/tmp/ferrous.log 2>&1 || status=$?
        # timeout exits with 124 when Ferrous is still running.
        if [[ ${status} -ne 124 ]]; then
            echo "Ferrous exited during startup with status ${status}:" >&2
            cat /tmp/ferrous.log "${HOME}/.local/share/ferrous/diagnostics.log" >&2 || true
            exit 1
        fi
        echo "Ferrous started and kept running."
    ' smoke-test "${rpm_name}"
