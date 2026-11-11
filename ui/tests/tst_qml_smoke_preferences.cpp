// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// Preferences, diagnostics and album art.
class PreferencesSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void albumArtTileKeepsHeightInsideColumnLayout();
    void libraryPreferencesToggleWatchingAndShowUnwatchedRoots();
    void albumArtViewerInfoUsesImageProviderLocalPath();
    void diagnosticsLogUsesLowercaseAppDir();
    void replacingAlbumArtAsksFirstWhenTheTrackHasArt();
    void clearingDiagnosticsAsksFirst();
    void enterConfirmsAsSoonAsTheConfirmationShows();
};

namespace {

// The diagnostics dialog over a window of its own, with a bridge that counts
// the clears in the window's `clears`.
QObject *createDiagnosticsHarness(QQmlApplicationEngine &engine, QString *errorText) {
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    return createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "dialogs" as Dialogs
ApplicationWindow {
    id: window
    width: 1100
    height: 800
    visible: true
    property int clears: 0
    property alias dialog: dialog
    Components.UiPalette { id: palette; windowRoot: window }
    QtObject {
        id: bridge
        property string diagnosticsText: "a line worth keeping"
        property string diagnosticsLogPath: "/fixture/diagnostics.log"
        signal diagnosticsChanged()
        function reloadDiagnosticsFromDisk() {}
        function clearDiagnostics() { window.clears++; diagnosticsText = "" }
        function openContainingFolder(path) {}
    }
    Dialogs.DiagnosticsDialog {
        id: dialog
        uiBridge: bridge
        uiPalette: palette
        windowRoot: window
    }
}
)QML"), baseUrl, errorText);
}

} // namespace

void PreferencesSmokeTest::libraryPreferencesToggleWatchingAndShowUnwatchedRoots() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "preferences" as Preferences

ApplicationWindow {
    id: harness
    width: 720
    height: 640
    visible: true

    QtObject {
        id: bridge
        objectName: "bridge"
        property bool libraryWatchEnabled: true
        property int watchToggles: 0
        property int librarySortMode: 0
        property var libraryRootEntries: [
            { path: "/music", name: "", displayName: "/music", unwatchedReason: "" },
            { path: "/nas", name: "NAS", displayName: "NAS", unwatchedReason: "it is on nfs4" }
        ]
        function setLibraryWatchEnabled(value) {
            libraryWatchEnabled = value
            watchToggles += 1
        }
        function setLibrarySortMode(mode) {}
        function rescanAllLibraryRoots() {}
        function openInFileBrowser(path) {}
        function rescanLibraryRoot(path) {}
        function removeLibraryRoot(path) {}
    }

    Components.UiPalette {
        id: palette
        windowRoot: harness
    }

    // Applies each change at once, where Configure Ferrous waits for OK.
    QtObject {
        id: settings
        function value(key) { return bridge[key] }
        function setValue(key, value) {
            bridge["set" + key.charAt(0).toUpperCase() + key.slice(1)](value)
        }
    }

    Preferences.LibraryPage {
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
        settings: settings
        windowRoot: harness
        promptAddLibraryRoot: function(context) {}
        openLibraryRootNameDialog: function(mode, path, name) {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr);
    QQuickItem *harness = window->contentItem();
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("bridge"));
    QVERIFY(bridge != nullptr);

    QQuickItem *checkBox = findQuickItemByObjectName(harness, QStringLiteral("libraryWatchCheckBox"));
    QVERIFY(checkBox != nullptr);
    QVERIFY(checkBox->property("checked").toBool());
    bridge->setProperty("libraryWatchEnabled", false);
    QTRY_VERIFY(!checkBox->property("checked").toBool());
    bridge->setProperty("libraryWatchEnabled", true);
    QTRY_VERIFY(checkBox->property("checked").toBool());
    checkBox->setProperty("checked", false);
    QVERIFY(QMetaObject::invokeMethod(checkBox, "toggled"));
    QTRY_COMPARE(bridge->property("watchToggles").toInt(), 1);
    QVERIFY(!bridge->property("libraryWatchEnabled").toBool());

    // Only the root that is not watched says so, and why.
    QList<QQuickItem *> labels;
    std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
        if (item->objectName() == QStringLiteral("libraryRootUnwatchedLabel")) {
            labels.append(item);
        }
        for (QQuickItem *child : item->childItems()) {
            collect(child);
        }
    };
    QTRY_VERIFY((labels.clear(), collect(harness), labels.size() == 2));
    QStringList shown;
    for (QQuickItem *label : labels) {
        if (label->isVisible()) {
            shown.append(label->property("text").toString());
        }
    }
    QCOMPARE(shown, QStringList{QStringLiteral("Not watched for changes: it is on nfs4")});
}

