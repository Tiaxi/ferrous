#!/usr/bin/env python3
"""Exact colour counts of pixel arrays, fast.

`np.unique(px, axis=0, return_counts=True)` on an (N, 3) array sorts N rows lexicographically, which on a whole
shot takes about 0.4 s. A colour of three 0..255 channels packs into one integer that sorts the same way
(r << 16 | g << 8 | b), and numpy sorts 32-bit integers with the CPU's vector instructions: the same colours in
the same order with the same counts, in about 7 ms.
"""
import numpy as np


def unique_colours(px):
    """The same as np.unique(px.reshape(-1, 3), axis=0, return_counts=True): the distinct colours of an array of
    pixels (..., 3) in ascending (r, g, b) order, in px's dtype, and how often each occurs. Channels outside 0..255
    (not a colour) go to np.unique itself."""
    a = np.asarray(px).reshape(-1, 3)
    if a.dtype.kind not in "iu" or (a.size and (int(a.min()) < 0 or int(a.max()) > 255)):
        return np.unique(a, axis=0, return_counts=True)
    keys = (a[:, 0].astype(np.uint32) << 16) | (a[:, 1].astype(np.uint32) << 8) | a[:, 2].astype(np.uint32)
    keys.sort()
    starts = np.empty(len(keys), dtype=bool)  # where each colour's run starts
    starts[:1] = True
    starts[1:] = keys[1:] != keys[:-1]
    at = np.flatnonzero(starts)
    vals, counts = keys[at], np.diff(at, append=len(keys))
    out = np.empty((len(vals), 3), dtype=a.dtype)
    out[:, 0], out[:, 1], out[:, 2] = (vals >> 16) & 255, (vals >> 8) & 255, vals & 255
    return out, counts.astype(np.intp, copy=False)
