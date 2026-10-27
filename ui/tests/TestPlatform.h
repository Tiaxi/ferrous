// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArrayView>
#include <QGuiApplication>
#include <QString>
#include <QtTest/QtTest>

// Makes a test binary that is run without a platform chosen draw offscreen,
// so that running one directly never opens windows on the desktop. Call it
// in main(), with main()'s arguments, before the application object is made.
//
// A run that needs a native platform chooses it, with QT_QPA_PLATFORM or
// with -platform (or --platform), as CI's comparison of native waveform
// rendering does.
inline bool &offscreenPlatformChosenByDefault() {
    static bool chosen = false;
    return chosen;
}

inline void useOffscreenPlatformByDefault(int argc, char **argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        const QByteArrayView argument(argv[i]);
        if (argument == "-platform" || argument == "--platform") {
            return;
        }
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        offscreenPlatformChosenByDefault() = true;
    }
}

// GPU drivers write shader caches into the cache directory from threads of
// their own, and at exit: Mesa while a test removes its directories, which
// fails the test, and AMD's compiler after main() has removed the process's.
// Call this in main() so that tests write none.
inline void disableGpuShaderCaches() {
    qputenv("MESA_SHADER_CACHE_DISABLE", "true");
    qputenv("AMD_COMGR_CACHE", "0");
}

// The test of the default, for a test slot of each binary that shows windows.
inline void verifyOffscreenPlatformByDefault() {
    if (!offscreenPlatformChosenByDefault()) {
        QSKIP("QT_QPA_PLATFORM or -platform chose the platform");
    }
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
}
