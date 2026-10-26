// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <chrono>
#include <functional>
#include <optional>

// Nanoseconds on CLOCK_MONOTONIC: the clock the playback engine stamps its
// position samples with (src/playback/clock.rs). std::chrono::steady_clock
// reads the same clock on Linux, so render code can convert its time points.
qint64 monotonicNowNs();
qint64 monotonicNs(std::chrono::steady_clock::time_point time);

// A position the engine read from the pipeline.
struct PlaybackSample {
    double positionSeconds{0.0};
    // When the engine read it; 0 if unknown, which reads as now.
    qint64 sampledAtNs{0};
    // Whether the position advances in real time from sampledAtNs.
    bool advancing{false};
    double durationSeconds{0.0};
    quint64 trackToken{0};
};

// Where playback is: anchorSeconds at anchorNs, advancing by rate seconds per
// second after that. A copy can be evaluated on any thread.
struct PlaybackClockState {
    double anchorSeconds{0.0};
    qint64 anchorNs{0};
    double rate{0.0};
    double durationSeconds{0.0};
    quint64 trackToken{0};
    // Changes whenever the position jumps: a seek, a track change, a stop,
    // or a correction too large to make up gradually.
    quint64 epoch{0};

    bool advancing() const { return rate > 0.0; }
    // Clamped to [0, duration]; constant before anchorNs.
    double positionAt(qint64 nowNs) const;
    // The first time positionAt reaches `position`: the lowest time if it
    // always has, or none if it never does.
    std::optional<qint64> reachesAt(double position) const;
};

// The one playback clock of the UI. The bridge feeds it the engine's raw
// position samples; every consumer (transport bar, spectrogram, waveforms,
// level meter, MPRIS) reads it instead of extrapolating on its own.
//
// While playback advances, a new sample that differs from the displayed
// position by a little, such as a position the sink quantized to a buffer
// edge, is made up by running slightly faster or slower until the next
// sample. The displayed position never goes back then. A seek, a track
// change, a stop, or a larger difference starts over from the sample.
class PlaybackClock : public QObject {
    Q_OBJECT
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionTextChanged)
    Q_PROPERTY(bool advancing READ advancing NOTIFY stateChanged)
    // Whether positionSeconds follows playback frame by frame, for a window
    // that shows it. Without frames it changes with the engine's samples.
    Q_PROPERTY(bool frameUpdatesEnabled READ frameUpdatesEnabled WRITE setFrameUpdatesEnabled
                   NOTIFY frameUpdatesEnabledChanged)

public:
    using TimeSource = std::function<qint64()>;

    // The largest difference from a sample that is made up gradually.
    static constexpr double kMaxSlewErrorSeconds = 0.25;
    // A difference is made up over about this long.
    static constexpr double kSlewHorizonSeconds = 0.5;
    // How much faster or slower than real time the position may run.
    static constexpr double kMaxRateOffset = 0.05;
    // A jump to a sample or seek target smaller than this is no
    // discontinuity, such as a seek that lands on its target.
    static constexpr double kJumpThresholdSeconds = 0.03;
    // How often positionSeconds updates while playback advances, with frame
    // updates enabled.
    static constexpr int kFrameIntervalMs = 16;

    explicit PlaybackClock(QObject *parent = nullptr);

    // Tests drive the clock with their own time.
    void setTimeSource(TimeSource source);
    qint64 nowNs() const;

    PlaybackClockState state() const { return m_state; }
    double positionAt(qint64 nowNs) const { return m_state.positionAt(nowNs); }
    // The position at the last frame or change.
    double positionSeconds() const { return m_positionSeconds; }
    QString positionText() const { return m_positionText; }
    bool advancing() const { return m_state.advancing(); }

    // Take a sample from the engine. With `jump`, or for another track, the
    // position starts over from the sample instead of being corrected.
    void applySample(const PlaybackSample &sample, bool jump);
    // Show `positionSeconds` without advancing until the next sample, such
    // as the target of a seek the engine has not finished.
    void holdAt(double positionSeconds);
    // The duration of the current track, which bounds the position.
    void setDurationSeconds(double durationSeconds);
    // Update positionSeconds to now.
    void refresh();

    bool frameUpdatesEnabled() const { return m_frameUpdatesEnabled; }
    void setFrameUpdatesEnabled(bool enabled);

    static QString formatSeconds(double seconds);

signals:
    void stateChanged();
    void positionChanged();
    void positionTextChanged();
    void frameUpdatesEnabledChanged();

private:
    friend class PlaybackClockTest;

    void setState(const PlaybackClockState &state);
    void updateFrameTimer();

    TimeSource m_timeSource;
    PlaybackClockState m_state;
    double m_positionSeconds{0.0};
    QString m_positionText{QStringLiteral("00:00")};
    QTimer m_frameTimer;
    bool m_frameUpdatesEnabled{true};
};
