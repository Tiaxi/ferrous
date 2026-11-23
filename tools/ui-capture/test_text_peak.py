#!/usr/bin/env python3
"""Unit tests of the text-overlap amount (motion_strip.text_peak): the trace records effective opacities, so a
cross-fade of two titles inside fading layers is read at those values, not dimmed again by its ancestors (h24: a
track change drew both titles at .51/.22 and .20/.42 and the measure said .04).

Needs no KWin, app or shots. Run with `python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import motion_strip  # noqa: E402

RECT = [116, 141, 232, 100]
OUT = [(-50, 1.0), (19, 1.0), (26, 0.51), (41, 0.2), (57, 0.06), (74, 0.01), (91, 0.0)]
IN = [(-50, 0.0), (19, 0.0), (26, 0.22), (41, 0.42), (57, 0.57), (74, 0.7), (91, 0.8), (124, 1.0)]


def track(pts, text):
    return dict(text=text, rect=RECT, rects=[RECT] * len(pts), pts=pts, layers=["-"] * len(pts), fulls=None)


def tracks(out_pts, in_pts):
    """An outgoing title in a fading part and an incoming one in a fading window, as the trace records them: every
    item at its effective opacity (the part's fade shows in the title's own values too)."""
    return {
        "panel/title/outgoingLayer": track([(-50, 1.0), (224, 0.0)], False),
        "panel/title/outgoingLayer/outgoingPart": track(out_pts, False),
        "panel/title/outgoingLayer/outgoingPart/outgoingTitle": track(out_pts, True),
        "panel/title/incoming": track(in_pts, False),
        "panel/title/incoming/incomingWindow": track(in_pts, False),
        "panel/title/incoming/incomingWindow/LineText": track(in_pts, True),
    }


class Peak(unittest.TestCase):
    def test_a_cross_fade_inside_fading_layers_reads_at_its_drawn_opacities(self):
        p = motion_strip.text_peak(tracks(OUT, IN), [dict(start=0, end=300)])
        self.assertGreaterEqual(p["peak"], 0.2, p)
        self.assertGreater(p["ms"], 0, p)

    def test_the_outgoing_text_gone_before_the_incoming_comes_is_no_overlap(self):
        out = [(-50, 1.0), (19, 1.0), (40, 0.0)]
        inc = [(-50, 0.0), (40, 0.0), (60, 0.5), (100, 1.0)]
        p = motion_strip.text_peak(tracks(out, inc), [dict(start=0, end=300)])
        self.assertLess(p["peak"], 0.05, p)
        self.assertEqual(p["ms"], 0, p)


if __name__ == "__main__":
    unittest.main()
