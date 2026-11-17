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

// Where a message's first sentence ends, when it has more than one and the
// first is short enough for a title: the index of its full stop, or -1.
function firstSentenceEnd(text) {
    const maximumTitleLength = 90
    for (let i = 1; i < Math.min(text.length - 2, maximumTitleLength); ++i) {
        const mark = text.charAt(i)
        if (mark !== "." && mark !== "!" && mark !== "?") {
            continue
        }
        if (text.charAt(i + 1) !== " ") {
            continue
        }
        // A sentence starts with anything but a lower-case letter.
        const next = text.charAt(i + 2)
        if (next === next.toUpperCase() || next !== next.toLowerCase()) {
            return i
        }
    }
    return -1
}

// The title of an entry in Recent Issues, which names the problem: the
// first sentence of a message of several ("04 - Static.flac ends early"),
// or else the whole message; with how often it was reported. Its category
// is a label of its own, above it.
function issueTitle(entry) {
    if (!entry) {
        return ""
    }
    const text = String(entry.message || "")
    const end = firstSentenceEnd(text)
    const title = end < 0
        ? text
        : text.slice(0, text.charAt(end) === "." ? end : end + 1)
    return title + countSuffix(entry.count)
}

// The rest of the message, under the title, if it has more than one
// sentence.
function issueDescription(entry) {
    if (!entry) {
        return ""
    }
    const text = String(entry.message || "")
    const end = firstSentenceEnd(text)
    return end < 0 ? "" : text.slice(end + 2)
}

// The time of an entry, "23:41", with its date when it is not today's.
function issueTimeText(ms, nowMs) {
    const date = new Date(ms)
    const now = new Date(nowMs === undefined ? Date.now() : nowMs)
    const time = Qt.formatTime(date, "hh:mm")
    return date.toDateString() === now.toDateString()
        ? time
        : FerrousUi.I18n.i18nc("@info date, time", "%1 %2",
            Qt.formatDate(date, Qt.locale().dateFormat(1 /* Locale.ShortFormat */)), time)
}

// The glyph of an entry: Last.fm's waiting scrobbles and the library's
// notices are quiet; anything else that went wrong is a warning in the
// negative colour.
const GlyphWarning = 0
const GlyphOffline = 1
const GlyphInformation = 2

function issueGlyph(category, severity) {
    if (severity === SeverityWarning && category === CategoryLastFm) {
        return GlyphOffline
    }
    if (severity === SeverityWarning && category === CategoryLibrary) {
        return GlyphInformation
    }
    return GlyphWarning
}
