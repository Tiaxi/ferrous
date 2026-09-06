// SPDX-License-Identifier: GPL-3.0-or-later

#include "LevelMeterItem.h"
#include "FerrousBridgeFfi.h"

#include <QFutureWatcher>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>

namespace {
constexpr double kFloorDb = -60.0;
constexpr double kReleaseDbPerSecond = 60.0;
constexpr double kPeakReleaseDbPerSecond = 12.0;
constexpr double kPeakHoldSeconds = 1.5;
constexpr int kGradientWidth = 512;

struct MeterNode : QSGNode {
    QSGTexture *gradient{nullptr};
    int channels{-1};
    ~MeterNode() override {
        // Child nodes borrow the shared texture. Release them before its owner.
        while (auto *child = firstChild()) {
            removeChildNode(child);
            delete child;
        }
        delete gradient;
    }
};

double fraction(double db) { return std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0); }
}

void LevelMeterItem::Channel::decay(double seconds) {
    seconds = std::max(0.0, seconds);
    level = std::max(kFloorDb, level - kReleaseDbPerSecond * seconds);
    peak = std::max(kFloorDb, peak - kPeakReleaseDbPerSecond * std::max(0.0, seconds - hold));
    hold = std::max(0.0, hold - seconds);
}

void LevelMeterItem::Channel::observe(double db, double ageSeconds) {
    if (!std::isfinite(db)) return;
    db = std::clamp(db, kFloorDb, 0.0);
    const double age = std::max(0.0, ageSeconds);
    level = std::max(level, db - kReleaseDbPerSecond * age);
    const double held = db - kPeakReleaseDbPerSecond * std::max(0.0, age - kPeakHoldSeconds);
    if (held >= peak && db > kFloorDb) {
        hold = held > peak ? std::max(0.0, kPeakHoldSeconds - age)
                           : std::max(hold, kPeakHoldSeconds - age);
        peak = held;
    }
}

LevelMeterItem::LevelMeterItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setImplicitHeight(20);
    connect(this, &QQuickItem::windowChanged, this, &LevelMeterItem::bindWindow);
    connect(this, &QQuickItem::visibleChanged, this, &LevelMeterItem::visibilityChanged);
}

LevelMeterItem::~LevelMeterItem() {
    cancelRequest();
    // QQuickItem emits window/visibility changes from its destructor, after
    // our members are gone. Disconnect before entering that base destructor.
    disconnect(this, nullptr, this, nullptr);
    disconnect(m_frameConnection);
    disconnect(m_visibilityConnection);
}

void LevelMeterItem::cancelRequest() {
    ++m_generation;
    if (m_cancelled) m_cancelled->store(true);
}

void LevelMeterItem::reset() {
    cancelRequest();
    m_window = {};
    m_stagedWindow = {};
    m_channels.assign(static_cast<std::size_t>(m_channelCountHint), {});
    m_readPosition = m_positionSeconds;
    m_frameAt = Clock::now();
    m_retryAt = {};
    scheduleTick();
    update();
}

void LevelMeterItem::setSourcePath(const QString &value) {
    if (m_sourcePath == value) return;
    m_sourcePath = value;
    reset();
    emit sourcePathChanged();
}

void LevelMeterItem::setPositionSeconds(double value) {
    if (!std::isfinite(value)) return;
    value = std::max(0.0, value);
    if (m_positionSeconds == value) return;
    const auto now = Clock::now();
    const bool discontinuity = std::abs(value - currentPosition(now)) > 0.25;
    m_positionSeconds = value;
    m_positionAt = now;
    if (discontinuity) reset();
    emit positionSecondsChanged();
    scheduleTick();
}

void LevelMeterItem::resetForSeek(double seconds) {
    if (!std::isfinite(seconds)) return;
    m_positionSeconds = std::max(0.0, seconds);
    m_positionAt = Clock::now();
    reset();
    emit positionSecondsChanged();
}

