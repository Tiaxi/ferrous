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
    void errorHistoryCountsRepeatsAndKeepsTheLatest();
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
    void retryTakesNoSecondAnswerAsTheDialogFades();
    void enterRetriesAsSoonAsTheSaveFailureShows();
    void aFailedSaveBeforeQuitIsNotHiddenByRecentErrors();
    void saveAnsweredAfterItsTimeoutStillClosesTheWindow();
    void answerToAnEarlierSaveAttemptDoesNotSettleARetry();
    void escapeAfterASaveTimeoutKeepsTheWindowOpenWhenTheAnswerComesLate();
    void mprisQuitSavesBeforeClosingTheWindow();
    void failedSaveAfterMprisQuitShowsTheDialogOverAMinimizedWindow();
};

namespace {

// Sends an error from the bridge as BridgeClient does, with the system's
// text `detail`. The categories and severities are BinaryBridgeCodec's.
bool sendBridgeError(QObject *bridge, const QString &message, int category = 1,
                     int severity = 1, const QString &subject = QString(),
                     const QString &detail = QString()) {
    return QMetaObject::invokeMethod(bridge, "bridgeError", Q_ARG(QString, message),
                                     Q_ARG(int, category), Q_ARG(int, severity),
                                     Q_ARG(QString, subject), Q_ARG(QString, detail));
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

// Main.qml over its fallback bridge, shown, and active if `active`, as the
// keyboard needs, with what the tests here use of it. The context
// properties `context` are set before it loads.
struct ErrorsWindow {
    explicit ErrorsWindow(bool active = false, const QVariantMap &context = {}) {
        for (auto it = context.cbegin(); it != context.cend(); ++it) {
            engine.rootContext()->setContextProperty(it.key(), it.value());
        }
        MainWindowOptions options;
        options.activate = active;
        window = loadMainWindow(engine, libraryModel, options);
        if (window == nullptr) {
            return;
        }
        bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        errors = child("errorController");
        recentErrors = child("recentErrorsDialog");
        viewer = child("viewerController");
        saveFailed = child("quitSaveFailedDialog");
        saveFailedMessage = child("quitSaveFailedMessage");
    }
    bool loaded() const {
        return window != nullptr && bridge != nullptr && errors != nullptr && recentErrors != nullptr
            && viewer != nullptr && saveFailed != nullptr && saveFailedMessage != nullptr;
    }
    QObject *child(const char *name) const { return window->findChild<QObject *>(QString::fromLatin1(name)); }
    QQuickItem *item(const char *name) const { return window->findChild<QQuickItem *>(QString::fromLatin1(name)); }
    // The Recent Issues dialog's content, which its rows are under.
    QQuickItem *recentContent() const { return popupContent(recentErrors); }

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    QObject *bridge = nullptr;
    QObject *errors = nullptr;
    QObject *recentErrors = nullptr;
    QObject *viewer = nullptr;
    QObject *saveFailed = nullptr;
    QObject *saveFailedMessage = nullptr;
};

// What the window wrote to the log, or that it left objects half made, as
// the engine goes: nothing, where all is well.
QString unexpectedOutput(QQmlEngine &engine) {
    if (!finishIncubation(engine)) {
        return QStringLiteral("objects were still being made");
    }
    return takeCapturedMessagesText();
}

} // namespace

void ErrorsSmokeTest::consecutiveErrorsAreBothVisible() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QObject *dialog = ui.recentErrors;
    QQuickItem *statusButton = ui.item("statusBarErrorsButton");
    QQuickItem *transport = ui.item("transportBar");
    QQuickItem *mainSplit = ui.item("mainSplit");
    QObject *statusBar = ui.child("statusBar");
    QVERIFY(statusButton != nullptr && transport != nullptr && mainSplit != nullptr && statusBar != nullptr);
    QQuickItem *content = ui.recentContent();
    QVERIFY(content != nullptr);
    QQuickItem *historyList = itemNamed(content, QStringLiteral("errorHistoryList"));
    QVERIFY(historyList != nullptr);
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };

    // With no errors there is no indicator.
    QVERIFY(!statusButton->isVisible());
    const qreal transportBefore = transport->mapToScene(QPointF(0, 0)).y();
    const qreal splitBefore = mainSplit->mapToScene(QPointF(0, 0)).y();

    // The message the Rust bridge sends for a skipped track, then a library error.
    const QString skipped = QStringLiteral(
        "Couldn't play “bad.flac” (Resource not found); skipped it.");
    const QString scan = QStringLiteral("could not read 1 folder or file in '/music'");
    clearCapturedMessages();
    QVERIFY(sendBridgeError(ui.bridge, skipped, 1, 0, QStringLiteral("/music/bad.flac")));
    QVERIFY(sendBridgeError(ui.bridge, scan, 2, 1));

    // The status bar stands out and counts both; nothing else in the window
    // changes, as the main window has no banner.
    QCOMPARE(errorHistoryMessages(ui.errors), (QStringList{scan, skipped}));
    QTRY_COMPARE(statusButton->property("text").toString(), QStringLiteral("2 new issues"));
    QVERIFY(statusButton->isVisible());
    QVERIFY(ui.child("mainErrorBanner") == nullptr);
    QVERIFY(!dialog->property("visible").toBool());
    QCOMPARE(transport->mapToScene(QPointF(0, 0)).y(), transportBefore);
    QCOMPARE(mainSplit->mapToScene(QPointF(0, 0)).y(), splitBefore);
    // BridgeClient logs them; the window adds nothing to the console.
    QCOMPARE(takeCapturedMessagesText(), QString());

    // A click on the indicator opens the modal dialog, which lists both and
    // counts as seeing them.
    QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    settle();
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
    QVERIFY(statusBar->property("issuesOpen").toBool());
    // The dialog does not push the window's content.
    QCOMPARE(transport->mapToScene(QPointF(0, 0)).y(), transportBefore);

    // A new error joins the list while it is open.
    QVERIFY(sendBridgeError(ui.bridge, QStringLiteral("Last.fm session expired."), 5, 1));
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
    QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, centerInWindow(clearAll));
    settle();
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QVERIFY(errorHistoryMessages(ui.errors).isEmpty());
    QTRY_VERIFY(!statusButton->isVisible());
    QVERIFY(!statusBar->property("issuesOpen").toBool());
    const QString log = unexpectedOutput(ui.engine);
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::clearAllAsTheDialogFadesKeepsAnIssueThatArrives() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };
    QVERIFY(sendBridgeError(ui.bridge, QStringLiteral("Couldn't play it."), 1, 1));
    QVERIFY(QMetaObject::invokeMethod(ui.errors, "historyRequested"));
    settle();
    QTRY_VERIFY(ui.recentErrors->property("opened").toBool());
    QQuickItem *clearAll = itemNamed(ui.recentContent(), QStringLiteral("errorHistoryClearButton"));
    QVERIFY(clearAll != nullptr);

    QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, centerInWindow(clearAll));
    QVERIFY(popupFadingOut(ui.recentErrors));
    QVERIFY(errorHistoryMessages(ui.errors).isEmpty());

    // An issue arrives as the dialog fades out, and a quick second click on
    // Clear All, where it is now, does not clear it unseen.
    const QString late = QStringLiteral("Last.fm session expired.");
    QVERIFY(sendBridgeError(ui.bridge, late, 5, 1));
    QVERIFY(popupFadingOut(ui.recentErrors));
    QTest::mouseClick(ui.window, Qt::LeftButton, Qt::NoModifier, centerInWindow(clearAll));
    settle();
    QVERIFY(popupsSettled());
    QCOMPARE(errorHistoryMessages(ui.errors), (QStringList{late}));
}

