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
// A record wraps to the width (#735) and goes on indented under its first
// row. It breaks in a row's second half, after a space, a folder of a path
// or an ideograph, failing those inside a name after its punctuation, and
// failing that anywhere.
//
// An advance is the cell of the monospaced font for printable ASCII, and
// for any other character is measured once, by itself.
class DiagnosticsLogLayout {
public:
    struct Row {
        int start;
        int length;
        // A row that goes on with the record of the one before.
        bool continuation;
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

    // Adds `text`'s records after the last, as if a newline came between
    // them, taken as setText takes it. Laid out, it places the new records'
    // rows at the width it has, and the rows before stay as they are.
    void append(const QString &text);
    // Drops the text's first `lines` lines, the records of each, from one
    // newline of the text it was given to the next, and their rows; what
    // comes after moves up by the characters and the rows dropped. It keeps
    // the last line at least.
    struct Dropped {
        int characters = 0;
        int rows = 0;
    };
    Dropped dropLines(int lines);
    int recordCount() const { return static_cast<int>(m_records.size()); }

    // `designMetrics` as the text's render type draws it.
    void setFont(const QFont &font, bool designMetrics);

    // Places every row in `width`, for the text and the font it has.
    void layOut(qreal width);
    int rowCount() const { return static_cast<int>(m_rows.size()); }
    Row row(int index) const { return m_rows[static_cast<std::size_t>(index)]; }
    // The row that shows `position`.
    int rowAt(int position) const;
    qreal rowHeight() const { return m_rowHeight; }
    qreal cellWidth() const { return m_cell; }
    qreal indent(int row) const { return this->row(row).continuation ? m_indent : 0; }

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
        // After another kind of line break, inside a line of the text.
        bool continues;
    };

    // The records of `text`, its first at `base`, where the newlines at
    // `continuations` stand for other line breaks; false, with none, for
    // text that a plain text edit would take otherwise.
    static bool findRecords(const QString &text, int base, const std::vector<int> &continuations,
                            std::vector<Record> &records);
    // Adds the records of `text` as a plain text edit takes it, its first at
    // `base`, and returns the text as taken.
    static QString takeRecords(const QString &text, int base, std::vector<Record> &records);
    void placeRows(std::size_t fromRecord);
    qreal measure(const QString &text) const;
    qreal advance(char32_t character) const;
    int cellBreak(int from, int end, qreal width) const;
    int measuredBreak(int from, int end, qreal width) const;
    int breakBefore(int from, int overflow, int end) const;

    QString m_text;
    std::vector<Record> m_records;
    std::vector<Row> m_rows;
    QFont m_font;
    bool m_designMetrics = true;
    bool m_fixedPitch = true;
    qreal m_cell = 0;
    qreal m_ascent = 0;
    qreal m_rowHeight = 1;
    qreal m_indent = 0;
    // The width a record's first row has, and whether the rows are those
    // of the text and the font, laid out.
    qreal m_firstRowWidth = 0;
    bool m_laidOut = false;
    // The advances measured so far for the font: by code unit in the basic
    // plane, where a negative one is still to measure, and the rest by code
    // point.
    mutable std::vector<float> m_advances;
    mutable QHash<char32_t, float> m_otherAdvances;
};
