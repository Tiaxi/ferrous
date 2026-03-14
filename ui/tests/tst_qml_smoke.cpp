#include <QApplication>
#include <QFileInfo>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include "../src/LibraryTreeModel.h"
#define private public
#include "../src/SpectrogramItem.h"
#undef private
#include "../src/WaveformItem.h"

namespace {

struct BinaryTreeRow {
    quint8 rowType{0};
    quint16 depth{0};
    qint32 sourceIndex{-1};
    quint16 trackNumber{0};
    quint16 childCount{0};
    QString title;
    QString key;
    QString artist;
    QString path;
    QString coverPath;
    QString trackPath;
    QStringList playPaths;
};

template <typename T>
void appendLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<int>(sizeof(T)));
}

void appendUtf8U16(QByteArray &out, const QString &value) {
    QByteArray utf8 = value.toUtf8();
    if (utf8.size() > 65535) {
        utf8.truncate(65535);
    }
    appendLe<quint16>(out, static_cast<quint16>(utf8.size()));
    out.append(utf8);
}

QByteArray encodeRows(const QVector<BinaryTreeRow> &rows) {
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const BinaryTreeRow &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, row.sourceIndex);
        appendLe<quint16>(out, row.trackNumber);
        appendLe<quint16>(out, row.childCount);
        appendUtf8U16(out, row.title);
        appendUtf8U16(out, row.key);
        appendUtf8U16(out, row.artist);
        appendUtf8U16(out, row.path);
        appendUtf8U16(out, row.coverPath);
        appendUtf8U16(out, row.trackPath);
        appendLe<quint16>(out, static_cast<quint16>(row.playPaths.size()));
        for (const QString &playPath : row.playPaths) {
            appendUtf8U16(out, playPath);
        }
    }
    return out;
}

QByteArray sampleArtistAlbumTreeBinary() {
    const QString trackPath = QStringLiteral("/music/artist/album/track01.flac");
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        1,
        QStringLiteral("Artist A (1)"),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        2,
        1,
        0,
        0,
        1,
        QStringLiteral("Album A"),
        QStringLiteral("album|Artist A|Album A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A/Album A"),
        QStringLiteral("/music/Artist A/Album A/cover.jpg"),
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        4,
        2,
        -1,
        1,
        0,
        QStringLiteral("01 - Track 01"),
        QStringLiteral("track|/music/artist/album/track01.flac"),
        QStringLiteral("Artist A"),
        trackPath,
        {},
        trackPath,
        QStringList{trackPath},
    });
    return encodeRows(rows);
}

QByteArray artistWithManyAlbumsBinary(int albumCount) {
    QVector<BinaryTreeRow> rows;
    rows.reserve(1 + albumCount * 2);
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        static_cast<quint16>(albumCount),
        QStringLiteral("Artist A (%1)").arg(albumCount),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });

    for (int i = 0; i < albumCount; ++i) {
        const QString albumName = QStringLiteral("Album %1").arg(i + 1);
        const QString albumPath = QStringLiteral("/music/artist/%1").arg(albumName.toLower().replace(' ', ""));
        const QString trackPath = albumPath + QStringLiteral("/track.flac");
        rows.push_back(BinaryTreeRow{
            2,
            1,
            i,
            0,
            1,
            albumName,
            QStringLiteral("album|Artist A|%1").arg(albumName),
            QStringLiteral("Artist A"),
            albumPath,
            albumPath + QStringLiteral("/cover.jpg"),
            {},
            {},
        });
        rows.push_back(BinaryTreeRow{
            4,
            2,
            -1,
            1,
            0,
            QStringLiteral("01 - Track %1").arg(i + 1),
            QStringLiteral("track|%1").arg(trackPath),
            QStringLiteral("Artist A"),
            trackPath,
            {},
            trackPath,
            QStringList{trackPath},
        });
    }

    return encodeRows(rows);
}

QByteArray artistOnlyLazyBinary() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        2,
        QStringLiteral("Artist A (2)"),
        QStringLiteral("artist|/music|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    return encodeRows(rows);
}

