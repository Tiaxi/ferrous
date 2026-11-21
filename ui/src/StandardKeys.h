// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

// The key bindings of a standard key, for QML. A standard key can have more
// than one, as Open has Ctrl+O and the Open key, and KDE's Shortcuts
// settings give each an alternate. An action takes one shortcut, and Qt
// Quick, given a standard key, takes only its first binding: the action,
// and the Shortcut that Breeze's menu item shows it with, which warns of the
// rest. ShortcutAction.qml takes them all. It also reads and writes the
// lists of key sequences the settings keep for the shortcuts the user
// changed. QML has it as the FerrousUi singleton StandardKeys.
class StandardKeys : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // The bindings of a QKeySequence::StandardKey, the platform's first
    // first, as portable text, which a shortcut or a Shortcut's sequences
    // take; none for an unknown key.
    Q_INVOKABLE static QStringList bindings(int standardKey);

    // `sequence`, a QKeySequence or its text in portable or native form, as
    // portable text, which the settings keep; empty for no keys or text
    // that names none.
    Q_INVOKABLE static QString portableText(const QVariant &sequence);

    // The key sequences of a list as the settings keep it, "Media Play;
    // Space" (QKeySequence::listToString in portable text), each as
    // portable text, in order, without empty ones or repeats.
    Q_INVOKABLE static QStringList splitSequences(const QString &text);

    // The list of `sequences` as the settings keep it, each normalized as
    // portableText does, without empty ones or repeats; empty for none.
    Q_INVOKABLE static QString joinSequences(const QVariantList &sequences);

    // Registers the singleton with the FerrousUi module.
    static void registerQmlTypes();
};
