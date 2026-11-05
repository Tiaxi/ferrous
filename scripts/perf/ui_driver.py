#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drives the Ferrous desktop app with the LD_PRELOAD probe (ui-probe/).

Every run gets its own HOME/XDG_*/TMPDIR under the run directory, so the
real Ferrous config, data and cache are never read or written. Import it from
a scenario script, or run `ui_driver.py smoke RUNDIR` to check the setup.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
TICK = os.sysconf("SC_CLK_TCK")


def load1():
    return os.getloadavg()[0]


def wait_for_quiet(limit=3.0, max_wait=900, log=print):
    """Waits until the 1-minute load average is at most `limit`."""
    start = time.monotonic()
    while load1() > limit:
        if time.monotonic() - start > max_wait:
            log(f"load still {load1():.1f} after {max_wait} s; measuring anyway")
            return False
        time.sleep(15)
    return True


class App:
    def __init__(self, run_dir, app, probe, settings=None, library_db=None, env=None,
                 args=None, settle=8.0):
        self.run_dir = os.path.abspath(run_dir)
        if os.path.isdir(self.run_dir):
            shutil.rmtree(self.run_dir)
        for d in ("home", "config/ferrous", "data/ferrous", "cache", "tmp", "probe"):
            os.makedirs(os.path.join(self.run_dir, d), exist_ok=True)
        if settings:
            shutil.copy(settings, os.path.join(self.run_dir, "config/ferrous/settings.txt"))
        if library_db:
            shutil.copy(library_db, os.path.join(self.run_dir, "data/ferrous/library.sqlite3"))
        self.probe_dir = os.path.join(self.run_dir, "probe")
        e = dict(os.environ)
        e.update({
            "HOME": os.path.join(self.run_dir, "home"),
            "XDG_CONFIG_HOME": os.path.join(self.run_dir, "config"),
            "XDG_DATA_HOME": os.path.join(self.run_dir, "data"),
            "XDG_CACHE_HOME": os.path.join(self.run_dir, "cache"),
            "TMPDIR": os.path.join(self.run_dir, "tmp"),
            "GST_REGISTRY": os.path.join(REPO, "target/perf-ui/gst-registry.bin"),
            "FERROUS_PROBE_OUT": self.probe_dir,
            "LD_PRELOAD": probe,
        })
        # Keep the real session bus for MPRIS and the compositor; drop
        # anything that would point the app at another config.
        e.update(env or {})
        self.log_path = os.path.join(self.run_dir, "app.log")
        self.log = open(self.log_path, "w")
        self.proc = subprocess.Popen([app] + (args or []), env=e, stdout=self.log,
                                     stderr=subprocess.STDOUT, start_new_session=True)
        self.pid = self.proc.pid
        self.fifo = os.path.join(self.probe_dir, "cmd")
        deadline = time.monotonic() + 30
        while not os.path.exists(self.fifo):
            if time.monotonic() > deadline or self.proc.poll() is not None:
                raise RuntimeError("probe FIFO did not appear; see " + self.log_path)
            time.sleep(0.1)
        time.sleep(settle)
        if self.proc.poll() is not None:
            # A Ferrous already running in this session takes over the
            # activation and the new process quits; nothing here may then
            # drive that other instance.
            raise RuntimeError("the app exited during start-up (is another Ferrous running "
                               "in this session?); see " + self.log_path)

    # ---------------------------------------------------------- probe
    def cmd(self, line):
        fd = os.open(self.fifo, os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(fd, (line + "\n").encode())
        finally:
            os.close(fd)

    def mark(self, label):
        self.cmd("mark " + label)

    def probe_log(self):
        with open(os.path.join(self.probe_dir, "probe.log")) as f:
            return f.read().splitlines()

    def wait_reply(self, prefix, timeout=10.0, after=0):
        """Waits for an R line whose text starts with `prefix`; returns it."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in self.probe_log()[after:]:
                parts = line.split(" ", 2)
                if parts[0] == "R" and len(parts) > 2 and parts[2].startswith(prefix):
                    return parts[2]
            time.sleep(0.05)
        raise TimeoutError(prefix)

    def eval(self, js, timeout=10.0):
        n = len(self.probe_log())
        self.cmd("eval " + js.replace("\n", " "))
        text = self.wait_reply("eval ", timeout, after=n)[5:]
        if text.startswith("ERROR"):
            raise RuntimeError(f"{js}: {text}")
        return json.loads(text)[0]

    # ---------------------------------------------------------- MPRIS
    def mpris(self, *args):
        """Calls a Player method on this app's own D-Bus connection.

        Only the unique connection of an MPRIS name that the started
        process owns is called: the generic name may belong to another
        Ferrous running in the same session, such as the user's own, which
        a run must never control. A unique name is never reused, so the
        call fails once the app is gone instead of reaching another one.
        """
        names = subprocess.run(["busctl", "--user", "list", "--no-legend"], capture_output=True, text=True).stdout
        name = None
        for line in names.splitlines():
            parts = line.split()
            if (len(parts) > 4 and parts[0].startswith("org.mpris.MediaPlayer2.ferrous")
                    and parts[1] == str(self.pid) and parts[4].startswith(":")):
                name = parts[4]
                break
        if name is None:
            raise RuntimeError(f"no Ferrous MPRIS name is owned by PID {self.pid}; "
                               "refusing to call another instance")
        return subprocess.run(["busctl", "--user", "call", name, "/org/mpris/MediaPlayer2",
                               "org.mpris.MediaPlayer2.Player"] + list(args), capture_output=True, text=True)

    # ---------------------------------------------------------- sampling
    def tasks(self):
        out = {}
        try:
            tids = os.listdir(f"/proc/{self.pid}/task")
        except OSError:
            return out
        for tid in tids:
            base = f"/proc/{self.pid}/task/{tid}"
            try:
                stat = open(f"{base}/stat").read()
                comm = open(f"{base}/comm").read().strip()
                status = open(f"{base}/status").read()
            except OSError:
                continue
            rest = stat[stat.rfind(")") + 2:].split()
            ticks = int(rest[11]) + int(rest[12])
            vol = int(re.search(r"voluntary_ctxt_switches:\s+(\d+)", status).group(1))
            invol = int(re.search(r"nonvoluntary_ctxt_switches:\s+(\d+)", status).group(1))
            out[tid] = (comm, ticks, vol, invol)
        return out

    def sample(self, seconds, label, out_jsonl=None):
        """Per-thread CPU % and wakeups/s over `seconds`, marked in the probe log."""
        self.mark(f"begin {label}")
        start = self.tasks()
        first, last = dict(start), dict(start)
        t0 = time.monotonic()
        load0 = load1()
        while time.monotonic() - t0 < seconds:
            time.sleep(min(1.0, max(0.0, seconds - (time.monotonic() - t0))))
            now = self.tasks()
            for tid, v in now.items():
                first.setdefault(tid, (v[0], 0, 0, 0))
                last[tid] = v
        elapsed = time.monotonic() - t0
        self.mark(f"end {label}")
        by_name = {}
        for tid, (comm, ticks, vol, invol) in last.items():
            _, t0_, v0, i0 = first[tid]
            name = "main" if tid == str(self.pid) else re.sub(r"\d", "#", comm)
            e = by_name.setdefault(name, {"n": 0, "cpu": 0.0, "vcs": 0.0, "ivcs": 0.0})
            e["n"] += 1
            e["cpu"] += (ticks - t0_) / TICK / elapsed * 100
            e["vcs"] += (vol - v0) / elapsed
            e["ivcs"] += (invol - i0) / elapsed
        probe_threads = {k: v for k, v in by_name.items() if k.startswith("ferrous-probe")}
        for v in by_name.values():
            for k in ("cpu", "vcs", "ivcs"):
                v[k] = round(v[k], 2)
        result = {
            "label": label, "seconds": round(elapsed, 1), "load_start": load0, "load_end": load1(),
            "cpu_pct": round(sum(v["cpu"] for k, v in by_name.items() if k not in probe_threads), 2),
            "wakeups_per_s": round(sum(v["vcs"] for k, v in by_name.items() if k not in probe_threads), 1),
            "threads": dict(sorted(by_name.items(), key=lambda kv: -kv[1]["cpu"])),
        }
        if out_jsonl:
            with open(out_jsonl, "a") as f:
                f.write(json.dumps(result) + "\n")
        return result

    def quit(self):
        if self.proc.poll() is None:
            try:
                self.cmd("quit")
            except OSError:
                pass
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                os.kill(self.pid, signal.SIGTERM)
                try:
                    self.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.kill(self.pid, signal.SIGKILL)
                    self.proc.wait()
        self.log.close()


def default_paths():
    return {
        "app": os.environ.get("FERROUS_PERF_APP", os.path.join(REPO, "target/perf-ui/ui-build/ferrous")),
        "probe": os.environ.get("FERROUS_PERF_PROBE", os.path.join(REPO, "target/perf-ui/probe/libferrous_ui_probe.so")),
        "settings": os.path.join(HERE, "ui-settings.txt"),
    }


if __name__ == "__main__" and len(sys.argv) > 2 and sys.argv[1] == "smoke":
    p = default_paths()
    app = App(sys.argv[2], p["app"], p["probe"], p["settings"])
    try:
        app.cmd("hook")
        app.cmd("dump items.txt")
        time.sleep(2)
        print(app.eval("root.width + 'x' + root.height"))
        print(app.sample(3, "smoke"))
    finally:
        app.quit()
