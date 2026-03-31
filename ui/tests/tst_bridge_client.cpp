// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QDateTime>
#include <QDBusObjectPath>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

#define private public
#include "../src/BridgeClient.h"
#include "../src/MprisController.h"
#undef private

#include "../src/FerrousBridgeFfi.h"

namespace {

void isolateBridgeClient(BridgeClient &client) {
    client.m_bridgePollTimer.stop();
    if (client.m_bridgeWakeNotifier != nullptr) {
        client.m_bridgeWakeNotifier->setEnabled(false);
        delete client.m_bridgeWakeNotifier;
        client.m_bridgeWakeNotifier = nullptr;
    }
    client.m_bridgeWakeFd = -1;
    client.shutdownBridgeGracefully();
    if (client.m_ffiBridge != nullptr) {
        ferrous_ffi_bridge_destroy(client.m_ffiBridge);
        client.m_ffiBridge = nullptr;
    }
    client.m_connected = false;
}

} // namespace

class BridgeClientTest : public QObject {
    Q_OBJECT

private slots:
    void playAtDoesNotEmitImmediateSnapshotChanged();
    void queueSnapshotKeepsRawCoverPathsInRows();
    void queuePathFallbackUsesCachedFirstIndex();
    void inProcessBridgeInstallsWakeNotifier();
    void scheduleBridgePollDisablesWakeNotifierAndPrefersSoonerRearm();
    void diagnosticsWritesBatchOffHotPath();
    void clearDiagnosticsDropsPendingDiskWrites();
    void pendingSeekIgnoresStalePlaybackSnapshotUntilTargetArrives();
    void asyncImageFileDetailsRequestCachesAndSignals();
    void itunesRectangularArtworkRowUsesNormalizedFileDetails();
    void testMutedChannelsMaskDecoding();
    void itunesSquareArtworkReuseSkipsRedundantNormalization();
    void trackOnlySnapshotSetsTrackFlagOnly();
    void gaplessHandoffSetsIdentityFlagBeforeMetadata();
    void coverLookupSchedulesTrackChangedNotSnapshotChanged();
    void mprisRepublishesOnTrackMetadataChanged();
    void mprisRepublishesOnTrackIdentityChanged();
    void mprisPublishesPlaybackStateOnPlaybackSignal();
    void mprisCanPauseOnlyWhilePlaying();
    void mprisControllerConstructionDoesNotCrash();
    void spectrogramOverlaySettingsApplyFromSnapshot();
    void spectrogramOverlaySettingsDecodeFromBinaryPayload();
    void testSoloChannelCommandEncoding();
};

void BridgeClientTest::playAtDoesNotEmitImmediateSnapshotChanged() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_selectedQueueIndex = 2;

    QSignalSpy snapshotSpy(&client, SIGNAL(snapshotChanged()));

    client.playAt(9);

    QCOMPARE(snapshotSpy.count(), 0);
    QCOMPARE(client.m_selectedQueueIndex, 2);
    QCOMPARE(client.m_pendingQueueSelection, 9);
    QVERIFY(client.m_pendingQueueSelectionUntilMs > 0);
}

void BridgeClientTest::queueSnapshotKeepsRawCoverPathsInRows() {
    BridgeClient client;
    isolateBridgeClient(client);

    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.queue.present = true;
    snapshot.queue.len = 1;
    snapshot.queue.selectedIndex = 0;
    snapshot.queue.totalDurationSeconds = 185.0;
    snapshot.queue.unknownDurationCount = 0;
    snapshot.queue.tracks.push_back(BinaryBridgeCodec::DecodedQueueTrack{
        QStringLiteral("Track A"),
        QStringLiteral("Artist A"),
        QStringLiteral("Album A"),
        QStringLiteral("/music/Artist A/Album A/cover.jpg"),
        QStringLiteral("Electronic"),
        2024,
        1,
        185.0f,
        QStringLiteral("/music/Artist A/Album A/01 - Track A.flac"),
    });

    QVERIFY(client.processBinarySnapshot(snapshot));

    auto *rows = qobject_cast<QueueRowsModel *>(client.queueRows());
    QVERIFY(rows != nullptr);
    QCOMPARE(rows->rowCount(), 1);
    QCOMPARE(
        rows->data(rows->index(0, 0), QueueRowsModel::CoverPathRole).toString(),
        QStringLiteral("/music/Artist A/Album A/cover.jpg"));
}

