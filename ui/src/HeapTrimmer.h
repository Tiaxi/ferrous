// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include <functional>

// Gives the free memory inside the malloc arenas back to the system once the
// app settles after it released much of it: a track change, a queue that
// grew or shrank, or the main window leaving the screen. The fixed mmap
// threshold (HeapAllocator.h) returns large blocks as they are freed; this
// returns the pages of the small ones, such as library and queue strings and
// spectrogram ring blocks, which stay in the arenas otherwise.
//
// The trim runs on a worker thread of its own, never on the GUI thread:
// glibc walks every arena for it, taking a few to tens of milliseconds.
class HeapTrimmer : public QObject {
    Q_OBJECT

public:
    // How long the app must go without another request before a trim, so
    // that one trim follows a burst of changes, after their frees.
    static constexpr int kSettleMs = 5000;

    using TrimFunction = std::function<void()>;

    // Trims with malloc_trim where the C library is glibc, and does nothing
    // elsewhere.
    explicit HeapTrimmer(QObject *parent = nullptr);
    // For tests: `trim` runs in place of malloc_trim, `settleMs` in place of
    // kSettleMs. A null `trim` disables the trimmer.
    HeapTrimmer(TrimFunction trim, int settleMs, QObject *parent = nullptr);
    // Waits for a running trim.
    ~HeapTrimmer() override;

    bool enabled() const { return static_cast<bool>(m_trim); }
    // A trim is waiting for the app to settle, or is running.
    bool pending() const { return m_settle.isActive() || m_running; }

public slots:
    // Trims once kSettleMs passed without another request.
    void requestTrim();
    // Requests a trim when the queue's length changes; the first length it
    // is given is only remembered.
    void setQueueLength(int length);
    // Requests a trim when the main window stops being shown.
    void setWindowShown(bool shown);

signals:
    // A trim finished.
    void trimmed();

private:
    void settled();
    void finished();

    TrimFunction m_trim;
    QTimer m_settle;
    QThreadPool m_pool;
    int m_queueLength{-1};
    bool m_windowShown{false};
    bool m_running{false};
    bool m_requestedWhileRunning{false};
};
