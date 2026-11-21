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

    // `sequence` as the user reads it, in the platform's words: empty for
    // no keys.
    Q_INVOKABLE static QString nativeText(const QVariant &sequence);

    // What the window, its lists or its text fields keep `sequence` for,
    // so that no action may take it, as a code the shortcut editor words:
    // "activate" for Enter, which plays a list's selection and presses a
    // dialog's default button; "delete" for Delete and Backspace; "focus"
    // for Tab and Shift+Tab; "cancel" for Escape; "navigate" for the arrow,
    // Page, Home and End keys alone; "contextMenu" for Menu and Shift+F10;
    // "menuBar" for Ctrl+M, which shows KDE's menu bar. Empty for a key an
    // action may take, and for a sequence of more than one key.
    Q_INVOKABLE static QString keptKeyRole(const QVariant &sequence);

    // Whether `sequence` is one key that types a character, with Shift or
    // without, such as F or Shift+F: the library's type-ahead and text
    // fields take it while they have the keyboard focus. Not Space, which
    // the lists leave to the shortcuts.
    Q_INVOKABLE static bool typesCharacter(const QVariant &sequence);

    // Registers the singleton with the FerrousUi module.
    static void registerQmlTypes();
};
