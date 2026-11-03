// SPDX-License-Identifier: GPL-3.0-or-later

#include "PlaybackFrameScheduler.h"

#include "PlaybackClock.h"

#include <QVector>

#include <algorithm>

PlaybackFrameScheduler::PlaybackFrameScheduler(QObject *parent)
    : QObject(parent),
      m_nowNs(monotonicNowNs) {
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &PlaybackFrameScheduler::fire);
}

PlaybackFrameScheduler *PlaybackFrameScheduler::of(QObject *owner) {
    auto *scheduler = owner->findChild<PlaybackFrameScheduler *>(
        QString(), Qt::FindDirectChildrenOnly);
    return scheduler != nullptr ? scheduler : new PlaybackFrameScheduler(owner);
}

void PlaybackFrameScheduler::schedule(Client *client, qint64 dueNs, qint64 nowNs) {
    // Whole milliseconds, rounded up: the frame is not due before.
    const qint64 dueMs = dueNs / 1'000'000 + (dueNs % 1'000'000 > 0 ? 1 : 0);
    const auto it = m_dueMs.constFind(client);
    if (it != m_dueMs.cend() && it.value() == dueMs && m_timer.isActive()) {
        return;
    }
    m_dueMs.insert(client, dueMs);
    arm(nowNs);
}

void PlaybackFrameScheduler::cancel(const Client *client) {
    if (m_dueMs.remove(const_cast<Client *>(client)) == 0) {
        return;
    }
    if (m_dueMs.isEmpty()) {
        m_timer.stop();
    }
}

qint64 PlaybackFrameScheduler::dueNs(const Client *client) const {
    const auto it = m_dueMs.constFind(const_cast<Client *>(client));
    return it != m_dueMs.cend() ? it.value() * 1'000'000 : -1;
}

void PlaybackFrameScheduler::arm(qint64 nowNs) {
    if (m_dueMs.isEmpty()) {
        m_timer.stop();
        return;
    }
    const qint64 firstMs = *std::min_element(m_dueMs.cbegin(), m_dueMs.cend());
    if (m_timer.isActive() && m_armedMs == firstMs) {
        return;
    }
    const qint64 untilNs = std::max<qint64>(0, firstMs * 1'000'000 - nowNs);
    const qint64 untilMs = untilNs / 1'000'000 + (untilNs % 1'000'000 > 0 ? 1 : 0);
    m_armedMs = firstMs;
    m_timer.start(static_cast<int>(std::min<qint64>(untilMs, 3'600'000)));
}

void PlaybackFrameScheduler::fire() {
    // The timer fires at the millisecond it was armed for or after it, as
    // late as the event loop lets it: every frame due by then is asked for
    // now, in the same frame of the window.
    const qint64 nowNs = std::max(m_nowNs(), m_armedMs * 1'000'000);
    const qint64 reachedMs = nowNs / 1'000'000;
    QVector<Client *> due;
    for (auto it = m_dueMs.begin(); it != m_dueMs.end();) {
        if (it.value() <= reachedMs) {
            due.push_back(it.key());
            it = m_dueMs.erase(it);
        } else {
            ++it;
        }
    }
    arm(nowNs);
    for (Client *client : due) {
        client->requestPlaybackFrame();
    }
}