// The same message about the same file, of the same category, is one
// entry, counted; the history keeps the latest entries, up to its limit.
void ErrorsSmokeTest::errorHistoryCountsRepeatsAndKeepsTheLatest() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };
    const QString message = QStringLiteral("Couldn't play “bad.flac”; skipped it.");
    const QString subject = QStringLiteral("/music/bad.flac");

    QVERIFY(sendBridgeError(ui.bridge, message, 1, 0, subject));
    QVERIFY(sendBridgeError(ui.bridge, QStringLiteral("other"), 2, 1));
    QVERIFY(sendBridgeError(ui.bridge, message, 1, 0, subject));
    QVERIFY(sendBridgeError(ui.bridge, message, 1, 0, subject));

    // One entry, counted, and moved to the front as the newest.
    QCOMPARE(errorHistoryMessages(ui.errors), (QStringList{message, QStringLiteral("other")}));
    auto *entries = qvariant_cast<QAbstractItemModel *>(ui.errors->property("entries"));
    QVERIFY(entries != nullptr);
    QCOMPARE(entries->index(0, 0).data(entries->roleNames().key("count")).toInt(), 3);
    QVERIFY(QMetaObject::invokeMethod(ui.errors, "historyRequested"));
    settle();
    QTRY_VERIFY(ui.recentErrors->property("opened").toBool());
    // The title names the problem, with how often it came.
    QTRY_COMPARE(visibleTextItems(ui.recentContent(), message + QStringLiteral(" (×3)")).size(), 1);

    // The same message about another file, or of another category, is another error.
    QVERIFY(sendBridgeError(ui.bridge, message, 1, 0, QStringLiteral("/music/other.flac")));
    QVERIFY(sendBridgeError(ui.bridge, message, 2, 0, subject));
    QCOMPARE(errorHistoryMessages(ui.errors).size(), 4);
    // As is a failure the system tells another way; the same text again
    // is a repeat. The entry keeps its system's text.
    const QString denied = QStringLiteral("failed to replace /c/settings.txt: Permission denied (os error 13)");
    const QString full = QStringLiteral("failed to replace /c/settings.txt: No space left on device (os error 28)");
    const QString saving = QStringLiteral("Settings couldn\u2019t be saved");
    QVERIFY(sendBridgeError(ui.bridge, saving, 6, 1, QStringLiteral("/c/settings.txt"), denied));
    QVERIFY(sendBridgeError(ui.bridge, saving, 6, 1, QStringLiteral("/c/settings.txt"), full));
    QVERIFY(sendBridgeError(ui.bridge, saving, 6, 1, QStringLiteral("/c/settings.txt"), full));
    QCOMPARE(errorHistoryMessages(ui.errors).size(), 6);
    QCOMPARE(entries->index(0, 0).data(entries->roleNames().key("detail")).toString(), full);
    QCOMPARE(entries->index(0, 0).data(entries->roleNames().key("count")).toInt(), 2);
    QCOMPARE(entries->index(1, 0).data(entries->roleNames().key("detail")).toString(), denied);

    // Of more than it keeps, the latest.
    QVERIFY(QMetaObject::invokeMethod(ui.errors, "clear"));
    const int limit = ui.errors->property("historyLimit").toInt();
    QCOMPARE(limit, 20);
    for (int index = 0; index < limit + 5; ++index) {
        QVERIFY(sendBridgeError(ui.bridge, QStringLiteral("error %1").arg(index)));
    }
    QStringList expected;
    for (int index = limit + 4; index >= 5; --index) {
        expected.push_back(QStringLiteral("error %1").arg(index));
    }
    QCOMPARE(errorHistoryMessages(ui.errors), expected);
    const QString log = unexpectedOutput(ui.engine);
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::aLongErrorIsShownWhole() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QObject *popup = ui.child("spectrogramViewerPopup");
    QVERIFY(popup != nullptr);
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };
    QVERIFY(QMetaObject::invokeMethod(ui.viewer, "openSpectrogramViewer"));
    settle();
    QTRY_VERIFY(ui.viewer->property("spectrogramViewerOpen").toBool());
    QString message = QStringLiteral(
        "could not cache the embedded covers of 3 files in '/music'; kept their previous covers "
        "and will retry at the next scan (first: '/music/");
    for (int folder = 0; folder < 12; ++folder) {
        message += QStringLiteral("Some Artist %1 - Some Album With A Long Name/").arg(folder);
    }
    message += QStringLiteral("01 Track.flac': No space left on device (os error 28))");
    QVERIFY(message.size() > 600);

    QVERIFY(sendBridgeError(ui.bridge, message, 2, 0));

    // The viewer has no banner: its button to Recent Issues shows, and the
    // history shows the message whole.
    QTRY_VERIFY(popup->property("opened").toBool());
    QTRY_VERIFY(visibleItemNamed(ui.window, QStringLiteral("viewerIssuesButton")) != nullptr);
    QCOMPARE(errorHistoryMessages(ui.errors), QStringList{message});
    QVERIFY(QMetaObject::invokeMethod(ui.errors, "historyRequested"));
    settle();
    QTRY_VERIFY(ui.recentErrors->property("opened").toBool());
    // The message's label, which shows it with its path broken between
    // folders and its "(os error 28)" whole.
    const auto messageLabels = [&] {
        QList<QQuickItem *> labels;
        for (const QString &name : {QStringLiteral("errorHistoryHeading"), QStringLiteral("errorHistoryMessage")}) {
            for (QQuickItem *label : visibleItemsNamed(ui.recentContent(), name)) {
                if (label->property("message").toString() == message) {
                    labels << label;
                }
            }
        }
        return labels;
    };
    QTRY_COMPARE(messageLabels().size(), 1);
    QQuickItem *row = messageLabels().first();
    const QString shown = row->property("text").toString();
    QCOMPARE(QString(shown).remove(QChar(0x200b)).remove(QChar(0x2060)).replace(QChar(0x00a0), QLatin1Char(' ')),
             message);
    QVERIFY(shown.contains(QStringLiteral("(os error 28)")));
    QVERIFY(!row->property("truncated").toBool());
    QTRY_VERIFY(row->property("lineCount").toInt() > 1);
    QVERIFY(row->property("contentWidth").toReal() <= row->width() + 1);
    const QString log = unexpectedOutput(ui.engine);
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
    ErrorsWindow ui(false, {{QStringLiteral("displaySleepInhibitor"), QVariant::fromValue<QObject *>(&inhibitor)}});
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickItem *surface = ui.item("mainSpectrogramSurface");
    QVERIFY(surface != nullptr);
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };
    ui.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    ui.bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(ui.bridge->property("preventDisplaySleepInFullscreen").toBool());

    QVERIFY(QMetaObject::invokeMethod(ui.viewer, "openSpectrogramViewer"));
    settle();
    QTRY_VERIFY(surface->window() != nullptr && surface->window() != ui.window);
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
    const QStringList messages = errorHistoryMessages(ui.errors);
    QCOMPARE(messages.size(), 1);
    QVERIFY2(messages.first().contains(QStringLiteral("Couldn\u2019t keep the screen awake")),
             qPrintable(messages.first()));
    QVERIFY(messages.first().contains(QStringLiteral("no idle service")));

    // A click on it opens Recent Issues over the viewer, which stays open.
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(issuesButton));
    settle();
    QTRY_VERIFY(ui.recentErrors->property("opened").toBool());
    QCOMPARE(ui.recentContent()->window(), fullscreen);
    QVERIFY(ui.viewer->property("spectrogramViewerOpen").toBool());
    QVERIFY(QMetaObject::invokeMethod(ui.recentErrors, "close"));
    settle();
    QTRY_VERIFY(!ui.recentErrors->property("visible").toBool());
    // Seen, it goes.
    QTRY_VERIFY(visibleItemNamed(fullscreen, QStringLiteral("viewerIssuesButton")) == nullptr);

    QVERIFY(QMetaObject::invokeMethod(ui.viewer, "closeSpectrogramViewer"));
    settle();
    QTRY_VERIFY(!inhibitor.inhibited());
    const QString log = unexpectedOutput(ui.engine);
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::stoppedBackendShowsMessageAndDisablesControls() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QObject *message = ui.child("backendStoppedMessage");
    QObject *statusLabel = ui.child("statusBarBackendStoppedLabel");
    QObject *transportBar = ui.child("transportBar");
    QObject *mainSplit = ui.child("mainSplit");
    QVERIFY(message != nullptr && statusLabel != nullptr && transportBar != nullptr && mainSplit != nullptr);
    QVERIFY(!message->property("visible").toBool());
    QVERIFY(!statusLabel->property("visible").toBool());
    QVERIFY(transportBar->property("enabled").toBool());
    QVERIFY(mainSplit->property("enabled").toBool());

    // What the bridge client reports when the relay finds the backend gone.
    const QString reason = QStringLiteral(
        "The playback engine stopped unexpectedly. Restart Ferrous to continue.");
    QVERIFY(ui.bridge->setProperty("backendFailure", reason));

    QTRY_VERIFY(message->property("visible").toBool());
    QCOMPARE(message->property("text").toString(), reason);
    QVERIFY(statusLabel->property("visible").toBool());
    QVERIFY(!transportBar->property("enabled").toBool());
    QVERIFY(!mainSplit->property("enabled").toBool());
    // Recent Issues lists it, and the status bar's tooltip gives it.
    QCOMPARE(errorHistoryMessages(ui.errors), QStringList{reason});
    QCOMPARE(evaluateOn(ui.item("statusBarBackendStoppedLabel"), QStringLiteral("ToolTip.text")).toString(),
             reason);
}

