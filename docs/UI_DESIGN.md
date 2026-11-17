# UI design rules

The rules Ferrous's interface follows, each with the reason it exists. Follow them in new UI work. To change one, first address its reason. The [tried and rejected](#tried-and-rejected) table lists ideas that failed, and why.

Colours and sizes come from `ui/qml/components/UiPalette.qml`. The comments next to each value in the code give more detail. For the visualization's internals, see [visualization architecture](VISUALIZATION_ARCHITECTURE.md).

## Platform fit

- **KDE-native on Breeze and Kirigami.** Use QQC2 and Kirigami controls, Kirigami units, platform menus, KDE focus and Enter behaviour, and Breeze icons. Ferrous is a Plasma app, and it should look like Dolphin and Kate do.
- **Configure and About are real top-level windows** (`DialogWindow`), as KConfigDialog and KAboutApplicationDialog are. KWin draws their title bars, and they move and resize beyond the main window. The other dialogs use the same window. Two surfaces stay inside the main window: the search palette and the Recent Issues popup, which opens from the status bar.
- **Public Qt API only.** The private API pins the package to one exact Qt version and blocks Plasma updates.
- **A quiet main window.** Routine events go to the status bar's indicators. Problems go to Recent Issues. A dialog appears only when the user must act. Banners and notices compete with the music for attention.
- **Dark is the main theme, and light is first-class.** Check every change in Breeze Dark and Breeze Light. Dark gets a little more weight when the two pull apart, but every token is derived per theme and must hold in both.
- **No generic AI-made look.** Use none of the following unless the reason comes from Ferrous itself:
  - zinc or slate greys;
  - UPPERCASE letter-spaced labels;
  - pills or cards around everything;
  - gradients, glows or glass;
  - emoji, or icons in circles;
  - a web-dashboard feel;
  - warm orange on near-black as a "premium" palette.

  The app's identity is its visualization and its amber "now", not decoration.
- **Content has no cards and no internal radius.** Regions meet at 1 px seams. The plot fills its pane edge to edge, with no gutters, rulers or rails.

## Tokens

Every colour comes from the window's palette and the Kirigami theme. The only fixed colours are the amber of `now` and the plot colours. The values in brackets are what Breeze Dark and Breeze Light produce. Use them to check your work, never as hard-coded values. Several tokens raise or lower their lightness until they reach a contrast ratio (`readableOn`), so another colour scheme still meets 4.5:1 for text and 3:1 for graphics.

### Surfaces and lines

