// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAnimationDriver>
#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQuickWindow>
#include <QtTest/QtTest>

#include <algorithm>
#include <utility>

// Watching what a window draws frame by frame, and stepping animation time.

// Calls `sample` after each frame that `window` draws, in the window's
// thread, until stop() or the end of its scope: what it captures by
// reference must live as long.
class FrameSampler {
public:
    template <typename Sample>
    FrameSampler(QQuickWindow *window, Sample sample)
        : m_connection(QObject::connect(window, &QQuickWindow::frameSwapped, window, std::move(sample))) {}
    ~FrameSampler() { stop(); }
    FrameSampler(const FrameSampler &) = delete;
    FrameSampler &operator=(const FrameSampler &) = delete;

    void stop() { QObject::disconnect(m_connection); }

private:
    QMetaObject::Connection m_connection;
};

// The frames `window` draws over `ms` of wall time.
inline int framesOver(QQuickWindow *window, int ms) {
    int frames = 0;
    const FrameSampler counter(window, [&frames]() { ++frames; });
    QTest::qWait(ms);
    return frames;
}

// The time of every animation of the GUI thread, while it lives: it stands
// still but for the steps the test takes, however busy the machine. For the
// lists' transitions, whose animations cannot be paused or set to a point,
// and for checks along a whole fade, which a slow frame would skip on real
// time.
// Qt takes it only while no other driver is installed: the test's main()
// must keep Qt Quick on the basic render loop, whose window installs none.
// Qt also leaves it out while only pauses run, such as a PauseAnimation
// with no other animation beside it: it times those on real time, and
// step() returns false.
class SteppedAnimationDriver : public QAnimationDriver {
public:
    SteppedAnimationDriver() {
        install();
        // Qt starts the driver at once where animations already run, such
        // as a window's own from its start. They ran on real time, and take
        // what has passed of it since their last frame here: Qt otherwise
        // gives up to 50 ms of it to the animations that start next, which
        // then end that much before the steps of their duration.
        if (isRunning()) {
            advance();
        }
    }
    ~SteppedAnimationDriver() override { uninstall(); }
    SteppedAnimationDriver(const SteppedAnimationDriver &) = delete;
    SteppedAnimationDriver &operator=(const SteppedAnimationDriver &) = delete;

    // Since Qt last started the driver, which is what Qt adds to its own
    // time at that start: it stops the driver while nothing runs, and while
    // only pauses do.
    qint64 elapsed() const override { return m_elapsed - m_startedAt; }

    // Lets `ms` of animation time pass, a frame of 16 ms at a time, as the
    // window would draw them. Whether this driver ran the animations: Qt
    // starts the installed driver once one runs, so it is false when Qt
    // left it out, and the animations ran on real time instead.
    [[nodiscard]] bool step(int ms) {
        bool drove = false;
        for (int done = 0; done < ms; done += kFrame) {
            // The animations that started since: they run from the next
            // frame on. Qt starts them, and this driver for the first of
            // them, over two queued calls, the second posted by the first,
            // and a pass of the event loop delivers one.
            QCoreApplication::processEvents();
            if (!isRunning()) {
                QCoreApplication::processEvents();
            }
            drove = drove || isRunning();
            m_elapsed += std::min(kFrame, ms - done);
            advance();
        }
        return drove;
    }

protected:
    void start() override {
        if (!isRunning()) {
            m_startedAt = m_elapsed;
        }
        QAnimationDriver::start();
    }

private:
    static constexpr int kFrame = 16;
    qint64 m_elapsed = 0;
    qint64 m_startedAt = 0;
};
