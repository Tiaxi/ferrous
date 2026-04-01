// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <limits>

namespace BinaryBridgeCodec {

constexpr quint32 kSnapshotMagic = 0xFE550001u;

enum SnapshotSection : quint16 {
    SectionPlayback = 1u << 0,
    SectionQueue = 1u << 1,
    SectionLibraryMeta = 1u << 2,
    SectionReservedLibraryTree = 1u << 3,
    SectionMetadata = 1u << 4,
    SectionSettings = 1u << 5,
    SectionError = 1u << 6,
    SectionStopped = 1u << 7,
    SectionLastFm = 1u << 8,
};

enum CommandId : quint16 {
    CmdPlay = 1,
    CmdPause = 2,
    CmdStop = 3,
    CmdNext = 4,
    CmdPrevious = 5,
    CmdSetVolume = 6,
    CmdSeek = 7,
    CmdPlayAt = 8,
    CmdSelectQueue = 9,
    CmdRemoveAt = 10,
    CmdMoveQueue = 11,
    CmdClearQueue = 12,
    CmdAddTrack = 13,
    CmdPlayTrack = 14,
    CmdReplaceAlbum = 15,
    CmdAppendAlbum = 16,
    CmdReplaceAlbumByKey = 17,
    CmdAppendAlbumByKey = 18,
    CmdReplaceArtistByKey = 19,
    CmdAppendArtistByKey = 20,
    CmdAddRoot = 21,
    CmdRemoveRoot = 22,
    CmdRescanRoot = 23,
    CmdRescanAll = 24,
    CmdSetRepeatMode = 25,
    CmdSetShuffle = 26,
    CmdSetDbRange = 27,
    CmdSetLogScale = 28,
    CmdSetShowFps = 29,
    CmdSetLibrarySortMode = 30,
    CmdSetFftSize = 31,
    CmdSetSpectrogramViewMode = 32,
    CmdRequestSnapshot = 33,
    CmdShutdown = 34,
    CmdSetNodeExpanded = 35,
    CmdSetSearchQuery = 36,
    CmdReplaceAllTracks = 37,
    CmdAppendAllTracks = 38,
    CmdSetSystemMediaControls = 39,
    CmdSetLastFmScrobblingEnabled = 40,
    CmdBeginLastFmAuth = 41,
    CmdCompleteLastFmAuth = 42,
    CmdDisconnectLastFm = 43,
    CmdSetViewerFullscreenMode = 44,
    CmdRenameRoot = 45,
    CmdApplyAlbumArt = 46,
    CmdRefreshEditedPaths = 47,
    CmdReplaceRootByPath = 48,
    CmdAppendRootByPath = 49,
    CmdSetSpectrogramDisplayMode = 50,
    CmdSetSpectrogramCrosshair = 51,
    CmdSetSpectrogramScale = 52,
    CmdToggleChannelMute = 53,
    CmdSoloChannel = 54,
    CmdSetChannelButtonsVisibility = 55,
};

struct DecodedPlayback {
    bool present{false};
    int state{0};
    double positionSeconds{0.0};
    double durationSeconds{0.0};
    float volume{1.0f};
    int repeatMode{0};
    bool shuffleEnabled{false};
    int currentQueueIndex{-1};
    QString currentPath;
    quint64 mutedChannelsMask{0};
    int soloedChannel{-1};
};

struct DecodedQueueTrack {
    QString title;
    QString artist;
    QString album;
    QString coverPath;
    QString genre;
    int year{std::numeric_limits<int>::min()};
    int trackNumber{0};
    float lengthSeconds{-1.0f};
    QString path;
};

struct DecodedQueue {
    bool present{false};
    int len{0};
    int selectedIndex{-1};
    double totalDurationSeconds{0.0};
    int unknownDurationCount{0};
    QVector<DecodedQueueTrack> tracks;
};

struct DecodedLibraryRoot {
    QString path;
    QString name;
};

struct DecodedLibraryMeta {
    bool present{false};
    int rootCount{0};
    int trackCount{0};
    int artistCount{0};
    int albumCount{0};
    bool scanInProgress{false};
    int sortMode{0};
    QString lastError;
    int rootsCompleted{0};
    int rootsTotal{0};
    int filesDiscovered{0};
    int filesProcessed{0};
    double filesPerSecond{0.0};
    double etaSeconds{-1.0};
    QStringList rootPaths;
    QVector<DecodedLibraryRoot> rootEntries;
};

struct DecodedMetadata {
    bool present{false};
    QString sourcePath;
    QString title;
    QString artist;
    QString album;
    QString genre;
    int year{std::numeric_limits<int>::min()};
    int sampleRateHz{0};
    int bitrateKbps{0};
    int channels{0};
    int bitDepth{0};
    QString formatLabel;
    int currentBitrateKbps{0};
    QString coverPath;
    int trackNumber{0};
};

struct DecodedSettings {
    bool present{false};
    float volume{1.0f};
    int fftSize{8192};
    int spectrogramViewMode{0};
    int spectrogramDisplayMode{0};
    int viewerFullscreenMode{0};
    float dbRange{132.0f};
    bool logScale{false};
    bool showFps{false};
    int librarySortMode{0};
    bool systemMediaControlsEnabled{true};
    bool showSpectrogramCrosshair{false};
    bool showSpectrogramScale{false};
    int channelButtonsVisibility{1};
};

struct DecodedLastFm {
    bool present{false};
    bool enabled{false};
    bool buildConfigured{false};
    int authState{0};
    int pendingScrobbleCount{0};
    QString username;
    QString statusText;
    QString authUrl;
};

struct DecodedSnapshot {
    quint16 sectionMask{0};
    bool hasStopped{false};
    QString errorMessage;
    DecodedPlayback playback;
    DecodedQueue queue;
    DecodedLibraryMeta library;
    DecodedMetadata metadata;
    DecodedSettings settings;
    DecodedLastFm lastfm;
};

enum SearchRowType : quint8 {
    SearchRowArtist = 1,
    SearchRowAlbum = 2,
    SearchRowTrack = 3,
};

struct DecodedSearchRow {
    int rowType{0};
    float score{0.0f};
    int year{std::numeric_limits<int>::min()};
    int trackNumber{0};
    int count{0};
    float lengthSeconds{-1.0f};
    QString label;
    QString artist;
    QString album;
    QString rootLabel;
    QString genre;
    QString coverPath;
    QString artistKey;
    QString albumKey;
    QString sectionKey;
    QString trackKey;
    QString trackPath;
};

struct DecodedSearchResults {
    quint32 seq{0};
    QVector<DecodedSearchRow> rows;
};

QByteArray encodeCommandNoPayload(quint16 cmdId);
QByteArray encodeCommandU8(quint16 cmdId, quint8 value);
QByteArray encodeCommandI32(quint16 cmdId, qint32 value);
QByteArray encodeCommandU32(quint16 cmdId, quint32 value);
QByteArray encodeCommandF32(quint16 cmdId, float value);
QByteArray encodeCommandF64(quint16 cmdId, double value);
QByteArray encodeCommandString(quint16 cmdId, const QString &value);
QByteArray encodeCommandStringPair(quint16 cmdId, const QString &first, const QString &second);
QByteArray encodeCommandStringBool(quint16 cmdId, const QString &value, bool flag);
QByteArray encodeCommandSearchQuery(quint16 cmdId, quint32 seq, const QString &query);
QByteArray encodeCommandStringList(quint16 cmdId, const QStringList &values);
QByteArray encodeCommandMoveQueue(quint32 from, quint32 to);

bool decodeSnapshotPacket(const QByteArray &packet, DecodedSnapshot *out, QString *errorMessage);
bool decodeSearchResultsFrame(
    const QByteArray &payload,
    DecodedSearchResults *out,
    QString *errorMessage);

} // namespace BinaryBridgeCodec
