// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// The tag editor, renaming, and quitting with tag edits.
class TagEditorSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void tagEditorOpensALargeSelectionWithoutBuildingEveryRow();
    void tagEditorCancelAsksBeforeDroppingEdits();
    void closingTheTagEditorKeepsTheFieldBeingTyped();
    void tagEditorStaysOpenWhileSavingAndShowsTheResult();
    void closingTheWindowWithTagEditsAsksAndCancelKeepsThem();
    void closingTheWindowWithTagEditsCanDiscardThem();
    void closingTheWindowWithTagEditsCanSaveThemFirst();
    void tagSaveFailureWhileQuittingKeepsTheWindowOpen();
    void closingTheWindowWhileTagsSaveWaitsForTheSave();
    void mprisQuitWithTagEditsAsksFirst();
    void quittingDuringAStalledTagSaveOffersToQuitAnyway();
    void savingTagEditsChosenWhenQuittingWaitsWithinTheBound();
    void keepWaitingForAStalledTagSaveQuitsWhenItFinishes();
    void repeatedQuitsDuringATagSaveShowOneWait();
    void tagEditorReloadAsksBeforeDroppingEdits();
    void aSecondClickAsAConfirmationFadesAnswersNothingMore();
    void discardAndReloadClickedTwiceAsItFadesReloadsOnce();
    void quitAnywayTakesNoSecondAnswerAsTheWaitFades();
    void askingToCloseAfterAReloadQuestionOffersSaveAndDiscard_data();
    void askingToCloseAfterAReloadQuestionOffersSaveAndDiscard();
    void switchingTheTagEditorQuestionsKeepsEveryButton();
    void enterAnswersTheTagEditorsQuestionAsSoonAsItShows();
    void enterKeepsWaitingOnceTheQuitWaitOffersIt();
    void enterNumbersTracksAsSoonAsAutoNumberShows();
    void enterRenamesAsSoonAsThePreviewShows();
    void autoNumberRejectsStartsThatAreNotNumbers();
    void tagEditorRowsFollowTheKeyboard();
    void tagEditorShowsEveryValueOfMultiValueFields();
    void tagEditorLibrarySupportGateMatchesSupportedRows();
    void renameFilesShowsTheNewNamesAndCancelRenamesNothing();
    void renameRenamesThePreviewedFilesAfterTheSelectionChanged();
};

