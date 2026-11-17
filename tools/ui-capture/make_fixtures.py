#!/usr/bin/env python3
"""Generates a self-contained fixture library of synthetic music for UI screenshots.

All names are invented; audio is synthesized with ffmpeg, covers with ImageMagick.
Usage: make_fixtures.py <out-dir>
"""
import os
import random
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

OUT = os.path.abspath(sys.argv[1])
random.seed(373)

ARTISTS = {
    "Aster Vale": ["Paper Lanterns", "Slow Weather", "Night Ferry"],
    "The Quiet Meridian": ["Low Tide Atlas", "Glass Orchard", "Small Hours", "Field Recordings Vol. 2"],
    "Northbound Kites": ["Kites", "Second Wind", "Altitude"],
    "Juniper & Ash": ["Ember Season", "Under Birch"],
    "Lumen Drift": ["Parallax", "Signal / Noise", "Afterimage", "Dust Motes"],
    "Halvorsen Trio": ["Nocturnes for Trio", "Winter Suite"],
    "Orchid Static": ["Interference", "Tape Hiss Lullabies", "Static Bloom"],
    "Mira Calloway": ["Harbour Lights", "Copper Moon", "The Long Way Round"],
    "The Extraordinarily Long-Named Chamber Ensemble of Upper Westfield": [
        "Variations on a Theme That Nobody Remembers, Including the Extended Coda",
    ],
    "Sølvi Ørnulf": ["Fjær og Frost"],
    "Kōji Hanamura": ["Sakura Tape Loops"],
    "Various Artists": ["Sampler 2025"],
}

TITLE_WORDS = ("Amber Window Lantern Drift Harbour Quiet Signal Ember River Glass Copper "
               "Morning Static Hollow Orchard North Paper Velvet Echo Field Cinder Tide "
               "Willow Shore Marble Halo Pale Moth Lumen Fern Thread Arc Slow").split()

# Per-album audio format overrides (album name -> dict).
SPECIAL = {
    "Parallax": dict(rate=96000, bits=24, channels=2),                # hi-res stereo
    "Afterimage": dict(rate=48000, bits=24, channels=6),              # 5.1
    "Winter Suite": dict(rate=48000, bits=24, channels=8),            # 7.1
    "Dust Motes": dict(rate=192000, bits=24, channels=2),             # 192 kHz
    "Kites": dict(codec="mp3"),
    "Second Wind": dict(codec="opus"),
    "Field Recordings Vol. 2": dict(channels=1),
    "Small Hours": dict(long=True),                                   # ~3 min tracks to play
    "Nocturnes for Trio": dict(rate=88200, bits=24, channels=4),       # quad
}
NO_COVER = {"Static Bloom", "Copper Moon"}
EMBEDDED_COVER = {"Harbour Lights", "Interference", "Ember Season"}
PALETTES = [("#2b4162", "#fa9f42"), ("#0b6e4f", "#f4d35e"), ("#5b2a86", "#9bc1bc"),
            ("#1d3557", "#e63946"), ("#3d405b", "#f2cc8f"), ("#264653", "#e9c46a"),
            ("#6d597a", "#eaac8b"), ("#283618", "#dda15e"), ("#14213d", "#fca311"),
            ("#4a4e69", "#c9ada7"), ("#003049", "#fcbf49"), ("#2d6a4f", "#b7e4c7")]


def run(cmd):
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise SystemExit(f"failed: {' '.join(cmd)}\n{r.stderr[-2000:]}")


def cover(path, album, artist, idx):
    bg, fg = PALETTES[idx % len(PALETTES)]
    shape = idx % 3
    draw = {
        0: f"circle 600,600 600,{300 + (idx * 37) % 200}",
        1: f"rectangle {200 + idx % 5 * 20},300 {1000 - idx % 4 * 30},900",
        2: "polygon 600,180 1020,980 180,980",
    }[shape]
    run(["magick", "-size", "1200x1200", f"gradient:{bg}-black",
         "-fill", fg, "-draw", draw,
         "-fill", "white", "-pointsize", "64", "-gravity", "south",
         "-annotate", "+0+60", album[:28],
         "-pointsize", "40", "-gravity", "north", "-annotate", "+0+50", artist[:36],
         "-quality", "88", path])


