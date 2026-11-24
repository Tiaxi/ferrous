#!/usr/bin/env python3
"""Illustrative cover art for the fixture library: each folder's cover.jpg (1200 px) redrawn as a generated
landscape (a sky gradient, a sun or moon with its glow, three ridges in aerial perspective, film grain, a
vignette), seeded by the album's folder name, so covers look like pictures rather than flat colour fields. No
personal media; nothing downloaded. A folder done has `.cover-illustrated`; embedded covers are left alone.
Usage: make_cover_art.py <library> (or `landscape(name)` from another generator)"""
import glob
import hashlib
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

N = 1200


def landscape(seed_name, n=N):
    """The cover for a folder named `seed_name`: an RGB image, n x n, the same for the same name."""
    rng = np.random.default_rng(int(hashlib.sha1(seed_name.encode()).hexdigest()[:8], 16))
    hue = rng.uniform(0, 1)
    def hsv(h, s, v):
        h = (h % 1) * 6
        i, f = int(h), h - int(h)
        p_, q, t = v * (1 - s), v * (1 - s * f), v * (1 - s * (1 - f))
        return np.array([(v, t, p_), (q, v, p_), (p_, v, t), (p_, q, v), (t, p_, v), (v, p_, q)][i % 6]) * 255
    top, bot = hsv(hue, 0.55, 0.35), hsv(hue + 0.08, 0.45, 0.95)
    yy = np.linspace(0, 1, n)[:, None, None]
    img = top * (1 - yy) + bot * yy + np.zeros((n, n, 3))
    # the sun or moon and its glow
    cx, cy, rad = rng.uniform(0.25, 0.75) * n, rng.uniform(0.2, 0.45) * n, rng.uniform(0.05, 0.11) * n
    Y, X = np.mgrid[0:n, 0:n]
    dist = np.hypot(X - cx, Y - cy)
    glow = np.exp(-((dist / (rad * 3.2)) ** 2))[..., None]
    sun = hsv(hue + 0.12, 0.25, 1.0)
    img = img * (1 - 0.55 * glow) + sun * 0.55 * glow
    img[dist < rad] = sun
    # three ridges: farther ones lighter and bluer (aerial perspective)
    xs = np.arange(n)
    for k, (base, amp, shade) in enumerate(((0.58, 0.10, 0.70), (0.70, 0.12, 0.45), (0.84, 0.10, 0.20))):
        ridge = base * n + sum(amp * n / (j + 1) * np.sin(xs / n * np.pi * (j + 1) * rng.uniform(1.2, 3.5) + rng.uniform(0, 6.28))
                               for j in range(4)) / 2
        col = hsv(hue + 0.55, 0.35, shade) * 0.6 + top * 0.4 * (1 - k / 3)
        mask = Y >= ridge[None, :]
        img[mask] = img[mask] * 0.15 + col * 0.85
    # grain and a vignette
    img += rng.normal(0, 7, (n, n, 1))
    vig = 1 - 0.35 * (np.hypot(X - n / 2, Y - n / 2) / (n * 0.72)) ** 2
    img = np.clip(img * vig[..., None], 0, 255).astype(np.uint8)
    return Image.fromarray(img).filter(ImageFilter.GaussianBlur(0.6))


def main(lib):
    for p in sorted(glob.glob(os.path.join(lib, "*", "*", "cover.jpg"))):
        d = os.path.dirname(p)
        if os.path.exists(os.path.join(d, ".cover-illustrated")):
            continue
        landscape(os.path.basename(d)).save(p + ".partial.jpg", quality=90)
        os.replace(p + ".partial.jpg", p)
        open(os.path.join(d, ".cover-illustrated"), "w").close()
        print("cover:", os.path.relpath(p, lib))


if __name__ == "__main__":
    main(sys.argv[1])