namespace {

// A load response for `count` tracks, ten to an album folder. Row 0 of
// `extraArtists` holds artists after its first.
QByteArray tagEditorLoadResponse(int count, const QStringList &extraArtists = {}) {
    QJsonArray rows;
    QJsonArray paths;
    for (int i = 0; i < count; ++i) {
        const QString directory = QStringLiteral("/fixture/Album %1").arg(i / 10, 3, 10, QLatin1Char('0'));
        const QString fileName = QStringLiteral("%1 Song.flac").arg(i % 10 + 1, 2, 10, QLatin1Char('0'));
        const QString path = directory + QLatin1Char('/') + fileName;
        paths.push_back(path);
        QJsonObject row{
            {QStringLiteral("path"), path},
            {QStringLiteral("fileName"), fileName},
            {QStringLiteral("directory"), directory},
            {QStringLiteral("formatKind"), QStringLiteral("FLAC")},
            {QStringLiteral("title"), QStringLiteral("Song %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("Artist")},
            {QStringLiteral("album"), QStringLiteral("Album %1").arg(i / 10)},
            {QStringLiteral("trackNo"), QStringLiteral("%1").arg(i % 10 + 1, 2, 10, QLatin1Char('0'))},
        };
        if (i == 0 && !extraArtists.isEmpty()) {
            row.insert(QStringLiteral("extraValues"),
                       QJsonObject{{QStringLiteral("artist"), QJsonArray::fromStringList(extraArtists)}});
        }
        rows.push_back(row);
    }
    return QJsonDocument(QJsonObject{
        {QStringLiteral("resolvedPaths"), paths},
        {QStringLiteral("rows"), rows},
    }).toJson(QJsonDocument::Compact);
}

const QString kFirstTagEditorPath = QStringLiteral("/fixture/Album 000/01 Song.flac");

// What the injected tag backend answers saves with, and what it was sent.
// Saves run on a worker thread.
struct TagSaves {
    QMutex mutex;
    QByteArray response = QByteArray(R"({"results":[{"path":"/fixture/Album 000/01 Song.flac","ok":true}],)"
                                     R"("successfulPaths":["/fixture/Album 000/01 Song.flac"]})");
    QByteArray lastPayload;
    int count{0};
    // While set, saves wait for `release`.
    bool hold{false};
    QSemaphore release;

    void setResponse(const QByteArray &next) {
        QMutexLocker locker(&mutex);
        response = next;
    }
    void setHold(bool value) {
        QMutexLocker locker(&mutex);
        hold = value;
    }
    int saveCount() {
        QMutexLocker locker(&mutex);
        return count;
    }
    QByteArray payload() {
        QMutexLocker locker(&mutex);
        return lastPayload;
    }
};

const QByteArray kFailedTagSave(R"({"results":[{"path":"/fixture/Album 000/01 Song.flac","ok":false,)"
                                R"("error":"permission denied"}],"successfulPaths":[]})");

// Answers `editor`'s loads with `loadResponse` and its saves from `saves`.
void useTagBackend(TagEditorController &editor, const QByteArray &loadResponse,
                   const std::shared_ptr<TagSaves> &saves) {
    editor.m_executeOperation = [loadResponse, saves](TagEditorController::Operation operation,
                                                      const QByteArray &payload,
                                                      const TagEditorController::RenameGate &) {
        if (operation != TagEditorController::Operation::Save) {
            return loadResponse;
        }
        bool hold = false;
        {
            QMutexLocker locker(&saves->mutex);
            ++saves->count;
            saves->lastPayload = payload;
            hold = saves->hold;
        }
        if (hold) {
            saves->release.tryAcquire(1, 10000);
        }
        QMutexLocker locker(&saves->mutex);
        return saves->response;
    };
}

// Loads and shows Main.qml with `editor` as its tag editor.
QQuickWindow *loadMainWindowWithTagEditor(
    QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel, TagEditorController &editor) {
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    return loadMainWindowForQuit(engine, libraryModel);
}

bool openTagEditor(TagEditorController &editor) {
    return editor.openSelection(
        QVariantList{QVariantMap{{QStringLiteral("path"), QStringLiteral("/fixture")}}});
}

// The table rows the tag editor has created.
int tagEditorRowItemCount(QQuickWindow *window) {
    int count = 0;
    QList<QQuickItem *> items;
    collectQuickItems(window->contentItem(), &items);
    for (QQuickItem *item : std::as_const(items)) {
        if (item->metaObject()->indexOfProperty("errorText") >= 0
            && item->metaObject()->indexOfProperty("fileName") >= 0) {
            ++count;
        }
    }
    return count;
}

QQuickItem *tagEditorRowItem(QQuickWindow *window, int row) {
    QList<QQuickItem *> items;
    collectQuickItems(window->contentItem(), &items);
    for (QQuickItem *item : std::as_const(items)) {
        if (item->objectName() == QStringLiteral("tagEditorRow")
            && item->property("index").toInt() == row) {
            return item;
        }
    }
    return nullptr;
}

QStringList tagEditorCellTexts(QQuickItem *row) {
    QStringList texts;
    QList<QQuickItem *> items;
    collectQuickItems(row, &items);
    for (QQuickItem *item : std::as_const(items)) {
        if (item != row && item->metaObject()->indexOfProperty("text") >= 0) {
            texts.push_back(item->property("text").toString());
        }
    }
    return texts;
}

QVariant evaluateOn(QQuickItem *item, const QString &expression) {
    QQmlExpression evaluated(qmlContext(item), item, expression);
    bool undefined = false;
    const QVariant value = evaluated.evaluate(&undefined);
    return evaluated.hasError() || undefined ? QVariant() : value;
}

bool isDefaultButton(QQuickItem *button) {
    return evaluateOn(button, QStringLiteral("Accessible.defaultButton")).toBool();
}

QList<int> selectedTagEditorRows(const TagEditorController &editor) {
    QList<int> rows;
    for (const QVariant &row : editor.selectedRows()) {
        rows.push_back(row.toInt());
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

// Main.qml with a tag editor open on `rowCount` rows, the first of them
// edited when `edit` is set.
struct TagEditorWindow {
    std::shared_ptr<TagSaves> saves = std::make_shared<TagSaves>();
    TagEditorController editor{nullptr};
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window{nullptr};
    QObject *dialog{nullptr};
    QObject *confirm{nullptr};
    QObject *bridge{nullptr};

    bool open(int rowCount, bool edit, const QStringList &extraArtists = {}) {
        useTagBackend(editor, tagEditorLoadResponse(rowCount, extraArtists), saves);
        window = loadMainWindowWithTagEditor(engine, libraryModel, editor);
        if (window == nullptr) {
            return false;
        }
        dialog = window->findChild<QObject *>(QStringLiteral("tagEditorDialog"));
        confirm = window->findChild<QObject *>(QStringLiteral("tagEditorCloseConfirmDialog"));
        bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        if (dialog == nullptr || confirm == nullptr || bridge == nullptr || !openTagEditor(editor)) {
            return false;
        }
        if (!QTest::qWaitFor([this]() {
                return !editor.loading() && dialog->property("opened").toBool();
            })) {
            return false;
        }
        if (edit) {
            editor.setCell(0, QStringLiteral("title"), QStringLiteral("Edited"));
        }
        return editor.dirty() == edit;
    }

    // A closed dialog is visible until the style's exit transition ends:
    // wait for it to hide, QTRY_VERIFY(!...Shown()).
    bool confirmShown() const { return confirm->property("visible").toBool(); }
    bool editorShown() const { return dialog->property("visible").toBool(); }
    QString confirmMessage() const {
        QObject *message = window->findChild<QObject *>(QStringLiteral("tagEditorCloseConfirmMessage"));
        return message != nullptr ? message->property("text").toString() : QString();
    }
    int stateSaves() const { return bridge->property("lastStateSaveRequestId").toInt(); }
    // The confirmation's button `objectName`, when it is in its button row
    // and shown.
    QQuickItem *confirmButtonShown(const QString &objectName) const {
        auto *footer = qobject_cast<QQuickItem *>(qvariant_cast<QObject *>(confirm->property("footer")));
        auto *button = window->findChild<QQuickItem *>(objectName);
        return footer != nullptr && button != nullptr && footer->isAncestorOf(button) && button->isVisible()
            ? button
            : nullptr;
    }
    // Gives the keyboard to the confirmation's text, out of its buttons.
    bool focusConfirmContent() const {
        auto *content = qobject_cast<QQuickItem *>(qvariant_cast<QObject *>(confirm->property("contentItem")));
        if (content == nullptr) {
            return false;
        }
        content->forceActiveFocus();
        return content->hasActiveFocus();
    }
    // Deletes what waits for deletion, and what the engine no longer holds.
    void settleDeletions() {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        engine.collectGarbage();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    QObject *quitWait() const {
        return window->findChild<QObject *>(QStringLiteral("tagEditorQuitWaitDialog"));
    }
    bool quitWaitShown() const {
        return quitWait() != nullptr && quitWait()->property("visible").toBool();
    }
    bool quitAnywayOffered() const {
        QObject *button = window->findChild<QObject *>(QStringLiteral("tagEditorQuitWaitQuitButton"));
        return quitWaitShown() && quitWait()->property("offerChoice").toBool() && button != nullptr
            && button->property("visible").toBool();
    }
    // Runs the quit wait's bound now, in place of the running timer.
    bool passQuitWaitBound() const {
        QObject *timeout = window->findChild<QObject *>(QStringLiteral("tagEditorQuitWaitTimeout"));
        return timeout != nullptr && timeout->property("running").toBool()
            && timeout->setProperty("running", false)
            && QMetaObject::invokeMethod(timeout, "triggered");
    }
    // Starts a save of the edit that does not finish until `saves->release`.
    bool startStalledSave() {
        saves->setHold(true);
        return clickButton(window, QStringLiteral("tagEditorSaveButton"))
            && QTest::qWaitFor([this]() { return saves->saveCount() == 1; })
            && editor.saving();
    }
};

} // namespace

void TagEditorSmokeTest::tagEditorOpensALargeSelectionWithoutBuildingEveryRow() {
    constexpr int kRows = 2000;
    TagEditorWindow fixture;
    useTagBackend(fixture.editor, tagEditorLoadResponse(kRows), fixture.saves);
    fixture.window = loadMainWindowWithTagEditor(fixture.engine, fixture.libraryModel, fixture.editor);
    QVERIFY2(fixture.window != nullptr, "Main.qml failed to show");
    QQuickWindow *window = fixture.window;
    TagEditorController &editor = fixture.editor;
    QObject *dialog = findObjectByStringProperty(window, "title", QStringLiteral("Edit Tags"));
    QVERIFY(dialog != nullptr);

    QSignalSpy frames(window, &QQuickWindow::frameSwapped);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(openTagEditor(editor));
    QTRY_VERIFY(!editor.loading());
    const qint64 loadedMs = timer.elapsed();
    frames.clear();
    QTRY_VERIFY(frames.count() > 0);
    const qint64 firstFrameMs = timer.elapsed();
    const int rowItems = tagEditorRowItemCount(window);
    qInfo("tag editor: %d rows loaded in %lld ms, first frame after %lld ms, %d row items",
          kRows, static_cast<long long>(loadedMs), static_cast<long long>(firstFrameMs), rowItems);
    QVERIFY(dialog->property("visible").toBool());
    QCOMPARE(editor.loadedPaths().size(), kRows);
    QCOMPARE(selectedTagEditorRows(editor), QList<int>{0});

    // Only the rows in view have items. Building all 2,000 took seconds.
    QVERIFY2(rowItems > 0 && rowItems < 100, qPrintable(QString::number(rowItems)));
    QVERIFY2(firstFrameMs < 4000, qPrintable(QString::number(firstFrameMs)));

    // Selecting every row and scrolling to the end keeps it that way.
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    QTRY_COMPARE(editor.selectedCount(), kRows);
    QTest::keyClick(window, Qt::Key_End, Qt::ShiftModifier);
    QTRY_VERIFY(tagEditorRowItem(window, kRows - 1) != nullptr);
    QVERIFY(tagEditorRowItem(window, kRows - 1)->property("selected").toBool());
    QVERIFY(tagEditorRowItemCount(window) < 100);
}

void TagEditorSmokeTest::tagEditorCancelAsksBeforeDroppingEdits() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));

    // Cancel asks, as the close button and Escape do.
    QTRY_VERIFY(fixture.confirmShown());
    QCOMPARE(fixture.confirmMessage(), QStringLiteral("Save changes before closing the tag editor?"));
    QVERIFY(fixture.editorShown());
    QVERIFY(fixture.editor.isOpen());

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmKeepEditingButton")));
    QTRY_VERIFY(!fixture.confirmShown());
    QVERIFY(fixture.editorShown());
    QVERIFY(fixture.editor.dirty());

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(fixture.confirmShown());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmDiscardButton")));
    QTRY_VERIFY(!fixture.editorShown());
    QVERIFY(!fixture.editor.isOpen());
    QCOMPARE(fixture.saves->saveCount(), 0);

    // Without edits, Cancel closes at once.
    QVERIFY(openTagEditor(fixture.editor));
    QTRY_VERIFY(!fixture.editor.loading() && fixture.dialog->property("opened").toBool());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(!fixture.editorShown());
    QVERIFY(!fixture.confirmShown());
}

void TagEditorSmokeTest::closingTheTagEditorKeepsTheFieldBeingTyped() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, false));
    auto *title = qobject_cast<QQuickItem *>(findQuickItemByObjectName(
        fixture.window->contentItem(), QStringLiteral("tagEditorField_title")));
    QVERIFY(title != nullptr);
    title->forceActiveFocus();
    QTRY_VERIFY(title->hasActiveFocus());
    QTest::keyClick(fixture.window, 'x');
    QVERIFY(!fixture.editor.dirty());

    // Typed text counts as an edit once the editor is asked to close.
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(fixture.confirmShown());
    QVERIFY(fixture.editor.dirty());
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("Song 0x"));
}

