// SPDX-License-Identifier: GPL-3.0-or-later

#include "WindowActivation.h"

#include <QGuiApplication>
#include <QPointer>
#include <QWindow>

namespace {

QPointer<QWindow> &mainActivationWindow() {
    static QPointer<QWindow> window;
    return window;
}

} // namespace

void setMainActivationWindow(QWindow *window) {
    mainActivationWindow() = window;
}

void provideActivationToken(const QString &platformName, const QString &activationToken) {
    if (activationToken.isEmpty() || !platformName.startsWith(QLatin1String("wayland"))) {
        return;
    }
    // Qt has no public API for this; its xdg-shell integration reads this
    // variable in requestActivate(), the same hand-off Qt's own tray icon uses
    // for ProvideXdgActivationToken.
    qputenv("XDG_ACTIVATION_TOKEN", activationToken.toUtf8());
}

bool activateMainWindow(const QString &activationToken) {
    QWindow *window = mainActivationWindow();
    if (window == nullptr) {
        return false;
    }
    if (window->visibility() == QWindow::Minimized) {
        window->showNormal();
    } else if (!window->isVisible()) {
        window->show();
    }
    window->raise();
    provideActivationToken(QGuiApplication::platformName(), activationToken);
    window->requestActivate();
    return true;
}
