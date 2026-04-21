// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <QImage>
#include <QQuickRhiItem>
#include <QSize>

struct SpectrogramRhiVertex {
    float x{0.0f};
    float y{0.0f};
    float u{0.0f};
    float v{0.0f};
};

struct SpectrogramRhiSyncState {
    QImage sourceImage;
    quint64 sourceGeneration{0};
    int sourceColumns{0};
    int bins{0};
    qint64 displayLeft{0};
    qint64 displayRight{-1};
    double drawX{0.0};
    int drawPixels{0};
    double effectiveZoom{1.0};
    bool rollingMode{false};
};

class SpectrogramRhiItem final : public QQuickRhiItem {
    Q_OBJECT

public:
    explicit SpectrogramRhiItem(QQuickItem *parent = nullptr);

    void setSyncState(const SpectrogramRhiSyncState &state);
    SpectrogramRhiSyncState debugLastSyncState() const;
    static std::array<SpectrogramRhiVertex, 4> debugBuildQuadVertices(
        const SpectrogramRhiSyncState &state,
        const QSize &targetSize);

protected:
    QQuickRhiItemRenderer *createRenderer() override;

private:
    SpectrogramRhiSyncState m_syncState;
};