void TagEditorSmokeTest::tagEditorStaysOpenWhileSavingAndShowsTheResult() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    fixture.saves->setHold(true);
    fixture.saves->setResponse(kFailedTagSave);
    QObject *cancel = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorCancelButton"));
    QObject *closeButton = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorCloseButton"));
    QVERIFY(cancel != nullptr);
    QVERIFY(closeButton != nullptr);

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorSaveButton")));
    QTRY_COMPARE(fixture.saves->saveCount(), 1);
    QVERIFY(fixture.editor.saving());

    // Nothing closes the editor while it saves.
    QVERIFY(!cancel->property("enabled").toBool());
    QVERIFY(!closeButton->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
    QVERIFY(QMetaObject::invokeMethod(fixture.dialog, "requestClose"));
    QVERIFY(QMetaObject::invokeMethod(fixture.dialog, "closeEditor"));
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QCoreApplication::processEvents();
    QVERIFY(fixture.editorShown());
    QVERIFY(!fixture.confirmShown());

    fixture.saves->release.release();
    QTRY_VERIFY(!fixture.editor.saving());

    // The failure shows in the editor, still open on the unsaved edit.
    QVERIFY(fixture.editorShown());
    QVERIFY(fixture.editor.isOpen());
    QVERIFY(fixture.editor.statusIsError());
    QCOMPARE(fixture.editor.statusText(), QStringLiteral("Saved 0 files; 1 failed."));
    QVERIFY(fixture.dialog->property("statusHasFailure").toBool());
    QVERIFY(fixture.editor.dirty());
    QVERIFY(cancel->property("enabled").toBool());
    // The app's error list has it too, with the file that failed.
    QObject *errors = fixture.window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(errors != nullptr);
    QCOMPARE(errorHistoryMessages(errors),
             QStringList{QStringLiteral("Saved 0 files; 1 failed.\n01 Song.flac: permission denied")});
    QVERIFY(finishIncubation(fixture.engine));
    QCOMPARE(takeCapturedMessagesText(), QString());
}

void TagEditorSmokeTest::closingTheWindowWithTagEditsAsksAndCancelKeepsThem() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();

    fixture.window->close();

    // Quitting waits for an answer before it saves the settings and queue.
    QTRY_VERIFY(fixture.confirmShown());
    QCOMPARE(fixture.confirmMessage(), QStringLiteral("Save tag changes before quitting Ferrous?"));
    QVERIFY(fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 0);

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmKeepEditingButton")));
    QTRY_VERIFY(!fixture.confirmShown());
    QCoreApplication::processEvents();
    QVERIFY(fixture.window->isVisible());
    QVERIFY(fixture.editorShown());
    QVERIFY(fixture.editor.dirty());
    QCOMPARE(fixture.stateSaves(), 0);
    QCOMPARE(fixture.saves->saveCount(), 0);

    // Escape cancels too.
    fixture.window->close();
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(!fixture.confirmShown());
    QCoreApplication::processEvents();
    QVERIFY(fixture.window->isVisible());
    QVERIFY(fixture.editorShown());
    QCOMPARE(fixture.stateSaves(), 0);
}

void TagEditorSmokeTest::closingTheWindowWithTagEditsCanDiscardThem() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();

    fixture.window->close();
    QTRY_VERIFY(fixture.confirmShown());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmDiscardButton")));

    QTRY_VERIFY(!fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 1);
    QCOMPARE(fixture.saves->saveCount(), 0);
    QVERIFY(!fixture.editor.isOpen());
}

void TagEditorSmokeTest::closingTheWindowWithTagEditsCanSaveThemFirst() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    fixture.saves->setHold(true);

    fixture.window->close();
    QTRY_VERIFY(fixture.confirmShown());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmSaveButton")));
    QTRY_COMPARE(fixture.saves->saveCount(), 1);
    QCoreApplication::processEvents();
    QVERIFY(fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 0);

    fixture.saves->release.release();

    // The settings and queue are saved after the tags, then the window closes.
    QTRY_VERIFY(!fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 1);
    const QJsonArray rows = QJsonDocument::fromJson(fixture.saves->payload())
                                .object().value(QStringLiteral("rows")).toArray();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().toObject().value(QStringLiteral("title")).toString(), QStringLiteral("Edited"));
}

void TagEditorSmokeTest::tagSaveFailureWhileQuittingKeepsTheWindowOpen() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    fixture.saves->setResponse(kFailedTagSave);

    fixture.window->close();
    QTRY_VERIFY(fixture.confirmShown());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmSaveButton")));
    QTRY_COMPARE(fixture.saves->saveCount(), 1);
    QTRY_VERIFY(!fixture.editor.saving());

    // The failure is shown and Ferrous stays open.
    QTRY_VERIFY(!fixture.confirmShown());
    QCoreApplication::processEvents();
    QVERIFY(fixture.window->isVisible());
    QVERIFY(fixture.editorShown());
    QVERIFY(fixture.editor.statusIsError());
    QVERIFY(fixture.editor.dirty());
    QCOMPARE(fixture.stateSaves(), 0);
    QVERIFY(finishIncubation(fixture.engine));
    QCOMPARE(takeCapturedMessagesText(), QString());

    // Quitting again asks again.
    fixture.window->close();
    QTRY_VERIFY(fixture.confirmShown());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmDiscardButton")));
    QTRY_VERIFY(!fixture.window->isVisible());
}

