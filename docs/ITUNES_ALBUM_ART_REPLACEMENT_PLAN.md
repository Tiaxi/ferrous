# iTunes Album Art Replacement

## Summary

- Add a `Replace From iTunes...` right-click action to both album-art entry points:
  - the inline now-playing album-art widget
  - the fullscreen/shared album-art viewer
- Search the US iTunes storefront using the current track's album and artist, rank results locally, and present them in a modal in-app selection dialog.
- Always fetch the uncompressed/original high-resolution artwork asset for album results. Do not use the standard resized image for apply.
- If the selected image is not square, convert it to a square by centered high-quality crop before preview/apply.
- Let the user open any suggestion in the same fullscreen zoomable viewer already used for current album art, with the same pan/zoom/info behavior.
- After apply, update the visible UI immediately and refresh persisted library/external-track cover paths if they changed.

## iTunes API Notes For Another Agent

### Search request to use

- Endpoint: `https://itunes.apple.com/search`
- Query shape for this feature:
  - `term=<artist + album>`
  - `country=us`
  - `media=music`
  - `entity=album`
  - `limit=25`
- The Apple Search API returns album rows with fields such as:
  - `collectionName`
  - `artistName`
  - `collectionId`
  - `artworkUrl100`
  - `collectionViewUrl`

Example:

```text
https://itunes.apple.com/search?term=<urlencoded artist + album>&country=us&media=music&entity=album&limit=25
```

### Why `artworkUrl100` is only a seed URL

- Apple returns a thumbnail-style artwork URL in `artworkUrl100`.
- Bendodson's artwork finder uses that thumbnail URL as the starting point, then rewrites it to larger assets.
- For albums, the site exposes both a standard resized link and an uncompressed/original link. Ferrous should always prefer the uncompressed/original link.

### Standard resized URL derivation

- A resized artwork candidate can be derived by rewriting `100x100` to a larger size such as `600x600`.
- That is useful for quick inline previews, but it is not the asset we should apply.

### High-resolution and uncompressed album artwork derivation

Start with `artworkUrl100`.

1. Build a high-resolution seed by replacing `100x100bb` with `100000x100000-999`.
2. Parse the resulting URL.
3. Rebuild it as `https://is5-ssl.mzstatic.com` plus the parsed path.
4. For album results, derive the uncompressed/original artwork URL by:
   - splitting the path after `/image/thumb/`
   - dropping the final path component
   - rebuilding as `https://a5.mzstatic.com/us/r1000/0/<remaining path>`

This mirrors Bendodson's album logic and is the path Ferrous should treat as the preferred download URL.

In other words, for album rows:

- Preferred apply/download URL: uncompressed/original `https://a5.mzstatic.com/us/r1000/0/...`
- Fallback URL if the uncompressed request fails: the high-resolution `https://is5-ssl.mzstatic.com...100000x100000-999...`
- Do not fall back to the standard 600x600 URL unless both original and high-res fail and the product decision is explicitly changed later.

### Practical implementation notes

- Download the preferred original file to a temp/cache path first, then inspect the actual bytes to determine:
  - MIME type / format
  - byte size
  - decoded resolution
- Do not trust the nominal size embedded in the URL. Read the downloaded file and inspect the decoded image.
- Use the iTunes JSON result only as discovery metadata. The real file metadata shown in the results list should come from the downloaded asset.
- Keep the original downloaded temp file around long enough to support:
  - the selection list preview
  - fullscreen zoomable preview
  - the final apply step
- If the original URL fails but the high-resolution fallback succeeds, still show the actual file details from the fallback download and mark the row as usable.

## UX And Behavior

### Entry points

- Inline album-art widget:
  - add a right-click menu with `Replace From iTunes...`
- Fullscreen/shared album-art viewer:
  - add the same right-click menu on the album-art surface

### Search dialog

- Use a modal in-app dialog, not a separate OS window.
- Show:
  - artwork preview
  - album title
  - artist name
  - actual resolution
  - actual file type
  - actual file size
- Best local match should appear first.
- Include loading, empty, and error states.
- If album or artist metadata is missing, disable the action and show a clear reason instead of issuing a weak search.

### Fullscreen suggestion preview

- Reuse the current album-art viewer implementation rather than creating a second viewer.
- The same presentation logic should apply to:
  - current track artwork
  - iTunes suggestion preview artwork
- Shared behavior:
  - fullscreen/windowed presentation based on existing viewer mode
  - pan and zoom
  - mouse-wheel zoom
  - double-click zoom toggle
  - info overlay
  - same close gestures and controls
- Preview should show the normalized image that would actually be applied, not the raw pre-crop source.

## Image Processing Rules

### Square normalization

- If downloaded artwork is already square, keep it as-is.
- If width and height differ:
  - decode the image at full resolution
  - crop to a centered square using `min(width, height)`
  - do not stretch
  - do not pad
