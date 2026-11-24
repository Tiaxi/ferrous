#!/usr/bin/env python3
"""Packages a round for reviewers whose viewers downscale every image to ~1568 px on the long side.

Usage: pack.py <round-dir> <source-dir> [--geometry <geometry-round-dir>] [--jobs N]

- Contact sheets (sheets/): tiles cropped to what each state is about (the popup, dialog or focused
  item from the geometry recorded at grab time, else the area's band from states.SHEET_CROPS_PX), dark
  and light on separate sheets, at most 6 tiles, laid out so every tile is >= 700 px wide after a
  1568 px downscale and no sheet exceeds 2400 px.
- Transitions (strips/): one merged image per recording (motion_strip.py: caption, key frames, graph,
  perceived-at-real-speed rows), cropped to the region that changes; its key-frame pages (8-12 key frames
  with real timestamps) in strips/frames/. Dark only, plus light for states.STRIP_BOTH_THEMES. All frames
  stay in strips-raw/.
- Core sheets: the overview, states.COMPARE_SHEETS, states.PICK_SHEETS and the unscaled ring sheet.
- INDEX.md with the core set (states.CORE, about 20 images) at the top, the rest as reference, and the
  Not captured list; MOTION-METRICS.md.
"""
import argparse
import collections
from collections import Counter
import datetime
import fnmatch
import glob
import json
import math
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

from PIL import Image, ImageChops, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("SHOT_OUT", "/nonexistent/shots")
os.environ.setdefault("SHOT_APP", "x")
if len(sys.argv) > 2 and os.path.isdir(sys.argv[2]):  # the round's source: states read Configure's pages from it
    os.environ.setdefault("SHOT_SRC", sys.argv[2])
import states as st  # noqa: E402
from colours import unique_colours  # noqa: E402
from focus_select import packs_apart, tile_matches  # noqa: E402
from framestore import FrameStore  # noqa: E402

FONT = "/usr/share/fonts/google-noto/NotoSans-Regular.ttf"
FONT_B = "/usr/share/fonts/google-noto/NotoSans-Bold.ttf"
REVIEW = 1568      # reviewers' long-side downscale
MIN_TILE = 700     # tile width after that downscale
MAX_SIDE = 2400
PAD = 14
GROUP_ORDER = ["menubar", "scrobblering", "main", "plot", "transport", "nowplaying", "queue", "library", "statusbar", "menu", "dialog",
               "search", "viewer", "icon", "iconab", "animoff", "focus", "narrow", "hidpi", "bigfont:main", "bigfont:transport",
               "bigfont:statusbar", "bigfont:search", "bigfont:dialog", "bigfont:menu", "bigfont:viewer", "bigfont:focus",
               "narrow-bigfont"]
GROUP_TITLES = dict(main="Main window", plot="Spectrogram/waveform pane", transport="Transport, seek bar, volume",
                    nowplaying="Now playing", queue="Queue", library="Library", statusbar="Status bar",
                    menu="Menus (menu bar and context)", dialog="Dialogs", search="Search palette",
                    viewer="Viewers", icon="App icon", focus="Keyboard focus walk",
                    narrow="Narrow window 1000×720", hidpi="HiDPI scale 2",
                    iconab=f"App icon: {st.ICON_A_LABEL} vs {st.ICON_B_LABEL}",
                    menubar="Menu bar, open and hovered", scrobblering="Scrobble indicator, every state", animoff="Animations off (AnimationDurationFactor=0)",
                    **{"narrow-bigfont": "Narrow window 1000×720 at a large font (Noto Sans 24 pt)"})
