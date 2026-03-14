.pragma library

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
        return (value / 1000).toFixed(value % 1000 === 0 ? 0 : 1) + " kHz"
    }
    return Math.round(value).toString() + " Hz"
}

function formatBitDepthSampleRateText(bitDepth, sampleRateHz) {
    const parts = []
    const bitDepthValue = Number(bitDepth)
    if (isFinite(bitDepthValue) && bitDepthValue > 0) {
        parts.push(Math.round(bitDepthValue).toString() + "-bit")
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
        return "Repeat one"
    case 2:
        return "Repeat all"
    default:
        return "Repeat off"
    }
}
