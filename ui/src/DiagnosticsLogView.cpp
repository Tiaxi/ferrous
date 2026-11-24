// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiagnosticsLogView.h"

#include <QAccessible>
#include <QClipboard>
#include <QGuiApplication>
#include <QInputDevice>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGClipNode>
#include <QSGGeometry>
#include <QSGTextNode>
#include <QStyleHints>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTimerEvent>

#include <algorithm>
#include <cmath>

namespace {

// A frame's time, while a drag holds the pointer past an edge.
constexpr int kDragScrollIntervalMs = 16;

QSGTextNode::RenderType renderType() {
    switch (QQuickWindow::textRenderType()) {
    case QQuickWindow::NativeTextRendering:
        return QSGTextNode::NativeRendering;
    case QQuickWindow::CurveTextRendering:
        return QSGTextNode::CurveRendering;
    case QQuickWindow::QtTextRendering:
        break;
    }
    return QSGTextNode::QtRendering;
}

// The rows, inside the view's padding.
class LogNode : public QSGClipNode {
public:
    struct DrawnRow {
        QSGTextNode *node = nullptr;
        int serial = 0;
    };

    LogNode()
        : m_corners(QSGGeometry::defaultAttributes_Point2D(), 4) {
        setGeometry(&m_corners);
        setIsRectangular(true);
        setArea(QRectF());
    }

    void setArea(const QRectF &area) {
        QSGGeometry::updateRectGeometry(&m_corners, area);
        setClipRect(area);
        markDirty(DirtyGeometry);
    }

    std::map<int, DrawnRow> rows;

private:
    QSGGeometry m_corners;
};

} // namespace

DiagnosticsLogView::DiagnosticsLogView(QQuickItem *parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents);
    setClip(true);
    setAcceptedMouseButtons(Qt::LeftButton);
    setActiveFocusOnTab(true);
}

DiagnosticsLogView::~DiagnosticsLogView() = default;

void DiagnosticsLogView::setText(const QString &text) {
    if (m_source == text) {
        return;
    }
    m_source = text;
    const bool announces = QAccessible::isActive();
    const QString before = announces ? m_layout.text() : QString();
    endDrag();
    m_held = {};
    m_layout.setText(text);
    // The layouts of the rows that showed are of the text before.
    m_shown.clear();
    invalidateLayout();
    // Placed at once where the size is known, so that the scroll bar is of
    // this text in the first frame that shows it.
    if (width() > 0) {
        ensureLayout();
    }
    const bool selectionMoved = m_anchor != 0 || m_caret != 0;
    m_anchor = 0;
    m_caret = 0;
    emit textChanged();
    if (selectionMoved) {
        emit selectionChanged();
    }
    // New text opens at its start, as in a text edit.
    requestScroll(0);
    if (announces) {
        QAccessibleTextUpdateEvent update(this, 0, before, m_layout.text());
        QAccessible::updateAccessibility(&update);
    }
}

void DiagnosticsLogView::appendRecords(const QString &text, int drop) {
    if (text.isEmpty()) {
        return;
    }
    // Laid out, only the new records' rows are placed.
    if (width() > 0) {
        ensureLayout();
    }
    const bool laidOut = !m_layoutDirty;
    // A reader at the end of a log that scrolls follows it, as in a
    // terminal; anywhere else the rows in view stay where they are.
    const bool atEnd = laidOut && m_contentHeight > height() && m_contentY + height() >= m_contentHeight - 1;
    const bool announces = QAccessible::isActive();
    const int before = length();
    m_layout.append(text);
    if (announces) {
        QAccessibleTextInsertEvent inserted(this, before, m_layout.text().mid(before));
        QAccessible::updateAccessibility(&inserted);
    }
    DiagnosticsLogLayout::Dropped dropped;
    if (drop > 0) {
        const QString shown = announces ? m_layout.text() : QString();
        dropped = m_layout.dropLines(drop);
        if (announces && dropped.characters > 0) {
            QAccessibleTextRemoveEvent removal(this, 0, shown.left(dropped.characters));
            QAccessible::updateAccessibility(&removal);
        }
    }
    // Shares the layout's text: setting it again changes nothing.
    m_source = m_layout.text();

    const auto moved = [&dropped](int position) { return std::max(0, position - dropped.characters); };
    const bool selectionMoved = dropped.characters > 0 && (m_anchor > 0 || m_caret > 0);
    m_anchor = moved(m_anchor);
    m_caret = moved(m_caret);
    m_dragOrigin = {moved(m_dragOrigin.start), moved(m_dragOrigin.end)};
    if (m_held.position >= 0) {
        m_held.position = moved(m_held.position);
    }
    if (dropped.rows > 0) {
        // The rows that show keep their layouts, under their new numbers.
        std::map<int, ShownRow> shown;
        for (auto &[index, row] : m_shown) {
            if (index >= dropped.rows) {
                shown.emplace(index - dropped.rows, std::move(row));
            }
        }
        m_shown = std::move(shown);
    }
    if (laidOut) {
        updateContentHeight();
        const qreal bottom = std::max(0.0, m_contentHeight - height());
        const qreal y = atEnd ? bottom
                              : std::clamp(m_contentY - dropped.rows * m_layout.rowHeight(), 0.0, bottom);
        if (y != m_contentY) {
            requestScroll(y);
        }
    }
    polish();
    update();
    emit textChanged();
    if (selectionMoved) {
        emit selectionChanged();
    }
}