void TagEditorSmokeTest::closingTheWindowWhileTagsSaveWaitsForTheSave() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    fixture.saves->setHold(true);
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorSaveButton")));
    QTRY_COMPARE(fixture.saves->saveCount(), 1);

    fixture.window->close();
    QCoreApplication::processEvents();
    QVERIFY(fixture.window->isVisible());
    QVERIFY(fixture.editorShown());
    QVERIFY(!fixture.confirmShown());
    QCOMPARE(fixture.stateSaves(), 0);

    fixture.saves->release.release();
    QTRY_VERIFY(!fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 1);
    QVERIFY(!fixture.editor.dirty());
}

void TagEditorSmokeTest::mprisQuitWithTagEditsAsksFirst() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto restoreMainWindow = useAsMainWindow(fixture.window);
    fixture.window->showMinimized();
    QTRY_COMPARE(fixture.window->visibility(), QWindow::Minimized);

    callMprisQuit();

    // The question is drawn in the window, so the window comes forward.
    QTRY_VERIFY(fixture.confirmShown());
    QTRY_VERIFY(fixture.window->visibility() != QWindow::Minimized);
    QCOMPARE(fixture.confirmMessage(), QStringLiteral("Save tag changes before quitting Ferrous?"));
    QCOMPARE(fixture.stateSaves(), 0);

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmDiscardButton")));
    QTRY_VERIFY(!fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 1);
}

void TagEditorSmokeTest::quittingDuringAStalledTagSaveOffersToQuitAnyway() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(fixture.startStalledSave());

    fixture.window->close();

    // Quitting waits for the save and says which files it writes.
    QTRY_VERIFY(fixture.quitWaitShown());
    QObject *files = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorQuitWaitFiles"));
    QVERIFY(files != nullptr);
    QCOMPARE(files->property("text").toString(), QStringLiteral("01 Song.flac"));
    QVERIFY(!fixture.quitAnywayOffered());
    QVERIFY(fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 0);

    // After the bound, it offers to quit without the save.
    QVERIFY(fixture.passQuitWaitBound());
    QTRY_VERIFY(fixture.quitAnywayOffered());
    QObject *message = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorQuitWaitMessage"));
    QVERIFY(message != nullptr);
    QVERIFY(message->property("text").toString().contains(QStringLiteral("may be lost")));

    // Quit Anyway goes on to the usual shutdown: the settings and queue are
    // saved, and the window closes while the tag save still runs.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorQuitWaitQuitButton")));
    QTRY_VERIFY(!fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 1);
    QVERIFY(fixture.editor.saving());
    fixture.saves->release.release();
}

void TagEditorSmokeTest::savingTagEditsChosenWhenQuittingWaitsWithinTheBound() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    fixture.saves->setHold(true);
    fixture.window->close();
    QTRY_VERIFY(fixture.confirmShown());

    // Saving from the quit confirmation, on storage that does not answer.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmSaveButton")));
    QTRY_COMPARE(fixture.saves->saveCount(), 1);

    // The confirmation gives way to the wait, which shows the file. The
    // confirmation is visible until the style's exit transition ends.
    QTRY_VERIFY(fixture.quitWaitShown());
    QTRY_VERIFY(!fixture.confirmShown());
    QObject *files = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorQuitWaitFiles"));
    QVERIFY(files != nullptr);
    QCOMPARE(files->property("text").toString(), QStringLiteral("01 Song.flac"));
    QVERIFY(!fixture.quitAnywayOffered());
    QCOMPARE(fixture.stateSaves(), 0);

    QVERIFY(fixture.passQuitWaitBound());
    QTRY_VERIFY(fixture.quitAnywayOffered());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorQuitWaitQuitButton")));
    QTRY_VERIFY(!fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 1);
    QCOMPARE(fixture.window->findChildren<QObject *>(QStringLiteral("tagEditorQuitWaitDialog")).size(), 1);
    fixture.saves->release.release();
}

void TagEditorSmokeTest::keepWaitingForAStalledTagSaveQuitsWhenItFinishes() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(fixture.startStalledSave());
    fixture.window->close();
    QTRY_VERIFY(fixture.quitWaitShown());
    QVERIFY(fixture.passQuitWaitBound());
    QTRY_VERIFY(fixture.quitAnywayOffered());

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorQuitWaitKeepWaitingButton")));

    // The wait goes on, and offers the choice again after another bound.
    QCoreApplication::processEvents();
    QVERIFY(fixture.window->isVisible());
    QVERIFY(fixture.quitWaitShown());
    QVERIFY(!fixture.quitAnywayOffered());
    QCOMPARE(fixture.stateSaves(), 0);
    QVERIFY(fixture.passQuitWaitBound());
    QTRY_VERIFY(fixture.quitAnywayOffered());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorQuitWaitKeepWaitingButton")));

    // Once the save is done, quitting goes on.
    fixture.saves->release.release();
    QTRY_VERIFY(!fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 1);
    QTRY_VERIFY(!fixture.quitWaitShown());
    QVERIFY(!fixture.editor.dirty());
}

void TagEditorSmokeTest::repeatedQuitsDuringATagSaveShowOneWait() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const auto restoreMainWindow = useAsMainWindow(fixture.window);
    QVERIFY(fixture.startStalledSave());
    QObject *saveFailed = fixture.window->findChild<QObject *>(QStringLiteral("quitSaveFailedDialog"));
    QVERIFY(saveFailed != nullptr);

    // MPRIS Quit takes the same bounded wait.
    callMprisQuit();
    QTRY_VERIFY(fixture.quitWaitShown());
    QVERIFY(fixture.passQuitWaitBound());
    QTRY_VERIFY(fixture.quitAnywayOffered());

    fixture.window->close();
    callMprisQuit();
    QCoreApplication::processEvents();

    // Still the one wait, still offering to quit anyway.
    QCOMPARE(fixture.window->findChildren<QObject *>(QStringLiteral("tagEditorQuitWaitDialog")).size(), 1);
    QVERIFY(fixture.quitWaitShown());
    QVERIFY(fixture.quitAnywayOffered());
    QVERIFY(!fixture.confirmShown());
    QVERIFY(!saveFailed->property("visible").toBool());
    QCOMPARE(fixture.stateSaves(), 0);

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorQuitWaitQuitButton")));
    QTRY_VERIFY(!fixture.window->isVisible());
    QCOMPARE(fixture.stateSaves(), 1);
    fixture.saves->release.release();
}

void TagEditorSmokeTest::tagEditorReloadAsksBeforeDroppingEdits() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    QObject *save = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorCloseConfirmSaveButton"));
    QVERIFY(save != nullptr);

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QTRY_VERIFY(fixture.confirmShown());
    QCOMPARE(fixture.confirmMessage(),
             QStringLiteral("Discard unsaved changes and reload the tags from the files?"));
    QVERIFY(!save->property("visible").toBool());

    // Cancel keeps the edit.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmKeepEditingButton")));
    QTRY_VERIFY(!fixture.confirmShown());
    QVERIFY(!fixture.editor.loading());
    QVERIFY(fixture.editor.dirty());
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("Edited"));

    // Discard and Reload reads the files again; the editor stays open.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QTRY_VERIFY(fixture.confirmShown());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmDiscardButton")));
    QTRY_VERIFY(!fixture.editor.loading());
    QTRY_COMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("Song 0"));
    QVERIFY(!fixture.editor.dirty());
    QVERIFY(fixture.editorShown());
    QTRY_VERIFY(!fixture.confirmShown());

    // Without edits, Reload does not ask.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QVERIFY(fixture.editor.loading());
    QVERIFY(!fixture.confirmShown());
    QTRY_VERIFY(!fixture.editor.loading());
}

