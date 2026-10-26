// SPDX-License-Identifier: GPL-3.0-or-later

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

    qsizetype remaining() const {
        return m_bytes.size() - m_offset;
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

    bool readU64(quint64 *out) {
        if (!out || m_offset + 8 > m_bytes.size()) {
            return false;
        }
        *out = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar *>(m_bytes.constData() + m_offset));
        m_offset += 8;
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

    bool readUtf8(QString *out) {
        if (!out) {
            return false;
        }
        quint32 len = 0;
        if (!readU32(&len)) {
            return false;
        }
        if (static_cast<quint64>(len) > static_cast<quint64>(m_bytes.size() - m_offset)) {
            return false;
        }
        *out = QString::fromUtf8(m_bytes.constData() + m_offset, static_cast<qsizetype>(len));
        m_offset += static_cast<qsizetype>(len);
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

// Appends a u32 length and the UTF-8 bytes. A string that cannot fit in a
// command is refused whole, so no command ever carries a cut string.
bool appendUtf8(QByteArray &out, const QString &text) {
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() > static_cast<qsizetype>(kMaxCommandPayloadBytes)) {
        return false;
    }
    appendLe<quint32>(out, static_cast<quint32>(utf8.size()));
    out.append(utf8);
    return true;
}

// Frames a command as a u16 id, a u32 payload length, and the payload.
// Returns an empty array, never a cut command, for a payload over the bound.
QByteArray finalizeCommand(quint16 cmdId, const QByteArray &payload) {
    if (payload.size() > static_cast<qsizetype>(kMaxCommandPayloadBytes)) {
        return {};
    }
    QByteArray out;
    out.reserve(kCommandHeaderBytes + payload.size());
    appendLe<quint16>(out, cmdId);
    appendLe<quint32>(out, static_cast<quint32>(payload.size()));
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
    quint64 mutedMask = 0;
    if (!reader.readU8(&state)
        || !reader.readF64(&out->positionSeconds)
        || !reader.readF64(&out->durationSeconds)
        || !reader.readF32(&out->volume)
        || !reader.readU8(&repeatMode)
        || !reader.readU8(&shuffleEnabled)
        || !reader.readI32(&currentQueueIndex)
        || !reader.readUtf8(&currentPath)
        || !reader.readU64(&mutedMask)) {
        return false;
    }
    quint8 soloedRaw = 0xFF;
    if (!reader.atEnd() && !reader.readU8(&soloedRaw)) {
        return false;
    }
    quint64 trackToken = 0;
    if (!reader.atEnd() && !reader.readU64(&trackToken)) {
        return false;
    }
    quint64 seekSerial = 0;
    quint64 seekLandedSerial = 0;
    if (!reader.atEnd()
        && (!reader.readU64(&seekSerial) || !reader.readU64(&seekLandedSerial))) {
        return false;
    }
    quint64 sampledAtNs = 0;
    quint8 advancing = 0;
    if (!reader.atEnd()
        && (!reader.readU64(&sampledAtNs) || !reader.readU8(&advancing) || advancing > 1)) {
        return false;
    }
    if (!reader.atEnd()) {
        return false;
    }
    out->present = true;
    out->state = static_cast<int>(state);
    out->repeatMode = static_cast<int>(repeatMode);
    out->shuffleEnabled = shuffleEnabled != 0;
    out->currentQueueIndex = static_cast<int>(currentQueueIndex);
    out->currentPath = currentPath;
    out->mutedChannelsMask = mutedMask;
    out->soloedChannel = (soloedRaw == 0xFF) ? -1 : static_cast<int>(soloedRaw);
    out->trackToken = trackToken;
    out->seekSerial = seekSerial;
    out->seekLandedSerial = seekLandedSerial;
    out->positionSampledAtNs = static_cast<qint64>(
        std::min<quint64>(sampledAtNs, std::numeric_limits<qint64>::max()));
    out->positionAdvancing = advancing != 0;
    return true;
}

bool readQueueTrack(Reader &reader, DecodedQueueTrack *out) {
    qint32 year = std::numeric_limits<int>::min();
    quint16 trackNumber = 0;
    if (!reader.readUtf8(&out->title)
        || !reader.readUtf8(&out->artist)
        || !reader.readUtf8(&out->album)
        || !reader.readUtf8(&out->coverPath)
        || !reader.readU64(&out->coverStamp)
        || !reader.readUtf8(&out->genre)
        || !reader.readI32(&year)
        || !reader.readU16(&trackNumber)
        || !reader.readF32(&out->lengthSeconds)
        || !reader.readUtf8(&out->path)) {
        return false;
    }
    out->year = year;
    out->trackNumber = static_cast<int>(trackNumber);
    return true;
}

// Read `count` rows, refusing a count the rest of the section cannot hold
// before anything is allocated for it.
bool readQueueTracks(Reader &reader, quint32 count, QVector<DecodedQueueTrack> *out) {
    // A row takes at least six string lengths and its fixed fields.
    constexpr qsizetype kMinRowBytes = 6 * 4 + 4 + 2 + 4;
    if (static_cast<quint64>(count) * kMinRowBytes > static_cast<quint64>(reader.remaining())) {
        return false;
    }
    out->reserve(static_cast<qsizetype>(count));
    for (quint32 i = 0; i < count; ++i) {
        DecodedQueueTrack track;
        if (!readQueueTrack(reader, &track)) {
            return false;
        }
        out->push_back(std::move(track));
    }
    return true;
}

bool readQueueOp(Reader &reader, DecodedQueueOp *out) {
    quint8 type = 0;
    quint32 index = 0;
    if (!reader.readU8(&type) || !reader.readU32(&index)
        || index > static_cast<quint32>(std::numeric_limits<int>::max())) {
        return false;
    }
    out->type = type;
    out->index = static_cast<int>(index);
    quint32 second = 0;
    switch (type) {
    case QueueOpInsert:
        return reader.readU32(&second) && readQueueTracks(reader, second, &out->tracks);
    case QueueOpRemove:
    case QueueOpMove:
        if (!reader.readU32(&second)
            || second > static_cast<quint32>(std::numeric_limits<int>::max())) {
            return false;
        }
        if (type == QueueOpRemove) {
            out->count = static_cast<int>(second);
        } else {
            out->to = static_cast<int>(second);
        }
        return true;
    case QueueOpUpdate:
        return readQueueTracks(reader, 1, &out->tracks);
    case QueueOpMoveRows: {
        quint32 count = 0;
        quint32 to = 0;
        if (!reader.readU32(&count) || !reader.readU32(&to)
            || count > static_cast<quint32>(std::numeric_limits<int>::max())
            || to > static_cast<quint32>(std::numeric_limits<int>::max())) {
            return false;
        }
        out->count = static_cast<int>(count);
        out->to = static_cast<int>(to);
        return true;
    }
    default:
        return false;
    }
}

bool decodeQueueSection(const QByteArray &payload, DecodedQueue *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint8 kind = 0;
    quint64 revision = 0;
    quint32 len = 0;
    qint32 selectedIndex = -1;
    quint32 unknownDurationCount = 0;
    if (!reader.readU8(&kind)
        || !reader.readU64(&revision)
        || !reader.readU32(&len)
        || !reader.readI32(&selectedIndex)
        || !reader.readF64(&out->totalDurationSeconds)
        || !reader.readU32(&unknownDurationCount)
        || len > static_cast<quint32>(std::numeric_limits<int>::max())) {
        return false;
    }

    QVector<DecodedQueueTrack> tracks;
    QVector<DecodedQueueOp> ops;
    quint64 baseRevision = 0;
    switch (kind) {
    case QueueFull:
        if (!readQueueTracks(reader, len, &tracks)) {
            return false;
        }
        break;
    case QueueDelta: {
        quint32 opCount = 0;
        // An operation takes at least nine bytes.
        if (!reader.readU64(&baseRevision) || !reader.readU32(&opCount)
            || static_cast<quint64>(opCount) * 9 > static_cast<quint64>(reader.remaining())) {
            return false;
        }
        ops.reserve(static_cast<qsizetype>(opCount));
        for (quint32 i = 0; i < opCount; ++i) {
            DecodedQueueOp op;
            if (!readQueueOp(reader, &op)) {
                return false;
            }
            ops.push_back(std::move(op));
        }
        break;
    }
    case QueueResync:
        break;
    default:
        return false;
    }
    if (!reader.atEnd()) {
        return false;
    }

    out->present = true;
    out->kind = kind;
    out->revision = revision;
    out->baseRevision = baseRevision;
    out->len = static_cast<int>(len);
    out->selectedIndex = static_cast<int>(selectedIndex);
    out->unknownDurationCount = static_cast<int>(unknownDurationCount);
    out->tracks = std::move(tracks);
    out->ops = std::move(ops);
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
        || !reader.readUtf8(&lastError)
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
    QVector<DecodedLibraryRoot> rootEntries;
    rootEntries.reserve(rootPathCount);
    for (quint16 i = 0; i < rootPathCount; ++i) {
        DecodedLibraryRoot root;
        quint32 rootTrackCount = 0;
        if (!reader.readUtf8(&root.path) || !reader.readUtf8(&root.name)
            || !reader.readUtf8(&root.unwatchedReason) || !reader.readUtf8(&root.scanProblem)
            || !reader.readU32(&rootTrackCount)) {
            return false;
        }
        root.trackCount = static_cast<int>(
            std::min<quint32>(rootTrackCount, std::numeric_limits<int>::max()));
        rootPaths.push_back(root.path);
        rootEntries.push_back(std::move(root));
    }
    QStringList expandedKeys;
    QString viewSelectionKey;
    QString viewAnchorKey;
    float viewAnchorOffset = 0.0f;
    quint32 revealGeneration = 0;
    bool viewStatePresent = false;
    if (!reader.atEnd()) {
        viewStatePresent = true;
        quint16 expandedKeyCount = 0;
        if (!reader.readU16(&expandedKeyCount)) {
            return false;
        }
        expandedKeys.reserve(expandedKeyCount);
        for (quint16 i = 0; i < expandedKeyCount; ++i) {
            QString key;
            if (!reader.readUtf8(&key)) {
                return false;
            }
            expandedKeys.push_back(key);
        }
        if (!reader.readUtf8(&viewSelectionKey)
            || !reader.readUtf8(&viewAnchorKey)
            || !reader.readF32(&viewAnchorOffset)
            || !reader.readU32(&revealGeneration)) {
            return false;
        }
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
    out->rootEntries = rootEntries;
    out->viewStatePresent = viewStatePresent;
    out->expandedKeys = expandedKeys;
    out->viewSelectionKey = viewSelectionKey;
    out->viewAnchorKey = viewAnchorKey;
    out->viewAnchorOffset = static_cast<double>(viewAnchorOffset);
    out->revealGeneration = revealGeneration;
    return true;
}

bool decodeMetadataSection(const QByteArray &payload, DecodedMetadata *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint32 sampleRateHz = 0;
    quint32 bitrateKbps = 0;
    quint32 currentBitrateKbps = 0;
    quint16 channels = 0;
    quint16 bitDepth = 0;
    quint32 trackNumber = 0;
    if (!reader.readUtf8(&out->sourcePath)
        || !reader.readUtf8(&out->title)
        || !reader.readUtf8(&out->artist)
        || !reader.readUtf8(&out->album)
        || !reader.readUtf8(&out->albumArtist)
        || !reader.readUtf8(&out->genre)
        || !reader.readI32(&out->year)
        || !reader.readU32(&sampleRateHz)
        || !reader.readU32(&bitrateKbps)
        || !reader.readU16(&channels)
        || !reader.readU16(&bitDepth)
        || !reader.readUtf8(&out->formatLabel)
        || !reader.readU32(&currentBitrateKbps)
        || !reader.readUtf8(&out->coverPath)
        || !reader.readU64(&out->coverStamp)
        || !reader.readU32(&trackNumber)) {
        return false;
    }
    out->channelLabels.clear();
    if (!reader.atEnd()) {
        quint16 count = 0;
        if (!reader.readU16(&count) || count > 64 || (count != 0 && count != channels)) return false;
        for (quint16 i = 0; i < count; ++i) {
            QString label;
            if (!reader.readUtf8(&label)) return false;
            out->channelLabels.append(label);
        }
        if (!reader.atEnd()) return false;
    }
    out->present = true;
    out->sampleRateHz = static_cast<int>(sampleRateHz);
    out->bitrateKbps = static_cast<int>(bitrateKbps);
    out->channels = static_cast<int>(channels);
    out->bitDepth = static_cast<int>(bitDepth);
    out->currentBitrateKbps = static_cast<int>(currentBitrateKbps);
    out->trackNumber = static_cast<int>(trackNumber);
    return true;
}

bool decodeSettingsSection(const QByteArray &payload, DecodedSettings *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint8 spectrogramViewMode = 0;
    quint8 viewerFullscreenMode = 0;
    quint8 logScale = 0;
    quint8 showFps = 0;
    quint8 systemMediaControlsEnabled = 1;
    qint32 librarySortMode = 0;
    quint32 fftSize = 0;
    if (!reader.readF32(&out->volume)
        || !reader.readU32(&fftSize)
        || !reader.readU8(&spectrogramViewMode)
        || !reader.readF32(&out->dbRange)
        || !reader.readU8(&logScale)
        || !reader.readU8(&showFps)
        || !reader.readI32(&librarySortMode)) {
        return false;
    }
    if (!reader.atEnd() && !reader.readU8(&systemMediaControlsEnabled)) {
        return false;
    }
    if (!reader.atEnd() && !reader.readU8(&viewerFullscreenMode)) {
        return false;
    }
    quint8 spectrogramDisplayMode = 0;
    if (!reader.atEnd() && !reader.readU8(&spectrogramDisplayMode)) {
        return false;
    }
    quint8 showSpectrogramCrosshair = 0;
    if (!reader.atEnd() && !reader.readU8(&showSpectrogramCrosshair)) {
        return false;
    }
    quint8 showSpectrogramScale = 0;
    if (!reader.atEnd() && !reader.readU8(&showSpectrogramScale)) {
        return false;
    }
    quint8 channelButtonsVisibility = 1;
    if (!reader.atEnd() && !reader.readU8(&channelButtonsVisibility)) {
        return false;
    }
    quint8 spectrogramZoomEnabled = 1;
    if (!reader.atEnd() && !reader.readU8(&spectrogramZoomEnabled)) {
        return false;
    }
    quint8 preventDisplaySleepInFullscreen = 1;
    if (!reader.atEnd() && !reader.readU8(&preventDisplaySleepInFullscreen)) {
        return false;
    }
    quint8 showLevelMeter = 1;
    if (!reader.atEnd() && !reader.readU8(&showLevelMeter)) {
        return false;
    }
    quint8 libraryWatchEnabled = 1;
    if (!reader.atEnd() && !reader.readU8(&libraryWatchEnabled)) {
        return false;
    }
    if (!reader.atEnd()) {
        return false;
    }
    out->present = true;
    out->fftSize = static_cast<int>(fftSize);
    out->spectrogramViewMode = static_cast<int>(spectrogramViewMode);
    out->spectrogramDisplayMode = static_cast<int>(spectrogramDisplayMode);
    out->viewerFullscreenMode = static_cast<int>(viewerFullscreenMode);
    out->logScale = logScale != 0;
    out->showFps = showFps != 0;
    out->showLevelMeter = showLevelMeter != 0;
    out->librarySortMode = static_cast<int>(librarySortMode);
    out->systemMediaControlsEnabled = systemMediaControlsEnabled != 0;
    out->showSpectrogramCrosshair = showSpectrogramCrosshair != 0;
    out->showSpectrogramScale = showSpectrogramScale != 0;
    out->channelButtonsVisibility = std::clamp(static_cast<int>(channelButtonsVisibility), 0, 2);
    out->spectrogramZoomEnabled = spectrogramZoomEnabled != 0;
    out->preventDisplaySleepInFullscreen = preventDisplaySleepInFullscreen != 0;
    out->libraryWatchEnabled = libraryWatchEnabled != 0;
    return true;
}

bool decodeLastFmSection(const QByteArray &payload, DecodedLastFm *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint8 enabled = 0;
    quint8 buildConfigured = 0;
    quint8 authState = 0;
    quint32 pendingScrobbleCount = 0;
    if (!reader.readU8(&enabled)
        || !reader.readU8(&buildConfigured)
        || !reader.readU8(&authState)
        || !reader.readU32(&pendingScrobbleCount)
        || !reader.readUtf8(&out->username)
        || !reader.readUtf8(&out->statusText)
        || !reader.readUtf8(&out->authUrl)
        || !reader.atEnd()) {
        return false;
    }
    out->present = true;
    out->enabled = enabled != 0;
    out->buildConfigured = buildConfigured != 0;
    out->authState = static_cast<int>(authState);
    out->pendingScrobbleCount = static_cast<int>(pendingScrobbleCount);
    return true;
}

bool decodeQueueUndoSection(const QByteArray &payload, DecodedQueueUndo *out) {
    if (!out) {
        return false;
    }
    Reader reader(payload);
    quint8 kind = 0;
    quint32 trackCount = 0;
    quint32 rangeCount = 0;
    if (!reader.readU64(&out->offerId)
        || !reader.readU8(&kind)
        || kind > QueueUndoReplaced
        || !reader.readU32(&trackCount)
        || !reader.readU64(&out->restoredId)
        || !reader.readU64(&out->restoredRevision)
        || !reader.readU32(&rangeCount)
        || rangeCount > static_cast<quint32>(payload.size() / 8)) {
        return false;
    }
    out->restoredRanges.reserve(static_cast<qsizetype>(rangeCount));
    for (quint32 i = 0; i < rangeCount; ++i) {
        quint32 start = 0;
        quint32 count = 0;
        if (!reader.readU32(&start) || !reader.readU32(&count)
            || start > static_cast<quint32>(std::numeric_limits<int>::max())
            || count > static_cast<quint32>(std::numeric_limits<int>::max())) {
            return false;
        }
        out->restoredRanges.push_back({static_cast<int>(start), static_cast<int>(count)});
    }
    if (!reader.atEnd()) {
        return false;
    }
    out->present = true;
    out->kind = kind;
    out->trackCount = static_cast<int>(
        std::min<quint32>(trackCount, std::numeric_limits<int>::max()));
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

QByteArray encodeCommandU64(quint16 cmdId, quint64 value) {
    QByteArray payload;
    payload.reserve(8);
    appendLe<quint64>(payload, value);
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
    if (!appendUtf8(payload, value)) {
        return {};
    }
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandStringPair(quint16 cmdId, const QString &first, const QString &second) {
    QByteArray payload;
    if (!appendUtf8(payload, first) || !appendUtf8(payload, second)) {
        return {};
    }
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandApplyAlbumArt(const QString &trackPath, const QString &artworkPath,
                                      quint64 trackSize, qint64 trackModifiedMs) {
    QByteArray payload;
    if (!appendUtf8(payload, trackPath) || !appendUtf8(payload, artworkPath)) {
        return {};
    }
    appendLe<quint64>(payload, trackSize);
    appendLe<qint64>(payload, trackModifiedMs);
    return finalizeCommand(CmdApplyAlbumArt, payload);
}

QByteArray encodeCommandStringBool(quint16 cmdId, const QString &value, bool flag) {
    QByteArray payload;
    if (!appendUtf8(payload, value)) {
        return {};
    }
    payload.append(flag ? '\x01' : '\x00');
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandLibraryViewState(
    quint16 cmdId,
    const QString &selectionKey,
    const QString &anchorKey,
    float anchorOffset) {
    QByteArray payload;
    if (!appendUtf8(payload, selectionKey) || !appendUtf8(payload, anchorKey)) {
        return {};
    }
    quint32 bits = 0;
    static_assert(sizeof(anchorOffset) == sizeof(bits), "float size mismatch");
    std::memcpy(&bits, &anchorOffset, sizeof(bits));
    appendLe<quint32>(payload, bits);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandWindowLayout(const WindowLayout &layout) {
    QByteArray payload;
    payload.reserve(32);
    const auto appendF32 = [&payload](float value) {
        quint32 bits = 0;
        static_assert(sizeof(value) == sizeof(bits), "float size mismatch");
        std::memcpy(&bits, &value, sizeof(bits));
        appendLe<quint32>(payload, bits);
    };
    appendLe<quint8>(payload, layout.hasGeometry ? 1 : 0);
    appendLe<qint32>(payload, layout.x);
    appendLe<qint32>(payload, layout.y);
    appendLe<quint32>(payload, layout.width);
    appendLe<quint32>(payload, layout.height);
    appendLe<quint8>(payload, layout.maximized ? 1 : 0);
    appendF32(layout.sidebarWidthRatio);
    appendF32(layout.queueHeightRatio);
    appendLe<quint8>(payload, layout.visualizationMode);
    appendLe<quint8>(payload, layout.followCurrentTrack ? 1 : 0);
    return finalizeCommand(CmdSetWindowLayout, payload);
}

QByteArray encodeCommandSearchQuery(quint16 cmdId, quint32 seq, const QString &query, quint32 limit) {
    QByteArray payload;
    payload.reserve(12 + query.size());
    appendLe<quint32>(payload, seq);
    if (!appendUtf8(payload, query)) {
        return {};
    }
    appendLe<quint32>(payload, limit);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandStringList(quint16 cmdId, const QStringList &values) {
    if (values.size() > static_cast<qsizetype>(kMaxCommandPayloadBytes / 4)) {
        return {};
    }
    QByteArray payload;
    appendLe<quint32>(payload, static_cast<quint32>(values.size()));
    for (const QString &value : values) {
        // Stop at the bound rather than build a list that cannot be sent.
        if (!appendUtf8(payload, value)
            || payload.size() > static_cast<qsizetype>(kMaxCommandPayloadBytes)) {
            return {};
        }
    }
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandQueueRow(quint16 cmdId, quint64 revision, quint32 index) {
    QByteArray payload;
    payload.reserve(12);
    appendLe<quint64>(payload, revision);
    appendLe<quint32>(payload, index);
    return finalizeCommand(cmdId, payload);
}

QByteArray encodeCommandSelectQueue(quint64 revision, qint32 index) {
    QByteArray payload;
    payload.reserve(12);
    appendLe<quint64>(payload, revision);
    appendLe<qint32>(payload, index);
    return finalizeCommand(CmdSelectQueue, payload);
}

QByteArray encodeCommandMoveQueue(quint64 revision, quint32 from, quint32 to) {
    QByteArray payload;
    payload.reserve(16);
    appendLe<quint64>(payload, revision);
    appendLe<quint32>(payload, from);
    appendLe<quint32>(payload, to);
    return finalizeCommand(CmdMoveQueue, payload);
}

QByteArray encodeCommandRemoveMany(quint64 revision, const QList<quint32> &indices) {
    if (indices.size() > static_cast<qsizetype>((kMaxCommandPayloadBytes - 12) / 4)) {
        return {};
    }
    QByteArray payload;
    payload.reserve(12 + 4 * indices.size());
    appendLe<quint64>(payload, revision);
    appendLe<quint32>(payload, static_cast<quint32>(indices.size()));
    for (quint32 index : indices) appendLe<quint32>(payload, index);
    return finalizeCommand(CmdRemoveMany, payload);
}

QByteArray encodeCommandMoveQueueRows(
    quint64 revision, QueueRowsMove how, quint32 before, const QList<quint32> &rows) {
    if (rows.size() > static_cast<qsizetype>((kMaxCommandPayloadBytes - 17) / 4)) {
        return {};
    }
    QByteArray payload;
    payload.reserve(17 + 4 * rows.size());
    appendLe<quint64>(payload, revision);
    payload.append(static_cast<char>(how));
    appendLe<quint32>(payload, before);
    appendLe<quint32>(payload, static_cast<quint32>(rows.size()));
    for (quint32 row : rows) appendLe<quint32>(payload, row);
    return finalizeCommand(CmdMoveQueueRows, payload);
}

QByteArray encodeCommandQueueLibraryItems(
    QueuePlacement placement, quint64 revision, quint32 before, const LibraryQueueItems &items) {
    QByteArray payload;
    payload.append(static_cast<char>(placement));
    appendLe<quint64>(payload, revision);
    appendLe<quint32>(payload, before);
    appendLe<quint32>(payload, static_cast<quint32>(items.size()));
    for (const LibraryQueueItem &item : items) {
        payload.append(static_cast<char>(item.kind));
        bool appended = true;
        if (item.kind == LibraryQueueItem::Paths) {
            appendLe<quint32>(payload, static_cast<quint32>(item.paths.size()));
            for (const QString &path : item.paths) {
                if (!appendUtf8(payload, path)
                    || payload.size() > static_cast<qsizetype>(kMaxCommandPayloadBytes)) {
                    appended = false;
                    break;
                }
            }
        } else {
            appended = appendUtf8(payload, item.value);
        }
        // Stop at the bound rather than build a list that cannot be sent.
        if (!appended || payload.size() > static_cast<qsizetype>(kMaxCommandPayloadBytes)) {
            return {};
        }
    }
    return finalizeCommand(CmdQueueLibraryItems, payload);
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

    for (int bit = 0; bit < 16; ++bit) {
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
        case SectionLastFm:
            ok = decodeLastFmSection(sectionPayload, &out->lastfm);
            break;
        case SectionQueueUndo:
            ok = decodeQueueUndoSection(sectionPayload, &out->queueUndo);
            break;
        case SectionError: {
            Reader sectionReader(sectionPayload);
            quint8 category = 0;
            quint8 severity = 0;
            QString subject;
            QString message;
            ok = sectionReader.readU8(&category) && category <= ErrorCategoryLast
                && sectionReader.readU8(&severity) && severity <= ErrorSeverityError
                && sectionReader.readUtf8(&subject) && sectionReader.readUtf8(&message)
                && sectionReader.atEnd();
            if (ok) {
                out->errorCategory = category;
                out->errorSeverity = severity;
                out->errorSubject = subject;
                out->errorMessage = message;
            }
            break;
        }
        case SectionStopped: {
            out->hasStopped = true;
            ok = sectionPayload.isEmpty();
            break;
        }
        case SectionStateSaved: {
            Reader sectionReader(sectionPayload);
            quint32 requestId = 0;
            quint8 saved = 0;
            QString message;
            ok = sectionReader.readU32(&requestId) && sectionReader.readU8(&saved) && saved <= 1
                && sectionReader.readUtf8(&message) && sectionReader.atEnd();
            if (ok) {
                out->hasStateSaved = true;
                out->stateSaveRequestId = requestId;
                out->stateSaved = saved == 1;
                out->stateSaveError = message;
            }
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
    if (version < 2 || version > 5) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("unsupported search frame version: %1").arg(version);
        }
        return false;
    }

    if (version >= 3) {
        for (auto &total : out->totals) {
            if (!reader.readU32(&total)) {
                if (errorMessage) *errorMessage = QStringLiteral("search totals truncated");
                return false;
            }
        }
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
        QString rootLabel;
        QString genre;
        QString coverPath;
        quint64 coverStamp = 0;
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
            || !reader.readUtf8(&label)
            || !reader.readUtf8(&artist)
            || !reader.readUtf8(&album)
            || !reader.readUtf8(&rootLabel)
            || !reader.readUtf8(&genre)
            || !reader.readUtf8(&coverPath)
            || (version >= 5 && !reader.readU64(&coverStamp))
            || !reader.readUtf8(&artistKey)
            || !reader.readUtf8(&albumKey)
            || !reader.readUtf8(&sectionKey)
            || !reader.readUtf8(&trackKey)
            || !reader.readUtf8(&trackPath)) {
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
        if (version >= 4 && !reader.readUtf8(&row.matchDetail)) {
            if (errorMessage) *errorMessage = QStringLiteral("search match detail truncated");
            return false;
        }
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
        row.rootLabel = rootLabel;
        row.genre = genre;
        row.coverPath = coverPath;
        row.coverStamp = coverStamp;
        row.artistKey = artistKey;
        row.albumKey = albumKey;
        row.sectionKey = sectionKey;
        row.trackKey = trackKey;
        row.trackPath = trackPath;
        if (version == 2 && rowType >= 1 && rowType <= 3) ++out->totals[rowType - 1];
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
