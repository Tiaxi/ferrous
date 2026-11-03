// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <cstdint>

// What a waveform view or the level meter reads of the Rust waveform detail
// service: the file it asks for windows of, while it is shown. The service
// keeps what its readers need of a file and frees the rest: the fine caches
// of a file only the meter reads, and the session of a file nothing reads,
// keeping the coarse summaries of the current track for when a view is
// shown again.
//
// Retaining and releasing are cheap and run on the GUI thread, in the order
// the items change. Freeing what is released can take a while, as closing
// a GStreamer decoder does, so it runs on the global thread pool, and finds
// kept what a reader retained since.
class WaveformReaderLease {
public:
    enum class Kind : std::uint32_t { View = 0, Meter = 1 };

    // The service calls, which tests replace.
    struct Backend {
        void (*retain)(const QByteArray &path, Kind kind);
        void (*release)(const QByteArray &path, Kind kind);
        // Free what no reader needs; called off the GUI thread.
        void (*releaseUnread)();
    };

    explicit WaveformReaderLease(Kind kind) : m_kind(kind) {}
    ~WaveformReaderLease();
    WaveformReaderLease(const WaveformReaderLease &) = delete;
    WaveformReaderLease &operator=(const WaveformReaderLease &) = delete;

    // Read the file at `path` from now on; an empty path reads none.
    void setPath(const QString &path);
    const QString &path() const { return m_path; }

    static Backend &backend();

private:
    const Kind m_kind;
    QString m_path;
};
