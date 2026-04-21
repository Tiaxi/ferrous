// SPDX-License-Identifier: GPL-3.0-or-later

#include "SpectrogramRhiItem.h"

namespace {

class SpectrogramRhiRenderer final : public QQuickRhiItemRenderer {
protected:
    void initialize(QRhiCommandBuffer *cb) override {
        Q_UNUSED(cb);
    }

    void synchronize(QQuickRhiItem *item) override {
        Q_UNUSED(item);
    }

    void render(QRhiCommandBuffer *cb) override {
        Q_UNUSED(cb);
    }
};

} // namespace

SpectrogramRhiItem::SpectrogramRhiItem(QQuickItem *parent)
    : QQuickRhiItem(parent) {
    setVisible(true);
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
}

void SpectrogramRhiItem::setSyncState(const SpectrogramRhiSyncState &state) {
    m_syncState = state;
    update();
}

SpectrogramRhiSyncState SpectrogramRhiItem::debugLastSyncState() const {
    return m_syncState;
}

QQuickRhiItemRenderer *SpectrogramRhiItem::createRenderer() {
    return new SpectrogramRhiRenderer();
}