def track_audio(path, seconds, fmt, seed, meta, cover_path):
    rng = random.Random(seed)
    rate = fmt.get("rate", 44100)
    ch = fmt.get("channels", 2)
    base = rng.choice([110, 130.81, 146.83, 164.81, 196, 220])
    step = rng.choice([0.25, 0.5, 0.75])
    scale = rng.choice(["0,2,4,7,9,12,7,4", "0,3,5,7,10,12,10,7", "0,4,7,11,12,7,4,2"]).split(",")
    # Melody: piecewise notes from the scale, with harmonics and a decay envelope per note.
    note = "+".join(f"{s}*eq(mod(floor(t/{step}),8),{i})" for i, s in enumerate(scale))
    f = f"({base}*pow(2,({note})/12))"
    env = f"exp(-3*mod(t,{step}))"
    swell = f"(0.55+0.45*sin(2*PI*t/{rng.uniform(9, 23):.2f}))"
    mel = (f"{swell}*{env}*(0.5*sin(2*PI*{f}*t)+0.2*sin(4*PI*{f}*t)+0.1*sin(6*PI*{f}*t)"
           f"+0.05*sin(10*PI*{f}*t))")
    bass = f"0.25*sin(2*PI*{base / 2}*t)*(0.6+0.4*sin(2*PI*t/{step * 4}))"
    sweep = f"0.04*sin(2*PI*(200+{rng.randint(800, 3000)}*mod(t/8,1))*t)"
    exprs = []
    for c in range(ch):
        pan = 0.6 + 0.4 * ((c % 2) * 2 - 1) * 0.5
        if ch >= 6 and c == 3:  # LFE: bass only
            exprs.append(f"0.5*sin(2*PI*{base / 2}*t)")
        elif ch >= 6 and c >= 4:  # surrounds: quieter, sweep-heavy
            exprs.append(f"0.3*({mel})*0.5+{sweep}*3")
        else:
            exprs.append(f"{pan:.2f}*({mel})+{bass}+{sweep}")
    layout = {1: "mono", 2: "stereo", 4: "quad", 6: "5.1", 8: "7.1"}[ch]
    src = f"aevalsrc='{'|'.join(exprs)}':s={rate}:d={seconds}:c={layout}"
    noise = f"anoisesrc=d={seconds}:c=pink:r={rate}:a=0.03:seed={seed},aformat=channel_layouts={layout}"
    graph = f"[0][1]amix=inputs=2:normalize=0,afade=t=in:d=0.5,afade=t=out:st={seconds - 1.5}:d=1.5[a]"
    cmd = ["ffmpeg", "-nostdin", "-y", "-loglevel", "error", "-f", "lavfi", "-i", src,
           "-f", "lavfi", "-i", noise]
    if cover_path:
        cmd += ["-i", cover_path]
    cmd += ["-filter_complex", graph, "-map", "[a]"]
    codec = fmt.get("codec", "flac")
    if codec == "flac":
        cmd += ["-c:a", "flac", "-sample_fmt", "s32" if fmt.get("bits", 16) == 24 else "s16"]
        if fmt.get("bits", 16) == 24:
            cmd += ["-bits_per_raw_sample", "24"]
    elif codec == "mp3":
        cmd += ["-c:a", "libmp3lame", "-b:a", "192k", "-id3v2_version", "3"]
    elif codec == "opus":
        cmd += ["-c:a", "libopus", "-b:a", "128k"]
    if cover_path:
        cmd += ["-map", "2", "-c:v", "mjpeg" if codec != "opus" else "copy",
                "-disposition:v", "attached_pic", "-metadata:s:v", "comment=Cover (front)"]
    for k, v in meta.items():
        cmd += ["-metadata", f"{k}={v}"]
    cmd.append(path)
    run(cmd)


def main():
    jobs = []
    remove_later = []
    album_idx = 0
    for artist, albums in ARTISTS.items():
        for a_i, album in enumerate(albums):
            album_idx += 1
            fmt = SPECIAL.get(album, {})
            year = 1994 + (album_idx * 7) % 31
            d = os.path.join(OUT, artist, f"{year} - {album}")
            os.makedirs(d, exist_ok=True)
            cover_path = None
            if album not in NO_COVER:
                cover_path = os.path.join(d, "cover.jpg")
                cover(cover_path, album, artist, album_idx)
            embed = album in EMBEDDED_COVER
            n_tracks = 4 + (album_idx * 3) % 7
            if fmt.get("rate", 44100) >= 96000 or fmt.get("channels", 2) > 2:
                n_tracks = min(n_tracks, 4)
            ext = {"flac": "flac", "mp3": "mp3", "opus": "opus"}[fmt.get("codec", "flac")]
            for t in range(1, n_tracks + 1):
                rng = random.Random(album_idx * 100 + t)
                title = " ".join(rng.sample(TITLE_WORDS, rng.choice([1, 2, 2, 3])))
                if album_idx % 5 == 0 and t == 2:
                    title = ("A Remarkably Long Track Title That Will Certainly Need Eliding "
                             "Somewhere in the Queue")
                if artist == "Sølvi Ørnulf":
                    title = ["Nordlys", "Fjær", "Frostrøyk", "Åpent vann"][t % 4]
                if artist == "Kōji Hanamura":
                    title = ["夜桜", "Tape Loop " + str(t), "雨の音", "Ōmi"][t % 4]
                seconds = 200 + rng.randint(0, 60) if fmt.get("long") else 25 + rng.randint(0, 50)
                if fmt.get("rate", 44100) >= 96000 or fmt.get("channels", 2) > 2:
                    seconds = 40 + rng.randint(0, 30)
                track_artist = artist
                if artist == "Various Artists":
                    track_artist = rng.choice([a for a in ARTISTS if a != "Various Artists"][:8])
                meta = {"title": title, "artist": track_artist, "album_artist": artist,
                        "album": album, "date": str(year), "track": f"{t}/{n_tracks}",
                        "genre": rng.choice(["Ambient", "Folk", "Electronic", "Jazz", "Classical"])}
                path = os.path.join(d, f"{t:02d} - {title.replace('/', '-')}.{ext}")
                jobs.append((path, seconds, fmt, album_idx * 100 + t, meta,
                             cover_path if embed else None))
            if embed and cover_path:
                remove_later.append(cover_path)
    with ThreadPoolExecutor(max_workers=6) as ex:
        list(ex.map(lambda j: track_audio(*j), jobs))
    for p in remove_later:
        os.remove(p)  # embedded-only covers: no folder image
    print(f"{album_idx} albums, {len(jobs)} tracks in {OUT}")


if __name__ == "__main__":
    main()
