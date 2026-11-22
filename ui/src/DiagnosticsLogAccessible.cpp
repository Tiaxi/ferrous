// SPDX-License-Identifier: GPL-3.0-or-later

#include "DiagnosticsLogAccessible.h"

#include <QAccessible>
#include <QAccessibleObject>
#include <QQuickWindow>
#include <QTextBoundaryFinder>

#include <algorithm>

#include "DiagnosticsLogView.h"

namespace {

class Interface : public QAccessibleObject,
                  public QAccessibleTextInterface,
                  public QAccessibleActionInterface {
public:
    explicit Interface(DiagnosticsLogView *view)
        : QAccessibleObject(view) {}

    void *interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::TextInterface) {
            return static_cast<QAccessibleTextInterface *>(this);
        }
        if (type == QAccessible::ActionInterface) {
            return static_cast<QAccessibleActionInterface *>(this);
        }
        return nullptr;
    }

    QAccessible::Role role() const override { return QAccessible::EditableText; }

    QAccessible::State state() const override {
        QAccessible::State state;
        state.readOnly = true;
        state.multiLine = true;
        state.focusable = true;
        state.selectableText = true;
        if (const DiagnosticsLogView *log = view()) {
            state.focused = log->hasActiveFocus();
            state.invisible = !log->isVisible();
        } else {
            state.invalid = true;
        }
        return state;
    }

    QString text(QAccessible::Text kind) const override {
        const DiagnosticsLogView *log = view();
        if (log != nullptr && kind == QAccessible::Name) {
            return log->accessibleName();
        }
        if (log != nullptr && kind == QAccessible::Value) {
            return log->text();
        }
        return {};
    }

    QWindow *window() const override {
        const DiagnosticsLogView *log = view();
        return log != nullptr ? log->window() : nullptr;
    }

    QRect rect() const override {
        const DiagnosticsLogView *log = view();
        return log != nullptr ? onScreen(*log, log->boundingRect()) : QRect();
    }

    // The nearest of its ancestors that the window's tree shows.
    QAccessibleInterface *parent() const override {
        const DiagnosticsLogView *log = view();
        if (log == nullptr || log->window() == nullptr) {
            return nullptr;
        }
        for (QQuickItem *item = log->parentItem(); item != nullptr && item != log->window()->contentItem();
             item = item->parentItem()) {
            QAccessibleInterface *ancestor = QAccessible::queryAccessibleInterface(item);
            if (ancestor == nullptr) {
                continue;
            }
            const QAccessibleInterface *above = ancestor->parent();
            if (above != nullptr && above->indexOfChild(ancestor) >= 0) {
                return ancestor;
            }
        }
        return QAccessible::queryAccessibleInterface(log->window());
    }

    int childCount() const override { return 0; }
    QAccessibleInterface *child(int) const override { return nullptr; }
    int indexOfChild(const QAccessibleInterface *) const override { return -1; }
    QAccessibleInterface *childAt(int, int) const override { return nullptr; }

    QStringList actionNames() const override { return {setFocusAction()}; }

    void doAction(const QString &name) override {
        if (DiagnosticsLogView *log = view(); log != nullptr && name == setFocusAction()) {
            log->forceActiveFocus();
        }
    }

    QStringList keyBindingsForAction(const QString &) const override { return {}; }

    void selection(int index, int *start, int *end) const override {
        const DiagnosticsLogView *log = view();
        const bool has = log != nullptr && index == 0 && log->hasSelection();
        *start = has ? log->selectionStart() : 0;
        *end = has ? log->selectionEnd() : 0;
    }

    int selectionCount() const override {
        const DiagnosticsLogView *log = view();
        return log != nullptr && log->hasSelection() ? 1 : 0;
    }

    void addSelection(int start, int end) override { setSelection(0, start, end); }

    void removeSelection(int index) override {
        if (DiagnosticsLogView *log = view(); log != nullptr && index == 0) {
            log->select(log->cursorPosition(), log->cursorPosition());
        }
    }

    void setSelection(int index, int start, int end) override {
        if (DiagnosticsLogView *log = view(); log != nullptr && index == 0) {
            log->select(start, end);
        }
    }

    int cursorPosition() const override {
        const DiagnosticsLogView *log = view();
        return log != nullptr ? log->cursorPosition() : 0;
    }

    void setCursorPosition(int position) override {
        if (DiagnosticsLogView *log = view()) {
            log->moveCaretTo(position);
        }
    }

    int characterCount() const override {
        const DiagnosticsLogView *log = view();
        return log != nullptr ? log->length() : 0;
    }

    QString text(int start, int end) const override {
        const DiagnosticsLogView *log = view();
        return log != nullptr ? log->getText(start, end) : QString();
    }

    // Not the defaults, which read the whole log for each line.
    QString textAtOffset(int offset, QAccessible::TextBoundaryType type, int *start, int *end) const override {
        *start = -1;
        *end = -1;
        DiagnosticsLogView *log = view();
        if (log == nullptr) {
            return {};
        }
        // -1 is the caret, as for Qt's own text.
        const int at = offset == -1 ? log->cursorPosition() : offset;
        if (at < 0 || at > log->length()) {
            return {};
        }
        const auto [from, to] = spanAt(*log, at, type);
        *start = from;
        *end = to;
        return log->getText(from, to);
    }

    QString textBeforeOffset(int offset, QAccessible::TextBoundaryType type, int *start, int *end) const override {
        int from = -1;
        int to = -1;
        textAtOffset(offset, type, &from, &to);
        if (from <= 0) {
            *start = -1;
            *end = -1;
            return {};
        }
        return textAtOffset(from - 1, type, start, end);
    }

    QString textAfterOffset(int offset, QAccessible::TextBoundaryType type, int *start, int *end) const override {
        int from = -1;
        int to = -1;
        textAtOffset(offset, type, &from, &to);
        if (to < 0 || to >= characterCount()) {
            *start = -1;
            *end = -1;
            return {};
        }
        return textAtOffset(to, type, start, end);
    }

    QRect characterRect(int offset) const override {
        DiagnosticsLogView *log = view();
        if (log == nullptr || offset < 0 || offset > log->length()) {
            return {};
        }
        QRectF character = log->positionToRectangle(offset);
        const QRectF next = log->positionToRectangle(offset + 1);
        if (next.top() == character.top() && next.left() > character.left()) {
            character.setRight(next.left());
        }
        return onScreen(*log, character);
    }

    int offsetAtPoint(const QPoint &point) const override {
        DiagnosticsLogView *log = view();
        if (log == nullptr || log->window() == nullptr) {
            return -1;
        }
        const QPointF local = log->mapFromScene(log->window()->mapFromGlobal(QPointF(point)));
        return log->contains(local) ? log->positionAt(local.x(), local.y()) : -1;
    }

    void scrollToSubstring(int start, int end) override {
        if (DiagnosticsLogView *log = view()) {
            log->revealPosition(end);
            log->revealPosition(start);
        }
    }

    QString attributes(int, int *start, int *end) const override {
        *start = 0;
        *end = characterCount();
        return {};
    }

