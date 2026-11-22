#!/usr/bin/env python3
"""Real-time evidence for a motion strip (pack.py calls `analyse` per recorded transition).

A strip lays a ~100 ms change out as frozen frames: slow motion a reviewer cannot watch at speed. `analyse`
measures the recording and draws ONE merged image per transition, at the reviewers' 1568 px width:
- caption: whether every lane's crosshair is at one x (the recordings MOTION_FOCUS "lanes" names);
  latency (action to first change), each transition (first to last change; changes more than
  100 ms apart are separate transitions, e.g. a hover fade in and out, or a wait then an auto-hide) and the
  recording, kept apart; blink per scope and per changed item; text overlap; movement after the stated end;
- key frames (at most 12; the strip pages under strips/frames/ have the rest);
- a graph: the animated items' opacity (the probe's per-frame item trace), the ink of each scope (text: the
  rects of the text items in the recording; other: images, icons, plots), luminance, the transitions, blink
  stretches, and a bar per QML animation that ran (the runtime animation timeline);
- two "perceived at real speed" rows (a simulation: the time-weighted mean of the frames on screen over the
  45 ms before t): 5 moments across the main transition, then every 16.7 ms around its strongest change;
  small controls at 2x, or a crop given per strip (states.PERCEIVED_FOCUS).
"""
import bisect
import fnmatch
import itertools
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from colours import unique_colours

FONT = "/usr/share/fonts/google-noto/NotoSans-Regular.ttf"
FONT_B = "/usr/share/fonts/google-noto/NotoSans-Bold.ttf"
WINDOW_MS = 45.0
BLINK_MAX_MS = 250.0  # a longer dip is a state of its own, not a blink
# a blink is a dip and return to HALF or less; an item's opacity (which a cross-fade of two items cannot average
# down, as it does their ink) also blinks by a dip of over DIP for over DIP_MS
DIP, DIP_MS, HALF = 0.25, 30.0, 0.5
EVEN = 0.005  # what rounding (8-bit pixels, the trace's three decimals) adds to a dip to exactly HALF
GLANCE_S = 0.2  # assumed time a reviewer looks at one strip frame, for the slow-motion factor
COLORS = [(255, 196, 64), (102, 204, 255), (255, 120, 120), (150, 230, 120), (210, 150, 255), (255, 160, 220),
          (120, 230, 220), (230, 230, 120)]


def font(b, n):
    return ImageFont.truetype(FONT_B if b else FONT, n)


def resolve_rect(spec, meta, rec_w, rec_h):
    """A rect in the recording's coordinates from (x, y, w, h) (a None width or height: to the recording's edge) or
    from NAME[:fx,fy,fw,fh][+...]: the main window's items as the strip recorded them (meta "items"), in window
    coordinates, mapped by the recording's crop. A name the recording does not have fails loudly."""
    if spec is None:
        return None
    ox, oy = (meta.get("crop") or [0, 0])[:2]
    if isinstance(spec, str):
        items, box = meta.get("items") or {}, None
        for part in spec.split("+"):
            name, _, frac = part.partition(":")
            if name not in items:
                raise LookupError(f"{meta.get('name')}: no item {name} recorded with the strip")
            x, y, w, h = items[name]
            if frac:
                fx, fy, fw, fh = map(float, frac.split(","))
                x, y, w, h = x + w * fx, y + h * fy, w * fw, h * fh
            box = [x, y, w, h] if box is None else [min(box[0], x), min(box[1], y),
                                                    max(box[0] + box[2], x + w) - min(box[0], x),
                                                    max(box[1] + box[3], y + h) - min(box[1], y)]
        x, y, w, h = box
        x, y = x - ox, y - oy
    else:
        x, y, w, h = spec
        w = rec_w - x if w is None else w
        h = rec_h - y if h is None else h
    x, y = max(0, int(round(x))), max(0, int(round(y)))
    return (x, y, int(min(rec_w - x, round(w))), int(min(rec_h - y, round(h))))


def read_trace(d, t_action):
    """Opacity tracks {key: dict(text, rect, pts=[(t_rel_action, v)])} and the animations that ran."""
    tracks, anims = {}, []
    p = os.path.join(d, "opacity.tsv")
    if os.path.exists(p):
        t0 = None
        with open(p, encoding="utf-8") as tsv:
            lines = tsv.readlines()
        for line in lines:
            f = line.rstrip("\n").split("\t")
            if f[0] == "#t0":
                t0 = float(f[1])
                continue
            if len(f) < 4 or t0 is None:
                continue
            pts, rects, layers, fulls = [], [], [], []
            rect = [int(v) for v in f[2].split(",")]
            layer = f[4] if len(f) > 4 else "-"
            for tok in f[3].split():
                tok, _, lay = tok.partition("^")  # "^layer": it moved to another layer (a popup, or "-")
                if lay:
                    layer = lay
                tok, _, fl = tok.partition("~")  # "~x,y,w,h": the item's own rect where a clip cuts it
                tok, _, at = tok.partition("@")  # "@x,y,w,h": the item moved to there (list delegates are reused)
                if at:
                    rect = [int(v) for v in at.split(",")]
                a, b = tok.split(":")
                pts.append((t0 + float(a) - t_action, float(b)))
                rects.append(rect)
                fulls.append([int(v) for v in fl.split(",")] if fl else rect)
                layers.append(layer)
            tracks[f[0]] = dict(text=f[1] == "1", rect=rects[0] if rects else rect, rects=rects, pts=pts,
                                layers=layers, fulls=fulls)
    p = os.path.join(d, "animations.tsv")
    if os.path.exists(p):
        t0 = None
        with open(p, encoding="utf-8") as tsv:
            lines = tsv.readlines()
        for line in lines:
            f = line.rstrip("\n").split("\t")
            if f[0] == "#t0":
                t0 = float(f[1])
                continue
            if len(f) < 10 or t0 is None:
                continue
            kind, target, prop, cls, frm, to, dur, ease, st, en = f[:10]
            try:
                st = t0 + float(st) - t_action
                en = t0 + float(en) - t_action if float(en) >= 0 else None
            except ValueError:
                continue
            anims.append(dict(kind=kind, target=target.rsplit("/", 1)[-1], prop=prop, cls=cls, frm=frm, to=to,
                              duration=int(float(dur)) if dur not in ("", "-1") else None, easing=ease, start=st, end=en))
    # identical rows (several instances of one component) once
    seen, uniq = set(), []
    for a in anims:
        k = (a["target"], a["prop"], a["frm"], a["to"], round(a["start"]))
        if k not in seen:
            seen.add(k)
            uniq.append(a)
    return tracks, uniq


def value_at(pts, t):
    v = pts[0][1]
    for tt, vv in pts:
        if tt <= t:
            v = vv
        else:
            break
    return v


def at(pts, vals, t):
    """vals[k] for the last point of pts at or before t (the first before any)."""
    v = vals[0]
    for (tt, _), vv in zip(pts, vals):
        if tt <= t:
            v = vv
        else:
            break
    return v


def overlap_area(a, b):
    ix = max(0, min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0]))
    iy = max(0, min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1]))
    return ix * iy


def longest(mask_times):
    """Longest run of True in [(t_start, t_end, flag)] (ms)."""
    best = cur = 0.0
    for a, b, f in mask_times:
        if f:
            cur += b - a
            best = max(best, cur)
        else:
            cur = 0.0
    return best




SEG_GAP_MS = 100.0  # changes further apart than this are separate transitions (e.g. a hover fade in, then out)
FRAME_MS = 1000.0 / 60.0
MAX_H = 1568  # nor taller than the reviewers' downscale
W = 1200  # the merged image's width: under the reviewers' 1568 px downscale, and small in their context
PAD = 14
MIN_FOCUS_SCALE = 0.75  # the perceived rows show at least this much of a large change, cropped around its centre


def segments_of(trans, times):
    """Runs of transition frames; a new run after more than SEG_GAP_MS without one."""
    segs = []
    for k in trans:
        if segs and times[k] - times[segs[-1][-1]] <= SEG_GAP_MS:
            segs[-1].append(k)
        else:
            segs.append([k])
    return [dict(a=s[0], b=s[-1], frames=s, start=times[s[0]], end=times[s[-1]] + FRAME_MS,
                 dur=times[s[-1]] - times[s[0]] + FRAME_MS) for s in segs]


def scope_masks(tracks, region, box, stp, shape, scale, off=(0, 0)):
    """Strided boolean masks (text, other) over the change box: text = the rects of the text items shown
    in the recording (from the item tree), other = the rest (cover art, icons, plots). `off`: where the
    recording's image starts in the window (a recorded crop)."""
    x, y = region[0] + off[0], region[1] + off[1]
    bx0, by0 = box[0], box[1]
    m = np.zeros(shape, dtype=bool)
    n = 0
    for tr in tracks.values():
        if not tr["text"] or max(v for _, v in tr["pts"]) <= 0.3:
            continue
        hit = False
        for r in {tuple(r) for r in tr["rects"]}:
            c0 = int((r[0] * scale - x) / stp) - bx0
            r0 = int((r[1] * scale - y) / stp) - by0
            c1 = int(np.ceil(((r[0] + r[2]) * scale - x) / stp)) - bx0
            r1 = int(np.ceil(((r[1] + r[3]) * scale - y) / stp)) - by0
            c0, r0, c1, r1 = max(0, c0), max(0, r0), min(shape[1], c1), min(shape[0], r1)
            if c1 > c0 and r1 > r0:
                m[r0:r1, c0:c1] = True
                hit = True
        n += hit
    return m, ~m, n


def sum3(d):
    """d.sum(axis=-1) of pixels (..., 3), several times faster than numpy's reduction over so short an axis."""
    return d[..., 0] + d[..., 1] + d[..., 2]


def max3(d):
    """d.max(axis=-1) of pixels (..., 3), several times faster than numpy's reduction over so short an axis."""
    return np.maximum(np.maximum(d[..., 0], d[..., 1]), d[..., 2])


def ink_series(arrs, mask):
    """Mean colour distance from the scope's background (its most frequent colour in the first frame)."""
    # (a boolean index of rows of pixels is several times slower than the rows themselves or their positions)
    at = None if mask.all() else np.flatnonzero(mask)
    pick = lambda a: a.reshape(-1, 3) if at is None else a.reshape(-1, 3).take(at, axis=0)
    px0 = pick(arrs[0]).astype(np.int32)
    if len(px0) == 0:
        return None
    vals, counts = unique_colours(px0)
    bg = vals[counts.argmax()].astype(np.float32)
    return [float(sum3(np.abs(pick(a) - bg)).mean()) for a in arrs]


def blink_of(ink, spans, segs):
    """Per transition: the longest dip of the ink to HALF or less of the lower of its levels before and after (at
    most BLINK_MAX_MS: a longer low stretch is a state of its own, e.g. a menu held open). Not by DIP: a designed
    cross-fade of two texts takes their mixed ink down a third. Returns (ms, per-frame mask)."""
    bmask = [False] * len(spans)
    best = 0.0
    for s in segs:
        a, b = s["a"], s["b"]
        ref = min(ink[max(0, a - 1)], ink[b])
        if HALF * ref <= 0.5:
            continue
        lo = (HALF + EVEN) * ref
        k = a
        while k <= b:
            if ink[k] <= lo:
                j = k
                while j + 1 <= b and ink[j + 1] <= lo:
                    j += 1
                dur = spans[j][1] - spans[k][0]
                if dur <= BLINK_MAX_MS:
                    best = max(best, dur)
                    for q in range(k, j + 1):
                        bmask[q] = True
                k = j + 1
            else:
                k += 1
    return best, bmask


def opacity_dips(pts):
    """The dips of one item's traced opacity [(t, v)] that return: each to under 1 - DIP of the level it had and
    back over it. Yields (the level before, the indices of the first point of the decline, the first under that
    threshold, the first back over it and the one the return rises to). A fade-out, a fade-in and a low stretch
    still low at the trace's end are none."""
    n, i, top = len(pts), 1, 0
    while i < n:
        peak = pts[top][1]
        if pts[i][1] >= peak:
            top = i
        elif peak > 0.1 and pts[i][1] < (1 - DIP) * peak:
            j = i
            while j < n and pts[j][1] < (1 - DIP) * peak:
                j += 1
            if j == n:
                return
            r = j
            while r + 1 < n and pts[r + 1][1] > pts[r][1]:
                r += 1
            yield peak, top + 1, i, j, r
            top, i = r, r
        i += 1


def edge_energy(a):
    """How much is drawn in pixels (..., 3): the mean step between neighbours, which a dim scales as it does the
    contrast and which the colour behind an item does not move."""
    g = sum3(a)
    return sum(float(np.abs(np.diff(g, axis=ax)).mean()) for ax in (0, 1) if g.shape[ax] > 1)


