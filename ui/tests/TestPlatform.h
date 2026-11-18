// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArrayView>
#include <QCursor>
#include <QGuiApplication>
#include <QPoint>
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

// Puts the offscreen platform's pointer where no window is, so that a
// window a test shows next starts with nothing hovered. Call it once the
// application exists, before showing the window.
//
// The platform keeps a pointer of its own, at (10, 10) on its screen until
// something sets it; QTest's mouse events leave it there. It shows each
// window at (2, 2), inside the screen by the window frame's width, and
// sends the window an Enter at that pointer: (8, 8) in the window, on the
// first title of a menu bar. Qt Quick then hovers the item there, and on
// each frame after, one that has moved there, so a fresh window's first
// title was lit or not by when its first frames were drawn.
inline void movePointerOffTheWindows() {
    if (QGuiApplication::platformName() != QLatin1String("offscreen")) {
        return;
    }
    // Above and left of the screen, where the platform places no window.
    QCursor::setPos(QPoint(-1, -1));
}

// The test of the default, for a test slot of each binary that shows windows.
inline void verifyOffscreenPlatformByDefault() {
    if (!offscreenPlatformChosenByDefault()) {
        QSKIP("QT_QPA_PLATFORM or -platform chose the platform");
    }
    QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
}
