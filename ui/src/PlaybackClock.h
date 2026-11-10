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
//
// The properties change only when a view would show it: positionText when
// its second does, and positionSeconds when the position moves by a step,
// such as a device pixel of the seek bar, but no more often than the screen
// shows frames. Without a duration no view places the position, so only the
// text changes then. Code that needs the time itself,
// such as a seek relative to it, reads currentPositionSeconds(); render code
// reads state().
class PlaybackClock : public QObject {
    Q_OBJECT
    // The position at its last step, jump, pause or resume.
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionChanged)
    Q_PROPERTY(QString positionText READ positionText NOTIFY positionTextChanged)
    // The whole second positionText shows, for texts of their own, such as
    // an accessible description; it changes with positionText.
    Q_PROPERTY(int positionSecond READ positionSecond NOTIFY positionTextChanged)
    Q_PROPERTY(bool advancing READ advancing NOTIFY stateChanged)
    // Whether the properties follow playback as it advances, for a window
    // that shows them. Without frames they change with the engine's samples.
    Q_PROPERTY(bool frameUpdatesEnabled READ frameUpdatesEnabled WRITE setFrameUpdatesEnabled
                   NOTIFY frameUpdatesEnabledChanged)
    // The smallest move of the position a view shows, such as a device
    // pixel of the seek bar: positionSeconds changes when the position
    // rounds to another multiple of it. 0 changes it every frame.
    Q_PROPERTY(double positionStepSeconds READ positionStepSeconds WRITE setPositionStepSeconds
                   NOTIFY positionStepSecondsChanged)
    // The refresh rate of the screen that shows the position, in hertz: the
    // properties change at most once a frame of it, however fast the
    // position moves by its steps. 0 or less is kDefaultFrameRate.
    Q_PROPERTY(double frameRate READ frameRate WRITE setFrameRate NOTIFY frameRateChanged)

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
    // The frame rate of a screen that does not report its own.
    static constexpr double kDefaultFrameRate = 60.0;

    explicit PlaybackClock(QObject *parent = nullptr);

    // Tests drive the clock with their own time.
    void setTimeSource(TimeSource source);
    qint64 nowNs() const;

    PlaybackClockState state() const { return m_state; }
    double positionAt(qint64 nowNs) const { return m_state.positionAt(nowNs); }
    // The position now.
    Q_INVOKABLE double currentPositionSeconds() const { return positionAt(nowNs()); }
    // The position at the last step or change.
    double positionSeconds() const { return m_positionSeconds; }
    QString positionText() const { return m_positionText; }
    int positionSecond() const { return m_positionSecond; }
    bool advancing() const { return m_state.advancing(); }

    // Take a sample from the engine. With `jump`, or for another track, the
    // position starts over from the sample instead of being corrected.
    void applySample(const PlaybackSample &sample, bool jump);
    // Show `positionSeconds` without advancing until the next sample, such
    // as the target of a seek the engine has not finished.
    void holdAt(double positionSeconds);
    // The duration of the current track, which bounds the position.
    void setDurationSeconds(double durationSeconds);
    // Update positionSeconds and positionText to now.
    void refresh();
    // What the clock's timer does when it is due: updates the properties to
    // now, positionSeconds only by its steps. Tests that drive the clock
    // with their own time call it at nextTickNs().
    void tick();
    // When the timer is due, in the time source's nanoseconds; none while
    // it is stopped.
    std::optional<qint64> nextTickNs() const { return m_nextTickNs; }

    bool frameUpdatesEnabled() const { return m_frameUpdatesEnabled; }
    void setFrameUpdatesEnabled(bool enabled);

    double positionStepSeconds() const { return m_positionStepSeconds; }
    void setPositionStepSeconds(double stepSeconds);

    double frameRate() const { return m_frameRate; }
    void setFrameRate(double frameRate);
    // One frame of frameRate, rounded up to whole nanoseconds.
    qint64 frameIntervalNs() const;

    static QString formatSeconds(double seconds);
    // The whole second formatSeconds shows: rounded to the nearest.
    static int wholeSecond(double seconds);

signals:
    void stateChanged();
    void positionChanged();
    // The position started over rather than advancing: a seek, a track
    // change, a repeat of the track, or a sample far from the position
    // shown. Whatever counts from the position, such as where a play
    // scrobbles, counts again from here.
    void positionJumped();
    void positionTextChanged();
    void frameUpdatesEnabledChanged();
    void positionStepSecondsChanged();
    void frameRateChanged();

private:
    friend class PlaybackClockTest;

    void setState(const PlaybackClockState &state);
    // Updates the properties to `now`: positionSeconds to the exact
    // position, or only when it has moved by a step and has not moved by
    // one for a frame.
    void update(bool exact);
    // Whether positionSeconds shows `position` somewhere other than `shown`.
    bool showsMove(double shown, double position) const;
    // Whether a frame has passed since positionSeconds last moved by a step.
    bool frameSinceStep(qint64 now) const;
    // Wakes the clock when its properties would next change.
    void scheduleTick(qint64 now, double position);

    TimeSource m_timeSource;
    PlaybackClockState m_state;
    double m_positionSeconds{0.0};
    QString m_positionText{QStringLiteral("00:00")};
    int m_positionSecond{0};
    QTimer m_tickTimer;
    std::optional<qint64> m_nextTickNs;
    // When positionSeconds last moved by its steps; it moves by them again
    // a frame after that at the soonest. An exact change, such as a seek's,
    // starts over.
    std::optional<qint64> m_steppedAtNs;
    bool m_frameUpdatesEnabled{true};
    double m_positionStepSeconds{0.0};
    double m_frameRate{kDefaultFrameRate};
};
