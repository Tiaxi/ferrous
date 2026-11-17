#!/usr/bin/env python3
"""A large invented library for the library-density shot: 320 artists, 1-3 albums each, 1-4 short tracks
per album (about 1,500 files of one second). Names are built from syllables with a fixed seed: invented, never
real. Every file is a tagged copy of one generated FLAC (mutagen sets the tags), so the whole library takes
seconds to make. Usage: make_large_library.py <dir>"""
import os
import random
import shutil
import subprocess
import sys

from mutagen.flac import FLAC

d = sys.argv[1]
tmp = d + ".partial"
shutil.rmtree(tmp, ignore_errors=True)
os.makedirs(tmp)
base = os.path.join(tmp, ".base.flac")
subprocess.run(["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                "sine=frequency=330:duration=1:sample_rate=44100", "-ac", "2", "-c:a", "flac", base], check=True)
rnd = random.Random(1010)
SYL = ["ar", "bel", "cor", "dan", "el", "fen", "gal", "hol", "ir", "jas", "kel", "lun", "mar", "nor", "ol", "pel",
       "quin", "ros", "sel", "tor", "ul", "vin", "wen", "yar", "zel", "ash", "bri", "cal", "dor", "eth"]
WORDS = ["Harbor", "Lantern", "Signal", "Ember", "Copper", "Meadow", "Static", "Tide", "Field", "Glass", "Hollow",
         "River", "Quiet", "Velvet", "Orchard", "Paper", "Thread", "Marble", "Drift", "Echo", "Cinder", "Fern"]


def name(n):
    return "".join(rnd.choice(SYL) for _ in range(n)).capitalize()


seen = set()
artists = []
while len(artists) < 320:
    a = name(rnd.randint(2, 3)) + (" " + name(rnd.randint(2, 3)) if rnd.random() < 0.6 else "")
    if rnd.random() < 0.15:
        a = "The " + rnd.choice(WORDS) + " " + rnd.choice(WORDS) + "s"
    if a not in seen:
        seen.add(a)
        artists.append(a)
n = 0
for a in artists:
    for k in range(rnd.randint(1, 3)):
        album = f"{rnd.choice(WORDS)} {rnd.choice(WORDS)}" + (f" {k + 2}" if k and rnd.random() < 0.3 else "")
        year = rnd.randint(1968, 2025)
        ad = os.path.join(tmp, a, f"{year} - {album}")
        os.makedirs(ad, exist_ok=True)
        tracks = rnd.randint(1, 4)
        for t in range(1, tracks + 1):
            title = f"{rnd.choice(WORDS)} {rnd.choice(WORDS)}"
            p = os.path.join(ad, f"{t:02d} - {title}.flac")
            shutil.copyfile(base, p)
            f = FLAC(p)
            f.update({"artist": a, "albumartist": a, "album": album, "title": title, "date": str(year),
                      "tracknumber": f"{t}/{tracks}", "genre": rnd.choice(["Jazz", "Folk", "Ambient", "Classical", "Rock"])})
            f.save()
            n += 1
os.remove(base)
shutil.rmtree(d, ignore_errors=True)
os.rename(tmp, d)  # complete or absent: a partial library is never used
with open(os.path.join(d, ".count"), "w") as f:
    f.write(str(n))
print(f"large library: {len(artists)} artists, {n} tracks")