- Use high-quality resampling/cropping logic from the Rust `image` crate.
- The normalized square image is the source for both preview and final apply.

### Format handling

- Preserve the downloaded format when practical:
  - PNG stays PNG
  - JPEG stays JPEG with high-quality encoding
- If the current sidecar file extension differs from the selected image format:
  - write the new image with the same filename stem and the new extension
  - delete the old sidecar only after the new file exists successfully

## Apply Logic

### Source classification

When applying, first determine whether the active artwork source is:

- a real sidecar image file in the album directory, or
- embedded artwork currently surfaced via Ferrous's cached extracted cover file

### Sidecar replacement

- Identify the actual sidecar file currently being used.
- If the selected image keeps the same extension:
  - overwrite atomically via temp-file + rename
- If the extension changes:
  - write the new file beside the old one using the same stem
  - delete the old file only after the new file exists successfully

### Embedded replacement

- If there is no real sidecar file and the active art is embedded:
  - rewrite embedded front-cover artwork
  - apply to:
    - the current file
    - sibling supported audio files in the same folder whose normalized album + artist match the current track
- Use Lofty with each file's primary writable tag type.
- Replace only front-cover art and preserve all other tags and non-front pictures.

## Ferrous Implementation Changes

### QML / C++

- Add iTunes artwork lookup state to `BridgeClient`:
  - results list/model
  - loading flag
  - status/error text
  - invokables to:
    - search for current track artwork suggestions
    - cancel/clear search results
    - open a suggestion preview
    - apply a selected suggestion
- Implement network lookup/download in `BridgeClient` with `QNetworkAccessManager`.
- Cache downloaded suggestion files in a temp/cache location until the dialog is closed or superseded.

### Shared viewer refactor

- Make the existing album-art viewer source-agnostic.
- It should accept:
  - an arbitrary image source
  - file-info payload for the info overlay
  - viewer title/context state
- Current track album art and suggestion preview should both route through that shared surface.

### Rust backend

- Add one bridge command to apply a selected normalized temp image to the current track context.
- Add helpers to:
  - classify the current artwork source as `Sidecar` or `Embedded`
  - write sidecar replacement files safely
  - rewrite embedded artwork across matching album files
  - refresh affected DB rows directly without requiring a full library rescan

### Metadata / DB refresh

- Extract shared embedded-cover caching so metadata loading and library indexing resolve the same cached embedded image path.
- Update `read_track_info()` so that when no sidecar exists it can return the cached embedded-art path, not just folder-image results.
- After apply:
  - refresh current-track metadata immediately
  - invalidate `BridgeClient` cover caches for the affected track and directory
  - refresh affected `tracks` and `external_tracks` rows
  - emit snapshot/tree updates so the UI switches immediately
- If overwritten local files keep the same path, append a cache-busting query/version to the file URL or otherwise force QML image reload.

## Match Ranking

- Rank exact album + exact artist matches first.
- Then exact album matches.
- Then exact artist matches.
- Then partial/fuzzy matches.
- Then preserve original API order for ties.
- Ferrous should not rely on iTunes's returned order alone to define "best match first".

## Test Plan

### Rust tests

- `artworkUrl100` -> original/uncompressed album URL derivation matches the documented rewrite rules.
- Original download is preferred over standard resized artwork.
- If original fails, the high-resolution fallback path is attempted.
- Non-square images are center-cropped to the expected square dimensions.
- Sidecar replacement with extension change preserves stem and deletes old file only after success.
- Embedded replacement touches only sibling files in the same folder with matching normalized album + artist.
- `read_track_info()` returns cached embedded artwork when no sidecar exists.
- Targeted DB refresh updates affected `tracks.cover_path` and `external_tracks.cover_path`.

### UI / Qt tests

- Right-click action is available from both album-art entry points.
- Search dialog renders loading, empty, error, and populated states.
- Suggestion rows display actual resolution, type, and file size from the downloaded asset.
- Suggestion preview opens in the shared fullscreen viewer and supports the same zoom/pan logic as current album art.
- Applying a suggestion updates `currentTrackCoverPath` immediately.

### Validation

- Run `./scripts/run-tests.sh` because this change spans Rust, C++, and QML.

## Assumptions

- Storefront is fixed to US.
- The modal selection UI is in-app.
- Another agent implementing this should treat the Bendodson URL rewrite as the authoritative shortcut for album-art original URLs unless Apple changes the response format.
- For v1, Ferrous does not expose a storefront selector in the UI.

## References

- Apple iTunes Search API:
  - https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/iTuneSearchAPI/Searching.html
  - https://performance-partners.apple.com/search-api
- Bendodson iTunes Artwork Finder:
  - https://bendodson.com/projects/itunes-artwork-finder/
  - https://github.com/bendodson/itunes-artwork-finder
