#!/usr/bin/env python3
"""Extra fixtures: a 4 s track, a FLAC cut off before its header's length, an untagged file."""
import os, subprocess, sys
LIB = sys.argv[1]
d = os.path.join(LIB, "Orchid Static", "2019 - Loose Ends")
os.makedirs(d, exist_ok=True)
def tone(path, secs, meta, freq=330, rate=44100):
    cmd = ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-f", "lavfi", "-i",
           f"aevalsrc='0.4*sin(2*PI*{freq}*t)*(0.6+0.4*sin(2*PI*t/1.3))+0.15*sin(2*PI*(400+2500*mod(t/5,1))*t)|0.35*sin(2*PI*{freq*1.5}*t)+0.1*sin(2*PI*{freq/2}*t)':s={rate}:d={secs}:c=stereo",
           "-c:a", "flac", "-sample_fmt", "s16"]
    for k, v in meta.items():
        cmd += ["-metadata", f"{k}={v}"]
    subprocess.run(cmd + [path], check=True)
base = {"artist": "Orchid Static", "album_artist": "Orchid Static", "album": "Loose Ends", "date": "2019", "genre": "Electronic"}
tone(os.path.join(d, "01 - Brief Signal.flac"), 4, dict(base, title="Brief Signal", track="1/3"))
cut = os.path.join(d, "02 - Cut Tape.flac")
tone(cut, 60, dict(base, title="Cut Tape", track="2/3"), freq=262)
size = os.path.getsize(cut)
with open(cut, "r+b") as f:
    f.truncate(int(size * 0.55))
tone(os.path.join(d, "03 - Long Exhale.flac"), 45, dict(base, title="Long Exhale", track="3/3"), freq=196)
subprocess.run(["magick", "-size", "1200x1200", "gradient:#7a1f3d-black", "-fill", "#f6c177", "-draw",
                "circle 600,600 600,380", "-quality", "88", os.path.join(d, "cover.jpg")], check=True)
u = os.path.join(LIB, "Unsorted")
os.makedirs(u, exist_ok=True)
tone(os.path.join(u, "track07.flac"), 40, {}, freq=220)
print("ok")