void BridgeClientTest::queuePathFallbackUsesCachedFirstIndex() {
    BridgeClient client;
    isolateBridgeClient(client);

    const QString duplicatePath = QStringLiteral("/music/Artist/Album/duplicate.flac");

    BinaryBridgeCodec::DecodedSnapshot queueSnapshot;
    queueSnapshot.queue.present = true;
    queueSnapshot.queue.len = 3;
    queueSnapshot.queue.selectedIndex = 0;
    queueSnapshot.queue.totalDurationSeconds = 540.0;
    queueSnapshot.queue.unknownDurationCount = 0;
    queueSnapshot.queue.tracks.push_back(BinaryBridgeCodec::DecodedQueueTrack{
        QStringLiteral("Duplicate First"),
        QStringLiteral("Artist"),
        QStringLiteral("Album"),
        QStringLiteral(),
        QStringLiteral("Electronic"),
        2024,
        1,
        180.0f,
        duplicatePath,
    });
    queueSnapshot.queue.tracks.push_back(BinaryBridgeCodec::DecodedQueueTrack{
        QStringLiteral("Middle"),
        QStringLiteral("Artist"),
        QStringLiteral("Album"),
        QStringLiteral(),
        QStringLiteral("Electronic"),
        2024,
        2,
        180.0f,
        QStringLiteral("/music/Artist/Album/middle.flac"),
    });
    queueSnapshot.queue.tracks.push_back(BinaryBridgeCodec::DecodedQueueTrack{
        QStringLiteral("Duplicate Second"),
        QStringLiteral("Artist"),
        QStringLiteral("Album"),
        QStringLiteral(),
        QStringLiteral("Electronic"),
        2024,
        3,
        180.0f,
        duplicatePath,
    });

    QVERIFY(client.processBinarySnapshot(queueSnapshot));
    QCOMPARE(client.m_queuePathFirstIndex.value(duplicatePath, -1), 0);

    BinaryBridgeCodec::DecodedSnapshot playbackSnapshot;
    playbackSnapshot.playback.present = true;
    playbackSnapshot.playback.state = 1;
    playbackSnapshot.playback.currentQueueIndex = -1;
    playbackSnapshot.playback.currentPath = duplicatePath;

    QVERIFY(client.processBinarySnapshot(playbackSnapshot));
    QCOMPARE(client.m_playingQueueIndex, 0);
    QCOMPARE(client.currentTrackTitle(), QStringLiteral("Duplicate First"));
}

void BridgeClientTest::inProcessBridgeInstallsWakeNotifier() {
    BridgeClient client;
    QVERIFY(client.m_ffiBridge != nullptr);
    QVERIFY(client.m_bridgeWakeFd >= 0);
    QVERIFY(client.m_bridgeWakeNotifier != nullptr);
    QVERIFY(client.m_bridgeWakeNotifier->isEnabled());

    isolateBridgeClient(client);
}

void BridgeClientTest::scheduleBridgePollDisablesWakeNotifierAndPrefersSoonerRearm() {
    BridgeClient client;
    QVERIFY(client.m_ffiBridge != nullptr);
    QVERIFY(client.m_bridgeWakeNotifier != nullptr);

    client.m_bridgePollTimer.stop();
    client.m_bridgeWakeNotifier->setEnabled(true);
    client.scheduleBridgePoll(160);
    QVERIFY(client.m_bridgePollTimer.isActive());
    QCOMPARE(client.m_bridgePollTimer.interval(), 160);
    QVERIFY(!client.m_bridgeWakeNotifier->isEnabled());

    client.scheduleBridgePoll(0);
    QCOMPARE(client.m_bridgePollTimer.interval(), 0);
    QVERIFY(!client.m_bridgeWakeNotifier->isEnabled());

    isolateBridgeClient(client);
}

