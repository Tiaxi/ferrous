#include "WindowActivation.h"

#include <QGuiApplication>
#include <QWindow>

bool activateTopLevelWindow() {
    const auto windows = QGuiApplication::topLevelWindows();
    for (QWindow *window : windows) {
        if (window == nullptr) {
            continue;
        }
        if (window->visibility() == QWindow::Minimized) {
            window->showNormal();
        } else if (!window->isVisible()) {
            window->show();
        }
        window->raise();
        window->requestActivate();
        return true;
    }
    return false;
}
