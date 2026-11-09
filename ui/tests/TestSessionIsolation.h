// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QtGlobal>

#if defined(QT_QUICKCONTROLS2_LIB)
#include <QIcon>
#include <QQuickStyle>
#include <QString>
#endif

// A test program runs the same in a desktop session as without one, as in
// CI. In a KDE Plasma session, $XDG_CURRENT_DESKTOP and
// $QT_QPA_PLATFORMTHEME load the KDE platform theme even offscreen. It
// reads the session's colours, fonts, icon theme and animation speed,
// chooses the controls and widget styles, and draws icons itself. Call
// this in main(), before the application object is made. Without a style
// of its own, a test's controls then have Qt's default style, Fusion.
inline void isolateFromDesktopSession() {
    qunsetenv("XDG_CURRENT_DESKTOP");
    qunsetenv("QT_QPA_PLATFORMTHEME");
}

#if defined(QT_QUICKCONTROLS2_LIB)
// As isolateFromDesktopSession(), with the controls style that the app sets
// (src/main.cpp), which it needs to start (docs/INSTALL.md), and Breeze, the
// icon theme that a KDE session's platform theme sets.
inline void isolateFromDesktopSessionWithAppLook() {
    isolateFromDesktopSession();
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    QIcon::setThemeName(QStringLiteral("breeze"));
}
#endif
