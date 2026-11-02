#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Flame graphs of the app in a steady visualization state.

Usage: ui_flame.py OUT_DIR SAMPLE_FILE NAME [JS...]

Plays SAMPLE_FILE, runs each JS expression in the main window's QML context
(e.g. "root.uiBridge.setSpectrogramDisplayMode(0)"), waits WARM_S (default
35, so that a centered view scrolls), then records RECORD_S (default 15) s
with `perf record -e cpu-clock:u --call-graph fp`. Writes NAME.folded (stacks
rooted at the thread name) and NAME.svg in OUT_DIR, and NAME.probe.log.
"""
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_driver as d  # noqa: E402


def main():
    out, sample, name = sys.argv[1], sys.argv[2], sys.argv[3]
    scripts = sys.argv[4:]
    os.makedirs(out, exist_ok=True)
    p = d.default_paths()
    d.wait_for_quiet()
    run = os.path.join(out, "run-" + name)
    app = d.App(run, p["app"], p["probe"], p["settings"], settle=10)
    try:
        app.mpris("OpenUri", "s", "file://" + sample)
        time.sleep(2)
        for js in scripts:
            if js.startswith("probe:"):
                app.cmd(js[6:])
            else:
                app.eval(js)
        time.sleep(float(os.environ.get("WARM_S", "35")))
        data = os.path.join(run, "perf.data")
        rec = subprocess.Popen(["perf", "record", "-e", "cpu-clock:u", "-F", "1999", "--call-graph", "fp",
                                "--user-callchains", "-k", "CLOCK_MONOTONIC", "-p", str(app.pid), "-o", data],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        r = app.sample(float(os.environ.get("RECORD_S", "15")), name, os.path.join(out, "flame-states.jsonl"))
        rec.send_signal(2)
        rec.wait(timeout=60)
        print(name, r["cpu_pct"], r["wakeups_per_s"])
    finally:
        app.quit()
    script = subprocess.run(["perf", "script", "-i", data], capture_output=True, text=True).stdout
    folded = subprocess.run(["inferno-collapse-perf", "--all"], input=script, capture_output=True, text=True).stdout
    with open(os.path.join(out, name + ".folded"), "w") as f:
        f.write(folded)
    svg = subprocess.run(["inferno-flamegraph", "--title", name, "--width", "1800"], input=folded,
                         capture_output=True, text=True).stdout
    with open(os.path.join(out, name + ".svg"), "w") as f:
        f.write(svg)
    shutil.copy(os.path.join(run, "probe", "probe.log"), os.path.join(out, name + ".probe.log"))


if __name__ == "__main__":
    main()
