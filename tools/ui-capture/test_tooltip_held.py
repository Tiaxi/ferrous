#!/usr/bin/env python3
"""Regression test of the close-ups' race with the button's tooltip: their grabs hold it back rather than having to
finish inside its 700 ms, and always give it its own delay again.

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import os
import re
import shutil
import sys
import tempfile
import types
import unittest

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "round", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import focus_select  # noqa: E402
import states  # noqa: E402


def tearDownModule():
    shutil.rmtree(_TMP)


class TooltipHeld(unittest.TestCase):
    def setUp(self):
        self.delay = 700
        self.r = types.SimpleNamespace(s=types.SimpleNamespace(ev=self.ev))

    def ev(self, js):
        m = re.search(r"ToolTip\.delay=(\d+)", js)
        if m:
            self.delay = int(m.group(1))
        return True if "return true" in js else self.delay

    def test_held_for_the_grabs_then_its_own_again(self):
        with states.tooltip_held(self.r, "button"):
            self.assertEqual(self.delay, 60000)
        self.assertEqual(self.delay, 700)

    def test_its_own_again_after_a_failed_grab(self):
        with self.assertRaises(RuntimeError):
            with states.tooltip_held(self.r, "button"):
                raise RuntimeError("grab failed")
        self.assertEqual(self.delay, 700)

    def test_a_button_without_a_delay_fails_before_any_grab(self):
        self.delay = None
        with self.assertRaisesRegex(RuntimeError, "no tooltip delay"):
            with states.tooltip_held(self.r, "button"):
                self.fail("grabbed")

    def test_the_selection_still_reads_the_macros_writes(self):
        with open(os.path.splitext(states.__file__)[0] + ".py", encoding="utf-8") as f:
            writers = focus_select.Writers(f.read())
        self.assertEqual(writers.problems, [])
        self.assertIn(("shot", ("closeup", True), ("controls", True)), writers.of("m_closeups"))


if __name__ == "__main__":
    unittest.main()
