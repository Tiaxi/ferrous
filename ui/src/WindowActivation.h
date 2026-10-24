// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

class QWindow;

// Registers the window that MPRIS Raise and single-instance requests bring
// forward. Viewer windows are never raised in its place.
void setMainActivationWindow(QWindow *window);

// Shows, raises, and activates the main window. On Wayland, activationToken is
// the xdg-activation token of the process that asked for activation, which
// lets the compositor grant focus. Returns false if no main window is set.
bool activateMainWindow(const QString &activationToken = QString());

// Hands an xdg-activation token to Qt's Wayland integration, which uses and
// clears it on the next QWindow::requestActivate(). Ignored on other platforms.
void provideActivationToken(const QString &platformName, const QString &activationToken);
