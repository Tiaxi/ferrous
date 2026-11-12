// SPDX-License-Identifier: GPL-3.0-or-later

#include "LevelMeterItem.h"
#include "FerrousBridgeFfi.h"
#include "PlaybackClock.h"
#include "SpectrogramColorMap.h"
#include "WindowPresence.h"

#include <QFutureWatcher>
#include <QImage>
#include <QAccessible>
#include <QQuickWindow>
#include <QSGSimpleRectNode>
#include <QSGSimpleTextureNode>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>

namespace {
// A new window is requested this long before the current one runs out.
constexpr double kRequestLeadSeconds = 0.5;
// How far ahead an idle meter looks for a bin that changes the bars. It then
// wakes to look further, and for new windows.
constexpr double kLookaheadSeconds = 0.5;
// How often readers of the levels hear of new ones.
constexpr int kReadoutIntervalMs = 250;
// The scale's ticks, in dB, and those of a short lane.
constexpr double kTicksDb[] = {0.0, -12.0, -24.0, -48.0};
constexpr double kShortLaneTicksDb[] = {-12.0, -24.0};
// Per lane: the column's floor, the track, the lamp, the gap under it, the
// fill in one colour, the fill along the colour map, the four ticks of the
// scale, the gap under the peak tick and the peak tick.
constexpr int kNodesPerLane = 12;
// Which of a lane's nodes is the fill along the colour map; the others are
// rectangles of one colour.
constexpr int kRampNode = 5;

using Self = LevelMeterItem;

struct MeterNode : QSGNode {
    int lanes{-1};
};

// The fill along the colour map: a column of texels, one per device pixel
// of the lane's scale, each the map's colour of its level, of which the
// fill shows the rows from the bottom up to its level.
struct RampNode : QSGSimpleTextureNode {
    std::unique_ptr<QSGTexture> ramp;
    // What the ramp is drawn for: the scale's device pixels and the range.
    double scalePixels{-1.0};
    double dbRange{0.0};
};

int fillPixels(double level, double scalePixels) {
    return static_cast<int>(std::lround(Self::meterPosition(level, scalePixels)));
}

// -1 while the tick is hidden.
int peakPixels(double peak, double scalePixels) {
    return peak > Self::kFloorDb
        ? static_cast<int>(std::lround(Self::meterPosition(peak, scalePixels))) : -1;
}

// The level `pixels` device pixels up a scale `scalePixels` tall: the inverse
// of LevelMeterItem::meterPosition.
double levelAt(double pixels, double scalePixels) {
    if (scalePixels <= 0.0 || pixels <= 0.0) return Self::kFloorDb;
    const double position = std::clamp(pixels / scalePixels, 0.0, 1.0);
    // The listed levels are a fifth of the height apart.
    static constexpr double kLevels[] = {-60.0, -48.0, -24.0, -12.0, -6.0, 0.0};
    const double step = position * 5.0;
    const int below = std::min(4, static_cast<int>(std::floor(step)));
    return kLevels[below] + (kLevels[below + 1] - kLevels[below]) * (step - below);
}

// Seconds of decay without new data until the channel's fill, tick or lamp
// changes by a device pixel on a scale `scalePixels` tall; infinity if it
// never does.
template <typename Channel> double secondsUntilChange(const Channel &channel, double scalePixels) {
    double seconds = std::numeric_limits<double>::infinity();
    // A rounded position changes once it falls below its pixel's lower half.
    const auto threshold = [&](int pixels) {
        return std::max(Self::kFloorDb, levelAt(pixels - 0.5, scalePixels));
    };
    if (const int fill = fillPixels(channel.level, scalePixels); fill > 0) {
        seconds = (channel.level - threshold(fill)) / Self::kReleaseDbPerSecond;
    }
    if (const int peak = peakPixels(channel.peak, scalePixels); peak >= 0) {
        // At the floor the tick hides.
        seconds = std::min(seconds, channel.hold
            + (channel.peak - threshold(peak)) / Self::kPeakReleaseDbPerSecond);
    }
    if (channel.clip > 0.0) seconds = std::min(seconds, channel.clip);
    return std::max(0.0, seconds);
}

void setRect(QSGNode *node, const QRectF &rect, const QColor &color) {
    auto *rectNode = static_cast<QSGSimpleRectNode *>(node);
    if (rectNode->rect() != rect) rectNode->setRect(rect);
    if (rectNode->color() != color) rectNode->setColor(color);
}

// The ramp of a scale `scalePixels` device pixels tall in a view of
// `dbRange` dB, from its top row down; drawn again only when either changes.
void updateRamp(RampNode *node, QQuickWindow *window, double scalePixels, double dbRange) {
    if (node->ramp && node->scalePixels == scalePixels && node->dbRange == dbRange) return;
    const int rows = std::max(1, static_cast<int>(std::ceil(scalePixels)));
    QImage image(1, rows, QImage::Format_RGB32);
    for (int row = 0; row < rows; ++row) {
        // The colour of the level at the middle of the row's pixel.
        const double level = levelAt(rows - row - 0.5, scalePixels);
        image.setPixel(0, row, Self::colorMapColor(level, dbRange).rgb());
    }
    // The old texture goes once the node no longer draws it.
    std::unique_ptr<QSGTexture> texture(window->createTextureFromImage(image));
    node->setTexture(texture.get());
    node->ramp = std::move(texture);
    node->scalePixels = scalePixels;
    node->dbRange = dbRange;
}
}

