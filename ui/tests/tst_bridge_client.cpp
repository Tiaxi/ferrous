// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QDateTime>
#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QSemaphore>
#include <QThreadPool>
#include <QSocketNotifier>
#include <QFutureWatcher>
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

#include <poll.h>

#define private public
#include "../src/BridgeClient.h"
#include "../src/TagEditorController.h"
#include "../src/CoverImageProvider.h"
#include "../src/CoverRevisionLinks.h"
#include "../src/GlobalSearchResultsModel.h"
#include "../src/MprisController.h"
#include "../src/SpectrogramItem.h"
#undef private

#include "../src/FerrousBridgeFfi.h"
#include "../src/BinaryBridgeCodec.h"
#include <QtEndian>
#include "../src/SpectrogramTraceLogging.h"

namespace {

// Let `client`, isolated from the bridge, take commands as if it sent them.
void acceptCommands(BridgeClient &client) {
    client.m_sendCommandOverride = [](const QByteArray &) { return true; };
}

void isolateBridgeClient(BridgeClient &client) {
    // Settle the constructor's queued activity update before disconnecting.
    client.updateSpectrogramActivity();
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

QByteArray makePrecomputedFrame(
    const int bins,
    const int channelCount,
    const int columns,
    const int startIndex,
    const int totalEstimate,
    const quint64 trackToken,
    const quint64 generation) {
    constexpr int headerLen = 47;
    QByteArray raw(headerLen + bins * channelCount * columns, '\0');
    auto *d = reinterpret_cast<quint8 *>(raw.data());

    const quint32 payloadLen = static_cast<quint32>(raw.size() - 4);
    memcpy(d, &payloadLen, sizeof(payloadLen));
    const int base = 4;
    d[base] = 0xA2;
    memcpy(d + base + 1, &trackToken, sizeof(trackToken));
    memcpy(d + base + 9, &generation, sizeof(generation));
    const quint16 encodedBins = static_cast<quint16>(bins);
    const quint16 encodedColumns = static_cast<quint16>(columns);
    memcpy(d + base + 17, &encodedBins, sizeof(encodedBins));
    memcpy(d + base + 19, &encodedColumns, sizeof(encodedColumns));
    d[base + 21] = static_cast<quint8>(channelCount);
    const quint32 encodedStart = static_cast<quint32>(startIndex);
    const quint32 encodedTotal = static_cast<quint32>(totalEstimate);
    const quint32 sampleRate = 48000;
    const quint16 hopSize = 1024;
    const float coverage = 1.0f;
    memcpy(d + base + 22, &encodedStart, sizeof(encodedStart));
    memcpy(d + base + 26, &encodedTotal, sizeof(encodedTotal));
    memcpy(d + base + 30, &sampleRate, sizeof(sampleRate));
    memcpy(d + base + 34, &hopSize, sizeof(hopSize));
    memcpy(d + base + 36, &coverage, sizeof(coverage));
    d[base + 41] = 1;

    for (int col = 0; col < columns; ++col) {
        for (int ch = 0; ch < channelCount; ++ch) {
            for (int bin = 0; bin < bins; ++bin) {
                const int offset = headerLen + col * channelCount * bins + ch * bins + bin;
                raw[offset] = static_cast<char>(ch == 0 ? 0x11 : 0x77);
            }
        }
    }
    return raw;
}

// Whether `fd` would wake the client's notifier now.
bool wakeFdIsReadable(int fd, int timeoutMs = 0) {
    pollfd target{fd, POLLIN, 0};
    return ::poll(&target, 1, timeoutMs) > 0 && (target.revents & POLLIN) != 0;
}

// A command the backend does not know, which it answers with an error event.
constexpr quint16 kUnknownCommandId = 0xFFFF;

bool sendUnknownCommand(FerrousFfiBridge *bridge) {
    const QByteArray command = BinaryBridgeCodec::encodeCommandNoPayload(kUnknownCommandId);
    return ferrous_ffi_bridge_send_binary(
        bridge,
        reinterpret_cast<const std::uint8_t *>(command.constData()),
        static_cast<std::size_t>(command.size()));
}

int unknownCommandErrors(const QSignalSpy &errors) {
    const QString expected = QStringLiteral("unknown binary command id %1").arg(kUnknownCommandId);
    return static_cast<int>(std::count_if(errors.cbegin(), errors.cend(),
        [&expected](const QList<QVariant> &error) { return error.at(0).toString() == expected; }));
}

QByteArray littleEndian32(qsizetype value) {
    const quint32 encoded = qToLittleEndian(static_cast<quint32>(value));
    return QByteArray(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}

// A FLAC stream with a STREAMINFO block, the given Vorbis comments, and no audio frames.
QByteArray flacWithComments(const QStringList &comments) {
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

// A snapshot of `path` playing at `seconds`, as the engine read it now.
BinaryBridgeCodec::DecodedSnapshot playbackSnapshot(
    const QString &path, double seconds, quint64 trackToken) {
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1;
    snapshot.playback.currentPath = path;
    snapshot.playback.positionSeconds = seconds;
    snapshot.playback.durationSeconds = 180.0;
    snapshot.playback.trackToken = trackToken;
    snapshot.playback.positionSampledAtNs = monotonicNowNs();
    snapshot.playback.positionAdvancing = true;
    return snapshot;
}

// Where the client's playback clock is now.
double clockPosition(const BridgeClient &client) {
    const PlaybackClock *clock = client.playbackClock();
    return clock->positionAt(clock->nowNs());
}

// Show `item` the data of track `token` from its start, as the worker does.
void feedTrackData(SpectrogramItem &item, quint64 token) {
    constexpr int bins = 8;
    item.feedPrecomputedChunk(
        QByteArray(), bins, 0, 0, 0, 8192, 48000, 1024, false, true, token, true);
    item.feedPrecomputedChunk(
        QByteArray(64 * bins, '\x40'), bins, 0, 64, 0, 8192, 48000, 1024, false, false, token,
        false);
}

} // namespace


class BridgeClientTest : public QObject {
    Q_OBJECT

private slots:
    void libraryViewStateKeepsOffsetsIntoTallRows();
    void spectrogramActivityFollowsVisibleItemsAndWindows();
    void tagJobsKeepUiResponsiveAndDiscardClosedLoads();
    void tagEditorAsyncFileRoundTrip();
    void tagEditorRenamesThroughTheBridgeGate();
    void tagEditorRenameReachesTheBridgeQueue();
    void tagEditorSaveSendsOnlyChangedFields();
    void tagEditorSavesFullDateRowAndKeepsOtherTags();
    void tagEditorShowsRefusedEditsAndKeepsTheFile();
    void tagEditorLoadReportsUnreadableFiles();
    void tagEditorAutoNumberRestartsInEachFolder();
    void tagEditorReportsFailuresAsAProperty();
    void tagEditorKeepsExtraValuesUntilTheirFieldIsSaved();
    void tagEditorSelectionIsAModelRole();
    void tagEditorEditsOfManyRowsAnnounceOnce();
    void tagEditorRenameKeepsRowsInPlace();
    void tagEditorExitDoesNotWaitForAStalledSave();
    void batchRemovalCodecPreservesIndices();
    void pathListsFarBeyond64KiBAreSentWholeInOneCommand();
    void thousandsOfAppendedPathsReachTheBackendInOrder();
    void queueUndoSectionIsDecoded();
    void undoSendsTheOfferedChangeAndSelectsTheRestoredRowsOnce();
    void aClearedQueueComesBackThroughTheBackend();
    void tagEditorPreviewsRenamesWithoutRenaming();
    void aRenamePreviewOfRowsLoadedAgainIsOutOfDate();
    void anUndoOfAClearWhileStoppedKeepsTheStoppedRowOfARepeatedTrack();
    void artworkGoesToTheTrackItWasSearchedForAfterPlaybackMovesOn();
    void artworkForATrackMovedSinceTheSearchIsRefusedAndWritesNothing();
    void aConfirmedRenameRunsOnlyThePreviewedRenames();
    void commandsOverTheBoundAreRefusedVisibly();
    void stringsPastTheOldLengthLimitCrossWhole();
    void playAtDoesNotEmitImmediateSnapshotChanged();
    void playAtKeepsTheClockUntilTheEngineSwitches();
    void trackSwitchReanchorsBeforeSpectrogramData_data();
    void trackSwitchReanchorsBeforeSpectrogramData();
    void sameFileReplayReanchorsByTrackToken_data();
    void sameFileReplayReanchorsByTrackToken();
    void noOpNextPreservesPosition();
    void queueSnapshotKeepsRawCoverPathsInRows();
    void queuePathFallbackUsesCachedFirstIndex();
    void queueDeltasChangeOnlyTheirRowsWithoutAReset();
    void queueRowRangeMovesDecodeAndMoveTheirRowsOnce();
    void movedQueueRowsTravelAsOneCommandWithTheRustLayout();
    void libraryItemsTravelAsOneCommandWithTheRustLayout();
    void aMissedQueueRevisionAsksForTheWholeQueueOnce();
    void rowCommandsCarryTheQueueRevision();
    void aLargeQueueEditDecodesOnlyTheChangedRows();
    void gaplessAndRepeatOneMoveOnlyThePlayingRow();
    void realBridgeQueueEditsArriveAsRowSignals();
    void inProcessBridgeInstallsWakeNotifier();
    void aStalledClientReceivesTheLatestQueueAndEveryError();
    void aDrainEndsWithNoWakeupPending();
    void scheduleBridgePollDisablesWakeNotifierAndPrefersSoonerRearm();
    void bridgePollRunResultCountsPrecomputedSpectrogramWork();
    void precomputedSpectrogramChunksRouteDirectlyToRegisteredItems();
    void delayedSpectrogramBatchesPreserveColumnsAndRejectOldGeneration();
    void spectrogramDetailedTraceLoggingRequiresExplicitOptIn();
    void diagnosticsWritesBatchOffHotPath();
    void profileDiagnosticsBypassUiThreadDiskQueue();
    void clearDiagnosticsDropsPendingDiskWrites();
    void pendingSeekIgnoresStalePlaybackSnapshotUntilTargetArrives();
    void seekThatDoesNotLandReleasesThePlayhead();
    void seekLandingElsewhereReanchorsThePlayhead();
    void seekPublishesOptimisticPositionAndExtendsPendingWindow();
    void seekImmediatelyReanchorsRegisteredCenteredSpectrograms();
    void stopSnapshotReanchorsCenteredSpectrogramsToBeginning();
    void stopSnapshotLeavesRollingSpectrogramsParked();
    void stopSnapshotClearsPendingSeekWindow();
    void asyncImageFileDetailsRequestCachesAndSignals();
    void asyncImageFileDetailsAcceptsImageProviderUrl();
    void itunesRectangularArtworkRowUsesNormalizedFileDetails();
    void testMutedChannelsMaskDecoding();
    void currentTrackChannelsExposesDecodedMetadata();
    void metadataChannelLabelsDecodeOptionalTail();
    void channelLayoutFollowsMatchingMetadataAcrossTracks();
    void itunesSquareArtworkReuseSkipsRedundantNormalization();
    void trackOnlySnapshotSetsTrackFlagOnly();
    void gaplessHandoffSetsIdentityFlagBeforeMetadata();
    void trackCoverIsTheBackendsChoiceWithItsStamp();
    void trackCoverHasNoFallbackOfTheUisOwn();
    void mprisRepublishesOnTrackMetadataChanged();
    void mprisRepublishesOnTrackIdentityChanged();
    void mprisPublishesPlaybackStateOnPlaybackSignal();
    void mprisCanPauseWhilePaused();
    void mprisControllerConstructionDoesNotCrash();
    void mprisArtUrlConvertsImageProviderToFileUrl();
    void mprisPublishesNewArtWhenTheCoverIsReplacedAtTheSamePath();
    void coverRevisionLinksAreMadeOncePerStampAndOldOnesRemoved();
    void coverRevisionLinksKeepThePublishedLinkThroughABurst();
    void mprisMetadataIsRebuiltOnlyForTrackChanges();
    void mprisAlbumArtistComesFromAlbumArtistTag();
    void albumArtistFollowsMatchingMetadataAcrossTracks();
    void mprisPositionReadsThePlaybackClock();
    void mprisRateWritePausesOnlyForZero();
    void mprisOpenUriOpensOnlyLocalFiles();
    void mprisSeekedReportsEachSeekWhereItLanded();
    void mprisCannotSeekAStoppedTrack();
    void mprisReportsAReplayOfTheSameFileAsSeeked();
    void playbackErrorPacketReachesBridgeError();
    void backendFailurePacketShowsStoppedState();
    void requestedStopIsNotABackendFailure();
    void commandsToAStoppedBackendFailVisibly();
    void destroyingTheClientWaitsForTheBackendsFinalSave();
    void saveBeforeQuitReportsAFailedSaveAndSavesOnRetry();
    void saveBeforeQuitFailsWhenTheBackendHasStopped();
    void spectrogramOverlaySettingsApplyFromSnapshot();
    void spectrogramOverlaySettingsDecodeFromBinaryPayload();
    void testSoloChannelCommandEncoding();
    void testSoloedChannelDecoding();
    void testSoloedChannelDecodingNone();
    void testPlaybackTrackTokenDecoding();
    void testChannelButtonsVisibilityDecoding();
    void libraryViewStateAvailabilityFollowsSnapshot();
    void libraryChangesNotifyOnlyLibraryProperties();
    void libraryWatchingDecodesFromBinaryPayload();
    void libraryScanErrorsAreReportedAsLibraryErrors();
    void unwatchedLibraryRootsAreShownAndReportedOnce();
    void searchExtendedMatchDetailsDecodeAndHighlight();
    void searchResultsRankFilterAndHighlight();
    void searchProtocolTotalsAndExpansion();
    void searchDeferredResultsRejectSupersededQuery();
    void searchModelDelegateTypeRoleIsExposed();
    void searchModelDelegateTypeValuesCorrect();
    void clearSearchQueryRetainsModelRows();
    void newSearchResultsClearRetainedFlag();
    void searchModelBatchedInsertionFromEmpty();
    void searchModelBatchedInsertionSmallSetSkipsBatching();
    void searchModelReplaceRowsCancelsBatchInProgress();
    void searchRetainAndReuseIntegration();
    void coverImageProviderLoadsValidImage();
    void coverImageProviderRespectsRequestedSize();
    void coverImageProviderHandlesMissingFile();
    void coverImageProviderCancelDoesNotCrash();
    void coverImageProviderUrlForPathFormat();
    void coverImageProviderReadsTheFileOfAStampedId();
    void replacedCoverReloadsForANewStampButNotFromTheCache();
};

void BridgeClientTest::spectrogramActivityFollowsVisibleItemsAndWindows() {
    BridgeClient client;
    isolateBridgeClient(client);
    QQuickWindow first;
    QQuickWindow second;
    QQuickItem parent(first.contentItem());
    SpectrogramItem embedded(&parent);
    SpectrogramItem viewer(second.contentItem());
    client.registerSpectrogramItem(&embedded, 0);
    client.registerSpectrogramItem(&viewer, 0);
    QTRY_VERIFY(!client.m_spectrogramActive);
    first.show();
    QTRY_VERIFY(client.m_spectrogramActive);
    parent.setVisible(false);
    QTRY_VERIFY(!client.m_spectrogramActive);
    second.show();
    QTRY_VERIFY(client.m_spectrogramActive);
    second.setVisibility(QWindow::Minimized);
    QTRY_VERIFY(!client.m_spectrogramActive);
    second.showNormal();
    QTRY_VERIFY(client.m_spectrogramActive);
    viewer.setVisible(false);
    QTRY_VERIFY(!client.m_spectrogramActive);
    parent.setVisible(true);
    QTRY_VERIFY(client.m_spectrogramActive);
    second.hide();
    embedded.setParentItem(second.contentItem());
    QTRY_VERIFY(!client.m_spectrogramActive);
    second.show();
    QTRY_VERIFY(client.m_spectrogramActive);
    client.unregisterSpectrogramItem(&embedded);
    QTRY_VERIFY(!client.m_spectrogramActive);
    client.registerSpectrogramItem(&embedded, 0);
    client.registerSpectrogramItem(&embedded, 1);
    QCOMPARE(client.m_spectrogramRoutes.size(), 2);
    QTRY_VERIFY(client.m_spectrogramActive);
    client.unregisterSpectrogramItem(&embedded);
    QTRY_VERIFY(!client.m_spectrogramActive);
    auto *temporary = new SpectrogramItem(second.contentItem());
    client.registerSpectrogramItem(temporary, 0);
    QTRY_VERIFY(client.m_spectrogramActive);
    delete temporary;
    QTRY_VERIFY(!client.m_spectrogramActive);
    const QByteArray activeCommand = BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetSpectrogramActive, 1);
    QCOMPARE(activeCommand, QByteArray::fromHex("40000100000001"));
}

void BridgeClientTest::tagJobsKeepUiResponsiveAndDiscardClosedLoads() {
    TagEditorController editor(nullptr);
    auto gate = std::make_shared<QSemaphore>();
    auto started = std::make_shared<QSemaphore>();
    editor.m_executeOperation = [gate, started](TagEditorController::Operation, const QByteArray &,
                                                const TagEditorController::RenameGate &) {
        started->release();
        gate->tryAcquire(1, 5000);
        return QByteArray(R"({"resolvedPaths":["/fixture/song.ac3"],"rows":[]})");
    };
    for (const auto operation : {TagEditorController::Operation::Load,
                                TagEditorController::Operation::Save,
                                TagEditorController::Operation::Rename}) {
        bool completed = false;
        editor.runOperation(operation, {}, [&](const QByteArray &) { completed = true; });
        QTRY_VERIFY(started->available() > 0);
        started->acquire();
        bool uiTick = false;
        QTimer::singleShot(0, &editor, [&]() { uiTick = true; });
        QTRY_VERIFY(uiTick);
        QVERIFY(!completed);
        gate->release();
        QTRY_VERIFY(completed);
    }
    QVERIFY(editor.openForPaths({QStringLiteral("/fixture/song.ac3")}));
    QVERIFY(editor.loading());
    editor.close();
    gate->release();
    QTRY_VERIFY(editor.findChildren<QFutureWatcherBase *>().isEmpty());
    QVERIFY(!editor.isOpen());
    QVERIFY(!editor.loading());
    QVERIFY(editor.loadedPaths().isEmpty());
}

void BridgeClientTest::tagEditorAsyncFileRoundTrip() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("original.ac3"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QByteArray(128, '\0')), qint64(128));
    file.close();
    BridgeClient bridge;
    isolateBridgeClient(bridge);
    TagEditorController editor(&bridge);
    QVERIFY(editor.openForPaths({path}));
    QVERIFY(editor.loading());
    QTRY_VERIFY(!editor.loading());
    QCOMPARE(editor.loadedPaths(), QStringList{path});
    editor.setCell(0, QStringLiteral("title"), QStringLiteral("Renamed"));
    editor.setCell(0, QStringLiteral("trackNo"), QStringLiteral("1"));
    QVERIFY(editor.dirty());
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QVERIFY(editor.saving());
    editor.setCell(0, QStringLiteral("title"), QStringLiteral("Must not overwrite"));
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(saved.first().first().toBool());
    QVERIFY(!editor.dirty());
    editor.setSelectedRows({0});
    QSignalSpy renamed(&editor, &TagEditorController::renameFinished);
    QVERIFY(editor.renameSelectedFiles());
    QVERIFY(editor.saving());
    QTRY_COMPARE(renamed.size(), 1);
    QVERIFY2(!QFile::exists(path), qPrintable(editor.statusText() + " " + editor.statusDetails()));
    const QString renamedPath = editor.loadedPaths().first();
    QVERIFY(QFile::exists(renamedPath));
    QVERIFY(renamedPath.contains(QStringLiteral("Renamed")));
    editor.reload();
    QTRY_VERIFY(!editor.loading());
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("Renamed"));
}

void BridgeClientTest::tagEditorRenamesThroughTheBridgeGate() {
    BridgeClient bridge;
    QVERIFY(bridge.m_ffiBridge != nullptr);
    TagEditorController editor(&bridge);
    // Each operation finishes before the next starts, so the worker's writes are visible here.
    auto gated = std::make_shared<QList<bool>>();
    editor.m_executeOperation = [gated](TagEditorController::Operation, const QByteArray &,
                                        const TagEditorController::RenameGate &renameGate) {
        gated->push_back(renameGate != nullptr);
        return QByteArray(R"({"results":[]})");
    };
    const auto run = [&editor](TagEditorController::Operation operation) {
        bool completed = false;
        editor.runOperation(operation, {}, [&](const QByteArray &) { completed = true; });
        return QTest::qWaitFor([&]() { return completed; }, 5000);
    };
    QVERIFY(run(TagEditorController::Operation::Load));
    QVERIFY(run(TagEditorController::Operation::Save));
    QVERIFY(run(TagEditorController::Operation::Rename));
    QCOMPARE(*gated, (QList<bool>{false, false, true}));

    // Without a bridge, renames fall back to the plain rename.
    isolateBridgeClient(bridge);
    QVERIFY(run(TagEditorController::Operation::Rename));
    QCOMPARE(gated->size(), 4);
    QVERIFY(!gated->last());
}

void BridgeClientTest::tagEditorRenameReachesTheBridgeQueue() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QFileInfo(dir.path()).canonicalFilePath() + QStringLiteral("/original.ac3");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(QByteArray(128, '\0')), qint64(128));
    file.close();
    BridgeClient bridge;
    QVERIFY(bridge.m_ffiBridge != nullptr);
    bridge.appendPaths({path});
    QTRY_COMPARE(bridge.queueLength(), 1);
    QCOMPARE(bridge.queuePathAt(0), path);

    TagEditorController editor(&bridge);
    QVERIFY(editor.openForPaths({path}));
    QTRY_VERIFY(!editor.loading());
    editor.setCell(0, QStringLiteral("title"), QStringLiteral("Renamed"));
    editor.setCell(0, QStringLiteral("trackNo"), QStringLiteral("1"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(saved.first().first().toBool());
    editor.setSelectedRows({0});
    QSignalSpy renamed(&editor, &TagEditorController::renameFinished);
    QVERIFY(editor.renameSelectedFiles());
    QTRY_COMPARE(renamed.size(), 1);
    const QString renamedPath = editor.loadedPaths().first();
    QVERIFY2(renamedPath != path, qPrintable(editor.statusText() + " " + editor.statusDetails()));
    QVERIFY(QFile::exists(renamedPath));

    // The bridge learned of the rename without a command from the UI.
    QTRY_COMPARE(bridge.queuePathAt(0), renamedPath);
    QCOMPARE(bridge.queueLength(), 1);
    isolateBridgeClient(bridge);
}

void BridgeClientTest::tagEditorSaveSendsOnlyChangedFields() {
    TagEditorController editor(nullptr);
    QByteArray savePayload;
    editor.m_executeOperation = [&savePayload](TagEditorController::Operation operation,
                                               const QByteArray &payload,
                                               const TagEditorController::RenameGate &) {
        if (operation == TagEditorController::Operation::Save) {
            savePayload = payload;
            return QByteArray(R"({"results":[{"path":"/fixture/song.flac","ok":true}],)"
                              R"("successfulPaths":["/fixture/song.flac"]})");
        }
        return QByteArray(R"({"resolvedPaths":["/fixture/song.flac"],"rows":[{)"
                          R"("path":"/fixture/song.flac","title":"Old","artist":"A",)"
                          R"("year":"1997-05-12","genre":"Rock"}]})");
    };
    QVERIFY(editor.openForPaths({QStringLiteral("/fixture/song.flac")}));
    QTRY_VERIFY(!editor.loading());
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("year")), QStringLiteral("1997-05-12"));

    editor.setCell(0, QStringLiteral("title"), QStringLiteral("New"));
    editor.setCell(0, QStringLiteral("genre"), QStringLiteral("Pop"));
    editor.setCell(0, QStringLiteral("genre"), QStringLiteral("Rock"));
    editor.setCell(0, QStringLiteral("albumArtist"), QStringLiteral("Band"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(saved.first().first().toBool());

    const QJsonArray rows = QJsonDocument::fromJson(savePayload).object().value(QStringLiteral("rows")).toArray();
    QCOMPARE(rows.size(), 1);
    const QJsonObject row = rows.first().toObject();
    QCOMPARE(row.value(QStringLiteral("changedFields")).toArray(),
             QJsonArray({QStringLiteral("title"), QStringLiteral("albumArtist")}));
    QCOMPARE(row.value(QStringLiteral("year")).toString(), QStringLiteral("1997-05-12"));
    QVERIFY(!editor.dirty());
}

void BridgeClientTest::tagEditorSavesFullDateRowAndKeepsOtherTags() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("song.flac"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray flac = flacWithComments({
        QStringLiteral("TITLE=Old"),
        QStringLiteral("ARTIST=A"),
        QStringLiteral("ARTIST=B"),
        QStringLiteral("DATE=1997-05-12"),
        QStringLiteral("ORIGINALDATE=1995"),
        QStringLiteral("CUSTOM=kept"),
    });
    QCOMPARE(file.write(flac), qint64(flac.size()));
    file.close();
    BridgeClient bridge;
    isolateBridgeClient(bridge);
    TagEditorController editor(&bridge);
    QVERIFY(editor.openForPaths({path}));
    QTRY_VERIFY(!editor.loading());
    QCOMPARE(editor.m_tableModel.rowCount(), 1);
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("year")), QStringLiteral("1997-05-12"));

    editor.setCell(0, QStringLiteral("title"), QStringLiteral("New"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY2(saved.first().first().toBool(), qPrintable(editor.statusText() + " " + editor.statusDetails()));
    QVERIFY(!editor.dirty());

    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray written = file.readAll();
    file.close();
    for (const char *expected : {"TITLE=New", "ARTIST=A", "ARTIST=B", "DATE=1997-05-12", "ORIGINALDATE=1995", "CUSTOM=kept"}) {
        QVERIFY2(written.contains(expected), expected);
    }
    QVERIFY(written.indexOf("ARTIST=A") < written.indexOf("ARTIST=B"));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("song.flac")});

    editor.reload();
    QTRY_VERIFY(!editor.loading());
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("New"));
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("year")), QStringLiteral("1997-05-12"));
}

void BridgeClientTest::tagEditorShowsRefusedEditsAndKeepsTheFile() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("song.flac"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray flac = flacWithComments({QStringLiteral("TITLE=Old"), QStringLiteral("TRACKNUMBER=1")});
    QCOMPARE(file.write(flac), qint64(flac.size()));
    file.close();
    TagEditorController editor(nullptr);
    QVERIFY(editor.openForPaths({path}));
    QTRY_VERIFY(!editor.loading());
    QCOMPARE(editor.m_tableModel.rowCount(), 1);

    // A refused value fails the whole row, so the title is not written either.
    editor.setCell(0, QStringLiteral("title"), QStringLiteral("New"));
    editor.setCell(0, QStringLiteral("trackNo"), QStringLiteral("0"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(!saved.first().first().toBool());
    const QString error = QStringLiteral(
        "invalid Track '0' for Vorbis comments: a number must be from 1 to 4294967295; any other label, such as A1 "
        "or Side A, is stored as typed");
    QCOMPARE(editor.m_tableModel.rowAt(0)->errorText, error);
    QVERIFY(editor.dirty());
    QCOMPARE(editor.statusText(), QStringLiteral("Saved 0 file(s); 1 failed."));
    QCOMPARE(editor.statusDetails(), QStringLiteral("Failed:\n  song.flac: ") + error);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), flac);
    file.close();

    // Vorbis comments keep any other track text as typed.
    editor.setCell(0, QStringLiteral("trackNo"), QStringLiteral("A-1"));
    editor.setCell(0, QStringLiteral("year"), QStringLiteral("1997-02-29"));
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 2);
    QVERIFY(!saved.last().first().toBool());
    QCOMPARE(editor.m_tableModel.rowAt(0)->errorText,
             QStringLiteral("invalid year '1997-02-29' for Vorbis comments: February 1997 has no day 29"));

    editor.setCell(0, QStringLiteral("year"), QStringLiteral("1997-05"));
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 3);
    QVERIFY2(saved.last().first().toBool(), qPrintable(editor.statusText() + " " + editor.statusDetails()));
    QVERIFY(!editor.dirty());
    QVERIFY(editor.m_tableModel.rowAt(0)->errorText.isEmpty());
    editor.reload();
    QTRY_VERIFY(!editor.loading());
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("New"));
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("A-1"));
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("year")), QStringLiteral("1997-05"));
}