void PreferencesSmokeTest::albumArtTileKeepsHeightInsideColumnLayout() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "components" as Components

Item {
    width: 360
    height: 700

    QtObject {
        id: bridge
        property string currentTrackCoverPath: ""
    }

    Action { id: replaceFromItunesAction }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Components.AlbumArtTile {
            id: albumArtTile
            objectName: "albumArtTile"
            uiBridge: bridge
            replaceFromItunesAction: replaceFromItunesAction
            currentTrackItunesArtworkDisabledReason: function() { return "" }
            openAlbumArtViewer: function() {}
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *tile = root->findChild<QObject *>(QStringLiteral("albumArtTile"));
    QVERIFY(tile != nullptr);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QVERIFY2(tile->property("height").toReal() > 0.0, "AlbumArtTile collapsed to zero height");
}

void PreferencesSmokeTest::albumArtViewerInfoUsesImageProviderLocalPath() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import "controllers" as Controllers

Item {
    id: harness
    property string requestedPath: ""
    property int requestCount: 0
    property bool focusCalled: false

    function openAndToggleInfo() {
        viewerController.openAlbumArtViewer()
        viewerController.toggleAlbumArtInfoVisible(function() {
            harness.focusCalled = true
        })
    }

    QtObject {
        id: bridge
        property string currentTrackCoverPath: "image://covers//tmp/Front%20Cover.png#w=600&r=4"
        property bool embedded: false
        signal imageFileDetailsChanged(string path)
        signal trackIdentityChanged()

        function requestImageFileDetails(path) {
            harness.requestedPath = path
            harness.requestCount += 1
        }

        function cachedImageFileDetails(path) {
            if (path !== "/tmp/Front Cover.png") {
                return ({})
            }
            return ({
                fileName: "Front Cover.png",
                resolutionText: "300 x 200",
                fileSizeText: "12 KiB",
                embeddedCover: bridge.embedded,
                path: path
            })
        }
    }

    Controllers.ViewerController {
        id: viewerController
        objectName: "viewerController"
        uiBridge: bridge
        useWholeScreenViewerMode: false
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));

    QObject *controller = root->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(controller != nullptr);

    QVERIFY(QMetaObject::invokeMethod(root.data(), "openAndToggleInfo"));

    QCOMPARE(
        controller->property("albumArtViewerInfoSource").toString(),
        QStringLiteral("/tmp/Front Cover.png"));
    QCOMPARE(root->property("requestedPath").toString(), QStringLiteral("/tmp/Front Cover.png"));
    QCOMPARE(root->property("requestCount").toInt(), 1);
    QCOMPARE(controller->property("albumArtInfoVisible").toBool(), true);
    QCOMPARE(root->property("focusCalled").toBool(), true);

    // The details the info column shows, and where the image is from.
    const QVariantMap info = controller->property("albumArtViewerFileInfo").toMap();
    QCOMPARE(info.value(QStringLiteral("fileName")).toString(), QStringLiteral("Front Cover.png"));
    QCOMPARE(info.value(QStringLiteral("path")).toString(), QStringLiteral("/tmp/Front Cover.png"));
    QCOMPARE(controller->property("albumArtSourceKind").toString(), QStringLiteral("folder"));

    // Art the track's file embeds is cached by the backend: no folder of its own.
    QObject *bridge = nullptr;
    for (QObject *child : root->children()) {
        if (child->property("embedded").isValid()) {
            bridge = child;
        }
    }
    QVERIFY(bridge != nullptr);
    bridge->setProperty("embedded", true);
    QVERIFY(QMetaObject::invokeMethod(controller, "refreshAlbumArtFileInfo"));
    QCOMPARE(controller->property("albumArtSourceKind").toString(), QStringLiteral("embedded"));
}

