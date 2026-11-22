#!/usr/bin/env python3
"""Regression test of the fixture folders the dialog and queue macros write: sessions that run one at the same time
keep each other's files whole, at paths that don't change with the worker.

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import fcntl
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

_TMP = tempfile.mkdtemp(prefix="ui-capture-test-")
os.environ.setdefault("SHOT_WORK", os.path.join(_TMP, "work"))
os.environ.setdefault("SHOT_OUT", os.path.join(_TMP, "round", "shots"))
os.environ.setdefault("SHOT_APP", os.path.join(_TMP, "no-app"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import focus_select  # noqa: E402
import states  # noqa: E402


def tearDownModule():
    shutil.rmtree(_TMP)


class SharedFixtures(unittest.TestCase):
    def test_only_one_session_at_a_time_changes_the_tag_save_fixture(self):
        taken = []

        @states.one_at_a_time
        def m_change(r, arg):
            with open(os.path.join(_TMP, ".m_change.lock"), "w") as other:
                try:
                    fcntl.flock(other, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    taken.append("a second session got in")
                except BlockingIOError:
                    taken.append("a second session waits")

        with mock.patch.dict(os.environ, TMPDIR=_TMP):
            m_change(None, "")
        self.assertEqual(taken, ["a second session waits"])
        self.assertEqual(states.MACROS["tagsavefail"].__wrapped__.__name__, "m_tagsavefail")

    def test_a_reader_keeps_a_whole_file_while_it_is_replaced(self):
        path = os.path.join(_TMP, "track.flac")
        states.put_fixture(path, b"old" * 1000)
        with open(path, "rb") as reading:
            states.put_fixture(path, b"new")
            self.assertEqual(reading.read(), b"old" * 1000)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b"new")
        self.assertEqual([n for n in os.listdir(_TMP) if n.startswith(".track")], [])

    def test_the_selection_still_reads_the_wrapped_macros_writes(self):
        with open(os.path.splitext(states.__file__)[0] + ".py", encoding="utf-8") as f:
            writers = focus_select.Writers(f.read())
        self.assertEqual(states.MACROS["tagsavefail"].__name__, "m_tagsavefail")
        self.assertIn(("fact", ("tagsave-", False)), writers.of("m_tagsavefail"))


if __name__ == "__main__":
    unittest.main()