void TagEditorSmokeTest::aSecondClickAsAConfirmationFadesAnswersNothingMore() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    // The window stays open after the settings and queue are saved, so the
    // clicks stay in it.
    QVERIFY(fixture.bridge->setProperty("holdStateSaveAnswers", true));
    QSignalSpy discards(fixture.confirm, SIGNAL(discardRequested()));
    QSignalSpy kept(fixture.confirm, SIGNAL(keepEditingRequested()));
    QVERIFY(discards.isValid() && kept.isValid());

    // A save that fails closes the question, and Save, clicked again as it
    // fades out, does not save again.
    fixture.saves->setResponse(kFailedTagSave);
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QQuickItem *save = visibleItemNamed(fixture.window, QStringLiteral("tagEditorCloseConfirmSaveButton"));
    QVERIFY(save != nullptr && save->isEnabled());
    const QPoint saveAt = centerInWindow(save);
    QTest::mouseClick(fixture.window, Qt::LeftButton, Qt::NoModifier, saveAt);
    QVERIFY(QTest::qWaitFor([&fixture]() {
        return !fixture.editor.saving() && popupFadingOut(fixture.confirm);
    }));
    QTest::mouseClick(fixture.window, Qt::LeftButton, Qt::NoModifier, saveAt);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(fixture.saves->saveCount(), 1);
    QVERIFY(fixture.editor.dirty());
    QVERIFY(fixture.editorShown());
    QVERIFY(finishIncubation(fixture.engine));
    QCOMPARE(takeCapturedMessagesText(), QString());

    // Quitting: Discard, clicked twice, answers once, and Escape as the
    // question fades out does not take the quit back.
    fixture.window->close();
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(clickTwiceAsItCloses(fixture.window, QStringLiteral("tagEditorCloseConfirmDiscardButton"),
                                 fixture.confirm));
    QVERIFY(popupFadingOut(fixture.confirm));
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(discards.count(), 1);
    QCOMPARE(kept.count(), 0);
    QCOMPARE(fixture.stateSaves(), 1);
    QVERIFY(!fixture.editor.isOpen());
}

void TagEditorSmokeTest::discardAndReloadClickedTwiceAsItFadesReloadsOnce() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    QSignalSpy discards(fixture.confirm, SIGNAL(discardRequested()));
    QVERIFY(discards.isValid());

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(clickTwiceAsItCloses(fixture.window, QStringLiteral("tagEditorCloseConfirmDiscardButton"),
                                 fixture.confirm));
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(discards.count(), 1);
    QTRY_VERIFY(!fixture.editor.loading());
    QVERIFY(!fixture.editor.dirty());
    QVERIFY(fixture.editorShown());
}

void TagEditorSmokeTest::quitAnywayTakesNoSecondAnswerAsTheWaitFades() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(fixture.bridge->setProperty("holdStateSaveAnswers", true));
    QVERIFY(fixture.startStalledSave());
    fixture.window->close();
    QTRY_VERIFY(fixture.quitWaitShown());
    QVERIFY(fixture.passQuitWaitBound());
    QTRY_VERIFY(fixture.quitAnywayOffered() && fixture.quitWait()->property("opened").toBool());
    QSignalSpy quits(fixture.quitWait(), SIGNAL(quitAnywayRequested()));
    QSignalSpy stops(fixture.quitWait(), SIGNAL(stopQuittingRequested()));
    QVERIFY(quits.isValid() && stops.isValid());

    // Quit Anyway, clicked again quickly as the wait fades out, quits once,
    // and Escape meanwhile does not stop the quit.
    QVERIFY(clickTwiceAsItCloses(fixture.window, QStringLiteral("tagEditorQuitWaitQuitButton"),
                                 fixture.quitWait()));
    QVERIFY(popupFadingOut(fixture.quitWait()));
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(quits.count(), 1);
    QCOMPARE(stops.count(), 0);
    QCOMPARE(fixture.stateSaves(), 1);
    fixture.saves->release.release();
}

void TagEditorSmokeTest::askingToCloseAfterAReloadQuestionOffersSaveAndDiscard_data() {
    QTest::addColumn<bool>("quit");
    QTest::addColumn<bool>("save");
    QTest::newRow("close, save") << false << true;
    QTest::newRow("close, discard") << false << false;
    QTest::newRow("quit, save") << true << true;
    QTest::newRow("quit, discard") << true << false;
}

// The reload question makes Discard the default button; the close and quit
// questions that follow it still have Save and Discard (#559).
void TagEditorSmokeTest::askingToCloseAfterAReloadQuestionOffersSaveAndDiscard() {
    QFETCH(bool, quit);
    QFETCH(bool, save);
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    const QString saveName = QStringLiteral("tagEditorCloseConfirmSaveButton");
    const QString discardName = QStringLiteral("tagEditorCloseConfirmDiscardButton");

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(fixture.confirmButtonShown(discardName) != nullptr);
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCloseConfirmKeepEditingButton")));
    QTRY_VERIFY(!fixture.confirmShown());
    fixture.settleDeletions();

    if (quit) {
        fixture.window->close();
    } else {
        QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    }
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QCOMPARE(fixture.confirmMessage(), quit ? QStringLiteral("Save tag changes before quitting Ferrous?")
                                            : QStringLiteral("Save changes before closing the tag editor?"));
    fixture.settleDeletions();
    QQuickItem *saveButton = fixture.confirmButtonShown(saveName);
    QQuickItem *discardButton = fixture.confirmButtonShown(discardName);
    QVERIFY(saveButton != nullptr);
    QVERIFY(discardButton != nullptr);
    QVERIFY(fixture.confirmButtonShown(QStringLiteral("tagEditorCloseConfirmKeepEditingButton")) != nullptr);
    QVERIFY(saveButton->isEnabled());
    QVERIFY(discardButton->isEnabled());
    QVERIFY(isDefaultButton(saveButton));
    QVERIFY(!isDefaultButton(discardButton));

    QVERIFY(clickButton(fixture.window, save ? saveName : discardName));
    if (quit) {
        QTRY_VERIFY(!fixture.window->isVisible());
        QCOMPARE(fixture.stateSaves(), 1);
    } else {
        QTRY_VERIFY(!fixture.editorShown());
    }
    QVERIFY(!fixture.editor.isOpen());
    QCOMPARE(fixture.saves->saveCount(), save ? 1 : 0);
    if (save) {
        const QJsonArray rows = QJsonDocument::fromJson(fixture.saves->payload())
                                    .object().value(QStringLiteral("rows")).toArray();
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toObject().value(QStringLiteral("title")).toString(), QStringLiteral("Edited"));
    }
}