AREA_GROUP = dict(seek="transport", volume="transport")
FULL_AREAS = {"main", "viewer", "icon", "iconab", "animoff", "scrobblering", "closeup", "onetoone"}
STRIP_SIZE = (1200, 1568)  # a recording's merged image: its width and most height
HALF_SIZE = ((REVIEW - PAD) // 2, REVIEW - 78)  # a pair's half: narrower, and room for the pair's title
SKIP_AREAS = {"contrast"}  # measurement shots: in shots/ and CONTRAST.md, not on sheets


def font(bold, size):
    return ImageFont.truetype(FONT_B if bold else FONT, size)


# ------------------------------------------------------------------ layout
# The core set's sheets (states.CORE): small enough that ~20 images fit a reviewer's context, never downscaled.
CORE_MAX_W, CORE_MAX_H, CORE_MIN_TILE = 1240, 1568, 560
# The core's weight: a reviewer's image input (e.g. a vision model's) downscales anything over ~1.15 MP, and an image costs
# context in proportion to its pixels up to that. So: a 1:1 core image must fit 1.15 MP as drawn (else it is not
# seen at 1:1, and the check fails); any other core image (overviews, picks, merged transitions) gets a core copy
# fitted to 1.0 MP. INDEX.md states each core image's megapixels and the total.
CORE_MP, CORE_MP_HARD = 1.0e6, 1.15e6
CORE_MP_PICK = 0.8e6  # the overview and pick sheets: tiles downscaled anyway, the lightest
FOCUS_MAX_IMAGES = 14  # a focus pack's images (incl. the contact sheets of "neither" frames) that one review holds


def fit(tiles, cols, tw, max_tiles, label_h, min_tile=MIN_TILE, max_w=MAX_SIDE, max_h=MAX_SIDE):
    """How many of `tiles` fit one page at this column count and tile width."""
    W = cols * tw + PAD * (cols + 1)
    if W > max_w:
        return 0
    H, row_h, k = 82 + PAD, 0, 0
    for i, (w, h, _) in enumerate(tiles[:max_tiles]):
        th = int(h * tw / w) + label_h
        if i % cols == 0:
            H += row_h + (PAD if i else 0)
            row_h = 0
        row_h = max(row_h, th)
        Hn = H + row_h + PAD
        f = min(1.0, REVIEW / max(W, Hn))
        if Hn > max_h or tw * f < min_tile:
            break
        k = i + 1
    return k


def best_page(tiles, max_tiles, label_h, min_tile=MIN_TILE, max_w=MAX_SIDE, max_h=MAX_SIDE):
    best = None
    maxw = max(t[0] for t in tiles[:max_tiles])
    for cols in ((8, 7, 6, 5, 4, 3, 2, 1) if min_tile < CORE_MIN_TILE else (3, 2, 1)):
        top = min(maxw, (max_w - PAD * (cols + 1)) // cols)
        for tw in range(top, max(1, int(min(min_tile, maxw) * 0.98)) - 1, -30):
            k = fit(tiles, cols, tw, max_tiles, label_h, min_tile, max_w, max_h)
            if k:
                score = (k, tw)
                if best is None or score > best[0]:
                    best = (score, cols, tw, k)
    return best


def layout(tiles, max_tiles, label_h, min_tile=MIN_TILE, max_w=MAX_SIDE, max_h=MAX_SIDE):
    """Splits `tiles` (list of (w, h, payload)) into balanced pages. Returns [(cols, tw, [tiles])]."""
    pages, rest = [], list(tiles)
    while rest:
        best = best_page(rest, max_tiles, label_h, min_tile, max_w, max_h)
        if best is None:  # a tile that cannot meet the bar: alone, as large as allowed
            pages.append((1, min(rest[0][0], max_w - 2 * PAD), [rest.pop(0)]))
            continue
        _, cols, tw, k = best
        if k < len(rest):  # balance the pages: as many per page as an even split needs
            n = -(-len(rest) // k)
            balanced = best_page(rest, -(-len(rest) // n), label_h, min_tile, max_w, max_h)
            if balanced is not None:  # (fewer tiles can leave none that meets the bar: keep the full page then)
                _, cols, tw, k = balanced
        pages.append((cols, tw, rest[:k]))
        rest = rest[k:]
    return pages


def render_page(title, cols, tw, tiles, label_fn, out, label_h):
    rows = [tiles[i:i + cols] for i in range(0, len(tiles), cols)]
    imgs = []
    for row in rows:
        r = []
        for (w, h, payload) in row:
            im = payload["image"]() if callable(payload.get("image")) else payload["image"]
            if im.width != tw:
                resample = Image.NEAREST if payload.get("nearest") else Image.LANCZOS
                im = im.resize((tw, max(1, int(im.height * tw / im.width))), resample)
            r.append((im, payload))
        imgs.append(r)
    W = cols * tw + PAD * (cols + 1)
    ft = font(True, 26)
    tlines = wrap(title, ft, W - 2 * PAD, ImageDraw.Draw(Image.new("RGB", (8, 8))))[:3]
    head = 14 + 34 * len(tlines)
    H = head + sum(max(im.height for im, _ in r) + label_h + PAD for r in imgs) + PAD
    S = Image.new("RGB", (W, H), (78, 82, 88))
    d = ImageDraw.Draw(S)
    for i, ln in enumerate(tlines):
        d.text((PAD, 8 + 34 * i), ln, fill=(255, 255, 255), font=ft)
    y = head
    for r in imgs:
        rh = max(im.height for im, _ in r)
        for c, (im, payload) in enumerate(r):
            x = PAD + c * (tw + PAD)
            label_fn(d, x, y, tw, payload)
            S.paste(im, (x, y + label_h))
        y += rh + label_h + PAD
    S.save(out, optimize=False, compress_level=3)
    return S.size


def wrap(text, fnt, width, draw):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if draw.textlength(t, font=fnt) <= width:
            cur = t
        else:
            lines.append(cur)
            cur = w
    lines.append(cur)
    return lines


# ------------------------------------------------------------------ sheets
def variant_of(e):
    if e["scale"] != 1:
        return "hidpi"
    if "font24" in e["file"]:
        return "narrow-bigfont" if e["size"] == "1000x720" else "bigfont"
    if e["size"] == "1000x720":
        return "narrow"
    return "base"


def group_of(e):
    v = variant_of(e)
    if v == "bigfont":
        return "bigfont:" + AREA_GROUP.get(e["area"], e["area"])
    if v != "base":
        return v
    return AREA_GROUP.get(e["area"], e["area"])


def geo_key(e):
    return e["file"].replace("_dark_", "_T_").replace("_light_", "_T_")


def union(rects):
    x0 = min(r[0] for r in rects)
    y0 = min(r[1] for r in rects)
    x1 = max(r[0] + r[2] for r in rects)
    y1 = max(r[1] + r[3] for r in rects)
    return [x0, y0, x1 - x0, y1 - y0]


def grow(r, iw, ih, minw, minh, pad=28):
    x, y, w, h = r[0] - pad, r[1] - pad, r[2] + 2 * pad, r[3] + 2 * pad
    if w < minw:
        x -= (minw - w) / 2
        w = minw
    if h < minh:
        y -= (minh - h) / 2
        h = minh
    w, h = min(w, iw), min(h, ih)
    x = max(0, min(x, iw - w))
    y = max(0, min(y, ih - h))
    return [int(x), int(y), int(w), int(h)]


def crop_for(e, geo, iw, ih):
    """Pixel crop (x, y, w, h) of the shot for its sheet tile, and a note."""
    g = AREA_GROUP.get(e["area"], e["area"])
    v = variant_of(e)
    sc = e["scale"]
    if e["area"] in FULL_AREAS or e.get("window", "main") != "main" or v in ("hidpi", "narrow", "narrow-bigfont"):
        return [0, 0, iw, ih], ""
    # An item the probe found without a geometry (an empty list) can't be cropped around.
    popups = [[x * sc for x in p] for p in (geo.get("popups") or []) if p and len(p) == 4]
    subject = geo.get("subject")
    if subject is not None and len(subject) != 4:
        subject = None
    if subject and g not in ("menu", "dialog", "search", "focus"):  # the area's own item (+ its popups)
        r = [x * sc for x in subject]
        if not popups and r[3] > 760 * sc:  # a tall pane: its top part
            r = [r[0], r[1], r[2], 760 * sc]
        return grow(union([r] + popups) if popups else r, iw, ih, 820 * sc, 0, 8), "area crop"
    over = st.SHEET_CROPS_PX.get(f"{e['area']}/{e['state']}")
    if over:
        r = [int(over[0] * sc), int(over[1] * sc), int(over[2] * sc), int(over[3] * sc)]
        return grow(union([r] + popups) if popups else r, iw, ih, 0, 0, 0), "detail crop"
    if g == "focus":
        f = geo.get("focus")
        if f:
            return grow([v * sc for v in f], iw, ih, 900, 520, 40), "crop around the focused item"
        return [0, 0, iw, ih], ""
    band = st.SHEET_CROPS_PX.get(e["area"]) or st.SHEET_CROPS_PX.get(g)
    if g in ("menu", "dialog", "search") and popups:
        r = union(popups)
        if band:
            pass
        return grow(r, iw, ih, 760, 460), "crop around the popup"
    if band:
        r = [int(band[0] * sc), int(band[1] * sc), int(band[2] * sc), int(band[3] * sc)]
        if popups:
            r = union([r] + popups)
        return grow(r, iw, ih, 0, 0, 0), "area crop"
    return [0, 0, iw, ih], ""


def sheet_job(args):
    rd, title, out, entries, geo, group, theme = args[:7]
    core = len(args) > 7 and args[7]  # the core set's version: <= CORE_MAX_W wide, never downscaled
    tiles = []
    for e in entries:
        path = os.path.join(rd, "shots", e["file"])
        with Image.open(path) as im0:
            iw, ih = im0.size
        g = geo.get(geo_key(e), e)
        c, note = crop_for(e, g, iw, ih)
        parts = [(c, note)]
        if group_of(e) == "hidpi":
            parts.append(([0, 0, min(iw, 1180), min(ih, 760)], "1:1 physical-pixel detail (top-left)"))
        for (x, y, w, h), note in parts:
            def load(path=path, box=(x, y, x + w, y + h)):
                with Image.open(path) as im:
                    return im.convert("RGB").crop(box)
            up = 1
            if w < MIN_TILE:
                up = min(3, math.ceil(MIN_TILE / w))
            def image(load=load, up=up):
                im = load()
                return im if up == 1 else im.resize((im.width * up, im.height * up), Image.LANCZOS)
            payload = dict(image=image,
                e=e, note=", ".join(n for n in (note, f"shown at {up}×" if up > 1 else "") if n), crop=[x, y, w, h])
            tiles.append((w * up, h * up, payload))
    label_h = 70  # title + two description lines with their descenders
    f_b, f_r = font(True, 21), font(False, 18)

    def label(d, x, y, tw, p):
        e = p["e"]
        title = f"{e['area']}/{e['state']} · {e['theme']} · {e['size']}@{e['scale']}x"
        if d.textlength(title, font=f_b) > tw:  # narrow tiles (overview): drop the size, then elide the state
            base, tail = f"{e['area']}/{e['state']}", f" · {e['theme']}"
            title = base + tail
            while d.textlength(title, font=f_b) > tw and len(base) > 4:
                base = base[:-1]
                title = base + "…" + tail
        d.text((x, y), title, fill=(255, 255, 255), font=f_b)
        txt = e["desc"] + (f"  [{p['note']}]" if p["note"] else "")
        lines = wrap(txt, f_r, tw, d)[:2]
        for k, ln in enumerate(lines):
            d.text((x, y + 25 + 19 * k), ln, fill=(222, 222, 222), font=f_r)

    if group.startswith("compare:"):  # rows = states, columns = dark | light, as given
        tw = min((CORE_MAX_W - 3 * PAD) // 2 if core else 1180, max(t[0] for t in tiles))
        pages = [(2, tw, tiles[k:k + 6]) for k in range(0, len(tiles), 6)]
    elif group == "overview":  # every area at rest on one page (an overview; details are on the area sheets)
        pages = [(3, (CORE_MAX_W - 4 * PAD) // 3, tiles[:12])] if core else [(4, (MAX_SIDE - 5 * PAD) // 4, tiles[:12])]
    elif core:
        pages = layout(tiles, 6, label_h, CORE_MIN_TILE, CORE_MAX_W, CORE_MAX_H)
    else:
        pages = layout(tiles, 6, label_h)
    res = []
    base, ext = os.path.splitext(out)
    for i, (cols, tw, ts) in enumerate(pages):
        name = base + (f"-{i + 1}" if len(pages) > 1 else "") + ext
        t = title + (f" ({i + 1}/{len(pages)})" if len(pages) > 1 else "")
        size = render_page(t, cols, tw, ts, label, name, label_h)
        res.append(dict(file=("core/" if core else "") + os.path.basename(name), title=t, size=list(size), cols=cols,
                        tile_w=tw, group=group, theme=theme, page=i + 1, core=bool(core),
                        tiles=[dict(file=p["e"]["file"], area=p["e"]["area"], state=p["e"]["state"],
                                    theme=p["e"]["theme"], desc=p["e"]["desc"], size=p["e"]["size"],
                                    scale=p["e"]["scale"], note=p["note"]) for _, _, p in ts]))
    return res


def floor_colours(rd, entries, theme):
    """the plot's floor colour sampled from the pixels: never played (the empty pane's most frequent colour),
    analysing (the "analysing" recording, +50 to +200 ms after a new track starts: the pane's most frequent
    colour), playing (the median colour of the pane's top fifth, where the fixture tones leave only the floor)."""
    import numpy as np
    out = []

    def shot(area, state):
        es = [e for e in entries if e["area"] == area and e["state"] == state and e["theme"] == theme and e["scale"] == 1
              and e.get("size") == "1600x1000" and "font24" not in e["file"] and e.get("subject")]
        return es[0] if es else None

    def mode(a):
        v, c = unique_colours(a)
        return tuple(int(x) for x in v[c.argmax()])
    e = shot("plot", "floor-empty")
    if e:
        x, y, w, h = e["subject"]
        a = np.asarray(Image.open(os.path.join(rd, "shots", e["file"])).convert("RGB").crop((x + 10, y + 10, x + w - 10, y + h - 10)))
        out.append(("never played", mode(a), "plot/floor-empty, most frequent"))
    ds = sorted(glob.glob(os.path.join(rd, "strips-raw", f"analysing_{theme}_*")))
    if ds:
        frames, marks, meta = strip_frames(ds[0])
        t0 = action_time(marks, frames[0]["t"])
        sel = [f for f in frames if 50 <= f["t"] - t0 <= 200]
        if sel:
            a = np.asarray(Image.open(os.path.join(ds[0], sel[len(sel) // 2]["file"])).convert("RGB"))
            out.append(("analysing", mode(a[10:-10, 10:-10]), f"motion/analysing at {sel[len(sel) // 2]['t'] - t0:+.0f} ms, most frequent"))
    e = shot("plot", "floor-spectrogram")
    if e:
        x, y, w, h = e["subject"]
        a = np.asarray(Image.open(os.path.join(rd, "shots", e["file"])).convert("RGB").crop((x + 40, y + 6, x + w - 40, y + h // 5)))
        out.append(("playing", tuple(int(v) for v in np.median(a.reshape(-1, 3), axis=0)), "plot/floor-spectrogram, median of the top fifth"))
    return out


def stack_sheet(rd, sd, entries, name, area, state, title, opts=None):
    """Images made at their final size (the scrobble indicator's states, the close-ups, the 1:1 crops), never
    scaled: their 1x crops must stay 1x after the reviewers' 1568 px downscale. One row per theme (dark above
    light; opts themes=(...) for one theme) and per window size (opts sizes=(...); else the default variant: not
    24 pt, 1600x1000), the states of a tuple side by side in a row."""
    opts = opts or {}
    states_ = tuple(state) if isinstance(state, (tuple, list)) else (state,)
    themes = tuple(opts.get("themes", ("dark", "light")))
    sizes = opts.get("sizes")
    es_all = [e for e in entries if e["area"] == area and e["state"] in states_ and e["scale"] == 1 and e["theme"] in themes]
    rows = []
    for th in themes:
        for sz in (sizes or (None,)):
            row = []
            for st_ in states_:
                es = [e for e in es_all if e["theme"] == th and e["state"] == st_]
                if sz:
                    es = [e for e in es if e.get("size") == sz]
                else:
                    es = [e for e in es if e.get("size") == "1600x1000" and "font24" not in e["file"]] or es
                if es:
                    row.append(es[0])
            if row:
                rows.append(row)
    if not rows:
        return None
    imrows = [[Image.open(os.path.join(rd, "shots", e["file"])).convert("RGB") for e in row] for row in rows]
    wd = max(sum(im.width for im in r_) + PAD * (len(r_) - 1) for r_ in imrows) + 2 * PAD
    tl = wrap(title, font(True, 22), wd - 2 * PAD, ImageDraw.Draw(Image.new("RGB", (8, 8))))[:3]
    head = 18 + 30 * len(tl)
    S = Image.new("RGB", (wd, head + sum(max(im.height for im in r_) + 12 for r_ in imrows)), (78, 82, 88))
    d = ImageDraw.Draw(S)
    for i, ln in enumerate(tl):
        d.text((PAD, 10 + 30 * i), ln, fill=(255, 255, 255), font=font(True, 22))
    y = head
    for r_ in imrows:
        x = PAD
        for im in r_:
            S.paste(im, (x, y))
            x += im.width + PAD
        y += max(im.height for im in r_) + 12
    if opts.get("floors"):  # the plot's floor colours, per theme, as swatches under the rows
        fl = {th: floor_colours(rd, entries, th) for th in themes}
        n = max(len(v) for v in fl.values()) if fl else 0
        if n:
            S2 = Image.new("RGB", (S.width, S.height + 26 + 30 * len(themes)), (78, 82, 88))
            S2.paste(S, (0, 0))
            d = ImageDraw.Draw(S2)
            y = S.height
            d.text((PAD, y), "The plot's floor colour, sampled: never played · analysing · playing", fill=(255, 255, 255), font=font(True, 16))
            y += 26
            for th in themes:
                x = PAD
                d.text((x, y + 4), th, fill=(255, 220, 120), font=font(True, 14))
                x += 50
                for lb, rgb, how in fl[th]:
                    d.rectangle((x, y, x + 36, y + 22), fill=rgb, outline=(200, 200, 200))
                    t = f"{lb} #{rgb[0]:02x}{rgb[1]:02x}{rgb[2]:02x}"
                    d.text((x + 44, y + 3), t, fill=(235, 235, 235), font=font(False, 14))
                    x += 44 + int(d.textlength(t, font=font(False, 14))) + 24
                y += 30
            S = S2
    fname = f"00_{name}.png"
    S.save(os.path.join(sd, fname), compress_level=3)
    if max(S.size) > REVIEW:
        print(f"pack: {fname} is {S.width}x{S.height}: its 1x crops will be downscaled", file=sys.stderr)
    es = [e for r_ in rows for e in r_]
    return dict(file=fname, title=title, size=list(S.size), cols=1, tile_w=wd, group=name, one_to_one=True,
                theme=themes[0] if len(themes) == 1 else "both", page=1,
                tiles=[dict(file=e["file"], area=e["area"], state=e["state"], theme=e["theme"], desc=e["desc"],
                            size=e["size"], scale=e["scale"], note="") for e in es])


def source_notes(sc_):
    """a scope's notes stated from the app's source (focus.SCOPES[...]["source_notes"]: (file, phrases, text)):
    each phrase must still be in the file at SHOT_SRC, or the pack fails, so a note never outlives the code it
    describes; the note names the file it was read from."""
    out = []
    for rel, phrases, text in sc_.get("source_notes", []):
        path = os.path.join(os.environ.get("SHOT_SRC", ""), rel)
        try:
            with open(path, encoding="utf-8") as fh:
                src = fh.read()
        except OSError as e:
            raise LookupError(f"source note: cannot read {rel} under SHOT_SRC ({e})") from e
        gone = [ph for ph in phrases if ph not in src]
        if gone:
            raise LookupError(f"source note: {rel} no longer has {gone}: restate the note from the code")
        out.append(f"{text} (read from `{rel}`)")
    return out


def custom_sheet(rd, sd, entries, name, title, theme, rows):
    """A sheet of 1:1 crops (never scaled) laid out by hand (states.CUSTOM_SHEETS): rows of cells, a cell one
    image or a column of images; an image (area, state, crop, label) with crop "subject" (the area's item, as
    recorded at the grab), (fx, fy, fw, fh) fractions of the shot, or None (the whole shot)."""
    def strip_tile(c):
        """A recording's frames at given times (ms after the action), cut around an item it recorded, or around
        what changed in it (item "changed", with the item c["include"] names in a dialog shot's layout), zoomed, or
        shrunk to c["fit"] (W, H; the label then says the scale)."""
        import motion_strip
        import numpy as np
        ds = sorted(glob.glob(os.path.join(rd, "strips-raw", f"{c['strip']}_{c.get('theme', theme)}_*")))
        if not ds:
            raise LookupError(f"custom sheet {name}: no recording {c['strip']} ({c.get('theme', theme)})")
        frames, marks, meta = strip_frames(ds[0])
        t0 = action_time(marks, frames[0]["t"])
        # only the frames shown are read, each cropped at once: a whole recording at full size is gigabytes
        with Image.open(os.path.join(ds[0], frames[0]["file"])) as im0:
            fw, fh = im0.size
        p_, z = c.get("pad", 6), c.get("zoom", 3)
        follow = None
        if isinstance(c["item"], str) and c["item"].startswith("trace:"):
            # follow an item that moves (e.g. the now-playing text block while the card grows): its rect at each
            # frame from the recording's item trace (window coordinates, less the recording's crop)
            tracks, _ = motion_strip.read_trace(ds[0], t0)
            leaf = c["item"][len("trace:"):]
            keys = sorted((k_ for k_ in tracks if k_.split("/")[-1].split("#")[0] == leaf), key=len)
            if not keys:
                raise LookupError(f"custom sheet {name}: no {leaf} in the trace of {c['strip']}")
            follow = tracks[keys[0]]
            ox, oy = (meta.get("crop") or [0, 0])[:2]
        else:
            if c["item"] == "changed":
                box = change_box(c, ds[0], frames, t0)
            else:
                box = motion_strip.resolve_rect(c["item"], meta, fw, fh)
            x0, y0 = max(0, box[0] - p_), max(0, box[1] - p_)
            box = (x0, y0, box[2] + 2 * p_, box[3] + 2 * p_)
            if c["item"] == "changed":  # (inside the frames: no black margin where the change reaches an edge)
                box = (x0, y0, min(fw - x0, box[2]), min(fh - y0, box[3]))
        cut = []
        rel = 0.0
        if c.get("anchor") == "last-change":
            # times from the start of the last change inside the crop (the strip's fade, wherever this run
            # put it): consecutive frames compared there, read one at a time; a gap of 200 ms ends a run of changes
            prev, runs, last_t = None, [], None
            for f in frames:
                if f["t"] - t0 < 0:
                    continue
                with Image.open(os.path.join(ds[0], f["file"])) as fim:
                    cb = fim.convert("RGB").crop((box[0], box[1], box[0] + box[2], box[1] + box[3])).tobytes()
                if prev is not None and cb != prev:
                    if last_t is None or f["t"] - last_t > 200:
                        runs.append(f["t"] - t0)
                    last_t = f["t"]
                prev = cb
            if not runs:
                raise LookupError(f"custom sheet {name}: nothing changes in {c['strip']}'s crop")
            rel = runs[-1]
        spans = [(f["t"] - t0, (frames[i + 1]["t"] if i + 1 < len(frames) else f["t"] + 17) - t0) for i, f in enumerate(frames)]
        for t in c["times"]:
            t = t + rel
            if c.get("perceived"):
                # the perceived image at t: the time-weighted mean of the frames on screen in the eye's window
                # before t (motion_strip.WINDOW_MS), read one crop at a time
                acc, tot = None, 0.0
                for i_, (a_, b_) in enumerate(spans):
                    o_ = min(b_, t) - max(a_, t - motion_strip.WINDOW_MS)
                    if o_ <= 0:
                        continue
                    with Image.open(os.path.join(ds[0], frames[i_]["file"])) as fim:
                        arr = np.asarray(fim.convert("RGB").crop((box[0], box[1], box[0] + box[2], box[1] + box[3])), dtype=np.float32)
                    acc = arr * o_ if acc is None else acc + arr * o_
                    tot += o_
                if acc is None:
                    raise LookupError(f"custom sheet {name}: no frame of {c['strip']} before {t:+.0f} ms")
                im = Image.fromarray((acc / tot).clip(0, 255).astype(np.uint8))
                cut.append((f"~{t - rel:+.0f} ms, perceived" + (" (from the change)" if rel else ""),
                            im.resize((im.width * z, im.height * z), Image.NEAREST)))
                continue
            k = max([i for i, f in enumerate(frames) if f["t"] - t0 <= t] or [0])
            if follow:
                ft = frames[k]["t"] - t0
                j = max([i for i, (tt, _) in enumerate(follow["pts"]) if tt <= ft] or [0])
                rx, ry, rw, rh = follow["fulls"][j]  # (its own rect: a growing parent's clip does not shrink the crop)
                box = (max(0, int(rx - ox) - p_), max(0, int(ry - oy) - p_), int(rw) + 2 * p_,
                       int(rh) + 2 * p_ + int(c.get("below", 0)))  # (context below the item)
            with Image.open(os.path.join(ds[0], frames[k]["file"])) as fim:
                im = fim.convert("RGB").crop((box[0], box[1], box[0] + box[2], box[1] + box[3]))
            cut.append((f"{frames[k]['t'] - t0 - rel:+.0f} ms" + (" from the change" if rel else ""),
                        im.resize((im.width * z, im.height * z), Image.NEAREST)))
        label = c["label"]
        if c.get("fit"):
            w_, h_ = c["fit"]
            f_ = min([1.0] + [min(w_ / i.width, h_ / i.height) for _, i in cut])
            if f_ < 1.0:
                cut = [(lb, i.resize((int(i.width * f_), int(i.height * f_)), Image.LANCZOS)) for lb, i in cut]
                label += f" (shown at {f_:.2f}x)"
                fitted.append(f_)
        g = 10
        S_ = Image.new("RGB", (sum(i.width for _, i in cut) + g * (len(cut) - 1), max(i.height for _, i in cut) + 20),
                       (78, 82, 88))
        d_ = ImageDraw.Draw(S_)
        x = 0
        for lb, i in cut:
            d_.text((x, 0), lb, fill=(230, 230, 230), font=font(False, 13))
            S_.paste(i, (x, 20))
            x += i.width + g
        return label, S_, dict(_lh=c.get("lh"), _gap=c.get("gap"),  # (a frame row may set its label's height)
                                    file=os.path.basename(ds[0]), area="motion", state=c["strip"], theme=c.get("theme", theme),
                                    desc=c["label"], size=f"{meta['size'][0]}x{meta['size'][1]}", scale=meta.get("scale", 1))

    def change_box(c, d, frames, t0):
        """(x, y, w, h) around every pixel that changes after the action in a recording, with the rect of the item
        c["include"] (area, state, name: from that dialog shot's layout, whose coordinates its window's frames
        have)."""
        got, prev = None, None
        for f in frames:
            with Image.open(os.path.join(d, f["file"])) as fim:
                im = fim.convert("RGB")
            if prev is not None and f["t"] >= t0:
                b = ImageChops.difference(prev, im).convert("L").point(lambda v: 255 if v > 2 else 0).getbbox()
                if b:
                    got = b if got is None else (min(got[0], b[0]), min(got[1], b[1]), max(got[2], b[2]), max(got[3], b[3]))
            prev = im
        if got is None:
            raise LookupError(f"custom sheet {name}: nothing changes in {c['strip']}")
        if c.get("include"):
            area, state, item = c["include"]
            es = [e for e in entries if tile_matches(e, area, state, c.get("theme", theme))]
            g_ = (es[0].get("grabbed") or {}) if es else {}
            if not es or not es[0].get("layout"):
                raise LookupError(f"custom sheet {name}: no layout of {area}/{state} to take {item} from")
            with open(os.path.join(rd, es[0]["layout"])) as lf:
                wins = json.load(lf)["windows"]
            w_ = next((w for w in wins if w.get("cls") == g_.get("cls")), wins[-1])
            rs = [t["rect"] for t in (w_.get("texts") or []) + (w_.get("actions") or []) if t.get("name") == item]
            if not rs:
                raise LookupError(f"custom sheet {name}: no {item} in {area}/{state}'s layout")
            x, y, w, h = rs[0]
            got = (min(got[0], x), min(got[1], y), max(got[2], x + w), max(got[3], y + h))
        return (int(got[0]), int(got[1]), math.ceil(got[2] - got[0]), math.ceil(got[3] - got[1]))

    def tile(area, state, crop, label, opts=None):
        sc_ = (opts or {}).get("scale", 1)
        th_ = (opts or {}).get("theme", theme)
        var_ = (opts or {}).get("variant", "")  # e.g. "font24": that variant (any window size it was shot at)
        es = [e for e in entries if tile_matches(e, area, state, th_, var_, sc_)]
        if not es:
            if (opts or {}).get("optional"):  # a tile that is made only sometimes (e.g. a changed glyph)
                return None
            raise LookupError(f"custom sheet {name}: no {area}/{state} ({th_}, scale {sc_}{', ' + var_ if var_ else ''})")
        im = _redact(rd, es[0], Image.open(os.path.join(rd, "shots", es[0]["file"])).convert("RGB"))
        if crop in ("inner", "popup"):
            # "inner": the innermost dialog window of a nested capture (a confirmation over its parent), with its
            # title bar and a margin of the parent around it; "popup": the main window's first popup (the search
            # palette, Recent Issues) with a margin. Both from the shot's own index entry and layout.
            e0, mg = es[0], (opts or {}).get("margin", 16)
            if crop == "inner":
                g = e0.get("grabbed") or {}
                if g.get("kind") != "dialog" or not e0.get("layout"):
                    raise LookupError(f"custom sheet {name}: {area}/{state} is not a dialog capture with a layout")
                wins = json.load(open(os.path.join(rd, e0["layout"])))["windows"]
                ww, wh = next((w_["size"] for w_ in wins if w_["cls"] == g.get("cls")), wins[-1]["size"])
                dx, dy = g.get("image_offset", [0, 0])
                tb = (opts or {}).get("titlebar", 30)  # KWin's title bar above the client area
                box_ = (dx - mg, dy - tb - mg, dx + ww + mg, dy + wh + mg)
            else:
                if not e0.get("popups"):
                    raise LookupError(f"custom sheet {name}: {area}/{state} recorded no popup")
                x, y, w, h = e0["popups"][(opts or {}).get("popup", 0)]
                box_ = (x - mg, y - mg, x + w + mg, y + h + mg)
            im = im.crop((max(0, int(box_[0])), max(0, int(box_[1])), min(im.width, int(box_[2])), min(im.height, int(box_[3]))))
        elif crop == "focus":
            # a box (opts "around": w, h; default 420x150) on the item with keyboard focus as the shot recorded it:
            # a focus walk's stops at 1:1
            if not es[0].get("focus"):
                raise LookupError(f"custom sheet {name}: {area}/{state} recorded no focus item")
            x, y, w, h = es[0]["focus"]
            bw, bh = (opts or {}).get("around", (420, 150))
            mg = (opts or {}).get("margin", 8)
            # an item larger than the box (a list, the seek bar) shows its start, where its ring's corner is: every
            # stop's tile keeps the box's size
            x0 = x + w / 2 - bw / 2 if w + 2 * mg <= bw else x - mg
            y0 = y + h / 2 - bh / 2 if h + 2 * mg <= bh else y - mg
            x0 = int(min(max(0, x0), max(0, im.width - bw)))
            y0 = int(min(max(0, y0), max(0, im.height - bh)))
            im = im.crop((x0, y0, min(im.width, x0 + int(bw)), min(im.height, y0 + int(bh))))
        elif crop == "subject":
            x, y, w, h = es[0]["subject"]
            mg = (opts or {}).get("margin", 0)  # the window around the subject, its real colour, on every side
            im = im.crop((max(0, x - mg), max(0, y - mg), min(im.width, x + w + mg), min(im.height, y + h + mg)))
        elif crop:
            fx, fy, fw, fh = crop
            im = im.crop((int(fx * im.width), int(fy * im.height), int((fx + fw) * im.width), int((fy + fh) * im.height)))
        if (opts or {}).get("items"):
            # the union of named items' rects (texts and controls, as recorded in the dialog window's layout; a
            # name ending in "*" matches its prefix) with a margin: it follows the items when a window changes size
            e0, mg = es[0], (opts or {}).get("margin", 8)
            g = e0.get("grabbed") or {}
            if g.get("kind") != "dialog" or not e0.get("layout") or crop:
                raise LookupError(f"custom sheet {name}: items= needs a dialog capture with a layout and no crop ({area}/{state})")
            wins = json.load(open(os.path.join(rd, e0["layout"])))["windows"]
            w_ = next((w for w in wins if w["cls"] == g.get("cls")), wins[-1])
            want = (opts or {})["items"]
            hit = lambda n_: any(n_ == w or (w.endswith("*") and n_.startswith(w[:-1])) for w in want)
            rs = [t["rect"] for t in (w_.get("texts") or []) + (w_.get("actions") or []) if t.get("name") and hit(t["name"])]
            if (opts or {}).get("first"):  # only the topmost match of each name (e.g. one list row)
                by = {}
                for t in (w_.get("texts") or []) + (w_.get("actions") or []):
                    if t.get("name") and hit(t["name"]) and (t["name"] not in by or t["rect"][1] < by[t["name"]][1]):
                        by[t["name"]] = t["rect"]
                rs = list(by.values())
            if not rs:
                raise LookupError(f"custom sheet {name}: none of {want} in {area}/{state}'s layout")
            dx, dy = g.get("image_offset", [0, 0])
            gl, gt, gr, gb = (opts or {}).get("grow", (0, 0, 0, 0))  # extra pixels left, top, right, bottom
            x0, y0 = min(r[0] for r in rs) + dx - mg - gl, min(r[1] for r in rs) + dy - mg - gt
            x1, y1 = max(r[0] + r[2] for r in rs) + dx + mg + gr, max(r[1] + r[3] for r in rs) + dy + mg + gb
            im = im.crop((max(0, int(x0)), max(0, int(y0)), min(im.width, int(x1)), min(im.height, int(y1))))
        part = (opts or {}).get("part")  # a part of the crop, as fractions: e.g. its left half
        if part:
            fx, fy, fw, fh = part
            im = im.crop((int(fx * im.width), int(fy * im.height), int((fx + fw) * im.width), int((fy + fh) * im.height)))
        box = (opts or {}).get("box")  # a pixel box (x, y, w, h) within the crop so far: e.g. one glyph
        if box:
            bx, by, bw, bh = box
            im = im.crop((bx, by, min(im.width, bx + bw), min(im.height, by + bh)))
        zm = (opts or {}).get("zoom")  # magnified, nearest neighbour (an inset beside its 1:1): the label says so
        if zm:
            im = im.resize((im.width * zm, im.height * zm), Image.NEAREST)
            if f"{zm}x" not in label:
                raise LookupError(f"custom sheet {name}: the zoomed tile {label!r} must say {zm}x in its label")
            zoomed.append(zm)
        mw, mh = (opts or {}).get("max_w"), (opts or {}).get("max_h")  # 1:1, trimmed (from the left and top)
        if mw or mh:
            im = im.crop((0, 0, min(im.width, mw or im.width), min(im.height, mh or im.height)))
        fit = (opts or {}).get("fit")
        if fit:  # a whole window, scaled to fit (W, H): not 1:1, and the label says how far
            f = min(fit[0] / im.width, fit[1] / im.height, 1.0)
            if f < 1.0:
                im = im.resize((int(im.width * f), int(im.height * f)), Image.LANCZOS)
                label += f" (shown at {f:.2f}x)"
                fitted.append(f)
        if (opts or {}).get("plate"):  # the viewer's corner plate or strip as grabbed (fact plate-STATE-SUFFIX)
            pf = _facts_by_key(rd).get(f"plate-{state}-{es[0]['file'][len(area) + len(state) + 2:-4]}")
            if not pf:
                raise LookupError(f"custom sheet {name}: no plate fact for {area}/{state} ({es[0]['file']})")
            label += f" (its {pf['what']} {pf['shown']})"
        return label, im, dict(es[0], _lh=(opts or {}).get("lh"), _gap=(opts or {}).get("gap"))
    LH, GAP = 24, 12
    ft = font(False, 15)
    fitted, zoomed = [], []
    grid = [[[strip_tile(c)] if isinstance(c, dict) else [tile(*c)] if isinstance(c[0], str) else
              [strip_tile(i) if isinstance(i, dict) else tile(*i) for i in c] for c in row] for row in rows]
    # optional tiles that were not made drop out, then empty cells and rows
    grid = [[[t for t in c if t is not None] for c in row] for row in grid]
    grid = [[c for c in row if c] for row in grid]
    grid = [row for row in grid if row]
    lh_ = lambda e: LH if e.get("_lh") is None else e["_lh"]  # (a tile may set its label's height and gap)
    gap_ = lambda e: GAP if e.get("_gap") is None else e["_gap"]
    cell_w = lambda col: max(im.width for _, im, _ in col)
    cell_h = lambda col: sum(lh_(e) + im.height + gap_(e) for _, im, e in col)
    wd = max(sum(cell_w(c) for c in row) + PAD * (len(row) - 1) for row in grid) + 2 * PAD
    tl = wrap(title, font(True, 22), wd - 2 * PAD, ImageDraw.Draw(Image.new("RGB", (8, 8))))
    if len(tl) > 3:  # a title cut after three lines lost its end (F04's "dark above light" read "dark")
        raise LookupError(f"custom sheet {name}: its title needs {len(tl)} lines at {wd} px, three are drawn: shorten it")
    head = 18 + 30 * len(tl)
    S = Image.new("RGB", (wd, head + sum(max(cell_h(c) for c in row) for row in grid)), (78, 82, 88))
    d = ImageDraw.Draw(S)
    for i, ln in enumerate(tl):
        d.text((PAD, 10 + 30 * i), ln, fill=(255, 255, 255), font=font(True, 22))
    y = head
    used = []
    for row in grid:
        x = PAD
        for col in row:
            yy = y
            room = (cell_w(col) + PAD - 4) if col is not row[-1] else (wd - x)
            for label, im, e in col:
                if label:
                    f_ = ft if lh_(e) >= 24 else font(False, 13)
                    # a label wider than its cell runs into the next one: fail, so the sheet's labels get shortened
                    if d.textlength(label, font=f_) > room:
                        raise LookupError(f"custom sheet {name}: the label {label!r} ({d.textlength(label, font=f_):.0f} px) "
                                          f"is wider than its cell ({room} px)")
                    d.text((x, yy + 3 if lh_(e) >= 24 else yy), label, fill=(255, 220, 120), font=f_)
                S.paste(im, (x, yy + lh_(e)))
                yy += lh_(e) + im.height + gap_(e)
                used.append(e)
            x += cell_w(col) + PAD
        y += max(cell_h(c) for c in row)
    fname = f"00_{name}.png"
    S.save(os.path.join(sd, fname), compress_level=3)
    return dict(file=fname, title=title, size=list(S.size), cols=1, tile_w=wd, group=name, theme=theme, page=1,
                one_to_one=not fitted, tile_fit=round(min(fitted), 3) if fitted else 1.0,
                zoomed=sorted(set(zoomed)),
                tiles=[dict(file=e["file"], area=e["area"], state=e["state"], theme=e["theme"], desc=e["desc"],
                            size=e["size"], scale=e["scale"], note="1:1") for e in used])


_INVISIBLE = dict.fromkeys(map(ord, "\u200b\u200c\u200d\u2060\ufeff\u00ad"))  # break hints a path carries


def _has_home(text):
    """Whether a recorded text names the home folder, read without the invisible break hints the app puts in paths
    so they wrap (the art viewer's Folder row, "/\u200bhome/\u200b…", escaped a plain test)."""
    return bool(text) and os.path.expanduser("~") in str(text).translate(_INVISIBLE)


def _home_boxes(rd, e):
    """The boxes (image px) that name this machine's paths in a shot: every recorded text (its layout)
    holding the home folder, and the boxes the runner listed (shots/FILE.redact.json: the diagnostics log, already in
    image px). The walk is in logical px and the shot in device px, so its rects, sizes and logical offsets are
    scaled by the shot's scale (#666). Offsets: a dialog capture's own window at its image offset (image px), an
    outer dialog window of a nested capture below its title bar at the capture's origin (28 logical px: KWin draws
    title bars at the session's scale); a main-window or viewer-window shot at 0, 0, and the menus a main-window shot
    draws in at their offsets (logical px). Returns (boxes, inner): inner, the grabbed window's box in a nested
    capture (it lies over its parent's boxes)."""
    boxes, inner = [], None
    g = e.get("grabbed") or {}
    s = float(e.get("scale", 1) or 1)
    sc = os.path.join(rd, "shots", e["file"][:-4] + ".redact.json")
    side = json.load(open(sc)) if os.path.exists(sc) else {}
    precise = set(side.get("precise", []))  # texts whose home-folder characters the runner boxed exactly
    if e.get("layout") and os.path.exists(os.path.join(rd, e["layout"])):
        wins = json.load(open(os.path.join(rd, e["layout"])))["windows"]
        outer = [w for w in wins if not w.get("main")]
        for w in wins:
            if g.get("kind") == "dialog":
                if w["cls"] == g.get("cls"):
                    off = g.get("image_offset", [0, 0])
                elif not w.get("main") and outer and w is outer[0]:
                    off = [0, 28 * s]
                else:
                    continue
            elif g.get("kind") == "window":
                if w.get("name") != g.get("name"):
                    continue
                off = g.get("image_offset", [0, 0])
            elif w.get("main"):
                off = [0, 0]
            elif g.get("kind") == "main" and w.get("main_popup_offset"):  # a menu the grab draws in
                off = [v * s for v in w["main_popup_offset"]]
            else:
                continue
            for t in w.get("texts", []):
                if (t.get("home") or _has_home(t.get("text")) or _has_home(t.get("shown_text"))) \
                        and t.get("name") not in precise:
                    x, y, ww, hh = t["rect"]
                    boxes.append([x * s + off[0], y * s + off[1], ww * s, hh * s])
        if g.get("kind") == "dialog" and len(outer) > 1:
            ww_, wh_ = next((w["size"] for w in wins if w["cls"] == g.get("cls")), (0, 0))
            dx_, dy_ = g.get("image_offset", [0, 0])
            inner = (dx_, dy_ - 30 * s, dx_ + ww_ * s, dy_ + wh_ * s)
    boxes += side.get("boxes", [])
    return boxes, inner


_FACTS_CACHE = {}


def _facts_by_key(rd):
    """facts.jsonl by key (the last line of a key wins), read once per round."""
    if rd not in _FACTS_CACHE:
        d = {}
        p = os.path.join(rd, "facts.jsonl")
        if os.path.exists(p):
            for ln in open(p, encoding="utf-8"):
                try:
                    f = json.loads(ln)
                except ValueError:
                    continue
                d[f.get("key")] = f
        _FACTS_CACHE[rd] = d
    return _FACTS_CACHE[rd]


def _redact(rd, e, im):
    """Paints over the shot's home-folder boxes (_home_boxes) in their surroundings' colour with "[home folder
    hidden]". A nested capture: the grabbed window lies over its parent's boxes (the diagnostics log under the clear-log
    confirmation): the parent's are painted first, the window gets its own pixels back, then its own are painted.
    Returns the image (changed in place)."""
    boxes, inner = _home_boxes(rd, e)
    if not boxes:
        return im
    inside = lambda b: inner is not None and b[0] >= inner[0] and b[1] >= inner[1] and b[0] + b[2] <= inner[2] and b[1] + b[3] <= inner[3]
    keep = im.crop(tuple(int(v) for v in inner)) if inner else None
    fills = _paint(im, [b for b in boxes if not inside(b)])
    if keep is not None:
        im.paste(keep, (int(inner[0]), int(inner[1])))
    fills |= _paint(im, [b for b in boxes if inside(b)])
    im.info["redact_fills"] = fills
    im.info["redact_inner"] = inner
    return im


def _box_painted(im, b, fills, exclude=None):
    """Whether box b is painted over: at least 75 % of its pixels (inside the image) are fill colours (the label's
    few antialiased pixels aside; overlapping boxes may leave several fills in one box)."""
    x0, y0 = max(0, int(b[0]) - 1), max(0, int(b[1]) - 1)
    x1, y1 = min(im.width, int(b[0] + b[2]) + 1), min(im.height, int(b[1] + b[3]) + 1)
    if x1 - x0 < 2 or y1 - y0 < 2:
        return True
    cr = im.crop((x0, y0, x1, y1))
    if exclude:  # (a parent's box under the grabbed window, which keeps its own pixels: judged outside that window)
        ex0, ey0, ex1, ey1 = [int(v) for v in exclude]
        px = [cr.getpixel((x, y)) for y in range(cr.height) for x in range(cr.width)
              if not (ex0 <= x + x0 < ex1 and ey0 <= y + y0 < ey1)]
        return not px or sum(1 for col in px if tuple(col[:3]) in fills) >= 0.75 * len(px)
    c = Counter(getattr(cr, "get_flattened_data", cr.getdata)())
    return sum(n for col, n in c.items() if tuple(col[:3]) in fills) >= 0.75 * (x1 - x0) * (y1 - y0)


def _private_names(rd, entries):
    """Item names that showed the home folder in any shot's layout (and the runner's PRIVATE_ITEMS)."""
    names = set()  # (no item hidden whole by name any more)
    for e in entries:
        if not e.get("layout") or not os.path.exists(os.path.join(rd, e["layout"])):
            continue
        for w in json.load(open(os.path.join(rd, e["layout"])))["windows"]:
            for t in w.get("texts", []):
                if t.get("name") and (t.get("home") or _has_home(t.get("text")) or _has_home(t.get("shown_text"))):
                    names.add(t["name"])
    return names


def _walk_names_home(rd, rel):
    """Whether the layout walk at rd/rel records a text that names the home folder (as _home_boxes reads it)."""
    with open(os.path.join(rd, rel)) as f:
        windows = json.load(f)["windows"]
    for w in windows:
        for t in w.get("texts", []):
            if t.get("home") or _has_home(t.get("text")) or _has_home(t.get("shown_text")):
                return True
    return False


def privacy_evidence(rd, entries):
    """Fails closed: the privacy check reads the home folder's texts from the layout walks and recordings' traces,
    so an image without them is not "checked" but unverified, and fails the pack (exit 3, nothing published).
    - a shot needs its layout walk (missing with SHOT_LAYOUT=0, or when the probe could not write it);
    - a composite (cut from grabs with no entry of their own) needs `sources`: the walk of every grab it was cut
      from, or a shot it reuses, none of them naming the home folder (a composite cannot be painted: its crops'
      offsets are not recorded); an image with no app pixels (an icon rendered from its SVGs) has `sources=[]` and
      the explicit marker `no_app_pixels`, and one with neither sources nor the marker is unverified;
    - a recording needs its item trace and must have been made at the neutral paths (neutral_ns.sh): its trace
      names no texts, so at the real paths (SHOT_NEUTRAL=0) a home-folder text in a frame could go unseen. A menu
      its frames draw in (meta `popup_windows`, popup_frames.py) needs its items in the trace (layer popup@w<window>),
      where the painting reads their rects.
    Returns (failures, counts)."""
    import motion_strip
    fails = []
    n = Counter()
    by_file = {e["file"]: e for e in entries}
    has_walk = lambda e: bool(e.get("layout")) and os.path.exists(os.path.join(rd, e["layout"]))
    for e in entries:
        if not os.path.exists(os.path.join(rd, "shots", e["file"])):
            continue  # (not an image this round publishes: a geometry-only entry)
        if has_walk(e):
            n["shots"] += 1
            continue
        if "sources" not in e:
            fails.append(f"privacy: {e['file']} is unverified: it has no layout walk (SHOT_LAYOUT=0, or the probe "
                         "could not write it)")
            continue
        if not e["sources"]:
            if e.get("no_app_pixels") is True:
                n["drawn"] += 1
            else:
                fails.append(f"privacy: composite {e['file']} is unverified: it lists no grab it was cut from and is "
                             "not marked as drawn without app pixels")
            continue
        bad = []
        for src in e["sources"]:
            if src is None:
                bad.append("a grab without its layout walk")
            elif src.startswith("shot:"):
                se = by_file.get(src[5:])
                if se is None or not has_walk(se):
                    bad.append(f"{src[5:]} without its layout walk")
                elif _walk_names_home(rd, se["layout"]):
                    bad.append(f"{src[5:]}, which shows a home-folder text")
            elif not os.path.exists(os.path.join(rd, src)):
                bad.append(f"{src} missing")
            elif _walk_names_home(rd, src):
                bad.append(f"{src}, which shows a home-folder text")
        if bad:
            fails.append(f"privacy: composite {e['file']} is unverified: cut from {'; '.join(sorted(set(bad))[:3])}")
        else:
            n["composites"] += 1
    for d in sorted(glob.glob(os.path.join(rd, "strips-raw", "*"))):
        if not os.path.exists(os.path.join(d, "frames.tsv")):
            continue
        meta = json.load(open(os.path.join(d, "meta.json"))) if os.path.exists(os.path.join(d, "meta.json")) else {}
        if not os.path.exists(os.path.join(d, "opacity.tsv")):
            fails.append(f"privacy: recording {os.path.basename(d)} is unverified: it has no item trace (opacity.tsv)")
        elif not meta.get("neutral"):
            fails.append(f"privacy: recording {os.path.basename(d)} is unverified: recorded at the real paths "
                         "(SHOT_NEUTRAL=0, or before its meta.json said), where its frames cannot be checked")
        else:
            drawn = meta.get("popup_windows") or []
            layers = {la for tr in motion_strip.read_trace(d, 0.0)[0].values() for la in tr["layers"]} if drawn else set()
            untraced = [w for w in drawn if f"popup@w{w}" not in layers]
            if untraced:
                fails.append(f"privacy: recording {os.path.basename(d)} is unverified: its frames draw in menu "
                             f"window(s) {untraced}, whose items are not in its item trace")
            else:
                n["recordings"] += 1
    return fails, n


def redact_round(rd, entries):
    """Paints the home folder out of the round's own images before anything is made of them: every shot in
    place (shots/), and every recorded frame (strips-raw/) where an item that names it (by name: _private_names) is
    shown, from the recording's item trace (its rect and opacity at that frame). Then checks: every such box in
    every shot and frame is painted, else the pack fails (a privacy check on the recorded texts and their rects; no
    OCR), and every image has the evidence the check needs (privacy_evidence), else it fails as unverified.
    Returns the failures."""
    import motion_strip
    fails, counts = privacy_evidence(rd, entries)
    for e in entries:
        boxes, _ = _home_boxes(rd, e)
        if not boxes:
            continue
        p = os.path.join(rd, "shots", e["file"])
        im = Image.open(p).convert("RGB")
        _redact(rd, e, im)  # (painting again is harmless: the same boxes, their surroundings' colours)
        im.save(p)
        fills, inner = im.info.get("redact_fills", set()), im.info.get("redact_inner")
        within = lambda b: inner is not None and b[0] >= inner[0] and b[1] >= inner[1] \
            and b[0] + b[2] <= inner[2] and b[1] + b[3] <= inner[3]
        bad = [b for b in boxes if not _box_painted(im, b, fills, None if within(b) else inner)]
        if bad:
            fails.append(f"privacy: {e['file']} still shows a home-folder text at {bad[:2]}")
    names = _private_names(rd, entries)
    n_frames = 0
    for d in sorted(glob.glob(os.path.join(rd, "strips-raw", "*"))):
        if not os.path.exists(os.path.join(d, "frames.tsv")):
            continue
        meta = json.load(open(os.path.join(d, "meta.json"))) if os.path.exists(os.path.join(d, "meta.json")) else {}
        ox, oy = (meta.get("crop") or [0, 0])[:2]  # (logical px, as the trace's rects; the frames are device px)
        s = float(meta.get("scale", 1) or 1)
        tracks, _ = motion_strip.read_trace(d, 0.0)
        priv = [tr for k, tr in tracks.items() if k.split("/")[-1].split("#")[0] in names and tr["pts"]]
        if not priv:
            continue
        for line in open(os.path.join(d, "frames.tsv"), encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if f[0] != "frame":
                continue
            t, fp = float(f[2]), os.path.join(d, f[4])
            boxes = []
            for tr in priv:
                i = max([k for k, (tt, _v) in enumerate(tr["pts"]) if tt <= t] or [0])
                if tr["pts"][i][1] > 0.02:
                    x, y, w, h = tr["rects"][i]
                    boxes.append([(x - ox) * s, (y - oy) * s, w * s, h * s])
            if not boxes or not os.path.exists(fp):
                continue
            im = Image.open(fp).convert("RGB")
            fills = _paint(im, boxes)
            im.save(fp)
            n_frames += 1
            bad = [b for b in boxes if not _box_painted(im, b, fills)]
            if bad:
                fails.append(f"privacy: {os.path.basename(d)}/{f[4]} still shows a home-folder item at {bad[:2]}")
    print(f"pack: privacy: checked {counts['shots']} shots by their layout walks, {counts['composites']} composites by "
          f"their sources' walks, {counts['drawn']} images drawn without app pixels and {counts['recordings']} "
          "recordings (neutral paths, traced); home-folder texts "
          f"painted out of the shots and {n_frames} recorded frames"
          + (f"; {len(fails)} FAILED or unverified" if fails else "; check passed"))
    return fails


def _paint(im, boxes):
    """Paints the boxes; returns the fill colours used (for the privacy check: overlapping boxes, a tooltip over a
    row, leave more than one fill in a box)."""
    d = ImageDraw.Draw(im)
    f = font(False, 12)
    fills = set()
    for x, y, ww, hh in boxes:
        x0, y0, x1, y1 = int(x) - 1, int(y) - 1, int(x + ww) + 1, int(y + hh) + 1
        ring = [im.getpixel((min(im.width - 1, max(0, px)), min(im.height - 1, max(0, py))))
                for px, py in ((x0 - 2, y0 - 2), (x1 + 2, y0 - 2), (x0 - 2, y1 + 2), (x1 + 2, y1 + 2))]
        bg = Counter(ring).most_common(1)[0][0]
        d.rectangle((x0, y0, x1, y1), fill=bg)
        fills.add(tuple(bg[:3]))
        lum = 0.299 * bg[0] + 0.587 * bg[1] + 0.114 * bg[2]
        if hh >= 12:  # (a tall box, the log body: its label at the top, where a crop keeps it)
            lab = "[home folder hidden]" if d.textlength("[home folder hidden]", font=f) <= (x1 - x0) - 6 else "~"
            d.text((x0 + 4 if lab != "~" else x0 + 1, y0 + (4 if hh > 40 else max(0, (y1 - y0 - 14) // 2))), lab,
                   fill=(150, 150, 150) if lum < 128 else (110, 110, 110), font=f)
    return fills


def load_entries(rd):
    seen = {}
    for p in sorted(glob.glob(os.path.join(rd, "index", "*.json"))):
        for e in json.load(open(p)):
            seen[e["file"]] = e
    return [e for e in seen.values() if os.path.exists(os.path.join(rd, "shots", e["file"]))]


def load_geometry(gd):
    geo = {}
    if gd:
        for p in glob.glob(os.path.join(gd, "index", "*.json")):
            for e in json.load(open(p)):
                geo[geo_key(e)] = e
    return geo


# ------------------------------------------------------------------ strips
def strip_frames(d):
    frames, marks = [], []
    with open(os.path.join(d, "frames.tsv"), encoding="utf-8") as tsv:
        for line in tsv:
            f = line.rstrip("\n").split("\t")
            if f[0] == "frame":
                frames.append(dict(i=int(f[1]), t=float(f[2]), win=int(f[3]), file=f[4]))
            elif f[0] == "mark":
                marks.append((float(f[1]), f[2]))
    with open(os.path.join(d, "meta.json")) as mf:
        meta = json.load(mf)
    # The main window (index 0), or with win= the one recorded window (the one with the most frames).
    wins = Counter(f["win"] for f in frames)
    w0 = 0 if 0 in wins and "win=" not in meta.get("opts", "") else (wins.most_common(1)[0][0] if wins else 0)
    return [f for f in frames if f["win"] == w0], marks, meta


INPUT_CMDS = {"click", "rclick", "dblclick", "press", "release", "move", "dmove", "wheel", "key", "eval"}  # (an id leads)


def action_time(marks, default):
    """t = 0: when the probe delivered the action's first input to the app (its mark, taken as it handled the
    command), not when the harness started the action: looking a target up by name is a round trip of its own,
    and is the harness's time, not the app's. Falls back to the action mark."""
    ta = next((t for t, m in marks if "now ACTION" in m), None)
    if ta is None:
        return default
    after = [(t, (m.split(" ") + ["", ""])[1].split("@", 1)[0]) for t, m in marks if t >= ta]  # "<id> <command> ..."
    # a pointer or key event first; an eval is the action only when none follows (a lookup is an eval too)
    for t, c in after:
        if c in INPUT_CMDS and c != "eval":
            return t
    for t, c in after:
        if c == "eval":
            return t
    return ta


def pair_halves(rd, pjobs):
    """The recordings the pairs `pjobs` show: {pair job's index: (left dir, right dir)}; a pair with a half that was
    not recorded has none (it is missing from the pack)."""
    found = {}
    for n, (pn, left, right, theme, _title) in enumerate(pjobs):
        ds = []
        for side in (left, right):
            name, _, th_ = side.partition("@")  # ("name@theme": a half of another theme, e.g. dark beside light)
            ds += sorted(glob.glob(os.path.join(rd, "strips-raw", f"{name}_{th_ or theme}_*")))[:1]
        if len(ds) == 2:
            found[n] = tuple(ds)
    return found


def strip_todo(raws, half_ds, pairtmp):
    """strip_job's arguments: each recording of `raws`, its half too where a pair shows it, and the halves of
    recordings with no strip of their own (e.g. another theme's: only their pair is published). A half and a whole
    strip have the same file name, so the halves have their own folder."""
    halves = os.path.join(pairtmp, "halves")
    todo = [(d, out, halves if d in half_ds else None) for d, out in raws]
    return todo + [(d, pairtmp, halves) for d in sorted(half_ds - {d for d, _ in raws})]


def pair_job(args):
    """Two transitions of one theme side by side in one image (states.STRIP_PAIRS: e.g. both directions of the
    mode switch): each drawn as its own merged image at 777 px (strip_job's halves), next to each other under one
    title."""
    sdir, pname, theme, title, parts = args
    ims = [Image.open(x["file"]).convert("RGB") for x in parts]
    ft = font(True, 24)
    W = sum(im.width for im in ims) + PAD
    tl = wrap(title, ft, W - 2 * PAD, ImageDraw.Draw(Image.new("RGB", (8, 8))))[:2]
    head = 14 + 32 * len(tl)
    S = Image.new("RGB", (W, head + max(im.height for im in ims)), (34, 36, 40))
    d = ImageDraw.Draw(S)
    for i, ln in enumerate(tl):
        d.text((PAD, 8 + 32 * i), ln, fill=(255, 255, 255), font=ft)
    x = 0
    for im in ims:
        S.paste(im, (x, head))
        x += im.width + PAD
    fname = f"pair_{pname}_{theme}.png"
    S.save(os.path.join(sdir, fname), compress_level=3)
    return [dict(file=fname, pages=[], name=f"pair:{pname}", theme=theme, desc=title, frames=0, shown=0,
                 duration_ms=0, fps=0, crop=None, zoom=1, note="", size=list(S.size), raw="",
                 caption=" ".join(f"{side}: {x['name']}. {x['caption']}" for side, x in zip(("Left", "Right"), parts)),
                 metrics=None)]


def frames11_job(args):
    """A transition's frames at 1:1 (states.FRAMES11): one row per frame, crops in window coordinates, fixed or
    following an item of the probe's trace (its rect at that frame); beside them the opacity of the named items and
    the geometry curves (an item's y or height, or the smallest y of a set of rows). Never scaled."""
    d, outdir, spec = args
    import fnmatch
    import motion_strip
    frames, marks, meta = strip_frames(d)
    if len(frames) < 2:
        raise RuntimeError(f"frames11 {meta['name']}: no frames")
    t0 = action_time(marks, frames[0]["t"])
    times = [f["t"] - t0 for f in frames]
    ims = LazyFrames(d, frames)
    tracks, _ = motion_strip.read_trace(d, t0)
    ox, oy = (meta.get("crop") or [0, 0])[:2]
    sc = float(meta.get("scale", 1) or 1)

    def keys(pat):
        ks = sorted((k for k in tracks if fnmatch.fnmatch(k, pat)), key=lambda k: ("#" in k, len(k)))
        if not ks:
            raise LookupError(f"frames11 {meta['name']}: no item {pat} in the trace")
        return ks

    def rect_at(k, t):
        tr, r = tracks[k], None
        for (tp, v), rr in zip(tr["pts"], tr["rects"]):
            if tp <= t + 0.5:
                r = (rr, v)
        return r
    changed = [k for k in range(1, len(ims)) if times[k] >= -1 and
               ImageChops.difference(ims[k - 1], ims[k]).convert("L").point(lambda v: 255 if v > 2 else 0).getbbox()]
    if not changed:
        raise RuntimeError(f"frames11 {meta['name']}: nothing changed after the action")
    a, b = max(0, changed[0] - 1), min(len(ims) - 1, changed[-1] + 1)
    sel = list(range(a, b + 1))
    # rows: as many as fit the core's 1:1 budget (CORE_MP_HARD) with the header and the graphs
    _crop_h = max((c[1][1][3] if c[1][0] == "fixed" else c[1][3]) for c in spec["crops"]) + 8
    _w = PAD + 92 + sum(360 for _ in spec["crops"]) + 12 * (len(spec["crops"]) - 1) + PAD
    _over = 90 + 22 + 16 + 24 + 120 + 18 * max(len(spec["opacity"]), len(spec["geometry"]))  # title, graphs, legends
    MAXR = max(5, min(spec.get("rows", 12), int((0.98 * CORE_MP_HARD / _w - _over) // _crop_h)))  # (2 % slack)
    if len(sel) > MAXR:
        sel = sorted({sel[round(i * (len(sel) - 1) / (MAXR - 1))] for i in range(MAXR)})
    crops = []
    for label, how in spec["crops"]:
        rw = int(round(ims[0].width / sc + ox))  # the recording's right edge, in window coordinates
        if how[0] == "fixed":
            fx, fy, fw, fh = how[1]
            fw = rw - fx if fw is None else fw  # None: to the recording's edge (the sidebar's width, from the app)
            crops.append((label, lambda t, r=(fx, fy, fw, fh): r, fw, fh))
        else:  # ("follow", pattern, pad_top, height, x, width)
            _, pat, pad, hh, xx, ww = how
            ww = rw - xx if ww is None else ww
            k0 = keys(pat)[0]
            def at(t, k0=k0, pad=pad, hh=hh, xx=xx, ww=ww):
                got = rect_at(k0, t)
                y = (got[0][1] if got else tracks[k0]["rect"][1]) - pad
                return (xx, y, ww, hh)
            crops.append((label, at, ww, hh))
    f_t, f_l, f_s = motion_strip.font(True, 22), motion_strip.font(True, 15), motion_strip.font(False, 14)
    LW, G = 92, 12
    W = PAD + LW + sum(c[2] for c in crops) + G * (len(crops) - 1) + PAD
    probe = ImageDraw.Draw(Image.new("RGB", (8, 8)))
    title = f"{meta['name']} · {meta['theme']} — {spec['title']}"
    trv = max((max(r_[1] for r_ in tracks[k]["rects"]) - min(r_[1] for r_ in tracks[k]["rects"]))
              for k in keys("*libraryRowContent/libraryRowTitle*"))
    trav = f"The library's rows travel {trv:.0f} px. "
    cap = (trav + f"Actual pixels (1:1). Frames {times[a]:+.0f} to {times[b]:+.0f} ms after the action ({b - a + 1} rendered"
           + (f", {len(sel)} shown evenly" if len(sel) < b - a + 1 else ", all shown") + "). " + spec.get("note", ""))
    tl = wrap(title, f_t, W - 2 * PAD, probe)[:2]
    cl = wrap(cap, f_s, W - 2 * PAD, probe)[:4]
    head = 10 + 30 * len(tl) + 19 * len(cl) + 8
    row_h = max(c[3] for c in crops) + 8
    hdr = 22
    GH = 120
    while True:  # within the 1:1 budget as drawn (the caption's length varies): drop rows evenly until it fits
        H = head + hdr + row_h * len(sel) + 16 + 24 + GH + 30 + 18 * max(len(spec["opacity"]), len(spec["geometry"]), 1)
        if W * H <= 0.99 * CORE_MP_HARD or len(sel) <= 3:
            break
        sel = sel[:len(sel) // 2] + sel[len(sel) // 2 + 1:]
    S = Image.new("RGB", (W, int(H)), (34, 36, 40))
    dr = ImageDraw.Draw(S)
    y = 10
    for ln in tl:
        dr.text((PAD, y), ln, fill=(255, 255, 255), font=f_t); y += 30
    for ln in cl:
        dr.text((PAD, y), ln, fill=(215, 215, 215), font=f_s); y += 19
    y = head
    x = PAD + LW
    for label, _, cw, _ in crops:
        dr.text((x, y), label, fill=(255, 220, 120), font=f_l); x += cw + G
    y += hdr
    for k in sel:
        dr.text((PAD, y + 4), f"{times[k]:+.0f} ms", fill=(235, 235, 235), font=f_l)
        x = PAD + LW
        for _, at, cw, chh in crops:
            cx, cy, _, _ = at(times[k])
            box = (int((cx - ox) * sc), int((cy - oy) * sc), int((cx - ox + cw) * sc), int((cy - oy + chh) * sc))
            S.paste(ims[k].crop(box), (x, y))
            x += cw + G
        y += row_h
    # graphs: opacity (left), geometry in px (right), over the shown frames' span
    y += 16
    t_lo, t_hi = times[a] - 10, times[b] + 10
    half = (W - 4 * PAD) // 3  # three panels: opacity, geometry, the header's velocity
    ops, absent = [], []
    for lab, p_ in spec["opacity"]:  # optional: an item the build no longer has is named, not drawn
        try:
            ops.append((lab, list(tracks[keys(p_)[0]]["pts"])))
        except LookupError:
            absent.append(lab)
    panels = [("Opacity" + (f" (not in this build: {', '.join(absent)})" if absent else ""), ops, (0.0, 1.0)),
              ("Geometry, px (window coordinates)", [], None)]
    for lab, p_, what in spec["geometry"]:
        if what == "miny":  # the smallest y of a set of rows (the first visible one), at every point of any of them
            ks = keys(p_)
            ts = sorted({t for k in ks for t, _ in tracks[k]["pts"]})
            pts = []
            for t in ts:
                ys = [r_[0][1] for k in ks for r_ in [rect_at(k, t)] if r_ and r_[1] > 0.05]
                if ys:
                    pts.append((t, min(ys)))
        else:
            k0 = keys(p_)[0]
            pts = [(t, rr[1] if what == "y" else rr[3]) for (t, _), rr in zip(tracks[k0]["pts"], tracks[k0]["rects"])]
        panels[1][1].append((lab, pts))
    allv = [v for _, pts in panels[1][1] for _, v in pts] or [0, 1]
    panels[1] = (panels[1][0], panels[1][1], (min(allv) - 10, max(allv) + 10))
    # the library header's velocity (px/s) between its traced positions
    vel = []
    hy = panels[1][1][0][1] if panels[1][1] else []  # the first geometry series (the library's first row)
    for (ta, ya), (tb, yb) in zip(hy, hy[1:]):
        if tb > ta:
            vel.append((ta, (yb - ya) / (tb - ta) * 1000.0))
    if hy:
        vel.append((hy[-1][0], 0.0))
    vv = [v for _, v in vel] or [0.0]
    panels.append(("Library rows' velocity, px/s", [("first row velocity", vel)], (min(vv + [0.0]) - 50, max(vv + [0.0]) + 50)))
    cols = [(255, 140, 90), (120, 230, 140), (120, 190, 255), (255, 220, 120), (220, 140, 255)]
    for i, (gt, series, (v0, v1)) in enumerate(panels):
        gx0 = PAD + i * (half + PAD) + 40
        gx1 = PAD + i * (half + PAD) + half
        gy0, gy1 = y + 24, y + 24 + GH
        dr.text((gx0 - 40, y), gt[:52], fill=(235, 235, 235), font=f_l)
        dr.rectangle((gx0, gy0, gx1, gy1), outline=(110, 110, 110))
        X = lambda t: gx0 + (t - t_lo) / max(1e-6, t_hi - t_lo) * (gx1 - gx0)
        Y = lambda v: gy1 - (v - v0) / max(1e-6, v1 - v0) * (gy1 - gy0)
        for v in (v0, (v0 + v1) / 2, v1):
            dr.text((gx0 - 38, Y(v) - 8), f"{v:.1f}" if v1 <= 1.0 else f"{v:.0f}", fill=(170, 170, 170), font=f_s)
        for k in sel:
            dr.line((X(times[k]), gy1, X(times[k]), gy1 + 4), fill=(255, 220, 120))
        if t_lo <= 0 <= t_hi:
            dr.line((X(0), gy0, X(0), gy1), fill=(255, 255, 255))
        for j, (lab, pts) in enumerate(series):
            col = cols[j % len(cols)]
            pts = [(t, v) for t, v in pts]
            pts = [(t_lo, next((v for t, v in reversed(pts) if t <= t_lo), pts[0][1] if pts else v0))] + \
                  [(t, v) for t, v in pts if t_lo < t < t_hi] + [(t_hi, None)]
            for (ta, va), (tb, vb) in zip(pts, pts[1:]):
                dr.line((X(ta), Y(va), X(tb), Y(va)), fill=col, width=2)
                if vb is not None:
                    dr.line((X(tb), Y(va), X(tb), Y(vb)), fill=col, width=2)
            dr.rectangle((gx0 + 6 + j * 0, gy1 + 10 + j * 18, gx0 + 24, gy1 + 18 + j * 18), fill=col)
            dr.text((gx0 + 30, gy1 + 6 + j * 18), lab, fill=(215, 215, 215), font=f_s)
    H2 = gy1 + 30 + 18 * max(max(len(p_[1]) for p_ in panels), 1)
    S = S.crop((0, 0, W, int(min(S.height, max(H2, y + 24 + GH + 20)))))
    th = meta["theme"]
    fname = f"{meta['name']}-1to1_{th}.png"
    S.save(os.path.join(outdir, fname), compress_level=3)
    return [dict(file=fname, pages=[], name=meta["name"] + "-1to1", theme=th, desc=spec["title"] + " (1:1 frames)",
                 frames=len(sel), shown=len(sel), duration_ms=0, fps=0, crop=None, zoom=1, note="", size=list(S.size),
                 one_to_one=True,
                 raw="", caption=cap, metrics=None)]


def perceived_sheet(rd, sdir, name, title, rows, strips, tile_h=400, moments=5):
    """The whole recorded region (the left column of the art changes) as perceived at real speed, one row
    per recording: `moments` images from one frame before its main transition to 50 ms after it, each the
    time-weighted mean of the frames on screen in the 45 ms before t (motion_strip.perceived: a SIMULATION)."""
    import motion_strip
    out_rows = []
    one_to_one = True
    for strip, theme, label, *ro in rows:
        ro = ro[0] if ro else {}
        ds = sorted(glob.glob(os.path.join(rd, "strips-raw", f"{strip}_{theme}_*")))
        m = next((x.get("metrics") for x in strips if x["name"] == strip and x["theme"] == theme and x.get("metrics")), None)
        if not ds or not m or not m.get("segments"):
            raise LookupError(f"perceived sheet {name}: no recording or transition for {strip} ({theme})")
        frames, marks, meta = strip_frames(ds[0])
        t0 = action_time(marks, frames[0]["t"])
        ims = LazyFrames(ds[0], frames)
        times = [f["t"] - t0 for f in frames]
        t_stop = next((t for t, mk in marks if "stripstop" in mk), None)
        end = (t_stop - t0) if t_stop else times[-1] + motion_strip.FRAME_MS
        spans = [(times[k], times[k + 1] if k + 1 < len(times) else end) for k in range(len(times))]
        sg = max(m["segments"], key=lambda g: g["duration_ms"])
        a_, b_ = sg["start_ms"] - motion_strip.FRAME_MS, sg["start_ms"] + sg["duration_ms"] + 50
        box = (0, 0, ims[0].width, ims[0].height)
        if ro.get("items"):  # only the region these items cover while shown (union of their rects), padded
            tracks, _ = motion_strip.read_trace(ds[0], t0)
            off = tuple(meta.get("crop") or (0, 0))[:2]
            sc_ = meta.get("scale", 1)
            # (at "end": only where they are shown at the recording's end, e.g. the text block's final place)
            rs = [r_ for k, tr in tracks.items() if any(fnmatch.fnmatch(k, p_) for p_ in ro["items"])
                  for r_, (_, v) in (list(zip(tr["rects"], tr["pts"]))[-1:] if ro.get("at") == "end"
                                     else zip(tr["rects"], tr["pts"])) if v > 0.05 and r_[2] > 0 and r_[3] > 0]
            if not rs:
                raise LookupError(f"perceived sheet {name}: no item {ro['items']} shown in {strip} ({theme})")
            pd_ = ro.get("pad", 6)
            x0 = max(0, int((min(r_[0] for r_ in rs) - off[0] - pd_) * sc_))
            y0 = max(0, int((min(r_[1] for r_ in rs) - off[1] - pd_) * sc_))
            x1 = min(ims[0].width, int((max(r_[0] + r_[2] for r_ in rs) - off[0] + pd_) * sc_))
            y1 = min(ims[0].height, int((max(r_[1] + r_[3] for r_ in rs) - off[1] + pd_) * sc_))
            box = (x0, y0, x1 - x0, y1 - y0)
        n_m = ro.get("moments", moments)
        tiles = []
        for i in range(n_m):
            T = a_ + (b_ - a_) * i / (n_m - 1)
            im = motion_strip.perceived(ims, times, spans, box, T)
            th_ = ro.get("tile_h") or (im.height if ro.get("items") else tile_h)  # an item region: 1:1
            f = th_ / im.height
            if abs(f - 1) > 1e-6:
                one_to_one = False
                im = im.resize((int(im.width * f), th_), Image.LANCZOS)
            tiles.append((T, im))
        where = (f", the region of {', '.join(ro['items'])}{' where they end' if ro.get('at') == 'end' else ''} "
                 f"({box[2]}x{box[3]} px at {box[0]},{box[1]})" if ro.get("items") else "")
        scale_txt = "1:1" if abs(tiles[0][1].height / box[3] - 1) < 1e-6 else f"shown at {tiles[0][1].height / box[3]:.2f}x"
        per = ro.get("per_line") or n_m
        for j in range(0, n_m, per):
            out_rows.append(((f"{label}: transition +{sg['start_ms']} to +{sg['start_ms'] + sg['duration_ms']} ms{where}, "
                              f"{scale_txt}") if j == 0 else "", tiles[j:j + per]))
    f_t, f_l, f_s = motion_strip.font(True, 22), motion_strip.font(True, 15), motion_strip.font(False, 14)
    g = 12
    W = max(sum(t.width for _, t in tl) + g * (len(tl) - 1) for _, tl in out_rows) + 2 * PAD
    probe = ImageDraw.Draw(Image.new("RGB", (8, 8)))
    tls = wrap(title, f_t, W - 2 * PAD, probe)[:2]
    note = wrap("Perceived at real speed, a SIMULATION, not rendered frames: each image is the time-weighted mean of what "
                "was on screen over the 45 ms before t. A cross-fade reads as a soft blend; a jump or a dip shows. It "
                "overstates ghosting: the eye follows moving text, a mean does not.",
                f_s, W - 2 * PAD, probe)[:3]
    head = 10 + 30 * len(tls) + 19 * len(note) + 8
    rh_ = lambda tl: max(t.height for _, t in tl)
    S = Image.new("RGB", (W, head + sum(22 + 18 + rh_(tl) + 14 for _, tl in out_rows)), (34, 36, 40))
    d = ImageDraw.Draw(S)
    y = 10
    for ln in tls:
        d.text((PAD, y), ln, fill=(255, 255, 255), font=f_t); y += 30
    for ln in note:
        d.text((PAD, y), ln, fill=(255, 220, 120), font=f_s); y += 19
    y = head
    for label, tl in out_rows:
        d.text((PAD, y), label, fill=(235, 235, 235), font=f_l)
        x = PAD
        for T, im in tl:
            d.text((x, y + 22), f"t = {T:.0f} ms", fill=(255, 255, 255), font=f_s)
            S.paste(im, (x, y + 40))
            x += im.width + g
        y += 22 + 18 + rh_(tl) + 14
    theme = rows[0][1]
    fname = f"{name}_{theme}.png"
    S.save(os.path.join(sdir, fname), compress_level=3)
    return dict(file=fname, pages=[], name=name, theme=theme, desc=title, frames=0, shown=0, duration_ms=0, fps=0,
                crop=None, zoom=1, note="", size=list(S.size), raw="", caption="", metrics=None, one_to_one=one_to_one)


def strip_job(args):
    """One recording, measured once and drawn at each size the pack needs: (dir, out dir, half dir). Returns (its
    strips' records, its half in a pair: dict(file, name, caption), or None without a half dir or a drawable one)."""
    d, outdir, half_dir = args
    frames, marks, meta = strip_frames(d)
    if len(frames) < 2:
        return [], None
    t0 = action_time(marks, frames[0]["t"])
    ims = LazyFrames(d, frames)
    half = None
    diffs = [None]
    for k in range(1, len(ims)):
        # > 2 levels: a 100 ms fade changes a tint by only a few levels per frame (e.g. a hover fade).
        diffs.append(ImageChops.difference(ims[k - 1], ims[k]).convert("L").point(lambda v: 255 if v > 2 else 0).getbbox())
    changed = [b is not None for b in diffs]
    # The region that changes: union of strong differences from the first frame.
    box = None
    for k in range(1, len(ims)):
        b = ImageChops.difference(ims[0], ims[k]).convert("L").point(lambda v: 255 if v > 40 else 0).getbbox()
        if b:
            box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
    # a crop of the recording: STRIP_MODE "view" (any window), else STRIP_VIEW (main-window crops only)
    view = st.STRIP_MODE.get(meta["name"], {}).get("view") or (
        None if "win=" in meta.get("opts", "") else st.STRIP_VIEW.get(meta["name"]))
    if view is not None:  # a named view (an item the strip recorded): resolved from the app's geometry
        import motion_strip
        view = motion_strip.resolve_rect(view, meta, ims[0].width, ims[0].height)
    iw, ih = ims[0].size
    if view:
        r = grow(list(view), iw, ih, 0, 0, 0)
        note = "view crop"
    elif box:
        r = grow([box[0], box[1], box[2] - box[0], box[3] - box[1]], iw, ih, min(iw, 720), min(ih, 120), 24)
        note = "cropped to the changing region"
    else:
        r = [0, 0, iw, ih]
        note = ""
    # Key frames: per run of changes, the frame before, its first and last frame, evenly spaced
    # frames in between; then the frame after the last change. At most 12.
    after_t0 = [k for k in range(len(frames)) if frames[k]["t"] >= t0 - 1]
    first = after_t0[0] if after_t0 else 0
    runs, cur = [], None
    for k in range(max(1, first), len(frames)):
        if changed[k]:
            if cur and k - cur[1] <= 3:
                cur[1] = k
            else:
                cur = [k, k]
                runs.append(cur)
    pick = {max(0, first - 1)}
    budget = 12
    if runs:
        for a, b in runs:
            pick.update({max(0, a - 1), a, b})
        pick.add(min(len(frames) - 1, runs[-1][1] + 1))
        spare = budget - len(pick)
        total = sum(b - a for a, b in runs) or 1
        for a, b in runs:
            n = max(0, int(round(spare * (b - a) / total)))
            for j in range(1, n + 1):
                pick.add(a + int(round(j * (b - a) / (n + 1))))
    sel = sorted(pick)
    mode = st.STRIP_MODE.get(meta["name"], {})
    if "window" in mode:  # every rendered frame in a window after the action, plus the one before
        a_ms, b_ms = mode["window"]
        sel = [k for k in range(len(frames)) if t0 + a_ms <= frames[k]["t"] <= t0 + b_ms]
        sel = sorted(set(sel) | {max(0, first - 1)})
    while "window" not in mode and len(sel) > 12:  # drop interior frames evenly
        inner = [k for k in sel[1:-1] if k not in {r_[0] for r_ in runs} and k not in {r_[1] for r_ in runs}]
        if not inner:
            sel = sel[:12]
            break
        sel.remove(inner[len(inner) // 2])
    if len(sel) < 8 and "window" not in mode:  # pad with evenly spaced frames for slow changes
        extra = [int(round(j * (len(frames) - 1) / 9)) for j in range(10)]
        sel = sorted(set(sel) | set(extra))[:12]
    x, y, w, h = r
    up = mode.get("zoom", 1)
    if w * up < MIN_TILE and not mode.get("free"):
        up = min(3, math.ceil(MIN_TILE / w))
    tiles = []
    for k in sel:
        im = ims[k].crop((x, y, x + w, y + h))
        if up > 1:
            im = im.resize((w * up, h * up), Image.NEAREST)
        dt = frames[k]["t"] - frames[k - 1]["t"] if k else 0
        role = ("before" if frames[k]["t"] < t0 else "")
        tiles.append((im.width, im.height, dict(image=im, k=k, t=frames[k]["t"] - t0, dt=dt, changed=changed[k],
                                               role=role, nearest=up > 1)))
    compact = mode.get("free", False)  # small tiles: a two-line label
    label_h = 44 if compact else 30
    f_b, f_r = font(True, 17 if compact else 20), font(False, 15 if compact else 17)

    def label(dr, xx, yy, tw, p):
        col = (255, 220, 120) if p["changed"] else (180, 180, 180)
        if compact:
            dr.text((xx, yy), f"#{p['k']} t={p['t']:+.0f} ms", fill=col, font=f_b)
            dr.text((xx, yy + 21), f"+{p['dt']:.0f} ms {'changed' if p['changed'] else 'same'}"
                    f"{' (before)' if p['t'] < 0 else ''}", fill=col, font=f_r)
            return
        dr.text((xx, yy + 2), f"#{p['k']}  t = {p['t']:+.0f} ms   (+{p['dt']:.0f} ms, "
                f"{'changed' if p['changed'] else 'same'}){'  before the action' if p['t'] < 0 else ''}",
                fill=col, font=f_b if p["changed"] else f_r)

    total = frames[-1]["t"] - frames[0]["t"]
    fps = (len(frames) - 1) / total * 1000 if total > 0 else 0
    base = os.path.basename(d.rstrip("/"))
    # One merged image per transition (strips/<base>.png): caption, key frames, the opacity/ink graph with the
    # animations that ran, and the perceived-at-real-speed rows. The frame pages (strips/frames/) are reference.
    mstats, caption, merged_file = None, "", None
    extra = []
    try:
        import motion_strip
        t_stop = next((t for t, m in marks if "stripstop" in m), None)
        focus = getattr(st, "MOTION_FOCUS", {}).get(meta["name"])
        mstats, caption, draw = motion_strip.measure(frames, ims, t0, (x, y, w, h), sel, meta, d, zoom=up,
                                                     t_stop=t_stop, focus=focus, nearest=up > 1,
                                                     periodic=meta["name"] in getattr(st, "PERIODIC_UPDATES", ()),
                                                     elements=getattr(st, "ELEMENT_INK", {}).get(meta["name"]))
        if half_dir:
            try:  # (a half that cannot be drawn fails its pair, not the recording's own strip)
                os.makedirs(half_dir, exist_ok=True)
                hp = os.path.join(half_dir, base + ".png")
                draw(*HALF_SIZE).save(hp, compress_level=1)  # (read back once, by pair_job)
                half = dict(file=hp, name=meta["name"], caption=caption)
            except Exception:
                import traceback
                traceback.print_exc()
        mimg = draw(*STRIP_SIZE)
        merged_file = base + ".png"
        mimg.save(os.path.join(outdir, merged_file), compress_level=3)
        # more images of this recording for other regions (states.EXTRA_VIEWS), each its own transition entry
        for suffix, view in getattr(st, "EXTRA_VIEWS", {}).get(meta["name"], []):
            iw_, ih_ = ims[0].size
            rx = list(view) if view else [0, 0, iw_, ih_]
            m2 = dict(meta, name=f"{meta['name']}-{suffix}", desc=meta["desc"] + f" ({'the whole window' if not view else suffix})")
            st2, cap2, im2 = motion_strip.analyse(frames, ims, t0, tuple(rx), sel, m2, d, zoom=1, t_stop=t_stop,
                                                  focus=getattr(st, "MOTION_FOCUS", {}).get(m2["name"]),
                                                  elements=getattr(st, "ELEMENT_INK", {}).get(meta["name"]))
            f2 = base.replace(meta["name"], m2["name"], 1) + ".png"
            im2.save(os.path.join(outdir, f2), compress_level=3)
            extra.append(dict(file=f2, pages=[], name=m2["name"], theme=meta["theme"], desc=m2["desc"],
                              frames=len(frames), shown=len(sel), duration_ms=0, fps=0, crop=rx, zoom=1, note="",
                              size=list(im2.size), raw=f"strips-raw/{base}/", caption=cap2, metrics=st2))
    except Exception as ex:  # the frame pages still ship
        import traceback
        traceback.print_exc()
        caption = f"(motion analysis failed: {ex})"
    title = f"{meta['name']} · {meta['theme']} — {meta['desc']}. {caption}"
    pages = layout(tiles, 48 if "window" in mode else 12, label_h, 0 if mode.get("free") else MIN_TILE)
    fdir = os.path.join(outdir, "frames")
    os.makedirs(fdir, exist_ok=True)
    page_files = []
    for i, (cols, tw, ts) in enumerate(pages):
        name = base + (f"_p{i + 1}" if len(pages) > 1 else "") + ".png"
        render_page(title + (f" ({i + 1}/{len(pages)})" if len(pages) > 1 else ""), cols, tw, ts,
                    label, os.path.join(fdir, name), label_h)
        page_files.append("frames/" + name)
    size = None
    if merged_file:
        with Image.open(os.path.join(outdir, merged_file)) as merged_im:
            size = list(merged_im.size)
    recs = [dict(file=merged_file or page_files[0], pages=page_files, name=meta["name"], theme=meta["theme"],
                 desc=meta["desc"], frames=len(frames), shown=len(sel), duration_ms=round(total), fps=round(fps),
                 crop=r, zoom=up, note=note, size=size, raw=f"strips-raw/{base}/", caption=caption, metrics=mstats)] + extra
    return recs, half


class PhaseTimer:
    """Wall time of pack.py's steps, printed and written to <round>/timings/pack.tsv (step, seconds)."""

    def __init__(self, rd):
        import time
        self._now = time.monotonic
        self.path = os.path.join(rd, "timings", "pack.tsv")
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self.start = self.prev = self._now()
        open(self.path, "w").close()

    def __call__(self, step):
        now = self._now()
        with open(self.path, "a") as f:
            f.write(f"{step}\t{now - self.prev:.2f}\n")
        print(f"pack: {step} {now - self.prev:.1f} s", flush=True)
        self.prev = now

    def total(self):
        self.prev = self.start
        self("total")


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("round")
    ap.add_argument("src")
    ap.add_argument("--geometry", default="")
    ap.add_argument("--jobs", type=int, default=max(2, min(8, (os.cpu_count() or 4) // 2)))
    o = ap.parse_args()
    rd, src = os.path.abspath(o.round), os.path.abspath(o.src)
    phase = PhaseTimer(rd)
    entries = load_entries(rd)
    privacy_fails = redact_round(rd, entries)  # before any sheet, strip or pack is made of them
    phase("load+redact")
    geo = load_geometry(o.geometry)
    for e in entries:  # the shot's own geometry wins over a geometry pass
        if "popups" in e:
            geo[geo_key(e) + "@" + e["theme"]] = e
    sd, sdir = os.path.join(rd, "sheets"), os.path.join(rd, "strips")
    for dd in (sd, sdir, os.path.join(sdir, "frames")):
        os.makedirs(dd, exist_ok=True)
        for f in glob.glob(os.path.join(dd, "*.png")):
            os.remove(f)
    groups = collections.OrderedDict()
    for e in entries:
        if e["area"] in SKIP_AREAS:
            continue
        groups.setdefault((group_of(e), e["theme"]), []).append(e)
    order = sorted(groups, key=lambda k: (GROUP_ORDER.index(k[0]) if k[0] in GROUP_ORDER else 99, k[1] != "dark"))
    jobs = []
    for n, k in enumerate(order, 1):
        g, theme = k
        es = groups[k]
        # Geometry of this very shot first, then the geometry pass's (same state, any theme).
        gmap = {geo_key(e): geo.get(geo_key(e) + "@" + e["theme"], geo.get(geo_key(e), {})) for e in es}
        title = (f"Large font (Noto Sans 24 pt) — {GROUP_TITLES.get(g.split(':')[1], g)} — {theme}" if g.startswith("bigfont:")
                 else f"{GROUP_TITLES.get(g, g)} — {theme}")
        jobs.append((rd, title, os.path.join(sd, f"{n:02d}_{g.replace(':', '-')}_{theme}.png"), es, gmap, g, theme))
    # Comparison sheets (states.COMPARE_SHEETS): each state's dark and light shot side by side.
    by_key = {(e["area"], e["state"], e["theme"], e["size"], e["scale"], "font24" in e["file"]): e for e in entries}
    for c in getattr(st, "COMPARE_SHEETS", []):
        es = []
        for area, state in c["states"]:
            for theme in ("dark", "light"):
                e = by_key.get((area, state, theme, "1600x1000", 1, False))
                if e:
                    es.append(e)
        if es:
            gmap = {geo_key(e): geo.get(geo_key(e) + "@" + e["theme"], geo.get(geo_key(e), {})) for e in es}
            jobs.insert(0, (rd, c["title"] + " (dark left, light right)", os.path.join(sd, f"00_compare-{c['name']}.png"),
                            es, gmap, f"compare:{c['name']}", "both"))
    # Curated sheets (states.PICK_SHEETS): named states per theme, tiles as on their area's sheets.
    def variant_key(area, state, theme, var=None):
        parts = (var or "").split("+")
        size = next((p_ for p_ in parts if "x" in p_), "1600x1000")
        return (area, state, theme, size, 1, "font24" in parts)
    for c in getattr(st, "PICK_SHEETS", []):
        for theme in c["themes"]:
            es = [by_key[k] for k in (variant_key(*((t[0], t[1], theme) + tuple(t[2:]))) for t in c["states"]) if k in by_key]
            missing = len(c["states"]) - len(es)
            if missing:
                print(f"pack: pick sheet {c['name']} ({theme}): {missing} state(s) not shot", file=sys.stderr)
            if es:
                gmap = {geo_key(e): geo.get(geo_key(e) + "@" + e["theme"], geo.get(geo_key(e), {})) for e in es}
                jobs.insert(0, (rd, f"{c['title']} — {theme}", os.path.join(sd, f"00_pick-{c['name']}_{theme}.png"),
                                es, gmap, f"pick:{c['name']}", theme))
    # The overview ("every area at rest"), one page per theme, first in the core set.
    for theme in ("light", "dark"):
        es = [by_key[k] for k in ((a, s_, theme, "1600x1000", 1, False) for a, s_ in getattr(st, "OVERVIEW", []))
              if k in by_key]
        if es:
            gmap = {geo_key(e): geo.get(geo_key(e) + "@" + e["theme"], geo.get(geo_key(e), {})) for e in es}
            jobs.insert(0, (rd, f"Every area at rest — {theme}", os.path.join(sd, f"00_overview_{theme}.png"),
                            es, gmap, "overview", theme))
    # The core set's sheets again, at the core size (ring and icon sheets are built to stay 1x as they are).
    os.makedirs(os.path.join(sd, "core"), exist_ok=True)
    for f in glob.glob(os.path.join(sd, "core", "*.png")):
        os.remove(f)
    for kind, group, theme in getattr(st, "CORE", []):
        if kind != "sheet" or group in {x[0] for x in getattr(st, "STACK_SHEETS", [])}:
            continue
        for j in list(jobs):
            if j[5] == group and j[6] == theme and len(j) == 7:
                jobs.append(j[:2] + (os.path.join(sd, "core", os.path.basename(j[2])),) + j[3:] + (True,))
                break
    raws = []
    for d in sorted(glob.glob(os.path.join(rd, "strips-raw", "*"))):
        if not os.path.exists(os.path.join(d, "frames.tsv")):
            continue
        meta = json.load(open(os.path.join(d, "meta.json")))
        if meta["theme"] == "dark" or meta["name"] in st.STRIP_BOTH_THEMES:
            raws.append((d, sdir))
    # --focus-only (SHOT_FOCUS_ONLY=1): only what the scope's images need (its recordings and pairs; no area,
    # pick or custom sheets): a fast iteration on one surface. A full run packs everything.
    fo_need = None
    if os.environ.get("SHOT_FOCUS_ONLY") == "1" and os.environ.get("SHOT_FOCUS"):
        fo_need = focus_strip_needs(focus_scope_names())
        raws = [r_ for r_ in raws if json.load(open(os.path.join(r_[0], "meta.json")))["name"] in fo_need]
        jobs = []
    # (most frames first: the order perceived_sheet takes a recording's metrics in, when variants share a name)
    raws.sort(key=lambda r: -len(glob.glob(os.path.join(r[0], "f*.png"))))
    pjobs = [(pn, a_, b_, th, title) for pn, a_, b_, title, themes in getattr(st, "STRIP_PAIRS", []) for th in themes
             if fo_need is None or (a_.partition("@")[0] in fo_need and b_.partition("@")[0] in fo_need)]
    halves_of = pair_halves(rd, pjobs)
    half_ds = {d for ds in halves_of.values() for d in ds}
    pairtmp = os.path.join(sdir, "pairtmp")
    todo = strip_todo(raws, half_ds, pairtmp)
    # Costliest strips first (the frames' bytes), ahead of the sheets, so the pool doesn't end on one long job.
    cost = {j[0]: sum(f.stat().st_size for f in os.scandir(j[0]) if f.name.endswith(".png")) for j in todo}
    todo.sort(key=lambda j: (-cost[j[0]], j[0]))
    recs_of, halves = {}, {}
    with ProcessPoolExecutor(o.jobs) as ex:
        strip_f = ex.map(strip_job, todo)
        sheet_f = ex.map(sheet_job, jobs)
        for (d, out, _half_dir), (recs, half) in zip(todo, strip_f):
            recs_of[d] = recs
            if half:
                halves[d] = half
        strips = [s for d, _ in raws for s in recs_of[d]]
        sheets = [s for r in sheet_f for s in r]
        phase(f"sheets+strips ({len(jobs)} sheet jobs, {len(todo)} recordings)")
        composed = [(sdir, pn, th, title, [halves[d] for d in halves_of[n]])
                    for n, (pn, _a, _b, th, title) in enumerate(pjobs)
                    if n in halves_of and all(d in halves for d in halves_of[n])]
        pair_strips = [x for r in ex.map(pair_job, composed) for x in r]
        phase(f"pairs ({len(composed)} of {len(pjobs)})")
    shutil.rmtree(pairtmp, ignore_errors=True)
    fjobs = [(d, sdir, getattr(st, "FRAMES11", {})[json.load(open(os.path.join(d, "meta.json")))["name"]]) for d, _ in raws
             if json.load(open(os.path.join(d, "meta.json")))["name"] in getattr(st, "FRAMES11", {})]
    for fj in fjobs:  # loudly: a missing item is a failed image (the core check then fails)
        try:
            strips += frames11_job(fj)
        except (LookupError, RuntimeError) as ex:
            print(f"pack: {ex}", file=sys.stderr)
    phase(f"frames11 ({len(fjobs)})")
    for nm, title, rows_ in getattr(st, "PERCEIVED_SHEETS", []):  # loudly: a missing recording fails the core check
        try:
            strips.append(perceived_sheet(rd, sdir, nm, title, rows_, strips))
        except LookupError as ex:
            print(f"pack: {ex}", file=sys.stderr)
    phase("perceived sheets")
    strips += pair_strips
    strips.sort(key=lambda s: s["file"])
    for nm, title, theme, rows in ([] if fo_need is not None else getattr(st, "CUSTOM_SHEETS", [])):
        try:
            x = custom_sheet(rd, sd, entries, nm, title, theme, rows)
        except LookupError as ex:  # a missing state: the core check lists the sheet as missing
            print(f"pack: {ex}", file=sys.stderr)
            x = None
        if x:
            sheets.insert(0, x)
    phase("custom sheets")
    for nm, area, state, title, *so in ([] if fo_need is not None else getattr(st, "STACK_SHEETS", [])):
        x = stack_sheet(rd, sd, entries, nm, area, state, title, so[0] if so else None)
        if x:
            sheets.insert(0, x)
    phase("stack sheets")
    json.dump(sheets, open(os.path.join(sd, "sheets.json"), "w"), indent=1)
    json.dump(strips, open(os.path.join(sdir, "strips.json"), "w"), indent=1)
    geo_s = geometry_samples(rd, strips)
    phase("geometry samples")
    write_motion_metrics(rd, strips, geo_s)
    phase("MOTION-METRICS.md")
    bad = check_dialogs(rd, entries)
    bad = write_index(rd, src, entries, sheets, strips, dialog_failures=bad, geo_samples=geo_s)
    phase("INDEX.md, core set, CORE-FACTS.md")
    if os.environ.get("SHOT_FOCUS"):  # the focus packs (focus.py), after the whole pack: focus-build/<scope>/ each
        try:
            names = focus_scope_names()
        except KeyError as ex:
            names, bad = [], list(bad or []) + [f"focus: no scope {ex}"]
        bad = list(bad or []) + write_focus_packs(rd, src, names, entries, sheets, strips, geo_s, o.jobs)
        phase(f"focus packs ({len(names)})")
    print(f"pack: {len(sheets)} sheets, {len(strips)} strip images")
    # the documents a reviewer reads: the home folder written "~" (the layout findings quote elided paths),
    # then none may still hold it
    home = os.path.expanduser("~")
    for mp in round_docs(rd):
        txt = open(mp, encoding="utf-8").read()
        if home in txt.translate(_INVISIBLE):
            open(mp, "w", encoding="utf-8").write(txt.translate(_INVISIBLE).replace(home, "~"))
        if home in open(mp, encoding="utf-8").read():
            privacy_fails.append(f"privacy: {os.path.relpath(mp, rd)} still names the home folder")
    bad = list(bad or []) + privacy_fails
    phase("documents' privacy")
    phase.total()
    if bad:  # loudly: dialog shots not of the dialogs their states name, core images missing, privacy
        print(f"pack: CHECKS FAILED for {len(bad)} item(s):", file=sys.stderr)
        for b_ in bad:
            print("  " + b_, file=sys.stderr)
        sys.exit(3)


def latency_baseline(strips):
    """The input-latency baseline (states: a harness rectangle recoloured on press, no animation): the harness's
    own input-to-frame delay, to subtract from an app's latency."""
    clicks = sorted(s["metrics"]["latency_ms"] for s in strips if s["name"].startswith("latency-baseline-click")
                    and s.get("metrics") and s["metrics"]["segments"])
    script = [s["metrics"]["latency_ms"] for s in strips if s["name"] == "latency-baseline-script"
              and s.get("metrics") and s["metrics"]["segments"]]
    if not clicks and not script:
        return ""
    parts = []
    if clicks:
        parts.append(f"a real click to the first changed frame {', '.join(str(c) for c in clicks)} ms (median "
                     f"{clicks[len(clicks) // 2]} ms)")
    if script:
        parts.append(f"a script change (the probe's eval) {script[0]} ms")
    return ("**Input-latency baseline** (a plain rectangle the harness adds, recoloured on press with no animation): "
            + "; ".join(parts) + ". Latency counts from the moment the probe delivers the input to the app (after "
            "any lookup of the target), so this is the shortest an instant change takes to reach a rendered, read-back "
            "frame: subtract it from a transition's latency to get the app's own response.")


def check_dialogs(rd, entries):
    """Every dialog shot of a state in states.DIALOG_EXPECT: its window's title, and the body text in that
    window's layout walk. Returns the mismatches (a confirmation grabbed as its parent window, a missing text)."""
    bad = []
    exp = getattr(st, "DIALOG_EXPECT", {})
    for e in entries:
        if e.get("area") != "dialog" or e.get("state") not in exp:
            continue
        title, body = exp[e["state"]]
        got = e.get("window", "")
        if not got.startswith(title):
            bad.append(f"{e['file']}: the shot is of '{got}', not of '{title}'")
            continue
        if body:
            texts = []
            try:
                lj = json.load(open(os.path.join(rd, e["layout"])))
                texts = [t.get("text", "") for w in lj.get("windows", []) if str(w.get("title", "")).startswith(title)
                         for t in w.get("texts", [])]
            except Exception:
                pass
            if not any(body in t for t in texts):
                bad.append(f"{e['file']}: '{title}' does not show '{body}'")
    return bad


def geometry_samples(rd, strips):
    """states.GEOMETRY_SAMPLES: items' rects from a recording's trace at evenly spaced times after its first
    transition starts. Returns {(name, theme): (times, rows, note)}; rows are (label, cells or None when the item is
    not in the trace)."""
    import numpy as np
    import motion_strip
    out = {}
    for name, (step, count, items) in getattr(st, "GEOMETRY_SAMPLES", {}).items():
        for d in sorted(glob.glob(os.path.join(rd, "strips-raw", f"{name}_*"))):
            meta = json.load(open(os.path.join(d, "meta.json")))
            if meta.get("name") != name:
                continue
            m = next((x.get("metrics") for x in strips if x["name"] == name and x["theme"] == meta["theme"]
                      and x.get("metrics")), None)
            frames, marks, _ = strip_frames(d)
            t0 = action_time(marks, frames[0]["t"])
            tracks, _ = motion_strip.read_trace(d, t0)
            grey = {}  # a frame as luminance, decoded once for every item measured in it
            start = m["segments"][0]["start_ms"] if m and m.get("segments") else 0
            times = [start + k * step for k in range(count)]

            def at(tr, T):
                i = max([j for j, (t, _) in enumerate(tr["pts"]) if t <= T] or [0])
                return tr["rects"][i], tr["pts"][i][1]
            rows, perframe = [], []
            for label, pat, kind in items:
                ks = [k for k in tracks if fnmatch.fnmatch(k, pat)]
                if not ks and kind == "frames":
                    perframe.append(f"{label}: not in the trace")
                    continue
                if not ks:
                    rows.append((label, None))
                    continue
                if kind == "lum":  # the mean luminance of the item's own (largest) rect in every rendered frame
                    k = max(ks, key=lambda k_: (len(tracks[k_]["pts"]), -len(k_)))
                    tr = tracks[k]
                    rr = max(tr.get("fulls") or tr["rects"], key=lambda q: q[2] * q[3])
                    off = tuple(meta.get("crop") or (0, 0))[:2]
                    sc_ = meta.get("scale", 1)
                    box = tuple(int(round(v)) for v in ((rr[0] - off[0]) * sc_, (rr[1] - off[1]) * sc_,
                                                        (rr[0] - off[0] + rr[2]) * sc_, (rr[1] - off[1] + rr[3]) * sc_))
                    vals = []
                    for f_ in frames:
                        t_ = f_["t"] - t0
                        if start - 40 <= t_ <= start + 600:
                            if f_["file"] not in grey:
                                with Image.open(os.path.join(d, f_["file"])) as gim:
                                    grey[f_["file"]] = gim.convert("L")
                            g = np.asarray(grey[f_["file"]].crop(box), dtype=np.float32)
                            vals.append(f"+{t_:.0f}: {g.mean():.1f}")
                    perframe.append(f"{label} (mean luminance 0-255 of its rect {rr[2]}x{rr[3]} at {rr[0]},{rr[1]}, every "
                                    f"rendered frame): " + ", ".join(vals))
                    continue
                if kind == "frames":  # every traced change of the item's size from just before to 600 ms after
                    k = max(ks, key=lambda k_: (len(tracks[k_]["pts"]), -len(k_)))
                    tr = tracks[k]
                    fl_ = tr.get("fulls") or tr["rects"]
                    pts = [(t, v, r_, f_) for (t, v), r_, f_ in zip(tr["pts"], tr["rects"], fl_) if start - 20 <= t <= start + 600]
                    perframe.append(f"{label} (each traced frame, t: its own w x h at x,y; shown (clipped) w x h; opacity): " + (", ".join(
                        f"+{t:.0f}: {f_[2]}x{f_[3]} at {f_[0]},{f_[1]}" + (f", shown {r_[2]}x{r_[3]}" if r_ != f_ else "") + f" · {v:.2f}"
                        for t, v, r_, f_ in pts) or "no change in the window"))
                    continue
                if kind == "miny":
                    cells = []
                    for T in times:
                        ys = [at(tracks[k], T)[0][1] for k in ks if at(tracks[k], T)[1] > 0.05]
                        cells.append(f"y {min(ys)}" if ys else "hidden")
                else:
                    k = max(ks, key=lambda k_: (round(tracks[k_]["pts"][-1][1], 2) if tracks[k_]["pts"] else 0,
                                                len(tracks[k_]["pts"]), -len(k_)))
                    cells = []
                    for T in times:
                        r_, v = at(tracks[k], T)
                        cells.append(f"{r_[2]}x{r_[3]} = {r_[2] / r_[3]:.3f} at x {r_[0]}" if kind == "aspect" and r_[3] else
                                     f"y {r_[1]} h {r_[3]} · {v:.2f}")
                rows.append((label, cells))
            out[(name, meta["theme"])] = (times, rows, f"t = ms after the action; the first transition starts at +{start} ms",
                                          perframe)
    return out


def geometry_table(times, rows):
    L = ["| item | " + " | ".join(f"+{t}" for t in times) + " |", "|---|" + "---|" * len(times)]
    for label, cells in rows:
        L.append(f"| {label} | " + (" | ".join(cells) if cells else f"not in the trace ({len(times)} samples)") + " |")
    return L


def after_cell(m, side):
    """MOTION-METRICS' after-end cells: the app's items, or tooltips/popups; older metrics have one total."""
    v = m.get(f"after_end_{side}_ms")
    if v is None:
        if side == "app" and m.get("moves_after_ms") is not None and "after_end_app_ms" not in m:
            return f"{m['moves_after_ms']} ms (not split)"
        return "—"
    w = m.get(f"after_end_{side}_what")
    return f"{v} ms" + (f" ({w.replace('|', '/')})" if v and w else "")


def write_motion_metrics(rd, strips, geo_samples=None):
    """MOTION-METRICS.md: per transition, measured from the recording and the item tree."""
    seen = collections.OrderedDict()
    for s in strips:
        if s.get("metrics"):
            seen.setdefault((s["name"], s["theme"]), s)
    L = ["# Motion metrics\n",
         "Measured per recorded transition (every rendered frame, read back with its timestamp; t = 0 is the action). "
         "**Latency**: action to the first change. **Transitions**: first to last change of each run of changes; "
         "runs more than 100 ms apart are separate transitions (a hover fade in, then out; a pointer move, then an "
         "auto-hide 5 s later), and the time between them is a hold or a wait, part of no transition. With content "
         "that keeps moving (playback), only frames that change clearly more than that motion count. **Recording**: "
         "the whole recording. **Blink**, per scope: the longest dip (at most 250 ms) of the scope's ink (mean colour "
         "distance from its background) to 50 % or less of its level before and after the transition; scope *text* = "
         "the rects of the text items shown (item tree), *other* = the rest of the change box (cover art, icons, "
         "plots); — = the scope does not change. **Blink per element** (states.ELEMENT_INK; the now-playing title, artist "
         "line and details line): the same measure in each element's own rect, at full resolution, so a local dim is "
         "not averaged away. **Blink per changed item**: an item (a popup, a list) whose traced opacity dips to 50 % or "
         "less, or by over 25 % for over 30 ms, and returns in its place, where the frames show the dip in its own "
         "rect; of items dipping together the largest. **Whole window under the darker end**: the lowest of the "
         "window's mean luminance (of the sRGB values as the compositor blends them, 45 ms blend) as a share of the "
         "darker of a transition's two ends; under 97 % is a DIP. **Under the one-layer mix** (cross-fades, "
         "states.MOTION_FOCUS cross_fade): each frame fitted as a mix of the two ends at one opacity, and the most "
         "its luminance falls under that mix's; over 3 % FAILS (layers that fade apart). "
         "**Overlap**: during a transition, two text items in the same place "
         "both above 30 % opacity, one mid-fade (item tree). **After end**: movement after the stated end (the QML "
         "animations that started in the transition: start + duration). Each transition's merged image "
         "(`strips/<strip>.png`) has the key frames, the graph with the animations that ran, and the "
         "perceived-at-real-speed rows (a simulation: 45 ms box average).\n",
         latency_baseline(strips) + "\n",
         "**After end** is split: the app's own items (and changed pixels no item explains) apart from tooltips "
         "and other popups (items on a popup layer), so a tooltip fading in after a transition is not read as the "
         "transition running long. **Neither** (plot recordings only): frames that are neither the start nor the end "
         "nor a blend of them.\n",
         "| transition | theme | latency | transitions (start: duration) | recording | blink: text / other | blink per changed item | blink per element (own rect) | whole window under the darker end | under the one-layer mix | text overlap | after stated end: the app's items | after stated end: tooltip/popup | neither | animations that ran | image |",
         "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for (name, theme), s in seen.items():
        m = s["metrics"]
        anims = "; ".join(f"{a['target']}.{a['prop']} {a['frm']}→{a['to']} {a['duration']} ms {a['easing']} "
                          f"@{a['start']}–{a['end'] if a['end'] is not None else '?'} ms" for a in m["animations"][:4] if a["prop"])
        if len(m["animations"]) > 4:
            anims += f"; … {len(m['animations']) - 4} more"
        segs = ", ".join(f"+{g['start_ms']}: {g['duration_ms']} ms" for g in m["segments"]) or "none"
        bl = " / ".join("—" if v is None else f"{v} ms" for v in (m["blink_text_ms"], m["blink_other_ms"]))
        el = "; ".join(f"{e['label']} {e['blink_ms']} ms" for e in m.get("elements", [])) or "—"
        items = m.get("blink_items")
        ib = "—" if items is None else "; ".join(
            f"{b['label'].replace('|', '/')} {b['ms']} ms (to {100 * b['low']:.0f} %) at +{b['start_ms']}"
            for b in items) or "0 ms"
        dip, mix = m.get("window_dip"), m.get("mix_shortfall")
        wd = "—" if not dip else f"{100 * dip['low']:.0f} %" + (f" DIP at +{dip['at_ms']}" if dip["flag"] else "")
        ms_ = "—" if not mix else f"{100 * mix['short']:.0f} %" + (f" FAIL at +{mix['at_ms']}" if mix["flag"] else "")
        L.append(f"| {name} | {theme} | {m['latency_ms']} ms | {segs}{' (content keeps moving)' if m['continuous'] else ''} | "
                 f"{m['recording_ms']} ms | {bl} | {ib} | {el} | {wd} | {ms_} | {m['overlap_ms']} ms | {after_cell(m, 'app')} | "
                 f"{after_cell(m, 'overlay')} | {'—' if m.get('neither_ms') is None else str(m['neither_ms']) + ' ms'} | "
                 f"{anims or '—'} | `strips/{s['file']}` |")
    for (name, theme), (times, rows, note, *_pf) in sorted((geo_samples or {}).items()):
        L += ["", f"## {name} ({theme}): geometry every {times[1] - times[0]} ms\n",
              f"Each item's rect (window px: its top y and height) and opacity from the trace, sampled at evenly spaced "
              f"times ({note}); *library first row* is the topmost shown row title.\n"] + geometry_table(times, rows)
    with open(os.path.join(rd, "MOTION-METRICS.md"), "w") as f_mm:
        f_mm.write("\n".join(L) + "\n")


def toc(entries, sheets, strips, core):
    """INDEX.md's contents by area: per area, its core images (IDs), its sheets and its number of shots."""
    ids, i_s, i_m = {}, 0, 0
    for kind, x in core:
        if kind == "sheet":
            i_s += 1
            ids.setdefault(x["file"], f"S{i_s:02d}")
        else:
            i_m += 1
            ids.setdefault(x["file"], f"M{i_m:02d}")
    shots = Counter(e["area"] for e in entries)
    core_by_area = collections.defaultdict(list)
    for kind, x in core:
        if kind != "sheet":
            continue
        for t in x.get("tiles", []):
            if ids[x["file"]] not in core_by_area[t["area"]]:
                core_by_area[t["area"]].append(ids[x["file"]])
    sheets_by_area = collections.defaultdict(list)
    for x in sheets:
        if x.get("core"):
            continue
        for t in x.get("tiles", []):
            if x["file"] not in sheets_by_area[t["area"]]:
                sheets_by_area[t["area"]].append(x["file"])
    areas = sorted(shots, key=lambda a: (GROUP_ORDER.index(a) if a in GROUP_ORDER else 99, a))
    L = ["## Contents by area\n",
         "Per area: the core images showing it (their IDs below), its sheets in the reference, its full-resolution "
         "shots (`shots/<area>_<state>_<variant>.png`). Transitions are listed under the core set and in \"All "
         "transitions\".\n",
         "| area | core images | sheets | shots |", "|---|---|---|---|"]
    for a in areas:
        if a in SKIP_AREAS:
            continue
        sh = sheets_by_area.get(a, [])
        L.append(f"| {a} | {', '.join(core_by_area.get(a, [])) or '—'} | "
                 + (", ".join(f"`sheets/{f}`" for f in sh[:4]) + (f" (+{len(sh) - 4})" if len(sh) > 4 else "") if sh else "—")
                 + f" | {shots[a]} |")
    mids = [ids[x["file"]] + " " + x["name"] for kind, x in core if kind != "sheet"]
    L.append(f"| motion (transitions) | {', '.join(m.split()[0] for m in mids)} | — | {len(strips)} images |")
    L.append("")
    return L


def write_focus_a11y(rd):
    """FOCUS-A11Y.md, the focus walk's accessibility transcript: per focus stop the role, name, value,
    description and states its accessibility interface gives (a11y_<variant>.jsonl, written by the Tab macro).
    Returns (stops, stops without a name)."""
    files = sorted(glob.glob(os.path.join(rd, "a11y_*.jsonl")))
    if not files:
        return 0, 0
    L = ["# Focus walk: accessibility transcript\n",
         "What the accessibility interface (Qt's `QAccessible`, what a screen reader is told) gives for the item with "
         "keyboard focus at each stop of the focus walk: Tab from the window's start, 24 presses, in the main window "
         "with a track loaded, Last.fm on and one issue reported. **Role** is Qt's role name; **value** is a slider's "
         "or field's value; *on* names the ancestor that carries the role when the focused item is an inner part. "
         "A stop without an accessible name is marked.\n"]
    stops = unnamed = 0
    home = os.path.expanduser("~")  # (a library folder's path is in its row's name: shown from ~)
    esc = lambda t: str(t or "").replace(home, "~").replace("|", "\\|").replace("\n", " ")
    for p in files:
        var = os.path.basename(p)[5:-6]
        rows = [json.loads(l) for l in open(p, encoding="utf-8") if l.strip()]
        L += [f"## {var}\n", "| stop | focused item | role | name | value | states | description |", "|---|---|---|---|---|---|---|"]
        seen = []
        for r_ in rows:
            key = (r_.get("item"), r_.get("role"), r_.get("name"))
            wrap_ = " (wraps: seen before)" if key in seen and r_["stop"] != "Launch" else ""
            seen.append(key)
            stops += 1
            nm = esc(r_.get("name"))
            if not nm:
                unnamed += 1
                nm = "**no accessible name**"
            item = esc(r_.get("item")) + (f" (role on `{esc(r_['on'])}`)" if r_.get("on") else "")
            L.append(f"| {r_['stop']}{wrap_} | `{item}` | {esc(r_.get('role')) or '**none**'} | {nm} | {esc(r_.get('value'))} | "
                     f"{esc(r_.get('states'))} | {esc(r_.get('description'))} |")
        L.append("")
    # dialogs' focus stops (a11y-dialogs_*.jsonl, macro dialogwalk) and checked-state announcements
    # (a11y-events_*.jsonl, macro checkann)
    dfiles = sorted(glob.glob(os.path.join(rd, "a11y-dialogs_*.jsonl")))
    if dfiles:
        L += ["# Dialogs: focus stops\n",
              "Each dialog window as opened, then after each Tab press (keys sent to that window): what the "
              "accessibility interface gives for the dialog's focus item. A wrap back to a stop seen before is marked.\n"]
    for p in dfiles:
        var = os.path.basename(p)[len("a11y-dialogs_"):-6]
        rows = [json.loads(l) for l in open(p, encoding="utf-8") if l.strip()]
        for dlg in dict.fromkeys(r_["dialog"] for r_ in rows):
            rs = [r_ for r_ in rows if r_["dialog"] == dlg]
            L += [f"## {dlg} — {var}\n", f"Window: {esc(rs[0].get('window_title') or rs[0].get('window'))}.\n",
                  "| stop | focused item | role | name | value | states | description |", "|---|---|---|---|---|---|---|"]
            seen = {}
            for r_ in rs:
                key = (r_.get("item"), r_.get("role"), r_.get("name"))
                wrap_ = f" (wraps: {seen[key]})" if key in seen else ""
                seen.setdefault(key, r_["stop"])
                stops += 1
                nm = esc(r_.get("name"))
                if not nm:
                    unnamed += 1
                    nm = "**no accessible name**"
                item = esc(r_.get("item")) + (f" (role on `{esc(r_['on'])}`)" if r_.get("on") else "")
                L.append(f"| {r_['stop']}{wrap_} | `{item or 'none'}` | {esc(r_.get('role')) or '**none**'} | {nm} | "
                         f"{esc(r_.get('value'))} | {esc(r_.get('states'))} | {esc(r_.get('description'))} |")
            L.append("")
    efiles = sorted(glob.glob(os.path.join(rd, "a11y-events_*.jsonl")))
    if efiles:
        L += ["# Checked-state announcements\n",
              "A toggle given keyboard focus and pressed with Space: its states before and after (the accessibility "
              "interface), and the accessibility events the app sent meanwhile (what a screen reader is notified "
              "of: `StateChanged` with the states that changed, `Focus`, `NameChanged`, `ValueChanged`; recorded by "
              "an update handler, Qt's public QAccessible API). A toggle whose checked state changes with no "
              "`StateChanged` event naming *checked* is marked.\n"]
    for p in efiles:
        var = os.path.basename(p)[len("a11y-events_"):-6]
        L += [f"## {var}\n", "| toggle | press | item | before | after | events sent |", "|---|---|---|---|---|---|"]
        for r_ in (json.loads(l) for l in open(p, encoding="utf-8") if l.strip()):
            b_, a_ = r_.get("before", {}), r_.get("after", {})
            evs = [e for e in r_.get("events", []) if e.get("event") not in ("LocationChanged",)]
            ev_t = "; ".join(f"+{e.get('t', 0):.0f} ms {e.get('event')} `{esc(e.get('item'))}`"
                             + (f" ({esc(e.get('role'))} \u201c{esc(e.get('name'))}\u201d)" if e.get("role") else "")
                             + (f" changed: {e['changed']}" if e.get("changed") else "")
                             + (f" → {esc(e.get('states'))}" if e.get("event") == "StateChanged" else "") for e in evs[:8])
            if len(evs) > 8:
                ev_t += f"; … {len(evs) - 8} more"
            is_checked = lambda x: "checked" in str(x.get("states") or "").split(", ")
            flip = is_checked(b_) != is_checked(a_)
            told = any(e.get("event") == "StateChanged" and "checked" in str(e.get("changed")) for e in evs)
            mark = " **(checked changed, no StateChanged event for it)**" if flip and not told else ""
            L.append(f"| {esc(r_['label'])} | {r_['press']} | `{esc(r_.get('object'))}` ({esc(b_.get('role'))} "
                     f"\u201c{esc(b_.get('name'))}\u201d) | {esc(b_.get('states'))} | {esc(a_.get('states'))} | "
                     f"{ev_t or '**none**'}{mark} |")
        L.append("")
    open(os.path.join(rd, "FOCUS-A11Y.md"), "w", encoding="utf-8").write("\n".join(L) + "\n")
    return stops, unnamed


def _key_colours(rd, e, n=3):
    """A shot's (its subject's, when recorded) most frequent exact colours, as '#rrggbb NN %'."""
    import numpy as np
    p = os.path.join(rd, "shots", e["file"])
    if not os.path.exists(p):
        return ""
    im = Image.open(p).convert("RGB")
    if e.get("subject"):
        x, y, w, h = [int(v * e.get("scale", 1)) for v in e["subject"]]
        im = im.crop((x, y, x + w, y + h))
    a = np.asarray(im).reshape(-1, 3)
    v, c = unique_colours(a)
    o = c.argsort()[::-1][:n]
    return ", ".join("#%02x%02x%02x %d %%" % (*v[i], round(100 * c[i] / len(a))) for i in o)


def _contrast_rows(rd):
    p = os.path.join(rd, "CONTRAST.md")
    rows = []
    if os.path.exists(p):
        with open(p, encoding="utf-8") as fh:
            lines = fh.readlines()
        for ln in lines:
            f = [x.strip() for x in ln.strip().strip("|").split("|")]
            if ln.startswith("| ") and len(f) >= 6 and f[1] in ("dark", "light"):
                rows.append(dict(element=f[0], theme=f[1], fg=f[2], bg=f[3], ratio=f[4].strip("*"), verdict=f[5]))
    return rows


def _layout_by_shot(rd):
    p = os.path.join(rd, "layout.json")
    by = collections.defaultdict(Counter)
    if os.path.exists(p):
        for fd in json.load(open(p)).get("findings", []):
            for oc in fd.get("occurrences", []):
                by[os.path.basename(oc.get("shot", ""))][fd["kind"]] += 1
    return by


def by_variant(f):
    """A fact's place among a document's: not the order its session finished in, which the workers decide."""
    return f.get("key", ""), f.get("theme", ""), f.get("suffix", "")


def _jsonl(p):
    if not os.path.exists(p):
        return []
    with open(p, encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]


def strip_ends(rd, nm, th, watch):
    """what a recording starts and ends on, measured: for each watched item (label, objectName) its opacity in the
    recording's item trace (opacity.tsv) at the first and the last frame (the largest over its instances); shown =
    at least 0.5. Empty without `watch`; loud (the text says so) when the recording or an item is not traced."""
    if not watch:
        return ""
    if isinstance(watch, dict):  # dict(watch=[...], sizes=[(label, objectName)])
        sizes, watch = watch.get("sizes") or [], watch.get("watch") or []
    else:
        sizes = []
    ds = sorted(glob.glob(os.path.join(rd, "strips-raw", f"{nm}_{th}_*")))
    if not ds or not os.path.exists(os.path.join(ds[0], "opacity.tsv")):
        return ": (no item trace recorded)"
    vals = {}
    for line in open(os.path.join(ds[0], "opacity.tsv"), encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) < 4 or f[0].startswith("#"):
            continue
        leaf = f[0].split("/")[-1].split("#")[0]
        pts = [float(tok.split("@")[0].split("~")[0].split("^")[0].split(":")[1]) for tok in f[3].split()]
        if pts:
            a_, b_ = vals.get(leaf, (0.0, 0.0))
            vals[leaf] = (max(a_, pts[0]), max(b_, pts[-1]))
    parts, start, end = [], [], []
    for lab, obj in watch:
        if obj not in vals:
            parts.append(f"{lab} not traced")
            continue
        a_, b_ = vals[obj]
        parts.append(f"{lab} {a_:.2f} -> {b_:.2f}")
        (start.append(lab) if a_ >= 0.5 else None)
        (end.append(lab) if b_ >= 0.5 else None)
    out = (f": the frames start on {', '.join(start) or 'none of the watched items'} and end on "
           f"{', '.join(end) or 'none of the watched items'} (opacity in the item trace, first -> last frame: "
           + "; ".join(parts) + ")") if watch else ""
    if sizes:  # the items' own sizes at the first and the last frame (e.g. the title's height across a change)
        rects = {}
        for line in open(os.path.join(ds[0], "opacity.tsv"), encoding="utf-8"):
            f = line.rstrip("\n").split("\t")
            if len(f) < 4 or f[0].startswith("#"):
                continue
            leaf = f[0].split("/")[-1].split("#")[0]
            if leaf not in {o_ for _, o_ in sizes} or "#" in f[0].split("/")[-1]:
                continue
            first = [int(v) for v in f[2].split(",")]
            last = first
            for tok in f[3].split():
                tok = tok.split("^")[0]
                full = tok.partition("~")[2]
                at_ = tok.partition("~")[0].partition("@")[2]
                last = [int(v) for v in (full or at_).split(",")] if (full or at_) else last
            rects[leaf] = (first, last)
        out += "; sizes, first -> last frame: " + "; ".join(
            f"{lab} {rects[o_][0][2]}x{rects[o_][0][3]} -> {rects[o_][1][2]}x{rects[o_][1][3]} px" if o_ in rects
            else f"{lab} not traced" for lab, o_ in sizes)
    return out


class LazyFrames(FrameStore):
    """a recording's frames (strip_frames) read when indexed, instead of every frame in memory at once (a
    full-window recording at 1600x1000 is ~5 MB a frame; with strip jobs in parallel that ran the host out of memory
    beside a build): each decoded once and kept as shared tiles within a budget (framestore.py)."""
    def __init__(self, d, frames):
        super().__init__(d, [f["file"] for f in frames])


def strip_facts(m):
    """One line of a recording's measured facts (its metrics)."""
    if not m:
        return "no metrics"
    segs = m.get("segments") or []
    # the visual handover first (the animations that ran, the items' opacities settling), apart from the pixel
    # transition, which counts every changed pixel: playback content and tooltips too
    h_ = m.get("handover") or {}
    hv = []
    if h_.get("anim_span"):
        hv.append(f"animations ran +{h_['anim_span'][0]} to +{h_['anim_span'][1]} ms ({h_['anims_n']}; the longest "
                  f"{h_['anim_longest']})")
    if h_.get("settle_ms") is not None:
        hv.append(f"the items' opacities settle at +{h_['settle_ms']} ms (last: {h_['settle_item']})")
    if not h_.get("anim_span") and h_.get("movers"):  # what ran, when no animation is named
        hv.append("no QML animation the trace names; by the item trace " + "; ".join(
            f"{n} moved {dx:+d},{dy:+d} px" + (f", width {dw:+d}" if dw else "") + (f" from +{a} to +{b} ms" if a is not None else "")
            for n, dx, dy, dw, a, b in h_["movers"]))
    if m.get("handover_by_transition"):  # (several transitions: each its own handover)
        mv_ = lambda g: "; ".join(f"{n} moved {dx:+d},{dy:+d} px" + (f", width {dw:+d}" if dw else "")
                                  + (f" from +{a} to +{b} ms" if a is not None else "") for n, dx, dy, dw, a, b in g.get("movers") or [])
        hv = [f"+{g['start_ms']}: " + (f"animations +{g['anim_span'][0]} to +{g['anim_span'][1]} ms" if g.get("anim_span")
                                       else (f"no QML animation the trace names; by the item trace {mv_(g)}" if g.get("movers")
                                             else "no animation the trace names"))
              + (f", opacities settle at +{g['settle_ms']} ms" if g.get("settle_ms") is not None else "")
              for g in m["handover_by_transition"]]
        hv = ["per transition " + "; ".join(hv) + (f" (the longest animation {h_['anim_longest']})" if h_.get("anim_longest") else "")]
    first = []
    if m.get("crosshair"):  # (first: the fact a pan is read for)
        import motion_strip
        first = [motion_strip.crosshair_text(m["crosshair"])]
    if m.get("crosshair_presence"):  # (first too: the fact a zoom is read for)
        import motion_strip
        first.append(motion_strip.presence_text(m["crosshair_presence"]))
    parts = first + [f"latency {m['latency_ms']} ms"] + ([f"visual handover: {', '.join(hv)}"] if hv else []) + [
        f"pixel transition {m['duration_ms']} ms ({m['frames_60']} frames at 60 fps; every changed pixel, playback content "
        "and tooltips included)"]
    if len(segs) > 1:
        parts.append(f"{len(segs)} transitions: " + ", ".join(f"+{g['start_ms']} for {g['duration_ms']} ms" for g in segs[:4]))
    # a content cut inside the now-playing panel (opacity stays, content swaps in one frame)
    if m.get("content_cuts") is not None:
        cc = m["content_cuts"]
        cut_ = [g for g in cc if g.get("cut")]
        if cut_:
            parts.append("CONTENT CUT in the now-playing panel: " + "; ".join(
                f"at +{g['at_ms']} ms {100 * g['max_jump']:.0f} % of its changing pixels jumped over 60 % of their whole change "
                f"in one frame; it held still {g['held_ms']} ms; frames that are neither the start nor the end (over a quarter "
                f"of the panel's changing 32 px tiles; {100 * (g['neither'] or 0):.0f} % right after the cut): "
                f"{g.get('neither_ms', 0)} ms" for g in cut_))
        elif cc:
            parts.append("no content cut in the now-playing panel (the largest one-frame jump: "
                         + ", ".join(f"{100 * g['max_jump']:.0f} %" for g in cc) + " of its changing pixels)")
        else:
            parts.append("the now-playing panel does not change (no transition in it)")
    bl = [f"text {m['blink_text_ms']} ms" if m.get("blink_text_ms") is not None else "text —",
          f"other {m['blink_other_ms']} ms" if m.get("blink_other_ms") is not None else "other —"]
    if m.get("blink_plot_ms") is not None:
        bl.append(f"plot content {m['blink_plot_ms']} ms")
    if m.get("blink_items") is not None:
        bl.append(f"changed items {max([b['ms'] for b in m['blink_items']], default=0)} ms")
    parts.append("blink " + " / ".join(bl))
    if m.get("blink_items"):
        import motion_strip
        parts.append("ITEM BLINK: " + "; ".join(motion_strip.item_blink_text(b) for b in m["blink_items"]))
    if m.get("window_dip") or m.get("mix_shortfall"):
        import motion_strip
        parts.extend(motion_strip.fade_texts(m))
    el = [f"{e['label']} {e['blink_ms']} ms" for e in m.get("elements", []) if e.get("blink_ms")]
    if el:
        parts.append("element blinks: " + "; ".join(el))
    # the overlap as an amount: the peak opacity two texts in one place show at once (no threshold)
    def pk_(v):  # (the pair's names, and how long it stays at or above 0.1)
        if isinstance(v, dict):
            if v.get("t") is None:
                return "0.00"
            return (f"{v['peak']:.2f} at +{v['t']} ms ({v['names'][0]} {v['ops'][0]:.2f}, {v['names'][1]} {v['ops'][1]:.2f}), "
                    f"at or above 0.10 for {v['ms']} ms")
        return f"{v[0]:.2f} at +{v[1]} ms ({v[2][0]:.2f} and {v[2][1]:.2f})" if v and v[1] is not None else "0.00"
    if m.get("overlap_peak") is not None:
        ov = (f"text overlap, the peak opacity of two texts drawn in one place at once (visible areas; a line and its own "
              f"tail are one line): {pk_(m['overlap_peak'])}")
        if m.get("overlap_peak_by_line"):
            ov += "; per line: " + ", ".join(f"{k} not in the item trace" if k in (m.get("lines_absent") or []) else f"{k} {pk_(v)}"
                                             for k, v in m["overlap_peak_by_line"].items())
    else:
        ov = f"text overlap {m['overlap_ms']} ms"
    parts.append(ov)
    if m.get("plot_latency_ms") is not None:
        parts.append(f"action to the plot's first change {m['plot_latency_ms']} ms")
    if m.get("neither_ms") is not None:
        parts.append(f"neither start nor end {m['neither_ms']} ms ({len(m.get('neither_frames') or [])} frames)")
    if m.get("travel_px") is not None:
        parts.append(f"travel {m['travel_px']} px")
    a_, o_ = m.get("after_end_app_ms"), m.get("after_end_overlay_ms")
    if a_ is not None or o_ is not None:
        parts.append(f"after the stated end: the app's items {a_ or 0} ms" + (f" ({m['after_end_app_what']})" if a_ else "")
                     + f", tooltip/popup {o_ or 0} ms" + (f" ({m['after_end_overlay_what']})" if o_ else ""))
    elif m.get("moves_after_ms") is not None:
        parts.append(f"after the stated end {m['moves_after_ms']} ms")
    if segs:
        parts.append(f"what changed first: {segs[0]['what'][:160]}")
    return "; ".join(parts) + "."


FLOOR_ORDER = ["unfilled", "pre-start", "silence", "post-end", "ends-early-centered", "ends-early-lanes", "ends-early-rolling"]


def write_core_facts(rd, core, strips, geo_samples, entries=(), out="CORE-FACTS.md", ids=None, looks=None,
                     fact_keys=None, fact_skip=(), title=None, intro=None, floor_labels=None, a11y=None, extra=None, contrast_map=None,
                     lead=None):
    """CORE-FACTS.md, the core images' measured facts as text (a reviewer keeps notes from text when the images
    leave its context): per sheet its tiles' states, sizes, key colours, layout findings and contrast rows; per
    recording its metrics; then the plot's empty regions and the mode switch's plates, sampled."""
    by_file = {e["file"]: e for e in entries}
    crow = _contrast_rows(rd)
    lay = _layout_by_shot(rd)
    # (a partial run appends again: the last line per fact and variant wins)
    facts = list({(f_["key"], f_["suffix"]): f_ for f_ in _jsonl(os.path.join(rd, "facts.jsonl"))}.values())
    floors = list({(f_["label"], f_["suffix"]): f_ for f_ in _jsonl(os.path.join(rd, "floors.jsonl"))}.values())
    by_name = {(x["name"], x["theme"]): x for x in strips}
    pairs = {f"pair:{nm}": (a_, b_) for nm, a_, b_, *_ in getattr(st, "STRIP_PAIRS", [])}
    # (the focus pack's FOCUS-FACTS.md: its own IDs, what to look at, only its facts and floor regions)
    if fact_keys is not None:
        facts = [f_ for f_ in facts if any(f_["key"].startswith(k) for k in fact_keys)
                 and not any(f_["key"].startswith(k) for k in fact_skip)]
    if floor_labels is not None:
        floors = [f_ for f_ in floors if f_["label"] in floor_labels]
    L = [title or "# Core facts: what was measured in each core image\n",
         intro or "Text to keep when the images are gone: per core image (IDs as in INDEX.md) its size, and for sheets each "
         "tile's state, its key colours (the most frequent exact colours of the shot's subject), the layout checker's "
         "findings in that shot and the contrast rows of CONTRAST.md about it; for recordings the motion metrics "
         "(MOTION-METRICS.md). Colours are sampled from the rendered pixels.\n"]
    if lead:  # a table first: e.g. the installed icon per size
        L += lead + [""]
    i_s = i_m = 0
    for n_, (kind, x) in enumerate(core):
        id_ = ids[n_] if ids else None
        look = (looks or [None] * len(core))[n_]
        if kind == "sheet":
            i_s += 1
            L.append(f"## {id_ or f'S{i_s:02d}'} — {x['title']}\n")
            if look:
                L.append(f"*Look at:* {look}\n")
            L.append(f"`sheets/{x['file']}`, {x['size'][0]}×{x['size'][1]} ({x['mp'] / 1e6:.2f} MP"
                     + _scale_text(x) + ").\n")
            seen = set()
            for t in x.get("tiles", []):
                if t["file"] in seen:
                    continue
                seen.add(t["file"])
                lf = lay.get(t["file"])
                lf_t = ("; layout: " + ", ".join(f"{k} {v}" for k, v in lf.most_common())) if lf else "; layout: none"
                kc = _key_colours(rd, by_file.get(t["file"], t)) if t.get("area") not in ("motion",) else ""
                L.append(f"- `{t['area']}/{t['state']}` ({t['theme']}, {t.get('size', '')}): {t.get('desc', '')[:420]}"
                         + (f"; key colours {kc}" if kc else "") + lf_t)
            pats = (contrast_map if contrast_map is not None else getattr(st, "CORE_FACT_CONTRAST", {})).get(x.get("group", ""), [])
            rows = [r_ for r_ in crow if any(p_ in r_["element"].lower() for p_ in pats)
                    and (contrast_map is not None or x.get("theme") in ("both", r_["theme"]))]
            if rows:
                L.append("- Contrast (CONTRAST.md): " + "; ".join(
                    f"{r_['element']} ({r_['theme']}) {r_['fg']} on {r_['bg']} {r_['ratio']}:1 {r_['verdict']}" for r_ in rows[:14])
                    + (f"; … {len(rows) - 14} more rows" if len(rows) > 14 else ""))
            if x.get("group") == "closeups" and floors:
                L.append("- The plot's empty regions, sampled: see *Plot empty regions* below.")
            if x.get("recording"):  # (a focus sheet of one recording's frames: that recording's metrics)
                nm_ = x["recording"]
                L.append(f"- The recording ({nm_}): " + strip_facts((by_name.get((nm_, x["theme"])) or {}).get("metrics")))
            L.append("")
        else:
            i_m += 1
            L.append(f"## {id_ or f'M{i_m:02d}'} — {x['name']} ({x['theme']})\n")
            if look:
                L.append(f"*Look at:* {look}\n")
            sz = x.get("size") or []
            L.append(f"`strips/{x['file']}`" + (f", {sz[0]}×{sz[1]}" if sz else "") + f". {x.get('desc', '')}\n")
            if x["name"] in pairs:
                for side, half in zip(("Left", "Right"), pairs[x["name"]]):
                    hn, _, ht = half.partition("@")
                    h_ = by_name.get((hn, ht or x["theme"]))
                    L.append(f"- {side}, {hn} ({ht or x['theme']}): " + strip_facts((h_ or {}).get("metrics")))
            elif x.get("metrics"):
                L.append("- " + strip_facts(x["metrics"]))
            elif x.get("caption"):
                L.append("- " + x["caption"][:900])
            base = x["name"][:-5] if x["name"].endswith("-1to1") else None
            if base and by_name.get((base, x["theme"])):
                L.append(f"- The recording ({base}): " + strip_facts(by_name[(base, x["theme"])].get("metrics")))
            src_rec = "first-play-from-idle" if x["name"] == "first-play-metadata" else x["name"]
            gs = (geo_samples or {}).get((src_rec, x["theme"]))
            if not gs and x["name"] in pairs:  # (a pair's half with geometry samples, e.g. First Play dark | light)
                for half in pairs[x["name"]]:
                    hn, _, ht = half.partition("@")
                    gs = (geo_samples or {}).get((hn, ht or x["theme"]))
                    if gs:
                        break
            if gs:
                times, rows, note, *pf = gs
                L += ["", f"Geometry every {times[1] - times[0]} ms ({note}; window px: top y, height · opacity):\n"]
                L += geometry_table(times, rows)
                for ln in (pf[0] if pf else []):
                    L += ["", f"- {ln}"]
            L.append("")
    if floors:
        L += ["## Plot empty regions (sampled)\n",
              "The spectrogram pane's empty regions, sampled from window grabs (`floorprobe` in states.py; the panes "
              "as sampled are the `plotfloor/*` shots): the tone-silence-tone fixture (4 s 440 Hz, 4 s digital "
              "silence, 4 s 660 Hz) paused, one lane, short-track stretch off; the never-played pane; a file cut at 55 % "
              "of its header's 60 s, 1.5 s before the cut. Colour = the "
              "most frequent exact colour of the region's middle rows; *plotFloor* is the palette token.\n",
              "| region | what | theme | colour (share of its pixels) | px of the pane | plotFloor | relation |", "|---|---|---|---|---|---|---|"]
        for f_ in sorted(floors, key=lambda f_: (FLOOR_ORDER.index(f_["label"])
                                                  if f_["label"] in FLOOR_ORDER else 9,
                                                  f_["theme"], f_["label"], f_["suffix"])):
            if f_.get("colour"):
                rel = "the same" if f_.get("equals_floor") else (f"{f_.get('vs_floor', 0):.2f}:1, max channel diff {f_.get('max_channel_diff')}")
                L.append(f"| {f_['label']} | {f_['what']} | {f_['theme']} | {f_['colour']} ({f_['share'] * 100:.0f} %) | "
                         f"{f_['x0']}–{f_['x1']} of {f_['pane_w']} | {f_['floor_token']} | {rel} |")
            else:
                why = f_["region"] if str(f_.get("region", "")).startswith("marker") else f"flat runs {f_.get('runs')}"
                L.append(f"| {f_['label']} | {f_['what']} | {f_['theme']} | **not in view** ({why}) | — | "
                         f"{f_['floor_token']} | — |")
        L.append("")
    if facts:  # (the mode switch's plates: also in S05's tile descriptions)
        L += ["## Other measured facts\n"] + [f"- {f_['text']}" for f_ in sorted(facts, key=by_variant)] + [""]
    if extra:
        L += ["## Also measured (recordings not shown)\n"] + [f"- {t}" for t in extra] + [""]
    if a11y:
        L += ["## Accessibility lines\n"] + [f"- {t}" for t in a11y] + [""]
    with open(os.path.join(rd, out), "w", encoding="utf-8") as doc:
        doc.write("\n".join(L) + "\n")


def neither_sheet(rd, sdir, name, theme, m, max_frames=12):
    """the raw frames of a plot recording that are neither its start nor its end nor a blend of them (motion
    metrics `neither_frames`), the plot's rect from each, beside the start and end frames, labelled with t, the
    best blend and its residual. None when there are none."""
    nf = (m or {}).get("neither_frames") or []
    if not nf:
        return None
    ds = sorted(glob.glob(os.path.join(rd, "strips-raw", f"{name}_{theme}_*")))
    if not ds:
        return None
    frames, marks, meta = strip_frames(ds[0])
    t0 = action_time(marks, frames[0]["t"])
    before = [f for f in frames if f["t"] - t0 < 0]
    ref = [("start", before[-1] if before else frames[0], None), ("end", frames[-1], None)]
    if len(nf) > max_frames:  # evenly
        nf = [nf[round(i * (len(nf) - 1) / (max_frames - 1))] for i in range(max_frames)]
    x, y, w, h = m["plot_rect"]
    cols = 3
    tw = min(w, (1500 - 2 * PAD - 12 * (cols - 1)) // cols)
    f = tw / w
    th = int(h * f)
    cells = [(lb, os.path.join(ds[0], fr["file"]), f"t {fr['t'] - t0:+.0f} ms") for lb, fr, _ in ref] + \
            [("neither", os.path.join(ds[0], q["file"]), f"t {q['t']:+.0f} ms, blend {q['blend']}, residual {q['residual']}")
             for q in nf]
    rows = [cells[:2]] + [cells[2 + i:2 + i + cols] for i in range(0, len(cells) - 2, cols)]
    head = 64
    S = Image.new("RGB", (2 * PAD + cols * tw + 12 * (cols - 1), head + len(rows) * (th + 26) + 10), (34, 36, 40))
    d = ImageDraw.Draw(S)
    d.text((PAD, 8), f"{name} ({theme}): the frames that are neither the start nor the end ({m.get('neither_ms')} ms; "
                     f"{len(m['neither_frames'])} frames), raw, the plot's {w}x{h} px at {f:.2f}x",
           fill=(255, 255, 255), font=font(True, 18))
    d.text((PAD, 36), "Top row: the last frame before the action and the last frame recorded. Blend: the closest mix of "
                      "start and end (0 = start, 1 = end); residual over |end - start| (above 0.18 counts).",
           fill=(255, 220, 120), font=font(False, 13))
    yy = head
    for row in rows:
        xx = PAD
        for lb, fp, cap in row:
            im = Image.open(fp).convert("RGB").crop((x, y, x + w, y + h)).resize((tw, th), Image.LANCZOS)
            d.text((xx, yy), f"{lb}: {cap}", fill=(230, 230, 230), font=font(False, 13))
            S.paste(im, (xx, yy + 20))
            xx += tw + 12
        yy += th + 26
    fname = f"neither_{name}_{theme}.png"
    S.save(os.path.join(sdir, fname), compress_level=3)
    return dict(file=fname, pages=[], name=f"neither:{name}", theme=theme, desc=f"Neither frames of {name}", frames=0,
                shown=0, duration_ms=0, fps=0, crop=None, zoom=1, note="", size=list(S.size), raw="", caption="",
                metrics=None, one_to_one=False)


def _a11y_event_lines(rd, labels):
    out = []
    for p in sorted(glob.glob(os.path.join(rd, "a11y-events_*.jsonl"))):
        var = os.path.basename(p)[len("a11y-events_"):-6]
        for r_ in _jsonl(p):
            if r_.get("label") not in labels:
                continue
            ev = [f"{e.get('event')} {e.get('item')}" + (f" ({e['changed']})" if e.get("changed") else "")
                  for e in r_.get("events", []) if e.get("event") == "StateChanged" and "checked" in str(e.get("changed"))]
            b_, a_ = r_.get("before", {}), r_.get("after", {})
            out.append(f"{r_['label']} {r_['press']} ({var}): `{r_.get('object')}` {b_.get('role')} \u201c{b_.get('name')}\u201d, "
                       f"{b_.get('states')} → {a_.get('states')}; checked events: {'; '.join(ev) or 'none'}")
    return out


def round_docs(rd):
    """The round's documents a reviewer reads (each checked for home-folder paths): its own and every focus pack's."""
    return glob.glob(os.path.join(rd, "*.md")) + glob.glob(os.path.join(rd, "focus-build", "**", "*.md"), recursive=True)


def focus_scope_names(spec=None):
    """The scopes of SHOT_FOCUS (or `spec`): a comma-separated list, "census" for the census scopes (focus.resolve)."""
    import focus
    return focus.resolve(os.environ.get("SHOT_FOCUS", "") if spec is None else spec)


def focus_strip_needs(names):
    """The recordings the scopes' images and facts need (a pair's two halves), for a --focus-only pack."""
    import focus
    pair_map = {f"pair:{nm}": (a_, b_) for nm, a_, b_, *_ in getattr(st, "STRIP_PAIRS", [])}
    view_of = {f"{nm}-{sfx}": nm for nm, views in getattr(st, "EXTRA_VIEWS", {}).items() for sfx, _ in views}
    need = set()
    for sc_f in (focus.SCOPES[n_] for n_ in names):
        for kind, nm, _th, _show, _look, *opt_ in sc_f.get("images", []):
            if opt_ and opt_[0].get("recording"):  # (a sheet of a recording's frames, which carries its facts)
                need.add(opt_[0]["recording"])
            if kind == "strip":
                # (another view of a recording, states.EXTRA_VIEWS, is made from that recording)
                need |= {view_of.get(h.partition("@")[0], h.partition("@")[0]) for h in pair_map.get(nm, (nm,))}
        need |= {nm for nm, *_ in sc_f.get("facts_strips", [])}
    return need


def focus_limits(scope_name, paths):
    """The pack's hard limits, which keep packs from creeping upward round by round: at most FOCUS_MAX_IMAGES
    images (the "neither"-frame contact sheets count) and every image within CORE_MP_HARD, read from the files
    as published. Returns the failures."""
    fails = []
    if len(paths) > FOCUS_MAX_IMAGES:
        fails.append(f"focus {scope_name}: {len(paths)} images, over the cap of {FOCUS_MAX_IMAGES} (the neither-frame "
                     "sheets count): trim the scope in focus.py")
    for p_ in paths:
        with Image.open(p_) as im:
            w_, h_ = im.size
        if w_ * h_ > CORE_MP_HARD:
            fails.append(f"focus {scope_name}: {os.path.basename(p_)} is {w_}x{h_} ({w_ * h_ / 1e6:.2f} MP), over "
                         f"{CORE_MP_HARD / 1e6:.2f} MP")
    return fails


def system_lead(rd, items, ids):
    """The whole-app pack's summary (a scope's `system_facts`), as FOCUS-FACTS lines: CONTRAST.md's verdicts and
    the rows that do not pass, the layout checker's findings in exactly the pack's shots, and the focus walk's
    order with what a screen reader is told at each stop."""
    esc = lambda t: str(t or "").replace("|", "\\|").replace("\n", " ")
    rows = _contrast_rows(rd)
    L = ["## Contrast, summarised (CONTRAST.md: every measured element)\n"]
    if rows:
        for th in ("dark", "light"):
            n = Counter(r_["verdict"] if r_["verdict"].startswith(("AA", "fails", "meets", "below")) else
                        "not required (see CONTRAST.md)" for r_ in rows if r_["theme"] == th)
            L.append(f"- {th}: " + (", ".join(f"{k} {v}" for k, v in n.most_common()) or "not measured"))
        low = [r_ for r_ in rows if r_["verdict"].startswith(("fails", "below", "AA large only"))]
        L += ["", "| element | theme | colours | ratio | verdict |", "|---|---|---|---|---|"]
        L += [f"| {esc(r_['element'])} | {r_['theme']} | {r_['fg']} on {r_['bg']} | {r_['ratio']}:1 | {r_['verdict']} |"
              for r_ in low] or ["| every measured element passes | | | | |"]
    else:
        L.append("- not measured in this round")
    where = collections.defaultdict(set)  # shot -> the images that show it
    for (_kind, x), id_ in zip(items, ids):
        for t in x.get("tiles") or []:
            where[t["file"]].add(id_)
    L += ["", "## Layout, summarised (the layout checker in this pack's shots)\n",
          "The app's own QML only; text contrast here is each visible text against its rendered background.\n",
          "| issue | component | worst in the round | shots | images |", "|---|---|---|---|---|"]
    p_, found = os.path.join(rd, "layout.json"), []
    layout = {}
    if os.path.exists(p_):
        with open(p_, encoding="utf-8") as f_:
            layout = json.load(f_)
    for fd in layout.get("findings", []):
        shots = {os.path.basename(o.get("shot", "")) for o in fd.get("occurrences", [])
                 if o.get("in_shot", True)} & set(where)  # (not a window behind the one the shot is of)
        if shots and fd.get("severity") != 5:
            worst = fd.get("worst")
            found.append((fd.get("severity", 9), -len(shots), f"| {fd['kind']} | {esc(fd.get('component') or fd.get('type'))} | "
                          f"{f'{worst:.2f}' if isinstance(worst, float) else worst} | {len(shots)} | "
                          f"{', '.join(sorted({i for s_ in shots for i in where[s_]}))} |"))
    found.sort()
    L += [f_[2] for f_ in found[:30]] or ["| none | | | | |"]
    if len(found) > 30:
        L.append(f"\n({len(found) - 30} more in the round's LAYOUT.md.)")
    walk = _jsonl(os.path.join(rd, "a11y_dark_1x_1600x1000.jsonl"))
    L += ["", "## Keyboard focus order (Tab from the window's start, dark, 10 pt)\n",
          "What the accessibility interface gives a screen reader at each stop.\n",
          "| stop | focused item | role | name |", "|---|---|---|---|"]
    home = os.path.expanduser("~")
    L += [f"| {r_['stop']} | `{esc(r_.get('item')).replace(home, '~')}` | {esc(r_.get('role')) or '**none**'} | "
          f"{esc(r_.get('name')).replace(home, '~') or '**no accessible name**'} |" for r_ in walk] or ["| not walked | | | |"]
    return L


def write_focus(rd, src, scope_name, entries, sheets, strips, geo_samples):
    """<round>/focus-build/<scope>/: only the scope's images (focus.SCOPES), FOCUS.md (ID, file, what it shows,
    what to look at) and FOCUS-FACTS.md (their measured facts as text); shoot_round.sh publishes it. Returns the
    failures (a missing or oversized image, or more images than the cap)."""
    import focus
    if scope_name not in focus.SCOPES:
        return [f"focus: no scope {scope_name!r} (scopes: {', '.join(focus.SCOPES)})"]
    sc_ = focus.SCOPES[scope_name]
    sd, sdir = os.path.join(rd, "sheets"), os.path.join(rd, "strips")
    fails, own, notes = [], {}, []
    optional = {im_[1] for im_ in sc_["images"] if len(im_) > 5 and im_[5].get("optional")}
    for nm, title, theme, rows in sc_.get("sheets", []):
        try:
            x = custom_sheet(rd, sd, entries, nm, title, theme, rows)
            own[(nm, theme)] = x
        except LookupError as ex:
            if nm in optional:  # an optional image (e.g. icon variants not rendered yet): noted, not failed
                notes.append(f"{nm}: not made ({ex})")
            else:
                fails.append(f"focus sheet {nm}: {ex}")
    by_sheet = {(x["group"], x["theme"]): x for x in sheets if x["page"] == 1}
    by_sheet.update(own)
    by_strip = {(x["name"], x["theme"]): x for x in strips}
    pairs = {f"pair:{nm}": (a_, b_) for nm, a_, b_, *_ in getattr(st, "STRIP_PAIRS", [])}
    items, shows, looks = [], [], []
    for kind, nm, theme, show, look, *opt_ in sc_["images"]:
        # an image that gives way when another (optional) one was made, e.g. to stay within the image count
        if opt_ and opt_[0].get("unless") and by_sheet.get((opt_[0]["unless"], theme)):
            notes.append(f"{nm}: left out, {opt_[0]['unless']} was made")
            continue
        x = by_sheet.get((nm, theme)) if kind == "sheet" else by_strip.get((nm, theme))
        if not x:
            if opt_ and opt_[0].get("optional"):
                continue
            fails.append(f"focus image {kind} {nm} ({theme}) is missing from the round")
            continue
        rec_ = opt_[0].get("recording") if opt_ else None
        if rec_:  # (a sheet of a recording's frames, with that recording's facts under it)
            if (rec_, theme) not in by_strip:
                fails.append(f"focus sheet {nm}: the recording {rec_} ({theme}) was not measured")
            x = dict(x, recording=rec_)
        items.append((kind, x)); shows.append(show); looks.append(look)
        if kind == "strip":  # any neither frames: their contact sheet next
            for half in (pairs.get(nm) or (nm,)):
                hn, _, ht = half.partition("@")
                h_ = by_strip.get((hn, ht or theme))
                cs = neither_sheet(rd, sdir, hn, ht or theme, (h_ or {}).get("metrics")) if h_ else None
                if cs:
                    items.append(("strip", cs))
                    shows.append(f"The raw frames of {hn} ({ht or theme}) that are neither its start nor its end")
                    looks.append("what is drawn in each: a half-drawn plot, an empty lane, a wrong scale")
    items, fw = weigh(rd, items, label="focus")
    fails += fw
    fd = os.path.join(rd, "focus-build", scope_name)
    shutil.rmtree(fd, ignore_errors=True)
    os.makedirs(fd)
    ids, L = [], []
    total = 0
    for k, ((kind, x), show, look) in enumerate(zip(items, shows, looks), 1):
        id_ = f"F{k:02d}"
        ids.append(id_)
        srcp = os.path.join(rd, "sheets" if kind == "sheet" else "strips", x["file"])
        dst = f"{id_}_{os.path.basename(x['file']).replace('-core.png', '.png')}"
        shutil.copyfile(srcp, os.path.join(fd, dst))
        total += x["mp"]
        L.append(f"| **{id_}** | `{dst}` | {show} | {look} | {x['size'][0]}×{x['size'][1]}, {x['mp'] / 1e6:.2f} MP"
                 + _scale_text(x) + " |")
    so = json.load(open(os.path.join(rd, "source.json"))) if os.path.exists(os.path.join(rd, "source.json")) else {}
    head = [f"# Focus: {sc_['title']} — round {os.path.basename(rd.rstrip('/'))}\n",
            # (the subject without its round tag, e.g. "(h24)")
            f"- Source: {so.get('rev', '?')} on {so.get('branch', '?')} — "
            f"{re.sub(r'[ ]*[(][Hh][0-9]{1,2}[)]', '', so.get('subject', ''))}",
            f"- Built in `{os.path.basename(rd.rstrip('/'))}/` (the paths in FOCUS-FACTS.md, e.g. `strips/`, "
            "are in that folder).",
            f"- {len(items)} images, {total / 1e6:.1f} MP in all; every image within {CORE_MP_HARD / 1e6:.2f} MP (a 1:1 image "
            f"as drawn; the others fitted to {CORE_MP / 1e6:.1f} MP). The measured facts for exactly these images: "
            "`FOCUS-FACTS.md`. The whole round (every area, both themes, LAYOUT, CONTRAST, MOTION-METRICS) is the "
            "evidence in the round's folder, not part of this pack.\n",
            *([f"- Not in this pack (optional, not made): {'; '.join(notes)}\n"] if notes else []),
            "| ID | file | what it shows | what to look at | size |", "|---|---|---|---|---|"]
    with open(os.path.join(fd, "FOCUS.md"), "w", encoding="utf-8") as f_md:
        f_md.write("\n".join(head + L) + "\n")
    notes_ = []
    for f_ in sorted(_jsonl(os.path.join(rd, "facts.jsonl")), key=by_variant):  # "Track N" without "of M" in a card's details line
        mt = re.search(r"details line \u201c([^\u201d]*)\u201d", f_.get("text", ""))
        if f_.get("key", "").startswith("card-") and mt and re.search(r"Track \d+(?! of)", mt.group(1)):
            notes_.append(f"{f_['key'][5:]} ({f_['suffix']}): \u201c{mt.group(1)}\u201d")
    extra_notes = ([f"\u201cTrack N\u201d without \u201cof M\u201d in a details line is the designed short form, not a "
                    f"truncation (it appears in: {'; '.join(dict.fromkeys(notes_))})."] if notes_ else [])
    lead = None
    lt = sc_.get("lead_table")
    if lt:
        rows_ = sorted({f_["key"]: f_ for f_ in _jsonl(os.path.join(rd, "facts.jsonl"))
                        if f_["key"].startswith(lt["prefix"])}.values(), key=lambda f_: f_["key"])
        lead = [f"## {lt['title']}\n", lt.get("note", "") + "\n" if lt.get("note") else "",
                "| " + " | ".join(h for h, _ in lt["columns"]) + " |", "|" + "---|" * len(lt["columns"])]
        for f_ in rows_:
            lead.append("| " + " | ".join(str(fn(f_)) for _, fn in lt["columns"]) + " |")
        if not rows_:
            lead.append("| (no rows measured) |")
    if sc_.get("fact_table"):
        # one row per image before the long lines: its size and scale, and for each dialog in it (dark, or its
        # only theme) the window's size, what has focus as it opens, the default button, any overflowing scroll area
        fk = _facts_by_key(rd)
        T_ = ["## At a glance: one row per image\n",
              "Window sizes in logical px; *focus*: the item with keyboard focus as the dialog opened; *default*: the "
              "highlighted button; *overflow*: a scroll area whose content is larger than its view (none: it fits). The "
              "long lines below give every theme and the contrast.\n",
              "| ID | image | dialogs in it (window size; focus; default; overflow) |", "|---|---|---|"]
        for (kind, x), id_ in zip(items, ids):
            seen_, parts = set(), []
            tiles = x.get("tiles") or []
            themes_of = {}
            for t in tiles:
                themes_of.setdefault((t.get("area"), t.get("state")), set()).add(t.get("theme"))
            for t in tiles:
                key_ = (t.get("area"), t.get("state"))
                if key_ in seen_ or t.get("area") not in ("dialog", "search"):
                    continue
                if t.get("theme") != "dark" and "dark" in themes_of[key_]:
                    continue
                seen_.add(key_)
                sfx = t["file"][len(t["area"]) + len(t["state"]) + 2:-4]
                f_ = fk.get(f"dlg-{t['state']}-{sfx}")
                if not f_:
                    continue
                w_ = f_.get("win") or []
                dflt = ", ".join(b[0] for b in f_.get("buttons", []) if b[1]) or "none"
                ovf = "; ".join(f"{n} {ch} in {vh} px" for n, _cw, ch, _vw, vh in f_.get("scroll", [])) or "none"
                parts.append(f"{w_[4] if len(w_) > 4 and w_[4] else t['state']} ({t.get('theme')}{', 24 pt' if 'font24' in sfx else ''}): "
                             f"{w_[0]}x{w_[1] if len(w_) > 1 else '?'}; focus {f_.get('focus') or 'none'}; default {dflt}; overflow {ovf}")
            T_.append(f"| {id_} | {x['size'][0]}×{x['size'][1]}, {x['mp'] / 1e6:.2f} MP{_scale_text(x)} | "
                      + ("<br>".join(parts) if parts else "—") + " |")
        # the contrast rows that fail or sit near their threshold, from the dialogs in these images (every theme
        # and size shown), measured as the strongest ink against the colour behind it
        W_ = []
        for (kind, x), id_ in zip(items, ids):
            seen2 = set()
            for t in x.get("tiles") or []:
                if t.get("area") not in ("dialog", "search"):
                    continue
                sfx = t["file"][len(t["area"]) + len(t["state"]) + 2:-4]
                if (t["state"], sfx) in seen2:
                    continue
                seen2.add((t["state"], sfx))
                f_ = fk.get(f"dlg-{t['state']}-{sfx}")
                for row in (f_ or {}).get("contrast", []):
                    nm_, txt_, decl_, ink_, bg_, r_decl, r_ink, px_, bold_ = row
                    large = px_ >= 24 or (bold_ and px_ >= 19)
                    need = 3.0 if large else 4.5
                    if r_ink < need + 0.5:
                        W_.append((r_ink, f"| {id_} | {(f_.get('win') or ['', '', '', '', t['state']])[4] or t['state']} "
                                          f"({sfx.replace('_', ' ')}) | {nm_} “{txt_.strip('“”')}” | {ink_} on {bg_} | "
                                          f"{r_ink:.2f}:1{f' (declared {r_decl:.2f})' if r_decl else ''} | "
                                          f"{'fails' if r_ink < need else 'near'} {need:g}:1{' (large text)' if large else ''} |"))
        W_.sort(key=lambda w: w[0])
        T_ += ["", "## Contrast to watch: rows that fail or sit within 0.5 of their threshold\n",
               "From the dialogs in these images (each theme and size shown); the strongest ink against the colour behind "
               "it; texts the layout lists but the frame does not draw are left out (see the long lines).\n",
               "| ID | dialog (theme, size) | item | ink on background | ratio | verdict |", "|---|---|---|---|---|---|"]
        T_ += [w[1] for w in W_[:30]] or ["| — | none | | | | |"]
        if len(W_) > 30:
            T_.append(f"\n({len(W_) - 30} more in the long lines below.)")
        lead = T_ + [""] + (lead or [])
    if sc_.get("system_facts"):
        lead = system_lead(rd, items, ids) + [""] + (lead or [])
    write_core_facts(rd, items, strips, geo_samples, entries, out=f"focus-build/{scope_name}/FOCUS-FACTS.md", ids=ids, looks=looks, lead=lead,
                     fact_keys=sc_.get("facts", []), fact_skip=sc_.get("facts_skip", ()), floor_labels=sc_.get("floors"),
                     title=f"# Focus facts: {sc_['title']}\n",
                     intro="What was measured in each focus image (IDs as in FOCUS.md), as text: tiles' states, key "
                           "colours, layout findings and contrast rows; recordings' motion metrics; then the sampled "
                           "facts of this surface. Colours are sampled from the rendered pixels.\n"
                           + "".join(f"\n**Note:** {n_}\n" for n_ in sc_.get("notes", []) + source_notes(sc_)),  # the scope's notes
                     a11y=_a11y_event_lines(rd, sc_.get("a11y_labels", [])), contrast_map=sc_.get("contrast", {}),
                     extra=[f"{lb}{strip_ends(rd, nm, th, (o_[0] if o_ else None))} (`{nm}`, {th}, not shown): "
                            + strip_facts((by_strip.get((nm, th)) or {}).get("metrics"))
                            for nm, th, lb, *o_ in sc_.get("facts_strips", [])] + extra_notes)
    print(f"pack: focus {scope_name}: {len(items)} images, {total / 1e6:.1f} MP in {fd}")
    fails += focus_limits(scope_name, [os.path.join(fd, f_) for f_ in sorted(os.listdir(fd)) if f_.endswith(".png")])
    return fails


def write_focus_packs(rd, src, names, entries, sheets, strips, geo_samples, jobs):
    """Each scope's focus pack, at the same time where they write no file in common. Returns the failures, in the
    scopes' order."""
    import focus
    if len(names) < 2 or jobs < 2 or not packs_apart([focus.SCOPES[nm] for nm in names], st):
        return [f for nm in names for f in write_focus(rd, src, nm, entries, sheets, strips, geo_samples)]
    with ProcessPoolExecutor(min(jobs, len(names))) as ex:
        done = [ex.submit(write_focus, rd, src, nm, entries, sheets, strips, geo_samples) for nm in names]
        return [f for d in done for f in d.result()]


def write_index(rd, src, entries, sheets, strips, dialog_failures=(), geo_samples=None):
    rnd = os.path.basename(rd.rstrip("/"))
    sj = os.path.join(rd, "source.json")  # recorded by shoot_round.sh before mirroring
    if os.path.exists(sj):
        so = json.load(open(sj))
        rev = f"{so['rev']} on {so['branch']} — {so.get('subject', '')}"
        dirty = so.get("dirty")
        src = so.get("path", src)
    else:
        rev = subprocess.run(["git", "-C", src, "log", "-1", "--format=%h %s"], capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", src, "status", "--porcelain"], capture_output=True, text=True).stdout.strip()
    fp = os.path.join(rd, "failures.txt")
    fails = open(fp).read().strip().splitlines() if os.path.exists(fp) else []
    renderers = collections.Counter(e.get("renderer") for e in entries)
    nraw = len(glob.glob(os.path.join(rd, "strips-raw", "*", "frames.tsv")))
    by_sheet = {(x["group"], x["theme"]): x for x in sheets if x["page"] == 1 and not x.get("core")}
    by_sheet.update({(x["group"], x["theme"]): x for x in sheets if x["page"] == 1 and x.get("core")})
    by_strip = {(x["name"], x["theme"]): x for x in strips}
    core = []
    for kind, key, theme in st.CORE:
        x = by_sheet.get((key, theme)) if kind == "sheet" else by_strip.get((key, theme))
        if x:
            core.append((kind, x))
            if kind == "sheet":  # loudly: only a core sheet's first page is in the core; its other tiles would not be
                more = [y for y in sheets if y["group"] == key and y["theme"] == theme and y["page"] > 1
                        and bool(y.get("core")) == bool(x.get("core"))]
                if more:
                    dialog_failures = list(dialog_failures) + [
                        f"core sheet {key} ({theme}) spills onto {len(more) + 1} pages: "
                        f"{sum(len(y['tiles']) for y in more)} tile(s) are not in the core"]
            wh = x.get("size") or []
            if wh and max(wh) > 1568:  # the reviewers' downscale would shrink it: the core is drawn to fit
                dialog_failures = list(dialog_failures) + [
                    f"core {kind} {key} ({theme}) is {wh[0]}x{wh[1]}, over the reviewers' 1568 px"]
        elif os.environ.get("SHOT_PARTIAL"):  # a partial run re-shoots some sessions: the rest may be missing
            print(f"pack: core {kind} {key} ({theme}) missing (partial run)", file=sys.stderr)
        else:  # loudly: listed at the top of INDEX.md, and pack fails (shoot_round.sh exits non-zero at its end)
            print(f"pack: CORE IMAGE MISSING: {kind} {key} ({theme})", file=sys.stderr)
            dialog_failures = list(dialog_failures) + [f"core {kind} {key} ({theme}) is missing from the round"]
    # Popup open/close recordings with a measured blink join the core by themselves (states.POPUP_STRIP_RE), the
    # worst theme of each, unless the core shows that recording already (as itself or a pair's half).
    shown = {x["name"] for kind, x in core if kind == "strip"}
    for nm, left, right, *_ in getattr(st, "STRIP_PAIRS", []):
        if f"pair:{nm}" in shown:
            shown |= {left, right}
    worst = {}
    for x in strips:
        m_ = x.get("metrics") or {}
        if not m_ or x["name"] in shown or not re.search(getattr(st, "POPUP_STRIP_RE", r"$^"), x["name"]):
            continue
        b = max([m_.get("blink_ms") or 0] + [e.get("blink_ms") or 0 for e in m_.get("elements", [])])
        if b > 0 and b > worst.get(x["name"], (0, None))[0]:
            worst[x["name"]] = (b, x)
    for nm, (b, x) in sorted(worst.items(), key=lambda kv: -kv[1][0]):
        core.append(("strip", dict(x, desc=f"[in the core by itself: a blink of {b} ms] {x['desc']}")))
        shown.add(nm)
        wh = x.get("size") or []
        if wh and max(wh) > 1568:
            dialog_failures = list(dialog_failures) + [f"core strip {nm} ({x['theme']}) is {wh[0]}x{wh[1]}, over 1568 px"]
    # any recording with frames that are neither its start nor its end nor a blend of them for more than
    # states.NEITHER_CORE_MS joins the core by itself (its worst theme), unless the core shows it already
    worst_n = {}
    for x in strips:
        nm_ = (x.get("metrics") or {}).get("neither_ms") or 0
        if nm_ > getattr(st, "NEITHER_CORE_MS", 50) and x["name"] not in shown and nm_ > worst_n.get(x["name"], (0, None))[0]:
            worst_n[x["name"]] = (nm_, x)
    for nm, (b, x) in sorted(worst_n.items(), key=lambda kv: -kv[1][0]):
        core.append(("strip", dict(x, desc=f"[in the core by itself: {b} ms of frames that are neither its start nor "
                                            f"its end nor a blend] {x['desc']}")))
        wh = x.get("size") or []
        if wh and max(wh) > 1568:
            dialog_failures = list(dialog_failures) + [f"core strip {nm} ({x['theme']}) is {wh[0]}x{wh[1]}, over 1568 px"]
    core, fails_w = weigh(rd, core)
    dialog_failures = list(dialog_failures) + fails_w
    write_core_facts(rd, core, strips, geo_samples, entries)
    core_mp = sum(x["mp"] for _, x in core)
    n_core = len(core)
    return _write_index_rest(rd, rnd, src, rev, dirty, entries, sheets, strips, core, core_mp, n_core, fails, renderers,
                             nraw, dialog_failures, geo_samples)


def split_one_to_one(rd, kind, x):
    """a 1:1 core image over CORE_MP_HARD, cut into parts that each fit, at rows of one colour (the gaps between
    a sheet's tiles), the part's title saying which part it is; None when no such cut fits (it then fails)."""
    import numpy as np
    sub = "sheets" if kind == "sheet" else "strips"
    im = Image.open(os.path.join(rd, sub, x["file"])).convert("RGB")
    a = np.asarray(im)
    uniform = np.nonzero((a == a[:, :1, :]).all(axis=(1, 2)))[0]  # rows of one colour
    max_h = int(CORE_MP_HARD * 0.99 // im.width)
    n_parts = -(-im.height // max_h)  # as few parts as fit, cut near equal heights
    cuts, y0 = [], 0
    while im.height - y0 > max_h:
        left = n_parts - len(cuts)
        target = y0 + (im.height - y0) / left
        cand = uniform[(uniform > y0 + 40) & (uniform <= y0 + max_h)]
        if not len(cand):
            return None
        cuts.append(int(cand[np.argmin(np.abs(cand - target))]))
        y0 = cuts[-1]
    if not cuts:
        return None
    os.makedirs(os.path.join(rd, sub, "core"), exist_ok=True)
    bounds = [0] + cuts + [im.height]
    out = []
    base = os.path.splitext(os.path.basename(x["file"]))[0]
    for k, (b0, b1) in enumerate(zip(bounds, bounds[1:]), 1):
        part = im.crop((0, b0, im.width, b1))
        nf = f"core/{base}-part{k}.png"
        part.save(os.path.join(rd, sub, nf), optimize=True)
        t_ = (x.get("title") or x.get("desc") or "") + f" (part {k} of {len(bounds) - 1}, cut at rows {b0}-{b1})"
        out.append(dict(x, file=nf, size=list(part.size), mp=part.width * part.height, full=x["file"],
                        **({"title": t_} if kind == "sheet" else {"desc": t_})))
    print(f"pack: core {kind} {x.get('group') or x.get('name')} ({im.width}x{im.height}) split into {len(out)} 1:1 parts")
    return out


def _scale_text(x):
    """", 1:1" (and its magnified insets, "with 4x insets"), or the scale the panels are shown at."""
    if not x.get("one_to_one"):
        return _scale_label(x)
    z = x.get("zoomed") or []
    return ", 1:1" + (f" with {' and '.join(f'{v}x' for v in z)} inset{'s' if z else ''}" if z else "")


def _scale_label(x):
    """the scale the panels are shown at: a sheet's own scaling times its tiles' fit (one number for a reader)."""
    f = (x.get("scaled") or 1.0) * (x.get("tile_fit") or 1.0)
    if f >= 0.999:
        return ""
    return f", panels at {f:.2f}x" if x.get("tile_fit", 1.0) < 1.0 else f", at {f:.2f}x"


def weigh(rd, core, label="core"):
    """The core's weight (CORE_MP): 1:1 images must fit as drawn (else a failure); the others get a copy fitted to
    CORE_MP (overview and picks CORE_MP_PICK). Returns (weighed items, failures)."""
    dialog_failures = []
    weighed = []
    for kind, x in core:
        wh = x.get("size") or [0, 0]
        mp = wh[0] * wh[1]
        if x.get("one_to_one"):
            if mp > CORE_MP_HARD and label == "core":  # split at empty rows into 1:1 parts that each fit
                parts = split_one_to_one(rd, kind, x)
                if parts:
                    weighed += [(kind, p_) for p_ in parts]
                    continue
            if mp > CORE_MP_HARD:
                dialog_failures = list(dialog_failures) + [
                    f"{label} {kind} {x.get('group') or x.get('name')} is 1:1 at {wh[0]}x{wh[1]} ({mp / 1e6:.2f} MP), over "
                    f"{CORE_MP_HARD / 1e6:.2f} MP: a reviewer would see it downscaled"]
            weighed.append((kind, dict(x, mp=mp)))
            continue
        budget = CORE_MP_PICK if kind == "sheet" and (x.get("group", "").startswith("pick:") or x.get("group") == "overview") \
            else CORE_MP
        if mp > budget and x.get("tile_fit", 1.0) < 1.0 and mp <= CORE_MP_HARD:
            weighed.append((kind, dict(x, mp=mp)))  # (fitted panels shown as fitted, not shrunk a second time)
            continue
        if mp > budget and x.get("tile_fit", 1.0) < 1.0:
            dialog_failures = list(dialog_failures) + [
                f"{label} {kind} {x.get('group') or x.get('name')} has fitted panels and is {mp / 1e6:.2f} MP, over "
                f"{CORE_MP_HARD / 1e6:.2f} MP: lower its tiles' fit (shrinking it again would make their labels wrong)"]
        if mp > budget:
            sub = "sheets" if kind == "sheet" else "strips"
            srcp = os.path.join(rd, sub, x["file"])
            f = (budget / mp) ** 0.5
            im = Image.open(srcp).convert("RGB")
            im = im.resize((int(im.width * f), int(im.height * f)), Image.LANCZOS)
            os.makedirs(os.path.join(rd, sub, "core"), exist_ok=True)
            nf = "core/" + os.path.splitext(os.path.basename(x["file"]))[0] + "-core.png"
            im.save(os.path.join(rd, sub, nf), optimize=True)
            x = dict(x, file=nf, size=list(im.size), scaled=f, full=x["file"])
            mp = im.width * im.height
        weighed.append((kind, dict(x, mp=mp)))
    return weighed, dialog_failures


def _write_index_rest(rd, rnd, src, rev, dirty, entries, sheets, strips, core, core_mp, n_core, fails, renderers, nraw,
                      dialog_failures, geo_samples):
    L = [f"# Ferrous UI capture — {rnd} screenshot pack\n",
         f"- Source: `{src}` at {rev}{' (with uncommitted changes)' if dirty else ''}",
         f"- Packaged: {datetime.datetime.now():%Y-%m-%d %H:%M} by `tools/ui-capture/pack.py` (part of `shoot_round.sh`).",
         f"- {len(entries)} full-resolution shots, {len(sheets)} contact sheets, {len(strips)} recorded transitions "
         f"(one merged image each, plus their frame pages).",
         f"- Rendering: headless `kwin_wayland --virtual`, Qt Quick on OpenGL (Mesa); renderer api per shot: "
         f"{dict(renderers)} (3 = OpenGL).",
         "- Theme: real Breeze Dark / Breeze Light colour schemes through the KDE platform theme, Noto Sans 10 pt, "
         "Breeze icons. Window 1600×1000 unless noted; fullscreen viewers are their own 2000×1400 window.",
         "- Content: a synthetic fixture library only (invented names, generated tones and covers).",
         "- Sheets are sized for a ~1568 px downscale: each tile stays ≥ 700 px wide. Tiles are cropped to what the "
         "state is about (the label says so); the full-resolution shot is always in `shots/` under the same name.",
         "- A transition's merged image (`strips/<strip>.png`, drawn 1568 px wide) has its caption (latency, each "
         "transition, the recording), up to 12 key frames with real timestamps (t = ms since the action; +Δ = ms "
         "since the previous rendered frame), the opacity/ink graph with the QML animations that ran, and two "
         "perceived-at-real-speed rows (a simulation). All key frames are on its frame pages "
         "(`strips/frames/`), every rendered frame in `strips-raw/<strip>/`. Transitions are recorded in dark, and "
         f"also in light for {', '.join(sorted(st.STRIP_BOTH_THEMES))}.\n",
         *([f"## CHECKS FAILED ({len(dialog_failures)})\n",
            "Dialog shots not of the window their state names (title and body checked, `states.DIALOG_EXPECT`; do "
            "not judge them as that dialog), and core images missing from the round:\n"]
           + [f"- {b_}" for b_ in dialog_failures] + [""] if dialog_failures else []),
         *toc(entries, sheets, strips, core),
         f"## Core set — every reviewer views these {n_core} images ({core_mp / 1e6:.1f} MP in all)\n",
         "Every core image has an ID (S = sheet, M = transition, D = document) for the list of images viewed. "
         "Everything else is reference: open it where a judgement needs it. **Weight:** a 1:1 image is at most "
         f"{CORE_MP_HARD / 1e6:.2f} MP as drawn (a reviewer's image input downscales anything larger, so 1:1 stays 1:1); "
         f"the others are core copies fitted to {CORE_MP / 1e6:.1f} MP (overview and picks {CORE_MP_PICK / 1e6:.1f} MP), "
         "their full size in the reference. **`CORE-FACTS.md`** (D1) gives each core image's measured facts as text "
         "(key colours, sizes, contrast, blink, overlap, travel, the plot's empty regions): keep notes from it.\n",
         "### Sheets\n"]
    i_s = i_m = 0
    for kind, x in core:
        if kind == "sheet":
            i_s += 1
            wt = f", {x['mp'] / 1e6:.2f} MP" + (" 1:1" if x.get("one_to_one") else f", at {x['scaled']:.2f}x of `sheets/{x['full']}`" if x.get("scaled") else "")
            L.append(f"- **S{i_s:02d}** `sheets/{x['file']}` — {x['title']} ({len(x['tiles'])} tile{'' if len(x['tiles']) == 1 else 's'}, {x['size'][0]}×{x['size'][1]}{wt})")
    L.append("\n### Transitions (one merged image each)\n")
    lb = latency_baseline(strips)
    if lb:
        L.append(lb + "\n")
    for kind, x in core:
        if kind == "strip":
            i_m += 1
            sz = (f" ({x['size'][0]}×{x['size'][1]}, {x['mp'] / 1e6:.2f} MP" + (" 1:1" if x.get("one_to_one") else
                  f", at {x['scaled']:.2f}x of `strips/{x['full']}`" if x.get("scaled") else "") + ")") if x.get("size") else ""
            L.append(f"- **M{i_m:02d}** `strips/{x['file']}`{sz} — {x['name']} ({x['theme']}): {x['desc']}")
            if x.get("caption"):
                L.append(f"  - {x['caption']}")
            for y in strips:  # its other views (states.EXTRA_VIEWS), in the reference
                if y["theme"] == x["theme"] and y["name"].startswith(x["name"] + "-") and y["name"][len(x["name"]) + 1:] in \
                        {sfx for sfx, _ in getattr(st, "EXTRA_VIEWS", {}).get(x["name"], [])}:
                    L.append(f"  - Also, {y['desc'].rsplit('(', 1)[-1].rstrip(')')}: `strips/{y['file']}` (reference)")
    L.append("\n### Measurements\n")
    docs = []
    if os.path.exists(os.path.join(rd, "CORE-FACTS.md")):
        docs.append("`CORE-FACTS.md` — the core images' measured facts as text, by ID: tiles, key colours, layout "
                    "findings, contrast rows; motion metrics; the plot's empty regions and the mode switch's plates, sampled.")
    if os.path.exists(os.path.join(rd, "LAYOUT.md")):
        docs.append("`LAYOUT.md` — layout defects the live app reports in every state (clipped and overflowing text, "
                    "out-of-bounds items, overlaps, elisions, text contrast); machine-readable `layout.json`.")
    if os.path.exists(os.path.join(rd, "CONTRAST.md")):
        docs.append("`CONTRAST.md` — WCAG contrast ratios sampled from the rendered pixels, both themes.")
    if os.path.exists(os.path.join(rd, "MOTION-METRICS.md")):
        docs.append("`MOTION-METRICS.md` — per transition: latency, each transition's duration, the recording, blink "
                    "per scope (text, other), text overlap, the whole window under its darker end, cross-fades under the "
                    "one-layer mix of their ends, movement after the stated end, the QML animations that ran.")
    n_a11y, n_unnamed = write_focus_a11y(rd)
    if n_a11y:
        docs.append(f"`FOCUS-A11Y.md` — the focus walk's accessibility transcript: role, name, value and states at each of "
                    f"{n_a11y} focus stops (10 pt and 24 pt, both themes); {n_unnamed} without an accessible name.")
    docs.append("`MOTION.md` — every animation with file:line, property, duration and easing.")
    for i, d in enumerate(docs, 1):
        L.append(f"- **D{i}** {d}")
    L.append("")
    L.append("## Reference — open where a judgement needs it\n")
    L.append("### All sheets\n")
    for s in sheets:
        if s.get("core"):  # the core set's smaller version of a sheet listed here
            continue
        L.append(f"#### `sheets/{s['file']}` — {s['title']} ({s['size'][0]}×{s['size'][1]})\n")
        L.append("| state | theme | what it shows | full-resolution shot |\n|---|---|---|---|")
        for t in s["tiles"]:
            L.append(f"| {t['area']}/{t['state']} | {t['theme']} | {t['desc']}"
                     f"{' (' + t['note'] + ')' if t['note'] else ''} | `shots/{t['file']}` |")
        L.append("")
    L.append("### All transitions\n")
    L.append("| strip | theme | what it shows | frames recorded (key frames) | avg fps | merged image | frame pages | all frames |\n|---|---|---|---|---|---|---|---|")
    for s in strips:
        L.append(f"| {s['name']} | {s['theme']} | {s['desc']} | {s['frames']} ({s['shown']}) | {s['fps']} | "
                 f"`strips/{s['file']}` | {', '.join('`strips/' + p + '`' for p in s.get('pages', []))} | `{s['raw']}` |")
    L.append("\nLight-theme recordings of the other transitions are in `strips-raw/` too.\n")
    L.append("### Other files\n")
    L.append("- `MOTION.md`: the animation inventory. `BUGS.md`: non-visual bugs noticed while driving.")
    L.append("- `focus_owners_<theme>.txt`: the item each Tab stop reached. `failures.txt`: states a step failed on.\n")
    L.append("## Not captured\n")
    for name, why in st.NOT_CAPTURED:
        L.append(f"- **{name}**: {why}")
    if fails:
        L.append("\n### Driver failures in this run\n")
        L += [f"- {f}" for f in fails]
    open(os.path.join(rd, "INDEX.md"), "w").write("\n".join(L) + "\n")
    return list(dialog_failures)


if __name__ == "__main__":
    main()
