# Spectrogram Zoom Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add mouse wheel zoom in/out and middle-click reset for the spectrogram, gated by a persisted settings toggle, working in both rolling and centered display modes.

**Architecture:** The zoom level is a frontend-only concept (no Rust backend changes for zoom itself, only for the settings toggle). SpectrogramItem gains a `zoomLevel` property (qreal, default 1.0) that changes the column-to-pixel mapping in the display range calculation and canvas rendering. A shared QML property synchronizes zoom across per-channel panes. The `spectrogram_zoom_enabled` boolean setting follows the existing settings pipeline (Rust struct → config → binary protocol → C++ property → QML binding).

**Tech Stack:** Rust (settings persistence), C++ / Qt Quick Scene Graph (rendering), QML (UI wiring)

---

## Context

The spectrogram currently renders at a fixed 1:1 mapping: one STFT column per pixel. There is no way to see more or less of the track's spectral content than what fits in the widget at native resolution. This change adds horizontal zoom (time axis) controlled by mouse wheel, with middle-click to reset. The feature is gated by a settings toggle.

**Key insight — how zoom works:** Instead of 1 pixel = 1 column, zoom changes the mapping to 1 pixel = `1/zoomLevel` columns. Zoom in (`zoomLevel > 1.0`) means fewer columns visible (each column spans multiple pixels). Zoom out (`zoomLevel < 1.0`) means more columns visible (multiple columns per pixel). The display range (`displayLeft`..`displayRight`) widens or narrows accordingly, and the canvas rebuild maps columns to pixels with the zoom factor.

**Rolling mode:** Zoom works identically to centered mode. Zoom out shows more playback history; zoom in shows finer temporal detail. When zoomed out beyond what the ring buffer holds, empty areas render black (same as track start). The ring buffer capacity grows to accommodate the zoomed-out view.

**Design decisions:**
- Default zoom = 1.0 (current behavior, no visual change)
- Zoom step = 1.25x per wheel tick (logarithmic — consistent feel at all zoom levels)
- Max zoom in = 16.0x (each column = 16 pixels, ~2.6 seconds visible on 1920px display)
- Min zoom out = dynamic, computed so the entire track fits in the widget (centered mode), same numeric zoom level for rolling mode
- Min zoom floor = 0.05 (absolute minimum for safety)
- Zoom level persists across track changes (user chose this zoom; middle-click to reset)
- Column-to-pixel mapping uses nearest-neighbor sampling (simple, correct; max-pooling for zoom-out quality can be added later)
- Incremental canvas advance (`advancePrecomputedCanvasLocked`) only used at zoom=1.0; non-1.0 zoom always does full canvas rebuild (acceptable performance — full rebuild is O(width×height) and completes well within 16ms)
- Setting defaults to enabled (`true`) — zoom is harmless when not used, no accidental activation without deliberate wheel scroll on the spectrogram

---

## Task 1: Add `spectrogram_zoom_enabled` setting — Rust backend

**Files:**
- Modify: `src/frontend_bridge/mod.rs:192-213` (BridgeSettingsCommand enum)
- Modify: `src/frontend_bridge/mod.rs:368-374` (BridgeDisplaySettings struct)
- Modify: `src/frontend_bridge/mod.rs:396-422` (Default impl)
- Modify: `src/frontend_bridge/config.rs:190-274` (parse_settings_text)
- Modify: `src/frontend_bridge/config.rs:298-317` (format_settings_text)
- Modify: `src/frontend_bridge/ffi.rs:1248-1288` (parse_settings_command)
- Modify: `src/frontend_bridge/ffi.rs:1760-1800` (encode_settings_section)
- Modify: `src/frontend_bridge/commands.rs:182-279` (handle_settings_bridge_command)
- Modify: `src/frontend_bridge/config.rs:319+` (tests)

- [ ] **Step 1: Add field to BridgeDisplaySettings and default**

In `src/frontend_bridge/mod.rs`, add `spectrogram_zoom_enabled: bool` to `BridgeDisplaySettings`:

```rust
pub struct BridgeDisplaySettings {
    pub log_scale: bool,
    pub show_fps: bool,
    pub show_spectrogram_crosshair: bool,
    pub show_spectrogram_scale: bool,
    pub channel_buttons_visibility: u8,
    pub spectrogram_zoom_enabled: bool,
}
```

In `Default for BridgeSettings`, set:

```rust
display: BridgeDisplaySettings {
    log_scale: false,
    show_fps,
    show_spectrogram_crosshair: false,
    show_spectrogram_scale: false,
    channel_buttons_visibility: 1,
    spectrogram_zoom_enabled: true,
},
```

- [ ] **Step 2: Add command variant**

