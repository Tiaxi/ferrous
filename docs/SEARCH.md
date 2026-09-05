# Library search

Open global search with Ctrl+F. Search matches titles, artists (including repeated values), albums, album artists, genres, years/dates, composers, conductors, performers, labels/publishers, comments, lyrics, and filenames/folder paths across the indexed library. Matching ignores case and accents; all query words must match, and words may match different fields. Substring matches remain available even when other results match a word prefix.

Exact entity names rank first, followed by prefixes, phrases, and supporting metadata. Comments and lyrics rank below name matches; path-only matches rank last. Artist and album results are ranked by their own names, independently of track ranking. Artists and albums follow the library's folder grouping, including separate copies in different library roots.

The All view starts with up to three best matches across result types. Each result appears once. Use Artists, Albums, or Tracks to focus the list; highlighted text shows the matching words. Matches in fields that are not normally displayed include a short explanation, such as `Album artist: orchestra` or `Comment: remaster`. Hover a result to read its full title and supporting details.

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

## Phrases and filters

Put double quotes around words that must occur together. All terms and filters are combined with AND; plain words may match different fields.

| Example | Finds |
| --- | --- |
| `"blue skies"` | That phrase, rather than the words in a different order |
| `albumartist:"london symphony" year:1997` | Tracks credited to that album artist with the year tag 1997 |
| `genre:ambient comment:remaster` | Ambient music with remaster in a comment |
| `composer:sibelius conductor:salonen` | Matching composer and conductor credits |
| `lyrics:"silver moon"` | That phrase in lyrics |
| `date:1997-05` | A recording/original-release date containing that month |
| `track:1 disc:2` | Track 1 on disc 2 |
| `root:classical` | Music in the library root named Classical |

Available fields: `title`, `artist`, `album`, `albumartist`, `genre`, `year`, `date`, `composer`, `conductor`, `performer`, `label`, `comment`, `lyrics`, `path`, `root`, `track`, and `disc`. `album_artist`/`album-artist`, `publisher`, and `filename` are aliases for `albumartist`, `label`, and `path`. Year, track, and disc filters match complete numbers. Unknown prefixes are treated as ordinary text, so filenames containing colons remain searchable. Unclosed quotes are accepted while typing. Boolean operators, numeric ranges, and typo correction are not supported.

Album results can also match genre, dates, credits, and other metadata from main-album tracks and recognized disc folders. Bonus-folder matches appear as tracks. Playing an album still plays the main album, not only its matching tracks. Artist results represent library folders; searching a guest artist or performer may therefore return tracks/albums without an artist-folder result.

## Existing libraries

On upgrade, missing search-tag data is backfilled by the library worker after the cached library appears. Offline roots retain their cached tracks and are filled on a later scan or startup when available. Metadata edits and rescans refresh these fields. The scanner collects supported text values from all tag blocks; fields unsupported by a file format or its tag reader cannot be indexed. Raw AC3/DTS APE tags additionally expose the album-artist, comment, and disc fields supported by the raw-audio reader.
