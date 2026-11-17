# UI capture harness

Takes faithful screenshots and frame-by-frame recordings of the Ferrous UI, measures them, and packs them into
review-sized images with text reports. It drives a Release build of a source tree through the state table in
`states.py` (about 550 grabs and recordings, plus setup steps), one fresh, isolated app per session and theme, in
both Breeze themes, at several window sizes, fonts and scales.

Everything runs headless and private: a nested `kwin_wayland --virtual` (so Qt Quick renders with OpenGL, as on a
desktop), a private D-Bus session, throwaway `HOME` and XDG folders, `fakeaudiosink`, and a generated fixture
library. Nothing opens on the screen, nothing plays aloud, and nothing reaches a Ferrous you are running.

This is an optional developer tool. It is not part of the build, the tests or CI.

## Setup

On Fedora, besides the [build dependencies](../../docs/INSTALL.md#build-dependencies):

```bash
sudo dnf install python3-pillow python3-numpy python3-mutagen python3-dbus \
  ImageMagick ffmpeg-free kwin plasma-integration plasma-breeze-common \
  google-noto-sans-fonts kf6-kiconthemes rsync bc binutils util-linux-core \
  gstreamer1 gstreamer1-plugins-bad-free
```

With RPM Fusion's `ffmpeg` installed, leave out `ffmpeg-free`; either has the encoders.

- Python 3 with Pillow, numpy, mutagen (tags for the large library) and dbus-python (KWin's screenshot and window
  interfaces).
- ImageMagick (`magick`) and ffmpeg with the FLAC, MP3 (`libmp3lame`) and Opus encoders, for the fixtures.
- `kwin_wayland`, `dbus-run-session`, `busctl`; the KDE platform theme (`plasma-integration`), the Breeze colour
  schemes and icons, and Noto Sans, so the app looks as it does on Plasma.
- GStreamer's `fakeaudiosink` (`gstreamer1-plugins-bad-free`) and `gst-launch-1.0` (`gstreamer1`). Every app
  plays to `fakeaudiosink`; the app falls back to a real audio device when it cannot create its sink, so
  `shoot_round.sh` and `driver.py` refuse to start any app unless `gst-launch-1.0` can create and run it.
- `unshare` (user and mount namespaces, for the neutral paths below), `flock`, `rsync`, `bc`, `nm`.
- `g++`, `pkg-config` and the Qt 6 development files for the two probes, and the KF6 IconThemes library, which
  `iconprobe` links.

The probes are built by `build_probes.sh`, which `shoot_round.sh` runs before each capture. They use public Qt API
only and are rebuilt when their source changes or the system Qt does:

- `shotprobe.cpp`: loaded into the app with `LD_PRELOAD`; reads commands from a FIFO (evaluate QML, find, click,
  key, hover, grab, record frames, walk the item tree) and writes replies to a file;
- `iconprobe.cpp`: asks Qt's and KDE's icon lookup which `hicolor` folder they take for each size and device
  pixel ratio.

## Paths and options

| Setting | Default | What it is |
| --- | --- | --- |
| `<source-dir>` (2nd argument) | the checkout that holds this folder | the tree under test |
| `--out DIR` or `SHOT_OUT_ROOT` | `$SHOT_WORK/rounds` | where `<round>/` and `<round>-focus/` go |
| `SHOT_WORK` | `$XDG_CACHE_HOME/ferrous-ui-capture` | source mirror (`src/`), build (`build/`), probes (`probe/`), fixtures (`fixtures/`), app folders and the template library DB (`run/`), bus sockets (`b/`), `tmp-<round>/` |
| `SHOT_BUILD_WRAPPER` | none | a command the compiles run under, e.g. one that holds a build slot or lowers priority |
| `SHOT_BUILD_TAG` | none | a separate mirror and build (`src$TAG`, `build$TAG`), to test the harness beside a real run |
| `SHOT_NEUTRAL=0` | `1` | run the app at the real paths instead of `/mnt/ferrous` |

The icon comparison sheets read candidate icon trees from outside the source, and only when set:
`SHOT_ICON_B` with `SHOT_ICON_AB=1`, `SHOT_ICON_VARIANTS_DIR` with `SHOT_ICON_VARIANT_NAMES` (and
`SHOT_ICON_VARIANTS=1` to require them), `SHOT_ICON_BEFORE`. `SHOT_DIALOG_HOVER=1` adds the dialogs' hovered
footer and sidebar states. See the comments in `states.py`.

## Running

A full capture: every state, both themes, all reports. About 4 to 6 minutes on 4 workers once built. The first run
also builds the app into the work folder (about 2 minutes with a warm compiler cache) and generates the fixtures
(about 8 minutes, mostly the large library); later runs reuse both.

```bash
tools/ui-capture/shoot_round.sh r1                      # this checkout
tools/ui-capture/shoot_round.sh r2 ~/src/ferrous-branch --out /tmp/ui-rounds
```

A focus capture shoots only the sessions one scope of `focus.py` needs and packs only its images, into
`<round>-focus/`; when it passes, it publishes them to `<round>/focus/`. Scopes: `nowplaying-plot`, `chrome`,
`dialogs`, `app-icon`.

```bash
tools/ui-capture/shoot_round.sh r1 --focus-only dialogs   # about 5 minutes
tools/ui-capture/shoot_round.sh r1 --focus dialogs        # the full capture, then the focus pack
```

While fixing one area: `--only AREA[,AREA]`, `--sessions S1,S2` or `--themes dark` re-shoot just those states
into an existing round (wiping nothing) and rebuild its sheets and `INDEX.md`. `--workers N` sets the parallel
sessions (at most 4).

The exit status is not 0 when a state failed (`failures.txt`, 4), a check in the pack failed (a dialog shot not
of its dialog, a missing core image, an image over its size, a home-folder path left in a shot: 3), or
`CONTRAST.md` is incomplete (5).

### What a run does

1. Mirrors the source with `rsync` (no `.git` or build folders) into `$SHOT_WORK/src` and builds the `ferrous`
   target there (Release, `BUILD_TESTING=OFF`). The build always runs; it is a no-op when nothing changed.
2. Builds the probes, generates any missing fixtures, and rescans the fixture library into a template database
   with the new binary when the Rust sources or the fixtures changed.
3. Runs `states.py` with `runner.py` in up to 4 isolated workers, longest sessions first (scheduled from the last
   run's timings), each at `nice 10`.
4. Writes `MOTION.md`, `CONTRAST.md` and `LAYOUT.md`, then `pack.py` builds the sheets, strips and `INDEX.md`.

## Fixtures

Generated, never copied: `make_fixtures.py` (the main library: invented artists, albums and titles, synthesized
tones and noise in FLAC, MP3 and Opus, mono to 7.1, up to 192 kHz, plain covers), `make_extra_fixtures.py` (a short
track, a truncated FLAC, an untagged file), `make_cover_art.py` (generated landscape covers), `make_library2.py`
(a second root), `make_broadband_fixtures.py` (bright, level-moving signals for the plot and meter),
`make_standin_fixtures.py` (tracks without any art) and `make_large_library.py` (320 invented artists for the
density shots). `shoot_round.sh` runs each when its output is missing; delete `$SHOT_WORK/fixtures` to regenerate.

## Output

`<round>/`: `shots/` (every grab at full size, with its `layout/` walk and `index/` entries), `sheets/` (contact
sheets: tiles cropped to their subject, one theme per sheet, sized so each tile stays at least 700 px wide after a
1568 px downscale), `strips/` (one merged image per recorded transition, its frame pages in `strips/frames/`),
`strips-raw/` (every recorded frame and its traces), `timings/`, `run.log`, `worker*.log`, `failures.txt`, and the
reports below. `INDEX.md` lists every image with an ID: the core set first (S01… sheets, M01… transitions, D1…
documents), then everything else as reference.

### Reports

- **`LAYOUT.md`, `layout.json`**: with every grab, the probe walks the item tree of every shown window (main
  window, popups, viewers, dialog windows) and records text clipped by its window, an ancestor or its popup; text
  wider than its box and not elided; items out of bounds; overlapping siblings in layouts; elided text; and each
  visible text's colour, font and whether a modal dim covers it. `layout.py` drops allow-listed findings
  (`states.ELIDE_OK`, `LAYOUT_IGNORE`, `CONTRAST_SKIP`), samples each text's rendered background from the shot for
  WCAG contrast (4.5:1, 3:1 for large text), and groups one line per component and issue with its states, worst
  value and shots. Findings in QML that is not the app's own (Breeze, Qt Quick Controls, Kirigami) go to a
  reference section.
- **`CONTRAST.md`**: the `contrast` session's `measure` steps record each item's rect, declared colour and palette
  role; `contrast.py` samples the grab (background: the most frequent colour; foreground: the pixel of highest
  contrast, a glyph stem's core; focus rings: the band along the rect's edge) and reports WCAG ratios per theme
  beside the declared colour's ratio, with the shot and pixel box of every sample.
- **`MOTION.md`**: every QML animation, Behavior, Transition, Timer and tooltip delay in the source, and the C++
  lines that time visual change, with `file:line` (`motion_inventory.py`).
- **`MOTION-METRICS.md`** and the **strips**: a recorded transition is every frame the window rendered while the
  action ran, with the probe's trace of item opacities, rects and running animations. Each gets one merged image
  (1200 px wide, under the 1568 px downscale) with a caption that keeps apart the latency (input to first change),
  each transition (a run of changes; runs more than 100 ms apart are separate) and the recording; up to 12 key
  frames with real timestamps; a graph of the animated items' opacity, ink per scope (text and other) and the
  runtime animation timeline; and measured flags: blink (ink dipping below half for at most 250 ms), text overlap
  (two texts in one place, one mid-fade), movement after the stated end, and frames that are neither the start nor
  the end nor a blend of them. Key frames are far slower than real time, so each image also has **real-speed rows**,
  labelled as a simulation: each cell is the time-weighted mean of the frames on screen in the 45 ms before it (the
  eye's integration time), 5 moments around the main transition, then 8 cells one 60 Hz frame apart. Some
  recordings also get a whole-region perceived sheet and a smooth-pursuit simulation of a sliding label.
- **`CORE-FACTS.md`**: the core images' measured facts as text (tile states, key colours, layout and contrast rows,
  motion metrics, geometry samples).
- **`FOCUS.md`, `FOCUS-FACTS.md`** (in `<round>/focus/`): the focus pack's images (`F01_…png`) with what each shows
  and what to look at; then, for exactly these images, the same facts as `CORE-FACTS.md` plus the scope's sampled
  facts (`facts.jsonl`: fills, edges, plate and glyph sizes, focus owners), floor regions and accessibility lines.
- **`BUGS.md`**: a stub for non-visual bugs noticed while driving the app.

### Pack limits

The pack is made for reviewers whose image input downscales anything over about 1568 px on the long side or
1.15 MP. A focus pack holds at most 14 images (`pack.py` warns above that), and every image fits 1.15 MP: a 1:1
image as drawn, or the pack fails; the others are fitted to 1.0 MP. The core set in `INDEX.md` is about 20 images
under the same per-image limits.

## Privacy

Only generated fixtures are shown. The app runs in a private user and mount namespace (`neutral_ns.sh`) where the
work folder is bind-mounted at `/mnt/ferrous`, so the folder rows, the art viewer's Folder row and the diagnostics
log name no home folder. As a second guard, the probe records every text that names the real home folder with its
rect, and `pack.py` paints those out of every shot and frame and fails if any remain.

## Known traps

- **Always rebuild.** The build step always runs. Never point `SHOT_APP` at a binary someone else may rebuild
  mid-run, and never skip the build because the mirror looked unchanged: a failed build would leave an older
  binary under a newer source.
- **Don't overlap captures with builds.** A full capture and a focus capture use about 11 GB together over the
  idle baseline; with a build beside them, a 32 GB machine can run out of memory. The recordings are also timing
  measurements, which a busy machine skews. Runs take a lock per build tag (`$SHOT_WORK/.run$TAG.lock`): a second
  run waits for the first. Use `SHOT_BUILD_WRAPPER` to keep the compile step in line with your other builds.
- **Neutral paths need user namespaces.** `neutral_ns.sh` needs unprivileged user and mount namespaces (on by
  default on Fedora); the work folder must not be under `/mnt`, which the namespace covers. Paths outside the work
  folder (such as the source tree) stay real. `SHOT_NEUTRAL=0` runs without; the pack then paints the paths out.
- **Offscreen and private only.** Each app runs in its own `dbus-run-session` with a private bus socket and its
  own XDG folders, so the single-instance handoff and MPRIS never reach a Ferrous you are running; the driver
  records the private bus's names at start, and the template scan refuses to run if a Ferrous or MPRIS name is
  already there. Run it from a terminal session; it unsets `DISPLAY` and `WAYLAND_DISPLAY` for the app and opens
  no window. Use `SHOT_KWIN=1` (the default in `shoot_round.sh`): plain `QT_QPA_PLATFORM=offscreen` uses the
  software renderer, which draws the plots wrong.
- **Fresh template database.** The library database stores absolute paths. It is rebuilt when its key (Rust
  sources, fixtures, the library's path, neutral or not) changes; a session fails loudly if the library's roots
  are not the fixture library.
- **KWin's own window animations** are not in the recordings: they run in the compositor, and KWin's screenshot
  interface manages only about 17 frames a second.

## Files

| File | Role |
| --- | --- |
| `shoot_round.sh` | the entry point: mirror, build, probes, fixtures, template DB, shoot, analyse, pack |
| `build_probes.sh` | builds `shotprobe.so` and `iconprobe` into `$SHOT_WORK/probe` |
| `neutral_ns.sh` | runs a command with the work folder at `/mnt/ferrous` |
| `driver.py` | starts one isolated app (KWin, D-Bus, XDG folders, settings, theme) and talks to the probe |
| `shotlib.py` | a session: grabs, recordings, KWin window captures, the shot index |
| `runner.py` | runs the state table in parallel workers; its docstring lists the step syntax |
| `states.py` | the state table, sessions, macros, sheets, the core set and the checks a state must pass |
| `focus.py` | the focus scopes: sessions, sheets and images with what to look at |
| `layout.py`, `contrast.py`, `motion_strip.py`, `motion_inventory.py` | the analysers |
| `pack.py` | sheets, strips, privacy painting, `INDEX.md`, `CORE-FACTS.md`, the focus pack |
| `setup_db_round.py` | scans the fixture library into the template DB |
| `make_*.py` | the fixture generators |
| `shotprobe.cpp`, `iconprobe.cpp` | the probes |

To add a state, add one line to `states.STATES`: `(session, area, state, steps, description)`, with steps separated
by ` | ` (see `runner.py`). Lines run in order in one app per session and theme; a state starting with `_` is setup
only.
