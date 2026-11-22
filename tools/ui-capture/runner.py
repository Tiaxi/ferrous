#!/usr/bin/env python3
"""Runs the state table (states.py) against the app: one app session per (session, theme).

Usage: runner.py [--only AREA[,AREA...]] [--sessions S1,S2] [--themes dark,light] [--focus SCOPES] [--list]

Each STATES line is (session, area, state, steps, description). Steps are separated by
" | "; the line ends with a grab named <area>_<state>_<theme>_<scale>x_<W>x<H>.png unless
the state starts with "_" (setup) or the area is "motion" (a frame strip, see `strip`).
With --only, a session runs its setup lines and the lines of the given areas only.
With --focus, a session runs its lines up to the last one any of the scopes' packs reads (focus_select.py).
Step verbs: see Runner.step_* below.
"""
import argparse
import glob
import json
import os
import re
import shutil
import sys
import time
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shotlib import Session, session_suffix, KEY, SHIFT, CTRL, ALT, FIND  # noqa: E402
import states  # noqa: E402

MODS = {"shift": SHIFT, "ctrl": CTRL, "alt": ALT}


class Runner:
    def __init__(self, s):
        self.s = s
        self.win = ""
        self.verb_times = {}  # verb (macro NAME for macros) -> [count, seconds], top-level steps only
        self._depth = 0

    # --- targets: NAME, NAME#k, NAME@fx,fy, NAME#k@fx,fy, xy:X,Y, menubar:I, menuitem:M:I
    def named_rect(self, name):
        """@NAME targets: a JS expression in states.TARGETS that returns the item."""
        r = self.s.ev(f"(function(){{var it=({states.TARGETS[name]});if(!it)return '';var p=it.mapToItem(null,0,0);"
                      f"return p.x+' '+p.y+' '+it.width+' '+it.height}})()")
        if not r:
            raise LookupError(f"no target @{name}")
        return tuple(map(float, r.split()))

    def point(self, spec):
        s = self.s
        if spec.startswith("@"):
            name, _, frac = spec[1:].partition("@")
            x, y, w, h = self.named_rect(name)
            fx, fy = (map(float, frac.split(","))) if frac else (0.5, 0.5)
            return x + w * fx, y + h * fy
        if spec.startswith("xy:"):
            x, y = spec[3:].split(",")
            return float(x), float(y)
        if spec.startswith("menubar:"):
            i = int(spec.split(":")[1])
            r = s.ev(f"(function(){{var it=root.menuBar.itemAt({i});var p=it.mapToItem(null,0,0);"
                     f"return p.x+' '+p.y+' '+it.width+' '+it.height}})()")
            x, y, w, h = map(float, r.split())
            return x + w / 2, y + h / 2
        if spec.startswith("menuitem:"):
            _, m, i = spec.split(":")
            r = s.ev(f"(function(){{var m=root.menuBar.menus[{m}];var k={i};if(k<0)k=m.count+k;var it=m.itemAt(k);"
                     f"var p={states.MAIN_POINT_JS}(it);return p.x+' '+p.y+' '+it.width+' '+it.height}})()")
            x, y, w, h = map(float, r.split())
            return x + w / 2, y + h / 2
        m = re.match(r"^([A-Za-z0-9_]+)(?:#(\d+))?(?:@([-\d.]+),([-\d.]+))?$", spec)
        if not m:
            raise ValueError(f"bad target {spec}")
        name, k, fx, fy = m.group(1), m.group(2), m.group(3), m.group(4)
        rs = [] if self.win else s.visible_rects(name)
        if k is not None:
            if len(rs) <= int(k):
                rs = s.rects(name)  # e.g. items under a fading plate: no visibility filter
            if len(rs) <= int(k):
                raise LookupError(f"no {name}#{k}")
            _, x, y, w, h = rs[int(k)]
            r = (x, y, w, h)
        elif rs:
            r = tuple(rs[0][1:])
        else:
            r = s.rect(name)
            if not r:
                raise LookupError(f"no item {name}")
        fx = float(fx) if fx is not None else 0.5
        fy = float(fy) if fy is not None else 0.5
        return r[0] + r[2] * fx, r[1] + r[3] * fy

    def rect_spec(self, spec):
        """Crop for a strip: '-' (whole window), 'X,Y,W,H', or NAME[:fx,fy,fw,fh]."""
        if spec in ("-", ""):
            return None
        if re.match(r"^[\d.,]+$", spec):
            return [int(float(v)) for v in spec.split(",")]
        box = None
        for part in spec.split("+"):  # union of NAME[:fx,fy,fw,fh] (or @TARGET[:...])
            name, _, frac = part.partition(":")
            r = self.named_rect(name[1:]) if name.startswith("@") else self.s.rect(name)
            if not r:
                continue
            if frac:
                fx, fy, fw, fh = map(float, frac.split(","))
                r = (r[0] + r[2] * fx, r[1] + r[3] * fy, r[2] * fw, r[3] * fh)
            if box is None:
                box = list(r)
            else:
                x0, y0 = min(box[0], r[0]), min(box[1], r[1])
                x1, y1 = max(box[0] + box[2], r[0] + r[2]), max(box[1] + box[3], r[1] + r[3])
                box = [x0, y0, x1 - x0, y1 - y0]
        return None if box is None else [int(v) for v in box]

    def at(self):
        return ("@" + self.win) if self.win else ""

    # --- steps
    def run_steps(self, text):
        for step in [t.strip() for t in text.split(" | ") if t.strip()]:
            verb, _, rest = step.partition(" ")
            t0 = time.time()
            self._depth += 1
            try:
                getattr(self, "step_" + verb.replace("-", "_"))(rest.strip())
            finally:
                self._depth -= 1
                if not self._depth:  # (a strip's action steps count as the strip's)
                    key = "macro " + rest.split()[0] if verb == "macro" and rest.split() else verb
                    c = self.verb_times.setdefault(key, [0, 0.0])
                    c[0] += 1
                    c[1] += time.time() - t0

    def step_wait(self, a):
        time.sleep(float(a))

    def step_away(self, a):
        self.s.away()

    def step_js(self, a):
        r = self.s.ev(a)
        return r

    def step_expect(self, a):
        """expect NAME[:ARG]: the state shows what it claims (states.EXPECT: a check evaluated in the app, true or a
        reason), or it fails: a state that silently shows something else (e.g. a collapsed library under
        "expanded-playing") must fail the run."""
        name, _, arg = a.strip().partition(":")
        if name not in states.EXPECT:
            raise ValueError(f"expect: no check named {name}")
        end = time.time() + 1.5  # a state settles asynchronously (rows folding away, a viewer opening)
        while True:
            res = self.s.ev(states.EXPECT[name].replace("{arg}", arg))
            if res is True or time.time() > end:
                break
            time.sleep(0.1)
        if res is not True and os.environ.get("SHOT_NO_EXPECT") == "1":  # diagnostic: see what the state shows
            print(f"  WARNING expect {a.strip()}: {res}", flush=True)
        elif res is not True:
            raise RuntimeError(f"expect {a.strip()}: {res if res not in (None, False, '') else 'not shown'}")

    def step_shown(self, a):
        """shown NAME: the item exists, is visible and fully opaque (with its ancestors), or the state fails."""
        js = (f"(function(){{var it={self.s.find_js(a.strip())};if(!it)return 'missing';var o=1;"
              f"for(var p=it;p;p=p.parent){{if(!p.visible)return 'hidden';o*=p.opacity}}return o>0.99?'ok':'opacity '+o}})()")
        res = self.s.ev(js)
        if res != "ok":
            raise RuntimeError(f"shown {a.strip()}: {res}")

    def step_until(self, a):
        if not self.s.a.wait_for(a, timeout=20):  # loudly: the state is not the one its caption names
            raise RuntimeError(f"until: not true within 20 s: {a[:120]}")

    def settle_or(self, t):
        """Popup transitions done; in a separate window (no probe popups there), a plain wait."""
        if self.win:
            time.sleep(t)
        else:
            self.s.settle(timeout=t)

    def step_settle(self, a):
        """settle [MAX]: until no popup is opening or closing."""
        self.settle_or(float(a or 1.0))

    def step_tip(self, a):
        """tip [MAX]: until a tooltip is fully shown."""
        self.s.tip(timeout=float(a or 1.4))

    def step_ready(self, a):
        """ready [MAX]: until the playing track's analysis is complete and the visible plots have it."""
        self.s.ready(timeout=float(a or 6))

    def step_waitpopup(self, a):
        """waitpopup CLASSPART [MAX]: until a popup whose QML type name contains CLASSPART is fully open
        (for popups that open asynchronously, e.g. after the backend answers)."""
        parts = a.split()
        end = time.time() + (float(parts[1]) if len(parts) > 1 else 2.0)
        while time.time() < end:
            dws = [] if self.win else self.s.dialog_windows()
            pops = self.s.popups(self.win) + [p for w in dws for p in self.s.popups(w["cls"])]
            if any(parts[0] in p["cls"] and p["opened"] for p in pops):
                return
            # (or the dialog has become a window of its own: DialogWindow, the class named the same)
            if any(parts[0] in w["cls"] for w in dws):
                time.sleep(0.2)  # mapped, activated and drawn by KWin
                return
            time.sleep(0.04)

    def step_window(self, a):
        self.win = a

    def point_in_dialog(self, spec):
        """NAME[#k][@fx,fy] inside a shown dialog window (DialogWindow), as (cls, x, y), or None."""
        dw = None if self.win else self.s.dialog_window()
        m = re.match(r"^([A-Za-z0-9_]+)(?:#(\d+))?(?:@([-\d.]+),([-\d.]+))?$", spec)
        if not dw or not m:
            return None
        js = ("(function(n){var o=[];var r=contentItem;while(r.parent)r=r.parent;var s=[r];while(s.length){"
              "var it=s.shift();if(!it.visible)continue;if(it.objectName===n){var p=it.mapToItem(null,0,0);"
              "o.push([p.x,p.y,it.width,it.height])}var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}"
              f"o.sort(function(a,b){{return a[1]-b[1]||a[0]-b[0]}});return JSON.stringify(o)}})('{m.group(1)}')")
        r = self.s.cmd(f"eval@{dw['cls']} {js}")
        try:
            rects = json.loads(json.loads(r)[0])
        except (ValueError, TypeError, IndexError):
            return None
        k = int(m.group(2) or 0)
        if len(rects) <= k:
            return None
        x, y, w, h = rects[k]
        fx = float(m.group(3)) if m.group(3) else 0.5
        fy = float(m.group(4)) if m.group(4) else 0.5
        return dw["cls"], x + w * fx, y + h * fy

    def _mouse(self, verb, a):
        parts = a.split()
        try:
            x, y = self.point(parts[0])
            at = self.at()
        except LookupError:
            found = self.point_in_dialog(parts[0])  # e.g. a page's control inside the Configure window
            if not found:
                raise
            cls, x, y = found
            at = "@" + cls
        self.s.cmd(f"{verb}{at} {x:.1f} {y:.1f}")
        if len(parts) > 1:
            time.sleep(float(parts[1]))

    def step_move(self, a):
        self._mouse("move", a)

    def step_hover(self, a):
        """hover TARGET [WAIT]: a WAIT of 1.2 s or more means "until its tooltip shows" (WAIT at most)."""
        parts = a.split()
        self._mouse("move", parts[0])
        w = float(parts[1]) if len(parts) > 1 else 0.6
        if w >= 1.2 and not self.win:
            if not self.s.tip(timeout=w):  # the state is of its tooltip: without one it is not that state
                raise RuntimeError(f"no tooltip fully shown within {w} s of hovering {parts[0]}")
        else:
            time.sleep(w)

    def step_click(self, a):
        self._mouse("click", a)

    def step_rclick(self, a):
        self._mouse("rclick", a)
        if len(a.split()) == 1:
            self.settle_or(0.7)

    def step_dblclick(self, a):
        self._mouse("dblclick", a)

    def step_press(self, a):
        self._mouse("press", a)

    def step_release(self, a):
        self._mouse("release", a)

    def step_dmove(self, a):
        self._mouse("dmove", a)

    # the middle button (a middle-drag pans a zoomed plot; a middle click starts autoscroll)
    def step_mpress(self, a):
        self._mouse("mpress", a)

    def step_mrelease(self, a):
        self._mouse("mrelease", a)

    def step_mclick(self, a):
        self._mouse("mclick", a)

    def step_mdmove(self, a):
        self._mouse("mdmove", a)

    def step_leave(self, a):
        """leave [WAIT]: the pointer leaves the window (a Leave with no position, as the compositor sends it)."""
        self.s.cmd(f"leave{self.at()}")
        if a:
            time.sleep(float(a))

    def step_wheel(self, a):
        # wheel TARGET DELTA [COUNT]
        parts = a.split()
        x, y = self.point(parts[0])
        for _ in range(int(parts[2]) if len(parts) > 2 else 1):
            self.s.cmd(f"wheel{self.at()} {x:.0f} {y:.0f} {parts[1]}")
            time.sleep(0.25)

    def step_key(self, a):
        self._key("key", a)

    def step_kkey(self, a):
        """kkey KEY: a real key through KWin (kwin_input.KEYS), for a shortcut that must take it from the focused
        item: the probe's keys go to the window directly, past Qt's shortcut map, and the item gets them."""
        self.s.kwin_key(a)
        time.sleep(0.08)

    def step_keydown(self, a):
        """keydown KEY: a key held (its press alone) until keyup KEY."""
        self._key("keydown", a)

    def step_keyup(self, a):
        self._key("keyup", a)

    def _key(self, verb, a):
        parts = a.split()
        mods = 0
        for m in parts[1:]:
            mods |= MODS[m]
        code = KEY.get(parts[0])
        if code is None:
            code = ord(parts[0].upper())
        self.s.cmd(f"{verb}{self.at()} {code} {mods}")
        time.sleep(0.08)

    def step_type(self, a):
        """type TEXT: each character as a real key press with its text; "\\s" is a space (steps are trimmed)."""
        for ch in a.replace("\\s", " "):
            self.s.cmd(f"key{self.at()} {ord(ch.upper())} 0 {'%20' if ch == ' ' else ch}")
            time.sleep(0.08)

    def step_esc(self, a):
        for _ in range(int(a or 1)):
            at = self.at()
            dw = None if self.win else self.s.dialog_window()
            if dw:  # a dialog that is its own window gets the key
                at = "@" + dw["cls"]
                if not any(p["opened"] and p["rect"][2] > 1 for p in self.s.popups(dw["cls"])):
                    # Nothing open inside it: the window's Cancel shortcut would reject it. The probe's key
                    # events reach the window directly, past Qt's shortcut map, so close() it instead: the
                    # same onClosing -> reject() path.
                    self.s.cmd(f"eval@{dw['cls']} close()")
                    for _ in range(25):
                        if not self.s.dialog_window():
                            break
                        time.sleep(0.04)
                    continue
            self.s.cmd(f"key{at} {KEY['Escape']} 0")
            self.settle_or(0.35)

    def step_menu(self, a):
        """menu I: the menu bar's menu I, opened by a click, and that menu is what shows, or the state fails (a
        state named for one menu once showed another, the one under the compositor's pointer)."""
        x, y = self.point(f"menubar:{a}")
        self.s.click(x, y)
        self.settle_or(0.6)
        shown = ("(function(){var m=root.menuBar.menus,o=[];for(var i=0;i<m.length;i++){if(m[i].visible)o.push(i)}"
                 "return o.join()})()")
        end = time.time() + 1.0  # (a menu's window maps asynchronously)
        while True:
            got = str(self.s.ev(shown))
            # (and KWin shows its window: a key sent before that goes to the main window, past the menu)
            if got == str(int(a)) and self.popup_open(menu=True, win=""):
                return
            if time.time() > end:
                raise RuntimeError(f"menu {a}: the menus shown are [{got}], not that one" if got != str(int(a))
                                   else f"menu {a}: its window is not shown")
            time.sleep(0.05)

    def step_queue(self, a):
        self.s.load_queue()

    def step_play(self, a):
        # play NEEDLE [seek|-] [wait]
        parts = a.split()
        seek = None if len(parts) < 2 or parts[1] == "-" else float(parts[1])
        self.s.play(parts[0].replace("_", " "), seek=seek, wait=float(parts[2]) if len(parts) > 2 else 0)

    def step_start(self, a):
        """start NEEDLE: Play on the queue row whose path contains NEEDLE, without waiting."""
        i = self.s.queue_index(a.replace("_", " "))
        self.s.ev(f"root.uiBridge.playAt({i})")

    def step_focus(self, a):
        self.s.ev(f"{FIND}('{a}').forceActiveFocus(Qt.TabFocusReason)")
        time.sleep(0.15)

    def step_unfocus(self, a):
        self.s.ev("root.contentItem.forceActiveFocus()")

    def step_focusdefault(self, a):
        dw = None if self.win else self.s.dialog_window()
        r = self.s.focus_default_button(win=dw["cls"] if dw else "")
        self.last_focus = r
        time.sleep(0.15)

    def step_macro(self, a):
        name, _, rest = a.partition(" ")
        self.s.begin_composites()
        states.MACROS[name](self, rest.strip())

    def step_strip(self, a):
        # strip DURATION PRE CROP [win=NAME] [every=N] [scale=F] :: action steps separated by " ; "
        head, _, action = a.partition("::")
        dur, pre, crop, *opts = head.split()
        # dialog=TITLE: record that dialog's own window when it is one (DialogWindow: About, Configure), else
        # the main window as before (a dialog that is an in-window popup).
        dial = [o for o in opts if o.startswith("dialog=")]
        if dial:
            opts = [o for o in opts if not o.startswith("dialog=")]
            part = dial[0][7:]
            ws = self.s.windows()
            # a window's objectName (exact: "aboutDialog" is not "About KDE") or a part of its title
            if any(part == w.get("name") or part in w["title"] for w in ws[1:]):
                opts.append(f"win={part}")
                crop = "-"
        if getattr(self, "no_record", False):
            self.run_steps(action.replace(" ; ", " | "))
            return
        rect = None if any(o.startswith("win=") for o in opts) and re.match(r"^[A-Za-z]", crop) else self.rect_spec(crop)
        name = self.line_state
        self.s.strip(name, lambda: self.run_steps(action.replace(" ; ", " | ")), duration=float(dur),
                     pre=float(pre), crop=rect, desc=self.line_desc, opts=" ".join(opts))

    def step_repaint(self, a):
        """repaint: the window renders a frame now (a strip then shows a held state at this time)."""
        self.s.cmd(f"update{self.at()}")

    def popup_open(self, win=None, menu=False):
        """A menu, dialog or other popup (not just a tooltip) is fully open in the window; with `menu`, a menu (an
        open search palette under it is not the state). Zero-size popups don't count: DialogWindow puts an empty
        modal popup (dialogWindowShortcutBlocker) in the main window while a dialog window is shown, to block its
        shortcuts; it is not the dialog. A menu in a window of its own counts once KWin shows that window."""
        return any(p["opened"] and p.get("exposed", True) and not p["tooltip"]
                   and p["rect"][2] > 1 and p["rect"][3] > 1
                   and p.get("name") != "dialogWindowShortcutBlocker"
                   and (not menu or "Menu" in p["cls"])
                   for p in self.s.popups(self.win if win is None else win))


