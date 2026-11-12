// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <QVariantMap>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "PlaybackClock.h"
#include "WaveformReaderLease.h"

// The level meter of the visualization: a column at the right end of each
// lane, with a 1 px seam on its left, out to the pane's edge. Each column
// is a vertical meter from -60 to 0 dBFS, with its listed levels evenly
// spaced (-6, -12, -24 and -48 dB at 4/5 to 1/5 of the height): a 16 px
// track the lane's height, and on it a 15 px bar out to the column's right
// edge, with the track showing 1 px left of it: the fill from the bottom
// and a 3 px clip lamp at the top. A bar that wide reads as an instrument,
// not as the 6 px thumb of the queue's scrollbar above the pane, whose
// right edge it shares, so it needs no strip of the plot's floor beside it
// (#373 H8; H7's 8 px strip read as a black edge on the window). While
// `colorMapFill` is set, the fill is a ramp of the spectrogram's colours:
// each of its rows takes the map's colour of its own level, along the map
// up to its last colour under kFillLightness of the map's top, so that the
// meter reads as the plot's own scale rather than as its brightest
// highlight (#373 H10 V2). While `scaleShown`,
// 1 px ticks cross the track at 0, -12, -24 and -48 dB (only -12 and -24
// in a lane under 64 px), and a 2 px peak tick, at full brightness, with a
// 1 px gap under it where it is on the fill. Without a track, stopped, it
// is an empty track.
class LevelMeterItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY sourcePathChanged)
    // The lanes of the visualization, each with its column: one per channel,
    // or one for all of them, which shows the loudest.
    Q_PROPERTY(int laneCount READ laneCount WRITE setLaneCount NOTIFY laneCountChanged)
    // Whether a track is playing or paused: otherwise the columns are empty
    // troughs, with no fill, scale or lamp.
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    // The plot's colours (UiPalette's PlotColors).
    Q_PROPERTY(QColor backgroundColor MEMBER m_backgroundColor WRITE setBackgroundColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor seamColor MEMBER m_seamColor WRITE setSeamColor NOTIFY colorsChanged)
    // The scale's gaps across the track, and the one under the lamp.
    Q_PROPERTY(QColor gapColor MEMBER m_gapColor WRITE setGapColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor troughColor MEMBER m_troughColor WRITE setTroughColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor fillColor MEMBER m_fillColor WRITE setFillColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor peakColor MEMBER m_peakColor WRITE setPeakColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor clipOffColor MEMBER m_clipOffColor WRITE setClipOffColor NOTIFY colorsChanged)
    Q_PROPERTY(QColor clipColor MEMBER m_clipColor WRITE setClipColor NOTIFY colorsChanged)
    // The scale's ticks across the track.
    Q_PROPERTY(QColor tickColor MEMBER m_tickColor WRITE setTickColor NOTIFY colorsChanged)
    // Whether the fill is a ramp of the spectrogram's colours, each row in
    // that of its level in a view of `dbRange` dB, rather than fillColor.
    Q_PROPERTY(bool colorMapFill READ colorMapFill WRITE setColorMapFill NOTIFY colorsChanged)
    Q_PROPERTY(double dbRange READ dbRange WRITE setDbRange NOTIFY colorsChanged)
    // Whether the plot shows its scale: the meter then shows its ticks.
    Q_PROPERTY(bool scaleShown READ scaleShown WRITE setScaleShown NOTIFY scaleShownChanged)
    // Whether something reads the levels, such as a tooltip: then
    // readoutChanged() tells it of new ones, a few times a second. It also
    // does while an assistive technology is active.
    Q_PROPERTY(bool readoutWanted READ readoutWanted WRITE setReadoutWanted NOTIFY readoutWantedChanged)
    // Counts readoutChanged(), for bindings that read laneReadout().
    Q_PROPERTY(int readoutRevision READ readoutRevision NOTIFY readoutChanged)
    // Where playback is; the meter shows the levels at its position.
    Q_PROPERTY(PlaybackClock *clock READ clock WRITE setClock NOTIFY clockChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(int channelCountHint READ channelCountHint WRITE setChannelCountHint NOTIFY channelCountHintChanged)

public:
    // The ballistics: the fill rises at once and falls at 20 dB/s; the peak
    // holds for 1.5 s, then falls at 20 dB/s; the clip lamp holds for 2 s.
    static constexpr double kFloorDb = -60.0;
    static constexpr double kReleaseDbPerSecond = 20.0;
    static constexpr double kPeakReleaseDbPerSecond = 20.0;
    static constexpr double kPeakHoldSeconds = 1.5;
    static constexpr double kClipHoldSeconds = 2.0;
    // A bin at full scale clips; its level is rounded to 0.001 of a dB.
    static constexpr double kClipDb = -0.001;
    // The column: a 1 px seam and the track out to its right edge.
    static constexpr double kColumnWidth = 17.0;
    static constexpr double kTrackLeft = 1.0;
    static constexpr double kTrackWidth = kColumnWidth - kTrackLeft;
    // The bar on the track, out to the column's right edge: the fill and
    // the lamp.
    static constexpr double kBarLeft = 2.0;
    static constexpr double kBarWidth = kColumnWidth - kBarLeft;
    // The meter keeps 3 px clear above and below in its lane; its top 3 px
    // are the clip lamp, with a 1 px gap under it.
    static constexpr double kInset = 3.0;
    static constexpr double kLampHeight = 3.0;
    // Lanes under this height have only the -12 and -24 dB ticks.
    static constexpr double kFullScaleLaneHeight = 64.0;
    // The ticks of the scale, in dB, as a lane of `laneHeight` shows them.
    static std::vector<double> ticksDb(double laneHeight);
    // The colour-map fill's lightness at most: this much of the lightness
    // of the map's top (see lightness()).
    static constexpr double kFillLightness = 0.7;
    // A colour's lightness, from 0 to 1: its luma, the Rec. 709 weights of
    // its gamma-encoded components, as a picture's brightness is measured.
    static double lightness(const QColor &color);
    // The highest place on the spectrogram's map, from 0 to 1, whose colour
    // is at most kFillLightness of the lightness of the map's top.
    static double fillCapPosition();
    // The ramp's colour of a level of `db` dBFS on the spectrogram's map in
    // a view of `dbRange` dB: the map's colour, up to that of
    // fillCapPosition(), so that loud levels top out in the map's own
    // orange rather than in a darkened yellow or white (#373 H9 T2).
    static QColor colorMapColor(double db, double dbRange);

    explicit LevelMeterItem(QQuickItem *parent = nullptr);
    ~LevelMeterItem() override;

    // Where `db` is on a meter `height` pixels tall, from its bottom.
    static double meterPosition(double db, double height);
    // The logical rows [top, bottom) of `lane` of `count` in `height`, with
    // a 1 px seam between lanes, as the plots lay them out.
    static std::pair<double, double> laneRows(int lane, int count, double height);

    // The levels of `lane` as shown now: "level" and "peak" in dBFS (-inf
    // below the floor) and whether it "clipping".
    Q_INVOKABLE QVariantMap laneReadout(int lane) const;

    int laneCount() const { return m_laneCount; }
    void setLaneCount(int value);
    bool active() const { return m_active; }
    void setActive(bool value);
    bool readoutWanted() const { return m_readoutWanted; }
    int readoutRevision() const { return m_readoutRevision; }
    void setReadoutWanted(bool value);
    void setBackgroundColor(const QColor &value) { setColor(m_backgroundColor, value); }
    void setSeamColor(const QColor &value) { setColor(m_seamColor, value); }
    void setGapColor(const QColor &value) { setColor(m_gapColor, value); }
    void setTroughColor(const QColor &value) { setColor(m_troughColor, value); }
    void setFillColor(const QColor &value) { setColor(m_fillColor, value); }
    void setPeakColor(const QColor &value) { setColor(m_peakColor, value); }
    void setClipOffColor(const QColor &value) { setColor(m_clipOffColor, value); }
    void setClipColor(const QColor &value) { setColor(m_clipColor, value); }
    void setTickColor(const QColor &value) { setColor(m_tickColor, value); }
    bool colorMapFill() const { return m_colorMapFill; }
    void setColorMapFill(bool value);
    double dbRange() const { return m_dbRange; }
    void setDbRange(double value);
    bool scaleShown() const { return m_scaleShown; }
    void setScaleShown(bool value);

    QString sourcePath() const { return m_sourcePath; }
    PlaybackClock *clock() const { return m_clock; }
    double durationSeconds() const { return m_durationSeconds; }
    bool playing() const { return m_playing; }
    int channelCountHint() const { return m_channelCountHint; }
    void setSourcePath(const QString &value);
    void setClock(PlaybackClock *value);
    void setDurationSeconds(double value);
    void setPlaying(bool value);
    void setChannelCountHint(int value);

signals:
    void sourcePathChanged();
    void clockChanged();
    void durationSecondsChanged();
    void playingChanged();
    void channelCountHintChanged();
    void laneCountChanged();
    void activeChanged();
    void colorsChanged();
    void readoutWantedChanged();
    void scaleShownChanged();
    // The levels laneReadout() gives changed.
    void readoutChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    using Clock = std::chrono::steady_clock;
    using Cancellation = std::shared_ptr<std::atomic_bool>;
    struct Channel {
        double level{-60.0};
        double peak{-60.0};
        double hold{0.0};
        // How much longer the clip lamp stays lit.
        double clip{0.0};
        void decay(double seconds);
        void observe(double db, double ageSeconds);
    };
    // What a lane's column draws, in device pixels from the bottom of its
    // scale: the fill, the peak tick (-1 while hidden) and the clip lamp.
    struct Painted {
        int fill{0};
        int peak{-1};
        bool clip{false};
        bool operator==(const Painted &) const = default;
    };
    struct Window {
        double start{0.0};
        double end{0.0};
        double step{0.0};
        int channels{0};
        std::vector<float> peaksDb;
    };

    static Window decodeWindow(const QString &path, double start, double end,
                               const Cancellation &cancelled);
    static Window parseWindow(const QByteArray &bytes);
    void reset();
    void cancelRequest();
    void bindWindow(QQuickWindow *window);
    void visibilityChanged();
    bool visibleForPlayback() const;
    // Tells the detail service which file the meter reads: its source
    // while it is visible for playback, none otherwise.
    void updateReaderLease();
    void scheduleTick();
    void tick();
    // Runs the ballistics up to `now`; returns the playback position there.
    double advanceTo(Clock::time_point now);
    void showCurrent();
    void setColor(QColor &member, const QColor &value);
    // The channel `lane` shows: one channel, or the loudest of all.
    Channel laneChannel(const std::vector<Channel> &channels, int lane) const;
    // The device pixels of the scale of `lane`.
    double scalePixels(int lane) const;
    std::vector<Painted> painted(const std::vector<Channel> &channels) const;
    bool showsPainted(const std::vector<Channel> &channels) const;
    void noteReadout();
    void scheduleWake(Clock::time_point now, double position);
    void requestWindow(double position);
    void advance(double position, double elapsed);
    double currentPosition(Clock::time_point now) const;
    void clockStateChanged();

    QString m_sourcePath;
    QPointer<PlaybackClock> m_clock;
    // The clock's epoch the meter last followed; a new one is a jump.
    quint64 m_clockEpoch{0};
    double m_durationSeconds{0.0};
    bool m_playing{false};
    int m_channelCountHint{2};
    // The meter's time; tests drive it by hand.
    std::function<Clock::time_point()> m_now{&Clock::now};
    Clock::time_point m_frameAt{Clock::now()};
    Clock::time_point m_retryAt{};
    double m_readPosition{0.0};
    std::vector<Channel> m_channels{2};
    // The channels as drawn. While playback consumes bins, they stay at the
    // end of the newest bin heard (m_shownAt, in track seconds), so a steady
    // signal shows steady bars; otherwise they are m_channels (m_shownAt NaN).
    std::vector<Channel> m_shown{2};
    double m_shownAt{std::numeric_limits<double>::quiet_NaN()};
    double m_shownStep{0.0};
    // What each lane's column drew in the last frame.
    std::vector<Painted> m_painted;
    int m_laneCount{1};
    bool m_active{false};
    bool m_readoutWanted{false};
    int m_readoutRevision{0};
    void announceReadout();
    QColor m_backgroundColor{QStringLiteral("#000000")};
    QColor m_seamColor{QStringLiteral("#1a1a1a")};
    QColor m_gapColor{QStringLiteral("#000000")};
    QColor m_troughColor{QStringLiteral("#2a2a2a")};
    QColor m_fillColor{QStringLiteral("#a3a3a3")};
    QColor m_peakColor{QStringLiteral("#f4f4f6")};
    QColor m_clipOffColor{QColor::fromRgbF(1, 1, 1, 0.2)};
    QColor m_clipColor{QStringLiteral("#ed5a6a")};
    QColor m_tickColor{QColor::fromRgbF(1, 1, 1, 0.4)};
    bool m_colorMapFill{false};
    double m_dbRange{132.0};
    bool m_scaleShown{false};
    // Tells readers of the levels of new ones, at most every 250 ms.
    QTimer m_readoutTimer;
    // Wakes the meter when the drawn levels next change, while no frame is due.
    QTimer m_wakeTimer;
    Window m_window;
    Window m_stagedWindow;
    quint64 m_generation{0};
    bool m_requestActive{false};
    bool m_tickQueued{false};
    Cancellation m_cancelled;
    QMetaObject::Connection m_frameConnection;
    QMetaObject::Connection m_visibilityConnection;
    QMetaObject::Connection m_clockConnection;
    WaveformReaderLease m_readerLease{WaveformReaderLease::Kind::Meter};
    std::function<Window(const QString &, double, double, const Cancellation &)> m_decodeWindow{decodeWindow};
};
