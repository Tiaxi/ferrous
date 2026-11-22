#!/usr/bin/env python3
"""Focus packs: one surface reviewed at a time from a small pack. A scope names the sessions it
needs (for `shoot_round.sh --focus-only`), its own sheets (built like states.CUSTOM_SHEETS) and its images in order:
each with what it shows and what to look at, and the facts (facts.jsonl keys, floor regions) FOCUS-FACTS.md carries.
`shoot_round.sh <round> <src> --focus SCOPE[,SCOPE...]` writes <round>/focus/<scope>/ (FOCUS.md, FOCUS-FACTS.md, the
images) for each after the whole pack; `--census` shoots and packs the CENSUS scopes alone. Add a scope here.

A pack holds at most pack.FOCUS_MAX_IMAGES (14) images, each within 1.15 MP: over either, the pack fails. So a
scope leaves room for the "neither" sheets below, and an image added to a full scope replaces another. Every sheet
of a scope is one of its images, and its tiles and recordings come from the scope's own sessions (test_focus.py).

An image: ("sheet", NAME, THEME, shows, look) with NAME a focus sheet (below) or any pack sheet group, or
("strip", NAME, THEME, shows, look) with NAME a recording, a pair ("pair:..."), or a perceived sheet. A strip
whose recording has frames that are neither its start nor its end gets their raw-frame contact sheet as an extra
image right after it.
"""
import os

# the first icon variant compared (states.ICON_VARIANTS; its mark sheet is made only with SHOT_ICON_VARIANTS=1)
_VAR = (os.environ.get("SHOT_ICON_VARIANT_NAMES") or "variant").split(",")[0]

_LEFT, _RIGHT = (0, 0, 330 / 1239, 1), (1 - 170 / 1239, 0, 170 / 1239, 1)  # the pane's left and right parts
_R560 = (1 - 520 / 1239, 0, 520 / 1239, 1)  # the pane's right 520 px: the meter and the mode switch

# the 24 pt dialog tiles (the 1600x1000 session at 24 pt) and the iTunes searching -> failed frames: the
# status block of the 920x604 Replace window (the spinner and its text, then the warning and its two lines). The
# search starts about 3 ms after the action; the busy indicator's running goes false -> true as the real bridge is
# given (it was the stand-in's), so its fade-in starts over: a trial run drew one frame (+19 ms) of the text alone
# before the failure (+36 ms), as a search started from rest does
B24 = dict(variant="1600x1000_font24", lh=16, gap=6)
ITUNES_FAIL_BAND = (300, 200, 320, 125)
ITUNES_FAIL_TIMES = [-10, 22, 40, 150]

_PANE = (0.2256, 0.544, 0.7744, 0.431)  # the pane's place in a 1600x1000 window (fractions)

