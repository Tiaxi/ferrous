#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw calls per frame (the probe's QRhiCommandBuffer counts) per state.

Usage: ui_batches.py OUT_DIR SAMPLE_FILE

Plays SAMPLE_FILE and samples 4 s each of: centered per channel, centered
downmix, rolling downmix, rolling per channel (after its history refills),
centered per channel with the level meter, and the waveform view.
Summarize with ui_summarize.py.
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_driver as d  # noqa: E402


def main():
    out, sample = sys.argv[1], sys.argv[2]
    p = d.default_paths()
    app = d.App(os.path.join(out, "run-batches"), p["app"], p["probe"], p["settings"], settle=10)
    try:
        app.mpris("OpenUri", "s", "file://" + sample)
        time.sleep(30)
        for label, js, wait in (("centered_perchannel", None, 0),
                                ("centered_downmix", "root.uiBridge.setSpectrogramViewMode(0)", 3),
                                ("rolling_downmix", "root.uiBridge.setSpectrogramDisplayMode(0)", 30),
                                ("rolling_perchannel", "root.uiBridge.setSpectrogramViewMode(1)", 30),
                                ("centered_perchannel_meter", "root.uiBridge.setSpectrogramDisplayMode(1); root.uiBridge.setShowLevelMeter(true)", 3),
                                ("waveform", "root.uiBridge.setShowLevelMeter(false); viewerController.setVisualizationMode(1)", 3)):
            if js:
                for part in js.split("; "):
                    app.eval(part)
            time.sleep(wait)
            app.sample(4, label, os.path.join(out, "batches.jsonl"))
    finally:
        app.quit()


if __name__ == "__main__":
    main()