In `src/frontend_bridge/mod.rs`, add to `BridgeSettingsCommand`:

```rust
SetSpectrogramZoomEnabled(bool),
```

- [ ] **Step 3: Add config parse and serialize**

In `src/frontend_bridge/config.rs`, add parse case in `parse_settings_text` after the `channel_buttons_visibility` case:

```rust
"spectrogram_zoom_enabled" => {
    if let Ok(x) = value.parse::<i32>() {
        settings.display.spectrogram_zoom_enabled = x != 0;
    }
}
```

In `format_settings_text`, add after the `channel_buttons_visibility` line:

```
spectrogram_zoom_enabled={}
```

with `i32::from(settings.display.spectrogram_zoom_enabled)`.

- [ ] **Step 4: Add FFI command parsing and encoding**

In `src/frontend_bridge/ffi.rs` `parse_settings_command`, add command ID 56:

```rust
56 => BridgeSettingsCommand::SetSpectrogramZoomEnabled(reader.read_u8()? != 0),
```

In `encode_settings_section`, append after `channel_buttons_visibility`:

```rust
push_u8(
    &mut out,
    u8::from(snapshot.settings.display.spectrogram_zoom_enabled),
);
```

- [ ] **Step 5: Add command handler**

In `src/frontend_bridge/commands.rs` `handle_settings_bridge_command`, add:

```rust
BridgeSettingsCommand::SetSpectrogramZoomEnabled(enabled) => {
    state.settings.display.spectrogram_zoom_enabled = *enabled;
    *context.settings_dirty = true;
}
```

- [ ] **Step 6: Update tests**

In `src/frontend_bridge/config.rs` tests, update `settings_roundtrip_text_format`:
- Add `spectrogram_zoom_enabled: true` to the test BridgeDisplaySettings
- Add assertion: `assert!(parsed.display.spectrogram_zoom_enabled)`

Add a new roundtrip test `settings_roundtrip_zoom_enabled`:

```rust
#[test]
fn settings_roundtrip_zoom_enabled() {
    let mut settings = BridgeSettings::default();
    settings.display.spectrogram_zoom_enabled = false;
    let text = format_settings_text(&settings);
    let mut parsed = BridgeSettings::default();
    parse_settings_text(&mut parsed, &text);
    assert!(!parsed.display.spectrogram_zoom_enabled);

    // Default (key absent) should be true.
    let mut default_parsed = BridgeSettings::default();
    parse_settings_text(&mut default_parsed, "volume=1.0\n");
    assert!(default_parsed.display.spectrogram_zoom_enabled);
}
```

- [ ] **Step 7: Build and test**

Run: `./scripts/run-tests.sh --rust-only`
Expected: All tests pass, no clippy warnings.

- [ ] **Step 8: Commit**

```bash
git add src/frontend_bridge/mod.rs src/frontend_bridge/config.rs \
        src/frontend_bridge/ffi.rs src/frontend_bridge/commands.rs
git commit -m "feat: add spectrogram_zoom_enabled setting to Rust backend"
```

---

## Task 2: Add setting to C++ codec and BridgeClient

**Files:**
- Modify: `ui/src/BinaryBridgeCodec.h:34-83` (command constant), `ui/src/BinaryBridgeCodec.h:163-178` (DecodedSettings)
- Modify: `ui/src/BinaryBridgeCodec.cpp:376-435` (decodeSettingsSection)
- Modify: `ui/src/BridgeClient.h:107-146` (Q_PROPERTY), `ui/src/BridgeClient.h:255-289` (setter), `ui/src/BridgeClient.h:535-555` (member)
- Modify: `ui/src/BridgeClient.cpp` (getter, setter, snapshot apply)

- [ ] **Step 1: Add command constant and decoded struct field**

In `ui/src/BinaryBridgeCodec.h`, add command constant:

```cpp
CmdSetSpectrogramZoomEnabled = 56,
```

Add field to `DecodedSettings`:

```cpp
bool spectrogramZoomEnabled{true};
```

- [ ] **Step 2: Add decode logic**

In `ui/src/BinaryBridgeCodec.cpp` `decodeSettingsSection`, replace the final `atEnd` check:

```cpp
// Before:
//     if (!reader.atEnd()) {
//         return false;
//     }

// After:
    quint8 spectrogramZoomEnabled = 1;
    if (!reader.atEnd() && !reader.readU8(&spectrogramZoomEnabled)) {
        return false;
    }
    if (!reader.atEnd()) {
        return false;
    }
    // ... existing assignments ...
    out->spectrogramZoomEnabled = spectrogramZoomEnabled != 0;
```

- [ ] **Step 3: Add BridgeClient property, getter, setter, member**

In `ui/src/BridgeClient.h`:

Add Q_PROPERTY after `showSpectrogramScale`:

