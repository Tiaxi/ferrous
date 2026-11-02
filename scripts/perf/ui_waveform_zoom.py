#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The waveform view whole-track and zoomed in (8x, 64x), 3 repetitions.

Usage: ui_waveform_zoom.py OUT_DIR STEREO_FILE
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_driver as d
p = d.default_paths()
out = sys.argv[1] if len(sys.argv) > 1 else "target/perf-ui/data/wfzoom"
for rep in (1, 2, 3):
    app = d.App(f"{out}/run-{rep}", p["app"], p["probe"], p["settings"], settle=10)
    try:
        app.mpris("OpenUri", "s", "file://" + os.path.abspath(sys.argv[2]))
        time.sleep(3)
        app.eval("viewerController.setVisualizationMode(1)")
        time.sleep(3)
        app.sample(10, "waveform_whole", f"{out}/states.jsonl")
        for z in (8, 64):
            n = len(app.probe_log()); app.cmd(f"setprop waveformEditorItem zoomLevel {z}"); print(app.wait_reply("setprop", after=n))
            time.sleep(4)
            app.sample(10, f"waveform_zoom{z}", f"{out}/states.jsonl")
            app.cmd("dirty on"); time.sleep(3); n = len(app.probe_log()); app.cmd(f"dirty off dirty-zoom{z}.txt"); app.wait_reply("dirty off", after=n)
    finally:
        app.quit()
