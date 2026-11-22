#!/usr/bin/env python3
"""Unit tests of the focus packs: selecting several scopes, what each scope refers to, one folder per pack, and the
hard cap on a pack's images. Needs no KWin, app or shots.

    python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'
"""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
# (before the harness's modules load: shotlib and driver read these when imported)
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "unused", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image  # noqa: E402

import focus  # noqa: E402
import pack  # noqa: E402
import runner  # noqa: E402
import shotlib  # noqa: E402
import states  # noqa: E402

# the images of these areas are saved by macros, under state names of their own
MACRO_AREAS = {"details", "appicon"}


def tearDownModule():
    shutil.rmtree(_TMP)


def cells(rows):
    for c in rows:
        if isinstance(c, list):
            yield from cells(c)
        else:
            yield c


def shot_by(cell, theme, sessions):
    """Whether one of `sessions` shoots a sheet cell's shot: its area, state, theme, scale and window variant
    (as pack.custom_sheet picks it)."""
    area, state, opts = cell[0], cell[1], cell[4] if len(cell) > 4 else {}
    th, want, scale = opts.get("theme", theme), opts.get("variant", ""), opts.get("scale", 1)
    for s, a, t, *_ in states.STATES:
        if (a, t) != (area, state) or s not in sessions:
            continue
        kw = states.SESSIONS[s]
        if kw.get("scale", 1) != scale or th not in kw.get("themes", ("dark", "light")):
            continue
        size = "%dx%d" % tuple(kw.get("size", (1600, 1000)))
        name = size + kw.get("suffix_extra", "")
        if want:
            if want in name and ("font24" in want or "font24" not in name):
                return True
        elif size == "1600x1000" and "font24" not in name:
            return True
    return False


def recorded_by(name, theme, sessions):
    """Whether one of `sessions` records the transition `name`, or the recording it is another view of, in `theme`
    (light: only STRIP_BOTH_THEMES)."""
    for base, views in states.EXTRA_VIEWS.items():
        if name in {f"{base}-{suffix}" for suffix, _ in views}:
            name = base
    return ((theme == "dark" or name in states.STRIP_BOTH_THEMES)
            and any(s in sessions and (a, t) == ("motion", name) for s, a, t, *_ in states.STATES))


class ScopeSelection(unittest.TestCase):
    def test_one_scope(self):
        self.assertEqual(focus.resolve("dialogs"), ["dialogs"])

    def test_a_list_keeps_its_order_and_names_each_scope_once(self):
        self.assertEqual(focus.resolve("viewers, dialogs,viewers"), ["viewers", "dialogs"])

    def test_census_stands_for_the_seven_census_scopes(self):
        self.assertEqual(focus.resolve("census"), ["nowplaying-plot", "viz-interaction", "chrome", "library-search",
                                                   "dialogs", "viewers", "whole-app"])
        self.assertEqual(focus.resolve("dialogs,census,app-icon")[0], "dialogs")
        self.assertEqual(len(focus.resolve("dialogs,census,app-icon")), 8)

    def test_an_unknown_or_empty_list_is_refused(self):
        for bad in ("no-such-scope", "dialogs,no-such-scope", "", ","):
            with self.subTest(spec=bad), self.assertRaises(KeyError):
                focus.resolve(bad)

    def test_sessions_are_the_union_each_once(self):
        names = focus.resolve("census")
        ss = focus.sessions_of(names)
        self.assertEqual(len(ss), len(set(ss)))
        self.assertEqual(set(ss), {s for n in names for s in focus.SCOPES[n]["sessions"]})

    def test_the_pack_needs_every_scopes_recordings(self):
        both = pack.focus_strip_needs(["viewers", "library-search"])
        self.assertEqual(both, pack.focus_strip_needs(["viewers"]) | pack.focus_strip_needs(["library-search"]))
        self.assertIn("albumart-info-toggle", both)  # an image
        self.assertLessEqual({"search-open", "search-close"}, both)  # a pair's two halves
        self.assertIn("menu-open-close", both)  # facts only

    def test_another_view_of_a_recording_needs_that_recording(self):
        self.assertIn("first-play-from-idle", pack.focus_strip_needs(["whole-app"]))


