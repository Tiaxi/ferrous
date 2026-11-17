#!/usr/bin/env python3
"""Focus packs: one surface reviewed at a time from a small pack. A scope names the sessions it
needs (for `shoot_round.sh --focus-only`), its own sheets (built like states.CUSTOM_SHEETS) and its images in order:
each with what it shows and what to look at, and the facts (facts.jsonl keys, floor regions) FOCUS-FACTS.md carries.
`shoot_round.sh <round> <src> --focus SCOPE` writes <round>/focus/ (FOCUS.md, FOCUS-FACTS.md, the images) after the
whole pack; add a scope here.

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

SCOPES = {
    "nowplaying-plot": dict(
        title="Now playing and the visualization pane",
        sessions=["main", "playing", "bigfont", "narrow", "narrow-bigfont", "motion", "firstplay-noart"],
        # now playing only (the pane, motion and states have their own scopes): the cards in every layout
        # and font at 1:1, the idle row, paused and stopped, the title change at 1:1; one recording each of First Play,
        # art <-> no art and Stop -> idle
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
            ("focus-np-24pt-narrow", "Now playing at 24 pt, 1000x720 (the narrow window), 1:1: a short, a medium and a long "
             "title and a tag-less file; dark above light (the title's lines and elision: FOCUS-FACTS card-title-*)", "dark",
             [[("nowplaying", f"title-{k}", "subject", f"{lb}, {th}", dict(variant="1000x720_font24", theme=th, margin=4, lh=16, gap=6))
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
            # paused, and stopped with the track still loaded (Stop alone): the transport and the card, 1:1
            ("focus-np-paused-stopped", "Paused, and stopped with the track loaded (Stop alone, not Clear), wide at the default "
             "font, 1:1: the window's top left (the transport and the card); dark above light", "dark",
             [[("nowplaying", st, (0, 0, 0.3, 0.5), f"{lb}, {th}", dict(theme=th, lh=16, gap=6))
               for st, lb in (("paused", "Paused"), ("stopped-with-track", "Stopped, track loaded"))] for th in ("dark", "light")]),
            # medium -> long title in the narrow window at 24 pt, the card at 1:1 frame by frame
            ("focus-title-change", "A track change in the narrow window at 24 pt, from a medium title (two lines) to a long "
             "one: the now-playing panel at 1:1, frame by frame (dark; the title's size before and after: FOCUS-FACTS)", "dark",
             [[dict(strip="title-change-narrow24", item="nowPlayingPanel", times=ts, zoom=1, pad=6, below=20, lh=16, gap=6,
                    label="about " + ", ".join(f"{t:+d} ms" for t in ts))]
              for ts in ([-30, 40, 100], [160, 240, 500])]),
            # First Play's text block sampled through the reveal; with 20 px of context below
            ("focus-firstplay-text", "First Play from idle: the now-playing text block at 1:1, frame by frame through the "
             "reveal (the block followed as the card grows; 20 px below it), dark and light", "dark",
             [[dict(strip="first-play-from-idle", theme=th, item="trace:nowPlayingWideText", times=ts, zoom=1, pad=3, below=20,
                    lh=16, gap=6, label=f"{th}: about " + ", ".join(f"+{t} ms" for t in ts))]
              for th in ("dark", "light") for ts in ([250, 330, 390], [420, 450, 520])]),
        ],
        images=[
            ("sheet", "focus-np-wide", "dark", "The wide now playing in the window, with album art and without (the "
             "no-art mark), and the library's no-art row, both themes, 1:1", "the no-art mark among the card's text; "
             "the 20 px tile against the cover's mark"),
            ("sheet", "focus-np-24pt-compact", "dark", "24 pt, 1600x1000: short, medium and long titles, a tag-less file, "
             "both themes, 1:1", "the title beside the cover on one or two lines, then elided; the lines below"),
            ("sheet", "focus-np-24pt-narrow", "dark", "24 pt, 1000x720: the same four cases, both themes, 1:1",
             "the same in the narrow window"),
            ("sheet", "focus-np-narrow", "dark", "The narrow window at the default font, with and without art, both themes, 1:1",
             "the narrow card at the default font: the cover filling its block, the lines; without art the no-art mark"),
            ("sheet", "focus-np-idle", "dark", "The idle row at 10 pt and 24 pt, wide and narrow, both themes, 1:1",
             "the idle row's glyph and text at both fonts"),
            ("sheet", "focus-np-paused-stopped", "dark", "Paused, and stopped with the track loaded, both themes, 1:1",
             "what the card and the transport show when paused and when stopped (Stop alone)"),
            ("sheet", "focus-title-change", "dark", "Medium -> long title in the narrow window at 24 pt, the panel 1:1 frame by frame",
             "whether the title's size changes in use (its rect before and after: FOCUS-FACTS)"),
            ("sheet", "focus-firstplay-text", "dark", "First Play: the text block frame by frame through the reveal, 1:1",
             "the lines arriving: their place and opacity as the card grows"),
            ("strip", "pair:first-play-themes", "dark", "First Play from idle, dark (left) and light (right)",
             "the idle row gone before the cover; blink per element and content cuts in the facts"),
            ("strip", "pair:art-to-no-art-themes", "dark", "Album art to none (Next), wide, dark (left) and light (right)",
             "the no-art mark replacing the cover; the text block's lines; text overlap per line"),
            ("strip", "pair:stop-to-idle-themes", "dark", "Stop, then Clear the queue: the card back to idle, dark (left) "
             "and light (right)", "what the card ends on and any content cut (measured in the facts); the idle row arriving"),
        ],
        facts=["noart-mark", "library-noart-glyph", "info-toggle-edge", "card-"],
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
                      ("queue-end-to-idle", "dark", "The queue running out on its own", dict(watch=[("the idle row", "nowPlayingIdleRow"), ("the track's lines", "nowPlayingLines"), ("the cover", "nowPlayingCover")]))],
        floors=[],  # (the pane has its own scope)
        a11y_labels=[],
        # CONTRAST.md rows quoted under each focus sheet (by element-name fragment, case ignored)
        contrast={"focus-np-wide": ["now-playing", "library row title", "library header"],
                  "focus-np-24pt-compact": ["now-playing", "library"], "focus-np-24pt-narrow": ["now-playing", "library"],
                  "focus-np-narrow": ["now-playing"], "focus-np-idle": ["now-playing"],
                  "focus-np-paused-stopped": ["now-playing"]},
    ),
    # The chrome: the transport bar, the status bar, the queue (header and rows), at 1:1 in context, both
    # themes; the measured contrasts of each text, glyph and hover/selected state (CONTRAST.md rows), the status bar's
    # items while numbers change (status-jumps), and the recordings' handover and content-cut facts
    "chrome": dict(
        title="The chrome: the queue (the transport and the status bar have their own images)",
        sessions=["main", "queue", "empty", "bigfont", "narrow", "narrow-bigfont", "motion", "contrast"],
        # the queue again: its states, focus, play states with 4x glyph insets, sizes, motion
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
            for th in ("dark", "light")
        ] + [
            ("focus-queue-focus", "The queue's keyboard focus, reached with real keys, 1:1: in the long list scrolled to its "
             "end (Tab, End, Up: row 49 of 50), beside the scrollbar's thumb (the list's lower part); in a queue of 4 rows "
             "with no scrollbar (row 3 playing; Tab, Down: unreadable row 4); dark, then light", "dark",
             [[("queue", "focus-scrolled", "subject", f"Long list, scrollbar shown, {th}", dict(theme=th, part=(0, 0.612, 1, 0.388), lh=16, gap=6))]
              for th in ("dark", "light")]
             + [[("queue", "focus-short", "subject", f"4 rows, no scrollbar, {th}", dict(theme=th, part=(0, 0, 1, 0.29), lh=16, gap=6))]
                for th in ("dark", "light")]),
            ("focus-queue-more", "The queue, 1:1: the ring on an unselected row (Tab alone; the list scrolled to row 26, its "
             "thumb lower down); a column's resize handle hovered and focused (real Tab); long names elided; dark, light", "dark",
             [[("queue", "ring-unselected", "subject", f"Ring on unselected row 26, {th}", dict(theme=th, part=(0, 0, 1, 0.23), lh=16, gap=6))]
              for th in ("dark", "light")]
             + [[("queue", st, "subject", f"{lb}, {th}", dict(theme=th, part=(0, 0, 1, 0.06), lh=16, gap=6))]
                for th in ("dark", "light") for st, lb in (("handle-hover", "Album's resize handle hovered"),
                                                            ("handle-focus", "A resize handle focused (Tab)"))]
             + [[("queue", "elision", "subject", f"Long artist, album and title, {th}", dict(theme=th, part=(0, 0, 1, 0.28), lh=16, gap=6))]
                for th in ("dark", "light")]),
            ("focus-queue-playstate", "The queue, 1:1: the playing row paused (amber, by design) and stopped (no amber; the "
             "track still loaded); unreadable row 4 just after it failed (dimmed at once; its warning mark while still the "
             "current row) and once row 3 plays; dark, then light", "dark",
             [[("queue", st, "subject", f"{lb}, {th}", dict(theme=th, part=pt, lh=16, gap=6))]
              for th in ("dark", "light")
              for st, lb, pt in (("paused-row", "Paused", (0, 0, 1, 0.17)), ("stopped-row", "Stopped", (0, 0, 1, 0.17)),
                                 ("unreadable", "4-row queue: row 4 unreadable, just failed (the current row)", (0, 0.1665, 1, 0.1235)),
                                 ("unreadable-later", "Row 4 unreadable, row 3 playing", (0, 0.1665, 1, 0.1235)))]),
            # each state glyph at 1:1 with a 4x nearest-neighbour inset; the unreadable row with the
            # status bar's right half from the same frame (its warning beside the row's)
            ("focus-queue-glyphs", "The queue's state glyphs, 1:1 (the row's first 240 px) with a 4x inset (nearest "
             "neighbour): playing, paused, stopped, unreadable; dark, light. Then unreadable row 4 and the status bar's right "
             "half from the same frame", "dark",
             [[g for th in ("dark", "light") for g in (
                 ("queue", st, "subject", f"{lb}, {th}", dict(theme=th, box=(0, top, 240, 26), lh=16, gap=6)),
                 ("queue", st, "subject", "4x", dict(theme=th, box=(4, top + 1, 24, 24), zoom=4, lh=16, gap=6)))]
              for st, lb, top in (("at-rest", "Playing", 26), ("paused-row", "Paused", 26), ("stopped-row", "Stopped", 26),
                                  ("unreadable-later", "Unreadable (row 4)", 104))]
             + [r_ for th in ("dark", "light") for r_ in (
                 [("queue", "unreadable-later", "subject", f"Unreadable row 4 (row 3 playing), {th}", dict(theme=th, part=(0, 0.222, 1, 0.0675), lh=16, gap=6))],
                 [("queue", "unreadable-later", (0.55, 0.974, 0.45, 0.026), f"The same frame's status bar, right half, {th}", dict(theme=th, lh=16, gap=6))])]),
            ("focus-queue-drop-empty", "The queue, 1:1: the drop indicator between rows 3 and 4 (made visible directly: a "
             "drag cannot be driven headless), dark and light; emptied (dark, the pane's top 62 %); the first run, both themes",
             "dark",
             [[("queue", "drop-indicator", "subject", f"Drop indicator, {th}", dict(theme=th, part=(0, 0.111, 1, 0.167), lh=16, gap=6))]
              for th in ("dark", "light")]
             + [[("queue", "empty", "subject", "Emptied, dark (the pane's top 62 %)", dict(part=(0, 0, 1, 0.62), lh=16, gap=6))]]
             + [[("main", "first-run-empty-library", (0.45, 0.235, 0.34, 0.2), f"First run, {th}", dict(theme=th, lh=16, gap=6))
                 for th in ("dark", "light")]]),
            ("focus-queue-sizes", "The queue, 1:1, at 24 pt (1600x1000), its header and first rows, dark and light", "dark",
             [[("queue", "at-rest", "subject", f"24 pt, {th}", dict(variant="1600x1000_font24", theme=th, part=(0, 0, 1, 0.5), lh=16, gap=6))]
              for th in ("dark", "light")]),
            ("focus-queue-narrow", "The queue, 1:1, in the narrow window (1000x720): at 10 pt and at 24 pt, dark and light", "dark",
             [[("queue", "at-rest", "subject", f"Narrow, 10 pt, {th}", dict(variant="1000x720", theme=th, part=(0, 0, 1, 0.34), lh=16, gap=6))
               for th in ("dark", "light")],
              [("queue", "at-rest", "subject", f"Narrow, 24 pt, {th}", dict(variant="1000x720_font24", theme=th, part=(0, 0, 1, 0.66), lh=16, gap=6))
               for th in ("dark", "light")]]),
            # the recordings at 1:1 frame by frame (a pair fitted to 1 MP showed them at a fifth of their size)
            ("focus-queue-now", "Next: the queue's playing row (scrolled to the list's second row first) moving down one "
             "row; the list's first three rows at 1:1, frame by frame; dark (top three rows), light (bottom three)", "dark",
             [[dict(strip="queue-now-move", theme=th, item="mainQueuePane:0,0.05,0.5,0.18", times=ts, zoom=1, pad=0, lh=16,
                    gap=6, label=f"{th}: about " + ", ".join(f"{t:+d} ms" for t in ts))]
              for th in ("dark", "light") for ts in ([-30, 15], [40, 70], [110, 250])]),
            ("focus-queue-hover-fade", "A queue row's hover tint fading in, then out as the pointer leaves, at 1:1 frame by "
             "frame (the rows' first 500 px): dark (top four rows), light (bottom four)", "dark",
             [[dict(strip="hover-fade-queue-row", theme=th, item=(0, 0, 500, None), times=ts, zoom=1, pad=0, lh=16, gap=6,
                    label=f"{th}: about " + ", ".join(f"{t:+d} ms" for t in ts))]
              for th in ("dark", "light") for ts in ([-30, 20], [45, 80], [340, 380], [405, 440])]),
        ],
        images=[
            ("sheet", "focus-queue", "dark", "The queue, dark, 1:1: rest, hover, selected, multi-selection, pressed",
             "the header; the playing row's amber; the hover tint; selection; a range; the pressed row"),
            ("sheet", "focus-queue-light", "light", "The queue, light, 1:1: the same", "the same in light"),
            ("sheet", "focus-queue-hover", "dark", "Hover on selected and playing rows; the inactive selection; dark",
             "whether a selected row answers hover; the inactive selection; the playing row selected"),
            ("sheet", "focus-queue-hover-light", "light", "The same in light", "the same in light"),
            ("sheet", "focus-queue-focus", "dark", "Keyboard focus (real keys) beside the scrollbar and with none",
             "the ring's right edge against the scrollbar's lane and thumb (edges in FOCUS-FACTS' table)"),
            ("sheet", "focus-queue-more", "dark", "The ring on an unselected row; the column handles; elision",
             "the ring alone, its thumb elsewhere; the handle's hover and focus lines; how long names elide"),
            ("sheet", "focus-queue-playstate", "dark", "The playing row paused and stopped; an unreadable row",
             "pause in amber (by design), stop without; the unreadable row's warning glyph and dimmed text"),
            ("sheet", "focus-queue-glyphs", "dark", "Each state glyph at 1:1 with a 4x inset; the unreadable row and the status bar",
             "the glyphs' shapes (the warning's \"!\"); the row's warning beside the status bar's"),
            ("sheet", "focus-queue-drop-empty", "dark", "The drop indicator, both themes; emptied; the first run",
             "the drop line against the row boundary; the empty message; the call to action"),
            ("sheet", "focus-queue-sizes", "dark", "The queue at 24 pt, both themes", "the header and rows at a large font"),
            ("sheet", "focus-queue-narrow", "dark", "The queue in the narrow window at 10 and 24 pt, both themes",
             "what elides and crowds in a narrow window"),
            ("sheet", "focus-queue-now", "dark", "Next: the playing row moving down a row, at 1:1 frame by frame, dark and light",
             "the amber marker's move between rows (its handover in the facts)"),
            ("sheet", "focus-queue-hover-fade", "dark", "A row's hover fading in and out, at 1:1 frame by frame, dark and light",
             "the hover tint's fade in both themes"),
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
        facts=["queue-focus", "queue-now-", "queue-unreadable"],
        # read before judging a colour by eye
        notes=["The sheets are the app's own frames, and its text is drawn with subpixel (RGB) antialiasing: thin strokes "
               "carry coloured fringes, so grey text such as the queue's \u201cLength\u201d header can look blue (or "
               "orange) at 1:1 and in a zoom while its measured colour is grey. Judge text colours by the contrast rows "
               "and the key colours, which are measured, not by the fringes."],
        facts_strips=[("queue-now-move", "dark", "Next: the playing row moving down, dark"),
                      ("queue-now-move", "light", "Next: the playing row moving down, light"),
                      ("hover-fade-queue-row", "dark", "A queue row's hover fading in and out, dark"),
                      ("hover-fade-queue-row", "light", "A queue row's hover fading in and out, light")],
        floors=[],
        a11y_labels=[],
        # CONTRAST.md rows quoted under each focus sheet (by element-name fragment, case ignored)
        contrast={"focus-queue": ["queue title", "queue artist", "queue track", "queue header", "queue hover", "playing row"],
                  "focus-queue-light": ["queue title", "queue artist", "queue track", "queue header", "queue hover", "playing row"],
                  "focus-queue-hover": ["queue hover", "playing row"], "focus-queue-hover-light": ["queue hover", "playing row"],
                  "focus-queue-sizes": ["queue title", "queue header"]},
    ),
    # Dialogs and viewers. Configure tiles are fitted to 770x600: 1:1 once Configure is sized to its
    # content (about 760x560), labelled with their scale while it is larger. Confirmations: the innermost window
    # with its title bar and a margin of its parent ("inner"); main-window popups (search, Recent Issues): "popup".
    "dialogs": dict(
        title="Dialogs and viewers",
        sessions=["dialogs", "viewers", "menus", "statusbar", "roots", "bigfont", "narrow", "motion", "motion-ui"],
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
            ("dlg-confirmations", "Confirmations as they open, 1:1, each window with a margin of its parent: delete the "
             "diagnostics log, replace the album art, discard tag edits (dark, light); the quit failure (dark) and Remove "
             "Library Folder (light)", "dark",
             [[("dialog", st, crop, f"{lb}, {th}", dict(theme=th, margin=16, lh=16, gap=6)) for th in ("dark", "light")]
              for st, lb, crop in (("confirm-clear-diagnostics", "Delete diagnostics log", "inner"),
                                   ("confirm-replace-cover", "Replace album art", "inner"),
                                   ("tag-editor-close-confirm", "Unsaved tag changes", "inner"))]
             + [[("dialog", "quit-save-failed", None, "Couldn't save before quitting, dark", dict(theme="dark", lh=16, gap=6)),
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
            ("dlg24-library", "At 24 pt, dark, 1:1: Configure's Library page (the heading, the folder row with its glyph and "
             "full path, the buttons); its footer's two ends; Replace from iTunes' count line", "dark",
             [[("dialog", "preferences-library", None, "Library page: heading, folder list, buttons", dict(B24, part=(0.13, 0.025, 0.84, 0.215)))],
              [("dialog", "preferences-library", None, "Footer, left", dict(B24, part=(0, 0.935, 0.1, 0.065))),
               ("dialog", "preferences-library", None, "Footer, right", dict(B24, part=(0.78, 0.935, 0.22, 0.065)))],
              [("dialog", "itunes-results", None, "Replace from iTunes: the count line (16-26 % down)",
                dict(B24, part=(0, 0.16, 1, 0.1)))]]),
            ("dlg24-folders", "At 24 pt, dark, 1:1: Add Library Folder (whole); Remove Library Folder over Configure (its "
             "window with 4 px of Configure)", "dark",
             [[("dialog", "library-root-name-add", None, "Add Library Folder, whole", B24)],
              [("dialog", "confirm-remove-library-root", "inner", "Remove Library Folder", dict(B24, margin=4))]]),
            ("dlg24-about", "At 24 pt, 1:1: About's header (icon, name, version, tabs), dark and light; About KDE's "
             "header; both footers' ends (dark)", "dark",
             [[("dialog", "about", None, "About: header and tabs", dict(B24, part=(0, 0.06, 0.75, 0.4)))],
              [("dialog", "about", None, "About: header and tabs, light", dict(B24, theme="light", part=(0, 0.06, 0.75, 0.4)))],
              [("dialog", "about-kde", None, "About KDE: header and first lines", dict(B24, part=(0, 0.035, 1, 0.16)))],
              [("dialog", "about", None, "About: footer, left", dict(B24, part=(0, 0.83, 0.4, 0.17))),
               ("dialog", "about", None, "About: Close", dict(B24, part=(0.85, 0.83, 0.15, 0.17))),
               ("dialog", "about-kde", None, "About KDE: Close", dict(B24, part=(0.85, 0.91, 0.15, 0.09)))]]),
            ("dlg24-diagnostics", "At 24 pt, dark, 1:1: Diagnostics (the log file line and the toolbar, left and right; "
             "its Close); Delete Diagnostics Log over it", "dark",
             [[("dialog", "diagnostics", None, "Diagnostics: log file line and toolbar, left", dict(B24, part=(0, 0.025, 0.42, 0.095))),
               ("dialog", "diagnostics", None, "The same, right", dict(B24, part=(0.55, 0.025, 0.45, 0.095)))],
              [("dialog", "confirm-clear-diagnostics", "inner", "Delete Diagnostics Log", dict(B24, margin=4)),
               ("dialog", "diagnostics", None, "Diagnostics: Close", dict(B24, part=(0.85, 0.93, 0.15, 0.07)))]]),
            ("dlg24-quit", "At 24 pt, dark, 1:1: Couldn't Save Before Quitting (whole); Finishing Tag Changes over Edit "
             "Tags (its window with 4 px of Edit Tags)", "dark",
             [[("dialog", "quit-save-failed", None, "Couldn't Save Before Quitting", B24),
               ("dialog", "tag-editor-quit-wait", "inner", "Finishing Tag Changes", dict(B24, margin=4))]]),
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
            ("dlg24-details", "At 24 pt, dark, 1:1: Tag Editor Details after a real save with one folder not writable "
             "(whole, with 4 px of Edit Tags); Replace from iTunes' first result loading its artwork", "dark",
             [[("dialog", "tag-editor-status-details", "inner", "Tag Editor Details", dict(B24, margin=4))],
              [("dialog", "itunes-results-loading", None, "Replace from iTunes: the first result, its full-size image loading",
                dict(B24, items=["itunesArtworkResultTitle", "itunesArtworkResultDetails", "itunesArtworkPreviewButton",
                                 "itunesArtworkApplyButton"], first=True, margin=4, grow=(130, 0, 0, 0)))]]),
            ("dlg24-tag-tools", "At 24 pt, dark, 1:1: Auto Number; Edit Tags (top-left; footer ends)", "dark",
             [[("dialog", "tag-editor-auto-number", "inner", "Auto Number", dict(B24, margin=4))],
              [("dialog", "tag-editor", None, "Edit Tags, top-left", dict(B24, part=(0, 0, 0.6, 0.35)))],
              [("dialog", "tag-editor", None, "Edit Tags footer, left", dict(B24, part=(0, 0.89, 0.34, 0.11))),
               ("dialog", "tag-editor", None, "Edit Tags footer, right", dict(B24, part=(0.8, 0.89, 0.2, 0.11)))]]),
            ("dlg24-replace", "At 24 pt, dark, 1:1: Unsaved Changes; Replace Album Art; the failed iTunes search's status",
             "dark",
             [[[("dialog", "tag-editor-close-confirm", "inner", "Unsaved Changes", dict(B24, margin=4)),
                ("dialog", "confirm-replace-cover", "inner", "Replace Album Art", dict(B24, margin=4))],
               ("dialog", "itunes-error", None, "iTunes search failed, the status", dict(B24, part=(0.33, 0.42, 0.34, 0.16)))]]),
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
            ("sheet", "dlg-confirmations", "dark", "Confirmations, the quit failure, Remove Folder",
             "which button is the default and has focus; the wording"),
            ("sheet", "dlg-tag-editor", "dark", "Edit Tags: at rest, one typed edit, committed", "the fields, the table, the mark"),
            ("sheet", "dlg-itunes-controls", "dark", "iTunes searching -> failed frames; results; Preview/Apply, revert glyphs, "
             "Recent Issues links incl. Tab focus", "the failed state as it arrives; the flat controls' states"),
            ("sheet", "dlg24-library", "dark", "24 pt: Library page's folder row, buttons; Configure's footer; iTunes' count line",
             "the folder glyph and full path; the footer"),
            ("sheet", "dlg24-folders", "dark", "24 pt: Add Library Folder; Remove Library Folder", "the field, the wrap"),
            ("sheet", "dlg24-about", "dark", "24 pt: About's header dark and light; About KDE's header; the footers", "the icon, the tabs, the footers"),
            ("sheet", "dlg24-diagnostics", "dark", "24 pt: Diagnostics' log file line, toolbar and Close; Delete Diagnostics Log",
             "the toolbar; the confirmation"),
            ("sheet", "dlg24-quit", "dark", "24 pt: Couldn't Save Before Quitting; Finishing Tag Changes", "the wrap; the buttons"),
            ("sheet", "dlg24-rename", "dark", "Rename Files, twelve files: 24 pt whole (fitted) and in part; 10 pt whole",
             "the preview's width against its content; the arrows"),
            ("sheet", "dlg24-details", "dark", "24 pt: Tag Editor Details; an iTunes result loading its artwork",
             "the details' width and wrap; the loading line uncut"),
            ("sheet", "dlg24-tag-tools", "dark", "24 pt: Auto Number; Edit Tags and its footer", "the check boxes' edge; the form"),
            ("sheet", "dlg24-replace", "dark", "24 pt: Unsaved Changes; Replace Album Art; iTunes failed", "the wording; the buttons"),
            ("sheet", "dlg24-light", "light", "24 pt: Recent Issues dark and light; Edit Tags' Track and Disc placeholders",
             "the popup's width, the glyph; the placeholders uncut"),
        ],
        facts=["dlg-", "screen-", "itunes-failed-", "tagsave-", "renamebatch-", "recent-issues-opened", "plate-", "fill-"],
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
               "The screen is KWin's virtual output (screen-* below): a fullscreen viewer fills it. Configure keeps its own "
               "size in the 1000x720 session (dlg-preferences-* facts).",
               "Real and injected states: the failed iTunes search is real (the app's application proxy set to 127.0.0.1:9, "
               "which refuses every request; nothing leaves the machine): BridgeClient sets itunesArtworkFailed; Tag Editor "
               "Details shows a real save with one folder not writable; Finishing Tag Changes is given two loaded files' "
               "names (a save here takes milliseconds) and its second stage. Searching and the stand-in results are "
               "injected (no network).",
               "Recordings are the app's own frames: KWin's window animations (a dialog or viewer window opening, fullscreen "
               "entry and exit) are not in them; a new window's first frame is drawn whole."],
        facts_strips=[("albumart-info-toggle", "dark", "Album art viewer, windowed: the info column shown, then hidden"),
                      ("albumart-info-toggle", "light", "The same, light"),
                      ("albumart-open-windowed", "dark", "Album art viewer opening, windowed"),
                      ("albumart-close-windowed", "dark", "Album art viewer closing, windowed"),
                      ("albumart-autohide-fullscreen", "dark", "Album art viewer, fullscreen: its controls hiding"),
                      ("viewer-open-windowed", "dark", "Spectrogram viewer opening, windowed"),
                      ("viewer-close-windowed", "dark", "Spectrogram viewer closing, windowed"),
                      ("viewer-autohide-fullscreen", "dark", "Spectrogram viewer, fullscreen: its controls hiding"),
                      ("viewer-strip-show-fullscreen", "dark", "Spectrogram viewer, fullscreen: its strip showing again"),
                      ("dialog-open-close", "dark", "A dialog (About) opening and closing"),
                      ("preferences-open", "dark", "Configure opening"),
                      ("preferences-page-switch", "dark", "Configure: a page switch"),
                      ("search-open", "dark", "The search palette opening"),
                      ("search-close", "dark", "The search palette closing")],
        floors=[],
        a11y_labels=[],
        contrast={},
    ),
    # The app icon (sessions icons and firstplay-noart: short; --focus-only app-icon is quick)
    "app-icon": dict(
        title="The app icon",
        sessions=["icons", "firstplay-noart", "icon-noart24"],
        # the installed icon per size first; the history (before, squint, variants) below in the facts
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
        # lead, the magnified ones follow (supporting evidence), the history last. the variants (only with
        # SHOT_ICON_VARIANTS=1) after the installed sheets; the vision sheet gives way to them (8 images at most)
        sheets=[
            ("focus-icon-small", "1:1 where Plasma shows the icon small: the title bar, 24 px on a 32 px strip, a 16 px "
             "menu, a 22 px task-switcher list, the task manager at 32 px on a 44 px panel (not playing, playing, active), "
             "36 px on a 52 px panel; apart, a 2x crop of the playing cell; Breeze Dark above Breeze Light",
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
            ("focus-icon-history", "Magnified: the 4th harmonic at 128/256 px with the true 3rd and 4th drawn over it, and "
             "its end at 4x without overlay (dark, light). Then the history: the previous round's icon vs installed at "
             "16-128 px with the squint row", "dark",
             [[("appicon", "h4true", None, "True harmonics, Breeze Dark", dict(lh=16, gap=6))],
              [("appicon", "h4end", None, "The end, Breeze Dark", dict(lh=16, gap=6)),
               ("appicon", "h4end", None, "Breeze Light", dict(theme="light", lh=16, gap=6))],
              [("appicon", "compare20", None, "History, Breeze Dark", dict(lh=16, gap=6))],
              [("details", "glyph-history", None, "Mode glyph, Breeze Dark", dict(optional=True))],
              [("details", "glyph-history", None, "Mode glyph, Breeze Light", dict(theme="light", optional=True))]]),
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
            ("sheet", "focus-icon-history", "dark", "Magnified 4th harmonic (true harmonics; its end, clean); then the history",
             "how far the drawn 4th sits from the true one and how its end meets the edge; what changed since the last round"),
        ],
        history=["focus-icon-history"],  # the only sheet that may name the previous round
        facts=["appicon", "motif"],
        contrast={},
    ),
}