namespace {

// A failed save before quitting, as BridgeClient's quitSaveFailureMap gives
// it: the settings, and the queue if `queue`, with `cause` and the system's
// text `detail`.
QVariantMap quitSaveFailure(const QString &detail, const QString &cause = {}, bool queue = false) {
    return {{QStringLiteral("settings"), true},
            {QStringLiteral("queue"), queue},
            {QStringLiteral("scrobbles"), false},
            {QStringLiteral("cause"), cause},
            {QStringLiteral("detail"), detail}};
}

// Answers the fallback bridge's save request with this ID: saved, or failed
// with the system's text `detail`.
bool answerStateSave(QObject *bridge, int requestId, bool saved, const QString &detail = {}) {
    return QMetaObject::invokeMethod(bridge, "stateSaveFinished", Q_ARG(int, requestId),
        Q_ARG(bool, saved), Q_ARG(QVariant, saved ? QVariantMap() : quitSaveFailure(detail)));
}

// Makes the fallback bridge's saves fail with `failure`, or succeed with none.
bool failStateSaves(QObject *bridge, const QVariantMap &failure) {
    return bridge->setProperty("stateSaveFailure", failure.isEmpty() ? QVariant() : QVariant(failure));
}

// Runs the save timeout's handler now, in place of the running timer.
bool timeOutStateSave(QObject *window) {
    QObject *timeout = window->findChild<QObject *>(QStringLiteral("quitStateSaveTimeout"));
    return timeout != nullptr && timeout->property("running").toBool()
        && timeout->setProperty("running", false)
        && QMetaObject::invokeMethod(timeout, "triggered");
}

// The ID of the fallback bridge's latest save request.
int lastStateSaveRequest(QObject *bridge) {
    return bridge->property("lastStateSaveRequestId").toInt();
}


} // namespace

