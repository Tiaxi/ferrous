#!/usr/bin/env python3
"""Drives a Ferrous build offscreen with shotprobe.so for screenshots.

Isolation: own HOME/XDG_* dirs, XDG_RUNTIME_DIR and TMPDIR under <work>/run/<name>,
a private session bus (dbus-run-session), no display variables, fakeaudiosink. The app falls back to
autoaudiosink (a real device) when it cannot create the sink it is given, so no app starts unless
fakeaudiosink can be created (require_fake_sink).

Environment:
  SHOT_APP   the ferrous binary to run (required)
  SHOT_OUT   where grab() writes PNGs (default <work>/shots)
  SHOT_WORK  the work folder: fixtures/, run/, b/ (bus sockets) and probe/ (the built probes);
             default $XDG_CACHE_HOME/ferrous-ui-capture
  SHOT_PROBE_DIR  where build_probes.sh put shotprobe.so (default <work>/probe)
"""
import os
import shutil
import signal
import subprocess
import time



def default_work():
    cache = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
    return os.path.join(cache, "ferrous-ui-capture")


# Resolved: no symlink in the app's paths (a path the app compares with another must not differ by one).
WORK = os.path.realpath(os.environ.get("SHOT_WORK") or default_work())
# The probes are built into the real work folder (build_probes.sh); LD_PRELOAD takes them from there.
PROBE = os.path.join(os.environ.get("SHOT_PROBE_DIR") or os.path.join(WORK, "probe"), "shotprobe.so")
# Under neutral_ns.sh the same folder at a neutral path (/mnt/ferrous): the app is given only those paths.
WORK = os.environ.get("SHOT_NEUTRAL_WORK") or WORK
APP = os.environ["SHOT_APP"]
LIB = os.path.join(WORK, "fixtures", "library")
SHOTS = os.environ.get("SHOT_OUT", os.path.join(WORK, "shots"))

SETTINGS = """volume_cubic=1.0000
volume=0.500000
fft_size=2048
spectrogram_view_mode=per_channel
spectrogram_display_mode=centered
viewer_fullscreen_mode=whole_screen
db_range=120.00
log_scale=0
show_fps=0
show_level_meter=0
prevent_display_sleep_in_fullscreen=0
show_spectrogram_crosshair=1
show_spectrogram_scale=0
channel_buttons_visibility=1
spectrogram_zoom_enabled=1
system_media_controls_enabled=1
library_sort_mode=0
library_watch_enabled=0
window_geometry=0,0,{w},{h}
window_maximized=0
visualization_mode=0
follow_current_track=1
"""


_FAKE_SINK_OK = False


def require_fake_sink():
    """Raises unless GStreamer can create and run fakeaudiosink, the sink every app is given. The app falls back
    to autoaudiosink, a real audio device, when its sink cannot be created, so a missing plugin would play the
    fixtures aloud: fail closed instead. Checked once per process, with a GStreamer registry in the work folder
    (never the user's cache)."""
    global _FAKE_SINK_OK
    if _FAKE_SINK_OK:
        return
    os.makedirs(os.path.join(WORK, "run"), exist_ok=True)
    env = dict(os.environ, GST_REGISTRY=os.path.join(WORK, "run", "gst-registry-sinkcheck.bin"))
    try:
        r = subprocess.run(["gst-launch-1.0", "-q", "audiotestsrc", "num-buffers=1", "!", "fakeaudiosink"],
                           env=env, capture_output=True, text=True, timeout=60)
        ok, why = r.returncode == 0, (r.stderr or r.stdout).strip()[-300:]
    except (OSError, subprocess.TimeoutExpired) as e:
        ok, why = False, str(e)
    if not ok:
        raise RuntimeError("GStreamer cannot create fakeaudiosink, and the app would fall back to a real audio "
                           "device: install gstreamer1-plugins-bad-free (Fedora) and gst-launch-1.0 (gstreamer1). "
                           + why)
    _FAKE_SINK_OK = True


def kdeglobals(scheme, pt=10, anim_off=False):
    src = f"/usr/share/color-schemes/{scheme}.colors"
    text = open(src, encoding="utf-8").read()
    icon = "breeze-dark" if "Dark" in scheme else "breeze"
    return (text + f"\n[General]\nColorScheme={scheme}\nfont=Noto Sans,{pt},-1,5,400,0,0,0,0,0,0,0,0,0,0,1\n"
            f"fixed=Hack,{pt},-1,5,400,0,0,0,0,0,0,0,0,0,0,1\n"
            f"smallestReadableFont=Noto Sans,{max(8, pt - 2)},-1,5,400,0,0,0,0,0,0,0,0,0,0,1\n"
            f"[Icons]\nTheme={icon}\n[KDE]\nwidgetStyle=Breeze\n"
            + ("AnimationDurationFactor=0\n" if anim_off else ""))