std::vector<double> LevelMeterItem::ticksDb(double laneHeight) {
    return laneHeight < kFullScaleLaneHeight
        ? std::vector<double>(std::begin(kShortLaneTicksDb), std::end(kShortLaneTicksDb))
        : std::vector<double>(std::begin(kTicksDb), std::end(kTicksDb));
}

double LevelMeterItem::lightness(const QColor &color) {
    return 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
}

double LevelMeterItem::fillCapPosition() {
    // The map lightens monotonically from where it first reaches the cap
    // (its quiet blues and purples stay far under it), so the cap is one
    // place on the map: found by a sweep for the first colour over it, then
    // bisection down to the last colour under it.
    static const double position = [] {
        const double cap = kFillLightness * lightness(QColor::fromRgb(SpectrogramColorMap::colorAt(1.0)));
        const auto over = [cap](double x) { return lightness(QColor::fromRgb(SpectrogramColorMap::colorAt(x))) > cap; };
        constexpr int kSteps = 1024;
        int step = 0;
        while (step < kSteps && !over(double(step + 1) / kSteps)) ++step;
        if (step == kSteps) return 1.0;
        double under = double(step) / kSteps;
        double above = double(step + 1) / kSteps;
        for (int i = 0; i < 40; ++i) {
            const double middle = (under + above) / 2.0;
            (over(middle) ? above : under) = middle;
        }
        return under;
    }();
    return position;
}

QColor LevelMeterItem::colorMapColor(double db, double dbRange) {
    // Along the ramp: a level whose colour is over the cap takes the colour
    // of the highest level still under it, the map's own, unscaled.
    const double position = std::min(SpectrogramColorMap::position(db, dbRange), fillCapPosition());
    return QColor::fromRgb(SpectrogramColorMap::colorAt(position));
}

double LevelMeterItem::meterPosition(double db, double height) {
    if (!(db > kFloorDb) || height <= 0.0) return 0.0;
    // The listed levels are evenly spaced: 0, -6, -12, -24, -48 and -60 dB
    // at 5/5 to 0/5 of the height.
    static constexpr std::pair<double, double> kPoints[] = {
        {0.0, 1.0}, {-6.0, 0.8}, {-12.0, 0.6}, {-24.0, 0.4}, {-48.0, 0.2}, {-60.0, 0.0}};
    db = std::min(db, 0.0);
    for (std::size_t i = 0; i + 1 < std::size(kPoints); ++i) {
        const auto [upperDb, upper] = kPoints[i];
        const auto [lowerDb, lower] = kPoints[i + 1];
        if (db >= lowerDb) {
            return height * (lower + (upper - lower) * (db - lowerDb) / (upperDb - lowerDb));
        }
    }
    return 0.0;
}

std::pair<double, double> LevelMeterItem::laneRows(int lane, int count, double height) {
    count = std::max(1, count);
    // The seams are on the rows at k * height / count, as the waveform draws
    // them; each lane starts on the row after its seam.
    const double top = lane > 0 ? std::round(lane * height / count) + 1.0 : 0.0;
    const double bottom = lane + 1 < count ? std::round((lane + 1) * height / count) : height;
    return {top, std::max(top, bottom)};
}

