#!/bin/bash
# neutral_ns.sh CMD...: runs CMD in a private user and mount namespace where the capture's work folder ($SHOT_WORK,
# resolved) is at /mnt/ferrous: a tmpfs over /mnt in that namespace only (nothing system-wide, no sudo), the work
# folder bind-mounted under it, the calling user again inside (a nested user namespace maps the uid back). The
# harness's paths into that folder (SHOT_APP, SHOT_OUT, SHOT_SRC, TMPDIR, and the folder itself as SHOT_NEUTRAL_WORK)
# are given as /mnt/ferrous/..., so what the app shows (folder rows, the art viewer's Folder row, the diagnostics log)
# names no home folder. Paths outside the work folder stay as they are. The user's real config, data and cache are
# not in the work folder.
set -e
WORKREAL=$(realpath -m "${SHOT_WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/ferrous-ui-capture}")
case "$WORKREAL" in
    /mnt|/mnt/*) echo "neutral_ns: the work folder $WORKREAL is under /mnt, which the namespace covers" >&2; exit 2;;
esac
mkdir -p "$WORKREAL"
N=/mnt/ferrous
for v in SHOT_APP SHOT_OUT SHOT_SRC TMPDIR; do
    val=${!v}
    [ -n "$val" ] || continue
    val=$(realpath -m "$val")
    case "$val" in "$WORKREAL"/*) export "$v=$N${val#"$WORKREAL"}";; esac
done
NS_UID=$(id -u)
NS_GID=$(id -g)
export SHOT_WORK="$WORKREAL" SHOT_NEUTRAL_WORK=$N NS_UID NS_GID
exec unshare --user --map-root-user --mount bash -c '
  set -e
  mount -t tmpfs -o size=1m,mode=755 ferrous-neutral /mnt
  mkdir /mnt/ferrous
  mount --bind "$0" /mnt/ferrous
  exec unshare --user --map-user="$NS_UID" --map-group="$NS_GID" "$@"
' "$WORKREAL" "$@"
