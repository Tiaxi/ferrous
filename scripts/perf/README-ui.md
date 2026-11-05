# UI performance harness

Scripts for before/after comparisons of the desktop app's rendering, frame
rate, uploads, CPU per thread and GUI-thread stalls, on the real display. They
run the app built by `scripts/run-ui.sh` with a measurement-only probe loaded
through `LD_PRELOAD`, so no product code changes are needed and the same
harness measures any commit. Every run gets its own `HOME`, `XDG_*_HOME` and
`TMPDIR` under its run directory (`ui_driver.App`), so nothing reads or writes
the real Ferrous config, data or cache. A window opens on the desktop and
audio plays.

The app runs on the real session bus, which the scripts use for MPRIS. Ferrous
is single-instance there, so a Ferrous already running in the session takes
over the start and the new process quits. `ui_driver.App` then fails. The
MPRIS calls of `ui_driver.App.mpris()`, `app_states.sh` and `app_library.sh`
go only to the unique D-Bus connection of the process the script started,
which fails once that process is gone, so a run never controls another
instance. Close any other Ferrous, or run the scripts under their own session
bus (`dbus-run-session`), before measuring.

Bring your own audio files; none are included. Name them `<ID>.<ext>` in one
directory; the scenarios use `S01` (stereo 44.1 kHz) and `S05` (5.1 96 kHz),
and `ui_stalls.py` queues `S01`–`S06`. `ui_stalls.py` and `ui_startup.py`
also take a copy of a library database (it is copied into the run directory;
the app may migrate the copy).

## Build

```bash
# The app, with frame pointers for perf call graphs (Fedora's Qt and Mesa
# have them too).
CXXFLAGS="-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer" \
RUSTFLAGS="-C force-frame-pointers=yes" \
FERROUS_UI_BUILD_DIR=$PWD/target/perf-ui/ui-build ./scripts/run-ui.sh --no-run
# The probe.
cmake -S scripts/perf/ui-probe -B target/perf-ui/probe -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build target/perf-ui/probe
```

`FERROUS_PERF_APP` and `FERROUS_PERF_PROBE` point the scripts at other
builds. The probe links Qt's private headers and interposes private-API QRhi
symbols, so build it against the Qt the app runs with.

## The probe (`ui-probe/`)

Loaded with `LD_PRELOAD` and `FERROUS_PROBE_OUT=<dir>`, it writes
`<dir>/probe.log` and reads commands from the FIFO `<dir>/cmd`:

- Per frame of every `QQuickWindow` (`F` lines, from the render thread's
  frame signals): sync and render times, frame wall time, render-thread CPU,
  Mesa/driver-thread CPU, texture and buffer bytes uploaded through
  `QRhiResourceUpdateBatch`, textures created, and draw calls
  (`QRhiCommandBuffer::draw`/`drawIndexed`).
- GUI-thread stalls: `stall on [ms]` starts a watchdog thread that posts an
  event to the GUI thread every millisecond and logs every latency of at
  least `ms` (default 3), plus a histogram at `stall off`.
  `FERROUS_PROBE_STALL=<ms>` starts it with the app. It wakes the GUI thread
  1000 times a second, so leave it off when measuring CPU or wakeups.
- `dirty on` / `dirty off FILE`: which items made each frame, from the
  window's dirty item list at the start of each sync.
- Control: `mark TEXT`, `eval JS` (in the main window's QML context),
  `click X Y`, `drag X0 Y0 X1 Y1 MS`, `move`, `wheel X Y DY`, `key CODE`,
  `type TEXT`, `resize W H`, `minimize`, `restore`, `maximize`,
  `cover [off]` (an opaque window over the app), `setprop NAME PROP NUMBER`
  (on items with that objectName), `winstate`, `dump FILE`
  (item tree with scene geometry), `quit`.

## Scripts

| Script | Measures |
| --- | --- |
| `ui_states.py OUT REP SAMPLES [IDS]` | per visualization state: CPU and wakeups per thread, frames, frame times, uploads, and a dirty-item census |
| `ui_states_table.py OUT` | median [min–max] tables of `ui_states.py` runs |
| `ui_stalls.py OUT REP SAMPLES DB [NAMES]` | GUI-thread stalls during track changes, seeks, mode switches, the fullscreen viewer, resizing, library and queue scrolling, a 26k-track queue, search typing, a hi-res track start and a rescan; `PERF_RECORD=1` adds `perf record` |
| `ui_stalls_table.py RUN...` | median [min–max] stall table |
| `ui_stall_stacks.py RUN [MIN_MS]` | the GUI thread's stacks during each stall of a `PERF_RECORD=1` run |
| `ui_startup.py OUT REP DB [WATCH]` | time to the first frame and GUI stalls in the first 30 s with a large library |
| `ui_flame.py OUT FILE NAME [JS...]` | a flame graph of a steady state (`perf record -e cpu-clock:u --call-graph fp`) |
| `ui_batches.py OUT FILE` | draw calls per frame per state |
| `ui_waveform_zoom.py OUT FILE` | the waveform view whole-track and zoomed in, with a dirty-item census |
| `ui_summarize.py probe.log` | per marked interval: fps, frame times, uploads, draws, stalls |
| `hold_display.py [S]` | holds a screensaver inhibitor; with the display off the compositor sends no frame callbacks and nothing renders |

Check `uptime` first; the scripts wait for a 1-minute load average of at
most 3. Repeat runs (`REP`) and compare medians; use the same build type,
window size and display refresh rate for before and after.
