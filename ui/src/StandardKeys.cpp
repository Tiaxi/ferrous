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

void StandardKeys::registerQmlTypes() {
    qmlRegisterSingletonType<StandardKeys>(
        "FerrousUi", 1, 0, "StandardKeys", [](QQmlEngine *, QJSEngine *) -> QObject * { return new StandardKeys(); });
}
