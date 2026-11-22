#!/usr/bin/env python3
"""Exact colour counts of pixel arrays, fast.

`np.unique(px, axis=0, return_counts=True)` on an (N, 3) array sorts N rows lexicographically, which on a whole
shot takes about 0.4 s. A colour of three 0..255 channels packs into one integer that sorts the same way
(r << 16 | g << 8 | b), so counting the packed integers gives the same colours in the same order with the same
counts, in a few milliseconds.
"""
import numpy as np

# above this many pixels a bincount over every 24-bit colour beats sorting the packed keys
_BINCOUNT_MIN = 1 << 18


def unique_colours(px):
    """The same as np.unique(px.reshape(-1, 3), axis=0, return_counts=True): the distinct colours of an array of
    pixels (..., 3) in ascending (r, g, b) order, in px's dtype, and how often each occurs. Channels outside 0..255
    (not a colour) go to np.unique itself."""
    a = np.asarray(px).reshape(-1, 3)
    if a.dtype.kind not in "iu" or (a.size and (int(a.min()) < 0 or int(a.max()) > 255)):
        return np.unique(a, axis=0, return_counts=True)
    keys = (a[:, 0].astype(np.int64) << 16) | (a[:, 1].astype(np.int64) << 8) | a[:, 2].astype(np.int64)
    if len(keys) >= _BINCOUNT_MIN:
        counts = np.bincount(keys, minlength=1 << 24)
        vals = np.flatnonzero(counts)
        counts = counts[vals]
    else:
        vals, counts = np.unique(keys, return_counts=True)
    out = np.empty((len(vals), 3), dtype=a.dtype)
    out[:, 0], out[:, 1], out[:, 2] = (vals >> 16) & 255, (vals >> 8) & 255, vals & 255
    return out, counts.astype(np.intp, copy=False)
