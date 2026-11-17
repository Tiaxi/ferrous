#!/usr/bin/env python3
"""Stand-in cover fixtures: tagged tracks without any art (none embedded, none in the folder), outside the
library, for the stand-in cover's text cases: an album title long enough to wrap, no album tag, and an album named
as the track. Invented names. Each file is made only if missing. Usage: make_standin_fixtures.py <dir>"""
import os
import subprocess
import sys

d = sys.argv[1]
os.makedirs(d, exist_ok=True)
base = {"artist": "Harrow Lane", "album_artist": "Harrow Lane", "date": "2024", "genre": "Folk"}
FILES = [
    ("01 - Long Album.flac", dict(base, title="Long Album", track="1/3",
                                  album="A Remarkably Long Album Title That Wraps Across the Stand-In Cover")),
    ("02 - No Album Here.flac", dict(base, title="No Album Here", track="2/3")),
    ("03 - Same Name.flac", dict(base, title="Same Name", track="3/3", album="Same Name")),
]
for i, (name, meta) in enumerate(FILES):
    p = os.path.join(d, name)
    if os.path.exists(p):
        continue
    cmd = ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-f", "lavfi", "-i",
           f"sine=frequency={330 + 110 * i}:duration=40:sample_rate=44100", "-ac", "2", "-c:a", "flac"]
    for k, v in meta.items():
        cmd += ["-metadata", f"{k}={v}"]
    subprocess.run(cmd + [p + ".partial.flac"], check=True)
    os.rename(p + ".partial.flac", p)  # complete or absent
