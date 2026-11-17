// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputModality.h"

#include <QCoreApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QTouchEvent>
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

// A mouse's or touchpad's own event to a window. Qt sends a touch on as
// mouse events, with the touchscreen as their device, to a window that
// took none of it and to items that take only the mouse's; and the items
// under the pointer get a window's mouse event after it.
bool isMouseEventToWindow(const QObject *watched, const QEvent *event) {
    return watched->isWindowType() && !isFromTouchScreen(static_cast<const QMouseEvent *>(event));
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

void InputModality::registerQmlType() {
    qmlRegisterSingletonType<InputModality>(
        "FerrousUi", 1, 0, "InputModality",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new InputModality(); });
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
    case QEvent::MouseMove:
        if (m_touch && isMouseEventToWindow(watched, event)) {
            setTouch(false);
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        setKeyboard(false);
        if (m_touch && isMouseEventToWindow(watched, event)) {
            setTouch(false);
        }
        break;
    case QEvent::TouchBegin:
        setKeyboard(false);
        if (isFromTouchScreen(static_cast<const QTouchEvent *>(event))) {
            setTouch(true);
        }
        break;
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

void InputModality::setTouch(bool touch) {
    if (m_touch != touch) {
        m_touch = touch;
        emit touchChanged();
    }
}
