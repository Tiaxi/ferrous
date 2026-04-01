# Channel M/S Buttons Design Spec

## Problem

The current click/double-click interaction on channel labels for muting and soloing is confusing. Qt Quick fires `onClicked` before `onDoubleClicked`, requiring a Timer to disambiguate, which adds latency to single-clicks and creates visual jitter. The interaction is also not discoverable — there's no indication that double-click does anything.

## Solution

Replace click/double-click with explicit **M** (mute) and **S** (solo) buttons inline with each channel label, following the DAW convention for track controls.

## UI Layout

Each per-channel spectrogram pane shows:

```
[L] [M] [S]      ← top-left corner, 8px margin
```

- **Channel label** (L, R, C, LFE, Ls, Rs, etc.): always visible. Retains existing mute styling — red background, strikeout text, and reduced opacity when the channel is muted.
- **M button**: appears based on visibility setting (see below). Toggles the channel's mute state.
- **S button**: appears based on visibility setting. Toggles solo on this channel.
- **Grayscale spectrogram**: unchanged. `SpectrogramItem.channelMuted` continues to control color vs grayscale rendering.

## Button States

| State | M button | S button |
|-------|----------|----------|
| Inactive | Dim background `rgba(0,0,0,0.35)`, muted text `rgba(180,180,200,0.8)` | Same as inactive M |
| Muted | Red background `rgba(200,60,60,0.5)`, bright text `rgba(255,200,200,0.95)` | — |
| Soloed | — | Amber background `rgba(180,160,40,0.55)`, bright yellow text `rgba(255,240,140,0.95)` |

Font: 10px, weight 600 (semi-bold), 0.5px letter-spacing. Border-radius 3px. Cursor: pointing hand.

## Button Behavior

- **M click**: calls `toggleChannelMute(channelIndex)`. If this is the soloed channel, unsolos (restores pre-solo mask) — existing backend behavior.
- **S click**: calls `soloChannel(channelIndex)`. If already soloed on this channel, unsolos.

## Solo State Exposure

The S button needs to know whether this specific channel is the currently soloed one.

**Backend**: Add `soloed_channel: i8` to the playback snapshot (−1 = no solo, 0–63 = soloed channel index). Encode in the binary snapshot protocol after `muted_channels_mask`.

**C++ BridgeClient**: New property `soloedChannel` (int, default −1), updated from snapshot, notifies via `playbackChanged`.

**QML**: S button binds its active state to `root.uiBridge.soloedChannel === modelData.channelIndex`.

## Visibility Setting

A three-state persistent setting controls M/S button visibility:

| Value | Label | Behavior |
|-------|-------|----------|
| 0 | Disabled | Buttons never shown. Muting/soloing not possible via UI. |
| 1 | On hover | Buttons appear when the mouse hovers anywhere on the channel pane. Default. |
| 2 | Always | Buttons always visible alongside the channel label. |

**Persistence**: Stored in the existing settings text format (`format_settings_text` / `load_settings_into`). Setting name: `channel_buttons_visibility`. Default: 1 (on hover).

**UI**: Exposed in the Spectrogram preferences page as a dropdown/combo box.

**Bridge property**: `channelButtonsVisibility` (int), notifies via `snapshotChanged`.

## Hover Detection

A `MouseArea` covering the entire channel pane delegate provides `containsMouse` for hover state. The M/S buttons bind their `visible` (or `opacity`) to this hover state (when the setting is "on hover"). The channel label's existing visibility is unaffected.

## What Gets Removed

- Timer-based click disambiguation (`clickTimer`)
- `onClicked` and `onDoubleClicked` handlers on the label's `MouseArea`
- The label `MouseArea` itself (no longer clickable)
- The label's hover-dependent opacity change (`labelMouse.containsMouse`)

## What Stays

- Channel label visual: position, colors, strikeout, red background when muted
- `SpectrogramItem.channelMuted` property and grayscale palette rendering
- All backend mute/solo logic (commands, state machine, pre-mask save/restore)
- `isChannelMuted()` C++ method for QML mask checks
- DAC auto-mute prevention (±1 LSB AC fill in mute probe)