```cpp
Q_PROPERTY(bool spectrogramZoomEnabled READ spectrogramZoomEnabled NOTIFY snapshotChanged)
```

Add getter declaration:

```cpp
bool spectrogramZoomEnabled() const;
```

Add setter declaration:

```cpp
Q_INVOKABLE void setSpectrogramZoomEnabled(bool value);
```

Add member variable:

```cpp
bool m_spectrogramZoomEnabled{true};
```

- [ ] **Step 4: Implement getter and setter in BridgeClient.cpp**

Getter (near other settings getters):

```cpp
bool BridgeClient::spectrogramZoomEnabled() const {
    return m_spectrogramZoomEnabled;
}
```

Setter (near other settings setters):

```cpp
void BridgeClient::setSpectrogramZoomEnabled(bool value) {
    if (m_spectrogramZoomEnabled != value) {
        m_spectrogramZoomEnabled = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetSpectrogramZoomEnabled,
        static_cast<quint8>(value ? 1 : 0)));
}
```

- [ ] **Step 5: Apply from snapshot**

In the snapshot application section of `BridgeClient.cpp` (near `showSpectrogramScale` handling, around line 4959):

```cpp
const bool spectrogramZoomEnabled = snapshot.settings.present
    ? snapshot.settings.spectrogramZoomEnabled
    : m_spectrogramZoomEnabled;
if (m_spectrogramZoomEnabled != spectrogramZoomEnabled) {
    m_spectrogramZoomEnabled = spectrogramZoomEnabled;
    changed = true;
    snapshotSignalChanged = true;
}
```

- [ ] **Step 6: Build**

Run: `./scripts/run-tests.sh --ui-only`
Expected: Build succeeds, tests pass.

- [ ] **Step 7: Commit**

```bash
git add ui/src/BinaryBridgeCodec.h ui/src/BinaryBridgeCodec.cpp \
        ui/src/BridgeClient.h ui/src/BridgeClient.cpp
git commit -m "feat: add spectrogram_zoom_enabled setting to C++ bridge"
```

---

## Task 3: Add zoom properties to SpectrogramItem

**Files:**
- Modify: `ui/src/SpectrogramItem.h`
- Modify: `ui/src/SpectrogramItem.cpp`

- [ ] **Step 1: Add properties, signals, and members to header**

In `ui/src/SpectrogramItem.h`, add Q_PROPERTYs after `channelMuted`:

```cpp
Q_PROPERTY(double zoomLevel READ zoomLevel WRITE setZoomLevel NOTIFY zoomLevelChanged)
Q_PROPERTY(bool zoomEnabled READ zoomEnabled WRITE setZoomEnabled NOTIFY zoomEnabledChanged)
```

Add public getters/setters:

```cpp
double zoomLevel() const;
void setZoomLevel(double value);

bool zoomEnabled() const;
void setZoomEnabled(bool value);
```

Add signals:

```cpp
void zoomLevelChanged();
void zoomEnabledChanged();
void zoomRequested(double newZoomLevel);
void zoomResetRequested();
```

Add protected event override:

```cpp
void wheelEvent(QWheelEvent *event) override;
```

Add private members:

```cpp
double m_zoomLevel{1.0};
bool m_zoomEnabled{false};
double m_precomputedCanvasZoomLevel{1.0};
```

- [ ] **Step 2: Implement property accessors**

In `ui/src/SpectrogramItem.cpp`:

```cpp
double SpectrogramItem::zoomLevel() const {
    return m_zoomLevel;
}

void SpectrogramItem::setZoomLevel(double value) {
    // Clamp to valid range
    value = std::clamp(value, 0.05, 16.0);
    QMutexLocker lock(&m_stateMutex);
    if (std::abs(m_zoomLevel - value) < 0.0001) {
        return;
    }
    m_zoomLevel = value;
    m_precomputedCanvasDirty = true;
    m_crosshairDirty = true;
    m_timeGridDirty = true;
    lock.unlock();
    emit zoomLevelChanged();
    update();
}

bool SpectrogramItem::zoomEnabled() const {
    return m_zoomEnabled;
}

void SpectrogramItem::setZoomEnabled(bool value) {
    if (m_zoomEnabled == value) {
        return;
    }
    m_zoomEnabled = value;
    emit zoomEnabledChanged();
    // Reset zoom to 1.0 when disabling so the user isn't stuck
    // at a non-default zoom with no way to reset.
    if (!value && std::abs(m_zoomLevel - 1.0) > 0.001) {
        emit zoomResetRequested();
    }
}
```

- [ ] **Step 3: Implement wheelEvent**

