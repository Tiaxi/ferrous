#!/usr/bin/env python3
"""Median [min-max] of seek timings per file and configuration, over all
seeks of all repeats, as Markdown. Seeks without a reset (the target was
decoded already) count as 0 ms to the target columns.
Usage: summarize_seek.py SEEK.jsonl SELECTION.txt"""
import json
import statistics
import sys
from collections import defaultdict

desc = {}
for line in open(sys.argv[2]):
    parts = line.rstrip("\n").split("\t")
    desc[parts[0]] = parts[1]
groups = defaultdict(list)
for line in open(sys.argv[1]):
    run = json.loads(line)
    groups[(run["config"], run["id"])].append(run)


def stat(values):
    values = [v for v in values if v is not None]
    if not values:
        return "–"
    q = statistics.quantiles(values, n=10) if len(values) >= 4 else None
    p90 = f", p90 {q[-1]:.0f}" if q else ""
    return f"{statistics.median(values):.0f} [{min(values):.0f}–{max(values):.0f}{p90}]"


print("| Config | Sample | runs | seeks | resets | start ms | landed ms | advancing ms | first column ms | columns at playhead ms | busy at 1st seek |")
print("|---|---|---|---|---|---|---|---|---|---|---|")
for config in ["idle_centered", "busy_centered", "idle_rolling", "busy_rolling"]:
    for sid in sorted(desc):
        runs = groups.get((config, sid))
        if not runs:
            continue
        seeks = [s for run in runs for s in run["r"]["seeks"]]
        target = [s["target_chunk_ms"] if s["reset"] else 0.0 for s in seeks]
        print(
            f"| {config} | {sid} {desc[sid]} | {len(runs)} | {len(seeks)} | {sum(s['reset'] for s in seeks)} | "
            f"{stat([r['r']['start_ms'] for r in runs])} | {stat([s['landed_ms'] for s in seeks])} | "
            f"{stat([s['advancing_ms'] for s in seeks])} | {stat([s['first_chunk_ms'] for s in seeks if s['reset']])} | "
            f"{stat(target)} | {sum(r['r']['analysis_busy_at_first_seek'] for r in runs)}/{len(runs)} |"
        )
