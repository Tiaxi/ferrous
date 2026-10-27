// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "PlaybackClock.h"

class LevelMeterItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY sourcePathChanged)
    // Where playback is; the meter shows the levels at its position.
    Q_PROPERTY(PlaybackClock *clock READ clock WRITE setClock NOTIFY clockChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(int channelCountHint READ channelCountHint WRITE setChannelCountHint NOTIFY channelCountHintChanged)

public:
    explicit LevelMeterItem(QQuickItem *parent = nullptr);
    ~LevelMeterItem() override;

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
        void decay(double seconds);
        void observe(double db, double ageSeconds);
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
    void scheduleTick();
    void tick();
    // Runs the ballistics up to `now`; returns the playback position there.
    double advanceTo(Clock::time_point now);
    void showCurrent();
    double deviceWidth() const;
    bool showsPainted(const std::vector<Channel> &channels, double deviceWidth) const;
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
    // Fill and peak marker device pixels of each channel in the last frame.
    std::vector<int> m_paintedPixels;
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
    std::function<Window(const QString &, double, double, const Cancellation &)> m_decodeWindow{decodeWindow};
};
