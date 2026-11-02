#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Median [min-max] stall table over ui_stalls.py repetitions.

Usage: ui_stalls_table.py RUN_DIR...  (each with probe/probe.log)
"""
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_summarize  # noqa: E402


def fmt(v, d=1):
    m = statistics.median(v)
    return f"{m:.{d}f}" if max(v) - min(v) < 10 ** -d else f"{m:.{d}f} [{min(v):.{d}f}–{max(v):.{d}f}]"


def main():
    by = {}
    order = []
    for run in sys.argv[1:]:
        for r in ui_summarize.summarize(os.path.join(run, "probe", "probe.log")):
            if r["label"] not in by:
                order.append(r["label"])
            by.setdefault(r["label"], []).append(r)
    print("| Interaction | s | GUI blocks ≥3 ms | >8 ms | >16 ms | >50 ms | longest ms | blocked ms total | ping p99.9 ms | render-thread CPU/frame max ms | frames with render CPU > 2.78 ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for lab in order:
        rs = by[lab]
        st = [r["stalls"] for r in rs]
        w0 = [r["windows"].get(0, {}) for r in rs]
        print(f"| {lab} | {rs[0]['seconds']:.0f} | {fmt([s['count_ge_threshold'] for s in st], 0)} | {fmt([s['over_8ms'] for s in st], 0)} | "
              f"{fmt([s['over_16ms'] for s in st], 0)} | {fmt([s['over_50ms'] for s in st], 0)} | {fmt([s['max_ms'] for s in st])} | "
              f"{fmt([s['total_ms'] for s in st], 0)} | {fmt([s.get('latency_ms_p999', 0) for s in st], 2)} | "
              f"{fmt([w.get('render_cpu_ms_max', 0) for w in w0], 2)} | {fmt([w.get('render_over_budget', 0) for w in w0], 0)} |")


if __name__ == "__main__":
    main()