void BridgeClientTest::tagEditorLoadReportsUnreadableFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString good = dir.filePath(QStringLiteral("good.ac3"));
    const QString garbage = dir.filePath(QStringLiteral("garbage.flac"));
    for (const auto &[path, bytes] : {std::pair{good, QByteArray(128, '\0')},
                                      std::pair{garbage, QByteArray("not audio")}}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
    }
    TagEditorController editor(nullptr);
    QVERIFY(editor.openForPaths({garbage, good}));
    QTRY_VERIFY(!editor.loading());

    QVERIFY(editor.isOpen());
    QCOMPARE(editor.m_tableModel.rowCount(), 1);
    QCOMPARE(editor.m_tableModel.rowAt(0)->path, good);
    QCOMPARE(editor.statusText(), QStringLiteral("Loaded 1 file(s); 1 failed."));
    QVERIFY(editor.statusIsError());
    QVERIFY2(editor.statusDetails().startsWith(QStringLiteral("Failed:\n  garbage.flac: ")),
             qPrintable(editor.statusDetails()));
    QCOMPARE(editor.loadedPaths(), (QStringList{garbage, good}));
}

namespace {

// Rows of `count` tracks, `perFolder` to a folder, at /fixture.
QJsonArray tagEditorRows(int count, int perFolder) {
    QJsonArray rows;
    for (int i = 0; i < count; ++i) {
        const QString directory = QStringLiteral("/fixture/Album %1").arg(i / perFolder);
        rows.push_back(QJsonObject{
            {QStringLiteral("path"), QStringLiteral("%1/%2.flac").arg(directory).arg(i)},
            {QStringLiteral("fileName"), QStringLiteral("%1.flac").arg(i)},
            {QStringLiteral("directory"), directory},
            {QStringLiteral("title"), QStringLiteral("Song %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("A")},
        });
    }
    return rows;
}

// Loads `rows` into `editor`; later operations answer with `responses`, by
// operation, or with an empty response.
bool loadTagEditorRows(TagEditorController &editor, const QJsonArray &rows,
                       QHash<TagEditorController::Operation, QByteArray> responses = {}) {
    QJsonArray paths;
    for (const QJsonValue &row : rows) {
        paths.push_back(row.toObject().value(QStringLiteral("path")));
    }
    responses.insert(TagEditorController::Operation::Load,
                     QJsonDocument(QJsonObject{{QStringLiteral("resolvedPaths"), paths},
                                               {QStringLiteral("rows"), rows}})
                         .toJson(QJsonDocument::Compact));
    editor.m_executeOperation = [responses](TagEditorController::Operation operation,
                                            const QByteArray &, const TagEditorController::RenameGate &) {
        return responses.value(operation);
    };
    if (!editor.openForPaths({QStringLiteral("/fixture")})) {
        return false;
    }
    return QTest::qWaitFor([&editor]() { return !editor.loading(); });
}

QStringList tagEditorColumn(const TagEditorController &editor, const QString &field) {
    QStringList values;
    for (int row = 0; row < editor.m_tableModel.rowCount(); ++row) {
        values.push_back(editor.m_tableModel.fieldValue(row, field));
    }
    return values;
}

} // namespace

void BridgeClientTest::tagEditorAutoNumberRestartsInEachFolder() {
    TagEditorController editor(nullptr);
    QVERIFY(loadTagEditorRows(editor, tagEditorRows(5, 3)));
    QCOMPARE(editor.m_tableModel.fieldValue(3, QStringLiteral("directory")),
             QStringLiteral("/fixture/Album 1"));

    editor.autoNumber(1, 1, false, false, true, false);
    QCOMPARE(tagEditorColumn(editor, QStringLiteral("trackNo")),
             (QStringList{QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
                          QStringLiteral("1"), QStringLiteral("2")}));

    editor.autoNumber(1, 1, false, false, false, false);
    QCOMPARE(tagEditorColumn(editor, QStringLiteral("trackNo")).last(), QStringLiteral("5"));
    // The folder is shown, not edited.
    editor.setCell(0, QStringLiteral("directory"), QStringLiteral("/elsewhere"));
    QCOMPARE(editor.m_tableModel.fieldValue(0, QStringLiteral("directory")),
             QStringLiteral("/fixture/Album 0"));
}

void BridgeClientTest::tagEditorReportsFailuresAsAProperty() {
    // Renames need a bridge to report them to.
    BridgeClient bridge;
    isolateBridgeClient(bridge);
    TagEditorController editor(&bridge);
    QVERIFY(loadTagEditorRows(
        editor, tagEditorRows(1, 1),
        {{TagEditorController::Operation::Save,
          QByteArray(R"({"results":[{"path":"/fixture/Album 0/0.flac","ok":false,"error":"Fehler"}]})")},
         {TagEditorController::Operation::Rename,
          QByteArray(R"({"results":[{"path":"/fixture/Album 0/0.flac","ok":false,"error":"busy"}]})")}}));
    QVERIFY(!editor.statusIsError());
    QSignalSpy status(&editor, &TagEditorController::statusChanged);
    QSignalSpy reported(&editor, &TagEditorController::errorReported);

    // A failure is flagged whatever words describe it.
    editor.setCell(0, QStringLiteral("title"), QStringLiteral("New"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(editor.statusIsError());
    QVERIFY(status.size() > 0);
    // The app's error list hears of it, with the file that failed.
    QCOMPARE(reported.size(), 1);
    QCOMPARE(reported.at(0).at(0).toString(),
             QStringLiteral("Saved 0 file(s); 1 failed.\n0.flac: Fehler"));

    editor.setSelectedRows({0});
    QSignalSpy renamed(&editor, &TagEditorController::renameFinished);
    QVERIFY(editor.renameSelectedFiles());
    QTRY_COMPARE(renamed.size(), 1);
    QVERIFY(editor.statusIsError());
    QCOMPARE(editor.m_tableModel.rowAt(0)->errorText, QStringLiteral("busy"));
    QCOMPARE(reported.size(), 2);
    QCOMPARE(reported.at(1).at(0).toString(),
             QStringLiteral("Renamed 0 file(s); 1 failed.\n0.flac: busy"));

    // A success whose wording mentions errors is not a failure.
    TagEditorController succeeding(nullptr);
    QJsonArray rows = tagEditorRows(1, 1);
    QJsonObject row = rows.first().toObject();
    row.insert(QStringLiteral("path"), QStringLiteral("/fixture/error failed.flac"));
    rows[0] = row;
    QVERIFY(loadTagEditorRows(
        succeeding, rows,
        {{TagEditorController::Operation::Save,
          QByteArray(R"({"results":[{"path":"/fixture/error failed.flac","ok":true}]})")}}));
    succeeding.setCell(0, QStringLiteral("title"), QStringLiteral("New"));
    QSignalSpy succeeded(&succeeding, &TagEditorController::saveFinished);
    QSignalSpy succeededReports(&succeeding, &TagEditorController::errorReported);
    QVERIFY(succeeding.save());
    QTRY_COMPARE(succeeded.size(), 1);
    QVERIFY(succeeded.first().first().toBool());
    QVERIFY(succeeding.statusDetails().contains(QStringLiteral("error failed.flac")));
    QVERIFY(!succeeding.statusIsError());
    QCOMPARE(succeededReports.size(), 0);
}

void BridgeClientTest::tagEditorKeepsExtraValuesUntilTheirFieldIsSaved() {
    QJsonArray rows = tagEditorRows(2, 2);
    QJsonObject first = rows.first().toObject();
    first.insert(QStringLiteral("extraValues"),
                 QJsonObject{{QStringLiteral("artist"), QJsonArray{QStringLiteral("B")}},
                             {QStringLiteral("genre"), QJsonArray{QStringLiteral("Jazz")}}});
    rows[0] = first;
    TagEditorController editor(nullptr);
    QVERIFY(loadTagEditorRows(
        editor, rows,
        {{TagEditorController::Operation::Save,
          QByteArray(R"({"results":[{"path":"/fixture/Album 0/0.flac","ok":true}]})")}}));
    const auto extras = [&editor]() {
        return editor.m_tableModel.data(editor.m_tableModel.index(0),
                                        TagEditorTableModel::ExtraValuesRole).toMap();
    };
    QCOMPARE(extras().value(QStringLiteral("artist")).toStringList(), QStringList{QStringLiteral("B")});

    // Both rows are edited when none is selected; one of them has more artists.
    QCOMPARE(editor.extraValuesNote(QStringLiteral("artist")),
             QStringLiteral("1 of these files have more than one value. Editing replaces them all "
                            "with the one above."));
    QVERIFY(editor.extraValuesNote(QStringLiteral("title")).isEmpty());
    editor.setSelectedRows({0});
    QCOMPARE(editor.extraValuesNote(QStringLiteral("artist")),
             QStringLiteral("Also: B. Editing replaces every value with the one above."));

    // A changed artist will replace them, so they are not shown; changing it
    // back shows them again.
    editor.setCell(0, QStringLiteral("artist"), QStringLiteral("C"));
    QVERIFY(!extras().contains(QStringLiteral("artist")));
    QVERIFY(extras().contains(QStringLiteral("genre")));
    QVERIFY(editor.extraValuesNote(QStringLiteral("artist")).isEmpty());
    editor.setCell(0, QStringLiteral("artist"), QStringLiteral("A"));
    QVERIFY(extras().contains(QStringLiteral("artist")));

    // Once saved, the file holds one artist.
    editor.setCell(0, QStringLiteral("artist"), QStringLiteral("C"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(saved.first().first().toBool());
    QVERIFY(!editor.m_tableModel.rowAt(0)->extraValues.contains(QStringLiteral("artist")));
    QVERIFY(editor.m_tableModel.rowAt(0)->extraValues.contains(QStringLiteral("genre")));
    editor.setCell(0, QStringLiteral("artist"), QStringLiteral("A"));
    QVERIFY(!extras().contains(QStringLiteral("artist")));
}

void BridgeClientTest::tagEditorSelectionIsAModelRole() {
    TagEditorController editor(nullptr);
    QVERIFY(loadTagEditorRows(editor, tagEditorRows(6, 6)));
    TagEditorTableModel &model = editor.m_tableModel;
    const auto selected = [&model]() {
        QList<int> rows;
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row), TagEditorTableModel::SelectedRole).toBool()) {
                rows.push_back(row);
            }
        }
        return rows;
    };
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy selection(&editor, &TagEditorController::selectionChanged);

    editor.selectRange(4, 1);
    QCOMPARE(selected(), (QList<int>{1, 2, 3, 4}));
    QCOMPARE(editor.selectedCount(), 4);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(changed.first().at(2).value<QList<int>>(), QList<int>{TagEditorTableModel::SelectedRole});

    editor.toggleRowSelected(2);
    QCOMPARE(selected(), (QList<int>{1, 3, 4}));
    editor.toggleRowSelected(5);
    QCOMPARE(selected(), (QList<int>{1, 3, 4, 5}));

    // Selecting everything notifies the rows that change, in runs.
    changed.clear();
    editor.selectAllRows();
    QCOMPARE(selected(), (QList<int>{0, 1, 2, 3, 4, 5}));
    QCOMPARE(changed.size(), 2);
    QCOMPARE(editor.selectedCount(), 6);

    editor.setSelectedRows({});
    QVERIFY(selected().isEmpty());
    QCOMPARE(selection.size(), 5);
    editor.close();
    QCOMPARE(editor.selectedCount(), 0);
}

void BridgeClientTest::tagEditorEditsOfManyRowsAnnounceOnce() {
    constexpr int kRows = 2000;
    TagEditorController editor(nullptr);
    QVERIFY(loadTagEditorRows(editor, tagEditorRows(kRows, 10)));
    QVariantList rows;
    for (int row = 0; row < kRows; ++row) {
        rows.push_back(row);
    }
    QSignalSpy summary(&editor, &TagEditorController::bulkSummaryChanged);
    QSignalSpy dirty(&editor, &TagEditorController::dirtyChanged);

    editor.applyBulkFieldToRows(rows, QStringLiteral("album"), QStringLiteral("One"));

    // Each announcement makes every field editor summarize the edited rows.
    QCOMPARE(summary.size(), 1);
    QCOMPARE(dirty.size(), 1);
    QVERIFY(editor.dirty());
    QCOMPARE(editor.bulkValue(QStringLiteral("album")), QStringLiteral("One"));
}

void BridgeClientTest::tagEditorRenameKeepsRowsInPlace() {
    BridgeClient bridge;
    isolateBridgeClient(bridge);
    TagEditorController editor(&bridge);
    QVERIFY(loadTagEditorRows(
        editor, tagEditorRows(3, 3),
        {{TagEditorController::Operation::Rename,
          QByteArray(R"({"results":[{"path":"/fixture/Album 0/1.flac","ok":true,)"
                     R"("newPath":"/fixture/Album 0/01 Song 1.flac"}]})")}}));
    editor.setSelectedRows({1});
    QSignalSpy reset(&editor.m_tableModel, &QAbstractItemModel::modelReset);
    QSignalSpy changed(&editor.m_tableModel, &QAbstractItemModel::dataChanged);
    QSignalSpy renamed(&editor, &TagEditorController::renameFinished);

    QVERIFY(editor.renameSelectedFiles());
    QTRY_COMPARE(renamed.size(), 1);

    QCOMPARE(reset.size(), 0);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(editor.m_tableModel.rowAt(1)->fileName, QStringLiteral("01 Song 1.flac"));
    QCOMPARE(editor.loadedPaths().at(1), QStringLiteral("/fixture/Album 0/01 Song 1.flac"));
    QCOMPARE(editor.selectedRows(), QVariantList{1});
    QVERIFY(editor.m_tableModel.rowAt(1)->selected);
    QVERIFY(!editor.statusIsError());
}

void BridgeClientTest::aRenamePreviewOfRowsLoadedAgainIsOutOfDate() {
    BridgeClient bridge;
    isolateBridgeClient(bridge);
    TagEditorController editor(&bridge);
    QVERIFY(loadTagEditorRows(
        editor, tagEditorRows(2, 2),
        {{TagEditorController::Operation::PreviewRename,
          QByteArray(R"({"results":[{"path":"/fixture/Album 0/1.flac","ok":true,)"
                     R"("newPath":"/fixture/Album 0/01 Song 1.flac"}]})")},
         {TagEditorController::Operation::Rename, QByteArray(R"({"results":[]})")}}));
    editor.setSelectedRows({0});
    QSignalSpy previewed(&editor, &TagEditorController::renamePreviewReady);
    QVERIFY(editor.previewRenameSelectedFiles());
    QTRY_COMPARE(previewed.size(), 1);

    // The files are read again: what the preview showed may be gone.
    editor.reload();
    QTRY_VERIFY(!editor.loading());
    QSignalSpy renamed(&editor, &TagEditorController::renameFinished);
    QVERIFY(!editor.renamePreviewedFiles());
    QVERIFY(!editor.saving());
    QVERIFY(editor.statusIsError());
    QVERIFY2(editor.statusText().contains(QStringLiteral("out of date")),
             qPrintable(editor.statusText()));
    QCOMPARE(renamed.size(), 0);

    // A fresh preview can be renamed, once.
    editor.setSelectedRows({0});
    QVERIFY(editor.previewRenameSelectedFiles());
    QTRY_COMPARE(previewed.size(), 2);
    QVERIFY(editor.renamePreviewedFiles());
    QTRY_COMPARE(renamed.size(), 1);
    QVERIFY(!editor.renamePreviewedFiles());
}

void BridgeClientTest::tagEditorExitDoesNotWaitForAStalledSave() {
    auto stalled = std::make_shared<QSemaphore>();
    auto finished = std::make_shared<QSemaphore>();
    auto editor = std::make_unique<TagEditorController>(nullptr);
    QVERIFY(loadTagEditorRows(*editor, tagEditorRows(1, 1)));
    editor->m_executeOperation = [stalled, finished](TagEditorController::Operation, const QByteArray &,
                                                     const TagEditorController::RenameGate &) {
        // Storage that does not answer, for longer than any exit may take.
        stalled->tryAcquire(1, 30000);
        finished->release();
        return QByteArray();
    };
    editor->setCell(0, QStringLiteral("title"), QStringLiteral("New"));
    QVERIFY(editor->save());
    QCOMPARE(editor->operationFiles(), QStringList{QStringLiteral("0.flac")});

    // The application waits for its global pool at exit however long its
    // work takes, so the save must not run there.
    QTest::qWait(50);
    QCOMPARE(QThreadPool::globalInstance()->activeThreadCount(), 0);

    // Destruction returns while the save is still stalled, without waiting
    // for it to end, rather than within a time a busy machine might miss.
    editor.reset();
    QCOMPARE(finished->available(), 0);

    stalled->release();
    QVERIFY(finished->tryAcquire(1, 5000));
}

void BridgeClientTest::batchRemovalCodecPreservesIndices() {
    const QByteArray bytes = BinaryBridgeCodec::encodeCommandRemoveMany(5, {9, 5, 1});
    QCOMPARE(bytes.size(), 30);
    QCOMPARE(qFromLittleEndian<quint16>(bytes.constData()), quint16(63));
    QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 2), quint32(24));
    QCOMPARE(qFromLittleEndian<quint64>(bytes.constData() + 6), quint64(5));
    QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 14), quint32(3));
    QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 18), quint32(9));
    QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 22), quint32(5));
    QCOMPARE(qFromLittleEndian<quint32>(bytes.constData() + 26), quint32(1));
}

namespace {

// Reads a path list command as the Rust decoder does: a u16 id, a u32
// payload length, a u32 count, then u32-length UTF-8 strings.
bool decodePathListCommand(const QByteArray &command, quint16 *cmdId, QStringList *paths) {
    if (command.size() < BinaryBridgeCodec::kCommandHeaderBytes + 4) {
        return false;
    }
    const auto *bytes = reinterpret_cast<const uchar *>(command.constData());
    *cmdId = qFromLittleEndian<quint16>(bytes);
    if (qFromLittleEndian<quint32>(bytes + 2) != command.size() - BinaryBridgeCodec::kCommandHeaderBytes) {
        return false;
    }
    qsizetype offset = BinaryBridgeCodec::kCommandHeaderBytes;
    const quint32 count = qFromLittleEndian<quint32>(bytes + offset);
    offset += 4;
    paths->clear();
    for (quint32 i = 0; i < count; ++i) {
        if (offset + 4 > command.size()) {
            return false;
        }
        const quint32 len = qFromLittleEndian<quint32>(bytes + offset);
        offset += 4;
        if (offset + len > command.size()) {
            return false;
        }
        paths->push_back(QString::fromUtf8(command.constData() + offset, len));
        offset += len;
    }
    return offset == command.size();
}

// 65,534 ASCII bytes, then characters of 2, 3, and 4 bytes that straddle
// the 65,535 bytes a u16 length could hold.
QString stringPastTheOldLengthLimit() {
    return QString(65534, QLatin1Char('a')) + QStringLiteral("é€😀tail");
}

} // namespace

void BridgeClientTest::libraryViewStateKeepsOffsetsIntoTallRows() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(payload);
        return true;
    };

    // Rows follow the font size, so the view's top can be well over 24
    // pixels into the row at it.
    client.setLibraryViewState(QStringLiteral("sel"), QStringLiteral("anchor"), 40.0);
    client.setLibraryViewState(QStringLiteral("sel"), QStringLiteral("anchor"), -40.0);
    client.setLibraryViewState(QStringLiteral("sel"), QStringLiteral("anchor"), 1e9);
    QCOMPARE(sent.size(), 3);
    const float expected[] = {40.0F, -40.0F, 4096.0F};
    for (int i = 0; i < sent.size(); ++i) {
        QCOMPARE(sent[i], BinaryBridgeCodec::encodeCommandLibraryViewState(
            BinaryBridgeCodec::CmdSetLibraryViewState,
            QStringLiteral("sel"),
            QStringLiteral("anchor"),
            expected[i]));
    }
}

void BridgeClientTest::pathListsFarBeyond64KiBAreSentWholeInOneCommand() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(payload);
        return true;
    };
    QSignalSpy errors(&client, &BridgeClient::bridgeError);
    // Reverse order, so a sort anywhere on the way would show.
    QStringList paths;
    for (int i = 4999; i >= 0; --i) {
        paths.push_back(QStringLiteral("/fixture/Ärtist %1/トラック – %1.flac")
                            .arg(i, 4, 10, QLatin1Char('0')));
    }

    // Opening files from outside the app, dropping them, and saving tags.
    client.replaceWithPaths(paths);
    client.appendPaths(paths);
    client.refreshEditedPaths(paths);

    QVERIFY(errors.isEmpty());
    QCOMPARE(sent.size(), 3);
    const quint16 expectedIds[] = {
        BinaryBridgeCodec::CmdReplaceAlbum,
        BinaryBridgeCodec::CmdAppendAlbum,
        BinaryBridgeCodec::CmdRefreshEditedPaths,
    };
    for (int i = 0; i < sent.size(); ++i) {
        QVERIFY(sent[i].size() > 3 * 65535);
        quint16 cmdId = 0;
        QStringList decoded;
        QVERIFY(decodePathListCommand(sent[i], &cmdId, &decoded));
        QCOMPARE(cmdId, expectedIds[i]);
        QCOMPARE(decoded.size(), 5000);
        QCOMPARE(decoded, paths);
    }
}

void BridgeClientTest::thousandsOfAppendedPathsReachTheBackendInOrder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    QStringList paths;
    for (int i = 4999; i >= 0; --i) {
        const QString path = base
            + QStringLiteral("/%1 – Ärtist トラック.flac").arg(i, 4, 10, QLatin1Char('0'));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("stub"), qint64(4));
        paths.push_back(path);
    }
    BridgeClient bridge;
    QVERIFY(bridge.m_ffiBridge != nullptr);
    QSignalSpy errors(&bridge, &BridgeClient::bridgeError);

    // Appending starts no playback, so the real backend can take the list.
    // CTest runs these tests with a fake audio output (ui/CMakeLists.txt),
    // so the bridge reports no missing audio device here.
    // The queue an earlier test left behind is cleared first; commands
    // apply in order.
    bridge.clearQueue();
    bridge.appendPaths(paths);
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 5000, 30000);
    for (int i = 0; i < paths.size(); ++i) {
        QCOMPARE(bridge.queuePathAt(i), paths[i]);
    }
    QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().first().toString()));
    // Leave no queue of removed fixtures to the tests after this one.
    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
    isolateBridgeClient(bridge);
}

void BridgeClientTest::commandsOverTheBoundAreRefusedVisibly() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(payload);
        return true;
    };
    QSignalSpy errors(&client, &BridgeClient::bridgeError);
    // About 10 MiB of paths, over the 8 MiB bound.
    QStringList paths;
    for (int i = 0; i < 2500; ++i) {
        paths.push_back(QStringLiteral("/fixture/%1/%2.flac").arg(QString(4000, QLatin1Char('d'))).arg(i));
    }

    client.replaceWithPaths(paths);
    QCOMPARE(errors.size(), 1);
    QString message = errors.takeFirst().first().toString();
    QVERIFY2(message.contains(QStringLiteral("2500 files")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("8 MiB")), qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("queue was not changed")), qPrintable(message));
    client.appendPaths(paths);
    QCOMPARE(errors.size(), 1);
    message = errors.takeFirst().first().toString();
    QVERIFY2(message.contains(QStringLiteral("queue was not changed")), qPrintable(message));
    client.refreshEditedPaths(paths);
    QCOMPARE(errors.size(), 1);
    message = errors.takeFirst().first().toString();
    QVERIFY2(message.contains(QStringLiteral("Rescan the library")), qPrintable(message));
    QVERIFY(sent.isEmpty());

    // A string over the bound is refused whole, never cut, and a refused
    // command fails and says why.
    const qsizetype maxString = BinaryBridgeCodec::kMaxCommandPayloadBytes - 4;
    QVERIFY(BinaryBridgeCodec::encodeCommandString(
                BinaryBridgeCodec::CmdPlayTrack, QString(maxString + 1, QLatin1Char('x')))
                .isEmpty());
    QVERIFY(!client.sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(
        BinaryBridgeCodec::CmdPlayTrack, QString(maxString + 1, QLatin1Char('x')))));
    QCOMPARE(errors.size(), 1);
    message = errors.takeFirst().first().toString();
    QVERIFY2(message.contains(QStringLiteral("8 MiB")), qPrintable(message));
    QVERIFY(sent.isEmpty());

    // A command at the bound is sent.
    const QByteArray atBound = BinaryBridgeCodec::encodeCommandString(
        BinaryBridgeCodec::CmdPlayTrack, QString(maxString, QLatin1Char('x')));
    QCOMPARE(atBound.size(),
             BinaryBridgeCodec::kCommandHeaderBytes + qsizetype(BinaryBridgeCodec::kMaxCommandPayloadBytes));
    QVERIFY(client.sendBinaryCommand(atBound));
    QCOMPARE(sent.size(), 1);
    QVERIFY(errors.isEmpty());
}

void BridgeClientTest::stringsPastTheOldLengthLimitCrossWhole() {
    // Mirrored by ffi::tests::strings_past_the_old_length_limit_cross_the_bridge_whole_both_ways.
    const QString text = stringPastTheOldLengthLimit();
    const QByteArray utf8 = text.toUtf8();
    QVERIFY(utf8.size() > 65535);

    // To Rust: the command carries every byte, with a u32 length.
    const QByteArray command =
        BinaryBridgeCodec::encodeCommandString(BinaryBridgeCodec::CmdPlayTrack, text);
    const auto *bytes = reinterpret_cast<const uchar *>(command.constData());
    QCOMPARE(command.size(), BinaryBridgeCodec::kCommandHeaderBytes + 4 + utf8.size());
    QCOMPARE(qFromLittleEndian<quint16>(bytes), quint16(BinaryBridgeCodec::CmdPlayTrack));
    QCOMPARE(qFromLittleEndian<quint32>(bytes + 2), quint32(4 + utf8.size()));
    QCOMPARE(qFromLittleEndian<quint32>(bytes + 6), quint32(utf8.size()));
    QCOMPARE(command.mid(10), utf8);

    // From Rust: an error packet with the same string decodes whole.
    QByteArray payload;
    QDataStream payloadStream(&payload, QIODevice::WriteOnly);
    payloadStream.setByteOrder(QDataStream::LittleEndian);
    // A system error with no subject, then the message.
    payloadStream << quint8(0) << quint8(1) << quint32(0) << quint32(utf8.size());
    payload.append(utf8);
    const auto packetWith = [](const QByteArray &section) {
        QByteArray packet;
        QDataStream header(&packet, QIODevice::WriteOnly);
        header.setByteOrder(QDataStream::LittleEndian);
        header << quint32(0xFE550001u) << quint32(16 + section.size())
               << quint16(BinaryBridgeCodec::SectionError) << quint16(0)
               << quint32(section.size());
        packet.append(section);
        return packet;
    };
    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(packetWith(payload), &decoded, &error),
             qPrintable(error));
    QCOMPARE(decoded.errorMessage, text);

    // A length past the end of the section is refused.
    QByteArray corrupt = payload;
    qToLittleEndian<quint32>(quint32(utf8.size() + 1), corrupt.data() + 6);
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(packetWith(corrupt), &decoded, &error));
    qToLittleEndian<quint32>(std::numeric_limits<quint32>::max(), corrupt.data() + 6);
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(packetWith(corrupt), &decoded, &error));
}

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