class ScopeDefinitions(unittest.TestCase):
    def test_sessions_exist(self):
        for name, sc in focus.SCOPES.items():
            with self.subTest(scope=name):
                self.assertLessEqual(set(sc["sessions"]), set(states.SESSIONS))

    def test_each_plans_no_more_than_the_cap(self):
        for name, sc in focus.SCOPES.items():
            with self.subTest(scope=name):
                n = sum(1 for i in sc["images"] if not (len(i) > 5 and i[5].get("unless")))
                self.assertLessEqual(n, pack.FOCUS_MAX_IMAGES)

    def test_sheets_are_named_once_and_each_is_shown(self):
        seen = set()  # (one run writes every scope's sheets into the round's one sheets/ folder)
        for name, sc in focus.SCOPES.items():
            with self.subTest(scope=name):
                made = {(nm, th) for nm, _title, th, _rows in sc.get("sheets", [])}
                shown = {(nm, th) for kind, nm, th, *_ in sc["images"] if kind == "sheet"}
                self.assertEqual(made, shown)
                self.assertLessEqual(set(sc.get("contrast", {})), {nm for nm, _th in made})
                self.assertFalse(seen & made)
                seen |= made

    def test_each_tile_is_shot_by_the_scopes_own_sessions(self):
        for name, sc in focus.SCOPES.items():
            for sheet, _title, theme, rows in sc.get("sheets", []):
                for c in cells(rows):
                    with self.subTest(scope=name, sheet=sheet, cell=str(c)[:80]):
                        if isinstance(c, dict):
                            self.assertTrue(recorded_by(c["strip"], c.get("theme", theme), sc["sessions"]))
                        elif c[0] not in MACRO_AREAS:
                            self.assertTrue(shot_by(c, theme, sc["sessions"]))

    def test_each_recording_is_made_by_the_scopes_own_sessions(self):
        pairs = {f"pair:{nm}": (a, b) for nm, a, b, *_ in states.STRIP_PAIRS}
        for name, sc in focus.SCOPES.items():
            used = [(i[1], i[2]) for i in sc["images"] if i[0] == "strip"]
            used += [(i[0], i[1]) for i in sc.get("facts_strips", [])]
            for nm, theme in used:
                for half in pairs.get(nm, (nm,)):
                    rec, _, th = half.partition("@")
                    with self.subTest(scope=name, recording=half):
                        self.assertTrue(recorded_by(rec, th or theme, sc["sessions"]))


