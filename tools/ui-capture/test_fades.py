#!/usr/bin/env python3
"""Unit tests of the fade measures (motion_strip): a whole window that dips under the darker end of a transition,
and a cross-fade that falls under the one-layer mix of its two ends, as a popup does whose floor and content each
fade at its opacity apart. A true one-layer fade between unlike pictures passes both.

Needs no KWin, app or shots: the recordings are synthetic. Run with
`python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import os
import shutil
import sys
import tempfile
import unittest

import numpy as np
from PIL import Image

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import motion_strip  # noqa: E402

STEP = 17.0
SIZE = (240, 400)  # h, w
OPACITY = [0, 0, 0.013, 0.054, 0.12, 0.218, 0.344, 0.49, 0.647, 0.776, 0.87, 0.942, 0.986, 1, 1]


def tearDownModule():
    shutil.rmtree(_TMP)


def picture(seed, colour):
    """A window's picture: blocks of content in shades around `colour`."""
    h, w = SIZE
    shade = np.random.default_rng(seed).uniform(0.4, 1.6, size=(h // 20, w // 20)).repeat(20, 0).repeat(20, 1)
    return np.clip(np.array(colour, dtype=np.float32) * shade[..., None], 0, 255).astype(np.float32)


MAIN = picture(1, (30, 35, 40))
PLOT = picture(2, (80, 10, 70))  # (about the main window's luminance, another picture)
ART = picture(3, (120, 90, 110))  # (much brighter than the main window)
FLOOR = np.array([12, 4, 30], dtype=np.float32)


def one_layer(under, over, a):
    return under * (1 - a) + over * a


def apart(floors):
    """A popup whose `floors` and whose content each fade at its opacity, apart."""
    def frame(under, over, a):
        out = under
        for _ in range(floors):
            out = out * (1 - a) + FLOOR * a
        return out * (1 - a) + over * a
    return frame


def recording(fade, under, over, opacities=OPACITY):
    return [Image.fromarray(np.rint(fade(under, over, a)).astype(np.uint8)) for a in opacities]


def measured(ims, cross_fade=True, crop=None, name="viewer-open-windowed"):
    d = tempfile.mkdtemp(dir=_TMP)
    frames = [dict(t=1000 + (k - 1) * STEP) for k in range(len(ims))]
    meta = dict(name=name, theme="dark", desc="a fade", crop=crop, scale=1, items={})
    h, w = SIZE
    m, cap, _draw = motion_strip.measure(frames, ims, 1000.0, (0, 0, w, h), [0, len(ims) // 2, len(ims) - 1], meta, d,
                                         focus=dict(cross_fade=True) if cross_fade else None)
    return m, cap


class OneLayer(unittest.TestCase):
    def test_a_one_layer_fade_between_unlike_pictures_passes(self):
        for what, under, over in (("alike in luminance", MAIN, PLOT), ("to brighter", MAIN, ART),
                                  ("to darker", ART, MAIN), ("from white", np.full_like(MAIN, 245.0), PLOT)):
            for opacities in (OPACITY, OPACITY[::-1]):
                with self.subTest(what, closing=opacities is not OPACITY):
                    m, cap = measured(recording(one_layer, under, over, opacities))
                    self.assertFalse(m["window_dip"]["flag"], m["window_dip"])
                    self.assertGreaterEqual(m["window_dip"]["low"], 0.99)
                    self.assertFalse(m["mix_shortfall"]["flag"], m["mix_shortfall"])
                    self.assertLessEqual(m["mix_shortfall"]["short"], 0.01)
                    self.assertNotIn("FAIL", cap)


class LayersApart(unittest.TestCase):
    def test_two_floors_dip_under_the_darker_end(self):
        for opacities in (OPACITY, OPACITY[::-1]):
            with self.subTest(closing=opacities is not OPACITY):
                m, cap = measured(recording(apart(2), MAIN, PLOT, opacities))
                self.assertTrue(m["window_dip"]["flag"], m["window_dip"])
                self.assertLess(m["window_dip"]["low"], 0.9)
                self.assertTrue(m["mix_shortfall"]["flag"], m["mix_shortfall"])
                self.assertIn("WINDOW DIP (FAIL)", cap)

    def test_a_brighter_end_hides_the_dip_and_not_the_shortfall(self):
        for opacities in (OPACITY, OPACITY[::-1]):
            with self.subTest(closing=opacities is not OPACITY):
                m, cap = measured(recording(apart(1), MAIN, ART, opacities))
                self.assertFalse(m["window_dip"]["flag"], m["window_dip"])
                self.assertTrue(m["mix_shortfall"]["flag"], m["mix_shortfall"])
                self.assertGreater(m["mix_shortfall"]["short"], 0.05)
                self.assertTrue(0.2 < m["mix_shortfall"]["blend"] < 0.8, m["mix_shortfall"])
                self.assertIn("UNDER THE ONE-LAYER MIX (FAIL)", cap)
                self.assertNotIn("WINDOW DIP", cap)

    def test_the_dip_is_the_eyes_45_ms_blend(self):
        # One dark frame, at 60 % for 17 ms of the 45.
        ims = [Image.fromarray(a.astype(np.uint8)) for a in (MAIN, MAIN, MAIN * 0.6, MAIN, MAIN, MAIN, MAIN)]
        m, _cap = measured(ims, cross_fade=False)
        self.assertTrue(m["window_dip"]["flag"])
        self.assertAlmostEqual(m["window_dip"]["low"], 1 - 0.4 * STEP / 45.0, delta=0.02)

    def test_the_blend_is_weighted_by_the_frames_times(self):
        spans = [(0.0, 10.0), (10.0, 40.0), (40.0, 100.0)]
        got = motion_strip.blended_series([90.0, 0.0, 90.0], spans, np.array([0.0, 40.0, 55.0, 120.0]))
        np.testing.assert_allclose(got, [90.0, 90.0 * 15 / 45, 90.0 * 15 / 45, 90.0])


class WhatIsMeasured(unittest.TestCase):
    def test_the_mix_only_for_a_cross_fade(self):
        m, _cap = measured(recording(apart(1), MAIN, ART), cross_fade=False)
        self.assertIsNone(m["mix_shortfall"])
        self.assertIsNotNone(m["window_dip"])

    def test_no_whole_window_in_a_crop(self):
        m, _cap = measured(recording(apart(2), MAIN, PLOT), crop=(10, 20, 400, 240))
        self.assertEqual((m["window_dip"], m["mix_shortfall"]), (None, None))


class Memory(unittest.TestCase):
    def test_a_sampled_frame_keeps_none_of_the_whole_one(self):
        # A slice of the whole float frame kept its 34 MB alive, and eight pack workers ran the machine out of memory.
        whole = Image.new("RGB", (2000, 1400), (10, 20, 30))
        pic = motion_strip.window_pictures([whole])[0]
        owner = pic
        while isinstance(getattr(owner, "base", None), np.ndarray):
            owner = owner.base
        self.assertEqual(pic.dtype, np.float32)
        self.assertEqual(pic[0, 0].tolist(), [10.0, 20.0, 30.0])
        self.assertLess(owner.nbytes, 3 * 10 ** 6)


class InThePacks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # (pack reads the round's folders from the environment as it loads: not while the test modules load)
        os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
        import pack
        import states
        cls.pack = pack
        cls.states = states

    def test_the_windowed_viewers_recordings_are_cross_fades(self):
        for viewer in ("viewer", "albumart"):
            for way in ("open", "close"):
                self.assertIs(self.states.MOTION_FOCUS[f"{viewer}-{way}-windowed"]["cross_fade"], True)

    def test_both_reach_the_facts_and_the_metrics_table(self):
        strips = [dict(name=n, theme="dark", file=n + ".png", metrics=measured(recording(f, MAIN, over))[0])
                  for n, f, over in (("apart", apart(2), PLOT), ("layer", one_layer, ART))]
        facts = [self.pack.strip_facts(s["metrics"]) for s in strips]
        self.assertIn("WINDOW DIP (FAIL)", facts[0])
        self.assertIn("UNDER THE ONE-LAYER MIX (FAIL)", facts[0])
        self.assertNotIn("FAIL", facts[1])
        self.assertIn("cross-fade against the one-layer mix", facts[1])

        rd = tempfile.mkdtemp(dir=_TMP)
        self.pack.write_motion_metrics(rd, strips)
        with open(os.path.join(rd, "MOTION-METRICS.md")) as f:
            table = [ln for ln in f.read().split("\n") if ln.startswith("|")]
        self.assertEqual({ln.count("|") for ln in table}, {table[0].count("|")})
        head = [c.strip() for c in table[0].split("|")]
        rows = [[c.strip() for c in ln.split("|")] for ln in table[2:]]
        dip = head.index("whole window under the darker end")
        mix = head.index("under the one-layer mix")
        self.assertRegex(rows[0][dip], r"^\d+ % DIP at \+\d+$")
        self.assertRegex(rows[0][mix], r"^\d+ % FAIL at \+\d+$")
        self.assertRegex(rows[1][dip], r"^\d+ %$")
        self.assertRegex(rows[1][mix], r"^\d+ %$")


if __name__ == "__main__":
    unittest.main()