void PreferencesSmokeTest::diagnosticsLogUsesLowercaseAppDir() {
    const QString logPath = DiagnosticsLog::defaultLogPath();
    QVERIFY(!logPath.isEmpty());

    const QString genericDataDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (genericDataDir.isEmpty()) {
        QVERIFY(logPath.endsWith(QStringLiteral("/diagnostics.log"))
            || logPath.endsWith(QStringLiteral("\\diagnostics.log")));
        return;
    }

    const QFileInfo info(logPath);
    QCOMPARE(info.fileName(), QStringLiteral("diagnostics.log"));
    QCOMPARE(info.dir().dirName(), QStringLiteral("ferrous"));
    QVERIFY(!logPath.contains(QStringLiteral("/Ferrous/")));
    QVERIFY(!logPath.contains(QStringLiteral("\\Ferrous\\")));
}

void PreferencesSmokeTest::replacingAlbumArtAsksFirstWhenTheTrackHasArt() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "dialogs" as Dialogs
ApplicationWindow {
    id: window
    width: 1000
    height: 760
    visible: true
    property var applied: []
    property alias dialog: dialog
    Components.UiPalette { id: palette; windowRoot: window }
    QtObject {
        id: bridge
        property string currentTrackPath: "/fixture/Album A/01 Song.flac"
        property string currentTrackCoverPath: ""
        property var itunesArtworkTarget: ({})
        property var itunesArtworkResults: [{ normalizedPath: "/fixture/cache/suggestion.jpg" }]
        property bool itunesArtworkLoading: false
        property string itunesArtworkStatusText: ""
        signal itunesArtworkChanged()
        signal imageFileDetailsChanged(string path)
        signal trackIdentityChanged()
        signal snapshotChanged()
        function itunesArtworkResultAt(index) { return itunesArtworkResults[index] || ({}) }
        function applyItunesArtworkSuggestion(index) { window.applied = window.applied.concat([index]) }
        function prepareItunesArtworkSuggestion(index) {}
        function requestImageFileDetails(path) {}
        function cachedImageFileDetails(path) { return ({}) }
        function clearItunesArtworkSuggestions() {}
    }
    Dialogs.ItunesArtworkDialog {
        id: dialog
        uiBridge: bridge
        uiPalette: palette
        windowRoot: window
        openAlbumArtViewerForSuggestion: function(row) {}
        openAlbumArtViewerForCurrentArt: function() {}
        endAlbumArtPreview: function(applied) {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *dialog = qvariant_cast<QObject *>(root->property("dialog"));
    QObject *confirm = root->findChild<QObject *>(QStringLiteral("replaceCoverConfirmDialog"));
    QVERIFY(dialog != nullptr && confirm != nullptr);
    // Replace from iTunes has a window of its own, which its question opens
    // in.
    auto *dialogWindow = qobject_cast<QQuickWindow *>(dialog);
    QVERIFY(dialogWindow != nullptr);
    const auto applied = [&root]() { return root->property("applied").toList(); };
    QObject *bridge = dialog->property("uiBridge").value<QObject *>();
    QVERIFY(bridge != nullptr);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cover = dir.filePath(QStringLiteral("cover.png"));
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::darkGreen);
    QVERIFY(image.save(cover));
    const QString coverUrl = QUrl::fromLocalFile(cover).toString();
    bridge->setProperty("currentTrackCoverPath", coverUrl);
    // The suggestions were searched for album A, which has art.
    bridge->setProperty("itunesArtworkTarget", QVariantMap{
        {QStringLiteral("trackPath"), QStringLiteral("/fixture/Album A/01 Song.flac")},
        {QStringLiteral("album"), QStringLiteral("Album A")},
        {QStringLiteral("artist"), QStringLiteral("Artist A")},
        {QStringLiteral("coverPath"), coverUrl},
    });
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(dialogWindow));

    // The album has art: Apply asks, naming it, and Cancel keeps the art.
    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionApply", Q_ARG(QVariant, 0)));
    QTRY_VERIFY(confirm->property("opened").toBool());
    QCOMPARE(confirm->property("message").toString(),
             QStringLiteral("Replace the album art of “Album A” by Artist A?"));
    QVERIFY(applied().isEmpty());
    QVERIFY2(confirm->property("details").toString().contains(QStringLiteral("keeps no copy")),
             qPrintable(confirm->property("details").toString()));
    // A window of its own above Replace from iTunes' (#373 H5), with
    // Replace's icon beside Cancel's.
    auto *confirmWindow = qobject_cast<QQuickWindow *>(confirm);
    QVERIFY(confirmWindow != nullptr);
    QCOMPARE(confirmWindow->transientParent(), dialogWindow);
    QVERIFY(QTest::qWaitForWindowExposed(confirmWindow));
    QCOMPARE(confirm->property("confirmIcon").toString(),
             QStringLiteral("document-replace"));
    QVERIFY(clickVisibleButton(confirmWindow, QStringLiteral("confirmDialogCancelButton")));
    QTRY_VERIFY(!confirm->property("visible").toBool());
    QVERIFY(applied().isEmpty());
    QVERIFY(dialog->property("visible").toBool());

    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionApply", Q_ARG(QVariant, 0)));
    QTRY_VERIFY(confirm->property("opened").toBool());
    // Playback moves on to a track without art: the question stands, for
    // album A, and the dialog still shows A's art.
    bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/Album B/01 Song.flac"));
    bridge->setProperty("currentTrackCoverPath", QString());
    QVERIFY(QMetaObject::invokeMethod(bridge, "snapshotChanged"));
    QVERIFY(confirm->property("visible").toBool());
    QCOMPARE(dialog->property("currentArtworkSource").toString(), coverUrl);
    // A quick second click on Replace, as the question fades out, applies
    // nothing more.
    QTRY_VERIFY(popupsSettled());
    QVERIFY(QTest::qWaitForWindowExposed(confirmWindow));
    QVERIFY(clickTwiceAsItCloses(dialogWindow, QStringLiteral("confirmDialogConfirmButton"), confirm));
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(applied(), (QVariantList{0}));
    QVERIFY(!dialog->property("visible").toBool());

    // Without art there is nothing to lose, so it applies at once.
    bridge->setProperty("itunesArtworkTarget", QVariantMap{
        {QStringLiteral("trackPath"), QStringLiteral("/fixture/Album B/01 Song.flac")},
        {QStringLiteral("album"), QStringLiteral("Album B")},
        {QStringLiteral("coverPath"), QString()},
    });
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(dialogWindow));
    // Nor does a quick second click on Apply: the window hides at once.
    QQuickItem *apply = visibleItemNamed(dialogWindow, QStringLiteral("itunesArtworkApplyButton"));
    QVERIFY(apply != nullptr);
    const QPoint applyAt = centerInWindow(apply);
    QTest::mouseClick(dialogWindow, Qt::LeftButton, Qt::NoModifier, applyAt);
    QVERIFY(!dialog->property("visible").toBool());
    QTest::mouseClick(dialogWindow, Qt::LeftButton, Qt::NoModifier, applyAt);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(applied(), (QVariantList{0, 0}));
    QVERIFY(!confirm->property("visible").toBool());
    QVERIFY(!dialog->property("visible").toBool());

    // Apply of a suggestion still downloading waits for it; a download that
    // finishes as the closed dialog fades out applies nothing.
    bridge->setProperty("itunesArtworkResults",
                        QVariantList{QVariantMap{{QStringLiteral("normalizedPath"), QString()}}});
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(dialogWindow));
    QVERIFY(clickVisibleButton(dialogWindow, QStringLiteral("itunesArtworkApplyButton")));
    QCOMPARE(dialog->property("pendingApplyIndex").toInt(), 0);
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    QVERIFY(!dialog->property("visible").toBool());
    bridge->setProperty("itunesArtworkResults", QVariantList{QVariantMap{
        {QStringLiteral("normalizedPath"), QStringLiteral("/fixture/cache/suggestion.jpg")}}});
    QVERIFY(QMetaObject::invokeMethod(bridge, "itunesArtworkChanged"));
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(applied(), (QVariantList{0, 0}));
    QVERIFY(!confirm->property("visible").toBool());
}

