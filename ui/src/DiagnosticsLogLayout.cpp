// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiagnosticsLogLayout.h"

#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace {

constexpr char16_t kZeroWidthJoiner = 0x200d;
// Qt Quick's text edit's tab stops.
constexpr qreal kTabStop = 80;
// How far a record's further rows stand in from its first.
constexpr int kContinuationCells = 4;
// Nothing wraps tighter, so that the widths an item passes through before
// it has its own don't make a row of each character. The window's minimum
// is several times this.
constexpr int kFewestCells = 16;
// What a sum of advances may be off by at the edge.
constexpr qreal kRoundingSlack = 0.01;
// A row is laid out in one line, however long.
constexpr qreal kUnboundedWidth = 1 << 23;

bool isPrintableAscii(char16_t c) {
    return c >= 0x20 && c < 0x7f;
}

// What a plain text edit takes as a line break, besides the newline.
bool isOtherBreak(char16_t c) {
    return c == u'\r' || c == QChar::LineSeparator || c == QChar::ParagraphSeparator || c == 0xfdd0
        || c == 0xfdd1;
}

// With the newlines that stand for another line break, inside a line of
// the text, in `continuations`.
QString asPlainTextEditHasIt(const QString &text, std::vector<int> &continuations) {
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        if (isOtherBreak(c)) {
            if (c == u'\r' && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            } else {
                continuations.push_back(static_cast<int>(out.size()));
            }
            out += QLatin1Char('\n');
        } else if (c == QChar::Nbsp) {
            out += QLatin1Char(' ');
        } else {
            out += QChar(c);
        }
    }
    return out;
}

bool isSpace(char16_t c) {
    return c == u' ' || c == u'\t' || c == 0x3000;
}

// Scripts that break between their characters.
bool breaksBetweenCharacters(char16_t c) {
    return (c >= 0x2e80 && c < 0xa000) || (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff)
        || (c >= 0xff00 && c <= 0xffef);
}

// Whether a row may start at `at`, which is not the start of one.
bool mayBreakBefore(const char16_t *text, int at) {
    const char16_t before = text[at - 1];
    const char16_t after = text[at];
    if (isSpace(after)) {
        return false;
    }
    if (isSpace(before)) {
        return true;
    }
    const char16_t twoBefore = at >= 2 ? text[at - 2] : u'\n';
    if (before == u'/') {
        // After a folder, not after what opens a path or a URL's slashes.
        constexpr std::u16string_view opens = u"\n \t\"'([<=:/\u201c";
        return after != u'/' && opens.find(twoBefore) == std::u16string_view::npos;
    }
    if (before == u'-') {
        return after != u'-' && !(after >= u'0' && after <= u'9') && twoBefore != u'-' && !isSpace(twoBefore);
    }
    if (!breaksBetweenCharacters(before) && !breaksBetweenCharacters(after)) {
        return false;
    }
    switch (QChar::category(char32_t(after))) {
    case QChar::Punctuation_Close:
    case QChar::Punctuation_FinalQuote:
    case QChar::Punctuation_Other:
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
    case QChar::Other_Format:
        return false;
    default:
        break;
    }
    const QChar::Category opening = QChar::category(char32_t(before));
    return opening != QChar::Punctuation_Open && opening != QChar::Punctuation_InitialQuote
        && before != kZeroWidthJoiner && !QChar::isLowSurrogate(after);
}

// Whether a name too long for its row may go on at `at`, as a setting's
// after its = or between its words.
bool mayBreakNameBefore(const char16_t *text, int at) {
    const char16_t before = text[at - 1];
    return (before == u'=' || before == u'_' || before == u'.' || before == u',') && text[at] != before
        && !isSpace(text[at]);
}

QTextOption rowOption(bool designMetrics) {
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTextDirection(Qt::LeftToRight);
    option.setTabStopDistance(kTabStop);
    option.setUseDesignMetrics(designMetrics);
    return option;
}

} // namespace

