#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frames, frame times, uploads, CPU and wakeups per visualization state.

Usage: ui_states.py OUT_DIR REP SAMPLE_DIR [IDS...]

Starts the app in an isolated tree with the probe, plays each sample (named
<ID>.<ext> in SAMPLE_DIR) and samples each state for STATE_S seconds
(default 15). Writes OUT_DIR/states.jsonl (thread CPU and wakeups) and keeps
the probe log under OUT_DIR/run-REP/ for ui_summarize.py.
"""
import glob
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_driver as d  # noqa: E402

STATE_S = float(os.environ.get("STATE_S", "15"))
SETTLE_S = float(os.environ.get("SETTLE_S", "3"))


def main():
    out, rep, sample_dir = sys.argv[1], sys.argv[2], sys.argv[3]
    ids = sys.argv[4:] or ["S01", "S05"]
    os.makedirs(out, exist_ok=True)
    jsonl = os.path.join(out, "states.jsonl")
    p = d.default_paths()
    d.wait_for_quiet()
    app = d.App(os.path.join(out, f"run-{rep}"), p["app"], p["probe"], p["settings"], settle=10)

    def winstate(label):
        n = len(app.probe_log())
        app.cmd("winstate")
        try:
            text = app.wait_reply("winstate", after=n)
        except TimeoutError:
            text = "winstate timeout"
        # Titles carry track names; keep only the state fields.
        import re
        app.mark(f"state {label} " + " ".join(re.findall(r"(?:visibility|exposed|active|state)=\d+", text)))

    def state(label, settle=SETTLE_S):
        app.cmd(f"dump items-{label.replace(' ', '_')}.txt")
        time.sleep(max(settle, 0.5))
        r = app.sample(STATE_S, f"r{rep} {label}", jsonl)
        print(f"{label}: cpu {r['cpu_pct']}% wakeups {r['wakeups_per_s']}/s load {r['load_start']:.1f}", flush=True)
        # Which items made the frames: a short census after the sample, as
        # it costs the render thread a little.
        app.cmd("dirty on")
        time.sleep(3)
        n = len(app.probe_log())
        app.cmd(f"dirty off dirty-{label.replace(' ', '_')}.txt")
        try:
            app.wait_reply("dirty off", after=n)
        except TimeoutError:
            pass

    def ensure_shown(label):
        # Wayland gives a client no way to un-minimize itself but
        # activation; showNormal + requestActivate does it on KWin.
        app.cmd("restore")
        time.sleep(2)
        app.cmd("maximize")
        time.sleep(2)
        n = len(app.probe_log())
        app.cmd("winstate")
        text = app.wait_reply("winstate", after=n)
        main = text.split("]")[0]
        if "exposed=1" not in main:
            raise RuntimeError(f"window not shown after {label}: " + main.split(" vis=")[1])

    try:
        state("idle_empty", 0)
        for sid in ids:
            path = glob.glob(os.path.join(sample_dir, sid + ".*"))[0]
            app.mpris("OpenUri", "s", "file://" + path)
            # The first half view of a centered track stands still while
            # the playhead walks to the middle (about 30 s at 44.1 kHz in a
            # 2545 px pane), then it scrolls.
            t0 = time.monotonic()
            app.cmd(f"dump items-{sid}_start.txt")
            time.sleep(3)
            r = app.sample(10, f"r{rep} {sid} centered_start", jsonl)
            print(f"{sid} centered_start: cpu {r['cpu_pct']}%", flush=True)
            time.sleep(max(0.0, 36 - (time.monotonic() - t0)))
            state(f"{sid} centered_perchannel", 0)
            app.eval("root.uiBridge.setSpectrogramViewMode(0)")
            state(f"{sid} centered_downmix")
            app.eval("root.uiBridge.setSpectrogramDisplayMode(0)")
            state(f"{sid} rolling_downmix")
            app.eval("root.uiBridge.setSpectrogramViewMode(1)")
            state(f"{sid} rolling_perchannel")
            app.eval("root.uiBridge.setSpectrogramDisplayMode(1)")
            app.eval("viewerController.setVisualizationMode(1)")
            state(f"{sid} waveform")
            app.eval("viewerController.setVisualizationMode(0)")
            app.eval("root.uiBridge.setShowLevelMeter(true)")
            state(f"{sid} centered_perchannel_meter")
            app.eval("root.uiBridge.setShowLevelMeter(false)")
            app.eval("viewerController.openSpectrogramViewer()")
            state(f"{sid} fullscreen_viewer")
            app.eval("viewerController.closeSpectrogramViewer()")
            app.mpris("Pause")
            state(f"{sid} paused", 4)
            app.mpris("Play")
            app.cmd("cover")
            time.sleep(1)
            winstate(f"{sid} covered")
            state(f"{sid} covered")
            app.cmd("cover off")
            time.sleep(2)
            app.cmd("minimize")
            time.sleep(2)
            winstate(f"{sid} minimized")
            state(f"{sid} minimized", 1)
            ensure_shown(f"{sid} minimized")
            winstate(f"{sid} restored")
            state(f"{sid} centered_perchannel_again")
        app.mpris("Stop")
        state("stopped", 4)
    finally:
        app.quit()


if __name__ == "__main__":
    main()
