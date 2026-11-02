#!/usr/bin/env python3
"""Median [min-max] tables of analysis-matrix runs, as Markdown.

Usage: summarize_analysis.py RUNS.jsonl SELECTION.txt
SELECTION.txt maps sample IDs to descriptions (tab-separated: id, desc, dur).
"""
import json
import statistics
import sys
from collections import defaultdict

runs = [json.loads(line) for line in open(sys.argv[1])]
desc = {}
for line in open(sys.argv[2]):
    parts = line.rstrip("\n").split("\t")
    desc[parts[0]] = (parts[1], float(parts[2]))

groups = defaultdict(list)
for run in runs:
    groups[(run["config"], run["id"])].append(run)


def stat(values, fmt="{:.0f}"):
    values = [v for v in values if v is not None]
    if not values:
        return "–"
    med = statistics.median(values)
    if len(values) == 1:
        return fmt.format(med)
    return f"{fmt.format(med)} [{fmt.format(min(values))}–{fmt.format(max(values))}]"


def thread_cpu(run, prefix):
    return sum(v["cpu_ms"] for k, v in run["r"]["threads"].items() if k.startswith(prefix))


configs = ["default", "full_pc", "full_dm", "full_pc8k"]
titles = {
    "default": "Default settings (centered, per channel, FFT 2048, 3440 px, 10 s lookahead): track start",
    "full_pc": "Full-track spectrogram, per channel, FFT 2048 (lookahead raised to the 64 MiB budget)",
    "full_dm": "Full-track spectrogram, downmix, FFT 2048",
    "full_pc8k": "Full-track spectrogram, per channel, FFT 8192",
}
for config in configs:
    print(f"\n#### {titles[config]}\n")
    print("| Sample | n | overview first ms | overview done ms | overview ×RT | spectro first ms | spectro last ms | spectro audio s | spectro ×RT | EOF | spectro thread CPU ms | overview CPU ms | process CPU ms |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for sid in sorted(desc):
        rows = groups.get((config, sid))
        if not rows:
            continue
        r = [row["r"] for row in rows]
        overview_cpu = [
            thread_cpu(row, "ferrous-wavefor") + thread_cpu(row, "ferrousfilesrc")
            for row in rows
        ]
        print(
            "| {} {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
                sid,
                desc[sid][0],
                len(rows),
                stat([x["waveform_first_ms"] for x in r]),
                stat([x["waveform_done_ms"] for x in r]),
                stat([x["waveform_rtf"] for x in r]),
                stat([x["spectral_first_ms"] for x in r], "{:.1f}"),
                stat([x["spectral_last_ms"] for x in r]),
                stat([x["spectral_audio_s"] for x in r]),
                stat([x["spectral_rtf"] for x in r]),
                "yes" if all(x["spectral_eof"] for x in r) else "no",
                stat([thread_cpu(row, "ferrous-spectro") + thread_cpu(row, "filesrc") for row in rows]),
                stat(overview_cpu),
                stat([x["threads"]["_process_total"]["cpu_ms"] for x in r]),
            )
        )
