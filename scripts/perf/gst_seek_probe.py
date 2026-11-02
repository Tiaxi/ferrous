#!/usr/bin/env python3
"""Seek latency of a bare playbin3 with different seek flags.

Plays FILE into a synced fakesink at volume 0, then seeks to each target
(seconds) with FLUSH plus the given flags and times the ASYNC_DONE.
Usage: gst_seek_probe.py FILE accurate|key_unit|key_snap|two_step TARGET...
"""
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import Gst  # noqa: E402

Gst.init(None)
path, mode = sys.argv[1], sys.argv[2]
targets = [float(t) for t in sys.argv[3:]]
two_step = mode == "two_step"
if two_step:
    mode = "key_snap"
flags = Gst.SeekFlags.FLUSH | {
    "accurate": Gst.SeekFlags.ACCURATE,
    "key_unit": Gst.SeekFlags.KEY_UNIT,
    "key_snap": Gst.SeekFlags.KEY_UNIT | Gst.SeekFlags.SNAP_BEFORE,
}[mode]
pipe = Gst.ElementFactory.make("playbin3")
pipe.set_property("uri", Gst.filename_to_uri(path))
sink = Gst.ElementFactory.make("fakesink")
sink.set_property("sync", True)
pipe.set_property("audio-sink", sink)
pipe.set_property("volume", 0.0)
bus = pipe.get_bus()


def wait_async_done(timeout_s=20):
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        msg = bus.timed_pop_filtered(
            100 * Gst.MSECOND, Gst.MessageType.ASYNC_DONE | Gst.MessageType.ERROR
        )
        if msg and msg.type == Gst.MessageType.ERROR:
            raise SystemExit(msg.parse_error())
        if msg:
            return True
    return False


pipe.set_state(Gst.State.PAUSED)
wait_async_done()
pipe.set_state(Gst.State.PLAYING)
time.sleep(1.0)
results = []
for target in targets:
    start = time.monotonic()
    pipe.seek_simple(Gst.Format.TIME, flags, int(target * Gst.SECOND))
    wait_async_done()
    if two_step:
        # Then an accurate seek, which starts from the index entry the first
        # one added.
        pipe.seek_simple(
            Gst.Format.TIME,
            Gst.SeekFlags.FLUSH | Gst.SeekFlags.ACCURATE,
            int(target * Gst.SECOND),
        )
        wait_async_done()
    elapsed = (time.monotonic() - start) * 1000
    time.sleep(0.3)
    ok, pos = pipe.query_position(Gst.Format.TIME)
    results.append(f"{target:.0f}s:{elapsed:.0f}ms(pos {pos / Gst.SECOND:.2f})")
pipe.set_state(Gst.State.NULL)
print(mode, " ".join(results))