void ErrorsSmokeTest::quitActionClosesTheWindowThroughItsCloseHandler() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QObject *quitAction = ui.child("quitAction");
    QVERIFY(quitAction != nullptr);
    QCOMPARE(quitAction->property("standardKey").toInt(), static_cast<int>(QKeySequence::Quit));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    // By its signature: the member pointer would import a symbol of Qt's
    // private API (its argument is private), which no binary may.
    QSignalSpy closing(ui.window, SIGNAL(closing(QQuickCloseEvent*)));
    QVERIFY(closing.isValid());
    QSignalSpy quit(&ui.engine, &QQmlEngine::quit);

    // Ctrl+Q triggers this action. The offscreen platform binds no key to
    // the standard Quit sequence, so the test triggers the action itself.
    QVERIFY(QMetaObject::invokeMethod(quitAction, "trigger"));

    // `closing` runs onClosing, as closing the window from its title bar
    // does. It keeps the window open until the backend has saved the
    // settings and the queue, and then closes it again.
    QTRY_VERIFY(!ui.window->isVisible());
    QCOMPARE(closing.count(), 2);
    QVERIFY(!ui.saveFailed->property("visible").toBool());
    QCOMPARE(quit.count(), 0);
}

void ErrorsSmokeTest::failedSaveBeforeQuitShowsTheErrorAndRetrySaves() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const QString error = QStringLiteral(
        "failed to write /config/ferrous/settings.txt: No space left on device (os error 28)\n"
        "failed to write /config/ferrous/session.txt: No space left on device (os error 28)");
    QVERIFY(failStateSaves(ui.bridge, quitSaveFailure(error, QStringLiteral("noSpace"), true)));

    ui.window->close();

    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    // The system's text as it is; shown with its paths broken between
    // folders and its "(os error 28)" whole.
    QObject *message = ui.saveFailedMessage;
    QCOMPARE(message->property("message").toString(), error);
    QCOMPARE(message->property("text").toString().remove(QChar(0x200b)).remove(QChar(0x2060))
                 .replace(QChar(0x00a0), QLatin1Char(' ')),
             error);
    // Whole: as it is, or with no-break spaces where it wraps.
    QVERIFY(message->property("text").toString().contains(
        QRegularExpression(QStringLiteral("\\(os[ \u00a0]error[ \u00a0]28\\)"))));
    QVERIFY(ui.window->isVisible());
    // The lead names what wasn't saved, the cause says why in plain words,
    // and the system's text is under them, in the secondary colour, as Tag
    // Change Details orders a failed file's lines.
    QQuickItem *heading = ui.item("quitSaveFailedHeading");
    QQuickItem *cause = ui.item("quitSaveFailedCause");
    auto *messageItem = qobject_cast<QQuickItem *>(message);
    QQuickItem *details = ui.item("quitSaveFailedDetails");
    QVERIFY(heading != nullptr && cause != nullptr && messageItem != nullptr && details != nullptr);
    QTRY_VERIFY(heading->isVisible() && cause->isVisible() && messageItem->isVisible());
    const QString headingText = heading->property("text").toString();
    QCOMPARE(headingText, QStringLiteral("Ferrous couldn’t save its settings or the queue."));
    QCOMPARE(cause->property("text").toString(), QStringLiteral("There isn’t enough free space on the disk."));
    QCOMPARE(cause->property("textFormat").toInt(), int(Qt::PlainText));
    QCOMPARE(heading->property("lineCount").toInt(), 1);
    QVERIFY(sceneRectOf(cause).top() >= sceneRectOf(heading).bottom());
    QVERIFY(sceneRectOf(messageItem).top() >= sceneRectOf(cause).bottom());
    // The lead, semibold, as every dialog's first sentence is; the cause
    // regular in the text's own ink; the system's text regular, secondary.
    QCOMPARE(heading->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(cause->property("font").value<QFont>().weight(), QFont::Normal);
    QCOMPARE(colorOf(cause, "color"), colorOf(heading, "color"));
    QCOMPARE(messageItem->property("font").value<QFont>().weight(), QFont::Normal);
    QCOMPARE(details->property("text").toString(),
             QStringLiteral("Quit Anyway loses unsaved settings, queue changes and Last.fm scrobbles."));
    // No cure that contradicts the cause above it, such as a denied
    // permission (it advised freeing disk space under one).
    QVERIFY(!details->property("text").toString().contains(QStringLiteral("disk space")));
    QVERIFY(sceneRectOf(details).top() >= sceneRectOf(messageItem).bottom());
    QVERIFY2(!headingText.contains(QStringLiteral("os error")), qPrintable(headingText));
    QVERIFY(sceneRectOf(messageItem).top() >= sceneRectOf(heading).bottom());
    QCOMPARE(message->property("textFormat").toInt(), int(Qt::PlainText));
    QVERIFY(colorOf(message, "color") != colorOf(heading, "color"));

    // The disk has room again.
    QVERIFY(failStateSaves(ui.bridge, {}));
    QVERIFY(clickButton(ui.window, QStringLiteral("quitSaveFailedRetryButton")));

    QTRY_VERIFY(!ui.window->isVisible());
    // Visible until the style's exit transition ends.
    QTRY_VERIFY(!ui.saveFailed->property("visible").toBool());
}