void BridgeClientTest::diagnosticsWritesBatchOffHotPath() {
    BridgeClient client;
    isolateBridgeClient(client);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("diagnostics.log"));

    client.m_diagnosticsFlushTimer.stop();
    client.m_diagnosticsLogPath = logPath;
    client.m_pendingDiagnosticsDiskLines.clear();
    client.m_diagnosticsLines.clear();
    QFile::remove(logPath);

    client.logDiagnostic(QStringLiteral("ui"), QStringLiteral("first line"));
    client.logDiagnostic(QStringLiteral("ui"), QStringLiteral("second line"));

    QVERIFY(client.m_diagnosticsFlushTimer.isActive());
    QCOMPARE(client.m_pendingDiagnosticsDiskLines.size(), 2);
    QVERIFY(!QFileInfo::exists(logPath));

    QTRY_COMPARE_WITH_TIMEOUT(client.m_pendingDiagnosticsDiskLines.size(), 0, 1000);

    QFile file(logPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString contents = QString::fromUtf8(file.readAll());
    QVERIFY(contents.contains(QStringLiteral("[ui] first line")));
    QVERIFY(contents.contains(QStringLiteral("[ui] second line")));
}

void BridgeClientTest::clearDiagnosticsDropsPendingDiskWrites() {
    BridgeClient client;
    isolateBridgeClient(client);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("diagnostics.log"));

    client.m_diagnosticsFlushTimer.stop();
    client.m_diagnosticsLogPath = logPath;
    client.m_pendingDiagnosticsDiskLines.clear();
    client.m_diagnosticsLines.clear();
    QFile::remove(logPath);

    client.logDiagnostic(QStringLiteral("ui"), QStringLiteral("stale line"));
    client.logDiagnostic(QStringLiteral("ui"), QStringLiteral("stale line 2"));
    QCOMPARE(client.m_pendingDiagnosticsDiskLines.size(), 2);

    client.clearDiagnostics();

    QCOMPARE(client.m_pendingDiagnosticsDiskLines.size(), 1);
    QVERIFY(client.m_diagnosticsFlushTimer.isActive());

    QTRY_COMPARE_WITH_TIMEOUT(client.m_pendingDiagnosticsDiskLines.size(), 0, 1000);

    QFile file(logPath);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const QStringList lines =
        QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.first().contains(QStringLiteral("[ui] diagnostics cleared")));
}

void BridgeClientTest::pendingSeekIgnoresStalePlaybackSnapshotUntilTargetArrives() {
    BridgeClient client;
    isolateBridgeClient(client);

    client.m_playbackState = QStringLiteral("Playing");
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    client.m_positionSeconds = 60.0;
    client.m_positionText = QStringLiteral("01:00");
    client.m_durationSeconds = 180.0;
    client.m_durationText = QStringLiteral("03:00");
    client.m_pendingSeek = true;
    client.m_pendingSeekTargetSeconds = 60.0;
    client.m_pendingSeekStartedAtMs = QDateTime::currentMSecsSinceEpoch() - 500;
    client.m_pendingSeekUntilMs = QDateTime::currentMSecsSinceEpoch() + 400;

    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1;
    snapshot.playback.currentPath = QStringLiteral("/music/track.flac");
    snapshot.playback.positionSeconds = 12.0;
    snapshot.playback.durationSeconds = 180.0;

    QVERIFY(client.processBinarySnapshot(snapshot));

    QVERIFY(client.m_pendingSeek);
    QCOMPARE(client.m_positionSeconds, 60.0);
    QCOMPARE(client.m_positionText, QStringLiteral("01:00"));
    QVERIFY(!client.m_pollPlaybackChanged);
}

void BridgeClientTest::asyncImageFileDetailsRequestCachesAndSignals() {
    BridgeClient client;
    isolateBridgeClient(client);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourcePath = tempDir.filePath(QStringLiteral("cover.png"));
    QImage image(128, 96, QImage::Format_RGB32);
    image.fill(Qt::green);
    QVERIFY(image.save(sourcePath, "PNG"));

    QSignalSpy detailsSpy(&client, SIGNAL(imageFileDetailsChanged(QString)));
    QVERIFY(client.cachedImageFileDetails(sourcePath).isEmpty());

    client.requestImageFileDetails(sourcePath);

    QTRY_VERIFY_WITH_TIMEOUT(detailsSpy.count() > 0, 3000);

    const QVariantMap result = client.cachedImageFileDetails(sourcePath);
    QCOMPARE(result.value(QStringLiteral("fileName")).toString(), QStringLiteral("cover.png"));
    QCOMPARE(result.value(QStringLiteral("width")).toInt(), 128);
    QCOMPARE(result.value(QStringLiteral("height")).toInt(), 96);
    QCOMPARE(result.value(QStringLiteral("format")).toString(), QStringLiteral("PNG"));
    QCOMPARE(result.value(QStringLiteral("path")).toString(), QFileInfo(sourcePath).canonicalFilePath());
}

