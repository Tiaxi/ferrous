// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWindow>

#include <functional>

// Whether a window is on screen, for what draws or analyses playback only
// while someone can see it: the playback clock's frame updates, spectral
// analysis, the level meter and the playback frames of the views.
//
// A window is shown while it is visible, not minimized, and exposed. On
// Wayland, xdg-shell has no minimized state, so a minimized window keeps
// reporting its last visibility; Qt then marks it unexposed once the
// compositor stops answering its frame callbacks, as it does for a
// minimized window, one on another virtual desktop, or one whose display
// is off. A window that stops being exposed counts as shown for a while
// longer, so that a brief unexposure, such as a desktop switch or a live
// resize, restarts nothing; one that is exposed again is shown at once.
class WindowPresence : public QObject {
    Q_OBJECT
    Q_PROPERTY(QWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(bool shown READ shown NOTIFY shownChanged)

public:
    // How long an unexposed window still counts as shown.
    static constexpr int kUnexposedGraceMs = 2000;

    explicit WindowPresence(QObject *parent = nullptr);
    ~WindowPresence() override;

    // The presence of `window`, shared by the C++ code that follows it:
    // made the first time it is asked for, and owned by the window.
    static WindowPresence *of(QWindow *window);

    QWindow *window() const { return m_window; }
    void setWindow(QWindow *window);
    bool shown() const { return m_shown; }

signals:
    void windowChanged();
    void shownChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void update();
    void unexposedGraceEnded();

    QPointer<QWindow> m_window;
    // Whether the window is visible and not minimized, and whether it is
    // exposed; tests replace them.
    std::function<bool(const QWindow &)> m_isVisible;
    std::function<bool(const QWindow &)> m_isExposed;
    QMetaObject::Connection m_visibleConnection;
    QMetaObject::Connection m_visibilityConnection;
    QMetaObject::Connection m_destroyedConnection;
    // Runs while an unexposed window still counts as shown.
    QTimer m_unexposedGrace;
    bool m_shown{false};
};