void BridgeClientTest::playAtKeepsTheClockUntilTheEngineSwitches() {
    BridgeClient client;
    isolateBridgeClient(client);
    acceptCommands(client);
    const QString path = QStringLiteral("/music/track.flac");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 120.0, 1)));
    const quint64 epoch = client.playbackClock()->state().epoch;

    // Playing the current track again: until the engine switches, its
    // snapshots describe the run that still plays.
    QSignalSpy playbackSpy(&client, &BridgeClient::playbackChanged);
    client.playAt(0);
    QCoreApplication::processEvents();
    QCOMPARE(playbackSpy.count(), 0);
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 120.1, 1)));
    QVERIFY(std::abs(clockPosition(client) - 120.1) < 0.2);
    QCOMPARE(client.playbackClock()->state().epoch, epoch);

    // The new run has a new track token, and starts over.
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 0.02, 2)));
    QVERIFY(clockPosition(client) < 0.2);
    QVERIFY(client.playbackClock()->state().epoch > epoch);
    QCOMPARE(client.playbackClock()->state().trackToken, quint64{2});
}

void BridgeClientTest::trackSwitchReanchorsBeforeSpectrogramData_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<int>("action");
    for (int mode = 0; mode < 2; ++mode) {
        for (int action = 0; action < 9; ++action) {
            QTest::newRow(qPrintable(QStringLiteral("mode-%1-action-%2").arg(mode).arg(action)))
                << mode << action;
        }
    }
}

void BridgeClientTest::trackSwitchReanchorsBeforeSpectrogramData() {
    QFETCH(int, mode);
    QFETCH(int, action);
    BridgeClient client;
    isolateBridgeClient(client);
    const QString oldPath = QStringLiteral("/fixture/old.wav");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(oldPath, 120.0, 1)));
    client.m_pendingSeek = true;
    client.m_pendingSeekTargetSeconds = 90.0;
    client.m_pendingSeekSerial = 1;
    client.playbackClock()->holdAt(90.0);

    SpectrogramItem item;
    item.setDisplayMode(mode);
    feedTrackData(item, 1);
    item.m_precomputedReady = true;
    item.setPlaying(true);
    item.setClock(client.playbackClock());
    client.registerSpectrogramItem(&item, 0);

    const QString newPath = QStringLiteral("/fixture/new.wav");
    switch (action) {
    case 0: client.next(); break;
    case 1: client.previous(); break;
    case 2: client.playTrack(newPath); break;
    case 3: client.replaceWithPaths({newPath}); break;
    case 4: client.replaceAlbumByKey(QStringLiteral("artist"), QStringLiteral("album")); break;
    case 5: client.replaceArtistByName(QStringLiteral("artist")); break;
    case 6: client.replaceRootByPath(QStringLiteral("/fixture")); break;
    case 7: client.replaceAllLibraryTracks(); break;
    case 8: client.playAt(1); break;
    }
    // The pending seek still holds the clock until the engine switches.
    QCOMPARE(clockPosition(client), 90.0);
    const quint64 epoch = client.playbackClock()->state().epoch;
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(newPath, 0.05, 2)));

    QVERIFY(!client.m_pendingSeek);
    QVERIFY(clockPosition(client) < 0.3);
    QVERIFY(client.playbackClock()->state().epoch > epoch);
    if (mode == 1) {
        // Centered views follow a new track before its data arrives.
        QVERIFY(!item.m_positionJumpHoldActive);
        QVERIFY(item.m_positionSeconds < 0.3);
    } else {
        // Rolling history remains continuous until the worker resets it.
        QVERIFY(item.m_positionJumpHoldActive);
        QVERIFY(item.m_positionSeconds >= 90.0);
        feedTrackData(item, 2);
        QVERIFY(!item.m_positionJumpHoldActive);
        QVERIFY(item.m_positionSeconds < 0.3);
    }
}

void BridgeClientTest::sameFileReplayReanchorsByTrackToken_data() {
    QTest::addColumn<int>("mode");
    QTest::newRow("rolling") << 0;
    QTest::newRow("centered") << 1;
}

void BridgeClientTest::sameFileReplayReanchorsByTrackToken() {
    // Repeat-one and a file queued twice start the same path again without
    // a user command. Only the track token tells the new play apart.
    QFETCH(int, mode);
    BridgeClient client;
    isolateBridgeClient(client);
    const QString path = QStringLiteral("/fixture/track.wav");
    auto snapshot = playbackSnapshot(path, 179.9, 4);
    snapshot.playback.currentQueueIndex = 0;
    QVERIFY(client.processBinarySnapshot(snapshot));

    SpectrogramItem item;
    item.setDisplayMode(mode);
    feedTrackData(item, 4);
    item.m_precomputedReady = true;
    item.setPlaying(true);
    item.setClock(client.playbackClock());
    client.registerSpectrogramItem(&item, 0);

    const quint64 epoch = client.playbackClock()->state().epoch;
    snapshot = playbackSnapshot(path, 179.95, 4);
    snapshot.playback.currentQueueIndex = 0;
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.playbackClock()->state().epoch, epoch);
    QVERIFY(!item.m_positionJumpHoldActive);

    snapshot = playbackSnapshot(path, 0.05, 5);
    snapshot.playback.currentQueueIndex = 1;
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.playbackClock()->state().epoch, epoch + 1);
    QCOMPARE(client.m_currentTrackToken, quint64{5});
    QCOMPARE(client.m_playingQueueIndex, 1);
    QVERIFY(clockPosition(client) < 0.3);
    if (mode == 1) {
        QVERIFY(!item.m_positionJumpHoldActive);
        QVERIFY(item.m_positionSeconds < 0.3);
    } else {
        // Rolling history remains continuous until the worker resets it.
        QVERIFY(item.m_positionJumpHoldActive);
    }
}

void BridgeClientTest::noOpNextPreservesPosition() {
    BridgeClient client;
    isolateBridgeClient(client);
    const QString path = QStringLiteral("/fixture/last.wav");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 120.0, 3)));
    const quint64 epoch = client.playbackClock()->state().epoch;
    client.next();
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 120.1, 3)));
    QVERIFY(std::abs(clockPosition(client) - 120.1) < 0.2);
    QCOMPARE(client.playbackClock()->state().epoch, epoch);
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
    QCOMPARE(client.queuePathFirstIndex(duplicatePath), 0);

    BinaryBridgeCodec::DecodedSnapshot playbackSnapshot;
    playbackSnapshot.playback.present = true;
    playbackSnapshot.playback.state = 1;
    playbackSnapshot.playback.currentQueueIndex = -1;
    playbackSnapshot.playback.currentPath = duplicatePath;

    QVERIFY(client.processBinarySnapshot(playbackSnapshot));
    QCOMPARE(client.m_playingQueueIndex, 0);
    QCOMPARE(client.currentTrackTitle(), QStringLiteral("Duplicate First"));
}

namespace {

BinaryBridgeCodec::DecodedQueueTrack queueTrack(const QString &path, const QString &title = {}) {
    BinaryBridgeCodec::DecodedQueueTrack track;
    track.title = title.isEmpty() ? QFileInfo(path).fileName() : title;
    track.path = path;
    track.lengthSeconds = 60.0f;
    return track;
}

// A snapshot with the whole queue of `paths` at `revision`.
BinaryBridgeCodec::DecodedSnapshot fullQueueSnapshot(
    quint64 revision, const QStringList &paths, int selected = -1) {
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.queue.present = true;
    snapshot.queue.kind = BinaryBridgeCodec::QueueFull;
    snapshot.queue.revision = revision;
    snapshot.queue.len = static_cast<int>(paths.size());
    snapshot.queue.selectedIndex = selected;
    for (const QString &path : paths) {
        snapshot.queue.tracks.push_back(queueTrack(path));
    }
    return snapshot;
}

// A snapshot with `ops` from `base` to `revision`, which leave `len` rows.
BinaryBridgeCodec::DecodedSnapshot queueDeltaSnapshot(
    quint64 base,
    quint64 revision,
    int len,
    QVector<BinaryBridgeCodec::DecodedQueueOp> ops,
    int selected = -1) {
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.queue.present = true;
    snapshot.queue.kind = BinaryBridgeCodec::QueueDelta;
    snapshot.queue.baseRevision = base;
    snapshot.queue.revision = revision;
    snapshot.queue.len = len;
    snapshot.queue.selectedIndex = selected;
    snapshot.queue.ops = std::move(ops);
    return snapshot;
}

BinaryBridgeCodec::DecodedQueueOp queueOp(
    int type, int index, int countOrTo = 0, QVector<BinaryBridgeCodec::DecodedQueueTrack> tracks = {}) {
    BinaryBridgeCodec::DecodedQueueOp op;
    op.type = type;
    op.index = index;
    if (type == BinaryBridgeCodec::QueueOpRemove) {
        op.count = countOrTo;
    } else if (type == BinaryBridgeCodec::QueueOpMove) {
        op.to = countOrTo;
    }
    op.tracks = std::move(tracks);
    return op;
}

QStringList queuePaths(const BridgeClient &client) {
    QStringList paths;
    for (int index = 0; index < client.queueLength(); ++index) {
        paths.push_back(client.queuePathAt(index));
    }
    return paths;
}

QStringList numberedPaths(int count) {
    QStringList paths;
    for (int index = 0; index < count; ++index) {
        paths.push_back(QStringLiteral("/music/%1.flac").arg(index));
    }
    return paths;
}

// Counts the row signals of the client's queue model.
struct QueueRowSignals {
    explicit QueueRowSignals(QAbstractItemModel *model)
        : resets(model, &QAbstractItemModel::modelReset),
          inserts(model, &QAbstractItemModel::rowsInserted),
          removes(model, &QAbstractItemModel::rowsRemoved),
          moves(model, &QAbstractItemModel::rowsMoved),
          updates(model, &QAbstractItemModel::dataChanged) {}

    QSignalSpy resets;
    QSignalSpy inserts;
    QSignalSpy removes;
    QSignalSpy moves;
    QSignalSpy updates;
};

// Little-endian writers for building packets as the Rust relay does.
template <typename T>
void putLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<qsizetype>(sizeof(T)));
}

void putString(QByteArray &out, const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    putLe<quint32>(out, static_cast<quint32>(utf8.size()));
    out.append(utf8);
}

void putQueueRow(QByteArray &out, const QString &path, const QString &title) {
    putString(out, title);
    // Artist, album and cover path, the cover stamp, and genre.
    for (int i = 0; i < 3; ++i) {
        putString(out, QString{});
    }
    putLe<quint64>(out, 0);
    putString(out, QString{});
    putLe<qint32>(out, std::numeric_limits<qint32>::min());
    putLe<quint16>(out, 0);
    const float seconds = 60.0f;
    quint32 bits = 0;
    std::memcpy(&bits, &seconds, sizeof(bits));
    putLe<quint32>(out, bits);
    putString(out, path);
}

void putQueueHeader(QByteArray &out, quint8 kind, quint64 revision, quint32 len) {
    putLe<quint8>(out, kind);
    putLe<quint64>(out, revision);
    putLe<quint32>(out, len);
    putLe<qint32>(out, -1);
    const double total = 60.0 * len;
    quint64 bits = 0;
    std::memcpy(&bits, &total, sizeof(bits));
    putLe<quint64>(out, bits);
    putLe<quint32>(out, 0);
}

QByteArray queuePacket(const QByteArray &section) {
    QByteArray packet;
    putLe<quint32>(packet, BinaryBridgeCodec::kSnapshotMagic);
    putLe<quint32>(packet, static_cast<quint32>(12 + 4 + section.size()));
    putLe<quint16>(packet, BinaryBridgeCodec::SectionQueue);
    putLe<quint16>(packet, 0);
    putLe<quint32>(packet, static_cast<quint32>(section.size()));
    packet.append(section);
    return packet;
}

} // namespace

void BridgeClientTest::queueUndoSectionIsDecoded() {
    // Mirrored by queue_undo_section_layout_matches_the_qt_decoder in
    // src/frontend_bridge/ffi.rs.
    QByteArray section;
    putLe<quint64>(section, 7);
    putLe<quint8>(section, BinaryBridgeCodec::QueueUndoRemoved);
    putLe<quint32>(section, 3);
    putLe<quint64>(section, 5);
    putLe<quint64>(section, 9);
    putLe<quint32>(section, 2);
    for (quint32 value : {1u, 2u, 6u, 1u}) {
        putLe<quint32>(section, value);
    }
    const auto packet = [](const QByteArray &payload) {
        QByteArray out;
        putLe<quint32>(out, BinaryBridgeCodec::kSnapshotMagic);
        putLe<quint32>(out, static_cast<quint32>(12 + 4 + payload.size()));
        putLe<quint16>(out, BinaryBridgeCodec::SectionQueueUndo);
        putLe<quint16>(out, 0);
        putLe<quint32>(out, static_cast<quint32>(payload.size()));
        out.append(payload);
        return out;
    };
    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(packet(section), &decoded, &error),
             qPrintable(error));
    QVERIFY(decoded.queueUndo.present);
    QCOMPARE(decoded.queueUndo.offerId, quint64(7));
    QCOMPARE(decoded.queueUndo.kind, int(BinaryBridgeCodec::QueueUndoRemoved));
    QCOMPARE(decoded.queueUndo.trackCount, 3);
    QCOMPARE(decoded.queueUndo.restoredId, quint64(5));
    QCOMPARE(decoded.queueUndo.restoredRevision, quint64(9));
    QCOMPARE(decoded.queueUndo.restoredRanges,
             (QVector<QPair<int, int>>{{1, 2}, {6, 1}}));
    QVERIFY(BinaryBridgeCodec::decodeSnapshotPacket(packet(QByteArray(33, '\0')), &decoded, &error));
    QCOMPARE(decoded.queueUndo.offerId, quint64(0));

    // An unknown kind, a range count past the payload, and trailing bytes
    // are refused.
    QByteArray unknownKind = section;
    unknownKind[8] = 4;
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(packet(unknownKind), &decoded, &error));
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(packet(section.left(section.size() - 4)),
                                                     &decoded, &error));
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(packet(section + QByteArray(1, '\0')),
                                                     &decoded, &error));
}

void BridgeClientTest::undoSendsTheOfferedChangeAndSelectsTheRestoredRowsOnce() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(payload);
        return true;
    };
    QSignalSpy changed(&client, &BridgeClient::queueUndoChanged);
    QSignalSpy restored(&client, &BridgeClient::queueUndoRestored);

    // Nothing is offered: nothing is sent.
    client.undoQueueChange();
    QVERIFY(sent.isEmpty());

    auto snapshot = fullQueueSnapshot(4, numberedPaths(3));
    snapshot.queueUndo.present = true;
    snapshot.queueUndo.offerId = 11;
    snapshot.queueUndo.kind = BinaryBridgeCodec::QueueUndoRemoved;
    snapshot.queueUndo.trackCount = 2;
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(client.queueUndoId(), 11.0);
    QCOMPARE(client.queueUndoKind(), int(BinaryBridgeCodec::QueueUndoRemoved));
    QCOMPARE(client.queueUndoTrackCount(), 2);
    client.undoQueueChange();
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent[0], BinaryBridgeCodec::encodeCommandU64(BinaryBridgeCodec::CmdUndoQueueChange, 11));
    QCOMPARE(qFromLittleEndian<quint16>(sent[0].constData()), quint16(68));

    // The undo's rows arrive with the rows they name, and are selected once.
    snapshot = fullQueueSnapshot(6, numberedPaths(5));
    snapshot.queueUndo.present = true;
    snapshot.queueUndo.restoredId = 11;
    snapshot.queueUndo.restoredRevision = 6;
    snapshot.queueUndo.restoredRanges = {{1, 1}, {3, 2}};
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.queueUndoId(), 0.0);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(restored.count(), 1);
    QCOMPARE(restored[0][0].toList(), (QVariantList{1, 3, 4}));
    snapshot.queue = {};
    client.processBinarySnapshot(snapshot);
    QCOMPARE(restored.count(), 1);

    // Rows the UI does not have yet are not selected.
    snapshot = fullQueueSnapshot(7, numberedPaths(5));
    snapshot.queueUndo.present = true;
    snapshot.queueUndo.restoredId = 12;
    snapshot.queueUndo.restoredRevision = 8;
    snapshot.queueUndo.restoredRanges = {{0, 1}};
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(restored.count(), 1);
}

void BridgeClientTest::aClearedQueueComesBackThroughTheBackend() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    QStringList paths;
    for (const char *name : {"a.flac", "b.flac", "c.flac"}) {
        const QString path = base + QLatin1Char('/') + QLatin1String(name);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("stub"), qint64(4));
        paths.push_back(path);
    }
    BridgeClient bridge;
    QVERIFY(bridge.m_ffiBridge != nullptr);
    QSignalSpy errors(&bridge, &BridgeClient::bridgeError);
    // A queue an earlier test left behind is cleared first.
    bridge.clearQueue();
    bridge.appendPaths(paths);
    // This queue, not one an earlier test left in the saved session.
    QTRY_VERIFY_WITH_TIMEOUT(bridge.queueLength() == 3 && bridge.queuePathAt(0) == paths[0]
                                 && bridge.queuePathAt(2) == paths[2],
                             10000);
    bridge.selectQueueIndex(1);

    bridge.clearQueue();
    QTRY_VERIFY_WITH_TIMEOUT(bridge.queueLength() == 0
                                 && bridge.queueUndoKind() == BinaryBridgeCodec::QueueUndoCleared,
                             10000);
    QCOMPARE(bridge.queueUndoTrackCount(), 3);
    const double cleared = bridge.queueUndoId();
    QSignalSpy restored(&bridge, &BridgeClient::queueUndoRestored);
    bridge.undoQueueChange();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 3, 10000);
    for (int i = 0; i < paths.size(); ++i) {
        QCOMPARE(bridge.queuePathAt(i), paths[i]);
    }
    QTRY_COMPARE(restored.count(), 1);
    QCOMPARE(restored[0][0].toList(), QVariantList{1});
    QCOMPARE(bridge.selectedQueueIndex(), 1);
    QTRY_COMPARE(bridge.queueUndoId(), 0.0);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().first().toString()));

    // Undoing the same change again is refused where the user sees it.
    bridge.m_queueUndoId = static_cast<quint64>(cleared);
    bridge.undoQueueChange();
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 10000);
    QVERIFY2(errors[0][0].toString().contains(QStringLiteral("Nothing was undone")),
             qPrintable(errors[0][0].toString()));
    // It changed nothing, so it is a warning about the queue.
    QCOMPARE(errors[0][1].toInt(), int(BinaryBridgeCodec::ErrorCategoryPlayback));
    QCOMPARE(errors[0][2].toInt(), int(BinaryBridgeCodec::ErrorSeverityWarning));
    QCOMPARE(bridge.queueLength(), 3);
    // Leave no queue of removed fixtures to the tests after this one.
    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
    isolateBridgeClient(bridge);
}

void BridgeClientTest::tagEditorPreviewsRenamesWithoutRenaming() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    QStringList paths;
    for (const char *name : {"first.ac3", "second.ac3"}) {
        const QString path = base + QLatin1Char('/') + QLatin1String(name);
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(128, '\0')), qint64(128));
        paths.push_back(path);
    }
    BridgeClient bridge;
    isolateBridgeClient(bridge);
    TagEditorController editor(&bridge);
    QVERIFY(editor.openForPaths(paths));
    QTRY_VERIFY(!editor.loading());
    // Both are named after the same tags.
    for (int row = 0; row < 2; ++row) {
        editor.setCell(row, QStringLiteral("title"), QStringLiteral("Twin"));
        editor.setCell(row, QStringLiteral("trackNo"), QStringLiteral("1"));
    }
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(saved.first().first().toBool());
    editor.selectAllRows();

    QSignalSpy previewed(&editor, &TagEditorController::renamePreviewReady);
    QVERIFY(editor.previewRenameSelectedFiles());
    QVERIFY(!editor.saving());
    QTRY_COMPARE(previewed.size(), 1);
    QVERIFY(previewed[0][1].toString().isEmpty());
    const QVariantList rows = previewed[0][0].toList();
    QCOMPARE(rows.size(), 2);
    const QVariantMap first = rows[0].toMap();
    const QVariantMap second = rows[1].toMap();
    QCOMPARE(first.value(QStringLiteral("oldName")).toString(), QStringLiteral("first.ac3"));
    QVERIFY(first.value(QStringLiteral("ok")).toBool());
    QVERIFY(!first.value(QStringLiteral("unchanged")).toBool());
    QVERIFY2(first.value(QStringLiteral("newName")).toString().contains(QStringLiteral("Twin")),
             qPrintable(first.value(QStringLiteral("newName")).toString()));
    QCOMPARE(second.value(QStringLiteral("newName")), first.value(QStringLiteral("newName")));
    QVERIFY(!second.value(QStringLiteral("ok")).toBool());
    QVERIFY(second.value(QStringLiteral("error")).toString().contains(QStringLiteral("conflicts")));
    // Nothing on disk or in the editor changed.
    for (const QString &path : paths) {
        QVERIFY(QFile::exists(path));
    }
    QCOMPARE(QDir(base).entryList(QDir::Files).size(), 2);
    QCOMPARE(editor.loadedPaths(), paths);
}

namespace {

// A 16-bit mono PCM WAV of `millis` milliseconds of a quiet tone at `path`.
bool writeTestWav(const QString &path, int millis) {
    constexpr quint32 rate = 8000;
    const quint32 frames = rate * static_cast<quint32>(millis) / 1000;
    QByteArray bytes;
    const auto le32 = [&bytes](quint32 value) { putLe<quint32>(bytes, value); };
    const auto le16 = [&bytes](quint16 value) { putLe<quint16>(bytes, value); };
    bytes.append("RIFF");
    le32(36 + frames * 2);
    bytes.append("WAVEfmt ");
    le32(16);
    le16(1);
    le16(1);
    le32(rate);
    le32(rate * 2);
    le16(2);
    le16(16);
    bytes.append("data");
    le32(frames * 2);
    for (quint32 frame = 0; frame < frames; ++frame) {
        putLe<qint16>(bytes, static_cast<qint16>((frame % 16) < 8 ? 800 : -800));
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

} // namespace

void BridgeClientTest::anUndoOfAClearWhileStoppedKeepsTheStoppedRowOfARepeatedTrack() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    const QString a = base + QStringLiteral("/a.wav");
    const QString b = base + QStringLiteral("/b.wav");
    const QString c = base + QStringLiteral("/c.wav");
    for (const QString &path : {a, b, c}) {
        QVERIFY(writeTestWav(path, 2000));
    }
    const QStringList queue{a, b, a, c};
    // The GStreamer backend, with the fake output CTest sets up. That
    // output plays faster than real time, so nothing here plays: Next
    // while stopped moves the current track without starting it, and each
    // step is checked in a state that holds.
    BridgeClient bridge;
    QVERIFY(bridge.m_ffiBridge != nullptr);
    QSignalSpy errors(&bridge, &BridgeClient::bridgeError);
    bridge.clearQueue();
    bridge.appendPaths(queue);
    // This queue, not one an earlier test left in the saved session.
    QTRY_VERIFY_WITH_TIMEOUT(bridge.queueLength() == 4 && bridge.queuePathAt(1) == b
                                 && bridge.queuePathAt(3) == c && bridge.currentTrackPath() == a,
                             10000);

    // The second A becomes current, then is stopped: the engine keeps A
    // and reports no row, so the queue shows the first A's row.
    bridge.next();
    bridge.next();
    QTRY_VERIFY_WITH_TIMEOUT(bridge.playingQueueIndex() == 2, 10000);
    bridge.stop();
    QTRY_VERIFY_WITH_TIMEOUT(bridge.playingQueueIndex() == 0 && bridge.currentTrackPath() == a,
                             10000);
    QCOMPARE(bridge.playbackState(), QStringLiteral("Stopped"));
    // B is selected, not played.
    bridge.selectQueueIndex(1);
    QTRY_COMPARE_WITH_TIMEOUT(bridge.selectedQueueIndex(), 1, 10000);

    bridge.clearQueue();
    QTRY_VERIFY_WITH_TIMEOUT(bridge.queueLength() == 0
                                 && bridge.queueUndoKind() == BinaryBridgeCodec::QueueUndoCleared,
                             10000);
    bridge.undoQueueChange();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 4, 10000);
    // The second A comes back as the current track, stopped, so Play would
    // start it, and the track after it is C.
    QTRY_VERIFY2_WITH_TIMEOUT(bridge.playingQueueIndex() == 2 && bridge.currentTrackPath() == a,
                              qPrintable(QString::number(bridge.playingQueueIndex()) + QLatin1Char(' ')
                                         + bridge.currentTrackPath()),
                              10000);
    QCOMPARE(bridge.playbackState(), QStringLiteral("Stopped"));
    QCOMPARE(bridge.selectedQueueIndex(), 1);
    bridge.next();
    QTRY_VERIFY2_WITH_TIMEOUT(bridge.playingQueueIndex() == 3 && bridge.currentTrackPath() == c,
                              qPrintable(QString::number(bridge.playingQueueIndex()) + QLatin1Char(' ')
                                         + bridge.currentTrackPath()),
                              10000);
    QCOMPARE(bridge.playbackState(), QStringLiteral("Stopped"));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.isEmpty() ? QString() : errors.first().first().toString()));

    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
    isolateBridgeClient(bridge);
}

void BridgeClientTest::artworkGoesToTheTrackItWasSearchedForAfterPlaybackMovesOn() {
    BridgeClient client;
    isolateBridgeClient(client);
    // The search must not reach the network: every request fails at once.
    client.m_itunesArtworkNetwork.setProxy(
        QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), 9));
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(payload);
        return true;
    };
    const auto playing = [](const QString &path, const QString &album) {
        BinaryBridgeCodec::DecodedSnapshot snapshot;
        snapshot.playback.present = true;
        snapshot.playback.state = 1;
        snapshot.playback.currentPath = path;
        snapshot.metadata.present = true;
        snapshot.metadata.sourcePath = path;
        snapshot.metadata.title = QStringLiteral("Song");
        snapshot.metadata.artist = QStringLiteral("Artist");
        snapshot.metadata.album = album;
        return snapshot;
    };
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("a.flac"));
    const QString b = dir.filePath(QStringLiteral("b.flac"));
    for (const QString &path : {a, b}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("stub"), qint64(4));
    }
    QVERIFY(client.processBinarySnapshot(playing(a, QStringLiteral("Album A"))));
    QCOMPARE(client.currentTrackPath(), a);

    // The dialog opens on A and searches for A's album; the file A is now
    // is noted for the backend to compare.
    client.searchCurrentTrackArtworkSuggestions();
    QTRY_VERIFY_WITH_TIMEOUT(!client.itunesArtworkLoading(), 10000);
    QTRY_VERIFY(client.m_itunesArtworkTarget.contains(QStringLiteral("identity")));
    const QFileInfo aInfo(a);
    QCOMPARE(client.itunesArtworkTarget().value(QStringLiteral("album")).toString(),
             QStringLiteral("Album A"));
    client.m_itunesArtworkResults = {
        QVariantMap{{QStringLiteral("normalizedPath"), QStringLiteral("/fixture/cache/a.jpg")}}};

    // Playback moves on to B while the user confirms.
    QVERIFY(client.processBinarySnapshot(playing(b, QStringLiteral("Album B"))));
    QCOMPARE(client.currentTrackPath(), b);
    client.applyItunesArtworkSuggestion(0);
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent[0], BinaryBridgeCodec::encodeCommandApplyAlbumArt(
                          a, QStringLiteral("/fixture/cache/a.jpg"),
                          static_cast<quint64>(aInfo.size()),
                          aInfo.fileTime(QFileDevice::FileModificationTime).toMSecsSinceEpoch()));
    QCOMPARE(qFromLittleEndian<quint16>(sent[0].constData()), quint16(46));

    // Closing the dialog drops the target with the suggestions.
    client.clearItunesArtworkSuggestions();
    QVERIFY(client.itunesArtworkTarget().isEmpty());
}