class PackWriting(unittest.TestCase):
    def setUp(self):
        self.rd = tempfile.mkdtemp(prefix="round-", dir=_TMP)
        os.makedirs(os.path.join(self.rd, "strips"))
        os.makedirs(os.path.join(self.rd, "sheets"))
        self.strips = []

        def facts(rd, *a, out="CORE-FACTS.md", **kw):
            with open(os.path.join(rd, out), "w") as f:
                f.write("facts\n")

        for p in (mock.patch.object(pack, "write_core_facts", facts),
                  mock.patch.object(pack, "source_notes", lambda sc: [])):
            p.start()
            self.addCleanup(p.stop)

    def scope(self, name, count, size=(64, 48)):
        """A scope of `count` recordings, each with its merged image on disk."""
        images = []
        for k in range(count):
            nm = f"{name}-{k}"
            Image.new("RGB", size).save(os.path.join(self.rd, "strips", nm + ".png"))
            self.strips.append(dict(name=nm, theme="dark", file=nm + ".png", size=list(size), metrics=None))
            images.append(("strip", nm, "dark", "shows", "look"))
        return dict(title=name, sessions=[], images=images)

    def write(self, scopes, *names):
        fails = []
        with mock.patch.dict(focus.SCOPES, scopes), contextlib.redirect_stdout(io.StringIO()):
            for nm in names:
                fails += pack.write_focus(self.rd, "", nm, [], [], self.strips, [])
        return fails

    def images(self, name):
        return sorted(f for f in os.listdir(os.path.join(self.rd, "focus-build", name)) if f.endswith(".png"))

    def test_each_scope_gets_a_folder_of_its_own(self):
        scopes = {"one": self.scope("one", 2), "two": self.scope("two", 3)}
        self.assertEqual(self.write(scopes, "one", "two"), [])
        self.assertEqual(self.images("one"), ["F01_one-0.png", "F02_one-1.png"])
        self.assertEqual(len(self.images("two")), 3)
        for name in scopes:
            for doc in ("FOCUS.md", "FOCUS-FACTS.md"):
                path = os.path.join(self.rd, "focus-build", name, doc)
                self.assertTrue(os.path.exists(path), path)
                self.assertIn(path, pack.round_docs(self.rd))  # (so the privacy check reads it)

    def test_writing_one_scope_again_leaves_the_others(self):
        scopes = {"one": self.scope("one", 2), "two": self.scope("two", 3)}
        self.write(scopes, "one", "two")
        self.assertEqual(self.write(scopes, "two"), [])
        self.assertEqual(len(self.images("one")), 2)

    def test_a_pack_at_the_cap_passes(self):
        self.assertEqual(self.write({"full": self.scope("full", pack.FOCUS_MAX_IMAGES)}, "full"), [])

    def test_a_pack_over_the_cap_fails(self):
        fails = self.write({"fat": self.scope("fat", pack.FOCUS_MAX_IMAGES + 1)}, "fat")
        self.assertEqual(len(fails), 1)
        self.assertIn("over the cap", fails[0])

    def test_neither_frame_sheets_count_toward_the_cap(self):
        scope = self.scope("full", pack.FOCUS_MAX_IMAGES)
        Image.new("RGB", (64, 48)).save(os.path.join(self.rd, "strips", "neither.png"))
        extra = dict(name="full-0-neither", theme="dark", file="neither.png", size=[64, 48])
        with mock.patch.object(pack, "neither_sheet", lambda rd, sd, nm, th, m: extra if nm == "full-0" else None):
            fails = self.write({"full": scope}, "full")
        self.assertIn("over the cap", " ".join(fails))

    def test_an_image_over_the_size_cap_fails(self):
        path = os.path.join(self.rd, "big.png")
        Image.new("RGB", (1300, 900)).save(path)  # 1.17 MP
        self.assertIn("over 1.15 MP", " ".join(pack.focus_limits("big", [path])))
        Image.new("RGB", (1250, 900)).save(path)  # 1.125 MP
        self.assertEqual(pack.focus_limits("big", [path]), [])


class DialogsPack(unittest.TestCase):
    sc = focus.SCOPES["dialogs"]

    def tiles(self, sheet=None):
        return [c for nm, _title, _th, rows in self.sc["sheets"] if sheet in (None, nm) for row in rows
                for c in cells(row)]

    def test_it_stays_at_the_cap_without_recordings_that_could_add_an_image(self):
        self.assertEqual(len(self.sc["images"]), pack.FOCUS_MAX_IMAGES)
        self.assertEqual({kind for kind, *_ in self.sc["images"]}, {"sheet"})

    def test_the_visualization_page_and_edit_tags_from_a_1000_px_window_are_in_it(self):
        got = {(c[1], (c[4] if len(c) > 4 else {}).get("variant", "")) for c in self.tiles() if isinstance(c, tuple)}
        self.assertIn(("preferences-visualization", ""), got)
        self.assertIn(("tag-editor-batch", "1000x720"), got)
        narrow = states.SESSIONS["narrow"]
        self.assertEqual(tuple(narrow["size"]), (1000, 720))
        self.assertIn(("narrow", "tag-editor"), states.DLG_FACTS)  # (the at-a-glance table's window and overflow)

    def test_the_shortcuts_at_rest_filtered_and_refused_share_one_crop(self):
        want = {"keyboard-shortcuts", "keyboard-shortcuts-filter", "keyboard-shortcuts-refused"}
        crops = {c[1]: (c[2], c[4].get("part")) for c in self.tiles()
                 if isinstance(c, tuple) and c[1] in want and not c[4].get("variant")}  # (not the one at 24 pt)
        self.assertEqual(set(crops), want)
        self.assertEqual(len(set(crops.values())), 1)

    def test_the_pointer_leaves_before_the_states_compared_with_the_one_at_rest(self):
        steps = next(s for _se, _a, st, s, _d in states.STATES if st == "_shortcuts-unfilter")
        self.assertIn("window keyboardShortcutsDialog | leave", steps)

    def test_the_itunes_failure_is_cut_to_what_changes_and_preview(self):
        frames = [c for c in self.tiles("dlg-itunes-controls") if isinstance(c, dict)]
        self.assertEqual([(c["item"], c["include"][2]) for c in frames],
                         [("changed", "itunesArtworkCurrentPreviewButton")])

    def test_rename_files_is_at_1_to_1(self):
        self.assertFalse([c for c in self.tiles("dlg24-rename") if c[4].get("fit")])


