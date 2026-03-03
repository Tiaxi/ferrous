#include <QFileInfo>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtTest/QtTest>
#include <qqml.h>

#include "../src/LibraryTreeModel.h"
#include "../src/SpectrogramItem.h"
#include "../src/WaveformItem.h"

class QmlSmokeTest : public QObject {
    Q_OBJECT

private slots:
    void loadsMainQmlWithFallbackBridge();
    void libraryTreeStartsCollapsedByDefault();
    void artistExpansionPopulatesInBatches();
};

void QmlSmokeTest::loadsMainQmlWithFallbackBridge() {
    qmlRegisterType<SpectrogramItem>("FerrousNative", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousNative", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);

    const QString qmlPath = QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml");
    QVERIFY2(QFileInfo::exists(qmlPath), qPrintable(QStringLiteral("QML file missing: %1").arg(qmlPath)));

    const QUrl url = QUrl::fromLocalFile(qmlPath);
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
}

void QmlSmokeTest::libraryTreeStartsCollapsedByDefault() {
    LibraryTreeModel model;

    const QVariantMap track{
        {QStringLiteral("title"), QStringLiteral("Track 01")},
        {QStringLiteral("path"), QStringLiteral("/music/artist/album/track01.flac")},
    };
    const QVariantMap album{
        {QStringLiteral("name"), QStringLiteral("Album A")},
        {QStringLiteral("count"), 1},
        {QStringLiteral("sourceIndex"), 0},
        {QStringLiteral("coverPath"), QStringLiteral("/music/artist/album/cover.jpg")},
        {QStringLiteral("tracks"), QVariantList{track}},
    };
    const QVariantMap artist{
        {QStringLiteral("artist"), QStringLiteral("Artist A")},
        {QStringLiteral("albums"), QVariantList{album}},
    };

    model.setLibraryTree(QVariantList{artist});

    // By default only artist rows should be visible until explicitly expanded.
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), LibraryTreeModel::RowTypeRole).toString(), QStringLiteral("artist"));
}

void QmlSmokeTest::artistExpansionPopulatesInBatches() {
    LibraryTreeModel model;

    QVariantList albums;
    albums.reserve(80);
    for (int i = 0; i < 80; ++i) {
        const QVariantMap track{
            {QStringLiteral("title"), QStringLiteral("Track %1").arg(i + 1)},
            {QStringLiteral("path"), QStringLiteral("/music/artist/album%1/track.flac").arg(i + 1)},
        };
        albums.push_back(QVariantMap{
            {QStringLiteral("name"), QStringLiteral("Album %1").arg(i + 1)},
            {QStringLiteral("count"), 1},
            {QStringLiteral("sourceIndex"), i},
            {QStringLiteral("coverPath"), QStringLiteral("/music/artist/album%1/cover.jpg").arg(i + 1)},
            {QStringLiteral("tracks"), QVariantList{track}},
        });
    }

    const QVariantMap artist{
        {QStringLiteral("artist"), QStringLiteral("Artist A")},
        {QStringLiteral("albums"), albums},
    };
    model.setLibraryTree(QVariantList{artist});
    QCOMPARE(model.rowCount(), 1);

    model.toggleArtist(QStringLiteral("Artist A"));

    // Expanded view should expose artist + album rows.
    QCOMPARE(model.rowCount(), 81);
}

QTEST_MAIN(QmlSmokeTest)

#include "tst_qml_smoke.moc"
