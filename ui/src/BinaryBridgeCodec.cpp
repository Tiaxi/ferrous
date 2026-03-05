#include "BinaryBridgeCodec.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <QtEndian>

namespace BinaryBridgeCodec {
namespace {

constexpr qsizetype kHeaderSize = 12;

class Reader {
public:
    explicit Reader(const QByteArray &bytes)
        : m_bytes(bytes)
        , m_offset(0) {}

    bool atEnd() const {
        return m_offset == m_bytes.size();
    }

    bool readU8(quint8 *out) {
        if (!out || m_offset + 1 > m_bytes.size()) {
            return false;
        }
        *out = static_cast<quint8>(m_bytes[m_offset]);
        m_offset += 1;
        return true;
    }

    bool readU16(quint16 *out) {
        if (!out || m_offset + 2 > m_bytes.size()) {
            return false;
        }
        *out = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(m_bytes.constData() + m_offset));
        m_offset += 2;
        return true;
    }

    bool readI32(qint32 *out) {
        if (!out || m_offset + 4 > m_bytes.size()) {
            return false;
        }
        *out = qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(m_bytes.constData() + m_offset));
        m_offset += 4;
        return true;
    }

    bool readU32(quint32 *out) {
        if (!out || m_offset + 4 > m_bytes.size()) {
            return false;
        }
        *out = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(m_bytes.constData() + m_offset));
        m_offset += 4;
        return true;
    }

    bool readF32(float *out) {
        if (!out || m_offset + 4 > m_bytes.size()) {
            return false;
        }
        quint32 bits = 0;
        if (!readU32(&bits)) {
            return false;
        }
        float value = 0.0f;
        static_assert(sizeof(value) == sizeof(bits), "float size mismatch");
        std::memcpy(&value, &bits, sizeof(value));
        *out = value;
        return true;
    }

    bool readF64(double *out) {
        if (!out || m_offset + 8 > m_bytes.size()) {
            return false;
        }
        quint64 bits = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar *>(m_bytes.constData() + m_offset));
        m_offset += 8;
        double value = 0.0;
        static_assert(sizeof(value) == sizeof(bits), "double size mismatch");
        std::memcpy(&value, &bits, sizeof(value));
        *out = value;
        return true;
    }

    bool readUtf8U16(QString *out) {
        if (!out) {
            return false;
        }
        quint16 len = 0;
        if (!readU16(&len)) {
            return false;
        }
        if (m_offset + len > m_bytes.size()) {
            return false;
        }
        *out = QString::fromUtf8(m_bytes.constData() + m_offset, len);
        m_offset += len;
        return true;
    }

    bool readBytes(quint32 len, QByteArray *out) {
        if (!out || len > static_cast<quint32>(std::numeric_limits<qsizetype>::max())) {
            return false;
        }
        const qsizetype qlen = static_cast<qsizetype>(len);
        if (m_offset + qlen > m_bytes.size()) {
            return false;
        }
        *out = m_bytes.mid(m_offset, qlen);
        m_offset += qlen;
        return true;
    }

private:
    const QByteArray &m_bytes;
    qsizetype m_offset;
};

template <typename T>
void appendLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<int>(sizeof(T)));
}

void appendUtf8U16(QByteArray &out, const QString &text) {
    QByteArray utf8 = text.toUtf8();
    if (utf8.size() > static_cast<int>(std::numeric_limits<quint16>::max())) {
        utf8.truncate(static_cast<int>(std::numeric_limits<quint16>::max()));
    }
    appendLe<quint16>(out, static_cast<quint16>(utf8.size()));
    out.append(utf8);
}

QByteArray finalizeCommand(quint16 cmdId, const QByteArray &payload) {
    if (payload.size() > static_cast<int>(std::numeric_limits<quint16>::max())) {
        return {};
    }
    QByteArray out;
    out.reserve(4 + payload.size());
    appendLe<quint16>(out, cmdId);
    appendLe<quint16>(out, static_cast<quint16>(payload.size()));
    out.append(payload);
    return out;
}

