// SPDX-License-Identifier: GPL-3.0-or-later

#include "WindowPresence.h"

#include <QEvent>

namespace {

const QString kSharedPresenceName = QStringLiteral("ferrousWindowPresence");

} // namespace

WindowPresence::WindowPresence(QObject *parent)
    : QObject(parent),
      m_isVisible([](const QWindow &window) {
          return window.isVisible()
              && window.visibility() != QWindow::Minimized
              && window.visibility() != QWindow::Hidden;
      }),
      m_isExposed([](const QWindow &window) { return window.isExposed(); }) {
    m_unexposedGrace.setSingleShot(true);
    m_unexposedGrace.setInterval(kUnexposedGraceMs);
    connect(&m_unexposedGrace, &QTimer::timeout, this, &WindowPresence::unexposedGraceEnded);
}

WindowPresence::~WindowPresence() {
    if (m_window) {
        m_window->removeEventFilter(this);
    }
}

WindowPresence *WindowPresence::of(QWindow *window) {
    if (window == nullptr) {
        return nullptr;
    }
    auto *presence = window->findChild<WindowPresence *>(
        kSharedPresenceName, Qt::FindDirectChildrenOnly);
    if (presence == nullptr) {
        presence = new WindowPresence(window);
        presence->setObjectName(kSharedPresenceName);
        presence->setWindow(window);
    }
    return presence;
}

void WindowPresence::setWindow(QWindow *window) {
    if (m_window == window) {
        return;
    }
    if (m_window) {
        m_window->removeEventFilter(this);
    }
    disconnect(m_visibleConnection);
    disconnect(m_visibilityConnection);
    disconnect(m_destroyedConnection);
    disconnect(m_screenConnection);
    m_unexposedGrace.stop();
    m_window = window;
    if (window != nullptr) {
        // Qt has no signal for exposure; it sends the window an expose
        // event after it has changed.
        window->installEventFilter(this);
        m_visibleConnection = connect(window, &QWindow::visibleChanged, this, &WindowPresence::update);
        m_visibilityConnection =
            connect(window, &QWindow::visibilityChanged, this, &WindowPresence::update);
        m_destroyedConnection = connect(window, &QObject::destroyed, this, [this]() {
            update();
            followScreen();
        });
        m_screenConnection = connect(window, &QWindow::screenChanged, this, &WindowPresence::followScreen);
    }
    emit windowChanged();
    update();
    followScreen();
}

void WindowPresence::followScreen() {
    QScreen *screen = m_window ? m_window->screen() : nullptr;
    if (screen != m_screen) {
        disconnect(m_refreshRateConnection);
        m_screen = screen;
        if (screen != nullptr) {
            m_refreshRateConnection = connect(
                screen, &QScreen::refreshRateChanged, this, &WindowPresence::updateScreenRefreshRate);
        }
    }
    updateScreenRefreshRate();
}

void WindowPresence::updateScreenRefreshRate() {
    const double rate = m_screen ? m_screen->refreshRate() : 0.0;
    if (rate != m_screenRefreshRate) {
        m_screenRefreshRate = rate;
        emit screenRefreshRateChanged();
    }
}

bool WindowPresence::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_window && event->type() == QEvent::Expose) {
        update();
    }
    return QObject::eventFilter(watched, event);
}

void WindowPresence::update() {
    QWindow *window = m_window;
    const bool visible = window != nullptr && m_isVisible(*window);
    bool shown = false;
    if (!visible) {
        m_unexposedGrace.stop();
    } else if (m_isExposed(*window)) {
        m_unexposedGrace.stop();
        shown = true;
    } else if (m_shown) {
        // Still shown until the grace ends, unless it is exposed again.
        if (!m_unexposedGrace.isActive()) {
            m_unexposedGrace.start();
        }
        shown = true;
    }
    if (shown != m_shown) {
        m_shown = shown;
        emit shownChanged();
    }
}

void WindowPresence::unexposedGraceEnded() {
    if (!m_shown || (m_window && m_isExposed(*m_window))) {
        return;
    }
    m_shown = false;
    emit shownChanged();
}