void LevelMeterItem::setDurationSeconds(double value) {
    if (!std::isfinite(value)) return;
    value = std::max(0.0, value);
    if (m_durationSeconds == value) return;
    m_durationSeconds = value;
    emit durationSecondsChanged();
    scheduleTick();
}

void LevelMeterItem::setPlaying(bool value) {
    if (m_playing == value) return;
    m_playing = value;
    m_positionAt = Clock::now();
    m_frameAt = m_positionAt;
    if (!value) cancelRequest();
    emit playingChanged();
    scheduleTick();
}

void LevelMeterItem::setChannelCountHint(int value) {
    value = std::clamp(value, 1, 64);
    if (m_channelCountHint == value) return;
    m_channelCountHint = value;
    if (m_window.channels == 0) m_channels.assign(static_cast<std::size_t>(value), {});
    emit channelCountHintChanged();
    update();
}

bool LevelMeterItem::visibleForPlayback() const {
    return isVisible() && window() && window()->isVisible()
        && window()->visibility() != QWindow::Minimized;
}

void LevelMeterItem::bindWindow(QQuickWindow *next) {
    disconnect(m_frameConnection);
    disconnect(m_visibilityConnection);
    if (next) {
        // Presentation drives animation; no fixed-rate timer limits high-refresh displays.
        m_frameConnection = connect(next, &QQuickWindow::frameSwapped,
                                    this, &LevelMeterItem::tick, Qt::QueuedConnection);
        m_visibilityConnection = connect(next, &QWindow::visibilityChanged,
                                         this, &LevelMeterItem::visibilityChanged);
    }
    visibilityChanged();
}

void LevelMeterItem::visibilityChanged() {
    if (!visibleForPlayback()) reset();
    else scheduleTick();
}

void LevelMeterItem::scheduleTick() {
    if (m_tickQueued) return;
    m_tickQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_tickQueued = false;
        tick();
    }, Qt::QueuedConnection);
}

double LevelMeterItem::currentPosition(Clock::time_point now) const {
    // Fill the gaps between transport updates, stopping extrapolation if they stall.
    const double elapsed = m_playing
        ? std::clamp(std::chrono::duration<double>(now - m_positionAt).count(), 0.0, 0.25)
        : 0.0;
    const double position = m_positionSeconds + elapsed;
    return m_durationSeconds > 0.0 ? std::min(position, m_durationSeconds) : position;
}

void LevelMeterItem::tick() {
    if (!visibleForPlayback()) return;
    const bool moving = m_playing || std::any_of(m_channels.begin(), m_channels.end(),
        [](const Channel &channel) { return channel.peak > kFloorDb || channel.level > kFloorDb; });
    if (!moving) return;
    const auto now = Clock::now();
    const double elapsed = std::chrono::duration<double>(now - m_frameAt).count();
    m_frameAt = now;
    const double position = currentPosition(now);
    advance(position, elapsed);
    if (m_playing) requestWindow(position);
    update();
}

void LevelMeterItem::advance(double position, double elapsed) {
    for (auto &channel : m_channels) channel.decay(elapsed);
    if (!m_playing) return;
    const auto consume = [&](const Window &data) {
        if (data.channels == 0 || data.step <= 0.0 || position <= m_readPosition) return;
        if (m_channels.size() != static_cast<std::size_t>(data.channels)) {
            m_channels.assign(static_cast<std::size_t>(data.channels), {});
        }
        const int points = static_cast<int>(data.peaksDb.size() / data.channels);
        // A late decode or resumed window must not replay old peaks. A bin is
        // eligible only after its end reaches the audible playhead.
        const double begin = std::max(m_readPosition, position - 0.1);
        const int first = static_cast<int>(std::clamp(std::floor((begin - data.start) / data.step), 0.0, double(points)));
        const int last = position >= data.end ? points
            : static_cast<int>(std::clamp(std::floor((position - data.start) / data.step), 0.0, double(points)));
        for (int point = first; point < last; ++point) {
            const double age = std::max(0.0, position - std::min(data.end, data.start + (point + 1) * data.step));
            for (int ch = 0; ch < data.channels; ++ch) {
                m_channels[static_cast<std::size_t>(ch)].observe(
                    data.peaksDb[static_cast<std::size_t>(point * data.channels + ch)], age);
            }
        }
    };
    consume(m_window);
    if (m_stagedWindow.channels > 0) {
        consume(m_stagedWindow);
        if (position >= m_stagedWindow.start) {
            m_window = std::move(m_stagedWindow);
            m_stagedWindow = {};
        }
    }
    m_readPosition = std::max(m_readPosition, position);
}

