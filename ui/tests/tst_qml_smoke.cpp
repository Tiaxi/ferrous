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
};

void QmlSmokeTest::loadsMainQmlWithFallbackBridge() {
    qmlRegisterType<SpectrogramItem>("FerrousNative", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousNative", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);

    const QString qmlPath = QStringLiteral(FERROUS_NATIVE_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml");
    QVERIFY2(QFileInfo::exists(qmlPath), qPrintable(QStringLiteral("QML file missing: %1").arg(qmlPath)));

    const QUrl url = QUrl::fromLocalFile(qmlPath);
    engine.load(url);
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
}

QTEST_MAIN(QmlSmokeTest)

#include "tst_qml_smoke.moc"
