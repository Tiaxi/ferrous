#include "SpectrogramItem.h"

#include <QPainter>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
std::array<quint8, 3> ddbColor(double norm) {
    static const std::array<std::array<int, 3>, 7> kColors{{
        {{255, 255, 255}},
        {{255, 255, 255}},
        {{255, 247, 0}},
        {{242, 54, 0}},
        {{176, 0, 91}},
        {{48, 0, 115}},
        {{4, 1, 71}},
    }};

    const double clamped = std::clamp(norm, 0.0, 1.0);
    const double pos = (1.0 - clamped) * static_cast<double>(kColors.size() - 1);
    const int i0 = static_cast<int>(std::floor(pos));
    const int i1 = std::min<int>(kColors.size() - 1, i0 + 1);
    const double t = pos - static_cast<double>(i0);

    std::array<quint8, 3> out{};
    for (int c = 0; c < 3; ++c) {
        const double v = static_cast<double>(kColors[static_cast<size_t>(i0)][static_cast<size_t>(c)])
            + (static_cast<double>(kColors[static_cast<size_t>(i1)][static_cast<size_t>(c)])
               - static_cast<double>(kColors[static_cast<size_t>(i0)][static_cast<size_t>(c)]))
                * t;
        out[static_cast<size_t>(c)] = static_cast<quint8>(std::clamp<int>(static_cast<int>(std::lround(v)), 0, 255));
    }
    return out;
}
} // namespace

SpectrogramItem::SpectrogramItem(QQuickItem *parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setOpaquePainting(true);
    rebuildPalette();
}

double SpectrogramItem::dbRange() const {
    return m_dbRange;
}

void SpectrogramItem::setDbRange(double value) {
    const double clamped = std::clamp(value, 50.0, 120.0);
    if (std::abs(m_dbRange - clamped) < 0.001) {
        return;
    }
    m_dbRange = clamped;
    emit dbRangeChanged();
    update();
}

bool SpectrogramItem::logScale() const {
    return m_logScale;
}

void SpectrogramItem::setLogScale(bool value) {
    if (m_logScale == value) {
        return;
    }
    m_logScale = value;
    emit logScaleChanged();
    invalidateMapping();
    update();
}

int SpectrogramItem::sampleRateHz() const {
    return m_sampleRateHz;
}

void SpectrogramItem::setSampleRateHz(int value) {
    const int clamped = std::max(1000, value);
    if (m_sampleRateHz == clamped) {
        return;
    }
    m_sampleRateHz = clamped;
    emit sampleRateHzChanged();
    invalidateMapping();
    update();
}

int SpectrogramItem::maxColumns() const {
    return m_maxColumns;
}

void SpectrogramItem::setMaxColumns(int value) {
    const int clamped = std::clamp(value, 128, 4096);
    if (m_maxColumns == clamped) {
        return;
    }
    m_maxColumns = clamped;
    emit maxColumnsChanged();
    while (static_cast<int>(m_columns.size()) > m_maxColumns) {
        m_columns.pop_front();
    }
    update();
}

void SpectrogramItem::reset() {
    m_columns.clear();
    m_binsPerColumn = 0;
    invalidateMapping();
    update();
}

void SpectrogramItem::appendRows(const QVariantList &rows) {
    if (rows.isEmpty()) {
        return;
    }

    bool anyAdded = false;
    for (const QVariant &rowVar : rows) {
        const QVariantList row = rowVar.toList();
        if (row.isEmpty()) {
            continue;
        }
        std::vector<quint8> mapped = rowToIntensity(row);
        if (mapped.empty()) {
            continue;
        }
        if (m_binsPerColumn <= 0) {
            m_binsPerColumn = static_cast<int>(mapped.size());
            invalidateMapping();
        }
        if (static_cast<int>(mapped.size()) != m_binsPerColumn) {
            continue;
        }
        m_columns.emplace_back(std::move(mapped));
        anyAdded = true;
    }

    if (!anyAdded) {
        return;
    }

    while (static_cast<int>(m_columns.size()) > m_maxColumns) {
        m_columns.pop_front();
    }
    update();
}