void BridgeClientTest::artworkForATrackMovedSinceTheSearchIsRefusedAndWritesNothing() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    const QString track = base + QStringLiteral("/a.wav");
    QVERIFY(writeTestWav(track, 2000));
    // The folder's cover, which applied artwork would replace.
    const QString cover = base + QStringLiteral("/cover.png");
    QImage oldCover(4, 4, QImage::Format_RGB32);
    oldCover.fill(Qt::red);
    QVERIFY(oldCover.save(cover));
    QFile coverFile(cover);
    QVERIFY(coverFile.open(QIODevice::ReadOnly));
    const QByteArray coverBytes = coverFile.readAll();
    coverFile.close();
    QTemporaryDir artworkDir;
    QVERIFY(artworkDir.isValid());
    const QString artwork = artworkDir.filePath(QStringLiteral("suggestion.png"));
    QImage newCover(4, 4, QImage::Format_RGB32);
    newCover.fill(Qt::blue);
    QVERIFY(newCover.save(artwork));

    BridgeClient bridge;
    QVERIFY(bridge.m_ffiBridge != nullptr);
    QSignalSpy errors(&bridge, &BridgeClient::bridgeError);
    bridge.clearQueue();
    bridge.appendPaths({track});
    // The loaded queue makes the track current, without playing it.
    QTRY_COMPARE_WITH_TIMEOUT(bridge.currentTrackPath(), track, 10000);

    // The search notes the track; then the track is renamed in its folder.
    bridge.searchCurrentTrackArtworkSuggestions();
    QTRY_VERIFY(bridge.m_itunesArtworkTarget.value(QStringLiteral("identity")).toMap().size() == 2);
    bridge.m_itunesArtworkResults = {
        QVariantMap{{QStringLiteral("normalizedPath"), artwork}}};
    QVERIFY(QFile::rename(track, base + QStringLiteral("/a renamed.wav")));
    errors.clear();

    bridge.applyItunesArtworkSuggestion(0);
    QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 10000);
    QVERIFY2(errors[0][0].toString().contains(QStringLiteral("moved, renamed or deleted")),
             qPrintable(errors[0][0].toString()));
    QCOMPARE(errors[0][1].toInt(), int(BinaryBridgeCodec::ErrorCategoryLibrary));
    QVERIFY(coverFile.open(QIODevice::ReadOnly));
    QCOMPARE(coverFile.readAll(), coverBytes);
    coverFile.close();

    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
    isolateBridgeClient(bridge);
}

void BridgeClientTest::aConfirmedRenameRunsOnlyThePreviewedRenames() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    const QString x = base + QStringLiteral("/x.ac3");
    const QString y = base + QStringLiteral("/y.ac3");
    for (const QString &path : {x, y}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(QByteArray(128, '\0')), qint64(128));
    }
    BridgeClient bridge;
    isolateBridgeClient(bridge);
    TagEditorController editor(&bridge);
    QVERIFY(editor.openForPaths({x, y}));
    QTRY_VERIFY(!editor.loading());
    editor.setCell(0, QStringLiteral("title"), QStringLiteral("One"));
    editor.setCell(0, QStringLiteral("trackNo"), QStringLiteral("1"));
    editor.setCell(1, QStringLiteral("title"), QStringLiteral("Two"));
    editor.setCell(1, QStringLiteral("trackNo"), QStringLiteral("2"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(saved.first().first().toBool());
    editor.selectAllRows();

    // y's new name is taken by another file, so the preview renames x and
    // says y keeps its name.
    const QString xRenamed = base + QStringLiteral("/01 - One.ac3");
    const QString blocker = base + QStringLiteral("/02 - Two.ac3");
    QFile taken(blocker);
    QVERIFY(taken.open(QIODevice::WriteOnly));
    QCOMPARE(taken.write("not ours"), qint64(8));
    taken.close();
    QSignalSpy previewed(&editor, &TagEditorController::renamePreviewReady);
    QVERIFY(editor.previewRenameSelectedFiles());
    QTRY_COMPARE(previewed.size(), 1);
    const QVariantList rows = previewed[0][0].toList();
    QCOMPARE(rows.size(), 2);
    QVERIFY(rows[0].toMap().value(QStringLiteral("ok")).toBool());
    QCOMPARE(rows[0].toMap().value(QStringLiteral("newPath")).toString(), xRenamed);
    QVERIFY(!rows[1].toMap().value(QStringLiteral("ok")).toBool());
    QCOMPARE(rows[1].toMap().value(QStringLiteral("newPath")).toString(), blocker);

    // The user frees y's new name in a file manager, then confirms.
    QVERIFY(QFile::remove(blocker));
    QSignalSpy renamed(&editor, &TagEditorController::renameFinished);
    QVERIFY(editor.renamePreviewedFiles());
    QTRY_COMPARE(renamed.size(), 1);
    QVERIFY2(QFile::exists(xRenamed), qPrintable(editor.statusText() + " " + editor.statusDetails()));
    QVERIFY(!QFile::exists(x));
    // y keeps its name, as the preview said.
    QVERIFY(QFile::exists(y));
    QVERIFY(!QFile::exists(blocker));
    QCOMPARE(QDir(base).entryList(QDir::Files).size(), 2);
}

void BridgeClientTest::queueDeltasChangeOnlyTheirRowsWithoutAReset() {
    BridgeClient client;
    isolateBridgeClient(client);
    QVERIFY(client.processBinarySnapshot(fullQueueSnapshot(1, numberedPaths(5), 3)));
    QCOMPARE(client.m_queueRevision, quint64(1));
    QCOMPARE(client.queueLength(), 5);
    QCOMPARE(client.selectedQueueIndex(), 3);

    auto *model = qobject_cast<QueueRowsModel *>(client.queueRows());
    QueueRowSignals rowSignals(model);
    // [0, 1, 2, 3, 4] → insert x at 2 → [0, 1, x, 2, 3, 4] → remove 0
    // → [1, x, 2, 3, 4] → move 4 to 0 → [4, 1, x, 2, 3] → retitle x.
    QVector<BinaryBridgeCodec::DecodedQueueOp> ops{
        queueOp(BinaryBridgeCodec::QueueOpInsert, 2, 0, {queueTrack(QStringLiteral("/music/x.flac"))}),
        queueOp(BinaryBridgeCodec::QueueOpRemove, 0, 1),
        queueOp(BinaryBridgeCodec::QueueOpMove, 4, 0),
        queueOp(BinaryBridgeCodec::QueueOpUpdate, 2, 0,
            {queueTrack(QStringLiteral("/music/x.flac"), QStringLiteral("Retitled"))}),
    };
    // The removal before row 3 moved it, and the backend says where.
    QVERIFY(client.processBinarySnapshot(queueDeltaSnapshot(1, 5, 5, ops, 4)));

    QCOMPARE(rowSignals.resets.count(), 0);
    QCOMPARE(rowSignals.inserts.count(), 1);
    QCOMPARE(rowSignals.removes.count(), 1);
    QCOMPARE(rowSignals.moves.count(), 1);
    QCOMPARE(rowSignals.updates.count(), 1);
    QCOMPARE(rowSignals.updates.at(0).at(0).toModelIndex().row(), 2);
    QCOMPARE(queuePaths(client),
        (QStringList{QStringLiteral("/music/4.flac"), QStringLiteral("/music/1.flac"),
            QStringLiteral("/music/x.flac"), QStringLiteral("/music/2.flac"),
            QStringLiteral("/music/3.flac")}));
    QCOMPARE(model->data(model->index(2, 0), QueueRowsModel::TitleRole).toString(),
        QStringLiteral("Retitled"));
    QCOMPARE(client.m_queueRevision, quint64(5));
    QCOMPARE(client.queueLength(), 5);
    QCOMPARE(client.selectedQueueIndex(), 4);
    QVERIFY(!client.m_queueResyncRequested);
}

void BridgeClientTest::queueRowRangeMovesDecodeAndMoveTheirRowsOnce() {
    QByteArray full;
    putQueueHeader(full, BinaryBridgeCodec::QueueFull, 1, 6);
    for (const QString &path : numberedPaths(6)) {
        putQueueRow(full, path, QStringLiteral("Track"));
    }
    // Rows 1 and 2 go after row 4, as the relay sends a move of rows.
    QByteArray delta;
    putQueueHeader(delta, BinaryBridgeCodec::QueueDelta, 2, 6);
    putLe<quint64>(delta, 1);
    putLe<quint32>(delta, 1);
    putLe<quint8>(delta, BinaryBridgeCodec::QueueOpMoveRows);
    putLe<quint32>(delta, 1);
    putLe<quint32>(delta, 2);
    putLe<quint32>(delta, 3);
    BinaryBridgeCodec::DecodedSnapshot decodedFull;
    BinaryBridgeCodec::DecodedSnapshot decodedDelta;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(queuePacket(full), &decodedFull, &error),
        qPrintable(error));
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(queuePacket(delta), &decodedDelta, &error),
        qPrintable(error));
    QCOMPARE(decodedDelta.queue.ops.size(), 1);
    QCOMPARE(decodedDelta.queue.ops.at(0).count, 2);
    QCOMPARE(decodedDelta.queue.ops.at(0).to, 3);

    BridgeClient client;
    isolateBridgeClient(client);
    QVERIFY(client.processBinarySnapshot(decodedFull));
    auto *model = qobject_cast<QueueRowsModel *>(client.queueRows());
    QueueRowSignals rowSignals(model);
    QVERIFY(client.processBinarySnapshot(decodedDelta));
    QCOMPARE(rowSignals.resets.count(), 0);
    QCOMPARE(rowSignals.moves.count(), 1);
    const QStringList paths = numberedPaths(6);
    QCOMPARE(queuePaths(client),
        (QStringList{paths[0], paths[3], paths[4], paths[1], paths[2], paths[5]}));
    QCOMPARE(client.m_queueRevision, quint64(2));

    // A move past the rows leaves them alone and asks for the whole queue.
    QVERIFY(!model->moveRows(5, 2, 0));
    QVERIFY(!model->moveRows(0, 2, 5));
    QVERIFY(model->moveRows(0, 6, 0));
    QCOMPARE(rowSignals.moves.count(), 1);
}

void BridgeClientTest::movedQueueRowsTravelAsOneCommandWithTheRustLayout() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(payload);
        return true;
    };
    QVERIFY(client.processBinarySnapshot(fullQueueSnapshot(12, numberedPaths(6))));
    sent.clear();

    // Rows the queue lacks and repeats are left out; the rest go in order.
    client.moveQueueRows(QVariantList{3, 0, 3, 99}, 4);
    client.shiftQueueRows(QVariantList{5, 1, 2}, -1);
    client.shiftQueueRows(QVariantList{2}, 1);
    client.moveQueueRows(QVariantList{1}, 99);
    client.moveQueueRows(QVariantList{99}, 0);
    client.shiftQueueRows(QVariantList{}, 1);
    QCOMPARE(sent.size(), 4);
    // As the Rust tests of command 70 read it.
    const auto expected = [](quint8 how, quint32 at, const QList<quint32> &rows) {
        QByteArray payload;
        putLe<quint64>(payload, 12);
        putLe<quint8>(payload, how);
        putLe<quint32>(payload, at);
        putLe<quint32>(payload, static_cast<quint32>(rows.size()));
        for (quint32 row : rows) {
            putLe<quint32>(payload, row);
        }
        QByteArray command;
        putLe<quint16>(command, 70);
        putLe<quint32>(command, static_cast<quint32>(payload.size()));
        return command + payload;
    };
    QCOMPARE(sent.at(0), expected(0, 4, {0, 3}));
    QCOMPARE(sent.at(1), expected(1, 0, {1, 2, 5}));
    QCOMPARE(sent.at(2), expected(2, 0, {2}));
    // Past the last row is after it.
    QCOMPARE(sent.at(3), expected(0, 6, {1}));
}

void BridgeClientTest::libraryItemsTravelAsOneCommandWithTheRustLayout() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(payload);
        return true;
    };
    QSignalSpy errors(&client, &BridgeClient::bridgeError);
    QVERIFY(client.processBinarySnapshot(fullQueueSnapshot(33, numberedPaths(4))));
    sent.clear();

    using Item = BinaryBridgeCodec::LibraryQueueItem;
    Item paths;
    paths.paths = {QStringLiteral("/music/a.flac"), QStringLiteral("/music/b.flac")};
    Item artist;
    artist.kind = Item::Artist;
    artist.value = QStringLiteral("artist|/music|Artist");
    Item root;
    root.kind = Item::Root;
    root.value = QStringLiteral("/music");
    Item empty;
    const BinaryBridgeCodec::LibraryQueueItems items{paths, empty, artist, root};
    client.insertLibraryItems(QVariant::fromValue(items), 2);
    client.playLibraryItems(QVariant::fromValue(items));
    client.appendLibraryItems(QVariant::fromValue(items));
    // An insertion after the last row is an append.
    client.insertLibraryItems(QVariant::fromValue(items), 4);
    client.insertPaths(QStringList{QStringLiteral("/music/dropped")}, 1);
    // Nothing to queue sends nothing.
    client.playLibraryItems(QVariant::fromValue(BinaryBridgeCodec::LibraryQueueItems{empty}));
    client.playLibraryItems(QVariant());
    QCOMPARE(sent.size(), 5);
    // As the Rust tests of command 69 read it.
    const auto expected = [](quint8 placement, quint32 at, const QList<Item> &rows) {
        QByteArray payload;
        putLe<quint8>(payload, placement);
        putLe<quint64>(payload, 33);
        putLe<quint32>(payload, at);
        putLe<quint32>(payload, static_cast<quint32>(rows.size()));
        for (const Item &row : rows) {
            putLe<quint8>(payload, row.kind);
            if (row.kind == Item::Paths) {
                putLe<quint32>(payload, static_cast<quint32>(row.paths.size()));
                for (const QString &path : row.paths) {
                    putString(payload, path);
                }
            } else {
                putString(payload, row.value);
            }
        }
        QByteArray command;
        putLe<quint16>(command, 69);
        putLe<quint32>(command, static_cast<quint32>(payload.size()));
        return command + payload;
    };
    QCOMPARE(sent.at(0), expected(2, 2, {paths, artist, root}));
    QCOMPARE(sent.at(1), expected(1, 0, {paths, artist, root}));
    QCOMPARE(sent.at(2), expected(0, 0, {paths, artist, root}));
    QCOMPARE(sent.at(3), expected(0, 0, {paths, artist, root}));
    Item dropped;
    dropped.paths = {QStringLiteral("/music/dropped")};
    QCOMPARE(sent.at(4), expected(2, 1, {dropped}));
    QCOMPARE(errors.count(), 0);

    // Rows over the command bound are refused whole, and the user is told.
    Item huge;
    huge.paths = QStringList(9, QLatin1Char('/') + QString(1024 * 1024, QLatin1Char('a')));
    client.playLibraryItems(QVariant::fromValue(BinaryBridgeCodec::LibraryQueueItems{huge}));
    QCOMPARE(sent.size(), 5);
    QCOMPARE(errors.count(), 1);
    QVERIFY(errors.at(0).at(0).toString().contains(QStringLiteral("The queue was not changed")));
}

void BridgeClientTest::aMissedQueueRevisionAsksForTheWholeQueueOnce() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<quint16> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(qFromLittleEndian<quint16>(payload.constData()));
        return true;
    };
    QVERIFY(client.processBinarySnapshot(fullQueueSnapshot(1, numberedPaths(3))));
    auto *model = qobject_cast<QueueRowsModel *>(client.queueRows());
    QueueRowSignals rowSignals(model);

    // The client's rows are at revision 1; a delta from revision 2 means
    // it missed one. Its rows stay as they are, and it asks once.
    const auto missed = queueDeltaSnapshot(
        2, 3, 2, {queueOp(BinaryBridgeCodec::QueueOpRemove, 0, 1)});
    client.processBinarySnapshot(missed);
    client.processBinarySnapshot(missed);
    BinaryBridgeCodec::DecodedSnapshot lost;
    lost.queue.present = true;
    lost.queue.kind = BinaryBridgeCodec::QueueResync;
    client.processBinarySnapshot(lost);
    QCOMPARE(sent, (QList<quint16>{BinaryBridgeCodec::CmdRequestSnapshot}));
    QCOMPARE(queuePaths(client), numberedPaths(3));
    QCOMPARE(client.m_queueRevision, quint64(1));
    QCOMPARE(rowSignals.removes.count(), 0);

    // The whole queue settles it, and deltas apply again.
    QVERIFY(client.processBinarySnapshot(fullQueueSnapshot(4, numberedPaths(2))));
    QVERIFY(!client.m_queueResyncRequested);
    QVERIFY(client.processBinarySnapshot(queueDeltaSnapshot(
        4, 5, 1, {queueOp(BinaryBridgeCodec::QueueOpRemove, 0, 1)})));
    QCOMPARE(queuePaths(client), (QStringList{QStringLiteral("/music/1.flac")}));
    QCOMPARE(client.m_queueRevision, quint64(5));

    // Operations that do not fit the rows leave them at no revision.
    client.processBinarySnapshot(queueDeltaSnapshot(
        5, 6, 0, {queueOp(BinaryBridgeCodec::QueueOpRemove, 3, 1)}));
    QCOMPARE(client.m_queueRevision, quint64(0));
    QCOMPARE(sent.count(BinaryBridgeCodec::CmdRequestSnapshot), 2);
}

void BridgeClientTest::rowCommandsCarryTheQueueRevision() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.push_back(payload);
        return true;
    };
    QVERIFY(client.processBinarySnapshot(fullQueueSnapshot(42, numberedPaths(4))));

    client.playAt(1);
    client.selectQueueIndex(2);
    client.removeAt(3);
    client.moveQueue(0, 2);
    client.removeIndices({QVariant(1), QVariant(3)});
    QCOMPARE(sent.size(), 5);
    const QList<quint16> ids{BinaryBridgeCodec::CmdPlayAt, BinaryBridgeCodec::CmdSelectQueue,
        BinaryBridgeCodec::CmdRemoveAt, BinaryBridgeCodec::CmdMoveQueue,
        BinaryBridgeCodec::CmdRemoveMany};
    for (int i = 0; i < sent.size(); ++i) {
        const QByteArray &command = sent.at(i);
        QCOMPARE(qFromLittleEndian<quint16>(command.constData()), ids.at(i));
        QCOMPARE(qFromLittleEndian<quint64>(command.constData() + 6), quint64(42));
    }
    QCOMPARE(qFromLittleEndian<quint32>(sent.at(0).constData() + 14), quint32(1));
    QCOMPARE(qFromLittleEndian<qint32>(sent.at(1).constData() + 14), qint32(2));
    QCOMPARE(qFromLittleEndian<quint32>(sent.at(2).constData() + 14), quint32(3));
    QCOMPARE(qFromLittleEndian<quint32>(sent.at(3).constData() + 14), quint32(0));
    QCOMPARE(qFromLittleEndian<quint32>(sent.at(3).constData() + 18), quint32(2));
    QCOMPARE(qFromLittleEndian<quint32>(sent.at(4).constData() + 14), quint32(2));
    QCOMPARE(qFromLittleEndian<quint32>(sent.at(4).constData() + 18), quint32(3));
    QCOMPARE(qFromLittleEndian<quint32>(sent.at(4).constData() + 22), quint32(1));
}

void BridgeClientTest::aLargeQueueEditDecodesOnlyTheChangedRows() {
    constexpr int kRows = 10000;
    QByteArray full;
    putQueueHeader(full, BinaryBridgeCodec::QueueFull, 1, kRows);
    for (int index = 0; index < kRows; ++index) {
        putQueueRow(full, QStringLiteral("/music/%1.flac").arg(index), QStringLiteral("Track"));
    }
    QByteArray delta;
    putQueueHeader(delta, BinaryBridgeCodec::QueueDelta, 2, kRows);
    putLe<quint64>(delta, 1);
    putLe<quint32>(delta, 1);
    putLe<quint8>(delta, BinaryBridgeCodec::QueueOpUpdate);
    putLe<quint32>(delta, 7777);
    putQueueRow(delta, QStringLiteral("/music/7777.flac"), QStringLiteral("Tagged"));

    BinaryBridgeCodec::DecodedSnapshot decodedFull;
    BinaryBridgeCodec::DecodedSnapshot decodedDelta;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(queuePacket(full), &decodedFull, &error),
        qPrintable(error));
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(queuePacket(delta), &decodedDelta, &error),
        qPrintable(error));
    QCOMPARE(decodedFull.queue.tracks.size(), kRows);
    // The edit decodes one row, not ten thousand.
    QVERIFY(decodedDelta.queue.tracks.isEmpty());
    QCOMPARE(decodedDelta.queue.ops.size(), 1);
    QCOMPARE(decodedDelta.queue.ops.at(0).tracks.size(), 1);
    QVERIFY2(delta.size() < 150, qPrintable(QString::number(delta.size())));
    QVERIFY(full.size() > kRows * 40);

    BridgeClient client;
    isolateBridgeClient(client);
    QVERIFY(client.processBinarySnapshot(decodedFull));
    auto *model = qobject_cast<QueueRowsModel *>(client.queueRows());
    QueueRowSignals rowSignals(model);
    QVERIFY(client.processBinarySnapshot(decodedDelta));
    QCOMPARE(rowSignals.resets.count(), 0);
    QCOMPARE(rowSignals.updates.count(), 1);
    QCOMPARE(model->data(model->index(7777, 0), QueueRowsModel::TitleRole).toString(),
        QStringLiteral("Tagged"));
    QCOMPARE(client.queueLength(), kRows);

    // A truncated or overlong section is refused, not misread.
    BinaryBridgeCodec::DecodedSnapshot refused;
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(
        queuePacket(delta.left(delta.size() - 1)), &refused, &error));
    QByteArray overlong = delta;
    qToLittleEndian<quint32>(1000000, overlong.data() + 37);
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(queuePacket(overlong), &refused, &error));
}

void BridgeClientTest::gaplessAndRepeatOneMoveOnlyThePlayingRow() {
    BridgeClient client;
    isolateBridgeClient(client);
    const QStringList paths = numberedPaths(3);
    QVERIFY(client.processBinarySnapshot(fullQueueSnapshot(1, paths)));
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(paths.at(0), 1.0, 1)));
    auto *model = qobject_cast<QueueRowsModel *>(client.queueRows());
    QueueRowSignals rowSignals(model);

    // A gapless handoff and a Repeat-One restart carry only the playback
    // section: the playing row moves, and no queue row changes.
    auto gapless = playbackSnapshot(paths.at(1), 0.0, 2);
    gapless.playback.currentQueueIndex = 1;
    client.m_pollTrackIdentityChanged = false;
    QVERIFY(client.processBinarySnapshot(gapless));
    QCOMPARE(client.playingQueueIndex(), 1);
    QVERIFY(client.m_pollTrackIdentityChanged);

    auto repeatOne = playbackSnapshot(paths.at(1), 0.0, 3);
    repeatOne.playback.currentQueueIndex = 1;
    QVERIFY(client.processBinarySnapshot(repeatOne));
    QCOMPARE(client.playingQueueIndex(), 1);
    QCOMPARE(client.m_currentTrackToken, quint64(3));
    QCOMPARE(client.currentTrackPath(), paths.at(1));

    // An edit before the playing row arrives with the new playing index.
    auto edit = queueDeltaSnapshot(1, 2, 2, {queueOp(BinaryBridgeCodec::QueueOpRemove, 0, 1)});
    edit.playback = repeatOne.playback;
    edit.playback.currentQueueIndex = 0;
    QVERIFY(client.processBinarySnapshot(edit));
    QCOMPARE(client.playingQueueIndex(), 0);
    QCOMPARE(client.queuePathAt(0), paths.at(1));
    QCOMPARE(rowSignals.resets.count(), 0);
    QCOMPARE(rowSignals.updates.count(), 0);
    QCOMPARE(rowSignals.removes.count(), 1);
}

void BridgeClientTest::realBridgeQueueEditsArriveAsRowSignals() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    QStringList paths;
    for (const QString &name : {QStringLiteral("a.flac"), QStringLiteral("b.flac"),
             QStringLiteral("c.flac")}) {
        QFile file(base + QLatin1Char('/') + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("stub"), qint64(4));
        paths.push_back(file.fileName());
    }
    BridgeClient bridge;
    QVERIFY(bridge.m_ffiBridge != nullptr);
    bridge.clearQueue();
    bridge.appendPaths(paths);
    // The append may arrive whole, merged with the cleared queue the
    // client had not taken yet.
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 3, 10000);
    QCOMPARE(queuePaths(bridge), paths);
    auto *model = qobject_cast<QueueRowsModel *>(bridge.queueRows());
    QueueRowSignals rowSignals(model);

    bridge.removeAt(1);
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 2, 10000);
    bridge.moveQueue(1, 0);
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queuePathAt(0), paths.at(2), 10000);
    QCOMPARE(queuePaths(bridge), (QStringList{paths.at(2), paths.at(0)}));

    // The edits arrived as row signals.
    QCOMPARE(rowSignals.resets.count(), 0);
    QCOMPARE(rowSignals.removes.count(), 1);
    QCOMPARE(rowSignals.moves.count(), 1);
    QVERIFY(!bridge.m_queueResyncRequested);

    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
    isolateBridgeClient(bridge);
}

void BridgeClientTest::inProcessBridgeInstallsWakeNotifier() {
    BridgeClient client;
    QVERIFY(client.m_ffiBridge != nullptr);
    QVERIFY(client.m_bridgeWakeFd >= 0);
    QVERIFY(client.m_bridgeWakeNotifier != nullptr);
    QVERIFY(client.m_bridgeWakeNotifier->isEnabled());

    isolateBridgeClient(client);
}

void BridgeClientTest::aStalledClientReceivesTheLatestQueueAndEveryError() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    QStringList paths;
    for (const QString &name : {QStringLiteral("a.flac"), QStringLiteral("b.flac")}) {
        QFile file(base + QLatin1Char('/') + name);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("stub"), qint64(4));
        paths.push_back(file.fileName());
    }
    BridgeClient bridge;
    QVERIFY(bridge.m_ffiBridge != nullptr);
    QVERIFY(bridge.m_bridgeWakeNotifier != nullptr);
    const int wakeFd = bridge.m_bridgeWakeFd;
    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
    // The client has taken everything, so the next event published wakes it.
    QTRY_VERIFY_WITH_TIMEOUT(
        !wakeFdIsReadable(wakeFd) && bridge.m_bridgeWakeNotifier->isEnabled()
            && !bridge.m_bridgePollTimer.isActive(),
        10000);
    QSignalSpy errors(&bridge, &BridgeClient::bridgeError);

    // The UI thread stalls: the event loop does not run, so nothing drains
    // the bridge. The queue changes, and the bridge sends it once: with
    // nothing playing, its snapshot is the first event published, and the
    // wake fd says when it waits for the client.
    bridge.m_bridgePollTimer.stop();
    bridge.m_bridgeWakeNotifier->setEnabled(false);
    bridge.appendPaths(paths);
    QVERIFY(wakeFdIsReadable(wakeFd, 10000));
    // Far more events than the bridge used to keep follow the queue.
    constexpr int kErrors = 30;
    for (int i = 0; i < kErrors; ++i) {
        QVERIFY(!sendUnknownCommand(bridge.m_ffiBridge));
    }

    bridge.m_bridgeWakeNotifier->setEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 2, 10000);
    QCOMPARE(bridge.queuePathAt(0), paths.at(0));
    QCOMPARE(bridge.queuePathAt(1), paths.at(1));
    QTRY_COMPARE_WITH_TIMEOUT(unknownCommandErrors(errors), kErrors, 10000);

    bridge.clearQueue();
    QTRY_COMPARE_WITH_TIMEOUT(bridge.queueLength(), 0, 10000);
    isolateBridgeClient(bridge);
}

