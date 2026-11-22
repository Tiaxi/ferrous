#!/usr/bin/env python3
"""Regression tests of the menus that showed in place of the ones their states name: a session gives the app one
pointer enter at a neutral spot before any step (Qt on Wayland takes the pointer to be at 0, 0, on the menu bar's
File, until it has had one), and `menu I` fails unless menu I is what shows.

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import os
import shutil
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "round", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kwin_input  # noqa: E402
import runner  # noqa: E402
import shotlib  # noqa: E402


def tearDownModule():
    shutil.rmtree(_TMP)


class FakeInput:
    def __init__(self, log):
        self.log = log

    def move(self, x, y):
        self.log.append(("move", x, y))

    def key(self, code):
        self.log.append(("key", code))

    def close(self):
        self.log.append(("close",))


class PrimeInput(unittest.TestCase):
    def prime(self, cursors, frame=(0.0, 0.0, 1600.0, 1030.0)):
        log, left = [], list(cursors)

        def cmd(text, timeout=20):
            self.assertEqual(text, "cursor")
            log.append(("cursor", left[0]))
            return left.pop(0) if len(left) > 1 else left[0]

        s = shotlib.Session.__new__(shotlib.Session)
        s.size = (1600, 1000)
        s.a = types.SimpleNamespace(iso=_TMP, wait_for=lambda *a, **kw: True)
        s.ev, s.kwin_frame, s.cmd = (lambda js: "Ferrous"), (lambda title: frame), cmd
        with mock.patch.object(kwin_input, "KWinInput", lambda path: FakeInput(log)):
            try:
                s.prime_input()
            finally:
                self.log = log
        return log

    def test_the_pointer_enters_at_the_neutral_spot_then_leaves(self):
        log = self.prime(["1595 450"])
        # (below the title bar of 30 px: where away() points)
        self.assertEqual(log, [("move", 1595, 480), ("cursor", "1595 450"), ("move", 1999, 1399),
                               ("key", kwin_input.KEY_LEFTSHIFT), ("close",)])

    def test_the_pointer_leaves_only_once_the_app_has_the_enter(self):
        log = self.prime(["0 0", "0 0", "1595 450"])
        self.assertEqual([e[0] for e in log], ["move", "cursor", "cursor", "cursor", "move", "key", "close"])

    def test_an_enter_that_never_arrives_fails_the_session(self):
        with mock.patch.object(shotlib.time, "time", side_effect=iter(range(100))), \
                mock.patch.object(shotlib.time, "sleep"):
            with self.assertRaisesRegex(RuntimeError, "never reached"):
                self.prime(["0 0"])
        self.assertEqual(self.log[-1], ("close",))

    def test_a_window_kwin_does_not_know_fails_the_session(self):
        with self.assertRaisesRegex(RuntimeError, "no frame"):
            self.prime(["1595 450"], frame=None)

    def test_the_pointer_request_on_the_wire(self):
        inp = kwin_input.KWinInput.__new__(kwin_input.KWinInput)
        sent = []
        inp.fake = 4
        inp._send = lambda obj, op, body=b"": sent.append((obj, op, body))
        inp._roundtrip = lambda: None
        inp.move(1595, 480.5)
        # pointer_motion_absolute, in wl_fixed (1/256 px)
        self.assertEqual(sent, [(4, 9, struct.pack("<ii", 1595 * 256, 480 * 256 + 128))])


class MenuStep(unittest.TestCase):
    def step(self, shown, exposed):
        s = mock.Mock()
        s.ev.return_value = shown
        r = runner.Runner(s)
        r.point = lambda spec: (20.0, 10.0)
        r.settle_or = lambda t: None
        r.popup_open = mock.Mock(side_effect=exposed)
        with mock.patch.object(runner.time, "time", side_effect=iter(n / 4 for n in range(100))), \
                mock.patch.object(runner.time, "sleep"):
            r.step_menu("1")
        return r

    def test_the_menu_named_passes(self):
        self.step("1", [True])

    def test_another_menu_fails_the_state(self):
        with self.assertRaisesRegex(RuntimeError, r"menus shown are \[0\]"):
            self.step("0", [True] * 100)

    def test_no_menu_fails_the_state(self):
        with self.assertRaisesRegex(RuntimeError, r"menus shown are \[\]"):
            self.step("", [False] * 100)

    def test_it_waits_for_the_menus_window(self):
        self.assertEqual(self.step("1", [False, True]).popup_open.call_count, 2)

    def test_a_window_that_never_shows_fails_the_state(self):
        with self.assertRaisesRegex(RuntimeError, "its window is not shown"):
            self.step("1", [False] * 100)


if __name__ == "__main__":
    unittest.main()