// After a failed save, Quit Anyway closes the window, also where Recent
// Issues was open: quitting from outside the window, past that modal
// dialog, the failure shows in its place.
void ErrorsSmokeTest::aFailedSaveBeforeQuitIsNotHiddenByRecentErrors() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };
    QVERIFY(sendBridgeError(ui.bridge, QStringLiteral("Couldn't play it."), 1, 1));
    QVERIFY(QMetaObject::invokeMethod(ui.errors, "historyRequested"));
    settle();
    QTRY_VERIFY(ui.recentErrors->property("opened").toBool());
    QVERIFY(failStateSaves(ui.bridge, quitSaveFailure(QStringLiteral("No space left on device"))));

    ui.window->close();
    settle();

    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    QTRY_VERIFY(!ui.recentErrors->property("visible").toBool());
    QVERIFY(clickButton(ui.window, QStringLiteral("quitSaveFailedQuitButton")));
    QTRY_VERIFY(!ui.window->isVisible());
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::retryTakesNoSecondAnswerAsTheDialogFades() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(ui.bridge->setProperty("holdStateSaveAnswers", true));

    ui.window->close();
    const int first = lastStateSaveRequest(ui.bridge);
    QVERIFY(answerStateSave(ui.bridge, first, false, QStringLiteral("failed to write /config/settings.txt")));
    QTRY_VERIFY(ui.saveFailed->property("opened").toBool());
    // A window of its own above the main window.
    auto *dialogWindow = qobject_cast<QQuickWindow *>(ui.saveFailed);
    QVERIFY(dialogWindow != nullptr);
    QCOMPARE(dialogWindow->transientParent(), ui.window);
    QVERIFY(QTest::qWaitForWindowExposed(dialogWindow));

    // A quick second click on Retry, as the dialog goes, saves no second
    // time.
    QVERIFY(clickTwiceAsItCloses(ui.window, QStringLiteral("quitSaveFailedRetryButton"), ui.saveFailed));
    const int retry = lastStateSaveRequest(ui.bridge);
    QCOMPARE(retry, first + 1);
    // Nor does Escape, as it goes, give up on the retry.
    QVERIFY(dialogClosing(ui.saveFailed));
    QTest::keyClick(dialogWindow, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(lastStateSaveRequest(ui.bridge), retry);

    // The retry's answer still closes the window.
    QVERIFY(answerStateSave(ui.bridge, retry, true));
    QTRY_VERIFY(!ui.window->isVisible());
}

// Enter presses Retry as soon as the failure shows, before the keyboard has
// moved into the dialog, and only once; Escape at once still keeps Ferrous
// open (#562).
void ErrorsSmokeTest::enterRetriesAsSoonAsTheSaveFailureShows() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(ui.bridge->setProperty("holdStateSaveAnswers", true));

    ui.window->close();
    const int first = lastStateSaveRequest(ui.bridge);
    QVERIFY(answerStateSave(ui.bridge, first, false, QStringLiteral("failed to write /config/settings.txt")));
    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    auto *dialogWindow = qobject_cast<QQuickWindow *>(ui.saveFailed);
    QVERIFY(dialogWindow != nullptr);
    QVERIFY(keyboardInPopupContent(ui.saveFailed));
    QTest::keyClick(dialogWindow, Qt::Key_Return);
    const int retry = lastStateSaveRequest(ui.bridge);
    QCOMPARE(retry, first + 1);
    // Enter again, as the dialog goes, saves no second time.
    QVERIFY(dialogClosing(ui.saveFailed));
    QTest::keyClick(dialogWindow, Qt::Key_Return);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(lastStateSaveRequest(ui.bridge), retry);

    // The retry fails too; Escape, at once, keeps Ferrous open.
    QVERIFY(answerStateSave(ui.bridge, retry, false, QStringLiteral("failed to write /config/settings.txt")));
    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    QVERIFY(keyboardInPopupContent(ui.saveFailed));
    QTest::keyClick(dialogWindow, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!ui.saveFailed->property("visible").toBool());
    QCOMPARE(lastStateSaveRequest(ui.bridge), retry);
    QVERIFY(ui.window->isVisible());
}

void ErrorsSmokeTest::saveAnsweredAfterItsTimeoutStillClosesTheWindow() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(ui.bridge->setProperty("holdStateSaveAnswers", true));

    ui.window->close();
    const int requestId = lastStateSaveRequest(ui.bridge);
    QCOMPARE(requestId, 1);
    QVERIFY(timeOutStateSave(ui.window));
    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    // It can't say they weren't saved, only that it doesn't know; there is
    // no system's text.
    QCOMPARE(ui.item("quitSaveFailedHeading")->property("text").toString(),
             QStringLiteral("Ferrous couldn’t confirm that its settings and the queue were saved."));
    QCOMPARE(ui.item("quitSaveFailedCause")->property("text").toString(),
             QStringLiteral("Saving didn’t finish in time."));
    QVERIFY(!qobject_cast<QQuickItem *>(ui.saveFailedMessage)->isVisible());
    QVERIFY(ui.window->isVisible());

    // Slow storage finishes the save after all.
    QVERIFY(answerStateSave(ui.bridge, requestId, true));

    QTRY_VERIFY(!ui.window->isVisible());
    QTRY_VERIFY(!ui.saveFailed->property("visible").toBool());
}

void ErrorsSmokeTest::answerToAnEarlierSaveAttemptDoesNotSettleARetry() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(ui.bridge->setProperty("holdStateSaveAnswers", true));

    ui.window->close();
    const int first = lastStateSaveRequest(ui.bridge);
    QVERIFY(timeOutStateSave(ui.window));
    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    // Retry before the first attempt has answered.
    QVERIFY(clickButton(ui.window, QStringLiteral("quitSaveFailedRetryButton")));
    const int retry = lastStateSaveRequest(ui.bridge);
    QVERIFY(retry != first);
    QTRY_VERIFY(!ui.saveFailed->property("visible").toBool());

    // Neither answer to the first attempt settles the retry.
    QVERIFY(answerStateSave(ui.bridge, first, false, QStringLiteral("failed to write /config/settings.txt")));
    QVERIFY(answerStateSave(ui.bridge, first, true));
    QCoreApplication::processEvents();
    QVERIFY(ui.window->isVisible());
    QVERIFY(!ui.saveFailed->property("visible").toBool());

    // The retry's own answer does: its failure is shown, not the first's.
    const QString error = QStringLiteral("failed to write /config/settings.txt: disk full");
    QVERIFY(answerStateSave(ui.bridge, retry, false, error));
    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    QCOMPARE(ui.saveFailedMessage->property("message").toString(), error);

    // A success after another retry closes the window.
    QVERIFY(clickButton(ui.window, QStringLiteral("quitSaveFailedRetryButton")));
    QVERIFY(answerStateSave(ui.bridge, lastStateSaveRequest(ui.bridge), true));
    QTRY_VERIFY(!ui.window->isVisible());
}

void ErrorsSmokeTest::escapeAfterASaveTimeoutKeepsTheWindowOpenWhenTheAnswerComesLate() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(ui.bridge->setProperty("holdStateSaveAnswers", true));

    ui.window->close();
    const int requestId = lastStateSaveRequest(ui.bridge);
    QVERIFY(timeOutStateSave(ui.window));
    QTRY_VERIFY(ui.saveFailed->property("opened").toBool());
    QTest::keyClick(qobject_cast<QQuickWindow *>(ui.saveFailed), Qt::Key_Escape);
    QTRY_VERIFY(!ui.saveFailed->property("visible").toBool());

    QVERIFY(answerStateSave(ui.bridge, requestId, true));
    QCoreApplication::processEvents();
    QVERIFY(ui.window->isVisible());

    // Closing the window again saves again.
    ui.window->close();
    QCOMPARE(lastStateSaveRequest(ui.bridge), requestId + 1);
    QVERIFY(answerStateSave(ui.bridge, requestId + 1, true));
    QTRY_VERIFY(!ui.window->isVisible());
}

