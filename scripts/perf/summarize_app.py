#!/usr/bin/env python3
"""Median [min-max] of the app's CPU and wakeups per state, and of the
busiest threads, as Markdown. Usage: summarize_app.py APP_STATES.jsonl"""
import json
import statistics
import sys
from collections import defaultdict

runs = defaultdict(list)
order = []
for line in open(sys.argv[1]):
    d = json.loads(line)
    state = d["label"].split(" ", 1)[1]
    if state not in order:
        order.append(state)
    runs[state].append(d)


def stat(values, fmt="{:.1f}"):
    if not values:
        return "–"
    med = statistics.median(values)
    if len(values) == 1:
        return fmt.format(med)
    return f"{fmt.format(med)} [{fmt.format(min(values))}–{fmt.format(max(values))}]"


groups = {
    "UI (main, render, GL, Wayland)": ("ferrous", "QSGRenderThread", "ferrous:gl#", "ferrous:gdrv#", "ferrous:cs#", "WaylandEventThr", "ferrous:sh#", "QQmlThread", "Thread (pooled)", "QXcbEventQueue"),
    "analysis": ("ferrous-spectro", "ferrous-wavefor", "ferrous-analysi", "ferrousfilesrc#", "filesrc#:src", "ferrous-spectra", "ferrous-ffi-rel"),
    "playback (engine, GStreamer)": ("ferrous-playbac", "multiqueue#:src", "aqueue:src", "flacparse#:sink", "typefind:sink", "ferrous-read", "pw-", "pulsesink", "audiosink", "threaded-ml", "data-loop.#"),
    "bridge": ("ferrous-bridge", "ferrous-bridge-"),
}
print("| State | n | process CPU % of a core | wakeups/s | UI threads % | analysis % | playback % | bridge % |")
print("|---|---|---|---|---|---|---|---|")
for state in order:
    rs = runs[state]
    cols = []
    for names in groups.values():
        cols.append(stat([sum(v["cpu"] for k, v in r["threads"].items() if k in names) for r in rs]))
    print(f"| {state} | {len(rs)} | {stat([r['cpu_pct'] for r in rs])} | {stat([r['wakeups_per_s'] for r in rs], '{:.0f}')} | " + " | ".join(cols) + " |")

print("\nBusiest threads per state (median CPU %, voluntary context switches/s):\n")
for state in order:
    rs = runs[state]
    names = defaultdict(list)
    for r in rs:
        for k, v in r["threads"].items():
            names[k].append(v)
    top = sorted(names.items(), key=lambda kv: -statistics.median(x["cpu"] for x in kv[1]))[:9]
    busy = ", ".join(
        f"{k} {statistics.median(x['cpu'] for x in v):.2f}%/{statistics.median(x['vcs'] for x in v):.0f}"
        for k, v in top
    )
    wake = sorted(names.items(), key=lambda kv: -statistics.median(x["vcs"] for x in kv[1]))[:5]
    wakes = ", ".join(f"{k} {statistics.median(x['vcs'] for x in v):.0f}/s" for k, v in wake)
    print(f"- **{state}**: {busy}. Most wakeups: {wakes}.")
