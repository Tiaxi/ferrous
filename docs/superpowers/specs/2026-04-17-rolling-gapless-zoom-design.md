# Rolling Gapless Zoom Preservation

## Goal

Preserve the current spectrogram zoom level across true rolling-mode natural gapless handoffs, while keeping the existing zoom-reset behavior for all other track-to-track transitions.

## Behavior

- Preserve zoom when the spectrogram is in rolling mode and the transition is a same-format natural gapless handoff that stays on the continuous path.
- Reset zoom to `1.0` for centered-mode track changes.
- Reset zoom to `1.0` for any rolling-mode transition that starts a fresh session instead of continuing the existing one.
- Do not change seek behavior. Same-track seeks keep the current zoom behavior they already have.

## Detection

Qt already distinguishes the relevant transition types from incoming chunk shape:

- True rolling gapless handoff: token changes without `bufferReset`, `appliedReset`, or `appliedImplicitReset`.
- Fresh-session transition: reset-driven track change (`appliedReset`/`isTrackChange`) even if it occurs in rolling mode.

No protocol or backend changes are needed for this policy change.

## Implementation

- Narrow the zoom reset condition in `ui/src/SpectrogramItem.cpp`.
- Keep zoom reset for:
  - centered `isGaplessTrackChange`
  - any `isTrackChange`
  - fresh-instance backend/hop resync cases that currently rely on the same reset path
- Exempt rolling `isGaplessTrackChange` from the `zoomLevel = 1.0` path so the seamless handoff keeps the active zoom level.

## Tests

- Add a UI smoke test proving that a non-default zoom survives a rolling gapless token-only handoff.
- Assert that the rolling gapless handoff does not emit `zoomResetRequested` or `backendZoomRequested(1.0)`.
- Keep the existing centered gapless and reset-path tests unchanged to preserve the current reset policy there.

## Risks

- The UI logic must not mistake reset-driven rolling transitions for seamless gapless handoffs.
- Fresh widget instances created during track changes must still resync correctly when the transition is not a true rolling gapless continuation.
