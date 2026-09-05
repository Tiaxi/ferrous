# Library search

Open global search with Ctrl+F. Search matches titles, artists, albums, genres, and filenames/folder paths across the indexed library. Matching ignores case and accents; all query words must match, and words may match different fields. Substring matches remain available even when other results match a word prefix.

Exact entity names rank first, followed by prefixes, phrases, and supporting metadata. Path-only matches rank last. Artist and album results are ranked by their own names, independently of track ranking. Artists and albums follow the library's folder grouping, including separate copies in different library roots.

The All view starts with up to three best matches across result types. Each result appears once. Use Artists, Albums, or Tracks to focus the list; highlighted text shows the matching words. Hover a result to read its full title and supporting details.

Counts show displayed results out of total matching artists, albums, and tracks. Show more increases the result budget. Results are bounded at 2,000 per type to keep interaction responsive; if more matches remain, refine the query. Initial budgets default to 5 artists, 10 albums, and 20 tracks.

| Action | Control |
| --- | --- |
| Move through results | Up/Down; Page Up/Page Down |
| Edit the query at its beginning/end | Home/End while typing |
| Move keyboard focus | Tab/Shift+Tab |
| Play the selection and replace the queue | Enter or double-click |
| Show the selection in the library | Ctrl+Enter or the context menu |
| Append to the queue | Queue in the context menu |
| Return to the query | Ctrl+F |
| Close search | Esc |

Search runs in a background worker. While a new query is pending, playback actions on old results are disabled.