```cpp
void SpectrogramItem::wheelEvent(QWheelEvent *event) {
    if (!m_zoomEnabled) {
        event->ignore();
        return;
    }
    event->accept();

    // angleDelta().y() is typically ±120 per notch
    const double steps = event->angleDelta().y() / 120.0;
    if (std::abs(steps) < 0.01) {
        return;
    }

    constexpr double kZoomStepFactor = 1.25;
    const double newZoom = m_zoomLevel * std::pow(kZoomStepFactor, steps);
    emit zoomRequested(newZoom);
}
```

- [ ] **Step 4: Add middle-click to mousePressEvent**

In `SpectrogramItem::mousePressEvent`, add middle button handling at the top (before the existing right-button check):

```cpp
if (event->button() == Qt::MiddleButton && m_zoomEnabled) {
    event->accept();
    emit zoomResetRequested();
    return;
}
```

- [ ] **Step 5: Build and test**

Run: `./scripts/run-tests.sh --ui-only`
Expected: Build succeeds.

- [ ] **Step 6: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp
git commit -m "feat: add zoom properties and mouse input to SpectrogramItem"
```

---

## Task 4: Wire up shared zoom in QML and add settings checkbox

**Files:**
- Modify: `ui/qml/viewers/SpectrogramSurface.qml`
- Modify: `ui/qml/preferences/SpectrogramPage.qml`

- [ ] **Step 1: Add shared zoom to SpectrogramSurface.qml**

In `ui/qml/viewers/SpectrogramSurface.qml`, add a shared zoom property alongside `_crosshairSharedX`:

```qml
property double _sharedZoomLevel: 1.0
```

In the SpectrogramItem delegate, bind zoom properties and handle signals:

```qml
SpectrogramItem {
    id: spectrogramPaneItem
    // ... existing bindings ...
    zoomEnabled: root.uiBridge.spectrogramZoomEnabled
    zoomLevel: root._sharedZoomLevel
    onZoomRequested: (newZoomLevel) => {
        root._sharedZoomLevel = Math.max(0.05, Math.min(16.0, newZoomLevel))
    }
    onZoomResetRequested: {
        root._sharedZoomLevel = 1.0
    }
}
```

- [ ] **Step 2: Add settings checkbox to SpectrogramPage.qml**

In `ui/qml/preferences/SpectrogramPage.qml`, add a CheckBox after the "Show Frequency/Time Scale" checkbox:

```qml
CheckBox {
    text: "Allow Spectrogram Zoom"
    focusPolicy: Qt.NoFocus
    checked: root.uiBridge.spectrogramZoomEnabled
    onToggled: root.uiBridge.setSpectrogramZoomEnabled(checked)
}

Label {
    Layout.fillWidth: true
    Layout.leftMargin: 32
    wrapMode: Text.Wrap
    color: Kirigami.Theme.disabledTextColor
    font.pixelSize: 12
    text: "Scroll to zoom in/out. Middle-click to reset."
}
```

- [ ] **Step 3: Build and test**

Run: `./scripts/run-tests.sh --ui-only`
Expected: Build succeeds. At this point, the setting toggle and zoom input work, but the spectrogram doesn't visually respond to zoom yet (rendering changes are in the next task).

- [ ] **Step 4: Commit**

```bash
git add ui/qml/viewers/SpectrogramSurface.qml \
        ui/qml/preferences/SpectrogramPage.qml
git commit -m "feat: wire up shared zoom in QML and add settings checkbox"
```

---

## Task 5: Implement zoom-aware display range and canvas rendering

This is the core rendering task. Changes are in `ui/src/SpectrogramItem.cpp`.

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp` — updatePaintNode (~lines 1460-1620), rebuildPrecomputedCanvasLocked (~line 2926), advancePrecomputedCanvasLocked (~line 2968), ensureRingCapacityLocked (~line 860)

- [ ] **Step 1: Modify display range calculation in updatePaintNode — centered mode**

In `updatePaintNode`, centered mode block (around line 1470-1507), change the display range to account for zoom:

```cpp
if (m_displayMode == 1) {
    rollingMode = false;
    // With zoom: visible columns = w / zoomLevel
    const int halfWindowCols = static_cast<int>(
        static_cast<double>(w) / m_zoomLevel / 2.0);
    const int visibleWindowCols = static_cast<int>(
        static_cast<double>(w) / m_zoomLevel);
    const qint64 totalCols = m_precomputedMaxColumnIndex >= 0
        ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
        : std::max(static_cast<qint64>(m_precomputedTotalColumnsEstimate),
                   static_cast<qint64>(1));
    displayLeft = std::max(static_cast<qint64>(0),
        static_cast<qint64>(nowCol) - halfWindowCols);
    displayRight = std::min(
        totalCols - 1,
        displayLeft + static_cast<qint64>(visibleWindowCols) - 1);
    displayLeft = std::max<qint64>(
        0, displayRight - static_cast<qint64>(visibleWindowCols) + 1);

    // Jitter prevention (same logic as before)
    const bool isSeekJump =
        m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
        && displayLeft < m_precomputedCanvasDisplayLeft - 2;
    if (!isSeekJump
        && m_precomputedCanvasDisplayRight >= m_precomputedCanvasDisplayLeft
        && std::abs(m_zoomLevel - m_precomputedCanvasZoomLevel) < 0.001) {
        displayLeft = std::max(displayLeft, m_precomputedCanvasDisplayLeft);
        displayRight = std::max(displayRight, m_precomputedCanvasDisplayRight);
    }

    playheadPixel = static_cast<int>(std::round(
        static_cast<double>(nowCol - displayLeft) * m_zoomLevel));
```

- [ ] **Step 2: Modify display range calculation — rolling mode**

In the rolling mode block (around line 1508-1525):

```cpp
} else {
    rollingMode = true;
    const int visibleWindowCols = static_cast<int>(
        static_cast<double>(w) / m_zoomLevel);
    const qint64 displaySeq =
        m_rollingEpoch + static_cast<qint64>(std::max(nowCol, 0));
    writeHeadSeq = m_ringWriteSeq - 1;
    displayRight = std::min(displaySeq, writeHeadSeq);
    displayLeft = std::max(m_ringOldestSeq,
        displayRight - static_cast<qint64>(visibleWindowCols) + 1);
    playheadPixel = -1;
```

- [ ] **Step 3: Modify needsFullRebuild and drawX calculations**

Replace the `needsFullRebuild` condition (around line 1571-1580). Replace the `visibleCols > m_canvas.width()` condition with a zoom-change check:

```cpp
const bool needsFullRebuild =
    visibleCols > 0
    && (m_canvas.isNull()
        || m_canvas.width() != w
        || m_canvas.height() != h
        || rollingMode != m_precomputedCanvasRolling
        || !hasCanvasRange
        || displayLeft < m_precomputedCanvasDisplayLeft
        || displayRight < m_precomputedCanvasDisplayRight
        || std::abs(m_zoomLevel - m_precomputedCanvasZoomLevel) > 0.001);
```

Modify the drawX calculation (around line 1596-1618) to scale columnPhase by zoom:

```cpp
if (rollingMode) {
    drawX = static_cast<double>(w - drawCols)
        - columnPhase * m_zoomLevel;
} else {
    const qint64 totalColsForScroll = m_precomputedMaxColumnIndex >= 0
        ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
        : std::max(static_cast<qint64>(m_precomputedTotalColumnsEstimate),
                   static_cast<qint64>(1));
    const int visibleWindowCols = static_cast<int>(
        static_cast<double>(w) / m_zoomLevel);
    const bool centeredScrolling =
        displayLeft > 0
        && displayRight < totalColsForScroll - 1;
    drawX = centeredScrolling
        ? -columnPhase * m_zoomLevel
        : 0.0;
}
```

- [ ] **Step 4: Modify rebuildPrecomputedCanvasLocked for zoom**

In `rebuildPrecomputedCanvasLocked` (around line 2926), change the drawing loop to map pixels to columns with zoom:

```cpp
void SpectrogramItem::rebuildPrecomputedCanvasLocked(
    int width, int height,
    qint64 displayLeft, qint64 displayRight,
    bool rollingMode) {
    if (width <= 0 || height <= 0 || displayRight < displayLeft) {
        invalidateCanvas();
        return;
    }

    if (m_canvas.isNull()
        || m_canvas.width() != width
        || m_canvas.height() != height
        || m_canvas.format() != QImage::Format_RGB32) {
        m_canvas = QImage(width, height, QImage::Format_RGB32);
    }
    m_canvas.fill(Qt::black);
    resizeDirtyTilesLocked();
    markAllTilesDirtyLocked();

    const qint64 sourceColumns = displayRight - displayLeft + 1;
    // How many pixels these source columns fill at the current zoom
    const int drawPixels = std::min(width,
        static_cast<int>(std::ceil(
            static_cast<double>(sourceColumns) * m_zoomLevel)));
    const double columnsPerPixel = 1.0 / m_zoomLevel;
    const auto dbRemap = buildPrecomputedDbRemapLocked();

    for (int px = 0; px < drawPixels; ++px) {
        const qint64 col = std::min(
            displayLeft + static_cast<qint64>(
                static_cast<double>(px) * columnsPerPixel),
            displayRight);
        drawPrecomputedColumnAtLocked(px, col, rollingMode, dbRemap);
    }

    m_canvasWriteX = width > 0 ? (drawPixels % width) : 0;
    m_canvasFilledCols = drawPixels;
    m_precomputedCanvasDisplayLeft = displayLeft;
    m_precomputedCanvasDisplayRight =
        drawPixels > 0
            ? (displayLeft + static_cast<qint64>(
                   static_cast<double>(drawPixels) * columnsPerPixel) - 1)
            : (displayLeft - 1);
    m_precomputedCanvasRolling = rollingMode;
    m_precomputedCanvasZoomLevel = m_zoomLevel;
    m_precomputedCanvasDirty = false;
}
```