// Moving the default button between Save and Discard destroys neither, and
// Enter presses the one that is the default now (#559).
void TagEditorSmokeTest::switchingTheTagEditorQuestionsKeepsEveryButton() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const QString saveName = QStringLiteral("tagEditorCloseConfirmSaveButton");
    const QString discardName = QStringLiteral("tagEditorCloseConfirmDiscardButton");
    const QString keepEditingName = QStringLiteral("tagEditorCloseConfirmKeepEditingButton");
    const QPointer<QQuickItem> save = fixture.window->findChild<QQuickItem *>(saveName);
    const QPointer<QQuickItem> discard = fixture.window->findChild<QQuickItem *>(discardName);
    const QPointer<QQuickItem> keepEditing = fixture.window->findChild<QQuickItem *>(keepEditingName);
    QVERIFY(save && discard && keepEditing);

    for (int round = 0; round < 3; ++round) {
        QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
        QTRY_VERIFY(fixture.confirm->property("opened").toBool());
        fixture.settleDeletions();
        QVERIFY(save && discard && keepEditing);
        QCOMPARE(fixture.confirmButtonShown(discardName), discard.data());
        QCOMPARE(fixture.confirmButtonShown(keepEditingName), keepEditing.data());
        QVERIFY(!save->isVisible());
        QVERIFY(isDefaultButton(discard));
        QVERIFY(!isDefaultButton(save));
        QVERIFY(clickButton(fixture.window, keepEditingName));
        QTRY_VERIFY(!fixture.confirmShown());
        fixture.settleDeletions();
        QVERIFY(save && discard && keepEditing);

        QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
        QTRY_VERIFY(fixture.confirm->property("opened").toBool());
        fixture.settleDeletions();
        QVERIFY(save && discard && keepEditing);
        QCOMPARE(fixture.confirmButtonShown(saveName), save.data());
        QCOMPARE(fixture.confirmButtonShown(discardName), discard.data());
        QCOMPARE(fixture.confirmButtonShown(keepEditingName), keepEditing.data());
        QVERIFY(isDefaultButton(save));
        QVERIFY(!isDefaultButton(discard));
        QVERIFY(clickButton(fixture.window, keepEditingName));
        QTRY_VERIFY(!fixture.confirmShown());
        fixture.settleDeletions();
        QVERIFY(save && discard && keepEditing);
    }
    QVERIFY(fixture.editor.dirty());
    QCOMPARE(fixture.saves->saveCount(), 0);

    // Enter in the question presses Discard and Reload on the reload
    // question...
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(fixture.focusConfirmContent());
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QTRY_COMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("Song 0"));
    QTRY_VERIFY(!fixture.editor.loading());
    QVERIFY(!fixture.editor.dirty());
    QTRY_VERIFY(!fixture.confirmShown());
    QVERIFY(fixture.editorShown());
    QCOMPARE(fixture.saves->saveCount(), 0);

    // ...and Save on the close question.
    fixture.editor.setCell(0, QStringLiteral("title"), QStringLiteral("Edited again"));
    QVERIFY(fixture.editor.dirty());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(fixture.focusConfirmContent());
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QTRY_COMPARE(fixture.saves->saveCount(), 1);
    QTRY_VERIFY(!fixture.editorShown());
    QVERIFY(!fixture.editor.isOpen());
    const QJsonArray rows = QJsonDocument::fromJson(fixture.saves->payload())
                                .object().value(QStringLiteral("rows")).toArray();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first().toObject().value(QStringLiteral("title")).toString(), QStringLiteral("Edited again"));
    fixture.settleDeletions();
    QVERIFY(save && discard && keepEditing);
}

// Enter answers the tag editor's question as soon as it shows, before the
// keyboard has moved into it, and only once; Escape and Tab still work
// (#562).
void TagEditorSmokeTest::enterAnswersTheTagEditorsQuestionAsSoonAsItShows() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    QSignalSpy saves(fixture.confirm, SIGNAL(saveRequested()));
    QSignalSpy discards(fixture.confirm, SIGNAL(discardRequested()));
    QSignalSpy kept(fixture.confirm, SIGNAL(keepEditingRequested()));
    QVERIFY(saves.isValid() && discards.isValid() && kept.isValid());

    // Escape, at once, keeps editing.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QVERIFY(fixture.confirmShown());
    QVERIFY(keyboardInPopupContent(fixture.confirm));
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!fixture.confirmShown());
    QCOMPARE(kept.count(), 1);
    QVERIFY(fixture.editorShown());
    QVERIFY(fixture.editor.dirty());

    // Tab goes on from the content through the dialog to its buttons.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(tabToPopupFooter(fixture.window, fixture.confirm));
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(kept.count(), 2);

    // The reload question, asked next, has the keyboard in its content, not
    // on the button Tab left it on. Enter, as it fades in, discards and
    // reloads once, and Enter again, as it fades out, does nothing more.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QVERIFY(fixture.confirmShown() && !fixture.confirm->property("opened").toBool());
    QVERIFY(keyboardInPopupContent(fixture.confirm));
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QCOMPARE(discards.count(), 1);
    QVERIFY(popupFadingOut(fixture.confirm));
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(discards.count(), 1);
    QTRY_VERIFY(!fixture.editor.loading());
    QVERIFY(!fixture.editor.dirty());
    QVERIFY(fixture.editorShown());

    // The close question: Enter, at once, saves once and closes the editor.
    fixture.editor.setCell(0, QStringLiteral("title"), QStringLiteral("Edited again"));
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QVERIFY(fixture.confirmShown());
    QVERIFY(keyboardInPopupContent(fixture.confirm));
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QTRY_VERIFY(!fixture.editorShown());
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(saves.count(), 1);
    QCOMPARE(fixture.saves->saveCount(), 1);
    QVERIFY(!fixture.editor.isOpen());
    QCOMPARE(discards.count(), 1);
    QCOMPARE(kept.count(), 2);
}

// The quit wait takes Enter for Keep Waiting once it offers the choice,
// with the keyboard where it was when the wait showed (#562).
void TagEditorSmokeTest::enterKeepsWaitingOnceTheQuitWaitOffersIt() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));
    const auto restoreQuit = keepRunningWhenTheWindowCloses();
    QVERIFY(fixture.bridge->setProperty("holdStateSaveAnswers", true));
    QVERIFY(fixture.startStalledSave());
    fixture.window->close();
    QTRY_VERIFY(fixture.quitWaitShown());
    QObject *wait = fixture.quitWait();
    QSignalSpy keepWaiting(wait, SIGNAL(keepWaitingRequested()));
    QSignalSpy quits(wait, SIGNAL(quitAnywayRequested()));
    QSignalSpy stops(wait, SIGNAL(stopQuittingRequested()));
    QVERIFY(keepWaiting.isValid() && quits.isValid() && stops.isValid());
    QVERIFY(keyboardInPopupContent(wait));

    // Before there is a choice, Enter does nothing.
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QCoreApplication::processEvents();
    QVERIFY(fixture.quitWaitShown());
    QCOMPARE(keepWaiting.count(), 0);

    QVERIFY(fixture.passQuitWaitBound());
    QTRY_VERIFY(fixture.quitAnywayOffered());
    QVERIFY(keyboardInPopupContent(wait));
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QCOMPARE(keepWaiting.count(), 1);
    QVERIFY(!wait->property("offerChoice").toBool());
    QVERIFY(fixture.quitWaitShown());
    QCOMPARE(quits.count(), 0);
    QCOMPARE(stops.count(), 0);
    QCOMPARE(fixture.stateSaves(), 0);

    // The save finishing still quits.
    fixture.saves->release.release();
    QTRY_COMPARE(fixture.stateSaves(), 1);
    QCOMPARE(quits.count(), 0);
    QCOMPARE(stops.count(), 0);
}