| Token | Use it for | Why |
|---|---|---|
| `chrome` | the window colour: the status bar, dialogs and their button rows [#202326 / #eff0f1] | window chrome reads apart from content |
| `view` | the base colour: all content, lists, now playing, the search palette | content sits on one surface |
| `seam` | the 1 px line where two regions meet | regions are separated by seams, not gaps or cards |
| `hairline` | the 1 px line under a header row | quieter than a seam: it divides inside a region |
| `chromeLine` | the 1 px line between chrome and content | chrome and content meet at one edge |
| `dim` | the scrim behind the search palette: black at 65 % dark, 25 % light | less left the dark window nearly as bright as the palette |

The transport uses Breeze's Header colour set, as Breeze's toolbars and the menu bar above it do, so the two read as one band.

### Text

| Token | Use it for | Why |
|---|---|---|
| `text` | primary text | the theme's own text colour |
| `textSecondary` | explanations, help text, a dialog's text after its lead, "Artist — Album" [#aeb7bd / #4f5a63] | one step below the text, still at least 7:1 |
| `textTertiary` | counts, years, track numbers | a second step down, at least 4.6:1 on the view and on its hover tint |
| `textTertiaryOn(selected, hovered)` | tertiary text on a list row | on a hovered row it moves to `textTertiaryHovered`, and on a selected row it moves to `textTertiarySelected`, so small figures stay at least `rowFigureContrast` (5.4:1) on the row's tint. Antialiasing loses up to 14 % of contrast, and 5.4:1 still renders at 4.5:1 or more |
| `textColumn` | the queue's Artist and Album cells | quieter than the title, and readable on every selection state |
| `placeholderText` | a field's placeholder, such as "Multiple values" | Breeze Light's own placeholder colour is 4.2:1 on white |
| `link` / `linkOn(background)` | link-coloured text and actions | 4.5:1 on both the window and the view colours, or on the tint under the link |
| `negative` | error words, the warning glyph, the clip lamp, a failed section's heading | the theme's negative colour, adjusted for text contrast |

### Row and button states

| Token | Use it for | Why |
|---|---|---|
| `hover` | hover on buttons and dialog lists: the least text-over-view tint that reaches 1.3:1 | reads as hover in any theme without looking like a selection |
| `rowHover` / `hoveredRow` | hover on the library and queue rows: 1.44:1 dark, 1.31:1 light | 1.3:1 was too faint across a wide list. In light, a stronger tint outweighed the selection |
| `pressed` | a pressed button: 1.65:1 on the view | a press must read as a clear step past the hover |
| `hoverOn(bg)` / `pressedOn(bg)` | hover and pressed tints on another opaque background, such as the Header chrome or the popup's window colour | they keep the same steps on any surface |
| `selection` | a selected row in a focused list: the view mixed with the accent [#1f3e4e / #b5e0f7] | above the hover in both weight and hue |
| `selectionInactive` | a selected row in a list without focus: the accent mix desaturated to 60 % | reads as a selection at rest, still above the hover |
| `selectionHovered` / `selectionInactiveHovered` | a selected queue row under the pointer | a selected row responds to the pointer like any other row |
| `focusRing` | the 1 px focus ring on a row, and the 2 px ring on a button | the accent, lighter on dark and darker on light, so it stands out from the selection |

### Data and playback

| Token | Use it for | Why |
|---|---|---|
| `now` | **only** what is playing: the playing row's glyph and title, the playing tree row, the collapsed row that holds it, and the seek playhead [#f2b035 / #814900] | amber means "now". Any other use dilutes it |
| `nowPausedOpacity` | the amber name of a collapsed tree row while paused: 70 %, or more where that falls below 4.5:1 | paused reads as quieter without losing readability |
| `accent` | Breeze's highlight colour, used for selection mixes, checked fills on a plot and the focus ring | not a decorative colour |
| `wave` | the seek bar's waveform | the theme's pale accent, at least 3:1 against chrome for the unplayed part |
| `paneWave` | the waveform in the visualization pane | in dark, `wave` was the brightest mass in the window |

### Plot colours

Anything drawn on a visualization takes its colours from the plot under it, never from the window: use `plotColors(mode)`, which returns `spectrogramPlot` or `waveformPlot`. The spectrogram's colour map is dark in both themes, so its set is fixed. The waveform is drawn on the view colour, so its set follows the theme. `mediaPlot` is the spectrogram set on black, for the album art viewer. Each set holds:
- `plotBg`, `plotPlate`, `plotPlateEdge`, `plotStrip`;
- `plotText`, `plotTextSecondary`, `plotOutline`;
- `plotLaneSeam`, `plotTick`, `plotLink`;
- the `meter*` colours;
- `checkedFill`, `checkedGlyph`, `segmentEdge` and `plotFocusRing`.

`plotFloor` (#14062e, the colour map's quiet indigo) is the floor where the plot has nothing to draw. The no-cover stand-in uses it too.

The `ui*` colours at the end of the file come from before these tokens. Don't use them in new code.

### Type and size

| Token | Use it for |
|---|---|
| `baseFont`, `boldFont`, `basePointSize` | the window's font, which list rows draw with |
| `smallPointSize` (0.9×), `compactPointSize` (0.825×) | headers, counts and plot labels; the labels of short lanes |
| `headingPointSize(level)` | plain text sized as a Kirigami heading |
| `fullscreenTextScale` (1.2) | the text of a viewer over the whole screen: its strip and the art details, on one scale |
| `tabularFigures` | times and counts that line up |
| `rowHeight` | a list row: a line of the font plus a small spacing above and below (26 px at 10 pt) |
| `gridUnit` | Kirigami's grid unit, on the window's font rather than the platform's |
| `labelIconSize` | a glyph beside a line of text (see below) |
| `scaledSize(px)` | a measure tuned at 10 pt (see below) |
| `toolIconSize`, `toolButtonSize` | a small tool button's glyph (16 px) and the button (24 px), scaled |
| `themeIsDark` | choosing between per-theme amounts |

## The one font rule

Every surface must hold up at 24 pt as well as at 10 pt. One rule covers dialogs, viewers and the main window:
- **A glyph beside text uses `labelIconSize`** (`Kirigami.Units.iconSizes.sizeForLabels`): 16 px at 10 pt, 32 px at 24 pt. This covers button glyphs, page-list icons, row glyphs and severity marks. A 16 px glyph beside 32 px text becomes the smallest thing in its row.
- **A pixel measure tuned at 10 pt goes through `scaledSize()`.** This includes widths that bound text, thumbnails, gutters, insets and caps. `scaledSize()` leaves 10 pt unchanged and grows with a larger font. A fixed width at 24 pt cuts text beside empty room.
- **A text measure is counted in characters.** A compact dialog is about 46 average characters wide, help text at most about 63, and the art details about 40. A fixed width holds fewer words at a larger font.
- **Check at 24 pt.** Nothing may elide or wrap a lead sentence while there is room beside it, and no window may run wider than the screen.

## Typography and wording

- **Each message dialog opens with a DemiBold lead sentence.** It says what happened or what is asked. The explanation, the path and the system's error follow in `textSecondary`. Weight, not size, ranks the lead, and one system everywhere keeps dialogs from reading as one grey block.
- **The lead says what happened in words.** The system's error text, such as "Permission denied (os error 13)", goes on its own line below as a detail. It never repeats the window title.
- **One term for one thing.** Use "Queue", never "Playlist". Use "Delete" for a delete. Use "full-size image", not "high-resolution artwork" in one place and "full-size" in another.
- **Pass every visible string through `I18n.i18nc` in QML or `i18nc` in C++,** with a context marker, and `i18ncp` for counts. Follow [translations](DEVELOPMENT.md#translations). Use KDE HIG wording:
  - Title Case for buttons, menus and window titles;
  - "…" (one character) only on items that ask for more input;
  - typographic apostrophes (’).
- **Paths break only between folder names, and error codes stay whole.** Show a message that can hold a path or an error with `MessageLabel`, or with `BreakableText.breakableMessage()`. A folder name stays on one line unless it is wider than the line, and "(os error 13)" never splits. The accessible name, the tooltip and copied text keep the original string. "…/2005 - Small" / "Hours" read as two folders.
- **A path the user reads but doesn't edit is a `PathLabel`:** one line, elided in the middle, with the full path in its tooltip. A read-only text field looks editable.
- **In list rows, weight marks state.** The playing title, a search match, a library folder's name and an unseen issue are DemiBold; the rest is regular. Bold used for anything else would compete with these.

## Dialogs

- **Buttons follow KDE order** (`DialogButtons`, `DialogButtonBox.KdeLayout`): the affirmative button, Apply, a destructive one, then Cancel. Configure's Defaults sits on the left. The buttons sit one grid unit in from the window's sides and bottom, at the same inset as the content. The style's own 9 px footer inset pushed them into the corner.
- **The safe choice is the default.** Enter presses the button marked `primary`. When the action destroys data (delete, remove, replace), Cancel is the default and has focus when the dialog opens, so neither Enter nor Space destroys anything until the user moves to the action. This follows the KDE HIG: a stray Enter once removed a library folder.
- **Enter works as in KDE:** in a text field, it accepts the text; on a focused button, it presses that button; elsewhere, it presses the default button. Escape and the title bar's close button cancel.
- **Dialog buttons carry a Breeze glyph** at `labelIconSize`, in footers and in action rows alike: `dialog-cancel`, `dialog-ok-apply`, `edit-delete`, `edit-rename` and so on. A glyphless caption in a row of glyphs reads as a label.
- **The colour of a destructive glyph comes from Breeze's own icon.** `edit-delete` and `edit-delete-remove` are red in Breeze, and Ferrous never tints them. Red belongs to removing and deleting only: Replace Album Art uses `document-swap`, because `document-replace` has a red × and read as Delete.
- **Dialogs size to their content, with caps.** A compact dialog (`CompactDialogWindow`) is as tall as its content and follows it as it wraps. Its width is the measure, or the width its form and buttons need. Lists such as Rename Files open as wide as their widest line, up to a cap. Configure opens as tall as its tallest page, so no page scrolls. Everything stays within the screen. A fixed size left either a scrollbar or an empty band.
- **Configure has one grouping rule.** Only a page with three or more groups (Visualization) gives them DemiBold subheadings (`FormGroupGap.heading`). Two-group pages separate them with the form label and a `FormGroupGap`, 16 px. Help text (`FormHelpText`) sits under its control in `textSecondary`. Three grouping styles in one dialog read as assembled work.
- **Lists that compare entries share columns.** In Configure, every library folder's path starts at one x, and Rename Files shows its rows as old | → | new. The things being compared need one left edge.
- **Empty and failed states keep their place.** "Nothing found" is quiet, secondary text. A failure gets a warning glyph in the spinner's slot and the system's words under it, so a network failure doesn't read as a routine empty result.
- **About keeps KDE's About layout,** with its tabs standing on the frame of their pages.

## States

- **Rest:** no fill. Flat tool buttons show their glyph only.
- **Hover:** the hover tint for the surface (`hover`, `rowHover`, or `hoverOn(bg)`) fades in and out over `shortDuration`. On a viewer's strip and corner plate, hover is a wash of the plot's text colour: 14 % on dark (about 1.4:1), 12 % on light. At 6.5 %, hover looked the same as rest.
- **Pressed:** `pressed` or `pressedOn(bg)`, a clear step past the hover. It shows on the first frame of the press, never faded in. A press that matches the hover reads as nothing.
- **Focus:** a focus ring appears only while the keyboard is in use:
  - a focused list row gets a 1 px `focusRing`, drawn as one rectangle with the selection tint when the row is also selected;
  - Ferrous's own tool buttons and link actions get a 2 px ring;
  - a button on a plot gets a 2 px ring 1 px outside it, with a 1 px gap in the plate's colour.

  Focus and selection change at once.
- **The focus ring lane:** a list whose overlay scrollbar lies over the rows' right end sets `focusRingLane`. The ring stops short of the lane, and the selection tint continues under it. When the thumb ran over the ring, the focused row's end looked broken.
- **Selected:** `selection` in a focused list, and `selectionInactive` without focus. A selected queue row under the pointer moves toward `selectionHovered`. Tertiary text on a selected row moves up (`textTertiaryOn`).
- **Checked:** on the window's colours, a checked tool button gets the selection tint alone, with no ring. On a plot:
  - a state that is on, such as Mute or Solo, gets a solid `checkedFill`;
  - the chosen segment of a choice, such as the mode switch, gets a neutral plate with a 1 px `segmentEdge`.

  A solid accent there read as keyboard focus and outshone the data.
- **The now-playing marks:**
  - The playing row has an amber play glyph (pause glyph while paused) and a DemiBold amber title.
  - A collapsed tree row that holds the playing track has an amber name and no glyph, because a glyph there reads as a play button. While paused, the name drops to `nowPausedOpacity`.
  - A stopped current row gets a neutral stop mark.
  - An unreadable file gets the status bar's warning sign, in `negative`.
  - The status bar's Playing glyph stays neutral.
  - Use Breeze icons or Ferrous's own SVGs, never text glyphs such as ▶ or ⏸.

## Motion

Durations are Kirigami's (`shortDuration` 100 ms, `longDuration` 200 ms, `veryLongDuration` 400 ms by default). They scale with Plasma's animation speed and drop to 0 when animations are off, so check `duration > 0` and make every change instant at 0.

| What | Motion |
|---|---|
| hover on rows and buttons, plates revealing, the crosshair hiding over a plate | fade, `shortDuration`, OutCubic |
| selection, focus ring, a press | none |
| Play ↔ Pause glyph | cross-fade from one progress value, `shortDuration`, so the button never goes blank |
| amber on a track change | queue: the old row fades out as the new one fades in, so two rows are never both fully amber. Tree: the new row is amber at once and the old one fades. Both over `shortDuration` |
| now playing, next track on the same album | the lines that change fade over `shortDuration`. Words and numbers that stay the same stay still |
| now playing, another album or cover | the cover and lines fade together over `longDuration`. Old lines leave in the first 40 % while new ones come in over the whole fade |
| now playing, first play | the panel grows from the compact row over `veryLongDuration` × 9/8 (450 ms, InOutQuad) when wide, or over `longDuration` when narrow. The library moves with it, and the lines fade in from 60 % of the way |
| spectrogram ↔ waveform | the chosen plot fades in over the other, `shortDuration`. Labels, switch and meter stay put |
| search palette and scrim | fade together, `shortDuration`, OutCubic |
| tree expand and collapse | rows below slide over the children, `longDuration`, OutCubic |
| queue reorder by drag | displaced rows slide, `shortDuration`, OutCubic. A keyboard reorder is instant |
| viewer controls auto-hiding | after 5 s at rest, one shared level fades the strip, plates, mode switch, lane buttons and crosshair together, `shortDuration`, OutCubic. The controls stay while they have keyboard focus |
| album art details | the column slides in and the art glides with it over 120 ms, OutCubic |
| playhead, seek, spectrogram scrolling | never tweened |
| meter or scale toggled | one step |

Rules:
- **No blink.** No layer may drop below half its ink while it changes. Cross-fades overlap rather than run in sequence, and no surface fades out before the one replacing it has arrived. The album art viewer keeps its image, and the visualization viewer its last frame, through the close fade. A popup draws one floor, never two layers at half opacity.
- **No jump of the thing being looked at.** Geometry eases rather than switching in one frame: the art glides as the info column appears, and the library slides as now playing grows. Layouts that change per track keep one height. A track without a cover gets a stand-in in the cover's place, not a different layout.
- **Details are already at rest when a viewer opens.** Only toggling the info column in an open viewer animates it. A viewer that opens with the column shown has it in place from the first frame. When the column slid in on open, Copy Path was briefly out of reach.
- **Going back to "Nothing playing" plays the first play backwards,** as when playback stops because the queue was cleared or ran out. The track that showed stays while the panel shrinks into the compact row, and its lines fade out as they faded in.
- **Interruptions continue from what shows.** A new change mid-fade starts from each layer's current opacity, and a reversal reverses from where it is.
- **Test motion on a stepped clock, not by counting frames.** `ui/tests/tst_ui_motion.cpp` installs a `SteppedAnimationDriver` that advances animation time 16 ms at a time. Tests check opacities and positions along the whole fade (no blink, no overlap, no jump), check that nothing is left running, and cover the instant path with animations off. Real-time frame counting fails on a slow machine.

## Visualization and viewers

The settled rules for presentation only. For internals, see [visualization architecture](VISUALIZATION_ARCHITECTURE.md).
- **Media surfaces are always dark.** The album art viewer draws the art on black, with its details in `mediaPlot`'s strip colour, in both themes. A white surround outshone the art.
- **The plot's playhead is never amber.** Amber is a data colour in the colour map. The seek bar's playhead is the only amber playhead.
- **One control surface per viewer.**
  - In the window, the visualization viewer's mode switch, the new-issues button and Close share one corner plate, divided by 1 px seams. Separate plates read as a row of stickers.
  - Over the whole screen, while the title strip is on, the strip holds them all, so no second cluster sits under it. With the strip turned off, they stay on the plot.
  - The strip is opaque, is drawn over the view, and never resizes the view.
- **The album art info column** is about 40 characters wide: 280 px with its seam at 10 pt, up to 320 px in a wide window, and 336 px over the whole screen. Its text is on the viewer's type scale. The art fits and centres in the room to the left of the seam, with a 16 px gutter (24 px during a preview). It glides as the column opens and never touches the seam.
- **Text on the viewers uses one type scale.** Over the whole screen, the strip and the details are both 1.2× the window's text, so the file name never outranks the track title.

## Tried and rejected

Ideas that were tried and rejected, and why. Bring one back only if your change addresses its reason.

| Area | Rejected | Why |
|---|---|---|
| Main window | banners or notices for routine events | the main window stays quiet: routine events go to the status bar; a dialog appears only when the user must act |
| Settings menu | a "Configure Keyboard Shortcuts…" item | there is no shortcut editor yet, and a menu item that does nothing is worse |
| Transport | the window's own colour as its background | the bar read as part of the content; it uses Breeze's Header colours |
| Seek bar | an accent or amber waveform fill | amber is reserved for "now" and the playhead |
| Seek time readout | a box on the waveform; a readout under the bar | it hid the part being scrubbed to; under the bar, it hung over the queue's header |
| Row hover (light) | a hover tint stronger than the selection | the hovered row looked more selected than the selected one |
| Inactive selection | a tint weaker than the hover | an unfocused selection read as less than a hover |
| Queue focus ring | a ring running under the scrollbar | the thumb broke the ring's end |
| Unreadable row | Breeze's `dialog-warning` drawn as a grey mask | it read as an eject arrow beside the amber ▶ |
| Plot frame | gutters, rulers, rails, a border | the plot fills its pane |
| Plot plates | the window's view colour as plates; bare outlined text | white stickers in light; text vanished on the waveform |
| Mode switch | the chosen segment in solid accent blue | read as keyboard focus, and outshone the data |
| Level meter | a strip under the plot; a neutral grey column; a saturated colour-map ramp | crowded 5.1; read as the queue's scrollbar; louder than the amber "now" |
| Never-played plot | pure black; a centred empty-state message | a black slab in the window; a second message beside the queue's first-run one |
| Last.fm listening | an arc, a pie or a progress ring around the glyph | read as a busy spinner; the states looked alike at 1× |
| Idle now playing | a 360 px empty square; a stock disc glyph | read as a dead slab; the glyph was unreadable at 16 px |
| Now playing | per-track heights; the cover scaling up from a square on first play | the library jumped up to 26 px on a track change; the scaling read as a zoom |
| No-cover tile | the album title or a glyph in the slot; a different layout | repeated the lines below it, read as a broken image, and moved the library |
| Track change | old and new text cross-fading over the same span | the two titles read as superimposed |
| First play | the idle row leaving before the cover arrived | the band was empty for up to 140 ms, a blink |
| Stop → idle | the cover and lines cut in one frame | a hard cut, not the reverse of first play |
| Album art viewer | the window's light view colour around the art | a white surround competed with the art |
| Album art viewer close | the image cut at once while the black floor faded | a blink on close |
| Album art viewer open | the details sliding in as the viewer opened | the art moved while opening, and Copy Path was briefly out of reach |
| Art info column | the art re-centring in one frame as the column appeared; the art meeting the seam | a 172 px jump of the thing being looked at; no gutter |
| Fullscreen art details | the strip at a different type scale from the details | two type scales on one screen; the file name outranked the title |
| Viewer corner | one plate per control group; a mode plate under the fullscreen strip's Close | a row of stickers; two clusters where the pointer goes |
| Viewer buttons | a 6.5 % hover fill | hover looked the same as rest |
| Preferences | one long page; a size shorter than its tallest page | hard to scan; a page scrolled by 8 px |
| Configure grouping | headings on some pages, none on others, a heading over one check box | three grouping styles in one dialog |
| Library folders (Configure) | a list as tall as its rows; each path starting where its name ends | one folder read as a text field; three left edges for the thing being compared |
| Destructive confirmations | the destructive action as the default button | Enter removed a library folder without moving to the action |
| Dialog leads | the system error as the heading; the lead regular in some dialogs | it didn't say in words what failed; two typographic systems |
| Dialog buttons | the style's 9 px footer inset; a glyphless caption beside glyph buttons | buttons pushed into the corner; it read as a label |
| Large fonts | pixel sizes and 16 px glyphs set for 10 pt | at 24 pt text was cut beside empty room, and glyphs were the smallest thing in the row |
| Paths in messages | breaking anywhere, including inside a folder name or "(os error 13)" | "…/2005 - Small" / "Hours" read as two folders; "13)" stood alone |
| Rename Files | a fixed opening width; each row as one "old → new" string | buttons far from what they confirm; new names had no column to read down |
| Replace Album Art | `document-replace` as its glyph; one 31-word sentence | the red × read as Delete; it had to be read twice |
| Replace from iTunes, empty | a framed dark well with a search glyph | read as a dead black box in the dialog |
| iTunes search failed | one grey line, the same as "no results" | a network failure read as a routine empty result |
| Edit Tags columns | File Name and Folder squeezed while tags kept room; tags giving way first | the file columns tell rows apart; Artist elided in every row |
| Edit Tags status cell | the raw system error in the cell | cut to "failed to …", it explained nothing |
| Tag Editor Details | the save results as a plain text dump | the screen that explains a failed save looked unfinished |
| Search footer | 4 px between a key and its action, as between pairs | "Ctrl+Enter Add to Queue" read as one phrase |
| Recent Issues links | the pressed fill the same as hover | a press read as nothing |
| Diagnostics | actions over two rows; "Clear…" for a delete | ragged edges, and two verbs for one action |
| About | Breeze's tab strip ending mid-window with no page under it | the selected tab opened onto bare window colour |
