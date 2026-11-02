#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""GUI-thread stacks during the stalls a ui_stalls.py run logged.

Usage: ui_stall_stacks.py RUN_DIR [MIN_MS] [OUT_PREFIX]

Needs RUN_DIR/perf.data (PERF_RECORD=1, recorded with -k CLOCK_MONOTONIC,
as the probe's timestamps are) and RUN_DIR/probe/probe.log. For every stall
of at least MIN_MS (default 8), it takes the main thread's samples inside the
stall, and per scenario prints the stalls with their top frames and writes
OUT_PREFIX-<scenario>.folded for a flame graph. A stall with few samples
for its length was spent off the CPU (blocked or preempted).
"""
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_summarize  # noqa: E402

SKIP = re.compile(r"^(QCoreApplication|QGuiApplication|QApplication|QEventLoop|QEventDispatcher|g_main|_start|__libc|main$|QObject::event|QMetaObject|QMetaCallEvent|QtPrivate|QSocketNotifier|g_poll|poll|ppoll|QThread|doActivate|\[unknown\])")


def main():
    run = sys.argv[1]
    min_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
    prefix = sys.argv[3] if len(sys.argv) > 3 else os.path.join(run, "stall")
    marks, _, _, stalls, _ = ui_summarize.parse(os.path.join(run, "probe", "probe.log"))
    pid = None
    for _, text in marks:
        m = re.search(r"probe-start pid=(\d+)", text)
        if m:
            pid = m.group(1)
    intervals = ui_summarize.intervals(marks)
    script = subprocess.run(["perf", "script", "-i", os.path.join(run, "perf.data"), "--tid", pid,
                             "-F", "tid,time,ip,sym,dso"], capture_output=True, text=True).stdout
    samples = []  # (time_ns, [frames leaf-first])
    cur = None
    for line in script.splitlines():
        if not line.strip():
            if cur:
                samples.append(cur)
            cur = None
            continue
        if not line.startswith("\t") and not line.startswith(" "):
            m = re.match(r"\s*(\d+)\s+(\d+)\.(\d+):", line)
            if m:
                cur = (int(m.group(2)) * 10**9 + int(m.group(3).ljust(9, "0")[:9]), [])
            continue
        if cur is not None:
            parts = line.strip().split(None, 1)
            if len(parts) == 2:
                sym = re.sub(r"\+0x[0-9a-f]+", "", parts[1])
                sym = re.sub(r"\s*\(.*\)$", "", sym)
                cur[1].append(sym)
    if cur:
        samples.append(cur)
    samples.sort()
    times = [s[0] for s in samples]
    import bisect
    for label, t0, t1 in intervals:
        st = [s for s in stalls if t0 <= s[0] < t1 and s[1] >= min_ms * 1e6]
        if not st:
            continue
        print(f"== {label}: {len(st)} stalls >= {min_ms} ms")
        folded = collections.Counter()
        for post, lat in sorted(st, key=lambda s: -s[1]):
            i0 = bisect.bisect_left(times, post)
            i1 = bisect.bisect_left(times, post + lat)
            inside = samples[i0:i1]
            tops = collections.Counter()
            for _, frames in inside:
                stack = ";".join(reversed(frames))
                folded[stack] += 1
                interesting = [f for f in frames if not SKIP.match(f)]
                tops[" < ".join(interesting[:4]) if interesting else "(event loop)"] += 1
            expected = lat / 1e9 * 2999
            print(f"  {lat / 1e6:7.1f} ms at +{(post - t0) / 1e9:6.2f} s, {len(inside)} samples (on-CPU ~{min(100, 100 * len(inside) / max(expected, 1)):.0f}%)")
            for k, n in tops.most_common(3):
                print(f"      {n:4d}  {k[:230]}")
        with open(f"{prefix}-{label.replace(' ', '_')}.folded", "w") as f:
            for k, n in folded.items():
                f.write(f"{k} {n}\n")


if __name__ == "__main__":
    main()
