#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Startup with a large library: time to the first frame and GUI stalls.

Usage: ui_startup.py OUT_DIR REP LIBRARY_DB [WATCH]

Starts the app with a copy of LIBRARY_DB and the stall watchdog on from the
start (FERROUS_PROBE_STALL), with library watching on (WATCH=1, default; the
app rescans its roots at startup) or off (0). Marks "begin startup" when the
probe comes up and "end startup" STARTUP_S (default 30) s later, and prints
the time from launch to the probe and to the first frame.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_driver as d  # noqa: E402
import ui_summarize  # noqa: E402


def main():
    out, rep, db = sys.argv[1], sys.argv[2], sys.argv[3]
    watch = sys.argv[4] if len(sys.argv) > 4 else "1"
    os.makedirs(out, exist_ok=True)
    p = d.default_paths()
    settings = os.path.join(out, f"settings-{rep}.txt")
    with open(p["settings"]) as f:
        text = f.read().replace("library_watch_enabled=1", f"library_watch_enabled={watch}")
    with open(settings, "w") as f:
        f.write(text)
    d.wait_for_quiet()
    launch_ns = time.monotonic_ns()
    app = d.App(os.path.join(out, f"run-{rep}"), p["app"], p["probe"], settings, library_db=db, settle=0,
                env={"FERROUS_PROBE_STALL": os.environ.get("STALL_MS", "3")})
    try:
        app.mark("begin startup")
        r = app.sample(float(os.environ.get("STARTUP_S", "30")), "startup_sample",
                       os.path.join(out, "startup.jsonl"))
        app.mark("end startup")
        app.cmd("stall off")
        time.sleep(1)
    finally:
        app.quit()
    marks, frames, _, _, _ = ui_summarize.parse(os.path.join(app.probe_dir, "probe.log"))
    probe_start = next(t for t, text in marks if text.startswith("probe-start"))
    first_frame = min((f[1] for f in frames), default=None)
    result = {"rep": rep, "watch": watch, "launch_to_probe_ms": (probe_start - launch_ns) / 1e6,
              "launch_to_first_frame_ms": (first_frame - launch_ns) / 1e6 if first_frame else None,
              "cpu_pct_first_30s": r["cpu_pct"]}
    print(json.dumps(result))
    with open(os.path.join(out, "startup-times.jsonl"), "a") as f:
        f.write(json.dumps(result) + "\n")


if __name__ == "__main__":
    main()