void BridgeClientTest::itunesRectangularArtworkRowUsesNormalizedFileDetails() {
    BridgeClient client;
    isolateBridgeClient(client);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourcePath = tempDir.filePath(QStringLiteral("rect.jpg"));
    QImage image(300, 200, QImage::Format_RGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(sourcePath, "JPG", 95));

    BridgeClient::ItunesArtworkCandidate candidate;
    candidate.albumTitle = QStringLiteral("Album");
    candidate.artistName = QStringLiteral("Artist");
    candidate.assetUrls = QStringList{QUrl::fromLocalFile(sourcePath).toString()};
    client.m_itunesArtworkCandidates.push_back(candidate);

    QVariantMap row;
    row.insert(QStringLiteral("albumTitle"), candidate.albumTitle);
    row.insert(QStringLiteral("artistName"), candidate.artistName);
    row.insert(QStringLiteral("previewSource"), QString());
    row.insert(QStringLiteral("normalizedPath"), QString());
    row.insert(QStringLiteral("normalizedUrl"), QString());
    row.insert(QStringLiteral("downloadPath"), QString());
    row.insert(QStringLiteral("assetLoading"), false);
    row.insert(QStringLiteral("assetReady"), false);
    row.insert(QStringLiteral("assetError"), QString());
    client.m_itunesArtworkResults.push_back(row);
    client.m_itunesArtworkGeneration = 1;

    QSignalSpy artworkSpy(&client, SIGNAL(itunesArtworkChanged()));
    client.startItunesArtworkAssetDownload(0);

    QTRY_VERIFY_WITH_TIMEOUT(artworkSpy.count() > 0, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.itunesArtworkResultAt(0).value(QStringLiteral("assetReady")).toBool(), 3000);

    const QVariantMap result = client.itunesArtworkResultAt(0);
    const QString normalizedPath = result.value(QStringLiteral("normalizedPath")).toString();
    const QString downloadPath = result.value(QStringLiteral("downloadPath")).toString();
    QVERIFY(!normalizedPath.isEmpty());
    QVERIFY(!downloadPath.isEmpty());
    QVERIFY(normalizedPath != downloadPath);

    const QFileInfo normalizedInfo(normalizedPath);
    const QFileInfo downloadInfo(downloadPath);
    QVERIFY(normalizedInfo.exists());
    QVERIFY(downloadInfo.exists());
    QCOMPARE(result.value(QStringLiteral("fileName")).toString(), normalizedInfo.fileName());
    QCOMPARE(result.value(QStringLiteral("fileSizeBytes")).toLongLong(), normalizedInfo.size());
    QVERIFY(result.value(QStringLiteral("fileSizeBytes")).toLongLong() != downloadInfo.size());
    QCOMPARE(result.value(QStringLiteral("path")).toString(), normalizedInfo.canonicalFilePath());
}

