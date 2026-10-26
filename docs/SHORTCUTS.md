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
| Quit | Ctrl+Q |

These work in the main window only:

| Action | Keys |
| --- | --- |
| Open files | Ctrl+O |
| Global search | Ctrl+F |
| Undo the latest Clear Playlist, removal of queue tracks, or replacement of the queue | Ctrl+Z |
| Select the previous or next track in the queue | Ctrl+Up, Ctrl+Down |
| Move the selected tracks up or down the queue | Ctrl+Shift+Up, Ctrl+Shift+Down |

Alt and an underlined letter open a menu, such as Alt+P for **Playback**. [Global search](SEARCH.md) has keys of its own.

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

Delete acts only while the queue has focus. Playing from the library or global search replaces the queue. After Clear Playlist, Delete, or a replacement, a message under the queue offers **Undo**, as does Undo in the **Edit** menu (Ctrl+Z), until the queue changes again. Undo brings back the tracks and their selection. It does not start playback: a track that plays on is left playing, otherwise the track that was current comes back stopped at its start.

## Controls and dialogs

Tab and Shift+Tab move between the controls, including the transport buttons, the seek bar and the volume. With the seek bar focused, Left and Right seek back or forward 5 seconds.

In **Preferences**, Tab reaches the page tabs, and Space opens the focused one. Left and Right on a tab, Ctrl+Tab and Ctrl+Shift+Tab, or Ctrl+PageDown and Ctrl+PageUp move to the next or previous page.

## Viewers

Double-click the visualization or the album art to open its viewer, and press Escape to close it. In the album art viewer, a double-click zooms in or out, I shows the image's details, Left and Right switch between the current and suggested artwork, and Menu or Shift+F10 opens the context menu.

The visualization's mode switch takes keyboard focus with Tab; Space or Enter picks the focused mode.

## Scrolling and zooming

Lists scroll by what touchpads and high-resolution wheels report, so small movements scroll a little.

The wheel zooms the spectrogram, the waveform and the album art viewer; a smaller movement zooms less. The spectrogram shows its current image at the new zoom at once and replaces it when the new zoom's data arrives. Its view follows playback, so it zooms about the playhead. The album art zooms about the pointer. Horizontal scrolling does not zoom, and a middle click resets the visualization's zoom.
