// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPointer>
#include <QWindow>

class QEvent;

// Whether the user is working the app with the keyboard, for focus marks
// that show only then, as the lists' current-row ring: the keyboard takes
// over with a key press, other than a modifier alone, and with focus that
// Tab, Backtab or a shortcut moved; the pointer takes over with a press.
// Focus moved for another reason, as a window's activation or a popup's,
// leaves it as it was, so nothing shows at launch.
//
// One app-wide event filter that reads only the event types it follows.
// QML has it as the FerrousUi singleton InputModality.
class InputModality : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool keyboard READ keyboard NOTIFY keyboardChanged)

public:
    explicit InputModality(QObject *parent = nullptr);
    ~InputModality() override;

    bool keyboard() const { return m_keyboard; }

    // Registers the singleton, one per engine, and WindowPointer with the
    // FerrousUi module.
    static void registerQmlTypes();

signals:
    void keyboardChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setKeyboard(bool keyboard);

    bool m_keyboard = false;
};

// Whether a window's pointer is a finger on a touchscreen, for hover that
// only a mouse or touchpad has; QML has it as FerrousUi's WindowPointer.
//
// Qt Quick keeps one pointer position for a window, the mouse's or a
// touch's first point, whichever came last. At each frame, and as a
// touch's point moves, it hovers what is under that position with an event
// that looks like the mouse's, which a HoverHandler's acceptedDevices
// cannot tell from the mouse's own hover. Off while `touch` is set, the
// handler has only the mouse's.
//
// As Qt takes the position: the pointer is a finger from a touchscreen's
// press or move in the window, and the mouse's again when a mouse or
// touchpad moves in the window or enters it, where Qt hovers what is under
// it at once. The mouse events that Qt makes of a touch have the
// touchscreen as their device, and are the finger's. A press or a wheel
// changes nothing: Qt takes no position from them and hovers nothing for
// them, so under a held finger the next frame's hover is still the
// finger's. Nor do other windows' events, as each has a position of its
// own.
class WindowPointer : public QObject {
    Q_OBJECT
    Q_PROPERTY(QWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool touch READ touch NOTIFY touchChanged)

public:
    explicit WindowPointer(QObject *parent = nullptr);

    QWindow *window() const { return m_window; }
    void setWindow(QWindow *window);
    bool touch() const { return m_touch; }

signals:
    void windowChanged();
    void touchChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setTouch(bool touch);

    QPointer<QWindow> m_window;
    bool m_touch = false;
};
