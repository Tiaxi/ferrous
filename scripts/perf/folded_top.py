#!/usr/bin/env python3
"""Summaries of an inferno folded file: per thread, self time per function,
and inclusive time of frames matching given substrings.
Usage: folded_top.py FILE [N] [pattern...]"""
import sys, re
from collections import Counter
path = sys.argv[1]; n = int(sys.argv[2]) if len(sys.argv) > 2 else 25
pats = sys.argv[3:]
threads, leaf, incl = Counter(), Counter(), Counter()
total = 0
def clean(f):
    f = re.sub(r"::h[0-9a-f]{16}$", "", f)
    f = re.sub(r"<[^<>]*>", "<>", f)
    return f[:110]
for line in open(path):
    stack, count = line.rsplit(" ", 1); count = int(count)
    frames = stack.split(";")
    total += count
    threads[re.sub(r"\d", "#", frames[0].split("-")[0] if False else frames[0])] += count
    leaf[clean(frames[-1])] += count
    seen = set()
    for p in pats:
        if any(p in f for f in frames) and p not in seen:
            incl[p] += count; seen.add(p)
print(f"total samples {total}")
print("== threads"); [print(f"{c/total*100:6.1f}%  {t}") for t, c in threads.most_common(12)]
print("== self"); [print(f"{c/total*100:6.1f}%  {t}") for t, c in leaf.most_common(n)]
if pats:
    print("== inclusive"); [print(f"{incl[p]/total*100:6.1f}%  {p}") for p in pats]
