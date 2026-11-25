.pragma library
.import FerrousUi 1.0 as FerrousUi

// What stands between the fields of a line: the headers' totals and the
// status bar's format ("FLAC · 16-bit / 44.1 kHz"). A var: a const is not
// visible to the files that import this one.
var fieldSeparator = " \u00b7 "

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

// A time of the clock texts ("01:17", "1:02:03") as the transport and the
// status bar show it, without the minutes' leading zero: "1:17".
function clockText(text) {
    return String(text || "").replace(/^0(\d:)/, "$1")
}

// `seconds` as the transport shows a time: "1:17", "1:02:03".
function formatClockTime(seconds) {
    return clockText(formatSeekTime(seconds))
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

// A gain in dB to one decimal, with a true minus sign for a cut and a plus
// for a boost: "−6.4 dB", "+2.0 dB", "0.0 dB".
function formatGainDb(db) {
    const tenths = Math.round(Number(db) * 10)
    if (!isFinite(tenths)) {
        return ""
    }
    const magnitude = (Math.abs(tenths) / 10).toFixed(1)
    const signed = tenths < 0 ? "\u2212" + magnitude : tenths > 0 ? "+" + magnitude : magnitude
    return FerrousUi.I18n.i18nc("@info gain in decibels", "%1 dB", signed)
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
