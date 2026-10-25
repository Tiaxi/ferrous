.pragma library

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

const categoryLabels = [
    "System",
    "Playback",
    "Library",
    "Waveform and spectrogram",
    "Tags",
    "Last.fm",
    "Settings and saving"
]

function categoryLabel(category) {
    return categoryLabels[category] || categoryLabels[CategorySystem]
}

function countSuffix(count) {
    return count > 1 ? " (×" + count + ")" : ""
}

// The heading and message of an entry, for one line of text.
function describe(entry) {
    if (!entry) {
        return ""
    }
    return categoryLabel(entry.category) + ": " + entry.message + countSuffix(entry.count)
}

function timeText(ms) {
    return Qt.formatTime(new Date(ms), "hh:mm:ss")
}
