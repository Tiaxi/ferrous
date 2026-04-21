// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QImage>
#include <QQuickRhiItem>

struct SpectrogramRhiSyncState {
    int sourceColumns{0};
    int bins{0};
    qint64 displayLeft{0};
    qint64 displayRight{-1};
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

protected:
    QQuickRhiItemRenderer *createRenderer() override;

private:
    SpectrogramRhiSyncState m_syncState;
};