// Enter numbers the tracks as soon as Auto Number shows, before the keyboard
// has moved into it, and only once (#562).
void TagEditorSmokeTest::enterNumbersTracksAsSoonAsAutoNumberShows() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(3, false));
    fixture.editor.selectAllRows();
    QObject *dialog = fixture.window->findChild<QObject *>(QStringLiteral("autoNumberDialog"));
    QVERIFY(dialog != nullptr);
    // Out of order, so that numbering shows.
    fixture.editor.setCell(0, QStringLiteral("trackNo"), QStringLiteral("7"));
    fixture.editor.setCell(2, QStringLiteral("trackNo"), QStringLiteral("8"));

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorAutoNumberButton")));
    QVERIFY(dialog->property("visible").toBool() && !dialog->property("opened").toBool());
    QVERIFY(keyboardInPopupContent(dialog));
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("1"));
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(2, QStringLiteral("trackNo")), QStringLiteral("3"));
    // Enter again, as the dialog fades out, numbers nothing more.
    QVERIFY(popupFadingOut(dialog));
    fixture.editor.setCell(0, QStringLiteral("trackNo"), QStringLiteral("7"));
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!dialog->property("visible").toBool());
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("7"));
    QCOMPARE(fixture.saves->saveCount(), 0);

    // Escape, at once, numbers nothing.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorAutoNumberButton")));
    QVERIFY(dialog->property("visible").toBool());
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!dialog->property("visible").toBool());
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("7"));
}

void TagEditorSmokeTest::autoNumberRejectsStartsThatAreNotNumbers() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(3, false));
    fixture.editor.selectAllRows();
    QObject *dialog = fixture.window->findChild<QObject *>(QStringLiteral("autoNumberDialog"));
    QObject *field = fixture.window->findChild<QObject *>(QStringLiteral("autoNumberStartingTrackField"));
    QObject *error = fixture.window->findChild<QObject *>(QStringLiteral("autoNumberStartingTrackError"));
    QObject *apply = fixture.window->findChild<QObject *>(QStringLiteral("autoNumberApplyButton"));
    QVERIFY(dialog != nullptr);
    QVERIFY(field != nullptr);
    QVERIFY(error != nullptr);
    QVERIFY(apply != nullptr);
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorAutoNumberButton")));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(!error->property("visible").toBool());

    for (const QString &text : {QStringLiteral("abc"), QString(), QStringLiteral("0")}) {
        QVERIFY(field->setProperty("text", text));
        QVERIFY2(error->property("visible").toBool(), qPrintable(text));
        QVERIFY2(!apply->property("enabled").toBool(), qPrintable(text));
        QVariant applied;
        QVERIFY(QMetaObject::invokeMethod(dialog, "apply", Q_RETURN_ARG(QVariant, applied)));
        QCOMPARE(applied, QVariant(false));
    }
    QVERIFY(dialog->property("visible").toBool());
    QVERIFY(!fixture.editor.dirty());

    // Typed letters do not get in at all.
    QVERIFY(field->setProperty("text", QString()));
    QMetaObject::invokeMethod(field, "forceActiveFocus");
    QTest::keyClick(fixture.window, 'x');
    QTest::keyClick(fixture.window, '3');
    QCOMPARE(field->property("text").toString(), QStringLiteral("3"));
    QVERIFY(!error->property("visible").toBool());
    QVERIFY(apply->property("enabled").toBool());

    QVERIFY(clickButton(fixture.window, QStringLiteral("autoNumberApplyButton")));
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("3"));
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(2, QStringLiteral("trackNo")), QStringLiteral("5"));
}

void TagEditorSmokeTest::tagEditorRowsFollowTheKeyboard() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(5, false));
    QObject *table = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorTable"));
    QVERIFY(table != nullptr);
    QTRY_VERIFY(table->property("activeFocus").toBool());
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{0});

    QTest::keyClick(fixture.window, Qt::Key_Down);
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{1});
    QTest::keyClick(fixture.window, Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(fixture.window, Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(selectedTagEditorRows(fixture.editor), (QList<int>{1, 2, 3}));
    QTest::keyClick(fixture.window, Qt::Key_Space);
    QCOMPARE(selectedTagEditorRows(fixture.editor), (QList<int>{1, 2}));
    QTest::keyClick(fixture.window, Qt::Key_End);
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{4});
    QTest::keyClick(fixture.window, Qt::Key_Home, Qt::ShiftModifier);
    QCOMPARE(selectedTagEditorRows(fixture.editor), (QList<int>{0, 1, 2, 3, 4}));
    QTest::keyClick(fixture.window, Qt::Key_Up);
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{0});

    // Rows draw their selection from the model.
    QQuickItem *second = tagEditorRowItem(fixture.window, 1);
    QVERIFY(second != nullptr);
    QVERIFY(!second->property("selected").toBool());
    QTest::keyClick(fixture.window, Qt::Key_Down);
    QVERIFY(second->property("selected").toBool());
}

void TagEditorSmokeTest::tagEditorShowsEveryValueOfMultiValueFields() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, false, {QStringLiteral("B"), QStringLiteral("C")}));
    // The field editors are delegates, which only the item tree holds.
    QObject *note = findQuickItemByObjectName(
        fixture.window->contentItem(), QStringLiteral("tagEditorExtraValuesNote_artist"));
    QVERIFY(note != nullptr);
    QQuickItem *first = nullptr;
    QTRY_VERIFY((first = tagEditorRowItem(fixture.window, 0)) != nullptr);

    QVERIFY(tagEditorCellTexts(first).contains(QStringLiteral("Artist; B; C")));
    QTRY_VERIFY(note->property("visible").toBool());
    QCOMPARE(note->property("text").toString(),
             QStringLiteral("Also: B; C. Editing replaces every value with the one above."));

    // An edit replaces them all, so they are no longer shown.
    fixture.editor.applyBulkFieldToRows({0}, QStringLiteral("artist"), QStringLiteral("D"));
    QTRY_VERIFY(!note->property("visible").toBool());
    QVERIFY(tagEditorCellTexts(first).contains(QStringLiteral("D")));
}

void TagEditorSmokeTest::tagEditorLibrarySupportGateMatchesSupportedRows() {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));

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

