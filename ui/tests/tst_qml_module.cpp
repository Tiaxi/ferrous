// SPDX-License-Identifier: GPL-3.0-or-later

// Loads the FerrousUi QML module as the app does: from the library that the
// app links, which holds the files qt_add_qml_module lists, rather than from
// the source tree. A file left out of that list fails here.

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "../src/LibraryTreeModel.h"
#include "../src/QmlTypes.h"
#include "TestPlatform.h"

namespace {

// Where the module's files are in the resources, as in the app.
const QString kModuleFiles = QStringLiteral(":/qt/qml/FerrousUi/qml");

QMutex &capturedMessageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
}

QtMessageHandler &previousMessageHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void captureQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker locker(&capturedMessageMutex());
        capturedMessages().push_back(message);
    }
    if (previousMessageHandler()) {
        previousMessageHandler()(type, context, message);
    } else {
        qt_message_output(type, context, message);
    }
}

QString takeCapturedMessagesText() {
    QMutexLocker locker(&capturedMessageMutex());
    const QString text = capturedMessages().join('\n');
    capturedMessages().clear();
    return text;
}

// The paths of the files under `root`, relative to it.
QStringList filesUnder(const QString &root, const QStringList &nameFilters = {}) {
    QStringList files;
    QDirIterator it(root, nameFilters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        files.push_back(QDir(root).relativeFilePath(it.next()));
    }
    files.sort();
    return files;
}

} // namespace

class QmlModuleTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void loadsMainFromTheModule();
    void theModuleHasEveryFileOfTheQmlSourceTree();
    void everyQmlFileOfTheModuleCompiles();
    void drawsOffscreenUnlessAPlatformIsChosen();
};

void QmlModuleTest::initTestCase() {
    previousMessageHandler() = qInstallMessageHandler(captureQtMessage);
    registerFerrousQmlTypes();
}

void QmlModuleTest::init() {
    takeCapturedMessagesText();
}

void QmlModuleTest::cleanup() {
    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings));
}

void QmlModuleTest::cleanupTestCase() {
    qInstallMessageHandler(previousMessageHandler());
    previousMessageHandler() = nullptr;
}

void QmlModuleTest::loadsMainFromTheModule() {
    // As the app does, without the bridge: Main.qml has a fallback for it.
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    QList<QQmlError> errors;
    connect(&engine, &QQmlEngine::warnings, this, [&errors](const QList<QQmlError> &warnings) {
        errors += warnings;
    });

    engine.loadFromModule("FerrousUi", "Main");
    QCOMPARE(engine.rootObjects().size(), 1);
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    QVERIFY(window != nullptr);
    QCOMPARE(qmlContext(window)->baseUrl(), QUrl(QStringLiteral("qrc:/qt/qml/FerrousUi/qml/Main.qml")));

    // What the window makes as it shows and draws its first frames loads too.
    QSignalSpy frames(window, &QQuickWindow::frameSwapped);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QTRY_VERIFY(frames.count() >= 2);

    QStringList errorText;
    for (const QQmlError &error : std::as_const(errors)) {
        errorText.push_back(error.toString());
    }
    QVERIFY2(errors.isEmpty(), qPrintable(errorText.join('\n')));
}

void QmlModuleTest::theModuleHasEveryFileOfTheQmlSourceTree() {
    // A file the app uses but qt_add_qml_module does not list is missing
    // from the app; one that nothing uses should be deleted.
    const QStringList sources = filesUnder(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml"));
    QVERIFY(sources.contains(QStringLiteral("Main.qml")));
    QStringList missing;
    for (const QString &file : sources) {
        if (!QFile::exists(kModuleFiles + '/' + file)) {
            missing.push_back(file);
        }
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("Not in the QML module: ") + missing.join(QStringLiteral(", "))));
}

void QmlModuleTest::everyQmlFileOfTheModuleCompiles() {
    // Main.qml makes some of its parts only when they are first shown.
    QQmlEngine engine;
    const QStringList files = filesUnder(kModuleFiles, {QStringLiteral("*.qml")});
    QVERIFY(files.size() > 1);
    for (const QString &file : files) {
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc") + kModuleFiles + '/' + file));
        QVERIFY2(component.isReady(), qPrintable(file + QStringLiteral(": ") + component.errorString()));
    }
}

void QmlModuleTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault();
    QTemporaryDir testStorage;
    if (!testStorage.isValid()) return 1;
    qputenv("XDG_DATA_HOME", QFile::encodeName(testStorage.filePath("data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(testStorage.filePath("config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(testStorage.filePath("cache")));
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");

    QApplication app(argc, argv);
    QmlModuleTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_qml_module.moc"