void ErrorsSmokeTest::mprisQuitSavesBeforeClosingTheWindow() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto restoreMainWindow = useAsMainWindow(ui.window);
    // By its signature: the member pointer would import a symbol of Qt's
    // private API (its argument is private), which no binary may.
    QSignalSpy closing(ui.window, SIGNAL(closing(QQuickCloseEvent*)));
    QVERIFY(closing.isValid());
    QSignalSpy quit(&ui.engine, &QQmlEngine::quit);

    callMprisQuit();

    // MPRIS Quit closes the window as its close button does: the close
    // handler keeps it open while the backend saves, and closes it after.
    QCOMPARE(lastStateSaveRequest(ui.bridge), 1);
    QVERIFY(ui.window->isVisible());
    QTRY_VERIFY(!ui.window->isVisible());
    QCOMPARE(closing.count(), 2);
    QCOMPARE(quit.count(), 0);
}

void ErrorsSmokeTest::failedSaveAfterMprisQuitShowsTheDialogOverAMinimizedWindow() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto restoreMainWindow = useAsMainWindow(ui.window);
    const QString error = QStringLiteral("failed to write /config/settings.txt");
    QVERIFY(failStateSaves(ui.bridge, quitSaveFailure(error)));
    ui.window->showMinimized();
    QTRY_COMPARE(ui.window->visibility(), QWindow::Minimized);

    callMprisQuit();

    // The dialog is drawn in the window, so the window comes forward with it.
    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    QCOMPARE(ui.saveFailedMessage->property("message").toString(), error);
    QVERIFY(ui.window->isVisible());
    QTRY_VERIFY(ui.window->visibility() != QWindow::Minimized);

    QVERIFY(clickButton(ui.window, QStringLiteral("quitSaveFailedQuitButton")));
    QTRY_VERIFY(!ui.window->isVisible());
}