void LevelMeterItem::Channel::decay(double seconds) {
    seconds = std::max(0.0, seconds);
    level = std::max(kFloorDb, level - kReleaseDbPerSecond * seconds);
    peak = std::max(kFloorDb, peak - kPeakReleaseDbPerSecond * std::max(0.0, seconds - hold));
    hold = std::max(0.0, hold - seconds);
    clip = std::max(0.0, clip - seconds);
}

void LevelMeterItem::Channel::observe(double db, double ageSeconds) {
    if (!std::isfinite(db)) return;
    const double age = std::max(0.0, ageSeconds);
    if (db >= kClipDb) clip = std::max(clip, kClipHoldSeconds - age);
    db = std::clamp(db, kFloorDb, 0.0);
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
    setImplicitWidth(kColumnWidth);
    m_readoutTimer.setSingleShot(true);
    m_readoutTimer.setInterval(kReadoutIntervalMs);
    connect(&m_readoutTimer, &QTimer::timeout, this, &LevelMeterItem::announceReadout);
    connect(this, &QQuickItem::windowChanged, this, &LevelMeterItem::bindWindow);
    connect(this, &QQuickItem::visibleChanged, this, &LevelMeterItem::visibilityChanged);
    m_wakeTimer.setSingleShot(true);
    m_wakeTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_wakeTimer, &QTimer::timeout, this, &LevelMeterItem::tick);
    // An item made with a parent in a window already has it; no change follows.
    if (window()) bindWindow(window());
}

LevelMeterItem::~LevelMeterItem() {
    cancelRequest();
    // QQuickItem emits window/visibility changes from its destructor, after
    // our members are gone. Disconnect before entering that base destructor.
    disconnect(this, nullptr, this, nullptr);
    disconnect(m_frameConnection);
    disconnect(m_visibilityConnection);
    disconnect(m_clockConnection);
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
    showCurrent();
    m_wakeTimer.stop();
    m_frameAt = m_now();
    m_readPosition = currentPosition(m_frameAt);
    m_retryAt = {};
    scheduleTick();
    update();
}

void LevelMeterItem::setSourcePath(const QString &value) {
    if (m_sourcePath == value) return;
    m_sourcePath = value;
    reset();
    updateReaderLease();
    emit sourcePathChanged();
}

void LevelMeterItem::setClock(PlaybackClock *value) {
    if (m_clock == value) return;
    disconnect(m_clockConnection);
    m_clock = value;
    if (m_clock) {
        m_clockEpoch = m_clock->state().epoch;
        m_clockConnection = connect(m_clock, &PlaybackClock::stateChanged,
                                    this, &LevelMeterItem::clockStateChanged);
    }
    reset();
    emit clockChanged();
}

void LevelMeterItem::clockStateChanged() {
    const quint64 epoch = m_clock ? m_clock->state().epoch : 0;
    if (epoch != m_clockEpoch) {
        // Playback jumped: the levels from before belong elsewhere.
        m_clockEpoch = epoch;
        reset();
        return;
    }
    scheduleTick();
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
    // An idle meter is not ticked; bring its levels to now under the old mode.
    advanceTo(m_now());
    m_playing = value;
    if (!value) cancelRequest();
    emit playingChanged();
    scheduleTick();
}

void LevelMeterItem::setChannelCountHint(int value) {
    value = std::clamp(value, 1, 64);
    if (m_channelCountHint == value) return;
    m_channelCountHint = value;
    if (m_window.channels == 0) {
        m_channels.assign(static_cast<std::size_t>(value), {});
        showCurrent();
    }
    emit channelCountHintChanged();
    update();
}

void LevelMeterItem::setLaneCount(int value) {
    value = std::clamp(value, 1, 64);
    if (m_laneCount == value) return;
    m_laneCount = value;
    emit laneCountChanged();
    update();
    noteReadout();
}

void LevelMeterItem::setActive(bool value) {
    if (m_active == value) return;
    m_active = value;
    emit activeChanged();
    update();
    // An inactive meter reads nothing: its readers hear of it.
    noteReadout();
}

void LevelMeterItem::setColorMapFill(bool value) {
    if (m_colorMapFill == value) return;
    m_colorMapFill = value;
    emit colorsChanged();
    update();
}