bool decodePlaybackSection(const QByteArray &payload, DecodedPlayback *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint8 state = 0;
    quint8 repeatMode = 0;
    quint8 shuffleEnabled = 0;
    qint32 currentQueueIndex = -1;
    QString currentPath;
    if (!reader.readU8(&state)
        || !reader.readF64(&out->positionSeconds)
        || !reader.readF64(&out->durationSeconds)
        || !reader.readF32(&out->volume)
        || !reader.readU8(&repeatMode)
        || !reader.readU8(&shuffleEnabled)
        || !reader.readI32(&currentQueueIndex)
        || !reader.readUtf8U16(&currentPath)
        || !reader.atEnd()) {
        return false;
    }
    out->present = true;
    out->state = static_cast<int>(state);
    out->repeatMode = static_cast<int>(repeatMode);
    out->shuffleEnabled = shuffleEnabled != 0;
    out->currentQueueIndex = static_cast<int>(currentQueueIndex);
    out->currentPath = currentPath;
    return true;
}

bool decodeQueueSection(const QByteArray &payload, DecodedQueue *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint32 len = 0;
    qint32 selectedIndex = -1;
    quint32 unknownDurationCount = 0;
    quint32 trackCount = 0;
    if (!reader.readU32(&len)
        || !reader.readI32(&selectedIndex)
        || !reader.readF64(&out->totalDurationSeconds)
        || !reader.readU32(&unknownDurationCount)
        || !reader.readU32(&trackCount)) {
        return false;
    }

    QVector<DecodedQueueTrack> tracks;
    tracks.reserve(static_cast<int>(trackCount));
    for (quint32 i = 0; i < trackCount; ++i) {
        QString title;
        QString artist;
        QString album;
        QString genre;
        qint32 year = std::numeric_limits<int>::min();
        float lengthSeconds = -1.0f;
        QString path;
        if (!reader.readUtf8U16(&title)
            || !reader.readUtf8U16(&artist)
            || !reader.readUtf8U16(&album)
            || !reader.readUtf8U16(&genre)
            || !reader.readI32(&year)
            || !reader.readF32(&lengthSeconds)
            || !reader.readUtf8U16(&path)) {
            return false;
        }
        DecodedQueueTrack item;
        item.title = title;
        item.artist = artist;
        item.album = album;
        item.genre = genre;
        item.year = year;
        item.lengthSeconds = lengthSeconds;
        item.path = path;
        tracks.push_back(std::move(item));
    }
    if (!reader.atEnd()) {
        return false;
    }

    out->present = true;
    out->len = static_cast<int>(len);
    out->selectedIndex = static_cast<int>(selectedIndex);
    out->unknownDurationCount = static_cast<int>(unknownDurationCount);
    out->tracks = std::move(tracks);
    return true;
}

