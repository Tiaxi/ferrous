// SPDX-License-Identifier: GPL-3.0-or-later

#include "ViewAutoscroll.h"

#include <QCursor>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <utility>

double ViewAutoscroll::viewsPerSecond(double offset, double plotWidth) {
    const double beyond = std::abs(offset) - kDeadZonePixels;
    if (!std::isfinite(beyond) || beyond <= 0.0) {
        return 0.0;
    }
    // Half the plot's width from the click, past the dead zone, is one view
    // a second.
    const double reference = std::max(1.0, 0.5 * plotWidth - kDeadZonePixels);
    return std::copysign(std::min(kMaxViewsPerSecond, beyond / reference), offset);
}

bool ViewAutoscroll::isClick(QPointF pressed, QPointF released) {
    return (released - pressed).manhattanLength()
        < QGuiApplication::styleHints()->startDragDistance();
}

ViewAutoscroll::ViewAutoscroll(QQuickItem *plot, Scroll scroll, std::function<qint64()> nowNs)
    : m_plot(plot)
    , m_scroll(std::move(scroll))
    , m_nowNs(std::move(nowNs)) {
    // A plot hidden, such as by a switch to the other plot, or moved to
    // another window, such as a viewer's, stops it.
    m_visibleConnection = connect(plot, &QQuickItem::visibleChanged, this, [this]() {
        if (!m_plot->isVisible()) {
            stop();
        }
    });
    m_windowConnection = connect(plot, &QQuickItem::windowChanged, this, [this]() { stop(); });
}

ViewAutoscroll::~ViewAutoscroll() {
    // The plot is going: nothing to tell it.
    m_active = false;
    m_swallowMiddleRelease = false;
    if (m_cursorSet) {
        QGuiApplication::restoreOverrideCursor();
        m_cursorSet = false;
    }
    unwatchWindow();
}

void ViewAutoscroll::start(QPointF position) {
    if (m_active) {
        return;
    }
    m_active = true;
    m_originScene = m_plot->mapToScene(position);
    m_pointerScene = m_originScene;
    m_pointerInWindow = true;
    m_lastFrameNs = 0;
    m_swallowMiddleRelease = false;
    watchWindow(m_plot->window());
    if (!m_cursorSet) {
        QGuiApplication::setOverrideCursor(QCursor(Qt::SizeHorCursor));
        m_cursorSet = true;
    }
}

void ViewAutoscroll::stop() {
    if (!m_active) {
        return;
    }
    m_active = false;
    m_lastFrameNs = 0;
    if (m_cursorSet) {
        QGuiApplication::restoreOverrideCursor();
        m_cursorSet = false;
    }
    disconnect(m_frameConnection);
    disconnect(m_focusConnection);
    if (!m_swallowMiddleRelease) {
        unwatchWindow();
    }
    emit stopped();
}

double ViewAutoscroll::currentViewsPerSecond() const {
    if (!m_active || !m_pointerInWindow) {
        return 0.0;
    }
    return viewsPerSecond(m_pointerScene.x() - m_originScene.x(), m_plot->width());
}

bool ViewAutoscroll::stoppedByPress(quint64 timestamp) const {
    return m_stopPressTimestamp == timestamp;
}

void ViewAutoscroll::resume() {
    wake();
}

void ViewAutoscroll::pointerMoved(QPointF scenePosition) {
    m_pointerScene = scenePosition;
    m_pointerInWindow = true;
    wake();
}

void ViewAutoscroll::setPointerInWindow(bool inWindow) {
    m_pointerInWindow = inWindow;
    // Out of the window, the view holds still from the next frame on.
    wake();
}

void ViewAutoscroll::wake() {
    if (!m_active || m_lastFrameNs != 0 || currentViewsPerSecond() == 0.0) {
        return;
    }
    m_lastFrameNs = m_nowNs();
    m_plot->update();
}

void ViewAutoscroll::frameShown() {
    if (!m_active || m_lastFrameNs == 0) {
        return;
    }
    const double speed = currentViewsPerSecond();
    if (speed == 0.0) {
        // Held still: no frames until the pointer moves the view again.
        m_lastFrameNs = 0;
        return;
    }
    const qint64 nowNs = m_nowNs();
    const double seconds = std::min(
        kMaxFrameSeconds, static_cast<double>(std::max<qint64>(0, nowNs - m_lastFrameNs)) * 1e-9);
    if (seconds <= 0.0) {
        m_plot->update();
        return;
    }
    m_lastFrameNs = nowNs;
    if (!m_scroll(speed, seconds)) {
        // At an end of the track: nothing moves until the pointer turns it.
        m_lastFrameNs = 0;
        return;
    }
    m_plot->update();
}

void ViewAutoscroll::watchWindow(QQuickWindow *window) {
    unwatchWindow();
    m_window = window;
    if (window == nullptr) {
        return;
    }
    window->installEventFilter(this);
    // The frame clock: each frame shown moves the view on.
    m_frameConnection = connect(
        window, &QQuickWindow::frameSwapped, this, &ViewAutoscroll::frameShown,
        Qt::QueuedConnection);
    // Focus moving to any other window stops it, also to a dialog of this
    // window's, with which Qt still counts this one as active.
    m_focusConnection = connect(
        qGuiApp, &QGuiApplication::focusWindowChanged, this, [this](QWindow *focus) {
            if (focus != m_window) {
                stop();
            }
        });
}

void ViewAutoscroll::unwatchWindow() {
    disconnect(m_frameConnection);
    disconnect(m_focusConnection);
    if (m_window != nullptr) {
        m_window->removeEventFilter(this);
    }
    m_window = nullptr;
}

bool ViewAutoscroll::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_window) {
        return QObject::eventFilter(watched, event);
    }
    switch (event->type()) {
    case QEvent::MouseMove:
        if (m_active) {
            pointerMoved(static_cast<QMouseEvent *>(event)->scenePosition());
        }
        break;
    case QEvent::Enter:
        if (m_active) {
            // Where it came back, which may be the other side of the click.
            pointerMoved(static_cast<QEnterEvent *>(event)->scenePosition());
        }
        break;
    case QEvent::Leave:
        if (m_active) {
            setPointerInWindow(false);
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::MiddleButton) {
            break;
        }
        if (m_active) {
            // Another middle press, anywhere in the window, stops it. On the
            // plot, it goes on to the plot, so that a drag from it pans as
            // ever; the plot does not start autoscroll again as it is
            // released (see stoppedByPress). Elsewhere it is its own.
            m_stopPressTimestamp = mouse->timestamp();
            if (m_plot->contains(m_plot->mapFromScene(mouse->scenePosition()))) {
                stop();
                return false;
            }
            m_swallowMiddleRelease = true;
            stop();
            return true;
        }
        if (m_swallowMiddleRelease) {
            return true;
        }
        break;
    }
    case QEvent::MouseButtonRelease:
        if (static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton
            && m_swallowMiddleRelease) {
            m_swallowMiddleRelease = false;
            if (!m_active) {
                unwatchWindow();
            }
            return true;
        }
        break;
    case QEvent::ShortcutOverride:
        // Escape stops it, before a shortcut of the window, such as a
        // viewer's, takes it.
        if (m_active && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            event->accept();
            return true;
        }
        break;
    case QEvent::KeyPress:
        if (m_active && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            stop();
            return true;
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