SCOPES = {
    "nowplaying-plot": dict(
        title="Now playing and the visualization pane",
        sessions=["main", "playing", "bigfont", "narrow", "narrow-bigfont", "motion", "firstplay-noart", "contrast"],
        # now playing: the cards in every layout and font at 1:1, the idle row; First Play and art -> no art. The
        # pane: at rest and playing, spectrogram, waveform and mixed, lanes to 5.1, the meter, the scale, the mode
        # switch and its motion (its interaction, zoom and pan, is viz-interaction's)
        sheets=[
            ("focus-np-wide", "Now playing, wide (1600x1000), 1:1 with the window around it: album art and no art (the "
             "no-art mark), and the library's no-art row (1:1, 4x); dark above light", "dark",
             [[("nowplaying", "at-rest", "subject", f"Art, {th}", dict(theme=th, margin=6, lh=16, gap=6)),
               ("nowplaying", "no-cover", "subject", f"No art, {th}", dict(theme=th, margin=6, lh=16, gap=6)),
               ("details", "library-noart-glyph", None, f"Library no-art row, {th}", dict(theme=th, max_w=300, lh=16, gap=6))]
              for th in ("dark", "light")]),
            ("focus-np-24pt-compact", "Now playing at 24 pt, 1600x1000 (the compact sidebar), 1:1: a short, a medium and a "
             "long title and a tag-less file; dark above light (the title's lines and elision: FOCUS-FACTS card-title-*)", "dark",
             [[("nowplaying", f"title-{k}", "subject", f"{lb}, {th}", dict(variant="1600x1000_font24", theme=th, margin=4, lh=16, gap=6))
               for k, lb in [("short", "one-line title"), ("medium", "two-line title"), ("long", "long title"), ("untagged", "tag-less file")][i:i + 2]] for th in ("dark", "light") for i in (0, 2)]),
            ("focus-np-narrow", "Narrow window (1000x720), default font, playing, 1:1: with art and without; dark above "
             "light", "dark",
             [[("nowplaying", "at-rest", "subject", f"Art, {th}", dict(variant="1000x720", theme=th, margin=6, lh=16, gap=6)),
               ("nowplaying", "no-cover", "subject", f"No art, {th}", dict(variant="1000x720", theme=th, margin=6, lh=16, gap=6))]
              for th in ("dark", "light")]),
            # the idle row (its glyph changes this round) at 10 pt and 24 pt, wide and narrow
            ("focus-np-idle", "Now playing idle (\"Nothing playing\"), 1:1 with the window around it: wide and narrow at "
             "10 pt, compact (1600x1000) and narrow (1000x720) at 24 pt; dark above light", "dark",
             [[("nowplaying", "idle-row", "subject", f"{lb}, {th}", dict(variant=v, theme=th, margin=8, lh=16, gap=6))
               for v, lb in (("1600x1000", "wide 10 pt"), ("1000x720", "narrow 10 pt"))]
              for th in ("dark", "light")]
             + [[("nowplaying", "idle-row", "subject", f"{lb}, {th}", dict(variant=v, theme=th, margin=8, lh=16, gap=6))
                 for v, lb in (("1600x1000_font24", "compact 24 pt"), ("1000x720_font24", "narrow 24 pt"))]
                for th in ("dark", "light")]),
            # the pane: each tile the pane as the app lays it out (1239 px wide at 1600x1000), cut where noted
            ("focus-pane-spectrogram", "The pane playing, 1:1 (its left 88 %): the centred spectrogram in dark, the waveform in "
             "light (the spectrogram is dark in both themes)", "dark",
             [[("plot", "at-rest", "subject", "Centred spectrogram, per channel, dark", dict(part=(0, 0, 0.88, 1), lh=16, gap=6))],
              [("plot", "floor-waveform", "subject", "Waveform, per channel, light", dict(theme="light", part=(0, 0, 0.88, 1), lh=16, gap=6))]]),
            ("focus-pane-meter", "The pane's right end at 1:1 with the level meter on; the frequency scale", "dark",
             [[("plot", "spectrogram-levelmeter", "subject", "Spectrogram, meter on", dict(part=(0.54, 0, 0.46, 1), lh=16, gap=6)),
               ("main", "playing-scale-levelmeter", (0.6444, 0.544, 0.3556, 0.431), "The frequency scale too", dict(lh=16, gap=6))],
              [("plot", "waveform-levelmeter", "subject", f"Waveform, meter on, {th}", dict(theme=th, part=(0.54, 0, 0.46, 1), lh=16, gap=6))
               for th in ("dark", "light")]]),
            ("focus-pane-lanes", "5.1 with the meter on, 1:1: pointer over the pane (dark); away (light, the top four lanes)", "dark",
             [[("plot", "5.1-levelmeter-lanes", "subject", "5.1, meter on, pointer over the pane, dark", dict(lh=16, gap=6))],
              [("plot", "5.1-meter-rest", "subject", "5.1, meter on, pointer away, light",
                dict(theme="light", part=(0, 0, 1, 0.67), lh=16, gap=6))]]),
            ("focus-pane-modes", "The pane's other modes at half size: rolling; mixed (a 5.1 downmix, one lane); the frequency "
             "scale with the meter; the waveform; 7.1 (eight lanes); before any audio; dark", "dark",
             [[("main", st, _PANE, lb, dict(fit=(600, 220), lh=16, gap=6)) for st, lb in row]
              for row in ((("playing-spectrogram-rolling", "Rolling spectrogram"), ("playing-5.1-downmix", "Mixed view (5.1 downmix)")),
                          (("playing-scale-levelmeter", "Frequency scale and meter"), ("playing-waveform", "Waveform per channel")))]
             + [[("main", "playing-7.1-spectrogram", _PANE, "7.1, eight lanes", dict(fit=(600, 220), lh=16, gap=6)),
                 ("plot", "floor-empty-lanes", "subject", "Before any audio, per channel", dict(fit=(600, 220), lh=16, gap=6))]]),
            ("focus-pane-hover", "The pane's controls at 1:1: crosshair and lane heads; the mode switch hovered (tooltip); "
             "the zoom readout; a 5.1 lane's Mute hovered (tooltip); dark", "dark",
             [[("plot", "hover-crosshair", "subject", "Crosshair and lane heads", dict(part=(0, 0, 0.5, 0.6), lh=16, gap=6)),
               ("plot", "mode-switch-hover", (0.78, 0.49, 0.22, 0.17), "Mode switch: Waveform hovered", dict(lh=16, gap=6))],
              [("plot", "zoomed-hover", "subject", "Zoomed 4 notches: the zoom readout", dict(part=(0.55, 0, 0.45, 0.4), lh=16, gap=6)),
               ("plot", "5.1-lane-mute-hover", "subject", "5.1 lane head: Mute hovered", dict(part=(0, 0, 0.5, 0.4), lh=16, gap=6))]]),
        ],
        images=[
            ("sheet", "focus-np-wide", "dark", "The wide now playing in the window, with album art and without (the "
             "no-art mark), and the library's no-art row, both themes, 1:1", "the no-art mark among the card's text; "
             "the 20 px tile against the cover's mark"),
            ("sheet", "focus-np-24pt-compact", "dark", "24 pt, 1600x1000: short, medium and long titles, a tag-less file, "
             "both themes, 1:1", "the title beside the cover on one or two lines, then elided; the lines below"),
            ("sheet", "focus-np-narrow", "dark", "The narrow window at the default font, with and without art, both themes, 1:1",
             "the narrow card at the default font: the cover filling its block, the lines; without art the no-art mark"),
            ("sheet", "focus-np-idle", "dark", "The idle row at 10 pt and 24 pt, wide and narrow, both themes, 1:1",
             "the idle row's glyph and text at both fonts"),
            ("strip", "pair:first-play-themes", "dark", "First Play from idle, dark (left) and light (right)",
             "the idle row gone before the cover; blink per element and content cuts in the facts"),
            ("strip", "pair:art-to-no-art-themes", "dark", "Album art to none (Next), wide, dark (left) and light (right)",
             "the no-art mark replacing the cover; the text block's lines; text overlap per line"),
            ("sheet", "focus-pane-spectrogram", "dark", "The pane playing: the spectrogram (dark), the waveform (light), 1:1",
             "the lanes, their seam and L/R plates, the floor colour, the playhead"),
            ("sheet", "focus-pane-meter", "dark", "The pane's right end: meter and frequency scale; the waveform in both themes, 1:1",
             "the meter's scale and marks, its seam against the plot, the waveform's lanes"),
            ("sheet", "focus-pane-lanes", "dark", "5.1: six lanes with the meter, pointer over (dark) and away (light), 1:1",
             "the lane heads and M/S buttons, their spacing, the meter's six bars"),
            ("sheet", "focus-pane-modes", "dark", "Rolling, mixed, scale with meter, waveform, 7.1, before any audio (half size)",
             "the pane as one family across its modes: labels, seams, plates, the rest lines"),
            ("sheet", "focus-pane-hover", "dark", "The crosshair, the mode switch's tooltip, the zoom readout, a lane's Mute tooltip, 1:1",
             "the plates' padding and type against the plot; the tooltips' place"),
            ("strip", "pair:mode-switch", "dark", "The mode switch, waveform to spectrogram (left) and back (right), per channel",
             "the plot's drawn content through the switch: empty lanes or a half-drawn frame (neither frames below)"),
        ],
        facts=["noart-mark", "library-noart-glyph", "info-toggle-edge", "card-", "plates-", "modeswitch", "meter-", "rest-lines-",
               "lanes-rest", "lane-heads-a11y"],
        # recordings measured but not shown: their facts in FOCUS-FACTS
        facts_strips=[("title-change-narrow24", "dark", "Medium -> long title, narrow window at 24 pt",
                       dict(sizes=[("the title", "nowPlayingNarrowTitle"), ("the panel", "nowPlayingPanel")])),
                      ("first-play-no-art", "dark", "First Play from idle without album art, dark"),
                      ("first-play-no-art", "light", "First Play from idle without album art, light"),
                      ("no-art-to-art", "dark", "No album art to art, wide"),
                      ("track-change-metadata", "dark", "Track changes across metadata cards, wide"),
                      ("track-change-metadata-24pt", "dark", "Track changes across metadata cards, 24 pt compact"),
                      ("track-change-metadata-narrow24", "dark", "Track changes in the narrow window at 24 pt (header travel)"),
                      ("stop-to-idle", "dark", "Stop, then Clear the queue (0.6 s later)", dict(watch=[("the idle row", "nowPlayingIdleRow"), ("the track's lines", "nowPlayingLines"), ("the cover", "nowPlayingCover")])),
                      ("stop-to-idle", "light", "Stop, then Clear the queue (0.6 s later), light", dict(watch=[("the idle row", "nowPlayingIdleRow"), ("the track's lines", "nowPlayingLines"), ("the cover", "nowPlayingCover")])),
                      ("stop-to-idle-narrow24", "dark", "Stop, then Clear, narrow window at 24 pt", dict(watch=[("the idle row", "nowPlayingIdleRow"), ("the track's lines", "nowPlayingLines"), ("the cover", "nowPlayingCover")])),
                      ("queue-end-to-idle", "dark", "The queue running out on its own", dict(watch=[("the idle row", "nowPlayingIdleRow"), ("the track's lines", "nowPlayingLines"), ("the cover", "nowPlayingCover")])),
                      ("mode-switch-5.1", "dark", "Mode switch to the waveform, 5.1 per channel (six lanes)"),
                      ("mode-switch-mixed", "dark", "Mode switch to the waveform in the mixed view, a later switch"),
                      ("mode-switch-mixed-first", "dark", "The first mode switch to the waveform in the mixed view"),
                      ("rolling-scroll", "dark", "The rolling spectrogram scrolling (0.5 s)"),
                      ("centered-scroll", "dark", "The centred spectrogram scrolling (0.5 s)"),
                      ("meter-motion-spectrogram", "dark", "The level meter moving on music-like levels, spectrogram"),
                      ("plates-reveal", "dark", "The pointer entering the pane: the plates revealed")],
        floors=None,  # (every floor region measured)
        a11y_labels=[],
        # CONTRAST.md rows quoted under each focus sheet (by element-name fragment, case ignored)
        contrast={"focus-np-wide": ["now-playing", "library row title", "library header"],
                  "focus-np-24pt-compact": ["now-playing", "library"],
                  "focus-np-narrow": ["now-playing"], "focus-np-idle": ["now-playing"],
                  "focus-pane-spectrogram": ["lane", "plate", "plot"], "focus-pane-meter": ["meter", "waveform"],
                  "focus-pane-lanes": ["lane", "meter", "5.1"], "focus-pane-hover": ["plate", "mode switch", "readout", "tooltip"]},
    ),
    # The chrome: the transport bar, the status bar, the queue (header and rows), at 1:1 in context, both
    # themes; the measured contrasts of each text, glyph and hover/selected state (CONTRAST.md rows), the status bar's
    # items while numbers change (status-jumps), and the recordings' handover and content-cut facts
    "chrome": dict(
        title="The chrome: the transport, the queue and the status bar",
        sessions=["main", "queue", "empty", "bigfont", "narrow", "narrow-bigfont", "motion", "contrast", "transport",
                  "statusbar"],
        # the queue: its states, focus, play states, motion; the transport: its buttons, seek bar and volume in
        # every state; the status bar: its indicators, Last.fm states and popups
        sheets=[
            (f"focus-queue{'' if th == 'dark' else '-light'}", f"The queue, {th}, 1:1 (its header and first rows): at rest "
             f"(the playing row in amber), a row hovered, a row selected, rows 3-5 selected (Shift+Down), a row pressed", th,
             [[("queue", "at-rest", "subject", "At rest (row 1 playing)", dict(variant="1600x1000", theme=th, part=(0, 0, 1, 0.28), lh=16, gap=6))],
              [("queue", "hover", "subject", "Row 5 hovered", dict(variant="1600x1000", theme=th, part=(0, 0, 1, 0.34), lh=16, gap=6))],
              [("queue", "selected", "subject", "Row 3 selected", dict(variant="1600x1000", theme=th, part=(0, 0, 1, 0.23), lh=16, gap=6))],
              [("queue", "multi-selection", "subject", "Rows 3-5 selected with Shift+Down (the list focused)",
                dict(variant="1600x1000", theme=th, part=(0, 0, 1, 0.34), lh=16, gap=6))],
              [("queue", "pressed", "subject", "Row 4 pressed (the pointer held down)", dict(theme=th, part=(0, 0, 1, 0.28), lh=16, gap=6))]])
            for th in ("dark", "light")
        ] + [
            (f"focus-queue-hover{'' if th == 'dark' else '-light'}", f"The queue, {th}, 1:1: hover on a selected row; the "
             f"inactive selection (the library has focus) beside a hovered row; the playing row hovered, selected, and both; "
             f"a row selected, ringed by keyboard focus and hovered", th,
             [[("queue", "hover-selected", "subject", "Row 3 selected and hovered", dict(theme=th, part=(0, 0, 1, 0.23), lh=16, gap=6))],
              [("queue", "inactive-hover", "subject", "Row 3 selected, the list unfocused (inactive selection); row 5 hovered",
                dict(theme=th, part=(0, 0, 1, 0.34), lh=16, gap=6))],
              [("queue", "hover-now", "subject", "The playing row hovered", dict(theme=th, part=(0, 0, 1, 0.17), lh=16, gap=6))],
              [("queue", "now-selected", "subject", "The playing row selected", dict(theme=th, part=(0, 0, 1, 0.17), lh=16, gap=6))],
              [("queue", "hover-now-selected", "subject", "The playing row selected and hovered",
                dict(theme=th, part=(0, 0, 1, 0.17), lh=16, gap=6))],
              [("queue", "focus-selected-hover", "subject", "Row 2 selected by keyboard (Tab, Down: the ring) and hovered",
                dict(theme=th, part=(0, 0, 1, 0.17), lh=16, gap=6))]])
            for th in ("dark",)
        ] + [
            ("focus-queue-focus", "The queue's keyboard focus, reached with real keys, 1:1: in the long list scrolled to its "
             "end (Tab, End, Up: row 49 of 50), beside the scrollbar's thumb (the list's lower part); in a queue of 4 rows "
             "with no scrollbar (row 3 playing; Tab, Down: unreadable row 4); dark, then light", "dark",
             [[("queue", "focus-scrolled", "subject", f"Long list, scrollbar shown, {th}", dict(theme=th, part=(0, 0.612, 1, 0.388), lh=16, gap=6))]
              for th in ("dark", "light")]
             + [[("queue", "focus-short", "subject", f"4 rows, no scrollbar, {th}", dict(theme=th, part=(0, 0, 1, 0.29), lh=16, gap=6))]
                for th in ("dark", "light")]),
            ("focus-queue-playstate", "The queue, 1:1: the playing row paused (amber, by design) and stopped (no amber; the "
             "track still loaded); unreadable row 4 just after it failed (dimmed at once; its warning mark while still the "
             "current row) and once row 3 plays; dark, then light", "dark",
             [[("queue", st, "subject", f"{lb}, {th}", dict(theme=th, part=pt, lh=16, gap=6))]
              for th in ("dark", "light")
              for st, lb, pt in (("paused-row", "Paused", (0, 0, 1, 0.17)), ("stopped-row", "Stopped", (0, 0, 1, 0.17)),
                                 ("unreadable", "4-row queue: row 4 unreadable, just failed (the current row)", (0, 0.1665, 1, 0.1235)),
                                 ("unreadable-later", "Row 4 unreadable, row 3 playing", (0, 0.1665, 1, 0.1235)))]),
            # the recordings at 1:1 frame by frame (a pair fitted to 1 MP showed them at a fifth of their size)
            ("focus-queue-now", "Next: the queue's playing row (scrolled to the list's second row first) moving down one "
             "row; the list's first three rows at 1:1, frame by frame; dark (top three rows), light (bottom three)", "dark",
             [[dict(strip="queue-now-move", theme=th, item="mainQueuePane:0,0.05,0.5,0.18", times=ts, zoom=1, pad=0, lh=16,
                    gap=6, label=f"{th}: about " + ", ".join(f"{t:+d} ms" for t in ts))]
              for th in ("dark", "light") for ts in ([-30, 15], [40, 70], [110, 250])]),
            # the transport: the left part (the buttons) and the right part (seek bar, volume), 1:1
            ("focus-transport-buttons", "The transport's buttons at 1:1: play states; hover, press, "
             "keyboard focus, with tooltips; dark beside light", "dark",
             [[("transport", st, (0, 0.026, 0.22, 0.078), f"{lb}, {th}", dict(theme=th, lh=16, gap=6))
               for st, lb in row for th in ("dark", "light")]
              for row in ((("playing", "Playing"), ("paused", "Paused")),
                          (("stopped", "Stopped"), ("previous-hover-tooltip", "Previous hovered")),
                          (("playPause-pressed", "Play/Pause pressed"), ("stop-focus", "Stop focused")),
                          (("next-focus", "Next focused"),))]),
            ("focus-transport-seek", "The seek bar and volume at 1:1: hover and its time box, drag, "
             "focus; mute and volume states; dark", "dark",
             [[("seek", st, (0.094, 0, 0.906, 0.074), lb, dict(lh=16, gap=6))]
              for st, lb in (("hover", "Seek bar hovered at 62 %: the time box"), ("hover-start", "Hovered near its start"),
                             ("dragging", "Dragged"), ("focus", "Keyboard focus"))]
             + [[("volume", st, (0.82, 0, 0.18, 0.1), lb, dict(lh=16, gap=6)) for st, lb in row]
                for row in ((("mute-hover", "Mute hovered"), ("mute-focus", "Mute focused"), ("muted", "Muted")),
                            (("slider-hover", "Volume hovered"), ("slider-focus", "Volume focused"), ("dragging", "Volume dragged")))]),
            # the drag frame by frame (the merged strip of so wide and flat a recording shows it a few pixels high)
            ("focus-transport-drag", "The seek bar dragged, frame by frame at 1:1: its time box appearing, following, "
             "then the hover's after the release; dark above light", "dark",
             [[dict(strip="seek-drag-box", theme=th, item=(470, 0, 360, 80), times=ts, zoom=1, pad=0, lh=16, gap=6,
                    label=f"{th}: about " + ", ".join(f"{t:+d} ms" for t in ts))]
              for th in ("dark", "light") for ts in ([-20, 60, 280, 440], [700, 820, 1200, 2000])]),
            ("focus-transport-sizes", "The transport at 24 pt and in the narrow window (1000x720), 1:1", "dark",
             [[("transport", "at-rest", "subject", f"24 pt, 1600x1000, {th}: left",
                dict(variant="1600x1000_font24", theme=th, part=(0, 0, 0.42, 1), lh=16, gap=6)),
               ("transport", "at-rest", "subject", "right",
                dict(variant="1600x1000_font24", theme=th, part=(0.5, 0, 0.5, 1), lh=16, gap=6))] for th in ("dark", "light")]
             + [[("transport", "at-rest", "subject", "Narrow (1000x720), 10 pt, dark", dict(variant="1000x720", lh=16, gap=6))],
                [("main", "playing-spectrogram-centered", (0, 0.07, 1, 0.11), "Narrow (1000x720), 24 pt, dark",
                  dict(variant="1000x720_font24", lh=16, gap=6))]]),
            # the status bar: its right half (the indicators) per state, dark beside light
            ("focus-statusbar", "The status bar at 1:1, its left and right ends, per state: dark, then the "
             "error states in light", "dark",
             [[("statusbar", st, (0, 0.975, 0.35, 0.025), lb + ("" if th == "dark" else ", light"), dict(theme=th, lh=16, gap=6)),
               ("statusbar", st, (0.65, 0.975, 0.35, 0.025), "", dict(theme=th, lh=16, gap=6))]
              for th, rows_ in (("dark", (("playing", "Playing"), ("repeat-one-shuffle", "Repeat One, Shuffle"),
                                          ("repeat-all", "Repeat All"), ("issues-new", "A new issue"), ("issues-seen", "Issue seen"),
                                          ("issues-focus", "Issues focused"), ("scanning-known", "Scanning (known size)"),
                                          ("scanning-unknown", "Scanning (unknown size)"),
                                          ("backend-stopped", "Playback engine stopped"),
                                          ("backend-disconnected", "Playback engine not running"))),
                                ("light", (("playing", "Playing"), ("issues-new", "A new issue"),
                                           ("backend-stopped", "Playback engine stopped"))))
              for st, lb in rows_]),
            ("focus-statusbar-lastfm", "The status bar's Last.fm indicator, 1:1 (the bar's right 40 %): listening 0 %, half, "
             "almost; sending, scrobbled, not scrobbled, queued offline, failed, paused; dark beside light", "dark",
             [[("statusbar", f"lastfm-{k}", (0.6, 0.975, 0.4, 0.025), f"{k.replace('-', ' ')}, {th}", dict(theme=th, lh=16, gap=6))
               for th in ("dark", "light")]
              for k in ("listening-0", "listening-half", "listening-almost", "sending", "scrobbled", "not-scrobbled",
                        "queued-offline", "failed", "paused")]),
            ("focus-statusbar-popups", "The status bar's popups at 1:1: tooltips, Last.fm's menu, "
             "Recent Issues; dark", "dark",
             [[("statusbar", "backend-stopped-hover", (0, 0.915, 0.32, 0.085), "Engine stopped, hovered: the reason", dict(lh=16, gap=6)),
               ("statusbar", "lastfm-failed-tooltip", (0.68, 0.915, 0.32, 0.085), "Last.fm failed: the reason", dict(lh=16, gap=6))],
              [("statusbar", "lastfm-listening-half-tooltip", (0.82, 0.93, 0.18, 0.07), "Last.fm tooltip", dict(lh=16, gap=6)),
               ("statusbar", "issues-hover", (0.82, 0.93, 0.18, 0.07), "Issues hovered", dict(lh=16, gap=6)),
               ("menu", "context-scrobble-indicator", "popup", "Last.fm menu (Shift+F10)", dict(margin=24, lh=16, gap=6))],
              [("dialog", "recent-errors", "popup", "Recent Issues", dict(margin=12, lh=16, gap=6))]]),
        ],
        images=[
            ("sheet", "focus-queue", "dark", "The queue, dark, 1:1: rest, hover, selected, multi-selection, pressed",
             "the header; the playing row's amber; the hover tint; selection; a range; the pressed row"),
            ("sheet", "focus-queue-light", "light", "The queue, light, 1:1: the same", "the same in light"),
            ("sheet", "focus-queue-hover", "dark", "Hover on selected and playing rows; the inactive selection; dark",
             "whether a selected row answers hover; the inactive selection; the playing row selected"),
            ("sheet", "focus-queue-focus", "dark", "Keyboard focus (real keys) beside the scrollbar and with none",
             "the ring's right edge against the scrollbar's lane and thumb (edges in FOCUS-FACTS' table)"),
            ("sheet", "focus-queue-playstate", "dark", "The playing row paused and stopped; an unreadable row",
             "pause in amber (by design), stop without; the unreadable row's warning glyph and dimmed text"),
            ("sheet", "focus-queue-now", "dark", "Next: the playing row moving down a row, at 1:1 frame by frame, dark and light",
             "the amber marker's move between rows (its handover in the facts)"),
            ("sheet", "focus-transport-buttons", "dark", "The transport's buttons: play states, hover with tooltip, pressed, focus",
             "the glyphs' weight and alignment; hover, press and focus; the tooltip's place"),
            ("sheet", "focus-transport-seek", "dark", "The seek bar and volume in every state, 1:1",
             "the time box against the bar's ends; the drag; the focus rings; the muted glyph"),
            ("sheet", "focus-transport-sizes", "dark", "The transport at 24 pt and in the narrow window", "what crowds or elides"),
            ("sheet", "focus-transport-drag", "dark", "The seek bar dragged, frame by frame: the time box, both themes",
             "the box's fade and its place over the playhead; the handover to the hover's box on release"),
            ("sheet", "focus-statusbar", "dark", "The status bar's indicators per state, both themes, 1:1",
             "the indicators' order, spacing and weight; the warning and error states"),
            ("sheet", "focus-statusbar-lastfm", "dark", "Last.fm's indicator in every state, both themes, 1:1",
             "the ring and its glyph per state; which states read as trouble"),
            ("sheet", "focus-statusbar-popups", "dark", "The status bar's tooltips, the Last.fm menu, Recent Issues, 1:1",
             "where each opens against the bar; their width and wording"),
        ],
        # the queue's rows as drawn: ink boxes, text x, the ring against the scrollbar's lane
        lead_table=dict(
            prefix="queuegeom-", title="Measured geometry: the queue's rows",
            note="Per state, each fully shown row that is playing, selected, ringed, hovered or unreadable (\"rest\": "
                 "also the first plain row), measured in the app (items, window px) and in its pixels. Cell x: each "
                 "queueCell_* item's left edge. Ink: per cell, the box of pixels more than 40 levels off the row's own "
                 "colour, x in the window / y from the row's top (the ring's pixels left out). Ring: the focus ring as an "
                 "item (rowBackgroundTint with a border; the lane it keeps clear of) and as pixels (uiPalette.focusRing "
                 "within 60 levels). Lane: the scrollbar's own x span (queueScrollBar), and its thumb's. Gap: from the "
                 "ring's last pixel to the lane's and the thumb's left edges (+: clear of them).",
            columns=[("size", lambda f: f["size"]), ("state", lambda f: f["state"]), ("row", lambda f: f["row"]),
                     ("flags", lambda f: f["flags"]), ("row x", lambda f: f["row_x"]), ("cell x", lambda f: f["text_x"]),
                     ("ink x / y", lambda f: f["ink"] or "-"), ("ring item", lambda f: f["ring_item"]),
                     ("ring px", lambda f: f["ring_px"]), ("scrollbar lane", lambda f: f["lane"]),
                     ("thumb", lambda f: f["thumb"]), ("gap", lambda f: f["gap"])]),
        facts=["queue-focus", "queue-now-", "queue-unreadable", "status-jumps-"],
        # read before judging a colour by eye
        notes=["The sheets are the app's own frames, and its text is drawn with subpixel (RGB) antialiasing: thin strokes "
               "carry coloured fringes, so grey text such as the queue's \u201cLength\u201d header can look blue (or "
               "orange) at 1:1 and in a zoom while its measured colour is grey. Judge text colours by the contrast rows "
               "and the key colours, which are measured, not by the fringes."],
        facts_strips=[("queue-now-move", "dark", "Next: the playing row moving down, dark"),
                      ("queue-now-move", "light", "Next: the playing row moving down, light"),
                      ("hover-fade-queue-row", "dark", "A queue row's hover fading in and out, dark"),
                      ("hover-fade-queue-row", "light", "A queue row's hover fading in and out, light"),
                      ("mute-toggle", "dark", "Mute, then unmute"),
                      ("seek-drag-box", "dark", "The seek bar dragged: the time box"),
                      ("play-pause-glyph", "dark", "The play/pause glyph on pause"),
                      ("hover-fade-transport-button", "dark", "A transport button's hover fading in and out"),
                      ("first-scan", "dark", "The first library scan: the status bar's scan label")],
        floors=[],
        a11y_labels=[],
        # CONTRAST.md rows quoted under each focus sheet (by element-name fragment, case ignored)
        contrast={"focus-queue": ["queue title", "queue artist", "queue track", "queue header", "queue hover", "playing row"],
                  "focus-queue-light": ["queue title", "queue artist", "queue track", "queue header", "queue hover", "playing row"],
                  "focus-queue-hover": ["queue hover", "playing row"],
                  "focus-transport-buttons": ["transport"], "focus-transport-seek": ["seek", "volume"],
                  "focus-statusbar": ["status bar", "status"], "focus-statusbar-lastfm": ["last.fm", "scrobble"]},
    ),
    # Dialogs (the viewers have their own scope). Configure tiles are fitted to 770x600: 1:1 once Configure is sized to its
    # content (about 760x560), labelled with their scale while it is larger. Confirmations: the innermost window
    # with its title bar and a margin of its parent ("inner"); main-window popups (search, Recent Issues): "popup".
    "dialogs": dict(
        title="Dialogs",
        sessions=["dialogs", "statusbar", "roots", "bigfont", "narrow", "motion-ui"],
        sheets=[
            # the 10 pt sheets: four (Configure, confirmations, Edit Tags, iTunes with its controls); the
            # large font on ten (every dialog at 24 pt at 1:1, cropped to regions where it is large; Recent Issues dark and light)
            ("dlg-configure-a", "Configure, 1:1: Library, three named folders, the third hovered (light, top 56 %); Last.fm "
             "connected (a stand-in: no network; top 40 %); General (top 41 %)", "light",
             [[("dialog", "preferences-library-three-roots-hover", None, "Library: three named folders, the third hovered, light",
                dict(theme="light", part=(0, 0, 1, 0.56), lh=16, gap=6))],
              [("dialog", "preferences-lastfm-account", None, "Last.fm, connected (stand-in), dark",
                dict(theme="dark", part=(0, 0, 1, 0.4), lh=16, gap=6))],
              [("dialog", "preferences-general", None, "General, dark", dict(theme="dark", part=(0, 0, 1, 0.41), lh=16, gap=6))]]),
            ("dlg-confirmations", "Confirmations as they open, 1:1, over their parents; the folder list's "
             "menu in Configure (Menu key)", "dark",
             [[("dialog", "confirm-clear-diagnostics", "inner", f"Delete diagnostics log, {th}", dict(theme=th, margin=16, lh=16, gap=6))
               for th in ("dark", "light")],
              [("dialog", "confirm-replace-cover", "inner", "Replace album art, dark", dict(margin=16, lh=16, gap=6)),
               ("dialog", "preferences-library-root-menu", (0.39, 0.09, 0.547, 0.295), "Configure: a folder's menu (Menu key), dark",
                dict(lh=16, gap=6))],
              [("dialog", "tag-editor-close-confirm", "inner", f"Unsaved tag changes, {th}", dict(theme=th, margin=16, lh=16, gap=6))
               for th in ("dark", "light")],
              [("dialog", "quit-save-failed", None, "Couldn't save before quitting, dark", dict(lh=16, gap=6)),
               ("dialog", "confirm-remove-library-root", "inner", "Remove Library Folder, light",
                dict(theme="light", margin=16, lh=16, gap=6))]]),
            ("dlg-tag-editor", "Edit Tags, 1:1 (its left 70 %): four tracks at rest (light); one edit typed with real keys in "
             "the Title field, then committed with Tab (its marked row); the last two their top 38 %", "dark",
             [[("dialog", "tag-editor", None, "At rest, light", dict(theme="light", part=(0, 0, 0.7, 1), lh=16, gap=6))],
              [("dialog", "tag-editor-editing", None, "\u201c (Live)\u201d typed in the Title field", dict(part=(0, 0, 0.7, 0.38), lh=16, gap=6))],
              [("dialog", "tag-editor-modified", None, "The same edit committed with Tab", dict(part=(0, 0, 0.7, 0.38), lh=16, gap=6))]]),
            # the 10 pt search sheet is left out (the palette has its own shots in shots/)
            # the searching -> failed recording (a real failed search; its frames at 1:1 around the status) with the
            # results and the controls' states; the failed window as a whole is in shots/ (dialog_itunes-error_*)
            ("dlg-itunes-controls", "Replace from iTunes, dark, 1:1: searching, then the real failed search, frame by frame "
             "(the status, from the recording); results (stand-ins, top 55 %); Preview/Apply at rest, hovered, pressed; "
             "Edit Tags' revert glyphs; Recent Issues links at rest, hovered, pressed; Tab focus on one", "dark",
             [[dict(strip="itunes-search-fails", item=ITUNES_FAIL_BAND, times=ITUNES_FAIL_TIMES, zoom=1, pad=0, lh=16, gap=6,
                    label="Searching -> failed: ms after the action (the search starts at about +3 ms)")],
              [("dialog", "itunes-results", None, "Results, top 55 %", dict(part=(0, 0, 1, 0.55), lh=16, gap=6)),
               [("dialog", st, (0.68, 0.27, 0.32, 0.1), lb, dict(lh=16, gap=6))
                for st, lb in (("itunes-results", "Preview, Apply at rest"), ("itunes-apply-hover", "Apply hovered"),
                               ("itunes-preview-pressed", "Preview pressed"))]],
              [("dialog", st, (0.06, 0.08, 0.25, 0.17), lb, dict(lh=16, gap=6))
               for st, lb in (("tag-editor-revert-hover", "Field revert hovered"), ("tag-editor-revert-pressed", "Field revert pressed"),
                              ("tag-editor-column-revert-hover", "Column revert hovered"))],
              # Tab lands on the second entry's link (Show in Library); the pointer states are the first's
              [("dialog", st, "popup", lb, dict(part=(0, 0.3, 0.6, 0.2), lh=16, gap=6))
               for st, lb in (("recent-errors", "Library Settings… at rest"), ("recent-errors-link-hover", "Hovered"),
                              ("recent-errors-link-pressed", "Pressed"))]
              + [("dialog", "recent-errors-link-focus", "popup", "Tab: Show in Library focused",
                  dict(part=(0, 0.66, 0.6, 0.2), lh=16, gap=6))]]),
            # 24 pt, 1:1: the dialogs now follow the font, so Configure and Diagnostics open at
            # 1964x1392 and About, About KDE, Rename Files and Tag Editor Details wider; crops of the regions the fix
            # changed (the folder row and its glyph, the headers, the footers) in place of whole windows
            ("dlg24-library", "At 24 pt, dark: Configure's Library page (fitted); its footer and iTunes' count "
             "line (1:1)", "dark",
             [[("dialog", "preferences-library", None, "Library page: heading, folder list, buttons",
                dict(B24, part=(0.13, 0.025, 0.84, 0.215), fit=(1500, 400)))],
              [("dialog", "preferences-library", None, "Footer, left", dict(B24, part=(0, 0.935, 0.1, 0.065))),
               ("dialog", "preferences-library", None, "Footer, right", dict(B24, part=(0.78, 0.935, 0.22, 0.065)))],
              [("dialog", "itunes-results", None, "Replace from iTunes: the count line (16-26 % down)",
                dict(B24, part=(0, 0.16, 1, 0.1)))]]),
            # Settings › Configure Keyboard Shortcuts (#700, #701): the list, filtered, and a shortcut being changed
            ("dlg-shortcuts", "Configure Keyboard Shortcuts, 1:1: at rest (dark, whole); the list filtered by \u201cplay\u201d typed "
             "in its search field (light, top 60 %)", "dark",
             [[("dialog", "keyboard-shortcuts", None, "At rest, dark", dict(lh=16, gap=6))],
              [("dialog", "keyboard-shortcuts-filter", None, "Filtered by \u201cplay\u201d, light", dict(theme="light", part=(0, 0, 1, 0.6), lh=16, gap=6))]]),
            ("dlg-shortcuts-edit", "Changing a shortcut, 1:1 (the dialog's lower part); Shortcut Conflict; dark", "dark",
             [[("dialog", "keyboard-shortcuts-custom", None, "Stop: keys of its own, not yet applied", dict(part=(0, 0.6, 1, 0.4), lh=16, gap=6)),
               ("dialog", "keyboard-shortcuts-refused", None, "Next: a key the queue keeps, refused", dict(part=(0, 0.6, 1, 0.4), lh=16, gap=6))],
              [("dialog", "keyboard-shortcuts-typed-key", None, "Follow Playhead: F, with its note", dict(part=(0, 0.6, 1, 0.4), lh=16, gap=6)),
               ("dialog", "keyboard-shortcuts-reassign", "inner", "Shortcut Conflict", dict(margin=16, lh=16, gap=6))]]),
            # the small 24 pt windows together: the folder dialogs and the quit ones
            ("dlg24-small", "At 24 pt, dark, 1:1: Add Library Folder; Finishing Tag Changes", "dark",
             [[("dialog", "library-root-name-add", None, "Add Library Folder", B24)],
              [("dialog", "tag-editor-quit-wait", "inner", "Finishing Tag Changes", dict(B24, margin=4))]]),
            ("dlg24-about", "At 24 pt, 1:1: About's header (icon, name, version, tabs), dark and light; About KDE's "
             "header; both footers' ends (dark)", "dark",
             [[("dialog", "about", None, "About: header and tabs", dict(B24, part=(0, 0.06, 0.75, 0.4)))],
              [("dialog", "about", None, "About: header and tabs, light", dict(B24, theme="light", part=(0, 0.06, 0.75, 0.4)))],
              [("dialog", "about-kde", None, "About KDE: header and first lines", dict(B24, part=(0, 0.035, 1, 0.16)))],
              [("dialog", "about", None, "About: footer, left", dict(B24, part=(0, 0.83, 0.4, 0.17))),
               ("dialog", "about", None, "About: Close", dict(B24, part=(0.85, 0.83, 0.15, 0.17))),
               ("dialog", "about-kde", None, "About KDE: Close", dict(B24, part=(0.85, 0.91, 0.15, 0.09)))]]),
            ("dlg24-diagnostics", "At 24 pt, dark, 1:1: Diagnostics' top (left, then right); Delete "
             "Diagnostics Log over it", "dark",
             [[("dialog", "diagnostics", None, "Diagnostics: log file line and toolbar, left", dict(B24, part=(0, 0.025, 0.47, 0.095)))],
              [("dialog", "diagnostics", None, "The same, right", dict(B24, part=(0.53, 0.025, 0.47, 0.095)))],
              [("dialog", "confirm-clear-diagnostics", "inner", "Delete Diagnostics Log", dict(B24, margin=4)),
               ("dialog", "diagnostics", None, "Diagnostics: Close", dict(B24, part=(0.85, 0.93, 0.15, 0.07)))]]),
            # Rename Files with the twelve-file batch (RENAME_BATCH): at 24 pt its window is about 1.4 MP, so it
            # is shown whole fitted (its width against its content; the scale in the label) and 1:1 in part; 10 pt whole
            ("dlg24-rename", "Rename Files, twelve files, dark: 24 pt whole (fitted); 10 pt whole, 1:1; 24 pt, 1:1: its top "
             "rows and its buttons", "dark",
             [[("dialog", "tag-editor-rename-preview", "inner", "24 pt, whole", dict(B24, margin=4, fit=(760, 340))),
               ("dialog", "tag-editor-rename-preview", "inner", "10 pt, whole, 1:1", dict(margin=4, lh=16, gap=6))],
              [("dialog", "tag-editor-rename-preview", "inner", "24 pt, 1:1: the top-left 1000 x 205 px",
                dict(B24, margin=4, box=(0, 0, 1000, 205))),
               ("dialog", "tag-editor-rename-preview", None, "24 pt, 1:1: its buttons",
                dict(B24, items=["renamePreviewRenameButton", "renamePreviewCancelButton"]))]]),
            ("dlg24-details", "At 24 pt, dark: Tag Editor Details after a failed save (fitted); an iTunes result "
             "loading its artwork (1:1)", "dark",
             [[("dialog", "tag-editor-status-details", "inner", "Tag Editor Details", dict(B24, margin=4, fit=(1520, 540)))],
              [("dialog", "itunes-results-loading", None, "Replace from iTunes: the first result, its full-size image loading",
                dict(B24, items=["itunesArtworkResultTitle", "itunesArtworkResultDetails", "itunesArtworkPreviewButton",
                                 "itunesArtworkApplyButton"], first=True, margin=4, grow=(130, 0, 0, 0)))]]),
            ("dlg24-tag-tools", "At 24 pt, dark, 1:1: Auto Number; Configure Keyboard Shortcuts' editor", "dark",
             [[("dialog", "tag-editor-auto-number", "inner", "Auto Number", dict(B24, margin=4))],
              [("dialog", "keyboard-shortcuts", None, "Configure Keyboard Shortcuts: its lower left",
                dict(B24, part=(0, 0.72, 0.565, 0.28)))]]),
            # 24 pt, light: Recent Issues dark above light (the whole popup: its width, the severity glyph)
            ("dlg24-light", "At 24 pt, 1:1: Recent Issues, dark above light (the whole popup over the status bar); Edit "
             "Tags' Track and Disc, both mixed (dark)", "light",
             [[("dialog", "recent-errors", "popup", "Recent Issues, dark", dict(B24, theme="dark"))],
              [("dialog", "recent-errors", "popup", "Recent Issues, light", dict(B24, theme="light"))],
              # twelve rows selected, Track and Disc mixed: their "Multiple values" placeholders, uncut
              [("dialog", "tag-editor-batch", None, "Edit Tags: Track and Disc, twelve files selected, dark",
                dict(B24, theme="dark", items=["tagEditorLabel_trackNo", "tagEditorOf_trackNo", "tagEditorLabel_discNo",
                                               "tagEditorOf_discNo"], grow=(0, 0, 150, 0)))]]),
        ],
        images=[
            ("sheet", "dlg-configure-a", "light", "Configure 1:1: Library with three named folders (light); Last.fm; General",
             "the folder list's columns; the pages as drawn"),
            ("sheet", "dlg-confirmations", "dark", "Confirmations, the quit failure, Remove Folder; Configure's folder menu",
             "which button is the default and has focus; the wording"),
            ("sheet", "dlg-tag-editor", "dark", "Edit Tags: at rest, one typed edit, committed", "the fields, the table, the mark"),
            ("sheet", "dlg-itunes-controls", "dark", "iTunes searching -> failed frames; results; Preview/Apply, revert glyphs, "
             "Recent Issues links incl. Tab focus", "the failed state as it arrives; the flat controls' states"),
            ("sheet", "dlg24-library", "dark", "24 pt: Library page's folder row, buttons; Configure's footer; iTunes' count line",
             "the folder glyph and full path; the footer"),
            ("sheet", "dlg-shortcuts", "dark", "Configure Keyboard Shortcuts: at rest; filtered", "the list's groups, columns and "
             "keys; the search field; what the filter keeps"),
            ("sheet", "dlg-shortcuts-edit", "dark", "Changing a shortcut: custom keys, a refusal, a typed key's note, a conflict",
             "the editor's wording and states; the conflict's choice"),
            ("sheet", "dlg24-small", "dark", "24 pt: Add Library Folder; Finishing Tag Changes", "the field, the wrap, the buttons"),
            ("sheet", "dlg24-about", "dark", "24 pt: About's header dark and light; About KDE's header; the footers", "the icon, the tabs, the footers"),
            ("sheet", "dlg24-diagnostics", "dark", "24 pt: Diagnostics' log file line, toolbar and Close; Delete Diagnostics Log",
             "the toolbar; the confirmation"),
            ("sheet", "dlg24-rename", "dark", "Rename Files, twelve files: 24 pt whole (fitted) and in part; 10 pt whole",
             "the preview's width against its content; the arrows"),
            ("sheet", "dlg24-details", "dark", "24 pt: Tag Editor Details; an iTunes result loading its artwork",
             "the details' width and wrap; the loading line uncut"),
            ("sheet", "dlg24-tag-tools", "dark", "24 pt: Auto Number; the shortcuts editor", "the check boxes' edge; the forms"),
            ("sheet", "dlg24-light", "light", "24 pt: Recent Issues dark and light; Edit Tags' Track and Disc placeholders",
             "the popup's width, the glyph; the placeholders uncut"),
        ],
        facts=["dlg-", "screen-", "itunes-failed-", "tagsave-", "renamebatch-", "recent-issues-opened",
               "fill-about", "fill-itunes", "fill-recent-errors", "fill-tag-editor", "fill-preferences"],
        fact_table=True,  # one row per image at the top of FOCUS-FACTS
        # what Replace Album Art does, stated from the backend; the pack fails if these phrases leave the code
        source_notes=[("src/artwork/mod.rs",
                       ["When the track's folder shows an image as its cover", "embedded into every track in the folder that has the track's album tag",
                        "center_crop_to_square", 'format!("cover.{}"', "PictureType::CoverFront", "nothing is written unless all of them"],
                       "Replace Album Art, by case: the chosen image is first cropped to a centred square (kept as PNG or "
                       "JPEG). If the track's folder shows an image as its cover, the art goes to a folder image: an image "
                       "with a recognised cover name (such as cover.jpg) is overwritten in its own format, otherwise a new "
                       "cover.<ext> is written and the folder's other images stay; no track file changes. If the folder has "
                       "no cover image, the art is embedded as the front cover of every track in the folder with the "
                       "track's album tag (their other embedded pictures kept), and nothing is written unless every one of "
                       "those tracks can take it")],
        notes=["Paths: the app runs in a private mount namespace where the capture's work folder is at /mnt/ferrous "
               "(the fixture library, the app's own config, data and log folders), so the paths it shows are real and "
               "name no home folder; the pack still checks every recorded text and paints out any that does (none, in "
               "a run at the neutral paths).",
               "The screen is KWin's virtual output (screen-* below). Configure keeps its own "
               "size in the 1000x720 session (dlg-preferences-* facts).",
               "Real and injected states: the failed iTunes search is real (the app's application proxy set to 127.0.0.1:9, "
               "which refuses every request; nothing leaves the machine): BridgeClient sets itunesArtworkFailed; Tag Editor "
               "Details shows a real save with one folder not writable; Finishing Tag Changes is given two loaded files' "
               "names (a save here takes milliseconds) and its second stage. Searching and the stand-in results are "
               "injected (no network).",
               "Recordings are the app's own frames: KWin's window animations (a dialog window opening and closing) are not "
               "in them; a new window's first frame is drawn whole."],
        facts_strips=[("dialog-open-close", "dark", "A dialog (About) opening and closing"),
                      ("preferences-open", "dark", "Configure opening"),
                      ("preferences-page-switch", "dark", "Configure: a page switch"),
                      ("itunes-search-fails", "dark", "Replace from iTunes: searching, then the failed search")],
        floors=[],
        a11y_labels=[],
        contrast={},
    ),
    # The app icon (sessions icons and firstplay-noart: short; --focus-only app-icon is quick)
    "app-icon": dict(
        title="The app icon",
        sessions=["icons", "firstplay-noart", "icon-noart24"],
        # the installed icon per size first; the squint row and the variants below in the facts
        lead_table=dict(
            prefix="appicon-table-", title="The installed icon per size",
            note="Each size at 1x with the file Plasma's icon lookup takes (real hicolor index; \"scaled\": drawn at another "
                 "size than its own). From each trace's centre line in the SVG (FOCUS-FACTS appicon-lines-*): widths ⟂ at 50 % "
                 "along, top trace first, and their ratio amber : rose : magenta (magenta = 1); the rise angle at 50 %; the "
                 "fan's opening (spacing at the right edge / at the start); the spacing between neighbouring centre lines half way "
                 "across the tile, top pair first. Top air: in the top trace's last 4 columns up to its exit, the rows "
                 "between the tile's top in that column (its rounded corner counts) and the trace as drawn at that size: clear (under 10 % covered) / faint "
                 "(10-50 %). Contrast: each trace against the tile.",
            columns=[("size", lambda f: f"{f['px']} px"), ("file", lambda f: f["file"]), ("rim px", lambda f: f["rim"]),
                     ("traces", lambda f: f["n"]),
                     ("widths ⟂ (px)", lambda f: ", ".join(f"{v:g}" for v in f.get("perp") or [])),
                     ("widths amber : rose : magenta", lambda f: f.get("ratio") or "-"),
                     ("rise at 50 % (deg)", lambda f: ", ".join(f"{v:g}" for v in f.get("angles") or [])),
                     ("fan opening", lambda f: f"{f['fan']}x" if f.get("fan") else "-"),
                     ("spacing half way (px)", lambda f: ", ".join(f"{v:g}" for v in f.get("spacing") or []) or "-"),
                     ("top trace exits", lambda f: {"right": "the right edge, rows ", "top": "the top edge, "}.get(
                         f["exits"], "short of the edges, rows ") + str(f["exit_rows"])),
                     ("top air, last 4 columns (rows clear / faint)", lambda f: (
                         f"{', '.join(map(str, f['air_clear']))} / {', '.join(map(str, f['air_faint']))} (x {f.get('air_col')})"
                         if isinstance(f.get("air_clear"), list) else "-")), ("contrast to the tile", lambda f: ", ".join(f"{v}:1" for v in f["contrast"]))]),
        # the evidence rule: a blocking flaw must show at 1:1 in a real context; so the 1:1 real-context sheets
        # lead, the magnified ones follow (supporting evidence). the variants (only with
        # SHOT_ICON_VARIANTS=1) after the installed sheets; the vision sheet gives way to them (8 images at most)
        sheets=[
            ("focus-icon-small", "1:1 where Plasma shows the icon small: the title bar, 24 px on a 32 px strip, a 16 px "
             "menu, a 22 px task switcher, the task manager at 32 px on a 44 px panel (idle, playing, active) and 36 px "
             "on a 52 px panel; a 2x crop of the playing cell; Breeze Dark above Breeze Light",
             "dark", [[("appicon", "ctx-small", None, "Breeze Dark")], [("appicon", "ctx-small", None, "Breeze Light", dict(theme="light"))]]),
            ("focus-icon-large", "1:1 where Plasma shows the icon large: a 48 px launcher with LabPlot and KmPlot, Alt-Tab "
             "at 64 px, an About page at 128 px, among media players at 32 and 48 px; Breeze Dark above Breeze Light", "dark",
             [[("appicon", "ctx-large", None, "Breeze Dark")], [("appicon", "ctx-large", None, "Breeze Light", dict(theme="light"))]]),
            ("focus-icon-hidpi", "The 32 px slot at 1.25x, 1.5x, 1.75x and 2x among Breeze icons (device px, 1:1), dark "
             "beside light; a Discover-like card at 256 px (dark, light); 128 and 256 px at 2x in context (dark)", "dark",
             [[("appicon", "ctx-hidpi", None, "Breeze Dark", dict(lh=16, gap=6)),
               ("appicon", "ctx-hidpi", None, "Breeze Light", dict(theme="light", lh=16, gap=6))],
              [("appicon", "ctx-discover", None, "256 px, Breeze Dark", dict(lh=16, gap=6)),
               ("appicon", "ctx-discover", None, "Breeze Light", dict(theme="light", lh=16, gap=6)),
               ("appicon", "ctx-2x", None, "2x, Breeze Dark", dict(lh=16, gap=6))]]),
            ("focus-icon-nowplaying", "The no-art mark in the now-playing panel, 1:1 (dark, light); the 3rd harmonic's "
             "colour in every file at 1:1, declared vs rendered", "dark",
             [[("details", "motif-panel", None, "Now playing, Breeze Dark", dict(lh=16, gap=6)),
               ("details", "motif-panel", None, "Breeze Light", dict(theme="light", lh=16, gap=6))],
              [("appicon", "h3strip", None, "Breeze Dark", dict(lh=16, gap=6))]]),
            ("focus-icon-variants", "Not installed: the installed icon vs the variant(s) at 1:1 in real contexts (32 px "
             "plain and playing, 48 px launcher, 64 px Alt-Tab, 128 px About); Breeze Dark beside Breeze Light", "dark",
             [[("appicon", "var-small", None, "Breeze Dark"), ("appicon", "var-small", None, "Breeze Light", dict(theme="light"))]]),
            ("focus-icon-variants-large", "Not installed: Discover-like cards at 256 px, installed vs the variant(s) (dark, "
             "light); the no-art mark, installed (the app) vs the variant (rendered PNGs) at 360 and 152 px", "dark",
             [[("appicon", "var-large", None, "Breeze Dark"), ("appicon", "var-large", None, "Breeze Light", dict(theme="light"))],
              [("details", "motif-mark", None, "installed mark, 360 px", dict(lh=16, gap=6)),
               ("appicon", f"var-mark-{_VAR}-360", None, f"{_VAR}, 360 px", dict(lh=16, gap=6)),
               ("details", "motif-mark", None, "installed, 152 px", dict(variant="font24", lh=16, gap=6)),
               ("appicon", f"var-mark-{_VAR}-152", None, f"{_VAR}, 152 px", dict(lh=16, gap=6))]]),
            ("focus-icon-hinted", "Magnified: every hand-drawn file at nearest-neighbour zoom (3x to 24 px, 2x for 32-48 "
             "px and the @2 files, 1x for 64@2); Breeze Dark above Breeze Light", "dark",
             [[("appicon", "hinted", None, "Breeze Dark")], [("appicon", "hinted", None, "Breeze Light", dict(theme="light"))]]),
            ("focus-icon-vision", "Colour vision at 16-128 px (protanopia; deuteranopia from 32 px); the 64 px drawing "
             "drawn at 32 px beside the 32 px file (1:1, 4x); the squint row per size; dark and light", "dark",
             [[("appicon", "cvd", None, "Colour vision, Breeze Dark", dict(lh=16, gap=6))],
              [("appicon", "drift", None, "Breeze Dark", dict(lh=16, gap=6)),
               ("appicon", "drift", None, "Breeze Light", dict(theme="light", lh=16, gap=6))],
              [("appicon", "squint", None, "Breeze Dark", dict(lh=16, gap=6))],
              [("appicon", "squint", None, "Breeze Light", dict(theme="light", lh=16, gap=6))]]),
        ],
        images=[
            ("sheet", "focus-icon-small", "dark", "1:1 real context, 16-36 px: title bar, 24 px strip, menu, task switcher, panel",
             "whether a flaw shows at real size: the traces apart at 16-24 px; the badge against the 32 px icon"),
            ("sheet", "focus-icon-large", "dark", "1:1 real context, 48-128 px: launcher, Alt-Tab, About; among media players",
             "whether it reads as a chart among chart apps, and as a player among players"),
            ("sheet", "focus-icon-hidpi", "dark", "1:1: the 32 px slot at 1.25x-2x; a 256 px store card; 128/256 px at 2x",
             "the @2 files at real device size among Breeze's; the large drawing in a store card"),
            ("sheet", "focus-icon-nowplaying", "dark", "1:1: the no-art mark in the now-playing panel; the 3rd harmonic's colour",
             "the mark among the panel's text; the magenta at every size"),
            ("sheet", "focus-icon-variants", "dark", "Not installed: installed vs the variant at 32-128 px in context",
             "whether the variant reads better or worse than the installed icon at real size", dict(optional=True)),
            ("sheet", "focus-icon-variants-large", "dark", "Not installed: 256 px cards and the no-art mark, installed vs the variant",
             "the variant at large size and in the mark", dict(optional=True)),
            ("sheet", "focus-icon-hinted", "dark", "Magnified: every hand-drawn file, both themes",
             "each file's pixels (supporting evidence only)"),
            ("sheet", "focus-icon-vision", "dark", "Colour vision, the 64 px drawing at 32 vs the 32 px file, the squint row",
             "the traces apart without red-green and in grey", dict(unless="focus-icon-variants")),
        ],
        facts=["appicon", "motif"],
        contrast={},
    ),
}