private:
    DiagnosticsLogView *view() const { return qobject_cast<DiagnosticsLogView *>(object()); }

    static QRect onScreen(const DiagnosticsLogView &log, const QRectF &rect) {
        if (log.window() == nullptr) {
            return {};
        }
        const QRectF scene = log.mapRectToScene(rect);
        return QRect(log.window()->mapToGlobal(scene.topLeft()).toPoint(), scene.size().toSize());
    }

    // The spans of each boundary cover the text without a gap: a line's and
    // a paragraph's hold their newline.
    static std::pair<int, int> spanAt(DiagnosticsLogView &log, int at, QAccessible::TextBoundaryType type) {
        if (type == QAccessible::NoBoundary) {
            return {0, log.length()};
        }
        const DiagnosticsLogLayout &layout = log.laidOut();
        const int record = layout.recordAt(at);
        const int start = layout.recordStart(record);
        const int end = layout.recordEnd(record);
        const int withNewline = std::min(end + 1, log.length());
        if (type == QAccessible::ParagraphBoundary) {
            return {start, withNewline};
        }
        if (type == QAccessible::LineBoundary) {
            const DiagnosticsLogLayout::Row row = layout.row(layout.rowAt(at));
            const int rowEnd = row.start + row.length;
            return {row.start, rowEnd == end ? withNewline : rowEnd};
        }
        if (at >= end) {
            return {at, std::min(at + 1, log.length())};
        }
        QTextBoundaryFinder::BoundaryType by = QTextBoundaryFinder::Grapheme;
        if (type == QAccessible::WordBoundary) {
            by = QTextBoundaryFinder::Word;
        } else if (type == QAccessible::SentenceBoundary) {
            by = QTextBoundaryFinder::Sentence;
        }
        const QStringView text = layout.recordText(record);
        QTextBoundaryFinder finder(by, text);
        finder.setPosition(at - start + 1);
        const auto from = static_cast<int>(finder.toPreviousBoundary());
        const auto to = static_cast<int>(finder.toNextBoundary());
        return {start + std::max(0, from), to < 0 ? end : start + to};
    }
};

QAccessibleInterface *create(const QString &, QObject *object) {
    auto *view = qobject_cast<DiagnosticsLogView *>(object);
    return view != nullptr ? new Interface(view) : nullptr;
}

} // namespace

void DiagnosticsLogAccessible::install() {
    // Installed after Qt Quick's, which would make it an item without text:
    // QAccessible asks the latest first.
    static const bool installed = []() {
        QAccessible::installFactory(create);
        return true;
    }();
    Q_UNUSED(installed)
}
