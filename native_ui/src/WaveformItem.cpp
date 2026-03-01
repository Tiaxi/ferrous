#include "WaveformItem.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

WaveformItem::WaveformItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(true);
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
}

