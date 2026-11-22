#!/usr/bin/env python3
"""Tests of shoot_round.sh's options that need no KWin, app, build or shots.

A copy of shoot_round.sh runs beside stand-in helpers (runner.py, pack.py, the fixture makers, ...) that do
nothing but record how they were called, with stand-in cmake, gst-launch-1.0, rsync and bc first on PATH, on a
stand-in source tree, all in a temporary folder. So a run takes well under a second and touches nothing outside it.

    python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import focus  # noqa: E402

SCRIPT = os.path.join(HERE, "shoot_round.sh")

# the helpers shoot_round.sh runs from its own folder; make_fixtures.py makes the folder it is given (the script
# then renames it into place), setup_db_round.py the template folder (the script writes its key there), and
# pack.py records its arguments
NOOP_PY = ["motion_inventory.py", "contrast.py", "layout.py",
           "make_extra_fixtures.py", "make_cover_art.py", "make_broadband_fixtures.py",
           "make_standin_fixtures.py", "make_large_library.py", "make_library2.py"]
MAKE_DIR_PY = "import os, sys\nos.makedirs(sys.argv[1], exist_ok=True)\n"
SETUP_DB_PY = "import os\nos.makedirs(os.path.join(os.environ['SHOT_WORK'], 'run', 'template'), exist_ok=True)\n"
# rsync -a ... SRC/ DEST/: a plain copy of the stand-in tree; bc: a fixed elapsed time
RSYNC_SH = '#!/bin/sh\nfor a; do src=$dst; dst=$a; done\ncp -a "$src." "$dst"\n'
BC_SH = "#!/bin/sh\ncat >/dev/null\necho 0\n"
PACK_PY = "import json, os, sys\njson.dump(sys.argv[1:], open(os.environ['PACK_ARGV_OUT'], 'w'))\n"
# a pack.py that writes one pack per scope of SHOT_FOCUS, but not FOCUS_PACK_SKIP's, and exits FOCUS_PACK_EXIT
FOCUS_PACK_PY = """import os, sys
for nm in os.environ["SHOT_FOCUS"].split(","):
    if nm == os.environ.get("FOCUS_PACK_SKIP"):
        continue
    fd = os.path.join(sys.argv[1], "focus-build", nm)
    os.makedirs(fd, exist_ok=True)
    with open(os.path.join(fd, "FOCUS.md"), "w") as f:
        f.write(nm + "\\n")
    with open(os.path.join(fd, "F01_image.png"), "w"):
        pass
    if nm == os.environ.get("FOCUS_PACK_UNREADABLE"):
        os.chmod(os.path.join(fd, "F01_image.png"), 0)
sys.exit(int(os.environ.get("FOCUS_PACK_EXIT", "0")))
"""
RUNNER_PY = """import json, os, sys
with open(os.environ["RUNNER_ARGV_OUT"], "w") as f:
    json.dump(sys.argv[1:], f)
if os.environ.get("RUNNER_FAILURE"):
    with open(os.path.join(os.path.dirname(os.environ["SHOT_OUT"]), "failures.txt"), "w") as f:
        f.write(os.environ["RUNNER_FAILURE"] + "\\n")