class VizInteractionPack(unittest.TestCase):
    sc = focus.SCOPES["viz-interaction"]

    def test_each_pan_is_a_1_to_1_sheet_of_both_lanes_with_the_recordings_facts(self):
        sheets = {nm: rows for nm, _title, _th, rows in self.sc["sheets"]}
        for rec in ("vi-pan-waveform", "vi-pan-spectrogram"):
            with self.subTest(recording=rec):
                image = next(i for i in self.sc["images"] if i[1] == f"{rec}-frames")
                self.assertEqual((image[0], image[5]["recording"]), ("sheet", rec))
                self.assertIn(rec, pack.focus_strip_needs(["viz-interaction"]))  # (measured in a focus-only pack too)
                self.assertEqual(states.STRIP_VIEW[rec], "spectrogramPane")
                self.assertEqual(states.MOTION_FOCUS[rec]["lanes"], "spectrogramPane")
                for c in cells(sheets[f"{rec}-frames"]):
                    name, _, frac = c["item"].partition(":")
                    x, y, w, h = map(float, frac.split(","))
                    self.assertEqual((name, c["zoom"], c.get("fit")), ("spectrogramPane", 1, None))
                    self.assertLess(x, 0.3 - 20 / 1239)  # (the drag's two ends, 70 % and 30 %, with the time box)
                    self.assertGreater(x + w, 0.7 + 20 / 1239)
                    self.assertLess(y, 0.1)  # both lanes, down to the time on the bottom edge
                    self.assertAlmostEqual(y + h, 1.0)

    def test_a_sheet_of_a_recordings_frames_carries_its_facts(self):
        rd = tempfile.mkdtemp(prefix="round-", dir=_TMP)
        x = dict(file="pan.png", title="A pan", size=[600, 400], mp=240000, theme="dark", group="pan", tiles=[],
                 one_to_one=True, recording="vi-pan")
        strips = [dict(name="vi-pan", theme="dark", metrics=dict(stand_in=True))]
        with mock.patch.object(pack, "strip_facts", lambda m: "the pan's facts"):
            pack.write_core_facts(rd, [("sheet", x)], strips, [], out="FACTS.md", ids=["F06"])
        with open(os.path.join(rd, "FACTS.md")) as f:
            self.assertIn("- The recording (vi-pan): the pan's facts", f.read())


class LibrarySearchPack(unittest.TestCase):
    sc = focus.SCOPES["library-search"]

    def test_the_typing_strip_is_shown_and_not_listed_as_not_shown(self):
        self.assertIn(("strip", "search-typing"), [i[:2] for i in self.sc["images"]])
        self.assertNotIn("search-typing", [f[0] for f in self.sc["facts_strips"]])
        self.assertLess(len(self.sc["images"]), pack.FOCUS_MAX_IMAGES)  # (room for a pair's "neither" frames)

    def test_the_typing_strips_real_speed_rows_show_the_field_and_the_rows_under_it(self):
        fx, fy, fw, fh = states.STRIP_VIEW["search-typing"]
        fo = states.MOTION_FOCUS["search-typing"]
        x, y, w, h = fo["rect"]
        self.assertLess(y, 120)  # the query's line (y 120-137 at 1600x1000)
        self.assertGreater(y + h, 300)  # several rows below it
        self.assertTrue(fx <= x and x + w <= fx + fw and fy <= y and y + h <= fy + fh)
        self.assertEqual(fo["b_lines"], 1)  # (room for the key frames)

    def test_a_focused_row_beside_the_scrollbar_stays_in_both_themes_with_the_panes_right_edge(self):
        rows = next(r for nm, _title, _th, r in self.sc["sheets"] if nm == "lib-density")
        edge = {}
        for c in cells([x for row in rows for x in row]):
            if c[1] == "density-scrolled":
                x, _y, w, _h = c[4]["part"]
                edge[c[4].get("theme", "dark")] = (c[2], x + w)
        self.assertEqual(edge, dict(dark=("subject", 1), light=("subject", 1)))
        self.assertIn(("sheet", "lib-density"), [i[:2] for i in self.sc["images"]])