def seen_dip(before, low, after):
    """Whether the pixels of an item's rect show its traced dip: the frame in it is neither the one before nor
    the one after, and it either draws less than both by over DIP or goes away and comes back as it was. An item
    whose trace dips under something opaque (an incoming text behind the outgoing one), or an empty container,
    shows none."""
    off = lambda p, q: float((sum3(np.abs(p - q)) > 30).mean())
    out, back = off(low, before), off(after, low)
    if min(out, back) < 0.01:
        return False
    if off(after, before) < 0.25 * min(out, back):
        return True
    return edge_energy(low) < (1 - DIP) * min(edge_energy(before), edge_energy(after))


def item_blinks(tracks, times, spans, ims, scale=1.0, off=(0, 0)):
    """Blink per popup or other changed item, in its own rect, where a wide change box averages it away: the
    item's traced opacity (every frame) dips by over DIP and returns at that place, for over DIP_MS or to HALF or
    less, for at most BLINK_MAX_MS of recorded frames, and those frames show it (seen_dip). One per dip: of the
    items that dip together in one place (a list, its rows, their texts) the largest. [dict(label, start, ms,
    low = the lowest share of its level, items = how many dip with it, box in window coordinates)], by time."""
    found = []
    for key, tr in tracks.items():
        pts, rects = tr["pts"], tr["rects"]
        for peak, d, a, b, r in opacity_dips(pts):
            if pts[a][0] < -1:
                continue
            ka, kb = bisect.bisect_left(times, pts[a][0]), bisect.bisect_left(times, pts[b][0])
            before = bisect.bisect_left(times, pts[d][0]) - 1
            if ka == kb or before < 0 or kb >= len(times):  # (no recorded frame of it, or none on either side)
                continue
            vals = [value_at(pts, times[k]) / peak for k in range(ka, kb)]
            ms, low = spans[kb - 1][1] - spans[ka][0], min(vals)
            if ms > BLINK_MAX_MS or (ms <= DIP_MS and low > HALF + EVEN):
                continue
            p, q = rects[d - 1], rects[r]
            if overlap_area(p, q) <= 0.5 * max(1, min(p[2] * p[3], q[2] * q[3])):  # (a reused delegate, elsewhere)
                continue
            # (seen at the first, middle and last of its lowest frames: an incoming item traced at 0 until the hand-
            # over ends is on screen well before, as a blend)
            bottom = [ka + i for i, v in enumerate(vals) if v <= low + EVEN]
            found.append(dict(key=key, label=short_name(key), start=times[ka], ms=ms, low=low, box=list(p),
                              frames=(before, (bottom[0], bottom[len(bottom) // 2], bottom[-1]),
                                      min(len(times) - 1, bisect.bisect_left(times, pts[r][0])))))
    found.sort(key=lambda f: (-f["box"][2] * f["box"][3], f["key"].count("/"), f["key"]))
    out, looked = [], {}
    for f in found:
        x, y, w, h = f["box"]
        with_ = next((o for o in out if f["start"] < o["start"] + o["ms"] and o["start"] < f["start"] + f["ms"]
                      and overlap_area(f["box"], o["box"]) > 0.5 * max(1, w * h)), None)
        if with_:
            with_["items"] += 1
            continue
        k = (tuple(f["box"]), f["frames"])
        if k not in looked:
            iw, ih = ims[0].size
            # (a pixel or two around it: a plain item's only edges are its own)
            x0, y0 = max(0, int((x - off[0]) * scale) - 2), max(0, int((y - off[1]) * scale) - 2)
            x1, y1 = min(iw, int((x + w - off[0]) * scale) + 2), min(ih, int((y + h - off[1]) * scale) + 2)
            first, lows, last = f["frames"]
            cut = lambda n: np.asarray(ims[n].crop((x0, y0, x1, y1)), dtype=np.int16)
            looked[k] = x1 - x0 >= 4 and y1 - y0 >= 4 and all(
                seen_dip(cut(first), cut(n), cut(last)) for n in dict.fromkeys(lows))
        if looked[k]:
            out.append(dict(f, items=1))
    return sorted(out, key=lambda f: f["start"])


def item_blink_text(b):
    """One item blink in words."""
    return (f"{b['label']}" + (f" and {b['items'] - 1} items in it" if b["items"] > 1 else "")
            + f" down to {100 * b['low']:.0f} % for {b['ms']} ms from +{b['start_ms']} ms")


def text_overlap(tracks, segs):
    """During each transition: two text items in the same place at that moment (delegates move; rects cut to
    clipping ancestors), both above 30 % opacity, one of them mid-fade and not fading with the other as one
    layer, both in the same layer (a popup's text over the content is drawn over it on purpose): a cross-fade's
    double text. Two steady overlapping texts are LAYOUT.md's."""
    texts = {k: v for k, v in tracks.items() if v["text"]}
    fading = {k for k, v in texts.items() if max(p[1] for p in v["pts"]) - min(p[1] for p in v["pts"]) > 0.05}
    best = 0.0
    # an item first traced after the trace began did not exist before its first point: absent then, not at its
    # first value (a later transition's layers were counted in an earlier one: track-change-metadata's 133 ms)
    t_first = min((v["pts"][0][0] for v in tracks.values() if v["pts"]), default=0.0)
    opac = lambda v, m: 0.0 if (v["pts"][0][0] > t_first + 1 and m < v["pts"][0][0]) else value_at(v["pts"], m)
    for s in segs:
        if not fading:
            break
        w_lo, w_hi = s["start"] - FRAME_MS, s["end"] + FRAME_MS
        cuts = sorted({t for k in fading for t, _ in texts[k]["pts"] if w_lo < t < w_hi} | {w_lo, w_hi})
        mids = [(a + b) / 2 for a, b in zip(cuts, cuts[1:])]
        unit = {}

        def one_layer(p, q):
            key = (p, q) if p < q else (q, p)
            if key not in unit:
                unit[key] = all(abs(value_at(texts[p]["pts"], m) - value_at(texts[q]["pts"], m)) < 0.02 for m in mids)
            return unit[key]
        runs = []
        for (a, b), m in zip(zip(cuts, cuts[1:]), mids):
            op = {k: opac(v, m) for k, v in texts.items()}
            vis = {k: at(v["pts"], v["rects"], m) for k, v in texts.items() if op[k] > 0.3}
            both = any(op[p] < 0.97 and overlap_area(vis[p], vis[q]) > 0.3 * max(1, min(vis[p][2] * vis[p][3], vis[q][2] * vis[q][3]))
                       and at(texts[p]["pts"], texts[p]["layers"], m) == at(texts[q]["pts"], texts[q]["layers"], m)
                       and not one_layer(p, q)
                       for p in fading if p in vis for q in vis if q != p)
            runs.append((a, b, both))
        best = max(best, longest(runs))
    return best


DASH, DASH_PERIOD = 4, 7  # the plots' crosshair line: 4 px drawn, 3 not (logical px)
CROSS_SEEN, CROSS_FAINT = 8.0, 5.0  # how far over the noise such a line stands: seen, or just there where expected


def dashed_columns(lane):
    """How clearly each column of a lane's luminance (rows, cols) is a dashed vertical line of DASH on, the rest of
    DASH_PERIOD off, at any phase and lighter or darker than the plot: the t statistic of the column against its
    neighbours 2 px either side, drawn rows against the others. The plot's own verticals (a transient, the solid
    playhead) have no such period and stay at the noise's few units."""
    r = lane[:, 2:-2] - 0.5 * (lane[:, :-4] + lane[:, 4:])
    n = r.shape[0]
    whole = n - n % DASH_PERIOD  # (the rows summed by their place in the period)
    by_place = r[:whole].reshape(-1, DASH_PERIOD, r.shape[1]).sum(axis=0)
    by_place[:n - whole] += r[whole:]
    drawn = np.zeros((DASH_PERIOD, DASH_PERIOD), dtype=np.float32)  # (the places drawn at each phase)
    for phase in range(DASH_PERIOD):
        drawn[phase, [(phase + k) % DASH_PERIOD for k in range(DASH)]] = 1.0
    on = drawn @ by_place
    n_on = (drawn @ np.bincount(np.arange(n) % DASH_PERIOD, minlength=DASH_PERIOD).astype(np.float32))[:, None]
    off, n_off = by_place.sum(axis=0) - on, n - n_on
    var = (np.einsum("ij,ij->j", r, r) - on * on / n_on - off * off / n_off) / max(1, n - 2)
    t = np.abs(on / n_on - off / n_off) / np.sqrt(np.maximum(var, 1.0) * (1.0 / n_on + 1.0 / n_off))
    return np.concatenate(([0, 0], t.max(axis=0), [0, 0]))


def crosshair_lanes(ims, times, pane, lanes, at_=(0, 0)):
    """Whether every lane of a plot shows its crosshair's vertical line at one x, in every recorded frame that
    shows one: pane (x, y, w, h) in the recording's images, its lanes sharing its height equally; at_: where those
    images start in the window. A frame where they differ by over a pixel, or where a lane lacks a line that another
    shows plainly, is a split. dict(ok: None when nothing could be compared, lanes, frames: how many show one,
    split: how many of those are split, first, last: the split frames' indices, start_ms, end_ms, apart_px: the
    most, worst: dict(frame, xs: per lane in the window, None without one))."""
    out = dict(ok=None, lanes=lanes, frames=0, split=0)
    if lanes < 2:
        return out
    x, y, w, h = pane
    lh = h / lanes
    split = []
    for k in range(len(ims)):
        g = np.asarray(ims[k].crop((x, y, x + w, y + h)).convert("L"), dtype=np.float32)
        scores = []
        for i in range(lanes):  # (a lane's edge rows hold the seam)
            scores.append(dashed_columns(g[int(i * lh) + 3:int((i + 1) * lh) - 3]))
        tops = [float(s.max()) for s in scores]
        strongest = max(range(lanes), key=lambda i: tops[i])
        if tops[strongest] < CROSS_SEEN:
            continue
        x0 = int(scores[strongest].argmax())
        xs = []
        for s, top in zip(scores, tops):
            faint = float(s[max(0, x0 - 1):x0 + 2].max()) >= CROSS_FAINT
            xs.append(int(s.argmax()) if top >= CROSS_SEEN else x0 if faint else None)
        seen = [v for v in xs if v is not None]
        apart = max(seen) - min(seen)
        missing = len(seen) < lanes
        if missing and apart <= 1 and tops[strongest] < 2 * CROSS_SEEN:
            continue  # (a crosshair still fading in, too faint to say that a lane lacks it)
        out["frames"] += 1
        if apart > 1 or missing:
            split.append((apart, k, [None if v is None else v + x + at_[0] for v in xs]))
    if out["frames"]:
        out["ok"] = not split
    if split:
        apart, k, xs = max(split, key=lambda s: s[0])
        out.update(split=len(split), first=split[0][1], last=split[-1][1], start_ms=round(times[split[0][1]]),
                   end_ms=round(times[split[-1][1]]), apart_px=apart, worst=dict(frame=k, xs=xs))
    return out


def crosshair_text(c):
    """crosshair_lanes in words."""
    q = "All lanes' crosshair x agree in every frame:"
    if c["ok"] is None:
        why = "fewer than two lanes in the trace" if c["lanes"] < 2 else "no frame shows a crosshair"
        return f"{q} NOT CHECKED ({why})"
    if c["ok"]:
        return f"{q} yes ({c['lanes']} lanes, the {c['frames']} frames that show one)"
    return (f"{q} NO (FAIL): split in {c['split']} of the {c['frames']} frames that show one, "
            f"f{c['first']:04d}-f{c['last']:04d}, +{c['start_ms']} to +{c['end_ms']} ms, up to {c['apart_px']} px apart "
            f"(f{c['worst']['frame']:04d}: x " + " / ".join("none" if v is None else str(v) for v in c["worst"]["xs"])
            + ", top lane first)")


def traced_lanes(tracks, pane):
    """How many lanes a plot pane (window coordinates) has: the distinct heights of its lane labels in the trace."""
    x, y, w, h = pane
    return len({tr["rects"][0][1] for k, tr in tracks.items()
                if "plotLaneLabel/" in k and k.split("/")[-1].startswith("plotPlateLabel")
                and x <= tr["rects"][0][0] < x + w and y <= tr["rects"][0][1] < y + h})


def _ancestors(key, keys):
    """the tracked ancestors of an item (by the trace's path keys: an ancestor's last segment in the item's path,
    the segments before it agreeing), grouped per ancestor path: [[key, ...], ...] (several instances of one path:
    an outgoing layer per track change)."""
    dn = [s_.split("#")[0] for s_ in key.split("/")]
    groups = {}
    for a_ in keys:
        if a_ == key:
            continue
        an = [s_.split("#")[0] for s_ in a_.split("/")]
        if an[-1] not in dn[:-1]:
            continue
        j = max(i for i, s_ in enumerate(dn[:-1]) if s_ == an[-1])
        pre_a, pre_d = an[:-1], dn[:j]
        k = min(len(pre_a), len(pre_d))
        if k and pre_a[-k:] != pre_d[-k:]:
            continue
        groups.setdefault(tuple(an), []).append(a_)
    return list(groups.values())


def text_peak(tracks, segs, keys=None):
    """the text overlap as an amount, not a thresholded duration: during each transition, the largest opacity
    two text items in the same place show at once (min of the two effective opacities: each item's own times its
    tracked ancestors', e.g. a fading outgoing layer), one of them mid-fade and the two not fading as one layer,
    both in the same layer. Only what is drawn counts: each item's rect is cut by its tracked ancestors' rects
    (a clip window wiping it), and an item never pairs with its own ancestor or descendant (a line and the elision
    tail drawn inside it are one line). Returns dict(peak, t, ops, names, ms): ms = how long any pair stays at or
    above 0.1. keys: the candidate texts (a now-playing line's items), all tracks otherwise."""
    none = dict(peak=0.0, t=None, ops=None, names=None, ms=0)
    cand = {k: v for k, v in tracks.items() if v["text"] and (keys is None or k in keys)}
    if not cand:
        return none
    t_first = min((v["pts"][0][0] for v in tracks.values() if v["pts"]), default=0.0)
    opac = lambda v, m: 0.0 if (v["pts"][0][0] > t_first + 1 and m < v["pts"][0][0]) else value_at(v["pts"], m)
    anc = {k: _ancestors(k, list(tracks)) for k in cand}

    def chosen(k, grp):
        num = k.split("/")[-1].partition("#")[2]
        same = [a_ for a_ in grp if a_.split("/")[-1].partition("#")[2] == num]
        return same if len(same) == 1 else grp

    def eff(k, m):
        v = opac(tracks[k], m)
        for grp in anc[k]:
            v *= max(opac(tracks[a_], m) for a_ in chosen(k, grp))
        return v

    def visible(k, m):
        x0, y0, w0, h0 = at(cand[k]["pts"], cand[k]["rects"], m)
        x1, y1 = x0 + w0, y0 + h0
        for grp in anc[k]:
            rs = [at(tracks[a_]["pts"], tracks[a_]["rects"], m) for a_ in chosen(k, grp)]
            if len(rs) == 1:
                ax, ay, aw, ah = rs[0]
                if aw > 0 and ah > 0:
                    x0, y0, x1, y1 = max(x0, ax), max(y0, ay), min(x1, ax + aw), min(y1, ay + ah)
        return [x0, y0, max(0, x1 - x0), max(0, y1 - y0)]
    related = lambda p_, q_: any(p_ in g for g in anc[q_]) or any(q_ in g for g in anc[p_])
    best = dict(none)
    total = 0.0
    for s_ in segs:
        w_lo, w_hi = s_["start"] - FRAME_MS, s_["end"] + FRAME_MS
        ks = set(cand) | {a_ for k in cand for g in anc[k] for a_ in g}
        cuts = sorted({t for k in ks for t, _ in tracks[k]["pts"] if w_lo < t < w_hi} | {w_lo, w_hi})
        spans_ = list(zip(cuts, cuts[1:]))
        mids = [(a_ + b_) / 2 for a_, b_ in spans_]
        es = [{k: eff(k, m) for k in cand} for m in mids]
        # (two items fading as one layer: equal at every moment of the transition, so not a cross-fade)
        one = lambda p_, q_: all(abs(e[p_] - e[q_]) < 0.02 for e in es)
        for (a_, b_), m, e in zip(spans_, mids, es):
            vis = {k: visible(k, m) for k in cand if e[k] > 0.02}
            vis = {k: r for k, r in vis.items() if r[2] * r[3] > 0}
            here = 0.0
            for p_ in vis:
                for q_ in vis:
                    if q_ <= p_ or (min(e[p_], e[q_]) >= 0.97):
                        continue  # (both steady: a layout overlap, LAYOUT.md's)
                    if overlap_area(vis[p_], vis[q_]) <= 0.3 * max(1, min(vis[p_][2] * vis[p_][3], vis[q_][2] * vis[q_][3])):
                        continue
                    if at(cand[p_]["pts"], cand[p_]["layers"], m) != at(cand[q_]["pts"], cand[q_]["layers"], m):
                        continue
                    if related(p_, q_) or one(p_, q_):
                        continue
                    v = min(e[p_], e[q_])
                    here = max(here, v)
                    if v > best["peak"]:
                        nm = lambda k: k.split("/")[-2] + "/" + k.split("/")[-1] if k.split("/")[-1].startswith(("LineText", "Item")) \
                            else k.split("/")[-1]
                        best = dict(peak=round(v, 2), t=round(m), ops=(round(e[p_], 2), round(e[q_], 2)),
                                    names=(nm(p_), nm(q_)), ms=0)
            if here >= 0.1:
                total += b_ - a_
    best["ms"] = round(total)
    return best


def _tile_neither(F, S, E, T):
    """the share of the changing T-px tiles (start vs end) of frame F that are neither the start nor the end nor a
    blend of them: a tile is explained by S or E at its place, by a blend of the two there (a cross-fade), or, when it
    has texture, by S or E moved up or down (content sliding: the library under a growing card). Plain tiles only at
    their place: an empty band must not pass as the plain gaps between library rows."""
    H, W = S.shape[:2]
    th, tw = H // T, W // T
    if th < 1 or tw < 1:
        return 0.0

    def tiles(a):
        return a[:th * T, :tw * T].reshape(th, T, tw, T).mean(axis=(1, 3))

    def off_share(R, dy):
        Rs = np.full_like(R, np.nan)
        if dy >= 0:
            Rs[dy:] = R[:H - dy]
        else:
            Rs[:H + dy] = R[-dy:]
        dd = sum3(np.abs(F - Rs))
        bad = np.isnan(dd)
        t = tiles(np.where(bad, 1.0, (dd > 30).astype(np.float32)))
        return np.where(tiles(bad.astype(np.float32)) > 0, np.inf, t)
    chg = tiles((sum3(np.abs(E - S)) > 30).astype(np.float32)) > 0.04
    if not chg.any():
        return 0.0
    r = np.minimum(off_share(S, 0), off_share(E, 0))
    D = E - S
    al = np.clip(sum3((F - S) * D) / (sum3(D * D) + 1e-6), 0, 1)
    r = np.minimum(r, tiles((sum3(np.abs(F - S - al[..., None] * D)) > 30).astype(np.float32)))
    plain = tiles(F.mean(axis=2) ** 2) - tiles(F.mean(axis=2)) ** 2 < 9
    for dy in range(-(H // 2), H // 2 + 1, max(1, T // 8)):  # (content sliding up to half the box)
        if dy == 0:
            continue
        rr = np.minimum(off_share(S, dy), off_share(E, dy))
        if abs(dy) > max(2, T // 4):
            rr = np.where(plain, np.inf, rr)
        r = np.minimum(r, rr)
    return float(((r > 0.04) & chg).sum() / chg.sum())


def content_cuts(full, times, spans, segs, box, stp=1, neither=True):
    """a content cut the opacity trace cannot see (a tile staying opaque while its content swaps): inside `box`
    (rows, cols in the strided frames: the now-playing panel), per transition its start S (the last frame before it)
    and its end E (its last frame), the pixels that change between them (RGB distance over 30); a frame where more
    than half of them jump over 60 % of their whole change in one step is a cut. Then (now-playing transitions:
    `neither`) what the cut showed: how long it held still (the frames after it changing under 2 % of the box) and
    the longest run of frames whose tiles are neither S nor E (_tile_neither over 0.25). One dict per transition."""
    r0, r1, c0, c1 = box
    out = []
    for i, sg in enumerate(segs):
        ks = [k for k, t in enumerate(times) if sg["start"] - 1 <= t <= sg["end"] + 1]
        if not ks:
            continue
        kS = max(0, ks[0] - 1)
        kE = ks[-1]
        S = full[kS][r0:r1, c0:c1].astype(np.float32)
        E = full[kE][r0:r1, c0:c1].astype(np.float32)
        if S.size == 0 or S.shape != E.shape:
            continue
        D = sum3(np.abs(E - S))
        M = D > 30
        if M.sum() < 20:
            out.append(dict(start_ms=round(sg["start"]), max_jump=0.0, at_ms=None, cut=False, neither=None, held_ms=0))
            continue
        best, kb = 0.0, None
        for k in ks:
            if k == 0 or full[k].shape != full[k - 1].shape:
                continue
            st = sum3(np.abs(full[k][r0:r1, c0:c1].astype(np.float32) - full[k - 1][r0:r1, c0:c1]))
            j = float(((st > 0.6 * D) & M).sum() / M.sum())
            if j > best:
                best, kb = j, k
        rec = dict(start_ms=round(sg["start"]), max_jump=round(best, 2), at_ms=None if kb is None else round(times[kb]),
                   cut=best > 0.5, neither=None, held_ms=0)
        if best > 0.5 and neither:
            T = max(4, int(round(32 / stp)))
            run, longest_run, nei = 0.0, 0.0, []
            for k in range(kb, kE + 1):
                v = _tile_neither(full[k][r0:r1, c0:c1].astype(np.float32), S, E, T)
                nei.append(v)
                if v >= 0.25:
                    run += spans[k][1] - spans[k][0]
                    longest_run = max(longest_run, run)
                else:
                    run = 0.0
            rec["neither"] = round(nei[0], 2)
            rec["neither_ms"] = round(longest_run)
            k = kb
            while k + 1 < len(full) and full[k + 1].shape == full[k].shape and float(
                    (sum3(np.abs(full[k + 1][r0:r1, c0:c1].astype(np.float32) - full[k][r0:r1, c0:c1])) > 30).mean()) < 0.02:
                k += 1
            rec["held_ms"] = round(spans[k][1] - times[kb])
        out.append(rec)
    return out


def handover(tracks, anims, window=None):
    """the visual handover apart from the recording's pixel tail (playback content, tooltips): the QML
    animations that ran after the action (their span and the longest), and when the items' opacities last changed
    (tooltips and other popups left out; places are left out too: a playhead moves with playback)."""
    is_overlay = lambda k, tr: "ToolTip" in k or any(str(l).startswith("popup") for l in tr.get("layers", []))
    lo, hi = window or (-FRAME_MS, float("inf"))  # (one transition's window: from its start to the next one's)
    an = [a_ for a_ in anims if a_["start"] is not None and lo <= a_["start"] < hi and "ToolTip" not in a_["target"]]
    span = None
    longest_a = None
    if an:
        ends = [a_["end"] if a_["end"] is not None else a_["start"] + (a_["duration"] or 0) for a_ in an]
        span = (round(min(a_["start"] for a_ in an)), round(max(ends)))
        longest_a = max(an, key=lambda a_: a_["duration"] or 0)
    last_t, last_k = None, None
    for k, v in tracks.items():
        if is_overlay(k, v):
            continue
        pts = v["pts"]
        for (ta, va), (tb, vb) in zip(pts, pts[1:]):
            if max(0, lo) < tb < hi and abs(vb - va) > 0.01 and (last_t is None or tb > last_t):
                last_t, last_k = tb, k
    return dict(anim_span=span,
                anim_longest=(f"{longest_a['target']}.{longest_a['prop']} {longest_a['frm']}->{longest_a['to']} "
                              f"{longest_a['duration']} ms {longest_a['easing']}") if longest_a else None,
                anims_n=len(an), settle_ms=None if last_t is None else round(last_t),
                settle_item=None if last_k is None else last_k.split("/")[-1])


def analyse(frames, ims, t0, region, shown_idx, meta, d, zoom=1, t_stop=None, focus=None, nearest=False,
            periodic=False, elements=None):
    """measure(), drawn at the size W and MAX_H give. Returns (metrics, caption, merged image)."""
    metrics, cap, draw = measure(frames, ims, t0, region, shown_idx, meta, d, zoom=zoom, t_stop=t_stop, focus=focus,
                                 nearest=nearest, periodic=periodic, elements=elements)
    return metrics, cap, draw(W, MAX_H)


def measure(frames, ims, t0, region, shown_idx, meta, d, zoom=1, t_stop=None, focus=None, nearest=False,
            periodic=False, elements=None):
    """frames: [{t,...}] (ms, absolute); ims: PIL images (the recording); t0: action time; region: (x, y, w, h)
    of the strip crop in those images; shown_idx: the key frames; zoom: their magnification; focus: an optional
    (x, y, w, h) for the perceived rows (small controls). Returns (metrics, caption, draw): draw(width, max_h) is
    the merged image at that size (a pair's halves are narrower), with nothing measured again."""
    x, y, w, h = region
    scale = float(meta.get("scale", 1) or 1)
    stp = max(1, int((w * h / 150000) ** 0.5))  # metrics on a strided sample (at most ~150k pixels a frame)
    full = [np.asarray(im.crop((x, y, x + w, y + h)), dtype=np.int16)[::stp, ::stp] for im in ims]
    n = len(full)
    times = [f["t"] - t0 for f in frames]
    end_rec = (t_stop - t0) if t_stop else times[-1] + FRAME_MS  # frames render only on change: stop = the end
    spans = [(times[k], times[k + 1] if k + 1 < n else end_rec) for k in range(n)]
    dmax, dmean = [0.0], [0.0]
    for k in range(1, n):
        dd = max3(np.abs(full[k] - full[k - 1])) if full[k].shape == full[0].shape else np.full(full[0].shape[:2], 255)
        dmax.append(float(dd.max()))
        dmean.append(float(dd.mean()))
    changed = [k > 0 and dmax[k] > 2 for k in range(n)]
    after = [k for k in range(n) if times[k] >= -1 and changed[k]]
    # Content that keeps moving (playback): changed frames still arriving when the recording stopped. Then the
    # transition is the frames that change clearly more than that steady motion (its median over the last frames).
    continuous = bool(after) and after[-1] >= n - 2 and (end_rec - times[after[-1]]) < 80
    trans = after
    if continuous:
        base = float(np.median([dmean[k] for k in after[-15:]]))
        trans = [k for k in after if dmean[k] > 2.5 * base + 0.05] or after
    segs = segments_of(trans, times)
    # Periodic single-frame updates (the playing position, its time text) are not transitions: in a strip whose
    # crop holds them, 3+ single-frame changes (or 2 at a steady interval) have their pixels masked, then the
    # transitions are detected again. Elsewhere single-frame changes are real steps (injected states, snaps).
    tick_note = ""
    if periodic:  # only where the crop holds the playing position (states.PERIODIC_UPDATES)
        grown_all = np.zeros(full[0].shape[:2], dtype=bool)
        seen_ticks = 0
        for _ in range(3):  # again while new ticks appear: one merged into a longer run shows once it is masked
            ticks = [s for s in segs if len(s["frames"]) == 1]
            if len(ticks) < 2:
                break
            iv = np.diff([s["start"] for s in ticks])
            if len(ticks) < 3 and float(np.std(iv)) > 0.35 * float(np.mean(iv)):
                break
            tmask = np.zeros(full[0].shape[:2], dtype=bool)
            for s in ticks:
                k = s["a"]
                if k > 0 and full[k].shape == full[0].shape:
                    tmask |= max3(np.abs(full[k] - full[k - 1])) > 2
            if not tmask.any():
                break
            seen_ticks += len(ticks)
            for dy in (-2, -1, 0, 1, 2):  # grown: the playhead moves a pixel at a time
                for dx in (-2, -1, 0, 1, 2):
                    grown_all |= np.roll(np.roll(tmask, dy, axis=0), dx, axis=1)
            keep = ~grown_all
            trans = [k for k in trans if k > 0 and full[k].shape == full[0].shape
                     and float((max3(np.abs(full[k] - full[k - 1]))[keep]).max(initial=0)) > 2]
            segs = segments_of(trans, times)
        if seen_ticks:
            tick_note = (f"Periodic single-frame updates (e.g. the playing position; {seen_ticks} seen) are left "
                         "out of the transitions.")
    # The main transition (perceived rows, graph): the longest, or the one states.MOTION_FOCUS names.
    fo = focus or {}
    main = (segs[-1] if fo.get("segment") == "last" else max(segs, key=lambda s: s["dur"])) if segs else None
    focus = resolve_rect(fo.get("rect"), meta, ims[0].width, ims[0].height)
    pa_max_h = int(fo.get("max_h", 200))
    b_lines = int(fo.get("b_lines", 2))
    kf_band = int(fo.get("band", 480))
    latency = segs[0]["start"] if segs else 0.0
    # The change box: where anything changes; the scopes split it into text and the rest.
    dif = np.zeros(full[0].shape[:2], dtype=bool)
    for k in trans:
        if full[k].shape == full[0].shape:
            dif |= max3(np.abs(full[k] - full[k - 1])) > 2
    ys, xs = np.nonzero(dif)
    if len(xs):
        box = (max(0, xs.min() - 4), max(0, ys.min() - 4), min(dif.shape[1], xs.max() + 5), min(dif.shape[0], ys.max() + 5))
    else:
        box = (0, 0, dif.shape[1], dif.shape[0])
    arrs = [a[box[1]:box[3], box[0]:box[2]].astype(np.float32) for a in full]
    tracks, anims = read_trace(d, t0)
    off = tuple(meta.get("crop") or (0, 0))[:2]  # the recording is this crop of the window
    tmask, omask, n_text = scope_masks(tracks, region, box, stp, arrs[0].shape[:2], scale, off)
    chg = dif[box[1]:box[3], box[0]:box[2]]
    scopes = {}
    for nm, mk in (("text", tmask), ("other", omask)):
        if (mk & chg).sum() >= 12:  # the scope changes at all
            ink = ink_series(arrs, mk)
            if ink:
                scopes[nm] = dict(ink=ink, share=float(mk.mean()), changed_share=float((mk & chg).sum() / max(1, chg.sum())))
    lum = [float((a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)).mean()) / 255.0 for a in arrs]
    for sc in scopes.values():
        sc["blink"], sc["bmask"] = blink_of(sc["ink"], spans, segs)
    item_bl = item_blinks(tracks, times, spans, ims, scale, off)
    cross = None
    if fo.get("lanes"):  # (recorded at 1x: the dash's period is in the recording's pixels)
        pane = resolve_rect(fo["lanes"], meta, ims[0].width, ims[0].height)
        in_window = (pane[0] + off[0], pane[1] + off[1], pane[2], pane[3])
        cross = crosshair_lanes(ims, times, pane, traced_lanes(tracks, in_window), off)
    overlap = text_overlap(tracks, segs)
    # the overlap per transition, and per now-playing line (states.ELEMENT_INK: only that line's text items, e.g.
    # the outgoing and incoming titles)
    overlap_by_seg = [round(text_overlap(tracks, [sg])) for sg in segs]
    overlap_by_el = {lb: round(text_overlap({k: v for k, v in tracks.items()
                                             if any(fnmatch.fnmatch(seg_, pat) for seg_ in k.split("/")[-2:])}, segs))
                     for lb, pat in (elements or []) if not pat.startswith("any:")}
    # the overlap as the peak opacity two texts in one place show at once, overall and per line
    peak_all = text_peak(tracks, segs)
    peak_by_el = {lb: text_peak(tracks, segs, {k for k in tracks if any(fnmatch.fnmatch(seg_, pat) for seg_ in k.split("/")[-2:])})
                  for lb, pat in (elements or []) if not pat.startswith("any:")}
    hand = handover(tracks, anims)
    # content cuts inside the now-playing panel (its rect at the start and at the end), where it is in the crop
    cuts = None
    pr0 = (meta.get("items") or {}).get("nowPlayingPanel")
    if pr0:
        pk_ = [k_ for k_ in tracks if k_.split("/")[-1] == "nowPlayingPanel"]
        pr1 = tracks[pk_[0]]["fulls"][-1] if pk_ and tracks[pk_[0]].get("fulls") else pr0
        cx_, cy_ = tuple(meta.get("crop") or (0, 0))[:2]
        bx0, by0 = min(pr0[0], pr1[0]) - cx_ - region[0], min(pr0[1], pr1[1]) - cy_ - region[1]
        bx1 = max(pr0[0] + pr0[2], pr1[0] + pr1[2]) - cx_ - region[0]
        by1 = max(pr0[1] + pr0[3], pr1[1] + pr1[3]) - cy_ - region[1]
        hh, ww = full[0].shape[:2]
        box_ = (max(0, int(by0 // stp)), min(hh, int(by1 // stp)), max(0, int(bx0 // stp)), min(ww, int(bx1 // stp)))
        if box_[1] - box_[0] > 4 and box_[3] - box_[2] > 4:
            # (the jump test everywhere the panel is in the crop; what a cut showed only for the now-playing
            # transitions, the recordings with now-playing elements: it is the costly part)
            cuts = content_cuts(full, times, spans, segs, box_, stp, neither=bool(elements))
    def movers(win):
        """What moved in a window of the recording without an animation the trace names: items whose rect (by
        the item trace) changed by over 2 px, the largest first: (name, dx, dy, dw, first, last) in ms."""
        a_, b_ = win
        out_ = []
        for k, tr in tracks.items():
            pts = [(t, r) for (t, _v), r in zip(tr["pts"], tr["rects"]) if a_ <= t <= b_]
            if len(pts) < 1:
                continue
            before = [r for (t, _v), r in zip(tr["pts"], tr["rects"]) if t < a_]
            r0 = before[-1] if before else tr["rects"][0]
            r1 = pts[-1][1]
            dx, dy, dw = r1[0] - r0[0], r1[1] - r0[1], r1[2] - r0[2]
            if max(abs(dx), abs(dy), abs(dw)) > 2:
                ch = [t for t, r in pts if r != r0]
                out_.append((k.split("/")[-1], dx, dy, dw, round(min(ch)) if ch else None, round(pts[-1][0])))
        out_.sort(key=lambda m_: -max(abs(m_[1]), abs(m_[2]), abs(m_[3])))
        return out_[:3]

    hand_by_seg = ([dict(handover(tracks, anims, (-FRAME_MS if i == 0 else sg["start"] - FRAME_MS,
                                                   segs[i + 1]["start"] - FRAME_MS if i + 1 < len(segs)
                                                    else float("inf"))), start_ms=round(sg["start"]))
                    for i, sg in enumerate(segs)] if len(segs) > 1 else [])
    for i, g in enumerate(hand_by_seg):
        g["movers"] = movers((-FRAME_MS if i == 0 else segs[i]["start"] - FRAME_MS,
                              segs[i + 1]["start"] - FRAME_MS if i + 1 < len(segs) else float("inf")))
    hand["movers"] = movers((-FRAME_MS, float("inf")))
    # a line whose pattern matches no item in the trace would read "0 ms" like a clean one: such lines are named
    lines_absent = [lb for lb, pat in (elements or []) if not pat.startswith("any:") and not any(
        any(fnmatch.fnmatch(seg_, pat) for seg_ in k.split("/")[-2:]) for k in tracks)]
    # Movement after the stated end, per transition: the animations that started in it (start + duration).
    # What still changes then is named: the items whose opacity, place or layer changed after that end (item
    # tree), else the box of the pixels that changed (a repaint no item property shows).
    moves_after, moves_what = None, ""
    # the tail split by who moves: a tooltip or other popup (overlay: its layer is a popup) vs the app's own
    # items (and pixels no item explains): {"overlay": (ms, names), "app": (ms, names)}
    tail_split = {}
    is_overlay = lambda k, tr: "ToolTip" in k or any(str(l).startswith("popup") for l in tr.get("layers", []))
    off = tuple(meta.get("crop") or (0, 0))[:2]
    for s in segs:
        ends = [a["start"] + a["duration"] for a in anims
                if a["duration"] is not None and a["start"] is not None and s["start"] - 60 <= a["start"] <= s["end"]]
        if ends:
            stated = max(ends)
            mv = max(0.0, (s["end"] - FRAME_MS) - stated)
            if moves_after is None or mv > moves_after:
                moves_after, moves_what = mv, ""
                tail_split = {}
                if mv > 0:
                    late = []  # (its opacity changed, last change, name): fades first, then items that only moved
                    for k, tr in tracks.items():
                        idx = [i for i, (t, _) in enumerate(tr["pts"]) if stated + 2 < t <= s["end"] + 5]
                        if idx:
                            faded = any(i > 0 and abs(tr["pts"][i][1] - tr["pts"][i - 1][1]) > 0.002 for i in idx)
                            late.append((faded, tr["pts"][idx[-1]][0], short_name(k)))
                            side = "overlay" if is_overlay(k, tr) else "app"
                            ms_, nms_ = tail_split.get(side, (0.0, []))
                            tail_split[side] = (max(ms_, tr["pts"][idx[-1]][0] - stated), nms_ + [short_name(k)])
                    if "app" not in tail_split and "overlay" not in tail_split:
                        tail_split["app"] = (mv, ["pixels no item explains"])
                    elif "app" not in tail_split:  # all named changes are popups: the rest of the tail is theirs
                        tail_split["overlay"] = (max(tail_split["overlay"][0], mv), tail_split["overlay"][1])
                    late.sort(reverse=True)
                    names = list(dict.fromkeys(nm + ("" if fd else " (moved)") for fd, _, nm in late))
                    if names:
                        moves_what = ", ".join(names[:3]) + (f" and {len(names) - 3} more" if len(names) > 3 else "")
                    else:
                        md = np.zeros(full[0].shape[:2], dtype=bool)
                        for k in s["frames"]:
                            if times[k] > stated and k > 0 and full[k].shape == full[0].shape:
                                md |= max3(np.abs(full[k] - full[k - 1])) > 2
                        yy_, xx_ = np.nonzero(md)
                        if len(xx_):
                            bx = (int(xx_.min() * stp + x + off[0]), int(yy_.min() * stp + y + off[1]),
                                  int((xx_.max() - xx_.min() + 1) * stp), int((yy_.max() - yy_.min() + 1) * stp))
                            moves_what = (f"pixels at {bx[0]},{bx[1]} {bx[2]}×{bx[3]} (window coordinates), no item's "
                                          "opacity, place or layer")
                            # inside the plot pane: the plot's own drawing (its data arriving), not an item's
                            pr_m = (meta.get("items") or {}).get("spectrogramPane")
                            if pr_m and bx[0] >= pr_m[0] - 2 and bx[1] >= pr_m[1] - 2 and \
                                    bx[0] + bx[2] <= pr_m[0] + pr_m[2] + 2 and bx[1] + bx[3] <= pr_m[1] + pr_m[3] + 2:
                                moves_what = (f"the plot's own drawing inside the pane ({bx[2]}×{bx[3]} px at {bx[0]},{bx[1]}): "
                                              "its data being drawn, no item's opacity, place or layer")
    # What changed in each transition ("say what the second step is"): the items whose opacity changed in it
    # (item tree, every frame) and whose rect meets its changed pixels, popups said; else the pixels' box.
    def seg_what(sg):
        md = np.zeros(full[0].shape[:2], dtype=bool)
        for k in sg["frames"]:
            if k > 0 and full[k].shape == full[0].shape:
                md |= max3(np.abs(full[k] - full[k - 1])) > 2
        yy_, xx_ = np.nonzero(md)
        if not len(xx_):
            return ""
        bx0, by0 = xx_.min() * stp + x + off[0], yy_.min() * stp + y + off[1]
        bx1, by1 = (xx_.max() + 1) * stp + x + off[0], (yy_.max() + 1) * stp + y + off[1]
        found = []
        for k, tr in tracks.items():
            idx = [i for i, (t, _) in enumerate(tr["pts"]) if sg["start"] - FRAME_MS - 2 <= t <= sg["end"] + FRAME_MS]
            if not idx:
                continue
            vals = ([tr["pts"][idx[0] - 1][1]] if idx[0] > 0 else []) + [tr["pts"][i][1] for i in idx]
            if max(vals) - min(vals) < 0.05:
                continue
            rx, ry, rw, rh = [v * scale for v in tr["rects"][idx[-1]]]
            if rx >= bx1 or ry >= by1 or rx + rw <= bx0 or ry + rh <= by0:
                continue
            how = ("fades in" if vals[-1] > vals[0] + 0.05 else "fades out" if vals[-1] < vals[0] - 0.05 else "dips and returns")
            pop = str(tr["layers"][idx[-1]]).startswith("popup")
            inter = (min(rx + rw, bx1) - max(rx, bx0)) * (min(ry + rh, by1) - max(ry, by0))
            found.append((-inter, rw * rh, f"{short_name(k)}{' (a popup)' if pop else ''} {how}"))
        found.sort()  # the most of the changed area first, what fades in and what fades out taken in turn
        ins = [f for _, _, f in found if f.endswith("fades in")]
        rest = [f for _, _, f in found if not f.endswith("fades in")]
        names = list(dict.fromkeys(v for pair in itertools.zip_longest(ins, rest) for v in pair if v))
        where = f"at {int(bx0 / scale)},{int(by0 / scale)} {int((bx1 - bx0) / scale)}×{int((by1 - by0) / scale)} (window coordinates)"
        if names:
            return ", ".join(names[:3]) + (f" and {len(names) - 3} more" if len(names) > 3 else "") + f"; pixels {where}"
        return f"pixels {where}, no item's opacity changing (content redrawn: a plot, an image, a moved item)"
    seg_whats = [seg_what(sg) for sg in segs]
    # Per-element ink (states.ELEMENT_INK, e.g. the now-playing title, artist line and details): each element's
    # own rect, full resolution, so a local dim is not averaged away by the rest of the region.
    elems = []
    for label, pat in (elements or []):
        any_ = pat.startswith("any:")  # "any:PATTERN": any item (the idle row, the cover), matched on the whole key
        rs = {tuple(r) for k, tr in tracks.items()
              if (fnmatch.fnmatch(k, pat[4:]) if any_ else (tr["text"] and fnmatch.fnmatch(k.split("/")[-1], pat)))
              for r in tr["rects"]}
        if not rs:
            continue
        ex0 = int(min(r[0] for r in rs) * scale - off[0]); ey0 = int(min(r[1] for r in rs) * scale - off[1])
        ex1 = int(max(r[0] + r[2] for r in rs) * scale - off[0]); ey1 = int(max(r[1] + r[3] for r in rs) * scale - off[1])
        iw, ih = ims[0].size
        ex0, ey0, ex1, ey1 = max(0, ex0), max(0, ey0), min(iw, ex1), min(ih, ey1)
        if ex1 - ex0 < 4 or ey1 - ey0 < 4:
            continue
        ea = [np.asarray(im.crop((ex0, ey0, ex1, ey1)), dtype=np.float32) for im in ims]
        msk = np.ones(ea[0].shape[:2], dtype=bool)
        ink_e = ink_series(ea, msk)
        bl, bm = blink_of(ink_e, spans, segs)
        elems.append(dict(label=label, ink=ink_e, blink=bl, bmask=bm, box=[ex0 + off[0], ey0 + off[1], ex1 - ex0, ey1 - ey0]))
    # A plot's drawn content ( MOTION_FOCUS "plot": a rect spec): per frame, the share of the lane's pixels
    # that differ from its own floor (its most frequent colour). A plot that goes empty while its layer stays
    # opaque (as the mode switch once did) is a blink no opacity or ink measure sees: the frames after the action
    # under half the smaller of the content before and at the end. And the frames that are neither the start nor
    # the end nor a blend of them: each fitted as start + a(end - start); residual over |end - start|.
    plot_note, neither_ms = "", None
    if fo.get("plot"):
        pr_ = resolve_rect(fo["plot"], meta, ims[0].width, ims[0].height)
        pa_ = [np.asarray(im.crop((pr_[0], pr_[1], pr_[0] + pr_[2], pr_[1] + pr_[3]))).astype(np.int16) for im in ims]
        content = []
        for a_ in pa_:
            v_, c_ = unique_colours(a_)
            content.append(float((sum3(np.abs(a_ - v_[c_.argmax()])) > 30).mean()))
        pre = [k for k in range(n) if times[k] < 0]
        k0 = pre[-1] if pre else 0
        lo = 0.5 * min(content[k0], content[-1])
        pmask, best, run = [False] * n, 0.0, 0.0
        for k in range(k0 + 1, n):
            if lo > 0.02 and content[k] < lo:
                pmask[k] = True
                run += spans[k][1] - spans[k][0]
                best = max(best, run)
            else:
                run = 0.0
        kmin = min(range(k0, n), key=lambda k: content[k])
        elems.append(dict(label="the plot's drawn content", ink=content, blink=best, bmask=pmask,
                          box=[pr_[0] + off[0], pr_[1] + off[1], pr_[2], pr_[3]]))
        g_ = [np.asarray(im.crop((pr_[0], pr_[1], pr_[0] + pr_[2], pr_[1] + pr_[3])).convert("L"), dtype=np.float32)[::2, ::2]
              for im in ims]
        S_, E_ = g_[k0], g_[-1]
        D_ = E_ - S_
        dd_ = float((D_ * D_).sum())
        neither_ms = 0.0
        neither_frames, plot_latency = [], None
        if dd_ > 0:
            def fit_res(G, S0, E0):
                D0 = E0 - S0
                dd0 = float((D0 * D0).sum()) or 1.0
                al0 = float(np.clip(((G - S0) * D0).sum() / dd0, 0, 1))
                return al0, float(np.sqrt(((G - S0 - al0 * D0) ** 2).sum() / dd0))
            Wg = S_.shape[1]
            for k in range(k0 + 1, n):
                al, res_ = fit_res(g_[k], S_, E_)
                # a plot that keeps scrolling with playback (the centered view moves): also fitted against the
                # end frame moved sideways by up to 24 columns of these half-size frames (48 px), on the overlap
                if res_ > 0.18:
                    for dx in range(-24, 25):
                        if dx == 0:
                            continue
                        a0, a1 = (dx, Wg) if dx > 0 else (0, Wg + dx)
                        b0, b1 = (0, Wg - dx) if dx > 0 else (-dx, Wg)
                        al_s, res_s = fit_res(g_[k][:, a0:a1], S_[:, a0:a1], E_[:, b0:b1])
                        if res_s < res_:
                            al, res_ = al_s, res_s
                if res_ > 0.18:
                    neither_ms += spans[k][1] - spans[k][0]
                    neither_frames.append(dict(i=frames[k]["i"], file=frames[k]["file"], t=round(times[k], 1), blend=round(al, 2),
                                               residual=round(res_, 2)))
                # the action to the plot's first change (more than 5 % of the way from its start to its end)
                if plot_latency is None and float(np.sqrt(((g_[k] - S_) ** 2).sum() / dd_)) > 0.05 and times[k] >= 0:
                    plot_latency = times[k]
        samples_ = []
        for T_ in range(0, int(times[-1]) + 1, 200):
            k_ = max([k for k in range(n) if times[k] <= T_] or [0])
            samples_.append(f"+{T_}: {100 * content[k_]:.0f} %")
        plot_note = ((f"Action to the plot's first change: {fms(plot_latency)}. " if plot_latency is not None else
                      "The plot does not change. ") +
                     f"Plot content (share of the lane drawn, {pr_[2]}x{pr_[3]} px): {100 * content[k0]:.0f} % before, "
                     f"{100 * content[-1]:.0f} % at the end, lowest {100 * content[kmin]:.0f} % at +{fms(max(0.0, times[kmin]))}; "
                     f"empty (under half the smaller) for {fms(best)}. Frames that are neither the start nor the end nor "
                     f"a blend of them: {fms(neither_ms)}. Drawn share every 200 ms: {', '.join(samples_)}.")
    # how far an item travels in the recording (MOTION_FOCUS "travel": label, item pattern), from its traced
    # rects: the art changes' library header should not move at all since the stand-in cover keeps the slot
    travel_px, travel_note = None, ""
    if fo.get("travel"):
        lab_t, pat_t = fo["travel"]
        ks_t = sorted((k for k in tracks if fnmatch.fnmatch(k, pat_t)), key=lambda k: ("#" in k, len(k)))
        if not ks_t:
            raise LookupError(f"{meta.get('name')}: travel: no item {pat_t} in the trace")
        # the largest move of any matching item (each row title's own rects)
        k_t = max(ks_t, key=lambda k: max(r[1] for r in tracks[k]["rects"]) - min(r[1] for r in tracks[k]["rects"]))
        ys_t = [r[1] for r in tracks[k_t]["rects"]]
        travel_px = max(ys_t) - min(ys_t)
        travel_note = (f"Travel of {lab_t}: {travel_px:.0f} px" + (f" (its top from y {ys_t[0]:.0f} to {ys_t[-1]:.0f}, "
                       f"{min(ys_t):.0f} to {max(ys_t):.0f} on the way)" if travel_px else " (it does not move)") + ".")
    shown_t = [times[k] for k in shown_idx]
    gaps = [b - a for a, b in zip(shown_t, shown_t[1:]) if b > a]
    spacing = sum(gaps) / len(gaps) if gaps else 0.0
    span_shown = (shown_t[-1] - shown_t[0]) if len(shown_t) > 1 else max(main["dur"] if main else FRAME_MS, FRAME_MS)
    slow = (len(shown_idx) * GLANCE_S * 1000.0) / max(span_shown, FRAME_MS)
    rec_ms = end_rec - times[0]
    blink_t = scopes.get("text", {}).get("blink")
    blink_o = scopes.get("other", {}).get("blink")
    metrics = dict(latency_ms=round(latency), duration_ms=round(main["dur"]) if main else 0,
                   frames_60=round(main["dur"] / FRAME_MS) if main else 0,
                   segments=[dict(start_ms=round(s["start"]), duration_ms=round(s["dur"]), what=w_) for s, w_ in zip(segs, seg_whats)],
                   recording_ms=round(rec_ms), recording_after_action_ms=round(end_rec), recorded_frames=n,
                   continuous=continuous, blink_text_ms=None if blink_t is None else round(blink_t),
                   blink_other_ms=None if blink_o is None else round(blink_o),
                   blink_ms=round(max([blink_t or 0, blink_o or 0] + [b["ms"] for b in item_bl])),
                   blink_items=[dict(label=b["label"], items=b["items"], start_ms=round(b["start"]), ms=round(b["ms"]),
                                     low=round(b["low"], 2), box=b["box"]) for b in item_bl],
                   overlap_ms=round(overlap),
                   overlap_by_transition=overlap_by_seg, overlap_by_line=overlap_by_el, lines_absent=lines_absent,
                   overlap_peak=peak_all, overlap_peak_by_line=peak_by_el, handover=hand, handover_by_transition=hand_by_seg, content_cuts=cuts,
                   text_items=n_text, scopes={k: dict(share=round(v["share"], 3), changed_share=round(v["changed_share"], 3))
                                              for k, v in scopes.items()},
                   moves_after_ms=None if moves_after is None else round(moves_after), moves_after_what=moves_what,
                   after_end_app_ms=round(tail_split["app"][0]) if "app" in tail_split else (None if moves_after is None else 0),
                   after_end_app_what=(moves_what if "app" in tail_split and tail_split["app"][1] == ["pixels no item explains"]
                                       and moves_what else ", ".join(dict.fromkeys(tail_split["app"][1][:3])) if "app" in tail_split else ""),
                   after_end_overlay_ms=round(tail_split["overlay"][0]) if "overlay" in tail_split else (None if moves_after is None else 0),
                   after_end_overlay_what=", ".join(dict.fromkeys(tail_split["overlay"][1][:3])) if "overlay" in tail_split else "",
                   elements=[dict(label=e["label"], blink_ms=round(e["blink"]), box=e["box"]) for e in elems],
                   shown=len(shown_idx),
                   spacing_ms=round(spacing), slow_factor=round(slow),
                   animations=[dict(a, start=round(a["start"]), end=None if a["end"] is None else round(a["end"])) for a in anims])
    metrics["periodic_note"] = tick_note
    if cross is not None:
        metrics["crosshair"] = cross
    if travel_px is not None:
        metrics["travel_px"] = round(travel_px)
        metrics["plot_note"] = travel_note
    if fo.get("plot"):
        metrics["blink_plot_ms"] = round(elems[-1]["blink"])
        metrics["neither_ms"] = round(neither_ms or 0)
        metrics["neither_frames"] = neither_frames
        metrics["plot_rect"] = [int(v) for v in pr_]
        metrics["plot_latency_ms"] = None if plot_latency is None else round(plot_latency)
        metrics["blink_ms"] = max(metrics["blink_ms"], metrics["blink_plot_ms"])
        metrics["plot_note"] = (metrics.get("plot_note", "") + " " + plot_note).strip()
    cap = caption(metrics, segs, continuous)
    def draw(width, max_h):
        global W, MAX_H
        W, MAX_H = width, max_h
        return merged(frames, ims, times, region, shown_idx, zoom, nearest, segs, main, scopes, lum, tracks, anims, spans,
                      metrics, meta, cap, dmean, focus, (box, stp, dif), full,
                      row_segs=[sg for sg in segs if len(sg["frames"]) > 1][:3] if fo.get("rows") == "all" and segs else None, elements=elems, pa_max_h=pa_max_h,
                      b_lines=b_lines, kf_band=kf_band, extra_focus=[(lb, resolve_rect(rc, meta, ims[0].width, ims[0].height)) for lb, rc in (fo.get("extra") or [])], zoom_graphs=fo.get("zoom_graphs"),
                      max_frames=int(fo.get("max_frames", 12)), pursuit=fo.get("pursuit"))
    return metrics, cap, draw


def tail_text(m):
    """the after-end tail, the app's own items apart from tooltips and other popups."""
    a, o = m.get("after_end_app_ms"), m.get("after_end_overlay_ms")
    if a is None and o is None:
        return f" ({m.get('moves_after_what') or 'unnamed'})"
    parts = []
    if a:
        parts.append(f"the app's items {a} ms: {m.get('after_end_app_what') or 'unnamed'}")
    if o:
        parts.append(f"tooltip/popup {o} ms: {m.get('after_end_overlay_what') or 'unnamed'}")
    return " (" + ("; ".join(parts) or m.get("moves_after_what") or "unnamed") + ")"


def short_name(key):
    """An item's name for a legend: its own, with its parent's when it is only a type (Item, LineText)."""
    parts = key.split("/")
    return "/".join(parts[-2:]) if len(parts) > 1 and parts[-1][:1].isupper() else parts[-1]


def fms(v):
    return f"{v:.0f} ms" if v < 1000 else f"{v / 1000:.2f} s"


def caption(m, segs, continuous):
    """Latency (action to first change), each transition (first to last change) and the recording, kept apart."""
    parts = [crosshair_text(m["crosshair"]) + "."] if m.get("crosshair") else []
    if not segs:
        parts.append("No change after the action.")
    else:
        parts.append(f"Latency (input delivered to the app, to the first change): {m['latency_ms']} ms.")
        whats = [sm.get("what", "") for sm in m.get("segments", [])] + [""] * len(segs)
        if len(segs) == 1:
            s = segs[0]
            parts.append(f"Transition (first to last change): {fms(s['dur'])} ({s['dur'] / FRAME_MS:.0f} frames at 60 fps)"
                         + (f"; what changed: {whats[0]}." if whats[0] else "."))
        else:
            seq = "; ".join(f"{i}) at +{fms(s['start'])}, {fms(s['dur'])} ({s['dur'] / FRAME_MS:.0f} frames)"
                            + (f": {whats[i - 1]}" if whats[i - 1] else "") for i, s in enumerate(segs, 1))
            holds = ", ".join(fms(b["start"] - a["end"]) for a, b in zip(segs, segs[1:]))
            parts.append(f"{len(segs)} transitions: {seq}; unchanged between them for {holds} (a hold or a wait, not part of any transition).")
    if continuous:
        parts.append("The content keeps moving (playback, a running indicator): transitions are the frames that change "
                     "clearly more than that motion.")
    if m.get("periodic_note"):
        parts.append(m["periodic_note"])
    parts.append(f"Recording: {fms(m['recording_ms'])}, {m['recorded_frames']} frames rendered.")
    sc = m.get("scopes", {})
    if sc:
        parts.append("Scopes: " + "; ".join(
            (f"text = {m['text_items']} text items, {100 * v['share']:.0f} % of the change box"
             if k == "text" else f"other = images, icons and plots, {100 * v['share']:.0f} %")
            + f", {100 * v['changed_share']:.0f} % of the changed pixels" for k, v in sc.items()) + ".")
    bl = []
    if m["blink_text_ms"] is not None:
        bl.append(f"text: {m['blink_text_ms']} ms")
    if m["blink_other_ms"] is not None:
        bl.append(f"other content: {m['blink_other_ms']} ms")
    parts.append(("Blink — " + "; ".join(bl) + "; " if bl else "") + f"text overlap {m['overlap_ms']} ms"
                 + (f"; moves {m['moves_after_ms']} ms after the stated end" + tail_text(m) if m["moves_after_ms"] else "") + ".")
    if m.get("blink_items"):
        worst = sorted(m["blink_items"], key=lambda b: -b["ms"])
        parts.append("Blink per changed item (its traced opacity, seen in its own rect): "
                     + "; ".join(item_blink_text(b) for b in worst[:2])
                     + (f"; {len(worst) - 2} more" if len(worst) > 2 else "") + ".")
    if m.get("plot_note"):
        parts.append(m["plot_note"])
    if m.get("elements"):
        parts.append("Blink per element (its own rect): " + "; ".join(f"{e['label']} {e['blink_ms']} ms" for e in m["elements"]) + ".")
    parts.append(f"Key frames {m['spacing_ms']} ms apart on average, about {m['slow_factor']}× slower than real time "
                 f"(at ~{GLANCE_S:.1f} s per frame viewed).")
    return " ".join(parts)


def perceived(ims, times, spans, rect, T):
    """Time-weighted mean of the frames on screen in [T - WINDOW_MS, T]."""
    x, y, w, h = rect
    acc = np.zeros((h, w, 3), dtype=np.float64)
    tot = 0.0
    for k, (a, b) in enumerate(spans):
        o = min(b, T) - max(a, T - WINDOW_MS)
        if o > 0:
            acc += o * np.asarray(ims[k].crop((x, y, x + w, y + h)), dtype=np.float64)
            tot += o
    if tot == 0:
        k = max([i for i, t in enumerate(times) if t <= T] or [0])
        return ims[k].crop((x, y, x + w, y + h))
    return Image.fromarray((acc / tot).clip(0, 255).astype(np.uint8))


def perceived_pursuit(ims, times, spans, rect, T, pos):
    """Label-slide ghosting (a SIMULATION): what an eye that follows a sliding label smoothly sees. Over the
    WINDOW_MS before T, at 1 ms steps, the frame on screen then, shifted by how far the eye has moved since (the
    label's path, interpolated through its traced positions); averaged. Even frame steps give a sharp label; uneven
    steps or a jump give doubled or smeared edges."""
    x, y, w, h = rect
    acc = np.zeros((h, w, 3), dtype=np.float64)
    n = 0
    px, py = pos(T)
    for tau in np.arange(T - WINDOW_MS, T, 1.0):
        k = next((i for i, (a, b) in enumerate(spans) if a <= tau < b), None)
        if k is None:
            continue
        qx, qy = pos(tau)
        dx, dy = int(round(qx - px)), int(round(qy - py))
        acc += np.asarray(ims[k].crop((x + dx, y + dy, x + dx + w, y + dy + h)), dtype=np.float64)
        n += 1
    if not n:
        return ims[0].crop((x, y, x + w, y + h))
    return Image.fromarray((acc / n).clip(0, 255).astype(np.uint8))


def wrap(d, text, fnt, width):
    out, cur = [], ""
    for word in text.split(" "):
        t = (cur + " " + word).strip()
        if d.textlength(t, font=fnt) <= width:
            cur = t
        else:
            out.append(cur)
            cur = word
    if cur:
        out.append(cur)
    return out


def focus_rect(region, main, dif_info, focus, ims, full, tile_w, tile_h, tracks=None, scale=1.0, off=(0, 0)):
    """The perceived rows' crop: `focus` if given (image coordinates); else what animates in the main transition
    (the items whose opacity changes then, from the item tree) or, without any, its pixel change box; grown to
    some context (at least 160×80 px) and, when too large to show at 0.75× in a tile, a tile-shaped window of it
    centred on the animated items or on where the pixels change most."""
    if focus:
        return [int(v) for v in focus]
    x, y, w, h = region
    box, stp, dif = dif_info
    iw, ih = ims[0].size
    if main and tracks:
        cap_w, cap_h = tile_w / MIN_FOCUS_SCALE, tile_h / MIN_FOCUS_SCALE
        boxes = []  # (change, area, box): each animated item where it is most visible in the transition
        for tr in tracks.values():
            vs = [(t, v) for t, v in tr["pts"] if main["start"] - 20 <= t <= main["end"] + 20]
            if len(vs) < 2 or max(v for _, v in vs) - min(v for _, v in vs) < 0.05:
                continue
            t_peak = max(vs, key=lambda tv: (tv[1], tv[0]))[0]  # (its rect then: a hover line moves first)
            r = at(tr["pts"], tr["rects"], t_peak)
            r = (r[0] * scale - off[0], r[1] * scale - off[1], r[2] * scale, r[3] * scale)  # in the image
            bx0, by0 = max(x, r[0]), max(y, r[1])
            bx1, by1 = min(x + w, r[0] + r[2]), min(y + h, r[1] + r[3])
            if bx1 > bx0 and by1 > by0:
                boxes.append((max(v for _, v in vs) - min(v for _, v in vs), (bx1 - bx0) * (by1 - by0), (bx0, by0, bx1, by1)))
        small = [b for b in boxes if b[1] < 0.25 * w * h]  # containers (a popup's overlay, a whole pane) aside
        boxes = small or [b for b in boxes if b[1] < 0.8 * w * h]
        if boxes:
            # all of them when they fit at >= 0.75×; else the strongest (then smallest) and what fits around it
            boxes.sort(key=lambda b: (-b[0], b[1]))
            u = list(boxes[0][2])
            for _, _, bb in boxes[1:]:
                cand = [min(u[0], bb[0]), min(u[1], bb[1]), max(u[2], bb[2]), max(u[3], bb[3])]
                if cand[2] - cand[0] + 16 <= cap_w and cand[3] - cand[1] + 16 <= cap_h:
                    u = cand
            fx0, fy0, fx1, fy1 = u[0] - 8, u[1] - 8, u[2] + 8, u[3] + 8
            fw, fh = max(160, fx1 - fx0), max(80, fy1 - fy0)
            cx, cy = (fx0 + fx1) / 2, (fy0 + fy1) / 2
            if fw * 1.5 <= tile_w and fh * 1.5 <= tile_h:  # a small control: a half-tile crop, shown at 2×
                fw, fh = tile_w // 2, min(tile_h // 2, max(fh, 80))
            fw, fh = min(fw, cap_w), min(fh, cap_h)
            fx0, fy0 = int(max(0, min(iw - fw, cx - fw / 2))), int(max(0, min(ih - fh, cy - fh / 2)))
            return [fx0, fy0, int(min(fw, iw - fx0)), int(min(fh, ih - fy0))]
    mag = np.zeros(full[0].shape[:2], dtype=np.float32)
    for k in (main["frames"] if main else []):
        if k > 0 and full[k].shape == full[0].shape:
            mag += max3(np.abs(full[k] - full[k - 1]))
    ys, xs = np.nonzero(mag > 2)
    if not len(xs):
        ys, xs = np.nonzero(dif)
    if len(xs):
        fx0, fy0, fx1, fy1 = xs.min() * stp + x - 8, ys.min() * stp + y - 8, (xs.max() + 1) * stp + x + 8, (ys.max() + 1) * stp + y + 8
    else:
        fx0, fy0, fx1, fy1 = x, y, x + w, y + h
    fw, fh = max(160, fx1 - fx0), max(80, fy1 - fy0)
    cx, cy = (fx0 + fx1) / 2, (fy0 + fy1) / 2
    cap_w, cap_h = tile_w / MIN_FOCUS_SCALE, tile_h / MIN_FOCUS_SCALE
    if fw > cap_w or fh > cap_h:
        tot = mag.sum()
        if tot > 0:  # the change's centre of mass
            yy, xx = np.indices(mag.shape)
            cx, cy = float((xx * mag).sum() / tot) * stp + x, float((yy * mag).sum() / tot) * stp + y
        fw, fh = min(fw, cap_w), min(fh, cap_h)
    fx0, fy0 = int(max(0, min(iw - fw, cx - fw / 2))), int(max(0, min(ih - fh, cy - fh / 2)))
    return [fx0, fy0, int(min(fw, iw - fx0)), int(min(fh, ih - fy0))]


def fit_image(im, max_w, max_h):
    """2× nearest-neighbour when a small crop fits twice (small controls), else scaled to fit."""
    s = min(max_w / im.width, max_h / im.height)
    if s >= 2:
        return im.resize((im.width * 2, im.height * 2), Image.NEAREST), 2.0
    if s >= 1:
        return im, 1.0
    return im.resize((max(1, int(im.width * s)), max(1, int(im.height * s))), Image.LANCZOS), s


def merged(frames, ims, times, region, shown_idx, zoom, nearest, segs, main, scopes, lum, tracks, anims, spans,
           metrics, meta, cap, dmean, focus, dif_info, full, row_segs=None, elements=None, pa_max_h=200, b_lines=2, kf_band=480,
           extra_focus=None, zoom_graphs=False, max_frames=12, kf_fit=False, pursuit=None, no_tb=False, gh_small=False):
    """One image per transition, drawn at the reviewers' 1568 px width: caption, key frames, the opacity/ink
    graph with the animations that ran, and two perceived-at-real-speed rows (5 moments across the main
    transition; then every 16.7 ms around its peak)."""
    args = dict(locals())  # to draw again with a smaller key-frame band when the image is too tall
    f_t, f_c, f_l, f_s = font(True, 22), font(False, 17), font(True, 15), font(False, 14)
    probe = ImageDraw.Draw(Image.new("RGB", (8, 8)))
    title = f"{meta['name']} · {meta['theme']} — {meta['desc']}"
    tl = wrap(probe, title, f_t, W - 2 * PAD)
    cl = wrap(probe, cap, f_c, W - 2 * PAD)
    head_h = 12 + 30 * len(tl) + 22 * len(cl) + 10
    # ---- key frames: as large as fits a ~560 px band, at most 12 (the strip pages have the rest)
    x, y, w, h = region
    sel = list(shown_idx)
    if len(sel) > max_frames:
        keep = {sel[0], sel[-1]}
        step = (len(sel) - 1) / (max_frames - 1)
        sel = sorted(keep | {sel[int(round(i * step))] for i in range(max_frames)})
    cw, chh = w * zoom, h * zoom
    lab_h, gap, band = 20, 10, kf_band
    best = None
    for cols in range(1, len(sel) + 1):
        tw = min(cw, (W - 2 * PAD - (cols - 1) * gap) / cols)
        th = chh * tw / cw
        rows = -(-len(sel) // cols)
        tot = rows * (th + lab_h + gap)
        if kf_fit and tot > band:  # drawn again to fit 1568 px: the tiles shrink to the band, not a whole step
            th2 = max(8.0, (band / rows) - lab_h - gap)
            if cw * th2 / chh >= 64:  # never slivers (their labels would overlap)
                th, tw = th2, cw * th2 / chh
                tot = rows * (th + lab_h + gap)
        cand = (tot <= band, round(tw) if tot <= band else -tot, -tot, cols, tw, th, rows, tot)
        if best is None or cand[:3] > best[:3]:  # largest tiles that fit, then the fewest rows
            best = cand
    _, _, _, cols, tw, th, rows, kf_h = best
    tw, th = int(tw), int(th)
    kf_h = int(rows * (th + lab_h + gap))
    # ---- graph geometry
    gh = 110 if gh_small else 190
    ranked = sorted(tracks.items(), key=lambda kv: -(max(v for _, v in kv[1]["pts"]) - min(v for _, v in kv[1]["pts"])))
    ranked = [kv for kv in ranked if max(v for _, v in kv[1]["pts"]) - min(v for _, v in kv[1]["pts"]) > 0.05][:6]
    legend = [(COLORS[i % len(COLORS)], short_name(key) + (" (text)" if tr["text"] else ""), False)
              for i, (key, tr) in enumerate(ranked)]
    ink_cols = {"text": (240, 240, 240), "other": (120, 190, 255)}
    for nm, sc in scopes.items():
        legend.append((ink_cols[nm], f"ink of {'text' if nm == 'text' else 'other content (images, icons, plots)'}, dashed", True))
    legend.append(((140, 140, 140), "mean luminance of the change box, dotted", True))
    el_cols = [(255, 140, 90), (120, 230, 140), (200, 160, 255), (255, 230, 120)]
    for i, e in enumerate(elements or []):
        legend.append((el_cols[i % len(el_cols)], f"ink of {e['label']} (own rect), dash-dot", True))
    if (metrics.get("blink_items") or any(any(sc["bmask"]) for sc in scopes.values())
            or any(any(e["bmask"]) for e in (elements or []))):
        legend.append(((95, 32, 32), "shaded: blink", False))
    leg_cols = 3 if W >= 1000 else 2
    leg_rows = -(-len(legend) // leg_cols)
    bars_h = (38 * len(anims) + 30) if anims else 26
    graph_h = 44 + gh + 26 + 20 * leg_rows + 8 + bars_h
    # zoomed graphs (MOTION_FOCUS "zoom_graphs"): ~250 ms around each transition, with its stated end and last change
    zsegs = [sg for sg in segs if len(sg["frames"]) > 1][:2] if zoom_graphs else []
    zoom_gh = 120
    zoom_h = (44 + zoom_gh + 30) if zsegs else 0
    graph_h += zoom_h
    # ---- perceived rows: per transition shown (the main one, or each for MOTION_FOCUS "rows": "all"), 5
    # moments across it, then every 16.7 ms around it (5 or 10 images, five to a line), all one tile size
    pa_w = (W - 2 * PAD - 4 * 12) // 5
    fr = focus_rect(region, main, dif_info, focus, ims, full, pa_w, pa_max_h, tracks, float(meta.get("scale", 1) or 1),
                    tuple(meta.get("crop") or (0, 0))[:2])
    probe_a, sa = fit_image(ims[0].crop((fr[0], fr[1], fr[0] + fr[2], fr[1] + fr[3])), pa_w, pa_max_h)
    pa_h = probe_a.height
    blocks = []
    for sg in (row_segs or ([main] if main else [])):
        Ta = [sg["start"] - FRAME_MS + (sg["end"] + 50 - (sg["start"] - FRAME_MS)) * k / 4 for k in range(5)]
        nb = 5 if sg["dur"] + 2 * FRAME_MS <= 5 * FRAME_MS or sg is not main or b_lines < 2 else 10  # one line for a secondary one
        if sg["dur"] + 2 * FRAME_MS <= nb * FRAME_MS:
            tb0 = sg["start"] - FRAME_MS
        else:  # a long transition: the frame times around its strongest change
            pk = max(sg["frames"], key=lambda k: dmean[k])
            tb0 = max(sg["start"] - FRAME_MS, times[pk] - (nb / 2 - 0.5) * FRAME_MS)
        blocks.append((sg, Ta, [] if no_tb else [tb0 + (j + 1) * FRAME_MS for j in range(nb)], fr, "", pa_h))
    if not blocks:
        blocks.append((None, [0.0, 50.0, 100.0, 150.0, 200.0], [(j + 1) * FRAME_MS for j in range(5)], fr, "", pa_h))
    # more crops of the main transition (MOTION_FOCUS "extra": [(label, rect)], e.g. the cover beside the text)
    for label, rect in (extra_focus or []):
        rect = [int(v) for v in rect]
        eh = fit_image(ims[0].crop((rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3])), pa_w, pa_max_h)[0].height
        b0 = blocks[0]
        blocks.append((b0[0], b0[1], [], rect, label, eh))  # its 5 moments only
    # the eye following a sliding label (MOTION_FOCUS "pursuit": (label, item pattern)): its 5 moments, a crop that
    # follows the label, each frame shifted as a smoothly moving eye sees it
    pursue = {}
    if pursuit and blocks and blocks[0][0] is not None:
        lab_, pat_ = pursuit
        ks = sorted((k for k in tracks if fnmatch.fnmatch(k, pat_)), key=lambda k: ("#" in k, len(k)))
        if ks:
            tr = tracks[ks[0]]
            sc_ = float(meta.get("scale", 1) or 1)
            ox_, oy_ = (meta.get("crop") or [0, 0])[:2]
            pts = [(t, (r[0] - ox_) * sc_, (r[1] - oy_) * sc_) for (t, _), r in zip(tr["pts"], tr["rects"])]

            def pos(t, pts=pts):
                if t <= pts[0][0]:
                    return pts[0][1:]
                for (ta, xa, ya), (tb_, xb, yb) in zip(pts, pts[1:]):
                    if ta <= t <= tb_:
                        f = (t - ta) / max(1e-6, tb_ - ta)
                        return xa + f * (xb - xa), ya + f * (yb - ya)
                return pts[-1][1:]
            b0 = blocks[0]
            rx, ry, rw, rh = tr["rects"][0]
            ph_ = fit_image(Image.new("RGB", (int((rw + 16) * sc_), int((rh + 72) * sc_))), pa_w, pa_max_h)[0].height
            pursue[len(blocks)] = (pos, int((rw + 16) * sc_), int((rh + 72) * sc_), lab_)
            blocks.append((b0[0], b0[1], [], None, f"an eye following {lab_} (simulation)", ph_ + 18))
    per_h = 88 + sum(22 + (18 + ph + 8) + 6 + ((22 + (18 + ph + 8) * (len(tb) // 5) + 6) if tb else 0)
                     for _, _, tb, _, _, ph in blocks)
    H = head_h + kf_h + 8 + graph_h + per_h + 10
    # The reviewers' 1568 px: fewer key frames first (their pages have them all), then the key frames fitted once
    # to the band left (never under 64 px wide), then the perceived rows' tiles, a step at a time.
    if H > MAX_H and max_frames > 6 and len(shown_idx) > 6:
        return merged(**dict(args, max_frames=max_frames - 2))
    if H > MAX_H and kf_h > 160 and not kf_fit:
        return merged(**dict(args, kf_band=min(kf_band, kf_h) - int(H - MAX_H) - 1, kf_fit=True))
    if H > MAX_H and not no_tb:  # then without the every-16.7 ms rows (the 5 moments stay; the pages have all)
        return merged(**dict(args, no_tb=True))
    if H > MAX_H and not gh_small:  # then a shorter graph
        return merged(**dict(args, gh_small=True))
    if H > MAX_H and pa_max_h > 60:  # then the perceived rows' tiles, a step at a time
        return merged(**dict(args, pa_max_h=pa_max_h - 15))
    if H > MAX_H:
        raise RuntimeError(f"merged image {meta.get('name')} is {int(H)} px tall, over {MAX_H}, with nothing left to shrink")
    S = Image.new("RGB", (W, int(H)), (34, 36, 40))
    d = ImageDraw.Draw(S)
    yy = 12
    for ln in tl:
        d.text((PAD, yy), ln, fill=(255, 255, 255), font=f_t)
        yy += 30
    for ln in cl:
        d.text((PAD, yy), ln, fill=(225, 225, 225), font=f_c)
        yy += 22
    yy = head_h
    # key frames
    in_seg = {k for s in segs for k in s["frames"]}
    for i, k in enumerate(sel):
        cx, cy = PAD + (i % cols) * (tw + gap), yy + (i // cols) * (th + lab_h + gap)
        im = ims[k].crop((x, y, x + w, y + h))
        im = im.resize((tw, th), Image.NEAREST if (nearest or zoom > 1) and tw >= w * zoom * 0.99 else Image.LANCZOS)
        S.paste(im, (int(cx), int(cy + lab_h)))
        ch = k in in_seg
        dt = times[k] - times[k - 1] if k else 0
        if tw >= 300:
            lab = f"t = {times[k]:+.0f} ms (+{dt:.0f})" + ("  before the action" if times[k] < 0 else "") + ("  changing" if ch else "")
        else:  # narrow tiles: the time only; amber marks a changing frame
            lab = f"{times[k]:+.0f} ms"
        d.text((cx, cy), lab, fill=(255, 220, 120) if ch else (175, 175, 175), font=f_l if ch else f_s)
    yy += kf_h + 8
    # graph
    fc = main["start"] if main else 0.0
    lc = main["end"] if main else FRAME_MS
    t_lo = min(-20.0, fc - 40.0)
    t_hi = max(lc + 80.0, fc + 150.0)
    outside = []
    if main and len(segs) > 1:  # several transitions: the whole sequence when it is short (or zoomed below), else the main one
        if segs[-1]["end"] - segs[0]["start"] <= 1500 or zoom_graphs:
            t_lo, t_hi = min(t_lo, segs[0]["start"] - 40.0), max(t_hi, segs[-1]["end"] + 80.0)
        else:
            outside = [s for s in segs if s is not main]
            t_lo = min(-20.0, fc - 40.0) if fc < 1500 else fc - 40.0
    t_hi = min(t_hi, times[-1] + 60)
    gtitle = wrap(d, "Opacity of the animated items (item tree, every frame), ink per scope and luminance; t = ms "
                  "after the input reached the app. Ticks on top: the key frames above (amber: changing)."
                  + (" Not in this graph: the transition" + ("s" if len(outside) > 1 else "") + " at "
                     + ", ".join(f"+{fms(s['start'])}" for s in outside) + "." if outside else ""), f_l, W - 2 * PAD)[:2]
    for i, ln in enumerate(gtitle):
        d.text((PAD, yy + i * 18), ln, fill=(235, 235, 235), font=f_l)
    def plot(gx0, gx1, gy0, gy1, t_lo, t_hi, sel_ticks=True):
        X = lambda t: gx0 + (t - t_lo) / max(1e-6, (t_hi - t_lo)) * (gx1 - gx0)
        Y = lambda v: gy1 - v * (gy1 - gy0)
        d.rectangle((gx0, gy0, gx1, gy1), outline=(110, 110, 110))
        for v in (0, 0.3, 0.5, 1.0):
            d.line((gx0, Y(v), gx1, Y(v)), fill=(66, 66, 66))
            d.text((gx0 - 36, Y(v) - 9), f"{v:.1f}", fill=(170, 170, 170), font=f_s)
        for s in segs:  # the transitions, faintly
            if s["end"] > t_lo and s["start"] < t_hi:
                d.rectangle((X(max(s["start"], t_lo)), gy0 + 1, X(min(s["end"], t_hi)), gy1 - 1), fill=(44, 48, 58))
        for sc in list(scopes.values()) + list(elements or []):
            for k, (a, b) in enumerate(spans):
                if sc["bmask"][k] and b > t_lo and a < t_hi:
                    d.rectangle((X(max(a, t_lo)), gy0 + 1, X(min(b, t_hi)), gy1 - 1), fill=(95, 32, 32))
        for bi_ in metrics.get("blink_items", []):
            a, b = bi_["start_ms"], bi_["start_ms"] + bi_["ms"]
            if b > t_lo and a < t_hi:
                d.rectangle((X(max(a, t_lo)), gy0 + 1, X(min(b, t_hi)), gy1 - 1), fill=(95, 32, 32))
        span = t_hi - t_lo
        step = 25 if span < 300 else 50 if span < 700 else 200 if span < 2500 else 1000
        t = int(np.ceil(t_lo / step) * step)
        while t <= t_hi:
            d.line((X(t), gy1, X(t), gy1 + 5), fill=(170, 170, 170))
            d.text((X(t) - 12, gy1 + 6), f"{t}", fill=(170, 170, 170), font=f_s)
            t += step
        if t_lo <= 0 <= t_hi:
            d.line((X(0), gy0, X(0), gy1), fill=(255, 255, 255))
            d.text((X(0) + 4, gy0 + 2), "action", fill=(255, 255, 255), font=f_s)
        for k in (sel if sel_ticks else []):
            if t_lo <= times[k] <= t_hi:
                d.line((X(times[k]), gy0 - 8, X(times[k]), gy0), fill=(255, 220, 120), width=2)

        def series(vals, col, dash, gapw, width=2):
            for k in range(len(times)):
                a, b = spans[k]
                if b < t_lo or a > t_hi:
                    continue
                xa, xb = X(max(a, t_lo)), X(min(b, t_hi))
                xx = xa
                while xx < xb:
                    d.line((xx, Y(vals[k]), min(xx + dash, xb), Y(vals[k])), fill=col, width=width)
                    xx += dash + gapw
        for nm, sc in scopes.items():
            mx = max(sc["ink"]) or 1.0
            series([v / mx for v in sc["ink"]], ink_cols[nm], 6, 4)
        series(lum, (140, 140, 140), 2, 4)
        for i, e in enumerate(elements or []):
            mx = max(e["ink"]) or 1.0
            series([v / mx for v in e["ink"]], el_cols[i % len(el_cols)], 10, 3, width=2)
        for i, (key, tr) in enumerate(ranked):
            col = COLORS[i % len(COLORS)]
            pts = tr["pts"] + [(t_hi, tr["pts"][-1][1])]
            for (ta, va), (tb, vb) in zip(pts, pts[1:]):
                if tb < t_lo or ta > t_hi:
                    continue
                d.line((X(max(ta, t_lo)), Y(va), X(min(tb, t_hi)), Y(va)), fill=col, width=3)
                if t_lo <= tb <= t_hi:
                    d.line((X(tb), Y(va), X(tb), Y(vb)), fill=col, width=2)
        return X, Y

    gx0, gx1, gy0, gy1 = 62, W - 20, yy + 44, yy + 44 + gh
    X, Y = plot(gx0, gx1, gy0, gy1, t_lo, t_hi)
    ly = gy1 + 26
    for i, (col, name, dashed) in enumerate(legend):
        lx, lyy = PAD + (i % leg_cols) * ((W - 2 * PAD) // leg_cols), ly + (i // leg_cols) * 20
        if dashed:
            for q in range(0, 24, 8):
                d.line((lx + q, lyy + 9, lx + q + 5, lyy + 9), fill=col, width=3)
        else:
            d.rectangle((lx, lyy + 5, lx + 22, lyy + 13), fill=col)
        room = (W - 2 * PAD) // leg_cols - 40
        while name and d.textlength(name, font=f_s) > room:
            name = name[:-2] + "…" if not name.endswith("…") else name[:-2] + "…"
        d.text((lx + 30, lyy), name, fill=(215, 215, 215), font=f_s)
    by = ly + 20 * leg_rows + 8
    d.text((PAD, by), ("QML animations that ran (Behaviors, standalone animations; popup transitions only as opacity "
                       "lines):" if W >= 1000 else "QML animations that ran:") if anims else
           ("No QML Behavior or standalone animation ran (popup transitions, if any, show as opacity lines)."
            if W >= 1000 else "No QML Behavior or standalone animation ran."), fill=(235, 235, 235), font=f_l)
    for i, a in enumerate(anims):
        ay = by + 24 + i * 38
        st_ = a["start"]
        en = a["end"] if a["end"] is not None else (st_ + (a["duration"] or 0))
        bx0, bx1 = X(max(st_, t_lo)), max(X(max(st_, t_lo)) + 2, X(min(en, t_hi)))
        d.rectangle((bx0, ay + 20, bx1, ay + 32), fill=(80, 140, 200))
        if a["prop"]:
            label = f"{a['target']}.{a['prop']} {a['frm']} to {a['to']}, {a['duration']} ms {a['easing']} ({a['cls']}), "
        else:  # a group animation (Sequential/Parallel): no property of its own
            label = f"{a['cls']} on {a['target'] or 'an item'}" + (f", {a['duration']} ms" if a["duration"] else "") + ", "
        label = (label + f"ran {st_:.0f}–{en:.0f} ms")[:110]
        tlen = d.textlength(label, font=f_s)
        d.text((min(max(10, bx0), W - 10 - tlen), ay), label, fill=(215, 215, 215), font=f_s)  # above its bar
    if zsegs:
        zy = yy + graph_h - zoom_h
        for i, ln in enumerate(wrap(d, "Zoomed, about 250 ms around each transition: dashed amber, its stated end (start + "
                                       "duration of the animations that started in it); red, its last changed frame.",
                                    f_l, W - 2 * PAD)[:2]):
            d.text((PAD, zy + i * 18), ln, fill=(235, 235, 235), font=f_l)
        half = (W - 62 - 20) // len(zsegs)
        for i, sg in enumerate(zsegs):
            ends = [a["start"] + a["duration"] for a in anims if a["duration"] is not None and a["start"] is not None
                    and sg["start"] - 60 <= a["start"] <= sg["end"]]
            stated = max(ends) if ends else None
            last = times[sg["b"]]
            z_lo = sg["start"] - 50.0
            z_hi = max(last, stated or last) + 50.0
            if z_hi - z_lo < 250:
                z_hi = z_lo + 250
            zx0, zx1 = 62 + i * half, 62 + (i + 1) * half - 30
            ZX, _ = plot(zx0, zx1, zy + 44, zy + 44 + zoom_gh, z_lo, z_hi, sel_ticks=False)
            if stated is not None and z_lo <= stated <= z_hi:
                yv = zy + 44
                while yv < zy + 44 + zoom_gh:
                    d.line((ZX(stated), yv, ZX(stated), min(yv + 6, zy + 44 + zoom_gh)), fill=(255, 200, 80), width=2)
                    yv += 10
            d.line((ZX(last), zy + 44, ZX(last), zy + 44 + zoom_gh), fill=(240, 90, 90), width=2)
            zl = f"+{fms(sg['start'])}" + (f"; after its stated end: {max(0.0, last - stated):.0f} ms" if stated is not None else "")
            d.text((zx1 - 6 - d.textlength(zl, font=f_s), zy + 46), zl, fill=(235, 235, 235), font=f_s)
    yy += graph_h
    # perceived rows
    for i, ln in enumerate(wrap(d, f"Perceived at real speed — a SIMULATION: each image is the mean of the "
                                f"{WINDOW_MS:.0f} ms before t (the eye's integration time); it overstates ghosting, as the "
                                "eye follows moving text.", f_l, W - 2 * PAD)[:2]):
        d.text((PAD, yy + i * 18), ln, fill=(255, 220, 120), font=f_l)
    yy += 18
    crop_note = (f"Crop: {fr[2]}×{fr[3]} px at ({fr[0]}, {fr[1]}), " + ("the small control at 2×, nearest-neighbour" if sa >= 2
                 else ("given for this strip" if focus else "around what animates in the transition")
                 + (f", shown at {sa:.2f}×" if sa < 1 else ", 1×")))
    note_lines = wrap(d, f"A 1-2 frame overlap reads as a soft change; a real dip or flash still shows as dimming. "
                         f"{crop_note}.", f_s, W - 2 * PAD)[:2]
    for i, ln in enumerate(note_lines):
        d.text((PAD, yy + 22 + 18 * i), ln, fill=(200, 200, 200), font=f_s)
    yy += 52 + 18 * (len(note_lines) - 1)
    for bi, (sg, Ta, Tb, bfr, blabel, bph) in enumerate(blocks):
        line_h = 18 + bph + 8
        if bi in pursue:
            pos, pw_, ph2, lab_ = pursue[bi]
            for i_, ln in enumerate(wrap(d, f"An eye following {lab_}, SIMULATED (45 ms of frames, each shifted as the eye "
                                     "has moved since; smeared or doubled edges = big or uneven steps):", f_l, W - 2 * PAD)[:2]):
                d.text((PAD, yy + 18 * i_), ln, fill=(255, 220, 120), font=f_l)
            yy += 18
            yy += 22
            for i, T in enumerate(Ta):
                px_, py_ = pos(T)
                rect_ = (int(px_ - 8), int(py_ - 30), pw_, ph2)
                im, _ = fit_image(perceived_pursuit(ims, times, spans, rect_, T, pos), pa_w, pa_max_h)
                px = PAD + i * (pa_w + 12)
                d.text((px, yy), f"t = {T:.0f} ms", fill=(255, 255, 255), font=f_s)
                S.paste(im, (px, yy + 18))
            yy += line_h + 6
            continue
        which = ("Across the transition" if len(segs) < 2 or sg is None else
                 f"Across the transition at +{fms(sg['start'])} ({segs.index(sg) + 1} of {len(segs)})")
        if blabel and bfr:
            which += f", {blabel} ({bfr[2]}×{bfr[3]} px at ({bfr[0]}, {bfr[1]}))"
        d.text((PAD, yy), f"{which}, 5 moments:", fill=(235, 235, 235), font=f_l)
        yy += 22
        for i, T in enumerate(Ta):
            im, _ = fit_image(perceived(ims, times, spans, bfr, T), pa_w, pa_max_h)
            px = PAD + i * (pa_w + 12)
            d.text((px, yy), f"t = {T:.0f} ms", fill=(255, 255, 255), font=f_s)
            S.paste(im, (px, yy + 18))
        yy += line_h + 6
        if not Tb:
            continue
        d.text((PAD, yy), f"Every 16.7 ms (one 60 Hz frame) from t = {Tb[0]:.0f} to {Tb[-1]:.0f} ms, around the actual "
               "change:", fill=(235, 235, 235), font=f_l)
        yy += 22
        for j, T in enumerate(Tb):
            im, _ = fit_image(perceived(ims, times, spans, bfr, T), pa_w, pa_max_h)
            px = PAD + (j % 5) * (pa_w + 12)
            py = yy + (j // 5) * line_h
            d.text((px, py), f"t = {T:.0f} ms", fill=(255, 255, 255), font=f_s)
            S.paste(im, (px, py + 18))
        yy += line_h * (len(Tb) // 5) + 6
    return S