bool decodeLibraryMetaSection(const QByteArray &payload, DecodedLibraryMeta *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint32 rootCount = 0;
    quint32 trackCount = 0;
    quint32 artistCount = 0;
    quint32 albumCount = 0;
    quint8 scanInProgress = 0;
    qint32 sortMode = 0;
    QString lastError;
    quint32 rootsCompleted = 0;
    quint32 rootsTotal = 0;
    quint32 filesDiscovered = 0;
    quint32 filesProcessed = 0;
    float filesPerSecond = 0.0f;
    float etaSeconds = -1.0f;
    quint16 rootPathCount = 0;

    if (!reader.readU32(&rootCount)
        || !reader.readU32(&trackCount)
        || !reader.readU32(&artistCount)
        || !reader.readU32(&albumCount)
        || !reader.readU8(&scanInProgress)
        || !reader.readI32(&sortMode)
        || !reader.readUtf8U16(&lastError)
        || !reader.readU32(&rootsCompleted)
        || !reader.readU32(&rootsTotal)
        || !reader.readU32(&filesDiscovered)
        || !reader.readU32(&filesProcessed)
        || !reader.readF32(&filesPerSecond)
        || !reader.readF32(&etaSeconds)
        || !reader.readU16(&rootPathCount)) {
        return false;
    }

    QStringList rootPaths;
    rootPaths.reserve(rootPathCount);
    for (quint16 i = 0; i < rootPathCount; ++i) {
        QString root;
        if (!reader.readUtf8U16(&root)) {
            return false;
        }
        rootPaths.push_back(root);
    }
    if (!reader.atEnd()) {
        return false;
    }

    out->present = true;
    out->rootCount = static_cast<int>(rootCount);
    out->trackCount = static_cast<int>(trackCount);
    out->artistCount = static_cast<int>(artistCount);
    out->albumCount = static_cast<int>(albumCount);
    out->scanInProgress = scanInProgress != 0;
    out->sortMode = static_cast<int>(sortMode);
    out->lastError = lastError;
    out->rootsCompleted = static_cast<int>(rootsCompleted);
    out->rootsTotal = static_cast<int>(rootsTotal);
    out->filesDiscovered = static_cast<int>(filesDiscovered);
    out->filesProcessed = static_cast<int>(filesProcessed);
    out->filesPerSecond = static_cast<double>(filesPerSecond);
    out->etaSeconds = static_cast<double>(etaSeconds);
    out->rootPaths = rootPaths;
    return true;
}

bool decodeMetadataSection(const QByteArray &payload, DecodedMetadata *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint32 sampleRateHz = 0;
    quint32 bitrateKbps = 0;
    quint16 channels = 0;
    quint16 bitDepth = 0;
    if (!reader.readUtf8U16(&out->sourcePath)
        || !reader.readUtf8U16(&out->title)
        || !reader.readUtf8U16(&out->artist)
        || !reader.readUtf8U16(&out->album)
        || !reader.readUtf8U16(&out->genre)
        || !reader.readI32(&out->year)
        || !reader.readU32(&sampleRateHz)
        || !reader.readU32(&bitrateKbps)
        || !reader.readU16(&channels)
        || !reader.readU16(&bitDepth)
        || !reader.readUtf8U16(&out->coverPath)
        || !reader.atEnd()) {
        return false;
    }
    out->present = true;
    out->sampleRateHz = static_cast<int>(sampleRateHz);
    out->bitrateKbps = static_cast<int>(bitrateKbps);
    out->channels = static_cast<int>(channels);
    out->bitDepth = static_cast<int>(bitDepth);
    return true;
}

bool decodeSettingsSection(const QByteArray &payload, DecodedSettings *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint8 logScale = 0;
    quint8 showFps = 0;
    qint32 librarySortMode = 0;
    quint32 fftSize = 0;
    if (!reader.readF32(&out->volume)
        || !reader.readU32(&fftSize)
        || !reader.readF32(&out->dbRange)
        || !reader.readU8(&logScale)
        || !reader.readU8(&showFps)
        || !reader.readI32(&librarySortMode)
        || !reader.atEnd()) {
        return false;
    }
    out->present = true;
    out->fftSize = static_cast<int>(fftSize);
    out->logScale = logScale != 0;
    out->showFps = showFps != 0;
    out->librarySortMode = static_cast<int>(librarySortMode);
    return true;
}

} // namespace

QByteArray encodeCommandNoPayload(quint16 cmdId) {
    return finalizeCommand(cmdId, {});
}

