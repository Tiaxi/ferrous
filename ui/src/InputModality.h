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
// One app-wide event filter that reads only the event types it follows.
// QML has it as the FerrousUi singleton InputModality.
class InputModality : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool keyboard READ keyboard NOTIFY keyboardChanged)

public:
    explicit InputModality(QObject *parent = nullptr);
    ~InputModality() override;

    bool keyboard() const { return m_keyboard; }

    // Registers the singleton with the FerrousUi module, one per engine.
    static void registerQmlType();

signals:
    void keyboardChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setKeyboard(bool keyboard);

    bool m_keyboard = false;
};
