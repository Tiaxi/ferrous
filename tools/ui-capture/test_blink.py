#!/usr/bin/env python3
"""Unit tests of the blink measures (motion_strip): the ink of a change box, and per changed item its traced opacity
where the frames show the dip, so a small popup's blink is not averaged away in a wide change box.

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
LIST = (40, 30, 240, 120)  # x, y, w, h
BG = (20, 22, 26)


def tearDownModule():
    shutil.rmtree(_TMP)


def drawn(level, rect=LIST, seed=3, under=None):
    """A 1000x200 window with an item of text-like pixels in `rect` at opacity `level`, over BG or `under`."""
    a = np.empty((200, 1000, 3), dtype=np.float32)
    a[...] = BG
    if under is not None:
        a[:] = under
    x, y, w, h = rect
    ink = np.random.default_rng(seed).choice([40.0, 230.0], size=(h, w), p=(0.7, 0.3))[..., None].repeat(3, axis=2)
    a[y:y + h, x:x + w] = a[y:y + h, x:x + w] * (1 - level) + ink * level
    return Image.fromarray(a.astype(np.uint8))


def track(levels, rect=LIST, rects=None, text=False, t0=-STEP):
    """A traced item: a point where its opacity changes, sampled just before each frame."""
    pts, rs = [], []
    for k, v in enumerate(levels):
        r = rects[k] if rects else rect
        if not pts or v != pts[-1][1] or list(r) != rs[-1]:
            pts.append((t0 + k * STEP - 1, float(v)))
            rs.append(list(r))
    return dict(text=text, rect=rs[0], rects=rs, pts=pts, layers=["-"] * len(pts), fulls=rs)


def blinks(tracks, ims, step=STEP, scale=1.0, off=(0, 0)):
    times = [-STEP + k * step for k in range(len(ims))]
    spans = [(t, times[k + 1] if k + 1 < len(times) else t + step) for k, t in enumerate(times)]
    return motion_strip.item_blinks(tracks, times, spans, ims, scale, off)


class ItemBlinks(unittest.TestCase):
    def of(self, levels):
        return blinks({"Main/list": track(levels)}, [drawn(v) for v in levels])

    def test_a_dim_to_exactly_half_is_a_blink(self):
        got = self.of([1, 1, 0.5, 0.5, 0.5, 1, 1])
        self.assertEqual(len(got), 1)
        self.assertEqual((got[0]["label"], round(got[0]["ms"]), got[0]["low"]), ("list", 51, 0.5))

    def test_half_for_one_frame_is_a_blink(self):
        self.assertEqual([round(b["ms"]) for b in self.of([1, 1, 0.5, 1, 1])], [17])

    def test_absent_for_a_frame_then_fading_back_is_one_blink(self):
        got = self.of([1, 1, 0, 0.407, 0.699, 0.867, 0.961, 1, 1])
        self.assertEqual([(round(b["ms"]), b["low"]) for b in got], [(51, 0.0)])  # (the time under 75 %)

    def test_over_a_quarter_blinks_only_when_it_lasts_over_30_ms(self):
        self.assertEqual(len(self.of([1, 1, 0.65, 0.65, 1, 1])), 1)
        self.assertEqual(self.of([1, 1, 0.65, 1, 1]), [])

    def test_a_dip_of_a_fifth_is_none(self):
        self.assertEqual(self.of([1, 1, 0.8, 0.8, 0.8, 1, 1]), [])

    def test_a_fade_a_hold_and_a_long_dip_are_none(self):
        for levels in ([1, 1, 0.6, 0.3, 0, 0], [0, 0, 0.3, 0.6, 1, 1], [1, 1] + [0.2] * 16 + [1, 1]):
            with self.subTest(levels=levels):
                self.assertEqual(self.of(levels), [])

    def test_two_dips_are_two_blinks(self):
        self.assertEqual(len(self.of([1, 1, 0.4, 1, 1, 1, 0.4, 1, 1])), 2)

    def test_a_dip_before_the_action_is_none(self):
        levels = [1, 0.2, 1, 1]
        ims = [drawn(v) for v in levels]
        self.assertEqual(blinks({"Main/list": track(levels, t0=-5 * STEP)}, ims), [])

    def test_a_traced_dip_no_frame_shows_is_none(self):
        levels = [1, 1, 0, 0, 1, 1]
        self.assertEqual(blinks({"Main/incoming": track(levels)}, [drawn(1) for _ in levels]), [])

    def test_an_incoming_text_traced_out_until_the_handover_ends_is_none(self):
        levels = [1, 1] + [0] * 9 + [1, 1]
        old, new = np.asarray(drawn(1, seed=3), dtype=np.float32), np.asarray(drawn(1, seed=9), dtype=np.float32)
        # (truncated for a frame, then the new text long drawn before the trace has it back)
        cut = old.copy()
        cut[:, 120:] = BG
        ims = [old, old, cut] + [new] * 10
        got = blinks({"Main/incoming": track(levels)}, [Image.fromarray(a.astype(np.uint8)) for a in ims])
        self.assertEqual(got, [])

    def test_a_popup_over_busy_content_that_goes_and_returns_as_it_was(self):
        busy = np.random.default_rng(1).integers(0, 255, size=(200, 1000, 3)).astype(np.float32)
        rect = (40, 30, 80, 40)

        def frame(v):
            a = busy.copy()
            a[30:70, 40:120] = a[30:70, 40:120] * (1 - v) + 60.0 * v  # (plainer than what is behind it)
            return Image.fromarray(a.astype(np.uint8))
        levels = [1, 1, 0, 1, 1]
        self.assertEqual(len(blinks({"Main/ToolTip": track(levels, rect)}, [frame(v) for v in levels])), 1)

    def test_items_that_dip_together_are_one_the_largest(self):
        levels = [1, 1, 0.5, 0.5, 1, 1]
        x, y, w, h = LIST
        tracks = {"Main/list/row#2": track(levels, (x, y + 20, w, 20)), "Main/list": track(levels),
                  "Main/list/row": track(levels, (x, y, w, 20))}
        got = blinks(tracks, [drawn(v) for v in levels])
        self.assertEqual([(b["label"], b["items"]) for b in got], [("list", 3)])

    def test_a_reused_delegate_that_returns_elsewhere_is_none(self):
        levels = [1, 1, 0, 1, 1]
        rects = [LIST] * 3 + [(600, 30, 240, 120)] * 2
        self.assertEqual(blinks({"Main/row": track(levels, rects=rects)}, [drawn(v) for v in levels]), [])

    def test_the_rect_is_found_in_a_crop_at_scale_2(self):
        levels = [1, 1, 0.5, 0.5, 1, 1]
        rect = (40 / 2 + 100, 30 / 2 + 50, 240 / 2, 120 / 2)  # where LIST's pixels are in the window
        got = blinks({"Main/list": track(levels, rect)}, [drawn(v) for v in levels], scale=2.0, off=(100, 50))
        self.assertEqual(len(got), 1)


class InkBlink(unittest.TestCase):
    SEG = [dict(a=1, b=4)]
    SPANS = [(k * STEP, (k + 1) * STEP) for k in range(5)]

    def blink(self, *ink):
        return round(motion_strip.blink_of(list(ink), self.SPANS, self.SEG)[0])

    def test_exactly_half_is_a_blink(self):
        self.assertEqual(self.blink(40, 20, 20, 20, 40), 51)
        self.assertEqual(self.blink(40, 20.1, 40, 40, 40), 17)  # (as 8-bit pixels round it)

    def test_a_cross_fades_mixed_ink_is_none(self):
        self.assertEqual(self.blink(40, 27, 25, 28, 40), 0)


class InARecording(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # (pack reads the round's folders from the environment as it loads, and test_composite_sources sets those:
        # so not while the test modules load)
        os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
        import pack
        cls.pack = pack

    def test_a_small_popups_blink_in_a_wide_change_box_reaches_the_facts(self):
        d = tempfile.mkdtemp(dir=_TMP)
        levels = [1, 1, 0, 0.407, 0.699, 0.867, 0.961, 1, 1]
        ims = []
        for k, v in enumerate(levels):
            a = np.asarray(drawn(v, rect=(600, 8, 38, 30))).copy()
            a[60:190, 20:980] = (90, 90, 90)  # a wide bar that moves with the drag
            a[60:190, 20 + 100 * k:40 + 100 * k] = (250, 160, 40)
            ims.append(Image.fromarray(a))
        with open(os.path.join(d, "opacity.tsv"), "w") as f:
            f.write("#t0\t1000\n")
            pts = " ".join(f"{k * STEP - 1:.2f}:{v:.3f}" for k, v in enumerate(levels))
            f.write(f"Main/ToolTip\t0\t600,8,38,30\t{pts}\tpopup@1\n")
        frames = [dict(t=1000 + k * STEP) for k in range(len(levels))]
        meta = dict(name="drag", theme="dark", desc="a drag", crop=None, scale=1, items={})
        m, cap, _draw = motion_strip.measure(frames, ims, 1000.0, (0, 0, 1000, 200), [0, 2, 8], meta, d)
        self.assertEqual((m["blink_text_ms"] or 0, m["blink_other_ms"] or 0), (0, 0))  # (the change box's average)
        self.assertEqual([(b["label"], b["ms"]) for b in m["blink_items"]], [("Main/ToolTip", 51)])
        self.assertEqual(m["blink_ms"], 51)
        self.assertIn("Main/ToolTip down to 0 %", cap)
        self.assertIn("ITEM BLINK: Main/ToolTip", self.pack.strip_facts(m))

        rd = tempfile.mkdtemp(dir=_TMP)
        self.pack.write_motion_metrics(rd, [dict(name="drag", theme="dark", file="drag.png", metrics=m)])
        with open(os.path.join(rd, "MOTION-METRICS.md")) as f:
            table = [ln for ln in f.read().split("\n") if ln.startswith("|")]
        self.assertEqual({ln.count("|") for ln in table}, {table[0].count("|")})
        self.assertIn("Main/ToolTip 51 ms (to 0 %)", table[2])


if __name__ == "__main__":
    unittest.main()