void BridgeClientTest::aDrainEndsWithNoWakeupPending() {
    BridgeClient client;
    QVERIFY(client.m_ffiBridge != nullptr);
    QVERIFY(client.m_bridgeWakeNotifier != nullptr);
    const int wakeFd = client.m_bridgeWakeFd;

    // Stop the backend, so only this test queues events from here on.
    const QByteArray shutdown =
        BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdShutdown);
    QVERIFY(ferrous_ffi_bridge_send_binary(
        client.m_ffiBridge,
        reinterpret_cast<const std::uint8_t *>(shutdown.constData()),
        static_cast<std::size_t>(shutdown.size())));
    QTRY_VERIFY_WITH_TIMEOUT(!client.backendFailure().isEmpty(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !wakeFdIsReadable(wakeFd) && client.m_bridgeWakeNotifier->isEnabled()
            && !client.m_bridgePollTimer.isActive(),
        10000);

    QSignalSpy wakeups(client.m_bridgeWakeNotifier, &QSocketNotifier::activated);
    QSignalSpy errors(&client, &BridgeClient::bridgeError);
    QVERIFY(!sendUnknownCommand(client.m_ffiBridge));
    QVERIFY(wakeFdIsReadable(wakeFd));
    QTRY_COMPARE(unknownCommandErrors(errors), 1);
    // The poll acknowledged after draining, so nothing is left to wake for.
    QVERIFY(!wakeFdIsReadable(wakeFd));
    QTest::qWait(100);
    QCOMPARE(wakeups.count(), 1);
    QVERIFY(client.m_bridgeWakeNotifier->isEnabled());
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

void BridgeClientTest::bridgePollRunResultCountsPrecomputedSpectrogramWork() {
    BridgeClient::BridgePollRunResult result;
    result.processedPrecomputedFrames = 1;
    result.processedPrecomputedBytes = 4096;
    result.precomputedPopMs = 2.0;
    result.precomputedDispatchMs = 3.0;
    result.maxPrecomputedFrameMs = 3.0;

    QVERIFY(result.anyWorkProcessed());
    QVERIFY(!result.shouldContinueImmediately());
    QCOMPARE(result.precomputedPopMs, 2.0);
    QCOMPARE(result.precomputedDispatchMs, 3.0);
    QCOMPARE(result.maxPrecomputedFrameMs, 3.0);

    result.precomputedCapSaturated = true;
    QVERIFY(result.shouldContinueImmediately());
}

void BridgeClientTest::precomputedSpectrogramChunksRouteDirectlyToRegisteredItems() {
    BridgeClient client;
    isolateBridgeClient(client);

    SpectrogramItem left;
    SpectrogramItem right;
    left.setWidth(320);
    right.setWidth(320);

    client.registerSpectrogramItem(&left, 0);
    client.registerSpectrogramItem(&right, 1);

    QSignalSpy legacyPayloadSpy(&client, &BridgeClient::precomputedSpectrogramChunkReady);
    QSignalSpy channelLayoutSpy(&client, &BridgeClient::precomputedSpectrogramChannelsReady);

    const QByteArray raw = makePrecomputedFrame(4, 2, 3, 0, 12, 42, 7);
    const BridgeClient::PrecomputedDispatchStats stats =
        client.parsePrecomputedSpectrogramFrame(raw);

    QVERIFY(stats.valid);
    QCOMPARE(channelLayoutSpy.count(), 1);
    QCOMPARE(channelLayoutSpy.takeFirst().at(0).toInt(), 2);
    QCOMPARE(legacyPayloadSpy.count(), 0);

    QVERIFY(left.m_precomputedReady);
    QVERIFY(right.m_precomputedReady);
    QCOMPARE(left.m_ringCapacity > 0, true);
    QCOMPARE(right.m_ringCapacity > 0, true);
    QCOMPARE(left.m_ring.column(0)[0], static_cast<quint8>(0x11));
    QCOMPARE(right.m_ring.column(0)[0], static_cast<quint8>(0x77));
}

void BridgeClientTest::delayedSpectrogramBatchesPreserveColumnsAndRejectOldGeneration() {
    for (int mode : {0, 1}) {
        BridgeClient client;
        isolateBridgeClient(client);
        SpectrogramItem item;
        item.setWidth(320);
        item.setDisplayMode(mode);
        client.registerSpectrogramItem(&item, 0);
        for (int column = 0; column < 200; ++column) {
            auto frame = makePrecomputedFrame(4, 1, 1, column, 200, 42, 7);
            frame[45] = column == 0 ? 1 : 0;
            frame[46] = column == 0 ? 1 : 0;
            QVERIFY(client.parsePrecomputedSpectrogramFrame(frame).valid);
            if (column % 20 == 0) {
                QCoreApplication::processEvents();
                QVERIFY(client.parsePrecomputedSpectrogramFrame(
                    makePrecomputedFrame(4, 1, 3, 0, 200, 41, 6)).valid);
            }
        }
        QCOMPARE(item.m_ringWriteSeq, qint64(200));
        QCOMPARE(item.m_precomputedCommittedGeneration, quint64(7));
        for (int column = 0; column < 200; ++column) {
            QCOMPARE(item.m_ringColumnId[column], column);
            QCOMPARE(item.m_ringTrackToken[column], quint64(42));
            QCOMPARE(item.m_ring.column(column)[0], quint8(0x11));
        }
    }
}

void BridgeClientTest::spectrogramDetailedTraceLoggingRequiresExplicitOptIn() {
    qunsetenv("FERROUS_PROFILE_SPECTROGRAM_TRACE");
    SpectrogramTraceLogging::refreshDetailedSettingForTests();
    QVERIFY(!SpectrogramTraceLogging::detailedEnabled());

    qputenv("FERROUS_PROFILE_SPECTROGRAM_TRACE", "1");
    SpectrogramTraceLogging::refreshDetailedSettingForTests();
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    QVERIFY(SpectrogramTraceLogging::detailedEnabled());
#else
    QVERIFY(!SpectrogramTraceLogging::detailedEnabled());
#endif

    qputenv("FERROUS_PROFILE_SPECTROGRAM_TRACE", "false");
    SpectrogramTraceLogging::refreshDetailedSettingForTests();
    QVERIFY(!SpectrogramTraceLogging::detailedEnabled());

    qunsetenv("FERROUS_PROFILE_SPECTROGRAM_TRACE");
    SpectrogramTraceLogging::refreshDetailedSettingForTests();
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

void BridgeClientTest::profileDiagnosticsBypassUiThreadDiskQueue() {
    BridgeClient client;
    isolateBridgeClient(client);

    client.m_profileUiEnabled = true;
    client.m_pendingDiagnosticsDiskLines.clear();

    client.logProfileDiagnostic(
        QStringLiteral("ui-prof"),
        QStringLiteral("event_loop_stall ms=42 timer_ms=8 connected=1 snapshot_pending=0"));

    QCOMPARE(client.m_pendingDiagnosticsDiskLines.size(), 0);
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
    acceptCommands(client);
    const QString path = QStringLiteral("/music/track.flac");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 10.0, 1)));
    QSignalSpy landed(&client, &BridgeClient::seekLanded);
    client.seek(30.0);
    client.seek(60.0);
    QVERIFY(client.m_pendingSeek);
    QCOMPARE(client.m_pendingSeekSerial, quint64{2});
    QCOMPARE(clockPosition(client), 60.0);
    QVERIFY(!client.playbackClock()->advancing());

    // Before the engine got the seek, however long it takes.
    auto snapshot = playbackSnapshot(path, 12.0, 1);
    client.processBinarySnapshot(snapshot);
    QVERIFY(client.m_pendingSeek);
    QCOMPARE(clockPosition(client), 60.0);

    // An older seek landed: the newer one still waits, and the landing is
    // not reported.
    snapshot = playbackSnapshot(path, 30.0, 1);
    snapshot.playback.seekSerial = 1;
    snapshot.playback.seekLandedSerial = 1;
    client.processBinarySnapshot(snapshot);
    QVERIFY(client.m_pendingSeek);
    QCOMPARE(clockPosition(client), 60.0);
    QCOMPARE(landed.count(), 0);

    // The pending seek landed; the pipeline has not resumed yet.
    snapshot = playbackSnapshot(path, 60.0, 1);
    snapshot.playback.positionAdvancing = false;
    snapshot.playback.seekSerial = 2;
    snapshot.playback.seekLandedSerial = 2;
    client.processBinarySnapshot(snapshot);
    QVERIFY(!client.m_pendingSeek);
    QCOMPARE(clockPosition(client), 60.0);
    QCOMPARE(landed.count(), 1);
    QCOMPARE(landed.first().first().toDouble(), 60.0);

    // Later snapshots carry the same landing and report nothing more.
    snapshot = playbackSnapshot(path, 60.5, 1);
    snapshot.playback.seekSerial = 2;
    snapshot.playback.seekLandedSerial = 2;
    QVERIFY(client.processBinarySnapshot(snapshot));
    QVERIFY(std::abs(clockPosition(client) - 60.5) < 0.2);
    QVERIFY(client.playbackClock()->advancing());
    QCOMPARE(landed.count(), 1);
}

void BridgeClientTest::seekThatDoesNotLandReleasesThePlayhead() {
    BridgeClient client;
    isolateBridgeClient(client);
    acceptCommands(client);
    const QString path = QStringLiteral("/music/track.flac");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 20.0, 1)));
    QSignalSpy landed(&client, &BridgeClient::seekLanded);
    client.seek(90.0);
    QCOMPARE(clockPosition(client), 90.0);

    // The engine is done with the seek without applying it, such as after
    // a failure; playback goes on where it was.
    auto snapshot = playbackSnapshot(path, 20.5, 1);
    snapshot.playback.seekSerial = 1;
    client.processBinarySnapshot(snapshot);
    QVERIFY(!client.m_pendingSeek);
    QVERIFY(std::abs(clockPosition(client) - 20.5) < 0.2);
    QCOMPARE(landed.count(), 0);
}

void BridgeClientTest::seekLandingElsewhereReanchorsThePlayhead() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.append(payload);
        return true;
    };
    const QString path = QStringLiteral("/music/track.dts");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 10.0, 1)));
    SpectrogramItem item;
    item.setDisplayMode(1); // Centered
    item.m_precomputedReady = true;
    item.setPlaying(true);
    item.setClock(client.playbackClock());
    client.registerSpectrogramItem(&item, 0);

    client.seek(60.0);
    QCOMPARE(sent.size(), 1);
    QCOMPARE(clockPosition(client), 60.0);
    QCOMPARE(item.m_positionSeconds, 60.0);
    const quint64 epoch = client.playbackClock()->state().epoch;
    QSignalSpy landed(&client, &BridgeClient::seekLanded);

    // The seek landed on the key frame before the target.
    auto snapshot = playbackSnapshot(path, 58.5, 1);
    snapshot.playback.positionAdvancing = false;
    snapshot.playback.seekSerial = 1;
    snapshot.playback.seekLandedSerial = 1;
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(clockPosition(client), 58.5);
    QCOMPARE(client.playbackClock()->state().epoch, epoch + 1);
    QCOMPARE(item.m_positionSeconds, 58.5);
    QCOMPARE(landed.count(), 1);
    QCOMPARE(landed.first().first().toDouble(), 58.5);
}

void BridgeClientTest::seekPublishesOptimisticPositionAndExtendsPendingWindow() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    bool accept = true;
    client.m_sendCommandOverride = [&sent, &accept](const QByteArray &payload) {
        sent.append(payload);
        return accept;
    };
    const QString path = QStringLiteral("/music/track.flac");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 12.0, 1)));

    client.seek(96.25);
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first(),
             BinaryBridgeCodec::encodeCommandF64(BinaryBridgeCodec::CmdSeek, 96.25));
    // The clock shows the target at once, and holds it.
    QCOMPARE(clockPosition(client), 96.25);
    QCOMPARE(client.playbackClock()->positionText(), QStringLiteral("01:36"));
    QVERIFY(!client.playbackClock()->advancing());
    QVERIFY(client.m_pendingSeek);
    QCOMPARE(client.m_pendingSeekSerial, quint64(1));

    // Each seek sent gets the next serial, as the engine numbers them.
    client.seek(100.0);
    QCOMPARE(client.m_pendingSeekSerial, quint64(2));

    // A seek the bridge did not take moves nothing.
    accept = false;
    client.seek(120.0);
    QCOMPARE(sent.size(), 3);
    QCOMPARE(clockPosition(client), 100.0);
    QCOMPARE(client.m_pendingSeekSerial, quint64(2));
    QCOMPARE(client.m_seeksSent, quint64(2));

    // A stopped track is not seekable.
    accept = true;
    client.m_pendingSeek = false;
    client.m_playbackState = QStringLiteral("Stopped");
    client.seek(50.0);
    QCOMPARE(sent.size(), 3);
    QCOMPARE(clockPosition(client), 100.0);
    QVERIFY(!client.m_pendingSeek);
}

void BridgeClientTest::seekImmediatelyReanchorsRegisteredCenteredSpectrograms() {
    BridgeClient client;
    isolateBridgeClient(client);
    QVERIFY(client.processBinarySnapshot(
        playbackSnapshot(QStringLiteral("/music/track.flac"), 120.0, 1)));

    SpectrogramItem item;
    item.setDisplayMode(1); // Centered
    feedTrackData(item, 1);
    item.m_precomputedReady = true;
    item.setPlaying(true);
    item.setClock(client.playbackClock());
    client.registerSpectrogramItem(&item, 0);
    QVERIFY(item.m_positionSeconds > 100.0);

    // A seek moves the centered viewport and playhead without waiting for
    // reset data, even to the start, which a gapless switch also reports.
    acceptCommands(client);
    client.seek(0.0);
    QVERIFY(!item.m_positionJumpHoldActive);
    QCOMPARE(item.m_positionSeconds, 0.0);
}

void BridgeClientTest::stopSnapshotReanchorsCenteredSpectrogramsToBeginning() {
    BridgeClient client;
    isolateBridgeClient(client);
    const QString path = QStringLiteral("/music/track.flac");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 120.0, 1)));

    SpectrogramItem item;
    item.setDisplayMode(1); // Centered
    feedTrackData(item, 1);
    item.m_precomputedReady = true;
    item.setPlaying(true);
    item.setClock(client.playbackClock());
    client.registerSpectrogramItem(&item, 0);

    auto snapshot = playbackSnapshot(path, 0.0, 1);
    snapshot.playback.state = 0; // Stopped
    snapshot.playback.positionAdvancing = false;
    QVERIFY(client.processBinarySnapshot(snapshot));
    item.setPlaying(false);

    QCOMPARE(client.m_playbackState, QStringLiteral("Stopped"));
    QCOMPARE(clockPosition(client), 0.0);
    QVERIFY(!item.m_positionJumpHoldActive);
    QCOMPARE(item.m_positionSeconds, 0.0);

    // Playing again goes on from the beginning.
    item.setPlaying(true);
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 0.08, 1)));
    QVERIFY(!item.m_positionJumpHoldActive);
    QVERIFY(item.m_positionSeconds < 1.0);
}

void BridgeClientTest::stopSnapshotLeavesRollingSpectrogramsParked() {
    BridgeClient client;
    isolateBridgeClient(client);
    const QString path = QStringLiteral("/music/track.flac");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 120.0, 1)));

    SpectrogramItem item;
    item.setDisplayMode(0); // Rolling
    feedTrackData(item, 1);
    item.m_precomputedReady = true;
    item.setPlaying(true);
    item.setClock(client.playbackClock());
    client.registerSpectrogramItem(&item, 0);

    auto snapshot = playbackSnapshot(path, 0.0, 1);
    snapshot.playback.state = 0; // Stopped
    snapshot.playback.positionAdvancing = false;
    QVERIFY(client.processBinarySnapshot(snapshot));
    item.setPlaying(false);

    // Rolling rings are write-order history; the decode worker restart on
    // the next play re-anchors them, so the playhead stays where it was.
    QVERIFY(item.m_positionJumpHoldActive);
    const double parked = item.m_positionSeconds;
    QVERIFY(parked >= 120.0);
    QCOMPARE(
        item.currentRenderPositionSecondsLocked(
            std::chrono::steady_clock::now() + std::chrono::seconds(5)),
        parked);
}

void BridgeClientTest::stopSnapshotClearsPendingSeekWindow() {
    BridgeClient client;
    isolateBridgeClient(client);
    acceptCommands(client);
    const QString path = QStringLiteral("/music/track.flac");
    QVERIFY(client.processBinarySnapshot(playbackSnapshot(path, 20.0, 1)));

    SpectrogramItem item;
    item.setDisplayMode(1); // Centered
    item.m_precomputedReady = true;
    item.setPlaying(true);
    item.setClock(client.playbackClock());
    client.registerSpectrogramItem(&item, 0);
    client.seek(96.0);
    QCOMPARE(item.m_positionSeconds, 96.0);

    auto snapshot = playbackSnapshot(path, 0.0, 1);
    snapshot.playback.state = 0; // Stopped
    snapshot.playback.positionAdvancing = false;
    QVERIFY(client.processBinarySnapshot(snapshot));

    // The stop ends the pending seek, whose target no longer applies.
    QVERIFY(!client.m_pendingSeek);
    QCOMPARE(clockPosition(client), 0.0);
    QCOMPARE(item.m_positionSeconds, 0.0);
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

void BridgeClientTest::asyncImageFileDetailsAcceptsImageProviderUrl() {
    BridgeClient client;
    isolateBridgeClient(client);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString sourcePath = tempDir.filePath(QStringLiteral("front cover.png"));
    QImage image(80, 60, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(image.save(sourcePath, "PNG"));

    const QString providerUrl = CoverImageProvider::urlForPath(sourcePath)
        + QStringLiteral("#w=600&r=3");
    QSignalSpy detailsSpy(&client, SIGNAL(imageFileDetailsChanged(QString)));

    client.requestImageFileDetails(providerUrl);

    QTRY_VERIFY_WITH_TIMEOUT(detailsSpy.count() > 0, 3000);

    const QVariantMap byLocalPath = client.cachedImageFileDetails(sourcePath);
    const QVariantMap byProviderUrl = client.cachedImageFileDetails(providerUrl);
    QCOMPARE(byLocalPath.value(QStringLiteral("fileName")).toString(), QStringLiteral("front cover.png"));
    QCOMPARE(byLocalPath.value(QStringLiteral("width")).toInt(), 80);
    QCOMPARE(byLocalPath.value(QStringLiteral("height")).toInt(), 60);
    QCOMPARE(byProviderUrl.value(QStringLiteral("fileName")).toString(), QStringLiteral("front cover.png"));
}

void BridgeClientTest::itunesRectangularArtworkRowUsesNormalizedFileDetails() {
    BridgeClient client;
    isolateBridgeClient(client);

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    client.m_itunesArtworkTempDir =
        std::make_unique<QTemporaryDir>(tempDir.filePath(QStringLiteral("itunes-artwork-XXXXXX")));
    QVERIFY(client.m_itunesArtworkTempDir->isValid());

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
    client.m_itunesArtworkTempDir =
        std::make_unique<QTemporaryDir>(tempDir.filePath(QStringLiteral("itunes-artwork-XXXXXX")));
    QVERIFY(client.m_itunesArtworkTempDir->isValid());

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

namespace {

BinaryBridgeCodec::DecodedSnapshot playingWithCover(
    const QString &trackPath, const QString &coverPath, quint64 coverStamp) {
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1;
    snapshot.playback.currentPath = trackPath;
    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = trackPath;
    snapshot.metadata.coverPath = coverPath;
    snapshot.metadata.coverStamp = coverStamp;
    return snapshot;
}

} // namespace

void BridgeClientTest::trackCoverIsTheBackendsChoiceWithItsStamp() {
    BridgeClient client;
    isolateBridgeClient(client);
    const QString track = QStringLiteral("/music/Album/01.flac");
    const QString cover = QStringLiteral("/music/Album/cover.jpg");

    QVERIFY(client.processBinarySnapshot(playingWithCover(track, cover, 5)));
    QCOMPARE(client.currentTrackCoverPath(),
             QStringLiteral("image://covers//music/Album/cover.jpg#v=5"));

    // The image was replaced at the same path: a new URL, which QML loads.
    client.m_pollTrackMetadataChanged = false;
    client.m_pollTrackIdentityChanged = false;
    client.m_pollSnapshotChanged = false;
    QVERIFY(client.processBinarySnapshot(playingWithCover(track, cover, 6)));
    QCOMPARE(client.currentTrackCoverPath(),
             QStringLiteral("image://covers//music/Album/cover.jpg#v=6"));
    QVERIFY(client.m_pollTrackMetadataChanged);
    QVERIFY(!client.m_pollTrackIdentityChanged);
    QVERIFY(!client.m_pollSnapshotChanged);
}

void BridgeClientTest::trackCoverHasNoFallbackOfTheUisOwn() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Images the backend's rule does not take, which the UI used to.
    for (const QString &name : {QStringLiteral("cover.webp"), QStringLiteral("albumart.bmp")}) {
        QImage image(4, 4, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(dir.filePath(name), name.endsWith(QStringLiteral("webp")) ? "PNG" : "BMP"));
    }
    const QString first = dir.filePath(QStringLiteral("01.flac"));
    const QString second = dir.filePath(QStringLiteral("02.flac"));
    const QString cover = dir.filePath(QStringLiteral("front.jpg"));
    BridgeClient client;
    isolateBridgeClient(client);
    QVERIFY(client.processBinarySnapshot(playingWithCover(first, cover, 1)));
    QVERIFY(!client.currentTrackCoverPath().isEmpty());

    // The backend now finds no cover, for this track and the next one of the
    // folder: the UI shows none rather than one it chose or kept itself.
    QVERIFY(client.processBinarySnapshot(playingWithCover(first, QString(), 0)));
    QCOMPARE(client.currentTrackCoverPath(), QString());
    QVERIFY(client.processBinarySnapshot(playingWithCover(second, QString(), 0)));
    QCOMPARE(client.currentTrackCoverPath(), QString());
    QCoreApplication::processEvents();
    QCOMPARE(client.currentTrackCoverPath(), QString());
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


void BridgeClientTest::mprisCanPauseWhilePaused() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 1;
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");

    MprisController controller(&client);

    client.m_playbackState = QStringLiteral("Stopped");
    QCOMPARE(controller.canPause(), false);

    client.m_playbackState = QStringLiteral("Paused");
    QCOMPARE(controller.canPause(), true);

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


namespace {

QString publishedArtUrl(const MprisController &controller) {
    return controller.m_lastPlayerState.metadata.value(QStringLiteral("mpris:artUrl")).toString();
}

// Waits for the controller's revision link, and returns the art it publishes.
QString artOnceLinked(MprisController &controller) {
    QSignalSpy ready(&controller.m_artLinks, &CoverRevisionLinks::linkReady);
    if (controller.m_artLinks.pending() && !ready.wait(5000)) {
        return {};
    }
    return publishedArtUrl(controller);
}

bool sentMetadataChange(const QList<QDBusMessage> &signalsSent) {
    return std::any_of(signalsSent.cbegin(), signalsSent.cend(), [](const QDBusMessage &message) {
        return message.member() == QStringLiteral("PropertiesChanged")
            && message.arguments().size() > 1
            && qdbus_cast<QVariantMap>(message.arguments().at(1))
                   .contains(QStringLiteral("Metadata"));
    });
}

} // namespace

void BridgeClientTest::mprisArtUrlConvertsImageProviderToFileUrl() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 1;
    client.m_playbackState = QStringLiteral("Playing");
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    // The async image provider URL of a stamped cover, in a folder whose
    // name has a '#'.
    client.m_currentTrackCoverPath =
        QStringLiteral("image://covers//music/a#b/cover.jpg#v=12");

    MprisController controller(&client);
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;

    emit client.trackMetadataChanged();

    QVERIFY(controller.m_hasPublishedPlayerState);
    // Until the revision's link exists, the cover file itself.
    QCOMPARE(publishedArtUrl(controller), QStringLiteral("file:///music/a%23b/cover.jpg"));
    const QString artUrl = artOnceLinked(controller);
    // Must be a file:// URL, not an image:// URL, and with no query or
    // fragment, which GNOME Shell drops or refuses.
    QVERIFY2(artUrl.startsWith(QStringLiteral("file://")),
             qPrintable(QStringLiteral("Expected file:// URL, got: ") + artUrl));
    const QUrl url(artUrl);
    QVERIFY(!url.hasQuery());
    QVERIFY(!url.hasFragment());
    const QFileInfo link(url.toLocalFile());
    QCOMPARE(link.absolutePath(), CoverRevisionLinks::defaultDirectory());
    QVERIFY(link.isSymLink());
    QCOMPARE(link.symLinkTarget(), QStringLiteral("/music/a#b/cover.jpg"));

    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}

void BridgeClientTest::mprisPublishesNewArtWhenTheCoverIsReplacedAtTheSamePath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cover = dir.filePath(QStringLiteral("cover.jpg"));
    QFile file(cover);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("first cover");
    file.close();

    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 1;
    const QString track = dir.filePath(QStringLiteral("01.flac"));
    QVERIFY(client.processBinarySnapshot(playingWithCover(track, cover, 1)));

    MprisController controller(&client);
    QList<QDBusMessage> signalsSent;
    controller.m_signalSink = [&signalsSent](const QDBusMessage &message) {
        signalsSent.append(message);
    };
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;
    emit client.trackMetadataChanged();
    const QString firstArt = artOnceLinked(controller);
    QVERIFY(QUrl(firstArt).toLocalFile().startsWith(CoverRevisionLinks::defaultDirectory()));
    signalsSent.clear();

    // The image is replaced at the same path, and the backend gives it stamp 2.
    QVERIFY(client.processBinarySnapshot(playingWithCover(track, cover, 2)));
    emit client.trackMetadataChanged();
    QCOMPARE(publishedArtUrl(controller), firstArt);
    const QString secondArt = artOnceLinked(controller);

    // A path of its own, which GNOME Shell's file comparison sees as new art.
    QVERIFY(QUrl(secondArt).toLocalFile() != QUrl(firstArt).toLocalFile());
    QCOMPARE(QFileInfo(QUrl(secondArt).toLocalFile()).symLinkTarget(), cover);
    QVERIFY2(sentMetadataChange(signalsSent), "the new art is published");
    // The cover itself is left as it was.
    QVERIFY(!QFileInfo(cover).isSymLink());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("first cover"));

    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}

