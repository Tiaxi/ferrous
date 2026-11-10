# Keyboard and mouse

## Shortcuts

These work in the main window and in the fullscreen viewers. Text fields, such as global search, keep the keys they use for typing and selecting text.

| Action | Keys |
| --- | --- |
| Play or pause | Space |
| Stop | Ctrl+Shift+Space |
| Previous or next track | Ctrl+Left, Ctrl+Right |
| Seek back or forward 5 seconds | Shift+Left, Shift+Right |
| Volume down or up 5% | Alt+Down, Alt+Up |
| Mute | Ctrl+Shift+M |
| Open or close the visualization viewer | F11 |
| Open or close the album art viewer | Shift+F11 |
| Show the spectrogram or the waveform | Ctrl+1, Ctrl+2 |
| Reset the visualization's zoom | Ctrl+0 |
| Quit | Ctrl+Q |

These work in the main window only:

| Action | Keys |
| --- | --- |
| Open files | Ctrl+O |
| Find (open global search) | Ctrl+F |
| Undo the latest Clear Queue, removal of queue tracks, move of queue tracks, or replacement of the queue | Ctrl+Z |
| Select the previous or next track in the queue | Ctrl+Up, Ctrl+Down |
| Move the selected tracks up or down the queue | Ctrl+Shift+Up, Ctrl+Shift+Down |
| Configure Ferrous | Ctrl+Shift+, (Plasma's standard key) |

Alt and an underlined letter open a menu, such as Alt+P for **Playback**. The menus show each item's shortcut; the queue's and the library's context menus also show the keys that do the same in the list, such as Enter and Delete. [Global search](SEARCH.md) has keys of its own.

## Library and queue

| Action | Keys |
| --- | --- |
| Move the selection | Up, Down, PageUp, PageDown, Home, End |
| Extend the selection | Shift with the keys above |
| Expand or collapse a library row | Right, Left |
| Play the selection | Enter |
| Remove the selected tracks (queue only) | Delete |
| Open the context menu | Menu, Shift+F10 |

In the library, typing the start of an artist's name selects that artist.

Delete acts only while the queue has focus. Playing from the library or global search replaces the queue. After **Edit › Clear Queue**, Delete, or a replacement, Undo in the **Edit** menu (Ctrl+Z) stays available until the queue changes again. Undo brings back the tracks and their selection. It does not start playback: a track that plays on is left playing, otherwise the track that was current comes back stopped at its start.

The handles between the queue's column headings resize the Artist, Album and Length columns; the title takes the rest. Tab reaches them: Left widens the column after the handle, Right narrows it, and Home, or a double-click, restores its default width. Ferrous keeps the widths you set.

## Controls and dialogs

Tab and Shift+Tab move between the controls, including the transport buttons, the seek bar and the volume. With the seek bar focused, Left and Right seek back or forward 5 seconds.

In **Configure Ferrous** (**Edit > Preferences**), the page list has the keyboard first: Up and Down move between the pages, and so do Ctrl+Tab and Ctrl+Shift+Tab, or Ctrl+PageDown and Ctrl+PageUp, from anywhere in the dialog. Tab goes on to the page's controls and the buttons. Changes take effect with OK or Apply; Escape cancels them.

In **Recent Issues**, the arrow and Page keys scroll the list, and Tab reaches each issue's action, Open Diagnostics… and Clear All.

## Viewers

Double-click the visualization or the album art to open its viewer, and press Escape to close it. In the album art viewer, a double-click zooms in or out, I shows the image's details, Left and Right switch between the current and suggested artwork, and Menu or Shift+F10 opens the context menu.

A viewer over the whole screen fills it, with its buttons in a title strip over its top edge; one in the main window has them in its top right corner. The buttons hide while the pointer rests, the title strip with them, except those of the album art viewer in the main window; a key press brings them back, and Tab moves through them, then to the visualization's mode switch. They stay while one of them has keyboard focus. Space presses the focused button.

With **Show the title strip when the pointer moves** turned off (**Edit > Preferences > Appearance**, for **Fill the screen**), a viewer over the whole screen never shows its title strip, and the visualization's mode switch and the first channel's Mute and Solo move up to the top edge. The keys and the pointer do what the strip's buttons did. Escape closes either viewer; so do F11 and a double-click in the visualization viewer, and Shift+F11 and a click beside the art in the album art viewer. A middle click resets the visualization's zoom, as Ctrl+0 does, and a double-click fits zoomed album art again. I shows the album art's details, and Left and Right switch between the current and suggested artwork. New issues show in the status bar's indicator once the viewer closes.

While the album art viewer previews artwork from **Replace from iTunes**, the image's details always show, and I does not hide them. At their foot are what the viewer shows, such as "iTunes Candidate", then Back to Results and, for a candidate, Apply. Escape also goes back to the results.

The visualization's mode switch, in its top right corner, shows while the pointer is over the visualization or keyboard focus is on it, as the channels' Mute and Solo buttons do. It takes keyboard focus with Tab while hidden, which shows it; Space or Enter picks the focused mode. Ctrl+1 and Ctrl+2, and the **View** menu, pick the mode without it.

## Scrolling and zooming

Lists scroll by what touchpads and high-resolution wheels report, so small movements scroll a little.

The wheel zooms the spectrogram, the waveform and the album art viewer; a smaller movement zooms less. The spectrogram shows its current image at the new zoom at once, with the columns it has around it, and replaces it as the new zoom's data arrives. Its view follows playback, so it zooms about the playhead. The waveform zooms in at once, and zooms out once it has the detail for the wider view. The album art zooms about the pointer. Horizontal scrolling does not zoom. A middle click, Ctrl+0 or **View > Reset Zoom** resets the zoom of the visualization that shows, and the spectrogram keeps what it has drawn; the menu item is available only while it is zoomed.

The waveform's default zoom shows the whole track. The spectrogram's shows about 45 pixels a second, so a track shorter than the view, such as a one-second clip, fills only part of it. With **Stretch tracks shorter than the view to its full width** turned on (**Edit > Preferences > Appearance**, on by default), its default zoom for such a track is the one at which the track fills the view, down to a column every 16 samples at 44.1 and 48 kHz. That is where a track starts, and where Reset Zoom goes. A view at the default follows it as the view's width changes, and when a file's length is known only after it starts. The wheel zooms in up to 16 times, or up to the stretched default where that is deeper. In rolling mode, a gapless transition keeps a zoom you chose. A zoom shown in a viewer is relative to the default. While a viewer is zoomed, it shows the zoom and a Reset button, which fits the album art again or resets the visualization's zoom.
