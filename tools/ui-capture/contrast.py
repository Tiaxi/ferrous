#!/usr/bin/env python3
"""WCAG contrast ratios sampled from rendered pixels. Usage: contrast.py <round-dir>

Reads <round>/contrast/*.json (written by the runner's `measure` macro: each item's rect in logical px,
its declared colour and palette role) and the matching shots, and writes <round>/CONTRAST.md.

Sampling, per measured item:
- background = the most frequent exact colour inside the item's rect (the row tint, the menu, the chrome);
- foreground = the pixel inside the rect with the highest contrast against that background; for text,
  the core of a glyph stem, which antialiasing does not lighten; for a focus ring, only a band along the
  rect's edge (2 px outside to 3 px inside) is searched, so an icon inside a button cannot stand in;
- seek waveform: the rect is the waveform left (played) or right (unplayed) of the playhead; the
  background is the chrome between the peaks.
The declared colour (QML `color`, or `border.color` of a ring) is also checked against the sampled
background, composited over it when translucent.
"""
import glob
import json
import os
import sys
from collections import Counter

from PIL import Image


def lin(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lum(rgb):
    r, g, b = rgb[:3]
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def ratio(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def hexc(c):
    return "#%02x%02x%02x" % tuple(c[:3])


def parse_qcolor(s, bg):
    """'#rrggbb' or '#aarrggbb' (Qt's string for translucent colours), composited over bg."""
    if not s or not s.startswith("#"):
        return None
    s = s[1:]
    a = 255
    if len(s) == 8:
        a, s = int(s[:2], 16), s[2:]
    r, g, b = int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16)
    k = a / 255.0
    return tuple(round(c * k + bc * (1 - k)) for c, bc in zip((r, g, b), bg[:3])), a


def mode_in(img, rect, sc):
    x, y, w, h = [round(v * sc) for v in rect]
    crop = img.crop((max(0, x), max(0, y), min(img.width, x + w), min(img.height, y + h)))
    return Counter(getattr(crop, "get_flattened_data", crop.getdata)()).most_common(1)[0][0]


def sample(img, rect, kind, sc, declared=None, ref_rect=None):
    x, y, w, h = [round(v * sc) for v in rect]
    x0, y0 = max(0, x), max(0, y)
    x1, y1 = min(img.width, x + w), min(img.height, y + h)
    if x1 - x0 < 2 or y1 - y0 < 2:
        return None
    crop = img.crop((x0, y0, x1, y1))
    px = list(getattr(crop, "get_flattened_data", crop.getdata)())
    bg = Counter(px).most_common(1)[0][0]
    if kind == "fill":  # a tint's own colour (the rect's mode) against a reference rect's (e.g. a plain row)
        fg = bg
        bg = mode_in(img, ref_rect, sc) if ref_rect else bg
        return dict(fg=fg, bg=bg, ratio=ratio(fg, bg), ink=0, box=[x0, y0, x1 - x0, y1 - y0])
    if kind == "ring":  # a band along the edge: 2 px outside to 3 px inside
        bx0, by0, bx1, by1 = max(0, x - 2), max(0, y - 2), min(img.width, x + w + 2), min(img.height, y + h + 2)
        band = []
        for yy in range(by0, by1):
            for xx in range(bx0, bx1):
                if min(xx - (x - 2), (x + w + 2) - 1 - xx, yy - (y - 2), (y + h + 2) - 1 - yy) < 5:
                    band.append(img.getpixel((xx, yy)))
        cands = band
        # the background just outside the ring: 3-4 px beyond each edge that is inside the image (an edge at the
        # image's border has no outside: a clamped sample would be the ring itself: a false 1.00:1)
        out = []
        for yy in (y - 4, y - 3, y + h + 2, y + h + 3):
            if 0 <= yy < img.height:
                out += [img.getpixel((xx, yy)) for xx in range(max(0, x + 4), min(img.width, x + w - 4))]
        for xx in (x - 4, x - 3, x + w + 2, x + w + 3):
            if 0 <= xx < img.width:
                out += [img.getpixel((xx, yy)) for yy in range(max(0, y + 4), min(img.height, y + h - 4))]
        bg = Counter(out).most_common(1)[0][0] if out else bg
    else:
        cands = px
    if kind == "ring" and declared:  # the ring's own pixels: those closest to its declared colour
        dc = parse_qcolor(declared, bg)  # translucent colours composited over the sampled background
        declared = dc[0] if dc else None
    if kind == "ring" and declared and sum((a - b) ** 2 for a, b in zip(bg, declared)) < 12 ** 2:
        raise RuntimeError(f"ring at {rect}: the background sampled outside it is the ring's own colour {hexc(bg)}")
    if kind == "ring" and declared:
        fg = min(cands, key=lambda c: sum((a - b) ** 2 for a, b in zip(c, declared)))
        if sum((a - b) ** 2 for a, b in zip(fg, declared)) > 40 ** 2:
            return dict(fg=fg, bg=bg, ratio=ratio(fg, bg), ink=0, box=[x0, y0, x1 - x0, y1 - y0], missing=True)
    else:
        fg = max(cands, key=lambda c: ratio(c, bg))
    ink = sum(1 for c in px if ratio(c, bg) > 1.5) / len(px)
    return dict(fg=fg, bg=bg, ratio=ratio(fg, bg), ink=ink, box=[x0, y0, x1 - x0, y1 - y0])


def verdict(kind, r):
    if kind == "tint":
        return "a tint, not a WCAG requirement (shown for its visibility)"
    if kind == "text":
        return "AA" if r >= 4.5 else ("AA large only" if r >= 3 else "fails AA")
    return "meets 3:1 (non-text)" if r >= 3 else "below 3:1 (non-text)"


WHAT = {
    "menu-disabled-text": "Disabled menu item text (Edit menu, empty queue)",
    "menu-enabled-text": "Enabled menu item text (same menu)",
    "queue-row-primary": "Queue title (primary text) on the view",
    "queue-row-secondary": "Queue artist (secondary text) on the view",
    "queue-row-number": "Queue track number on the view",
    "queue-row-selected": "Queue title on the selection tint",
    "queue-row-hover": "Queue title on the hover tint",
    "queue-playing-selected": "Playing row title (amber) on the selection tint",
    "queue-header-totals": "Queue header totals",
    "library-row-primary": "Library row title on the window/sidebar colour",
    "library-header-totals": "Library header totals",
    "now-playing-text": "Now-playing text block (strongest line)",
    "status-bar-state": "Status bar playback state",
    "status-bar-format": "Status bar format text",
    "seek-played": "Seek waveform, played part, against the chrome",
    "seek-unplayed": "Seek waveform, unplayed part, against the chrome",
    "plot-lane-label": "Plot plate text (lane label) on the plot",
    "plot-lane-mute": "Plot plate button text (M) on the plot",
    "focus-ring-seek": "Seek bar focus ring against its surroundings",
    "focus-ring-volume": "Volume slider focus ring",
    "focus-ring-transport-button": "Transport button focus frame (style)",
    "queue-hover-tint": "Queue hover tint against the view (fill colour vs a plain row)",
    "library-focus-frame": "Library current-row focus frame (1 px focusRing border)",
    "scrobble-bar-listening-0": "Scrobble indicator's bar while listening, 0 % (at least 3 px), against the status bar",
    "scrobble-trough": "Scrobble indicator's trough (behind the listening bar) against the status bar",
    "scrobble-bar-listening-25": "Scrobble indicator's bar while listening, 25 %, against the status bar",
    "scrobble-bar-listening": "Scrobble indicator's bar while listening (50 %) against the status bar",
    "scrobble-bar-done": "Scrobble indicator's bar when scrobbled (none by design)",
    "scrobble-glyph-done": "Last.fm glyph when scrobbled (its own colour), against the status bar",
}


def main():
    rd = os.path.abspath(sys.argv[1])
    rows = {}
    for p in sorted(glob.glob(os.path.join(rd, "contrast", "*.json"))):
        d = json.load(open(p))
        shot = os.path.join(rd, "shots", d["shot"])
        if not os.path.exists(shot):
            continue
        img = Image.open(shot).convert("RGB")
        for m in d["measures"]:
            # rows that are not a sample: n/a by design, an item not shown, a bar under its minimum width
            ex = m.get("extra") or {}
            special = None
            if m["kind"] == "na":
                special = ("n/a", m.get("reason", "not applicable"))
            elif ex.get("min_w") and (m.get("shown") is False or m["rect"][2] < ex["min_w"]):
                special = ("**fails**", ("not drawn" if m.get("shown") is False else f"{m['rect'][2]:.1f} px wide")
                           + f", under its {ex['min_w']} px minimum"
                           + (f" (progress {ex['progress']:.2f})" if isinstance(ex.get("progress"), (int, float)) else ""))
            elif m.get("shown") is False:
                special = ("not shown", "the item is not visible at grab time")
            if special:
                rows.setdefault(m["label"], {})[d["theme"]] = dict(m=m, special=special, shot=d["shot"])
                continue
            # a glyph is sampled as text is (its strongest pixel against the rect's background), judged as non-text
            kind = m["kind"] if m["kind"] in ("ring", "fill") else ("fill" if m["kind"] == "bar" else "text")
            s = sample(img, m["rect"], kind, d.get("scale", 1), m.get("declared") or None, m.get("ref_rect"))
            if not s:
                continue
            dec = parse_qcolor(m.get("declared", ""), s["bg"])
            rows.setdefault(m["label"], {})[d["theme"]] = dict(m=m, s=s, dec=dec, shot=d["shot"])
    L = ["# Contrast (WCAG 2.x) sampled from rendered pixels\n",
         f"Round `{os.path.basename(rd)}`. Every ratio is computed from the full-resolution shots in `shots/` "
         "(Breeze Dark and Breeze Light, 1600×1000 at scale 1), not from palette values. Thresholds: text 4.5:1 (AA), "
         "3:1 for large text; non-text (focus rings, the waveform) 3:1 (WCAG 1.4.11).\n",
         "**How it was sampled.** The harness records the item's rect at grab time (`measure` in `states.py`). "
         "*Background* = the most frequent exact colour in that rect (the row tint, the menu, the chrome). "
         "*Foreground* = the pixel in the rect with the highest contrast against it, i.e. the core of a glyph stem, which "
         "antialiasing does not lighten (so small text is not under-reported). Focus rings: only a band along the "
         "rect's edge (2 px outside to 3 px inside) is searched, and the background is taken just outside the ring. "
         "Seek waveform: the waveform's rect left (played) or right (unplayed) of the playhead; the background is "
         "the chrome between the peaks. Tints (kind fill): the most frequent colour of the tinted row against that of a "
         "plain row. Translucent declared colours (the scrobble ring's track) are composited over the sampled background "
         "before the ring's pixels are matched. *Declared* is the item's QML colour (composited over the sampled background "
         "when translucent) with its `uiPalette` role, as a cross-check; it differs from the sample where the "
         "item draws in another colour than its `color` (a style-drawn label, opacity, a layer).\n",
         "| element | theme | foreground (sampled) | background (sampled) | ratio | verdict | declared colour (role) → ratio | where |",
         "|---|---|---|---|---|---|---|---|"]
    order = list(WHAT) + sorted(set(rows) - set(WHAT))
    for label in order:
        if label not in rows:
            continue
        for theme in ("dark", "light"):
            x = rows[label].get(theme)
            if not x:
                continue
            if x.get("special"):
                v, why = x["special"]
                L.append(f"| {WHAT.get(label, label)} | {theme} | — | — | {v} | {why} | — | `shots/{x['shot']}` |")
                continue
            m, s, dec = x["m"], x["s"], x["dec"]
            kind = "text" if m["kind"] == "text" else ("tint" if m["kind"] == "fill" else "ui")  # bar: sampled as a fill, 3:1
            dtxt = "—"
            if dec:
                (dc, alpha) = dec
                dtxt = (f"{m['declared']} ({m['role'] or 'no palette role'}{', translucent' if alpha < 255 else ''}) → "
                        f"{ratio(dc, s['bg']):.2f}")
            if s.get("missing"):
                L.append(f"| {WHAT.get(label, label)} | {theme} | not found | {hexc(s['bg'])} | — | the ring's declared "
                         f"colour is not on screen (not drawn at grab time) | {dtxt} | `shots/{x['shot']}` px {s['box']} |")
                continue
            L.append(f"| {WHAT.get(label, label)} | {theme} | {hexc(s['fg'])} | {hexc(s['bg'])} | **{s['ratio']:.2f}** | "
                     f"{verdict(kind, s['ratio'])} | {dtxt} | `shots/{x['shot']}` px {s['box']} |")
    L.append("\nRaw measurements (rects, declared colours, item text): `contrast/*.json`.\n")
    out = os.path.join(rd, "CONTRAST.md")
    if not sum(len(v) for v in rows.values()):
        # a run without the contrast session (a focus build) measures nothing: no header-only CONTRAST.md (its
        # own earlier one, from this run's folder, goes too); the focus facts carry their scope's contrast instead
        if os.path.exists(out) and open(out, encoding="utf-8").read(64).startswith("# Contrast"):
            os.remove(out)
        print("CONTRAST.md: no measurements in this run, not written")
        return
    open(out, "w").write("\n".join(L) + "\n")
    print(f"CONTRAST.md: {sum(len(v) for v in rows.values())} measurements")


if __name__ == "__main__":
    main()
