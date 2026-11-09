// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// Error reporting, and saving before quitting.
class ErrorsSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void consecutiveErrorsAreBothVisible();
    void clearAllAsTheDialogFadesKeepsAnIssueThatArrives();
    void repeatedErrorsAreCounted();
    void errorHistoryKeepsTheLatestErrors();
    void aLongErrorIsShownWhole();
    void escapeClosesFullscreenViewersWhileTheirControlsHaveFocus();
    void quitTimeErrorClosesAFullscreenViewerFirst();
    void displaySleepFailureShowsInTheFullscreenViewer();
    void recentErrorsWorkByKeyboardAtTheSmallestWindow();
    void viewerIssuesButtonsOpenRecentErrorsAboveTheViewer();
    void recentErrorsFollowAFullscreenViewerThatCloses();
    void stoppedBackendShowsMessageAndDisablesControls();
    void quitActionClosesTheWindowThroughItsCloseHandler();
    void failedSaveBeforeQuitShowsTheErrorAndRetrySaves();
    void quitAnywayClosesAfterAFailedSave();
    void retryTakesNoSecondAnswerAsTheDialogFades();
    void aFailedSaveBeforeQuitIsNotHiddenByRecentErrors();
    void saveAnsweredAfterItsTimeoutStillClosesTheWindow();
    void answerToAnEarlierSaveAttemptDoesNotSettleARetry();
    void escapeAfterASaveTimeoutKeepsTheWindowOpenWhenTheAnswerComesLate();
    void mprisQuitSavesBeforeClosingTheWindow();
    void failedSaveAfterMprisQuitShowsTheDialogOverAMinimizedWindow();
};

namespace {

// Sends an error from the bridge as BridgeClient does. The categories and
// severities are BinaryBridgeCodec's.
bool sendBridgeError(QObject *bridge, const QString &message, int category = 1,
                     int severity = 1, const QString &subject = QString()) {
    return QMetaObject::invokeMethod(bridge, "bridgeError", Q_ARG(QString, message),
                                     Q_ARG(int, category), Q_ARG(int, severity),
                                     Q_ARG(QString, subject));
}

// The content of the popup `popup`, which the error dialog's rows are under.
QQuickItem *popupContent(QObject *popup) {
    return qvariant_cast<QQuickItem *>(popup->property("contentItem"));
}

// Whether `item` shows and lies wholly inside its window.
bool shownInsideItsWindow(QQuickItem *item) {
    const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return item->isVisible() && rect.top() >= 0 && rect.bottom() <= item->window()->height()
        && rect.left() >= 0 && rect.right() <= item->window()->width();
}

} // namespace

