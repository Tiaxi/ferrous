#!/usr/bin/env python3
"""Tests of shoot_round.sh's options that need no KWin, app, build or shots.

A copy of shoot_round.sh runs beside stand-in helpers (runner.py, pack.py, the fixture makers, ...) that do
nothing but record how they were called, with stand-in cmake and gst-launch-1.0 first on PATH, on a stand-in
source tree, all in a temporary folder. So a run takes well under a second and touches nothing outside it.

    python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'
"""
import json
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "shoot_round.sh")

# the helpers shoot_round.sh runs from its own folder; make_fixtures.py makes the folder it is given (the script
# then renames it into place), setup_db_round.py the template folder (the script writes its key there), and
# pack.py records its arguments
NOOP_PY = ["runner.py", "motion_inventory.py", "contrast.py", "layout.py",
           "make_extra_fixtures.py", "make_cover_art.py", "make_broadband_fixtures.py",
           "make_standin_fixtures.py", "make_large_library.py", "make_library2.py"]
MAKE_DIR_PY = "import os, sys\nos.makedirs(sys.argv[1], exist_ok=True)\n"
SETUP_DB_PY = "import os\nos.makedirs(os.path.join(os.environ['SHOT_WORK'], 'run', 'template'), exist_ok=True)\n"
PACK_PY = "import json, os, sys\njson.dump(sys.argv[1:], open(os.environ['PACK_ARGV_OUT'], 'w'))\n"


def write(path, text, mode=0o644):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    os.chmod(path, mode)


class ShootRoundOptions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="shoot-round-test-")
        self.addCleanup(shutil.rmtree, self.tmp)
        self.h = os.path.join(self.tmp, "harness")
        os.makedirs(self.h)
        shutil.copy2(SCRIPT, self.h)
        for name in NOOP_PY:
            write(os.path.join(self.h, name), "")
        write(os.path.join(self.h, "make_fixtures.py"), MAKE_DIR_PY)
        write(os.path.join(self.h, "setup_db_round.py"), SETUP_DB_PY)
        write(os.path.join(self.h, "pack.py"), PACK_PY)
        write(os.path.join(self.h, "build_probes.sh"), "#!/bin/sh\nexit 0\n", 0o755)
        bindir = os.path.join(self.tmp, "bin")
        for tool in ("cmake", "gst-launch-1.0"):
            write(os.path.join(bindir, tool), "#!/bin/sh\nexit 0\n", 0o755)
        self.src = os.path.join(self.tmp, "src")
        write(os.path.join(self.src, "ui", "CMakeLists.txt"), "")
        write(os.path.join(self.src, "src", "main.rs"), "")
        write(os.path.join(self.src, "Cargo.lock"), "")
        self.argv_out = os.path.join(self.tmp, "pack-argv.json")
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("SHOT_")}
        self.env.update(PATH=bindir + os.pathsep + os.environ.get("PATH", ""),
                        HOME=self.tmp, XDG_CACHE_HOME=os.path.join(self.tmp, "cache"),
                        SHOT_WORK=os.path.join(self.tmp, "work"), SHOT_NEUTRAL="0", PACK_ARGV_OUT=self.argv_out)

    def run_script(self, *args):
        return subprocess.run(["bash", os.path.join(self.h, "shoot_round.sh"), "r1", self.src, *args],
                              env=self.env, cwd=self.tmp, capture_output=True, text=True, timeout=60)

    def pack_argv(self, *args):
        p = self.run_script(*args)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        with open(self.argv_out) as f:
            return json.load(f)

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

    def test_pack_jobs_needs_a_value(self):
        p = self.run_script("--pack-jobs")
        self.assertEqual(p.returncode, 2, p.stdout + p.stderr)
        self.assertFalse(os.path.exists(self.argv_out))


if __name__ == "__main__":
    unittest.main()