void LevelMeterItem::setDbRange(double value) {
    if (!std::isfinite(value) || m_dbRange == value) return;
    m_dbRange = value;
    emit colorsChanged();
    update();
}

void LevelMeterItem::setScaleShown(bool value) {
    if (m_scaleShown == value) return;
    m_scaleShown = value;
    emit scaleShownChanged();
    update();
}


void LevelMeterItem::setReadoutWanted(bool value) {
    if (m_readoutWanted == value) return;
    m_readoutWanted = value;
    emit readoutWantedChanged();
    if (value) announceReadout();
}

void LevelMeterItem::announceReadout() {
    ++m_readoutRevision;
    emit readoutChanged();
}

void LevelMeterItem::setColor(QColor &member, const QColor &value) {
    if (member == value) return;
    member = value;
    emit colorsChanged();
    update();
}

void LevelMeterItem::noteReadout() {
    // Only for a reader: an idle meter tells nobody.
    if ((m_readoutWanted || QAccessible::isActive()) && !m_readoutTimer.isActive()) {
        m_readoutTimer.start();
    }
}

LevelMeterItem::Channel LevelMeterItem::laneChannel(const std::vector<Channel> &channels, int lane) const {
    if (m_laneCount == 1 && channels.size() > 1) {
        Channel loudest;
        for (const auto &channel : channels) {
            if (channel.level > loudest.level) loudest.level = channel.level;
            // The highest peak, with its hold.
            if (channel.peak > loudest.peak
                || (channel.peak == loudest.peak && channel.hold > loudest.hold)) {
                loudest.peak = channel.peak;
                loudest.hold = channel.hold;
            }
            loudest.clip = std::max(loudest.clip, channel.clip);
        }
        return loudest;
    }
    return lane >= 0 && static_cast<std::size_t>(lane) < channels.size()
        ? channels[static_cast<std::size_t>(lane)] : Channel{};
}

double LevelMeterItem::scalePixels(int lane) const {
    if (!window()) return 0.0;
    const auto [top, bottom] = laneRows(lane, m_laneCount, height());
    // The lane's insets, the lamp and the gap under it.
    const double scale = bottom - top - 2 * kInset - kLampHeight - 1.0;
    return std::max(0.0, scale) * window()->effectiveDevicePixelRatio();
}

std::vector<LevelMeterItem::Painted> LevelMeterItem::painted(const std::vector<Channel> &channels) const {
    std::vector<Painted> result(static_cast<std::size_t>(m_laneCount));
    if (!m_active) return result;
    for (int lane = 0; lane < m_laneCount; ++lane) {
        const Channel channel = laneChannel(channels, lane);
        const double pixels = scalePixels(lane);
        result[static_cast<std::size_t>(lane)] = {
            fillPixels(channel.level, pixels), peakPixels(channel.peak, pixels), channel.clip > 0.0};
    }
    return result;
}

QVariantMap LevelMeterItem::laneReadout(int lane) const {
    const Channel channel = laneChannel(m_shown, lane);
    const auto db = [](double value) {
        return value > kFloorDb ? value : -std::numeric_limits<double>::infinity();
    };
    return {
        {QStringLiteral("level"), m_active ? db(channel.level) : -std::numeric_limits<double>::infinity()},
        {QStringLiteral("peak"), m_active ? db(channel.peak) : -std::numeric_limits<double>::infinity()},
        {QStringLiteral("clipping"), m_active && channel.clip > 0.0},
    };
}

bool LevelMeterItem::visibleForPlayback() const {
    return isVisible() && window() && WindowPresence::of(window())->shown();
}

void LevelMeterItem::bindWindow(QQuickWindow *next) {
    disconnect(m_frameConnection);
    disconnect(m_visibilityConnection);
    if (next) {
        // Presentation drives animation; no fixed-rate timer limits high-refresh displays.
        m_frameConnection = connect(next, &QQuickWindow::frameSwapped,
                                    this, &LevelMeterItem::tick, Qt::QueuedConnection);
        m_visibilityConnection = connect(WindowPresence::of(next), &WindowPresence::shownChanged,
                                         this, &LevelMeterItem::visibilityChanged);
    }
    visibilityChanged();
}

void LevelMeterItem::visibilityChanged() {
    if (!visibleForPlayback()) reset();
    else scheduleTick();
    updateReaderLease();
}

