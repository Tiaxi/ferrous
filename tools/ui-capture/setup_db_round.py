#!/usr/bin/env python3
"""Scans the fixture library into run/template/library.sqlite3 with the binary under test, so
the template always matches the current library schema."""
import glob
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from driver import App, LIB, WORK  # noqa: E402

n = len([p for p in glob.glob(f"{LIB}/**/*", recursive=True) if p.endswith((".flac", ".mp3", ".opus"))])
a = App("setup")
try:
    bus = open(a.pdir + "/bus-before.txt").read()
    if "org.mpris" in bus or "ferrous" in bus.lower():
        raise SystemExit("private bus already has a Ferrous/MPRIS name: refusing")
    print("renderer:", a.ev_soft("GraphicsInfo.api"))
    a.ev(f"root.uiBridge.addLibraryRoot('{LIB}', 'Fixtures')")
    if not a.wait_for(f"root.uiBridge.libraryTrackCount >= {n}", timeout=120):
        raise SystemExit(f"scan did not reach {n} tracks")
    time.sleep(3)
    print("tracks:", a.ev("root.uiBridge.libraryTrackCount"))
finally:
    a.quit()
os.makedirs(f"{WORK}/run/template", exist_ok=True)
for s in ("", "-wal", "-shm"):
    p = f"{WORK}/run/template/library.sqlite3{s}"
    if os.path.exists(p):
        os.remove(p)
    q = f"{WORK}/run/setup/data/ferrous/library.sqlite3{s}"
    if os.path.exists(q):
        shutil.copy(q, f"{WORK}/run/template/")
print("template:", os.listdir(f"{WORK}/run/template"))
