// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QBasicTimer>
#include <QColor>
#include <QElapsedTimer>
#include <QFont>
#include <QQuickItem>
#include <QRectF>
#include <QString>
#include <QTextLayout>

#include <map>
#include <memory>

#include "DiagnosticsLogLayout.h"

// The diagnostics log, read-only text that selects and copies as a text
// edit's does, however long it is. Its records wrap to its width (#735),
// which adds nothing to the text. It is the size of its view and draws the
// rows that show at contentY: the Flickable it lies over does the
// scrolling, with content as high as contentHeight, and follows
// scrollRequested.
//
// Positions are those of text(), which is also what it copies.
class DiagnosticsLogView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged FINAL)
    Q_PROPERTY(int length READ length NOTIFY textChanged FINAL)
    Q_PROPERTY(QFont font READ font WRITE setFont NOTIFY fontChanged FINAL)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(QColor selectedTextColor READ selectedTextColor WRITE setSelectedTextColor NOTIFY colorsChanged FINAL)
    Q_PROPERTY(qreal leftPadding READ leftPadding WRITE setLeftPadding NOTIFY paddingChanged FINAL)
    Q_PROPERTY(qreal rightPadding READ rightPadding WRITE setRightPadding NOTIFY paddingChanged FINAL)
    Q_PROPERTY(qreal topPadding READ topPadding WRITE setTopPadding NOTIFY paddingChanged FINAL)
    Q_PROPERTY(qreal bottomPadding READ bottomPadding WRITE setBottomPadding NOTIFY paddingChanged FINAL)
    Q_PROPERTY(qreal contentY READ contentY WRITE setContentY NOTIFY contentYChanged FINAL)
    Q_PROPERTY(qreal contentHeight READ contentHeight NOTIFY contentHeightChanged FINAL)
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE moveCaretTo NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int selectionStart READ selectionStart NOTIFY selectionChanged FINAL)
    Q_PROPERTY(int selectionEnd READ selectionEnd NOTIFY selectionChanged FINAL)
    // For bindings: selectedText would copy up to the whole log for each.
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged FINAL)
    Q_PROPERTY(QString selectedText READ selectedText NOTIFY selectionChanged FINAL)
    Q_PROPERTY(bool readOnly READ readOnly CONSTANT FINAL)

public:
    explicit DiagnosticsLogView(QQuickItem *parent = nullptr);
    ~DiagnosticsLogView() override;

    QString text() const { return m_layout.text(); }
    void setText(const QString &text);
    // Adds `text` as records after the last, on lines of their own, and
    // keeps the last `keep` records, or all of them for 0. Unlike setText,
    // it keeps the place in the log and the selection with their text, and
    // a place at the end of a log that scrolls at its end; it lays out and
    // shapes nothing that was there.
    Q_INVOKABLE void appendRecords(const QString &text, int keep = 0);
    int length() const { return m_layout.length(); }

    QFont font() const { return m_font; }
    void setFont(const QFont &font);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    QColor selectionColor() const { return m_selectionColor; }
    void setSelectionColor(const QColor &color);
    QColor selectedTextColor() const { return m_selectedTextColor; }
    void setSelectedTextColor(const QColor &color);

    qreal leftPadding() const { return m_leftPadding; }
    void setLeftPadding(qreal padding);
    qreal rightPadding() const { return m_rightPadding; }
    void setRightPadding(qreal padding);
    qreal topPadding() const { return m_topPadding; }
    void setTopPadding(qreal padding);
    qreal bottomPadding() const { return m_bottomPadding; }
    void setBottomPadding(qreal padding);

    qreal contentY() const { return m_contentY; }
    void setContentY(qreal y);
    qreal contentHeight() const { return m_contentHeight; }

    int cursorPosition() const { return m_caret; }
    int selectionStart() const;
    int selectionEnd() const;
    bool hasSelection() const { return m_anchor != m_caret; }
    QString selectedText() const;

    bool readOnly() const { return true; }

    Q_INVOKABLE void select(int start, int end);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void copy() const;
    Q_INVOKABLE QString getText(int start, int end) const;
    // In the item's coordinates, outside it for text that is scrolled away.
    Q_INVOKABLE QRectF positionToRectangle(int position);
    Q_INVOKABLE int positionAt(qreal x, qreal y);

    // For assistive technology, which sets cursorPosition.
    void moveCaretTo(int position);
    DiagnosticsLogLayout &laidOut();

    // What the view has cost so far, which the tests bound: how often every
    // row was placed, and how many rows Qt's text layout was asked for to
    // show them.
    int layoutPasses() const { return m_layoutPasses; }
    int shapedRows() const { return m_shapedRows; }

signals:
    void textChanged();
    void fontChanged();
    void colorsChanged();
    void paddingChanged();
    void contentYChanged();
    void contentHeightChanged();
    void selectionChanged();
    void scrollRequested(qreal y);

protected:
    bool event(QEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void updatePolish() override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseUngrabEvent() override;
    void keyPressEvent(QKeyEvent *event) override;
    void timerEvent(QTimerEvent *event) override;

private:
    // What a drag selects by: the click that began it says.
    enum class SelectBy { Character, Word, Record };
    struct Span {
        int start;
        int end;
    };
    // What a resize keeps where it is: the first character of the top row,
    // or the log's end.
    struct Held {
        int position = -1;
        qreal intoRow = 0;
        bool end = false;
    };
    // A row that shows, as Qt lays it out with what of it is selected. The
    // serial tells the scene graph of another layout.
    struct ShownRow {
        std::unique_ptr<QTextLayout> layout;
        int selectedFrom = 0;
        int selectedCount = 0;
        int serial = 0;
    };

    void invalidateLayout();
    void invalidateDrawing();
    void ensureLayout();
    void updateContentHeight();
    bool handlesKey(const QKeyEvent *event) const;
    bool scrollForKey(const QKeyEvent *event);
    void requestScroll(qreal y);
    void holdPlace(qreal viewHeight);
    void reveal(int position);
    void setSelection(int anchor, int caret);
    void setPrimarySelection() const;
    Span unitAt(int position, SelectBy by) const;
    void extendDragTo(const QPointF &point);
    void endDrag();
    qreal xOf(int row, int position) const;
    int positionInRow(int row, qreal x) const;

    DiagnosticsLogLayout m_layout;
    QString m_source;
    QFont m_font;
    QColor m_color{Qt::black};
    QColor m_selectionColor{Qt::blue};
    QColor m_selectedTextColor{Qt::white};
    qreal m_leftPadding = 0;
    qreal m_rightPadding = 0;
    qreal m_topPadding = 0;
    qreal m_bottomPadding = 0;
    qreal m_contentY = 0;
    qreal m_contentHeight = 0;
    // Kept through a drag of the window's edge, until the log scrolls, so
    // that the steps don't add up to a drift.
    Held m_held;

    bool m_fontDirty = true;
    bool m_layoutDirty = true;
    std::map<int, ShownRow> m_shown;
    int m_layoutPasses = 0;
    int m_shapedRows = 0;

    int m_anchor = 0;
    int m_caret = 0;
    SelectBy m_selectBy = SelectBy::Character;
    Span m_dragOrigin{0, 0};
    bool m_dragging = false;
    QPointF m_dragPoint;
    QBasicTimer m_dragScroll;
    QElapsedTimer m_sinceDoubleClick;
    QPointF m_doubleClickPoint;
};
