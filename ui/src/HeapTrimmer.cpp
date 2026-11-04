// SPDX-License-Identifier: GPL-3.0-or-later

#include "HeapTrimmer.h"

#include "HeapAllocator.h"

#include <utility>

namespace {

HeapTrimmer::TrimFunction systemTrim() {
    if (!FerrousHeap::canTrim()) {
        return {};
    }
    return [] { FerrousHeap::trimFreedHeap(); };
}

} // namespace

HeapTrimmer::HeapTrimmer(QObject *parent)
    : HeapTrimmer(systemTrim(), kSettleMs, parent) {}

HeapTrimmer::HeapTrimmer(TrimFunction trim, int settleMs, QObject *parent)
    : QObject(parent), m_trim(std::move(trim)) {
    m_settle.setSingleShot(true);
    m_settle.setInterval(settleMs);
    connect(&m_settle, &QTimer::timeout, this, &HeapTrimmer::settled);
    m_pool.setMaxThreadCount(1);
    m_pool.setObjectName(QStringLiteral("ferrous-heap-trim"));
}

HeapTrimmer::~HeapTrimmer() {
    m_settle.stop();
    m_pool.waitForDone();
}

void HeapTrimmer::requestTrim() {
    if (!enabled()) {
        return;
    }
    if (m_running) {
        m_requestedWhileRunning = true;
        return;
    }
    m_settle.start();
}

void HeapTrimmer::setQueueLength(int length) {
    const int previous = std::exchange(m_queueLength, length);
    if (previous >= 0 && previous != length) {
        requestTrim();
    }
}

void HeapTrimmer::setWindowShown(bool shown) {
    const bool wasShown = std::exchange(m_windowShown, shown);
    if (wasShown && !shown) {
        requestTrim();
    }
}

void HeapTrimmer::settled() {
    m_running = true;
    m_pool.start([this, trim = m_trim] {
        trim();
        // The destructor waits for this task, so the trimmer is alive here;
        // the queued call is dropped if it is destroyed before the call runs.
        QMetaObject::invokeMethod(this, &HeapTrimmer::finished, Qt::QueuedConnection);
    });
}

void HeapTrimmer::finished() {
    m_running = false;
    if (std::exchange(m_requestedWhileRunning, false)) {
        m_settle.start();
    }
    emit trimmed();
}