void BridgeClientTest::coverRevisionLinksAreMadeOncePerStampAndOldOnesRemoved() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString linkDir = dir.filePath(QStringLiteral("cache/ferrous/mpris_art"));
    const QString cover = dir.filePath(QStringLiteral("Album/Cover.JPG"));
    QVERIFY(QDir().mkpath(QFileInfo(cover).absolutePath()));
    QFile coverFile(cover);
    QVERIFY(coverFile.open(QIODevice::WriteOnly));
    coverFile.write("cover");
    coverFile.close();

    CoverRevisionLinks links(linkDir);
    QSignalSpy ready(&links, &CoverRevisionLinks::linkReady);
    QSignalSpy failed(&links, &CoverRevisionLinks::linkFailed);
    QStringList made;
    for (quint64 stamp = 1; stamp <= 7; ++stamp) {
        QCOMPARE(links.linkFor(cover, stamp), QString());
        QVERIFY(links.pending());
        QVERIFY(ready.wait(5000));
        const QString link = links.linkFor(cover, stamp);
        QVERIFY(!link.isEmpty());
        QVERIFY(!made.contains(link));
        QCOMPARE(QFileInfo(link).symLinkTarget(), cover);
        QVERIFY(link.endsWith(QStringLiteral("-%1.jpg").arg(stamp)));
        made.append(link);
        if (stamp == 1) {
            // Files Ferrous did not make there stay, whatever their names.
            QFile foreign(QDir(linkDir).filePath(QFileInfo(link).fileName().replace(
                QStringLiteral("-1.jpg"), QStringLiteral("-0.jpg"))));
            QVERIFY(foreign.open(QIODevice::WriteOnly));
            foreign.close();
            QFile notes(QDir(linkDir).filePath(QStringLiteral("notes.txt")));
            QVERIFY(notes.open(QIODevice::WriteOnly));
            notes.close();
        }
    }
    QCOMPARE(failed.count(), 0);

    // The newest links stay, so a consumer reading the previous one finds it.
    const QStringList names = QDir(linkDir).entryList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
    QStringList expected;
    for (qsizetype index = made.size() - CoverRevisionLinks::kKeptLinks; index < made.size(); ++index) {
        expected.append(QFileInfo(made.at(index)).fileName());
    }
    expected.append(QFileInfo(made.first()).fileName().replace(
        QStringLiteral("-1.jpg"), QStringLiteral("-0.jpg")));
    expected.append(QStringLiteral("notes.txt"));
    expected.sort();
    QCOMPARE(names, expected);
    QVERIFY(QFileInfo(cover).isFile() && !QFileInfo(cover).isSymLink());

    // Asking again for a revision that exists needs no work.
    QCOMPARE(links.linkFor(cover, 7), made.last());
    QVERIFY(!links.pending());
}

void BridgeClientTest::mprisMetadataIsRebuiltOnlyForTrackChanges() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 1;
    client.m_playbackState = QStringLiteral("Playing");
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    client.m_currentTrackTitle = QStringLiteral("First");
    client.m_durationSeconds = 100.0;

    MprisController controller(&client);
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;
    emit client.playbackChanged();
    QCOMPARE(controller.m_lastPlayerState.metadata.value(QStringLiteral("xesam:title")).toString(),
             QStringLiteral("First"));
    QCOMPARE(controller.m_lastPlayerState.metadata.value(QStringLiteral("mpris:length")).toLongLong(),
             100000000LL);

    // Playback heartbeats reuse the cached map instead of rebuilding it.
    client.m_currentTrackTitle = QStringLiteral("Unannounced");
    client.playbackClock()->holdAt(12.0);
    emit client.playbackChanged();
    QCOMPARE(controller.metadata().value(QStringLiteral("xesam:title")).toString(),
             QStringLiteral("First"));

    // A new length is still published.
    client.m_durationSeconds = 200.0;
    emit client.playbackChanged();
    QCOMPARE(controller.m_lastPlayerState.metadata.value(QStringLiteral("mpris:length")).toLongLong(),
             200000000LL);

    client.m_currentTrackTitle = QStringLiteral("Second");
    emit client.trackMetadataChanged();
    QCOMPARE(controller.m_lastPlayerState.metadata.value(QStringLiteral("xesam:title")).toString(),
             QStringLiteral("Second"));

    const QString firstTrackId = controller.currentTrackIdPath();
    client.m_currentTrackPath = QStringLiteral("/music/other.flac");
    emit client.trackIdentityChanged();
    QVERIFY(controller.currentTrackIdPath() != firstTrackId);
    QCOMPARE(
        controller.m_lastPlayerState.metadata.value(QStringLiteral("mpris:trackid"))
            .value<QDBusObjectPath>().path(),
        controller.currentTrackIdPath());

    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}

void BridgeClientTest::mprisAlbumArtistComesFromAlbumArtistTag() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_queueLength = 1;
    client.m_currentTrackPath = QStringLiteral("/music/concerto.flac");
    client.m_currentTrackArtist = QStringLiteral("Soloist");
    client.m_currentTrackAlbumArtist = QStringLiteral("Orchestra");

    MprisController controller(&client);
    QVariantMap meta = controller.metadata();
    QCOMPARE(meta.value(QStringLiteral("xesam:artist")).toStringList(), QStringList{QStringLiteral("Soloist")});
    QCOMPARE(meta.value(QStringLiteral("xesam:albumArtist")).toStringList(), QStringList{QStringLiteral("Orchestra")});

    // Without an album artist tag the field is left out, not filled with the track artist.
    client.m_currentTrackAlbumArtist.clear();
    emit client.trackMetadataChanged();
    meta = controller.metadata();
    QVERIFY(!meta.contains(QStringLiteral("xesam:albumArtist")));
    QCOMPARE(meta.value(QStringLiteral("xesam:artist")).toStringList(), QStringList{QStringLiteral("Soloist")});
}

void BridgeClientTest::albumArtistFollowsMatchingMetadataAcrossTracks() {
    BridgeClient client;
    isolateBridgeClient(client);
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1;
    snapshot.playback.currentPath = QStringLiteral("/fixture/first.flac");
    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = snapshot.playback.currentPath;
    snapshot.metadata.artist = QStringLiteral("Soloist");
    snapshot.metadata.albumArtist = QStringLiteral("Orchestra");
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.currentTrackAlbumArtist(), QStringLiteral("Orchestra"));

    // Metadata still describes the previous track: do not carry its album artist over.
    snapshot.playback.currentPath = QStringLiteral("/fixture/next.flac");
    QVERIFY(client.processBinarySnapshot(snapshot));
    QVERIFY(client.currentTrackAlbumArtist().isEmpty());

    snapshot.metadata.sourcePath = snapshot.playback.currentPath;
    snapshot.metadata.albumArtist = QStringLiteral("Ensemble");
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.currentTrackAlbumArtist(), QStringLiteral("Ensemble"));

    // The decoder reads the album artist between album and genre.
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    auto appendText = [&stream](const QByteArray &value) {
        stream << quint32(value.size());
        stream.writeRawData(value.constData(), value.size());
    };
    for (const QByteArray &text : {QByteArray("/fixture/a.flac"), QByteArray("Title"), QByteArray("Artist"),
                                   QByteArray("Album"), QByteArray("Album Artist"), QByteArray("Genre")}) {
        appendText(text);
    }
    stream << qint32(2001) << quint32(44100) << quint32(0) << quint16(2) << quint16(16);
    appendText("FLAC");
    stream << quint32(0);
    appendText("/fixture/cover.jpg");
    stream << quint64(0x0102030405060708ULL); // cover stamp, after the cover path
    stream << quint32(0);
    QByteArray packet;
    QDataStream header(&packet, QIODevice::WriteOnly);
    header.setByteOrder(QDataStream::LittleEndian);
    header << quint32(0xFE550001u) << quint32(16 + payload.size())
           << quint16(BinaryBridgeCodec::SectionMetadata) << quint16(0)
           << quint32(payload.size());
    packet.append(payload);
    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(packet, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.metadata.albumArtist, QStringLiteral("Album Artist"));
    QCOMPARE(decoded.metadata.genre, QStringLiteral("Genre"));
    QCOMPARE(decoded.metadata.year, 2001);
    QCOMPARE(decoded.metadata.coverPath, QStringLiteral("/fixture/cover.jpg"));
    QCOMPARE(decoded.metadata.coverStamp, quint64(0x0102030405060708ULL));
}

void BridgeClientTest::mprisPositionReadsThePlaybackClock() {
    BridgeClient client;
    isolateBridgeClient(client);
    acceptCommands(client);
    client.m_queueLength = 1;
    // The engine read 10 s two seconds ago, and playback went on.
    auto snapshot = playbackSnapshot(QStringLiteral("/music/track.flac"), 10.0, 1);
    snapshot.playback.positionSampledAtNs = monotonicNowNs() - 2'000'000'000LL;
    QVERIFY(client.processBinarySnapshot(snapshot));

    MprisController controller(&client);
    const double position = static_cast<double>(controller.positionUs()) / 1e6;
    QVERIFY2(position >= 12.0 && position < 12.5, qPrintable(QString::number(position)));

    // A relative seek starts from there too.
    controller.seek(5'000'000);
    QVERIFY(client.m_pendingSeek);
    QVERIFY(client.m_pendingSeekTargetSeconds >= 17.0);
    QVERIFY(client.m_pendingSeekTargetSeconds < 17.5);
    QCOMPARE(controller.positionUs(),
             static_cast<qlonglong>(std::llround(client.m_pendingSeekTargetSeconds * 1e6)));
}

void BridgeClientTest::mprisRateWritePausesOnlyForZero() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    client.m_playbackState = QStringLiteral("Playing");
    MprisController controller(&client);
    // The isolated client reports every command it tries to send as a bridge error.
    QSignalSpy sentCommands(&client, &BridgeClient::bridgeError);

    controller.setRate(1.5);
    controller.setRate(1.0);
    QCOMPARE(sentCommands.count(), 0);
    QCOMPARE(controller.rate(), 1.0);

    controller.setRate(0.0);
    QCOMPARE(sentCommands.count(), 1);
    QCOMPARE(controller.rate(), 1.0);

    // The D-Bus player interface exposes Rate as writable and has OpenUri.
    const QMetaObject *playerMeta = nullptr;
    for (const QDBusAbstractAdaptor *adaptor : controller.findChildren<QDBusAbstractAdaptor *>()) {
        const QMetaObject *meta = adaptor->metaObject();
        const int info = meta->indexOfClassInfo("D-Bus Interface");
        if (info >= 0 && QByteArray(meta->classInfo(info).value()) == "org.mpris.MediaPlayer2.Player") {
            playerMeta = meta;
        }
    }
    QVERIFY(playerMeta != nullptr);
    QVERIFY(playerMeta->property(playerMeta->indexOfProperty("Rate")).isWritable());
    QVERIFY(playerMeta->indexOfSlot("OpenUri(QString)") >= 0);
}

void BridgeClientTest::mprisOpenUriOpensOnlyLocalFiles() {
    QCOMPARE(MprisController::localPathForOpenUri(QStringLiteral("file:///music/A%20Song.flac")),
             QStringLiteral("/music/A Song.flac"));
    QCOMPARE(MprisController::localPathForOpenUri(QStringLiteral("  file:///music/album/  ")),
             QStringLiteral("/music/album/"));
    QVERIFY(MprisController::localPathForOpenUri(QStringLiteral("https://example.com/a.mp3")).isEmpty());
    QVERIFY(MprisController::localPathForOpenUri(QStringLiteral("/music/plain-path.flac")).isEmpty());
    QVERIFY(MprisController::localPathForOpenUri(QStringLiteral("file:")).isEmpty());

    BridgeClient client;
    isolateBridgeClient(client);
    MprisController controller(&client);
    QSignalSpy sentCommands(&client, &BridgeClient::bridgeError);
    QVERIFY(!controller.openUri(QStringLiteral("https://example.com/a.mp3")));
    QCOMPARE(sentCommands.count(), 0);
    QVERIFY(controller.openUri(QStringLiteral("file:///music/track.flac")));
    QCOMPARE(sentCommands.count(), 1);
}

namespace {
// The positions of the MPRIS Seeked signals among `messages`.
QList<qlonglong> seekedPositions(const QList<QDBusMessage> &messages) {
    QList<qlonglong> positions;
    for (const QDBusMessage &message : messages) {
        if (message.member() == QStringLiteral("Seeked")) {
            positions.append(message.arguments().value(0).toLongLong());
        }
    }
    return positions;
}

BinaryBridgeCodec::DecodedSnapshot playingSnapshot(const QString &path, double position) {
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1;
    snapshot.playback.currentPath = path;
    snapshot.playback.positionSeconds = position;
    snapshot.playback.durationSeconds = 180.0;
    return snapshot;
}
} // namespace

void BridgeClientTest::mprisSeekedReportsEachSeekWhereItLanded() {
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_sendCommandOverride = [](const QByteArray &) { return true; };
    const QString path = QStringLiteral("/music/track.flac");
    QVERIFY(client.processBinarySnapshot(playingSnapshot(path, 10.0)));

    MprisController controller(&client);
    QList<QDBusMessage> signalsSent;
    controller.m_signalSink = [&signalsSent](const QDBusMessage &message) {
        signalsSent.append(message);
    };
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;
    emit client.playbackChanged();
    QVERIFY(controller.m_hasPublishedPlayerState);

    // A seek of 0.2 s, too small to tell from playback by position.
    client.seek(10.2);
    emit client.playbackChanged();
    QVERIFY(seekedPositions(signalsSent).isEmpty());
    auto landed = playingSnapshot(path, 10.2);
    landed.playback.seekSerial = 1;
    landed.playback.seekLandedSerial = 1;
    client.processBinarySnapshot(landed);
    QCOMPARE(seekedPositions(signalsSent), QList<qlonglong>{10200000});

    // A burst: the first seek landed before the second was handled.
    client.seek(40.0);
    client.seek(70.0);
    auto stale = playingSnapshot(path, 40.0);
    stale.playback.seekSerial = 2;
    stale.playback.seekLandedSerial = 2;
    client.processBinarySnapshot(stale);
    QCOMPARE(seekedPositions(signalsSent).size(), 1);
    QCOMPARE(clockPosition(client), 70.0);
    auto last = playingSnapshot(path, 70.0);
    last.playback.seekSerial = 3;
    last.playback.seekLandedSerial = 3;
    client.processBinarySnapshot(last);
    QCOMPARE(seekedPositions(signalsSent), (QList<qlonglong>{10200000, 70000000}));

    // Playback that only moves on reports no seek.
    auto later = playingSnapshot(path, 75.0);
    later.playback.seekSerial = 3;
    later.playback.seekLandedSerial = 3;
    QVERIFY(client.processBinarySnapshot(later));
    emit client.playbackChanged();
    QCOMPARE(seekedPositions(signalsSent).size(), 2);

    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}

void BridgeClientTest::mprisCannotSeekAStoppedTrack() {
    BridgeClient client;
    isolateBridgeClient(client);
    QList<QByteArray> sent;
    client.m_sendCommandOverride = [&sent](const QByteArray &payload) {
        sent.append(payload);
        return true;
    };
    client.m_queueLength = 1;
    client.m_currentTrackPath = QStringLiteral("/music/track.flac");
    client.m_durationSeconds = 180.0;
    client.m_playbackState = QStringLiteral("Stopped");

    MprisController controller(&client);
    QList<QDBusMessage> signalsSent;
    controller.m_signalSink = [&signalsSent](const QDBusMessage &message) {
        signalsSent.append(message);
    };
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;
    emit client.playbackChanged();
    QVERIFY(!controller.canSeek());
    QVERIFY(!controller.m_lastPlayerState.canSeek);

    controller.seek(5000000);
    controller.setPosition(controller.currentTrackIdPath(), 5000000);
    QVERIFY(sent.isEmpty());
    QCOMPARE(clockPosition(client), 0.0);

    // Playing, it can seek, and clients are told.
    signalsSent.clear();
    client.m_playbackState = QStringLiteral("Playing");
    emit client.playbackChanged();
    QVERIFY(controller.canSeek());
    bool announced = false;
    for (const QDBusMessage &message : std::as_const(signalsSent)) {
        const QVariantMap changed = message.arguments().value(1).toMap();
        announced |= message.member() == QStringLiteral("PropertiesChanged")
            && changed.value(QStringLiteral("CanSeek")).toBool();
    }
    QVERIFY(announced);
    controller.seek(5000000);
    QCOMPARE(sent.size(), 1);

    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}

void BridgeClientTest::mprisReportsAReplayOfTheSameFileAsSeeked() {
    BridgeClient client;
    isolateBridgeClient(client);
    const QString path = QStringLiteral("/music/track.flac");
    auto snapshot = playingSnapshot(path, 179.5);
    snapshot.playback.trackToken = 1;
    QVERIFY(client.processBinarySnapshot(snapshot));

    MprisController controller(&client);
    QList<QDBusMessage> signalsSent;
    controller.m_signalSink = [&signalsSent](const QDBusMessage &message) {
        signalsSent.append(message);
    };
    controller.m_serviceRegistered = true;
    controller.m_hasPublishedPlayerState = false;
    emit client.playbackChanged();

    // Repeat-One plays the file again: same track ID, position from 0.
    snapshot = playingSnapshot(path, 0.05);
    snapshot.playback.trackToken = 2;
    QVERIFY(client.processBinarySnapshot(snapshot));
    emit client.trackIdentityChanged();
    QCOMPARE(seekedPositions(signalsSent), QList<qlonglong>{50000});

    // Another file is a track change, not a seek.
    snapshot = playingSnapshot(QStringLiteral("/music/other.flac"), 0.0);
    snapshot.playback.trackToken = 3;
    QVERIFY(client.processBinarySnapshot(snapshot));
    emit client.trackIdentityChanged();
    QCOMPARE(seekedPositions(signalsSent).size(), 1);

    controller.m_serviceRegistered = false;
    controller.m_objectRegistered = false;
}

void BridgeClientTest::playbackErrorPacketReachesBridgeError() {
    // Mirrored by ffi::error_packet_tests::playback_error_packet_layout_matches_the_qt_decoder.
    BridgeClient client;
    isolateBridgeClient(client);
    const QString message = QStringLiteral(
        "Couldn't play \u201cbad.flac\u201d (Resource not found); skipped it.");
    const QString subject = QStringLiteral("/music/bad.flac");

    // The packet the Rust bridge encodes for an error event: the header,
    // then the error section with the category, the severity, and the
    // subject and the message as u32-length UTF-8 strings.
    const auto packetWith = [&](quint8 category, quint8 severity) {
        const QByteArray text = message.toUtf8();
        const QByteArray subjectText = subject.toUtf8();
        QByteArray payload;
        QDataStream payloadStream(&payload, QIODevice::WriteOnly);
        payloadStream.setByteOrder(QDataStream::LittleEndian);
        payloadStream << category << severity << quint32(subjectText.size());
        payloadStream.writeRawData(subjectText.constData(), static_cast<int>(subjectText.size()));
        payloadStream << quint32(text.size());
        payloadStream.writeRawData(text.constData(), static_cast<int>(text.size()));
        QByteArray packet;
        QDataStream header(&packet, QIODevice::WriteOnly);
        header.setByteOrder(QDataStream::LittleEndian);
        header << quint32(0xFE550001u) << quint32(16 + payload.size())
               << quint16(BinaryBridgeCodec::SectionError) << quint16(0)
               << quint32(payload.size());
        packet.append(payload);
        return packet;
    };

    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(
                 packetWith(BinaryBridgeCodec::ErrorCategoryPlayback,
                            BinaryBridgeCodec::ErrorSeverityWarning),
                 &decoded, &error),
             qPrintable(error));
    QSignalSpy errors(&client, &BridgeClient::bridgeError);
    QVERIFY(!client.processBinarySnapshot(decoded));
    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors.at(0).at(0).toString(), message);
    QCOMPARE(errors.at(0).at(1).toInt(), int(BinaryBridgeCodec::ErrorCategoryPlayback));
    QCOMPARE(errors.at(0).at(2).toInt(), int(BinaryBridgeCodec::ErrorSeverityWarning));
    QCOMPARE(errors.at(0).at(3).toString(), subject);

    // Every category and severity Rust sends decodes; values past them do not.
    for (quint8 category = 0; category <= BinaryBridgeCodec::ErrorCategoryLast; ++category) {
        decoded = {};
        QVERIFY(BinaryBridgeCodec::decodeSnapshotPacket(
            packetWith(category, BinaryBridgeCodec::ErrorSeverityError), &decoded, &error));
        QCOMPARE(decoded.errorCategory, category);
        QCOMPARE(decoded.errorSeverity, quint8(BinaryBridgeCodec::ErrorSeverityError));
    }
    QCOMPARE(int(BinaryBridgeCodec::ErrorCategoryLast), 6);
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(
        packetWith(BinaryBridgeCodec::ErrorCategoryLast + 1, 0), &decoded, &error));
    QVERIFY(!BinaryBridgeCodec::decodeSnapshotPacket(packetWith(0, 2), &decoded, &error));
}

namespace {

// The packet the Rust relay encodes when the backend stops without being asked
// to: the error section with the reason, then an empty stopped section.
QByteArray backendFailurePacket(const QString &message) {
    const QByteArray text = message.toUtf8();
    QByteArray payload;
    QDataStream payloadStream(&payload, QIODevice::WriteOnly);
    payloadStream.setByteOrder(QDataStream::LittleEndian);
    // A system error with no subject.
    payloadStream << quint8(0) << quint8(1) << quint32(0) << quint32(text.size());
    payload.append(text);
    QByteArray packet;
    QDataStream stream(&packet, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint32(0xFE550001u) << quint32(20 + payload.size())
           << quint16(BinaryBridgeCodec::SectionError | BinaryBridgeCodec::SectionStopped)
           << quint16(0) << quint32(payload.size());
    stream.writeRawData(payload.constData(), static_cast<int>(payload.size()));
    stream << quint32(0);
    return packet;
}

} // namespace

void BridgeClientTest::backendFailurePacketShowsStoppedState() {
    // Mirrored by ffi::error_packet_tests::failure_packet_layout_matches_the_qt_decoder.
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_bridgeShutdownRequested = false;
    client.m_connected = true;
    const QString message = QStringLiteral("The Ferrous backend stopped.");

    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(
        BinaryBridgeCodec::decodeSnapshotPacket(backendFailurePacket(message), &decoded, &error),
        qPrintable(error));
    QSignalSpy failures(&client, &BridgeClient::backendFailureChanged);
    QSignalSpy connection(&client, &BridgeClient::connectedChanged);
    QVERIFY(!client.processBinarySnapshot(decoded));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(client.backendFailure(), message);
    QCOMPARE(client.property("backendFailure").toString(), message);
    QCOMPARE(connection.count(), 1);
    QVERIFY(!client.connected());

    // The first reason stays.
    QVERIFY(BinaryBridgeCodec::decodeSnapshotPacket(
        backendFailurePacket(QStringLiteral("later")), &decoded, &error));
    QVERIFY(!client.processBinarySnapshot(decoded));
    QCOMPARE(failures.count(), 1);
    QCOMPARE(client.backendFailure(), message);
}

void BridgeClientTest::requestedStopIsNotABackendFailure() {
    BridgeClient client;
    isolateBridgeClient(client);
    QVERIFY(client.m_bridgeShutdownRequested);
    BinaryBridgeCodec::DecodedSnapshot decoded;
    decoded.hasStopped = true;
    QSignalSpy failures(&client, &BridgeClient::backendFailureChanged);
    QVERIFY(!client.processBinarySnapshot(decoded));
    QCOMPARE(failures.count(), 0);
    QVERIFY(client.backendFailure().isEmpty());
}

void BridgeClientTest::commandsToAStoppedBackendFailVisibly() {
    BridgeClient client;
    QVERIFY(client.m_ffiBridge != nullptr);
    QSignalSpy errors(&client, &BridgeClient::bridgeError);

    // Stop the backend behind the client's back, as if it had ended on its own.
    const QByteArray shutdown =
        BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdShutdown);
    QVERIFY(ferrous_ffi_bridge_send_binary(
        client.m_ffiBridge,
        reinterpret_cast<const std::uint8_t *>(shutdown.constData()),
        static_cast<std::size_t>(shutdown.size())));
    QTRY_VERIFY_WITH_TIMEOUT(!client.backendFailure().isEmpty(), 10000);
    QVERIFY(!client.connected());
    QVERIFY(client.backendFailure().contains(QStringLiteral("stopped unexpectedly")));

    // Once the backend thread has ended, commands fail and say why.
    const QByteArray snapshot =
        BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdRequestSnapshot);
    QTRY_VERIFY_WITH_TIMEOUT(!client.sendBinaryCommand(snapshot), 10000);
    QVERIFY(!errors.isEmpty());
    QCOMPARE(
        errors.constLast().at(0).toString(),
        QStringLiteral("command not sent: the backend has stopped"));
}

void BridgeClientTest::destroyingTheClientWaitsForTheBackendsFinalSave() {
    const QDir configDir(QDir(qEnvironmentVariable("XDG_CONFIG_HOME")).filePath(QStringLiteral("ferrous")));
    // Back to back, as consecutive tests and app runs save the same file.
    for (const double dbRange : {111.0, 117.0}) {
        auto client = std::make_unique<BridgeClient>();
        QVERIFY(client->m_ffiBridge != nullptr);
        client->setDbRange(dbRange);
        client.reset();

        QFile settings(configDir.filePath(QStringLiteral("settings.txt")));
        QVERIFY(settings.open(QIODevice::ReadOnly));
        const QByteArray saved = settings.readAll();
        QVERIFY2(saved.contains(QStringLiteral("db_range=%1\n").arg(dbRange, 0, 'f', 2).toUtf8()),
            saved.constData());
    }
    const QStringList leftovers = configDir.entryList(
        {QStringLiteral("*.tmp")}, QDir::Files | QDir::Hidden);
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QStringLiteral(", "))));
}

void BridgeClientTest::saveBeforeQuitReportsAFailedSaveAndSavesOnRetry() {
    QDir configDir(QDir(qEnvironmentVariable("XDG_CONFIG_HOME")).filePath(QStringLiteral("ferrous")));
    const QString settingsPath = configDir.filePath(QStringLiteral("settings.txt"));
    BridgeClient client;
    QVERIFY(client.m_ffiBridge != nullptr);
    QSignalSpy finished(&client, &BridgeClient::stateSaveFinished);

    // A second request sent before the first is answered: each answer
    // carries the ID of its own request.
    const quint32 first = client.saveStateBeforeQuit();
    const quint32 second = client.saveStateBeforeQuit();
    QVERIFY(first != 0);
    QVERIFY(second != first);
    QTRY_COMPARE(finished.count(), 2);
    QCOMPARE(finished.at(0).at(0).value<quint32>(), first);
    QVERIFY2(finished.at(0).at(1).toBool(), qPrintable(finished.at(0).at(2).toString()));
    QCOMPARE(finished.at(1).at(0).value<quint32>(), second);
    QVERIFY2(finished.at(1).at(1).toBool(), qPrintable(finished.at(1).at(2).toString()));
    QVERIFY(QFileInfo(settingsPath).isFile());
    finished.clear();

    // Nothing can be renamed over a directory, as when the disk is full.
    QVERIFY(QFile::remove(settingsPath));
    QVERIFY(configDir.mkdir(QStringLiteral("settings.txt")));
    const auto unblock = qScopeGuard([&configDir]() {
        configDir.rmdir(QStringLiteral("settings.txt"));
    });
    client.setDbRange(103.0);
    const quint32 failing = client.saveStateBeforeQuit();
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(0).value<quint32>(), failing);
    QVERIFY(!finished.at(0).at(1).toBool());
    const QString message = finished.at(0).at(2).toString();
    QVERIFY2(message.startsWith(QStringLiteral("Settings could not be saved")), qPrintable(message));
    QVERIFY2(message.contains(settingsPath), qPrintable(message));

    // Retry once the cause is gone.
    QVERIFY(configDir.rmdir(QStringLiteral("settings.txt")));
    const quint32 retry = client.saveStateBeforeQuit();
    QTRY_COMPARE(finished.count(), 2);
    QCOMPARE(finished.at(1).at(0).value<quint32>(), retry);
    QVERIFY2(finished.at(1).at(1).toBool(), qPrintable(finished.at(1).at(2).toString()));
    QFile settings(settingsPath);
    QVERIFY(settings.open(QIODevice::ReadOnly));
    const QByteArray saved = settings.readAll();
    QVERIFY2(saved.contains("db_range=103.00\n"), saved.constData());
}