void BridgeClientTest::itunesSquareArtworkReuseSkipsRedundantNormalization() {
    BridgeClient client;
    isolateBridgeClient(client);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourcePath = tempDir.filePath(QStringLiteral("square.jpg"));
    QImage image(300, 300, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(sourcePath, "JPG", 95));

    BridgeClient::ItunesArtworkCandidate candidate;
    candidate.albumTitle = QStringLiteral("Album");
    candidate.artistName = QStringLiteral("Artist");
    candidate.assetUrls = QStringList{QUrl::fromLocalFile(sourcePath).toString()};
    client.m_itunesArtworkCandidates.push_back(candidate);

    QVariantMap row;
    row.insert(QStringLiteral("albumTitle"), candidate.albumTitle);
    row.insert(QStringLiteral("artistName"), candidate.artistName);
    row.insert(QStringLiteral("previewSource"), QString());
    row.insert(QStringLiteral("normalizedPath"), QString());
    row.insert(QStringLiteral("normalizedUrl"), QString());
    row.insert(QStringLiteral("downloadPath"), QString());
    row.insert(QStringLiteral("assetLoading"), false);
    row.insert(QStringLiteral("assetReady"), false);
    row.insert(QStringLiteral("assetError"), QString());
    client.m_itunesArtworkResults.push_back(row);
    client.m_itunesArtworkGeneration = 1;

    QSignalSpy artworkSpy(&client, SIGNAL(itunesArtworkChanged()));
    client.startItunesArtworkAssetDownload(0);

    QTRY_VERIFY_WITH_TIMEOUT(artworkSpy.count() > 0, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(client.itunesArtworkResultAt(0).value(QStringLiteral("assetReady")).toBool(), 3000);

    const QVariantMap result = client.itunesArtworkResultAt(0);
    const QString normalizedPath = result.value(QStringLiteral("normalizedPath")).toString();
    const QString downloadPath = result.value(QStringLiteral("downloadPath")).toString();
    QVERIFY(!normalizedPath.isEmpty());
    QVERIFY(!downloadPath.isEmpty());
    QCOMPARE(normalizedPath, downloadPath);
    QCOMPARE(result.value(QStringLiteral("fileSizeBytes")).toLongLong(), QFileInfo(normalizedPath).size());
}

void BridgeClientTest::trackOnlySnapshotSetsTrackFlagOnly() {
    BridgeClient client;
    isolateBridgeClient(client);

    // Pre-populate a playing track so the snapshot changes metadata fields.
    client.m_playbackState = QStringLiteral("Playing");
    client.m_currentTrackPath = QStringLiteral("/music/old.flac");
    client.m_currentTrackTitle = QStringLiteral("Old Title");
    client.m_currentTrackArtist = QStringLiteral("Old Artist");
    client.m_playingQueueIndex = 0;

    // Build a snapshot that changes both identity (path/index) and metadata.
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1; // Playing
    snapshot.playback.currentQueueIndex = 1;
    snapshot.playback.currentPath = QStringLiteral("/music/new.flac");

    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = QStringLiteral("/music/new.flac");
    snapshot.metadata.title = QStringLiteral("New Title");
    snapshot.metadata.artist = QStringLiteral("New Artist");
    snapshot.metadata.album = QStringLiteral("New Album");

    // Reset poll flags before processing.
    client.m_pollPlaybackChanged = false;
    client.m_pollTrackIdentityChanged = false;
    client.m_pollTrackMetadataChanged = false;
    client.m_pollSnapshotChanged = false;

    QVERIFY(client.processBinarySnapshot(snapshot));

    // Both identity and metadata changed, neither is a snapshot-level change.
    QVERIFY(client.m_pollTrackIdentityChanged);
    QVERIFY(client.m_pollTrackMetadataChanged);
    QVERIFY(!client.m_pollSnapshotChanged);
}

void BridgeClientTest::gaplessHandoffSetsIdentityFlagBeforeMetadata() {
    BridgeClient client;
    isolateBridgeClient(client);

    // Pre-populate a playing track with metadata.
    // m_currentTrackFormatLabel must be pre-set because processBinarySnapshot
    // falls back to formatLabelFromPath(currentPath) when the existing label
    // is empty. Without this, phase 1 would derive "FLAC" from the new path
    // and set the metadata flag.
    client.m_playbackState = QStringLiteral("Playing");
    client.m_currentTrackPath = QStringLiteral("/music/old.flac");
    client.m_currentTrackTitle = QStringLiteral("Old Title");
    client.m_currentTrackArtist = QStringLiteral("Old Artist");
    client.m_currentTrackFormatLabel = QStringLiteral("FLAC");
    client.m_playingQueueIndex = 0;

    // Phase 1: Gapless handoff — path/index advance, metadata still for OLD track.
    // The backend preserves old metadata until the metadata worker catches up.
    // sourcePath = old path, so metadataSourcePath != currentPath and metadata
    // fields are ignored by processBinarySnapshot.
    BinaryBridgeCodec::DecodedSnapshot handoff;
    handoff.playback.present = true;
    handoff.playback.state = 1; // Playing
    handoff.playback.currentQueueIndex = 1;
    handoff.playback.currentPath = QStringLiteral("/music/new.flac");

    handoff.metadata.present = true;
    handoff.metadata.sourcePath = QStringLiteral("/music/old.flac"); // stale source
    handoff.metadata.title = QStringLiteral("Old Title");
    handoff.metadata.artist = QStringLiteral("Old Artist");

    client.m_pollTrackIdentityChanged = false;
    client.m_pollTrackMetadataChanged = false;
    client.m_pollSnapshotChanged = false;

    QVERIFY(client.processBinarySnapshot(handoff));
    QVERIFY(client.m_pollTrackIdentityChanged);
    QVERIFY(!client.m_pollTrackMetadataChanged);  // metadata unchanged

    // Phase 2: Metadata worker delivers new metadata, same path/index.
    // sourcePath now matches currentPath, so metadata fields ARE applied.
    BinaryBridgeCodec::DecodedSnapshot metaUpdate;
    metaUpdate.playback.present = true;
    metaUpdate.playback.state = 1;
    metaUpdate.playback.currentQueueIndex = 1;
    metaUpdate.playback.currentPath = QStringLiteral("/music/new.flac");

    metaUpdate.metadata.present = true;
    metaUpdate.metadata.sourcePath = QStringLiteral("/music/new.flac");
    metaUpdate.metadata.title = QStringLiteral("New Title");
    metaUpdate.metadata.artist = QStringLiteral("New Artist");
    metaUpdate.metadata.album = QStringLiteral("New Album");

    client.m_pollTrackIdentityChanged = false;
    client.m_pollTrackMetadataChanged = false;

    QVERIFY(client.processBinarySnapshot(metaUpdate));
    QVERIFY(!client.m_pollTrackIdentityChanged);  // identity unchanged
    QVERIFY(client.m_pollTrackMetadataChanged);
}

void BridgeClientTest::coverLookupSchedulesTrackChangedNotSnapshotChanged() {
    BridgeClient client;
    isolateBridgeClient(client);

    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    client.m_currentTrackCoverPath = QStringLiteral("file:///old-cover.jpg");

    // Apply a new cover via the async lookup path.
    client.applyTrackCoverLookupResult(
        QStringLiteral("/music/track.flac"),
        QStringLiteral("file:///new-cover.jpg"));

    // The cover update must schedule trackMetadataChanged, not identity or snapshot.
    QVERIFY(client.m_trackMetadataChangedPending);
    QVERIFY(!client.m_trackIdentityChangedPending);
    QVERIFY(!client.m_snapshotChangedPending);
    QCOMPARE(client.m_currentTrackCoverPath, QStringLiteral("file:///new-cover.jpg"));
}

void BridgeClientTest::mprisRepublishesOnTrackMetadataChanged() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 1;
    client.m_playbackState = QStringLiteral("Playing");
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    client.m_currentTrackTitle = QStringLiteral("Old Title");
    client.m_currentTrackArtist = QStringLiteral("Old Artist");

    MprisController controller(&client);
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;

    // Mutate only metadata fields (not path/index).
    client.m_currentTrackTitle = QStringLiteral("New Title");
    client.m_currentTrackArtist = QStringLiteral("New Artist");
    emit client.trackMetadataChanged();

    // MPRIS must have republished with the new metadata.
    QVERIFY(controller.m_hasPublishedPlayerState);
    const QVariantMap meta = controller.m_lastPlayerState.metadata;
    QCOMPARE(meta.value(QStringLiteral("xesam:title")).toString(),
             QStringLiteral("New Title"));
    const QStringList artists = meta.value(QStringLiteral("xesam:artist")).toStringList();
    QVERIFY(!artists.isEmpty());
    QCOMPARE(artists.first(), QStringLiteral("New Artist"));

    // Clean up D-Bus state before destruction.
    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}

