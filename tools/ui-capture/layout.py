#!/usr/bin/env python3
"""The layout checker's report. Usage: layout.py <round-dir>

Reads, per captured state, the probe's `layout` dump (layout/<shot>.json: every shown window's findings
and visible text, taken from the live item tree right after the grab) and the index, applies the
allowlists in states.py, samples text backgrounds from the shots for contrast, groups the findings per
(component, issue) across states, and writes <round>/LAYOUT.md and <round>/layout.json.

Findings (probe, public Qt API: QQuickItem geometry, visibility, opacity, clip; QQuickText properties):
- text-clipped: a text item's box extends past its window, a clipping ancestor or its popup;
- text-overflow: a text's painted size (contentWidth/Height) exceeds its box minus padding, not elided;
- out-of-bounds: any other visible item past its window, a clipping ancestor or its popup (reported at the
  outermost item; content of a Flickable is checked only where it lies inside the viewport);
- overlap: visible children of a layout or positioner (z 0) whose rects overlap by more than 1 px;
- elided: a text with `truncated` true (minus states.ELIDE_OK);
- contrast: an enabled text's declared colour (with its opacity) against the background sampled around its
  rect, below 4.5:1 (3:1 for >= 18 pt or bold >= 14 pt).
"""
import collections
import fnmatch
import glob
import json
import os
import sys
from collections import Counter

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("SHOT_OUT", "/nonexistent/shots")
os.environ.setdefault("SHOT_APP", "x")
import states as st  # noqa: E402

SEVERITY = {"text-clipped": 1, "text-overflow": 1, "out-of-bounds": 1, "overlap": 2, "elided": 3, "contrast": 4}
TITLES = {1: "Clipped or overflowing text, and items out of bounds", 2: "Overlapping siblings in layouts",
          3: "Elided text (not allowlisted)", 4: "Text contrast below WCAG AA",
          5: "Owned by the platform style (Breeze / qqc2-desktop-style controls), for reference"}


def style_owned(f):
    """Findings in QML that is not the app's own: the app's modules are under /FerrousUi/ (compiled in);
    the platform style (org.kde.desktop), QtQuick.Controls and Kirigami are not."""
    u = f.get("file", "")
    return bool(u) and "/FerrousUi/" not in u


def short(u):
    return u.rsplit("/", 1)[-1] if u else ""


