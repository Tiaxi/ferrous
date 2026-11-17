// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtEndian>

namespace flac_fixture {

inline QByteArray littleEndian32(qsizetype value) {
    const quint32 encoded = qToLittleEndian(static_cast<quint32>(value));
    return QByteArray(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}

// A FLAC stream with a STREAMINFO block, the given Vorbis comments, and no audio frames.
inline QByteArray flacWithComments(const QStringList &comments) {
    QByteArray bytes("fLaC");
    bytes.append(QByteArray::fromHex("00000022"));
    bytes.append(QByteArray::fromHex("10001000000000000000"));
    // 44.1 kHz, two channels, 16 bits, one second of samples.
    const quint64 packed = (quint64(44100) << 44) | (quint64(1) << 41) | (quint64(15) << 36) | 44100;
    const quint64 encodedPacked = qToBigEndian(packed);
    bytes.append(reinterpret_cast<const char *>(&encodedPacked), sizeof(encodedPacked));
    bytes.append(QByteArray(16, '\0'));

    const QByteArray vendor("ferrous-test");
    QByteArray block = littleEndian32(vendor.size()) + vendor + littleEndian32(comments.size());
    for (const QString &comment : comments) {
        const QByteArray entry = comment.toUtf8();
        block.append(littleEndian32(entry.size()) + entry);
    }
    const quint32 blockHeader = qToBigEndian(quint32(0x84000000) | quint32(block.size()));
    bytes.append(reinterpret_cast<const char *>(&blockHeader), sizeof(blockHeader));
    return bytes + block;
}

} // namespace flac_fixture