QByteArray encodeCommandU8(quint16 cmdId, quint8 value) {
    QByteArray payload;
    payload.reserve(1);
    payload.append(static_cast<char>(value));
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandI32(quint16 cmdId, qint32 value) {
    QByteArray payload;
    payload.reserve(4);
    appendLe<qint32>(payload, value);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandU32(quint16 cmdId, quint32 value) {
    QByteArray payload;
    payload.reserve(4);
    appendLe<quint32>(payload, value);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandF32(quint16 cmdId, float value) {
    QByteArray payload;
    payload.reserve(4);
    quint32 bits = 0;
    static_assert(sizeof(value) == sizeof(bits), "float size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    appendLe<quint32>(payload, bits);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandF64(quint16 cmdId, double value) {
    QByteArray payload;
    payload.reserve(8);
    quint64 bits = 0;
    static_assert(sizeof(value) == sizeof(bits), "double size mismatch");
    std::memcpy(&bits, &value, sizeof(bits));
    appendLe<quint64>(payload, bits);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandString(quint16 cmdId, const QString &value) {
    QByteArray payload;
    appendUtf8U16(payload, value);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandStringPair(quint16 cmdId, const QString &first, const QString &second) {
    QByteArray payload;
    appendUtf8U16(payload, first);
    appendUtf8U16(payload, second);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandStringBool(quint16 cmdId, const QString &value, bool flag) {
    QByteArray payload;
    appendUtf8U16(payload, value);
    payload.append(flag ? '\x01' : '\x00');
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandSearchQuery(quint16 cmdId, quint32 seq, const QString &query) {
    QByteArray payload;
    payload.reserve(8 + query.size());
    appendLe<quint32>(payload, seq);
    appendUtf8U16(payload, query);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandStringList(quint16 cmdId, const QStringList &values) {
    QByteArray payload;
    const int count = std::min<int>(values.size(), std::numeric_limits<quint16>::max());
    appendLe<quint16>(payload, static_cast<quint16>(count));
    for (int i = 0; i < count; ++i) {
        appendUtf8U16(payload, values[i]);
    }
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandMoveQueue(quint32 from, quint32 to) {
    QByteArray payload;
    payload.reserve(8);
    appendLe<quint32>(payload, from);
    appendLe<quint32>(payload, to);
    return finalizeCommand(CmdMoveQueue, payload);
}

bool decodeSnapshotPacket(const QByteArray &packet, DecodedSnapshot *out, QString *errorMessage) {
    if (!out) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("output snapshot pointer is null");
        }
        return false;
    }
    *out = DecodedSnapshot{};

    if (packet.size() < kHeaderSize) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("packet too short");
        }
        return false;
    }

    Reader reader(packet);
    quint32 magic = 0;
    quint32 totalLength = 0;
    quint16 mask = 0;
    quint16 reserved = 0;
    if (!reader.readU32(&magic)
        || !reader.readU32(&totalLength)
        || !reader.readU16(&mask)
        || !reader.readU16(&reserved)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("failed to read packet header");
        }
        return false;
    }

    Q_UNUSED(reserved);
    if (magic != kSnapshotMagic) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("invalid packet magic: 0x%1").arg(magic, 8, 16, QChar('0'));
        }
        return false;
    }
    if (totalLength != static_cast<quint32>(packet.size())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("packet length mismatch: header=%1 actual=%2")
                                .arg(totalLength)
                                .arg(packet.size());
        }
        return false;
    }

    out->sectionMask = mask;

    for (int bit = 0; bit < 8; ++bit) {
        const quint16 sectionBit = static_cast<quint16>(1u << bit);
        if ((mask & sectionBit) == 0) {
            continue;
        }
        quint32 sectionLen = 0;
        if (!reader.readU32(&sectionLen)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("truncated section length");
            }
            return false;
        }
        QByteArray sectionPayload;
        if (!reader.readBytes(sectionLen, &sectionPayload)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("truncated section payload");
            }
            return false;
        }

        bool ok = true;
        switch (sectionBit) {
        case SectionPlayback:
            ok = decodePlaybackSection(sectionPayload, &out->playback);
            break;
        case SectionQueue:
            ok = decodeQueueSection(sectionPayload, &out->queue);
            break;
        case SectionLibraryMeta:
            ok = decodeLibraryMetaSection(sectionPayload, &out->library);
            break;
        case SectionReservedLibraryTree:
            ok = true;
            break;
        case SectionMetadata:
            ok = decodeMetadataSection(sectionPayload, &out->metadata);
            break;
        case SectionSettings:
            ok = decodeSettingsSection(sectionPayload, &out->settings);
            break;
        case SectionError: {
            Reader sectionReader(sectionPayload);
            QString message;
            ok = sectionReader.readUtf8U16(&message) && sectionReader.atEnd();
            if (ok) {
                out->errorMessage = message;
            }
            break;
        }
        case SectionStopped: {
            out->hasStopped = true;
            ok = sectionPayload.isEmpty();
            break;
        }
        default:
            ok = false;
            break;
        }

        if (!ok) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("failed to decode section bit %1").arg(bit);
            }
            return false;
        }
    }

    if (!reader.atEnd()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("packet has trailing bytes");
        }
        return false;
    }

    return true;
}