void BridgeClientTest::mprisRepublishesOnTrackIdentityChanged() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 2;
    client.m_playbackState = QStringLiteral("Playing");
    client.m_currentTrackPath = QStringLiteral("/music/old.flac");
    client.m_playingQueueIndex = 0;

    MprisController controller(&client);
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;

    // Change path/index (identity) and emit trackIdentityChanged.
    client.m_currentTrackPath = QStringLiteral("/music/new.flac");
    client.m_playingQueueIndex = 1;
    emit client.trackIdentityChanged();

    // MPRIS must have republished after identity changed.
    QVERIFY(controller.m_hasPublishedPlayerState);
    const QVariantMap meta = controller.m_lastPlayerState.metadata;
    const QString trackId =
        meta.value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path();
    // Track ID is a D-Bus path derived from the file path hash — verify
    // it's a valid path (not the NoTrack sentinel).
    QVERIFY(!trackId.isEmpty());
    QVERIFY(!trackId.contains(QStringLiteral("NoTrack")));

    // Clean up D-Bus state before destruction.
    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}

void BridgeClientTest::mprisPublishesPlaybackStateOnPlaybackSignal() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 1;
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    client.m_playbackState = QStringLiteral("Stopped");

    MprisController controller(&client);
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;

    emit client.playbackChanged();

    QVERIFY(controller.m_hasPublishedPlayerState);
    QCOMPARE(controller.m_lastPlayerState.playbackStatus, QStringLiteral("Stopped"));
    QCOMPARE(controller.m_lastPlayerState.canPause, false);

    client.m_playbackState = QStringLiteral("Playing");
    emit client.playbackChanged();

    QCOMPARE(controller.m_lastPlayerState.playbackStatus, QStringLiteral("Playing"));
    QCOMPARE(controller.m_lastPlayerState.canPause, true);

    // Reset injected D-Bus state before destruction. The destructor calls
    // unregisterService() when m_serviceRegistered is true, which is a blocking
    // D-Bus roundtrip. In the RPM %check environment this crashes inside libdbus
    // (uninitialised pending-call slot mutex) because no real registration happened.
    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}


