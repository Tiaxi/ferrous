#!/usr/bin/env python3
"""Unit tests of driver.py's KWin session: how a session's scale reaches KWin and the app (#665).

Self-contained: a temporary work folder, no KWin, no app. Run with
  python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'
"""
import json
import os
import sys
import tempfile
import unittest

_WORK = tempfile.TemporaryDirectory(prefix="ui-capture-test-")
# driver.py reads these when imported; never the real work folder or a real binary
os.environ["SHOT_WORK"] = _WORK.name
os.environ["SHOT_APP"] = os.path.join(_WORK.name, "no-such-ferrous")
os.environ.pop("SHOT_NEUTRAL_WORK", None)
os.environ.pop("SHOT_PROBE_DIR", None)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import driver  # noqa: E402


def tearDownModule():
    _WORK.cleanup()


class KWinSessionTest(unittest.TestCase):
    def session(self, scale, size=(1600, 1000)):
        iso = tempfile.mkdtemp(dir=_WORK.name)
        argv = driver.kwin_session(iso, size, scale, 'exec "/app"')
        config = json.load(open(os.path.join(iso, "config", "kwinoutputconfig.json")))
        launcher = open(os.path.join(iso, "launch.sh")).read()
        return argv, config, launcher

    def test_output_is_logical_size_at_the_scale(self):
        for scale in (1, 2):
            argv, config, _ = self.session(scale)
            opt = lambda name: argv[argv.index(name) + 1]
            # the same logical output at every scale: the window's size plus room, scaled by KWin alone
            self.assertEqual((opt("--width"), opt("--height"), opt("--scale")), ("2000", "1400", str(scale)))
            outputs = {o["name"]: o["data"] for o in config}
            out = outputs["outputs"][0]
            self.assertEqual(out["connectorName"], "Virtual-0")
            self.assertEqual(out["scale"], scale)
            self.assertEqual((out["mode"]["basic"]["width"], out["mode"]["basic"]["height"]),
                             (2000 * scale, 1400 * scale))
            self.assertEqual(outputs["setups"][0]["outputs"][0]["outputIndex"], 0)
            self.assertTrue(outputs["setups"][0]["outputs"][0]["enabled"])

    def test_app_is_not_scaled_by_qt(self):
        for scale in (1, 2):
            argv, _, launcher = self.session(scale)
            self.assertEqual(argv[-1].rsplit("/", 1)[-1], "launch.sh")
            self.assertNotIn("export QT_SCALE_FACTOR", launcher)
            unset = next(ln for ln in launcher.splitlines() if ln.startswith("unset "))
            for var in ("QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_ENABLE_HIGHDPI_SCALING"):
                self.assertIn(var, unset.split())
            self.assertIn("QT_QPA_PLATFORM=wayland", launcher)
            self.assertTrue(launcher.rstrip().endswith('exec "/app"'))
        # scale 2 differs from scale 1 only in KWin's output
        self.assertEqual(self.session(1)[2], self.session(2)[2])

    def test_output_has_room_for_the_window(self):
        argv, config, _ = self.session(2, size=(1000, 720))
        self.assertEqual((argv[argv.index("--width") + 1], argv[argv.index("--height") + 1]), ("1400", "1120"))
        self.assertEqual(config[0]["data"][0]["mode"]["basic"]["width"], 2800)


if __name__ == "__main__":
    unittest.main()
