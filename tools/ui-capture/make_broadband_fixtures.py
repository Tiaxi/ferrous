#!/usr/bin/env python3
"""Broadband fixtures: bright data for the plot's labels, plates and level meter, outside the library (the
library and its counts stay as they are): white noise in stereo and pink noise in 5.1, every channel its own
noise (a different seed), at a high level. Usage: make_broadband_fixtures.py <dir>"""
import os
import subprocess
import sys

d = sys.argv[1]
os.makedirs(d, exist_ok=True)


def noise(path, secs, color, channels, layout, meta, amp=0.5, rate=48000):
    srcs = ";".join(f"anoisesrc=color={color}:amplitude={amp}:duration={secs}:sample_rate={rate}:seed={i + 1}[n{i}]"
                    for i in range(channels))
    merge = "".join(f"[n{i}]" for i in range(channels)) + f"amerge=inputs={channels},aformat=channel_layouts={layout}[out]"
    cmd = ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-filter_complex", f"{srcs};{merge}", "-map", "[out]",
           "-c:a", "flac", "-sample_fmt", "s16"]
    for k, v in meta.items():
        cmd += ["-metadata", f"{k}={v}"]
    subprocess.run(cmd + [path], check=True)


base = {"artist": "Test Signals", "album_artist": "Test Signals", "album": "Broadband", "date": "2026", "genre": "Test"}
p1, p2 = os.path.join(d, "01 - White Noise.flac"), os.path.join(d, "02 - Pink Noise 5.1.flac")
if not os.path.exists(p1):
    noise(p1, 60, "white", 2, "stereo", dict(base, title="White Noise", track="1/3"))
if not os.path.exists(p2):
    noise(p2, 60, "pink", 6, "5.1", dict(base, title="Pink Noise 5.1", track="2/3"))
# a tone, digital silence, a tone (4 s each, stereo): the plot's colour for silence inside a track, and its
# pre-start and post-end regions on a short track
p3 = os.path.join(d, "03 - Tone Silence Tone.flac")
if not os.path.exists(p3):
    cmd = ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-filter_complex",
           "sine=frequency=440:duration=4:sample_rate=48000[a];anullsrc=r=48000:cl=mono:d=4[b];"
           "sine=frequency=660:duration=4:sample_rate=48000[c];[a][b][c]concat=n=3:v=0:a=1,pan=stereo|c0=c0|c1=c0[out]",
           "-map", "[out]", "-c:a", "flac", "-sample_fmt", "s16"]
    for k, v in dict(base, title="Tone Silence Tone", track="3/3").items():
        cmd += ["-metadata", f"{k}={v}"]
    subprocess.run(cmd + [p3], check=True)
print("ok")

# a music-like level for the meter's stills: band-limited noise and a chord under a slow swell and a beat,
# about -18 dB RMS with dynamics (peaks near -6 dB), stereo, 60 s
p4 = os.path.join(d, "04 - Moving Levels.flac")
if not os.path.exists(p4):
    cmd = ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-filter_complex",
           "anoisesrc=color=pink:amplitude=0.35:duration=60:sample_rate=48000:seed=7,lowpass=f=5000[n];"
           "sine=frequency=220:duration=60:sample_rate=48000[a];sine=frequency=277:duration=60:sample_rate=48000[b];"
           "sine=frequency=330:duration=60:sample_rate=48000[c];[a][b][c]amix=inputs=3,volume=0.6[ch];"
           "[n][ch]amix=inputs=2,volume='0.55+0.35*sin(2*PI*t*0.23)+0.25*lt(mod(t,0.5),0.08)':eval=frame,"
           "aecho=0.6:0.5:60|90:0.3|0.25,pan=stereo|c0=0.9*c0|c1=0.7*c0,volume=20,alimiter=limit=0.5:level=disabled[out]",
           "-map", "[out]", "-c:a", "flac", "-sample_fmt", "s16"]
    for k, v in dict(base, title="Moving Levels", track="4/4").items():
        cmd += ["-metadata", f"{k}={v}"]
    subprocess.run(cmd + [p4 + ".partial.flac"], check=True)
    os.rename(p4 + ".partial.flac", p4)
