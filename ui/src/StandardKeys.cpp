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

void StandardKeys::registerQmlTypes() {
    qmlRegisterSingletonType<StandardKeys>(
        "FerrousUi", 1, 0, "StandardKeys", [](QQmlEngine *, QJSEngine *) -> QObject * { return new StandardKeys(); });
}
