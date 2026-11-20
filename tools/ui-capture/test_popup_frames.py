#!/usr/bin/env python3
"""Unit tests of how a strip draws its menus in (#664): menus are windows of their own, so the probe records their
frames beside the main window's and notes where each is; popup_frames.merge turns them into main-window frames with
each menu at its place (device px at the session's scale, cut by the crop), and the pack's privacy check needs the
menus' items in the recording's item trace.

Needs no KWin, app or shots: each test writes a synthetic recording (frames.tsv, frames, trace). Run with
`python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
# (before the harness's modules load: shotlib reads the round's folder when imported; setdefault: another test
# module may have loaded them already, and these tests pass their own round's folder)
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "unused", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image  # noqa: E402

import pack  # noqa: E402
import popup_frames  # noqa: E402
import shotlib  # noqa: E402

MAIN, MENU, EDGE = (40, 40, 40), (200, 60, 60), (60, 200, 60)
BACKDROP = popup_frames.POPUP_BACKDROP


def tearDownModule():
    shutil.rmtree(_TMP)


def menu_image(w, h, margin=0):
    """A menu window's frame (RGBA): opaque with a green top-left pixel, inside a transparent margin."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    im.paste(Image.new("RGBA", (w - 2 * margin, h - 2 * margin), MENU + (255,)), (margin, margin))
    im.putpixel((margin, margin), EDGE + (255,))
    return im


class Recording:
    """A recording as the probe writes it: main window 0, menu windows from 1."""

    def __init__(self, d):
        self.d, self.lines, self.n = d, [], 0
        os.makedirs(d, exist_ok=True)

    def frame(self, t, win, im):
        name = f"f{self.n:04d}_w{win}.png"
        im.save(os.path.join(self.d, name))
        self.lines.append(f"frame\t{self.n}\t{t:.3f}\t{win}\t{name}")
        self.n += 1

    def popup(self, t, win, shown, off, depth=0):
        self.lines.append(f"popup\t{t:.3f}\t{win}\t{1 if shown else 0}\t{off[0]},{off[1]}\t0\t{depth if shown else -1}")

    def write(self, menus=(1,)):
        other = ["mark\t5.000\t12 now ACTION", "window\t0\tQQuickApplicationWindow\tFerrous\t0,0,100x60\t0.000\twindow"]
        other += [f"window\t{w}\tQQuickPopupWindow\t\t0,0,40x30\t0.000\tpopup" for w in menus]
        with open(os.path.join(self.d, "frames.tsv"), "w") as f:
            f.write("".join(ln + "\n" for ln in self.lines + other))

    def merged(self):
        frames, popups, _wins, other = popup_frames.read_tsv(self.d)
        return frames, popups, other

    def image(self, file):
        return Image.open(os.path.join(self.d, file)).convert("RGB")


