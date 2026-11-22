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
    """Whether one of `sessions` records the transition `name` in `theme` (light: only STRIP_BOTH_THEMES)."""
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
        return sorted(f for f in os.listdir(os.path.join(self.rd, "focus", name)) if f.endswith(".png"))

    def test_each_scope_gets_a_folder_of_its_own(self):
        scopes = {"one": self.scope("one", 2), "two": self.scope("two", 3)}
        self.assertEqual(self.write(scopes, "one", "two"), [])
        self.assertEqual(self.images("one"), ["F01_one-0.png", "F02_one-1.png"])
        self.assertEqual(len(self.images("two")), 3)
        for name in scopes:
            for doc in ("FOCUS.md", "FOCUS-FACTS.md"):
                path = os.path.join(self.rd, "focus", name, doc)
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


class SystemFacts(unittest.TestCase):
    def test_contrast_layout_and_focus_order_are_summarised_for_the_packs_shots(self):
        rd = tempfile.mkdtemp(prefix="round-", dir=_TMP)
        with open(os.path.join(rd, "CONTRAST.md"), "w") as f:
            f.write("| Body text | dark | #ffffff | #000000 | **21.00** | AA | - | x |\n"
                    "| Hint text | light | #999999 | #ffffff | **2.85** | fails AA | - | x |\n")
        finding = dict(kind="contrast", severity=4, component="hintLabel", worst=2.85,
                       occurrences=[dict(shot="shots/shown.png"), dict(shot="shots/other.png")])
        elsewhere = dict(kind="elided", severity=3, component="unseenLabel", worst=0.5,
                         occurrences=[dict(shot="shots/other.png")])
        with open(os.path.join(rd, "layout.json"), "w") as f:
            json.dump(dict(findings=[finding, elsewhere]), f)
        with open(os.path.join(rd, "a11y_dark_1x_1600x1000.jsonl"), "w") as f:
            f.write(json.dumps(dict(stop="Tab 1", item="playButton", role="Button", name="Play")) + "\n")
        text = "\n".join(pack.system_lead(rd, [("sheet", dict(tiles=[dict(file="shown.png")]))], ["F01"]))
        self.assertIn("| Hint text | light |", text)
        self.assertNotIn("| Body text |", text)
        self.assertIn("| contrast | hintLabel | 2.85 | 1 | F01 |", text)
        self.assertNotIn("unseenLabel", text)
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


if __name__ == "__main__":
    unittest.main()