DiagnosticsLogLayout::DiagnosticsLogLayout() {
    setText({});
}

DiagnosticsLogLayout::~DiagnosticsLogLayout() = default;

bool DiagnosticsLogLayout::findRecords(const QString &text, int base, const std::vector<int> &continuations,
                                       std::vector<Record> &records) {
    const std::size_t before = records.size();
    bool cells = true;
    bool continues = false;
    auto continuation = continuations.begin();
    int start = 0;
    const char16_t *units = QStringView(text).utf16();
    const int size = static_cast<int>(text.size());
    for (int i = 0; i < size; ++i) {
        const char16_t c = units[i];
        if (isPrintableAscii(c)) {
            continue;
        }
        if (c == u'\n') {
            records.push_back({base + start, i - start, cells, continues});
            start = i + 1;
            cells = true;
            continues = continuation != continuations.end() && *continuation == i;
            if (continues) {
                ++continuation;
            }
        } else if (isOtherBreak(c) || c == QChar::Nbsp) {
            records.resize(before);
            return false;
        } else {
            cells = false;
        }
    }
    records.push_back({base + start, size - start, cells, continues});
    return true;
}

QString DiagnosticsLogLayout::takeRecords(const QString &text, int base, std::vector<Record> &records) {
    if (findRecords(text, base, {}, records)) {
        return text;
    }
    std::vector<int> continuations;
    QString taken = asPlainTextEditHasIt(text, continuations);
    findRecords(taken, base, continuations, records);
    return taken;
}

void DiagnosticsLogLayout::setText(const QString &text) {
    m_laidOut = false;
    // A closed Diagnostics keeps nothing of the log's.
    if (text.isEmpty()) {
        m_text = QString();
        m_records = std::vector<Record>{Record{0, 0, true, false}};
        m_rows = std::vector<Row>{Row{0, 0, false}};
        return;
    }
    m_records.clear();
    m_text = takeRecords(text, 0, m_records);
}

void DiagnosticsLogLayout::append(const QString &text) {
    if (m_text.isEmpty()) {
        const bool laidOut = m_laidOut;
        setText(text);
        if (laidOut) {
            layOut(m_firstRowWidth);
        }
        return;
    }
    const std::size_t first = m_records.size();
    const QString taken = takeRecords(text, length() + 1, m_records);
    m_text += QLatin1Char('\n');
    m_text += taken;
    if (m_laidOut) {
        placeRows(first);
    }
}

DiagnosticsLogLayout::Dropped DiagnosticsLogLayout::dropLines(int lines) {
    // The first record of the line that stays.
    int record = 0;
    for (int index = 1, seen = 0; index < recordCount() && seen < lines; ++index) {
        if (!m_records[static_cast<std::size_t>(index)].continues) {
            record = index;
            ++seen;
        }
    }
    if (record == 0) {
        return {};
    }
    Dropped dropped;
    dropped.characters = recordStart(record);
    m_text.remove(0, dropped.characters);
    m_records.erase(m_records.begin(), m_records.begin() + record);
    for (Record &kept : m_records) {
        kept.start -= dropped.characters;
    }
    if (m_laidOut) {
        const auto firstKept = std::lower_bound(m_rows.begin(), m_rows.end(), dropped.characters,
                                                [](const Row &row, int at) { return row.start < at; });
        dropped.rows = static_cast<int>(firstKept - m_rows.begin());
        m_rows.erase(m_rows.begin(), firstKept);
        for (Row &kept : m_rows) {
            kept.start -= dropped.characters;
        }
    }
    return dropped;
}