- [ ] **Step 5: Guard advancePrecomputedCanvasLocked for zoom=1.0 only**

At the top of `advancePrecomputedCanvasLocked` (around line 2968), add a zoom guard:

```cpp
bool SpectrogramItem::advancePrecomputedCanvasLocked(
    qint64 displayLeft, qint64 displayRight, bool rollingMode) {
    // Incremental advance only works at 1:1 column-to-pixel mapping.
    // TODO: Implement incremental advance for non-1.0 zoom levels if
    // full rebuild shows measurable frame drops on target hardware.
    if (std::abs(m_zoomLevel - 1.0) > 0.001) {
        return false;
    }
    // ... rest of existing code unchanged ...
```

- [ ] **Step 6: Adjust ring buffer capacity for zoom in rolling mode**

In `ensureRingCapacityLocked` (around line 860-876), adjust rolling mode capacity:

```cpp
} else {
    // Rolling: need screen width / zoomLevel of history + lookahead.
    const int zoomAdjustedWidth = static_cast<int>(
        static_cast<double>(screenWidth) / std::max(0.05, m_zoomLevel));
    neededCapacity = zoomAdjustedWidth
        + static_cast<int>(extraSeconds * colsPerSecond);
}
```

- [ ] **Step 7: Build and test**

Run: `./scripts/run-tests.sh --ui-only`
Expected: Build succeeds. Zoom should now visually work — scrolling the wheel over the spectrogram changes the visible range.

- [ ] **Step 8: Commit**

```bash
git add ui/src/SpectrogramItem.cpp
git commit -m "feat: implement zoom-aware display range and canvas rendering"
```

---

## Task 6: Update pixel-time helpers and overlays for zoom

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp` — free functions pixelToTimeSeconds/timeToPixelX (~line 266-299), updateCrosshairOverlayLocked, updateTimeGridOverlayLocked, mousePressEvent

- [ ] **Step 1: Add zoom parameter to pixelToTimeSeconds and timeToPixelX**

These are free functions near the top of SpectrogramItem.cpp (lines 266-299). Add a `zoomLevel` parameter:

```cpp
double pixelToTimeSeconds(
    double pixelX,
    qint64 displayLeft,
    bool rollingMode,
    qint64 rollingEpoch,
    double columnsPerSecond,
    double drawX,
    double zoomLevel = 1.0) {
    if (columnsPerSecond <= 0.0) {
        return -1.0;
    }
    const double columnsPerPixel = 1.0 / zoomLevel;
    const double columnF =
        static_cast<double>(displayLeft) + (pixelX - drawX) * columnsPerPixel;
    double trackColumn = columnF;
    if (rollingMode) {
        trackColumn -= static_cast<double>(rollingEpoch);
    }
    return trackColumn / columnsPerSecond;
}

double timeToPixelX(
    double timeSeconds,
    qint64 displayLeft,
    bool rollingMode,
    qint64 rollingEpoch,
    double columnsPerSecond,
    double drawX,
    double zoomLevel = 1.0) {
    double column = timeSeconds * columnsPerSecond;
    if (rollingMode) {
        column += static_cast<double>(rollingEpoch);
    }
    return drawX + (column - static_cast<double>(displayLeft)) * zoomLevel;
}
```

- [ ] **Step 2: Pass zoom to all callers**

In `mousePressEvent` (around line 2190):

```cpp
const double seconds = pixelToTimeSeconds(
    event->position().x(),
    m_crosshairCachedDisplayLeft,
    m_crosshairCachedRollingMode,
    m_rollingEpoch,
    columnsPerSecond,
    m_crosshairCachedDrawX,
    m_zoomLevel);