void BridgeClientTest::mprisCanPauseOnlyWhilePlaying() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 1;
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");

    MprisController controller(&client);

    client.m_playbackState = QStringLiteral("Stopped");
    QCOMPARE(controller.canPause(), false);

    client.m_playbackState = QStringLiteral("Paused");
    QCOMPARE(controller.canPause(), false);

    client.m_playbackState = QStringLiteral("Playing");
    QCOMPARE(controller.canPause(), true);
}

void BridgeClientTest::mprisControllerConstructionDoesNotCrash() {
    // Regression test: constructing MprisController must not crash even when
    // the D-Bus session bus is in the partially-initialised state found in the
    // RPM %check environment (bus is connected but libdbus's global
    // pending-call slot mutex has never been touched in this process).
    //
    // The constructor defers setEnabled() via QTimer::singleShot(0), so the
    // blocking registerService() call only runs when an event loop is spinning.
    // In this test we deliberately do NOT call processEvents() so that the
    // timer never fires — that is the correct test-environment behaviour.
    //
    // The destructor is the other crash site: it called unregisterService()
    // unconditionally when m_serviceRegistered was true. It is now guarded
    // behind isConnected(). In this test m_serviceRegistered stays false
    // (no event loop ran) so the destructor does nothing either way.
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    client.m_playbackState = QStringLiteral("Playing");

    {
        MprisController controller(&client);
        // Construction must not crash. The deferred timer has not fired yet.
        // m_enabled is false until setEnabled() runs, which is intentional.
        QCOMPARE(controller.playbackStatus(), QStringLiteral("Playing"));
        QCOMPARE(controller.canPause(), true);
        // Destructor runs here: must not crash.
    }
}


void BridgeClientTest::spectrogramOverlaySettingsApplyFromSnapshot() {
    BridgeClient client;
    isolateBridgeClient(client);

    // Defaults should be false.
    QCOMPARE(client.showSpectrogramCrosshair(), false);
    QCOMPARE(client.showSpectrogramScale(), false);

    // Apply a snapshot with both enabled.
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.settings.present = true;
    snapshot.settings.showSpectrogramCrosshair = true;
    snapshot.settings.showSpectrogramScale = true;
    QVERIFY(client.processBinarySnapshot(snapshot));

    QCOMPARE(client.showSpectrogramCrosshair(), true);
    QCOMPARE(client.showSpectrogramScale(), true);

    // Apply a snapshot with both disabled.
    snapshot.settings.showSpectrogramCrosshair = false;
    snapshot.settings.showSpectrogramScale = false;
    QVERIFY(client.processBinarySnapshot(snapshot));

    QCOMPARE(client.showSpectrogramCrosshair(), false);
    QCOMPARE(client.showSpectrogramScale(), false);
}

