#!/usr/bin/env python3
"""A second, tiny library folder (one album) for the two-roots states. Usage: make_library2.py <dir>"""
import os, subprocess, sys
d = os.path.join(sys.argv[1], "Pale Harbor", "2021 - Demos")
os.makedirs(d, exist_ok=True)
for i, (t, f) in enumerate([("Tin Roof", 247), ("Night Bus", 294), ("Salt Air", 330)], 1):
    subprocess.run(["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-f", "lavfi", "-i",
        f"aevalsrc='0.4*sin(2*PI*{f}*t)*(0.6+0.4*sin(2*PI*t/2))|0.3*sin(2*PI*{f*1.25}*t)':s=44100:d=30:c=stereo",
        "-c:a", "flac", "-metadata", f"title={t}", "-metadata", "artist=Pale Harbor", "-metadata", "album=Demos",
        "-metadata", "date=2021", "-metadata", f"track={i}/3", os.path.join(d, f"{i:02d} - {t}.flac")], check=True)
subprocess.run(["magick", "-size", "800x800", "gradient:#1f4e5f-black", "-fill", "#e0e1dd", "-draw",
                "rectangle 200,300 600,500", os.path.join(d, "cover.jpg")], check=True)