void DiagnosticsLogView::setFont(const QFont &font) {
    if (m_font == font) {
        return;
    }
    m_font = font;
    m_fontDirty = true;
    invalidateLayout();
    emit fontChanged();
}

void DiagnosticsLogView::setColor(const QColor &color) {
    if (m_color != color) {
        m_color = color;
        invalidateDrawing();
        emit colorsChanged();
    }
}

void DiagnosticsLogView::setSelectionColor(const QColor &color) {
    if (m_selectionColor != color) {
        m_selectionColor = color;
        invalidateDrawing();
        emit colorsChanged();
    }
}

void DiagnosticsLogView::setSelectedTextColor(const QColor &color) {
    if (m_selectedTextColor != color) {
        m_selectedTextColor = color;
        invalidateDrawing();
        emit colorsChanged();
    }
}

void DiagnosticsLogView::setLeftPadding(qreal padding) {
    if (m_leftPadding != padding) {
        m_leftPadding = padding;
        invalidateLayout();
        emit paddingChanged();
    }
}

void DiagnosticsLogView::setRightPadding(qreal padding) {
    if (m_rightPadding != padding) {
        m_rightPadding = padding;
        invalidateLayout();
        emit paddingChanged();
    }
}

void DiagnosticsLogView::setTopPadding(qreal padding) {
    if (m_topPadding != padding) {
        m_topPadding = padding;
        invalidateLayout();
        emit paddingChanged();
    }
}

void DiagnosticsLogView::setBottomPadding(qreal padding) {
    if (m_bottomPadding != padding) {
        m_bottomPadding = padding;
        invalidateLayout();
        emit paddingChanged();
    }
}

void DiagnosticsLogView::setContentY(qreal y) {
    if (m_contentY == y) {
        return;
    }
    m_contentY = y;
    m_held = {};
    polish();
    update();
    emit contentYChanged();
    // A wheel turned during a drag takes the selection with it.
    if (m_dragging && !m_dragScroll.isActive()) {
        extendDragTo(m_dragPoint);
    }
}

void DiagnosticsLogView::invalidateLayout() {
    m_layoutDirty = true;
    polish();
    update();
}

void DiagnosticsLogView::invalidateDrawing() {
    m_shown.clear();
    polish();
    update();
}

void DiagnosticsLogView::ensureLayout() {
    if (!m_layoutDirty) {
        return;
    }
    m_layoutDirty = false;
    if (m_fontDirty) {
        m_fontDirty = false;
        m_layout.setFont(m_font, renderType() != QSGTextNode::NativeRendering);
    }
    m_layout.layOut(width() - m_leftPadding - m_rightPadding);
    if (length() > 0) {
        ++m_layoutPasses;
    }
    m_shown.clear();
    update();

    // Scrolling forgets it, and the new height may itself scroll.
    const Held held = m_held;
    updateContentHeight();
    if (held.end || held.position >= 0) {
        const qreal bottom = std::max(0.0, m_contentHeight - height());
        const qreal y = held.end
            ? bottom
            : std::clamp(m_topPadding + m_layout.rowAt(held.position) * m_layout.rowHeight() + held.intoRow, 0.0,
                         bottom);
        requestScroll(y);
        m_held = held;
    }
}