```

In `updateCrosshairOverlayLocked`, all calls to `pixelToTimeSeconds` and `timeToPixelX` need the zoom parameter. Pass `m_zoomLevel` as the last argument to each call. (These are accessed under mutex already.)

In `updateTimeGridOverlayLocked`, similarly pass `m_zoomLevel` to all `pixelToTimeSeconds` and `timeToPixelX` calls. Also update the `secondsPerPixel` calculation:

```cpp
const double secondsPerPixel = 1.0 / (columnsPerSecond * m_zoomLevel);
```

- [ ] **Step 3: Cache zoom for crosshair**

The crosshair uses cached values for drawing between frames. Cache the zoom level alongside other cached values. No new member needed — the crosshair overlay is rebuilt every time `m_crosshairDirty` is set, and `setZoomLevel` already sets `m_crosshairDirty = true`.

- [ ] **Step 4: Build and verify**

Run: `./scripts/run-tests.sh --ui-only`
Expected: Build succeeds. Crosshair, time grid, and seek-by-click all work correctly at non-1.0 zoom levels.

- [ ] **Step 5: Commit**

```bash
git add ui/src/SpectrogramItem.cpp
git commit -m "feat: update pixel-time helpers and overlays for zoom"
```

---

## Task 7: Add dynamic zoom limits based on track duration

**Files:**
- Modify: `ui/src/SpectrogramItem.cpp` — setZoomLevel, wheelEvent
- Modify: `ui/qml/viewers/SpectrogramSurface.qml` — zoom clamping

The zoom-out limit should allow seeing the entire track. This depends on the track's total columns and the widget width.

- [ ] **Step 1: Add minimum zoom calculation**

Add a private helper method to SpectrogramItem.h:

```cpp
double minimumZoomLevelLocked() const;
```

Implement in SpectrogramItem.cpp:

```cpp
double SpectrogramItem::minimumZoomLevelLocked() const {
    const int w = static_cast<int>(width());
    if (w <= 0) {
        return 0.05;
    }
    const qint64 totalCols = m_precomputedMaxColumnIndex >= 0
        ? static_cast<qint64>(m_precomputedMaxColumnIndex) + 1
        : std::max(static_cast<qint64>(m_precomputedTotalColumnsEstimate),
                   static_cast<qint64>(1));
    if (totalCols <= 0) {
        return 0.05;
    }
    const double minZoom =
        static_cast<double>(w) / static_cast<double>(totalCols);
    return std::max(0.05, minZoom);
}
```

- [ ] **Step 2: Apply dynamic limits in wheelEvent**

Update the `wheelEvent` to clamp against the dynamic minimum:

```cpp
void SpectrogramItem::wheelEvent(QWheelEvent *event) {
    if (!m_zoomEnabled) {
        event->ignore();
        return;
    }
    event->accept();

    const double steps = event->angleDelta().y() / 120.0;
    if (std::abs(steps) < 0.01) {
        return;
    }

    constexpr double kZoomStepFactor = 1.25;
    constexpr double kMaxZoom = 16.0;
    QMutexLocker lock(&m_stateMutex);
    const double minZoom = minimumZoomLevelLocked();
    const double currentZoom = m_zoomLevel;
    lock.unlock();
    const double newZoom = std::clamp(
        currentZoom * std::pow(kZoomStepFactor, steps),
        minZoom, kMaxZoom);
    emit zoomRequested(newZoom);
}
```

- [ ] **Step 3: Add a Q_INVOKABLE for minimum zoom**

Add to SpectrogramItem.h:

```cpp
Q_INVOKABLE double minimumZoomLevel() const;
```

Implement:

```cpp
double SpectrogramItem::minimumZoomLevel() const {
    QMutexLocker lock(&m_stateMutex);
    return minimumZoomLevelLocked();
}
```

- [ ] **Step 4: Update QML zoom clamping**

In `ui/qml/viewers/SpectrogramSurface.qml`, update the `onZoomRequested` handler to use the dynamic minimum. Since all panes share the same track data, use the first pane's minimum:

```qml
onZoomRequested: (newZoomLevel) => {
    const minZoom = spectrogramPaneItem.minimumZoomLevel()
    root._sharedZoomLevel = Math.max(minZoom,
        Math.min(16.0, newZoomLevel))
}
```

- [ ] **Step 5: Clamp zoom on track change**

In `setZoomLevel`, apply the dynamic minimum:

```cpp
void SpectrogramItem::setZoomLevel(double value) {
    QMutexLocker lock(&m_stateMutex);
    const double minZoom = minimumZoomLevelLocked();
    value = std::clamp(value, minZoom, 16.0);
    if (std::abs(m_zoomLevel - value) < 0.0001) {
        lock.unlock();
        return;
    }
    m_zoomLevel = value;
    m_precomputedCanvasDirty = true;
    m_crosshairDirty = true;
    m_timeGridDirty = true;
    lock.unlock();
    emit zoomLevelChanged();
    update();
}
```

- [ ] **Step 6: Build and test**

Run: `./scripts/run-tests.sh --ui-only`
Expected: Build succeeds. Zoom out stops at the level where the entire track is visible. Zoom in stops at 16x.

- [ ] **Step 7: Commit**

```bash
git add ui/src/SpectrogramItem.h ui/src/SpectrogramItem.cpp \
        ui/qml/viewers/SpectrogramSurface.qml
