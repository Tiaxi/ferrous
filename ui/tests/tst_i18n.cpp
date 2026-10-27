// SPDX-License-Identifier: GPL-3.0-or-later

// The user interface under x-test, a pseudo-translation that wraps every
// message in ⟦ and ⟧: what Ferrous shows comes from the catalog, plurals
// follow the count, and no logic depends on the English text.

#include <KLocalizedString>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include <clocale>
#include <cstring>
#include <memory>

#define private public
#include "../src/BridgeClient.h"
#include "../src/TagEditorController.h"
#undef private
#include "../src/FerrousBridgeFfi.h"
#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/SpectrogramItem.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"

namespace {

QtMessageHandler &previousMessageHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

QMutex &capturedMessageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
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
    const QString text = capturedMessages().join(QLatin1Char('\n'));
    capturedMessages().clear();
    return text;
}

// `english` as the x-test catalog has it.
QString pseudo(const QString &english) {
    return QStringLiteral("⟦") + english + QStringLiteral("⟧");
}

QUrl sourceUrl(const QString &relativePath) {
    return QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QLatin1Char('/') + relativePath);
}

// Evaluates the QML `expression` in a component next to Main.qml, with
// `imports`, and returns its value as text.
QString evaluateQml(QQmlEngine &engine, const QString &imports, const QString &expression) {
    QQmlComponent component(&engine);
    component.setData(
        (QStringLiteral("import QtQml\nimport FerrousUi 1.0\n") + imports
         + QStringLiteral("\nQtObject { property string value: ") + expression + QStringLiteral(" }"))
            .toUtf8(),
        sourceUrl(QStringLiteral("qml/I18nHarness.qml")));
    std::unique_ptr<QObject> object(component.create());
    if (object == nullptr) {
        return QStringLiteral("<error: %1>").arg(component.errorString());
    }
    return object->property("value").toString();
}

// Stops the in-process bridge that `client` started, as the bridge tests do.
void isolateBridgeClient(BridgeClient &client) {
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

// Two rows whose titles differ and whose albums are both the text that the
// tag editor shows for different values.
QByteArray mixedTagRows() {
    QJsonArray rows;
    QJsonArray paths;
    for (int i = 0; i < 2; ++i) {
        const QString path = QStringLiteral("/fixture/Album/%1 Song.flac").arg(i + 1);
        paths.push_back(path);
        rows.push_back(QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("fileName"), QStringLiteral("%1 Song.flac").arg(i + 1)},
            {QStringLiteral("directory"), QStringLiteral("/fixture/Album")},
            {QStringLiteral("formatKind"), QStringLiteral("FLAC")},
            {QStringLiteral("title"), QStringLiteral("Song %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("Artist")},
            {QStringLiteral("album"), QStringLiteral("<keep>")},
        });
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("resolvedPaths"), paths},
                                     {QStringLiteral("rows"), rows}})
        .toJson(QJsonDocument::Compact);
}

// Searches the visual tree, where a dialog's delegates are, unlike the
// object tree.
QQuickItem *findQuickItemByObjectName(QQuickItem *root, const QString &objectName) {
    if (root == nullptr || root->objectName() == objectName) {
        return root;
    }
    const QList<QQuickItem *> children = root->childItems();
    for (QQuickItem *child : children) {
        if (QQuickItem *match = findQuickItemByObjectName(child, objectName)) {
            return match;
        }
    }
    return nullptr;
}

QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel) {
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(sourceUrl(QStringLiteral("qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    return window;
}

} // namespace

class I18nTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void cppMessagesComeFromTheCatalog();
    void qmlMessagesComeFromTheCatalog();
    void pluralsFollowTheCount();
    void javaScriptLibrariesTranslate();
    void channelLayoutIconFollowsTheLayoutNotItsText();
    void mainWindowShowsTranslatedText();
    void tagEditorTellsMixedValuesByAFlag();
};

void I18nTest::initTestCase() {
    previousMessageHandler() = qInstallMessageHandler(captureQtMessage);
    // Gettext ignores the language KI18n asks for under the C locale.
    const char *messages = std::setlocale(LC_MESSAGES, nullptr);
    QVERIFY2(messages != nullptr && std::strcmp(messages, "C") != 0
                 && std::strcmp(messages, "POSIX") != 0 && std::strcmp(messages, "C.UTF-8") != 0,
             "The en_US.UTF-8 locale is missing; on Fedora, install glibc-langpack-en.");
    KLocalizedString::addDomainLocaleDir(
        FerrousLocalization::kTranslationDomain, QStringLiteral(FERROUS_PSEUDO_LOCALE_DIR));
    KLocalizedString::setLanguages({QStringLiteral("x-test")});
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
}

void I18nTest::init() {
    takeCapturedMessagesText();
}

void I18nTest::cleanup() {
    KLocalizedString::setLanguages({QStringLiteral("x-test")});
    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings));
}