void DiagnosticsLogView::updateContentHeight() {
    const qreal contentHeight = m_topPadding + m_layout.rowCount() * m_layout.rowHeight() + m_bottomPadding;
    if (m_contentHeight != contentHeight) {
        m_contentHeight = contentHeight;
        emit contentHeightChanged();
    }
}

DiagnosticsLogLayout &DiagnosticsLogView::laidOut() {
    ensureLayout();
    return m_layout;
}

void DiagnosticsLogView::holdPlace(qreal viewHeight) {
    // At the top, and until it has been laid out, there is nothing to hold.
    if (m_held.end || m_held.position >= 0 || m_layoutDirty || m_contentY <= 0 || length() == 0) {
        return;
    }
    if (m_contentY + viewHeight >= m_contentHeight - 1) {
        m_held.end = true;
        return;
    }
    const qreal rowHeight = m_layout.rowHeight();
    const int top = static_cast<int>(
        std::clamp<qreal>(std::floor((m_contentY - m_topPadding) / rowHeight), 0, m_layout.rowCount() - 1));
    m_held.position = m_layout.row(top).start;
    m_held.intoRow = m_contentY - m_topPadding - top * rowHeight;
}

void DiagnosticsLogView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.width() != oldGeometry.width()) {
        holdPlace(oldGeometry.height());
        invalidateLayout();
    } else if (newGeometry.height() != oldGeometry.height()) {
        polish();
        update();
    }
}

void DiagnosticsLogView::itemChange(ItemChange change, const ItemChangeData &value) {
    if (change == ItemDevicePixelRatioHasChanged) {
        invalidateDrawing();
    } else if (change == ItemSceneChange && value.window != nullptr) {
        polish();
    }
    QQuickItem::itemChange(change, value);
}

void DiagnosticsLogView::updatePolish() {
    // Without a width there is nothing to wrap to.
    if (width() > 0) {
        ensureLayout();
    }
    if (width() <= 0 || height() <= 0 || length() == 0) {
        m_shown.clear();
        update();
        return;
    }
    const qreal rowHeight = m_layout.rowHeight();
    const auto rowAtY = [&](qreal y) {
        return static_cast<int>(std::clamp<qreal>(std::floor((y - m_topPadding) / rowHeight), 0,
                                                  m_layout.rowCount() - 1));
    };
    const int first = rowAtY(m_contentY);
    const int last = rowAtY(m_contentY + height());
    m_shown.erase(m_shown.begin(), m_shown.lower_bound(first));
    m_shown.erase(m_shown.upper_bound(last), m_shown.end());
    const int selectedFrom = selectionStart();
    const int selectedTo = selectionEnd();
    for (int index = first; index <= last; ++index) {
        const DiagnosticsLogLayout::Row row = m_layout.row(index);
        DiagnosticsLogLayout::Highlight selected{0, 0, m_selectedTextColor, m_selectionColor};
        const int from = std::max(selectedFrom, row.start);
        const int to = std::min(selectedTo, row.start + row.length);
        if (from < to) {
            selected.from = from - row.start;
            selected.count = to - from;
        }
        ShownRow &shown = m_shown[index];
        if (!shown.layout || shown.selectedFrom != selected.from || shown.selectedCount != selected.count) {
            shown.layout = m_layout.shape(index, selected);
            shown.selectedFrom = selected.from;
            shown.selectedCount = selected.count;
            shown.serial = ++m_shapedRows;
        }
    }
    update();
}

