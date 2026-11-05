#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Collects the RPMs that the release workflow downloaded into one directory of
# release assets and writes SHA256SUMS for them, so that `sha256sum -c
# SHA256SUMS` works in a directory of downloaded assets.
#
# The RPMs are found at any depth: actions/download-artifact puts each
# artifact in a directory of its own only when it downloads more than one, and
# extracts a lone artifact straight into the download directory.

set -euo pipefail

usage() {
    cat <<USAGE
Usage: $(basename "$0") [--require-dist DIST]... DOWNLOAD_DIR ASSETS_DIR

Moves every *.rpm under DOWNLOAD_DIR into ASSETS_DIR, which must not exist
yet, and writes ASSETS_DIR/SHA256SUMS. Fails when no RPM is found, when two
RPMs have the same file name, or when no RPM has a required distribution tag.

Options:
  --require-dist DIST  Require an RPM whose file name has the distribution
                       tag DIST, for example fc44 for ferrous-1.0.0-1.fc44.x86_64.rpm
  -h, --help           Show this help
USAGE
}

required_dists=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --require-dist)
            if [[ -z "${2:-}" ]]; then
                echo "--require-dist requires a value." >&2
                exit 2
            fi
            required_dists+=("$2")
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            break
            ;;
    esac
    shift
done

if [[ $# -ne 2 ]]; then
    usage >&2
    exit 2
fi
download_dir="$1"
assets_dir="$2"

if [[ ! -d "${download_dir}" ]]; then
    echo "${download_dir} is not a directory." >&2
    exit 1
fi
if [[ -e "${assets_dir}" ]]; then
    echo "${assets_dir} already exists." >&2
    exit 1
fi

mapfile -d '' -t rpms < <(find "${download_dir}" -type f -name '*.rpm' -print0 | LC_ALL=C sort -z)
if [[ ${#rpms[@]} -eq 0 ]]; then
    echo "No packages to release in ${download_dir}." >&2
    exit 1
fi

# Check every name before moving anything, so a failure leaves the downloads
# as they were.
declare -A sources_by_name=()
for rpm in "${rpms[@]}"; do
    name="$(basename "${rpm}")"
    if [[ -n "${sources_by_name[${name}]:-}" ]]; then
        echo "Two packages are named ${name}: ${sources_by_name[${name}]} and ${rpm}." >&2
        exit 1
    fi
    sources_by_name["${name}"]="${rpm}"
done

for dist in "${required_dists[@]}"; do
    found=0
    for name in "${!sources_by_name[@]}"; do
        if [[ "${name}" == *."${dist}".*.rpm ]]; then
            found=1
            break
        fi
    done
    if [[ ${found} -eq 0 ]]; then
        echo "No ${dist} package to release; that build is required." >&2
        exit 1
    fi
done

mkdir -p "${assets_dir}"
for rpm in "${rpms[@]}"; do
    mv -- "${rpm}" "${assets_dir}/$(basename "${rpm}")"
done

cd "${assets_dir}"
mapfile -d '' -t names < <(printf '%s\0' "${!sources_by_name[@]}" | LC_ALL=C sort -z)
sha256sum -- "${names[@]}" >SHA256SUMS
sha256sum --check --strict --quiet SHA256SUMS
cat SHA256SUMS
