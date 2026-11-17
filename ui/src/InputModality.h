// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

class QEvent;

// Whether the user is working the app with the keyboard, for focus marks
// that show only then, as the lists' current-row ring: the keyboard takes
// over with a key press, other than a modifier alone, and with focus that
// Tab, Backtab or a shortcut moved; the pointer takes over with a press.
// Focus moved for another reason, as a window's activation or a popup's,
// leaves it as it was, so nothing shows at launch.
//
// And whether the pointer is a finger on a touchscreen, for hover that only
// a mouse or touchpad has: from a touch's press until a mouse or touchpad
// next moves or presses in a window. Qt Quick takes a held touch's point
// for the mouse's position and, at each frame, hovers what is under it
// with an event that looks like the mouse's, so a HoverHandler's
// acceptedDevices cannot tell them apart.
//
// One app-wide event filter that reads only the event types it follows.
// QML has it as the FerrousUi singleton InputModality.
class InputModality : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool keyboard READ keyboard NOTIFY keyboardChanged)
    Q_PROPERTY(bool touch READ touch NOTIFY touchChanged)

public:
    explicit InputModality(QObject *parent = nullptr);
    ~InputModality() override;

    bool keyboard() const { return m_keyboard; }
    bool touch() const { return m_touch; }

    // Registers the singleton with the FerrousUi module, one per engine.
    static void registerQmlType();

signals:
    void keyboardChanged();
    void touchChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setKeyboard(bool keyboard);
    void setTouch(bool touch);

    bool m_keyboard = false;
    bool m_touch = false;
};