class MergeMenuFrames(unittest.TestCase):
    def setUp(self):
        self.d = os.path.join(tempfile.mkdtemp(dir=_TMP), "rec")

    def merged(self):
        return Recording.merged(self)

    def image(self, file):
        return Recording.image(self, file)

    def open_close(self, scale=2, crop=None, off=(30, 20), menu=(40, 30), margin=0):
        """The main window renders once, then a menu opens at `off` (two frames of its fade) and closes."""
        r = Recording(self.d)
        cw, ch = (crop[2], crop[3]) if crop else (100, 60)
        r.frame(1.0, 0, Image.new("RGB", (cw * scale, ch * scale), MAIN))
        r.popup(10.0, 1, True, off)
        r.frame(12.0, 1, menu_image(menu[0] * scale, menu[1] * scale, margin * scale))
        r.frame(20.0, 1, menu_image(menu[0] * scale, menu[1] * scale, margin * scale))
        r.popup(40.0, 1, False, off)
        r.write()
        return popup_frames.merge(self.d, dpr=scale, crop=crop)

    def test_a_menu_is_drawn_in_at_its_place_at_scale_2(self):
        self.assertEqual(self.open_close(scale=2), [1])
        frames, _p, other = self.merged()
        # the main frame, each menu frame drawn in, and the close: main-window frames only, in time order
        self.assertEqual([(f["t"], f["win"]) for f in frames], [(1.0, 0), (12.0, 0), (20.0, 0), (40.0, 0)])
        self.assertTrue(any(ln.startswith("mark\t") for ln in other), "the marks are kept")
        self.assertEqual(sum(ln.startswith("popup\t") for ln in other), 2, "and where the menu was")
        im = self.image(frames[1]["file"])
        self.assertEqual(im.size, (200, 120))
        self.assertEqual(im.getpixel((60, 40)), EDGE)  # (30, 20) logical: (60, 40) device px
        self.assertEqual(im.getpixel((59, 40)), MAIN)
        self.assertEqual(im.getpixel((60 + 79, 40 + 59)), MENU)
        self.assertEqual(im.getpixel((60 + 80, 40 + 60)), MAIN)
        self.assertEqual(self.image(frames[3]["file"]).getpixel((70, 50)), MAIN, "the closed menu is gone")
        on_disk = sorted(f for f in os.listdir(self.d) if f.endswith(".png"))
        # (the close shows the main window's first frame again: the same file)
        self.assertEqual(frames[3]["file"], frames[0]["file"])
        self.assertEqual(on_disk, sorted({f["file"] for f in frames}), "the menu window's own frames are removed")

    def test_its_transparent_margin_shows_the_window_under_it(self):
        self.open_close(scale=1, margin=4)
        im = self.image(self.merged()[0][1]["file"])
        self.assertEqual(im.getpixel((31, 21)), MAIN)
        self.assertEqual(im.getpixel((34, 24)), EDGE)

    def test_the_crop_cuts_a_menu_and_moves_it(self):
        # a crop at (20, 10) logical: the menu at (30, 20) is at (10, 10) in it, (20, 20) device px; cut at its edge
        self.open_close(scale=2, crop=[20, 10, 40, 25])
        im = self.image(self.merged()[0][1]["file"])
        self.assertEqual(im.size, (80, 50))
        self.assertEqual(im.getpixel((20, 20)), EDGE)
        self.assertEqual(im.getpixel((79, 49)), MENU)

    def test_a_crop_cuts_a_menu_left_of_it(self):
        self.open_close(scale=1, crop=[40, 0, 50, 60])
        im = self.image(self.merged()[0][1]["file"])
        self.assertEqual(im.getpixel((0, 20)), MENU)  # (the menu's columns 10 on)
        self.assertEqual(im.getpixel((30, 20)), MAIN)

    def test_a_menu_outside_the_crop_is_not_drawn_in(self):
        # (the pack then needs no trace of its items: none of its pixels are in the frames)
        self.assertEqual(self.open_close(scale=2, crop=[0, 0, 20, 15]), [])
        frames = self.merged()[0]
        self.assertEqual([f["win"] for f in frames], [0, 0, 0, 0])
        self.assertEqual({tuple(self.image(f["file"]).getcolors()) for f in frames}, {((40 * 30, MAIN),)})

    def test_a_crop_over_only_its_transparent_margin_does_not_draw_it_in(self):
        # the menu at (30, 20) with a 4 px transparent margin; the crop ends 2 px into the margin
        self.assertEqual(self.open_close(scale=1, crop=[0, 0, 32, 22], margin=4), [])

    def test_the_frame_scale_is_read_as_the_probe_reads_it(self):
        self.assertEqual(popup_frames.frame_scale(""), 1.0)
        self.assertEqual(popup_frames.frame_scale("every=2 scale=0.5"), 0.5)
        self.assertEqual(popup_frames.frame_scale("scale=0.5 scale=0.25"), 0.25)
        self.assertEqual(popup_frames.frame_scale("scale=.5e-0"), 0.5)
        for bad in ("scale=0", "scale=2", "scale=x", "scale=0.5 scale=x", "scale=0.2_5", "scale=nan"):
            self.assertEqual(popup_frames.frame_scale(bad), 1.0, bad)

    def test_without_a_crop_the_frames_grow_for_a_menu_past_the_window(self):
        self.open_close(scale=2, off=(80, 40))
        frames = self.merged()[0]
        sizes = {self.image(f["file"]).size for f in frames}
        self.assertEqual(sizes, {(240, 140)}, "every frame grows alike, right and down")
        first = self.image(frames[0]["file"])
        self.assertEqual(first.getpixel((230, 10)), BACKDROP)
        self.assertEqual(first.getpixel((10, 10)), MAIN)
        self.assertEqual(self.image(frames[1]["file"]).getpixel((239, 139)), MENU)

    def test_a_submenu_is_drawn_over_its_menu(self):
        r = Recording(self.d)
        r.frame(1.0, 0, Image.new("RGB", (100, 60), MAIN))
        r.popup(10.0, 1, True, (10, 10), 0)
        r.frame(11.0, 1, menu_image(40, 30))
        r.popup(15.0, 2, True, (30, 20), 1)
        r.frame(16.0, 2, Image.new("RGBA", (20, 20), EDGE + (255,)))
        r.frame(17.0, 1, menu_image(40, 30))  # (the menu redraws: its submenu stays over it)
        r.write(menus=(1, 2))
        self.assertEqual(popup_frames.merge(self.d), [1, 2])
        frames = self.merged()[0]
        self.assertEqual(self.image(frames[-1]["file"]).getpixel((35, 25)), EDGE)

    def test_a_menu_frame_after_it_closed_is_not_drawn(self):
        r = Recording(self.d)
        r.frame(1.0, 0, Image.new("RGB", (100, 60), MAIN))
        r.popup(10.0, 1, True, (30, 20))
        r.frame(12.0, 1, menu_image(40, 30))
        r.popup(20.0, 1, False, (30, 20))
        r.frame(21.0, 1, menu_image(40, 30))  # (a frame already rendering as it hid)
        r.frame(30.0, 0, Image.new("RGB", (100, 60), MAIN))
        r.write()
        frames = self.merged()[0] if popup_frames.merge(self.d) else []
        self.assertEqual([f["t"] for f in frames], [1.0, 12.0, 20.0, 30.0])
        self.assertEqual(self.image(frames[-1]["file"]).getpixel((40, 30)), MAIN)

    def test_a_recording_without_menus_is_left_as_it_was(self):
        r = Recording(self.d)
        r.frame(1.0, 0, Image.new("RGB", (100, 60), MAIN))
        r.frame(2.0, 0, Image.new("RGB", (100, 60), MAIN))
        r.write(menus=())
        before = open(os.path.join(self.d, "frames.tsv")).read()
        self.assertEqual(popup_frames.merge(self.d, dpr=2), [])
        self.assertEqual(open(os.path.join(self.d, "frames.tsv")).read(), before)

    def test_frames_of_a_menu_not_of_the_main_window_are_removed(self):
        # a menu window that rendered but never was one of the main window's (no popup line): not drawn, not kept
        r = Recording(self.d)
        r.frame(1.0, 0, Image.new("RGB", (100, 60), MAIN))
        r.frame(2.0, 1, menu_image(40, 30))
        r.write()
        self.assertEqual(popup_frames.merge(self.d), [])
        frames = self.merged()[0]
        self.assertEqual([f["win"] for f in frames], [0])
        self.assertEqual(sorted(f for f in os.listdir(self.d) if f.endswith(".png")), [frames[0]["file"]])


