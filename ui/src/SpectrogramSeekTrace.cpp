// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramSeekTrace.h"

#include <QDateTime>
#include <QtGlobal>

#include <atomic>
#include <cstdio>

namespace {
constexpr qint64 kSeekTraceWindowMs = 1800;

std::atomic<quint64> g_seekGeneration{0};
std::atomic<qint64> g_seekStartedAtMs{0};
std::atomic<double> g_seekTargetSeconds{0.0};
} // namespace

bool SpectrogramSeekTrace::runtimeEnabled() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    return qEnvironmentVariableIsSet("FERROUS_PROFILE_UI")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE");
#else
    return false;
#endif
}

void SpectrogramSeekTrace::noteSeekIssued(double targetSeconds) {
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    const quint64 generation = g_seekGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    g_seekStartedAtMs.store(startedAtMs, std::memory_order_release);
    g_seekTargetSeconds.store(targetSeconds, std::memory_order_release);
    (void)generation;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (!runtimeEnabled()) {
        return;
    }
    std::fprintf(
        stderr,
        "[ui-spectrogram] seek_window_start gen=%llu target_s=%.3f started_ms=%lld\n",
        static_cast<unsigned long long>(generation),
        targetSeconds,
        static_cast<long long>(startedAtMs));
#endif
}

quint64 SpectrogramSeekTrace::currentGeneration() {
    return g_seekGeneration.load(std::memory_order_acquire);
}

qint64 SpectrogramSeekTrace::startedAtMs() {
    return g_seekStartedAtMs.load(std::memory_order_acquire);
}

double SpectrogramSeekTrace::targetSeconds() {
    return g_seekTargetSeconds.load(std::memory_order_acquire);
}

bool SpectrogramSeekTrace::isActive(qint64 nowMs) {
    const qint64 startedAtMs = g_seekStartedAtMs.load(std::memory_order_acquire);
    if (startedAtMs <= 0) {
        return false;
    }
    return nowMs >= startedAtMs && (nowMs - startedAtMs) <= kSeekTraceWindowMs;
}
