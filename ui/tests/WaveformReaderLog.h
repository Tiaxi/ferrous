// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <atomic>
#include <map>
#include <utility>

#include "../src/WaveformReaderLease.h"

// Records what the items tell the waveform detail service of the files
// they read, in place of the service, while it lives.
class WaveformReaderLog {
public:
    using Kind = WaveformReaderLease::Kind;

    WaveformReaderLog() : m_saved(WaveformReaderLease::backend()) {
        counts().clear();
        order().clear();
        releasesUnread().store(0);
        WaveformReaderLease::backend() = {
            [](const QByteArray &path, Kind kind) {
                ++counts()[{QString::fromUtf8(path), kind}];
                order().append(QStringLiteral("+") + label(QString::fromUtf8(path), kind));
            },
            [](const QByteArray &path, Kind kind) {
                const auto key = std::make_pair(QString::fromUtf8(path), kind);
                if (--counts()[key] == 0) counts().erase(key);
                order().append(QStringLiteral("-") + label(QString::fromUtf8(path), kind));
            },
            [] { ++releasesUnread(); },
        };
    }
    ~WaveformReaderLog() { WaveformReaderLease::backend() = m_saved; }
    WaveformReaderLog(const WaveformReaderLog &) = delete;
    WaveformReaderLog &operator=(const WaveformReaderLog &) = delete;

    // The readers held, as "path kind", sorted; a count above one repeats.
    static QStringList held() {
        QStringList readers;
        for (const auto &[key, count] : counts()) {
            for (int i = 0; i < count; ++i) readers.append(label(key.first, key.second));
        }
        return readers;
    }
    // Every retain (+) and release (-), in order.
    static QStringList &order() {
        static QStringList events;
        return events;
    }
    // How many passes freed what was released.
    static std::atomic_int &releasesUnread() {
        static std::atomic_int passes{0};
        return passes;
    }

private:
    static QString label(const QString &path, Kind kind) {
        return path + (kind == Kind::View ? QStringLiteral(" view") : QStringLiteral(" meter"));
    }
    static std::map<std::pair<QString, Kind>, int> &counts() {
        static std::map<std::pair<QString, Kind>, int> held;
        return held;
    }

    WaveformReaderLease::Backend m_saved;
};