void ErrorsSmokeTest::consecutiveErrorsAreBothVisible() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    QVERIFY(bridge != nullptr && errors != nullptr && dialog != nullptr);
    auto *statusButton = window->findChild<QQuickItem *>(QStringLiteral("statusBarErrorsButton"));
    auto *transport = window->findChild<QQuickItem *>(QStringLiteral("transportBar"));
    auto *mainSplit = window->findChild<QQuickItem *>(QStringLiteral("mainSplit"));
    QVERIFY(statusButton != nullptr && transport != nullptr && mainSplit != nullptr);
    QQuickItem *content = popupContent(dialog);
    QVERIFY(content != nullptr);
    QQuickItem *historyList = itemNamed(content, QStringLiteral("errorHistoryList"));
    QVERIFY(historyList != nullptr);

    // With no errors there is no indicator.
    QVERIFY(!statusButton->isVisible());
    const qreal transportBefore = transport->mapToScene(QPointF(0, 0)).y();
    const qreal splitBefore = mainSplit->mapToScene(QPointF(0, 0)).y();

    // The message the Rust bridge sends for a skipped track, then a library error.
    const QString skipped = QStringLiteral(
        "Couldn't play \u201cbad.flac\u201d (Resource not found); skipped it.");
    const QString scan = QStringLiteral("could not read 1 folder or file in '/music'");
    clearCapturedMessages();
    QVERIFY(sendBridgeError(bridge, skipped, 1, 0, QStringLiteral("/music/bad.flac")));
    QVERIFY(sendBridgeError(bridge, scan, 2, 1));

    // The status bar stands out and counts both; nothing else in the window
    // changes, as the main window has no banner.
    QCOMPARE(errorHistoryMessages(errors), (QStringList{scan, skipped}));
    QTRY_COMPARE(statusButton->property("text").toString(), QStringLiteral("2 new issues"));
    QVERIFY(statusButton->isVisible());
    QVERIFY(window->findChild<QObject *>(QStringLiteral("mainErrorBanner")) == nullptr);
    QVERIFY(!dialog->property("visible").toBool());
    QCOMPARE(transport->mapToScene(QPointF(0, 0)).y(), transportBefore);
    QCOMPARE(mainSplit->mapToScene(QPointF(0, 0)).y(), splitBefore);
    // BridgeClient logs them; the window adds nothing to the console.
    QCOMPARE(takeCapturedMessagesText(), QString());

    // A click on the indicator opens the modal dialog, which lists both and
    // counts as seeing them.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(dialog->property("modal").toBool());
    QTRY_COMPARE(historyList->property("count").toInt(), 2);
    QTRY_COMPARE(visibleTextItems(content, skipped).size(), 1);
    QCOMPARE(visibleTextItems(content, scan).size(), 1);
    // A one-sentence message is titled by its category. The file is named
    // in the message, so its path does not repeat; the entry shows it in
    // the library, as it is not in the queue.
    QCOMPARE(visibleTextItems(content, QStringLiteral("Playback")).size(), 1);
    QCOMPARE(visibleTextItems(content, QStringLiteral("Library")).size(), 1);
    QCOMPARE(visibleTextItems(content, QStringLiteral("/music/bad.flac")).size(), 0);
    QCOMPARE(visibleTextItems(content, QStringLiteral("Show in Library")).size(), 1);
    // They stay new while the dialog shows them, on the open button's tint.
    QCOMPARE(statusButton->property("text").toString(), QStringLiteral("2 new issues"));
    QObject *statusBar = window->findChild<QObject *>(QStringLiteral("statusBar"));
    QVERIFY(statusBar != nullptr);
    QVERIFY(statusBar->property("issuesOpen").toBool());
    // The dialog does not push the window's content.
    QCOMPARE(transport->mapToScene(QPointF(0, 0)).y(), transportBefore);

    // A new error joins the list while it is open.
    QVERIFY(sendBridgeError(bridge, QStringLiteral("Last.fm session expired."), 5, 1));
    QTRY_COMPARE(historyList->property("count").toInt(), 3);
    QTRY_COMPARE(statusButton->property("text").toString(), QStringLiteral("3 new issues"));

    // The rows have no dismiss buttons of their own: Clear All empties the
    // list.
    QVERIFY(itemsNamed(content, QStringLiteral("errorHistoryRemoveButton")).isEmpty());
    QVERIFY(dialog->property("visible").toBool());

    // Clear All empties the list, which closes the dialog: the last entry
    // has gone, and so does the indicator.
    QQuickItem *clearAll = itemNamed(content, QStringLiteral("errorHistoryClearButton"));
    QVERIFY(clearAll != nullptr);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(clearAll));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(errorHistoryMessages(errors).isEmpty());
    QTRY_VERIFY(!statusButton->isVisible());
    QVERIFY(!statusBar->property("issuesOpen").toBool());
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::clearAllAsTheDialogFadesKeepsAnIssueThatArrives() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    QVERIFY(bridge != nullptr && errors != nullptr && dialog != nullptr);
    QVERIFY(sendBridgeError(bridge, QStringLiteral("Couldn't play it."), 1, 1));
    QVERIFY(QMetaObject::invokeMethod(errors, "historyRequested"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QQuickItem *clearAll = itemNamed(popupContent(dialog), QStringLiteral("errorHistoryClearButton"));
    QVERIFY(clearAll != nullptr);

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(clearAll));
    QVERIFY(popupFadingOut(dialog));
    QVERIFY(errorHistoryMessages(errors).isEmpty());

    // An issue arrives as the dialog fades out, and a quick second click on
    // Clear All, where it is now, does not clear it unseen.
    const QString late = QStringLiteral("Last.fm session expired.");
    QVERIFY(sendBridgeError(bridge, late, 5, 1));
    QVERIFY(popupFadingOut(dialog));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(clearAll));
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(errorHistoryMessages(errors), (QStringList{late}));
}

