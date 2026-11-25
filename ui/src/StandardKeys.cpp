// SPDX-License-Identifier: GPL-3.0-or-later

#include "StandardKeys.h"

#include <QKeySequence>
#include <qqml.h>

QStringList StandardKeys::bindings(int standardKey) {
    QStringList texts;
    const QList<QKeySequence> sequences =
        QKeySequence::keyBindings(static_cast<QKeySequence::StandardKey>(standardKey));
    for (const QKeySequence &sequence : sequences) {
        texts << sequence.toString(QKeySequence::PortableText);
    }
    return texts;
}

namespace {

QKeySequence sequenceOf(const QVariant &value) {
    if (value.metaType() == QMetaType::fromType<QKeySequence>()) {
        return value.value<QKeySequence>();
    }
    const QString text = value.toString().trimmed();
    if (text.isEmpty()) {
        return {};
    }
    const QKeySequence portable = QKeySequence::fromString(text, QKeySequence::PortableText);
    return portable.isEmpty() ? QKeySequence::fromString(text, QKeySequence::NativeText) : portable;
}

} // namespace

QString StandardKeys::portableText(const QVariant &sequence) {
    return sequenceOf(sequence).toString(QKeySequence::PortableText);
}

QStringList StandardKeys::splitSequences(const QString &text) {
    QStringList texts;
    for (const QKeySequence &sequence : QKeySequence::listFromString(text, QKeySequence::PortableText)) {
        const QString portable = sequence.toString(QKeySequence::PortableText);
        // A repeat would be the action's and its alternate's, which makes
        // the key trigger neither.
        if (!portable.isEmpty() && !texts.contains(portable)) {
            texts << portable;
        }
    }
    return texts;
}

QString StandardKeys::joinSequences(const QVariantList &sequences) {
    QList<QKeySequence> kept;
    for (const QVariant &value : sequences) {
        const QKeySequence sequence = sequenceOf(value);
        if (!sequence.isEmpty() && !kept.contains(sequence)) {
            kept << sequence;
        }
    }
    return QKeySequence::listToString(kept, QKeySequence::PortableText);
}

QString StandardKeys::nativeText(const QVariant &sequence) {
    return sequenceOf(sequence).toString(QKeySequence::NativeText);
}

QString StandardKeys::keptKeyRole(const QVariant &sequence) {
    const QKeySequence keys = sequenceOf(sequence);
    if (keys.count() != 1) {
        return {};
    }
    const QKeyCombination combination = keys[0];
    // The keypad's keys are the same keys to the lists and text fields.
    const Qt::KeyboardModifiers modifiers = combination.keyboardModifiers() & ~Qt::KeypadModifier;
    const bool alone = modifiers == Qt::NoModifier;
    switch (combination.key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (alone) {
            return QStringLiteral("activate");
        }
        // The library and search add their selection to the queue with it.
        return modifiers == Qt::ControlModifier ? QStringLiteral("queue") : QString();
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        return alone ? QStringLiteral("delete") : QString();
    // Shift+Tab comes as Backtab, with Shift or without.
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return alone || modifiers == Qt::ShiftModifier ? QStringLiteral("focus") : QString();
    case Qt::Key_Escape:
        return alone ? QStringLiteral("cancel") : QString();
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
    case Qt::Key_Home:
    case Qt::Key_End:
        return alone ? QStringLiteral("navigate") : QString();
    case Qt::Key_Menu:
        return alone ? QStringLiteral("contextMenu") : QString();
    case Qt::Key_F10:
        return modifiers == Qt::ShiftModifier ? QStringLiteral("contextMenu") : QString();
    case Qt::Key_M:
        return modifiers == Qt::ControlModifier ? QStringLiteral("menuBar") : QString();
    default:
        return {};
    }
}

bool StandardKeys::typesCharacter(const QVariant &sequence) {
    const QKeySequence keys = sequenceOf(sequence);
    if (keys.count() != 1) {
        return false;
    }
    const QKeyCombination combination = keys[0];
    if ((combination.keyboardModifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier)) != Qt::NoModifier) {
        return false;
    }
    // Qt names a key that types a character by its code point; the other
    // keys, such as F1 or Media Play, are past Unicode's range.
    const int key = static_cast<int>(combination.key());
    return key != Qt::Key_Space && key <= 0x10FFFF && QChar::isPrint(static_cast<char32_t>(key));
}

void StandardKeys::registerQmlTypes() {
    qmlRegisterSingletonType<StandardKeys>(
        "FerrousUi", 1, 0, "StandardKeys", [](QQmlEngine *, QJSEngine *) -> QObject * { return new StandardKeys(); });
}