QSGNode *DiagnosticsLogView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
    // The GUI thread waits while this runs, so it reads the item's state.
    auto *log = static_cast<LogNode *>(oldNode);
    if (log == nullptr) {
        log = new LogNode;
    }
    const QRectF area(m_leftPadding, m_topPadding, std::max(0.0, width() - m_leftPadding - m_rightPadding),
                      std::max(0.0, height() - m_topPadding - m_bottomPadding));
    if (log->clipRect() != area) {
        log->setArea(area);
    }
    for (auto drawn = log->rows.begin(); drawn != log->rows.end();) {
        const auto shown = m_shown.find(drawn->first);
        if (shown == m_shown.end() || shown->second.serial != drawn->second.serial) {
            log->removeChildNode(drawn->second.node);
            delete drawn->second.node;
            drawn = log->rows.erase(drawn);
        } else {
            ++drawn;
        }
    }

    const qreal ratio = window()->effectiveDevicePixelRatio();
    const auto onPixel = [ratio](qreal value) { return static_cast<float>(std::round(value * ratio) / ratio); };
    for (const auto &[index, shown] : m_shown) {
        auto drawn = log->rows.find(index);
        if (drawn == log->rows.end()) {
            QSGTextNode *node = window()->createTextNode();
            node->setRenderType(renderType());
            node->setColor(m_color);
            if (!shown.layout->text().isEmpty()) {
                node->addTextLayout(QPointF(), shown.layout.get());
            }
            log->appendChildNode(node);
            drawn = log->rows.emplace(index, LogNode::DrawnRow{node, shown.serial}).first;
        }
        // In doubles up to here: single precision is too coarse for a y
        // millions of pixels down a long log.
        QMatrix4x4 place;
        place.translate(onPixel(m_leftPadding + m_layout.indent(index)),
                        onPixel(m_topPadding + index * m_layout.rowHeight() - m_contentY));
        if (drawn->second.node->matrix() != place) {
            drawn->second.node->setMatrix(place);
        }
    }
    return log;
}

int DiagnosticsLogView::selectionStart() const {
    return std::min(m_anchor, m_caret);
}

int DiagnosticsLogView::selectionEnd() const {
    return std::max(m_anchor, m_caret);
}

QString DiagnosticsLogView::selectedText() const {
    return getText(selectionStart(), selectionEnd());
}

QString DiagnosticsLogView::getText(int start, int end) const {
    const int from = std::clamp(std::min(start, end), 0, length());
    const int to = std::clamp(std::max(start, end), 0, length());
    return m_layout.text().mid(from, to - from);
}

void DiagnosticsLogView::setSelection(int anchor, int caret) {
    anchor = std::clamp(anchor, 0, length());
    caret = std::clamp(caret, 0, length());
    if (m_anchor == anchor && m_caret == caret) {
        return;
    }
    const bool hadSelection = hasSelection();
    m_anchor = anchor;
    m_caret = caret;
    polish();
    update();
    emit selectionChanged();
    if (QAccessible::isActive()) {
        if (hadSelection || hasSelection()) {
            QAccessibleTextSelectionEvent selection(this, selectionStart(), selectionEnd());
            selection.setCursorPosition(m_caret);
            QAccessible::updateAccessibility(&selection);
        }
        QAccessibleTextCursorEvent cursor(this, m_caret);
        QAccessible::updateAccessibility(&cursor);
    }
}

void DiagnosticsLogView::select(int start, int end) {
    setSelection(start, end);
}

void DiagnosticsLogView::moveCaretTo(int position) {
    select(position, position);
    reveal(m_caret);
}

void DiagnosticsLogView::selectAll() {
    select(0, length());
    // As in a text edit, the caret is then at the end, which shows.
    reveal(m_caret);
    setPrimarySelection();
}

void DiagnosticsLogView::copy() const {
    if (hasSelection()) {
        QGuiApplication::clipboard()->setText(selectedText());
    }
}

void DiagnosticsLogView::setPrimarySelection() const {
    QClipboard *clipboard = QGuiApplication::clipboard();
    if (hasSelection() && clipboard->supportsSelection()) {
        clipboard->setText(selectedText(), QClipboard::Selection);
    }
}

qreal DiagnosticsLogView::xOf(int row, int position) const {
    const auto shown = m_shown.find(row);
    const std::unique_ptr<QTextLayout> made = shown == m_shown.end() ? m_layout.shape(row) : nullptr;
    const QTextLayout *layout = made ? made.get() : shown->second.layout.get();
    if (layout->lineCount() == 0) {
        return 0;
    }
    const DiagnosticsLogLayout::Row span = m_layout.row(row);
    return m_layout.indent(row) + layout->lineAt(0).cursorToX(std::clamp(position - span.start, 0, span.length));
}

