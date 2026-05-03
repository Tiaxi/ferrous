#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Compare Ferrous spectrogram diagnostics logs.

The parser accepts the profile lines emitted by SpectrogramItem and BridgeClient
when FERROUS_PROFILE_UI / FERROUS_PROFILE are enabled. It intentionally keeps
the output compact so A/B runs from main and the GPU branch can be compared in
one terminal pane.
"""

from __future__ import annotations

import argparse
import math
import re
from collections import Counter
from pathlib import Path


KEY_VALUE_RE = re.compile(r"([A-Za-z_./-]+)=([^ \n]+)")


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = (len(ordered) - 1) * pct
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return ordered[lower]
    fraction = index - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def parse_key_values(line: str) -> dict[str, str]:
    return {match.group(1): match.group(2).strip('"') for match in KEY_VALUE_RE.finditer(line)}


def parse_log(path: Path) -> dict[str, object]:
    metrics: dict[str, object] = {
        "paint_ms": [],
        "cache_ms": [],
        "display_ms": [],
        "retained_upload_ms": [],
        "texture_upload_ms": [],
        "frame_gap_ms": [],
        "event_stall_ms": [],
        "paths": Counter(),
        "fallbacks": Counter(),
        "cpu_rebuilds": 0,
        "retained_resets": 0,
        "retained_upload_cols": 0,
    }

    for line in path.read_text(errors="replace").splitlines():
        fields = parse_key_values(line)

        if "[ui-spectrogram] paint_spike" in line:
            append_float(metrics["paint_ms"], fields, "ms")
            append_float(metrics["cache_ms"], fields, "cache_ms")
            append_float(metrics["display_ms"], fields, "display_ms")
            append_float(metrics["retained_upload_ms"], fields, "retained_upload_ms")
            append_float(metrics["texture_upload_ms"], fields, "tex_ms")
            count_name(metrics["paths"], fields.get("path"))
            count_name(metrics["fallbacks"], fields.get("fallback"))
            metrics["cpu_rebuilds"] = int(metrics["cpu_rebuilds"]) + int_value(fields, "cpu_rebuild")
            metrics["retained_resets"] = int(metrics["retained_resets"]) + int_value(fields, "retained_reset")
            metrics["retained_upload_cols"] = (
                int(metrics["retained_upload_cols"]) + int_value(fields, "retained_cols")
            )
        elif "[ui-spectrogram] paints/s=" in line:
            append_float(metrics["paint_ms"], fields, "avg_ms")
            append_float(metrics["display_ms"], fields, "last_display_ms")
            append_float(metrics["retained_upload_ms"], fields, "last_retained_upload_ms")
            count_name(metrics["paths"], fields.get("last_path"))
            count_name(metrics["fallbacks"], fields.get("last_fallback"))
            metrics["retained_upload_cols"] = (
                int(metrics["retained_upload_cols"]) + int_value(fields, "last_retained_cols")
            )
        elif "[ui-spectrogram] smoothness_window" in line:
            append_float(metrics["frame_gap_ms"], fields, "max_gap_ms")
        elif "[ui-prof] event_loop_stall" in line:
            append_float(metrics["event_stall_ms"], fields, "ms")

    return metrics


def append_float(target: object, fields: dict[str, str], key: str) -> None:
    if not isinstance(target, list) or key not in fields:
        return
    try:
        target.append(float(fields[key].replace(",", ".")))
    except ValueError:
        return


def int_value(fields: dict[str, str], key: str) -> int:
    try:
        return int(float(fields.get(key, "0").replace(",", ".")))
    except ValueError:
        return 0


def count_name(counter: object, name: str | None) -> None:
    if isinstance(counter, Counter) and name and name != "none":
        counter[name] += 1


def summarize(label: str, metrics: dict[str, object]) -> list[str]:
    paint = list_value(metrics, "paint_ms")
    cache = list_value(metrics, "cache_ms")
    display = list_value(metrics, "display_ms")
    upload = list_value(metrics, "retained_upload_ms")
    texture = list_value(metrics, "texture_upload_ms")
    gaps = list_value(metrics, "frame_gap_ms")
    stalls = list_value(metrics, "event_stall_ms")
    paths = counter_value(metrics, "paths")
    fallbacks = counter_value(metrics, "fallbacks")

    return [
        f"{label}:",
        f"  paint samples={len(paint)} avg={avg(paint):.3f}ms p90={percentile(paint, 0.90):.3f}ms "
        f"p99={percentile(paint, 0.99):.3f}ms max={max(paint, default=0.0):.3f}ms",
        f"  cache avg={avg(cache):.3f}ms display avg={avg(display):.3f}ms "
        f"retained-upload avg={avg(upload):.3f}ms texture-upload avg={avg(texture):.3f}ms",
        f"  frame gaps count={len(gaps)} p90={percentile(gaps, 0.90):.1f}ms max={max(gaps, default=0.0):.1f}ms "
        f"event stalls count={len(stalls)} max={max(stalls, default=0.0):.1f}ms",
        f"  cpu rebuilds={metrics['cpu_rebuilds']} retained resets={metrics['retained_resets']} "
        f"retained uploaded cols={metrics['retained_upload_cols']}",
        f"  paths={format_counter(paths)}",
        f"  fallbacks={format_counter(fallbacks)}",
    ]


def list_value(metrics: dict[str, object], key: str) -> list[float]:
    value = metrics[key]
    return value if isinstance(value, list) else []


def counter_value(metrics: dict[str, object], key: str) -> Counter:
    value = metrics[key]
    return value if isinstance(value, Counter) else Counter()


def avg(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def format_counter(counter: Counter) -> str:
    if not counter:
        return "none"
    return ", ".join(f"{name}:{count}" for name, count in counter.most_common())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="baseline diagnostics log, typically main")
    parser.add_argument("candidate", type=Path, help="candidate diagnostics log, typically GPU branch")
    args = parser.parse_args()

    baseline = parse_log(args.baseline)
    candidate = parse_log(args.candidate)
    for line in summarize(args.baseline.name, baseline):
        print(line)
    print()
    for line in summarize(args.candidate.name, candidate):
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
