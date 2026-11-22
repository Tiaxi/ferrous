#!/usr/bin/env python3
"""Unit tests of how pack.py reads and draws recordings: frames kept as shared tiles are the decoded frames pixel
for pixel (framestore.py), one measurement drawn at two sizes is what two measurements draw (motion_strip.measure),
and a pair is composed from its recordings' halves without measuring them again.

Needs no KWin, app or shots: the recordings are synthetic. Run with
`python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

import numpy as np
from PIL import Image

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "round", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import framestore  # noqa: E402
import motion_strip  # noqa: E402
import pack  # noqa: E402


def tearDownModule():
    shutil.rmtree(_TMP)


def frames_of(n=14, w=400, h=150):
    """A still window with a box that moves, and a last frame that changes everywhere."""
    base = np.random.default_rng(1).integers(0, 40, size=(h, w, 3), dtype=np.uint8)
    out = []
    for k in range(n):
        a = base.copy()
        a[40:90, 20 + 15 * k:70 + 15 * k] = (200, 100, 50 + k)
        out.append(a)
    out.append(255 - base)
    return out


def record(d, arrays, name="move", theme="dark"):
    os.makedirs(d)
    lines = ["mark\t995\tnow ACTION"]
    for k, a in enumerate(arrays):
        Image.fromarray(a).save(os.path.join(d, f"f{k:04d}.png"))
        lines.append(f"frame\t{k}\t{980 + 17 * k}\t0\tf{k:04d}.png")
    lines.append(f"mark\t{980 + 17 * len(arrays)}\tstripstop")
    with open(os.path.join(d, "frames.tsv"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump(dict(name=name, theme=theme, desc="a box moves", crop=None, scale=1, size=[400, 150], opts=""), f)
    return [f"f{k:04d}.png" for k in range(len(arrays))]


class Case(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(dir=_TMP)


class Frames(Case):
    def check(self, arrays, **kw):
        rec = os.path.join(self.d, "rec")
        store = framestore.FrameStore(rec, record(rec, arrays), **kw)
        order = list(range(len(arrays))) + list(range(len(arrays))) + [5, 0, -1, 3]
        for k in order:
            np.testing.assert_array_equal(np.asarray(store[k]), arrays[k], f"frame {k}")
        return store

    def test_every_read_is_the_decoded_frame(self):
        self.check(frames_of())

    def test_sizes_that_are_no_whole_number_of_tiles(self):
        self.check(frames_of(w=391, h=139))

    def test_a_frame_of_another_size_in_the_recording(self):
        arrays = frames_of()
        arrays[6] = arrays[6][:101, :333].copy()
        self.check(arrays)

    def test_each_frame_is_decoded_once(self):
        arrays = frames_of()
        rec = os.path.join(self.d, "rec")
        store = framestore.FrameStore(rec, record(rec, arrays))
        with mock.patch.object(framestore.Image, "open", wraps=Image.open) as opened:
            for _ in range(4):
                for k in range(len(arrays)):
                    store[k]
        self.assertEqual(opened.call_count, len(arrays))

    def test_unchanged_tiles_are_shared(self):
        arrays = frames_of()
        store = self.check(arrays)
        self.assertLess(store.bytes, 0.6 * sum(a.size for a in arrays))

    def test_over_the_budget_frames_are_decoded_again(self):
        store = self.check(frames_of(), max_bytes=1)
        self.assertEqual(len(store.kept), 1)

    def test_reading_backwards(self):
        arrays = frames_of()
        rec = os.path.join(self.d, "rec")
        store = framestore.FrameStore(rec, record(rec, arrays), recent=1)
        for _ in range(2):
            for k in reversed(range(len(arrays))):
                np.testing.assert_array_equal(np.asarray(store[k]), arrays[k])


class OneMeasurement(Case):
    def test_drawing_again_at_another_size_matches_measuring_again(self):
        rec = os.path.join(self.d, "rec")
        record(rec, frames_of())
        frames, marks, meta = pack.strip_frames(rec)
        ims = pack.LazyFrames(rec, frames)
        args = (frames, ims, pack.action_time(marks, frames[0]["t"]), (0, 0, 400, 150), [0, 2, 4, 6, 8, 10, 12, 14],
                meta, rec)
        metrics, cap, draw = motion_strip.measure(*args)
        half, full = draw(*pack.HALF_SIZE), draw(*pack.STRIP_SIZE)
        for size, drawn in ((pack.HALF_SIZE, half), (pack.STRIP_SIZE, full)):
            motion_strip.W, motion_strip.MAX_H = size
            m, c, im = motion_strip.analyse(*args)
            self.assertEqual((m, c, im.size), (metrics, cap, drawn.size))
            self.assertEqual(im.tobytes(), drawn.tobytes())
        self.assertNotEqual(half.width, full.width)

    def test_ink_is_the_masked_pixels_mean_distance(self):
        rng = np.random.default_rng(3)
        arrs = [rng.integers(0, 256, size=(40, 90, 3)).astype(np.float32) for _ in range(4)]
        arrs[0][:25] = (12, 34, 56)  # (the background: the most frequent colour)
        for mask in (np.ones((40, 90), dtype=bool), rng.random((40, 90)) < 0.4):
            bg = np.array([12, 34, 56], dtype=np.float32)
            want = [float(np.abs(a[mask] - bg).sum(axis=1).mean()) for a in arrs]
            self.assertEqual(motion_strip.ink_series(arrs, mask), want)
        self.assertIsNone(motion_strip.ink_series(arrs, np.zeros((40, 90), dtype=bool)))

    def test_channel_reductions_equal_numpys(self):
        rng = np.random.default_rng(2)
        ints = rng.integers(0, 256, size=(50, 70, 3)).astype(np.int16)
        floats = (rng.random((50, 70, 3)) * 255).astype(np.float32)
        for a in (ints, floats):
            for view in (a, a.reshape(-1, 3)):
                np.testing.assert_array_equal(motion_strip.sum3(view), view.sum(axis=-1))
                np.testing.assert_array_equal(motion_strip.max3(view), view.max(axis=-1))


class FactOrder(Case):
    def test_the_documents_do_not_follow_the_order_sessions_finished_in(self):
        facts = [dict(key="card-wide", theme="dark", suffix=f"dark_1x_{v}", text=f"card at {v}")
                 for v in ("1000x720", "1600x1000", "1600x1000_font24")]
        floors = [dict(label="left", theme="dark", suffix=f"dark_1x_{v}", what=v, region="marker", floor_token="#000")
                  for v in ("1000x720", "1600x1000")]
        docs = []
        for step in (1, -1):
            for name, rows in (("facts.jsonl", facts), ("floors.jsonl", floors)):
                with open(os.path.join(self.d, name), "w") as f:
                    f.writelines(json.dumps(row) + "\n" for row in rows[::step])
            pack.write_core_facts(self.d, [], [], {})
            with open(os.path.join(self.d, "CORE-FACTS.md"), encoding="utf-8") as f:
                docs.append(f.read())
        self.assertEqual(docs[0], docs[1])
        self.assertIn("card at 1600x1000_font24", docs[0])


class Pairs(Case):
    def test_a_strip_job_makes_the_half_and_the_pair_is_made_of_halves(self):
        rd = os.path.join(self.d, "round")
        out, tmp = os.path.join(rd, "strips"), os.path.join(rd, "strips", "pairtmp")
        os.makedirs(out)
        dirs = []
        for theme in ("dark", "light"):
            dirs.append(os.path.join(rd, "strips-raw", f"move_{theme}_1x_400x150"))
            record(dirs[-1], frames_of(), theme=theme)
        pjobs = [("themes", "move@dark", "move@light", "dark", "Both themes"), ("gone", "move", "none", "dark", "-")]
        self.assertEqual(pack.pair_halves(rd, pjobs), {0: tuple(dirs)})
        halves = []
        for d in dirs:
            recs, half = pack.strip_job((d, out, tmp))
            self.assertEqual(recs[0]["size"][0], pack.STRIP_SIZE[0])
            with Image.open(half["file"]) as him:
                self.assertEqual(him.width, pack.HALF_SIZE[0])
            self.assertEqual(half["caption"], recs[0]["caption"])
            halves.append(half)
        with mock.patch.object(motion_strip, "measure", side_effect=AssertionError("measured again")):
            [rec] = pack.pair_job((out, "themes", "dark", "Both themes", halves))
        self.assertEqual(rec["name"], "pair:themes")
        self.assertEqual(rec["size"][0], 2 * pack.HALF_SIZE[0] + pack.PAD)
        self.assertTrue(os.path.exists(os.path.join(out, rec["file"])))

    def test_without_a_pair_no_half_is_drawn(self):
        rec = os.path.join(self.d, "rec")
        record(rec, frames_of())
        recs, half = pack.strip_job((rec, self.d, None))
        self.assertTrue(recs)
        self.assertIsNone(half)


if __name__ == "__main__":
    unittest.main()
