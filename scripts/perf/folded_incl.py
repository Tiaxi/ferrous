#!/usr/bin/env python3
"""Inclusive share of frames matching patterns, per thread.
Usage: folded_incl.py FILE THREAD_PREFIX pattern..."""
import sys
from collections import Counter
path, thread, pats = sys.argv[1], sys.argv[2], sys.argv[3:]
incl, total = Counter(), 0
for line in open(path):
    stack, count = line.rsplit(" ", 1); count = int(count)
    frames = stack.split(";")
    if not frames[0].startswith(thread):
        continue
    total += count
    for p in pats:
        if any(p in f for f in frames):
            incl[p] += count
print(f"thread {thread}: {total} samples")
for p in pats:
    print(f"{incl[p]/max(total,1)*100:6.1f}%  {p}")
