#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keeps the display from sleeping while measurements run, as a media player
does: org.freedesktop.ScreenSaver.Inhibit on the session bus, held until
this process ends. With the display off the compositor sends no frame
callbacks and the app renders nothing, which would void GPU-side numbers.

Usage: hold_display.py [SECONDS]  (default 7200)
"""
import sys
import time

from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
cookie = bus.call_sync("org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver", "Inhibit",
                       GLib.Variant("(ss)", ("ferrous-perf", "Ferrous UI performance measurement")),
                       GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, -1, None).unpack()[0]
print("inhibit cookie", cookie, flush=True)
try:
    time.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 7200)
finally:
    bus.call_sync("org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver", "UnInhibit",
                  GLib.Variant("(u)", (cookie,)), None, Gio.DBusCallFlags.NONE, -1, None)
