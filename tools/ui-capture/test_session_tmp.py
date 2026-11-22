#!/usr/bin/env python3
"""Regression test of the fixture folders the dialog and queue macros write: sessions that run one at the same time
share none, and the folder's shown name stays.

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import os
import shutil
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

import states  # noqa: E402


def tearDownModule():
    shutil.rmtree(_TMP)


class SessionTmp(unittest.TestCase):
    def test_sessions_share_no_fixture_folder(self):
        with mock.patch.dict(os.environ, TMPDIR=_TMP):
            got = [states.session_tmp(types.SimpleNamespace(s=types.SimpleNamespace(a=types.SimpleNamespace(
                iso=f"/work/run/r1-w{worker}-{name}"))), "ferrous-tagsave")
                for worker, name in ((0, "dialogs-dark"), (1, "dialogs-light"), (2, "bigfont-dark"))]
        self.assertEqual(len(set(got)), 3)
        self.assertEqual({os.path.basename(p) for p in got}, {"ferrous-tagsave"})

    def test_the_writing_macros_use_it(self):
        for macro in ("tagsavefail", "renamebatch", "unreadable"):
            self.assertIn("session_tmp", states.MACROS[macro].__code__.co_names, macro)


if __name__ == "__main__":
    unittest.main()