def lin(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def ratio(a, b):
    la = 0.2126 * lin(a[0]) + 0.7152 * lin(a[1]) + 0.0722 * lin(a[2])
    lb = 0.2126 * lin(b[0]) + 0.7152 * lin(b[1]) + 0.0722 * lin(b[2])
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def matches(patterns, f):
    keys = [f.get("name", ""), f.get("path", "") + "/" + f.get("name", ""), f.get("file", ""), f.get("type", "")]
    if f.get("column"):  # a table cell: also "path/name(col=KEY)"
        keys.append(f"{f.get('path', '')}/{f.get('name', '')}(col={f['column']})")
    return any(fnmatch.fnmatch(k, p) for p in patterns for k in keys if k)


def variant(e):
    v = []
    v.append("24 pt" if "font24" in e["file"] else "")
    v.append("scale 2" if e.get("scale", 1) != 1 else "")
    v.append(e["size"].replace("x", "×"))
    v.append(e["theme"])
    v.append("animations off" if "animoff" in e["file"] else "")
    return " ".join(x for x in v if x)


def band_background(arr, rect, sc, ink):
    """The rendered background of a text: the most frequent colour inside its rect and 2 px around it,
    leaving out pixels near the text's own colour (the glyphs and their antialiasing). Returns (colour,
    share of the remaining pixels); a low share means a busy background (an image, a plot)."""
    import numpy as np
    x, y, w, h = [v * sc for v in rect]
    H, W = arr.shape[:2]
    x0, y0 = max(0, int(x - 2)), max(0, int(y - 2))
    x1, y1 = min(W, int(x + w + 2)), min(H, int(y + h + 2))
    if x1 - x0 < 2 or y1 - y0 < 2:
        return None, 0
    sub = arr[y0:y1, x0:x1].reshape(-1, 3).astype(np.int32)
    sub = sub[((sub - np.array(ink)) ** 2).sum(1) > 70 ** 2]
    if len(sub) == 0:
        return None, 0
    keys = (sub[:, 0] << 16) | (sub[:, 1] << 8) | sub[:, 2]
    vals, counts = np.unique(keys, return_counts=True)
    k = int(vals[counts.argmax()])
    return ((k >> 16) & 255, (k >> 8) & 255, k & 255), counts.max() / len(sub)


def parse_argb(s):
    s = s.lstrip("#")
    if len(s) == 8:
        return int(s[:2], 16), tuple(int(s[i:i + 2], 16) for i in (2, 4, 6))
    return 255, tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def edge_text(by):
    return ", ".join(f"{k} by {v} px" for k, v in by.items())


def analyse(args):
    """One state: its raw findings (filtered) and its contrast failures, as records to group."""
    rd, file, e = args
    elide_ok = getattr(st, "ELIDE_OK", [])
    ignore = getattr(st, "LAYOUT_IGNORE", [])
    skip = getattr(st, "CONTRAST_SKIP", [])
    stats = Counter()
    recs = []
    dump = json.load(open(os.path.join(rd, e["layout"])))
    sc = e.get("scale", 1)
    grabbed = e.get("grabbed", {})
    # Window coordinates -> shot pixels: the scale, and for a decorated window capture its title bar offset.
    ox, oy = grabbed.get("image_offset", [0, 0])
    arr = None
    def occ(wname, rect, shown, off=(ox, oy)):
        # In the shot's pixels when the window is the one in the shot (decorated captures are offset).
        if shown and rect and (off[0] or off[1] or sc != 1):
            rect = [round(rect[0] * sc + off[0]), round(rect[1] * sc + off[1]),
                    round(rect[2] * sc), round(rect[3] * sc)]
        return dict(state=f"{e['area']}/{e['state']}", shot=f"shots/{file}", window=wname, rect=rect, in_shot=shown)
    for w in dump.get("windows", []):
        # a menu of the main window, which a main-window grab draws in at its offset
        pop = grabbed.get("kind") == "main" and w.get("main_popup_offset")
        is_grabbed = ((grabbed.get("kind") == "main" and w.get("main")) or bool(pop) or
                      (grabbed.get("kind") == "window" and grabbed.get("name") in (w.get("name"), w.get("title"))) or
                      (grabbed.get("kind") == "dialog" and grabbed.get("cls") == w.get("cls")))
        wname = "main window" if w.get("main") else (w.get("name") or w.get("title") or w.get("cls"))
        woff = (ox + pop[0] * sc, oy + pop[1] * sc) if pop else (ox, oy)
        for f in w.get("findings", []):
            stats["raw"] += 1
            if matches(ignore, f) or (f["kind"] == "elided" and matches(elide_ok, f)):
                stats["allowlisted"] += 1
                continue
            if f["kind"] == "elided" and f.get("share", 0) >= 0.999 and f.get("shown_text") == f.get("text"):
                # Qt's `truncated` with the whole text fitting its width (elidedText gives it back whole): a
                # sub-pixel flag, not an ellipsis on screen (the search's match-emphasis runs)
                stats["elided_subpixel"] += 1
                continue
            detail = ""
            if f["kind"] in ("text-clipped", "out-of-bounds"):
                detail = f"past its {f.get('bound', '?').split(':')[0]} ({f.get('bound', '')}): {edge_text(f.get('by', {}))}"
            elif f["kind"] == "text-overflow":
                detail = " and ".join(x for x in (f"{f['over_x']} px wider" if f.get("over_x") else "",
                                                  f"{f['over_y']} px taller" if f.get("over_y") else "") if x) + \
                    " than its box (not elided" + (", wraps" if f.get("wrap") else "") + ")"
            elif f["kind"] == "overlap":
                ov = f.get("overlap", [0, 0, 0, 0])
                detail = f"overlaps {f.get('with')} ({f.get('with_type')}) by {ov[2]}×{ov[3]} px in {f.get('layout')}"
            elif f["kind"] == "elided":
                detail = f"elided at {f.get('shown_width')} px"
            key = (f["kind"], f.get("file", ""), f.get("path", ""), f.get("name", ""), f.get("type", ""),
                   f.get("bound", "") if f["kind"] != "overlap" else f.get("with", ""), tuple(sorted(f.get("by", {}))))
            worst = max([*f.get("by", {}).values(), f.get("over_x", 0), f.get("over_y", 0)] or [0])
            recs.append((key, f, detail, f.get("text", ""), occ(wname, f.get("rect"), is_grabbed, woff), worst))
        if not is_grabbed:
            continue
        for t in w.get("texts", []):
            if matches(ignore, t) or matches(skip, t):
                continue
            if t.get("dimmed"):  # under a modal popup's dim, on purpose
                stats["contrast_dimmed"] += 1
                continue
            if arr is None:
                import numpy as np
                arr = np.asarray(Image.open(os.path.join(rd, "shots", file)).convert("RGB"))
            a, rgb = parse_argb(t["color"])
            x, y, w_, h_ = t["rect"]
            img_rect = [x + woff[0] / sc, y + woff[1] / sc, w_, h_]  # band_background scales by sc
            bg, share = band_background(arr, img_rect, sc, rgb)
            stats["texts"] += 1
            if bg is None or share < 0.5:  # a busy background (image, plot): no reliable sample
                stats["contrast_unsampled"] += 1
                continue
            k = a / 255.0 * float(t.get("opacity", 1.0))
            fg = tuple(round(c * k + b * (1 - k)) for c, b in zip(rgb, bg))
            r = ratio(fg, bg)
            large = t["pt"] >= 18 or (t.get("bold") and t["pt"] >= 14)
            need = 3.0 if large else 4.5
            if r >= need:
                continue
            key = ("contrast", t.get("file", ""), t.get("path", ""), t.get("name", ""), t.get("type", ""),
                   "#%02x%02x%02x" % fg, "#%02x%02x%02x" % bg)
            detail = (f"{r:.2f}:1 (needs {need:.1f}; {'large' if large else 'normal'} text {t['pt']:.0f} pt"
                      f"{' bold' if t.get('bold') else ''}): {'#%02x%02x%02x' % fg} on {'#%02x%02x%02x' % bg}")
            recs.append((key, dict(kind="contrast", file=t.get("file", ""), path=t.get("path", ""), name=t.get("name", ""),
                                   type=t.get("type", "")), detail, t["text"], occ(wname, t.get("rect"), True, woff), r))
    return file, e, stats, recs


def main():
    from concurrent.futures import ProcessPoolExecutor
    rd = os.path.abspath(sys.argv[1])
    entries = {}
    for p in glob.glob(os.path.join(rd, "index", "*.json")):
        for e in json.load(open(p)):
            if e.get("layout") and os.path.exists(os.path.join(rd, e["layout"])):
                entries[e["file"]] = e
    groups = collections.OrderedDict()
    stats = Counter()
    with ProcessPoolExecutor(max(2, min(8, (os.cpu_count() or 4) // 2))) as ex:
        results = list(ex.map(analyse, [(rd, f, e) for f, e in sorted(entries.items())], chunksize=8))
    for file, e, s_, recs in results:
        stats.update(s_)
        for key, f, detail, text, o, worst in recs:
            g = groups.setdefault(key, dict(kind=f["kind"], file=f.get("file", ""), path=f.get("path", ""),
                                            name=f.get("name", ""), type=f.get("type", ""), details=Counter(),
                                            texts=Counter(), states=[], variants=Counter(),
                                            worst=99 if f["kind"] == "contrast" else 0))
            g["details"][detail] += 1
            if text:
                g["texts"][text[:80]] += 1
            g["states"].append(o)
            g["variants"][variant(e)] += 1
            g["worst"] = min(g["worst"], worst) if f["kind"] == "contrast" else max(g["worst"], worst)
    findings = []
    for g in groups.values():
        n = len({s["shot"] for s in g["states"]})
        findings.append(dict(kind=g["kind"], severity=5 if style_owned(g) else SEVERITY[g["kind"]],
                             component=g["name"], type=g["type"], qml_file=short(g["file"]), qml_url=g["file"],
                             path=g["path"], details=dict(g["details"]), texts=dict(g["texts"]),
                             states=n, variants=dict(g["variants"]), worst=g["worst"],
                             occurrences=g["states"]))
    findings.sort(key=lambda f: (f["severity"], -f["states"], f["qml_file"], f["component"]))
    json.dump(dict(round=os.path.basename(rd), states_checked=len(entries), stats=dict(stats), findings=findings),
              open(os.path.join(rd, "layout.json"), "w"), indent=1)
    write_md(rd, entries, stats, findings)
    print(f"LAYOUT.md: {len(findings)} grouped findings from {stats['raw']} raw ({stats['allowlisted']} allowlisted), "
          f"{len(entries)} states")


NAME_AREAS = (("library", ("libraryRowTitle",)), ("now playing", ("nowPlaying*",)))


def name_shares(rd, entries):
    """At a large font (the font24 shots of the main window), how much of each ordinary name is shown: for every
    library row and now-playing text, its share of characters kept (1 when shown whole; the elided ones from the
    probe's elidedText in the item's own font, mode and width). Elision of these is allowed by ELIDE_OK, which is
    why it does not show as a finding; this table shows what it costs."""
    import fnmatch
    rows = {}  # (window size, area, name, text) -> (share, shown, shots)
    for file, e in entries.items():
        if "font24" not in file:
            continue
        try:
            lj = json.load(open(os.path.join(rd, e["layout"])))
        except Exception:
            continue
        size = e.get("size", "")
        for w in lj.get("windows", [])[:1]:  # the main window
            elided = {}
            for f in w.get("findings", []):
                if f.get("kind") == "elided" and "share" in f:
                    elided[(f.get("name"), tuple(f.get("rect", [])))] = f
            for t in w.get("texts", []):
                for area, pats in NAME_AREAS:
                    if any(fnmatch.fnmatch(t.get("name", ""), p) for p in pats):
                        f = elided.get((t.get("name"), tuple(t.get("rect", []))))
                        full = f["text"] if f else t.get("text", "")
                        share = f["share"] if f else 1.0
                        shown = f.get("shown_text", "") if f else full
                        k = (size, area, t.get("name"), full)
                        if k not in rows or share < rows[k][0]:
                            rows[k] = (share, shown, rows.get(k, (0, 0, set()))[2] | {file})
                        else:
                            rows[k][2].add(file)
    if not rows:
        return []
    L = ["## Names at a large font (Noto Sans 24 pt): the share of each name shown\n",
         "Library rows and now-playing texts in every 24 pt shot of the main window. **Share** = the characters "
         "kept of the whole name (the ellipsis aside), from the probe's `QFontMetricsF::elidedText` in the item's "
         "own font, elide mode and width; 100 % = shown whole. Their elision is allowed (`ELIDE_OK`), so it is not "
         "a finding above: this is what it costs.\n"]
    for size in sorted({k[0] for k in rows}, reverse=True):
        L.append(f"### Window {size}\n")
        L.append("| area | names | shown whole | median share | under 50 % |\n|---|---|---|---|---|")
        for area, _ in NAME_AREAS:
            vals = sorted(v[0] for k, v in rows.items() if k[0] == size and k[1] == area)
            if vals:
                L.append(f"| {area} | {len(vals)} | {sum(v >= 0.999 for v in vals)} | {100 * vals[len(vals) // 2]:.0f} % | "
                         f"{sum(v < 0.5 for v in vals)} |")
        L.append("\n| area | name | shown | share | shots |\n|---|---|---|---|---|")
        for (sz, area, name, full), (share, shown, shots) in sorted(rows.items(), key=lambda kv: (kv[0][1], kv[1][0])):
            if sz == size and share < 0.999:
                L.append(f"| {area} | {full} | {shown} | {100 * share:.0f} % | {len(shots)} |")
        L.append("")
    return L


def drawn_caption(img, a, off, sc):
    """The caption's size as drawn: the ink width of the button's content in the shot against the caption's
    tight width in the control's own font. Returns (upper bound px, estimate px) or None: the upper bound takes all
    the ink as text (never under the truth: a caption at its font's size cannot be flagged), the estimate takes the
    icon and its spacing off."""
    import numpy as np
    tw = (a.get("tight") or [0, 0])[0]
    if tw <= 0 or not a.get("enabled", True):
        return None
    x, y, w, h = a["rect"]
    X0, X1 = int((x + 3) * sc + off[0]), int((x + w - 3) * sc + off[0])
    Y0, Y1 = int((y + h * 0.2) * sc + off[1]), int((y + h * 0.8) * sc + off[1])
    if X1 - X0 < 4 or Y1 - Y0 < 4 or X1 > img.width or Y1 > img.height:
        return None
    arr = np.asarray(img.crop((X0, Y0, X1, Y1)).convert("RGB")).astype(int)
    flat = arr.reshape(-1, 3)
    vals, counts = np.unique(flat, axis=0, return_counts=True)
    bg = vals[counts.argmax()]
    ink = np.abs(arr - bg).sum(axis=2) > 120
    cols = np.nonzero(ink.any(axis=0))[0]
    if not len(cols):
        return None
    wc = (cols[-1] - cols[0] + 1) / sc
    upper = a["px"] * wc / tw
    allowance = ((a.get("icon_w") or 16) + (a.get("spacing") or 4)) if a.get("icon") else 0
    est = a["px"] * max(0.0, wc - allowance) / tw
    return upper, est


def dialog_fonts(rd, entries):
    """in every 24 pt dialog shot, the pixel size (QFontInfo) of the dialog's labels against its actions'
    (buttons with a caption; the desktop style draws those through QStyle in the button's own font). An action
    more than 1 px smaller than the dialog's labels (their most common size) is flagged."""
    rows = {}
    for file, e in sorted(entries.items()):
        if "font24" not in file or e.get("area") != "dialog":
            continue
        try:
            data = json.load(open(os.path.join(rd, e["layout"])))
        except (OSError, ValueError):
            continue
        g = e.get("grabbed") or {}
        for w in data.get("windows", []):
            if g.get("kind") == "dialog":
                if w.get("cls") != g.get("cls"):
                    continue
                texts, acts = w.get("texts", []), w.get("actions", [])
            else:  # a dialog drawn in the main window: what is in its overlay (not under a dim)
                if not w.get("main"):
                    continue
                texts = [t for t in w.get("texts", []) if t.get("overlay") and not t.get("dimmed")]
                acts = [a for a in w.get("actions", []) if a.get("overlay") and not a.get("dimmed")]
            if not acts or not texts or any("px" not in t for t in texts):
                continue
            inside = lambda t: any(a["rect"][0] <= t["rect"][0] + t["rect"][2] / 2 <= a["rect"][0] + a["rect"][2] and
                                   a["rect"][1] <= t["rect"][1] + t["rect"][3] / 2 <= a["rect"][1] + a["rect"][3] for a in acts)
            labels = [t for t in texts if not inside(t)]
            # the dialog's body labels: not bold, not a title or heading (a page title is larger by design)
            body = [t["px"] for t in labels if not t.get("bold") and "Heading" not in (t.get("type") or "")
                    and not (t.get("name") or "").endswith("PageTitle") and "heading" not in (t.get("name") or "").lower()]
            if not body:
                k = (e["state"], e.get("size", ""))
                rows.setdefault(k, (None, acts, [], file, w.get("title") or w.get("cls")))
                continue
            lab = Counter(body).most_common(1)[0][0]
            from PIL import Image
            img = Image.open(os.path.join(rd, "shots", file))
            off = (g.get("image_offset") or [0, 0]) if g.get("kind") == "dialog" else [0, 0]
            sc = float(e.get("scale", 1) or 1)
            for a in acts:  # what is drawn: a caption of its own (a Text in the button), else the ink in the shot
                inner = [t["px"] for t in texts if a["rect"][0] <= t["rect"][0] + t["rect"][2] / 2 <= a["rect"][0] + a["rect"][2]
                         and a["rect"][1] <= t["rect"][1] + t["rect"][3] / 2 <= a["rect"][1] + a["rect"][3]]
                if inner:
                    a["drawn"], a["drawn_up"] = min(inner), min(inner)
                    continue
                m = drawn_caption(img, a, off, sc)
                if m:
                    a["drawn_up"], a["drawn"] = m
            small = [a for a in acts if a.get("drawn_up", lab) < lab - 1]
            k = (e["state"], e.get("size", ""))
            if k not in rows or len(small) > len(rows[k][2]):
                rows[k] = (lab, acts, small, file, w.get("title") or w.get("cls"))
    flagged = sum(1 for v in rows.values() if v[2])
    L = ["## Dialog fonts at 24 pt: actions against labels\n",
         "Every 24 pt dialog shot: the body labels' pixel size (`QFontInfo`, their most common size) against each "
         "action's caption **as drawn**: its ink width in the shot against the caption's width in the control's own "
         "font (a style can paint a caption in another font than the control's `font` says: the desktop style's "
         "ToolButton paints in the application's tool button font). **Flagged**: an action whose drawn size, even "
         "taking all its ink as text, is more than 1 px under its dialog's labels; ≈ takes the icon off.\n",
         "| dialog (state, window) | labels px | actions (px) | flagged |", "|---|---|---|---|"]
    for (state, size), (lab, acts, small, file, title) in sorted(rows.items(), key=lambda kv: (-len(kv[1][2]), kv[0])):
        def dr(a):
            if a.get("drawn") is None:
                return f"{a['text']} font {a.get('px', '?')}" + ("" if a.get("enabled", True) else " (disabled, not judged)")
            return f"{a['text']} drawn ≈{a['drawn']:.0f} (font {a.get('px', '?')})"
        acts_txt = ", ".join(sorted({dr(a) for a in acts}))
        L.append(f"| {state} ({size}, {title}) | {lab if lab is not None else 'no body labels'} | {acts_txt} | "
                 + (("**" + ", ".join(sorted({f"{a['text']} (drawn ≈{a['drawn']:.0f} px)" for a in small})) + "**") if small else "—")
                 + f" |")
    L.append("")
    return L, flagged, len(rows)


def write_md(rd, entries, stats, findings):
    L = [f"# Layout check ({os.path.basename(rd)})\n",
         f"Objective layout defects found by asking the live app, in each of the {len(entries)} captured states, "
         "right after its grab: the probe walks every shown window's item tree (main window, popups, viewer and "
         "dialog windows) with public Qt API, and `layout.py` filters, samples and groups. One line per "
         "(component, issue), with the states it occurs in. The method is in `tools/ui-capture/layout.py`; the "
         "allowlists (`ELIDE_OK`, `LAYOUT_IGNORE`, `CONTRAST_SKIP`) are in `states.py`. Machine-readable: "
         "`layout.json`.\n",
         "## Summary\n",
         "| severity | issue | grouped findings | state occurrences |", "|---|---|---|---|"]
    for sev in (1, 2, 3, 4, 5):
        fs = [f for f in findings if f["severity"] == sev]
        L.append(f"| {sev} | {TITLES[sev]} | {len(fs)} | {sum(f['states'] for f in fs)} |")
    df_lines, df_flagged, df_n = dialog_fonts(rd, entries)
    L.append(f"| — | Dialog actions smaller than their labels at 24 pt (> 1 px; below) | {df_flagged} of {df_n} dialogs | — |")
    L.append(f"\nRaw candidates {stats['raw']}, allowlisted {stats['allowlisted']}; texts checked for contrast "
             f"{stats['texts']}, of which {stats['contrast_unsampled']} on a busy background (image, plot) were not "
             f"sampled; {stats['contrast_dimmed']} under a modal popup's dim were skipped; {stats['elided_subpixel']} "
             "`truncated` flags with the whole text shown (a sub-pixel shortfall, no ellipsis drawn) were not counted.\n")
    for sev in (1, 2, 3, 4, 5):
        fs = [f for f in findings if f["severity"] == sev]
        L.append(f"## {sev}. {TITLES[sev]} ({len(fs)})\n")
        if not fs:
            L.append("None.\n")
            continue
        for i, f in enumerate(fs, 1):
            where = f"`{f['component']}`" + (f" ({f['type']})" if f["type"] != f["component"] else "") + \
                (f" in `{f['path']}`" if f["path"] else "") + (f", `{f['qml_file']}`" if f["qml_file"] else "")
            detail = "; ".join(f"{d}" + (f" ×{n}" if n > 1 else "") for d, n in Counter(f["details"]).most_common(3))
            var = ", ".join(f"{v}" + (f" ×{n}" if n > 1 else "") for v, n in Counter(f["variants"]).most_common(6))
            L.append(f"**{sev}.{i} {f['kind']}** — {where}: {detail}. **{f['states']} state(s)**: {var}.")
            if f["texts"]:
                L.append(f"  Text: " + "; ".join(f"“{t}”" for t in list(f["texts"])[:3]))
            shots = []
            # Occurrences whose window is the one in the shot first; others name the window they are in.
            for o in sorted(f["occurrences"], key=lambda o: not o.get("in_shot", True)):
                s = f"{o['state']} (`{o['shot']}`, " + (f"{o['window']}, " if o["window"] != "main window" else "") + \
                    f"rect {o['rect']}" + ("" if o.get("in_shot", True) else f"; in the {o['window']}, not this shot's window") + ")"
                if s not in shots:
                    shots.append(s)
            L.append("  States: " + "; ".join(shots[:6]) + (f"; … {len(shots) - 6} more" if len(shots) > 6 else ""))
            L.append("")
    L += df_lines
    L += name_shares(rd, entries)
    open(os.path.join(rd, "LAYOUT.md"), "w").write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