# the seek bar in the transport's row at 1600x1000: from the buttons to the time
_SEEK = (0.095, 0.03, 0.775, 0.04)
_HALF = dict(fit=(620, 216), lh=16, gap=6)  # a pane at half size
# the seek bar in each recording whose band the recording's merged image would show a few pixels high
_VI_MOVES = [("vi-pan-waveform", "Middle-drag", [-20, 300, 700]), ("vi-follow-waveform", "Follow Playhead (F)", [-20, 150]),
             ("vi-seek-plot", "Right click on the plot: the view stays", [-20, 150]),
             ("vi-seek-bar", "Click on the seek bar: the view follows", [-20, 150])]
_VI_TIMES = [-20, 300, 700, 1100, 1500, 1900, 2300, 2700, 3100]


def _vi_band(st, lb, th="dark"):
    return [("main", st, _SEEK, f"{lb}, {th}", dict(theme=th, lh=16, gap=4))]


SCOPES.update({
    # Visualization interaction: zoom, the seek bar's band, middle-drag pan, autoscroll, back to the playhead, seeks
    "viz-interaction": dict(
        title="Visualization interaction: zoom, the view band, pan, autoscroll, follow, seek",
        sessions=["viz-interaction", "motion"],
        sheets=[
            ("vi-band", "The seek bar at 1:1: its band marks the part of the track the zoomed plot shows; dark, one row light", "dark",
             [_vi_band("vi-waveform-zoomed", "Waveform zoomed, following"),
              _vi_band("vi-waveform-panned", "Middle-dragged back (view detached)", "light"),
              _vi_band("vi-waveform-panned", "Middle-dragged back (view detached)"),
              _vi_band("vi-waveform-panned-held", "3 s later: the view stays"),
              _vi_band("vi-waveform-followed", "Follow Playhead (F): back on the playhead"),
              _vi_band("vi-autoscroll-near", "Autoscroll (zoomed deeper), pointer a little right of the click"),
              _vi_band("vi-autoscroll-edge", "Autoscroll, pointer at the window's edge"),
              _vi_band("vi-autoscroll-outside", "Autoscroll, pointer outside the window"),
              _vi_band("vi-autoscroll-stopped", "A second middle click: stopped, the view stays"),
              _vi_band("vi-spectrogram-zoomed", "Centred spectrogram zoomed, following"),
              _vi_band("vi-spectrogram-panned", "Spectrogram middle-dragged back"),
              _vi_band("vi-rolling-zoomed", "Rolling spectrogram, zoomed")]),
            ("vi-waveform-views", "The zoomed waveform's view, the pane at half size (scale in each label): following the "
             "playhead; middle-dragged back so the playhead is past the view's right edge; 3 s later (the view stays); "
             "after Follow Playhead (F); dark", "dark",
             [[("main", st, _PANE, lb, _HALF) for st, lb in row]
              for row in ((("vi-waveform-zoomed", "Zoomed, following"), ("vi-waveform-panned", "Middle-dragged back")),
                          (("vi-waveform-panned-held", "3 s later, still playing"), ("vi-waveform-followed", "After F")))]),
            ("vi-spectrogram-views", "The zoomed centred spectrogram's view, the pane at half size: following; just "
             "middle-dragged back (its columns being decoded where it is); 2.5 s later (filled in, staying); the rolling "
             "spectrogram still zoomed; dark, then the zoomed waveform in light", "dark",
             [[("main", st, _PANE, lb, _HALF) for st, lb in row]
              for row in ((("vi-spectrogram-zoomed", "Zoomed, following"), ("vi-spectrogram-panned", "Just middle-dragged back")),
                          (("vi-spectrogram-panned-decoded", "2.5 s later"), ("vi-rolling-zoomed", "Rolling, zoomed")))]
             + [[("main", "vi-waveform-zoomed", _PANE, "Waveform zoomed, light", dict(_HALF, theme="light")),
                 ("main", "vi-waveform-panned", _PANE, "Middle-dragged back, light", dict(_HALF, theme="light"))]]),
            ("vi-moves-band", "The band through a drag, Follow Playhead and the two seeks: the seek bar at 1:1 from "
             "the recordings", "dark",
             [[dict(strip=nm, item=(152, 4, 1240, 40), times=[t], zoom=1, pad=0, lh=16, gap=4, label=f"{lb}: about {t:+d} ms")]
              for nm, lb, ts in _VI_MOVES for t in ts]),
            ("vi-autoscroll-band", "Autoscroll, the seek bar at 1:1 from the recording: slow near the click, fast at "
             "the window's edge, going on outside it, stopped", "dark",
             # (the recording's crop starts at the transport's top: item is in its coordinates)
             [[dict(strip="vi-autoscroll", item=(152, 4, 1240, 40), times=[t], zoom=1, pad=0, lh=16, gap=4,
                    label=f"{t:+d} ms after the middle click")] for t in _VI_TIMES]),
        ],
        images=[
            ("sheet", "vi-band", "dark", "The seek bar's view band in every state of the zoomed view, 1:1",
             "the band's colour against the bar's waveform and the playhead; where it sits as the view moves and comes back"),
            ("sheet", "vi-waveform-views", "dark", "The waveform's view: following, dragged back, held, followed again (half size)",
             "the view staying where the drag put it while the playhead runs on; the way back"),
            ("sheet", "vi-spectrogram-views", "dark", "The centred spectrogram's view: following, dragged, filled in; rolling zoomed",
             "the moved view's columns arriving; nothing jumping back"),
            ("sheet", "vi-autoscroll-band", "dark", "Autoscroll: the band every 0.4 s from the recording, 1:1",
             "slow near the click, faster at the edge, going on outside the window, then stopping"),
            ("sheet", "vi-moves-band", "dark", "The band through a drag, F and the two seeks, frame by frame, 1:1",
             "the band moving with the drag; coming back on F and a seek-bar click; staying for a seek on the plot"),
            ("strip", "vi-pan-waveform", "dark", "A middle-drag of the zoomed waveform, the seek bar with it",
             "the time grabbed staying under the pointer; the band following; no jump at the release"),
            ("strip", "vi-follow-waveform", "dark", "Follow Playhead (F) after a drag: the view back on the playhead",
             "how the view returns (a cut or a glide); the band with it"),
            ("strip", "vi-pan-spectrogram", "dark", "A middle-drag of the zoomed centred spectrogram, then 1.2 s",
             "the moved view's columns filling in; the old canvas as the backdrop meanwhile"),
            ("strip", "vi-autoscroll", "dark", "Autoscroll: slow, fast at the edge, outside the window, stopped",
             "the speed rising with the distance; no stall when the pointer leaves"),
            ("strip", "vi-seek-plot", "dark", "A right click on the moved waveform: a seek that keeps the view",
             "the playhead landing in the view; the view not moving"),
            ("strip", "vi-seek-bar", "dark", "A click on the seek bar while the view is moved: the view follows again",
             "the playhead's jump and the view's"),
            ("strip", "spectrogram-zoom", "dark", "The wheel zooming the spectrogram 3 notches, then Reset Zoom",
             "each notch's step; the readout; the reset"),
        ],
        facts=["plates-"],
        facts_strips=[("vi-follow-spectrogram", "dark", "Follow Playhead (F) after a drag of the centred spectrogram"),
                      ("zoom-notch-lane", "dark", "One wheel notch on a lane"), ("zoom-reset-lane", "dark", "Reset Zoom on a lane")],
        notes=["The pointer's cursor is not in the grabs or recordings (KWin draws it). Autoscroll shows a horizontal "
               "resize cursor while it runs.",
               "\u201cOutside the window\u201d is the window told the pointer left (a Leave event, as KWin sends it), after a "
               "last move at its right edge: the app keeps the speed it had there (#662)."],
        floors=[],
        a11y_labels=[],
        contrast={},
    ),
    # The library, the search palette and the menus
    "library-search": dict(
        title="The library, search and the menus",
        sessions=["main", "menus", "roots", "density", "bigfont", "wide-bigfont", "narrow-bigfont", "hidpi", "motion-ui", "contrast"],
        sheets=[
            ("lib-states", "The library's row states at 1:1 (the pane's top): hover, click, keys; dark", "dark",
             [[("library", st, "subject", lb, dict(part=(0, 0, 1, pt), lh=16, gap=6)) for st, lb, pt in row]
              for row in ((("hover", "Artist hovered", 0.48), ("selected", "Selected (click)", 0.48),
                           ("expanded-keyboard", "Expanded with Right", 0.48)),
                          (("expanded-hover", "A track hovered", 0.48), ("collapsed-again", "Collapsed with Left", 0.48),
                           ("at-rest", "At rest, playing", 0.92)))]),
            ("lib-now", "The playing track's mark in the library at 1:1; dark above light", "dark",
             [[("library", st, "subject", f"{lb}, {th}", dict(theme=th, part=(0, 0, 1, 0.94), lh=16, gap=6)) for st, lb in
               (("collapsed-holding-playing", "Collapsed, a track inside playing"), ("follows-playing", "Marking the playing track"),
                ("expanded-playing", "Expanded to it (Show in Library)"))] for th in ("dark", "light")]),
            ("lib-density", "Density and folders at 1:1; 24 pt at 1920x1200", "dark",
             [[("library", "density-scrolled", "subject", "320 artists, dark", dict(part=(0, 0, 1, 0.56), lh=16, gap=6)),
               ("library", "density-scrolled", "subject", "Light", dict(theme="light", part=(0, 0, 1, 0.56), lh=16, gap=6)),
               [("library", "two-roots", "subject", "Two folders", dict(part=(0, 0, 1, 0.26), lh=16, gap=6)),
                ("library", "two-roots-hover", "subject", "A root row hovered", dict(part=(0, 0, 1, 0.26), lh=16, gap=6))]],
              [("library", "expanded-playing", "subject", "24 pt, 1920x1200: expanded to the playing track",
                dict(variant="1920x1200_font24", part=(0, 0.32, 1, 0.5), lh=16, gap=6))]]),
            ("lib-menus", "The library's keyboard menus (Menu key), 1:1 with the rows around them: on a track, an album, an "
             "artist, a library folder's root row; dark", "dark",
             [[("menu", st, "popup", lb, dict(margin=40, lh=16, gap=6)) for st, lb in row]
              for row in ((("context-library-track", "On a track"), ("context-library-album", "On an album")),
                          (("context-library-artist", "On an artist"), ("context-library-root", "On a folder's root row")))]),
            ("search-states", "The search palette (Ctrl+F) at 1:1; dark", "dark",
             [[("search", "empty", "popup", "Empty (its footer keys)", dict(margin=4, lh=16, gap=6))],
              [("search", "results", "popup", "\u201cmer\u201d: results", dict(margin=4, lh=16, gap=6))],
              [("search", "no-results", "popup", "No results", dict(margin=4, lh=16, gap=6))],
              [("search", "results-keyboard", "popup", "The keyboard selection three rows down (the top half)",
                dict(margin=4, part=(0, 0, 1, 0.5), lh=16, gap=6))]]),
            ("search-more", "The search palette at 1:1: many results, hover, help, non-ASCII; light", "light",
             [[("search", "results-many", "popup", "Many results (\u201cl\u201d): the groups cut", dict(margin=4, lh=16, gap=6))],
              [("search", "result-hover", "popup", "A result hovered: its tooltip (the top)", dict(margin=4, part=(0, 0, 1, 0.3), lh=16, gap=6))],
              [("search", "help-hover", (0.24, 0.076, 0.555, 0.115), "Help hovered: its tooltip", dict(lh=16, gap=6))],
              [("search", "results-unicode", "popup", "\u201c\u00f8rn\u201d (the top)", dict(margin=4, part=(0, 0, 1, 0.3), lh=16, gap=6))]]),
            ("search-24pt", "The search palette at 24 pt, 1:1; dark", "dark",
             [[("search", "results", "popup", "Results in the narrow window (1000x720)", dict(variant="1000x720_font24", margin=4, lh=16, gap=6))],
              [("search", "empty", "popup", "Empty, 1600x1000 (its left 64 %)",
                dict(variant="1600x1000_font24", margin=4, part=(0, 0, 0.64, 1), lh=16, gap=6))],
              [("search", "no-results", "popup", "No results, 1600x1000 (its left 64 %)",
                dict(variant="1600x1000_font24", margin=4, part=(0, 0, 0.64, 1), lh=16, gap=6))]]),
            ("menus-edit", "File and Edit while playing (Undo, Redo); Edit disabled; a search result's menu; "
             "1:1, dark", "dark",
             [[("menu", st, "popup", lb, dict(margin=12, lh=16, gap=6)) for st, lb in
               (("file-playing", "File"), ("edit-playing", "Edit"), ("edit-undo", "Edit after removing a row: Undo"))],
              [("menu", "edit-redo", "popup", "Edit after undoing it: Redo", dict(margin=12, lh=16, gap=6)),
               ("menu", "edit-empty-queue", "popup", "Edit, empty queue", dict(margin=12, lh=16, gap=6)),
               ("menu", "context-search-result-keyboard", "popup", "A search result's menu (Menu key)",
                dict(popup=1, margin=40, lh=16, gap=6))]]),
            ("menus-more", "View, Playback, Settings, Help while playing, 1:1 (light); a Repeat item hovered (dark)", "light",
             [[("menu", st, "popup", lb, dict(margin=12, lh=16, gap=6)) for st, lb in
               (("view-playing", "View"), ("playback-playing", "Playback"), ("settings-playing", "Settings"), ("help-playing", "Help"))],
              [("menu", "playback-repeat-hover", "popup", "Playback, a Repeat item hovered, dark (its lower half)",
                dict(theme="dark", margin=12, part=(0, 0.52, 1, 0.48), lh=16, gap=6))]]),
            ("menus-sizes", "The menus at 24 pt and at scale 2; the menu bar hovered; 1:1, dark", "dark",
             [[("menu", "playback", "popup", "24 pt, the narrow window: the menu reaches past it", dict(variant="1000x720_font24", margin=8, lh=16, gap=6)),
               [("menu", "edit", "popup", "Edit, 24 pt", dict(variant="1600x1000_font24", margin=8, lh=16, gap=6)),
                # (a popup's rect is in logical px: at scale 2 the crop is by fractions)
                ("menu", "playback", (0.07, 0, 0.18, 0.2), "Playback at scale 2 (device px)", dict(scale=2, lh=16, gap=6)),
                ("menubar", "hovered", (0, 0, 0.3, 0.03), "Menu bar: View hovered", dict(lh=16, gap=6))]]]),
        ],
        images=[
            ("sheet", "lib-states", "dark", "The library's row states: hover, selection, expanded by keyboard, collapsed",
             "the rows' rhythm and indent; hover and selection; the disclosure glyphs"),
            ("sheet", "lib-now", "dark", "The playing track's mark: collapsed, in the tree, expanded to it; both themes",
             "the amber mark at each depth; Show in Library's result"),
            ("sheet", "lib-density", "dark", "320 artists; two folders; 24 pt at 1920x1200", "density and scanning; the root rows"),
            ("sheet", "lib-menus", "dark", "The library's keyboard menus on a track, album, artist and root row",
             "where each opens against its row; the items and their order"),
            ("strip", "pair:library-expand-collapse", "dark", "An artist expanded (left) and collapsed (right) with the keyboard",
             "the rows' motion; content cuts"),
            ("sheet", "search-states", "dark", "The search palette: empty, results, keyboard selection, no results",
             "the field, the groups, the footer keys; the selection; the empty message"),
            ("sheet", "search-more", "light", "Many results, a hovered result, help, a non-ASCII query (light)",
             "truncated groups; the tooltips; the match's weight"),
            ("sheet", "search-24pt", "dark", "The search palette at 24 pt, wide and narrow", "the palette's width and rows at 24 pt"),
            ("strip", "pair:search", "light", "The search palette opening (left) and closing (right)", "the fade; content cuts"),
            ("sheet", "menus-edit", "dark", "File and Edit while playing; Edit with Undo, then with Redo, disabled; a search result's menu",
             "the items' wording (Undo Remove from Queue, Redo), shortcuts and icons"),
            ("sheet", "menus-more", "light", "View, Playback, Settings, Help; a Repeat item hovered",
             "the menus as one family; checkable items"),
            ("sheet", "menus-sizes", "dark", "The menus at 24 pt (past the window) and scale 2; the menu bar hovered",
             "the menus' width and padding at a large font and at 2x"),
        ],
        facts=["dlg-empty", "dlg-results", "dlg-no-results"],
        facts_strips=[("menu-open-close", "dark", "A menu opening and closing"),
                      ("search-typing", "dark", "Typing in the search palette"),
                      ("library-expand", "light", "An artist expanded with the keyboard, light"),
                      ("library-collapse", "light", "The artist collapsed, light")],
        floors=[],
        a11y_labels=[],
        contrast={"lib-states": ["library"], "lib-now": ["library", "playing"], "search-states": ["search"],
                  "search-more": ["search"], "menus-edit": ["menu"], "menus-more": ["menu"]},
    ),
    # The viewers: album art and the visualization, windowed and fullscreen
    "viewers": dict(
        title="The viewers: album art and the visualization, windowed and fullscreen",
        sessions=["viewers", "bigfont", "narrow", "motion"],
        sheets=[
            ("vw-albumart", "The album art viewer, fitted: windowed with its info column; fullscreen at rest", "dark",
             [[("viewer", "albumart-windowed-info", None, "Windowed, with its info column", dict(fit=(740, 520), lh=16, gap=6)),
               ("viewer", "albumart-fullscreen", None, "Fullscreen", dict(fit=(740, 520), lh=16, gap=6))]]),
            ("vw-albumart-controls", "The album art viewer's controls at 1:1: the windowed corner plate, the "
             "fullscreen strip, its menus; dark", "dark",
             [[("viewer", st, (0.8, 0, 0.2, 0.1), lb, dict(lh=16, gap=6)) for st, lb in
               (("albumart-windowed-corner-hover", "Corner: info hovered"), ("albumart-windowed-corner-pressed", "Info pressed"),
                ("albumart-windowed-corner-focus", "Close focused"))],
              [("viewer", st, (0.84, 0, 0.16, 0.07), lb, dict(lh=16, gap=6)) for st, lb in
               (("albumart-fullscreen-strip-hover", "Fullscreen strip: info hovered"), ("albumart-fullscreen-strip-focus", "Info focused"),
                ("albumart-fullscreen-strip-key-pressed", "Info held with Space"))],
              [("menu", "context-albumart-viewer-windowed", "popup", "Menu, windowed", dict(popup=1, margin=24, lh=16, gap=6)),
               ("viewer", "albumart-fullscreen", (0, 0, 0.3, 0.04), "The strip's title", dict(lh=16, gap=6))],
              [("menu", "context-albumart-viewer-fullscreen", (0.49, 0.48, 0.14, 0.055), "Menu, fullscreen (Menu key)", dict(lh=16, gap=6))]]),
            ("vw-info", "The album art viewer's info column at 1:1", "dark",
             [[("viewer", "albumart-windowed-info", (0.77, 0, 0.23, 0.33), "Windowed, dark", dict(lh=16, gap=6)),
               ("viewer", "albumart-fullscreen-info", (0.78, 0, 0.22, 0.27), "Fullscreen, dark", dict(lh=16, gap=6))],
              [("viewer", "albumart-windowed-info", (0.5, 0, 0.5, 0.45), "The narrow window (1000x720), light",
                dict(variant="1000x720", theme="light", lh=16, gap=6))]]),
            ("vw-visualization", "The visualization viewer, fitted: windowed and fullscreen, both "
             "plots; dark", "dark",
             [[("viewer", "spectrogram-windowed-meter", None, "Windowed, the level meter on", dict(fit=(590, 400), lh=16, gap=6)),
               ("viewer", "spectrogram-fullscreen-title-strip", None, "Fullscreen, its title strip", dict(fit=(590, 400), lh=16, gap=6))],
              [("viewer", "waveform-windowed", None, "Windowed, waveform", dict(fit=(590, 400), lh=16, gap=6)),
               ("viewer", "waveform-fullscreen", None, "Fullscreen, waveform", dict(fit=(590, 400), lh=16, gap=6))]]),
            ("vw-spectrogram-controls", "The visualization viewers' corners at 1:1: the windowed "
             "plate, the fullscreen strip; dark", "dark",
             [[("viewer", st, (0.75, 0, 0.25, 0.075), lb, dict(lh=16, gap=6)) for st, lb in
               (("spectrogram-windowed", "Windowed, at rest"), ("spectrogram-windowed-zoomed", "Zoomed"),
                ("spectrogram-windowed-reset-hover", "Reset Zoom hovered"))],
              [("viewer", st, (0.78, 0, 0.22, 0.06), lb, dict(lh=16, gap=6)) for st, lb in
               (("spectrogram-fullscreen-close-hover", "Fullscreen: Close hovered"),
                ("spectrogram-fullscreen-close-focus", "Close focused"), ("spectrogram-fullscreen-zoomed", "Zoomed: the readout"))],
              [("viewer", st, (0.78, 0, 0.22, 0.06), lb, dict(lh=16, gap=6)) for st, lb in
               (("spectrogram-fullscreen-no-strip", "Title strip turned off"),
                ("spectrogram-fullscreen-auto-hidden", "Auto-hidden after 6.5 s"), ("waveform-fullscreen", "Waveform"))],
              [("viewer", "spectrogram-fullscreen-title-strip", (0, 0, 0.3, 0.06), "Fullscreen: the title and the lane head", dict(lh=16, gap=6)),
               ("viewer", "spectrogram-windowed-hover", (0, 0, 0.3, 0.084), "Windowed: the lane head, pointer over the plot", dict(lh=16, gap=6))]]),
            ("vw-24pt", "The viewers at 24 pt (fitted); dark", "dark",
             [[("viewer", st, None, lb, dict(variant="font24", fit=(580, 406), lh=16, gap=6)) for st, lb in row]
              for row in ((("spectrogram-windowed", "Spectrogram, windowed"), ("spectrogram-fullscreen", "Fullscreen")),
                          (("albumart-windowed", "Album art, windowed"), ("albumart-fullscreen", "Fullscreen")))]),
        ],
        images=[
            ("sheet", "vw-albumart", "dark", "The album art viewer windowed with its info, and fullscreen (fitted)",
             "the art's frame; the info column; the strip against the screen"),
            ("sheet", "vw-albumart-controls", "dark", "Its corner plate and strip: hover, press (pointer, Space), focus; its menus, 1:1",
             "the plate's and strip's buttons in each state; the menus"),
            ("sheet", "vw-info", "dark", "The info column at 1:1: windowed, fullscreen, narrow (light)", "its rows, labels and wrap"),
            ("strip", "albumart-info-toggle", "dark", "The info column shown, then hidden (windowed)", "the art re-centring; content cuts"),
            ("strip", "albumart-open-windowed", "dark", "The album art viewer opening, windowed", "the first frame; blink"),
            ("strip", "albumart-close-windowed", "dark", "The album art viewer closing, windowed", "the last frames; blink"),
            ("sheet", "vw-visualization", "dark", "The visualization viewer windowed and fullscreen, both plots (fitted)",
             "the plot filling its window; the meter; the title strip"),
            ("sheet", "vw-spectrogram-controls", "dark", "The viewers' corners at 1:1: plate, strip, hidden",
             "hover, focus, the readout; what stays when the controls hide"),
            ("sheet", "vw-24pt", "dark", "The viewers at 24 pt (fitted)", "the strip, plate and info at a large font"),
            ("strip", "viewer-open-windowed", "dark", "The spectrogram viewer opening, windowed", "the first frame; blink"),
            ("strip", "viewer-close-windowed", "dark", "The spectrogram viewer closing, windowed", "the last frames; blink"),
            ("strip", "viewer-autohide-fullscreen", "dark", "The fullscreen viewer's controls hiding", "the hide's fade; what stays"),
        ],
        facts=["plate-", "fill-albumart", "fill-spectrogram", "screen-"],
        facts_strips=[("albumart-info-toggle", "light", "The info column shown, then hidden, light"),
                      ("albumart-autohide-fullscreen", "dark", "Album art viewer, fullscreen: its controls hiding"),
                      ("viewer-strip-show-fullscreen", "dark", "Spectrogram viewer, fullscreen: its strip showing again")],
        notes=["The screen is KWin's virtual output (screen-* below): a fullscreen viewer fills it.",
               "Recordings are the app's own frames: KWin's window animations (a viewer window opening, fullscreen entry "
               "and exit) are not in them; a new window's first frame is drawn whole."],
        floors=[],
        a11y_labels=[],
        contrast={},
    ),
    # The app as one system: whole windows at the main sizes, both themes, 24 pt, 2x, its states side by side, the
    # keyboard focus walk, and the contrast and layout facts of these images
    "whole-app": dict(
        title="The whole app: spacing, type, colour, states, motion, consistency, identity, KDE fit, accessibility",
        sessions=["main", "empty", "playing", "narrow", "narrow-bigfont", "bigfont", "hidpi", "focus", "motion", "dialogs",
                  "menus", "contrast"],
        sheets=[
            *[(f"wa-wide-{th}", f"The main window, 1600x1000, {th}, playing (centred spectrogram, per channel), whole, fitted "
               "to 1 MP (scale in the label)", th,
               [[("main", "playing-spectrogram-centered", None, f"1600x1000, {th}", dict(fit=(1240, 775), lh=16, gap=6))]])
              for th in ("dark", "light")],
            *[(f"wa-narrow-{th}", f"The narrow window, 1000x720, {th}, playing, whole at 1:1", th,
               [[("main", "playing-spectrogram-centered", None, f"1000x720, {th}, 1:1", dict(variant="1000x720", lh=16, gap=6))]])
              for th in ("dark", "light")],
            ("wa-24pt", "Noto Sans 24 pt: the main window at 1600x1000 (the compact sidebar), whole, fitted; dark", "dark",
             [[("main", "playing-spectrogram-centered", None, "24 pt, 1600x1000", dict(variant="1600x1000_font24", fit=(1240, 775), lh=16, gap=6))]]),
            ("wa-24pt-narrow", "Noto Sans 24 pt in the narrow window (1000x720), playing, whole at 1:1; light", "light",
             [[("main", "playing-spectrogram-centered", None, "24 pt, 1000x720, light, 1:1", dict(variant="1000x720_font24", lh=16, gap=6))]]),
            ("wa-2x", "Scale 2 (HiDPI), dark, at 1:1 device pixels: the main window's top left (512x480 logical px), playing",
             "dark", [[("main", "playing-spectrogram-centered", (0, 0, 0.32, 0.48), "Scale 2, device pixels", dict(scale=2, lh=16, gap=6))]]),
            *[(f"wa-states-{th}", f"The app's states side by side, {th}, each window fitted (scale in the label): first run "
               "(no library folder), idle (library, nothing queued), playing, and a file that ends early (an issue reported)", th,
               [[("main", st, None, lb, dict(fit=(620, 388), lh=16, gap=6)) for st, lb in row]
                for row in ((("first-run-empty-library", "First run"), ("library-idle", "Idle")),
                            (("playing-spectrogram-centered", "Playing"), ("playing-ends-early", "A file that ends early")))])
              for th in ("dark", "light")],
            ("wa-surfaces", "The app's surfaces side by side (scale in the label where fitted); dark", "dark",
             [[("dialog", "preferences-general", None, "Configure", dict(fit=(560, 380), lh=16, gap=6)),
               ("dialog", "about", None, "About", dict(fit=(560, 380), lh=16, gap=6))],
              [("search", "results", (0.24, 0.1, 0.52, 0.6), "Search over the window", dict(fit=(560, 408), lh=16, gap=6)),
               ("menu", "playback-playing", (0, 0, 0.4, 0.48), "A menu over the window", dict(fit=(560, 408), lh=16, gap=6))]]),
            ("wa-focus-walk", "The keyboard focus walk (Tab, real keys): at launch, then Tab 1 to 15; "
             "1:1 at each stop, dark", "dark",
             [[("focus", "launch-default" if n == 0 else f"tab-{n:02d}", "focus", "At launch" if n == 0 else f"Tab {n}",
                dict(around=(360, 120), lh=16, gap=4)) for n in range(i, i + 4)] for i in (0, 4, 8, 12)]),
            ("wa-focus-walk-light", "The focus walk at 24 pt, Tab 1 to 12; 1:1 at each stop, light", "light",
             [[("focus", f"tab-{n:02d}", "focus", f"Tab {n}", dict(variant="1600x1000_font24", around=(360, 130), lh=16, gap=4))
               for n in range(i, i + 4)] for i in (1, 5, 9)]),
        ],
        images=[
            ("sheet", "wa-wide-dark", "dark", "The main window, 1600x1000, dark, playing (fitted)",
             "the grid: the sidebar, the queue and the pane; alignment and spacing between regions; hierarchy"),
            ("sheet", "wa-wide-light", "light", "The same, light (fitted)", "the light theme as a system: surfaces, separators, accents"),
            ("sheet", "wa-narrow-dark", "dark", "The narrow window, 1000x720, dark, 1:1", "what gives way in a small window"),
            ("sheet", "wa-narrow-light", "light", "The narrow window, light, 1:1", "the same in light"),
            ("sheet", "wa-24pt", "dark", "24 pt at 1600x1000 (fitted)", "the type scale at a large font; what wraps or elides"),
            ("sheet", "wa-24pt-narrow", "light", "24 pt in the narrow window, light, 1:1", "the tightest layout"),
            ("sheet", "wa-2x", "dark", "Scale 2 at 1:1 device pixels", "crispness: icons, hairlines, glyphs at 2x"),
            ("sheet", "wa-states-dark", "dark", "First run, idle, playing, an error: dark (fitted)",
             "each state's message and call to action; how the error shows without a banner"),
            ("sheet", "wa-states-light", "light", "The same, light (fitted)", "the same in light"),
            ("sheet", "wa-surfaces", "dark", "Configure, About, search and a menu side by side (fitted)",
             "one system across windows and popups: headers, margins, buttons, type; KDE platform fit"),
            ("sheet", "wa-focus-walk", "dark", "The keyboard focus walk, stops 0-15, 1:1", "the ring on every stop; the order"),
            ("sheet", "wa-focus-walk-light", "light", "The focus walk at 24 pt, light, stops 1-12, 1:1", "the ring at a large font in light"),
            ("strip", "first-play-from-idle-window", "dark", "First Play from idle: the whole window, as a transition",
             "what moves at once when playback starts; consistency of the motion"),
        ],
        facts=["screen-", "status-jumps-"],
        system_facts=True,  # contrast and layout summarised, and the focus order
        notes=["The wide window (1600x1000) cannot be shown at 1:1 within 1.15 MP: it is fitted (scale in its label); the "
               "narrow window (1000x720) and the 2x crop are 1:1. The other packs hold each surface at 1:1.",
               "The focus order below is the 10 pt dark walk's; the round's FOCUS-A11Y.md has every walk's states too."],
        floors=[],
        a11y_labels=[],
        contrast={"wa-wide-dark": ["queue", "library", "now playing", "transport", "status"],
                  "wa-wide-light": ["queue", "library", "now playing", "transport", "status"],
                  "wa-narrow-dark": ["queue", "library", "now playing"], "wa-narrow-light": ["queue", "library", "now playing"],
                  "wa-states-dark": ["first run", "empty", "placeholder", "issue"],
                  "wa-states-light": ["first run", "empty", "placeholder", "issue"],
                  "wa-focus-walk": ["focus"], "wa-focus-walk-light": ["focus"]},
    ),
})

