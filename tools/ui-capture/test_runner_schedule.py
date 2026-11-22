#!/usr/bin/env python3
"""Unit tests of runner.py's schedule: the session-time estimates (this round, the work folder's history, other
rounds, defaults, in that order), the history a whole run records, and the longest-first plan.

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import json
import os
import shutil
import sys
import tempfile
import unittest

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
# (before the harness's modules load: shotlib reads the round's folder when imported)
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "round", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import runner  # noqa: E402


def tearDownModule():
    shutil.rmtree(_TMP)


def timing(rd, session, theme, total):
    os.makedirs(os.path.join(rd, "timings"), exist_ok=True)
    with open(os.path.join(rd, "timings", f"{session}_{theme}.json"), "w") as f:
        json.dump(dict(session=session, theme=theme, total=total, lines={}), f)


class Schedule(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(dir=_TMP)
        self.rounds = os.path.join(self.d, "rounds")
        self.rd = os.path.join(self.rounds, "r2")
        os.makedirs(self.rd)
        self.hist = os.path.join(self.d, "work", "run", runner.HISTORY)

    def write(self, hist):
        with open(self.hist, "w") as f:
            f.write(hist if isinstance(hist, str) else json.dumps(hist))

    def test_order_round_then_history_then_other_rounds_then_defaults(self):
        other = os.path.join(self.rounds, "r1")
        timing(other, "main", "dark", 11.0)
        timing(other, "menus", "dark", 12.0)
        timing(other, "queue", "dark", 13.0)
        os.makedirs(os.path.dirname(self.hist))
        self.write({"main:dark": 21.0, "menus:dark": 22.0})
        timing(self.rd, "main", "dark", 31.0)
        est = runner.estimates(self.rd, history=self.hist)
        self.assertEqual(est(("main", "dark")), 31.0)  # this round
        self.assertEqual(est(("menus", "dark")), 22.0)  # the history
        self.assertEqual(est(("queue", "dark")), 13.0)  # another round
        self.assertGreater(est(("bigfont", "dark")), 60)  # a default from a measured run, not an old guess
        self.assertEqual(est(("no-such-session", "dark")), 30)

    def test_a_new_out_folder_takes_the_history(self):
        # (no rounds beside it: before the history, every session got a default)
        os.makedirs(os.path.dirname(self.hist))
        self.write({"bigfont:light": 94.6})
        self.assertEqual(runner.estimates(self.rd, history=self.hist)(("bigfont", "light")), 94.6)

    def test_record_history_keeps_other_sessions_and_skips_missing_timings(self):
        os.makedirs(os.path.dirname(self.hist))
        self.write({"main:dark": 1.0, "icons:light": 2.0})
        timing(self.rd, "main", "dark", 85.0)
        runner.record_history(self.rd, [("main", "dark"), ("motion", "dark")], path=self.hist)
        self.assertEqual(runner.load_json(self.hist), {"main:dark": 85.0, "icons:light": 2.0})
        self.assertFalse(os.path.exists(self.hist + ".tmp"))

    def test_record_history_starts_a_missing_or_broken_file(self):
        timing(self.rd, "main", "dark", 85.0)
        runner.record_history(self.rd, [("main", "dark")], path=self.hist)
        self.assertEqual(runner.load_json(self.hist), {"main:dark": 85.0})
        self.write("{not json")
        runner.record_history(self.rd, [("main", "dark")], path=self.hist)
        self.assertEqual(runner.load_json(self.hist), {"main:dark": 85.0})

    def test_plan_puts_the_longest_jobs_first_on_the_least_loaded_worker(self):
        t = {("a", "dark"): 90, ("b", "dark"): 60, ("c", "dark"): 50, ("d", "dark"): 40, ("e", "dark"): 10}
        loads = runner.plan(list(t), 2, t.get)
        self.assertEqual(sorted(l for l, _ in loads), [120, 130])
        self.assertEqual(loads[0][1][0], ("a", "dark"))


if __name__ == "__main__":
    unittest.main()
