#!/bin/bash
# Usage: iso.sh <isolation-dir> <command...>
# Runs a command with HOME, XDG_* and TMPDIR inside <isolation-dir>.
set -euo pipefail
dir="$1"; shift
mkdir -p "$dir"/{home,config,data,cache,tmp}
export HOME="$dir/home" XDG_CONFIG_HOME="$dir/config" XDG_DATA_HOME="$dir/data" XDG_CACHE_HOME="$dir/cache" TMPDIR="$dir/tmp"
# A persistent GStreamer registry, as a real user has, so that runs do not
# rebuild it on first use.
export GST_REGISTRY="${GST_REGISTRY:-$(cd "$(dirname "$0")/../.." && pwd)/target/perf/gst-registry.bin}"
exec "$@"
