#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Median [min-max] tables over repetitions of ui_states.py.

Usage: ui_states_table.py OUT_DIR

Reads OUT_DIR/states.jsonl and OUT_DIR/run-*/probe/probe.log and prints
Markdown tables: CPU and wakeups per thread group, and frames, frame times
and uploads per window.
"""
import glob
import json
import os
import re
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_summarize  # noqa: E402

GROUPS = {
    "main": lambda n: n == "main",
    "render": lambda n: n == "QSGRenderThread",
    "mesa/amdgpu": lambda n: n.startswith("ferrous:"),
    "wayland": lambda n: n.startswith("WaylandEventThr"),
    "analysis": lambda n: n.startswith(("ferrous-spectro", "ferrous-wavefor", "ferrous-analysi", "filesrc", "ferrousfilesrc")),
}


def fmt(values, digits=1):
    values = [v for v in values if v is not None]
    if not values:
        return "–"
    med = statistics.median(values)
    if len(values) == 1 or max(values) - min(values) < 10 ** -digits:
        return f"{med:.{digits}f}"
    return f"{med:.{digits}f} [{min(values):.{digits}f}–{max(values):.{digits}f}]"


def main():
    out = sys.argv[1]
    rows = {}
    order = []
    with open(os.path.join(out, "states.jsonl")) as f:
        for line in f:
            r = json.loads(line)
            label = r["label"].split(" ", 1)[1]
            if label not in rows:
                order.append(label)
            rows.setdefault(label, []).append(r)
    frames = {}
    for log in sorted(glob.glob(os.path.join(out, "run-*", "probe", "probe.log"))):
        for res in ui_summarize.summarize(log):
            label = res["label"].split(" ", 1)[1]
            frames.setdefault(label, []).append(res)

    print("| State | process CPU % | wakeups/s | main % (wk/s) | render % | Mesa/amdgpu % | Wayland wk/s | analysis % |")
    print("|---|---|---|---|---|---|---|---|")
    for label in order:
        rs = rows[label]
        def grp(name, key):
            return [sum(v[key] for n, v in r["threads"].items() if GROUPS[name](n)) for r in rs]
        print(f"| {label} | {fmt([r['cpu_pct'] for r in rs])} | {fmt([r['wakeups_per_s'] for r in rs], 0)} | "
              f"{fmt(grp('main', 'cpu'))} ({fmt(grp('main', 'vcs'), 0)}) | {fmt(grp('render', 'cpu'))} | "
              f"{fmt(grp('mesa/amdgpu', 'cpu'))} | {fmt(grp('wayland', 'vcs'), 0)} | {fmt(grp('analysis', 'cpu'))} |")

    print()
    print("| State | window | fps | render-thread CPU ms/frame p50 (p99) | sync ms p50 | frame wall ms p50 (p99) | Mesa/amdgpu CPU ms/frame | tex MB/s | textures created/s | missed vsyncs/s |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for label in order:
        fr = frames.get(label, [])
        wins = sorted({w for res in fr for w in res["windows"]})
        if not wins:
            print(f"| {label} | – | 0 | | | | | | | |")
        for w in wins:
            ws = [res["windows"][w] for res in fr if w in res["windows"]]
            secs = [res["seconds"] for res in fr if w in res["windows"]]
            drv_per_frame = [s["driver_cpu_pct"] / 100 * 1000 / s["fps"] if s["fps"] else None for s in ws]
            print(f"| {label} | {w} | {fmt([s['fps'] for s in ws], 0)} | {fmt([s['render_cpu_ms_p50'] for s in ws], 3)} "
                  f"({fmt([s['render_cpu_ms_p99'] for s in ws], 2)}) | {fmt([s['sync_ms_p50'] for s in ws], 3)} | "
                  f"{fmt([s['frame_wall_ms_p50'] for s in ws], 2)} ({fmt([s['frame_wall_ms_p99'] for s in ws], 2)}) | "
                  f"{fmt(drv_per_frame, 3)} | {fmt([s['tex_MB_per_s'] for s in ws], 1)} | {fmt([s['tex_created_per_s'] for s in ws], 0)} | "
                  f"{fmt([s['missed_vsyncs'] / sec for s, sec in zip(ws, secs)], 1)} |")


if __name__ == "__main__":
    main()
