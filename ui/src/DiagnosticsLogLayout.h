// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QString>
#include <QStringView>
#include <QTextLayout>

#include <memory>
#include <vector>

// The diagnostics log's text as the rows of DiagnosticsLogView. A text
// edit lays out each of the log's records in one document, a quarter of a
// second for a full log (#724). Here the rows follow from the characters'
// advances, all of them in a few milliseconds, and Qt's text layout is only
// asked for the rows that show.
//
// An advance is the cell of the monospaced font for printable ASCII, and
// for any other character is measured once, by itself.
class DiagnosticsLogLayout {
public:
    struct Row {
        int start;
        int length;
    };
    // The selected part of a row, from the row's start, and its colours.
    struct Highlight {
        int from = 0;
        int count = 0;
        QColor text;
        QColor band;
    };

    DiagnosticsLogLayout();
    ~DiagnosticsLogLayout();
    DiagnosticsLogLayout(const DiagnosticsLogLayout &) = delete;
    DiagnosticsLogLayout &operator=(const DiagnosticsLogLayout &) = delete;

    // Takes `text` as a plain text edit does, which is also how it copies:
    // each kind of line break is a newline and a no-break space is a space.
    void setText(const QString &text);
    const QString &text() const { return m_text; }
    int length() const { return static_cast<int>(m_text.size()); }

    // `designMetrics` as the text's render type draws it.
    void setFont(const QFont &font, bool designMetrics);

    // Places every row, for the text and the font it has.
    void layOut();
    int rowCount() const { return static_cast<int>(m_rows.size()); }
    Row row(int index) const { return m_rows[static_cast<std::size_t>(index)]; }
    // The row that shows `position`.
    int rowAt(int position) const;
    qreal rowHeight() const { return m_rowHeight; }
    qreal cellWidth() const { return m_cell; }
    qreal widestRow() const { return m_widest; }

    // Qt's layout of one row, its baseline on the rows' own, to draw it and
    // to place the pointer and the selection in it.
    std::unique_ptr<QTextLayout> shape(int row) const { return shape(row, Highlight()); }
    // The selection is a format of the layout's: QSGTextNode's own selection
    // takes one character more than the count it is given.
    std::unique_ptr<QTextLayout> shape(int row, const Highlight &highlight) const;

    int recordAt(int position) const;
    int recordStart(int record) const { return m_records[static_cast<std::size_t>(record)].start; }
    int recordEnd(int record) const;
    QStringView recordText(int record) const;

private:
    struct Record {
        int start;
        int length;
        // Printable ASCII alone, each character a cell wide.
        bool cells;
    };

    qreal measure(const QString &text) const;
    qreal advance(char32_t character) const;
    qreal recordWidth(const Record &record) const;

    QString m_text;
    std::vector<Record> m_records;
    std::vector<Row> m_rows;
    QFont m_font;
    bool m_designMetrics = true;
    bool m_fixedPitch = true;
    qreal m_cell = 0;
    qreal m_ascent = 0;
    qreal m_rowHeight = 1;
    qreal m_widest = 0;
    // The advances measured so far for the font: by code unit in the basic
    // plane, where a negative one is still to measure, and the rest by code
    // point.
    mutable std::vector<float> m_advances;
    mutable QHash<char32_t, float> m_otherAdvances;
};
