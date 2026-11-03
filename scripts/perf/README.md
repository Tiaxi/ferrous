# Backend performance harness

Scripts for before/after comparisons of analysis throughput, seek latency,
waveform detail windows and the desktop app's CPU use. They drive
`examples/perf_analysis.rs` and the app built by `scripts/run-ui.sh`. Every
run gets its own `HOME`, `XDG_*_HOME` and `TMPDIR` under `target/perf/runs/`
(see `iso.sh`), so nothing reads or writes the real Ferrous config, data or
cache. `iso.sh` also points `GST_REGISTRY` at `target/perf/gst-registry.bin`:
a fresh registry per run would make GStreamer rescan its plugins on first use
and add most of a second to the first decode of raw AC-3 and DTS files.

Bring your own audio files; none are included. Give each sample an ID and
name the file `<ID>.<ext>` in one directory. A selection file lists them as
tab-separated `id`, `description`, `duration seconds`, `kbps`, `MiB`,
`extension` and `path` for the summaries (`run_nfs.sh` takes `id`,
`description`, `duration` and `path`, and reads files in place).

## Build

```bash
cargo build --release --example perf_analysis
# Profiles: frame pointers, as perf here cannot unwind DWARF call graphs.
CARGO_TARGET_DIR=target/fp RUSTFLAGS="-C force-frame-pointers=yes" \
  cargo build --release --example perf_analysis
FERROUS_UI_BUILD_DIR=$PWD/target/perf/ui-build ./scripts/run-ui.sh --no-run
```

## The driver

`perf_analysis` prints one JSON object per run, with the CPU time of every
thread of the process (thread names with digits folded to `#`).

- `analysis FILE`: an `AnalysisEngine` without playback. Times the overview
  waveform (first and complete) and the spectrogram columns until the
  session reaches the end of the file or parks at its lookahead. Options:
  `--fft`, `--view per-channel|downmix`, `--mode centered|rolling`,
  `--width`, `--zoom` (default 1; at 0.5 a column spans twice the audio).
  `FERROUS_SPECTROGRAM_LOOKAHEAD_SECONDS=100000` lets a centered session
  decode up to the 64 MiB lookahead budget, the whole track for most; a
  lower `--zoom` makes the budget cover a longer track.
  `spectral_hash` is an FNV-1a hash of every chunk's start column, column
  count and levels, in order, to check that a change leaves the output alone.
- `seek FILE`: plays the file through `FrontendBridgeHandle` with the
  GStreamer backend and audio output, then seeks to `--seeks` fractions of
  the track. For each seek: when the engine reported it applied
  (`landed_ms`), when the position advanced again, when the first columns of
  the restarted spectrogram session arrived, and when columns covering the
  target did (`reset: false` means they were decoded already), and, as
  `view_ms`, when every column of a view of `--width` columns around the
  target had (the view a centered spectrogram shows at zoom 1). `--idle`
  waits for the overview and the lookahead before seeking; `--warmup` and
  `--settle` set the waits.
- `window FILE`: waveform detail windows through
  `ferrous_ffi_waveform_window_cancellable`, as the waveform editor asks for
  them: with `--duration`, the whole track twice, then 10 s windows, then
  the whole track again.
- `idle FILE`: plays the file through the FFI bridge, as the Qt app drives
  it, with a thread that takes its output whenever the bridge signals some,
  then pauses and stops it. Reports each thread's voluntary context switches
  per second over `--hold` seconds (default 10) while paused and while
  stopped, after `--warmup` seconds of playback and `--settle` seconds in
  each state. The driver's own `perf-sampler` thread wakes 20 times a second.
  `FERROUS_GST_OUTPUT_SINK=fakeaudiosink` plays in real time without audio.

## Scripts

| Script | Measures |
| --- | --- |
| `run_analysis_matrix.sh OUT REPS DIR [IDS]` | `analysis` cold, in four configurations |
| `run_seek_matrix.sh OUT REPS DIR IDS` | `seek`, idle and busy, centered and rolling; `PERF_BIN`, `PERF_CONFIGS` and `PERF_ARGS` choose the driver, configurations and extra options |
| `run_window.sh OUT SELECTION DIR REPS IDS` | `window`, cold service each run |
| `run_nfs.sh OUT SELECTION` | `analysis` of files in place: first run cold, then warm |
| `app_states.sh OUT REP STEREO SURROUND RAW` | the app's per-thread CPU and wakeups: idle, playing, paused, stopped, driven over MPRIS |
| `app_library.sh OUT DB FILE` | the app's startup rescan of a copied library database, then playback |
| `flame.sh NAME OUTDIR ARGS` | `perf record` of the driver into a folded file and an SVG flame graph (needs `inferno`) |
| `gst_seek_probe.py FILE MODE TARGETS` | seek latency of a bare `playbin3` with ACCURATE, KEY_UNIT or two-step flags |
| `summarize_*.py` | Markdown tables with medians and ranges |
| `threadsample.py PID SECONDS LABEL` | one process's per-thread CPU and context switches |
| `folded_top.py`, `folded_incl.py` | self and inclusive shares in a folded profile |

The app scripts open a visible window and play audio, refuse to start while
another `ferrous` process runs, and register the app's D-Bus names, so a
Ferrous launched meanwhile would forward to it. `app-settings.txt` holds the
display settings they start with.

Run measurements one at a time on an otherwise quiet machine, repeat them,
and compare medians from the same build type.
