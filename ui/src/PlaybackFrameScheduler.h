// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>

#include <functional>

// Wakes the items of a window that draw playback, such as the spectrogram
// panes and the waveform editor, for their next playback frames, with one
// timer for all of them. Frames are due on a grid of whole milliseconds of
// the monotonic clock, rounded up, so items whose pixels change at the
// same time ask for the same frame of the window.
class PlaybackFrameScheduler : public QObject {
    Q_OBJECT

public:
    // An item the scheduler wakes. It cancels its frame before it goes.
    class Client {
    public:
        // Asks for the frame that is due.
        virtual void requestPlaybackFrame() = 0;

    protected:
        Client() = default;
        Client(const Client &) = default;
        Client &operator=(const Client &) = default;
        ~Client() = default;
    };

    explicit PlaybackFrameScheduler(QObject *parent = nullptr);

    // The scheduler of `owner`, a window, or an item outside one: made
    // the first time it is asked for, and owned by `owner`.
    static PlaybackFrameScheduler *of(QObject *owner);

    // Asks for `client`'s frame at dueNs, nowNs being now, in place of the
    // one it asked for before.
    void schedule(Client *client, qint64 dueNs, qint64 nowNs);
    void cancel(const Client *client);
    // The time `client`'s frame is due at, rounded up to its millisecond,
    // or -1 for none.
    qint64 dueNs(const Client *client) const;
    bool timerActive() const { return m_timer.isActive(); }

private:
    void arm(qint64 nowNs);
    void fire();

    // Now on the monotonic clock; tests set their own.
    std::function<qint64()> m_nowNs;
    QTimer m_timer;
    QHash<Client *, qint64> m_dueMs;
    // The millisecond the timer fires for.
    qint64 m_armedMs{0};
};
