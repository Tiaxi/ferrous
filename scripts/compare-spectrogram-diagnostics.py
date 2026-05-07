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


class MetricSeries(list[float]):
    @property
    def max_value(self) -> float:
        return max(self, default=0.0)


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
        "lod_upload_ms": MetricSeries(),
        "lod_blend": [],
        "frame_gap_ms": [],
        "event_stall_ms": [],
        "bridge_poll_ms": [],
        "bridge_precomputed_ms": [],
        "bridge_precomputed_pop_ms": [],
        "bridge_precomputed_dispatch_ms": [],
        "bridge_precomputed_max_frame_ms": [],
        "precomputed_frame_ms": [],
        "precomputed_frame_pop_ms": [],
        "precomputed_frame_dispatch_ms": [],
        "precomputed_feed_total_ms": [],
        "precomputed_feed_lock_wait_ms": [],
        "precomputed_feed_freeze_ms": [],
        "precomputed_feed_reset_ms": [],
        "precomputed_feed_alloc_ms": [],
        "precomputed_feed_write_ms": [],
        "precomputed_feed_ready_ms": [],
        "precomputed_feed_emit_update_ms": [],
        "precomputed_feed_reallocs": 0,
        "precomputed_feed_pending_before": 0,
        "precomputed_feed_applied_reset": 0,
        "precomputed_feed_implicit_reset": 0,
        "paths": Counter(),
        "fallbacks": Counter(),
        "lod_primary_levels": Counter(),
        "lod_secondary_levels": Counter(),
        "lod_bridge_levels": Counter(),
        "lod_bridge_spans": Counter(),
        "lod_bridge_planes": Counter(),
        "cpu_rebuilds": 0,
        "retained_resets": 0,
        "retained_upload_cols": 0,
        "lod_missing_frames": 0,
    }

    for line in path.read_text(errors="replace").splitlines():
        fields = parse_key_values(line)

        if "[ui-spectrogram] paint_spike" in line:
            append_float(metrics["paint_ms"], fields, "ms")
            append_float(metrics["cache_ms"], fields, "cache_ms")
            append_float(metrics["display_ms"], fields, "display_ms")
            append_float(metrics["retained_upload_ms"], fields, "retained_upload_ms")
            append_float(metrics["texture_upload_ms"], fields, "tex_ms")
            append_float(metrics["lod_upload_ms"], fields, "lod_tex_upload_ms")
            append_float(metrics["lod_blend"], fields, "lod_blend")
            count_name(metrics["paths"], fields.get("path"))
            count_name(metrics["fallbacks"], fields.get("fallback"))
            count_int(metrics["lod_primary_levels"], fields, "lod_primary")
            count_int(metrics["lod_secondary_levels"], fields, "lod_secondary")
            if fields.get("lod_missing_reason", "none") != "none":
                metrics["lod_missing_frames"] = int(metrics["lod_missing_frames"]) + 1
            metrics["cpu_rebuilds"] = int(metrics["cpu_rebuilds"]) + int_value(fields, "cpu_rebuild")
            metrics["retained_resets"] = int(metrics["retained_resets"]) + int_value(fields, "retained_reset")
            metrics["retained_upload_cols"] = (
                int(metrics["retained_upload_cols"]) + int_value(fields, "retained_cols")
            )
        elif "[ui-spectrogram] paints/s=" in line:
            append_float(metrics["paint_ms"], fields, "avg_ms")
            append_float(metrics["display_ms"], fields, "last_display_ms")
            append_float(metrics["retained_upload_ms"], fields, "last_retained_upload_ms")
            append_float(metrics["lod_upload_ms"], fields, "last_lod_tex_upload_ms")
            append_float(metrics["lod_blend"], fields, "last_lod_blend")
            count_name(metrics["paths"], fields.get("last_path"))
            count_name(metrics["fallbacks"], fields.get("last_fallback"))
            count_int(metrics["lod_primary_levels"], fields, "last_lod_primary")
            count_int(metrics["lod_secondary_levels"], fields, "last_lod_secondary")
            if fields.get("last_lod_missing_reason", "none") != "none":
                metrics["lod_missing_frames"] = int(metrics["lod_missing_frames"]) + 1
            metrics["retained_upload_cols"] = (
                int(metrics["retained_upload_cols"]) + int_value(fields, "last_retained_cols")
            )
        elif "[ui-spectrogram] smoothness_window" in line:
            append_float(metrics["frame_gap_ms"], fields, "max_gap_ms")
        elif "[ui-spectrogram] frame_gap" in line:
            append_float(metrics["frame_gap_ms"], fields, "ms")
        elif "[ui-prof] event_loop_stall" in line:
            append_float(metrics["event_stall_ms"], fields, "ms")
        elif "[ui-prof] bridge_poll" in line:
            append_float(metrics["bridge_poll_ms"], fields, "ms")
            append_float(metrics["bridge_precomputed_ms"], fields, "precomputed_ms")
            append_float(metrics["bridge_precomputed_pop_ms"], fields, "precomputed_pop_ms")
            append_float(
                metrics["bridge_precomputed_dispatch_ms"],
                fields,
                "precomputed_dispatch_ms",
            )
            append_float(
                metrics["bridge_precomputed_max_frame_ms"],
                fields,
                "precomputed_max_frame_ms",
            )
        elif "[ui-prof] precomputed_frame" in line:
            append_float(metrics["precomputed_frame_ms"], fields, "ms")
            append_float(metrics["precomputed_frame_pop_ms"], fields, "pop_ms")
            append_float(metrics["precomputed_frame_dispatch_ms"], fields, "dispatch_ms")
            count_int(metrics["lod_bridge_levels"], fields, "lod_level")
            count_int(metrics["lod_bridge_spans"], fields, "lod_span")
            count_int(metrics["lod_bridge_planes"], fields, "lod_planes")
        elif "[ui-spectrogram] precomputed_feed_phase" in line:
            append_float(metrics["precomputed_feed_total_ms"], fields, "total_ms")
            append_float(metrics["precomputed_feed_lock_wait_ms"], fields, "lock_wait_ms")
            append_float(metrics["precomputed_feed_freeze_ms"], fields, "freeze_ms")
            append_float(metrics["precomputed_feed_reset_ms"], fields, "reset_ms")
            append_float(metrics["precomputed_feed_alloc_ms"], fields, "alloc_ms")
            append_float(metrics["precomputed_feed_write_ms"], fields, "write_ms")
            append_float(metrics["precomputed_feed_ready_ms"], fields, "ready_ms")
            append_float(metrics["precomputed_feed_emit_update_ms"], fields, "emit_update_ms")
            metrics["precomputed_feed_reallocs"] = int(metrics["precomputed_feed_reallocs"]) + int_value(
                fields, "realloc"
            )
            metrics["precomputed_feed_pending_before"] = int(
                metrics["precomputed_feed_pending_before"]
            ) + int_value(fields, "pending_before")
            metrics["precomputed_feed_applied_reset"] = int(
                metrics["precomputed_feed_applied_reset"]
            ) + int_value(fields, "applied_reset")
            metrics["precomputed_feed_implicit_reset"] = int(
                metrics["precomputed_feed_implicit_reset"]
            ) + int_value(fields, "implicit_reset")

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