bool decodeSearchResultsFrame(
    const QByteArray &payload,
    DecodedSearchResults *out,
    QString *errorMessage) {
    if (!out) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("output search results pointer is null");
        }
        return false;
    }
    *out = DecodedSearchResults{};

    Reader reader(payload);
    quint8 magic = 0;
    quint8 version = 0;
    quint16 rowCount = 0;
    quint32 seq = 0;
    if (!reader.readU8(&magic)
        || !reader.readU8(&version)
        || !reader.readU16(&rowCount)
        || !reader.readU32(&seq)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("search frame header truncated");
        }
        return false;
    }
    if (magic != static_cast<quint8>('S')) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("invalid search frame magic");
        }
        return false;
    }
    if (version != 1) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unsupported search frame version: %1").arg(version);
        }
        return false;
    }

    QVector<DecodedSearchRow> rows;
    rows.reserve(static_cast<int>(rowCount));
    for (quint16 i = 0; i < rowCount; ++i) {
        quint8 rowType = 0;
        float score = 0.0f;
        qint32 year = std::numeric_limits<int>::min();
        quint16 trackNumber = 0;
        quint32 count = 0;
        float lengthSeconds = -1.0f;
        QString label;
        QString artist;
        QString album;
        QString genre;
        QString coverPath;
        QString artistKey;
        QString albumKey;
        QString sectionKey;
        QString trackKey;
        QString trackPath;

        if (!reader.readU8(&rowType)
            || !reader.readF32(&score)
            || !reader.readI32(&year)
            || !reader.readU16(&trackNumber)
            || !reader.readU32(&count)
            || !reader.readF32(&lengthSeconds)
            || !reader.readUtf8U16(&label)
            || !reader.readUtf8U16(&artist)
            || !reader.readUtf8U16(&album)
            || !reader.readUtf8U16(&genre)
            || !reader.readUtf8U16(&coverPath)
            || !reader.readUtf8U16(&artistKey)
            || !reader.readUtf8U16(&albumKey)
            || !reader.readUtf8U16(&sectionKey)
            || !reader.readUtf8U16(&trackKey)
            || !reader.readUtf8U16(&trackPath)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("search frame row %1 truncated").arg(i);
            }
            return false;
        }
        if (rowType < static_cast<quint8>(SearchRowArtist)
            || rowType > static_cast<quint8>(SearchRowTrack)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("invalid search row type at row %1").arg(i);
            }
            return false;
        }

        DecodedSearchRow row;
        row.rowType = static_cast<int>(rowType);
        row.score = score;
        row.year = year;
        row.trackNumber = static_cast<int>(trackNumber);
        row.count = count > static_cast<quint32>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(count);
        row.lengthSeconds = lengthSeconds;
        row.label = label;
        row.artist = artist;
        row.album = album;
        row.genre = genre;
        row.coverPath = coverPath;
        row.artistKey = artistKey;
        row.albumKey = albumKey;
        row.sectionKey = sectionKey;
        row.trackKey = trackKey;
        row.trackPath = trackPath;
        rows.push_back(std::move(row));
    }

    if (!reader.atEnd()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("search frame has trailing bytes");
        }
        return false;
    }

    out->seq = seq;
    out->rows = std::move(rows);
    return true;
}

} // namespace BinaryBridgeCodec
