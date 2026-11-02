#!/usr/bin/env python3
"""Per-thread CPU and wakeups of one process over an interval.

Usage: threadsample.py PID SECONDS LABEL [OUT.jsonl]

Reads /proc/PID/task/*/{stat,status,comm} at the start and end (and every
second in between, to catch threads that end), and prints one JSON line:
the process CPU percent (of one core), and per thread name (digits folded
to '#') the CPU percent and the context switches per second, voluntary
(sleeps and wakeups) and involuntary.
"""
import json
import os
import re
import sys
import time

TICK = os.sysconf("SC_CLK_TCK")


def read_task(pid, tid):
    base = f"/proc/{pid}/task/{tid}"
    try:
        with open(f"{base}/stat") as f:
            stat = f.read()
        with open(f"{base}/comm") as f:
            comm = f.read().strip()
        with open(f"{base}/status") as f:
            status = f.read()
    except OSError:
        return None
    rest = stat[stat.rfind(")") + 2:].split()
    ticks = int(rest[11]) + int(rest[12])
    vol = int(re.search(r"voluntary_ctxt_switches:\s+(\d+)", status).group(1))
    invol = int(re.search(r"nonvoluntary_ctxt_switches:\s+(\d+)", status).group(1))
    return comm, ticks, vol, invol


def snapshot(pid):
    out = {}
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return out
    for tid in tids:
        t = read_task(pid, tid)
        if t:
            out[tid] = t
    return out


def proc_ticks(pid):
    with open(f"/proc/{pid}/stat") as f:
        stat = f.read()
    rest = stat[stat.rfind(")") + 2:].split()
    return int(rest[11]) + int(rest[12])


def main():
    pid, seconds, label = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
    out_path = sys.argv[4] if len(sys.argv) > 4 else None
    start = snapshot(pid)
    first = dict(start)
    last = dict(start)
    p0 = proc_ticks(pid)
    t0 = time.monotonic()
    while time.monotonic() - t0 < seconds:
        time.sleep(min(1.0, max(0.0, seconds - (time.monotonic() - t0))))
        now = snapshot(pid)
        for tid, value in now.items():
            first.setdefault(tid, (value[0], 0, 0, 0))  # Thread born during the interval.
            last[tid] = value
    elapsed = time.monotonic() - t0
    p1 = proc_ticks(pid)
    by_name = {}
    for tid, (comm, ticks, vol, invol) in last.items():
        _, ticks0, vol0, invol0 = first[tid]
        name = re.sub(r"\d", "#", comm)
        entry = by_name.setdefault(name, {"n": 0, "cpu": 0.0, "vcs": 0.0, "ivcs": 0.0})
        entry["n"] += 1
        entry["cpu"] += (ticks - ticks0) / TICK / elapsed * 100
        entry["vcs"] += (vol - vol0) / elapsed
        entry["ivcs"] += (invol - invol0) / elapsed
    for entry in by_name.values():
        for key in ("cpu", "vcs", "ivcs"):
            entry[key] = round(entry[key], 2)
    result = {
        "label": label,
        "seconds": round(elapsed, 1),
        "load": os.getloadavg()[0],
        "cpu_pct": round((p1 - p0) / TICK / elapsed * 100, 2),
        "wakeups_per_s": round(sum(e["vcs"] for e in by_name.values()), 1),
        "threads": dict(sorted(by_name.items(), key=lambda kv: -kv[1]["cpu"])),
    }
    line = json.dumps(result)
    print(line)
    if out_path:
        with open(out_path, "a") as f:
            f.write(line + "\n")


if __name__ == "__main__":
    main()
