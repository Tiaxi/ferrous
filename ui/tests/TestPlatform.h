// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QGuiApplication>
#include <QString>
#include <QtTest/QtTest>

// Makes a test binary that is run without a platform chosen draw offscreen,
// so that running one directly never opens windows on the desktop. Call it
// in main() before the application object is made.
//
// A run that needs a native platform chooses it, with QT_QPA_PLATFORM or
// -platform, as CI's comparison of native waveform rendering does.
inline bool &offscreenPlatformChosenByDefault() {
    static bool chosen = false;
    return chosen;
}

inline void useOffscreenPlatformByDefault() {
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        offscreenPlatformChosenByDefault() = true;
    }
}

// The test of the default, for a test slot of each binary that shows windows.
inline void verifyOffscreenPlatformByDefault() {
    if (!offscreenPlatformChosenByDefault()) {
        QSKIP("QT_QPA_PLATFORM chose the platform");
    }
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
}
