// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import "../logic/FormatUtils.js" as FormatUtils

QtObject {
    id: root

    required property var uiBridge
    property bool visualFeedsEnabled: false
    property bool seekPressed: false

    property real displayedPositionSeconds: 0
    property real spectrogramPositionSeconds: 0
    property bool positionSmoothingPrimed: false
    property real positionSmoothingAnchorSeconds: 0
    property real positionSmoothingLastMs: 0
    property string positionSmoothingTrackPath: ""
    property string stoppedSpectrogramTrackPath: ""
    property string lastSpectrogramPlaybackState: ""
    property real rememberedVolumeBeforeMute: 1.0
    property bool volumeMuted: false

    // Local interpolation: advance position at 1 s/s between bridge
    // updates, anchored to the last known position + wall-clock time.
    property real interpolationAnchorPosition: 0
    property real interpolationAnchorMs: 0
    property bool interpolationActive: false

    // Timer drives local interpolation at ~60 fps.
    property Timer interpolationTimer: Timer {
        interval: 16
        repeat: true
        running: root.interpolationActive && !root.seekPressed
        onTriggered: {
            const nowMs = Date.now()
            const elapsed = (nowMs - root.interpolationAnchorMs) / 1000.0
            const duration = Math.max(root.uiBridge.durationSeconds, 0)
            let next = root.interpolationAnchorPosition + elapsed
            if (duration > 0) {
                next = Math.min(next, duration)
            }
            root.displayedPositionSeconds = Math.max(0, next)
        }
    }

    function windowTitleContext() {
        const playbackState = (root.uiBridge.playbackState || "").trim()
        if (playbackState === "Stopped") {
            return ""
        }
        const explicitTitle = (root.uiBridge.currentTrackTitle || "").trim()
        if (explicitTitle.length > 0) {
            return explicitTitle
        }
        const trackPath = (root.uiBridge.currentTrackPath || "").trim()
        if (trackPath.length > 0) {
            return FormatUtils.basenameFromPath(trackPath)
        }
        return ""
    }

    function normalizedVolumeValue(value) {
        const numericValue = Number(value)
        if (!isFinite(numericValue)) {
            return 0.0
        }
        return Math.max(0.0, Math.min(1.0, numericValue))
    }

    function syncMutedVolumeState() {
        const currentVolume = root.normalizedVolumeValue(root.uiBridge.volume)
        if (currentVolume > 0.0001) {
            root.rememberedVolumeBeforeMute = currentVolume
            root.volumeMuted = false
        } else if (!root.volumeMuted && root.rememberedVolumeBeforeMute <= 0.0001) {
            root.rememberedVolumeBeforeMute = 1.0
        }
    }

    function setAppVolume(value) {
        const nextVolume = root.normalizedVolumeValue(value)
        if (nextVolume > 0.0001) {
            root.rememberedVolumeBeforeMute = nextVolume
            root.volumeMuted = false
        } else if (!root.volumeMuted) {
            const currentVolume = root.normalizedVolumeValue(root.uiBridge.volume)
            if (currentVolume > 0.0001) {
                root.rememberedVolumeBeforeMute = currentVolume
            }
        }
        root.uiBridge.setVolume(nextVolume)
    }

    function toggleMutedVolume() {
        const currentVolume = root.normalizedVolumeValue(root.uiBridge.volume)
        if (root.volumeMuted || currentVolume <= 0.0001) {
            const restoreVolume = root.rememberedVolumeBeforeMute > 0.0001
                ? root.rememberedVolumeBeforeMute
                : 1.0
            root.volumeMuted = false
            root.uiBridge.setVolume(restoreVolume)
            return
        }

        root.rememberedVolumeBeforeMute = currentVolume
        root.volumeMuted = true
        root.uiBridge.setVolume(0.0)
    }

    function togglePlayPause() {
        const state = root.uiBridge.playbackState || ""
        if (state === "Playing") {
            root.uiBridge.pause()
        } else {
            root.uiBridge.play()
        }
    }

    function seekCommitted(value) {
        root.interpolationActive = false
        root.displayedPositionSeconds = value
        root.interpolationAnchorPosition = value
        root.interpolationAnchorMs = Date.now()
        root.positionSmoothingPrimed = true
        root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        root.uiBridge.seek(value)
    }

    function shouldResetSpectrogramForStoppedTrackSwitch(previousPlaybackState, currentPlaybackState, stoppedTrackPath, currentTrackPath) {
        const previousState = previousPlaybackState || ""
        const currentState = currentPlaybackState || ""
        const stoppedPath = stoppedTrackPath || ""
        const currentPath = currentTrackPath || ""
        return currentState === "Playing"
            && previousState === "Stopped"
            && stoppedPath.length > 0
            && stoppedPath !== currentPath
    }

    function handleSnapshotChanged(haltSpectrogram, resetSpectrogram) {
        const stopped = (root.uiBridge.playbackState || "") === "Stopped"
        const currentTrackPath = root.uiBridge.currentTrackPath || ""
        if (stopped) {
            const stoppedTrackChanged = root.stoppedSpectrogramTrackPath.length > 0
                && root.stoppedSpectrogramTrackPath !== currentTrackPath
            if (stoppedTrackChanged) {
                resetSpectrogram(true)
            } else {
                haltSpectrogram()
            }
            root.stoppedSpectrogramTrackPath = currentTrackPath
        } else {
            root.stoppedSpectrogramTrackPath = currentTrackPath
        }
        root.syncMutedVolumeState()
    }

    function handlePlaybackChanged(haltSpectrogram, resetSpectrogram) {
        const playbackState = root.uiBridge.playbackState || ""
        if (root.shouldResetSpectrogramForStoppedTrackSwitch(
                    root.lastSpectrogramPlaybackState,
                    playbackState,
                    root.stoppedSpectrogramTrackPath,
                    root.uiBridge.currentTrackPath || "")) {
            resetSpectrogram(true)
            root.stoppedSpectrogramTrackPath = root.uiBridge.currentTrackPath || ""
        }

        const incomingPosition = root.uiBridge.positionSeconds
        const trackChanged = root.positionSmoothingTrackPath !== root.uiBridge.currentTrackPath
        const nowMs = Date.now()

        root.spectrogramPositionSeconds = incomingPosition

        if (playbackState !== "Playing") {
            if (playbackState === "Stopped") {
                haltSpectrogram()
            }
            root.interpolationActive = false
            root.displayedPositionSeconds = incomingPosition
            root.interpolationAnchorPosition = incomingPosition
            root.interpolationAnchorMs = nowMs
            root.positionSmoothingPrimed = false
            root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        } else if (!root.positionSmoothingPrimed || trackChanged) {
            // First update or track change: snap to position, start interpolating.
            root.displayedPositionSeconds = incomingPosition
            root.interpolationAnchorPosition = incomingPosition
            root.interpolationAnchorMs = nowMs
            root.interpolationActive = true
            root.positionSmoothingPrimed = true
            root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        } else {
            // Steady-state: update the anchor so the interpolation timer
            // tracks the real position.  The timer is already advancing
            // displayedPositionSeconds smoothly at ~60 fps.
            root.interpolationAnchorPosition = incomingPosition
            root.interpolationAnchorMs = nowMs
            root.interpolationActive = true
        }

        root.lastSpectrogramPlaybackState = playbackState
    }

    function initializeFromBridge() {
        root.displayedPositionSeconds = root.uiBridge.positionSeconds
        root.spectrogramPositionSeconds = root.uiBridge.positionSeconds
        root.syncMutedVolumeState()
        root.positionSmoothingPrimed = root.uiBridge.playbackState === "Playing"
        root.interpolationAnchorPosition = root.uiBridge.positionSeconds
        root.interpolationAnchorMs = Date.now()
        root.interpolationActive = root.uiBridge.playbackState === "Playing"
        root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        root.stoppedSpectrogramTrackPath = root.uiBridge.currentTrackPath || ""
        root.lastSpectrogramPlaybackState = root.uiBridge.playbackState || ""
    }
}