void LevelMeterItem::updateReaderLease() {
    // After reset() cancelled the request of a meter that went away.
    m_readerLease.setPath(visibleForPlayback() ? m_sourcePath : QString());
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
    if (!m_clock) return 0.0;
    const double position = m_clock->positionAt(monotonicNs(now));
    return m_durationSeconds > 0.0 ? std::min(position, m_durationSeconds) : position;
}

void LevelMeterItem::showCurrent() {
    m_shown = m_channels;
    m_shownAt = std::numeric_limits<double>::quiet_NaN();
}

bool LevelMeterItem::showsPainted(const std::vector<Channel> &channels) const {
    return m_painted == painted(channels);
}

double LevelMeterItem::advanceTo(Clock::time_point now) {
    const double elapsed = std::chrono::duration<double>(now - m_frameAt).count();
    m_frameAt = now;
    const double position = currentPosition(now);
    advance(position, elapsed);
    return position;
}

void LevelMeterItem::tick() {
    m_wakeTimer.stop();
    if (!visibleForPlayback()) return;
    const bool moving = m_playing || std::any_of(m_channels.begin(), m_channels.end(),
        [](const Channel &channel) {
            return channel.peak > kFloorDb || channel.level > kFloorDb || channel.clip > 0.0;
        });
    if (!moving) return;
    const auto now = m_now();
    const double position = advanceTo(now);
    if (m_playing) requestWindow(position);
    noteReadout();
    // A frame that would draw the same pixels is skipped. The frame after a
    // change ticks again; otherwise the meter sleeps until its next change.
    if (!showsPainted(m_shown)) update();
    else scheduleWake(now, position);
}

void LevelMeterItem::scheduleWake(Clock::time_point now, double position) {
    // The tallest scale changes first.
    double scaleHeight = 0.0;
    for (int lane = 0; lane < m_laneCount; ++lane) scaleHeight = std::max(scaleHeight, scalePixels(lane));
    if (scaleHeight <= 0.0 || !m_active) return;
    const qint64 nowNs = monotonicNs(now);
    std::optional<qint64> wakeNs;
    const auto wakeAt = [&](qint64 ns) {
        ns = std::max(ns, nowNs);
        wakeNs = wakeNs ? std::min(*wakeNs, ns) : ns;
    };
    const PlaybackClockState clock = m_clock ? m_clock->state() : PlaybackClockState{};
    const Window &data = m_window;
    const bool dataAhead = m_playing && clock.advancing() && data.channels > 0 && data.step > 0.0
        && data.end > position && m_shown.size() == static_cast<std::size_t>(data.channels);
    if (dataAhead) {
        // Play the bins ahead as frames would, until one changes the bars.
        auto channels = m_shown;
        double at = std::isnan(m_shownAt) ? position : m_shownAt;
        const double limit = std::min(data.end, position + kLookaheadSeconds);
        double target = limit;
        const int points = static_cast<int>(data.peaksDb.size() / data.channels);
        int point = static_cast<int>(std::clamp(std::floor((m_readPosition - data.start) / data.step),
                                                0.0, double(points)));
        for (; point < points; ++point) {
            const double end = std::min(data.end, data.start + (point + 1) * data.step);
            if (end > limit) break;
            for (auto &channel : channels) channel.decay(end - at);
            at = std::max(at, end);
            for (int ch = 0; ch < data.channels; ++ch) {
                channels[static_cast<std::size_t>(ch)].observe(
                    data.peaksDb[static_cast<std::size_t>(point * data.channels + ch)], at - end);
            }
            if (!showsPainted(channels)) {
                target = end;
                break;
            }
        }
        const double requestDue = data.end - kRequestLeadSeconds;
        if (!m_requestActive && !m_sourcePath.isEmpty() && data.end < m_durationSeconds
            && requestDue > position) target = std::min(target, requestDue);
        if (const auto ns = clock.reachesAt(target)) wakeAt(*ns);
    } else {
        double seconds = std::numeric_limits<double>::infinity();
        for (const auto &channel : m_shown) seconds = std::min(seconds, secondsUntilChange(channel, scaleHeight));
        if (std::isfinite(seconds)) wakeAt(nowNs + static_cast<qint64>(std::ceil(seconds * 1e9)));
    }
    if (m_playing && !m_requestActive && m_retryAt > now) wakeAt(monotonicNs(m_retryAt));
    if (!wakeNs) return;
    const auto wait = std::chrono::ceil<std::chrono::milliseconds>(std::chrono::nanoseconds(*wakeNs - nowNs));
    m_wakeTimer.start(std::max(wait, std::chrono::milliseconds(1)));
}