QByteArray multiRootBinary() {
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        0,
        0,
        -1,
        0,
        1,
        QStringLiteral("/music-a"),
        QStringLiteral("root|/music-a"),
        {},
        QStringLiteral("/music-a"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        1,
        1,
        -1,
        0,
        0,
        QStringLiteral("Artist A (0)"),
        QStringLiteral("artist|/music-a|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music-a/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        0,
        0,
        -1,
        0,
        1,
        QStringLiteral("/music-b"),
        QStringLiteral("root|/music-b"),
        {},
        QStringLiteral("/music-b"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        1,
        1,
        -1,
        0,
        0,
        QStringLiteral("Artist B (0)"),
        QStringLiteral("artist|/music-b|Artist B"),
        QStringLiteral("Artist B"),
        QStringLiteral("/music-b/Artist B"),
        {},
        {},
        {},
    });
    return encodeRows(rows);
}

} // namespace

class QmlSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void loadsMainQmlWithFallbackBridge();
    void tagEditorLibrarySupportGateMatchesSupportedRows();
    void libraryTreeStartsCollapsedByDefault();
    void rootRowsStartExpandedByDefault();
    void artistExpansionPopulatesInBatches();
    void lazyArtistRowRequestsBackendExpansion();
    void spectrogramItemRendersNonBackgroundPixels();
    void spectrogramItemRendersRowsAppendedAfterInitialBlankFrame();
    void spectrogramSeedsOnlyFirstResetBurstIntoHistory();
    void spectrogramSteadyStateAppendKeepsRowsPendingForAnimation();
    void spectrogramHaltDropsPendingMotion();
    void stoppedTrackSwitchRequiresSpectrogramResetOnResume();
};

void QmlSmokeTest::loadsMainQmlWithFallbackBridge() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);

    const QString qmlPath = QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml");
    QVERIFY2(QFileInfo::exists(qmlPath), qPrintable(QStringLiteral("QML file missing: %1").arg(qmlPath)));

    const QUrl url = QUrl::fromLocalFile(qmlPath);
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
}

void QmlSmokeTest::tagEditorLibrarySupportGateMatchesSupportedRows() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);

    const QUrl url = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml"));
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);

    QVariant supported;
    QVariant unsupported;
    const QVariant supportedRow = QVariant::fromValue(QVariantMap{
        {QStringLiteral("rowType"), QStringLiteral("album")},
        {QStringLiteral("selectionKey"), QStringLiteral("album|/music|Artist|Album")},
    });
    const QVariant unsupportedRow = QVariant::fromValue(QVariantMap{
        {QStringLiteral("rowType"), QStringLiteral("artist")},
        {QStringLiteral("selectionKey"), QStringLiteral("artist|/music|Artist")},
    });
    const bool supportedInvoked = QMetaObject::invokeMethod(
        root,
        "canOpenTagEditorForLibrary",
        Q_RETURN_ARG(QVariant, supported),
        Q_ARG(QVariant, supportedRow));
    const bool unsupportedInvoked = QMetaObject::invokeMethod(
        root,
        "canOpenTagEditorForLibrary",
        Q_RETURN_ARG(QVariant, unsupported),
        Q_ARG(QVariant, unsupportedRow));

    QVERIFY(supportedInvoked);
    QVERIFY(unsupportedInvoked);
    QCOMPARE(supported.toBool(), true);
    QCOMPARE(unsupported.toBool(), false);
}