void ErrorsSmokeTest::repeatedErrorsAreCounted() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    QVERIFY(bridge != nullptr && errors != nullptr && dialog != nullptr);
    const QString message = QStringLiteral("Couldn't play \u201cbad.flac\u201d; skipped it.");
    const QString subject = QStringLiteral("/music/bad.flac");

    QVERIFY(sendBridgeError(bridge, message, 1, 0, subject));
    QVERIFY(sendBridgeError(bridge, QStringLiteral("other"), 2, 1));
    QVERIFY(sendBridgeError(bridge, message, 1, 0, subject));
    QVERIFY(sendBridgeError(bridge, message, 1, 0, subject));

    // One entry, counted, and moved to the front as the newest.
    QCOMPARE(errorHistoryMessages(errors), (QStringList{message, QStringLiteral("other")}));
    auto *entries = qvariant_cast<QAbstractItemModel *>(errors->property("entries"));
    QVERIFY(entries != nullptr);
    QCOMPARE(entries->index(0, 0).data(entries->roleNames().key("count")).toInt(), 3);
    QVERIFY(QMetaObject::invokeMethod(errors, "historyRequested"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_COMPARE(visibleTextItems(popupContent(dialog), QStringLiteral("Playback (\u00d73)")).size(), 1);

    // The same message about another file, or of another category, is another error.
    QVERIFY(sendBridgeError(bridge, message, 1, 0, QStringLiteral("/music/other.flac")));
    QVERIFY(sendBridgeError(bridge, message, 2, 0, subject));
    QCOMPARE(errorHistoryMessages(errors).size(), 4);
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::errorHistoryKeepsTheLatestErrors() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(bridge != nullptr && errors != nullptr);
    const int limit = errors->property("historyLimit").toInt();
    QCOMPARE(limit, 20);

    for (int index = 0; index < limit + 5; ++index) {
        QVERIFY(sendBridgeError(bridge, QStringLiteral("error %1").arg(index)));
    }

    QStringList expected;
    for (int index = limit + 4; index >= 5; --index) {
        expected.push_back(QStringLiteral("error %1").arg(index));
    }
    QCOMPARE(errorHistoryMessages(errors), expected);
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::aLongErrorIsShownWhole() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr && popup != nullptr);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
    QString message = QStringLiteral(
        "could not cache the embedded covers of 3 files in '/music'; kept their previous covers "
        "and will retry at the next scan (first: '/music/");
    for (int folder = 0; folder < 12; ++folder) {
        message += QStringLiteral("Some Artist %1 - Some Album With A Long Name/").arg(folder);
    }
    message += QStringLiteral("01 Track.flac': No space left on device (os error 28))");
    QVERIFY(message.size() > 600);

    QVERIFY(sendBridgeError(bridge, message, 2, 0));

    // The viewer has no banner: its button to Recent Issues shows, and the
    // history shows the message whole.
    QTRY_VERIFY(popup->property("opened").toBool());
    QTRY_VERIFY(visibleItemNamed(window, QStringLiteral("viewerIssuesButton")) != nullptr);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(errors != nullptr);
    QCOMPARE(errorHistoryMessages(errors), QStringList{message});
    QVERIFY(QMetaObject::invokeMethod(errors, "historyRequested"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_COMPARE(visibleTextItems(popupContent(dialog), message).size(), 1);
    QQuickItem *row = visibleTextItems(popupContent(dialog), message).first();
    QVERIFY(!row->property("truncated").toBool());
    QTRY_VERIFY(row->property("lineCount").toInt() > 1);
    QVERIFY(row->property("contentWidth").toReal() <= row->width() + 1);
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::displaySleepFailureShowsInTheFullscreenViewer() {
    // Stands in for a desktop without org.freedesktop.ScreenSaver.
    class FailingBackend final : public DisplaySleepInhibitorBackend {
    public:
        void setInhibited(bool inhibited) override {
            if (inhibited) {
                reportFailure(QStringLiteral("no idle service"));
            }
        }
    };
    DisplaySleepInhibitor inhibitor(std::make_unique<FailingBackend>());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("displaySleepInhibitor"), &inhibitor);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(bridge != nullptr && viewer != nullptr && surface != nullptr && errors != nullptr);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(bridge->property("preventDisplaySleepInFullscreen").toBool());

    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(surface->window() != nullptr && surface->window() != window);
    QQuickWindow *fullscreen = surface->window();
    QTRY_VERIFY(inhibitor.inhibited());

    // Shown where the user is looking: in the viewer's title strip, as a
    // button to Recent Issues, with no banner over the view.
    QTRY_VERIFY(visibleItemNamed(fullscreen, QStringLiteral("viewerIssuesButton")) != nullptr);
    QQuickItem *issuesButton = visibleItemNamed(fullscreen, QStringLiteral("viewerIssuesButton"));
    QCOMPARE(issuesButton->property("text").toString(), QStringLiteral("1 new issue"));
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenStrip"));
    QQuickItem *close = visibleItemNamed(fullscreen, QStringLiteral("spectrogramFullscreenCloseButton"));
    QVERIFY(strip != nullptr && close != nullptr);
    QVERIFY(strip->isAncestorOf(issuesButton));
    // Laid out beside Close.
    QTRY_VERIFY(clearOfTheOtherButtons(issuesButton));
    QVERIFY(sceneRectOf(issuesButton).right() <= sceneRectOf(close).left());
    const QStringList messages = errorHistoryMessages(errors);
    QCOMPARE(messages.size(), 1);
    QVERIFY2(messages.first().contains(QStringLiteral("Couldn't keep the screen awake")),
             qPrintable(messages.first()));
    QVERIFY(messages.first().contains(QStringLiteral("no idle service")));

    // A click on it opens Recent Issues over the viewer, which stays open.
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    QVERIFY(dialog != nullptr);
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(issuesButton));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(popupContent(dialog)->window(), fullscreen);
    QVERIFY(viewer->property("spectrogramViewerOpen").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    // Seen, it goes.
    QTRY_VERIFY(visibleItemNamed(fullscreen, QStringLiteral("viewerIssuesButton")) == nullptr);

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!inhibitor.inhibited());
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::stoppedBackendShowsMessageAndDisablesControls() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    QVERIFY2(!engine.rootObjects().isEmpty(), "Main.qml failed to instantiate");
    QObject *root = engine.rootObjects().constFirst();
    QObject *bridge = qvariant_cast<QObject *>(root->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    QObject *message = root->findChild<QObject *>(QStringLiteral("backendStoppedMessage"));
    QObject *statusLabel = root->findChild<QObject *>(QStringLiteral("statusBarBackendStoppedLabel"));
    QObject *transportBar = root->findChild<QObject *>(QStringLiteral("transportBar"));
    QObject *mainSplit = root->findChild<QObject *>(QStringLiteral("mainSplit"));
    QVERIFY(message != nullptr);
    QVERIFY(statusLabel != nullptr);
    QVERIFY(transportBar != nullptr);
    QVERIFY(mainSplit != nullptr);
    QVERIFY(!message->property("visible").toBool());
    QVERIFY(!statusLabel->property("visible").toBool());
    QVERIFY(transportBar->property("enabled").toBool());
    QVERIFY(mainSplit->property("enabled").toBool());

    // What the bridge client reports when the relay finds the backend gone.
    const QString reason = QStringLiteral(
        "The Ferrous backend stopped unexpectedly. Restart Ferrous to continue.");
    QVERIFY(bridge->setProperty("backendFailure", reason));

    QTRY_VERIFY(message->property("visible").toBool());
    QCOMPARE(message->property("text").toString(), reason);
    QVERIFY(statusLabel->property("visible").toBool());
    QVERIFY(!transportBar->property("enabled").toBool());
    QVERIFY(!mainSplit->property("enabled").toBool());
}

namespace {

// Answers the fallback bridge's save request with this ID.
bool answerStateSave(QObject *bridge, int requestId, bool saved, const QString &message = {}) {
    return QMetaObject::invokeMethod(bridge, "stateSaveFinished", Q_ARG(int, requestId),
        Q_ARG(bool, saved), Q_ARG(QString, message));
}

// Runs the save timeout's handler now, in place of the running timer.
bool timeOutStateSave(QObject *window) {
    QObject *timeout = window->findChild<QObject *>(QStringLiteral("quitStateSaveTimeout"));
    return timeout != nullptr && timeout->property("running").toBool()
        && timeout->setProperty("running", false)
        && QMetaObject::invokeMethod(timeout, "triggered");
}

const QString kStateSaveTimeoutMessage = QStringLiteral(
    "The backend did not answer in time; settings and the queue may not have been saved.");

} // namespace

void ErrorsSmokeTest::quitActionClosesTheWindowThroughItsCloseHandler() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *quitAction = window->findChild<QObject *>(QStringLiteral("quitAction"));
    QVERIFY(quitAction != nullptr);
    QCOMPARE(quitAction->property("shortcut").toInt(), static_cast<int>(QKeySequence::Quit));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QVERIFY(dialog != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    // By its signature: the member pointer would import a symbol of Qt's
    // private API (its argument is private), which no binary may.
    QSignalSpy closing(window, SIGNAL(closing(QQuickCloseEvent*)));
    QVERIFY(closing.isValid());
    QSignalSpy quit(&engine, &QQmlEngine::quit);

    // Ctrl+Q triggers this action. The offscreen platform binds no key to
    // the standard Quit sequence, so the test triggers the action itself.
    QVERIFY(QMetaObject::invokeMethod(quitAction, "trigger"));

    // `closing` runs onClosing, as closing the window from its title bar
    // does. It keeps the window open until the backend has saved the
    // settings and the queue, and then closes it again.
    QTRY_VERIFY(!window->isVisible());
    QCOMPARE(closing.count(), 2);
    QVERIFY(!dialog->property("visible").toBool());
    QCOMPARE(quit.count(), 0);
}

void ErrorsSmokeTest::failedSaveBeforeQuitShowsTheErrorAndRetrySaves() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QObject *message = window->findChild<QObject *>(QStringLiteral("quitSaveFailedMessage"));
    QVERIFY(dialog != nullptr);
    QVERIFY(message != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const QString error = QStringLiteral(
        "Settings could not be saved: failed to write /config/ferrous/settings.txt: "
        "No space left on device (os error 28)");
    QVERIFY(bridge->setProperty("stateSaveError", error));

    window->close();

    QTRY_VERIFY(dialog->property("visible").toBool());
    QCOMPARE(message->property("text").toString(), error);
    QVERIFY(window->isVisible());

    // The disk has room again.
    QVERIFY(bridge->setProperty("stateSaveError", QString()));
    QVERIFY(clickButton(window, QStringLiteral("quitSaveFailedRetryButton")));

    QTRY_VERIFY(!window->isVisible());
    // Visible until the style's exit transition ends.
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

void ErrorsSmokeTest::aFailedSaveBeforeQuitIsNotHiddenByRecentErrors() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QObject *recent = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    QObject *failed = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QVERIFY(bridge != nullptr && errors != nullptr && recent != nullptr && failed != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(sendBridgeError(bridge, QStringLiteral("Couldn't play it."), 1, 1));
    QVERIFY(QMetaObject::invokeMethod(errors, "historyRequested"));
    QTRY_VERIFY(recent->property("opened").toBool());
    QVERIFY(bridge->setProperty("stateSaveError", QStringLiteral("No space left on device")));

    // Quitting from outside the window, past the modal dialog.
    window->close();

    QTRY_VERIFY(failed->property("visible").toBool());
    QTRY_VERIFY(!recent->property("visible").toBool());
    QVERIFY(clickButton(window, QStringLiteral("quitSaveFailedQuitButton")));
    QTRY_VERIFY(!window->isVisible());
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::quitAnywayClosesAfterAFailedSave() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QVERIFY(dialog != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(bridge->setProperty("stateSaveError", QStringLiteral("Settings could not be saved")));

    window->close();
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(clickButton(window, QStringLiteral("quitSaveFailedQuitButton")));

    QTRY_VERIFY(!window->isVisible());
}

void ErrorsSmokeTest::retryTakesNoSecondAnswerAsTheDialogFades() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QVERIFY(dialog != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(bridge->setProperty("holdStateSaveAnswers", true));

    window->close();
    const int first = bridge->property("lastStateSaveRequestId").toInt();
    QVERIFY(answerStateSave(bridge, first, false, QStringLiteral("Settings could not be saved")));
    QTRY_VERIFY(dialog->property("opened").toBool());

    // A quick second click on Retry, as the dialog fades out, saves no
    // second time.
    QVERIFY(clickTwiceAsItCloses(window, QStringLiteral("quitSaveFailedRetryButton"), dialog));
    const int retry = bridge->property("lastStateSaveRequestId").toInt();
    QCOMPARE(retry, first + 1);
    // Nor does Escape, as it still fades out, give up on the retry.
    QVERIFY(popupFadingOut(dialog));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(bridge->property("lastStateSaveRequestId").toInt(), retry);

    // The retry's answer still closes the window.
    QVERIFY(answerStateSave(bridge, retry, true));
    QTRY_VERIFY(!window->isVisible());
}

void ErrorsSmokeTest::saveAnsweredAfterItsTimeoutStillClosesTheWindow() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QObject *message = window->findChild<QObject *>(QStringLiteral("quitSaveFailedMessage"));
    QVERIFY(dialog != nullptr);
    QVERIFY(message != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(bridge->setProperty("holdStateSaveAnswers", true));

    window->close();
    const int requestId = bridge->property("lastStateSaveRequestId").toInt();
    QCOMPARE(requestId, 1);
    QVERIFY(timeOutStateSave(window));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QCOMPARE(message->property("text").toString(), kStateSaveTimeoutMessage);
    QVERIFY(window->isVisible());

    // Slow storage finishes the save after all.
    QVERIFY(answerStateSave(bridge, requestId, true));

    QTRY_VERIFY(!window->isVisible());
    QTRY_VERIFY(!dialog->property("visible").toBool());
}

void ErrorsSmokeTest::answerToAnEarlierSaveAttemptDoesNotSettleARetry() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QObject *message = window->findChild<QObject *>(QStringLiteral("quitSaveFailedMessage"));
    QVERIFY(dialog != nullptr);
    QVERIFY(message != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(bridge->setProperty("holdStateSaveAnswers", true));

    window->close();
    const int first = bridge->property("lastStateSaveRequestId").toInt();
    QVERIFY(timeOutStateSave(window));
    QTRY_VERIFY(dialog->property("visible").toBool());
    // Retry before the first attempt has answered.
    QVERIFY(clickButton(window, QStringLiteral("quitSaveFailedRetryButton")));
    const int retry = bridge->property("lastStateSaveRequestId").toInt();
    QVERIFY(retry != first);
    QTRY_VERIFY(!dialog->property("visible").toBool());

    // Neither answer to the first attempt settles the retry.
    QVERIFY(answerStateSave(bridge, first, false, QStringLiteral("Settings could not be saved")));
    QVERIFY(answerStateSave(bridge, first, true));
    QCoreApplication::processEvents();
    QVERIFY(window->isVisible());
    QVERIFY(!dialog->property("visible").toBool());

    // The retry's own answer does: its failure is shown, not the first's.
    const QString error = QStringLiteral("Settings could not be saved: disk full");
    QVERIFY(answerStateSave(bridge, retry, false, error));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QCOMPARE(message->property("text").toString(), error);

    // A success after another retry closes the window.
    QVERIFY(clickButton(window, QStringLiteral("quitSaveFailedRetryButton")));
    const int last = bridge->property("lastStateSaveRequestId").toInt();
    QVERIFY(answerStateSave(bridge, last, true));
    QTRY_VERIFY(!window->isVisible());
}

void ErrorsSmokeTest::escapeAfterASaveTimeoutKeepsTheWindowOpenWhenTheAnswerComesLate() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QVERIFY(dialog != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(bridge->setProperty("holdStateSaveAnswers", true));

    window->close();
    const int requestId = bridge->property("lastStateSaveRequestId").toInt();
    QVERIFY(timeOutStateSave(window));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());

    QVERIFY(answerStateSave(bridge, requestId, true));
    QCoreApplication::processEvents();
    QVERIFY(window->isVisible());

    // Closing the window again saves again.
    window->close();
    QCOMPARE(bridge->property("lastStateSaveRequestId").toInt(), requestId + 1);
    QVERIFY(answerStateSave(bridge, requestId + 1, true));
    QTRY_VERIFY(!window->isVisible());
}

void ErrorsSmokeTest::mprisQuitSavesBeforeClosingTheWindow() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto restoreMainWindow = useAsMainWindow(window);
    // By its signature: the member pointer would import a symbol of Qt's
    // private API (its argument is private), which no binary may.
    QSignalSpy closing(window, SIGNAL(closing(QQuickCloseEvent*)));
    QVERIFY(closing.isValid());
    QSignalSpy quit(&engine, &QQmlEngine::quit);

    callMprisQuit();

    // MPRIS Quit closes the window as its close button does: the close
    // handler keeps it open while the backend saves, and closes it after.
    QCOMPARE(bridge->property("lastStateSaveRequestId").toInt(), 1);
    QVERIFY(window->isVisible());
    QTRY_VERIFY(!window->isVisible());
    QCOMPARE(closing.count(), 2);
    QCOMPARE(quit.count(), 0);
}

void ErrorsSmokeTest::failedSaveAfterMprisQuitShowsTheDialogOverAMinimizedWindow() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QVERIFY(bridge != nullptr);
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QObject *message = window->findChild<QObject *>(QStringLiteral("quitSaveFailedMessage"));
    QVERIFY(dialog != nullptr);
    QVERIFY(message != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto restoreMainWindow = useAsMainWindow(window);
    const QString error = QStringLiteral("Settings could not be saved");
    QVERIFY(bridge->setProperty("stateSaveError", error));
    window->showMinimized();
    QTRY_COMPARE(window->visibility(), QWindow::Minimized);

    callMprisQuit();

    // The dialog is drawn in the window, so the window comes forward with it.
    QTRY_VERIFY(dialog->property("visible").toBool());
    QCOMPARE(message->property("text").toString(), error);
    QVERIFY(window->isVisible());
    QTRY_VERIFY(window->visibility() != QWindow::Minimized);

    QVERIFY(clickButton(window, QStringLiteral("quitSaveFailedQuitButton")));
    QTRY_VERIFY(!window->isVisible());
}

void ErrorsSmokeTest::quitTimeErrorClosesAFullscreenViewerFirst() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("mainSpectrogramSurface"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && surface != nullptr && dialog != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto restoreMainWindow = useAsMainWindow(window);
    QVERIFY(bridge->setProperty("stateSaveError", QStringLiteral("Settings could not be saved")));

    // The visualization viewer over the whole screen, a modal window of its own.
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(surface->window() != nullptr && surface->window() != window);
    QQuickWindow *fullscreen = surface->window();
    QVERIFY(QTest::qWaitForWindowExposed(fullscreen));

    callMprisQuit();

    // The dialog opens in the main window, so the viewer closes first.
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    QTRY_VERIFY(!fullscreen->isVisible());
    QVERIFY(window->isVisible());
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::escapeClosesFullscreenViewersWhileTheirControlsHaveFocus() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    QObject *seekBackward = window->findChild<QObject *>(QStringLiteral("seekBackwardAction"));
    QVERIFY(seekBackward != nullptr);
    QSignalSpy seeks(seekBackward, SIGNAL(triggered(QObject*)));
    // A track that can seek.
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("durationSeconds", 100.0);
    bridge->setProperty("viewerFullscreenMode", 1);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());

    struct Viewer {
        const char *open;
        const char *openProperty;
        QString windowName;
    };
    const QList<Viewer> viewers{
        {"openSpectrogramViewer", "spectrogramViewerOpen",
         QStringLiteral("spectrogramFullscreenWindow")},
        {"openAlbumArtViewerForCurrentArt", "albumArtViewerOpen",
         QStringLiteral("albumArtFullscreenWindow")},
    };
    for (const Viewer &shown : viewers) {
        QVERIFY(QMetaObject::invokeMethod(viewer, shown.open));
        QTRY_VERIFY(viewer->property(shown.openProperty).toBool());
        QQuickWindow *fullscreen = viewerWindowNamed(engine, shown.windowName);
        QVERIFY(fullscreen != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(fullscreen));
        QVERIFY(sendBridgeError(bridge, QStringLiteral("Couldn't play \u201cbad.flac\u201d; skipped it."),
                                1, 0, QStringLiteral("/music/bad.flac")));
        QTRY_VERIFY(visibleItemNamed(fullscreen, QStringLiteral("viewerIssuesButton")) != nullptr);
        QQuickItem *issues = visibleItemNamed(fullscreen, QStringLiteral("viewerIssuesButton"));

        // Tab moves into the strip's buttons.
        for (int press = 0; press < 8 && !issues->hasActiveFocus(); ++press) {
            QTest::keyClick(fullscreen, Qt::Key_Tab);
        }
        QVERIFY2(issues->hasActiveFocus(), qPrintable(shown.windowName));
        QVERIFY(issues->property("visualFocus").toBool());

        // The main window's shortcuts still work, and Escape still closes
        // the viewer.
        const int seeksBefore = seeks.count();
        QTest::keyClick(fullscreen, Qt::Key_Left, Qt::ShiftModifier);
        QTRY_VERIFY(seeks.count() > seeksBefore);
        QVERIFY(viewer->property(shown.openProperty).toBool());
        QTest::keyClick(fullscreen, Qt::Key_Escape);
        QTRY_VERIFY2(!viewer->property(shown.openProperty).toBool(), qPrintable(shown.windowName));
        QTRY_VERIFY(!fullscreen->isVisible());
    }
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::recentErrorsWorkByKeyboardAtTheSmallestWindow() {
    // Popups name themselves to assistive technology only while it is active.
    QAccessible::setActive(true);
    const QScopeGuard stopAccessibility([]() { QAccessible::setActive(false); });
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    window->resize(window->minimumWidth(), window->minimumHeight());
    QTRY_COMPARE(window->height(), window->minimumHeight());
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    auto *statusButton = window->findChild<QQuickItem *>(QStringLiteral("statusBarErrorsButton"));
    QVERIFY(bridge != nullptr && errors != nullptr && dialog != nullptr && playPause != nullptr
            && statusButton != nullptr);
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    QQuickItem *content = popupContent(dialog);
    QVERIFY(content != nullptr);
    QQuickItem *view = itemNamed(content, QStringLiteral("errorHistoryView"));
    QQuickItem *diagnostics = itemNamed(content, QStringLiteral("errorHistoryDiagnosticsButton"));
    QQuickItem *clearAll = itemNamed(content, QStringLiteral("errorHistoryClearButton"));
    QVERIFY(view != nullptr && diagnostics != nullptr && clearAll != nullptr);

    // As many errors as the history keeps, with a message of some length.
    const int limit = errors->property("historyLimit").toInt();
    for (int index = 0; index < limit; ++index) {
        QVERIFY(sendBridgeError(
            bridge,
            QStringLiteral("Couldn't play track %1 of an album with quite a long name").arg(index),
            1, 1, QStringLiteral("/music/track %1.flac").arg(index)));
    }

    QTRY_VERIFY(shownInsideItsWindow(statusButton));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    QTRY_VERIFY(dialog->property("opened").toBool());

    // The popup fits the window; the list scrolls, and has the keyboard.
    QVERIFY(shownInsideItsWindow(content));
    QVERIFY(shownInsideItsWindow(diagnostics));
    QVERIFY(shownInsideItsWindow(clearAll));
    QVERIFY(view->height() >= QFontMetricsF(QGuiApplication::font()).lineSpacing() * 4);
    auto *flickable = qvariant_cast<QQuickItem *>(view->property("contentItem"));
    QVERIFY(flickable != nullptr);
    QTRY_VERIFY(flickable->property("contentHeight").toReal() > view->height());
    QTRY_VERIFY(view->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0.0);
    QTest::keyClick(window, Qt::Key_End);
    QTRY_VERIFY(std::abs(flickable->property("contentY").toReal()
                         - (flickable->property("contentHeight").toReal() - flickable->height()))
                <= 1.0);
    QTest::keyClick(window, Qt::Key_Home);
    QTRY_COMPARE(flickable->property("contentY").toReal(), 0.0);

    // Closing counts as having seen what arrived, so an error that arrives,
    // repeated or not, comes into view; so does the newest on opening.
    QTest::keyClick(window, Qt::Key_End);
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0.0);
    QVERIFY(sendBridgeError(
        bridge, QStringLiteral("Couldn't play track 0 of an album with quite a long name"), 1, 1,
        QStringLiteral("/music/track 0.flac")));
    QTRY_COMPARE(flickable->property("contentY").toReal(), 0.0);
    QTest::keyClick(window, Qt::Key_End);
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0.0);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(flickable->property("contentY").toReal(), 0.0);
    QTRY_VERIFY(view->hasActiveFocus());

    // Everything has a name.
    QAccessibleInterface *popupAccessible = QAccessible::queryAccessibleInterface(content);
    QVERIFY(popupAccessible != nullptr);
    QCOMPARE(popupAccessible->text(QAccessible::Name), QStringLiteral("Recent Issues"));
    QCOMPARE(popupAccessible->role(), QAccessible::Dialog);
    QCOMPARE(QAccessible::queryAccessibleInterface(view)->text(QAccessible::Name),
             QStringLiteral("Recent issues"));

    // Tab goes on to each row's action, scrolling the row into view.
    QList<QQuickItem *> actions = itemsNamed(content, QStringLiteral("errorHistoryActionButton"));
    QCOMPARE(actions.size(), limit);
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(actions.first()->hasActiveFocus());
    QCOMPARE(QAccessible::queryAccessibleInterface(actions.first())->text(QAccessible::Name),
             QStringLiteral("Show in Library"));
    for (int press = 1; press < limit; ++press) {
        QTest::keyClick(window, Qt::Key_Tab);
    }
    QQuickItem *lastAction = actions.last();
    QTRY_VERIFY(lastAction->hasActiveFocus());
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0.0);
    const QRectF lastInView =
        lastAction->mapRectToItem(flickable, QRectF(0, 0, lastAction->width(), lastAction->height()));
    QVERIFY(lastInView.top() >= 0 && lastInView.bottom() <= flickable->height() + 1);

    // On to Open Diagnostics and Clear All; Escape closes the popup from
    // any of them, and the keyboard goes back to the window.
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(diagnostics->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(clearAll->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(errorHistoryMessages(errors).size(), limit);
    QVERIFY(window->activeFocusItem() == nullptr
            || !content->isAncestorOf(window->activeFocusItem()));
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_COMPARE(toggled.count(), 1);

    // Space presses a focused action rather than playing or pausing.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_VERIFY(view->hasActiveFocus());
    actions = itemsNamed(content, QStringLiteral("errorHistoryActionButton"));
    QVERIFY(!actions.isEmpty());
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(actions.first()->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(toggled.count(), 1);
    QCOMPARE(bridge->property("shownInLibrary").toStringList(), QStringList{QStringLiteral("/music/track 0.flac")});
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::viewerIssuesButtonsOpenRecentErrorsAboveTheViewer() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    QVERIFY(bridge != nullptr && viewer != nullptr && errors != nullptr && dialog != nullptr
            && playPause != nullptr);
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    QQuickItem *content = popupContent(dialog);
    QVERIFY(content != nullptr);
    QQuickItem *clearAll = itemNamed(content, QStringLiteral("errorHistoryClearButton"));
    QQuickItem *view = itemNamed(content, QStringLiteral("errorHistoryView"));
    QVERIFY(clearAll != nullptr && view != nullptr);

    struct Viewer {
        const char *open;
        const char *openProperty;
        int fullscreenMode;
        const char *windowName;
        const char *name;
        const char *closeName;
    };
    const QList<Viewer> viewers{
        {"openSpectrogramViewer", "spectrogramViewerOpen", 0, nullptr, "spectrogram popup",
         "spectrogramPopupCloseButton"},
        {"openAlbumArtViewerForCurrentArt", "albumArtViewerOpen", 0, nullptr, "album art popup",
         "albumArtPopupCloseButton"},
        {"openSpectrogramViewer", "spectrogramViewerOpen", 1, "spectrogramFullscreenWindow",
         "spectrogram fullscreen", "spectrogramFullscreenCloseButton"},
        {"openAlbumArtViewerForCurrentArt", "albumArtViewerOpen", 1, "albumArtFullscreenWindow",
         "album art fullscreen", "albumArtFullscreenCloseButton"},
    };
    for (const Viewer &shown : viewers) {
        const QByteArray name(shown.name);
        bridge->setProperty("viewerFullscreenMode", shown.fullscreenMode);
        QVERIFY(QMetaObject::invokeMethod(viewer, shown.open));
        QTRY_VERIFY(viewer->property(shown.openProperty).toBool());
        QQuickWindow *host = window;
        if (shown.windowName != nullptr) {
            host = viewerWindowNamed(engine, QLatin1String(shown.windowName));
            QVERIFY(host != nullptr);
        }
        QVERIFY(QTest::qWaitForWindowActive(host));

        QVERIFY(sendBridgeError(bridge, QStringLiteral("Couldn't play \u201cbad.flac\u201d; skipped it."),
                                1, 0, QStringLiteral("/music/bad.flac")));
        QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("viewerIssuesButton")) != nullptr,
                     name.constData());
        QQuickItem *historyButton = visibleItemNamed(host, QStringLiteral("viewerIssuesButton"));
        QQuickItem *viewerClose = visibleItemNamed(host, QLatin1String(shown.closeName));
        QVERIFY2(viewerClose != nullptr, name.constData());
        QTRY_VERIFY(shownInsideItsWindow(historyButton));
        // Laid out before Close.
        QTRY_VERIFY2(clearOfTheOtherButtons(historyButton), name.constData());
        QVERIFY2(sceneRectOf(historyButton).right() <= sceneRectOf(viewerClose).left(),
                 name.constData());

        // A real click, which only arrives if nothing covers the button.
        QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, centerInWindow(historyButton));
        QTRY_VERIFY2(dialog->property("opened").toBool(), name.constData());
        QVERIFY(viewer->property(shown.openProperty).toBool());
        QTRY_VERIFY2(view->hasActiveFocus(), name.constData());

        // In the window the user is looking at, and on top of it. A press
        // outside closes it alone: the viewer stays open.
        QCOMPARE(content->window(), host);
        QVERIFY(dialog->property("modal").toBool());
        QTRY_VERIFY(shownInsideItsWindow(clearAll));
        QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, QPoint(host->width() / 3, host->height() / 3));
        QTRY_VERIFY2(!dialog->property("visible").toBool(), name.constData());
        QVERIFY(viewer->property(shown.openProperty).toBool());
        // What the dialog showed counts as seen, as for the status bar, so
        // the button has gone.
        QTRY_VERIFY2(!historyButton->isVisible(), name.constData());

        // Space does not reach the main window's shortcuts while the dialog
        // is open, and Escape closes the dialog alone.
        QVERIFY(QMetaObject::invokeMethod(errors, "historyRequested"));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY2(view->hasActiveFocus(), name.constData());
        const int toggles = toggled.count();
        QTest::keyClick(host, Qt::Key_Space);
        QCoreApplication::processEvents();
        QCOMPARE(toggled.count(), toggles);
        QTest::keyClick(host, Qt::Key_Escape);
        QTRY_VERIFY2(!dialog->property("visible").toBool(), name.constData());
        QVERIFY2(viewer->property(shown.openProperty).toBool(), name.constData());

        // The viewer has the keyboard back, and not on the button that
        // went: its Escape closes it.
        QVERIFY2(host->activeFocusItem() != historyButton, name.constData());
        QTest::keyClick(host, Qt::Key_Escape);
        QTRY_VERIFY2(!viewer->property(shown.openProperty).toBool(), name.constData());
        if (host != window) {
            QTRY_VERIFY(!host->isVisible());
        }
        QVERIFY(QMetaObject::invokeMethod(errors, "clear"));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::recentErrorsFollowAFullscreenViewerThatCloses() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("recentErrorsDialog"));
    auto *statusButton = window->findChild<QQuickItem *>(QStringLiteral("statusBarErrorsButton"));
    QVERIFY(bridge != nullptr && viewer != nullptr && errors != nullptr && dialog != nullptr
            && statusButton != nullptr);
    QQuickItem *content = popupContent(dialog);
    QVERIFY(content != nullptr);
    QQuickItem *clearAll = itemNamed(content, QStringLiteral("errorHistoryClearButton"));
    QVERIFY(clearAll != nullptr);
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(sendBridgeError(bridge, QStringLiteral("Couldn't play it."), 1, 1));

    // A viewer can also close from outside, such as for a quit from the
    // desktop's media controls. The dialog then cannot stay in its window,
    // which would leave it out of reach.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("spectrogramFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QVERIFY(QMetaObject::invokeMethod(errors, "historyRequested"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(content->window(), fullscreen);

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!fullscreen->isVisible());
    QTRY_COMPARE(content->window(), window);
    QVERIFY(dialog->property("visible").toBool());
    QVERIFY(QTest::qWaitForWindowActive(window));
    QTRY_VERIFY(shownInsideItsWindow(clearAll));
    // A press outside it, in the main window, closes it.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(window->width() / 3, window->height() / 3));
    QTRY_VERIFY(!dialog->property("visible").toBool());

    // Nothing of it stays in the way of the main window.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(finishIncubation(engine));
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

std::unique_ptr<QmlSmokeTest> makeErrorsSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<ErrorsSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_errors.moc"