class ChangeCrop(unittest.TestCase):
    """A recording's frames cut around all that changes after the action, with an item of a dialog shot's layout."""

    def setUp(self):
        self.rd = tempfile.mkdtemp(prefix="round-", dir=_TMP)
        rec = os.path.join(self.rd, "strips-raw", "fails_dark_1x_1600x1000")
        for d in ("sheets", "layout", rec):
            os.makedirs(os.path.join(self.rd, d), exist_ok=True)
        lines = ["mark\t1000\tnow ACTION"]
        for k in range(4):
            im = Image.new("RGB", (920, 604), (32, 35, 38))
            if k == 0:
                im.paste((255, 0, 0), (0, 590, 10, 600))  # (gone before the action: not in the crop)
            if k >= 2:
                im.paste((200, 200, 200), (300, 280, 400, 340))
            im.paste((0, 255, 0), (700, 40, 780, 80))  # Preview, which holds still
            im.save(os.path.join(rec, f"f{k}.png"))
            lines.append(f"frame\t{k}\t{(970, 990, 1030, 1200)[k]}\t0\tf{k}.png")
        with open(os.path.join(rec, "frames.tsv"), "w") as f:
            f.write("\n".join(lines) + "\n")
        with open(os.path.join(rec, "meta.json"), "w") as f:
            json.dump(dict(name="fails", theme="dark", size=[1600, 1000], crop=None, scale=1, opts="", items={}), f)
        with open(os.path.join(self.rd, "layout", "error.json"), "w") as f:
            json.dump(dict(windows=[dict(cls="Main"), dict(cls="Replace", actions=[
                dict(name="previewButton", rect=[700, 40, 80, 40])])]), f)
        self.entries = [dict(file="dialog_error_dark.png", area="dialog", state="error", theme="dark", scale=1,
                             size="1600x1000", desc="", layout="layout/error.json", grabbed=dict(cls="Replace"))]

    def sheet(self, item="previewButton", **kw):
        cell = dict(strip="fails", item="changed", include=("dialog", "error", item), times=[-5, 50], zoom=1, pad=8,
                    lh=16, gap=6, label="Frames", **kw)
        return pack.custom_sheet(self.rd, os.path.join(self.rd, "sheets"), self.entries, "fails", "Failing search",
                                 "dark", [[cell]])

    def test_the_crop_is_the_change_box_and_the_item(self):
        x = self.sheet()
        im = Image.open(os.path.join(self.rd, "sheets", x["file"])).convert("RGB")
        colours = {c: n for n, c in im.getcolors(1 << 20)}
        # (300,280)-(400,340) with Preview's (700,40)-(780,80), 8 px around: 496x316 a frame
        self.assertEqual(x["size"][0], 2 * 496 + 10 + 2 * pack.PAD)
        self.assertEqual(colours.get((0, 255, 0)), 2 * 80 * 40)
        self.assertEqual(colours.get((200, 200, 200)), 100 * 60)
        self.assertNotIn((255, 0, 0), colours)
        self.assertTrue(x["one_to_one"])

    def test_a_larger_change_is_shrunk_to_fit_and_says_so(self):
        x = self.sheet(fit=(248, 400))
        self.assertEqual((x["one_to_one"], x["tile_fit"]), (False, 0.5))
        self.assertEqual(x["size"][0], 2 * 248 + 10 + 2 * pack.PAD)

    def test_a_missing_item_fails_the_sheet(self):
        with self.assertRaisesRegex(LookupError, "no noSuchButton"):
            self.sheet("noSuchButton")