void QmlSmokeTest::libraryTreeStartsCollapsedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(sampleArtistAlbumTreeBinary());

    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void QmlSmokeTest::rootRowsStartExpandedByDefault() {
    LibraryTreeModel model;
    model.setLibraryTreeFromBinary(multiRootBinary());

    QTRY_COMPARE(model.rowCount(), 4);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.data(model.index(1, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
    QCOMPARE(model.data(model.index(2, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("root"));
    QCOMPARE(model.data(model.index(3, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void QmlSmokeTest::artistExpansionPopulatesInBatches() {
    LibraryTreeModel model;

    model.setLibraryTreeFromBinary(artistWithManyAlbumsBinary(80));
    QTRY_COMPARE(model.rowCount(), 1);

    model.toggleArtist(QStringLiteral("Artist A"));

    QTRY_COMPARE(model.rowCount(), 81);
}

void QmlSmokeTest::lazyArtistRowRequestsBackendExpansion() {
    LibraryTreeModel model;
    QSignalSpy spy(&model, SIGNAL(nodeExpansionRequested(QString,bool)));

    model.setLibraryTreeFromBinary(artistOnlyLazyBinary());
    QTRY_COMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), false);

    model.toggleArtist(QStringLiteral("Artist A"));
    QTRY_COMPARE(spy.count(), 1);
    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.value(0).toString(), QStringLiteral("artist|/music|Artist A"));
    QCOMPARE(args.value(1).toBool(), true);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::ExpandedRole).toBool(), true);
}

void QmlSmokeTest::spectrogramItemRendersNonBackgroundPixels() {
    QQuickWindow window;
    window.resize(320, 180);

    auto *item = new SpectrogramItem(window.contentItem());
    item->setWidth(320);
    item->setHeight(180);
    item->setSampleRateHz(48000);

    constexpr int rowCount = 320;
    constexpr int binsPerRow = 128;
    QByteArray packedRows;
    packedRows.resize(rowCount * binsPerRow);
    for (int row = 0; row < rowCount; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            const int index = row * binsPerRow + bin;
            packedRows[index] = static_cast<char>((row * 5 + bin * 3) % 256);
        }
    }
    item->appendPackedRows(packedRows, rowCount, binsPerRow);

    window.show();
    QTest::qWait(100);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const QImage frame = window.grabWindow();
    QVERIFY2(!frame.isNull(), "Spectrogram frame grab failed");

    const QColor background(0x0b, 0x0b, 0x0f);
    int minX = frame.width();
    int maxX = -1;
    int nonBackgroundPixels = 0;
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            if (frame.pixelColor(x, y) != background) {
                ++nonBackgroundPixels;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
        }
    }
    QVERIFY2(nonBackgroundPixels > (frame.width() * frame.height()) / 50,
        "Spectrogram rendered too few non-background pixels");
    QVERIFY2(maxX >= 0 && (maxX - minX) > frame.width() / 3,
        "Spectrogram pixels did not span enough horizontal width");
}

void QmlSmokeTest::spectrogramItemRendersRowsAppendedAfterInitialBlankFrame() {
    QQuickWindow window;
    window.resize(320, 180);

    auto *item = new SpectrogramItem(window.contentItem());
    item->setWidth(320);
    item->setHeight(180);
    item->setSampleRateHz(48000);

    window.show();
    QTest::qWait(50);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    constexpr int rowCount = 320;
    constexpr int binsPerRow = 128;
    QByteArray packedRows;
    packedRows.resize(rowCount * binsPerRow);
    for (int row = 0; row < rowCount; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            const int index = row * binsPerRow + bin;
            packedRows[index] = static_cast<char>((row * 11 + bin * 7) % 256);
        }
    }
    item->appendPackedRows(packedRows, rowCount, binsPerRow);

    QTest::qWait(100);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    const QImage frame = window.grabWindow();
    QVERIFY2(!frame.isNull(), "Spectrogram frame grab failed after delayed append");

    const QColor background(0x0b, 0x0b, 0x0f);
    int minX = frame.width();
    int maxX = -1;
    int nonBackgroundPixels = 0;
    for (int y = 0; y < frame.height(); ++y) {
        for (int x = 0; x < frame.width(); ++x) {
            if (frame.pixelColor(x, y) != background) {
                ++nonBackgroundPixels;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
        }
    }
    QVERIFY2(nonBackgroundPixels > (frame.width() * frame.height()) / 50,
        "Spectrogram stayed nearly blank after delayed append");
    QVERIFY2(maxX >= 0 && (maxX - minX) > frame.width() / 3,
        "Delayed spectrogram append only rendered a narrow strip");
}

void QmlSmokeTest::spectrogramSeedsOnlyFirstResetBurstIntoHistory() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setSampleRateHz(48000);

    constexpr int rowCount = 24;
    constexpr int binsPerRow = 32;
    QByteArray packedRows;
    packedRows.resize(rowCount * binsPerRow);
    for (int row = 0; row < rowCount; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            packedRows[row * binsPerRow + bin] = static_cast<char>((row * 13 + bin * 5) % 256);
        }
    }

    item.appendPackedRows(packedRows, rowCount, binsPerRow);

    QCOMPARE(item.m_columns.size(), static_cast<size_t>(rowCount - 2));
    QCOMPARE(item.m_pendingColumns.size(), static_cast<size_t>(2));
    QCOMPARE(item.m_binsPerColumn, binsPerRow);
    QVERIFY(!item.m_seedHistoryOnNextAppend);
}

