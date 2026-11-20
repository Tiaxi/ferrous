#!/usr/bin/env python3
"""Unit tests of where the pack paints a home-folder text at scale 2 (#666): the layout walk, the runner's
redaction sidecar and a recording's item trace give rects in logical px, a shot or frame is in device px, so each
box lands on its text only when it is scaled by the shot's device pixel ratio.

Needs no KWin, app or shots: each test draws a synthetic shot with a bright block where the walk says a text is,
under a made-up home folder, and checks that the pack paints that block. Run with
`python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import json
import os
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
# (before the harness's modules load: shotlib reads the round's folder when imported; setdefault: another test
# module may have loaded them already, and these tests pass their own round's folder)
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "unused", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image, ImageDraw  # noqa: E402

import pack  # noqa: E402
import states  # noqa: E402

HOME = "/home/example"
GREY, TEXT = (40, 40, 40), (250, 250, 250)


def tearDownModule():
    shutil.rmtree(_TMP)


def home_text(rect, name="folderRow"):
    return dict(name=name, text=HOME + "/Music/Fixture Album", rect=rect)


class PrivacyAtScale2(unittest.TestCase):
    def setUp(self):
        self.rd = tempfile.mkdtemp(dir=_TMP)
        os.makedirs(os.path.join(self.rd, "shots", "layout"))
        os.makedirs(os.path.join(self.rd, "strips-raw"))
        p = mock.patch.dict(os.environ, {"HOME": HOME, "SHOT_OUT": os.path.join(self.rd, "shots")})
        p.start()
        self.addCleanup(p.stop)

    def shot(self, file, size, texts_dev, walk, grabbed, scale=2):
        """Writes a grey shot (device px) with a bright block at each of texts_dev, its walk, and its entry."""
        im = Image.new("RGB", size, GREY)
        d = ImageDraw.Draw(im)
        for x, y, w, h in texts_dev:
            d.rectangle((x, y, x + w - 1, y + h - 1), fill=TEXT)
        im.save(os.path.join(self.rd, "shots", file))
        with open(os.path.join(self.rd, "shots", "layout", file[:-4] + ".json"), "w") as f:
            json.dump(dict(windows=walk), f)
        return dict(file=file, area="test", state=file[:-4], theme="dark", scale=scale, size="100x60",
                    layout=f"shots/layout/{file[:-4]}.json", grabbed=grabbed)

    def assert_painted(self, file, texts_dev, entries):
        """The pack's check passes and no pixel of a text's block is left bright."""
        fails = pack.redact_round(self.rd, entries)
        self.assertEqual(fails, [])
        im = Image.open(os.path.join(self.rd, "shots", file)).convert("RGB")
        for x, y, w, h in texts_dev:
            left = [im.getpixel((px, py)) for py in range(y, y + h) for px in range(x, x + w)].count(TEXT)
            self.assertEqual(left, 0, f"{file}: {left} px of the home-folder text at {[x, y, w, h]} not painted")

    def test_a_main_window_text(self):
        e = self.shot("main.png", (200, 120), [(40, 20, 80, 16)],
                      [dict(cls="QQuickApplicationWindow", main=True, size=[100, 60], texts=[home_text([20, 10, 40, 8])])],
                      dict(kind="main", name="", image_offset=[0, 0]))
        self.assert_painted("main.png", [(40, 20, 80, 16)], [e])

    def test_a_menu_the_main_window_grab_draws_in(self):
        # the menu window at (30, 20) logical px from the main window, its text at (5, 4) in it
        e = self.shot("menu.png", (200, 120), [(70, 48, 60, 16)],
                      [dict(cls="QQuickApplicationWindow", main=True, size=[100, 60], texts=[]),
                       dict(cls="QQuickPopupWindow", size=[40, 20], main_popup_offset=[30, 20],
                            texts=[home_text([5, 4, 30, 8], "menuItem")])],
                      dict(kind="main", name="", image_offset=[0, 0]))
        self.assert_painted("menu.png", [(70, 48, 60, 16)], [e])

    def test_a_nested_dialog_capture(self):
        # An outer dialog window (150x120 logical) at the capture's origin below its title bar, 28 px at scale 1,
        # 56 device px at scale 2; the grabbed window (60x40) over it at its image offset (80, 140) in device px.
        outer_dev, inner_dev = (10, 66, 60, 16), (90, 150, 80, 16)
        e = self.shot("nested.png", (300, 300), [outer_dev, inner_dev],
                      [dict(cls="QQuickApplicationWindow", main=True, size=[100, 60], texts=[]),
                       dict(cls="ConfigureWindow", size=[150, 120], texts=[home_text([5, 5, 30, 8])]),
                       dict(cls="ConfirmWindow", size=[60, 40], texts=[home_text([5, 5, 40, 8], "detail")])],
                      dict(kind="dialog", cls="ConfirmWindow", decorated=True, image_offset=[80, 140]))
        boxes, inner = pack._home_boxes(self.rd, e)
        self.assertEqual(sorted(boxes), sorted([list(outer_dev), list(inner_dev)]))
        self.assertEqual(inner, (80, 80, 200, 220))  # the grabbed window with its 30 px (60 device px) title bar
        self.assert_painted("nested.png", [outer_dev, inner_dev], [e])

    def test_a_scale_1_text_stays_where_it_is(self):
        e = self.shot("main1.png", (100, 60), [(20, 10, 40, 8)],
                      [dict(cls="QQuickApplicationWindow", main=True, size=[100, 60], texts=[home_text([20, 10, 40, 8])])],
                      dict(kind="main", name="", image_offset=[0, 0]), scale=1)
        self.assertEqual(pack._home_boxes(self.rd, e)[0], [[20, 10, 40, 8]])
        self.assert_painted("main1.png", [(20, 10, 40, 8)], [e])

    def test_a_recorded_frame(self):
        # a recording of the window cropped at (10, 5) logical px (as Session.strip records its crop; the probe reads
        # the frame at (20, 10) device px); the item at (20, 10) logical px, 40x8
        d = os.path.join(self.rd, "strips-raw", "rec_dark_2x")
        os.makedirs(d)
        im = Image.new("RGB", (160, 80), GREY)
        ImageDraw.Draw(im).rectangle((20, 10, 99, 25), fill=TEXT)
        im.save(os.path.join(d, "f0.png"))
        with open(os.path.join(d, "frames.tsv"), "w") as f:
            f.write("frame\t0\t100.0\t-\tf0.png\n")
        with open(os.path.join(d, "opacity.tsv"), "w") as f:
            f.write("#t0\t0\nwin/folderRow#1\t1\t20,10,40,8\t0:1\t-\n")
        with open(os.path.join(d, "meta.json"), "w") as f:
            json.dump(dict(name="rec", crop=[10, 5, 80, 40], scale=2, neutral=True), f)
        e = self.shot("main.png", (200, 120), [(40, 20, 80, 16)],
                      [dict(cls="QQuickApplicationWindow", main=True, size=[100, 60], texts=[home_text([20, 10, 40, 8])])],
                      dict(kind="main", name="", image_offset=[0, 0]))
        self.assertEqual(pack.redact_round(self.rd, [e]), [])
        fr = Image.open(os.path.join(d, "f0.png")).convert("RGB")
        left = [fr.getpixel((x, y)) for y in range(10, 26) for x in range(20, 100)].count(TEXT)
        self.assertEqual(left, 0, f"{left} px of the recorded home-folder item not painted")


class SidecarAtScale2(unittest.TestCase):
    """The runner's sidecar (states.private_sidecar) lists its boxes in the image's device px."""

    def setUp(self):
        self.out = tempfile.mkdtemp(dir=_TMP)
        p = mock.patch.dict(os.environ, {"HOME": HOME, "SHOT_OUT": self.out})
        p.start()
        self.addCleanup(p.stop)

    def sidecar(self, grabbed, answers, dialogs=()):
        """Runs private_sidecar with windows that answer `answers` ({window class or None: logical boxes})."""
        reply = lambda win: json.dumps(dict(boxes=answers[win], precise=[]))
        s = types.SimpleNamespace(scale=2, index=[dict(file="shot.png", grabbed=grabbed)],
                                  dialog_windows=lambda: [dict(cls=c) for c in dialogs],
                                  a=types.SimpleNamespace(cmd=lambda c: reply(c.split()[0].partition("@")[2])),
                                  ev=lambda js: reply(None))
        states.private_sidecar(types.SimpleNamespace(s=s), "shot")
        with open(os.path.join(self.out, "shot.redact.json")) as f:
            return json.load(f)["boxes"]

    def test_a_main_window_box(self):
        self.assertEqual(self.sidecar(dict(kind="main", name="", image_offset=[0, 0]), {None: [[20, 10, 40, 8]]}),
                         [[40, 20, 80, 16]])

    def test_a_nested_dialog_capture(self):
        boxes = self.sidecar(dict(kind="dialog", cls="ConfirmWindow", image_offset=[80, 140]),
                             {"ConfirmWindow": [[5, 5, 40, 8]], "ConfigureWindow": [[5, 5, 30, 8]]},
                             dialogs=("ConfirmWindow", "ConfigureWindow"))
        self.assertEqual(boxes, [[90, 150, 80, 16], [10, 66, 60, 16]])


if __name__ == "__main__":
    unittest.main()