void BridgeClientTest::saveBeforeQuitFailsWhenTheBackendHasStopped() {
    BridgeClient client;
    isolateBridgeClient(client);
    acceptCommands(client);
    client.setBackendFailure(QStringLiteral("The Ferrous backend stopped unexpectedly."));
    QSignalSpy finished(&client, &BridgeClient::stateSaveFinished);

    const quint32 requestId = client.saveStateBeforeQuit();

    // Answered later, as the backend would answer.
    QCOMPARE(finished.count(), 0);
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.at(0).at(0).value<quint32>(), requestId);
    QVERIFY(requestId != 0);
    QVERIFY(!finished.at(0).at(1).toBool());
    QCOMPARE(finished.at(0).at(2).toString(),
        QStringLiteral("Settings and the queue could not be saved: the backend has stopped."));
}

void BridgeClientTest::spectrogramOverlaySettingsApplyFromSnapshot() {
    BridgeClient client;
    isolateBridgeClient(client);

    // Defaults should be false.
    QCOMPARE(client.showSpectrogramCrosshair(), false);
    QCOMPARE(client.showSpectrogramScale(), false);
    QCOMPARE(client.preventDisplaySleepInFullscreen(), true);
    QCOMPARE(client.showLevelMeter(), true);

    // Apply a snapshot with both enabled.
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.settings.present = true;
    snapshot.settings.showSpectrogramCrosshair = true;
    snapshot.settings.showSpectrogramScale = true;
    snapshot.settings.preventDisplaySleepInFullscreen = false;
    snapshot.settings.showLevelMeter = false;
    QVERIFY(client.processBinarySnapshot(snapshot));

    QCOMPARE(client.showSpectrogramCrosshair(), true);
    QCOMPARE(client.showSpectrogramScale(), true);
    QCOMPARE(client.preventDisplaySleepInFullscreen(), false);
    QCOMPARE(client.showLevelMeter(), false);

    // Apply a snapshot with both disabled.
    snapshot.settings.showSpectrogramCrosshair = false;
    snapshot.settings.showSpectrogramScale = false;
    snapshot.settings.preventDisplaySleepInFullscreen = true;
    snapshot.settings.showLevelMeter = true;
    QVERIFY(client.processBinarySnapshot(snapshot));

    QCOMPARE(client.showSpectrogramCrosshair(), false);
    QCOMPARE(client.showSpectrogramScale(), false);
    QCOMPARE(client.preventDisplaySleepInFullscreen(), true);
    QCOMPARE(client.showLevelMeter(), true);
}

void BridgeClientTest::spectrogramOverlaySettingsDecodeFromBinaryPayload() {
    // Build a settings section payload matching the Rust encode layout:
    // volume(f32), fftSize(u32), viewMode(u8), dbRange(f32),
    // logScale(u8), showFps(u8), sortMode(i32), sysMC(u8),
    // viewerFS(u8), displayMode(u8), crosshair(u8), scale(u8),
    // channelButtons(u8), zoomEnabled(u8), preventDisplaySleep(u8).
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
    ds << quint8(1);       // channelButtonsVisibility
    ds << quint8(1);       // spectrogramZoomEnabled
    ds << quint8(0);       // preventDisplaySleepInFullscreen
    ds << quint8(0);       // showLevelMeter

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
    QCOMPARE(decoded.settings.preventDisplaySleepInFullscreen, false);
    QCOMPARE(decoded.settings.showLevelMeter, false);
    const QByteArray command = BinaryBridgeCodec::encodeCommandU8(BinaryBridgeCodec::CmdSetShowLevelMeter, 0);
    QCOMPARE(command, QByteArray::fromHex("41000100000000"));
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
    // u32-prefixed string (path)
    QByteArray pathUtf8 = QStringLiteral("/test.flac").toUtf8();
    appendLe(quint32(pathUtf8.size()));
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

void BridgeClientTest::currentTrackChannelsExposesDecodedMetadata()
{
    BridgeClient client;
    isolateBridgeClient(client);
    client.m_currentTrackChannels = 6;

    QCOMPARE(client.currentTrackChannels(), 6);
    QCOMPARE(client.currentTrackChannelLayoutText(), QStringLiteral("6 ch"));
    QVERIFY(client.currentTrackChannelLayoutIconKey().isEmpty());
}


void BridgeClientTest::metadataChannelLabelsDecodeOptionalTail()
{
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    auto appendText = [&stream](const QByteArray &value) {
        stream << quint32(value.size());
        stream.writeRawData(value.constData(), value.size());
    };
    appendText("/fixture/surround.wav");
    for (int i = 0; i < 5; ++i) appendText(""); // title, artist, album, album artist, genre
    stream << qint32(0) << quint32(48000) << quint32(0) << quint16(6) << quint16(16);
    appendText("WAV");
    stream << quint32(0);
    appendText("");
    stream << quint64(0); // cover stamp
    stream << quint32(0);
    auto decode = [](const QByteArray &body, BinaryBridgeCodec::DecodedSnapshot *decoded) {
        QByteArray packet;
        QDataStream header(&packet, QIODevice::WriteOnly);
        header.setByteOrder(QDataStream::LittleEndian);
        header << quint32(0xFE550001u) << quint32(16 + body.size())
               << quint16(BinaryBridgeCodec::SectionMetadata) << quint16(0)
               << quint32(body.size());
        packet.append(body);
        QString error;
        return BinaryBridgeCodec::decodeSnapshotPacket(packet, decoded, &error);
    };
    BinaryBridgeCodec::DecodedSnapshot decoded;
    QVERIFY(decode(payload, &decoded)); // Old payloads remain valid.
    QVERIFY(decoded.metadata.channelLabels.isEmpty());
    const QByteArray legacy = payload;
    stream << quint16(6);
    const QStringList positions{"C", "L", "R", "SL", "SR", "LFE"};
    for (const auto &position : positions) appendText(position.toUtf8());
    QVERIFY(decode(payload, &decoded));
    QCOMPARE(decoded.metadata.channelLabels, positions); // Preserve native order.
    QVERIFY(!decode(payload.first(payload.size() - 1), &decoded));
    QByteArray mismatched = payload;
    mismatched[legacy.size()] = 5;
    QVERIFY(!decode(mismatched, &decoded));
    QVERIFY(!decode(payload + QByteArray(1, '\0'), &decoded));
    QVERIFY(decode(legacy + QByteArray(2, '\0'), &decoded));
    QVERIFY(decoded.metadata.channelLabels.isEmpty());
}

void BridgeClientTest::channelLayoutFollowsMatchingMetadataAcrossTracks()
{
    BridgeClient client;
    isolateBridgeClient(client);
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.playback.present = true;
    snapshot.playback.state = 1;
    snapshot.playback.currentPath = QStringLiteral("/fixture/surround.wav");
    snapshot.metadata.present = true;
    snapshot.metadata.sourcePath = snapshot.playback.currentPath;
    snapshot.metadata.channels = 6;
    snapshot.metadata.channelLabels = {"L", "R", "C", "LFE", "SL", "SR"};
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.currentTrackChannelLabels(), snapshot.metadata.channelLabels);
    QCOMPARE(client.currentTrackChannelLayoutText(), QStringLiteral("5.1"));
    QCOMPARE(client.currentTrackChannelLayoutIconKey(), QStringLiteral("5.1"));
    snapshot.playback.currentPath = QStringLiteral("/fixture/next.wav");
    QVERIFY(client.processBinarySnapshot(snapshot)); // Stale metadata for prior track.
    QCOMPARE(client.currentTrackChannels(), 0);
    QVERIFY(client.currentTrackChannelLabels().isEmpty());
    QVERIFY(client.currentTrackChannelLayoutIconKey().isEmpty());
    snapshot.metadata.sourcePath = snapshot.playback.currentPath;
    snapshot.metadata.channelLabels = {"L", "R", "C", "LFE", "FLC", "FRC"};
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.currentTrackChannelLayoutText(), QStringLiteral("6 ch"));
    QVERIFY(client.currentTrackChannelLayoutIconKey().isEmpty());
    snapshot.metadata.channelLabels = {"C", "L", "R", "RL", "RR", "LFE"};
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.currentTrackChannelLayoutText(), QStringLiteral("5.1"));
    QCOMPARE(client.currentTrackChannelLayoutIconKey(), QStringLiteral("5.1"));
    QCOMPARE(client.currentTrackChannelLabels(), snapshot.metadata.channelLabels);
}

void BridgeClientTest::testSoloChannelCommandEncoding()
{
    // Verify that CmdSoloChannel encodes as command ID 54 + 1-byte channel index,
    // using the same encodeCommandU8 helper as CmdToggleChannelMute.
    QByteArray cmd = BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSoloChannel, 3);
    // Format: u16 cmd_id (LE) + u32 payload_len (LE) + u8 channel
    QCOMPARE(cmd.size(), 7);
    quint16 cmdId = qFromLittleEndian<quint16>(cmd.constData());
    quint32 payloadLen = qFromLittleEndian<quint32>(cmd.constData() + 2);
    quint8 channel = static_cast<quint8>(cmd.at(6));
    QCOMPARE(cmdId, quint16(BinaryBridgeCodec::CmdSoloChannel));
    QCOMPARE(payloadLen, quint32(1));
    QCOMPARE(channel, quint8(3));
}

void BridgeClientTest::testSoloedChannelDecoding()
{
    QByteArray payload;
    auto appendLe = [&payload](auto value) {
        const auto le = qToLittleEndian(value);
        payload.append(reinterpret_cast<const char *>(&le), sizeof(le));
    };

    appendLe(quint8(1));          // state = Playing
    appendLe(double(5.0));        // position
    appendLe(double(100.0));      // duration
    appendLe(float(0.8f));        // volume
    appendLe(quint8(0));          // repeat mode
    appendLe(quint8(0));          // shuffle
    appendLe(qint32(0));          // currentQueueIndex
    QByteArray pathUtf8 = QStringLiteral("/test.flac").toUtf8();
    appendLe(quint32(pathUtf8.size()));
    payload.append(pathUtf8);
    appendLe(quint64(0));         // mutedChannelsMask
    payload.append(static_cast<char>(3)); // soloedChannel = 3

    QByteArray packet;
    QDataStream ps(&packet, QIODevice::WriteOnly);
    ps.setByteOrder(QDataStream::LittleEndian);
    ps << quint32(0xFE550001u);
    quint32 totalLen = 12 + 4 + payload.size();
    ps << quint32(totalLen);
    ps << quint16(BinaryBridgeCodec::SectionPlayback);
    ps << quint16(0);
    ps << quint32(payload.size());
    packet.append(payload);

    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(packet, &decoded, &error),
             qPrintable(error));
    QCOMPARE(decoded.playback.soloedChannel, 3);
}

void BridgeClientTest::testSoloedChannelDecodingNone()
{
    QByteArray payload;
    auto appendLe = [&payload](auto value) {
        const auto le = qToLittleEndian(value);
        payload.append(reinterpret_cast<const char *>(&le), sizeof(le));
    };

    appendLe(quint8(1));
    appendLe(double(5.0));
    appendLe(double(100.0));
    appendLe(float(0.8f));
    appendLe(quint8(0));
    appendLe(quint8(0));
    appendLe(qint32(0));
    QByteArray pathUtf8 = QStringLiteral("/test.flac").toUtf8();
    appendLe(quint32(pathUtf8.size()));
    payload.append(pathUtf8);
    appendLe(quint64(0));
    payload.append(static_cast<char>(0xFF)); // no solo

    QByteArray packet;
    QDataStream ps(&packet, QIODevice::WriteOnly);
    ps.setByteOrder(QDataStream::LittleEndian);
    ps << quint32(0xFE550001u);
    quint32 totalLen = 12 + 4 + payload.size();
    ps << quint32(totalLen);
    ps << quint16(BinaryBridgeCodec::SectionPlayback);
    ps << quint16(0);
    ps << quint32(payload.size());
    packet.append(payload);

    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(packet, &decoded, &error),
             qPrintable(error));
    QCOMPARE(decoded.playback.soloedChannel, -1);
}

