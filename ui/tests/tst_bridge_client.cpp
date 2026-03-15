#include <QApplication>
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
    void spectrogramDeltaSkipsMetadataOnlyChannels();
    void stoppedTrackChangeClearsPendingSpectrogramDelta();
    void asyncImageFileDetailsRequestCachesAndSignals();
    void itunesRectangularArtworkRowUsesNormalizedFileDetails();
    void itunesSquareArtworkReuseSkipsRedundantNormalization();
    void mprisPublishesPlaybackStateOnPlaybackSignal();
    void mprisCanPauseOnlyWhilePlaying();
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

void BridgeClientTest::spectrogramDeltaSkipsMetadataOnlyChannels() {
    BridgeClient client;
    isolateBridgeClient(client);

    BridgeClient::SpectrogramChannelDelta channel;
    channel.label = QStringLiteral("L");
    channel.packedBins = 128;
    channel.packedRowsCount = 0;
    client.m_spectrogramChannels.push_back(channel);

    const QVariantMap delta = client.takeSpectrogramRowsDeltaPacked();
    const QVariantList channels = delta.value(QStringLiteral("channels")).toList();
    QCOMPARE(channels.size(), 0);
    QCOMPARE(client.m_spectrogramChannels.size(), 0);
}

void BridgeClientTest::stoppedTrackChangeClearsPendingSpectrogramDelta() {
    BridgeClient client;
    isolateBridgeClient(client);

    client.m_playbackState = QStringLiteral("Stopped");
    client.m_currentTrackPath = QStringLiteral("/music/old-track.flac");
    client.m_spectrogramReset = true;

    BridgeClient::SpectrogramChannelDelta channel;
    channel.label = QStringLiteral("L");
    channel.packedBins = 4;
    channel.packedRows = QByteArray::fromHex("01020304");
    channel.packedRowsCount = 1;
    client.m_spectrogramChannels.push_back(channel);

    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 0;
    snapshot.playback.currentPath = QStringLiteral("/music/new-track.flac");

    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.m_currentTrackPath, QStringLiteral("/music/new-track.flac"));
    QCOMPARE(client.m_spectrogramChannels.size(), 0);
    QCOMPARE(client.m_spectrogramReset, false);
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

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    BridgeClientTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_bridge_client.moc"
