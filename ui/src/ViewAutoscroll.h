// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMetaObject>
#include <QObject>
#include <QPointF>
#include <QPointer>

#include <functional>
#include <optional>

class QQuickItem;
class QQuickWindow;

// Autoscroll of a zoomed plot's view, as in Adobe Audition: a middle click
// on the plot starts it, and the pointer's distance left or right of the
// click moves the view that way, the faster the farther, with a dead zone
// around the click where it holds still. Another middle click anywhere in
// the window, Escape, the window losing focus, or the plot being hidden or
// leaving the window stops it. While it runs, the pointer is a horizontal
// resize cursor; nothing else shows it.
//
// It is the plot's second way to move its view, beside the middle-drag,
// and kept apart from it: the plot starts it on a middle click that did
// not drag, and is told through `scroll` how far to move the view each
// frame and through stopped() when it ends.
//
// The view moves with the frames of the plot's window: each frame shown
// moves it on by the time since the last, and asks for the next only while
// the pointer is out of the dead zone and the view moved, so a view held
// still, or one at an end of the track, asks for no frames.
//
// It lives on the GUI thread.
class ViewAutoscroll : public QObject {
    Q_OBJECT

public:
    // The view moves by `viewsPerSecond` of its width a second, positive
    // towards the end of the track, for `seconds`: returns whether it
    // moved.
    using Scroll = std::function<bool(double viewsPerSecond, double seconds)>;

    // How far, in logical pixels, the pointer may be from the click with
    // the view held still.
    static constexpr double kDeadZonePixels = 12.0;
    // The fastest the view moves, in its widths a second: a pointer at the
    // plot's edge, from a click in its middle, moves it by one.
    static constexpr double kMaxViewsPerSecond = 2.0;
    // The most time one frame moves the view by, so that a frame late after
    // a stall moves it no further than this.
    static constexpr double kMaxFrameSeconds = 0.1;

    // The speed, in the view's widths a second, for the pointer `offset`
    // logical pixels right (positive) or left of the click on a plot
    // `plotWidth` wide: none in the dead zone, then in proportion to the
    // distance past it, one view a second at half the plot's width from the
    // click, up to kMaxViewsPerSecond.
    static double viewsPerSecond(double offset, double plotWidth);
    // Whether a middle press at `pressed` and its release at `released`, in
    // logical pixels, are a click rather than a drag: the pointer moved less
    // than the platform's drag distance.
    static bool isClick(QPointF pressed, QPointF released);

    // `nowNs` is the plot's monotonic clock, which tests set.
    ViewAutoscroll(QQuickItem *plot, Scroll scroll, std::function<qint64()> nowNs);
    ~ViewAutoscroll() override;

    bool active() const { return m_active; }
    // Starts at `position`, the click in the plot's coordinates.
    void start(QPointF position);
    // Stops it, and tells stopped(), if it runs.
    void stop();
    // The speed now, as the pointer is: none while the window does not
    // have it.
    double currentViewsPerSecond() const;

    // Whether the middle press of `timestamp` stopped it: the plot, which
    // that press reached, starts no autoscroll as it is released. Asked once
    // for each middle press the plot takes, as it takes it.
    bool takeStopPress(quint64 timestamp);
    // The view may move again where it could not, such as at an end of the
    // track after a zoom: asks for a frame if the pointer would move it.
    void resume();

    // The pointer is at `scenePosition`, in the window: from the window's
    // events, and from tests.
    void pointerMoved(QPointF scenePosition);
    // The pointer left the window, or came back.
    void setPointerInWindow(bool inWindow);
    // A frame of the window was shown: moves the view by the time since the
    // last, and asks for the next.
    void frameShown();

signals:
    // It stopped: the plot arms its re-attach point, as at the end of a
    // drag.
    void stopped();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void watchWindow(QQuickWindow *window);
    void unwatchWindow();
    // Asks for a frame once the pointer moves the view, if no frame is on
    // its way already.
    void wake();

    QQuickItem *m_plot;
    Scroll m_scroll;
    std::function<qint64()> m_nowNs;
    bool m_active{false};
    // Whether the override cursor is the autoscroll's.
    bool m_cursorSet{false};
    // The click and the pointer, in the window's coordinates.
    QPointF m_originScene;
    QPointF m_pointerScene;
    bool m_pointerInWindow{true};
    // When the view last moved, or 0 while it holds still and asks for no
    // frames.
    qint64 m_lastFrameNs{0};
    // The middle press that stopped it: its release, and a double click of
    // it, are its own, not the window's.
    bool m_swallowMiddleRelease{false};
    // The timestamp of the middle press that last stopped it.
    std::optional<quint64> m_stopPressTimestamp;
    QPointer<QQuickWindow> m_window;
    QMetaObject::Connection m_frameConnection;
    QMetaObject::Connection m_focusConnection;
    QMetaObject::Connection m_visibleConnection;
    QMetaObject::Connection m_windowConnection;
};