void PreferencesSmokeTest::clearingDiagnosticsAsksFirst() {
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createDiagnosticsHarness(engine, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *dialog = qvariant_cast<QObject *>(root->property("dialog"));
    QObject *confirm = root->findChild<QObject *>(QStringLiteral("clearDiagnosticsConfirmDialog"));
    QVERIFY(dialog != nullptr && confirm != nullptr);
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    // Diagnostics and its question are windows of their own (#373 H5),
    // the question above Diagnostics.
    auto *logWindow = qobject_cast<QQuickWindow *>(dialog);
    auto *confirmWindow = qobject_cast<QQuickWindow *>(confirm);
    QVERIFY(logWindow != nullptr && confirmWindow != nullptr);
    QCOMPARE(logWindow->transientParent(), window);
    QCOMPARE(confirmWindow->transientParent(), logWindow);
    QVERIFY(QTest::qWaitForWindowExposed(logWindow));

    QVERIFY(clickVisibleButton(logWindow, QStringLiteral("diagnosticsClearButton")));
    QTRY_VERIFY(confirm->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(confirmWindow));
    QCOMPARE(root->property("clears").toInt(), 0);
    QVERIFY(clickVisibleButton(confirmWindow, QStringLiteral("confirmDialogCancelButton")));
    QTRY_VERIFY(!confirm->property("visible").toBool());
    QCOMPARE(root->property("clears").toInt(), 0);
    QCOMPARE(dialog->property("diagnosticsText").toString(), QStringLiteral("a line worth keeping"));

    QVERIFY(clickVisibleButton(logWindow, QStringLiteral("diagnosticsClearButton")));
    QTRY_VERIFY(confirm->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(confirmWindow));
    // A quick second click, as the question goes, clears nothing more.
    QVERIFY(clickTwiceAsItCloses(window, QStringLiteral("confirmDialogConfirmButton"), confirm));
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(root->property("clears").toInt(), 1);
    QCOMPARE(dialog->property("diagnosticsText").toString(), QString());
}

// Enter answers the shared confirmation as soon as it shows, before the
// keyboard has moved into it, and only once; Escape and Tab still work
// (#562).
void PreferencesSmokeTest::enterConfirmsAsSoonAsTheConfirmationShows() {
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createDiagnosticsHarness(engine, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *dialog = qvariant_cast<QObject *>(root->property("dialog"));
    QObject *confirm = root->findChild<QObject *>(QStringLiteral("clearDiagnosticsConfirmDialog"));
    QVERIFY(dialog != nullptr && confirm != nullptr);
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    // Diagnostics and its question are windows of their own (#373 H5),
    // the question above Diagnostics.
    auto *logWindow = qobject_cast<QQuickWindow *>(dialog);
    auto *confirmWindow = qobject_cast<QQuickWindow *>(confirm);
    QVERIFY(logWindow != nullptr && confirmWindow != nullptr);
    QCOMPARE(logWindow->transientParent(), window);
    QCOMPARE(confirmWindow->transientParent(), logWindow);
    QVERIFY(QTest::qWaitForWindowExposed(logWindow));

    // Escape, at once, keeps the log.
    QVERIFY(clickVisibleButton(logWindow, QStringLiteral("diagnosticsClearButton")));
    QVERIFY(confirm->property("visible").toBool());
    QVERIFY(keyboardInPopupContent(confirm));
    QTest::keyClick(confirmWindow, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!confirm->property("visible").toBool());
    QCOMPARE(root->property("clears").toInt(), 0);

    // Tab goes on from the content through the dialog to its buttons.
    QVERIFY(clickVisibleButton(logWindow, QStringLiteral("diagnosticsClearButton")));
    QTRY_VERIFY(confirm->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(confirmWindow));
    QVERIFY(tabToPopupFooter(window, confirm));
    QTest::keyClick(confirmWindow, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(root->property("clears").toInt(), 0);

    // Deleting the log destroys it, so Cancel is the default button, as
    // KDE's guidelines ask, and Delete Log is not. Asked again, the
    // question has the keyboard in its content, not on the button Tab left
    // it on, and Enter, as it shows, answers Cancel: the log stays.
    QVERIFY(confirm->property("destructive").toBool());
    QVERIFY(clickVisibleButton(logWindow, QStringLiteral("diagnosticsClearButton")));
    QVERIFY(confirm->property("visible").toBool());
    QQuickItem *footer = qvariant_cast<QQuickItem *>(confirm->property("footer"));
    QQuickItem *deleteButton = visibleItemNamed(confirmWindow, QStringLiteral("confirmDialogConfirmButton"));
    QQuickItem *cancelButton = visibleItemNamed(confirmWindow, QStringLiteral("confirmDialogCancelButton"));
    QVERIFY(footer != nullptr && deleteButton != nullptr && cancelButton != nullptr);
    QCOMPARE(qvariant_cast<QQuickItem *>(footer->property("primary")), cancelButton);
    const auto isDefault = [](QQuickItem *button) {
        return QQmlExpression(qmlContext(button), button, QStringLiteral("Accessible.defaultButton"))
            .evaluate().toBool();
    };
    QVERIFY(isDefault(cancelButton));
    QVERIFY(!isDefault(deleteButton));
    QCOMPARE(deleteButton->property("text").toString(), QStringLiteral("Delete Log"));
    QVERIFY(keyboardInPopupContent(confirm));
    QTest::keyClick(confirmWindow, Qt::Key_Return);
    QVERIFY(dialogClosing(confirm));
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(root->property("clears").toInt(), 0);
    QCOMPARE(dialog->property("diagnosticsText").toString(), QStringLiteral("a line worth keeping"));

    // A question whose action destroys nothing has its confirm button as
    // the default: Enter, as it shows, answers it once. Enter again, as
    // it fades out, answers nothing more.
    QVERIFY(confirm->setProperty("destructive", false));
    QVERIFY(clickVisibleButton(logWindow, QStringLiteral("diagnosticsClearButton")));
    QVERIFY(confirm->property("visible").toBool());
    QVERIFY(isDefault(deleteButton));
    QVERIFY(!isDefault(cancelButton));
    QVERIFY(keyboardInPopupContent(confirm));
    QTest::keyClick(confirmWindow, Qt::Key_Return);
    QCOMPARE(root->property("clears").toInt(), 1);
    QVERIFY(dialogClosing(confirm));
    QTest::keyClick(confirmWindow, Qt::Key_Return);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(root->property("clears").toInt(), 1);
    QVERIFY(!confirm->property("visible").toBool());
    QCOMPARE(dialog->property("diagnosticsText").toString(), QString());
}

std::unique_ptr<QmlSmokeTest> makePreferencesSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<PreferencesSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_preferences.moc"