# The census: the whole app graded surface by surface, one small pack per reviewer task, all from one run
# (`shoot_round.sh <round> --census`, or "census" in a --focus/--focus-only list).
CENSUS = ["nowplaying-plot", "viz-interaction", "chrome", "library-search", "dialogs", "viewers", "whole-app"]


def resolve(spec):
    """The scopes a --focus/--focus-only value names, in its order, each once: a comma-separated list of scope
    names, where "census" stands for the census scopes. Raises KeyError naming an unknown scope."""
    names = []
    for part in (p.strip() for p in (spec or "").split(",")):
        for nm in (CENSUS if part == "census" else [part] if part else []):
            if nm not in SCOPES:
                raise KeyError(nm)
            if nm not in names:
                names.append(nm)
    if not names:
        raise KeyError(spec or "")
    return names


def sessions_of(names):
    """The sessions the scopes need, each once, in the order the scopes list them."""
    out = []
    for nm in names:
        out += [s for s in SCOPES[nm]["sessions"] if s not in out]
    return out


if __name__ == "__main__":  # focus.py SPEC: the scopes (line 1) and the sessions they need (line 2), comma-separated
    import sys
    try:
        _names = resolve(sys.argv[1] if len(sys.argv) > 1 else "")
    except KeyError as _ex:
        sys.exit(f"no focus scope {_ex} in focus.py (scopes: {', '.join(SCOPES)}; \"census\": {', '.join(CENSUS)})")
    print(",".join(_names))
    print(",".join(sessions_of(_names)))
