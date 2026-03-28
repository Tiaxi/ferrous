#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

#
# find-unusual-flac.sh — find albums containing FLAC files with non-standard
# block sizes and optionally re-encode them in-place with modern settings.
#
# Usage:
#   find-unusual-flac.sh <music-directory> [--reencode] [--expected-block=N]
#
# Options:
#   --reencode            Prompt to re-encode each album's FLACs in-place
#   --expected-block=N    Block size considered standard (default: 4096)

set -euo pipefail

MUSIC_DIR=""
REENCODE=false
EXPECTED_BLOCK=4096

for arg in "$@"; do
    case "$arg" in
        --reencode)       REENCODE=true ;;
        --expected-block=*) EXPECTED_BLOCK="${arg#*=}" ;;
        -*)               echo "Unknown option: $arg" >&2; exit 1 ;;
        *)                MUSIC_DIR="$arg" ;;
    esac
done

if [[ -z "$MUSIC_DIR" ]]; then
    echo "Usage: $0 <music-directory> [--reencode] [--expected-block=N]" >&2
    exit 1
fi

if ! command -v metaflac &>/dev/null; then
    echo "Error: metaflac not found (install flac package)" >&2
    exit 1
fi

if ! command -v flac &>/dev/null; then
    echo "Error: flac not found" >&2
    exit 1
fi

# Collect albums (directories) with unusual FLAC block sizes.
# Sample one FLAC per directory — if it has a non-standard block size,
# assume the whole album does and collect all FLACs in that directory.
declare -A album_files
declare -A album_info

scanned=0
matched=0

while IFS= read -r -d '' album_dir; do
    scanned=$((scanned + 1))
    printf '\rScanning: %d albums checked, %d with non-standard blocks...' "$scanned" "$matched" >&2

    # Sample one FLAC from this directory
    sample=$(find "$album_dir" -maxdepth 1 -name '*.flac' -print -quit 2>/dev/null)
    [[ -z "$sample" ]] && continue

    block_size=$(metaflac --show-min-blocksize "$sample" 2>/dev/null) || continue
    [[ "$block_size" -eq "$EXPECTED_BLOCK" ]] && continue

    matched=$((matched + 1))
    encoder=$(metaflac --show-vendor-tag "$sample" 2>/dev/null || echo "unknown")
    album_info["$album_dir"]="block=$block_size  encoder=$encoder"

    while IFS= read -r -d '' flac_file; do
        album_files["$album_dir"]+=$'\n'"$flac_file"
    done < <(find "$album_dir" -maxdepth 1 -name '*.flac' -print0 | sort -z)
done < <(find "$MUSIC_DIR" -type f -name '*.flac' -printf '%h\0' | sort -zu)

# Clear the progress line
printf '\r%*s\r' 80 '' >&2

if [[ ${#album_info[@]} -eq 0 ]]; then
    echo "Scanned $scanned album(s). No FLAC files with unusual block sizes found (expected: $EXPECTED_BLOCK)."
    exit 0
fi

echo "Scanned $scanned album(s). Found ${#album_info[@]} with non-standard FLAC block sizes:"
echo ""

reencode_album() {
    local album_dir="$1"
    local files="$2"
    local failed=0
    local count=0
    local total
    total=$(echo "$files" | grep -c '[^[:space:]]')

    while IFS= read -r flac_file; do
        [[ -z "$flac_file" ]] && continue
        count=$((count + 1))
        local basename
        basename=$(basename "$flac_file")
        local tmpfile="${flac_file}.reencoding.tmp"

        printf "  [%d/%d] %s ... " "$count" "$total" "$basename"

        if flac --best --verify --silent -o "$tmpfile" "$flac_file" 2>/dev/null; then
            mv "$tmpfile" "$flac_file"
            echo "OK"
        else
            echo "FAILED"
            rm -f "$tmpfile"
            failed=$((failed + 1))
        fi
    done <<< "$files"

    if [[ $failed -gt 0 ]]; then
        echo "  Warning: $failed file(s) failed to re-encode."
    fi
}

while IFS= read -r -d '' album_dir; do
    info="${album_info["$album_dir"]}"
    files="${album_files["$album_dir"]}"
    file_count=$(echo "$files" | grep -c '[^[:space:]]')

    echo "--- $album_dir"
    echo "    $info  ($file_count file(s))"

    if $REENCODE; then
        printf "    Re-encode this album? [y/N/q] "
        read -r answer </dev/tty
        case "$answer" in
            y|Y) reencode_album "$album_dir" "$files" ;;
            q|Q) echo "Quitting."; exit 0 ;;
            *)   echo "    Skipped." ;;
        esac
        echo ""
    fi
done < <(printf '%s\0' "${!album_info[@]}" | sort -zu)

if ! $REENCODE; then
    echo ""
    echo "Run with --reencode to interactively re-encode albums."
fi