def run_session(name, tag, lines, only, cut=None):
    kw = dict(states.SESSIONS[name])
    kw.pop("themes", None)
    t_start = time.time()
    s = Session(name, tag, **kw)
    r = Runner(s)
    failures = []
    timing = dict(session=name, theme=tag, startup=round(s.startup_s, 2), lines={}, line_s=[])
    if only or cut:  # (part of the session: its total is not the session's time)
        timing.update(part=cut or True)
    try:
        for (_, area, state, steps, desc) in lines:
            setup = state.startswith("_")
            if only and not setup and area not in only:
                continue
            # Strips are exported dark only, except where colour matters: in light, a strip's
            # action still runs (later states may depend on it), without recording.
            r.no_record = area == "motion" and tag != "dark" and state not in states.STRIP_BOTH_THEMES
            t_line = time.time()
            r.win = ""
            r.pending_measures = []
            r.line_state = state
            r.line_desc = desc
            try:
                r.run_steps(steps)
                t_shot = time.time()
                if not setup and area != "motion":
                    dialog_win = None
                    menu_state = ((area == "menu" and state not in states.VERIFY_OPEN_EXEMPT)
                                  or (area, state) in states.VERIFY_MENU_STATES)
                    if (area in states.VERIFY_OPEN_AREAS and state not in states.VERIFY_OPEN_EXEMPT) or menu_state:
                        # A dialog that is its own top-level window (DialogWindow: Configure, About) is the
                        # state when it is shown: it is grabbed, with any popup inside it (the folder-name
                        # dialog, the Remove confirmation). Otherwise a real popup must be open in the window.
                        dialog_win = None if (r.win or area != "dialog") else s.dialog_window(timeout=0.3)
                        if dialog_win:
                            for _ in range(25):  # popups inside the dialog window: their transitions done
                                if all(p["opened"] for p in s.popups(dialog_win["cls"])):
                                    break
                                time.sleep(0.04)
                            time.sleep(0.2)  # mapped, activated and drawn by KWin
                        else:
                            if not r.popup_open(menu=menu_state):
                                r.settle_or(1.0)
                            if not r.popup_open(menu=menu_state):  # a capture failure, not a shot of something else
                                raise RuntimeError("no dialog window, menu or popup shown at grab time")
                    if not r.win and not dialog_win:  # no popup (a tooltip) caught mid-fade: it settles, or fails
                        moving = lambda: [p for p in s.popups() if not p["opened"] or 0.02 < p["opacity"] < 0.99]
                        if moving():
                            r.settle_or(1.0)
                            left = moving()
                            if left:
                                raise RuntimeError("a popup is still opening or closing at grab time: " + ", ".join(
                                    f"{p.get('cls', '?')} opacity {p['opacity']:.2f}" for p in left))
                    s.grab(area, state, r.line_desc, win=r.win, subject=states.AREA_SUBJECT.get(area),
                           dialog_window=dialog_win)
                    if area == "dialog":  # the shot is of the dialog the state names (DIALOG_EXPECT), or it fails
                        s.check_dialog(state, f"{area}_{state}_{s.suffix}")
                    if area in ("dialog", "viewer", "search"):  # boxes to paint over: the diagnostics log
                        states.private_sidecar(r, f"{area}_{state}_{s.suffix}")
                    if getattr(r, "pending_measures", None):  # contrast measurements of this shot
                        # measured again now, on the frame just grabbed (an item can move or change between)
                        fresh = []
                        for m in r.pending_measures:
                            try:
                                n = states.measure_now(r, m["label"], m["kind"], m["_expr"])
                                n["moved_since_step"] = n.get("rect") != m.get("rect")
                                fresh.append(n)
                            except Exception:
                                fresh.append(m)
                        r.pending_measures = [{k: v for k, v in m.items() if k != "_expr"} for m in fresh]
                        cd = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "contrast")
                        os.makedirs(cd, exist_ok=True)
                        with open(os.path.join(cd, f"{area}_{state}_{s.suffix}.json"), "w") as f:
                            json.dump(dict(shot=f"{area}_{state}_{s.suffix}.png", theme=tag, scale=s.scale,
                                           measures=r.pending_measures), f, indent=1)
                        r.pending_measures = []
                    c = r.verb_times.setdefault("(grab and its checks)", [0, 0.0])
                    c[0] += 1
                    c[1] += time.time() - t_shot
            except Exception as e:  # keep going: record what failed
                failures.append(f"{name}/{tag} {area} {state}: {e}")
                print(f"  !! {area}_{state}: {e}", flush=True)
                traceback.print_exc(limit=2)
                try:
                    r.win = ""
                    r.step_esc("2")
                except Exception:
                    pass
            timing["lines"][f"{area}/{state}"] = round(time.time() - t_line, 2)
            timing["line_s"].append(round(time.time() - t_line, 2))
    finally:
        s.save_index(name)
        t_quit = time.time()
        s.quit()
        timing.update(quit=round(time.time() - t_quit, 2), total=round(time.time() - t_start, 2),
                      grabs=round(getattr(s, "t_grab", 0), 2), layout=round(getattr(s, "t_layout", 0), 2), strip_save=round(getattr(s, "t_stripsave", 0), 2),
                      verbs={k: [n, round(t, 2)] for k, (n, t) in sorted(r.verb_times.items(), key=lambda kv: -kv[1][1])})
        td = os.path.join(os.path.dirname(os.environ["SHOT_OUT"].rstrip("/")), "timings")
        os.makedirs(td, exist_ok=True)
        with open(os.path.join(td, f"{name}_{tag}.json"), "w") as f:
            json.dump(timing, f, indent=1)
    return failures


