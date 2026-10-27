.pragma library
.import FerrousUi 1.0 as FerrousUi

// The categories and severities of the errors the UI shows. The values
// mirror ErrorCategory and ErrorSeverity in ui/src/BinaryBridgeCodec.h and
// src/frontend_bridge/errors.rs.
const CategorySystem = 0
const CategoryPlayback = 1
const CategoryLibrary = 2
const CategoryAnalysis = 3
const CategoryTags = 4
const CategoryLastFm = 5
const CategorySettings = 6

const SeverityWarning = 0
const SeverityError = 1

function categoryLabel(category) {
    const i18n = FerrousUi.I18n
    switch (category) {
    case CategoryPlayback:
        return i18n.i18nc("@title error category", "Playback")
    case CategoryLibrary:
        return i18n.i18nc("@title error category", "Library")
    case CategoryAnalysis:
        return i18n.i18nc("@title error category", "Waveform and spectrogram")
    case CategoryTags:
        return i18n.i18nc("@title error category", "Tags")
    case CategoryLastFm:
        return i18n.i18nc("@title error category", "Last.fm")
    case CategorySettings:
        return i18n.i18nc("@title error category", "Settings and saving")
    default:
        return i18n.i18nc("@title error category", "System")
    }
}

function countSuffix(count) {
    return count > 1
        ? FerrousUi.I18n.i18nc("@info how often an error was reported", " (×%1)", count)
        : ""
}

// The heading and message of an entry, for one line of text.
function describe(entry) {
    if (!entry) {
        return ""
    }
    return FerrousUi.I18n.i18nc("@info error category, message, repeat count", "%1: %2%3",
        categoryLabel(entry.category), entry.message, countSuffix(entry.count))
}

function timeText(ms) {
    return Qt.formatTime(new Date(ms), "hh:mm:ss")
}