void QmlSmokeTest::spectrogramSteadyStateAppendKeepsRowsPendingForAnimation() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setSampleRateHz(48000);

    constexpr int initialRows = 24;
    constexpr int extraRows = 8;
    constexpr int binsPerRow = 32;
    QByteArray initialPackedRows;
    initialPackedRows.resize(initialRows * binsPerRow);
    for (int row = 0; row < initialRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            initialPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 7 + bin * 3) % 256);
        }
    }
    item.appendPackedRows(initialPackedRows, initialRows, binsPerRow);

    const size_t seededColumns = item.m_columns.size();
    const size_t seededPending = item.m_pendingColumns.size();

    QByteArray extraPackedRows;
    extraPackedRows.resize(extraRows * binsPerRow);
    for (int row = 0; row < extraRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            extraPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 17 + bin * 11) % 256);
        }
    }
    item.appendPackedRows(extraPackedRows, extraRows, binsPerRow);

    QCOMPARE(item.m_columns.size(), seededColumns);
    QCOMPARE(item.m_pendingColumns.size(), seededPending + static_cast<size_t>(extraRows));
    QVERIFY(!item.m_seedHistoryOnNextAppend);
}

void QmlSmokeTest::spectrogramHaltDropsPendingMotion() {
    SpectrogramItem item;
    item.setWidth(320);
    item.setHeight(180);
    item.setSampleRateHz(48000);

    constexpr int initialRows = 24;
    constexpr int extraRows = 8;
    constexpr int binsPerRow = 32;
    QByteArray initialPackedRows;
    initialPackedRows.resize(initialRows * binsPerRow);
    for (int row = 0; row < initialRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            initialPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 7 + bin * 3) % 256);
        }
    }
    item.appendPackedRows(initialPackedRows, initialRows, binsPerRow);

    QByteArray extraPackedRows;
    extraPackedRows.resize(extraRows * binsPerRow);
    for (int row = 0; row < extraRows; ++row) {
        for (int bin = 0; bin < binsPerRow; ++bin) {
            extraPackedRows[row * binsPerRow + bin] = static_cast<char>((row * 17 + bin * 11) % 256);
        }
    }
    item.appendPackedRows(extraPackedRows, extraRows, binsPerRow);
    QVERIFY(!item.m_pendingColumns.empty());

    item.halt();

    QVERIFY(item.m_pendingColumns.empty());
    QCOMPARE(item.m_pendingPhase, 0.0);
}

void QmlSmokeTest::stoppedTrackSwitchRequiresSpectrogramResetOnResume() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);

    const QUrl url = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml"));
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QVERIFY(root != nullptr);

    QVariant result;
    bool invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Stopped")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/old-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/new-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), true);

    invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/old-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/new-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), false);

    invoked = QMetaObject::invokeMethod(
        root,
        "shouldResetSpectrogramForStoppedTrackSwitch",
        Q_RETURN_ARG(QVariant, result),
        Q_ARG(QVariant, QStringLiteral("Stopped")),
        Q_ARG(QVariant, QStringLiteral("Playing")),
        Q_ARG(QVariant, QStringLiteral("/music/same-track.flac")),
        Q_ARG(QVariant, QStringLiteral("/music/same-track.flac")));
    QVERIFY(invoked);
    QCOMPARE(result.toBool(), false);
}

int main(int argc, char **argv) {
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");

    QApplication app(argc, argv);
    QmlSmokeTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qml_smoke.moc"
