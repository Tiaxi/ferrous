// SPDX-License-Identifier: GPL-3.0-or-later

#include "WaveformReaderLease.h"

#include <QByteArray>
#include <QThreadPool>

#include <utility>

#include "FerrousBridgeFfi.h"

namespace {

void retainReader(const QByteArray &path, WaveformReaderLease::Kind kind) {
    ferrous_ffi_waveform_reader_retain(reinterpret_cast<const std::uint8_t *>(path.constData()),
                                       static_cast<std::size_t>(path.size()),
                                       static_cast<std::uint32_t>(kind));
}

void releaseReader(const QByteArray &path, WaveformReaderLease::Kind kind) {
    ferrous_ffi_waveform_reader_release(reinterpret_cast<const std::uint8_t *>(path.constData()),
                                        static_cast<std::size_t>(path.size()),
                                        static_cast<std::uint32_t>(kind));
}

// Frees what is released, off the GUI thread. A pass finds what readers
// hold when it runs, so one that runs after a later retain keeps that.
void scheduleReleaseUnread() {
    // Read here, on the GUI thread, which alone changes the backend.
    QThreadPool::globalInstance()->start([releaseUnread = WaveformReaderLease::backend().releaseUnread] {
        releaseUnread();
    });
}

} // namespace

WaveformReaderLease::Backend &WaveformReaderLease::backend() {
    static Backend backend{retainReader, releaseReader, ferrous_ffi_waveform_release_unread};
    return backend;
}

WaveformReaderLease::~WaveformReaderLease() { setPath({}); }

void WaveformReaderLease::setPath(const QString &path) {
    if (path == m_path) return;
    // The new file first: a view moving between windows, or a track
    // played again, keeps what it reads.
    if (!path.isEmpty()) backend().retain(path.toUtf8(), m_kind);
    const QString previous = std::exchange(m_path, path);
    if (previous.isEmpty()) return;
    backend().release(previous.toUtf8(), m_kind);
    scheduleReleaseUnread();
}
