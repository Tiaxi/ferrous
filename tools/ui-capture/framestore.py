#!/usr/bin/env python3
"""A recording's frames for the analysers: each PNG decoded once, then kept as tiles.

The analysers read a recording many times over (the changes, the key frames, each element's ink, the perceived
rows), and decoding a 1600x1000 PNG takes about 18 ms. Between two frames of a UI recording little changes, so a
frame is kept as tiles that it shares with the frame before where their bytes are the same: whole recordings fit in
5 to 15 % of their raw size and a frame is put together again in about 2 ms, pixel for pixel. A recording that does
not shrink (a plot that changes everywhere) stops being kept at MAX_BYTES; the rest of it is decoded on each read.
"""
import collections
import os

import numpy as np
from PIL import Image

TILE_W, TILE_H = 128, 32
# per recording, in tiles: pack.py's workers each hold one recording at a time, so this bounds what they add
MAX_BYTES = 256 << 20
# the last frames read, whole (neighbours are compared, and the first frame with every other)
RECENT = 6


class FrameStore:
    """The frames `files` of folder `d` as RGB PIL images: ims[k], len(ims) and iteration work as on a list. An
    image handed out is shared with later reads of the same frame: crop or copy it, don't draw on it."""

    def __init__(self, d, files, max_bytes=MAX_BYTES, recent=RECENT):
        self.d, self.files, self.max_bytes, self.recent = d, list(files), max_bytes, recent
        self.kept = {}  # frame -> ((width, height), its tiles in row order)
        self.bytes = 0  # of the distinct tiles kept
        self.cache = collections.OrderedDict()

    def __len__(self):
        return len(self.files)

    def __getitem__(self, k):
        if k < 0:
            k += len(self.files)
        if k in self.cache:
            self.cache.move_to_end(k)
            return self.cache[k]
        if k in self.kept:
            im = self._assemble(*self.kept[k])
        else:
            with Image.open(os.path.join(self.d, self.files[k])) as f:
                im = f.convert("RGB")
            self._keep(k, im)
        self.cache[k] = im
        while len(self.cache) > self.recent:
            self.cache.popitem(last=False)
        return im

    @staticmethod
    def _boxes(size):
        w, h = size
        return ((x, y, min(w, x + TILE_W), min(h, y + TILE_H))
                for y in range(0, h, TILE_H) for x in range(0, w, TILE_W))

    def _keep(self, k, im):
        if self.bytes >= self.max_bytes:
            return
        a = np.asarray(im)
        size, before = self.kept.get(k - 1, (None, None))
        if size != im.size:
            before = None
        tiles = []
        for i, (x0, y0, x1, y1) in enumerate(self._boxes(im.size)):
            t = a[y0:y1, x0:x1].tobytes()
            if before is not None and t == before[i]:
                t = before[i]
            else:
                self.bytes += len(t)
            tiles.append(t)
        self.kept[k] = (im.size, tiles)

    def _assemble(self, size, tiles):
        a = np.empty((size[1], size[0], 3), dtype=np.uint8)
        for t, (x0, y0, x1, y1) in zip(tiles, self._boxes(size)):
            a[y0:y1, x0:x1] = np.frombuffer(t, dtype=np.uint8).reshape(y1 - y0, x1 - x0, 3)
        return Image.fromarray(a)
