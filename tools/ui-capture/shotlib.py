#!/usr/bin/env python3
"""Shared helpers for the capture sessions (driver.App + shotprobe.so)."""
import glob
import json
import re
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from driver import App, WORK, LIB, kwin_output_size  # noqa: E402
import popup_frames  # noqa: E402

TEMPLATE_DB = f"{WORK}/run/template/library.sqlite3"
STRIP_GEOMETRY = ("sidebarPane", "spectrogramPane", "transportBar", "nowPlayingPanel", "libraryPane", "mainQueuePane", "statusBar",
                  "visualizationModeSwitch")
LIB2 = os.path.join(WORK, "fixtures", "library2")
OUT = os.environ["SHOT_OUT"]
STRIPS_RAW = os.path.join(os.path.dirname(OUT.rstrip("/")), "strips-raw")
SCHEMES = {"dark": "BreezeDark", "light": "BreezeLight"}

KEY = dict(Escape=16777216, Tab=16777217, Backtab=16777218, Backspace=16777219, Return=16777220,
           Enter=16777221, Delete=16777223, Home=16777232, End=16777233, Left=16777234, Up=16777235,
           Right=16777236, Down=16777237, PageUp=16777238, PageDown=16777239, Space=32, F=70, A=65,
           Q=81, O=79, Z=90, N=78, Menu=16777301, F10=16777273)
SHIFT, CTRL, ALT = 0x02000000, 0x04000000, 0x08000000

# Finds a visible item by objectName anywhere in the window (overlay included).
FIND = ("(function(n){var s=[root.Overlay.overlay.parent];while(s.length){var it=s.shift();"
        "if(it.objectName===n&&it.visible)return it;var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}"
        "return null})")
# Finds every visible item whose objectName starts with a prefix, in tree order.
FINDALL = ("(function(n){var o=[];var s=[root.Overlay.overlay.parent];while(s.length){var it=s.shift();"
           "if(!it.visible)continue;if(String(it.objectName).indexOf(n)===0)o.push(it);var c=it.children;"
           "for(var i=0;i<c.length;i++)s.push(c[i]);}return o})")

QUEUE_ALBUMS = ("The Quiet Meridian/*Small Hours", "Lumen Drift/*Afterimage", "Mira Calloway/*Harbour Lights",
                "Orchid Static/*Static Bloom", "Orchid Static/*Loose Ends", "Lumen Drift/*Dust Motes",
                "Northbound Kites/*Kites", "Halvorsen Trio/*Winter Suite", "Unsorted",
                "Kōji Hanamura/*Sakura Tape Loops")


def album_paths(*patterns):
    out = []
    for d in patterns:
        out += sorted(p for p in glob.glob(f"{LIB}/{d}/*") if not p.endswith(".jpg"))
    return out