int DiagnosticsLogView::positionInRow(int row, qreal x) const {
    const DiagnosticsLogLayout::Row span = m_layout.row(row);
    const auto shown = m_shown.find(row);
    const std::unique_ptr<QTextLayout> made = shown == m_shown.end() ? m_layout.shape(row) : nullptr;
    const QTextLayout *layout = made ? made.get() : shown->second.layout.get();
    if (layout->lineCount() == 0) {
        return span.start;
    }
    return span.start + std::clamp(layout->lineAt(0).xToCursor(x - m_layout.indent(row)), 0, span.length);
}

QRectF DiagnosticsLogView::positionToRectangle(int position) {
    ensureLayout();
    position = std::clamp(position, 0, length());
    const int row = m_layout.rowAt(position);
    return {m_leftPadding + xOf(row, position),
            m_topPadding + row * m_layout.rowHeight() - m_contentY, 1, m_layout.rowHeight()};
}

int DiagnosticsLogView::positionAt(qreal x, qreal y) {
    ensureLayout();
    const qreal row = std::floor((y + m_contentY - m_topPadding) / m_layout.rowHeight());
    if (row >= m_layout.rowCount()) {
        return length();
    }
    return positionInRow(static_cast<int>(std::max<qreal>(0, row)), x - m_leftPadding);
}

void DiagnosticsLogView::requestScroll(qreal y) {
    emit scrollRequested(y);
}

void DiagnosticsLogView::reveal(int position) {
    ensureLayout();
    if (width() <= 0 || height() <= 0) {
        return;
    }
    const int row = m_layout.rowAt(position);
    const qreal top = m_topPadding + row * m_layout.rowHeight();
    const qreal bottom = top + m_layout.rowHeight();
    qreal y = m_contentY;
    if (top < y) {
        // The first row brings the padding above it along, the last the one below.
        y = row == 0 ? 0 : top;
    } else if (bottom > y + height()) {
        y = (row == m_layout.rowCount() - 1 ? m_contentHeight : bottom) - height();
    }
    if (y != m_contentY) {
        requestScroll(y);
    }
}

DiagnosticsLogView::Span DiagnosticsLogView::unitAt(int position, SelectBy by) const {
    if (by == SelectBy::Character) {
        return {position, position};
    }
    const int record = m_layout.recordAt(position);
    const int start = m_layout.recordStart(record);
    const QStringView text = m_layout.recordText(record);
    if (by == SelectBy::Record || text.isEmpty()) {
        return {start, m_layout.recordEnd(record)};
    }
    QTextBoundaryFinder words(QTextBoundaryFinder::Word, text);
    const int at = std::min(position - start, static_cast<int>(text.size()) - 1);
    words.setPosition(at + 1);
    const int from = std::max(0, static_cast<int>(words.toPreviousBoundary()));
    const int to = static_cast<int>(words.toNextBoundary());
    return {start + from, start + (to < 0 ? static_cast<int>(text.size()) : to)};
}

void DiagnosticsLogView::extendDragTo(const QPointF &point) {
    // Past an edge, the selection goes on to what that edge shows.
    const QPointF inside(std::clamp(point.x(), 0.0, width()),
                         std::clamp(point.y(), 0.0, std::max(0.0, height() - 1)));
    const int position = positionAt(inside.x(), inside.y());
    const Span unit = unitAt(position, m_selectBy);
    if (position < m_dragOrigin.start) {
        setSelection(m_dragOrigin.end, unit.start);
    } else if (position > m_dragOrigin.end) {
        setSelection(m_dragOrigin.start, unit.end);
    } else {
        setSelection(m_dragOrigin.start,
                     m_selectBy == SelectBy::Character ? position : m_dragOrigin.end);
    }
}

void DiagnosticsLogView::endDrag() {
    m_dragging = false;
    m_dragScroll.stop();
    setKeepMouseGrab(false);
}