class SearchTooltipState(unittest.TestCase):
    def test_the_tooltip_state_hovers_a_row_that_has_a_tooltip(self):
        steps = next(s for _se, _a, st, s, _d in states.STATES if st == "result-hover")
        self.assertEqual(steps.split()[:2], ["hover", "@tooltipResultRow@0.15,0.5"])
        js = states.TARGETS["tooltipResultRow"]
        self.assertIn("typeof it.toolTipAdds!=='function'||it.toolTipAdds()", js)  # (an app without it: any row)
        self.assertIn("it.isResult", js)

    def test_the_packs_crop_reaches_a_tooltip_under_the_first_rows(self):
        sheet = next(r for nm, _t, _th, r in focus.SCOPES["library-search"]["sheets"] if nm == "search-more")
        x, y, w, h = next(c[4]["part"] for c in cells([c for row in sheet for c in row]) if c[1] == "result-hover")
        self.assertEqual((x, y, w), (0, 0, 1))
        # (the 581 px palette: a row ending 169 px down, the tooltip's two lines 3 px under it)
        self.assertGreaterEqual(h * 581, 169 + 3 + 52)

    def test_the_target_is_resolved_by_its_name(self):
        s = mock.Mock(**{"ev.return_value": "400 240 800 32"})
        self.assertEqual(runner.Runner(s).point("@tooltipResultRow@0.15,0.5"), (520.0, 256.0))


class Captions(unittest.TestCase):
    """What is said of the Menu key: the windowed album art viewer and the search palette take it, as shortcuts."""

    def test_no_state_or_note_says_a_view_takes_no_menu_key(self):
        said = [d for *_, d in states.STATES] + [" ".join(note) for note in states.NOT_CAPTURED]
        self.assertFalse([t for t in said if "no Menu key" in t])

    def test_the_windowed_viewers_caption_says_both_keys_open_its_menu(self):
        desc = next(d for _se, _a, st, _s, d in states.STATES if st == "context-albumart-viewer-windowed")
        self.assertIn("the Menu key and Shift+F10 open it too", desc)


class SystemFacts(unittest.TestCase):
    def test_contrast_layout_and_focus_order_are_summarised_for_the_packs_shots(self):
        rd = tempfile.mkdtemp(prefix="round-", dir=_TMP)
        with open(os.path.join(rd, "CONTRAST.md"), "w") as f:
            f.write("| Body text | dark | #ffffff | #000000 | **21.00** | AA | - | x |\n"
                    "| Hint text | light | #999999 | #ffffff | **2.85** | fails AA | - | x |\n")
        finding = dict(kind="contrast", severity=4, component="hintLabel", worst=2.85,
                       occurrences=[dict(shot="shots/shown.png"), dict(shot="shots/other.png")])
        behind = dict(kind="contrast", severity=4, component="behindLabel", worst=1.5,
                      occurrences=[dict(shot="shots/shown.png", in_shot=False)])
        elsewhere = dict(kind="elided", severity=3, component="unseenLabel", worst=0.5,
                         occurrences=[dict(shot="shots/other.png")])
        with open(os.path.join(rd, "layout.json"), "w") as f:
            json.dump(dict(findings=[finding, elsewhere, behind]), f)
        with open(os.path.join(rd, "a11y_dark_1x_1600x1000.jsonl"), "w") as f:
            f.write(json.dumps(dict(stop="Tab 1", item="playButton", role="Button", name="Play")) + "\n")
        text = "\n".join(pack.system_lead(rd, [("sheet", dict(tiles=[dict(file="shown.png")]))], ["F01"]))
        self.assertIn("| Hint text | light |", text)
        self.assertNotIn("| Body text |", text)
        self.assertIn("| contrast | hintLabel | 2.85 | 1 | F01 |", text)
        self.assertNotIn("unseenLabel", text)
        self.assertNotIn("behindLabel", text)
        self.assertIn("| Tab 1 | `playButton` | Button | Play |", text)


class FocusCrop(unittest.TestCase):
    def test_a_focus_tile_is_cut_around_the_recorded_item(self):
        rd = tempfile.mkdtemp(prefix="round-", dir=_TMP)
        for d in ("shots", "sheets"):
            os.makedirs(os.path.join(rd, d))
        im = Image.new("RGB", (1600, 1000), (0, 0, 0))
        im.paste((255, 0, 0), (1540, 400, 1580, 440))  # the focused item, near the right edge
        im.save(os.path.join(rd, "shots", "focus.png"))
        entry = dict(file="focus.png", area="focus", state="tab-01", theme="dark", scale=1, size="1600x1000",
                     desc="", focus=[1540, 400, 40, 40])
        seen = []
        real = Image.Image.crop
        with mock.patch.object(pack, "_redact", lambda _rd, _e, img: img), \
                mock.patch.object(Image.Image, "crop", lambda img, box=None: seen.append(box) or real(img, box)):
            pack.custom_sheet(rd, os.path.join(rd, "sheets"), [entry], "walk", "Walk", "dark",
                              [[("focus", "tab-01", "focus", "Tab 1", dict(around=(380, 120)))]])
        self.assertIn((1220, 360, 1600, 480), seen)
        seen.clear()
        big = dict(entry, focus=[300, 400, 1200, 500])  # larger than the box: the box stays, at the item's start
        with mock.patch.object(pack, "_redact", lambda _rd, _e, img: img), \
                mock.patch.object(Image.Image, "crop", lambda img, box=None: seen.append(box) or real(img, box)):
            pack.custom_sheet(rd, os.path.join(rd, "sheets"), [big], "walk-big", "Walk", "dark",
                              [[("focus", "tab-01", "focus", "Tab 1", dict(around=(380, 120)))]])
        self.assertIn((292, 392, 672, 512), seen)