class SessionStripDrawsMenusIn(unittest.TestCase):
    """Session.strip merges the menus into the frames the probe saved and lists them in meta.json."""

    def test_strip(self):
        raw = tempfile.mkdtemp(dir=_TMP)

        class FakeSession(shotlib.Session):
            def __init__(self):  # (no App)
                self.tag, self.scale, self.size, self.suffix = "dark", 2, (100, 60), "dark_2x_100x60"

            def cmd(self, c, timeout=0):
                if c.startswith("stripstop "):
                    rec = Recording(c.split(" ", 1)[1])
                    rec.frame(1.0, 0, Image.new("RGB", (80, 50), MAIN))
                    rec.popup(10.0, 1, True, (30, 20))
                    rec.frame(12.0, 1, menu_image(80, 60))
                    rec.write()
                    return "saved 2 frames"
                return "ok"

            def rect(self, name):
                return None

        with mock.patch.object(shotlib, "STRIPS_RAW", raw), mock.patch.object(shotlib.time, "sleep"):
            d = FakeSession().strip("menu-open-close", lambda: None, crop=(20.0, 10.0, 40.0, 25.0))
        with open(os.path.join(d, "meta.json")) as f:
            meta = json.load(f)
        self.assertEqual(meta["popup_windows"], [1])
        frames = popup_frames.read_tsv(d)[0]
        self.assertEqual([f["win"] for f in frames], [0, 0])
        im = Image.open(os.path.join(d, frames[1]["file"])).convert("RGB")
        self.assertEqual(im.getpixel((20, 20)), EDGE)  # (30, 20) - the crop's (20, 10), at scale 2