void DiagnosticsLogView::mousePressEvent(QMouseEvent *event) {
    // A finger scrolls the log.
    const QInputDevice *device = event->device();
    if (event->button() != Qt::LeftButton
        || (device != nullptr && device->type() == QInputDevice::DeviceType::TouchScreen)) {
        event->ignore();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
    setKeepMouseGrab(true);
    m_dragging = true;
    m_dragPoint = event->position();
    const int position = positionAt(m_dragPoint.x(), m_dragPoint.y());
    const QStyleHints *hints = QGuiApplication::styleHints();
    const bool third = m_sinceDoubleClick.isValid()
        && m_sinceDoubleClick.elapsed() < hints->mouseDoubleClickInterval()
        && (m_dragPoint - m_doubleClickPoint).manhattanLength() < hints->startDragDistance();
    m_sinceDoubleClick.invalidate();
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        m_selectBy = SelectBy::Character;
        m_dragOrigin = {m_anchor, m_anchor};
        setSelection(m_anchor, position);
    } else {
        m_selectBy = third ? SelectBy::Record : SelectBy::Character;
        m_dragOrigin = unitAt(position, m_selectBy);
        setSelection(m_dragOrigin.start, m_dragOrigin.end);
    }
    event->accept();
}

void DiagnosticsLogView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    m_dragging = true;
    m_dragPoint = event->position();
    m_selectBy = SelectBy::Word;
    m_dragOrigin = unitAt(positionAt(m_dragPoint.x(), m_dragPoint.y()), m_selectBy);
    setSelection(m_dragOrigin.start, m_dragOrigin.end);
    m_doubleClickPoint = m_dragPoint;
    m_sinceDoubleClick.start();
    event->accept();
}

void DiagnosticsLogView::mouseMoveEvent(QMouseEvent *event) {
    if (!m_dragging) {
        event->ignore();
        return;
    }
    if (event->position() != m_dragPoint) {
        m_dragPoint = event->position();
        extendDragTo(m_dragPoint);
    }
    if (m_dragPoint.y() < 0 || m_dragPoint.y() > height()) {
        if (!m_dragScroll.isActive()) {
            m_dragScroll.start(kDragScrollIntervalMs, this);
        }
    } else {
        m_dragScroll.stop();
    }
    event->accept();
}

void DiagnosticsLogView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_dragging) {
        endDrag();
        setPrimarySelection();
    }
    event->accept();
}

void DiagnosticsLogView::mouseUngrabEvent() {
    endDrag();
}

void DiagnosticsLogView::timerEvent(QTimerEvent *event) {
    if (event->timerId() != m_dragScroll.timerId()) {
        QQuickItem::timerEvent(event);
        return;
    }
    // Faster the farther past the edge, up to a page a frame.
    const qreal point = m_dragPoint.y();
    const qreal past = point < 0 ? point : std::max<qreal>(0, point - height());
    const qreal step = std::clamp(past / 2, -height(), height());
    const qreal y = std::clamp(m_contentY + step, 0.0, std::max(0.0, m_contentHeight - height()));
    if (y != m_contentY) {
        requestScroll(y);
    }
    extendDragTo(m_dragPoint);
}

bool DiagnosticsLogView::handlesKey(const QKeyEvent *event) const {
    if (event->matches(QKeySequence::Copy) || event->matches(QKeySequence::SelectAll)) {
        return true;
    }
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    switch (event->key()) {
    case Qt::Key_Home:
    case Qt::Key_End:
        return modifiers == Qt::NoModifier || modifiers == Qt::ControlModifier;
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
        return modifiers == Qt::NoModifier;
    default:
        return false;
    }
}

bool DiagnosticsLogView::event(QEvent *event) {
    // Before a shortcut of the same keys, as in a text edit.
    if (event->type() == QEvent::ShortcutOverride && handlesKey(static_cast<QKeyEvent *>(event))) {
        event->accept();
        return true;
    }
    return QQuickItem::event(event);
}

bool DiagnosticsLogView::scrollForKey(const QKeyEvent *event) {
    ensureLayout();
    const qreal line = m_layout.rowHeight();
    const qreal page = std::max(line, height() - line);
    qreal y = m_contentY;
    switch (event->key()) {
    case Qt::Key_Up: y -= line; break;
    case Qt::Key_Down: y += line; break;
    case Qt::Key_PageUp: y -= page; break;
    case Qt::Key_PageDown: y += page; break;
    case Qt::Key_Home: y = 0; break;
    case Qt::Key_End: y = m_contentHeight; break;
    default: return false;
    }
    requestScroll(std::clamp(y, 0.0, std::max(0.0, m_contentHeight - height())));
    return true;
}

void DiagnosticsLogView::keyPressEvent(QKeyEvent *event) {
    if (!handlesKey(event)) {
        event->ignore();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        copy();
    } else if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
    } else {
        scrollForKey(event);
    }
    event->accept();
}
