// SPDX-License-Identifier: GPL-3.0-or-later

#include "WaveformItem.h"

#include <QImage>
#include <QMutexLocker>
#include <QPainter>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

WaveformItem::WaveformItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(true);
    // Keep Image render path by default; allow FBO only via explicit opt-in.
    const bool useFboTarget = qEnvironmentVariableIsSet("FERROUS_UI_PAINT_FBO");
    if (useFboTarget) {
        setRenderTarget(QQuickPaintedItem::FramebufferObject);
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    m_profileEnabled = qEnvironmentVariableIsSet("FERROUS_PROFILE_UI")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE");
    if (m_profileEnabled) {
        m_profileLast = std::chrono::steady_clock::now();
    }
#endif
}

QByteArray WaveformItem::peaksData() const {
    QMutexLocker lock(&m_stateMutex);
    return m_peaksData;
}

void WaveformItem::setPeaksData(const QByteArray &data) {
    QRect dirtyRect;
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_peaksData == data) {
            return;
        }
        const int width = currentWidthLocked();
        const int previousDrawWidth = drawnWidthLocked(
            m_generatedSeconds,
            m_waveformComplete,
            m_durationSeconds);
        int dirtyStartX = 0;
        if (!m_peaksData.isEmpty() && !data.isEmpty() && width > 0 && previousDrawWidth > 0) {
            int prefix = 0;
            const int prefixLimit = std::min(m_peaksData.size(), data.size());
            while (prefix < prefixLimit && m_peaksData[prefix] == data[prefix]) {
                ++prefix;
            }
            if (prefix > 0 && prefix < prefixLimit) {
                dirtyStartX = xForPeakIndexLocked(prefix, data.size(), previousDrawWidth);
            } else if (prefix == prefixLimit && data.size() >= m_peaksData.size()) {
                dirtyStartX = xForPeakIndexLocked(prefixLimit, data.size(), previousDrawWidth);
            }
        }
        m_peaksData = data;
        markDirtyRangeLocked(dirtyStartX, width);
        dirtyRect = m_dirtyRect;
    }
    emit peaksDataChanged();
    update(dirtyRect);
}

double WaveformItem::generatedSeconds() const {
    QMutexLocker lock(&m_stateMutex);
    return m_generatedSeconds;
}

void WaveformItem::setGeneratedSeconds(double value) {
    QRect dirtyRect;
    {
        QMutexLocker lock(&m_stateMutex);
        if (std::abs(m_generatedSeconds - value) < 0.0001) {
            return;
        }
        const int previousDrawWidth = drawnWidthLocked(
            m_generatedSeconds,
            m_waveformComplete,
            m_durationSeconds);
        m_generatedSeconds = value;
        const int nextDrawWidth = drawnWidthLocked(
            m_generatedSeconds,
            m_waveformComplete,
            m_durationSeconds);
        markDirtyRangeLocked(
            std::min(previousDrawWidth, nextDrawWidth),
            std::max(previousDrawWidth, nextDrawWidth));
        dirtyRect = m_dirtyRect;
    }
    emit generatedSecondsChanged();
    update(dirtyRect);
}

bool WaveformItem::waveformComplete() const {
    QMutexLocker lock(&m_stateMutex);
    return m_waveformComplete;
}

void WaveformItem::setWaveformComplete(bool value) {
    QRect dirtyRect;
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_waveformComplete == value) {
            return;
        }
        const int previousDrawWidth = drawnWidthLocked(
            m_generatedSeconds,
            m_waveformComplete,
            m_durationSeconds);
        m_waveformComplete = value;
        const int nextDrawWidth = drawnWidthLocked(
            m_generatedSeconds,
            m_waveformComplete,
            m_durationSeconds);
        markDirtyRangeLocked(
            std::min(previousDrawWidth, nextDrawWidth),
            std::max(previousDrawWidth, nextDrawWidth));
        dirtyRect = m_dirtyRect;
    }
    emit waveformCompleteChanged();
    update(dirtyRect);
}

double WaveformItem::positionSeconds() const {
    QMutexLocker lock(&m_stateMutex);
    return m_positionSeconds;
}

void WaveformItem::setPositionSeconds(double value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (std::abs(m_positionSeconds - value) < 0.0001) {
            return;
        }
        m_positionSeconds = value;
    }
    emit positionSecondsChanged();
}

double WaveformItem::durationSeconds() const {
    QMutexLocker lock(&m_stateMutex);
    return m_durationSeconds;
}

void WaveformItem::setDurationSeconds(double value) {
    QRect dirtyRect;
    {
        QMutexLocker lock(&m_stateMutex);
        if (std::abs(m_durationSeconds - value) < 0.0001) {
            return;
        }
        m_durationSeconds = value;
        markDirtyAllLocked();
        dirtyRect = m_dirtyRect;
    }
    emit durationSecondsChanged();
    update(dirtyRect);
}

void WaveformItem::paint(QPainter *painter) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto paint_start = std::chrono::steady_clock::now();
    int peaksCountLocal = 0;
