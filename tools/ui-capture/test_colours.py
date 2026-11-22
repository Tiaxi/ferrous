#!/usr/bin/env python3
"""colours.unique_colours gives exactly what np.unique(axis=0) gives, on every path it takes."""
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import colours  # noqa: E402


class UniqueColours(unittest.TestCase):
    def check(self, a):
        want_v, want_c = np.unique(a.reshape(-1, 3), axis=0, return_counts=True)
        got_v, got_c = colours.unique_colours(a)
        self.assertEqual(got_v.dtype, want_v.dtype)
        np.testing.assert_array_equal(got_v, want_v)
        np.testing.assert_array_equal(got_c, want_c)
        if not len(want_c):
            return
        # the callers' picks (the most frequent; the n most frequent by argsort) come out the same
        self.assertEqual(int(got_c.argmax()), int(want_c.argmax()))
        np.testing.assert_array_equal(got_c.argsort()[::-1][:4], want_c.argsort()[::-1][:4])

    def test_small_and_large_arrays_of_every_dtype(self):
        rng = np.random.default_rng(7)
        for dtype in (np.uint8, np.int16, np.int32, np.int64):
            for shape in ((5, 7, 3), (600, 700, 3)):  # sorted keys, then a bincount
                # few colours, so counts tie and the order of ties matters
                pal = rng.integers(0, 256, size=(9, 3))
                a = pal[rng.integers(0, len(pal), size=shape[:-1])].astype(dtype)
                self.check(a)
                self.check(rng.integers(0, 256, size=shape).astype(dtype))

    def test_flat_pixels_and_edges(self):
        self.check(np.array([[0, 0, 0], [255, 255, 255], [0, 0, 255], [255, 0, 0], [0, 0, 0]], dtype=np.uint8))
        self.check(np.zeros((0, 3), dtype=np.uint8))

    def test_values_outside_a_colour_fall_back(self):
        self.check(np.array([[-3, 4, 5], [300, 1, 2], [-3, 4, 5]], dtype=np.int32))


if __name__ == "__main__":
    unittest.main()