void LevelMeterItem::advance(double position, double elapsed) {
    elapsed = std::max(0.0, elapsed);
    if (!m_playing) {
        for (auto &channel : m_channels) channel.decay(elapsed);
        showCurrent();
        return;
    }
    const double previousPosition = m_readPosition;
    // Bins are applied oldest first, each when it ended. `at` is the time of
    // the channels in seconds after the previous frame.
    double at = 0.0;
    double newestStep = 0.0;
    bool observed = false;
    const auto consume = [&](const Window &data) {
        if (data.channels == 0 || data.step <= 0.0 || position <= m_readPosition) return;
        if (m_channels.size() != static_cast<std::size_t>(data.channels)) {
            m_channels.assign(static_cast<std::size_t>(data.channels), {});
        }
        const int points = static_cast<int>(data.peaksDb.size() / data.channels);
        // Every bin that ended since the last frame counts once, also after a
        // long idle wait. Bins before it, such as those of a late decode or of
        // a window resumed after a pause, are never replayed. A bin is
        // eligible only after its end reaches the audible playhead.
        const int first = static_cast<int>(std::clamp(std::floor((m_readPosition - data.start) / data.step), 0.0, double(points)));
        const int last = position >= data.end ? points
            : static_cast<int>(std::clamp(std::floor((position - data.start) / data.step), 0.0, double(points)));
        for (int point = first; point < last; ++point) {
            const double age = std::max(0.0, position - std::min(data.end, data.start + (point + 1) * data.step));
            const double endedAt = elapsed - age;
            if (endedAt > at) {
                for (auto &channel : m_channels) channel.decay(endedAt - at);
                at = endedAt;
            }
            for (int ch = 0; ch < data.channels; ++ch) {
                m_channels[static_cast<std::size_t>(ch)].observe(
                    data.peaksDb[static_cast<std::size_t>(point * data.channels + ch)], at - endedAt);
            }
            observed = true;
            newestStep = data.step;
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
    if (observed) {
        // Show the levels as of the newest bin, not partway into the next.
        m_shown = m_channels;
        m_shownAt = position - (elapsed - at);
        m_shownStep = newestStep;
    }
    const bool showsBin = observed || (!std::isnan(m_shownAt) && position > previousPosition
        && position - m_shownAt <= m_shownStep && m_shown.size() == m_channels.size());
    for (auto &channel : m_channels) channel.decay(elapsed - at);
    // Without a bin due, or with playback stalled, the bars decay from now.
    if (!showsBin) showCurrent();
}

void LevelMeterItem::requestWindow(double position) {
    if (m_requestActive || m_sourcePath.isEmpty() || m_durationSeconds <= position
        || !visibleForPlayback() || m_now() < m_retryAt) return;
    const Window &latest = m_stagedWindow.channels > 0 ? m_stagedWindow : m_window;
    if (latest.channels > 0 && latest.end >= std::min(m_durationSeconds, position + kRequestLeadSeconds)) return;
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
            else m_retryAt = m_now() + std::chrono::seconds(1);
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
    if (node->lanes != m_laneCount) {
        while (auto *child = node->firstChild()) {
            node->removeChildNode(child);
            delete child;
        }
        // The seam: the column's left edge and the rows between lanes.
        node->appendChildNode(new QSGSimpleRectNode);
        for (int i = 0; i < m_laneCount * kNodesPerLane; ++i) {
            if (i % kNodesPerLane == kRampNode) {
                auto *ramp = new RampNode;
                ramp->setFiltering(QSGTexture::Nearest);
                // In the tree, a texture node has a texture to draw: the
                // software renderer looks at it as soon as it is added.
                updateRamp(ramp, window(), scalePixels(i / kNodesPerLane), m_dbRange);
                node->appendChildNode(ramp);
            } else {
                node->appendChildNode(new QSGSimpleRectNode);
            }
        }
        node->lanes = m_laneCount;
    }
    setRect(node->firstChild(), boundingRect(), m_seamColor);
    const double dpr = window()->effectiveDevicePixelRatio();
    const auto snap = [dpr](double value) { return std::round(value * dpr) / dpr; };
    m_painted = painted(m_shown);
    QSGNode *child = node->firstChild()->nextSibling();
    const auto next = [&child](const QRectF &rect, const QColor &color) {
        setRect(child, rect, color);
        child = child->nextSibling();
    };
    // The fill along the map, showing `rows` device pixels of its ramp.
    const auto nextRamp = [&child, this](const QRectF &rect, double scalePixels, int rows) {
        auto *ramp = static_cast<RampNode *>(child);
        updateRamp(ramp, window(), scalePixels, m_dbRange);
        const int height = ramp->ramp->textureSize().height();
        rows = std::clamp(rows, 0, height);
        const QRectF source(0, height - rows, 1, rows);
        if (ramp->sourceRect() != source) ramp->setSourceRect(source);
        if (ramp->rect() != rect) ramp->setRect(rect);
        child = child->nextSibling();
    };
    // Across the track, and along the bar on it.
    const auto track = [](double y, double height) { return QRectF(kTrackLeft, y, kTrackWidth, height); };
    const auto bar = [](double y, double height) { return QRectF(kBarLeft, y, kBarWidth, height); };
    for (int lane = 0; lane < m_laneCount; ++lane) {
        const auto [top, bottom] = laneRows(lane, m_laneCount, height());
        next(QRectF(kTrackLeft, top, std::max(0.0, width() - kTrackLeft), bottom - top), m_backgroundColor);
        const double meterTop = top + kInset;
        const double meterBottom = std::max(meterTop, bottom - kInset);
        next(track(meterTop, meterBottom - meterTop), m_troughColor);
        const Painted &shown = m_painted[static_cast<std::size_t>(lane)];
        const double scaleTop = meterTop + kLampHeight + 1.0;
        const double scaleHeight = std::max(0.0, meterBottom - scaleTop);
        if (!m_active || scaleHeight <= 0.0) {
            for (int i = 2; i < kNodesPerLane; ++i) {
                if (i == kRampNode) nextRamp(QRectF(), scaleHeight * dpr, 0);
                else next(QRectF(), m_backgroundColor);
            }
            continue;
        }
        next(bar(meterTop, kLampHeight), shown.clip ? m_clipColor : m_clipOffColor);
        next(track(meterTop + kLampHeight, 1.0), m_gapColor);
        // The fill and the tick end on device pixels; frames are drawn only
        // when these change. On the colour map, each row of the fill takes
        // the map's colour of its own level, a ramp from the floor up to the
        // level (#373 H10 V2); otherwise the fill is of one colour.
        const double fill = shown.fill / dpr;
        const QRectF fillRect = bar(meterBottom - fill, fill);
        next(m_colorMapFill ? QRectF() : fillRect, m_fillColor);
        nextRamp(m_colorMapFill ? fillRect : QRectF(), scaleHeight * dpr, m_colorMapFill ? shown.fill : 0);
        // The scale's ticks, while the plot shows its scale.
        const std::vector<double> ticks = m_scaleShown ? ticksDb(bottom - top) : std::vector<double>{};
        for (std::size_t i = 0; i < std::size(kTicksDb); ++i) {
            const double y = i < ticks.size()
                ? std::max(scaleTop, snap(meterBottom - meterPosition(ticks[i], scaleHeight))) : 0.0;
            next(i < ticks.size() ? track(y, 1.0) : QRectF(), m_tickColor);
        }
        // The 2 px tick is centred on its level, inside the scale. On the
        // fill, a 1 px gap under it keeps it a mark of its own, whatever
        // the fill's colour.
        const double tickY = std::clamp(meterBottom - shown.peak / dpr - 1.0, scaleTop, meterBottom - 2.0);
        const bool onFill = shown.peak >= 0 && meterBottom - fill < tickY + 3.0 && tickY + 3.0 <= meterBottom;
        next(onFill ? track(tickY + 2.0, 1.0) : QRectF(), m_gapColor);
        next(shown.peak >= 0 ? track(tickY, 2.0) : QRectF(), m_peakColor);
    }
    return node;
}

void LevelMeterItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    // Frames follow pixel changes, so a new size draws and re-plans its own.
    if (newGeometry.size() != oldGeometry.size()) update();
}

void LevelMeterItem::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemDevicePixelRatioHasChanged) update();
}