void DiagnosticsLogLayout::setFont(const QFont &font, bool designMetrics) {
    m_laidOut = false;
    m_font = font;
    m_designMetrics = designMetrics;
    m_advances.clear();
    m_otherAdvances.clear();

    QTextLayout sample(QStringLiteral("Mg"), m_font);
    sample.setTextOption(rowOption(m_designMetrics));
    sample.beginLayout();
    QTextLine line = sample.createLine();
    line.setLineWidth(kUnboundedWidth);
    sample.endLayout();
    m_ascent = line.ascent();
    m_rowHeight = std::max<qreal>(1, std::ceil(line.height()));

    // Over many, so that the cell is exact along a long record.
    constexpr int kSamples = 100;
    const qreal narrow = measure(QString(kSamples, QLatin1Char('i')));
    const qreal wide = measure(QString(kSamples, QLatin1Char('W')));
    m_fixedPitch = std::abs(narrow - wide) < 0.5;
    m_cell = std::max<qreal>(1, wide / kSamples);
}

qreal DiagnosticsLogLayout::measure(const QString &text) const {
    QTextLayout layout(text, m_font);
    layout.setTextOption(rowOption(m_designMetrics));
    layout.beginLayout();
    QTextLine line = layout.createLine();
    line.setLineWidth(kUnboundedWidth);
    layout.endLayout();
    // The natural width leaves out spaces at the end, and the cursor after
    // right-to-left text is at its left.
    return std::max<qreal>(line.naturalTextWidth(), line.cursorToX(static_cast<int>(text.size())));
}

qreal DiagnosticsLogLayout::advance(char32_t character) const {
    if (character < 0x80 && m_fixedPitch && isPrintableAscii(static_cast<char16_t>(character))) {
        return m_cell;
    }
    float *known = nullptr;
    if (character < 0x10000) {
        if (m_advances.empty()) {
            m_advances.assign(0x10000, -1.0F);
        }
        known = &m_advances[character];
        if (*known >= 0) {
            return *known;
        }
    } else if (const auto found = m_otherAdvances.constFind(character); found != m_otherAdvances.cend()) {
        return *found;
    }

    // The ideographs, and the Hangul syllables, are all as wide as the
    // first of them: a log may hold thousands of different ones.
    char32_t measured = character;
    if (character > 0x4e00 && character <= 0x9fff) {
        measured = 0x4e00;
    } else if (character > 0xac00 && character <= 0xd7a3) {
        measured = 0xac00;
    }
    qreal width = 0;
    if (measured != character) {
        width = advance(measured);
    } else {
        // What joins the character before it takes no room of its own.
        const QChar::Category category = QChar::category(character);
        if (category != QChar::Mark_NonSpacing && category != QChar::Mark_Enclosing
            && category != QChar::Other_Format) {
            width = std::max<qreal>(0, measure(QString::fromUcs4(&character, 1)));
        }
    }
    if (known != nullptr) {
        *known = static_cast<float>(width);
    } else {
        m_otherAdvances.insert(character, static_cast<float>(width));
    }
    return width;
}

int DiagnosticsLogLayout::breakBefore(int from, int overflow, int end) const {
    const char16_t *text = QStringView(m_text).utf16();
    if (isSpace(text[overflow])) {
        // The spaces stay at the end of their row, past the edge.
        int next = overflow;
        while (next < end && isSpace(text[next])) {
            ++next;
        }
        return next;
    }
    // Looked for in the second half of the row only: a long name then
    // starts where it is, rather than leave most of the row empty, and a
    // resize looks at less than half of the log.
    const int nearest = from + (overflow - from) / 2;
    for (int at = overflow; at > nearest; --at) {
        if (mayBreakBefore(text, at)) {
            return at;
        }
    }
    for (int at = overflow; at > nearest; --at) {
        if (mayBreakNameBefore(text, at)) {
            return at;
        }
    }
    int at = overflow;
    while (at > from + 1 && QChar::category(char32_t(text[at])) == QChar::Mark_SpacingCombining) {
        --at;
    }
    return at;
}

int DiagnosticsLogLayout::cellBreak(int from, int end, qreal width) const {
    const int cells = std::max(1, static_cast<int>((width + kRoundingSlack) / m_cell));
    return end - from <= cells ? end : breakBefore(from, from + cells, end);
}

