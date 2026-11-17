// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputModality.h"

#include <QCoreApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QPointingDevice>
#include <QPointerEvent>
#include <qqml.h>

namespace {

bool isModifierKey(int key) {
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Meta:
    case Qt::Key_Alt:
    case Qt::Key_AltGr:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_Hyper_L:
    case Qt::Key_Hyper_R:
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
    case Qt::Key_ScrollLock:
        return true;
    default:
        return false;
    }
}

bool isFromTouchScreen(const QPointerEvent *event) {
    const QPointingDevice *device = event->pointingDevice();
    return device != nullptr && device->type() == QInputDevice::DeviceType::TouchScreen;
}

} // namespace

InputModality::InputModality(QObject *parent) : QObject(parent) {
    if (QCoreApplication *app = QCoreApplication::instance()) {
        app->installEventFilter(this);
    }
}

InputModality::~InputModality() {
    if (QCoreApplication *app = QCoreApplication::instance()) {
        app->removeEventFilter(this);
    }
}

void InputModality::registerQmlTypes() {
    qmlRegisterSingletonType<InputModality>(
        "FerrousUi", 1, 0, "InputModality",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new InputModality(); });
    qmlRegisterType<WindowPointer>("FerrousUi", 1, 0, "WindowPointer");
}

bool InputModality::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress:
        if (!isModifierKey(static_cast<QKeyEvent *>(event)->key())) {
            setKeyboard(true);
        }
        break;
    case QEvent::FocusIn:
        switch (static_cast<QFocusEvent *>(event)->reason()) {
        case Qt::TabFocusReason:
        case Qt::BacktabFocusReason:
        case Qt::ShortcutFocusReason:
            setKeyboard(true);
            break;
        case Qt::MouseFocusReason:
            setKeyboard(false);
            break;
        default:
            break;
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::TouchBegin:
    case QEvent::TabletPress:
        setKeyboard(false);
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void InputModality::setKeyboard(bool keyboard) {
    if (m_keyboard != keyboard) {
        m_keyboard = keyboard;
        emit keyboardChanged();
    }
}

WindowPointer::WindowPointer(QObject *parent) : QObject(parent) {}

void WindowPointer::setWindow(QWindow *window) {
    if (m_window == window) {
        return;
    }
    if (m_window) {
        m_window->removeEventFilter(this);
    }
    m_window = window;
    setTouch(false);
    if (m_window) {
        m_window->installEventFilter(this);
    }
    emit windowChanged();
}

// The window's own events, as it filters no other object's: the items
// under a pointer get the events Qt makes for them from these.
bool WindowPointer::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
        if (isFromTouchScreen(static_cast<const QPointerEvent *>(event))) {
            setTouch(true);
        }
        break;
    case QEvent::MouseMove:
    case QEvent::Enter:
        if (m_touch && !isFromTouchScreen(static_cast<const QPointerEvent *>(event))) {
            setTouch(false);
        }
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void WindowPointer::setTouch(bool touch) {
    if (m_touch != touch) {
        m_touch = touch;
        emit touchChanged();
    }
}
