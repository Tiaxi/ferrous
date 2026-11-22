// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiagnosticsLogLayout.h"

#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace {

constexpr char16_t kZeroWidthJoiner = 0x200d;
// Qt Quick's text edit's tab stops.
constexpr qreal kTabStop = 80;
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

QString asPlainTextEditHasIt(const QString &text) {
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t c = text.at(i).unicode();
        if (isOtherBreak(c)) {
            if (c == u'\r' && i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
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

// The code point at `index` of `text`, and the code units it has.
char32_t codePointAt(QStringView text, qsizetype index, int *units) {
    const QChar first = text.at(index);
    if (first.isHighSurrogate() && index + 1 < text.size() && text.at(index + 1).isLowSurrogate()) {
        *units = 2;
        return QChar::surrogateToUcs4(first, text.at(index + 1));
    }
    *units = 1;
    return first.unicode();
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

void DiagnosticsLogLayout::setText(const QString &text) {
    // A closed Diagnostics keeps nothing of the log's.
    if (text.isEmpty()) {
        m_text = QString();
        m_records = std::vector<Record>{Record{0, 0, true}};
        m_rows = std::vector<Row>{Row{0, 0}};
        m_widest = 0;
        return;
    }
    QString taken = text;
    for (;;) {
        m_records.clear();
        bool retake = false;
        bool cells = true;
        int start = 0;
        const char16_t *units = QStringView(taken).utf16();
        const int size = static_cast<int>(taken.size());
        for (int i = 0; i < size; ++i) {
            const char16_t c = units[i];
            if (isPrintableAscii(c)) {
                continue;
            }
            if (c == u'\n') {
                m_records.push_back({start, i - start, cells});
                start = i + 1;
                cells = true;
            } else if (isOtherBreak(c) || c == QChar::Nbsp) {
                retake = true;
                break;
            } else {
                cells = false;
            }
        }
        if (!retake) {
            m_records.push_back({start, size - start, cells});
            break;
        }
        taken = asPlainTextEditHasIt(taken);
    }
    m_text = std::move(taken);
}

void DiagnosticsLogLayout::setFont(const QFont &font, bool designMetrics) {
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
    // natural width leaves out spaces at the end.
    return line.cursorToX(static_cast<int>(text.size()));
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

qreal DiagnosticsLogLayout::recordWidth(const Record &record) const {
    if (record.cells && m_fixedPitch) {
        return record.length * m_cell;
    }
    const QStringView text = QStringView(m_text).mid(record.start, record.length);
    qreal x = 0;
    for (qsizetype i = 0; i < text.size();) {
        int units = 1;
        const char32_t c = codePointAt(text, i, &units);
        if (c == U'\t') {
            x = (std::floor(x / kTabStop) + 1) * kTabStop;
        } else if (i == 0 || text.at(i - 1) != QChar(kZeroWidthJoiner)) {
            x += advance(c);
        }
        i += units;
    }
    return x;
}

void DiagnosticsLogLayout::layOut() {
    m_rows.clear();
    m_rows.reserve(m_records.size());
    m_widest = 0;
    for (const Record &record : m_records) {
        m_rows.push_back({record.start, record.length});
        m_widest = std::max(m_widest, recordWidth(record));
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
