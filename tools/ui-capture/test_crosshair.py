#!/usr/bin/env python3
"""Unit tests of a pan's crosshair fact (motion_strip.crosshair_lanes): whether every lane's dashed line is at one x in
every frame, found in the pixels beside the plot's own verticals.

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

PANE = (10, 50, 600, 260)  # x, y, w, h in the recording's images
AT = (0, 26)  # where those start in the window


def tearDownModule():
    shutil.rmtree(_TMP)


def frame(xs, seed=0, light=False, alpha=0.35, noise=12, gaps=1.0, crossings=()):
    """A plot of len(xs) noisy lanes with a transient and a solid playhead, and a dashed crosshair at each lane's x.
    gaps: what the line's gaps multiply the plot by (the app's light line has darker gaps); crossings: rows of the
    top lane that horizontals cross, dashed the same way and drawn over the vertical line (the crosshair's other
    hair, and bright partials where the hair's gap falls on them)."""
    rng = np.random.default_rng(seed)
    base = 240 if light else 60
    a = np.full((330, 640), float(base))
    x, y, w, h = PANE
    a[y:y + h, x:x + w] += rng.normal(0, noise, size=(h, w))
    a[y:y + h, x + 200:x + 202] = 100.0  # a transient
    a[y:y + h, x + 410] = 10.0 if light else 250.0  # the playhead, solid
    lh = h // len(xs)
    ink = 0.0 if light else 255.0
    for i, cx in enumerate(xs):
        if cx is None:
            continue
        drawn = np.arange(lh) % 7 < 4
        rows, gap_rows = y + i * lh + np.flatnonzero(drawn), y + i * lh + np.flatnonzero(~drawn)
        a[rows, x + cx] = a[rows, x + cx] * (1 - alpha) + ink * alpha
        a[gap_rows, x + cx] *= gaps
    drawn = np.arange(w) % 7 < 4
    cols, gap_cols = x + np.flatnonzero(drawn), x + np.flatnonzero(~drawn)
    for r in crossings:
        a[y + r, cols] = a[y + r, cols] * (1 - alpha) + ink * alpha
        a[y + r, gap_cols] *= gaps
    return Image.fromarray(a.clip(0, 255).astype(np.uint8)).convert("RGB")


class Lanes(unittest.TestCase):
    def check(self, *frames, **kw):
        ims = [frame(xs, seed, **kw) for seed, xs in enumerate(frames)]
        return motion_strip.crosshair_lanes(ims, [17.0 * k for k in range(len(ims))], PANE, len(frames[0]), AT)

    def test_lanes_that_follow_the_pointer_together_agree(self):
        c = self.check((500, 500), (430, 430), (300, 300), (120, 120))
        self.assertEqual((c["ok"], c["frames"], c["split"]), (True, 4, 0))
        self.assertIn("yes", motion_strip.crosshair_text(c))

    def test_a_lane_left_at_the_press_fails_with_the_frames_and_the_distance(self):
        c = self.check((500, 500), (430, 500), (300, 500), (40, 500), (40, 40))
        self.assertIs(c["ok"], False)
        self.assertEqual((c["split"], c["first"], c["last"], c["start_ms"], c["end_ms"]), (3, 1, 3, 17, 51))
        self.assertEqual((c["apart_px"], c["worst"]), (460, dict(frame=3, xs=[50, 510])))  # (in the window)
        text = motion_strip.crosshair_text(c)
        self.assertIn("NO (FAIL)", text)
        self.assertIn("f0001-f0003", text)

    def test_a_lane_without_the_line_fails(self):
        c = self.check((300, 300), (300, None), alpha=0.6)
        self.assertEqual((c["ok"], c["split"], c["worst"]["xs"]), (False, 1, [310, None]))

    def test_any_lane_of_six_counts(self):
        self.assertIs(self.check((300,) * 6, alpha=0.6)["ok"], True)
        self.assertIs(self.check((300,) * 4 + (90, 300), alpha=0.6)["ok"], False)

    def test_a_darker_line_on_a_light_plot(self):
        self.assertIs(self.check((300, 300), (150, 150), light=True)["ok"], True)
        self.assertIs(self.check((300, 150), light=True)["ok"], False)

    def test_rows_crossing_one_lanes_line_leave_it_where_it_is(self):
        # (the h16 pan: where the other hair and bright partials crossed the top lane's line, their gaps over the
        # line and the column 2 px left of it made the line's own column vary more than that copy of it, which
        # then scored higher: the top lane read 2 px left of the bottom one, at the same x in the pixels)
        app = dict(alpha=0.6, noise=6, gaps=0.4, crossings=(30, 58))
        c = self.check(*[(300, 300)] * 6, **app)
        self.assertEqual((c["ok"], c["frames"], c["split"]), (True, 6, 0), c.get("worst"))
        c = self.check((300, 302), **app)  # (a real 2 px split still fails, at the lines' own x)
        self.assertEqual((c["ok"], c["worst"]["xs"]), (False, [310, 312]))

    def test_without_a_crosshair_nothing_passes(self):
        c = self.check((None, None), (None, None))
        self.assertIsNone(c["ok"])
        self.assertIn("NOT CHECKED", motion_strip.crosshair_text(c))

    def test_one_lane_is_not_checked(self):
        c = self.check((300,))
        self.assertIsNone(c["ok"])
        self.assertIn("fewer than two lanes", motion_strip.crosshair_text(c))



class Presence(unittest.TestCase):
    """motion_strip.crosshair_presence: the crosshair kept through a zoom (h21: it vanished for ~215 ms per notch)."""
    def check(self, *frames, pre=1):
        ims = [frame(xs, seed, alpha=0.6) for seed, xs in enumerate(frames)]
        times = [17.0 * (k - pre) for k in range(len(ims))]
        return motion_strip.crosshair_presence(ims, times, PANE, len(frames[0]))

    def test_a_crosshair_kept_through_the_action_passes(self):
        c = self.check((300, 300), (300, 300), (280, 280), (280, 280))
        self.assertEqual((c["ok"], c["frames"], c["shown"]), (True, 4, 4))
        self.assertIn("yes", motion_strip.presence_text(c))

    def test_each_gap_is_named_with_its_frames_and_length(self):
        on, off = (300, 300), (None, None)
        c = self.check(on, off, off, on, off, on)
        self.assertIs(c["ok"], False)
        self.assertEqual([(g["first"], g["last"], g["start_ms"], g["ms"]) for g in c["gaps"]],
                         [(1, 2, 0, 34), (4, 4, 51, 17)])
        text = motion_strip.presence_text(c)
        self.assertIn("NO (FAIL)", text)
        self.assertIn("f0001-f0002 from +0 ms for 34 ms", text)

    def test_one_lane_counts(self):
        self.assertIs(self.check((300,), (300,))["ok"], True)
        self.assertIs(self.check((300,), (None,))["ok"], False)

    def test_without_a_crosshair_before_the_action_nothing_is_checked(self):
        c = self.check((None, None), (300, 300))
        self.assertIsNone(c["ok"])
        self.assertIn("NOT CHECKED", motion_strip.presence_text(c))

class InARecording(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # (pack reads the round's folders from the environment as it loads, and test_composite_sources sets those:
        # so not while the test modules load)
        os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
        import pack
        cls.pack = pack

    def test_the_fact_leads_the_pans_caption_and_facts(self):
        d = tempfile.mkdtemp(dir=_TMP)
        with open(os.path.join(d, "opacity.tsv"), "w") as f:
            f.write("#t0\t1000\n")
            for n, y in enumerate((80, 210)):  # the lanes' labels, in window coordinates
                key = "plotLayer/plotLaneHead/plotLaneLabel/content/plotPlateLabel" + ("#2" if n else "")
                f.write(f"{key}\t1\t14,{y},7,20\t0.00:1.000\t-\n")
        meta = dict(name="pan", theme="dark", desc="a pan", crop=[*AT, 640, 330], scale=1,
                    items=dict(spectrogramPane=[PANE[0] + AT[0], PANE[1] + AT[1], PANE[2], PANE[3]]))
        lanes = [(500, 500), (430, 500), (40, 500), (40, 40)]
        ims = [frame(xs, k) for k, xs in enumerate(lanes)]
        frames = [dict(t=1000 + 17 * k) for k in range(len(ims))]
        m, cap, _draw = motion_strip.measure(frames, ims, 1000.0, PANE, [0, 3], meta, d,
                                             focus=dict(lanes="spectrogramPane"))
        self.assertEqual((m["crosshair"]["lanes"], m["crosshair"]["split"]), (2, 2))
        self.assertTrue(cap.startswith("All lanes' crosshair x agree in every frame: NO (FAIL)"))
        self.assertTrue(self.pack.strip_facts(m).startswith("All lanes' crosshair x agree in every frame: NO (FAIL)"))


if __name__ == "__main__":
    unittest.main()
