#!/usr/bin/env python3
"""Writes <round>/MOTION.md: an automatic scan of every animation in the source tree (file:line,
kind, property, duration, easing, trigger context).

Usage: motion_inventory.py <round-dir> <source-dir>
"""
import os
import re
import subprocess
import sys

KINDS = r"(Behavior on [\w.]+|NumberAnimation|ColorAnimation|PropertyAnimation|RotationAnimation|SmoothedAnimation|" \
        r"SpringAnimation|OpacityAnimator|XAnimator|YAnimator|ScaleAnimator|RotationAnimator|UniformAnimator|" \
        r"SequentialAnimation|ParallelAnimation|PauseAnimation|Transition|AnchorAnimation)"
START = re.compile(r"^\s*(?:(\w+)\s*:\s*)?" + KINDS + r"\s*\{")


def block(lines, i):
    depth, out = 0, []
    for j in range(i, min(len(lines), i + 60)):
        out.append(lines[j])
        depth += lines[j].count("{") - lines[j].count("}")
        if depth <= 0:
            break
    return " ".join(x.strip() for x in out)


def field(text, name):
    m = re.search(r"\b" + name + r"\s*:\s*([^;{}]+?)(?=\s+\w+(?:\.\w+)?\s*:|\s*}|$)", text)
    return m.group(1).strip() if m else ""


def owner(lines, i):
    # The nearest enclosing objectName or id above the animation.
    for j in range(i - 1, max(-1, i - 80), -1):
        m = re.match(r"\s*(id|objectName)\s*:\s*\"?([\w]+)\"?", lines[j])
        if m:
            return m.group(2)
    return ""


def scan_qml(src):
    rows = []
    root = os.path.join(src, "ui", "qml")
    for dp, _, fs in os.walk(root):
        for f in sorted(fs):
            if not f.endswith(".qml"):
                continue
            p = os.path.join(dp, f)
            lines = open(p, encoding="utf-8").read().splitlines()
            for i, line in enumerate(lines):
                m = START.match(line)
                if not m:
                    continue
                slot, kind = m.group(1) or "", m.group(2)
                b = block(lines, i)
                if kind == "Transition" and not slot:
                    slot = "transition"
                prop = kind[len("Behavior on "):] if kind.startswith("Behavior on") else (
                    field(b, "properties") or field(b, "property"))
                rows.append(dict(file=os.path.relpath(p, src), line=i + 1, kind=kind, slot=slot,
                                 owner=owner(lines, i), prop=prop, duration=field(b, "duration"),
                                 easing=field(b, "easing.type") or field(b, "type"),
                                 cond=field(b, "enabled") or field(b, "running"),
                                 values=" ".join(x for x in (("from " + field(b, "from")) if field(b, "from") else "",
                                                             ("to " + field(b, "to")) if field(b, "to") else "") if x)))
            for i, line in enumerate(lines):
                if re.search(r"\b(ToolTip\.delay|Timer\s*\{)", line):
                    b = block(lines, i) if "Timer" in line else line
                    rows.append(dict(file=os.path.relpath(p, src), line=i + 1, kind="Timer" if "Timer" in line else "ToolTip.delay",
                                     slot="", owner=owner(lines, i) if "Timer" in line else "", prop="",
                                     duration=field(b, "interval") if "Timer" in line else line.split(":", 1)[1].strip(),
                                     easing="", cond=field(b, "running") if "Timer" in line else "", values=""))
    return rows


def scan_cpp(src):
    rows = []
    pat = re.compile(r"QPropertyAnimation|QVariantAnimation|QEasingCurve|QTimeLine|QAbstractAnimation|"
                     r"setInterval\(|QTimer::singleShot|kFade|kDecay|PeakHold|decibelsPerSecond", re.I)
    root = os.path.join(src, "ui", "src")
    for f in sorted(os.listdir(root)):
        if not f.endswith((".cpp", ".h")):
            continue
        for i, line in enumerate(open(os.path.join(root, f), encoding="utf-8", errors="replace")):
            if pat.search(line):
                rows.append((f"ui/src/{f}", i + 1, line.strip()[:140]))
    return rows


def esc(s):
    return s.replace("|", "\\|")


def main():
    rd, src = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    rev = subprocess.run(["git", "-C", src, "log", "-1", "--format=%h"], capture_output=True, text=True).stdout.strip()
    q = sorted(scan_qml(src), key=lambda r: (r["file"], r["line"]))
    c = scan_cpp(src)
    L = [f"# Ferrous motion inventory ({os.path.basename(rd.rstrip('/'))})\n",
         f"Generated from the source at `{rev}` by `tools/ui-capture/motion_inventory.py` on every run, so its "
         "file:line references are current: every QML animation, behavior, transition, timer and tooltip delay, and "
         "the C++ lines that time visual change. The recorded transitions show what the app does. Kirigami units: shortDuration 100 ms, longDuration 200 ms, "
         "veryShortDuration 50 ms, veryLongDuration 400 ms, toolTipDelay 700 ms (Plasma's animation speed scales "
         "all but toolTipDelay). Qt's default easing, where none is set, is Linear. The frame strips in `strips/` "
         "show these running; INDEX.md lists them.\n",
         f"## Automatic scan ({len(q)} QML entries, {len(c)} C++ lines)\n",
         "| file:line | kind | on (id/objectName) | property | duration | easing | from/to | enabled/running |",
         "|---|---|---|---|---|---|---|---|"]
    for r in q:
        kind = r["kind"] + (f" ({r['slot']})" if r["slot"] and r["slot"] != "transition" else "")
        L.append(f"| `{r['file']}:{r['line']}` | {esc(kind)} | {esc(r['owner'])} | {esc(r['prop'])} | {esc(r['duration'])} | "
                 f"{esc(r['easing'])} | {esc(r['values'])} | {esc(r['cond'])[:60]} |")
    L.append("\n### C++ lines that time or shape visual change\n")
    L.append("| file:line | code |\n|---|---|")
    for f, n, t in c:
        L.append(f"| `{f}:{n}` | `{esc(t)}` |")
    open(os.path.join(rd, "MOTION.md"), "w").write("\n".join(L) + "\n")
    print(f"MOTION.md: {len(q)} QML entries, {len(c)} C++ lines")


if __name__ == "__main__":
    main()
