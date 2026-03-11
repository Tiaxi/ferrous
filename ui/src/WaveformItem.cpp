#include "WaveformItem.h"

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
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_peaksData == data) {
            return;
        }
        m_peaksData = data;
    }
    emit peaksDataChanged();
    update();
}

double WaveformItem::generatedSeconds() const {
    QMutexLocker lock(&m_stateMutex);
    return m_generatedSeconds;
}

void WaveformItem::setGeneratedSeconds(double value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (std::abs(m_generatedSeconds - value) < 0.0001) {
            return;
        }
        m_generatedSeconds = value;
    }
    emit generatedSecondsChanged();
    update();
}

bool WaveformItem::waveformComplete() const {
    QMutexLocker lock(&m_stateMutex);
    return m_waveformComplete;
}

void WaveformItem::setWaveformComplete(bool value) {
    {
        QMutexLocker lock(&m_stateMutex);
        if (m_waveformComplete == value) {
            return;
        }
        m_waveformComplete = value;
    }
    emit waveformCompleteChanged();
    update();
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
    {
        QMutexLocker lock(&m_stateMutex);
        if (std::abs(m_durationSeconds - value) < 0.0001) {
            return;
        }
        m_durationSeconds = value;
    }
    emit durationSecondsChanged();
}

void WaveformItem::paint(QPainter *painter) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const auto paint_start = std::chrono::steady_clock::now();
#endif
    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));

    QByteArray peaksDataLocal;
    double generatedSecondsLocal = 0.0;
    bool waveformCompleteLocal = false;
    double durationSecondsLocal = 0.0;
    {
        QMutexLocker lock(&m_stateMutex);
        peaksDataLocal = m_peaksData;
        generatedSecondsLocal = m_generatedSeconds;
        waveformCompleteLocal = m_waveformComplete;
        durationSecondsLocal = m_durationSeconds;
    }

    painter->fillRect(QRect(0, 0, w, h), QColor("#ffffff"));
    if (!peaksDataLocal.isEmpty()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#0f2e5d"));
        const int count = peaksDataLocal.size();
        const double half = static_cast<double>(h) / 2.0;
        const auto *src = reinterpret_cast<const uchar *>(peaksDataLocal.constData());
        int drawWidth = w;
        if (!waveformCompleteLocal && generatedSecondsLocal > 0.0 && durationSecondsLocal > 0.0) {
            drawWidth = std::clamp(
                static_cast<int>(std::floor((generatedSecondsLocal / durationSecondsLocal) * w)),
                0,
                w);
        }
        for (int x = 0; x < drawWidth; ++x) {
            const int idx = (count <= 1 || drawWidth <= 1) ? 0 : (x * (count - 1) / (drawWidth - 1));
            const double peak = static_cast<double>(src[idx]) / 255.0;
            const int bar = std::max(1, static_cast<int>(std::floor(peak * half)));
            const int y = static_cast<int>(half) - bar;
            painter->drawRect(x, y, 1, bar * 2);
        }
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
                static_cast<long long>(peaksDataLocal.size()));
            m_profileLast = paint_end;
            m_profilePaints = 0;
            m_profilePaintMs = 0.0;
        }
    }
#endif
}
