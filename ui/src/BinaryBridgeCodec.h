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

// A command is a u16 id, a u32 payload length, and the payload. Strings in
// it, and in every frame from Rust, are a u32 byte length and UTF-8; lists
// of strings or indices start with a u32 count. The payload bound matches
// MAX_COMMAND_PAYLOAD_BYTES in src/frontend_bridge/ffi.rs. An encoder
// returns an empty array for a command over it.
constexpr qsizetype kCommandHeaderBytes = 6;
constexpr quint32 kMaxCommandPayloadBytes = 8u * 1024u * 1024u;

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
    // The answer to CmdSaveState: the u32 request ID that CmdSaveState
    // carried, a u8, 1 if the save succeeded, and a string with what to show
    // the user otherwise.
    SectionStateSaved = 1u << 9,
};

// Queue commands that name rows (CmdPlayAt, CmdSelectQueue, CmdRemoveAt,
// CmdMoveQueue, CmdRemoveMany) start with the u64 queue revision the rows
// showed, so the backend can carry them across edits made since.
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
    CmdSetSpectrogramZoomEnabled = 56,
    CmdSetSpectrogramZoomLevel = 57,
    CmdSetSpectrogramWidgetWidth = 58,
    CmdSetPreventDisplaySleepInFullscreen = 59,
    CmdSetLibraryViewState = 60,
    CmdShowTrackInLibrary = 61,
    // 62 was CmdRefreshRenamedPaths; renames now reach the bridge through its rename gate.
    CmdRemoveMany = 63,
    CmdSetSpectrogramActive = 64,
    CmdSetShowLevelMeter = 65,
    // Payload: a u32 request ID, echoed in SectionStateSaved.
    CmdSaveState = 66,
    CmdSetLibraryWatchEnabled = 67,
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
    // Changes on every track change, also when the same file plays again.
    quint64 trackToken{0};
    // Serial of the last seek the engine is done with, and of the last one
    // it applied; the engine numbers seeks from 1 in the order it gets them.
    // The first snapshot with a new seekLandedSerial carries the landing
    // position.
    quint64 seekSerial{0};
    quint64 seekLandedSerial{0};
    // positionSeconds is where the pipeline was at this CLOCK_MONOTONIC time,
    // in nanoseconds; it advances from there in real time while
    // positionAdvancing is set. See PlaybackClock.
    qint64 positionSampledAtNs{0};
    bool positionAdvancing{false};
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

// The queue section: the whole queue, the row operations since an earlier
// revision, or word that the backend lost track of the queue and the client
// should ask for it whole. The layout is described in
// src/frontend_bridge/queue_relay.rs.
enum QueueSectionKind : quint8 {
    QueueFull = 0,
    QueueDelta = 1,
    QueueResync = 2,
};

enum QueueOpType : quint8 {
    // Insert `tracks` before row `index`.
    QueueOpInsert = 1,
    // Remove `count` rows from `index`.
    QueueOpRemove = 2,
    // Take row `index` out and put it back at `to`.
    QueueOpMove = 3,
    // Row `index` is now `tracks[0]`.
    QueueOpUpdate = 4,
};

struct DecodedQueueOp {
    int type{0};
    int index{0};
    int count{0};
    int to{0};
    QVector<DecodedQueueTrack> tracks;
};

struct DecodedQueue {
    bool present{false};
    int kind{QueueFull};
    // The revision of the rows once the section is applied, and for a
    // delta, the revision it applies to.
    quint64 revision{0};
    quint64 baseRevision{0};
    int len{0};
    int selectedIndex{-1};
    double totalDurationSeconds{0.0};
    int unknownDurationCount{0};
    // The rows of a whole queue.
    QVector<DecodedQueueTrack> tracks;
    // The operations of a delta, in order.
    QVector<DecodedQueueOp> ops;
};

struct DecodedLibraryRoot {
    QString path;
    QString name;
    // Why the library does not watch the root for changes; empty if it does.
    QString unwatchedReason;
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
    bool viewStatePresent{false};
    QStringList expandedKeys;
    QString viewSelectionKey;
    QString viewAnchorKey;
    double viewAnchorOffset{0.0};
    quint32 revealGeneration{0};
};

struct DecodedMetadata {
    bool present{false};
    QString sourcePath;
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString genre;
    int year{std::numeric_limits<int>::min()};
    int sampleRateHz{0};
    int bitrateKbps{0};
    int channels{0};
    QStringList channelLabels;
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
    bool showLevelMeter{true};
    int librarySortMode{0};
    bool systemMediaControlsEnabled{true};
    bool preventDisplaySleepInFullscreen{true};
    bool showSpectrogramCrosshair{false};
    bool showSpectrogramScale{false};
    int channelButtonsVisibility{1};
    bool spectrogramZoomEnabled{true};
    bool libraryWatchEnabled{true};
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
    bool hasStateSaved{false};
    quint32 stateSaveRequestId{0};
    bool stateSaved{false};
    QString stateSaveError;
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
    QString matchDetail;
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
    quint32 totals[3]{0, 0, 0};
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
QByteArray encodeCommandLibraryViewState(
    quint16 cmdId,
    const QString &selectionKey,
    const QString &anchorKey,
    float anchorOffset);
QByteArray encodeCommandSearchQuery(quint16 cmdId, quint32 seq, const QString &query, quint32 limit = 0);
QByteArray encodeCommandStringList(quint16 cmdId, const QStringList &values);
// Queue commands for rows of the queue at `revision`.
QByteArray encodeCommandQueueRow(quint16 cmdId, quint64 revision, quint32 index);
QByteArray encodeCommandSelectQueue(quint64 revision, qint32 index);
QByteArray encodeCommandMoveQueue(quint64 revision, quint32 from, quint32 to);
QByteArray encodeCommandRemoveMany(quint64 revision, const QList<quint32> &indices);

bool decodeSnapshotPacket(const QByteArray &packet, DecodedSnapshot *out, QString *errorMessage);
bool decodeSearchResultsFrame(
    const QByteArray &payload,
    DecodedSearchResults *out,
    QString *errorMessage);

} // namespace BinaryBridgeCodec
