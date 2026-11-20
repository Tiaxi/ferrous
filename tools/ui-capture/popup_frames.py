#!/usr/bin/env python3
"""Draws a main-window strip's menus into its frames (#664), as a main-window grab draws them in.

Menus are windows of their own (#642), so the probe records each menu window's frames whole (with alpha) beside the
main window's, and notes in frames.tsv when each of the main window's menus shows, moves and hides ("popup" lines,
its offset from the main window in logical px). `merge` turns these into main-window frames only: one frame at every
frame either window rendered and at every hide, each the main window's latest frame with every shown menu's latest
frame drawn in at its place. The menu windows' own frame files are then removed, so the pack's privacy painting
(whose item trace has the menus' items at the same place) covers every recorded pixel of them.

Coordinates: the frames are device px (the session's scale), cut to the recording's crop (logical px) and scaled by
its `scale=F` option. A menu that reaches past the crop is cut there; with no crop, the frames grow right and down
for it, mid grey outside the window (as a grab does)."""
import os
import re

from PIL import Image

POPUP_BACKDROP = (0x80, 0x80, 0x80)  # (shotprobe.cpp's POPUP_BACKDROP)


def frame_scale(opts):
    """The downscale of a strip's frames, read from its options as the probe reads it: the last `scale=F`, only
    0 < F < 1, anything else none."""
    f = 1.0
    for o in opts.split(" "):
        if o.startswith("scale="):
            # (QString::toDouble's syntax; anything else reads as 0, where Python's float would take "0.2_5")
            f = float(o[6:]) if re.fullmatch(r"[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?", o[6:]) else 0.0
    return f if 0 < f < 1 else 1.0