"""


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


class ShootRoundBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shoot-round-test-")
        self.addCleanup(shutil.rmtree, self.tmp)
        self.h = os.path.join(self.tmp, "harness")
        os.makedirs(self.h)
        shutil.copy2(SCRIPT, self.h)
        shutil.copy2(os.path.join(HERE, "focus.py"), self.h)  # (the real scopes: it needs nothing else)
        for name in NOOP_PY:
            write(os.path.join(self.h, name), "")
        write(os.path.join(self.h, "make_fixtures.py"), MAKE_DIR_PY)
        write(os.path.join(self.h, "setup_db_round.py"), SETUP_DB_PY)
        write(os.path.join(self.h, "pack.py"), PACK_PY)
        write(os.path.join(self.h, "runner.py"), RUNNER_PY)
        write(os.path.join(self.h, "build_probes.sh"), "#!/bin/sh\nexit 0\n", 0o755)
        bindir = os.path.join(self.tmp, "bin")
        for tool in ("cmake", "gst-launch-1.0"):
            write(os.path.join(bindir, tool), "#!/bin/sh\nexit 0\n", 0o755)
        write(os.path.join(bindir, "rsync"), RSYNC_SH, 0o755)
        write(os.path.join(bindir, "bc"), BC_SH, 0o755)
        write(os.path.join(bindir, "nproc"), '#!/bin/sh\necho "${STUB_NPROC:-16}"\n', 0o755)
        self.src = os.path.join(self.tmp, "src")
        write(os.path.join(self.src, "ui", "CMakeLists.txt"), "")
        write(os.path.join(self.src, "src", "main.rs"), "")
        write(os.path.join(self.src, "Cargo.lock"), "")
        self.argv_out = os.path.join(self.tmp, "pack-argv.json")
        self.runner_argv_out = os.path.join(self.tmp, "runner-argv.json")
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("SHOT_")}
        self.env.update(PATH=bindir + os.pathsep + os.environ.get("PATH", ""),
                        HOME=self.tmp, XDG_CACHE_HOME=os.path.join(self.tmp, "cache"),
                        SHOT_WORK=os.path.join(self.tmp, "work"), SHOT_NEUTRAL="0", PACK_ARGV_OUT=self.argv_out,
                        RUNNER_ARGV_OUT=self.runner_argv_out)

    def run_script(self, *args):
        return subprocess.run(["bash", os.path.join(self.h, "shoot_round.sh"), "r1", self.src, *args],
                              env=self.env, cwd=self.tmp, capture_output=True, text=True, timeout=60)


class ShootRoundOptions(ShootRoundBase):
    def pack_argv(self, *args):
        p = self.run_script(*args)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        with open(self.argv_out) as f:
            return json.load(f)

    def runner_argv(self, *args):
        self.pack_argv(*args)
        with open(self.runner_argv_out) as f:
            return json.load(f)

    def test_workers_default_to_half_the_threads_from_4_to_8(self):
        for threads, workers in (("2", "4"), ("12", "6"), ("16", "8"), ("64", "8")):
            with self.subTest(threads=threads):
                self.env["STUB_NPROC"] = threads
                self.assertEqual(self.runner_argv(), ["--workers", workers])

    def test_workers_are_capped_at_8(self):
        self.assertEqual(self.runner_argv("--workers", "3"), ["--workers", "3"])
        self.assertEqual(self.runner_argv("--workers", "30"), ["--workers", "8"])

    def test_pack_jobs_reaches_pack(self):
        argv = self.pack_argv("--pack-jobs", "3")
        self.assertEqual(argv[2:], ["--jobs", "3"])

    def test_default_leaves_pack_its_own_count(self):
        argv = self.pack_argv()
        self.assertEqual(len(argv), 2, argv)  # the round and the source only
        self.assertNotIn("--jobs", argv)

    def test_bad_pack_jobs_fails_before_any_work(self):
        for bad in ("0", "-2", "two", "3x", ""):
            with self.subTest(value=bad):
                p = self.run_script("--pack-jobs", bad)
                self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
                self.assertIn("--pack-jobs", p.stderr)
                self.assertFalse(os.path.exists(self.env["SHOT_WORK"]))  # refused before the work folder


class ShootRoundFocus(ShootRoundBase):
    def setUp(self):
        super().setUp()
        write(os.path.join(self.h, "pack.py"), FOCUS_PACK_PY)
        write(os.path.join(self.h, "runner.py"), RUNNER_PY)
        self.runner_out = os.path.join(self.tmp, "runner-argv.json")
        self.env["RUNNER_ARGV_OUT"] = self.runner_out
        self.round = os.path.join(self.env["SHOT_WORK"], "rounds", "r1")

    def sessions_shot(self):
        with open(self.runner_out) as f:
            argv = json.load(f)
        return argv[argv.index("--sessions") + 1].split(",") if "--sessions" in argv else None

    def published(self):
        fd = os.path.join(self.round, "focus")
        return sorted(os.listdir(fd)) if os.path.isdir(fd) else []

    def test_several_scopes_are_one_run_of_their_sessions_and_a_pack_each(self):
        p = self.run_script("--focus-only", "dialogs,viewers")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.sessions_shot(), focus.sessions_of(["dialogs", "viewers"]))
        self.assertEqual(self.published(), ["dialogs", "viewers"])
        for name in ("dialogs", "viewers"):
            self.assertEqual(sorted(os.listdir(os.path.join(self.round, "focus", name))),
                             ["F01_image.png", "FOCUS.md", "source.json"])

    def test_census_is_the_seven_census_packs_from_one_run(self):
        p = self.run_script("--census")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.sessions_shot(), focus.sessions_of(focus.CENSUS))
        self.assertEqual(self.published(), sorted(focus.CENSUS))

    def test_the_runner_shoots_only_what_the_named_scopes_read(self):
        for given, scopes in (("census", focus.CENSUS), ("viewers, dialogs,viewers", ["viewers", "dialogs"])):
            with self.subTest(scopes=given):
                self.assertEqual(self.run_script("--focus-only", given).returncode, 0)
                with open(self.runner_out) as f:
                    argv = json.load(f)
                self.assertEqual(argv[argv.index("--focus") + 1], ",".join(scopes))  # (resolved: each once)

    def test_a_full_run_is_never_cut_for_its_focus_packs(self):
        self.assertEqual(self.run_script("--focus", "census").returncode, 0)
        with open(self.runner_out) as f:
            self.assertEqual(json.load(f), ["--workers", "8"])

    def test_census_takes_options_after_it(self):
        p = self.run_script("--census", "--workers", "2")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(self.published(), sorted(focus.CENSUS))

    def test_focus_after_a_full_run_shoots_everything_and_packs_in_the_round(self):
        p = self.run_script("--focus", "dialogs,chrome")
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertIsNone(self.sessions_shot())
        self.assertEqual(self.published(), ["chrome", "dialogs"])

    def test_a_later_run_replaces_its_own_scopes_packs_only(self):
        self.assertEqual(self.run_script("--focus-only", "dialogs,viewers").returncode, 0)
        old = os.path.join(self.round, "focus", "dialogs", "F14_old.png")
        kept = os.path.join(self.round, "focus", "viewers", "kept.txt")
        write(old, "")
        write(kept, "")
        self.assertEqual(self.run_script("--focus-only", "dialogs").returncode, 0)
        self.assertFalse(os.path.exists(old))
        self.assertTrue(os.path.exists(kept))

    def test_a_flat_pack_of_the_earlier_layout_is_cleared_when_publishing(self):
        fd = os.path.join(self.round, "focus")
        for name in ("FOCUS.md", "FOCUS-FACTS.md", "source.json", "F01_old.png", "notes.txt"):
            write(os.path.join(fd, name), "")
        self.assertEqual(self.run_script("--focus-only", "dialogs").returncode, 0)
        self.assertEqual(self.published(), ["dialogs", "notes.txt"])  # (a file it did not make stays)

    def test_a_failed_pack_publishes_none(self):
        self.env["FOCUS_PACK_EXIT"] = "3"  # e.g. one scope over the cap
        p = self.run_script("--focus-only", "dialogs,viewers")
        self.assertEqual(p.returncode, 3, p.stdout + p.stderr)
        self.assertEqual(self.published(), [])

    def test_a_pack_not_written_publishes_none(self):
        self.env["FOCUS_PACK_SKIP"] = "viewers"
        p = self.run_script("--focus-only", "dialogs,viewers")
        self.assertEqual(p.returncode, 3, p.stdout + p.stderr)
        self.assertEqual(self.published(), [])

    def keep_an_earlier_pack(self):
        kept = os.path.join(self.round, "focus", "dialogs", "earlier.txt")
        write(kept, "")
        return kept

    def test_a_failed_state_keeps_the_earlier_packs(self):
        self.env["RUNNER_FAILURE"] = "dialogs/dark dialog about: no window"
        kept = self.keep_an_earlier_pack()
        p = self.run_script("--focus-only", "dialogs,viewers")
        self.assertEqual(p.returncode, 4, p.stdout + p.stderr)
        self.assertEqual(self.published(), ["dialogs"])
        self.assertTrue(os.path.exists(kept))

    def test_a_failed_measurement_keeps_the_earlier_packs(self):
        write(os.path.join(self.h, "contrast.py"), "import sys\nsys.exit(1)\n")
        kept = self.keep_an_earlier_pack()
        p = self.run_script("--focus-only", "dialogs,viewers")
        self.assertEqual(p.returncode, 5, p.stdout + p.stderr)
        self.assertEqual(self.published(), ["dialogs"])
        self.assertTrue(os.path.exists(kept))

    def test_a_full_runs_failed_pack_keeps_the_earlier_packs(self):
        self.env["FOCUS_PACK_EXIT"] = "3"
        kept = self.keep_an_earlier_pack()
        p = self.run_script("--focus", "dialogs")
        self.assertEqual(p.returncode, 3, p.stdout + p.stderr)
        self.assertTrue(os.path.exists(kept))

    @unittest.skipIf(os.geteuid() == 0, "root reads any file")
    def test_a_failed_copy_replaces_no_pack_and_claims_none(self):
        self.env["FOCUS_PACK_UNREADABLE"] = "viewers"  # the second of the two
        kept = self.keep_an_earlier_pack()
        p = self.run_script("--focus-only", "dialogs,viewers")
        self.assertNotEqual(p.returncode, 0)
        self.assertTrue(os.path.exists(kept))
        self.assertNotIn("published", p.stdout)

    def test_an_unknown_scope_fails_before_any_work(self):
        p = self.run_script("--focus-only", "dialogs,no-such-scope")
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
        self.assertIn("no-such-scope", p.stderr)
        self.assertFalse(os.path.exists(self.env["SHOT_WORK"]))


if __name__ == "__main__":
    unittest.main()
