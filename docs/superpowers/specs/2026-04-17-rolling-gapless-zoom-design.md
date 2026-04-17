# Rolling Gapless Zoom Preservation

## Goal

Preserve the current spectrogram zoom level across true rolling-mode natural gapless handoffs, while keeping the existing zoom-reset behavior for all other track-to-track transitions.

## Behavior

- Preserve zoom when the spectrogram is in rolling mode and the transition is a same-format natural gapless handoff that stays on the continuous path.
- Reset zoom to `1.0` for centered-mode track changes.
- Reset zoom to `1.0` for any rolling-mode transition that starts a fresh session instead of continuing the existing one.
- Do not change seek behavior. Same-track seeks keep the current zoom behavior they already have.

## Detection

Qt already has enough information to distinguish the relevant transition types without any protocol or backend changes, but the decision is not based on chunk shape alone. It uses both transition classification and widget-local zoom state:

- True rolling gapless handoff: token changes without `bufferReset`, `appliedReset`, or `appliedImplicitReset` (`isGaplessTrackChange` in rolling mode).
- Fresh-session transition: reset-driven track change (`appliedReset`/`isTrackChange`) even if it occurs in rolling mode.
- Fresh-instance/backend-hop resync: existing widget-local guards (`qtRenderNotAtDefault` and `backendNotAtReferenceHop`) still determine whether a reset-path track change must force Qt and backend back to zoom `1.0`.

No protocol or backend changes are needed for this policy change.

## Implementation

- Narrow the zoom reset condition in `ui/src/SpectrogramItem.cpp`.
- The preserve rule is exact:
  - preserve zoom only when `m_displayMode == 0 && isGaplessTrackChange`
- Keep zoom reset behavior for:
  - centered `isGaplessTrackChange`
  - any `isTrackChange`
  - existing fresh-instance/backend-hop resync cases guarded by `qtRenderNotAtDefault || backendNotAtReferenceHop`
- The implementation must keep the existing `qtRenderNotAtDefault || backendNotAtReferenceHop` gate and only change which transition classes feed into it.

## Tests

- Add an explicit UI smoke test proving that a non-default zoom survives a rolling gapless token-only handoff.
- Assert that the rolling gapless handoff does not emit `zoomResetRequested` or `backendZoomRequested(1.0)`.
- Add an explicit centered token-only gapless test that still expects the zoom reset path to fire.
- Add an explicit rolling reset-driven track-change test that still expects the zoom reset path to fire.
- Keep the existing fresh-instance/backend-hop resync test coverage intact.

## Risks

- The UI logic must not mistake reset-driven rolling transitions for seamless gapless handoffs.
- Fresh widget instances created during track changes must still resync correctly when the transition is not a true rolling gapless continuation.
- Pane recreation during a natural rolling gapless handoff is intentionally out of the preserve contract. The preserve behavior applies to surviving rolling `SpectrogramItem` instances that observe the token-only handoff; fresh-instance recreation remains handled by the existing resync/reset path.