# Per (session, theme) seconds of the last whole run of each session in the work folder, for the schedule of the
# next run, whatever its out folder (a new out folder has no rounds beside it to take them from).
HISTORY = "session-times.json"


def load_json(path):
    with open(path) as f:
        return json.load(f)


def history_path():
    from driver import WORK
    return os.path.join(WORK, "run", HISTORY)


def record_history(rd, jobs, path=None):
    """Adds the times of the jobs just run (this round's timings: the total and each line's) to the work folder's
    history. Only whole sessions: a part of one would schedule the next whole run on its shorter time."""
    path = path or history_path()
    try:
        hist = load_json(path)
    except (OSError, ValueError):
        hist = {}
    for name, tag, cut in jobs:
        if cut:
            continue
        try:
            t = load_json(os.path.join(rd, "timings", f"{name}_{tag}.json"))
            if not t.get("part"):
                hist[f"{name}:{tag}"] = dict(total=t["total"], lines=t.get("line_s", []))
        except (OSError, ValueError, KeyError):
            pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(hist, f, indent=1, sort_keys=True)
    os.replace(tmp, path)


def estimates(rd, history=None):
    """A function of a job (session, theme, lines or None for all): its seconds, from this round's last timings of
    the whole session, else the work folder's history (the last whole run of each session), else the most recent
    other round's, else defaults (measured on a full capture). A part of a session is the whole less the lines after
    it, where those were timed."""
    defaults = dict(empty=7, main=86, playing=79, transport=36, statusbar=52, menus=20, dialogs=55, viewers=52,
                    focus=20, roots=7, motion=88, narrow=18, hidpi=5, bigfont=96, icons=12, animoff=7, contrast=8,
                    queue=36, density=12)
    defaults.update({"motion-ui": 32, "narrow-empty": 2, "narrow-bigfont": 32, "wide-bigfont": 9, "tall-bigfont": 3,
                     "firstplay-noart": 23, "firstrun-defaults": 2, "icon-noart24": 8})
    light_motion = {"motion": 0.66, "motion-ui": 0.6}
    found = {}

    def take(td):
        for p in glob.glob(os.path.join(td, "*.json")):
            try:
                t = load_json(p)
                if not t.get("part"):
                    found.setdefault((t["session"], t["theme"]), (float(t["total"]), t.get("line_s", [])))
            except (OSError, ValueError, KeyError, TypeError, AttributeError):  # (not a session's timings)
                pass

    take(os.path.join(rd, "timings"))
    try:
        for k, v in load_json(history or history_path()).items():
            name, _, tag = k.rpartition(":")
            found.setdefault((name, tag), (float(v["total"]), v.get("lines", [])))
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        pass
    for td in sorted(glob.glob(os.path.join(os.path.dirname(rd), "*", "timings")), key=os.path.getmtime, reverse=True):
        take(td)

    def est(job):
        name, tag, cut = job
        total, lines = found.get((name, tag), (
            defaults.get(name, 30) * (light_motion.get(name, 1) if tag == "light" else 1), []))
        if not cut:
            return total
        n = sum(1 for line in states.STATES if line[0] == name)
        if len(lines) == n:
            return total - sum(lines[cut:])
        return total * min(1.0, cut / max(1, n))

    return est


