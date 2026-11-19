// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QStringList>

// The key bindings of a standard key, for QML. A standard key can have more
// than one, as Open has Ctrl+O and the Open key, and KDE's Shortcuts
// settings give each an alternate. An action takes one shortcut, and Qt
// Quick, given a standard key, takes only its first binding: the action,
// and the Shortcut that Breeze's menu item shows it with, which warns of the
// rest. StandardKeyAction.qml takes them all. QML has it as the FerrousUi
// singleton StandardKeys.
class StandardKeys : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // The bindings of a QKeySequence::StandardKey, the platform's first
    // first, as portable text, which a shortcut or a Shortcut's sequences
    // take; none for an unknown key.
    Q_INVOKABLE static QStringList bindings(int standardKey);

    // Registers the singleton with the FerrousUi module.
    static void registerQmlTypes();
};
