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
| `SHOT_BUILD_TAG` | none | a separate mirror and build (`src$TAG`, `build$TAG`), so a test of the harness never shares a binary with a real round; runs of every tag still take turns (see the run lock below) |
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

A focus capture shoots the sessions that the named scopes of `focus.py` need, once (the union of theirs), and
packs only their images, into `<round>-focus/`; when all of them pass, it publishes each to
`<round>/focus/<scope>/`. A scope's folder is the whole of one reviewer task: `FOCUS.md`, `FOCUS-FACTS.md` and the
images. The round's packs of other scopes stay.

```bash
tools/ui-capture/shoot_round.sh r1 --census                      # the seven census packs, one run
tools/ui-capture/shoot_round.sh r1 --focus-only dialogs          # one pack
tools/ui-capture/shoot_round.sh r1 --focus-only viewers,dialogs  # several, one run
tools/ui-capture/shoot_round.sh r1 --focus census                # the full capture, then the census packs
```

`census` stands for the seven census scopes in either list, and `--census` is `--focus-only census`. The census
grades the whole app surface by surface:

| Scope | Areas |
| --- | --- |
| `nowplaying-plot` | now playing; the visualization pane |
| `viz-interaction` | visualization interaction: zoom, the seek bar's view band, pan, autoscroll, follow, seek |
| `chrome` | transport; queue; status bar |
| `library-search` | library; search; menus |
| `dialogs` | dialogs |
| `viewers` | album art and visualization viewers |
| `whole-app` | the app as one system: spacing, type, colour, states, motion, consistency, identity, KDE fit, accessibility |

`app-icon` is a scope outside the census.

While fixing one area: `--only AREA[,AREA]`, `--sessions S1,S2` or `--themes dark` re-shoot just those states
into an existing round (wiping nothing) and rebuild its sheets and `INDEX.md`. `--workers N` sets the parallel
sessions (at most 4).

`--pack-jobs N` sets the worker processes of `pack.py` (default: half the CPU threads, from 2 to 8). Memory grows
with them: at 8 workers a full pack took about 6.6 GB; at 3 it finished in about 4 minutes with no process above
130 MB resident. Beside a build, use `--pack-jobs 3`.

The exit status is not 0 when a state failed (`failures.txt`, 4), a check in the pack failed (a dialog shot not
of its dialog, a missing core image, an image over its size, a home-folder path left in a shot, an image the
privacy check cannot verify: 3), or `CONTRAST.md` is incomplete (5).

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
- **`FOCUS.md`, `FOCUS-FACTS.md`** (in `<round>/focus/<scope>/`): that pack's images (`F01_…png`) with what each shows
  and what to look at; then, for exactly these images, the same facts as `CORE-FACTS.md` plus the scope's sampled
  facts (`facts.jsonl`: fills, edges, plate and glyph sizes, focus owners), floor regions and accessibility lines.
- **`BUGS.md`**: a stub for non-visual bugs noticed while driving the app.

### Pack limits

The pack is made for reviewers whose image input downscales anything over about 1568 px on the long side or
1.15 MP. Every image fits 1.15 MP: a 1:1 image as drawn, or the pack fails; the others are fitted to 1.0 MP.

A focus pack holds at most 14 images, counting the contact sheets of "neither" frames that follow a recording which
has them. Over 14, or with an image over 1.15 MP, `pack.py` fails (exit 3) and a focus-only run publishes nothing,
so a pack cannot grow round by round: an image added to a full scope replaces another. Keep a scope that shows
recordings under 14, so a "neither" sheet has room.

The core set in `INDEX.md` is about 20 images under the same per-image limits. The full capture is for the
automatic checks and whoever leads a review; a reviewer gets one focus pack.

## Privacy

Only generated fixtures are shown. The app runs in a private user and mount namespace (`neutral_ns.sh`) where the
work folder is bind-mounted at `/mnt/ferrous`, so the folder rows, the art viewer's Folder row and the diagnostics
log name no home folder. As a second guard, the probe records every text that names the real home folder with its
rect, and `pack.py` paints those out of every shot and frame and fails if any remain.

The check fails closed: an image without the evidence it reads is unverified, and an unverified image fails the
pack like a path left in a shot (exit 3; a focus-only run then publishes nothing). The evidence is:

- for a shot, its layout walk, so `SHOT_LAYOUT=0` fails the check;
- for a composite (an image a macro cuts from grabs that have no entry of their own, such as the 1:1 sheets), the
  layout walk of every grab it was cut from, none of which may name the home folder, since a composite cannot be
  painted. Every image a macro saves lists all the grabs and shots that macro has used so far, so each image cut
  from one grab lists it;
- for an image drawn without app pixels (the icon sheets, drawn from SVGs), the marker `no_app_pixels`, which a
  macro sets with `draws_without_app_pixels()`. A macro that saves an image from no grab without it fails its
  state, and an index entry with neither sources nor the marker fails the check;
- for a recording, its item trace, and the neutral paths: the trace records where items are, not their text, so
  frames recorded with `SHOT_NEUTRAL=0` cannot be checked. A run at the real paths therefore fails the check
  whenever it records a transition. A menu drawn into a recording's frames needs its items in the trace too.

