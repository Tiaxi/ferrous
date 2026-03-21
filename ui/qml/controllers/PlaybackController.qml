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
    property int positionSmoothingAnimationMs: 0
    property real positionSmoothingLastMs: 0
    property string positionSmoothingTrackPath: ""
    property string stoppedSpectrogramTrackPath: ""
    property string lastSpectrogramPlaybackState: ""
    property real rememberedVolumeBeforeMute: 1.0
    property bool volumeMuted: false

    Behavior on displayedPositionSeconds {
        enabled: root.positionSmoothingAnimationMs > 0
            && !root.seekPressed
            && root.visualFeedsEnabled
        NumberAnimation {
            duration: root.positionSmoothingAnimationMs
            easing.type: Easing.Linear
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
        root.positionSmoothingAnimationMs = 0
        root.displayedPositionSeconds = value
        root.positionSmoothingPrimed = true
        root.positionSmoothingAnchorSeconds = value
        root.positionSmoothingLastMs = Date.now()
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
        const duration = Math.max(root.uiBridge.durationSeconds, 0)

        if (playbackState !== "Playing") {
            if (playbackState === "Stopped") {
                haltSpectrogram()
            }
            root.positionSmoothingAnimationMs = 0
            root.displayedPositionSeconds = incomingPosition
            root.spectrogramPositionSeconds = incomingPosition
            root.positionSmoothingPrimed = false
            root.positionSmoothingAnchorSeconds = incomingPosition
            root.positionSmoothingLastMs = nowMs
            root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        } else if (!root.positionSmoothingPrimed || trackChanged) {
            root.positionSmoothingAnimationMs = 0
            root.displayedPositionSeconds = incomingPosition
            root.spectrogramPositionSeconds = incomingPosition
            root.positionSmoothingPrimed = true
            root.positionSmoothingAnchorSeconds = incomingPosition
            root.positionSmoothingLastMs = nowMs
            root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        } else {
            const cadenceMs = root.positionSmoothingLastMs > 0
                ? Math.max(120, Math.min(1200, nowMs - root.positionSmoothingLastMs))
                : 1000
            const drift = incomingPosition - root.displayedPositionSeconds
            root.spectrogramPositionSeconds = incomingPosition
            if (Math.abs(drift) > 0.20) {
                root.positionSmoothingAnimationMs = 0
                root.displayedPositionSeconds = incomingPosition
            } else {
                root.positionSmoothingAnimationMs = cadenceMs
                const predictedTarget = incomingPosition + (cadenceMs / 1000.0)
                const nextPosition = duration > 0
                    ? Math.min(duration, Math.max(0.0, predictedTarget))
                    : Math.max(0.0, predictedTarget)
                root.displayedPositionSeconds = nextPosition
            }
            root.positionSmoothingAnchorSeconds = incomingPosition
            root.positionSmoothingLastMs = nowMs
            root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        }

        root.lastSpectrogramPlaybackState = playbackState
    }

    function initializeFromBridge() {
        root.displayedPositionSeconds = root.uiBridge.positionSeconds
        root.spectrogramPositionSeconds = root.uiBridge.positionSeconds
        root.syncMutedVolumeState()
        root.positionSmoothingPrimed = root.uiBridge.playbackState === "Playing"
        root.positionSmoothingAnchorSeconds = root.uiBridge.positionSeconds
        root.positionSmoothingAnimationMs = 0
        root.positionSmoothingLastMs = Date.now()
        root.positionSmoothingTrackPath = root.uiBridge.currentTrackPath
        root.stoppedSpectrogramTrackPath = root.uiBridge.currentTrackPath || ""
        root.lastSpectrogramPlaybackState = root.uiBridge.playbackState || ""
    }
}
