// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtLogging>

// The warnings and errors that Qt logs while a test runs, from any thread,
// kept so that a test fails for one it does not expect: a QML warning, a
// binding loop, a failed connection.
//
// A test class starts capturing in initTestCase(), takes what the set-up
// left in init(), and fails in cleanup() when takeCapturedMessagesText() is
// not empty. A test that expects a message takes it itself, or drops it.

inline QMutex &capturedMessageMutex() {
    static QMutex mutex;
    return mutex;
}

// Use it with capturedMessageMutex() held.
inline QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
}

inline QtMessageHandler &previousMessageHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

// Keeps a warning or an error, and passes every message on to the handler
// that was installed before, which prints it.
inline void captureQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker locker(&capturedMessageMutex());
        capturedMessages().push_back(message);
    }
    if (previousMessageHandler()) {
        previousMessageHandler()(type, context, message);
    }
}

inline void startCapturingMessages() {
    previousMessageHandler() = qInstallMessageHandler(captureQtMessage);
}

inline void stopCapturingMessages() {
    qInstallMessageHandler(previousMessageHandler());
    previousMessageHandler() = nullptr;
}

// The messages captured so far, which it forgets.
inline QStringList takeCapturedMessages() {
    QMutexLocker locker(&capturedMessageMutex());
    QStringList messages = capturedMessages();
    capturedMessages().clear();
    return messages;
}

// As takeCapturedMessages(), a message to a line.
inline QString takeCapturedMessagesText() {
    return takeCapturedMessages().join(QLatin1Char('\n'));
}

inline void clearCapturedMessages() {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().clear();
}

// Drops the captured messages that are `message`, which the test expects.
inline void dropCapturedMessages(const QString &message) {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().removeAll(message);
}

// Drops the captured messages `pattern` matches.
inline void dropCapturedMessagesMatching(const QRegularExpression &pattern) {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().removeIf([&pattern](const QString &message) { return pattern.match(message).hasMatch(); });
}

// What the offscreen platform logs when a window's size hints change, as
// when a larger font raises its minimum size or a resize goes below it:
// the platform does not pass size hints on. It says nothing about the app.
inline QString offscreenSizeHintsWarning() {
    return QStringLiteral("This plugin does not support propagateSizeHints()");
}