int DiagnosticsLogLayout::measuredBreak(int from, int end, qreal width) const {
    const char16_t *text = QStringView(m_text).utf16();
    const qreal edge = width + kRoundingSlack;
    qreal x = 0;
    for (int at = from; at < end;) {
        const char16_t c = text[at];
        int units = 1;
        qreal next = x;
        if (m_fixedPitch && isPrintableAscii(c)) {
            next += m_cell;
        } else if (c == u'\t') {
            next = (std::floor(x / kTabStop) + 1) * kTabStop;
        } else if (at > from && text[at - 1] == kZeroWidthJoiner) {
            // Joined to the one before, in its room.
            units = QChar::isHighSurrogate(c) && at + 1 < end ? 2 : 1;
        } else if (QChar::isHighSurrogate(c) && at + 1 < end && QChar::isLowSurrogate(text[at + 1])) {
            next += advance(QChar::surrogateToUcs4(c, text[at + 1]));
            units = 2;
        } else if (!m_advances.empty() && m_advances[c] >= 0) {
            next += static_cast<qreal>(m_advances[c]);
        } else {
            next += advance(c);
        }
        if (next > edge && next > x && at > from) {
            return breakBefore(from, at, end);
        }
        x = next;
        at += units;
    }
    return end;
}

void DiagnosticsLogLayout::layOut(qreal width) {
    m_rows.clear();
    m_rows.reserve(m_records.size());
    m_firstRowWidth = std::max(width, kFewestCells * m_cell);
    m_indent = kContinuationCells * m_cell;
    m_laidOut = true;
    placeRows(0);
}

void DiagnosticsLogLayout::placeRows(std::size_t fromRecord) {
    const qreal furtherWidth = m_firstRowWidth - m_indent;
    for (std::size_t index = fromRecord; index < m_records.size(); ++index) {
        const Record &record = m_records[index];
        const int end = record.start + record.length;
        const bool byCells = record.cells && m_fixedPitch;
        int from = record.start;
        bool continuation = false;
        for (;;) {
            const qreal room = continuation ? furtherWidth : m_firstRowWidth;
            const int next = byCells ? cellBreak(from, end, room) : measuredBreak(from, end, room);
            m_rows.push_back({from, next - from, continuation});
            if (next >= end) {
                break;
            }
            from = next;
            continuation = true;
        }
    }
}

int DiagnosticsLogLayout::rowAt(int position) const {
    const auto after = std::upper_bound(m_rows.begin(), m_rows.end(), position,
                                        [](int at, const Row &row) { return at < row.start; });
    return std::max(0, static_cast<int>(after - m_rows.begin()) - 1);
}

std::unique_ptr<QTextLayout> DiagnosticsLogLayout::shape(int row, const Highlight &highlight) const {
    const Row span = this->row(row);
    auto layout = std::make_unique<QTextLayout>(m_text.mid(span.start, span.length), m_font);
    layout->setTextOption(rowOption(m_designMetrics));
    if (highlight.count > 0) {
        QTextLayout::FormatRange selected;
        selected.start = highlight.from;
        selected.length = highlight.count;
        selected.format.setForeground(highlight.text);
        selected.format.setBackground(highlight.band);
        layout->setFormats({selected});
    }
    layout->beginLayout();
    QTextLine line = layout->createLine();
    if (line.isValid()) {
        line.setLineWidth(kUnboundedWidth);
        line.setPosition(QPointF(0, m_ascent - line.ascent()));
    }
    layout->endLayout();
    return layout;
}

int DiagnosticsLogLayout::recordAt(int position) const {
    const auto after = std::upper_bound(m_records.begin(), m_records.end(), position,
                                        [](int at, const Record &record) { return at < record.start; });
    return std::max(0, static_cast<int>(after - m_records.begin()) - 1);
}

int DiagnosticsLogLayout::recordEnd(int record) const {
    const Record &span = m_records[static_cast<std::size_t>(record)];
    return span.start + span.length;
}

QStringView DiagnosticsLogLayout::recordText(int record) const {
    return QStringView(m_text).sliced(recordStart(record), recordEnd(record) - recordStart(record));
}