class PrivacyOfMenuFrames(unittest.TestCase):
    """The pack needs the drawn-in menus' items in the trace, and paints a home-folder item of a menu out of the
    frames it is drawn into."""

    def setUp(self):
        self.rd = tempfile.mkdtemp(dir=_TMP)
        os.makedirs(os.path.join(self.rd, "shots", "layout"))
        self.d = os.path.join(self.rd, "strips-raw", "menu-open-close_dark_1x")
        p = mock.patch.dict(os.environ, {"HOME": "/home/example"})
        p.start()
        self.addCleanup(p.stop)

    def record(self, trace_rows):
        r = Recording(self.d)
        r.frame(1.0, 0, Image.new("RGB", (100, 60), MAIN))
        r.popup(10.0, 1, True, (30, 20))
        menu = menu_image(40, 30)
        menu.paste(Image.new("RGBA", (30, 8), (250, 250, 250, 255)), (5, 4))  # (its item's text, bright)
        r.frame(12.0, 1, menu)
        r.write()
        drawn = popup_frames.merge(self.d)
        with open(os.path.join(self.d, "opacity.tsv"), "w") as f:
            f.write("#t0\t0\n" + "".join(row + "\n" for row in trace_rows))
        with open(os.path.join(self.d, "meta.json"), "w") as f:
            json.dump(dict(name="menu-open-close", scale=1, neutral=True, popup_windows=drawn), f)

    def test_a_menu_whose_items_are_not_traced_is_unverified(self):
        self.record(["root/statusLabel\t1\t0,0,50,10\t0:1\t-"])
        fails, _n = pack.privacy_evidence(self.rd, [])
        self.assertEqual(len(fails), 1)
        self.assertIn("menu window(s) [1]", fails[0])

    def test_a_home_folder_item_of_a_menu_is_painted_out(self):
        # its item at (5, 4) in the menu, so at (35, 24) in the main window, as the probe traces it
        self.record(["menu/recentFile\t1\t35,24,30,8\t0:1\tpopup@w1"])
        walk = dict(windows=[dict(main=True, texts=[]),
                             dict(texts=[dict(name="recentFile", text="/home/example/Music/a.flac", rect=[5, 4, 30, 8])],
                                  main_popup_offset=[30, 20])])
        Image.new("RGB", (100, 60), MAIN).save(os.path.join(self.rd, "shots", "menu.png"))
        with open(os.path.join(self.rd, "shots", "layout", "menu.json"), "w") as f:
            json.dump(walk, f)
        e = dict(file="menu.png", scale=1, layout="shots/layout/menu.json", grabbed=dict(kind="main"))
        self.assertEqual(pack.redact_round(self.rd, [e]), [])
        frames = popup_frames.read_tsv(self.d)[0]
        im = Image.open(os.path.join(self.d, frames[1]["file"])).convert("RGB")
        left = [im.getpixel((x, y)) for y in range(24, 32) for x in range(35, 65)].count((250, 250, 250))
        self.assertEqual(left, 0, f"{left} px of the menu's home-folder item not painted")


if __name__ == "__main__":
    unittest.main()