class Session:
    def __init__(self, name, tag, scale=1, size=(1600, 1000), font_pt=10, db=True, settings_extra="",
                 suffix_extra="", anim_off=False, app_defaults=False):
        self.tag = tag
        self.scale = scale
        self.size = size
        self.index = []
        self.composite_sources = []  # raw_grab/source_shot in the running macro (composite_evidence)
        self.composite_no_app_pixels = False  # the running macro draws images without app pixels
        self.suffix = f"{tag}_{scale}x_{size[0]}x{size[1]}" + suffix_extra
        t_app = time.time()
        prefix = os.environ.get("SHOT_RUN_PREFIX", "run")
        self.a = App(f"{prefix}-{name}-{tag}", scheme=SCHEMES[tag], scale=scale, size=size,
                     db_from=TEMPLATE_DB if db else None, font_pt=font_pt, settings_extra=settings_extra,
                     anim_off=anim_off, app_defaults=app_defaults)
        t0 = time.time()
        cond = "root.visible && root.uiBridge.connected" + (" && root.uiBridge.libraryTrackCount > 0" if db else "")
        self.a.wait_for(cond, timeout=30, interval=0.05)
        if db:  # loudly: the library is the fixture library at the path the harness uses (a template DB
            # built at another path for the library matches no queue track: the DB stores absolute paths)
            roots = self.a.ev_soft("JSON.stringify(root.uiBridge.libraryRootEntries)") or ""
            if json.dumps(LIB)[1:-1] not in roots and os.environ.get("SHOT_ALLOW_STALE_LIBRARY") == "1":
                print(f"  WARNING (diagnostic run): the library's roots are not {LIB}", flush=True)
            elif json.dumps(LIB)[1:-1] not in roots:
                raise RuntimeError(f"the library's roots {roots[:300]} are not the fixture library {LIB}: the template DB "
                                   "is stale (shoot_round.sh rebuilds it when its key, which includes this path, changes)")
        self.settle(0.3)
        time.sleep(0.2)  # the first frames after the window maps
        # Loudly: the window renders at the session's scale (under KWin, its output's scale, from the output
        # configuration the driver writes), or every shot and measurement of the session is at the wrong size.
        dpr = self.a.ev_soft("root.screen.devicePixelRatio")
        if not isinstance(dpr, (int, float)) or abs(dpr - scale) > 0.01:
            self.a.quit()
            raise RuntimeError(f"the main window's screen has device pixel ratio {dpr}, not the session's scale {scale}")
        if os.environ.get("SHOT_KWIN"):
            try:
                self.prime_input()
            except Exception:
                self.a.quit()  # (the runner's cleanup starts only once the session exists)
                raise
        self.startup_s = time.time() - t_app
        r = self.a.ev_soft("GraphicsInfo.api")
        print(f"[{name}/{tag}] renderer: {r}", flush=True)
        if r != 3 and not os.environ.get("SHOT_SOFTWARE"):
            raise RuntimeError(f"renderer {r}, not OpenGL")
        self.renderer = r

    def prime_input(self):
        """KWin's input set up before any step (kwin_input.py). One real key press: KWin opens a menu, a window of its
        own since #642, only for a client that has had input from the compositor, and the probe's events go past
        it; a lone Shift does nothing in the app. And one real pointer enter at away()'s spot, then out of the
        window again: Qt on Wayland takes the pointer to be where it last entered a window, (0, 0) while it never
        has, and hovers what is there when a menu's window closes. That was the menu bar's File item, and each
        later click on the bar then opened File, in place of the menu the state names."""
        import kwin_input
        if not self.a.wait_for("root.active", timeout=10, interval=0.05):
            raise RuntimeError("the main window is not active: a key through KWin would not reach it")
        frame = self.kwin_frame(str(self.ev("root.title")))
        if not frame:
            raise RuntimeError("KWin knows no frame of the main window: the pointer cannot be put in it")
        fx, fy, fw, fh = frame
        inp = kwin_input.KWinInput(kwin_input.socket_path(os.path.join(self.a.iso, "runtime")))
        try:
            ow, oh = kwin_output_size(self.size)
            x, y = self.neutral_point()
            inp.move(fx + (fw - self.size[0]) / 2 + x, fy + fh - self.size[1] + y)
            # out again only once the app has the enter: KWin's seat gets a pointer with this client, and an app
            # still binding it would miss an enter followed at once by the leave
            end = time.time() + 5
            while self.cmd("cursor") == "0 0":
                if time.time() > end:
                    raise RuntimeError("the pointer's enter through KWin never reached the app: its menus would "
                                       "hover the item at 0, 0 as they close")
                time.sleep(0.02)
            inp.move(ow - 1, oh - 1)
            inp.key(kwin_input.KEY_LEFTSHIFT)
        finally:
            inp.close()

    def kwin_key(self, name):
        """A real key through the session's KWin, to the window that has the keyboard."""
        import kwin_input
        inp = kwin_input.KWinInput(kwin_input.socket_path(os.path.join(self.a.iso, "runtime")))
        try:
            inp.key(kwin_input.KEYS[name])
        finally:
            inp.close()

    # --- probe wrappers
    def cmd(self, c, timeout=20):
        return self.a.cmd(c, timeout)

    def ev(self, js):
        return self.a.ev_soft(js)

    def find_js(self, name):
        return f"{FIND}('{name}')"

    def rect(self, name):
        r = self.cmd(f"find {name}")
        if r == "none":
            r = self.ev(f"(function(){{var it={FIND}('{name}');if(!it)return 'none';var p=it.mapToItem(null,0,0);"
                        f"return p.x+' '+p.y+' '+it.width+' '+it.height}})()")
            if not r or r == "none":
                return None
        return tuple(map(float, r.split()))

    def visible_rects(self, name):
        """Items named `name` whose centre is on screen and inside every clipping ancestor, by position."""
        r = self.ev("(function(n){var o=[];var s=[root.Overlay.overlay.parent];while(s.length){var it=s.shift();"
                    "if(!it.visible||it.opacity===0)continue;if(it.objectName===n){var p=it.mapToItem(null,0,0);"
                    "var cx=p.x+it.width/2,cy=p.y+it.height/2;var ok=cx>=0&&cy>=0&&cx<root.width&&cy<root.height;"
                    "var a=it.parent;while(ok&&a){if(a.clip){var q=a.mapToItem(null,0,0);"
                    "if(cx<q.x||cy<q.y||cx>q.x+a.width||cy>q.y+a.height)ok=false}a=a.parent}"
                    "if(ok)o.push([it.objectName,p.x,p.y,it.width,it.height])}var c=it.children;"
                    "for(var i=0;i<c.length;i++)s.push(c[i]);}o.sort(function(a,b){return a[2]-b[2]||a[1]-b[1]});"
                    f"return JSON.stringify(o)}})('{name}')")
        return json.loads(r) if r else []

    def rects(self, prefix):
        r = self.ev(f"JSON.stringify({FINDALL}('{prefix}').map(function(it){{var p=it.mapToItem(null,0,0);"
                    f"return [it.objectName,p.x,p.y,it.width,it.height]}}))")
        return json.loads(r) if r else []

    def center(self, name, fx=0.5, fy=0.5):
        r = self.rect(name)
        if not r:
            print(f"  ! no item {name}")
            return None
        return r[0] + r[2] * fx, r[1] + r[3] * fy

    def move(self, x, y):
        self.cmd(f"move {x:.1f} {y:.1f}")

    def click(self, x, y, kind="click"):
        self.cmd(f"{kind} {x:.1f} {y:.1f}")

    def hover(self, name, wait=0.6, fx=0.5, fy=0.5):
        c = self.center(name, fx, fy)
        if c:
            self.move(*c)
            time.sleep(wait)
        return c

    def neutral_point(self):
        """A spot over the queue's empty header end: no hover state, no tooltip."""
        return self.size[0] - 5, self.size[1] * 0.45

    def away(self):
        self.move(*self.neutral_point())
        time.sleep(0.15)  # hover tints fade out in 100 ms

    def key(self, name, mods=0, text=""):
        code = KEY[name] if isinstance(name, str) else name
        self.cmd(f"key {code} {mods} {text}".rstrip())

    def type(self, text, delay=0.08):
        for ch in text:
            self.cmd(f"key {ord(ch.upper())} 0 {ch}")
            time.sleep(delay)

    def esc(self, n=1):
        for _ in range(n):
            self.key("Escape")
            time.sleep(0.3)

    # --- capture
    # --- top-level windows (dialogs that are real windows, viewers)
    VIEWER_WINDOWS = ("spectrogramFullscreenWindow", "albumArtFullscreenWindow")

    def windows(self):
        out = []
        txt = self.cmd("windows")
        for m in re.finditer(r"\[(\S+) title=(.*?) name=(\S*) vis=(\d) (\d+)x(\d+) dpr=([\d.]+) pos=(-?\d+),(-?\d+) "
                             r"flags=(\w+)(?: active=(\w+))?(?: parent=(-?\d+))?\]", txt):
            out.append(dict(cls=m.group(1), title=m.group(2), name=m.group(3), visible=m.group(4) == "1",
                            w=int(m.group(5)), h=int(m.group(6)), active=m.group(11) in ("1", "true"),
                            parent=int(m.group(12)) if m.group(12) else -1, index=len(out),
                            popup=int(m.group(10), 16) & 0xFF == 0x09))  # Qt::Popup: a menu's window
        return out

    def dialog_windows(self):
        """Every visible dialog window (not the main window, not a viewer, not a menu's window), innermost first: a
        window no other visible dialog window is transient for (a confirmation over Configure or Edit Tags) before
        its parents, then the active one. KWin's activation of a new transient window can lag the grab, so the structure
        decides, not the focus."""
        ws = self.windows()
        main = ws[0]["cls"] if ws else ""
        cands = [w for w in ws[1:] if w["visible"] and w["cls"] != main and w["name"] not in self.VIEWER_WINDOWS
                 and not w["popup"]]
        parents = {w["parent"] for w in cands}
        return sorted(cands, key=lambda w: (w["index"] in parents, not w["active"]))

    def dialog_window(self, timeout=0.0):
        """A visible top-level window other than the main window and the viewers (a dialog that is a
        real window, e.g. Configure or About once they stop being in-window popups), or None."""
        end = time.time() + timeout
        while True:
            cands = self.dialog_windows()
            if cands:  # the innermost (a confirmation over its parent dialog) first
                return cands[0]
            if time.time() >= end:
                return None
            time.sleep(0.05)

    def _kwin_bus(self):
        import dbus
        addr = open(os.path.join(self.a.pdir, "bus-address.txt")).read().strip()
        return dbus.bus.BusConnection(addr)

    def kwin_uuid(self, title):
        """A window's KWin id, found by its exact title through KWin's windows runner (org.kde.krunner1 at
        /WindowsRunner), on this session's private bus; None when no window has that title."""
        import dbus
        try:
            kr = dbus.Interface(self._kwin_bus().get_object("org.kde.KWin", "/WindowsRunner"), "org.kde.krunner1")
            ms = list(kr.Match(title))
            for exact in (True, False):  # KWin may show the title with the application's name appended
                for m in ms:
                    cap = str(m[1])
                    if cap == title if exact else cap.startswith(title + " \u2014 "):
                        return str(m[0]).split("_", 1)[-1]
        except Exception as e:
            print("  kwin window lookup failed:", e)
        return None

    def kwin_frame(self, title):
        """A window's frame (x, y, w, h, logical px) from KWin's getWindowInfo, by its exact title."""
        import dbus
        uuid = self.kwin_uuid(title)
        if not uuid:
            return None
        try:
            k = dbus.Interface(self._kwin_bus().get_object("org.kde.KWin", "/KWin"), "org.kde.KWin")
            i = k.getWindowInfo(uuid)
            return tuple(float(i[key]) for key in ("x", "y", "width", "height"))
        except Exception as e:
            print("  kwin frame failed:", e)
        return None

    def kwin_capture_area(self, path, rect):
        """Captures an area of the screen (logical px) through KWin's ScreenShot2: nested dialog windows together,
        with their decorations and what lies under the area. Returns (w, h) or None."""
        try:
            import dbus
            iface = dbus.Interface(self._kwin_bus().get_object("org.kde.KWin", "/org/kde/KWin/ScreenShot2"),
                                   "org.kde.KWin.ScreenShot2")
            rfd, wfd = os.pipe()
            x, y, w, h = [int(round(v)) for v in rect]
            res = iface.CaptureArea(x, y, dbus.UInt32(w), dbus.UInt32(h), {"native-resolution": True},
                                    dbus.types.UnixFd(wfd))
            os.close(wfd)
            data = b""
            while True:
                c = os.read(rfd, 1 << 20)
                if not c:
                    break
                data += c
            os.close(rfd)
            W, H, stride = int(res["width"]), int(res["height"]), int(res["stride"])
            from PIL import Image
            Image.frombuffer("RGBA", (W, H), data, "raw", "RGBA", stride, 1).save(path)
            return W, H
        except Exception as e:
            print("  kwin area capture failed:", e)
            return None

    def kwin_capture(self, path, expect=None, title=None):
        """Captures a window with its KWin decoration through org.kde.KWin.ScreenShot2 on this session's private
        bus (allowed by the .desktop file the driver writes): the window titled `title` (by its KWin id, whatever
        is active), else the active window. Returns (w, h) or None."""
        try:
            import dbus
            addr = open(os.path.join(self.a.pdir, "bus-address.txt")).read().strip()
            bus = dbus.bus.BusConnection(addr)
            iface = dbus.Interface(bus.get_object("org.kde.KWin", "/org/kde/KWin/ScreenShot2"), "org.kde.KWin.ScreenShot2")
            rfd, wfd = os.pipe()
            opts = {"include-decoration": True, "include-shadow": False, "native-resolution": True}
            uuid = self.kwin_uuid(title) if title else None
            if uuid:
                res = iface.CaptureWindow(uuid, opts, dbus.types.UnixFd(wfd))
            else:
                res = iface.CaptureActiveWindow(opts, dbus.types.UnixFd(wfd))
            os.close(wfd)
            data = b""
            while True:
                c = os.read(rfd, 1 << 20)
                if not c:
                    break
                data += c
            os.close(rfd)
            W, H, stride = int(res["width"]), int(res["height"]), int(res["stride"])
            if expect and not (expect[0] <= W <= expect[0] + 120 and expect[1] <= H <= expect[1] + 120):
                return None  # not the window we meant (another one is active)
            from PIL import Image
            im = Image.frombuffer("RGBA", (W, H), data, "raw", "RGBA", stride, 1)  # format 18: RGBA8888 premul.
            im.save(path)
            return W, H
        except Exception as e:
            print("  kwin capture failed:", e)
            return None

    # --- condition waits (each with a timeout as a safety net)
    def popups(self, win=""):
        try:
            return json.loads(self.a.cmd("popups" + (("@" + win) if win else "")))
        except Exception:
            return []

    def settle(self, timeout=1.0, minimum=0.03):
        """Until no popup is mid-transition (visible but not yet opened, or fading out)."""
        time.sleep(minimum)
        end = time.time() + timeout
        while time.time() < end:
            if all(p["opened"] and not 0.02 < p["opacity"] < 0.99 for p in self.popups()):
                return True
            time.sleep(0.03)
        return False

    def tip(self, timeout=1.4):
        """Until a tooltip is fully shown (its delay and fade-in done)."""
        end = time.time() + timeout
        while time.time() < end:
            if any(p["tooltip"] and p["opened"] and p["opacity"] > 0.99 for p in self.popups()):
                time.sleep(0.03)
                return True
            time.sleep(0.04)
        return False

    READY = ("(function(){var b=root.uiBridge;if(!(b.waveformComplete||b.waveformEndsEarly))return false;"
             "var s=[root.Overlay.overlay.parent];while(s.length){var it=s.shift();if(!it.visible)continue;"
             "if(it.objectName==='spectrogramPaneItem'&&it.precomputedReady===false)return false;"
             "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return true})()")

    def ready(self, timeout=6.0, extra=0.35):
        """Until the analysis of the playing track is complete and the visible plots have it."""
        end = time.time() + timeout
        while time.time() < end:
            if self.ev(self.READY):
                time.sleep(min(extra, max(0, end - time.time())))
                return True
            time.sleep(0.1)
        return False

    def geometry(self, subject=None):
        """Rects (logical px) of the visible popups (menus, dialogs, tooltips; not the full-window dim)
        and of the item with keyboard focus, for the contact sheets' crops."""
        pops = [p["rect"] for p in self.popups() if p["opacity"] > 0.05 and p["rect"][2] > 1 and p["rect"][3] > 1]
        f = self.ev("(function(){var f=root.activeFocusItem;if(!f||f.width<=0)return '';var q=f.mapToItem(null,0,0);"
                    "return JSON.stringify([Math.round(q.x),Math.round(q.y),Math.round(f.width),Math.round(f.height)])})()")
        ok4 = lambda r: isinstance(r, list) and len(r) == 4 and all(isinstance(v, (int, float)) for v in r)
        pops = [p for p in pops if ok4(p)]
        fr = json.loads(f) if f else None
        out = dict(popups=pops, focus=fr if ok4(fr) else None)
        if subject:  # the item the area is about (e.g. the transport bar), for the sheet's crop
            r = self.rect(subject)
            if r and len(r) == 4:
                out["subject"] = [round(v) for v in r]
        return out

    def settle_opacity(self, win="", timeout=1.2):
        """Until nothing in the window is fading (its opacity fingerprint is the same twice, 50 ms apart),
        so fading furniture (a viewer's title strip, hover tints) is grabbed and measured settled."""
        end = time.time() + timeout
        prev = None
        while time.time() < end:
            cur = self.a.cmd(f"opacity{('@' + win) if win else ''}")
            if cur == prev:
                return True
            prev = cur
            time.sleep(0.05)
        return False

    def grab(self, area, state, desc, win="", subject=None, dialog_window=None):
        file = f"{area}_{state}_{self.suffix}"
        if not os.environ.get("SHOT_GEOMETRY_ONLY"):
            self.settle_opacity(win or (dialog_window["cls"] if dialog_window else ""))
        geo = {} if (win or dialog_window) else self.geometry(subject)
        if dialog_window and not os.environ.get("SHOT_GEOMETRY_ONLY"):
            path = os.path.join(OUT, file + ".png")
            # KWin's capture is asynchronous (over D-Bus): the layout walk runs right before it, on the
            # settled state, so geometry and image describe the same layout.
            lpath = self.layout_path(file)
            walked = lpath and self.a.cmd(f"layout {lpath}", timeout=30).startswith("ok")
            # Nested dialog windows (a confirmation over Configure, over Edit Tags): all of them in one image,
            # the area their frames cover, so the child is seen over its parent.
            nested = None
            # its transient ancestors that are dialog windows (Configure under its confirmation), not the main window
            ws = self.windows()
            others, pi = [], dialog_window.get("parent", -1)
            while 0 < pi < len(ws) and ws[pi]["visible"] and ws[pi] not in others:
                others.append(ws[pi])
                pi = ws[pi]["parent"]
            if others:
                frames = [self.kwin_frame(w["title"]) for w in [dialog_window] + others]
                if all(frames):
                    ux0, uy0 = min(f[0] for f in frames), min(f[1] for f in frames)
                    ux1, uy1 = max(f[0] + f[2] for f in frames), max(f[1] + f[3] for f in frames)
                    nested = (frames[0], (ux0, uy0, ux1 - ux0, uy1 - uy0), [w["title"] for w in others])
            elif any(p["opened"] and "Menu" in p["cls"] and p.get("exposed", True)
                     for p in self.popups(dialog_window["cls"])):
                # a menu open in the dialog is a window of its own (#642), which a capture of the dialog's window
                # leaves out: the screen inside the dialog's frame has both
                frame = self.kwin_frame(dialog_window["title"])
                if frame:
                    nested = (frame, frame, [])
            got = None
            if nested:
                got = self.kwin_capture_area(path, nested[1])
                how = (f"with its KWin decoration, over {', '.join(nested[2])} (nested windows, captured together "
                       "with their decorations)") if others else "with its KWin decoration and its open menu"
            if not got:
                nested = None
                got = self.kwin_capture(path, expect=(dialog_window["w"] * self.scale, dialog_window["h"] * self.scale),
                                        title=dialog_window["title"])
                how = "with its KWin decoration"
            if not got:
                r = self.a.cmd(f"grab@{dialog_window['cls']} {path}", timeout=30)
                how = "client area only (no decoration capture)"
                if not r.startswith("ok"):
                    return r
            print(f"  {file}: dialog window '{dialog_window['title']}' {how}", flush=True)
            self.index.append(dict(file=file + ".png", area=area, state=state, theme=self.tag, scale=self.scale,
                                   size=f"{self.size[0]}x{self.size[1]}", desc=desc + f" [its own window, {how}]",
                                   renderer=self.renderer, window=dialog_window["title"] or dialog_window["cls"]))
            if walked:
                # The decorated image is the window plus its title bar (and borders): window coordinates
                # map into it by this offset (top: the title bar; sides: half the extra width).
                dx = dy = 0
                if got and nested:  # the active window's client area inside the union's image
                    fx, fy, fw, fh = nested[0]
                    bx = max(0, (fw - dialog_window["w"]) / 2)
                    by = max(0, fh - dialog_window["h"] - bx)
                    dx = int(round((fx - nested[1][0] + bx) * self.scale))
                    dy = int(round((fy - nested[1][1] + by) * self.scale))
                elif got:
                    dx = max(0, (got[0] - dialog_window["w"] * self.scale) // 2)
                    dy = max(0, got[1] - dialog_window["h"] * self.scale - dx)
                self.index[-1]["layout"] = f"layout/{file}.json"
                self.index[-1]["grabbed"] = dict(kind="dialog", cls=dialog_window["cls"], decorated=bool(got),
                                                 image_offset=[dx, dy])
            return "ok"
        if os.environ.get("SHOT_GEOMETRY_ONLY"):
            self.index.append(dict(file=file + ".png", area=area, state=state, theme=self.tag, scale=self.scale,
                                   size=f"{self.size[0]}x{self.size[1]}", desc=desc, renderer=self.renderer,
                                   window=win or "main", **geo))
            print(f"  geometry {file}: {geo}", flush=True)
            return "ok geometry"
        tg = time.time()
        lpath = self.layout_path(file)
        r = self.a.cmd(f"grab{('@' + win) if win else ''} {os.path.join(OUT, file + '.png')}"
                       + (f" {lpath}" if lpath else ""), timeout=30)
        self.t_grab = getattr(self, "t_grab", 0) + time.time() - tg
        print(f"  {file}: {r}", flush=True)
        if r.startswith("ok"):
            self.index.append(dict(file=file + ".png", area=area, state=state, theme=self.tag, scale=self.scale,
                                   size=f"{self.size[0]}x{self.size[1]}", desc=desc, renderer=self.renderer,
                                   window=win or "main", **geo))
            if lpath and "layout not written" not in r:
                self.index[-1]["layout"] = f"layout/{file}.json"
                self.index[-1]["grabbed"] = dict(kind="window" if win else "main", name=win, image_offset=[0, 0])
        return r

    def check_dialog(self, state, file):
        """The DIALOG_EXPECT check of a dialog shot just grabbed: its window's title and a body text, from the
        layout walk taken with the grab. Raises when the shot is of another window or lacks the text."""
        import states
        exp = states.DIALOG_EXPECT.get(state)
        if not exp:
            return
        title, body = exp
        e = next((x for x in reversed(self.index) if x["file"] == file + ".png"), None)
        got = (e or {}).get("window", "")
        if not got.startswith(title):
            raise RuntimeError(f"dialog check: the shot is of '{got}', not of '{title}'")
        if body and e and e.get("layout"):
            lj = json.load(open(os.path.join(os.path.dirname(OUT.rstrip("/")), e["layout"])))
            texts = [t.get("text", "") for w in lj.get("windows", []) if str(w.get("title", "")).startswith(title)
                     for t in w.get("texts", [])]
            if not any(body in t for t in texts):
                raise RuntimeError(f"dialog check: '{title}' does not show '{body}' (texts: {texts[:4]})")

    # --- privacy evidence for composites: images a macro cuts from grabs that have no index entry of their own.
    # pack.py's privacy check needs, for every image it publishes, the layout walk of what it shows; a composite's
    # entry lists its sources (composite_evidence), or says it has no app pixels, and an entry with neither a walk,
    # nor sources, nor that marker fails the check.
    def begin_composites(self):
        """Starts a macro's evidence (runner.step_macro): forgets the sources and the marker of the macro before."""
        self.composite_sources = []
        self.composite_no_app_pixels = False

    def draws_without_app_pixels(self):
        """Declares that the running macro's images cut from no grab are drawn without app pixels (an icon
        rendered from its SVGs): they get the explicit marker instead of failing as images without sources."""
        self.composite_no_app_pixels = True

    def raw_grab(self, path, win=""):
        """Grabs the window to `path` with its layout walk beside it (path[:-4] + ".layout.json"; none when
        SHOT_LAYOUT=0) and records it as a source of the running macro's composites. Returns the probe's reply."""
        walk = None if os.environ.get("SHOT_LAYOUT", "1") == "0" else path[:-4] + ".layout.json"
        r = self.cmd(f"grab{('@' + win) if win else ''} {path}" + (f" {walk}" if walk else ""), timeout=30)
        if str(r).startswith("ok"):
            walked = walk is not None and "layout not written" not in str(r)
            # (None: a source without a walk, which the privacy check fails)
            self.composite_sources.append(os.path.relpath(walk, os.path.dirname(OUT.rstrip("/"))) if walked else None)
        return r

    def source_shot(self, file):
        """Records an indexed shot (shots/FILE) that the running macro's composites reuse pixels of."""
        self.composite_sources.append("shot:" + file)

    def composite_evidence(self, file):
        """The privacy fields of the composite entry `file`: as `sources`, every grab and shot the running macro
        recorded so far, so each image cut from one grab lists it, not only the first (checked more than needed,
        never less); for an image from none, in a macro that declared draws_without_app_pixels, `sources=[]` with
        `no_app_pixels=True`. Raises for an image from no grab and without that marker."""
        if self.composite_sources:
            return dict(sources=list(self.composite_sources))
        if self.composite_no_app_pixels:
            return dict(sources=[], no_app_pixels=True)
        raise RuntimeError(f"{file}: a composite from no grab or shot: grab with raw_grab or record the shot with "
                           "source_shot, or declare draws_without_app_pixels if it holds no app pixels")

    def layout_path(self, file):
        """Where the layout checker's walk for this shot goes (None when SHOT_LAYOUT=0)."""
        if os.environ.get("SHOT_LAYOUT", "1") == "0":
            return None
        d = os.path.join(os.path.dirname(OUT.rstrip("/")), "layout")
        os.makedirs(d, exist_ok=True)
        return os.path.join(d, file + ".json")

    def layout_dump(self, file, grabbed):
        """The layout checker: the probe walks every shown window's item tree right after the grab and writes
        its findings and visible text; layout.py analyses them (LAYOUT.md)."""
        if os.environ.get("SHOT_LAYOUT", "1") == "0":
            return
        tl = time.time()
        d = os.path.join(os.path.dirname(OUT.rstrip("/")), "layout")
        os.makedirs(d, exist_ok=True)
        r = self.a.cmd(f"layout {os.path.join(d, file + '.json')}", timeout=30)
        self.t_layout = getattr(self, "t_layout", 0) + time.time() - tl
        if r.startswith("ok"):
            self.index[-1]["layout"] = f"layout/{file}.json"
            self.index[-1]["grabbed"] = grabbed

    def strip(self, name, action, duration=1.2, pre=0.15, crop=None, desc="", opts=""):
        """Records every frame for `duration` s; `action` runs after `pre` s (a mark is logged)."""
        if os.environ.get("SHOT_GEOMETRY_ONLY"):
            action()
            time.sleep(duration)
            return None
        d = os.path.join(STRIPS_RAW, f"{name}_{self.suffix}")
        if os.path.isdir(d):
            for f in glob.glob(d + "/*"):
                os.remove(f)
        # the main window's panes where they are now (crops and focus rects named after items, never a
        # hardcoded width that spills into a region that keeps moving)
        items = {} if "win=" in opts else {n: r for n in STRIP_GEOMETRY for r in [self.rect(n)] if r}
        c = "stripstart" + (" %d %d %d %d" % tuple(crop) if crop else "") + ((" " + opts) if opts else "")
        self.cmd(c)
        time.sleep(pre)
        self.cmd("now ACTION")
        action()
        time.sleep(duration)
        ts = time.time()
        r = self.cmd(f"stripstop {d}", timeout=120)
        # the main window's menus (windows of their own) drawn into its frames at their place; a win= strip has none
        popups = popup_frames.merge(d, dpr=self.scale, crop=[int(v) for v in crop] if crop else None,
                                    frame_scale=popup_frames.frame_scale(opts))
        self.t_stripsave = getattr(self, "t_stripsave", 0) + time.time() - ts
        print(f"  strip {name}: {r}" + (f", menu window(s) {popups} drawn in" if popups else ""), flush=True)
        with open(os.path.join(d, "meta.json"), "w") as f:
            # neutral: recorded with the work folder at /mnt/ferrous (neutral_ns.sh), which the privacy check needs:
            # a recording's trace names no texts, so frames recorded at the real paths cannot be checked
            json.dump(dict(name=name, theme=self.tag, desc=desc, crop=crop, scale=self.scale,
                           size=list(self.size), opts=opts, variant=self.suffix, items=items, popup_windows=popups,
                           neutral=bool(os.environ.get("SHOT_NEUTRAL_WORK"))), f)
        return d

    def save_index(self, label):
        os.makedirs(os.path.join(os.path.dirname(OUT.rstrip("/")), "index"), exist_ok=True)
        p = os.path.join(os.path.dirname(OUT.rstrip("/")), "index", f"{label}_{self.suffix}.json")
        # Merge: an --only run replaces its own states and keeps the session's others.
        old = json.load(open(p)) if os.path.exists(p) else []
        mine = {e["file"] for e in self.index}
        with open(p, "w") as f:
            json.dump([e for e in old if e["file"] not in mine] + self.index, f, indent=1)

    def quit(self):
        self.a.quit()

    # --- app helpers
    def load_queue(self, patterns=QUEUE_ALBUMS):
        paths = album_paths(*patterns)
        self.ev(f"root.uiBridge.replaceWithPaths({json.dumps(paths)})")
        self.a.wait_for(f"root.uiBridge.queueLength >= {len(paths)}", timeout=20, interval=0.05)
        time.sleep(0.2)
        return paths

    def queue_index(self, needle):
        return self.ev("(function(){for (var i = 0; i < root.uiBridge.queueLength; ++i)"
                       f" if (root.uiBridge.queuePathAt(i).indexOf({json.dumps(needle)}) >= 0) return i; return -1}})()")

    def play_index(self, i, seek=None, settle=1.0):
        self.ev(f"root.uiBridge.playAt({i})")
        self.a.wait_for("root.uiBridge.playbackState.toLowerCase() === 'playing'", timeout=15)
        time.sleep(settle)
        if seek is not None:
            self.ev(f"root.uiBridge.seek({seek})")

    def play(self, needle, seek=None, settle=1.0, wait=0):
        i = self.queue_index(needle)
        self.ev(f"root.uiBridge.playAt({i})")
        self.a.wait_for("root.uiBridge.playbackState.toLowerCase() === 'playing'", timeout=15, interval=0.05)
        if seek is not None:
            self.a.wait_for("root.uiBridge.durationSeconds > 0", timeout=5, interval=0.05)
            self.ev(f"root.uiBridge.seek({seek})")
        if wait:
            self.ready(timeout=wait)
        return i

    def open_menu_bar(self, index):
        r = self.ev(f"(function(){{var it=root.menuBar.itemAt({index});var p=it.mapToItem(null,0,0);"
                    f"return p.x+' '+p.y+' '+it.width+' '+it.height}})()")
        x, y, w, h = map(float, r.split())
        self.click(x + w / 2, y + h / 2)
        time.sleep(0.6)
        return x + w / 2, y + h / 2

    def focus_default_button(self, win=""):
        """Gives the open dialog's default (primary) button keyboard focus, as Tab would."""
        js = ("(function(){var s=[root.Overlay.overlay];while(s.length){var it=s.shift();"
                       "if(!it.visible)continue;if(it.primary!==undefined&&it.primary&&it.primary.visible){"
                       "it.primary.forceActiveFocus(Qt.TabFocusReason);return it.primary.text||true}"
                       "var c=it.children;for(var i=0;i<c.length;i++)s.push(c[i]);}return false})()")
        if win:  # a dialog window: evaluated in that window's own context
            # In the window's context: start from its root item (content and overlay, so popups inside it too).
            # Overlay (popups) before content: a confirmation inside the window gets the focus, not the window's own OK.
            top = ("(function(){var r=contentItem;while(r.parent)r=r.parent;"
                   "return Array.prototype.slice.call(r.children).reverse()})()")
            r = self.a.cmd(f"eval@{win} {js.replace('[root.Overlay.overlay]', top)}")
            try:
                return json.loads(r)[0] if not r.startswith("ERROR") else None
            except ValueError:
                return None
        return self.ev(js)

    def focus_owner(self):
        return self.ev("(function(){var f = root.activeFocusItem; if (!f) return 'none';"
                       " var r = f.mapToItem(null, 0, 0);"
                       " return (f.objectName || '') + ' ' + String(f).split('(')[0] + ' @' + Math.round(r.x) + ','"
                       " + Math.round(r.y) + ' ' + Math.round(f.width) + 'x' + Math.round(f.height)"
                       " + (f.text ? ' text=' + f.text : '') + (f.Accessible && f.Accessible.name ? ' acc=' + f.Accessible.name : '')})()")


def run_sessions(fn, tags, *args, **kw):
    for tag in tags:
        fn(tag, *args, **kw)
