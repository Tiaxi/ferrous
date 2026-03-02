#include "WaveformItem.h"

#include <QPainter>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

WaveformItem::WaveformItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(true);
    const bool useImageTarget = qEnvironmentVariableIsSet("FERROUS_UI_PAINT_IMAGE");
    if (!useImageTarget) {
        setRenderTarget(QQuickPaintedItem::FramebufferObject);
    }
    m_profileEnabled = qEnvironmentVariableIsSet("FERROUS_PROFILE_UI")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE");
    if (m_profileEnabled) {
        m_profileLast = std::chrono::steady_clock::now();
    }
}

QByteArray WaveformItem::peaksData() const {
    return m_peaksData;
}

void WaveformItem::setPeaksData(const QByteArray &data) {
    if (m_peaksData == data) {
        return;
    }
    m_peaksData = data;
    emit peaksDataChanged();
    update();
}

double WaveformItem::positionSeconds() const {
    return m_positionSeconds;
}

void WaveformItem::setPositionSeconds(double value) {
    if (std::abs(m_positionSeconds - value) < 0.0001) {
        return;
    }
    m_positionSeconds = value;
    emit positionSecondsChanged();
    update();
}

double WaveformItem::durationSeconds() const {
    return m_durationSeconds;
}

void WaveformItem::setDurationSeconds(double value) {
    if (std::abs(m_durationSeconds - value) < 0.0001) {
        return;
    }
    m_durationSeconds = value;
    emit durationSecondsChanged();
    update();
}

void WaveformItem::paint(QPainter *painter) {
    const auto paint_start = std::chrono::steady_clock::now();
    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));

    painter->fillRect(QRect(0, 0, w, h), QColor("#ffffff"));
    if (!m_peaksData.isEmpty()) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#0f2e5d"));
        const int count = m_peaksData.size();
        const double half = static_cast<double>(h) / 2.0;
        const auto *src = reinterpret_cast<const uchar *>(m_peaksData.constData());
        for (int x = 0; x < w; ++x) {
            const int idx = (count <= 1 || w <= 1) ? 0 : (x * (count - 1) / (w - 1));
            const double peak = static_cast<double>(src[idx]) / 255.0;
            const int bar = std::max(1, static_cast<int>(std::floor(peak * half)));
            const int y = static_cast<int>(half) - bar;
            painter->drawRect(x, y, 1, bar * 2);
        }
    }

    const double progress = (m_durationSeconds > 0.0)
        ? std::clamp(m_positionSeconds / m_durationSeconds, 0.0, 1.0)
        : 0.0;
    const int progressX = static_cast<int>(std::floor(progress * w));

    painter->fillRect(QRect(0, 0, progressX, h), QColor(120, 190, 255, 66));
    painter->fillRect(QRect(progressX, 0, 1, h), QColor("#2f7cd6"));

    if (m_profileEnabled) {
        const auto paint_end = std::chrono::steady_clock::now();
        m_profilePaints += 1;
        m_profilePaintMs += std::chrono::duration<double, std::milli>(paint_end - paint_start).count();
        const double elapsed = std::chrono::duration<double>(paint_end - m_profileLast).count();
        if (elapsed >= 1.0) {
            std::fprintf(
                stderr,
                "[ui-waveform] paints/s=%llu paint_ms/s=%.2f avg_ms=%.3f peaks=%d\n",
                static_cast<unsigned long long>(m_profilePaints),
                m_profilePaintMs,
                m_profilePaints > 0 ? (m_profilePaintMs / static_cast<double>(m_profilePaints)) : 0.0,
                m_peaksData.size());
            m_profileLast = paint_end;
            m_profilePaints = 0;
            m_profilePaintMs = 0.0;
        }
    }
}
