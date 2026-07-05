// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>

#include <atomic>

namespace SpectrogramTraceLogging {

namespace detail {

inline std::atomic<int> &detailedSettingCache() {
    static std::atomic<int> cache{-1};
    return cache;
}

inline bool parseEnabled(const QByteArray &raw) {
    const QByteArray normalized = raw.trimmed().toLower();
    return !normalized.isEmpty()
        && normalized != QByteArrayLiteral("0")
        && normalized != QByteArrayLiteral("false")
        && normalized != QByteArrayLiteral("no");
}

} // namespace detail

inline bool detailedEnabled() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    int cached = detail::detailedSettingCache().load(std::memory_order_acquire);
    if (cached < 0) {
        const bool enabled = detail::parseEnabled(qgetenv("FERROUS_PROFILE_SPECTROGRAM_TRACE"));
        cached = enabled ? 1 : 0;
        detail::detailedSettingCache().store(cached, std::memory_order_release);
    }
    return cached == 1;
#else
    return false;
#endif
}

inline void refreshDetailedSettingForTests() {
    detail::detailedSettingCache().store(-1, std::memory_order_release);
}

} // namespace SpectrogramTraceLogging
