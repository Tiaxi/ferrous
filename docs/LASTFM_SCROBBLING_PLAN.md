# Last.fm Desktop Scrobbling

## Summary

- Do not store a Last.fm password. Use Last.fm desktop auth (`auth.getToken` -> browser approval -> `auth.getSession`) and store only the returned session key in the OS keyring. Persist only `lastfm_scrobbling_enabled` and the connected username in Ferrous's plain-text settings file.
- Keep scrobble timing fixed to Last.fm's documented rule: only tracks longer than 30 seconds are eligible, and a scrobble is sent when playback stops or the track changes after actual listened time reaches `min(240s, 50% of track length)`. Show that rule in Preferences as help text; do not make it user-editable.
- API payloads for v1:
  - `track.updateNowPlaying`: send `artist`, `track`, `api_key`, `api_sig`, `sk`; also send `album`, `trackNumber`, and `duration` when available.
  - `track.scrobble`: send `artist`, `track`, `timestamp`, `api_key`, `api_sig`, `sk`; also send `album`, `trackNumber`, and `duration` when available.
  - Omit `mbid`, `albumArtist`, `context`, `streamId`, and `chosenByUser` in v1 unless Ferrous starts exposing that metadata explicitly.

## Implementation Changes

- Add a dedicated Rust `lastfm` service module and wire it into the bridge loop. The bridge remains the coordinator, but all HTTP/keyring work runs on a separate worker thread so playback/UI polling never blocks.
- Source app credentials from build-time env vars such as `FERROUS_LASTFM_API_KEY` and `FERROUS_LASTFM_SHARED_SECRET`. If either is missing, the UI shows "Last.fm not configured in this build" and disables connect actions.
- Persist non-secret preferences in `settings.txt`:
  - `lastfm_scrobbling_enabled=0|1`
  - `lastfm_username=<username>`
  Store the session key only in the OS keyring under a fixed service name like `ferrous.lastfm`, keyed by username. No plaintext fallback if keyring access fails.
- Add a Last.fm runtime state snapshot separate from plain settings, and extend the binary bridge to expose:
  - enabled flag
  - build-configured flag
  - connected username
  - auth phase (`Disconnected`, `AwaitingBrowserApproval`, `Connected`, `ReauthRequired`, `Error`)
  - pending scrobble count
  - status/error text
  - pending auth URL while approval is in progress
- Add bridge commands for:
  - set Last.fm enabled
  - begin desktop auth
  - complete desktop auth
  - disconnect Last.fm
- Implement auth flow as:
  - `Begin`: worker calls `auth.getToken`, bridge exposes the auth URL, UI opens it in the browser.
  - `Complete`: after the user approves in the browser and returns, worker exchanges the stored token with `auth.getSession`, writes the session key to keyring, stores the returned username in settings, and marks the account connected.
  - `Disconnect`: delete keyring entry, clear username/settings state, clear pending auth state, and drop cached scrobbles.
- Add a durable scrobble cache file in the config dir, e.g. `lastfm_queue.json`. Cache only scrobble submissions, not now-playing calls.
- Track scrobble eligibility from actual listened time, not playback position, so seeks do not falsely trigger scrobbles. Maintain per-track state:
  - active path
  - track-start Unix timestamp
  - accumulated listened duration
  - known duration
  - last resolved metadata
  - now-playing sent flag
  - scrobble queued/sent flag
- Use only structured metadata for submission. Do not derive artist/title from filenames or paths. If required metadata is still missing, defer submission; if the track changes before enough metadata arrives, skip that submission.
- Submission policy:
  - send `track.updateNowPlaying` once per active track after playback is actually `Playing` and required metadata is available
  - mark a scrobble eligible once the fixed threshold is crossed, then queue it when playback stops or the active track changes
  - flush cached scrobbles oldest-first, batching up to 50 per Last.fm request
  - never retry failed now-playing calls
  - retry cached scrobbles on transient failures; keep them ordered
  - on invalid session, preserve cached scrobbles, clear live auth state, and require reconnect
- Add a Last.fm section to Preferences:
  - `Enable Last.fm scrobbling` toggle
  - fixed-rule explanatory text
  - account/status line
  - `Connect`, `Reconnect`, `Complete Connection`, and `Disconnect` actions
  - pending scrobble count and latest error/status text
  - no username/password fields

## Test Plan

- Rust settings tests: round-trip/parse compatibility for the new Last.fm fields, including omission defaults.
- Last.fm client tests: signature generation, exclusion of `format` from `api_sig`, auth-response parsing, and error mapping.
- Bridge/service tests:
  - no scrobble for tracks `<= 30s`
  - scrobble eligibility at `min(240s, 50%)`, with submission deferred until playback stops or the track changes
  - pause/resume preserves listened time
  - forward seek does not trigger a false scrobble
  - metadata arriving late still allows now-playing/scrobble for the same active track
  - invalid session transitions to `ReauthRequired` and preserves cached scrobbles
  - explicit disconnect clears keyring-backed account state and cached scrobbles
- FFI/UI tests: binary snapshot decode for the new Last.fm state, command encoding/dispatch for the new actions, and a QML smoke path that opens Preferences in disconnected and connected states.

## Assumptions

- Ferrous supports one Last.fm account per app profile.
- "Enable scrobbling" also enables Last.fm now-playing updates.
- Desktop auth is manual completion in v1: Ferrous opens the browser and the user clicks `Complete Connection` after authorizing; there is no app-side callback handler.
- Use HTTPS for Last.fm API requests; do not fall back to plaintext credential submission.

## References

- https://www.last.fm/api/scrobbling
- https://www.last.fm/api/desktopauth
- https://www.last.fm/api/authspec
- https://www.last.fm/api/show/track.scrobble
- https://www.last.fm/api/show/track.updateNowPlaying
- https://docs.rs/keyring/latest/keyring/