git commit -m "feat: add dynamic zoom limits based on track duration"
```

---

## Task 8: Tests

**Files:**
- Modify: `ui/tests/tst_qml_smoke.cpp`

- [ ] **Step 1: Add zoom property test**

Add a test to `tst_qml_smoke.cpp` that verifies zoom property behavior:

```cpp
void QmlSmokeTest::spectrogramZoomProperty() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Default zoom is 1.0
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);
    QCOMPARE(item.zoomEnabled(), false);

    // Setting zoom level works
    item.setZoomLevel(2.0);
    QVERIFY(std::abs(item.zoomLevel() - 2.0) < 0.0001);

    // Zoom clamps to maximum
    item.setZoomLevel(100.0);
    QVERIFY(std::abs(item.zoomLevel() - 16.0) < 0.0001);

    // Zoom clamps to minimum floor
    item.setZoomLevel(0.001);
    QVERIFY(std::abs(item.zoomLevel() - 0.05) < 0.0001);

    // Reset to 1.0
    item.setZoomLevel(1.0);
    QVERIFY(std::abs(item.zoomLevel() - 1.0) < 0.0001);
}
```

Add declaration in the test class:

```cpp
void spectrogramZoomProperty();
```

Add to test slots.

- [ ] **Step 2: Add zoom with track data test**

Test that minimum zoom level adjusts based on track data:

```cpp
void QmlSmokeTest::spectrogramZoomLimitsWithTrackData() {
    SpectrogramItem item;
    item.setWidth(1920);
    item.setHeight(400);

    // Feed some precomputed data to set up track columns
    constexpr int binsPerColumn = 64;
    constexpr int columns = 9600; // ~200 seconds at 48 cols/sec
    QByteArray chunk(binsPerColumn * columns, '\x40');
    item.feedPrecomputedChunk(
        chunk, binsPerColumn, 0, columns,
        0, columns, 48000, 1024, true,
        true, 1, false);

    // Minimum zoom should allow seeing all columns
    const double minZoom = item.minimumZoomLevel();
    QVERIFY(minZoom > 0.0);
    QVERIFY(minZoom <= 1.0);
    // 1920 / 9600 = 0.2
    QVERIFY(std::abs(minZoom - 0.2) < 0.01);
}
```

- [ ] **Step 3: Build and run all tests**

Run: `./scripts/run-tests.sh`
Expected: All Rust and C++ tests pass.

- [ ] **Step 4: Commit**

```bash
git add ui/tests/tst_qml_smoke.cpp
git commit -m "test: add SpectrogramItem zoom property and limit tests"
```

---

## Verification

After all tasks are complete:

1. **Build full project:** `./scripts/run-tests.sh`
2. **Manual testing checklist:**
   - Open a track, scroll wheel on spectrogram → zoom in/out smoothly
   - Middle-click → zoom resets to 1.0
   - Zoom out until entire track is visible (centered mode) → stop at limit
   - Zoom in to 16x → stop at limit
   - Switch to rolling mode → zoom works the same
   - Zoom out in rolling mode → see more history, black areas fill as playback continues
   - Disable zoom in settings → scroll wheel does nothing on spectrogram
   - Crosshair shows correct time/frequency at all zoom levels
   - Right-click seek works correctly at non-1.0 zoom
   - Time grid labels adapt spacing to zoom level
   - Playhead stays at center in centered mode at all zoom levels
   - Gapless transition while zoomed → no visual glitches
   - Switch display mode while zoomed → zoom persists, rendering correct
3. **Performance check:** Playback at high zoom (16x) or full zoom-out — verify smooth scrolling without frame drops (check FPS overlay). Specifically measure canvas rebuild time with the FPS overlay at non-1.0 zoom during playback.

---

## Future Enhancements (not in scope for v1)

- **Cursor-anchored zoom:** Zoom toward the cursor position instead of the playhead/write head, matching DAW/image editor UX. The `wheelEvent` would pass the cursor's column position, and the display range calculation would shift to keep that column at the same pixel.
- **Visual zoom indicator:** A small overlay (e.g., "2.0x" in a corner, fading out after a second) to show the current zoom level and help users discover middle-click reset.
- **Max-pooling for zoom-out:** Replace nearest-neighbor column sampling with per-bin max-pooling across the column range that maps to each pixel, preventing thin spectral features from being invisible at low zoom.
- **Rolling mode memory cap:** At extreme zoom-out (0.05x), the ring buffer can grow large (~20 MB/channel). A secondary cap on ring buffer capacity (e.g., 60 seconds of audio) could limit memory while degrading gracefully (black fill for oldest columns).
