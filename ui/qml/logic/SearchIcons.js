// SPDX-License-Identifier: GPL-3.0-or-later

.pragma library

// The icon of a search result type ("album", "track" or "artist") in
// `foreground`, as an SVG data URL. Rendering the small SVG with its actual
// foreground colour also works with the software renderer and does not
// depend on the desktop icon theme.
function source(resultType, foreground) {
    const shapes = resultType === "album"
        ? '<circle cx="8" cy="8" r="6"/><circle cx="8" cy="8" r="1.5"/><path d="M4 7a4 4 0 0 1 3-3m2 8a4 4 0 0 0 3-3"/>'
        : resultType === "track"
            ? '<path d="M6 11V4l7-2v7M6 6l7-2"/><ellipse fill="currentColor" stroke="none" cx="4" cy="11.5" rx="2.75" ry="1.75"/><ellipse fill="currentColor" stroke="none" cx="11" cy="9.5" rx="2.75" ry="1.75"/>'
            : '<circle cx="8" cy="4.5" r="2.5"/><path d="M2.5 14v-1.5a5.5 4 0 0 1 11 0V14z"/>'
    return "data:image/svg+xml," + encodeURIComponent(
        '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16" color="' + foreground
        + '"><g fill="none" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round">' + shapes + '</g></svg>')
}

// The icons of the three types in `foreground`, by type.
function sources(foreground) {
    return {
        album: source("album", foreground),
        artist: source("artist", foreground),
        track: source("track", foreground)
    }
}