void I18nTest::cleanupTestCase() {
    qInstallMessageHandler(previousMessageHandler());
    previousMessageHandler() = nullptr;
}

void I18nTest::cppMessagesComeFromTheCatalog() {
    QCOMPARE(i18nc("@info:status", "Loading tags..."), pseudo(QStringLiteral("Loading tags...")));

    // A status the tag editor makes, through the catalog of its domain.
    TagEditorController editor(nullptr);
    editor.m_executeOperation = [](TagEditorController::Operation, const QByteArray &,
                                   const TagEditorController::RenameGate &) {
        return mixedTagRows();
    };
    QVERIFY(editor.openSelection(
        QVariantList{QVariantMap{{QStringLiteral("path"), QStringLiteral("/fixture")}}}));
    QCOMPARE(editor.statusText(), pseudo(QStringLiteral("Loading tags...")));
    QTRY_VERIFY(!editor.loading());
}

void I18nTest::qmlMessagesComeFromTheCatalog() {
    QQmlEngine engine;
    QCOMPARE(evaluateQml(engine, {}, QStringLiteral(R"qml(I18n.i18nc("@title:window", "Edit Tags"))qml")),
             pseudo(QStringLiteral("Edit Tags")));
    // Arguments go into the translation.
    QCOMPARE(evaluateQml(engine, {}, QStringLiteral(R"qml(I18n.i18nc("@info:status bitrate", "%1 kbps", 320))qml")),
             pseudo(QStringLiteral("320 kbps")));
    // A message missing from the catalog shows as written.
    QCOMPARE(evaluateQml(engine, {}, QStringLiteral(R"qml(I18n.i18nc("@info", "Not in the template"))qml")),
             QStringLiteral("Not in the template"));
}

void I18nTest::pluralsFollowTheCount() {
    // The queue summary of `count` tracks, as the status bar shows it.
    const auto queueSummary = [](QQmlEngine &engine, int count) {
        return evaluateQml(
            engine, {},
            QStringLiteral(R"qml(I18n.i18ncp("@info:status queue length and total duration", )qml"
                           R"qml("%1 track (%2)", "%1 tracks (%2)", )qml")
                + QString::number(count) + QStringLiteral(R"qml(, "3:00"))qml"));
    };
    {
        QQmlEngine engine;
        QCOMPARE(queueSummary(engine, 1), pseudo(QStringLiteral("1 track (3:00)")));
        QCOMPARE(queueSummary(engine, 5), pseudo(QStringLiteral("5 tracks (3:00)")));
        QCOMPARE(queueSummary(engine, 0), pseudo(QStringLiteral("0 tracks (3:00)")));
        QCOMPARE(i18ncp("@info:status", "Saved %1 file.", "Saved %1 files.", 1),
                 pseudo(QStringLiteral("Saved 1 file.")));
        QCOMPARE(i18ncp("@info:status", "Saved %1 file.", "Saved %1 files.", 3),
                 pseudo(QStringLiteral("Saved 3 files.")));
    }

    // In English, without a catalog, the same forms.
    KLocalizedString::setLanguages({QStringLiteral("en_US")});
    QQmlEngine engine;
    QCOMPARE(queueSummary(engine, 1), QStringLiteral("1 track (3:00)"));
    QCOMPARE(queueSummary(engine, 5), QStringLiteral("5 tracks (3:00)"));
    QCOMPARE(i18ncp("@info:status", "Saved %1 file.", "Saved %1 files.", 1), QStringLiteral("Saved 1 file."));
    QCOMPARE(i18ncp("@info:status", "Saved %1 file.", "Saved %1 files.", 3), QStringLiteral("Saved 3 files."));
}

void I18nTest::javaScriptLibrariesTranslate() {
    QQmlEngine engine;
    const QString errorUtils = QStringLiteral(R"qml(import "logic/ErrorUtils.js" as ErrorUtils)qml");
    QCOMPARE(evaluateQml(engine, errorUtils, QStringLiteral("ErrorUtils.categoryLabel(ErrorUtils.CategoryTags)")),
             pseudo(QStringLiteral("Tags")));
    // An unknown category is the system's.
    QCOMPARE(evaluateQml(engine, errorUtils, QStringLiteral("ErrorUtils.categoryLabel(99)")),
             pseudo(QStringLiteral("System")));
    const QString formatUtils = QStringLiteral(R"qml(import "logic/FormatUtils.js" as FormatUtils)qml");
    QCOMPARE(evaluateQml(engine, formatUtils, QStringLiteral("FormatUtils.repeatModeText(1)")),
             pseudo(QStringLiteral("Repeat one")));
}

