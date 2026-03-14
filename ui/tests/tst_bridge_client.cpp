#include <QApplication>
#include <QSignalSpy>
#include <QtTest/QtTest>

#define private public
#include "../src/BridgeClient.h"
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

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    BridgeClientTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_bridge_client.moc"