void LevelMeterItem::requestWindow(double position) {
    if (m_requestActive || m_sourcePath.isEmpty() || m_durationSeconds <= position
        || !visibleForPlayback() || Clock::now() < m_retryAt) return;
    const Window &latest = m_stagedWindow.channels > 0 ? m_stagedWindow : m_window;
    if (latest.channels > 0 && latest.end >= std::min(m_durationSeconds, position + 0.5)) return;
    // Two seconds of millisecond-scale extrema, shared with waveform detail's
    // bounded Rust PCM cache. Decoding and payload parsing stay off the GUI thread.
    const double start = std::max(0.0, position - 0.1);
    const double end = std::min(m_durationSeconds, start + 2.0);
    const QString path = m_sourcePath;
    const auto generation = m_generation;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    m_cancelled = cancelled;
    m_requestActive = true;
    auto *watcher = new QFutureWatcher<Window>(this);
    connect(watcher, &QFutureWatcher<Window>::finished, this, [this, watcher, generation] {
        m_requestActive = false;
        m_cancelled.reset();
        Window next = watcher->result();
        watcher->deleteLater();
        if (generation == m_generation) {
            if (next.channels > 0) m_stagedWindow = std::move(next);
            else m_retryAt = Clock::now() + std::chrono::seconds(1);
        }
        scheduleTick();
    });
    watcher->setFuture(QtConcurrent::run([decode = m_decodeWindow, path, start, end, cancelled] {
        return decode(path, start, end, cancelled);
    }));
}

LevelMeterItem::Window LevelMeterItem::decodeWindow(
    const QString &path, double start, double end, const Cancellation &cancelled) {
    const QByteArray encodedPath = path.toUtf8();
    std::size_t length = 0;
    auto *data = ferrous_ffi_waveform_window_cancellable(
        reinterpret_cast<const std::uint8_t *>(encodedPath.constData()),
        static_cast<std::size_t>(encodedPath.size()), start, end, 2048, &length,
        [](const void *context) { return static_cast<const std::atomic_bool *>(context)->load(); },
        cancelled.get());
    if (!data) return {};
    const QByteArray bytes(reinterpret_cast<const char *>(data), static_cast<qsizetype>(length));
    ferrous_ffi_waveform_window_free(data, length);
    return cancelled->load() ? Window{} : parseWindow(bytes);
}

LevelMeterItem::Window LevelMeterItem::parseWindow(const QByteArray &bytes) {
    constexpr qsizetype header = 36;
    if (bytes.size() < header || bytes.first(4) != QByteArrayLiteral("WVF2")) return {};
    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    const auto rate = qFromLittleEndian<quint32>(data + 4);
    const auto channels = qFromLittleEndian<quint16>(data + 8);
    const double start = std::bit_cast<double>(qFromLittleEndian<quint64>(data + 12));
    const double end = std::bit_cast<double>(qFromLittleEndian<quint64>(data + 20));
    const auto frames = qFromLittleEndian<quint32>(data + 28);
    const auto points = qFromLittleEndian<quint32>(data + 32);
    const quint64 expected = header + (quint64(channels) + 1) * points * 8;
    if (rate == 0 || channels == 0 || channels > 64 || frames == 0 || points == 0
        || points > 65536 || !std::isfinite(start) || !std::isfinite(end)
        || start < 0.0 || end <= start || expected != quint64(bytes.size())) return {};
    Window result{start, end, double(frames) / rate, channels, {}};
    result.peaksDb.resize(static_cast<std::size_t>(points) * channels);
    for (std::size_t i = 0; i < result.peaksDb.size(); ++i) {
        const float minimum = std::bit_cast<float>(qFromLittleEndian<quint32>(data + header + i * 8));
        const float maximum = std::bit_cast<float>(qFromLittleEndian<quint32>(data + header + i * 8 + 4));
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) return {};
        const double amplitude = std::max(std::abs(double(minimum)), std::abs(double(maximum)));
        result.peaksDb[i] = static_cast<float>(20.0 * std::log10(std::clamp(amplitude, 0.001, 1.0)));
    }
    return result;
}

