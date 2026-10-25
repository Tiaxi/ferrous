// SPDX-License-Identifier: GPL-3.0-or-later

import QtQuick 2.15
import "../logic/FormatUtils.js" as FormatUtils

QtObject {
    id: root

    required property var uiBridge
    property bool visualFeedsEnabled: false

    property string stoppedSpectrogramTrackPath: ""
    property string lastSpectrogramPlaybackState: ""
    property real rememberedVolumeBeforeMute: 1.0
    property bool volumeMuted: false

    // Where playback is. The bridge's clock extrapolates the engine's
    // position samples; everything that shows the position reads it.
    readonly property var playbackClock: root.uiBridge && root.uiBridge.playbackClock
        ? root.uiBridge.playbackClock
        : null
    readonly property real positionSeconds: root.playbackClock ? root.playbackClock.positionSeconds : 0
    readonly property string positionText: root.playbackClock ? root.playbackClock.positionText : "00:00"

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
        // Play starts a stopped track from its beginning, so a stopped
        // track is not seekable.
        if ((root.uiBridge.playbackState || "") === "Stopped") {
            return
        }
        root.uiBridge.seek(value)
    }

    // Play starts a stopped track from its beginning, so a stopped
    // track is not seekable.
    readonly property bool canSeek: (root.uiBridge.playbackState || "") !== "Stopped"
        && (root.uiBridge.durationSeconds || 0) > 0

    function seekBy(deltaSeconds) {
        if (!root.canSeek) {
            return
        }
        const duration = root.uiBridge.durationSeconds || 0
        root.seekCommitted(Math.max(0, Math.min(duration, root.positionSeconds + deltaSeconds)))
    }

    function changeVolumeBy(delta) {
        const base = root.volumeMuted ? 0.0 : root.normalizedVolumeValue(root.uiBridge.volume)
        root.setAppVolume(base + delta)
    }

    function playAt(index) {
        if (index < 0) {
            return
        }
        root.uiBridge.playAt(index)
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
        if (playbackState === "Stopped") {
            haltSpectrogram()
        }
        root.lastSpectrogramPlaybackState = playbackState
    }

    function initializeFromBridge() {
        root.syncMutedVolumeState()
        root.stoppedSpectrogramTrackPath = root.uiBridge.currentTrackPath || ""
        root.lastSpectrogramPlaybackState = root.uiBridge.playbackState || ""
    }
}