void SpectrogramItem::paint(QPainter *painter) {
    const int w = std::max(1, static_cast<int>(std::floor(width())));
    const int h = std::max(1, static_cast<int>(std::floor(height())));

    painter->fillRect(QRect(0, 0, w, h), QColor(0x0b, 0x0b, 0x0f));
    if (m_columns.empty() || m_binsPerColumn <= 0) {
        return;
    }

    ensureMapping(h);
    const int cols = std::min<int>(std::min<int>(w, m_maxColumns), static_cast<int>(m_columns.size()));
    if (cols <= 0) {
        return;
    }

    QImage img(cols, h, QImage::Format_RGBA8888);
    img.fill(Qt::black);

    const int start = static_cast<int>(m_columns.size()) - cols;
    for (int x = 0; x < cols; ++x) {
        const auto &col = m_columns[static_cast<size_t>(start + x)];
        for (int y = 0; y < h; ++y) {
            const int bin = std::clamp(m_yToBin[static_cast<size_t>(y)], 0, m_binsPerColumn - 1);
            const quint8 idx = col[static_cast<size_t>(bin)];
            const auto &rgb = m_palette[static_cast<size_t>(idx)];
            uchar *px = img.scanLine(y) + x * 4;
            px[0] = rgb[0];
            px[1] = rgb[1];
            px[2] = rgb[2];
            px[3] = 255;
        }
    }

    painter->drawImage(QPoint(w - cols, 0), img);
}

void SpectrogramItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        invalidateMapping();
    }
}

void SpectrogramItem::rebuildPalette() {
    for (int i = 0; i < 256; ++i) {
        m_palette[static_cast<size_t>(i)] = ddbColor(static_cast<double>(i) / 255.0);
    }
}

void SpectrogramItem::invalidateMapping() {
    m_yToBin.clear();
    m_yToBinHeight = -1;
}

void SpectrogramItem::ensureMapping(int height) {
    if (height <= 0 || m_binsPerColumn <= 0) {
        return;
    }
    if (m_yToBinHeight == height && static_cast<int>(m_yToBin.size()) == height) {
        return;
    }

    m_yToBin.resize(static_cast<size_t>(height));
    if (m_logScale) {
        const double minFreq = 25.0;
        const double nyquist = std::max(0.5 * static_cast<double>(m_sampleRateHz), minFreq * 1.1);
        const double logStep = (std::log2(nyquist) - std::log2(minFreq)) / std::max(1, height);
        const double freqRes = std::max(1.0, static_cast<double>(m_sampleRateHz)
                                               / (2.0 * std::max(1, m_binsPerColumn - 1)));
        for (int y = 0; y < height; ++y) {
            const int i = height - 1 - y;
            const double freq = std::pow(2.0, static_cast<double>(i) * logStep + std::log2(minFreq));
            const int bin = static_cast<int>(std::lround(freq / freqRes));
            m_yToBin[static_cast<size_t>(y)] = std::clamp(bin, 0, m_binsPerColumn - 1);
        }
    } else {
        for (int y = 0; y < height; ++y) {
            const int i = height - 1 - y;
            const int bin = static_cast<int>(std::floor((static_cast<double>(i) / std::max(1, height - 1))
                                                        * static_cast<double>(m_binsPerColumn - 1)));
            m_yToBin[static_cast<size_t>(y)] = std::clamp(bin, 0, m_binsPerColumn - 1);
        }
    }

    m_yToBinHeight = height;
}

std::vector<quint8> SpectrogramItem::rowToIntensity(const QVariantList &row) const {
    std::vector<quint8> out;
    out.reserve(static_cast<size_t>(row.size()));

    const double dbRange = std::clamp(m_dbRange, 50.0, 120.0);
    const double dbScale = 10.0 / std::log(10.0);

    for (const QVariant &value : row) {
        const double v = value.toDouble();
        int idx = 0;
        if (v >= 0.0 && v <= 255.0 && std::floor(v) == v) {
            idx = static_cast<int>(v);
        } else {
            const double db = v > 0.0 ? (dbScale * std::log(v)) : -200.0;
            const double xdb = std::clamp(db + dbRange - 63.0, 0.0, dbRange);
            idx = static_cast<int>(std::lround((xdb / dbRange) * 255.0));
        }
        out.push_back(static_cast<quint8>(std::clamp(idx, 0, 255)));
    }

    return out;
}
