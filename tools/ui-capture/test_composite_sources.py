#!/usr/bin/env python3
"""Unit tests of the privacy evidence a composite image carries (#605): every image a macro cuts from a grab lists
that grab's layout walk, an image drawn with no app pixels says so, and the pack fails an image that has neither.

Needs no KWin, app or shots: a fake session writes a blank grab and its walk into a temporary round. Run with
`python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import json
import os
import shutil
import sys
import tempfile
import types
import unittest

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
# (before the harness's modules load: shotlib reads the round's folder when imported)
os.environ["SHOT_WORK"] = os.path.join(_TMP, "work")
os.environ["SHOT_OUT"] = os.path.join(_TMP, "round", "shots")
os.environ["SHOT_APP"] = os.path.join(_TMP, "no-app")
os.environ.pop("SHOT_LAYOUT", None)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image  # noqa: E402

import pack  # noqa: E402
import runner  # noqa: E402
import shotlib  # noqa: E402
import states  # noqa: E402

ROUND = os.path.dirname(os.environ["SHOT_OUT"])


def tearDownModule():
    shutil.rmtree(_TMP)


class FakeSession(shotlib.Session):
    """A session without an app: each grab writes a blank image and a layout walk with one text, which names the
    home folder when `home_text` is set."""

    def __init__(self, home_text=False):  # (no App: Session.__init__ starts one)
        self.tag, self.scale, self.size, self.suffix, self.renderer = "dark", 1, (64, 48), "dark_1x_64x48", "test"
        self.index = []
        self.composite_sources = []
        self.home_text = home_text

    def settle_opacity(self, win="", timeout=1.2):
        pass

    def cmd(self, c, timeout=0):
        verb, path, *walk = c.split()
        assert verb == "grab", c
        Image.new("RGB", (64, 48), (40, 40, 40)).save(path)
        if walk:
            with open(walk[0], "w") as f:
                json.dump({"windows": [{"title": "Ferrous", "texts": [{"text": "a row", "home": self.home_text}]}]}, f)
        return "ok"


def m_two_from_one(r, arg):
    """Two images cut from one grab, as m_motif saves its mark and its panel."""
    im = states._grab_window(r, "test-one")
    states._save_shot(r, im.crop((0, 0, 32, 24)), "details", "test-first", "the first crop")
    states._save_shot(r, im, "details", "test-second", "the second crop")


def m_drawn(r, arg):
    """An image drawn without app pixels, declared so (as the icon sheets are)."""
    r.s.draws_without_app_pixels()
    states._save_shot(r, Image.new("RGB", (16, 16)), "icon", "test-drawn", "drawn from nothing")


def m_undeclared(r, arg):
    """An image from no grab whose macro does not say it has no app pixels."""
    states._save_shot(r, Image.new("RGB", (16, 16)), "details", "test-undeclared", "from nowhere")


def m_reused(r, arg):
    """An image cut from an indexed shot of an earlier state."""
    r.s.source_shot("main_test-window_dark_1x_64x48.png")
    states._save_shot(r, Image.new("RGB", (16, 16)), "details", "test-reused", "from a shot")


TEST_MACROS = dict(test_two_from_one=m_two_from_one, test_drawn=m_drawn, test_undeclared=m_undeclared,
                   test_reused=m_reused)


class CompositeSources(unittest.TestCase):
    def setUp(self):
        if os.path.isdir(ROUND):
            shutil.rmtree(ROUND)
        os.makedirs(os.environ["SHOT_OUT"])
        states.MACROS.update(TEST_MACROS)
        self.addCleanup(lambda: [states.MACROS.pop(k) for k in TEST_MACROS])

    def run_macros(self, session, *names):
        r = types.SimpleNamespace(s=session)
        for name in names:
            runner.Runner.step_macro(r, name)
        return session.index

    def test_every_image_from_a_grab_lists_its_walk(self):
        entries = self.run_macros(FakeSession(), "test_two_from_one")
        self.assertEqual([e["file"] for e in entries],
                         ["details_test-first_dark_1x_64x48.png", "details_test-second_dark_1x_64x48.png"])
        walk = "closeup-raw/test-one_dark_1x_64x48.layout.json"
        for e in entries:
            self.assertEqual(e["sources"], [walk], e["file"])
            self.assertNotIn("no_app_pixels", e)

    def test_a_later_image_from_a_grab_is_checked_without_the_first(self):
        # The issue's case: the first image is missing, and the shared walk names the home folder.
        entries = self.run_macros(FakeSession(home_text=True), "test_two_from_one")
        os.remove(os.path.join(os.environ["SHOT_OUT"], entries[0]["file"]))
        fails, counts = pack.privacy_evidence(ROUND, entries)
        self.assertEqual(len(fails), 1, fails)
        self.assertIn("details_test-second_dark_1x_64x48.png is unverified", fails[0])
        self.assertIn("home-folder text", fails[0])
        self.assertEqual(counts["composites"], 0)

    def test_images_from_a_clean_grab_pass(self):
        entries = self.run_macros(FakeSession(), "test_two_from_one")
        fails, counts = pack.privacy_evidence(ROUND, entries)
        self.assertEqual(fails, [])
        self.assertEqual(counts["composites"], 2)

    def test_an_image_drawn_without_app_pixels_is_marked(self):
        entries = self.run_macros(FakeSession(), "test_drawn")
        self.assertEqual(entries[0]["sources"], [])
        self.assertIs(entries[0]["no_app_pixels"], True)
        fails, counts = pack.privacy_evidence(ROUND, entries)
        self.assertEqual(fails, [])
        self.assertEqual(counts["drawn"], 1)

    def test_an_image_from_no_grab_and_no_marker_fails_its_state(self):
        with self.assertRaisesRegex(RuntimeError, "details_test-undeclared_dark_1x_64x48.png"):
            self.run_macros(FakeSession(), "test_undeclared")
        # (and writes no image: on a partial rerun it would replace an earlier one that keeps its old entry)
        self.assertFalse(os.path.exists(os.path.join(os.environ["SHOT_OUT"], "details_test-undeclared_dark_1x_64x48.png")))

    def test_sources_do_not_carry_into_the_next_macro(self):
        entries = self.run_macros(FakeSession(), "test_two_from_one", "test_drawn")
        self.assertEqual(entries[-1]["sources"], [])
        self.assertIs(entries[-1]["no_app_pixels"], True)

    def test_a_reused_shot_is_a_source(self):
        entries = self.run_macros(FakeSession(), "test_reused")
        self.assertEqual(entries[0]["sources"], ["shot:main_test-window_dark_1x_64x48.png"])

    def test_the_pack_fails_an_entry_with_no_sources_and_no_marker(self):
        Image.new("RGB", (8, 8)).save(os.path.join(os.environ["SHOT_OUT"], "a.png"))
        Image.new("RGB", (8, 8)).save(os.path.join(os.environ["SHOT_OUT"], "b.png"))
        fails, counts = pack.privacy_evidence(ROUND, [dict(file="a.png", sources=[]),
                                                      dict(file="b.png", sources=[], no_app_pixels=True)])
        self.assertEqual(len(fails), 1, fails)
        self.assertIn("a.png is unverified", fails[0])
        self.assertEqual(counts["drawn"], 1)


if __name__ == "__main__":
    unittest.main()