void TagEditorSmokeTest::renameFilesShowsTheNewNamesAndCancelRenamesNothing() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, false));
    std::atomic<int> renames{0};
    const QByteArray loadResponse = tagEditorLoadResponse(2);
    const QByteArray preview(R"({"results":[)"
        R"({"path":"/fixture/Album 000/01 Song.flac","newPath":"/fixture/Album 000/01 Intro.flac","ok":true},)"
        R"({"path":"/fixture/Album 000/02 Song.flac","newPath":"/fixture/Album 000/01 Intro.flac",)"
        R"("ok":false,"error":"target conflicts with another selected file"}],"successfulPaths":[]})");
    fixture.editor.m_executeOperation = [&](TagEditorController::Operation operation,
                                            const QByteArray &,
                                            const TagEditorController::RenameGate &) {
        switch (operation) {
        case TagEditorController::Operation::PreviewRename:
            return preview;
        case TagEditorController::Operation::Rename:
            ++renames;
            return QByteArray(R"({"results":[],"successfulPaths":[]})");
        default:
            return loadResponse;
        }
    };
    fixture.editor.selectAllRows();
    QObject *previewDialog = fixture.window->findChild<QObject *>(QStringLiteral("renamePreviewDialog"));
    QVERIFY(previewDialog != nullptr);

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    QTRY_VERIFY(previewDialog->property("opened").toBool());
    QCOMPARE(renames.load(), 0);
    auto *summary = fixture.window->findChild<QQuickItem *>(QStringLiteral("renamePreviewSummary"));
    QVERIFY(summary != nullptr);
    QCOMPARE(summary->property("text").toString(),
             QStringLiteral("1 file will be renamed. 1 file can't be renamed and keeps its name."));
    const QVariantList rows = previewDialog->property("rows").toList();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].toMap().value(QStringLiteral("oldName")).toString(), QStringLiteral("01 Song.flac"));
    QCOMPARE(rows[0].toMap().value(QStringLiteral("newName")).toString(), QStringLiteral("01 Intro.flac"));
    QVERIFY(!rows[1].toMap().value(QStringLiteral("ok")).toBool());
    QVERIFY(rows[1].toMap().value(QStringLiteral("error")).toString().contains(QStringLiteral("conflicts")));
    QVERIFY(visibleItemNamed(fixture.window, QStringLiteral("renamePreviewList")) != nullptr);

    QVERIFY(clickVisibleButton(fixture.window, QStringLiteral("renamePreviewCancelButton")));
    QTRY_VERIFY(!previewDialog->property("visible").toBool());
    QTest::qWait(50);
    QCOMPARE(renames.load(), 0);
    QVERIFY(!fixture.editor.saving());

    // Rename goes on to rename the files, which needs the bridge this
    // fixture lacks; BridgeClientTest covers the rename itself.
    QSignalSpy renameRequested(previewDialog, SIGNAL(renameRequested()));
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    QTRY_VERIFY(previewDialog->property("opened").toBool());
    QVERIFY(clickVisibleButton(fixture.window, QStringLiteral("renamePreviewRenameButton")));
    QCOMPARE(renameRequested.count(), 1);
    QTRY_VERIFY(!previewDialog->property("visible").toBool());
}

// Enter renames as soon as the preview shows, before the keyboard has moved
// into it, and only once (#562).
void TagEditorSmokeTest::enterRenamesAsSoonAsThePreviewShows() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, false));
    const QByteArray loadResponse = tagEditorLoadResponse(2);
    const QByteArray preview(R"({"results":[{"path":"/fixture/Album 000/01 Song.flac",)"
        R"("newPath":"/fixture/Album 000/01 Intro.flac","ok":true}],"successfulPaths":[]})");
    fixture.editor.m_executeOperation = [=](TagEditorController::Operation operation, const QByteArray &,
                                            const TagEditorController::RenameGate &) {
        if (operation == TagEditorController::Operation::PreviewRename) {
            return preview;
        }
        if (operation == TagEditorController::Operation::Rename) {
            return QByteArray(R"({"results":[],"successfulPaths":[]})");
        }
        return loadResponse;
    };
    fixture.editor.selectAllRows();
    QObject *previewDialog = fixture.window->findChild<QObject *>(QStringLiteral("renamePreviewDialog"));
    QVERIFY(previewDialog != nullptr);
    QSignalSpy renameRequested(previewDialog, SIGNAL(renameRequested()));
    QVERIFY(renameRequested.isValid());

    // Escape, at once, renames nothing.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    QTRY_VERIFY(previewDialog->property("visible").toBool());
    QVERIFY(keyboardInPopupContent(previewDialog));
    QTest::keyClick(fixture.window, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!previewDialog->property("visible").toBool());
    QCOMPARE(renameRequested.count(), 0);

    // Renaming needs the bridge this fixture lacks; BridgeClientTest covers
    // the rename itself.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    QTRY_VERIFY(previewDialog->property("visible").toBool());
    QVERIFY(keyboardInPopupContent(previewDialog));
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QCOMPARE(renameRequested.count(), 1);
    // Enter again, as the preview fades out, renames nothing more.
    QVERIFY(popupFadingOut(previewDialog));
    QTest::keyClick(fixture.window, Qt::Key_Return);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(renameRequested.count(), 1);
    QTRY_VERIFY(!fixture.editor.saving());
}

void TagEditorSmokeTest::renameRenamesThePreviewedFilesAfterTheSelectionChanged() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, false));
    // Renames need a bridge, whose rename gate the fake backend ignores.
    BridgeClient bridge;
    fixture.editor.m_bridge = &bridge;
    const QByteArray loadResponse = tagEditorLoadResponse(2);
    const QString first = QStringLiteral("/fixture/Album 000/01 Song.flac");
    const QString second = QStringLiteral("/fixture/Album 000/02 Song.flac");
    const QByteArray preview(R"({"results":[{"path":"/fixture/Album 000/01 Song.flac",)"
        R"("newPath":"/fixture/Album 000/01 Intro.flac","ok":true}],"successfulPaths":[]})");
    auto release = std::make_shared<QSemaphore>();
    auto renamed = std::make_shared<QList<QByteArray>>();
    auto renamedMutex = std::make_shared<QMutex>();
    fixture.editor.m_executeOperation = [=](TagEditorController::Operation operation,
                                            const QByteArray &payload,
                                            const TagEditorController::RenameGate &) {
        switch (operation) {
        case TagEditorController::Operation::PreviewRename:
            // A large batch: the check takes a while.
            release->tryAcquire(1, 10000);
            return preview;
        case TagEditorController::Operation::Rename: {
            QMutexLocker locker(renamedMutex.get());
            renamed->push_back(payload);
            return QByteArray(R"({"results":[],"successfulPaths":[]})");
        }
        default:
            return loadResponse;
        }
    };
    const auto renames = [=]() {
        QMutexLocker locker(renamedMutex.get());
        return *renamed;
    };
    QObject *previewDialog = fixture.window->findChild<QObject *>(QStringLiteral("renamePreviewDialog"));
    QVERIFY(previewDialog != nullptr);

    // The preview of the first file runs while the user selects the second.
    fixture.editor.setSelectedRows({0});
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    fixture.editor.setSelectedRows({1});
    release->release();
    QTRY_VERIFY(previewDialog->property("opened").toBool());
    QVERIFY(clickVisibleButton(fixture.window, QStringLiteral("renamePreviewRenameButton")));

    // The rename is the one previewed: the first file, never the second.
    QTRY_COMPARE(renames().size(), 1);
    const QJsonArray rows = QJsonDocument::fromJson(renames().first())
        .object().value(QStringLiteral("rows")).toArray();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].toObject().value(QStringLiteral("path")).toString(), first);
    QVERIFY(!renames().first().contains(second.toUtf8()));
    QTRY_VERIFY(!fixture.editor.saving());
    fixture.editor.m_bridge = nullptr;
}

std::unique_ptr<QmlSmokeTest> makeTagEditorSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<TagEditorSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_tag_editor.moc"
