// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramSeekTrace.h"

#include <QDateTime>
#include <QtGlobal>

#include <atomic>
#include <cstdio>

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
namespace {
constexpr qint64 kSeekTraceWindowMs = 1800;

std::atomic<quint64> g_seekGeneration{0};
std::atomic<qint64> g_seekStartedAtMs{0};
std::atomic<double> g_seekTargetSeconds{0.0};
} // namespace
#endif

bool SpectrogramSeekTrace::runtimeEnabled() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    return qEnvironmentVariableIsSet("FERROUS_PROFILE_UI")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE");
#else
    return false;
#endif
}

void SpectrogramSeekTrace::noteSeekIssued(double targetSeconds) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (!runtimeEnabled()) {
        return;
    }
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    const quint64 generation = g_seekGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    g_seekStartedAtMs.store(startedAtMs, std::memory_order_release);
    g_seekTargetSeconds.store(targetSeconds, std::memory_order_release);
    std::fprintf(
        stderr,
        "[ui-spectrogram] seek_window_start gen=%llu target_s=%.3f started_ms=%lld\n",
        static_cast<unsigned long long>(generation),
        targetSeconds,
        static_cast<long long>(startedAtMs));
#else
    (void)targetSeconds;
#endif
}

quint64 SpectrogramSeekTrace::currentGeneration() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    return g_seekGeneration.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

qint64 SpectrogramSeekTrace::startedAtMs() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    return g_seekStartedAtMs.load(std::memory_order_acquire);
#else
    return 0;
#endif
}

double SpectrogramSeekTrace::targetSeconds() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    return g_seekTargetSeconds.load(std::memory_order_acquire);
#else
    return 0.0;
#endif
}

bool SpectrogramSeekTrace::isActive(qint64 nowMs) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (!runtimeEnabled()) {
        return false;
    }
    const qint64 startedAtMs = g_seekStartedAtMs.load(std::memory_order_acquire);
    if (startedAtMs <= 0) {
        return false;
    }
    return nowMs >= startedAtMs && (nowMs - startedAtMs) <= kSeekTraceWindowMs;
#else
    (void)nowMs;
    return false;
#endif
}