void BridgeClientTest::testPlaybackTrackTokenDecoding()
{
    QByteArray payload;
    auto appendLe = [&payload](auto value) {
        const auto le = qToLittleEndian(value);
        payload.append(reinterpret_cast<const char *>(&le), sizeof(le));
    };

    appendLe(quint8(1));
    appendLe(double(5.0));
    appendLe(double(100.0));
    appendLe(float(0.8f));
    appendLe(quint8(1));
    appendLe(quint8(0));
    appendLe(qint32(0));
    QByteArray pathUtf8 = QStringLiteral("/test.flac").toUtf8();
    appendLe(quint32(pathUtf8.size()));
    payload.append(pathUtf8);
    appendLe(quint64(0));
    payload.append(static_cast<char>(0xFF));
    appendLe(quint64(0xFFFF'FFFF'0000'0007ULL)); // trackToken

    auto decode = [](const QByteArray &body, BinaryBridgeCodec::DecodedSnapshot *decoded) {
        QByteArray packet;
        QDataStream ps(&packet, QIODevice::WriteOnly);
        ps.setByteOrder(QDataStream::LittleEndian);
        ps << quint32(0xFE550001u);
        ps << quint32(12 + 4 + body.size());
        ps << quint16(BinaryBridgeCodec::SectionPlayback);
        ps << quint16(0);
        ps << quint32(body.size());
        packet.append(body);
        QString error;
        return BinaryBridgeCodec::decodeSnapshotPacket(packet, decoded, &error);
    };

    BinaryBridgeCodec::DecodedSnapshot decoded;
    QVERIFY(decode(payload, &decoded));
    QCOMPARE(decoded.playback.trackToken, quint64(0xFFFF'FFFF'0000'0007ULL));
    QCOMPARE(decoded.playback.soloedChannel, -1);
    QCOMPARE(decoded.playback.seekSerial, quint64(0));
    QCOMPARE(decoded.playback.seekLandedSerial, quint64(0));

    BinaryBridgeCodec::DecodedSnapshot truncated;
    QVERIFY(!decode(payload.left(payload.size() - 3), &truncated));

    // The seek serials follow the track token.
    appendLe(quint64(0xFFFF'FFFF'0000'0009ULL));
    appendLe(quint64(0xFFFF'FFFF'0000'0008ULL));
    BinaryBridgeCodec::DecodedSnapshot withSeeks;
    QVERIFY(decode(payload, &withSeeks));
    QCOMPARE(withSeeks.playback.trackToken, quint64(0xFFFF'FFFF'0000'0007ULL));
    QCOMPARE(withSeeks.playback.seekSerial, quint64(0xFFFF'FFFF'0000'0009ULL));
    QCOMPARE(withSeeks.playback.seekLandedSerial, quint64(0xFFFF'FFFF'0000'0008ULL));
    QVERIFY(!decode(payload.left(payload.size() - 8), &truncated));
    QCOMPARE(withSeeks.playback.positionSampledAtNs, qint64(0));
    QVERIFY(!withSeeks.playback.positionAdvancing);

    // The position sample's time stamp and whether it advances end the
    // section, as the Rust encoder writes them.
    appendLe(quint64(123'456'789'012ULL));
    appendLe(quint8(1));
    BinaryBridgeCodec::DecodedSnapshot withSample;
    QVERIFY(decode(payload, &withSample));
    QCOMPARE(withSample.playback.positionSeconds, 5.0);
    QCOMPARE(withSample.playback.positionSampledAtNs, qint64(123'456'789'012LL));
    QVERIFY(withSample.playback.positionAdvancing);
    QVERIFY(!decode(payload.left(payload.size() - 1), &truncated));
    payload[payload.size() - 1] = char(2);
    QVERIFY(!decode(payload, &truncated));
}

void BridgeClientTest::testChannelButtonsVisibilityDecoding()
{
    // Build a minimal settings section with channelButtonsVisibility = 2.
    // Encoding order: volume(f32) + fft_size(u32) + view_mode(u8)
    // + db_range(f32) + log_scale(u8) + show_fps(u8)
    // + library_sort_mode(i32) + system_media_controls(u8)
    // + viewer_fullscreen(u8) + display_mode(u8)
    // + crosshair(u8) + scale(u8) + channel_buttons_visibility(u8)
    QByteArray payload;
    auto appendLe = [&payload](auto value) {
        const auto le = qToLittleEndian(value);
        payload.append(reinterpret_cast<const char *>(&le), sizeof(le));
    };

    appendLe(float(0.75f));      // volume
    appendLe(quint32(4096));     // fft_size
    payload.append(char(0));     // view_mode (downmix)
    appendLe(float(132.0f));     // db_range
    payload.append(char(0));     // log_scale
    payload.append(char(0));     // show_fps
    appendLe(qint32(0));         // library_sort_mode
    payload.append(char(1));     // system_media_controls
    payload.append(char(0));     // viewer_fullscreen
    payload.append(char(0));     // display_mode
    payload.append(char(0));     // crosshair
    payload.append(char(0));     // scale
    payload.append(char(2));     // channel_buttons_visibility = 2

    QByteArray packet;
    QDataStream ps(&packet, QIODevice::WriteOnly);
    ps.setByteOrder(QDataStream::LittleEndian);
    ps << quint32(0xFE550001u);
    quint32 totalLen = 12 + 4 + payload.size();
    ps << quint32(totalLen);
    ps << quint16(BinaryBridgeCodec::SectionSettings);
    ps << quint16(0);
    ps << quint32(payload.size());
    packet.append(payload);

    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(packet, &decoded, &error),
             qPrintable(error));
    QCOMPARE(decoded.settings.channelButtonsVisibility, 2);
    QCOMPARE(decoded.settings.preventDisplaySleepInFullscreen, true);
    QCOMPARE(decoded.settings.showLevelMeter, true); // Older settings payloads default on.
}

void BridgeClientTest::libraryViewStateAvailabilityFollowsSnapshot() {
    BridgeClient client;
    isolateBridgeClient(client);
    QCOMPARE(client.libraryViewStateAvailable(), false);

    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.library.present = true;
    snapshot.library.viewStatePresent = true;
    snapshot.library.expandedKeys = QStringList{QStringLiteral("artist|/music|Artist")};
    snapshot.library.viewSelectionKey = QStringLiteral("album|/music|Artist|Album");
    snapshot.library.viewAnchorKey = QStringLiteral("artist|/music|Artist");
    snapshot.library.viewAnchorOffset = 5.0;

    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.libraryViewStateAvailable(), true);
    QCOMPARE(client.libraryExpandedKeys(), snapshot.library.expandedKeys);
    QCOMPARE(client.libraryViewSelectionKey(), snapshot.library.viewSelectionKey);
    QCOMPARE(client.libraryViewAnchorKey(), snapshot.library.viewAnchorKey);
    QCOMPARE(client.libraryViewAnchorOffset(), 5.0);
}


void BridgeClientTest::libraryChangesNotifyOnlyLibraryProperties() {
    BridgeClient client;
    isolateBridgeClient(client);
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.library.present = true;
    snapshot.library.scanInProgress = true;
    snapshot.library.filesDiscovered = 100;
    snapshot.library.filesProcessed = 10;

    // Scan progress: only the scan's properties are notified.
    QVERIFY(client.processBinarySnapshot(snapshot));
    QVERIFY(client.m_pollLibraryScanChanged);
    QVERIFY(!client.m_pollLibraryChanged);
    QVERIFY(!client.m_pollSnapshotChanged);

    // New tracks: the library's properties, not the rest of the snapshot.
    client.m_pollLibraryScanChanged = false;
    snapshot.library.trackCount = 5;
    snapshot.library.artistCount = 1;
    QVERIFY(client.processBinarySnapshot(snapshot));
    QVERIFY(client.m_pollLibraryChanged);
    QVERIFY(!client.m_pollLibraryScanChanged);
    QVERIFY(!client.m_pollSnapshotChanged);

    QSignalSpy library(&client, &BridgeClient::libraryChanged);
    QSignalSpy scan(&client, &BridgeClient::libraryScanChanged);
    QSignalSpy other(&client, &BridgeClient::snapshotChanged);
    client.scheduleLibraryScanChanged();
    QTRY_COMPARE(scan.count(), 1);
    QCOMPARE(library.count(), 0);
    QCOMPARE(other.count(), 0);
    client.scheduleLibraryChanged();
    QTRY_COMPARE(library.count(), 1);
    QCOMPARE(other.count(), 0);
    QCOMPARE(client.libraryTrackCount(), 5);
    QCOMPARE(client.libraryScanProcessed(), 10);
}

void BridgeClientTest::libraryWatchingDecodesFromBinaryPayload() {
    QByteArray bytes;
    const auto append = [&bytes](auto value) {
        const auto le = qToLittleEndian(value);
        bytes.append(reinterpret_cast<const char *>(&le), sizeof(le));
    };
    const auto text = [&bytes, &append](const QString &value) {
        const QByteArray utf8 = value.toUtf8();
        append(quint32(utf8.size()));
        bytes.append(utf8);
    };
    const auto section = [&bytes](quint16 mask) {
        QByteArray packet;
        const auto le = [&packet](auto value) {
            const auto v = qToLittleEndian(value);
            packet.append(reinterpret_cast<const char *>(&v), sizeof(v));
        };
        le(quint32(0xFE550001u));
        le(quint32(12 + 4 + bytes.size()));
        le(mask);
        le(quint16(0));
        le(quint32(bytes.size()));
        packet.append(bytes);
        return packet;
    };

    // The library meta section as Rust encodes it: path, name, the reason
    // a root is not watched, its scan problem, and its track count, per root.
    for (quint32 value : {2u, 0u, 0u, 0u}) append(value); // roots, tracks, artists, albums
    append(quint8(0));                                       // scan in progress
    append(qint32(0));                                       // sort mode
    text(QString());                                         // last error
    for (quint32 value : {0u, 0u, 0u, 0u}) append(value);    // scan progress
    append(float(0.0f));
    append(float(-1.0f));
    append(quint16(2));
    for (const QString &value : {QStringLiteral("/music"), QStringLiteral("Music"), QString(),
                                 QStringLiteral("could not read 1 folder")}) {
        text(value);
    }
    append(quint32(1234));
    for (const QString &value : {QStringLiteral("/nas"), QString(), QStringLiteral("it is on nfs4"),
                                 QString()}) {
        text(value);
    }
    append(quint32(0));
    BinaryBridgeCodec::DecodedSnapshot decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(section(BinaryBridgeCodec::SectionLibraryMeta),
                                                     &decoded, &error),
             qPrintable(error));
    QCOMPARE(decoded.library.rootEntries.size(), 2);
    QCOMPARE(decoded.library.rootEntries[0].name, QStringLiteral("Music"));
    QVERIFY(decoded.library.rootEntries[0].unwatchedReason.isEmpty());
    QCOMPARE(decoded.library.rootEntries[1].path, QStringLiteral("/nas"));
    QCOMPARE(decoded.library.rootEntries[1].unwatchedReason, QStringLiteral("it is on nfs4"));
    QCOMPARE(decoded.library.rootEntries[0].scanProblem, QStringLiteral("could not read 1 folder"));
    QVERIFY(decoded.library.rootEntries[1].scanProblem.isEmpty());
    QCOMPARE(decoded.library.rootEntries[0].trackCount, 1234);
    QCOMPARE(decoded.library.rootEntries[1].trackCount, 0);

    // The settings section ends with library watching, and older payloads
    // without it default on.
    bytes.clear();
    append(float(1.0f));
    append(quint32(8192));
    append(quint8(0));
    append(float(132.0f));
    append(quint8(0));
    append(quint8(0));
    append(qint32(0));
    for (int field = 0; field < 9; ++field) append(quint8(1));
    decoded = {};
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(section(BinaryBridgeCodec::SectionSettings),
                                                     &decoded, &error),
             qPrintable(error));
    QCOMPARE(decoded.settings.libraryWatchEnabled, true);
    append(quint8(0));
    decoded = {};
    QVERIFY2(BinaryBridgeCodec::decodeSnapshotPacket(section(BinaryBridgeCodec::SectionSettings),
                                                     &decoded, &error),
             qPrintable(error));
    QCOMPARE(decoded.settings.libraryWatchEnabled, false);
    QCOMPARE(decoded.settings.showLevelMeter, true);

    QCOMPARE(BinaryBridgeCodec::encodeCommandU8(BinaryBridgeCodec::CmdSetLibraryWatchEnabled, 1),
             QByteArray::fromHex("43000100000001"));
}

void BridgeClientTest::libraryScanErrorsAreReportedAsLibraryErrors() {
    BridgeClient client;
    isolateBridgeClient(client);
    QSignalSpy errors(&client, &BridgeClient::bridgeError);
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.library.present = true;
    snapshot.library.lastError = QStringLiteral("could not read 1 folder or file in '/music'");

    client.processBinarySnapshot(snapshot);
    QCOMPARE(errors.count(), 1);
    // The category says where it came from; the message has no prefix.
    QCOMPARE(errors[0][0].toString(), snapshot.library.lastError);
    QCOMPARE(errors[0][1].toInt(), int(BinaryBridgeCodec::ErrorCategoryLibrary));
    QCOMPARE(errors[0][2].toInt(), int(BinaryBridgeCodec::ErrorSeverityError));

    // The same error again is not told again.
    client.processBinarySnapshot(snapshot);
    QCOMPARE(errors.count(), 1);
}

void BridgeClientTest::unwatchedLibraryRootsAreShownAndReportedOnce() {
    BridgeClient client;
    isolateBridgeClient(client);
    QSignalSpy errors(&client, &BridgeClient::bridgeError);
    BinaryBridgeCodec::DecodedSnapshot snapshot;
    snapshot.settings.present = true;
    snapshot.settings.libraryWatchEnabled = false;
    snapshot.library.present = true;
    BinaryBridgeCodec::DecodedLibraryRoot watched;
    watched.path = QStringLiteral("/music");
    BinaryBridgeCodec::DecodedLibraryRoot share;
    share.path = QStringLiteral("/nas");
    share.name = QStringLiteral("NAS");
    share.unwatchedReason = QStringLiteral("it is on nfs4");
    watched.scanProblem = QStringLiteral("could not read 1 folder");
    share.trackCount = 42;
    snapshot.library.rootEntries = {watched, share};
    snapshot.library.rootPaths = {watched.path, share.path};

    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.libraryWatchEnabled(), false);
    const QVariantList entries = client.libraryRootEntries();
    QCOMPARE(entries.size(), 2);
    QVERIFY(entries[0].toMap().value(QStringLiteral("unwatchedReason")).toString().isEmpty());
    QCOMPARE(entries[1].toMap().value(QStringLiteral("unwatchedReason")).toString(),
             QStringLiteral("it is on nfs4"));
    QCOMPARE(entries[0].toMap().value(QStringLiteral("scanProblem")).toString(),
             QStringLiteral("could not read 1 folder"));
    QCOMPARE(entries[1].toMap().value(QStringLiteral("trackCount")).toInt(), 42);
    QCOMPARE(errors.count(), 1);
    QCOMPARE(errors[0][0].toString(),
             QStringLiteral("Not watched for changes in library folder NAS: it is on nfs4"));
    QCOMPARE(errors[0][1].toInt(), int(BinaryBridgeCodec::ErrorCategoryLibrary));
    QCOMPARE(errors[0][2].toInt(), int(BinaryBridgeCodec::ErrorSeverityWarning));
    QCOMPARE(errors[0][3].toString(), QStringLiteral("/nas"));

    // The same roots again: nothing changed, and they were told once.
    QVERIFY(!client.processBinarySnapshot(snapshot));
    QCOMPARE(errors.count(), 1);
    // A new reason is told.
    snapshot.library.rootEntries[1].unwatchedReason = QStringLiteral("the folder is not available");
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(errors.count(), 2);

    // The setting is sent, and shown before the backend answers.
    snapshot.settings.libraryWatchEnabled = true;
    QVERIFY(client.processBinarySnapshot(snapshot));
    QCOMPARE(client.libraryWatchEnabled(), true);
    client.setLibraryWatchEnabled(false);
    QCOMPARE(client.libraryWatchEnabled(), false);
}

void BridgeClientTest::searchExtendedMatchDetailsDecodeAndHighlight() {
    QByteArray bytes;
    const auto append = [&bytes](auto value) { auto le = qToLittleEndian(value); bytes.append(reinterpret_cast<const char *>(&le), sizeof(le)); };
    const auto text = [&bytes, &append](const QString &value) { const auto utf8 = value.toUtf8(); append(quint32(utf8.size())); bytes.append(utf8); };
    append(quint8('S')); append(quint8(4)); append(quint16(1)); append(quint32(9));
    append(quint32(0)); append(quint32(0)); append(quint32(1));
    append(quint8(3)); append(quint32(0)); append(qint32(1997)); append(quint16(1)); append(quint32(0)); append(quint32(0));
    for (const auto &value : QStringList{"Song", "Artist", "Album", "", "", "", "", "", "", "track|/fixture/song", "/fixture/song", "Comment: remaster"}) text(value);
    BinaryBridgeCodec::DecodedSearchResults decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSearchResultsFrame(bytes, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.rows[0].matchDetail, QStringLiteral("Comment: remaster"));
    BridgeClient client; isolateBridgeClient(client);
    client.m_latestGlobalSearchSeqSent = 9;
    QVERIFY(client.processSearchResultsFrame(decoded));
    client.applyDeferredSearchDisplayRows();
    QCOMPARE(client.m_globalSearchModel.data(client.m_globalSearchModel.index(0), GlobalSearchResultsModel::MatchDetailRole).toString(), QStringLiteral("Comment: remaster"));
    QCOMPARE(client.m_globalSearchModel.highlightText("Comment: anniversary remaster", "comment:\"anniversary remaster\""), QStringLiteral("Comment: <b>anniversary remaster</b>"));
    QCOMPARE(client.m_globalSearchModel.highlightText("live:oslo", "live:oslo"), QStringLiteral("<b>live:oslo</b>"));
    bytes.chop(1);
    QVERIFY(!BinaryBridgeCodec::decodeSearchResultsFrame(bytes, &decoded, &error));
}

void BridgeClientTest::searchResultsRankFilterAndHighlight() {
    GlobalSearchResultsModel model;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> rows;
    for (const QString &type : {QStringLiteral("artist"), QStringLiteral("album"), QStringLiteral("track")}) {
        GlobalSearchResultsModel::SearchDisplayRow row;
        row.kind = "item"; row.rowType = type; row.label = "Blue"; row.score = 0;
        rows.push_back(row);
    }
    // Weak album matches must stay below every stronger track, even beyond
    // the first three positions. Matching copies expose their library roots.
    for (int i = 0; i < 5; ++i) {
        auto row = rows.last(); row.label = QStringLiteral("Track %1").arg(i); row.score = 1;
        rows.push_back(row);
    }
    auto weak = rows[1]; weak.label = "Weak album"; weak.score = 6;
    rows.push_back(weak);
    auto copy = rows[1]; copy.rootLabel = "Surround";
    rows[1].rootLabel = "Stereo";
    rows.push_back(copy);
    model.presentSearchRows(rows);
    QCOMPARE(model.rowDataAt(0).value("rowType").toString(), QStringLiteral("album"));
    QCOMPARE(model.rowDataAt(1).value("rowType").toString(), QStringLiteral("album"));
    QVERIFY(model.rowDataAt(0).value("showRoot").toBool());
    QVERIFY(model.rowDataAt(1).value("showRoot").toBool());
    QCOMPARE(model.rowCount(), 10); // No section rows or repeated results.
    QCOMPARE(model.rowDataAt(9).value("label").toString(), QStringLiteral("Weak album"));
    QVERIFY(!model.rowDataAt(9).value("showRoot").toBool());
    model.setResultFilter("album");
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.rowDataAt(0).value("rowType").toString(), QStringLiteral("album"));
    model.setResultFilter("invalid");
    QCOMPARE(model.resultFilter(), QStringLiteral("album"));
    model.setResultFilter("all");
    QCOMPARE(model.rowCount(), 10);
    QCOMPARE(model.highlightText("Album: Signify", "type:album signify"), QStringLiteral("Album: <b>Signify</b>"));
    QCOMPARE(model.highlightText(QString::fromUtf8("Björk & <Blue>"), "bjork blue"), QString::fromUtf8("<b>Björk</b> &amp; &lt;<b>Blue</b>&gt;"));
    QCOMPARE(model.highlightText(QString::fromUtf8("Cafe\xcc\x81"), "cafe"), QString::fromUtf8("<b>Cafe\xcc\x81</b>"));
    // Letters without a decomposition match their base letters, as the
    // Rust search folds them.
    QCOMPARE(model.highlightText(QString::fromUtf8("Øystein Sørensen"), "oystein sorensen"),
             QString::fromUtf8("<b>Øystein</b> <b>Sørensen</b>"));
    QCOMPARE(model.highlightText(QString::fromUtf8("Straße"), "strasse"), QString::fromUtf8("<b>Straße</b>"));
    QCOMPARE(model.highlightText(QString::fromUtf8("Ægir Łódź"), "aegir lodz"),
             QString::fromUtf8("<b>Ægir</b> <b>Łódź</b>"));
    // A match of part of a letter's spelling highlights the whole letter.
    QCOMPARE(model.highlightText(QString::fromUtf8("Ægir"), "a"), QString::fromUtf8("<b>Æ</b>gir"));
    QCOMPARE(model.highlightText(QString::fromUtf8("Ægir"), "eg"), QString::fromUtf8("<b>Æg</b>ir"));
    QCOMPARE(model.highlightText(QString::fromUtf8("Œuvre"), "o"), QString::fromUtf8("<b>Œ</b>uvre"));
    QCOMPARE(model.highlightText(QString::fromUtf8("Þór"), "t"), QString::fromUtf8("<b>Þ</b>ór"));
    QCOMPARE(model.highlightText(QString::fromUtf8("\xc3\x86\xcc\x81x"), "a"),
             QString::fromUtf8("<b>\xc3\x86\xcc\x81</b>x")); // Ǽ decomposed
}

void BridgeClientTest::searchProtocolTotalsAndExpansion() {
    QByteArray bytes;
    const auto append = [&bytes](auto value) { auto le = qToLittleEndian(value); bytes.append(reinterpret_cast<const char *>(&le), sizeof(le)); };
    append(quint8('S')); append(quint8(3)); append(quint16(0)); append(quint32(9));
    append(quint32(7)); append(quint32(12)); append(quint32(500));
    BinaryBridgeCodec::DecodedSearchResults decoded;
    QString error;
    QVERIFY2(BinaryBridgeCodec::decodeSearchResultsFrame(bytes, &decoded, &error), qPrintable(error));
    QCOMPARE(decoded.seq, 9u); QCOMPARE(decoded.totals[2], 500u);
    bytes.chop(1);
    QVERIFY(!BinaryBridgeCodec::decodeSearchResultsFrame(bytes, &decoded, &error));
    const auto command = BinaryBridgeCodec::encodeCommandSearchQuery(BinaryBridgeCodec::CmdSetSearchQuery, 9, "blue", 80);
    QCOMPARE(qFromLittleEndian<quint32>(command.constData() + command.size() - 4), 80u);
    BridgeClient client; isolateBridgeClient(client);
    client.m_globalSearchTotals = {7, 12, 500};
    client.m_globalSearchTrackCount = 20;
    client.m_pendingGlobalSearchQuery = "blue";
    QVERIFY(client.globalSearchCanExpand());
    client.expandGlobalSearch();
    QCOMPARE(client.m_globalSearchLimit, 40u);
    QVERIFY(client.m_globalSearchBusy);
    QVERIFY(!client.globalSearchCanExpand());
    client.setGlobalSearchQuery("new query");
    QCOMPARE(client.m_globalSearchLimit, 0u);
}

void BridgeClientTest::searchDeferredResultsRejectSupersededQuery() {
    BridgeClient client; isolateBridgeClient(client);
    client.m_latestGlobalSearchSeqSent = 2;
    BridgeClient::SearchWorkerOutputFrame oldFrame; oldFrame.seq = 1;
    QVERIFY(!client.applyPreparedSearchResultsFrame(std::move(oldFrame)));
    BridgeClient::SearchWorkerOutputFrame frame; frame.seq = 2;
    frame.trackCount = 1; frame.totals = {0, 0, 30};
    GlobalSearchResultsModel::SearchDisplayRow row; row.kind = "item"; row.rowType = "track"; row.label = "Old";
    frame.displayRows.push_back(row);
    QVERIFY(client.applyPreparedSearchResultsFrame(std::move(frame)));
    client.setGlobalSearchQuery("new");
    QVERIFY(client.m_deferredSearchDisplayRows.isEmpty());
    client.applyDeferredSearchDisplayRows();
    QCOMPARE(client.m_globalSearchModel.rowCount(), 0);
    QVERIFY(client.m_globalSearchBusy);
}

void BridgeClientTest::searchModelDelegateTypeRoleIsExposed() {
    GlobalSearchResultsModel model;
    const auto roles = model.roleNames();
    QVERIFY(roles.values().contains("delegateType"));
}

void BridgeClientTest::searchModelDelegateTypeValuesCorrect() {
    GlobalSearchResultsModel model;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> rows;

    GlobalSearchResultsModel::SearchDisplayRow section;
    section.kind = QStringLiteral("section");
    section.rowType = QStringLiteral("artist");
    rows.push_back(section);

    GlobalSearchResultsModel::SearchDisplayRow columns;
    columns.kind = QStringLiteral("columns");
    columns.rowType = QStringLiteral("album");
    rows.push_back(columns);

    GlobalSearchResultsModel::SearchDisplayRow item;
    item.kind = QStringLiteral("item");
    item.rowType = QStringLiteral("track");
    rows.push_back(item);

    model.replaceRows(rows);

    const int dtRole = GlobalSearchResultsModel::DelegateTypeRole;
    QCOMPARE(model.data(model.index(0), dtRole).toString(), QStringLiteral("section"));
    QCOMPARE(model.data(model.index(1), dtRole).toString(), QStringLiteral("columns-album"));
    QCOMPARE(model.data(model.index(2), dtRole).toString(), QStringLiteral("track"));
}

void BridgeClientTest::clearSearchQueryRetainsModelRows() {
    BridgeClient client;
    isolateBridgeClient(client);

    // Simulate having an active search with results.
    QVector<GlobalSearchResultsModel::SearchDisplayRow> rows;
    GlobalSearchResultsModel::SearchDisplayRow row;
    row.kind = QStringLiteral("item");
    row.rowType = QStringLiteral("track");
    row.label = QStringLiteral("Test Track");
    rows.push_back(row);
    client.m_globalSearchModel.replaceRows(rows);
    client.m_globalSearchArtistCount = 0;
    client.m_globalSearchAlbumCount = 0;
    client.m_globalSearchTrackCount = 1;
    // Set non-empty pending query so the empty→clear transition path runs.
    client.m_pendingGlobalSearchQuery = QStringLiteral("test");
    client.m_lastGlobalSearchQuerySent = QStringLiteral("test");

    client.setGlobalSearchQuery(QStringLiteral(""));

    QCOMPARE(client.m_globalSearchModel.rowCount(), 1); // Rows preserved
    QVERIFY(client.m_globalSearchModelRetained);         // Flag set
    QCOMPARE(client.m_globalSearchTrackCount, 0);        // Counts zeroed
}

void BridgeClientTest::newSearchResultsClearRetainedFlag() {
    BridgeClient client;
    isolateBridgeClient(client);

    // Set up retained state.
    QVector<GlobalSearchResultsModel::SearchDisplayRow> rows;
    GlobalSearchResultsModel::SearchDisplayRow row;
    row.kind = QStringLiteral("item");
    row.rowType = QStringLiteral("track");
    rows.push_back(row);
    client.m_globalSearchModel.replaceRows(rows);
    client.m_globalSearchModelRetained = true;

    // Simulate new results arriving via applyPreparedSearchResultsFrame.
    BridgeClient::SearchWorkerOutputFrame frame;
    frame.seq = 1;
    frame.artistCount = 0;
    frame.albumCount = 0;
    frame.trackCount = 1;
    GlobalSearchResultsModel::SearchDisplayRow newRow;
    newRow.kind = QStringLiteral("item");
    newRow.rowType = QStringLiteral("track");
    newRow.label = QStringLiteral("New Track");
    frame.displayRows.push_back(newRow);
    client.m_latestGlobalSearchSeqSent = 1;
    client.applyPreparedSearchResultsFrame(std::move(frame));

    QVERIFY(!client.m_globalSearchModelRetained); // Flag cleared
}

void BridgeClientTest::searchModelBatchedInsertionFromEmpty() {
    GlobalSearchResultsModel model;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> rows;
    for (int i = 0; i < 30; ++i) {
        GlobalSearchResultsModel::SearchDisplayRow row;
        row.kind = QStringLiteral("item");
        row.rowType = QStringLiteral("track");
        row.label = QStringLiteral("Track %1").arg(i);
        rows.push_back(row);
    }

    QSignalSpy insertSpy(&model, SIGNAL(rowsInserted(QModelIndex,int,int)));
    model.replaceRowsBatched(rows, 10);

    QCOMPARE(model.rowCount(), 10);   // First batch immediate
    QCOMPARE(insertSpy.count(), 1);

    QCoreApplication::processEvents();
    QCOMPARE(model.rowCount(), 20);   // Second batch after event loop
    QCOMPARE(insertSpy.count(), 2);

    QCoreApplication::processEvents();
    QCOMPARE(model.rowCount(), 30);   // Third batch
    QCOMPARE(insertSpy.count(), 3);
}

void BridgeClientTest::searchModelBatchedInsertionSmallSetSkipsBatching() {
    GlobalSearchResultsModel model;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> rows;
    for (int i = 0; i < 8; ++i) {
        GlobalSearchResultsModel::SearchDisplayRow row;
        row.kind = QStringLiteral("item");
        row.rowType = QStringLiteral("track");
        rows.push_back(row);
    }

    QSignalSpy insertSpy(&model, SIGNAL(rowsInserted(QModelIndex,int,int)));
    model.replaceRowsBatched(rows, 10);

    QCOMPARE(model.rowCount(), 8);    // All at once, no batching
    QCOMPARE(insertSpy.count(), 1);
}

void BridgeClientTest::searchModelReplaceRowsCancelsBatchInProgress() {
    GlobalSearchResultsModel model;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> rows;
    for (int i = 0; i < 30; ++i) {
        GlobalSearchResultsModel::SearchDisplayRow row;
        row.kind = QStringLiteral("item");
        row.rowType = QStringLiteral("track");
        rows.push_back(row);
    }
    model.replaceRowsBatched(rows, 10);
    QCOMPARE(model.rowCount(), 10); // First batch

    // Now replace with new data mid-batch
    QVector<GlobalSearchResultsModel::SearchDisplayRow> newRows;
    GlobalSearchResultsModel::SearchDisplayRow newRow;
    newRow.kind = QStringLiteral("item");
    newRow.rowType = QStringLiteral("artist");
    newRow.label = QStringLiteral("New Artist");
    newRows.push_back(newRow);

    model.replaceRows(newRows);
    QCOMPARE(model.rowCount(), 1); // Batch cancelled, new data applied

    QCoreApplication::processEvents(); // No stale batch fires
    QCOMPARE(model.rowCount(), 1);
}

void BridgeClientTest::searchRetainAndReuseIntegration() {
    GlobalSearchResultsModel model;

    // Phase 1: Cold start — batched insertion
    QVector<GlobalSearchResultsModel::SearchDisplayRow> initialRows;
    for (int i = 0; i < 30; ++i) {
        GlobalSearchResultsModel::SearchDisplayRow row;
        row.kind = QStringLiteral("item");
        row.rowType = QStringLiteral("track");
        row.label = QStringLiteral("Track %1").arg(i);
        initialRows.push_back(row);
    }
    model.replaceRowsBatched(initialRows, 10);
    QCOMPARE(model.rowCount(), 10); // First batch
    QTRY_COMPARE(model.rowCount(), 30); // Drain remaining batches

    // Phase 2: Simulate clear — model stays populated, no replaceRows({})
    // (In real code, BridgeClient sets m_globalSearchModelRetained instead)
    QCOMPARE(model.rowCount(), 30);

    // Phase 3: New results arrive — diff-based update (N→M), no batching
    QVector<GlobalSearchResultsModel::SearchDisplayRow> newRows;
    for (int i = 0; i < 20; ++i) {
        GlobalSearchResultsModel::SearchDisplayRow row;
        row.kind = QStringLiteral("item");
        row.rowType = QStringLiteral("artist");
        row.label = QStringLiteral("Artist %1").arg(i);
        newRows.push_back(row);
    }
    // Model is non-empty → replaceRowsBatched falls through to replaceRows.
    // rowType changes (track→artist), so replaceRows does a full rebuild
    // (remove+insert) to force DelegateChooser to re-evaluate delegate types.
    model.replaceRowsBatched(newRows, 10);

    QCOMPARE(model.rowCount(), 20); // Immediate update, no batching
    // Verify data is from the new set
    QCOMPARE(model.data(model.index(0), GlobalSearchResultsModel::LabelRole).toString(),
             QStringLiteral("Artist 0"));
}

void BridgeClientTest::coverImageProviderLoadsValidImage() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString imagePath = tempDir.filePath(QStringLiteral("cover.png"));
    QImage source(200, 150, QImage::Format_ARGB32);
    source.fill(Qt::blue);
    QVERIFY(source.save(imagePath, "PNG"));

    CoverImageProvider provider;
    QQuickImageResponse *response = provider.requestImageResponse(imagePath, QSize());
    QVERIFY(response != nullptr);

    provider.waitForDone();
    QVERIFY(static_cast<CoverImageResponse *>(response)->isFinished());

    QQuickTextureFactory *factory = response->textureFactory();
    QVERIFY(factory != nullptr);
    QImage result = factory->image();
    QCOMPARE(result.width(), 200);
    QCOMPARE(result.height(), 150);

    delete factory;
    delete response;
}

void BridgeClientTest::coverImageProviderRespectsRequestedSize() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString imagePath = tempDir.filePath(QStringLiteral("large.png"));
    QImage source(800, 600, QImage::Format_ARGB32);
    source.fill(Qt::red);
    QVERIFY(source.save(imagePath, "PNG"));

    CoverImageProvider provider;
    QQuickImageResponse *response = provider.requestImageResponse(imagePath, QSize(100, 100));
    QVERIFY(response != nullptr);

    provider.waitForDone();
    QVERIFY(static_cast<CoverImageResponse *>(response)->isFinished());

    QQuickTextureFactory *factory = response->textureFactory();
    QVERIFY(factory != nullptr);
    QImage result = factory->image();
    // Scaled to fit within 100x100 preserving aspect ratio (800x600 → 100x75)
    QVERIFY(result.width() <= 100);
    QVERIFY(result.height() <= 100);
    QVERIFY(!result.isNull());

    delete factory;
    delete response;
}

void BridgeClientTest::coverImageProviderHandlesMissingFile() {
    CoverImageProvider provider;
    QQuickImageResponse *response = provider.requestImageResponse(
        QStringLiteral("/nonexistent/path/cover.jpg"), QSize());
    QVERIFY(response != nullptr);

    provider.waitForDone();
    QVERIFY(static_cast<CoverImageResponse *>(response)->isFinished());

    QQuickTextureFactory *factory = response->textureFactory();
    QVERIFY(factory == nullptr);
    QVERIFY(!response->errorString().isEmpty());

    delete response;
}

void BridgeClientTest::coverImageProviderCancelDoesNotCrash() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString imagePath = tempDir.filePath(QStringLiteral("cancel.png"));
    QImage source(100, 100, QImage::Format_ARGB32);
    source.fill(Qt::green);
    QVERIFY(source.save(imagePath, "PNG"));

    // Cancelling before the worker runs still emits finished().
    {
        CoverImageResponse response(imagePath, QSize());
        QSignalSpy finishedSpy(&response, &QQuickImageResponse::finished);
        response.cancel();
        response.run();
        QCOMPARE(finishedSpy.count(), 1);
        QVERIFY(response.isFinished());
        QVERIFY(response.textureFactory() == nullptr);
    }

    // finished() may be emitted before anyone connects; wait for the worker so
    // that always happens here, and check that completion is still observable.
    CoverImageProvider provider;
    QQuickImageResponse *response = provider.requestImageResponse(imagePath, QSize());
    QVERIFY(response != nullptr);
    response->cancel();
    provider.waitForDone();
    QVERIFY(static_cast<CoverImageResponse *>(response)->isFinished());

    // After cancel, textureFactory may be null or valid — either is acceptable.
    delete response->textureFactory();
    delete response;
}

void BridgeClientTest::coverImageProviderUrlForPathFormat() {
    QCOMPARE(CoverImageProvider::urlForPath(QString()), QString());
    QCOMPARE(CoverImageProvider::urlForPath(QStringLiteral("")), QString());

    const QString url = CoverImageProvider::urlForPath(
        QStringLiteral("/mnt/nassikka/Music/cover.jpg"));
    QCOMPARE(url, QStringLiteral("image://covers//mnt/nassikka/Music/cover.jpg"));
    QCOMPARE(CoverImageProvider::urlForPath(QStringLiteral("/music/cover.jpg"), 42),
             QStringLiteral("image://covers//music/cover.jpg#v=42"));

    // Qt passes the fragment in the image id; only a trailing stamp is the
    // provider's, whatever else the path holds.
    QCOMPARE(CoverImageProvider::pathForId(QStringLiteral("/music/cover.jpg#v=42")),
             QStringLiteral("/music/cover.jpg"));
    QCOMPARE(CoverImageProvider::stampForId(QStringLiteral("/music/cover.jpg#v=42")), quint64(42));
    QCOMPARE(CoverImageProvider::pathForId(QStringLiteral("/music/#v=1/cover.jpg")),
             QStringLiteral("/music/#v=1/cover.jpg"));
    QCOMPARE(CoverImageProvider::stampForId(QStringLiteral("/music/#v=1/cover.jpg")), quint64(0));
    QCOMPARE(CoverImageProvider::pathForId(QStringLiteral("/music/a#b/cover.jpg")),
             QStringLiteral("/music/a#b/cover.jpg"));
}

void BridgeClientTest::coverImageProviderReadsTheFileOfAStampedId() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString imagePath = tempDir.filePath(QStringLiteral("cover.png"));
    QImage source(20, 10, QImage::Format_ARGB32);
    source.fill(Qt::blue);
    QVERIFY(source.save(imagePath, "PNG"));

    CoverImageProvider provider;
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(imagePath + QStringLiteral("#v=7"), QSize()));
    provider.waitForDone();

    std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
    QVERIFY2(factory != nullptr, qPrintable(response->errorString()));
    QCOMPARE(factory->image().size(), QSize(20, 10));
}

namespace {

// Loads `source` into a QML Image and returns the size it shows.
QSize shownImageSize(QObject *image, const QString &source) {
    // Image.Ready is 1, Image.Loading 2.
    image->setProperty("source", QUrl(source));
    const bool loaded = QTest::qWaitFor(
        [image]() { return image->property("status").toInt() != 2; }, 5000);
    if (!loaded || image->property("status").toInt() != 1) {
        return {};
    }
    return QSize(image->property("implicitWidth").toInt(), image->property("implicitHeight").toInt());
}

} // namespace

void BridgeClientTest::replacedCoverReloadsForANewStampButNotFromTheCache() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString coverPath = tempDir.filePath(QStringLiteral("cover.png"));
    const auto writeCover = [&coverPath](const QSize &size) {
        QImage image(size, QImage::Format_ARGB32);
        image.fill(Qt::green);
        // As artwork apply does: a new file renamed over the old one.
        const QString next = coverPath + QStringLiteral(".next.png");
        return image.save(next, "PNG") && std::rename(
            QFile::encodeName(next).constData(), QFile::encodeName(coverPath).constData()) == 0;
    };
    QVERIFY(writeCover(QSize(4, 3)));

    QQmlEngine engine;
    engine.addImageProvider(QStringLiteral("covers"), new CoverImageProvider);
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nImage { asynchronous: true; cache: true }", QUrl());
    std::unique_ptr<QObject> image(component.create());
    QVERIFY2(image != nullptr, qPrintable(component.errorString()));
    BridgeClient client;
    isolateBridgeClient(client);

    // The now-playing view, through the provider, and the library tree,
    // through a file URL, show the image of their stamp.
    const QList<std::function<QString(quint64)>> views{
        [&coverPath](quint64 stamp) { return CoverImageProvider::urlForPath(coverPath, stamp); },
        [&client, &coverPath](quint64 stamp) {
            return client.libraryThumbnailSource(coverPath, QString::number(stamp));
        },
    };
    for (const auto &url : views) {
        QVERIFY(writeCover(QSize(4, 3)));
        QCOMPARE(shownImageSize(image.get(), url(1)), QSize(4, 3));
        QVERIFY(writeCover(QSize(8, 6)));
        image->setProperty("source", QUrl());
        // The same URL comes from Qt's cache, as it did before stamps.
        QCOMPARE(shownImageSize(image.get(), url(1)), QSize(4, 3));
        // The scan gives the replaced file a new stamp, so a new URL.
        QCOMPARE(shownImageSize(image.get(), url(2)), QSize(8, 6));
    }
}

void BridgeClientTest::coverRevisionLinksKeepThePublishedLinkThroughABurst() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString linkDir = dir.filePath(QStringLiteral("cache/ferrous/mpris_art"));
    const QString cover = dir.filePath(QStringLiteral("cover.jpg"));
    QFile coverFile(cover);
    QVERIFY(coverFile.open(QIODevice::WriteOnly));
    coverFile.write("cover");
    coverFile.close();

    CoverRevisionLinks links(linkDir);
    QSignalSpy ready(&links, &CoverRevisionLinks::linkReady);
    QCOMPARE(links.linkFor(cover, 1), QString());
    QVERIFY(ready.wait(5000));
    const QString published = links.linkFor(cover, 1);
    QVERIFY(!published.isEmpty());

    // Six revisions arrive before any of their links reaches the UI: the
    // worker makes them all, more than the links it keeps.
    for (quint64 stamp = 2; stamp <= 7; ++stamp) {
        QCOMPARE(links.linkFor(cover, stamp), QString());
    }
    links.waitForDone();
    QVERIFY2(QFileInfo(published).isSymLink(), "the published link is still there");
    QFile readable(published);
    QVERIFY(readable.open(QIODevice::ReadOnly));
    QCOMPARE(readable.readAll(), QByteArray("cover"));

    // The newest link reaches the UI and replaces it; the next revision's
    // cleanup may then remove the old one.
    QTRY_VERIFY_WITH_TIMEOUT(!links.pending(), 5000);
    const QString newest = links.linkFor(cover, 7);
    QVERIFY(!newest.isEmpty() && newest != published);
    QCOMPARE(links.linkFor(cover, 8), QString());
    QVERIFY(ready.wait(5000));
    QVERIFY(!QFileInfo(published).exists() && !QFileInfo(published).isSymLink());
    QVERIFY(QFileInfo(newest).isSymLink());
}

int main(int argc, char **argv) {
    // Bridge construction starts library workers; keep scans and tag operations
    // independent of saved application data and concurrent test processes.
    QTemporaryDir testStorage;
    if (!testStorage.isValid()) return 1;
    qputenv("XDG_DATA_HOME", QFile::encodeName(testStorage.filePath("data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(testStorage.filePath("config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(testStorage.filePath("cache")));
    QApplication app(argc, argv);
    BridgeClientTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_bridge_client.moc"