void BridgeClientTest::spectrogramOverlaySettingsDecodeFromBinaryPayload() {
    // Build a settings section payload matching the Rust encode layout:
    // volume(f32), fftSize(u32), viewMode(u8), dbRange(f32),
    // logScale(u8), showFps(u8), sortMode(i32), sysMC(u8),
    // viewerFS(u8), displayMode(u8), crosshair(u8), scale(u8).
    QByteArray settingsPayload;
    QDataStream ds(&settingsPayload, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.setFloatingPointPrecision(QDataStream::SinglePrecision);
    ds << float(1.0f);     // volume
    ds << quint32(8192);   // fftSize
    ds << quint8(0);       // spectrogramViewMode
    ds << float(132.0f);   // dbRange
    ds << quint8(0);       // logScale
    ds << quint8(0);       // showFps
    ds << qint32(0);       // librarySortMode
    ds << quint8(1);       // systemMediaControlsEnabled
    ds << quint8(0);       // viewerFullscreenMode
    ds << quint8(0);       // spectrogramDisplayMode
    ds << quint8(1);       // showSpectrogramCrosshair
    ds << quint8(1);       // showSpectrogramScale

    // Wrap in a full snapshot packet:
    // header: magic(u32) + totalLength(u32) + sectionMask(u16) + reserved(u16)
    // section: length(u32) + payload
    const quint32 magic = 0xFE550001u;
    const quint16 mask = 1u << 5; // SectionSettings
    const quint32 sectionLen = static_cast<quint32>(settingsPayload.size());
    const quint32 totalLen = 12 + 4 + sectionLen; // header + section header + payload

    QByteArray packet;
    QDataStream ps(&packet, QIODevice::WriteOnly);
    ps.setByteOrder(QDataStream::LittleEndian);
    ps << magic;
    ps << totalLen;
    ps << mask;
    ps << quint16(0); // reserved
    ps << sectionLen;
    packet.append(settingsPayload);

    // Decode through the real decodeSnapshotPacket path.
    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(packet, &decoded, &error),
             qPrintable(error));

    QVERIFY(decoded.settings.present);
    QCOMPARE(decoded.settings.showSpectrogramCrosshair, true);
    QCOMPARE(decoded.settings.showSpectrogramScale, true);
    // Verify other fields survived too.
    QCOMPARE(decoded.settings.systemMediaControlsEnabled, true);
    QCOMPARE(decoded.settings.fftSize, 8192);
}

void BridgeClientTest::testMutedChannelsMaskDecoding()
{
    // Build a minimal snapshot packet with a playback section that
    // includes a muted_channels_mask field (u64 LE at the end).
    // Write raw LE bytes to avoid QDataStream floating-point precision quirks.
    QByteArray payload;
    auto appendLe = [&payload](auto value) {
        const auto le = qToLittleEndian(value);
        payload.append(reinterpret_cast<const char *>(&le), sizeof(le));
    };

    appendLe(quint8(1));          // state = Playing
    appendLe(double(10.0));       // position (f64)
    appendLe(double(200.0));      // duration (f64)
    appendLe(float(0.8f));        // volume   (f32)
    appendLe(quint8(0));          // repeat mode
    appendLe(quint8(0));          // shuffle
    appendLe(qint32(2));          // currentQueueIndex
    // u16-prefixed string (path)
    QByteArray pathUtf8 = QStringLiteral("/test.flac").toUtf8();
    appendLe(quint16(pathUtf8.size()));
    payload.append(pathUtf8);
    // muted_channels_mask
    appendLe(quint64(0b10101));   // channels 0, 2, 4 muted

    // Wrap in snapshot packet.
    QByteArray packet;
    QDataStream ps(&packet, QIODevice::WriteOnly);
    ps.setByteOrder(QDataStream::LittleEndian);
    ps << quint32(0xFE550001u);       // magic
    quint32 totalLen = 12 + 4 + payload.size(); // header + section_len + payload
    ps << quint32(totalLen);
    ps << quint16(BinaryBridgeCodec::SectionPlayback); // section mask
    ps << quint16(0);                 // reserved
    ps << quint32(payload.size());
    packet.append(payload);

    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(packet, &decoded, &error),
             qPrintable(error));
    QCOMPARE(decoded.playback.mutedChannelsMask, quint64(0b10101));
}

void BridgeClientTest::testSoloChannelCommandEncoding()
{
    // Verify that CmdSoloChannel encodes as command ID 54 + 1-byte channel index,
    // using the same encodeCommandU8 helper as CmdToggleChannelMute.
    QByteArray cmd = BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSoloChannel, 3);
    // Format: u16 cmd_id (LE) + u16 payload_len (LE) + u8 channel
    QCOMPARE(cmd.size(), 5);
    quint16 cmdId = qFromLittleEndian<quint16>(cmd.constData());
    quint16 payloadLen = qFromLittleEndian<quint16>(cmd.constData() + 2);
    quint8 channel = static_cast<quint8>(cmd.at(4));
    QCOMPARE(cmdId, quint16(BinaryBridgeCodec::CmdSoloChannel));
    QCOMPARE(payloadLen, quint16(1));
    QCOMPARE(channel, quint8(3));
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    BridgeClientTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_bridge_client.moc"
