#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""GUI-thread stalls during interactions, with the probe's 1 kHz watchdog.

Usage: ui_stalls.py OUT_DIR REP SAMPLE_DIR LIBRARY_DB [SCENARIOS...]

Starts the app in an isolated tree with a copy of LIBRARY_DB (watching off,
so that no rescan runs unless a scenario asks for one), queues the local
samples S01..S06 and plays S05 (5.1), centered per channel. Each scenario is
marked "begin X"/"end X" in the probe log with the watchdog on; the log's S
lines are the stalls of at least STALL_MS (default 3) and its
"stall-hist" replies the full latency histograms. Set PERF_RECORD=1 to run
`perf record` on the app for the whole run (stacks for the stalls).
"""
import glob
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_driver as d  # noqa: E402

STALL_MS = os.environ.get("STALL_MS", "3")
KEY_ESCAPE = 0x01000000
SEEK_Y = 47          # Transport slider row (scene px), from the item dump.


def main():
    out, rep, sample_dir, db = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    only = set(sys.argv[5:])
    os.makedirs(out, exist_ok=True)
    p = d.default_paths()
    settings = os.path.join(out, f"settings-{rep}.txt")
    with open(p["settings"]) as f:
        text = f.read().replace("library_watch_enabled=1", "library_watch_enabled=0")
    with open(settings, "w") as f:
        f.write(text)
    d.wait_for_quiet()
    app = d.App(os.path.join(out, f"run-{rep}"), p["app"], p["probe"], settings, library_db=db, settle=12)
    perf = None
    if os.environ.get("PERF_RECORD"):
        perf = subprocess.Popen(["perf", "record", "-e", "cpu-clock:u", "-F", "2999", "--call-graph", "fp",
                                 "--user-callchains", "-k", "CLOCK_MONOTONIC", "-p", str(app.pid),
                                 "-o", os.path.join(out, f"run-{rep}", "perf.data")],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)

    samples = {sid: glob.glob(os.path.join(sample_dir, sid + ".*"))[0] for sid in
               ("S01", "S02", "S03", "S04", "S05", "S06")}

    def scenario(name, body, tail=1.5):
        if only and name not in only:
            return
        app.cmd(f"stall on {STALL_MS}")
        time.sleep(0.3)
        app.mark(f"begin {name}")
        body()
        time.sleep(tail)
        app.mark(f"end {name}")
        app.cmd("stall off")
        time.sleep(0.5)
        print(name, "done", flush=True)

    def seek_x(frac):
        x0, w = 194, 2924
        return x0 + frac * w

    try:
        # Queue the samples and play S05 (5.1, 96 kHz), the heavier case.
        paths = [samples[s] for s in ("S01", "S02", "S03", "S04", "S05", "S06")]
        app.eval("root.uiBridge.replaceWithPaths(" + str(paths).replace("'", '"') + ")")
        time.sleep(1)
        app.eval("root.uiBridge.playAt(4)")
        time.sleep(20)

        scenario("steady_playback", lambda: time.sleep(10), 0)

        def track_changes():
            for i in (5, 0, 1, 4):
                app.eval(f"root.uiBridge.playAt({i})")
                time.sleep(4)
        scenario("track_change", track_changes)
        time.sleep(15)

        def seek_clicks():
            for frac in (0.6, 0.2, 0.8, 0.4, 0.65):
                app.cmd(f"click {seek_x(frac):.0f} {SEEK_Y}")
                time.sleep(2.5)
        scenario("seek_click_slider", seek_clicks)

        def seek_drag():
            app.cmd(f"drag {seek_x(0.2):.0f} {SEEK_Y} {seek_x(0.7):.0f} {SEEK_Y} 1500")
            time.sleep(3)
        scenario("seek_drag_slider", seek_drag)

        def seek_spectrogram():
            # Clicks right and left of the centered playhead.
            for x in (2900, 1300, 3300, 1000):
                app.cmd(f"click {x} 1100")
                time.sleep(2.5)
        scenario("seek_click_spectrogram", seek_spectrogram)

        def mode_switches():
            for js in ("root.uiBridge.setSpectrogramViewMode(0)", "root.uiBridge.setSpectrogramViewMode(1)",
                       "root.uiBridge.setSpectrogramDisplayMode(0)", "root.uiBridge.setSpectrogramDisplayMode(1)",
                       "viewerController.setVisualizationMode(1)", "viewerController.setVisualizationMode(0)",
                       "root.uiBridge.setFftSize(8192)", "root.uiBridge.setFftSize(2048)"):
                app.eval(js)
                time.sleep(2)
        scenario("mode_switch", mode_switches)

        def viewer():
            for _ in range(2):
                app.eval("viewerController.openSpectrogramViewer()")
                time.sleep(2.5)
                app.eval("viewerController.closeSpectrogramViewer()")
                time.sleep(2.5)
        scenario("fullscreen_viewer_toggle", viewer)

        def resize():
            app.cmd("restore")
            time.sleep(1.5)
            for i in range(30):
                w = 3440 - i * 50
                app.cmd(f"resize {w} {1360 - i * 12}")
                time.sleep(0.033)
            for i in range(30, -1, -1):
                app.cmd(f"resize {3440 - i * 50} {1360 - i * 12}")
                time.sleep(0.033)
            time.sleep(1)
            app.cmd("maximize")
            time.sleep(2)
        scenario("resize", resize)

        def library_scroll():
            for _ in range(60):
                app.cmd("wheel 440 1250 -120")
                time.sleep(0.03)
            time.sleep(1)
            for _ in range(60):
                app.cmd("wheel 440 1250 120")
                time.sleep(0.03)
        scenario("library_scroll", library_scroll)

        def search_typing():
            app.eval("globalSearchController.openDialog()")
            time.sleep(1)
            app.cmd("type love")
            time.sleep(2)
            for _ in range(4):
                app.cmd("key 16777219")  # Backspace
                time.sleep(0.15)
            app.cmd("type the")
            time.sleep(2)
            app.cmd(f"key {KEY_ESCAPE}")
        scenario("search_typing", search_typing)

        def big_queue():
            # Appends the whole library (~26k tracks) to the queue, scrolls it,
            # selects, then removes them again.
            app.eval("root.uiBridge.appendAllLibraryTracks()")
            time.sleep(4)
            for _ in range(80):
                app.cmd("wheel 2000 500 -360")
                time.sleep(0.03)
            time.sleep(1)
            app.cmd("click 2000 400")
            time.sleep(1)
            app.eval("root.uiBridge.replaceWithPaths(" + str(paths).replace("'", '"') + ")")
            time.sleep(3)
        scenario("big_queue", big_queue)
        app.eval("root.uiBridge.playAt(4)")
        time.sleep(5)

        def analysis_burst():
            # A 24/192 5.1 track start: overview and lookahead decode.
            app.eval("root.uiBridge.playAt(5)")
            time.sleep(10)
        scenario("analysis_burst", analysis_burst, 0)

        def rescan():
            app.eval("root.uiBridge.rescanAllLibraryRoots()")
            time.sleep(15)
        scenario("library_rescan", rescan, 0)
    finally:
        if perf is not None:
            perf.send_signal(2)
            perf.wait(timeout=60)
        app.quit()


if __name__ == "__main__":
    main()