class App:
    def __init__(self, name, scheme="BreezeLight", scale=1, size=(1600, 1000), fresh=True,
                 db_from=None, extra_env=None, font_pt=10, settings_extra="", anim_off=False, app_defaults=False):
        require_fake_sink()  # before anything starts: never an app that could play aloud
        self.name = name
        self.iso = os.path.join(WORK, "run", name)
        if fresh and os.path.isdir(self.iso):
            shutil.rmtree(self.iso)
        for d in ("home", "config/ferrous", "data/ferrous", "cache", "tmp", "runtime", "probe"):
            os.makedirs(os.path.join(self.iso, d), exist_ok=True)
        os.chmod(os.path.join(self.iso, "runtime"), 0o700)
        open(os.path.join(self.iso, "config", "kdeglobals"), "w").write(kdeglobals(scheme, font_pt, anim_off))
        # Lets this harness's Python call KWin's restricted org.kde.KWin.ScreenShot2 (window captures with
        # their decoration) inside this isolated session only: KWin allows callers whose executable a
        # .desktop file declares with X-KDE-DBUS-Restricted-Interfaces.
        apps = os.path.join(self.iso, "data", "applications")
        os.makedirs(apps, exist_ok=True)
        import sys as _sys
        open(os.path.join(apps, "ferrous-shot-capture.desktop"), "w").write(
            "[Desktop Entry]\nType=Application\nName=Ferrous shot capture\nNoDisplay=true\n"
            f"Exec={os.path.realpath(_sys.executable)}\n"
            "X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n")
        settings = os.path.join(self.iso, "config", "ferrous", "settings.txt")
        if fresh or not os.path.exists(settings):
            # app_defaults: only the window's size and place, so the app starts with its own defaults (a new
            # install); every other session gets the harness's fixed settings
            text = (f"window_geometry=0,0,{size[0]},{size[1]}\nwindow_maximized=0\n" if app_defaults
                    else SETTINGS.format(w=size[0], h=size[1]))
            open(settings, "w").write(text + settings_extra)
        if db_from:
            for suffix in ("", "-wal", "-shm"):
                p = db_from + suffix
                if os.path.exists(p):
                    shutil.copy(p, os.path.join(self.iso, "data", "ferrous", "library.sqlite3" + suffix))
        self.pdir = os.path.join(self.iso, "probe")
        fifo = os.path.join(self.pdir, "cmd")
        if os.path.exists(fifo):
            os.remove(fifo)
        os.mkfifo(fifo)
        self.replies = os.path.join(self.pdir, "reply.log")
        open(self.replies, "w").close()
        env = {k: v for k, v in os.environ.items()
               if k not in ("WAYLAND_DISPLAY", "DISPLAY", "DBUS_SESSION_BUS_ADDRESS",
                            "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID", "SESSION_MANAGER")}
        env.update({
            "HOME": os.path.join(self.iso, "home"),
            # the real home folder, which the probe's layout walk flags texts by (the app's own HOME is isolated)
            "SHOT_PRIVATE_HOME": os.path.expanduser("~"),
            "XDG_CONFIG_HOME": os.path.join(self.iso, "config"),
            "XDG_DATA_HOME": os.path.join(self.iso, "data"),
            "XDG_CACHE_HOME": os.path.join(self.iso, "cache"),
            "XDG_RUNTIME_DIR": os.path.join(self.iso, "runtime"),
            "TMPDIR": os.path.join(self.iso, "tmp"),
            "QT_QPA_PLATFORM": "offscreen",
            "QT_QPA_PLATFORMTHEME": "kde",
            "QT_SCALE_FACTOR": str(scale),
            "QT_QUICK_BACKEND": "software" if os.environ.get("SHOT_SOFTWARE") else "",
            "FERROUS_GST_OUTPUT_SINK": "fakeaudiosink",
            # Per worker (prefix): parallel apps never share the registry or the shader cache.
            "GST_REGISTRY": os.path.join(WORK, "run", f"gst-registry-{os.environ.get('SHOT_RUN_PREFIX', 'run')}.bin"),
            "MESA_SHADER_CACHE_DIR": os.path.join(WORK, "run", f"mesa-cache-{os.environ.get('SHOT_RUN_PREFIX', 'run')}"),
            "LD_PRELOAD": PROBE,
            "SHOTPROBE_DIR": self.pdir,
            "LANG": "en_US.UTF-8",
            "LC_ALL": "en_US.UTF-8",
        })
        if not env["QT_QUICK_BACKEND"]:
            del env["QT_QUICK_BACKEND"]
        env.update(extra_env or {})
        self.log = open(os.path.join(self.iso, "app.log"), "w")
        # Inside the private bus: record the bus, then exec the app.
        script = ('busctl --user list --no-legend > "$SHOTPROBE_DIR/bus-before.txt" 2>&1; '
                  'echo "$DBUS_SESSION_BUS_ADDRESS" > "$SHOTPROBE_DIR/bus-address.txt"; '
                  f'exec "{APP}"')
        busconf = os.path.join(self.iso, "session-bus.conf")
        os.makedirs(os.path.join(WORK, "b"), exist_ok=True)
        import hashlib
        sock = os.path.join(WORK, "b", name[:12] + "-" + hashlib.sha1(name.encode()).hexdigest()[:7] + ".sock")
        if os.path.exists(sock):
            os.remove(sock)
        open(busconf, "w").write(
            '<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" '
            '"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">\n'
            '<busconfig><type>session</type>'
            f'<listen>unix:path={sock}</listen>'
            '<auth>EXTERNAL</auth>'
            '<policy context="default"><allow send_destination="*" eavesdrop="true"/>'
            '<allow eavesdrop="true"/><allow own="*"/></policy></busconfig>\n')
        screens = os.path.join(self.iso, "screens.json")
        sw, sh = max(size[0], 1600) + 400, max(size[1], 1000) + 400
        open(screens, "w").write(
            '{"screens": [{"name": "Shot", "x": 0, "y": 0, '
            f'"width": {sw * scale}, "height": {sh * scale}, '
            f'"logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}}]}}')
        env["QT_SCREEN_SCALE_FACTORS"] = f"Shot={scale}"
        env["QT_QPA_PLATFORM"] = f"offscreen:configfile={screens}"
        del env["QT_SCALE_FACTOR"]
        argv = ["bash", "-c", script]
        if os.environ.get("SHOT_KWIN"):
            launcher = os.path.join(self.iso, "launch.sh")
            open(launcher, "w").write(
                "#!/bin/bash\nexport QT_QPA_PLATFORM=wayland WAYLAND_DISPLAY=shotwl\n"
                f"export LD_PRELOAD={PROBE}\n"
                "unset QT_SCREEN_SCALE_FACTORS\n" + (f"export QT_SCALE_FACTOR={scale}\n" if scale != 1 else "") + script.replace("exec ", "exec ") + "\n")
            os.chmod(launcher, 0o755)
            ow, oh = (size[0] + 400) * scale, (size[1] + 400) * scale
            env.pop("LD_PRELOAD", None)
            env["QT_QPA_PLATFORM"] = "offscreen"
            argv = ["kwin_wayland", "--virtual", "--socket", "shotwl", "--width", str(ow), "--height", str(oh),
                    "--scale", "1", "--no-lockscreen", "--no-global-shortcuts", "--no-kactivities", "--inputmethod", "/bin/true",
                    "--exit-with-session", launcher]
        self.proc = subprocess.Popen(["dbus-run-session", f"--config-file={busconf}", "--"] + argv,
                                     env=env, stdout=self.log, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        self.n = 0
        self.fifo = os.fdopen(os.open(fifo, os.O_RDWR), "w")
        self.wait_reply("0", 30)

    def wait_reply(self, rid, timeout=20):
        # Only complete (newline-terminated) lines count: the probe may still be writing the last one,
        # and a reply read half-written is wrong data, e.g. "0 975 16" for "0 975 1600 25".
        end = time.time() + timeout
        prefix = f"R {rid} "
        if not hasattr(self, "_rbuf"):
            self._rbuf, self._rpos, self._got = b"", 0, {}
        polls = 0
        while time.time() < end:
            with open(self.replies, "rb") as f:
                f.seek(self._rpos)
                chunk = f.read()
            self._rpos += len(chunk)
            self._rbuf += chunk
            *lines, self._rbuf = self._rbuf.split(b"\n")
            for raw in lines:
                line = raw.decode("utf-8", errors="replace")
                if line.startswith("R "):
                    k, _, rest = line[2:].partition(" ")
                    self._got[k] = rest
            if rid in self._got:
                return self._got.pop(rid)
            if self.proc.poll() is not None:
                raise RuntimeError(f"app exited ({self.proc.returncode}); see {self.log.name}")
            # most replies come within a frame: poll fast first (1, 2, 4, 8, 16 ms), then every 20 ms
            time.sleep(min(0.02, 0.001 * (2 ** min(polls, 5))))
            polls += 1
        raise TimeoutError(f"no reply {rid}")

    def cmd(self, text, timeout=20):
        self.n += 1
        rid = str(self.n)
        self.fifo.write(f"{rid} {text}\n")
        self.fifo.flush()
        return self.wait_reply(rid, timeout)

    def ev(self, js, win=""):
        r = self.cmd(f"eval{('@' + win) if win else ''} {js}")
        if r.startswith("ERROR"):
            raise RuntimeError(f"{js}: {r}")
        import json
        return json.loads(r)[0]

    def ev_soft(self, js, win=""):
        try:
            return self.ev(js, win)
        except RuntimeError as e:
            print("  eval failed:", e)
            return None

    def wait_for(self, js, timeout=30, interval=0.25):
        end = time.time() + timeout
        while time.time() < end:
            if self.ev_soft(js):
                return True
            time.sleep(interval)
        print(f"  timeout waiting for {js}")
        return False

    def grab(self, name, win=""):
        os.makedirs(SHOTS, exist_ok=True)
        path = os.path.join(SHOTS, name + ".png")
        r = self.cmd(f"grab{('@' + win) if win else ''} {path}", timeout=30)
        print(f"  {name}: {r}")
        return path

    def quit(self):
        try:
            self.cmd("quit", timeout=5)
        except Exception:
            pass
        try:
            self.proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGTERM)  # own session: dbus-run-session + app only
            self.proc.wait(timeout=10)
        self.fifo.close()
        self.log.close()
