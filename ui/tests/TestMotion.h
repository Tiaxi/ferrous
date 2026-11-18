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
class SteppedAnimationDriver : public QAnimationDriver {
public:
    SteppedAnimationDriver() { install(); }
    ~SteppedAnimationDriver() override { uninstall(); }
    SteppedAnimationDriver(const SteppedAnimationDriver &) = delete;
    SteppedAnimationDriver &operator=(const SteppedAnimationDriver &) = delete;

    qint64 elapsed() const override { return m_elapsed; }

    // Lets `ms` of animation time pass, a frame of 16 ms at a time, as the
    // window would draw them. Whether this driver ran the animations: Qt
    // starts the installed driver once one runs, so it is false when Qt
    // left it out, and the animations ran on real time instead.
    [[nodiscard]] bool step(int ms) {
        bool drove = false;
        for (int done = 0; done < ms; done += kFrame) {
            // The animations that started since: they run from the next
            // frame on.
            QCoreApplication::processEvents();
            drove = drove || isRunning();
            m_elapsed += std::min(kFrame, ms - done);
            advance();
        }
        return drove;
    }

private:
    static constexpr int kFrame = 16;
    qint64 m_elapsed = 0;
};