## Known traps

- **Always rebuild.** The build step always runs. Never point `SHOT_APP` at a binary someone else may rebuild
  mid-run, and never skip the build because the mirror looked unchanged: a failed build would leave an older
  binary under a newer source.
- **Don't overlap captures with builds.** A full capture and a focus capture use about 11 GB together over the
  idle baseline; with a build beside them, a 32 GB machine can run out of memory. The recordings are also timing
  measurements, which a busy machine skews. Runs take one lock per work folder (`$SHOT_WORK/.run.lock`), whatever
  their build tag: a second run waits for the first. Every tag shares the probes, the fixtures, the template
  database, the setup app and the bus sockets, so runs that must go side by side need separate work folders
  (`SHOT_WORK`). Use `SHOT_BUILD_WRAPPER` to keep the compile step in line with your other builds.
- **Neutral paths need user namespaces.** `neutral_ns.sh` needs unprivileged user and mount namespaces (on by
  default on Fedora); the work folder must not be under `/mnt`, which the namespace covers. Paths outside the work
  folder (such as the source tree) stay real. `SHOT_NEUTRAL=0` runs without; the pack then paints the paths out
  of the shots, but it cannot check recordings, so its privacy check fails when there are any (see Privacy).
- **The probe's input goes to the app's windows**: the middle button (`mpress`, `mdmove`, `mrelease`, `mclick`),
  the pointer leaving the window (`leave`), keys, held keys (`keydown`, `keyup`). Such a key is past Qt's shortcut
  map, so the focused item gets it: where a shortcut must take it first (the search palette's Menu key), use
  `kkey`, a real key through KWin. The pointer's cursor is KWin's, so no grab or recording shows it.
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
- **Menus are windows of their own** (#642). KWin opens such a popup window only for a client that has had input
  from the compositor, and the probe's events go to the app's windows directly. So each session first presses
  Shift once through KWin's fake-input protocol (`kwin_input.py`; the driver's `.desktop` file lets this Python
  use it). After that the probe sends a key to the open menu's window, and a pointer event to the menu window
  under it, as KWin would.
- **A main-window grab draws in its open menus**, each at its place, and grows right and down for a menu that
  reaches past the window, mid grey there. The menus' KWin shadows are not in it. A main-window strip does the same
  for every frame (`popup_frames.py`): the probe records each menu window's frames and notes where it shows, moves
  and hides, and its item trace has the menus' items at their place in the main window, so the privacy check covers
  them. A menu is cut at the strip's crop; without one the frames grow for it. A `win=` strip records its window
  alone.
- **Scale 2 is KWin's output scale**, as on Plasma: the driver writes the session's `kwinoutputconfig.json` (KWin's
  `--scale` alone sizes the virtual output but leaves its scale at 1), and the app gets no Qt scale variable. With
  `QT_SCALE_FACTOR` instead, QtWayland would open a menu window at half its offset from the window (#665). KWin's
  coordinates (window frames, area captures) are then logical px like the app's, and its captures, its decorations
  and every grab are in device px. A session fails when its window's device pixel ratio is not its scale.

## Files

| File | Role |
| --- | --- |
| `shoot_round.sh` | the entry point: mirror, build, probes, fixtures, template DB, shoot, analyse, pack |
| `build_probes.sh` | builds `shotprobe.so` and `iconprobe` into `$SHOT_PROBE_DIR` (default `$SHOT_WORK/probe`), where `driver.py` loads both from |
| `neutral_ns.sh` | runs a command with the work folder at `/mnt/ferrous` |
| `driver.py` | starts one isolated app (KWin, D-Bus, XDG folders, settings, theme) and talks to the probe |
| `kwin_input.py` | sends real input through the session's KWin (its fake-input protocol), with no Wayland library |
| `shotlib.py` | a session: grabs, recordings, KWin window captures, the shot index |
| `runner.py` | runs the state table in parallel workers; its docstring lists the step syntax |
| `states.py` | the state table, sessions, macros, sheets, the core set and the checks a state must pass |
| `focus.py` | the focus scopes and the census: sessions, sheets and images with what to look at |
| `popup_frames.py` | draws a main-window strip's menus into its frames |
| `layout.py`, `contrast.py`, `motion_strip.py`, `motion_inventory.py` | the analysers |
| `pack.py` | sheets, strips, privacy painting, `INDEX.md`, `CORE-FACTS.md`, the focus packs |
| `setup_db_round.py` | scans the fixture library into the template DB |
| `make_*.py` | the fixture generators |
| `shotprobe.cpp`, `iconprobe.cpp` | the probes |

`test_*.py` are unit tests of the harness that need no KWin, app or shots:
`python3 -m unittest discover -s tools/ui-capture -p 'test_*.py'`.

To add a state, add one line to `states.STATES`: `(session, area, state, steps, description)`, with steps separated
by ` | ` (see `runner.py`). Lines run in order in one app per session and theme; a state starting with `_` is setup
only.