def read_tsv(d):
    """frames.tsv split into its frame lines (dicts), popup lines (dicts), menu window indices and its lines other
    than frames (the popup lines too)."""
    frames, popups, popup_wins, other = [], [], set(), []
    with open(os.path.join(d, "frames.tsv"), encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    for line in lines:
        f = line.split("\t")
        if f[0] == "frame":
            frames.append(dict(i=int(f[1]), t=float(f[2]), win=int(f[3]), file=f[4]))
        elif f[0] == "popup":
            x, y = (int(v) for v in f[4].split(","))
            popups.append(dict(t=float(f[1]), win=int(f[2]), shown=f[3] == "1", off=(x, y), main=int(f[5]),
                               depth=int(f[6])))
            other.append(line)  # (kept: where each menu was)
        else:
            if f[0] == "window" and len(f) > 6 and f[6] == "popup":
                popup_wins.add(int(f[1]))
            other.append(line)
    return frames, popups, popup_wins, other


def plan(frames, popups, main):
    """The merged frames: [(t, main frame file, [(popup frame file, (x, y) logical offset), ...])], the menus in
    chain order. A menu's frame is drawn while that menu is shown and until another of its frames comes; a hide
    makes a frame of its own (the main window does not render for a menu that closes)."""
    events = [(p["t"], 0, k, p) for k, p in enumerate(popups) if p["main"] == main]
    events += [(f["t"], 1, k, f) for k, f in enumerate(frames)]
    events.sort(key=lambda e: e[:3])  # (a show before a frame at the same time, then in file order)
    shown = {}  # menu window -> dict(off, depth, file)
    cur, out = None, []

    def emit(t):
        if cur is not None:
            pops = sorted((s for s in shown.values() if s["file"]), key=lambda s: s["depth"])
            out.append((t, cur, [(s["file"], s["off"]) for s in pops]))

    for t, kind, _, ev in events:
        if kind == 0:
            if ev["shown"]:
                was = shown.get(ev["win"])
                shown[ev["win"]] = dict(off=ev["off"], depth=ev["depth"], file=was["file"] if was else None)
                if was and was["file"] and was["off"] != ev["off"]:
                    emit(t)  # it moved
            elif (shown.pop(ev["win"], None) or {}).get("file"):
                emit(t)  # it closed: the main window's frame without it
        elif ev["win"] == main:
            cur = ev["file"]
            emit(t)
        elif ev["win"] in shown:
            shown[ev["win"]]["file"] = ev["file"]
            emit(t)
    return out


def merge(d, dpr=1.0, crop=None, frame_scale=1.0):
    """Rewrites the recording in `d` as main-window frames with its menus drawn in. Returns the menu windows drawn
    in (window indices: those with pixels in a frame), [] when there were none; any menu window's own frames are
    removed either way."""
    frames, popups, popup_wins, other = read_tsv(d)
    popup_wins |= {p["win"] for p in popups}
    if not any(f["win"] in popup_wins for f in frames):
        return []  # no menu rendered while it recorded: the recording stays as it was
    main = min((p["main"] for p in popups), default=0)
    merged = plan(frames, popups, main)
    win_of, drawn = {f["file"]: f["win"] for f in frames}, set()
    k = dpr * frame_scale
    ox, oy = (crop or (0, 0, 0, 0))[:2]
    sizes = {}

    def size(file):
        if file not in sizes:
            with Image.open(os.path.join(d, file)) as im:
                sizes[file] = im.size
        return sizes[file]

    def at(off):  # a menu's offset (logical, from the main window) in the frame (device px, from the crop)
        return round((off[0] - ox) * k), round((off[1] - oy) * k)

    cw, ch = size(merged[0][1]) if merged else (0, 0)
    if not crop:  # the frames grow right and down for a menu past the window, as a grab does
        for _t, _m, pops in merged:
            for pf, off in pops:
                x, y = at(off)
                cw, ch = max(cw, x + size(pf)[0]), max(ch, y + size(pf)[1])
    out_frames, cache = [], {}
    for n, (t, mf, pops) in enumerate(merged):
        if not pops and size(mf) == (cw, ch):
            out_frames.append((t, mf))  # the main window's frame as it was
            continue
        canvas = Image.new("RGBA", (cw, ch), POPUP_BACKDROP + (255,))
        with Image.open(os.path.join(d, mf)) as im:
            canvas.paste(im.convert("RGBA"), (0, 0))
        cache = {pf: cache[pf] for pf, _o in pops if pf in cache}  # (each menu's latest frame only)
        for pf, off in pops:
            if pf not in cache:
                with Image.open(os.path.join(d, pf)) as im:
                    cache[pf] = im.convert("RGBA")
            if _composite(canvas, cache[pf], at(off)):
                drawn.add(win_of[pf])
        name = f"m{n:04d}_w{main}.png"
        canvas.convert("RGB").save(os.path.join(d, name), compress_level=1)  # (the probe's PNG level)
        out_frames.append((t, name))
    keep = [f for f in frames if f["win"] != main and f["win"] not in popup_wins]  # another window's: as they were
    lines = [f"frame\t{i}\t{t:.3f}\t{main}\t{file}" for i, (t, file) in enumerate(out_frames)]
    lines += [f"frame\t{len(out_frames) + i}\t{f['t']:.3f}\t{f['win']}\t{f['file']}" for i, f in enumerate(keep)]
    tmp = os.path.join(d, "frames.tsv.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("".join(ln + "\n" for ln in lines + other))
    os.replace(tmp, os.path.join(d, "frames.tsv"))
    used = {file for _t, file in out_frames} | {f["file"] for f in keep}
    for f in frames:
        if f["file"] not in used:
            os.remove(os.path.join(d, f["file"]))  # a menu's own frame, drawn in where it was shown
    return sorted(drawn)


def _composite(canvas, im, xy):
    """Draws `im` over the canvas at xy, which may be left of or above it (a menu the crop cuts); returns whether
    any of its pixels (not its transparent margin) are on the canvas."""
    x, y = xy
    sx, sy = max(0, -x), max(0, -y)
    ex, ey = min(im.width, canvas.width - x), min(im.height, canvas.height - y)
    if sx >= ex or sy >= ey:
        return False  # none of it is on the canvas
    part = im.crop((sx, sy, ex, ey))
    if part.getchannel("A").getbbox() is None:
        return False  # only its transparent margin is
    canvas.alpha_composite(part, (max(0, x), max(0, y)))
    return True
