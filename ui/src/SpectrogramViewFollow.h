// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QMutex>
#include <QObject>
#include <QTimer>

#include <limits>

#include "PlaybackClock.h"
#include "ViewFollow.h"

// Where a centered spectrogram's view is, shared by the lanes of a plot, which
// each draw one channel of the same view: following the playhead, where a
// middle-drag on any of them moved it, or gliding back to the playhead from
// there (see ViewFollow, which the waveform editor shares).
//
// The lanes drive it: each tells it of the playback clock's changes, of the
// view's width and the track's length, and of the playhead as frames show
// it; it takes each change once, whichever lane tells it first. It works out
// where a detached view starts the glide back, and tells the analysis, through
// backendAnchorChanged, where the view is centred, so that sessions decode the
// view rather than the playhead's.
//
// Its methods are for the GUI thread, except snapshot(), which any thread may
// call; it calls nothing of its lanes, and tells them of a change with a
// queued signal.
class SpectrogramViewFollow : public QObject {
    Q_OBJECT
    // Whether the view stays where a middle-drag moved it rather than
    // following the playhead. It follows again once the playhead, moving
    // through it, reaches ViewFollow::kReattachViewFraction of its width, and
    // at once on followPlayhead(), a seek other than the plot's own, a track
    // change, or a reset of the zoom. A view gliding back is not detached.
    Q_PROPERTY(bool detached READ detached NOTIFY detachedChanged)

public:
    // How long the analysis waits at least between the anchors of a view
    // that moves, as a drag moves it: the last of them always reaches it.
    static constexpr int kBackendAnchorIntervalMs = 50;

    struct Snapshot {
        ViewFollow::State state;
        // Detached: the time that stays at anchorFraction of the view's width.
        double anchorSeconds{0.0};
        double anchorFraction{0.5};
        // Where the playhead starts the glide back, or infinity for none.
        double reattachSeconds{std::numeric_limits<double>::infinity()};
    };

    explicit SpectrogramViewFollow(QObject *parent = nullptr);

    bool detached() const { return m_detached; }
    Snapshot snapshot() const;
    // How a view of `snapshot` is placed with the playhead at `position`.
    static ViewFollow::State stateAt(const Snapshot &snapshot, double position);

    // The view stays where it is, with `anchorSeconds` at `fraction` of its
    // width: a pan or a zoom about the pointer. A view newly detached forgets
    // the plot's own seek, and glides back only once a pan has ended.
    void detach(double anchorSeconds, double fraction);
    // Whether a drag pans the view: until it ends, nothing glides back.
    void setPanning(bool panning);
    // The view `spanSeconds` wide at the zoom asked for, of a track
    // `endSeconds` long, or showing the whole track, with the playhead at
    // `position`. A detached view of the whole track follows the playhead;
    // one whose width changed glides back only if the playhead is left of
    // the new re-attach point.
    void setGeometry(double spanSeconds, double endSeconds, bool wholeTrack, double position);
    // A detached view glides back once the playhead reaches its re-attach
    // point only if it is left of it now, at `position`.
    void arm(double position);
    // Takes a glide begun or ended with the playhead at `position`.
    void settle(double position);
    // The plot asked to seek to `seconds`: a detached view stays while that
    // seek lands.
    void noteOwnSeek(double seconds);
    // The clock moved to `next`, `nowNs` on its clock: a seek other than the
    // plot's own, a stop or a track change follows the playhead again. Each
    // lane tells it; it takes a state once.
    void clockChanged(const PlaybackClockState &next, qint64 nowNs);
    // The view follows the playhead again, at once.
    void reattach();
    Q_INVOKABLE void followPlayhead() { reattach(); }

signals:
    void detachedChanged();
    // Where the view is placed changed; queued to the lanes.
    void changed();
    // Where the analysis decodes the view of the track of `trackToken`:
    // around `centerSeconds`, or around the playhead for NaN.
    void backendAnchorChanged(quint64 trackToken, double centerSeconds);

private:
    void publish();
    void sendBackendAnchor();
    double reattachPointLocked() const;
    double detachedLeftLocked() const;
    void reattachLocked();
    void armLocked(double position);

    mutable QMutex m_mutex;
    ViewFollow::State m_state;
    double m_anchorSeconds{0.0};
    double m_anchorFraction{0.5};
    bool m_reattachArmed{false};
    bool m_panning{false};
    double m_spanSeconds{0.0};
    double m_endSeconds{0.0};
    // The time the plot last asked to seek to while detached, or NaN. Once a
    // jump has landed there, a later jump is the same seek only from there,
    // as the engine confirms it.
    double m_ownSeekSeconds{std::numeric_limits<double>::quiet_NaN()};
    bool m_ownSeekLanded{false};
    PlaybackClockState m_clockState;
    bool m_clockKnown{false};

    // GUI thread: what detached and the analysis were last told.
    bool m_detached{false};
    quint64 m_sentToken{0};
    double m_sentCenter{std::numeric_limits<double>::quiet_NaN()};
    qint64 m_sentAtMs{0};
    QTimer m_anchorTimer;
};