void I18nTest::channelLayoutIconFollowsTheLayoutNotItsText() {
    BridgeClient client;
    isolateBridgeClient(client);

    client.m_currentTrackChannels = 2;
    QCOMPARE(client.currentTrackChannelLayoutText(), pseudo(QStringLiteral("stereo")));
    QCOMPARE(client.currentTrackChannelLayoutIconKey(), QStringLiteral("stereo"));

    client.m_currentTrackChannels = 1;
    QCOMPARE(client.currentTrackChannelLayoutText(), pseudo(QStringLiteral("mono")));
    QCOMPARE(client.currentTrackChannelLayoutIconKey(), QStringLiteral("mono"));

    client.m_currentTrackChannels = 6;
    QCOMPARE(client.currentTrackChannelLayoutText(), pseudo(QStringLiteral("6 ch")));
    QVERIFY(client.currentTrackChannelLayoutIconKey().isEmpty());
}

void I18nTest::mainWindowShowsTranslatedText() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);

    QObject *dialog = window->findChild<QObject *>(QStringLiteral("tagEditorDialog"));
    QVERIFY(dialog != nullptr);
    QCOMPARE(dialog->property("title").toString(), pseudo(QStringLiteral("Edit Tags")));

    // The bridge's playback state is a code; the status bar shows its text.
    QObject *state = window->findChild<QObject *>(QStringLiteral("statusBarPlaybackStateLabel"));
    QVERIFY(state != nullptr);
    QCOMPARE(state->property("text").toString(), pseudo(QStringLiteral("Stopped")));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Playing")));
    QCOMPARE(state->property("text").toString(), pseudo(QStringLiteral("Playing")));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Paused")));
    QCOMPARE(state->property("text").toString(), pseudo(QStringLiteral("Paused")));
    QVERIFY(bridge->setProperty("playbackState", QStringLiteral("Stopped")));

    QObject *summary = window->findChild<QObject *>(QStringLiteral("statusBarQueueSummaryLabel"));
    QVERIFY(summary != nullptr);
    QVERIFY(bridge->setProperty("queueDurationText", QStringLiteral("4:00")));
    QVERIFY(bridge->setProperty("queueLength", 1));
    QCOMPARE(summary->property("text").toString(), pseudo(QStringLiteral("1 track (4:00)")));
    QVERIFY(bridge->setProperty("queueLength", 2));
    QCOMPARE(summary->property("text").toString(), pseudo(QStringLiteral("2 tracks (4:00)")));
}

void I18nTest::tagEditorTellsMixedValuesByAFlag() {
    TagEditorController editor(nullptr);
    editor.m_executeOperation = [](TagEditorController::Operation, const QByteArray &,
                                   const TagEditorController::RenameGate &) {
        return mixedTagRows();
    };
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("tagEditorDialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(editor.openSelection(
        QVariantList{QVariantMap{{QStringLiteral("path"), QStringLiteral("/fixture")}}}));
    QTRY_VERIFY(!editor.loading() && dialog->property("opened").toBool());
    // Loading selects the first row; the fields then show both.
    editor.selectAllRows();
    QVERIFY(editor.bulkValueMixed(QStringLiteral("title")));

    // The titles differ: the field is empty, and its placeholder, which is
    // translated, says the files keep their own.
    QQuickItem *title = findQuickItemByObjectName(window->contentItem(), QStringLiteral("tagEditorField_title"));
    QVERIFY(title != nullptr);
    QTRY_COMPARE(title->property("placeholderText").toString(), pseudo(QStringLiteral("<keep>")));
    QCOMPARE(title->property("text").toString(), QString());

    // The albums are the same, whatever their text: it is the field's value.
    QQuickItem *album = findQuickItemByObjectName(window->contentItem(), QStringLiteral("tagEditorField_album"));
    QVERIFY(album != nullptr);
    QCOMPARE(album->property("text").toString(), QStringLiteral("<keep>"));
    QCOMPARE(album->property("placeholderText").toString(), QString());

    editor.close();
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

int main(int argc, char **argv) {
    // Main.qml keeps its settings and caches here, away from the user's.
    QTemporaryDir testStorage;
    if (!testStorage.isValid()) return 1;
    qputenv("XDG_DATA_HOME", QFile::encodeName(testStorage.filePath("data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(testStorage.filePath("config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(testStorage.filePath("cache")));
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    qputenv("FERROUS_GST_OUTPUT_SINK", "fakesink");
    // A locale other than C, in which gettext uses the catalog of the language
    // that KI18n asks for; QApplication sets it up from the environment.
    qputenv("LC_ALL", "en_US.UTF-8");

    QApplication app(argc, argv);
    I18nTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_i18n.moc"
