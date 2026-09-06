// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QQuickItem>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

class LevelMeterItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString sourcePath READ sourcePath WRITE setSourcePath NOTIFY sourcePathChanged)
    Q_PROPERTY(double positionSeconds READ positionSeconds WRITE setPositionSeconds NOTIFY positionSecondsChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds WRITE setDurationSeconds NOTIFY durationSecondsChanged)
    Q_PROPERTY(bool playing READ playing WRITE setPlaying NOTIFY playingChanged)
    Q_PROPERTY(int channelCountHint READ channelCountHint WRITE setChannelCountHint NOTIFY channelCountHintChanged)

public:
    explicit LevelMeterItem(QQuickItem *parent = nullptr);
    ~LevelMeterItem() override;

    QString sourcePath() const { return m_sourcePath; }
    double positionSeconds() const { return m_positionSeconds; }
    double durationSeconds() const { return m_durationSeconds; }
    bool playing() const { return m_playing; }
    int channelCountHint() const { return m_channelCountHint; }
    void setSourcePath(const QString &value);
    void setPositionSeconds(double value);
    void setDurationSeconds(double value);
    void setPlaying(bool value);
    void setChannelCountHint(int value);
    Q_INVOKABLE void resetForSeek(double seconds);

signals:
    void sourcePathChanged();
    void positionSecondsChanged();
    void durationSecondsChanged();
    void playingChanged();
    void channelCountHintChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

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
    void requestWindow(double position);
    void advance(double position, double elapsed);
    double currentPosition(Clock::time_point now) const;

    QString m_sourcePath;
    double m_positionSeconds{0.0};
    double m_durationSeconds{0.0};
    bool m_playing{false};
    int m_channelCountHint{2};
    Clock::time_point m_positionAt{Clock::now()};
    Clock::time_point m_frameAt{Clock::now()};
    Clock::time_point m_retryAt{};
    double m_readPosition{0.0};
    std::vector<Channel> m_channels{2};
    Window m_window;
    Window m_stagedWindow;
    quint64 m_generation{0};
    bool m_requestActive{false};
    bool m_tickQueued{false};
    Cancellation m_cancelled;
    QMetaObject::Connection m_frameConnection;
    QMetaObject::Connection m_visibilityConnection;
    std::function<Window(const QString &, double, double, const Cancellation &)> m_decodeWindow{decodeWindow};
};