def encode_job(job):
    name, tag, cut = job
    return f"{name}:{tag}" + (f":{cut}" if cut else "")


def decode_job(text):
    name, tag, *cut = text.split(":")
    return name, tag, int(cut[0]) if cut else None


def claim(folder, job):
    """Takes a job of the shared queue; False when another worker has it."""
    try:
        os.close(os.open(os.path.join(folder, f"{job[0]}_{job[1]}"), os.O_CREAT | os.O_EXCL | os.O_WRONLY))
    except FileExistsError:
        return False
    return True


def mark_done(folder, job):
    with open(os.path.join(folder, f"{job[0]}_{job[1]}.done"), "w"):
        pass


def unfinished(folder, jobs):
    """The jobs no worker finished: not started, or cut off (the worker died)."""
    return [j for j in jobs if not os.path.exists(os.path.join(folder, f"{j[0]}_{j[1]}.done"))]


def focus_jobs(spec, jobs):
    """The jobs the focus scopes `spec` names (focus.resolve) need together, each cut after the last line any of
    their packs reads; whole sessions, loudly, where that cannot be told."""
    import focus
    import focus_select
    suffix = lambda tag, kw: session_suffix(tag, kw.get("scale", 1), kw.get("size", (1600, 1000)), kw.get("suffix_extra", ""))
    names = focus.resolve(spec)
    need, problems = focus_select.prefixes([focus.SCOPES[nm] for nm in names], [j[:2] for j in jobs], states,
                                           suffix)
    if problems:
        print(f"!! focus {','.join(names)}: whole sessions are shot, as what they need cannot be told: "
              + "; ".join(problems[:5]), flush=True)
        return jobs
    out = []
    for name, tag, _ in jobs:
        n, total = need.get((name, tag), 0), sum(1 for line in states.STATES if line[0] == name)
        print(f"focus: {name}/{tag}: " + (f"{n} of {total} lines" if n else "not needed"), flush=True)
        if n:
            out.append((name, tag, n if n < total else None))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    ap.add_argument("--sessions", default="")
    ap.add_argument("--themes", default="")
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--focus", default="", help="shoot only what the packs of these focus scopes read (focus.resolve)")
    ap.add_argument("--jobs", default="", help="internal: session:theme[:lines],... the queue of one worker")
    ap.add_argument("--claims", default="", help="internal: the folder where workers claim the queue's jobs")
    ap.add_argument("--list", action="store_true")
    o = ap.parse_args()
    only = [x for x in o.only.split(",") if x]
    want = [x for x in o.sessions.split(",") if x]
    order = []
    for line in states.STATES:
        if line[0] not in order:
            order.append(line[0])
    if o.list:
        for line in states.STATES:
            print("\t".join(line[:3]) + "\t" + line[4])
        return
    rd = os.path.dirname(os.environ["SHOT_OUT"].rstrip("/"))
    if o.jobs:
        jobs = [decode_job(j) for j in o.jobs.split(",")]
    else:
        jobs = []
        for name in order:
            if want and name not in want:
                continue
            lines = [l for l in states.STATES if l[0] == name]
            if only and not any(l[1] in only for l in lines):
                continue
            themes = states.SESSIONS[name].get("themes", ("dark", "light"))
            if o.themes:
                themes = [t for t in themes if t in o.themes.split(",")]
            jobs += [(name, t, None) for t in themes]
        # (--only and --themes pick for themselves; SHOT_FOCUS_WHOLE=1 shoots whole sessions, to check a cut)
        if o.focus and not only and not o.themes and os.environ.get("SHOT_FOCUS_WHOLE") != "1":
            jobs = focus_jobs(o.focus, jobs)
    if not o.jobs and o.workers > 1 and len(jobs) > 1:
        import subprocess
        est = estimates(rd)
        jobs.sort(key=lambda j: -est(j))  # longest first; a free worker takes the next
        claims = os.path.join(rd, "claims")
        shutil.rmtree(claims, ignore_errors=True)
        os.makedirs(claims)
        n_workers = min(o.workers, len(jobs))
        total = sum(est(j) for j in jobs)
        print(f"{n_workers} workers, {len(jobs)} sessions, ~{max(total / n_workers, est(jobs[0])):.0f} s: "
              + " ".join(f"{a}/{b}" for a, b, _ in jobs), flush=True)
        args = [sys.executable, os.path.abspath(__file__), "--jobs", ",".join(map(encode_job, jobs)), "--claims", claims]
        if only:
            args += ["--only", ",".join(only)]
        procs = []
        t0 = time.time()
        for i in range(n_workers):
            # Per round and worker: run dirs and sockets. Per worker alone: the apps' caches (driver.app_caches).
            env = dict(os.environ, SHOT_RUN_PREFIX=f"{os.path.basename(rd)}-w{i}", SHOT_CACHE_SLOT=f"w{i}")
            log = open(os.path.join(rd, f"worker{i}.log"), "w")
            procs.append((subprocess.Popen(["nice", "-n", "10"] + args, env=env, stdout=log, stderr=subprocess.STDOUT), log))
        rc = 0
        for p, log in procs:
            rc |= p.wait()
            log.close()
        print(f"{len(procs)} workers done in {time.time() - t0:.0f} s (exit {rc})", flush=True)
        lost = unfinished(claims, jobs)
        record_failures(rd, [f"{name}/{tag}: session not finished: its worker ended (exit {rc}); see worker*.log"
                             for name, tag, _ in lost])
        for name, tag, _ in lost:
            print(f"!! {name}/{tag}: not finished", flush=True)
        shutil.rmtree(claims, ignore_errors=True)
        if not only:
            record_history(rd, [j for j in jobs if j not in lost])
        return
    n_failed = 0
    for job in jobs:
        if o.claims and not claim(o.claims, job):
            continue
        name, tag, cut = job
        lines = [l for l in states.STATES if l[0] == name][:cut]
        t0 = time.time()
        try:
            failures = run_session(name, tag, lines, only, cut)
        except Exception as e:
            failures = [f"{name}/{tag}: session failed: {e}"]
            traceback.print_exc()
        record_failures(rd, failures)  # (at once: the job counts as finished only with its failures written)
        n_failed += len(failures)
        if o.claims:
            mark_done(o.claims, job)
        print(f"== {name}/{tag} done in {time.time() - t0:.0f} s" + (f" (to line {cut})" if cut else ""), flush=True)
    print(f"{n_failed} failures (see {os.path.join(rd, 'failures.txt')})")
    if not o.jobs and not only:  # (a worker's parent records its workers' jobs)
        record_history(rd, jobs)


def record_failures(rd, failures):
    with open(os.path.join(rd, "failures.txt"), "a") as f:
        for x in failures:
            f.write(x + "\n")


if __name__ == "__main__":
    main()