#endif
    QImage waveformCacheLocal;
    {
        QMutexLocker lock(&m_stateMutex);
        ensureCacheLocked(currentWidthLocked(), currentHeightLocked());
        updateWaveformCacheLocked();
        waveformCacheLocal = m_waveformCache;
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
        peaksCountLocal = m_peaksData.size();
#endif
    }

    const QRect clipRect = painter->clipBoundingRect().toAlignedRect();
    if (clipRect.isValid() && !waveformCacheLocal.isNull()) {
        painter->drawImage(clipRect, waveformCacheLocal, clipRect);
    } else if (!waveformCacheLocal.isNull()) {
        painter->drawImage(QPoint(0, 0), waveformCacheLocal);
    }

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileEnabled) {
        const auto paint_end = std::chrono::steady_clock::now();
        m_profilePaints += 1;
        m_profilePaintMs += std::chrono::duration<double, std::milli>(paint_end - paint_start).count();
        const double elapsed = std::chrono::duration<double>(paint_end - m_profileLast).count();
        if (elapsed >= 1.0) {
            std::fprintf(
                stderr,
                "[ui-waveform] paints/s=%llu paint_ms/s=%.2f avg_ms=%.3f peaks=%lld\n",
                static_cast<unsigned long long>(m_profilePaints),
                m_profilePaintMs,
                m_profilePaints > 0 ? (m_profilePaintMs / static_cast<double>(m_profilePaints)) : 0.0,
                static_cast<long long>(peaksCountLocal));
            m_profileLast = paint_end;
            m_profilePaints = 0;
            m_profilePaintMs = 0.0;
        }
    }
#endif
}

int WaveformItem::currentWidthLocked() const {
    return std::max(1, static_cast<int>(std::floor(width())));
}

int WaveformItem::currentHeightLocked() const {
    return std::max(1, static_cast<int>(std::floor(height())));
}

int WaveformItem::drawnWidthLocked(
    double generatedSeconds,
    bool waveformComplete,
    double durationSeconds) const {
    const int width = currentWidthLocked();
    if (waveformComplete || generatedSeconds <= 0.0 || durationSeconds <= 0.0) {
        return width;
    }
    return std::clamp(
        static_cast<int>(std::floor((generatedSeconds / durationSeconds) * width)),
        0,
        width);
}

int WaveformItem::xForPeakIndexLocked(int peakIndex, int peakCount, int drawWidth) const {
    if (peakCount <= 1 || drawWidth <= 1) {
        return 0;
    }
    return std::clamp(
        static_cast<int>(std::floor(
            (static_cast<double>(peakIndex) / static_cast<double>(peakCount - 1))
            * static_cast<double>(drawWidth - 1))),
        0,
        drawWidth - 1);
}

void WaveformItem::ensureCacheLocked(int width, int height) {
    if (width <= 0 || height <= 0) {
        m_waveformCache = QImage();
        m_dirtyRect = QRect();
        m_cacheDirty = false;
        return;
    }
    if (m_waveformCache.size() == QSize(width, height)
        && m_waveformCache.format() == QImage::Format_RGB32) {
        return;
    }
    m_waveformCache = QImage(width, height, QImage::Format_RGB32);
    m_waveformCache.fill(Qt::white);
    m_dirtyRect = QRect(0, 0, width, height);
    m_cacheDirty = true;
}

void WaveformItem::markDirtyRangeLocked(int x0, int x1) {
    const int width = currentWidthLocked();
    const int height = currentHeightLocked();
    if (width <= 0 || height <= 0) {
        return;
    }
    const int left = std::clamp(std::min(x0, x1), 0, width);
    const int right = std::clamp(std::max(x0, x1), 0, width);
    if (right <= left) {
        return;
    }
    const QRect dirty(left, 0, right - left, height);
    m_dirtyRect = m_dirtyRect.isNull() ? dirty : m_dirtyRect.united(dirty);
    m_cacheDirty = true;
}

void WaveformItem::markDirtyAllLocked() {
    const int width = currentWidthLocked();
    const int height = currentHeightLocked();
    if (width <= 0 || height <= 0) {
        return;
    }
    m_dirtyRect = QRect(0, 0, width, height);
    m_cacheDirty = true;
}

void WaveformItem::updateWaveformCacheLocked() {
    if (!m_cacheDirty || m_waveformCache.isNull()) {
        return;
    }

    QRect dirty = m_dirtyRect.intersected(m_waveformCache.rect());
    if (!dirty.isValid() || dirty.isEmpty()) {
        m_dirtyRect = QRect();
        m_cacheDirty = false;
        return;
    }

    QPainter cachePainter(&m_waveformCache);
    cachePainter.fillRect(dirty, QColor("#ffffff"));
    if (!m_peaksData.isEmpty()) {
        cachePainter.setPen(Qt::NoPen);
        cachePainter.setBrush(QColor("#0f2e5d"));
        const int count = m_peaksData.size();
        const double half = static_cast<double>(m_waveformCache.height()) / 2.0;
        const auto *src = reinterpret_cast<const uchar *>(m_peaksData.constData());
        const int drawWidth = drawnWidthLocked(
            m_generatedSeconds,
            m_waveformComplete,
            m_durationSeconds);
        const int right = std::min(dirty.right() + 1, drawWidth);
        for (int x = dirty.x(); x < right; ++x) {
            const int idx = (count <= 1 || drawWidth <= 1) ? 0 : (x * (count - 1) / (drawWidth - 1));
            const double peak = static_cast<double>(src[idx]) / 255.0;
            const int bar = std::max(1, static_cast<int>(std::floor(peak * half)));
            const int y = static_cast<int>(half) - bar;
            cachePainter.drawRect(x, y, 1, bar * 2);
        }
    }
    cachePainter.end();

    m_dirtyRect = QRect();
    m_cacheDirty = false;
}
