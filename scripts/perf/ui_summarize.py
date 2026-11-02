#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarizes a ui-probe log per marked interval ("begin X" ... "end X").

Usage: ui_summarize.py PROBE_LOG [--json]

For each interval and window: frames per second, frame interval and
render-thread CPU per frame percentiles, sync/render split, driver-thread
CPU per frame, texture and buffer upload bytes, textures created, and the
GUI thread's afterAnimating count; and the GUI-thread stalls in it.
"""
import json
import sys


def pct(values, p):
    if not values:
        return 0.0
    s = sorted(values)
    k = min(len(s) - 1, max(0, int(round(p / 100.0 * (len(s) - 1)))))
    return s[k]


def parse(path):
    marks, frames, anim, stalls, replies = [], [], [], [], []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            kind = parts[0]
            if kind == "M":
                marks.append((int(parts[1]), " ".join(parts[2:])))
            elif kind == "F" and len(parts) >= 15:
                v = [int(x) for x in parts[1:16]]
                if len(v) == 14:
                    v.append(-1)  # Logs from before draw counting.
                frames.append(v)
            elif kind == "A":
                anim.append((int(parts[1]), int(parts[2]), int(parts[3])))
            elif kind == "S":
                stalls.append((int(parts[1]), int(parts[2])))
            elif kind == "R":
                replies.append((int(parts[1]), " ".join(parts[2:])))
    return marks, frames, anim, stalls, replies


def intervals(marks):
    open_ = {}
    out = []
    for t, text in marks:
        if text.startswith("begin "):
            open_[text[6:]] = t
        elif text.startswith("end ") and text[4:] in open_:
            label = text[4:]
            out.append((label, open_.pop(label), t))
    return out


def summarize(path):
    marks, frames, anim, stalls, replies = parse(path)
    results = []
    for label, t0, t1 in intervals(marks):
        secs = (t1 - t0) / 1e9
        res = {"label": label, "seconds": round(secs, 2), "windows": {}}
        wins = sorted({f[0] for f in frames if t0 <= f[1] < t1} | {a[0] for a in anim if t0 <= a[1] < t1})
        for w in wins:
            fs = [f for f in frames if f[0] == w and t0 <= f[1] < t1]
            an = [a for a in anim if a[0] == w and t0 <= a[1] < t1]
            begins = [f[1] for f in fs]
            gaps = [(b - a) / 1e6 for a, b in zip(begins, begins[1:])]
            cpu = [f[7] / 1e6 for f in fs]
            drv = [f[8] / 1e6 for f in fs]
            sync = [(f[3] - f[2]) / 1e6 for f in fs if f[2] and f[3]]
            render = [(f[5] - f[4]) / 1e6 for f in fs if f[4] and f[5]]
            total = [(f[6] - f[1]) / 1e6 for f in fs]
            tex = [f[9] for f in fs]
            buf = [f[11] for f in fs]
            vsync = 1000.0 / 360.0
            missed = sum(max(0, round(g / vsync) - 1) for g in gaps if g < 50)
            res["windows"][w] = {
                "missed_vsyncs": missed,
                "gaps_over_8ms": sum(1 for g in gaps if g > 8),
                "frames": len(fs),
                "fps": round(len(fs) / secs, 1),
                "gui_anim_per_s": round(len(an) / secs, 1),
                "interval_ms_p50": round(pct(gaps, 50), 2),
                "interval_ms_p99": round(pct(gaps, 99), 2),
                "interval_ms_max": round(max(gaps), 2) if gaps else 0,
                "render_cpu_ms_p50": round(pct(cpu, 50), 3),
                "render_cpu_ms_p95": round(pct(cpu, 95), 3),
                "render_cpu_ms_p99": round(pct(cpu, 99), 3),
                "render_cpu_ms_max": round(max(cpu), 3) if cpu else 0,
                "driver_cpu_ms_p50": round(pct(drv, 50), 3),
                "sync_ms_p50": round(pct(sync, 50), 3),
                "sync_ms_p99": round(pct(sync, 99), 3),
                "render_ms_p50": round(pct(render, 50), 3),
                "render_ms_p99": round(pct(render, 99), 3),
                "frame_wall_ms_p50": round(pct(total, 50), 3),
                "frame_wall_ms_p99": round(pct(total, 99), 3),
                "tex_bytes_per_frame_mean": round(sum(tex) / len(fs)) if fs else 0,
                "tex_bytes_per_frame_max": max(tex) if tex else 0,
                "tex_MB_per_s": round(sum(tex) / secs / 1e6, 2),
                "tex_uploads_per_s": round(sum(f[10] for f in fs) / secs, 1),
                "buf_bytes_per_frame_mean": round(sum(buf) / len(fs)) if fs else 0,
                "buf_MB_per_s": round(sum(buf) / secs / 1e6, 2),
                "buf_uploads_per_frame": round(sum(f[12] for f in fs) / len(fs), 1) if fs else 0,
                "draws_per_frame": round(sum(f[14] for f in fs) / len(fs), 1) if fs else 0,
                "tex_created_per_s": round(sum(f[13] for f in fs) / secs, 1),
                "render_over_budget": sum(1 for c in cpu if c > 1000.0 / 360.0),
                "render_cpu_pct": round(sum(cpu) / 1e3 / secs * 100, 2),
                "driver_cpu_pct": round(sum(drv) / 1e3 / secs * 100, 2),
            }
        st = [s for s in stalls if t0 <= s[0] < t1]
        lat = sorted((s[1] / 1e6 for s in st), reverse=True)
        res["stalls"] = {
            "count_ge_threshold": len(lat),
            "total_ms": round(sum(lat), 1),
            "max_ms": round(lat[0], 2) if lat else 0,
            "top": [round(x, 1) for x in lat[:10]],
            "over_8ms": sum(1 for x in lat if x > 8),
            "over_16ms": sum(1 for x in lat if x > 16),
            "over_50ms": sum(1 for x in lat if x > 50),
        }
        # The watchdog's histogram for this interval: the first stall-hist
        # reply after its end (scenarios switch the watchdog per interval).
        hist = next((text for t, text in replies if t >= t1 and text.startswith("stall-hist")), None)
        if hist is not None:
            bins = {}
            for item in hist.split()[1:]:
                b, n = item.split(":")
                bins[int(b)] = int(n)
            total = sum(bins.values())
            def hp(q):
                acc = 0
                for b in sorted(bins):
                    acc += bins[b]
                    if acc >= q * total:
                        return (b + 1) * 0.25
                return 0.0
            res["stalls"]["pings"] = total
            res["stalls"]["latency_ms_p50"] = hp(0.5)
            res["stalls"]["latency_ms_p99"] = hp(0.99)
            res["stalls"]["latency_ms_p999"] = hp(0.999)
        results.append(res)
    return results


def main():
    res = summarize(sys.argv[1])
    if "--json" in sys.argv:
        for r in res:
            print(json.dumps(r))
        return
    for r in res:
        st = r["stalls"]
        print(f"== {r['label']} ({r['seconds']} s) stalls>=thr {st['count_ge_threshold']} (>8 {st['over_8ms']}, >16 {st['over_16ms']}, >50 {st['over_50ms']}) total {st['total_ms']} ms max {st['max_ms']} ms top {st['top'][:6]}"
              + (f" | ping p50 {st['latency_ms_p50']} p99 {st['latency_ms_p99']} p99.9 {st['latency_ms_p999']} ms" if "pings" in st else ""))
        for w, s in r["windows"].items():
            print(f"  win{w}: fps {s['fps']} gui {s['gui_anim_per_s']}/s | rcpu p50 {s['render_cpu_ms_p50']} p99 {s['render_cpu_ms_p99']} ms ({s['render_cpu_pct']}%) drv {s['driver_cpu_pct']}% "
                  f"| missed {s['missed_vsyncs']} gaps>8ms {s['gaps_over_8ms']} | sync {s['sync_ms_p50']} render {s['render_ms_p50']} wall {s['frame_wall_ms_p50']} p99 {s['frame_wall_ms_p99']} | int p50 {s['interval_ms_p50']} p99 {s['interval_ms_p99']} max {s['interval_ms_max']} "
                  f"| tex {s['tex_bytes_per_frame_mean']} B/f {s['tex_MB_per_s']} MB/s ({s['tex_uploads_per_s']}/s, new {s['tex_created_per_s']}/s) buf {s['buf_bytes_per_frame_mean']} B/f x{s['buf_uploads_per_frame']} | draws/f {s['draws_per_frame']}")


if __name__ == "__main__":
    main()
