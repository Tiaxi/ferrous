#!/usr/bin/env python3
"""Unit tests of the core's hand-laid 1:1 sheets: one fits the reviewers' 1568 px without losing what it shows.

Needs no KWin, app or shots: the shots and the recording are synthetic. Run with
`python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

from PIL import Image, ImageDraw

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def tearDownModule():
    shutil.rmtree(_TMP)


class Details(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # (pack and states read the round's folders from the environment as they load, and test_composite_sources
        # sets those: so not while the test modules load)
        os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
        import pack
        import states
        cls.pack, cls.states = pack, states

    def test_the_24_pt_search_fields_two_ends_fit_the_reviewers_1568_px(self):
        rd = tempfile.mkdtemp(dir=_TMP)
        rec = os.path.join(rd, "strips-raw", "mode-switch-lane_dark_1x_1600x1000")
        for d in ("shots", "sheets", rec):
            os.makedirs(os.path.join(rd, d), exist_ok=True)
        glyph, cursor, button = (255, 0, 0), (0, 255, 0), (0, 0, 255)
        entries = []
        for state, size, theme, font in (("statusbar-glyph", (1028, 226), "dark", ""),
                                         ("searchfield24pt", (1600, 69), "dark", "_font24"),
                                         ("modeswitch-hover", (448, 270), "dark", ""),
                                         ("modeswitch-hover", (448, 270), "light", "")):
            im = Image.new("RGB", size, (20, 22, 26))
            if state == "searchfield24pt":  # its parts as drawn: the glyph, the typed query, the buttons
                d = ImageDraw.Draw(im)
                d.rectangle((24, 22, 48, 46), fill=glyph)
                d.rectangle((76, 12, 260, 56), fill=cursor)
                d.rectangle((1470, 16, 1546, 52), fill=button)
            fn = f"details_{state}_{theme}_1x_1600x1000{font}.png"
            im.save(os.path.join(rd, "shots", fn))
            entries.append(dict(file=fn, area="details", state=state, theme=theme, scale=1, size="1600x1000", desc=""))
        Image.new("RGB", (1600, 440)).save(os.path.join(rec, "f0.png"))
        with open(os.path.join(rec, "frames.tsv"), "w") as f:
            f.write("mark\t1000\tnow ACTION\n" + "".join(f"frame\t{k}\t{1000 + 17 * k}\t0\tf0.png\n" for k in range(3)))
        with open(os.path.join(rec, "meta.json"), "w") as f:
            json.dump(dict(name="mode-switch-lane", theme="dark", size=[1600, 1000], crop=None, scale=1, opts="",
                           items=dict(visualizationModeSwitch=[1520, 9, 60, 32])), f)
        name, title, theme, rows = next(s for s in self.states.CUSTOM_SHEETS if s[0] == "details-1to1")
        with mock.patch.object(self.pack, "_redact", lambda _rd, _e, im: im):
            x = self.pack.custom_sheet(rd, os.path.join(rd, "sheets"), entries, name, title, theme, rows)
        self.assertLessEqual(max(x["size"]), self.pack.REVIEW)
        self.assertLessEqual(x["size"][0] * x["size"][1], self.pack.CORE_MP_HARD)  # (one 1:1 image, not two parts)
        self.assertTrue(x["one_to_one"])
        drawn = {c: n for n, c in Image.open(os.path.join(rd, "sheets", x["file"])).getcolors(1 << 20)}
        self.assertEqual((drawn.get(glyph), drawn.get(cursor), drawn.get(button)), (25 * 25, 185 * 45, 77 * 37))


if __name__ == "__main__":
    unittest.main()