class PointerSteps(unittest.TestCase):
    def test_middle_button_and_leave_steps_reach_the_probe(self):
        sent = []
        s = mock.Mock(cmd=lambda c, **kw: sent.append(c))
        runner.Runner(s).run_steps("mpress xy:10,20 | mdmove xy:30,20 | mrelease xy:30,20 | mclick xy:40,20 | leave")
        self.assertEqual(sent, ["mpress 10.0 20.0", "mdmove 30.0 20.0", "mrelease 30.0 20.0", "mclick 40.0 20.0", "leave"])

    def test_a_held_key_is_a_press_alone_until_its_release(self):
        sent = []
        runner.Runner(mock.Mock(cmd=lambda c, **kw: sent.append(c))).run_steps("keydown Space | keyup Space")
        self.assertEqual(sent, ["keydown 32 0", "keyup 32 0"])

    def test_kkey_is_a_real_key_through_kwin(self):
        s = mock.Mock()
        runner.Runner(s).run_steps("kkey Menu")
        s.kwin_key.assert_called_once_with("Menu")


class DialogMenuGrab(unittest.TestCase):
    """A menu open in a dialog is a window of its own (#642), which a capture of the dialog's window leaves out."""

    def grab(self, popups, captured=(600, 400)):
        s = shotlib.Session.__new__(shotlib.Session)
        s.suffix, s.tag, s.scale, s.size, s.renderer, s.index = "dark_1x_64x48", "dark", 1, (64, 48), "test", []
        s.settle_opacity = lambda win="", timeout=1.2: None
        s.layout_path = lambda file: None
        s.windows = lambda: []
        s.popups = lambda win="": popups
        s.kwin_frame = lambda title: (10.0, 20.0, 600.0, 400.0)
        s.kwin_capture_area = mock.Mock(return_value=captured)
        s.kwin_capture = mock.Mock(return_value=(600, 400))
        with contextlib.redirect_stdout(io.StringIO()):
            s.grab("dialog", "folder-menu", "", dialog_window=dict(cls="Dialog", title="Configure", w=600, h=370, parent=0))
        return s

    def test_with_a_menu_the_screen_inside_the_dialogs_frame_is_captured(self):
        s = self.grab([dict(opened=True, cls="AppMenu_QMLTYPE_1")])
        self.assertEqual(s.kwin_capture_area.call_args[0][1], (10.0, 20.0, 600.0, 400.0))
        s.kwin_capture.assert_not_called()
        self.assertIn("open menu", s.index[-1]["desc"])

    def test_a_failed_screen_capture_fails_rather_than_leaving_the_menu_out(self):
        with self.assertRaisesRegex(RuntimeError, "menu would be missing"):
            self.grab([dict(opened=True, cls="AppMenu_QMLTYPE_1")], captured=None)

    def test_a_folder_menu_state_without_its_menu_fails(self):
        s = mock.Mock(**{"dialog_window.return_value": dict(cls="Dialog"), "popups.return_value": []})
        with mock.patch.object(states.time, "sleep"), \
                mock.patch.object(states.time, "time", side_effect=iter(range(100))):
            with self.assertRaisesRegex(RuntimeError, "no menu open"):
                states.m_dialogmenu(mock.Mock(s=s), "")

    def test_without_one_the_dialogs_window_is_captured(self):
        s = self.grab([])
        s.kwin_capture_area.assert_not_called()
        s.kwin_capture.assert_called_once()


if __name__ == "__main__":
    unittest.main()