def count_int(counter: object, fields: dict[str, str], key: str) -> None:
    if isinstance(counter, Counter) and key in fields:
        counter[int_value(fields, key)] += 1


def summarize(label: str, metrics: dict[str, object]) -> list[str]:
    paint = list_value(metrics, "paint_ms")
    cache = list_value(metrics, "cache_ms")
    display = list_value(metrics, "display_ms")
    upload = list_value(metrics, "retained_upload_ms")
    texture = list_value(metrics, "texture_upload_ms")
    lod_upload = list_value(metrics, "lod_upload_ms")
    lod_blend = list_value(metrics, "lod_blend")
    gaps = list_value(metrics, "frame_gap_ms")
    stalls = list_value(metrics, "event_stall_ms")
    bridge = list_value(metrics, "bridge_poll_ms")
    bridge_precomputed = list_value(metrics, "bridge_precomputed_ms")
    bridge_precomputed_pop = list_value(metrics, "bridge_precomputed_pop_ms")
    bridge_precomputed_dispatch = list_value(metrics, "bridge_precomputed_dispatch_ms")
    bridge_precomputed_frame = list_value(metrics, "bridge_precomputed_max_frame_ms")
    precomputed_frame = list_value(metrics, "precomputed_frame_ms")
    precomputed_frame_pop = list_value(metrics, "precomputed_frame_pop_ms")
    precomputed_frame_dispatch = list_value(metrics, "precomputed_frame_dispatch_ms")
    precomputed_feed = list_value(metrics, "precomputed_feed_total_ms")
    precomputed_feed_lock = list_value(metrics, "precomputed_feed_lock_wait_ms")
    precomputed_feed_freeze = list_value(metrics, "precomputed_feed_freeze_ms")
    precomputed_feed_reset = list_value(metrics, "precomputed_feed_reset_ms")
    precomputed_feed_alloc = list_value(metrics, "precomputed_feed_alloc_ms")
    precomputed_feed_write = list_value(metrics, "precomputed_feed_write_ms")
    precomputed_feed_ready = list_value(metrics, "precomputed_feed_ready_ms")
    precomputed_feed_emit = list_value(metrics, "precomputed_feed_emit_update_ms")
    paths = counter_value(metrics, "paths")
    fallbacks = counter_value(metrics, "fallbacks")
    lod_primary = counter_value(metrics, "lod_primary_levels")
    lod_secondary = counter_value(metrics, "lod_secondary_levels")
    lod_bridge_levels = counter_value(metrics, "lod_bridge_levels")
    lod_bridge_spans = counter_value(metrics, "lod_bridge_spans")
    lod_bridge_planes = counter_value(metrics, "lod_bridge_planes")

    lines = [
        f"{label}:",
        f"  paint samples={len(paint)} avg={avg(paint):.3f}ms p90={percentile(paint, 0.90):.3f}ms "
        f"p99={percentile(paint, 0.99):.3f}ms max={max(paint, default=0.0):.3f}ms",
        f"  cache avg={avg(cache):.3f}ms display avg={avg(display):.3f}ms "
        f"retained-upload avg={avg(upload):.3f}ms texture-upload avg={avg(texture):.3f}ms",
        f"  frame gaps count={len(gaps)} p90={percentile(gaps, 0.90):.1f}ms max={max(gaps, default=0.0):.1f}ms "
        f"event stalls count={len(stalls)} max={max(stalls, default=0.0):.1f}ms",
        f"  bridge polls count={len(bridge)} p90={percentile(bridge, 0.90):.1f}ms "
        f"max={max(bridge, default=0.0):.1f}ms precomputed avg={avg(bridge_precomputed):.3f}ms "
        f"max={max(bridge_precomputed, default=0.0):.1f}ms",
        f"  precomputed split pop avg={avg(bridge_precomputed_pop):.3f}ms "
        f"dispatch avg={avg(bridge_precomputed_dispatch):.3f}ms "
        f"max-frame p90={percentile(bridge_precomputed_frame, 0.90):.1f}ms "
        f"slow-frame count={len(precomputed_frame)} pop max={max(precomputed_frame_pop, default=0.0):.1f}ms "
        f"dispatch max={max(precomputed_frame_dispatch, default=0.0):.1f}ms",
        f"  cpu rebuilds={metrics['cpu_rebuilds']} retained resets={metrics['retained_resets']} "
        f"retained uploaded cols={metrics['retained_upload_cols']}",
        f"  lod primary={format_counter(lod_primary)} secondary={format_counter(lod_secondary)} "
        f"blended={sum(1 for value in lod_blend if value > 0.001)} missing={metrics['lod_missing_frames']} "
        f"upload avg={avg(lod_upload):.3f}ms p90={percentile(lod_upload, 0.90):.3f}ms "
        f"max={max(lod_upload, default=0.0):.3f}ms",
        f"  lod bridge levels={format_counter(lod_bridge_levels)} spans={format_counter(lod_bridge_spans)} "
        f"planes={format_counter(lod_bridge_planes)}",
        f"  paths={format_counter(paths)}",
        f"  fallbacks={format_counter(fallbacks)}",
    ]
    if precomputed_feed:
        lines.append(
            f"  feed phase samples={len(precomputed_feed)} total max={max(precomputed_feed, default=0.0):.1f}ms "
            f"lock max={max(precomputed_feed_lock, default=0.0):.1f}ms "
            f"freeze max={max(precomputed_feed_freeze, default=0.0):.1f}ms "
            f"reset max={max(precomputed_feed_reset, default=0.0):.1f}ms "
            f"alloc max={max(precomputed_feed_alloc, default=0.0):.1f}ms "
            f"write max={max(precomputed_feed_write, default=0.0):.1f}ms "
            f"ready max={max(precomputed_feed_ready, default=0.0):.1f}ms "
            f"emit max={max(precomputed_feed_emit, default=0.0):.1f}ms "
            f"reallocs={metrics['precomputed_feed_reallocs']} "
            f"pending={metrics['precomputed_feed_pending_before']} "
            f"applied_reset={metrics['precomputed_feed_applied_reset']} "
            f"implicit_reset={metrics['precomputed_feed_implicit_reset']}"
        )
    return lines


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
