.pragma library
.import FerrousUi 1.0 as FerrousUi

function basenameFromPath(pathValue) {
    const normalized = (pathValue || "").trim().replace(/\\/g, "/")
    if (normalized.length === 0) {
        return ""
    }
    const parts = normalized.split("/")
    return parts.length > 0 ? parts[parts.length - 1] : normalized
}

function formatSeekTime(seconds) {
    if (!isFinite(seconds) || seconds < 0) {
        return "00:00"
    }
    const totalSeconds = Math.floor(seconds)
    const hours = Math.floor(totalSeconds / 3600)
    const minutes = Math.floor((totalSeconds % 3600) / 60)
    const secs = totalSeconds % 60
    if (hours > 0) {
        return hours.toString()
            + ":" + minutes.toString().padStart(2, "0")
            + ":" + secs.toString().padStart(2, "0")
    }
    return minutes.toString().padStart(2, "0")
        + ":" + secs.toString().padStart(2, "0")
}

function metadataTrackNumberText(trackNumber) {
    if (trackNumber === undefined || trackNumber === null) {
        return "--"
    }
    const value = Number(trackNumber)
    if (!isFinite(value) || value <= 0) {
        return "--"
    }
    return Math.floor(value).toString().padStart(2, "0")
}

// `text` as shown, without the "&" that marks its mnemonic.
function withoutMnemonic(text) {
    return String(text || "").replace(/&(.)/g, "$1")
}

function playlistOrderText(index) {
    if (index === undefined || index === null || index < 0) {
        return "--"
    }
    return String(index + 1)
}

function formatSampleRateText(sampleRateHz) {
    const value = Number(sampleRateHz)
    if (!isFinite(value) || value <= 0) {
        return ""
    }
    if (value >= 1000) {
        return FerrousUi.I18n.i18nc("@info sample rate", "%1 kHz",
            (value / 1000).toFixed(value % 1000 === 0 ? 0 : 1))
    }
    return FerrousUi.I18n.i18nc("@info sample rate", "%1 Hz", Math.round(value).toString())
}

function formatBitDepthSampleRateText(bitDepth, sampleRateHz) {
    const parts = []
    const bitDepthValue = Number(bitDepth)
    if (isFinite(bitDepthValue) && bitDepthValue > 0) {
        parts.push(FerrousUi.I18n.i18nc("@info bit depth", "%1-bit", Math.round(bitDepthValue).toString()))
    }
    const sampleRateText = formatSampleRateText(sampleRateHz)
    if (sampleRateText.length > 0) {
        parts.push(sampleRateText)
    }
    return parts.join(" / ")
}

function repeatModeText(mode) {
    switch (Number(mode)) {
    case 1:
        return FerrousUi.I18n.i18nc("@info:status", "Repeat one")
    case 2:
        return FerrousUi.I18n.i18nc("@info:status", "Repeat all")
    default:
        return FerrousUi.I18n.i18nc("@info:status", "Repeat off")
    }
}
