#!/usr/bin/env python3
"""Unit tests of a sheet's page layout (pack.layout): balancing the pages may ask for fewer tiles than fit, and
fewer tiles can leave none that meets the size bar (their widest is narrower than the bar), so the layout keeps the
full page then (the close capture failed on such a sheet: best_page returned None and the unpacking raised).

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import pack  # noqa: E402


class Layout(unittest.TestCase):
    def test_a_balanced_page_that_meets_no_bar_keeps_the_full_page(self):
        bar = pack.MIN_TILE
        # five narrow tiles, then a wide one, then a narrow one: the first six fit one page; balancing the seven
        # over two pages asks for four, all narrower than the bar
        tiles = [(bar - 100, 200, i) for i in range(5)] + [(bar + 600, 300, 5), (bar - 100, 200, 6)]
        self.assertIsNotNone(pack.best_page(tiles, 6, 40))
        self.assertIsNone(pack.best_page(tiles, 4, 40))
        pages = pack.layout(tiles, 6, 40)
        self.assertEqual([t[2] for _c, _w, ts in pages for t in ts], list(range(7)))

    def test_pages_still_balance_where_they_can(self):
        tiles = [(900, 500, i) for i in range(7)]
        pages = pack.layout(tiles, 6, 40)
        self.assertEqual(sum(len(ts) for _c, _w, ts in pages), 7)
        self.assertLessEqual(max(len(ts) for _c, _w, ts in pages) - min(len(ts) for _c, _w, ts in pages), 1)


if __name__ == "__main__":
    unittest.main()
