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
    property bool interpolationAwaitingSeekReacquire: false
    property real interpolationSeekPinnedPosition: 0
    property real interpolationRate: 1.0
    property real interpolationLastHeartbeatPosition: 0
    property real interpolationLastHeartbeatMs: 0
    property bool interpolationLastHeartbeatValid: false
    property real interpolationCorrectionBleedRateCurrentSecondsPerSecond: 0.25
    readonly property real interpolationSnapThresholdSeconds: 0.75
    readonly property real interpolationSteadyStateIgnoreThresholdSeconds: 0.06
    readonly property real interpolationSteadyStateTrimThresholdSeconds: 0.06
    readonly property real interpolationSteadyStateTrimCorrectionSeconds: 0.012
    readonly property real interpolationSteadyStateTrimBleedRateSecondsPerSecond: 0.06
    readonly property real interpolationSteadyStateTrimMaxCorrectionDebtSeconds: 0.05
    readonly property real interpolationCorrectionSeconds: 0.06
    readonly property real interpolationSeekCorrectionSeconds: 0.12
    readonly property real interpolationCorrectionBleedRateSecondsPerSecond: 0.25
    readonly property real interpolationMaxCorrectionDebtSeconds: 0.24
    readonly property real interpolationRateAlpha: 0.5
    readonly property real interpolationRateLearnMin: 0.9
    readonly property real interpolationRateLearnMax: 1.1
    readonly property real interpolationRateClampMin: 0.92
    readonly property real interpolationRateClampMax: 1.05
    readonly property real interpolationSeekReacquireEpsilonSeconds: 0.002
    property real interpolationCorrectionDebtSeconds: 0

    // Timer drives local interpolation at ~60 fps.
    property Timer interpolationTimer: Timer {
        interval: 16
        repeat: true
        running: root.interpolationActive && !root.seekPressed
        onTriggered: {
            const clampedNext = root.advanceInterpolationClock(Date.now())
            root.displayedPositionSeconds = clampedNext
            root.spectrogramPositionSeconds = clampedNext
        }
    }

    function clampPositionToDuration(value) {
        const duration = Math.max(root.uiBridge.durationSeconds, 0)
        let next = Math.max(0, value)
        if (duration > 0) {
            next = Math.min(next, duration)
        }
        return next
    }

    function playbackProfileLogsEnabled() {
        return !!(root.uiBridge && root.uiBridge.profileLogsEnabled)
    }

    function logPlaybackProfile(tag, fields) {
        if (!root.playbackProfileLogsEnabled()) {
            return
        }
        console.warn("[qml-playback-profile] " + tag + " " + fields)
    }

    function currentInterpolatedBasePosition(nowMs) {
        if (!root.interpolationActive) {
            return root.clampPositionToDuration(root.displayedPositionSeconds)
        }
        const elapsed = Math.max(0, (nowMs - root.interpolationAnchorMs) / 1000.0)
        return root.clampPositionToDuration(
            root.interpolationAnchorPosition + (elapsed * root.interpolationRate))
    }

    function resetInterpolationState(position, nowMs) {
        const clampedPosition = root.clampPositionToDuration(position)
        root.interpolationAnchorPosition = clampedPosition
        root.interpolationAnchorMs = nowMs
        root.interpolationCorrectionDebtSeconds = 0
        root.interpolationCorrectionBleedRateCurrentSecondsPerSecond =
            root.interpolationCorrectionBleedRateSecondsPerSecond
        root.interpolationRate = 1.0
        root.interpolationLastHeartbeatPosition = clampedPosition
        root.interpolationLastHeartbeatMs = nowMs
        root.interpolationLastHeartbeatValid = true
    }

    function updateInterpolationRate(incomingPosition, nowMs) {
        if (root.interpolationLastHeartbeatValid) {
            const heartbeatElapsedSeconds =
                Math.max(0, (nowMs - root.interpolationLastHeartbeatMs) / 1000.0)
            const heartbeatPositionDelta =
                Math.max(0, incomingPosition - root.interpolationLastHeartbeatPosition)
            if (heartbeatElapsedSeconds >= 0.02) {
                const measuredRate = heartbeatPositionDelta / heartbeatElapsedSeconds
                if (measuredRate >= root.interpolationRateLearnMin
                        && measuredRate <= root.interpolationRateLearnMax) {
                    root.interpolationRate = Math.max(
                        root.interpolationRateClampMin,
                        Math.min(root.interpolationRateClampMax, measuredRate))
                }
            }
        }

        root.interpolationLastHeartbeatPosition = incomingPosition
        root.interpolationLastHeartbeatMs = nowMs
        root.interpolationLastHeartbeatValid = true
    }

    function advanceInterpolationClock(nowMs) {
        if (!root.interpolationActive) {
            return root.clampPositionToDuration(root.displayedPositionSeconds)
        }
        const nextPosition = root.currentInterpolatedBasePosition(nowMs)
        root.interpolationAnchorPosition = nextPosition
        root.interpolationAnchorMs = nowMs
        return nextPosition
    }

    function applyBoundedPlaybackCorrection(incomingPosition, nowMs) {
        const currentDisplayed = root.interpolationActive
            ? root.currentInterpolatedBasePosition(nowMs)
            : root.clampPositionToDuration(root.displayedPositionSeconds)
        const error = incomingPosition - currentDisplayed
        const action = Math.abs(error) >= root.interpolationSnapThresholdSeconds ? "snap" : "follow"
        const debtBefore = root.interpolationCorrectionDebtSeconds
        root.updateInterpolationRate(incomingPosition, nowMs)
        const clampedPosition = root.clampPositionToDuration(incomingPosition)
        root.displayedPositionSeconds = clampedPosition
        root.spectrogramPositionSeconds = clampedPosition
        root.interpolationAnchorPosition = clampedPosition
        root.interpolationAnchorMs = nowMs
        root.interpolationCorrectionDebtSeconds = 0
        root.interpolationCorrectionBleedRateCurrentSecondsPerSecond =
            root.interpolationCorrectionBleedRateSecondsPerSecond
        root.interpolationActive = true
        if (Math.abs(error) >= 0.001 || action !== "follow") {
            root.logPlaybackProfile(
                "heartbeat",
                "incoming=" + incomingPosition.toFixed(3)
                    + " displayed=" + currentDisplayed.toFixed(3)
                    + " error_ms=" + Math.round(error * 1000)
                    + " action=" + action
                    + " rate=" + root.interpolationRate.toFixed(4)
                    + " correction_ms=0"
                    + " debt_before_ms=" + Math.round(debtBefore * 1000)
                    + " debt_after_ms=0")
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
        root.interpolationAwaitingSeekReacquire = true
        root.interpolationSeekPinnedPosition = value
        root.displayedPositionSeconds = value
        root.resetInterpolationState(value, Date.now())
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

        if (playbackState !== "Playing") {
            if (playbackState === "Stopped") {
                haltSpectrogram()
            }
            root.interpolationActive = false
            root.interpolationAwaitingSeekReacquire = false
            root.displayedPositionSeconds = incomingPosition
            root.spectrogramPositionSeconds = incomingPosition
            root.resetInterpolationState(incomingPosition, nowMs)
            root.positionSmoothingPrimed = false
            root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        } else if (!root.positionSmoothingPrimed || trackChanged) {
            // First update or track change: snap to position, start interpolating.
            root.displayedPositionSeconds = incomingPosition
            root.spectrogramPositionSeconds = incomingPosition
            root.resetInterpolationState(incomingPosition, nowMs)
            root.interpolationActive = true
            root.interpolationAwaitingSeekReacquire = false
            root.positionSmoothingPrimed = true
            root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        } else if (root.interpolationAwaitingSeekReacquire) {
            const pinnedPosition = root.interpolationSeekPinnedPosition
            const movedFromPinned = Math.abs(incomingPosition - pinnedPosition)
                > root.interpolationSeekReacquireEpsilonSeconds
            if (!movedFromPinned) {
                root.interpolationActive = false
                root.displayedPositionSeconds = pinnedPosition
                root.spectrogramPositionSeconds = pinnedPosition
                root.resetInterpolationState(pinnedPosition, nowMs)
            } else {
                root.interpolationAwaitingSeekReacquire = false
                root.displayedPositionSeconds = incomingPosition
                root.spectrogramPositionSeconds = incomingPosition
                root.resetInterpolationState(incomingPosition, nowMs)
                root.interpolationActive = true
            }
        } else {
            // Steady-state and post-seek recovery: keep the local clock
            // continuous and use backend heartbeats only as bounded
            // re-anchors so coarse samples do not produce visible pulses.
            root.applyBoundedPlaybackCorrection(incomingPosition, nowMs)
        }

        root.lastSpectrogramPlaybackState = playbackState
    }

    function initializeFromBridge() {
        root.displayedPositionSeconds = root.uiBridge.positionSeconds
        root.spectrogramPositionSeconds = root.uiBridge.positionSeconds
        root.syncMutedVolumeState()
        root.positionSmoothingPrimed = root.uiBridge.playbackState === "Playing"
        root.resetInterpolationState(root.uiBridge.positionSeconds, Date.now())
        root.interpolationActive = root.uiBridge.playbackState === "Playing"
        root.interpolationAwaitingSeekReacquire = false
        root.interpolationSeekPinnedPosition = root.uiBridge.positionSeconds
        root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        root.stoppedSpectrogramTrackPath = root.uiBridge.currentTrackPath || ""
        root.lastSpectrogramPlaybackState = root.uiBridge.playbackState || ""
    }
}