QSGNode *LevelMeterItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) {
    // Qt blocks the GUI thread during scene graph synchronization; worker results
    // are delivered to that thread and never mutate render state directly.
    auto *node = static_cast<MeterNode *>(oldNode);
    if (!node) node = new MeterNode;
    if (!node->gradient) {
        QImage image(kGradientWidth, 1, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        QLinearGradient gradient(0, 0, kGradientWidth, 0);
        gradient.setColorAt(0, QColor("#2b6546"));
        gradient.setColorAt(fraction(-30), QColor("#4c9460"));
        gradient.setColorAt(fraction(-16), QColor("#849e59"));
        gradient.setColorAt(fraction(-12), QColor("#b2ab62"));
        gradient.setColorAt(fraction(-3), QColor("#c59456"));
        gradient.setColorAt(1, QColor("#c57f50"));
        painter.fillRect(image.rect(), gradient);
        painter.end();
        node->gradient = window()->createTextureFromImage(image);
    }
    const int count = static_cast<int>(m_channels.size());
    if (node->channels != count) {
        while (auto *child = node->firstChild()) {
            node->removeChildNode(child);
            delete child;
        }
        node->appendChildNode(new QSGSimpleRectNode(boundingRect(), QColor("#202427")));
        for (int ch = 0; ch < count; ++ch) {
            node->appendChildNode(new QSGSimpleRectNode({}, QColor("#090c0e")));
            auto *fill = new QSGSimpleTextureNode;
            fill->setTexture(node->gradient);
            fill->setFiltering(QSGTexture::Linear);
            node->appendChildNode(fill);
            node->appendChildNode(new QSGSimpleRectNode({}, QColor("#d6d3b9")));
        }
        node->channels = count;
    }
    auto *background = static_cast<QSGSimpleRectNode *>(node->firstChild());
    background->setRect(boundingRect());
    const double dpr = window()->effectiveDevicePixelRatio();
    const double row = height() / std::max(1, count);
    const double gap = std::min(count == 2 ? 2.0 : 1.0, row * 0.4);
    const double barHeight = (height() - gap * (count - 1)) / std::max(1, count);
    auto *child = background->nextSibling();
    for (int ch = 0; ch < count; ++ch) {
        const double top = std::round(ch * (barHeight + gap) * dpr) / dpr;
        const double bottom = std::round((ch * (barHeight + gap) + barHeight) * dpr) / dpr;
        const QRectF track(0, top, width(), std::max(0.0, bottom - top));
        static_cast<QSGSimpleRectNode *>(child)->setRect(track);
        child = child->nextSibling();
        const auto &channel = m_channels[static_cast<std::size_t>(ch)];
        const double amount = fraction(channel.level);
        auto *fill = static_cast<QSGSimpleTextureNode *>(child);
        fill->setRect(QRectF(0, top, width() * amount, track.height()));
        fill->setSourceRect(QRectF(0, 0, kGradientWidth * amount, 1));
        child = child->nextSibling();
        // Keep the marker a single physical pixel, including on scaled displays.
        const double markerWidth = std::min(width(), 1.0 / dpr);
        const double peakX = std::clamp(std::round(width() * fraction(channel.peak) * dpr) / dpr,
                                        0.0, std::max(0.0, width() - markerWidth));
        static_cast<QSGSimpleRectNode *>(child)->setRect(channel.peak > kFloorDb
            ? QRectF(peakX, top, markerWidth, track.height()) : QRectF());
        child = child->nextSibling();
    }
    return node;
}