void ErrorsSmokeTest::quitTimeErrorClosesAFullscreenViewerFirst() {
    ErrorsWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickItem *surface = ui.item("mainSpectrogramSurface");
    QVERIFY(surface != nullptr);
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto restoreMainWindow = useAsMainWindow(ui.window);
    QVERIFY(failStateSaves(ui.bridge, quitSaveFailure(QStringLiteral("failed to write /config/settings.txt"))));

    // The visualization viewer over the whole screen, a modal window of its own.
    ui.bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(ui.viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(surface->window() != nullptr && surface->window() != ui.window);
    QQuickWindow *fullscreen = surface->window();
    QVERIFY(QTest::qWaitForWindowExposed(fullscreen));

    callMprisQuit();

    // The dialog opens in the main window, so the viewer closes first.
    QTRY_VERIFY(ui.saveFailed->property("visible").toBool());
    QVERIFY(!ui.viewer->property("spectrogramViewerOpen").toBool());
    QTRY_VERIFY(!fullscreen->isVisible());
    QVERIFY(ui.window->isVisible());
    const QString log = unexpectedOutput(ui.engine);
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::escapeClosesFullscreenViewersWhileTheirControlsHaveFocus() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    ErrorsWindow ui(true);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    QObject *seekBackward = ui.child("seekBackwardAction");
    QVERIFY(seekBackward != nullptr);
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };
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
        QQuickWindow *fullscreen = viewerWindowNamed(ui.engine, shown.windowName);
        QVERIFY(fullscreen != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(fullscreen));
        QVERIFY(sendBridgeError(bridge, QStringLiteral("Couldn't play \u201cbad.flac\u201d; skipped it."),
                                1, 0, QStringLiteral("/music/bad.flac")));
        QTRY_VERIFY(visibleItemNamed(fullscreen, QStringLiteral("viewerIssuesButton")) != nullptr);
        QQuickItem *issues = visibleItemNamed(fullscreen, QStringLiteral("viewerIssuesButton"));

        // Tab moves into the strip's buttons.
        for (int press = 0; press < 8 && !issues->hasActiveFocus(); ++press) {
            QTest::keyClick(fullscreen, Qt::Key_Tab);
            settle();
        }
        QVERIFY2(issues->hasActiveFocus(), qPrintable(shown.windowName));
        QVERIFY(issues->property("visualFocus").toBool());

        // The main window's shortcuts still work, and Escape still closes
        // the viewer.
        const int seeksBefore = seeks.count();
        QTest::keyClick(fullscreen, Qt::Key_Left, Qt::ShiftModifier);
        settle();
        QTRY_VERIFY(seeks.count() > seeksBefore);
        QVERIFY(viewer->property(shown.openProperty).toBool());
        QTest::keyClick(fullscreen, Qt::Key_Escape);
        settle();
        QTRY_VERIFY2(!viewer->property(shown.openProperty).toBool(), qPrintable(shown.windowName));
        QTRY_VERIFY(!fullscreen->isVisible());
    }
    const QString log = unexpectedOutput(ui.engine);
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::recentErrorsWorkByKeyboardAtTheSmallestWindow() {
    // Popups name themselves to assistive technology only while it is active.
    QAccessible::setActive(true);
    const QScopeGuard stopAccessibility([]() { QAccessible::setActive(false); });
    ErrorsWindow ui(true);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickWindow *window = ui.window;
    window->resize(window->minimumWidth(), window->minimumHeight());
    QTRY_COMPARE(window->height(), window->minimumHeight());
    QObject *bridge = ui.bridge;
    QObject *errors = ui.errors;
    QObject *dialog = ui.recentErrors;
    QObject *playPause = ui.child("playPauseAction");
    QQuickItem *statusButton = ui.item("statusBarErrorsButton");
    QVERIFY(playPause != nullptr && statusButton != nullptr);
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    QQuickItem *content = popupContent(dialog);
    QVERIFY(content != nullptr);
    QQuickItem *view = itemNamed(content, QStringLiteral("errorHistoryView"));
    QQuickItem *diagnostics = itemNamed(content, QStringLiteral("errorHistoryDiagnosticsButton"));
    QQuickItem *clearAll = itemNamed(content, QStringLiteral("errorHistoryClearButton"));
    QVERIFY(view != nullptr && diagnostics != nullptr && clearAll != nullptr);
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };

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
    settle();
    QTRY_VERIFY(dialog->property("opened").toBool());

    // The popup fits the window; the list scrolls. Opened with the
    // pointer, its content has the keyboard, not a control, which would
    // show no ring, so no focus frame wraps the whole list; the list's keys
    // scroll it from there.
    QVERIFY(shownInsideItsWindow(content));
    QVERIFY(shownInsideItsWindow(diagnostics));
    QVERIFY(shownInsideItsWindow(clearAll));
    QVERIFY(view->height() >= QFontMetricsF(QGuiApplication::font()).lineSpacing() * 4);
    auto *flickable = qvariant_cast<QQuickItem *>(view->property("contentItem"));
    QVERIFY(flickable != nullptr);
    QTRY_VERIFY(flickable->property("contentHeight").toReal() > view->height());
    QList<QQuickItem *> actions = itemsNamed(content, QStringLiteral("errorHistoryActionButton"));
    QCOMPARE(actions.size(), limit);
    QTRY_COMPARE(window->activeFocusItem(), content);
    QVERIFY(!view->property("framed").toBool());
    QQuickItem *frame = itemNamed(view, QStringLiteral("keyScrollViewFrame"));
    QVERIFY(frame != nullptr);
    QCOMPARE(QQmlProperty(frame, QStringLiteral("border.color")).read().value<QColor>().alpha(), 0);
    // Nor a ring on the action: it does not have the keyboard.
    QVERIFY(!actions.first()->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Down);
    settle();
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0.0);
    QTest::keyClick(window, Qt::Key_End);
    settle();
    QTRY_VERIFY(std::abs(flickable->property("contentY").toReal()
                         - (flickable->property("contentHeight").toReal() - flickable->height()))
                <= 1.0);
    QTest::keyClick(window, Qt::Key_Home);
    settle();
    QTRY_COMPARE(flickable->property("contentY").toReal(), 0.0);

    // Closing counts as having seen what arrived, so an error that arrives,
    // repeated or not, comes into view; so does the newest on opening.
    QTest::keyClick(window, Qt::Key_End);
    settle();
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0.0);
    QVERIFY(sendBridgeError(
        bridge, QStringLiteral("Couldn't play track 0 of an album with quite a long name"), 1, 1,
        QStringLiteral("/music/track 0.flac")));
    QTRY_COMPARE(flickable->property("contentY").toReal(), 0.0);
    QTest::keyClick(window, Qt::Key_End);
    settle();
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0.0);
    QTest::keyClick(window, Qt::Key_Escape);
    settle();
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    settle();
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(flickable->property("contentY").toReal(), 0.0);
    QTRY_COMPARE(window->activeFocusItem(), content);
    // The keys scroll the list from the header's and the footer's
    // controls too.
    clearAll->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_End);
    settle();
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0.0);
    diagnostics->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Home);
    settle();
    QTRY_COMPARE(flickable->property("contentY").toReal(), 0.0);
    // Tab from Clear All goes on to the first action, with no frame round
    // the list; with the keyboard on the list itself, its frame shows.
    clearAll->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Tab);
    settle();
    QTRY_VERIFY(itemsNamed(content, QStringLiteral("errorHistoryActionButton")).first()->hasActiveFocus());
    QVERIFY(!view->property("framed").toBool());
    flickable->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(view->property("framed").toBool());
    QVERIFY(QQmlProperty(frame, QStringLiteral("border.color")).read().value<QColor>().alpha() > 0);

    // Everything has a name.
    QAccessibleInterface *popupAccessible = QAccessible::queryAccessibleInterface(content);
    QVERIFY(popupAccessible != nullptr);
    QCOMPARE(popupAccessible->text(QAccessible::Name), QStringLiteral("Recent Issues"));
    QCOMPARE(popupAccessible->role(), QAccessible::Dialog);
    QCOMPARE(QAccessible::queryAccessibleInterface(view)->text(QAccessible::Name),
             QStringLiteral("Recent issues"));

    // Tab goes on to each row's action, scrolling the row into view.
    actions = itemsNamed(content, QStringLiteral("errorHistoryActionButton"));
    QCOMPARE(actions.size(), limit);
    QTest::keyClick(window, Qt::Key_Tab);
    settle();
    QTRY_VERIFY(actions.first()->hasActiveFocus());
    QCOMPARE(QAccessible::queryAccessibleInterface(actions.first())->text(QAccessible::Name),
             QStringLiteral("Show in Library"));
    for (int press = 1; press < limit; ++press) {
        QTest::keyClick(window, Qt::Key_Tab);
        settle();
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
    settle();
    QTRY_VERIFY(diagnostics->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Tab);
    settle();
    QTRY_VERIFY(clearAll->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Escape);
    settle();
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(errorHistoryMessages(errors).size(), limit);
    QVERIFY(window->activeFocusItem() == nullptr
            || !content->isAncestorOf(window->activeFocusItem()));
    QTest::keyClick(window, Qt::Key_Space);
    settle();
    QTRY_COMPARE(toggled.count(), 1);

    // Opened with the pointer, Space neither plays nor pauses nor presses
    // an action; Tab marks the first one, and Space presses that.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    settle();
    QTRY_VERIFY(dialog->property("opened").toBool());
    actions = itemsNamed(content, QStringLiteral("errorHistoryActionButton"));
    QVERIFY(!actions.isEmpty());
    QTRY_COMPARE(window->activeFocusItem(), content);
    QTest::keyClick(window, Qt::Key_Space);
    settle();
    QCoreApplication::processEvents();
    QVERIFY(dialog->property("visible").toBool());
    QCOMPARE(toggled.count(), 1);
    QTest::keyClick(window, Qt::Key_Tab);
    settle();
    QTRY_VERIFY(actions.first()->hasActiveFocus());
    QVERIFY(actions.first()->property("visualFocus").toBool());
    QTest::keyClick(window, Qt::Key_Space);
    settle();
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(toggled.count(), 1);
    QCOMPARE(bridge->property("shownInLibrary").toStringList(), QStringList{QStringLiteral("/music/track 0.flac")});

    // With no entry that has an action, Tab takes the keyboard to Open
    // Diagnostics, never Clear All.
    QVERIFY(QMetaObject::invokeMethod(errors, "clear"));
    QTRY_COMPARE(errors->property("count").toInt(), 0);
    QVERIFY(sendBridgeError(bridge, QStringLiteral("Playback stopped unexpectedly"), 1, 1));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    settle();
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(itemsNamed(content, QStringLiteral("errorHistoryActionButton")).first()->isVisible(), false);
    QTRY_COMPARE(window->activeFocusItem(), content);
    QTest::keyClick(window, Qt::Key_Tab);
    settle();
    QTRY_VERIFY(diagnostics->hasActiveFocus());
    QVERIFY(!view->property("framed").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    settle();
    QTRY_VERIFY(!dialog->property("visible").toBool());
    const QString log = unexpectedOutput(ui.engine);
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::viewerIssuesButtonsOpenRecentErrorsAboveTheViewer() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    ErrorsWindow ui(true);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickWindow *window = ui.window;
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    QObject *errors = ui.errors;
    QObject *dialog = ui.recentErrors;
    QObject *playPause = ui.child("playPauseAction");
    QVERIFY(playPause != nullptr);
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    QQuickItem *content = popupContent(dialog);
    QVERIFY(content != nullptr);
    QQuickItem *clearAll = itemNamed(content, QStringLiteral("errorHistoryClearButton"));
    QQuickItem *view = itemNamed(content, QStringLiteral("errorHistoryView"));
    QVERIFY(clearAll != nullptr && view != nullptr);
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };

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
        settle();
        QTRY_VERIFY(viewer->property(shown.openProperty).toBool());
        QQuickWindow *host = window;
        if (shown.windowName != nullptr) {
            host = viewerWindowNamed(ui.engine, QLatin1String(shown.windowName));
            QVERIFY(host != nullptr);
        }
        QVERIFY(QTest::qWaitForWindowActive(host));

        QVERIFY(sendBridgeError(bridge, QStringLiteral("Couldn't play \u201cbad.flac\u201d; skipped it."),
                                1, 0, QStringLiteral("/music/bad.flac")));
        settle();
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
        settle();
        QTRY_VERIFY2(dialog->property("opened").toBool(), name.constData());
        QVERIFY(viewer->property(shown.openProperty).toBool());
        QTRY_VERIFY2(content->hasActiveFocus(), name.constData());

        // In the window the user is looking at, and on top of it. A press
        // outside closes it alone: the viewer stays open.
        QCOMPARE(content->window(), host);
        QVERIFY(dialog->property("modal").toBool());
        QTRY_VERIFY(shownInsideItsWindow(clearAll));
        QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, QPoint(host->width() / 3, host->height() / 3));
        settle();
        QTRY_VERIFY2(!dialog->property("visible").toBool(), name.constData());
        QVERIFY(viewer->property(shown.openProperty).toBool());
        // What the dialog showed counts as seen, as for the status bar, so
        // the button has gone.
        QTRY_VERIFY2(!historyButton->isVisible(), name.constData());

        // Space does not reach the main window's shortcuts while the dialog
        // is open, and Escape closes the dialog alone.
        QVERIFY(QMetaObject::invokeMethod(errors, "historyRequested"));
        settle();
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY2(content->hasActiveFocus(), name.constData());
        const int toggles = toggled.count();
        QTest::keyClick(host, Qt::Key_Space);
        QCoreApplication::processEvents();
        QCOMPARE(toggled.count(), toggles);
        QTest::keyClick(host, Qt::Key_Escape);
        settle();
        QTRY_VERIFY2(!dialog->property("visible").toBool(), name.constData());
        QVERIFY2(viewer->property(shown.openProperty).toBool(), name.constData());

        // The viewer has the keyboard back, and not on the button that
        // went: its Escape closes it.
        QVERIFY2(host->activeFocusItem() != historyButton, name.constData());
        QTest::keyClick(host, Qt::Key_Escape);
        settle();
        QTRY_VERIFY2(!viewer->property(shown.openProperty).toBool(), name.constData());
        if (host != window) {
            QTRY_VERIFY(!host->isVisible());
        }
        QVERIFY(QMetaObject::invokeMethod(errors, "clear"));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
    const QString log = unexpectedOutput(ui.engine);
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

