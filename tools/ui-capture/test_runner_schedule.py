#!/usr/bin/env python3
"""Unit tests of runner.py's schedule: the session-time estimates (this round, the work folder's history, other
rounds, defaults, in that order), the history a whole run records, and the queue its workers share.

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
# (before the harness's modules load: shotlib reads the round's folder when imported)
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "round", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import runner  # noqa: E402


def tearDownModule():
    shutil.rmtree(_TMP)


def timing(rd, session, theme, total, line_s=(), **kw):
    os.makedirs(os.path.join(rd, "timings"), exist_ok=True)
    with open(os.path.join(rd, "timings", f"{session}_{theme}.json"), "w") as f:
        json.dump(dict(session=session, theme=theme, total=total, lines={}, line_s=list(line_s), **kw), f)


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
        self.write({"main:dark": dict(total=21.0), "menus:dark": dict(total=22.0)})
        timing(self.rd, "main", "dark", 31.0)
        est = runner.estimates(self.rd, history=self.hist)
        self.assertEqual(est(("main", "dark", None)), 31.0)  # this round
        self.assertEqual(est(("menus", "dark", None)), 22.0)  # the history
        self.assertEqual(est(("queue", "dark", None)), 13.0)  # another round
        self.assertGreater(est(("bigfont", "dark", None)), 60)  # a default from a measured run, not an old guess
        self.assertEqual(est(("no-such-session", "dark", None)), 30)

    def test_a_new_out_folder_takes_the_history(self):
        # (no rounds beside it: before the history, every session got a default)
        os.makedirs(os.path.dirname(self.hist))
        self.write({"bigfont:light": dict(total=94.6)})
        self.assertEqual(runner.estimates(self.rd, history=self.hist)(("bigfont", "light", None)), 94.6)

    def test_a_part_of_a_session_is_the_whole_less_the_lines_after_it(self):
        with mock.patch.object(runner.states, "STATES", [("s", "a", str(k), "", "") for k in range(4)]):
            timing(self.rd, "s", "dark", 20.0, line_s=[1.0, 2.0, 3.0, 8.0])
            self.assertEqual(runner.estimates(self.rd, history=self.hist)(("s", "dark", 2)), 9.0)
            timing(self.rd, "s", "dark", 20.0)  # (no line times: by the share of its lines)
            self.assertEqual(runner.estimates(self.rd, history=self.hist)(("s", "dark", 2)), 10.0)

    def test_the_timings_of_a_part_are_no_estimate_of_the_session(self):
        timing(self.rd, "bigfont", "dark", 5.0, part=9)
        self.assertGreater(runner.estimates(self.rd, history=self.hist)(("bigfont", "dark", None)), 60)

    def test_record_history_keeps_other_sessions_and_skips_parts(self):
        os.makedirs(os.path.dirname(self.hist))
        self.write({"main:dark": dict(total=1.0), "icons:light": dict(total=2.0)})
        timing(self.rd, "main", "dark", 85.0, line_s=[4.0])
        timing(self.rd, "icons", "light", 1.0, part=True)
        timing(self.rd, "queue", "dark", 3.0)
        runner.record_history(self.rd, [("main", "dark", None), ("motion", "dark", None), ("icons", "light", None),
                                        ("queue", "dark", 5)], path=self.hist)
        self.assertEqual(runner.load_json(self.hist),
                         {"main:dark": dict(total=85.0, lines=[4.0]), "icons:light": dict(total=2.0)})
        self.assertFalse(os.path.exists(self.hist + ".tmp"))

    def test_record_history_starts_a_missing_or_broken_file(self):
        timing(self.rd, "main", "dark", 85.0)
        want = {"main:dark": dict(total=85.0, lines=[])}
        runner.record_history(self.rd, [("main", "dark", None)], path=self.hist)
        self.assertEqual(runner.load_json(self.hist), want)
        self.write("{not json")
        runner.record_history(self.rd, [("main", "dark", None)], path=self.hist)
        self.assertEqual(runner.load_json(self.hist), want)

    def test_a_job_crosses_to_its_worker_unchanged(self):
        for job in (("main", "dark", None), ("narrow-bigfont", "light", 11)):
            self.assertEqual(runner.decode_job(runner.encode_job(job)), job)

    def test_each_queued_job_is_claimed_once(self):
        a, b = ("main", "dark", None), ("main", "light", None)
        self.assertTrue(runner.claim(self.d, a))
        self.assertFalse(runner.claim(self.d, a))
        self.assertTrue(runner.claim(self.d, b))

    def test_a_claimed_job_that_never_finished_is_lost(self):
        a, b = ("main", "dark", None), ("main", "light", None)
        runner.claim(self.d, a)
        runner.claim(self.d, b)
        runner.mark_done(self.d, b)
        self.assertEqual(runner.unfinished(self.d, [a, b]), [a])

    def test_a_worker_records_its_failures_before_the_job_counts_as_finished(self):
        claims = os.path.join(self.d, "claims")
        os.makedirs(claims)
        seen = []

        def done(folder, job):
            with open(os.path.join(self.rd, "failures.txt")) as f:
                seen.append(f.read())

        argv = ["runner.py", "--jobs", "main:dark:3", "--claims", claims]
        with mock.patch.object(sys, "argv", argv), mock.patch.object(runner, "mark_done", done), \
                mock.patch.dict(os.environ, SHOT_OUT=os.path.join(self.rd, "shots")), \
                mock.patch.object(runner, "run_session", return_value=["main/dark: broke"]) as run:
            runner.main()
        self.assertEqual(seen, ["main/dark: broke\n"])
        self.assertEqual(len(run.call_args.args[2]), 3)  # (only the lines up to the cut)


if __name__ == "__main__":
    unittest.main()