void ErrorsSmokeTest::recentErrorsFollowAFullscreenViewerThatCloses() {
    ErrorsWindow ui(true);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickWindow *window = ui.window;
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    QObject *errors = ui.errors;
    QObject *dialog = ui.recentErrors;
    QQuickItem *statusButton = ui.item("statusBarErrorsButton");
    QQuickItem *content = ui.recentContent();
    QVERIFY(statusButton != nullptr && content != nullptr);
    // The popups' fades, each to its end, on the test's time.
    SteppedAnimationDriver animations;
    const auto settle = [&animations]() { (void)animations.step(1000); };
    QQuickItem *clearAll = itemNamed(content, QStringLiteral("errorHistoryClearButton"));
    QVERIFY(clearAll != nullptr);
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(sendBridgeError(bridge, QStringLiteral("Couldn't play it."), 1, 1));

    // A viewer can also close from outside, such as for a quit from the
    // desktop's media controls. The dialog then cannot stay in its window,
    // which would leave it out of reach.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    settle();
    QQuickWindow *fullscreen = viewerWindowNamed(ui.engine, QStringLiteral("spectrogramFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QVERIFY(QMetaObject::invokeMethod(errors, "historyRequested"));
    settle();
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(content->window(), fullscreen);

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    settle();
    QTRY_VERIFY(!fullscreen->isVisible());
    QTRY_COMPARE(content->window(), window);
    QVERIFY(dialog->property("visible").toBool());
    QVERIFY(QTest::qWaitForWindowActive(window));
    QTRY_VERIFY(shownInsideItsWindow(clearAll));
    // A press outside it, in the main window, closes it.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(window->width() / 3, window->height() / 3));
    settle();
    QTRY_VERIFY(!dialog->property("visible").toBool());

    // Nothing of it stays in the way of the main window.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(statusButton));
    settle();
    QTRY_VERIFY(dialog->property("opened").toBool());
    const QString log = unexpectedOutput(ui.engine);
    QVERIFY2(log.isEmpty(), qPrintable(log));
}

std::unique_ptr<QmlSmokeTest> makeErrorsSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<ErrorsSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_errors.moc"
