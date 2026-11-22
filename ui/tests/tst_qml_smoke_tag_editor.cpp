// SPDX-License-Identifier: GPL-3.0-or-later

#include "FlacFixture.h"
#include "QmlSmokeTest.h"
#include "TestColor.h"

#include <QAccessible>
#include <QJSValue>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <algorithm>
#include <functional>

// The tag editor, renaming, and quitting with tag edits.
class TagEditorSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void tagEditorOpensALargeSelectionWithoutBuildingEveryRow();
    void tagEditorCancelAsksBeforeDroppingEdits();
    void typedTextCanBeSavedBeforeTheFieldIsLeft();
    void typedTextMarksItsChangeAtOnce();
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
    void autoNumberTakesTypingAsSoonAsItShows();
    void tagEditorRowsFollowTheKeyboard();
    void tagEditorCurrentRowFollowsTheSelection();
    void tagEditorShowsEveryValueOfMultiValueFields();
    void tagEditorLibrarySupportGateMatchesSupportedRows();
    void renameFilesShowsTheNewNamesAndCancelRenamesNothing();
    void renameRenamesThePreviewedFilesAfterTheSelectionChanged();
    void saveDetailsReadAsAReport_data();
    void saveDetailsReadAsAReport();
    void renamePreviewLaysOutItsRows_data();
    void renamePreviewLaysOutItsRows();
    void renamePreviewALongNameThatStaysCutsNoNewName_data();
    void renamePreviewALongNameThatStaysCutsNoNewName();
    void renamePreviewShowsANameThatStaysWhole_data();
    void renamePreviewShowsANameThatStaysWhole();
    void saveDetailsSetFailedFilesApart();
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
    return loadMainWindowShown(engine, libraryModel);
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

    // The editor's window: Edit Tags has a window of its own, and so has
    // each of its questions, above it.
    QQuickWindow *tags() const { return qobject_cast<QQuickWindow *>(dialog); }
    QQuickWindow *confirmWindow() const { return qobject_cast<QQuickWindow *>(confirm); }
    QQuickWindow *quitWaitWindow() const { return qobject_cast<QQuickWindow *>(quitWait()); }
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
        auto *content = qobject_cast<QQuickItem *>(qvariant_cast<QObject *>(confirm->property("body")));
        if (content == nullptr || !QTest::qWaitForWindowActive(confirmWindow())) {
            return false;
        }
        content->forceActiveFocus();
        return QTest::qWaitFor([content]() { return content->hasActiveFocus(); });
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

    // Edit Tags has a window of its own: the time of its first frame once
    // the rows have loaded, each noted as it happens, so that a frame drawn
    // before a wait sees the load end still counts.
    QQuickWindow *tags = qobject_cast<QQuickWindow *>(dialog);
    QVERIFY(tags != nullptr);
    QElapsedTimer timer;
    qint64 loadedMs = -1;
    qint64 firstFrameMs = -1;
    const QMetaObject::Connection loadEnd = QObject::connect(
        &editor, &TagEditorController::loadingChanged, &editor, [&]() {
            if (!editor.loading() && loadedMs < 0) {
                loadedMs = timer.elapsed();
            }
        });
    const auto stopNoting = qScopeGuard([&] { QObject::disconnect(loadEnd); });
    const FrameSampler frames(tags, [&]() {
        if (loadedMs >= 0 && firstFrameMs < 0) {
            firstFrameMs = timer.elapsed();
        }
    });
    timer.start();
    QVERIFY(openTagEditor(editor));
    QTRY_VERIFY(loadedMs >= 0);
    QTRY_VERIFY(firstFrameMs >= 0);
    const int rowItems = tagEditorRowItemCount(tags);
    qInfo("tag editor: %d rows loaded in %lld ms, first frame after %lld ms, %d row items",
          kRows, static_cast<long long>(loadedMs), static_cast<long long>(firstFrameMs), rowItems);
    QVERIFY(dialog->property("visible").toBool());
    QCOMPARE(editor.loadedPaths().size(), kRows);
    QCOMPARE(selectedTagEditorRows(editor), QList<int>{0});

    // Only the rows in view have items. Building all 2,000 took seconds.
    QVERIFY2(rowItems > 0 && rowItems < 100, qPrintable(QString::number(rowItems)));
    QVERIFY2(firstFrameMs < 4000, qPrintable(QString::number(firstFrameMs)));

    // Selecting every row and scrolling to the end keeps it that way.
    QTest::keyClick(tags, Qt::Key_A, Qt::ControlModifier);
    QTRY_COMPARE(editor.selectedCount(), kRows);
    QTest::keyClick(tags, Qt::Key_End, Qt::ShiftModifier);
    QTRY_VERIFY(tagEditorRowItem(tags, kRows - 1) != nullptr);
    QVERIFY(tagEditorRowItem(tags, kRows - 1)->property("selected").toBool());
    QVERIFY(tagEditorRowItemCount(tags) < 100);
}

void TagEditorSmokeTest::tagEditorCancelAsksBeforeDroppingEdits() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, true));

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));

    // Cancel asks, as the close button and Escape do.
    QTRY_VERIFY(fixture.confirmShown());
    QCOMPARE(fixture.confirmMessage(), QStringLiteral("Save changes before closing the tag editor?"));
    // The question leads, semibold, as every dialog's first sentence is.
    QCOMPARE(fixture.window->findChild<QObject *>(QStringLiteral("tagEditorCloseConfirmMessage"))
                 ->property("font").value<QFont>().weight(),
             QFont::DemiBold);
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
    // The question fades out after the editor's window has hidden.
    QTRY_VERIFY(!fixture.confirmShown());

    // Without edits, Cancel closes at once.
    QVERIFY(openTagEditor(fixture.editor));
    QTRY_VERIFY(!fixture.editor.loading() && fixture.dialog->property("opened").toBool());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(!fixture.editorShown());
    QVERIFY(!fixture.confirmShown());
}

// Text typed in a field and not yet applied is an unsaved change: Save
// and Ctrl+S are enabled while it differs from what the field held, and
// save it (they stayed disabled until the field lost focus, and Ctrl+S did
// nothing). Typed back to what it held, there is nothing to save.
void TagEditorSmokeTest::typedTextCanBeSavedBeforeTheFieldIsLeft() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, false));
    auto *title = qobject_cast<QQuickItem *>(findQuickItemByObjectName(
        fixture.tags()->contentItem(), QStringLiteral("tagEditorField_title")));
    auto *save = qobject_cast<QQuickItem *>(findQuickItemByObjectName(
        fixture.tags()->contentItem(), QStringLiteral("tagEditorSaveButton")));
    QVERIFY(title != nullptr && save != nullptr);
    QVERIFY(!save->isEnabled());
    title->forceActiveFocus();
    QTRY_VERIFY(title->hasActiveFocus());
    QTest::keyClick(fixture.tags(), 'x');
    QVERIFY(!fixture.editor.dirty());
    QVERIFY(save->isEnabled());
    QTest::keyClick(fixture.tags(), Qt::Key_Backspace);
    QVERIFY(!save->isEnabled());
    QTest::keyClick(fixture.tags(), 'y');
    QVERIFY(save->isEnabled());
    QVERIFY(title->hasActiveFocus());

    QTest::keyClick(fixture.tags(), Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE(fixture.saves->saveCount(), 1);
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("title")), QStringLiteral("Song 0y"));
    QTRY_VERIFY(!fixture.editor.saving());
}

// Typed text marks its change at once, as it does once applied (Save was
// enabled, but nothing else showed the change until the field
// lost focus): the field's label turns semibold and its Revert shows, the
// column's header turns semibold with its Revert, and the rows the text
// goes to get the modified dot. Typed back to what the field held, the
// marks go; applied, they stay.
void TagEditorSmokeTest::typedTextMarksItsChangeAtOnce() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(3, false));
    fixture.editor.selectRange(0, 1);
    QQuickItem *content = fixture.tags()->contentItem();
    const auto item = [content](const QString &name) {
        return qobject_cast<QQuickItem *>(findQuickItemByObjectName(content, name));
    };
    QQuickItem *title = item(QStringLiteral("tagEditorField_title"));
    QQuickItem *label = item(QStringLiteral("tagEditorLabel_title"));
    QQuickItem *revert = item(QStringLiteral("tagEditorRevert_title"));
    QQuickItem *columnRevert = item(QStringLiteral("tagEditorColumnRevert_title"));
    QVERIFY(title != nullptr && label != nullptr && revert != nullptr && columnRevert != nullptr);
    // The rows' dots, by row.
    const auto marks = [&]() {
        QList<bool> shown(3, false);
        std::function<void(QQuickItem *)> visit = [&](QQuickItem *node) {
            for (QQuickItem *child : node->childItems()) {
                if (child->objectName() == QStringLiteral("tagEditorRow")) {
                    const int row = child->property("index").toInt();
                    for (QQuickItem *part : child->childItems()) {
                        if (part->objectName() == QStringLiteral("tagEditorRowModifiedMark") && row >= 0 && row < 3) {
                            shown[row] = part->isVisible();
                        }
                    }
                } else {
                    visit(child);
                }
            }
        };
        visit(content);
        return shown;
    };
    const auto marked = [&]() {
        return label->property("font").value<QFont>().weight() == QFont::DemiBold && revert->isVisible()
            && columnRevert->isVisible();
    };
    const auto unmarked = [&]() {
        return label->property("font").value<QFont>().weight() == QFont::Normal && !revert->isVisible()
            && !columnRevert->isVisible();
    };
    QTRY_VERIFY(unmarked());
    QCOMPARE(marks(), (QList<bool>{false, false, false}));

    title->forceActiveFocus();
    QTRY_VERIFY(title->hasActiveFocus());
    QTest::keyClick(fixture.tags(), 'x');
    QVERIFY(!fixture.editor.dirty());
    QTRY_VERIFY(marked());
    QTRY_COMPARE(marks(), (QList<bool>{true, true, false}));

    // Typed back: nothing changes.
    QTest::keyClick(fixture.tags(), Qt::Key_Backspace);
    QTRY_VERIFY(unmarked());
    QCOMPARE(marks(), (QList<bool>{false, false, false}));

    // Applied, the marks stay, now from the edits.
    QTest::keyClick(fixture.tags(), 'y');
    QTRY_VERIFY(marked());
    QTest::keyClick(fixture.tags(), Qt::Key_Return);
    QTRY_VERIFY(fixture.editor.dirty());
    // The two titles differed, so the field held none: both are "y" now.
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(1, QStringLiteral("title")), QStringLiteral("y"));
    QVERIFY(marked());
    QCOMPARE(marks(), (QList<bool>{true, true, false}));
}

void TagEditorSmokeTest::closingTheTagEditorKeepsTheFieldBeingTyped() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, false));
    auto *title = qobject_cast<QQuickItem *>(findQuickItemByObjectName(
        fixture.tags()->contentItem(), QStringLiteral("tagEditorField_title")));
    QVERIFY(title != nullptr);
    title->forceActiveFocus();
    QTRY_VERIFY(title->hasActiveFocus());
    QTest::keyClick(fixture.tags(), 'x');
    QVERIFY(!fixture.editor.dirty());

    // Typed text counts as an edit once the editor is asked to close.
    QTest::keyClick(fixture.tags(), Qt::Key_Escape);
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
    QVERIFY(cancel != nullptr);

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorSaveButton")));
    QTRY_COMPARE(fixture.saves->saveCount(), 1);
    QVERIFY(fixture.editor.saving());

    // Nothing closes the editor while it saves.
    QVERIFY(!cancel->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(cancel, "clicked"));
    QVERIFY(QMetaObject::invokeMethod(fixture.dialog, "requestClose"));
    QVERIFY(QMetaObject::invokeMethod(fixture.dialog, "closeEditor"));
    QTest::keyClick(fixture.tags(), Qt::Key_Escape);
    // Nor its window's title bar button.
    fixture.tags()->close();
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
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Escape);
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
    // The files come straight after the status that counts them, then why
    // it offers, and last what quitting anyway costs, over its button (the
    // files stood between the two sentences), each short (as one paragraph
    // they were five lines at 24 pt).
    auto *explanation = fixture.window->findChild<QQuickItem *>(QStringLiteral("tagEditorQuitWaitExplanation"));
    QVERIFY(explanation != nullptr);
    const QString consequence = message->property("text").toString();
    const QString why = explanation->property("text").toString();
    QCOMPARE(consequence, QStringLiteral("Quit Anyway stops waiting and may lose changes not yet written."));
    QCOMPARE(why, QStringLiteral("This is taking longer than it should, perhaps on an unresponsive network share. "
                                 "No file is left half-written."));
    QVERIFY(consequence.size() + why.size() < 200);
    // As a confirmation reads: what it waits for semibold, the
    // files under its text rather than the spinner, the explanation in the
    // secondary colour, and Keep Waiting with a glyph, as Quit Anyway.
    QObject *palette = qvariant_cast<QObject *>(fixture.window->property("uiPalette"));
    auto *status = fixture.window->findChild<QQuickItem *>(QStringLiteral("tagEditorQuitWaitStatus"));
    QVERIFY(palette != nullptr && status != nullptr);
    QCOMPARE(status->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(message->property("color").value<QColor>().rgba(), palette->property("textSecondary").value<QColor>().rgba());
    QCOMPARE(files->property("color").value<QColor>().rgba(), palette->property("textSecondary").value<QColor>().rgba());
    QTRY_COMPARE(sceneRectOf(qobject_cast<QQuickItem *>(files)).left(), sceneRectOf(status).left());
    // The explanation on the same left edge, as wide as the status's text
    // (it started under the spinner); the window wider by that indent, so
    // it keeps the lines it had.
    auto *messageItem = qobject_cast<QQuickItem *>(message);
    auto *filesItem = qobject_cast<QQuickItem *>(files);
    QTRY_VERIFY(sceneRectOf(explanation).bottom() <= sceneRectOf(messageItem).top());
    QVERIFY(sceneRectOf(status).bottom() <= sceneRectOf(filesItem).top());
    QVERIFY(sceneRectOf(filesItem).bottom() <= sceneRectOf(explanation).top());
    QTRY_COMPARE(sceneRectOf(explanation).left(), sceneRectOf(status).left());
    QCOMPARE(explanation->property("color").value<QColor>().rgba(),
             palette->property("textSecondary").value<QColor>().rgba());
    QTRY_COMPARE(sceneRectOf(messageItem).left(), sceneRectOf(status).left());
    QCOMPARE(messageItem->width(), status->width());
    QObject *quitWait = fixture.quitWait();
    const qreal indent = quitWait->property("textIndent").toReal();
    QCOMPARE(indent, sceneRectOf(status).left() - sceneRectOf(qobject_cast<QQuickItem *>(files)->parentItem()).left());
    QCOMPARE(quitWait->property("defaultWidth").toReal(),
             std::max(evaluateOn(status, QStringLiteral("Kirigami.Units.gridUnit")).toReal() * 26 + indent,
                      quitWait->property("minimumWidth").toReal()));
    auto *keepWaiting = fixture.window->findChild<QQuickItem *>(QStringLiteral("tagEditorQuitWaitKeepWaitingButton"));
    QVERIFY(keepWaiting != nullptr);
    QCOMPARE(evaluateOn(keepWaiting, QStringLiteral("icon.name")).toString(), QStringLiteral("chronometer"));

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

    // A save that fails closes the question, and Save, clicked again where
    // it was, does not save again.
    fixture.saves->setResponse(kFailedTagSave);
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(fixture.confirmWindow()));
    QQuickItem *save = visibleItemNamed(fixture.confirmWindow(), QStringLiteral("tagEditorCloseConfirmSaveButton"));
    QVERIFY(save != nullptr && save->isEnabled());
    const QPoint saveAt = centerInWindow(save);
    QTest::mouseClick(fixture.confirmWindow(), Qt::LeftButton, Qt::NoModifier, saveAt);
    QVERIFY(QTest::qWaitFor([&fixture]() {
        return !fixture.editor.saving() && dialogClosing(fixture.confirm);
    }));
    QTest::mouseClick(fixture.confirmWindow(), Qt::LeftButton, Qt::NoModifier, saveAt);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(fixture.saves->saveCount(), 1);
    QVERIFY(fixture.editor.dirty());
    QVERIFY(fixture.editorShown());
    QVERIFY(finishIncubation(fixture.engine));
    QCOMPARE(takeCapturedMessagesText(), QString());

    // Quitting: Discard, clicked twice, answers once, and Escape as the
    // question goes does not take the quit back.
    fixture.window->close();
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(fixture.confirmWindow()));
    QVERIFY(clickTwiceAsItCloses(fixture.tags(), QStringLiteral("tagEditorCloseConfirmDiscardButton"),
                                 fixture.confirm));
    QVERIFY(dialogClosing(fixture.confirm));
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Escape);
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
    QVERIFY(QTest::qWaitForWindowExposed(fixture.confirmWindow()));
    QVERIFY(clickTwiceAsItCloses(fixture.tags(), QStringLiteral("tagEditorCloseConfirmDiscardButton"),
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

    // Quit Anyway, clicked again quickly as the wait goes, quits once, and
    // Escape meanwhile does not stop the quit.
    QVERIFY(QTest::qWaitForWindowExposed(fixture.quitWaitWindow()));
    QVERIFY(clickTwiceAsItCloses(fixture.tags(), QStringLiteral("tagEditorQuitWaitQuitButton"),
                                 fixture.quitWait()));
    QVERIFY(dialogClosing(fixture.quitWait()));
    QTest::keyClick(fixture.quitWaitWindow(), Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(quits.count(), 1);
    QCOMPARE(stops.count(), 0);
    QCOMPARE(fixture.stateSaves(), 1);
    fixture.saves->release.release();
}

void TagEditorSmokeTest::askingToCloseAfterAReloadQuestionOffersSaveAndDiscard_data() {
    QTest::addColumn<bool>("quit");
    QTest::addColumn<bool>("save");
    QTest::newRow("close, discard") << false << false;
    QTest::newRow("quit, save") << true << true;
}

// The reload question makes Cancel the default button; the close and quit
// questions that follow it still have Save and Discard (#559), and each
// does what it says. Saving from the close question after a reload
// question: switchingTheTagEditorQuestionsKeepsEveryButton.
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

// Moving the default button between Save and Cancel destroys no button, and
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
        // Discarding drops the edits, so Cancel is the default button.
        QVERIFY(isDefaultButton(keepEditing));
        QVERIFY(!isDefaultButton(discard));
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

    // Enter in the question presses Cancel on the reload question, which
    // keeps the edits...
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(fixture.focusConfirmContent());
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Return);
    QTRY_VERIFY(!fixture.confirmShown());
    QVERIFY(fixture.editor.dirty());
    QVERIFY(!fixture.editor.loading());
    QVERIFY(fixture.editorShown());
    QCOMPARE(fixture.saves->saveCount(), 0);

    // ...and Save on the close question.
    fixture.editor.setCell(0, QStringLiteral("title"), QStringLiteral("Edited again"));
    QVERIFY(fixture.editor.dirty());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(fixture.focusConfirmContent());
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Return);
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
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!fixture.confirmShown());
    QCOMPARE(kept.count(), 1);
    QVERIFY(fixture.editorShown());
    QVERIFY(fixture.editor.dirty());

    // Tab goes on from the content through the dialog to its buttons.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QTRY_VERIFY(fixture.confirm->property("opened").toBool());
    QVERIFY(tabToPopupFooter(fixture.tags(), fixture.confirm));
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(kept.count(), 2);

    // The reload question, asked next, has the keyboard in its content, not
    // on the button Tab left it on. Enter, as it shows, presses its default
    // button, Cancel, once: the edits stay. Enter again, as it goes, does
    // nothing more.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorReloadButton")));
    QVERIFY(fixture.confirmShown());
    QVERIFY(keyboardInPopupContent(fixture.confirm));
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Return);
    QCOMPARE(kept.count(), 3);
    QVERIFY(dialogClosing(fixture.confirm));
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Return);
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(kept.count(), 3);
    QCOMPARE(discards.count(), 0);
    QVERIFY(fixture.editor.dirty());
    QVERIFY(fixture.editorShown());

    // The close question: Enter, at once, saves once and closes the editor.
    fixture.editor.setCell(0, QStringLiteral("title"), QStringLiteral("Edited again"));
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorCancelButton")));
    QVERIFY(fixture.confirmShown());
    QVERIFY(keyboardInPopupContent(fixture.confirm));
    QTest::keyClick(fixture.confirmWindow(), Qt::Key_Return);
    QTRY_VERIFY(!fixture.editorShown());
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(saves.count(), 1);
    QCOMPARE(fixture.saves->saveCount(), 1);
    QVERIFY(!fixture.editor.isOpen());
    QCOMPARE(discards.count(), 0);
    QCOMPARE(kept.count(), 3);
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
    QTest::keyClick(fixture.quitWaitWindow(), Qt::Key_Return);
    QCoreApplication::processEvents();
    QVERIFY(fixture.quitWaitShown());
    QCOMPARE(keepWaiting.count(), 0);

    QVERIFY(fixture.passQuitWaitBound());
    QTRY_VERIFY(fixture.quitAnywayOffered());
    QVERIFY(keyboardInPopupContent(wait));
    QTest::keyClick(fixture.quitWaitWindow(), Qt::Key_Return);
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

    auto *autoNumberWindow = qobject_cast<QQuickWindow *>(dialog);
    QVERIFY(autoNumberWindow != nullptr);
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorAutoNumberButton")));
    QVERIFY(dialog->property("visible").toBool());
    QCOMPARE(autoNumberWindow->transientParent(), fixture.tags());
    // In the first field, whose Enter numbers.
    auto *track = autoNumberWindow->findChild<QQuickItem *>(QStringLiteral("autoNumberStartingTrackField"));
    QVERIFY(track != nullptr);
    QTRY_VERIFY(autoNumberWindow->isActive() && autoNumberWindow->activeFocusItem() == track);
    QTest::keyClick(autoNumberWindow, Qt::Key_Return);
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("1"));
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(2, QStringLiteral("trackNo")), QStringLiteral("3"));
    // Enter again, as the dialog goes, numbers nothing more.
    QVERIFY(dialogClosing(dialog));
    fixture.editor.setCell(0, QStringLiteral("trackNo"), QStringLiteral("7"));
    QTest::keyClick(autoNumberWindow, Qt::Key_Return);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!dialog->property("visible").toBool());
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("7"));
    QCOMPARE(fixture.saves->saveCount(), 0);

    // Escape, at once, numbers nothing.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorAutoNumberButton")));
    QVERIFY(dialog->property("visible").toBool());
    QTest::keyClick(autoNumberWindow, Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!dialog->property("visible").toBool());
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("7"));
}

// Auto Number opens with its first field focused and selected, so typing
// replaces the number at once (the keyboard was on no field), and Enter in
// either field numbers, as it did as the dialog opened.
void TagEditorSmokeTest::autoNumberTakesTypingAsSoonAsItShows() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(3, false));
    fixture.editor.selectAllRows();
    auto *dialog = qobject_cast<QQuickWindow *>(
        fixture.window->findChild<QObject *>(QStringLiteral("autoNumberDialog")));
    QVERIFY(dialog != nullptr);
    auto *track = dialog->findChild<QQuickItem *>(QStringLiteral("autoNumberStartingTrackField"));
    auto *disc = dialog->findChild<QQuickItem *>(QStringLiteral("autoNumberStartingDiscField"));
    QVERIFY(track != nullptr && disc != nullptr);

    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorAutoNumberButton")));
    QVERIFY(QTest::qWaitForWindowExposed(dialog));
    QTRY_VERIFY(track->hasActiveFocus());
    QCOMPARE(track->property("selectedText").toString(), QStringLiteral("1"));
    QTest::keyClick(dialog, '5');
    QCOMPARE(track->property("text").toString(), QStringLiteral("5"));
    QTest::keyClick(dialog, Qt::Key_Return);
    QTRY_VERIFY(!dialog->isVisible());
    QTRY_VERIFY(popupsSettled());
    QCOMPARE(fixture.editor.m_tableModel.fieldValue(0, QStringLiteral("trackNo")), QStringLiteral("5"));

    // Each time it opens, whatever had the keyboard as it closed.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorAutoNumberButton")));
    QTRY_VERIFY(track->hasActiveFocus());
    QCOMPARE(track->property("selectedText").toString(), QStringLiteral("5"));
    auto *writeDisc = dialog->findChild<QObject *>(QStringLiteral("autoNumberWriteDiscCheck"));
    QVERIFY(writeDisc != nullptr && writeDisc->setProperty("checked", true));
    disc->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(dialog, Qt::Key_Return);
    QTRY_VERIFY(!dialog->isVisible());
    QTRY_VERIFY(popupsSettled());
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorAutoNumberButton")));
    QTRY_VERIFY(track->hasActiveFocus());
    QTest::keyClick(dialog, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->isVisible());
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
    QTest::keyClick(qobject_cast<QQuickWindow *>(dialog), 'x');
    QTest::keyClick(qobject_cast<QQuickWindow *>(dialog), '3');
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

    QTest::keyClick(fixture.tags(), Qt::Key_Down);
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{1});
    QTest::keyClick(fixture.tags(), Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(fixture.tags(), Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(selectedTagEditorRows(fixture.editor), (QList<int>{1, 2, 3}));
    QTest::keyClick(fixture.tags(), Qt::Key_Space);
    QCOMPARE(selectedTagEditorRows(fixture.editor), (QList<int>{1, 2}));
    QTest::keyClick(fixture.tags(), Qt::Key_End);
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{4});
    QTest::keyClick(fixture.tags(), Qt::Key_Home, Qt::ShiftModifier);
    QCOMPARE(selectedTagEditorRows(fixture.editor), (QList<int>{0, 1, 2, 3, 4}));
    QTest::keyClick(fixture.tags(), Qt::Key_Up);
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{0});

    // Rows draw their selection from the model.
    QQuickItem *second = tagEditorRowItem(fixture.tags(), 1);
    QVERIFY(second != nullptr);
    QVERIFY(!second->property("selected").toBool());
    QTest::keyClick(fixture.tags(), Qt::Key_Down);
    QVERIFY(second->property("selected").toBool());
}

// The current row, with the focus ring, follows the selection however it
// changes, so the ring and the selection's tint are on one row and the
// keyboard goes on from the selected row (with the selection set
// elsewhere, the ring stayed on row 1 while row 2 was selected).
void TagEditorSmokeTest::tagEditorCurrentRowFollowsTheSelection() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(5, false));
    QObject *table = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorTable"));
    QVERIFY(table != nullptr);
    QTRY_VERIFY(table->property("activeFocus").toBool());
    QCOMPARE(table->property("currentIndex").toInt(), 0);
    const auto ringOn = [&fixture](int row) {
        QQuickItem *item = tagEditorRowItem(fixture.tags(), row);
        return item != nullptr && item->property("current").toBool();
    };

    // Set from outside the table: the ring moves to the selected row.
    fixture.editor.setSelectedRows(QVariantList{2});
    QCOMPARE(table->property("currentIndex").toInt(), 2);
    QTRY_VERIFY(ringOn(2));
    QVERIFY(!ringOn(0));
    QVERIFY(tagEditorRowItem(fixture.tags(), 2)->property("selected").toBool());
    // The keyboard goes on from there.
    QTest::keyClick(fixture.tags(), Qt::Key_Down);
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{3});
    QCOMPARE(table->property("currentIndex").toInt(), 3);
    QTest::keyClick(fixture.tags(), Qt::Key_Up, Qt::ShiftModifier);
    QCOMPARE(selectedTagEditorRows(fixture.editor), (QList<int>{2, 3}));

    // A click selects the row it makes current.
    QQuickItem *second = tagEditorRowItem(fixture.tags(), 1);
    QVERIFY(second != nullptr);
    QTest::mouseClick(fixture.tags(), Qt::LeftButton, Qt::NoModifier,
                      second->mapToScene(QPointF(24, second->height() / 2)).toPoint());
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{1});
    QCOMPARE(table->property("currentIndex").toInt(), 1);
    QTRY_VERIFY(ringOn(1));

    // Select All keeps the current row, which is among them; a selection of
    // several rows set elsewhere makes its first row current.
    fixture.editor.selectAllRows();
    QCOMPARE(table->property("currentIndex").toInt(), 1);
    fixture.editor.setSelectedRows(QVariantList{4, 3});
    QCOMPARE(table->property("currentIndex").toInt(), 3);
    QTest::keyClick(fixture.tags(), Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(selectedTagEditorRows(fixture.editor), (QList<int>{3, 4}));

    // A row Ctrl deselects stays current, as in any list.
    QQuickItem *fourth = tagEditorRowItem(fixture.tags(), 3);
    QVERIFY(fourth != nullptr);
    QTest::mouseClick(fixture.tags(), Qt::LeftButton, Qt::ControlModifier,
                      fourth->mapToScene(QPointF(24, fourth->height() / 2)).toPoint());
    QCOMPARE(selectedTagEditorRows(fixture.editor), QList<int>{4});
    QCOMPARE(table->property("currentIndex").toInt(), 3);
}

void TagEditorSmokeTest::tagEditorShowsEveryValueOfMultiValueFields() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(2, false, {QStringLiteral("B"), QStringLiteral("C")}));
    // The field editors are delegates, which only the item tree holds.
    QObject *note = findQuickItemByObjectName(
        fixture.tags()->contentItem(), QStringLiteral("tagEditorExtraValuesNote_artist"));
    QVERIFY(note != nullptr);
    QQuickItem *first = nullptr;
    QTRY_VERIFY((first = tagEditorRowItem(fixture.tags(), 0)) != nullptr);

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
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QObject *root = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(root != nullptr, "Main.qml failed to show");

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
             QStringLiteral("1 file will be renamed.\n1 file can\u2019t be renamed and keeps its name."));
    // The lead, semibold, as every dialog's first sentence is.
    QCOMPARE(summary->property("font").value<QFont>().weight(), QFont::DemiBold);
    const QVariantList rows = previewDialog->property("rows").toList();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].toMap().value(QStringLiteral("oldName")).toString(), QStringLiteral("01 Song.flac"));
    QCOMPARE(rows[0].toMap().value(QStringLiteral("newName")).toString(), QStringLiteral("01 Intro.flac"));
    QVERIFY(!rows[1].toMap().value(QStringLiteral("ok")).toBool());
    QVERIFY(rows[1].toMap().value(QStringLiteral("error")).toString().contains(QStringLiteral("conflicts")));
    QVERIFY(QTest::qWaitForWindowExposed(qobject_cast<QQuickWindow *>(previewDialog)));
    QVERIFY(visibleItemNamed(qobject_cast<QQuickWindow *>(previewDialog), QStringLiteral("renamePreviewList")) != nullptr);

    QVERIFY(clickVisibleButton(qobject_cast<QQuickWindow *>(previewDialog), QStringLiteral("renamePreviewCancelButton")));
    QTRY_VERIFY(!previewDialog->property("visible").toBool());
    QTest::qWait(50);
    QCOMPARE(renames.load(), 0);
    QVERIFY(!fixture.editor.saving());

    // Rename goes on to rename the files, which needs the bridge this
    // fixture lacks; BridgeClientTest covers the rename itself.
    QSignalSpy renameRequested(previewDialog, SIGNAL(renameRequested()));
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    QTRY_VERIFY(previewDialog->property("opened").toBool());
    QVERIFY(clickVisibleButton(qobject_cast<QQuickWindow *>(previewDialog), QStringLiteral("renamePreviewRenameButton")));
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
    QTest::keyClick(qobject_cast<QQuickWindow *>(previewDialog), Qt::Key_Escape);
    QTRY_VERIFY(popupsSettled());
    QVERIFY(!previewDialog->property("visible").toBool());
    QCOMPARE(renameRequested.count(), 0);

    // Renaming needs the bridge this fixture lacks; BridgeClientTest covers
    // the rename itself.
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    QTRY_VERIFY(previewDialog->property("visible").toBool());
    QVERIFY(keyboardInPopupContent(previewDialog));
    QTest::keyClick(qobject_cast<QQuickWindow *>(previewDialog), Qt::Key_Return);
    QCOMPARE(renameRequested.count(), 1);
    // Enter again, as the preview goes, renames nothing more.
    QVERIFY(dialogClosing(previewDialog));
    QTest::keyClick(qobject_cast<QQuickWindow *>(previewDialog), Qt::Key_Return);
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
    QVERIFY(clickVisibleButton(qobject_cast<QQuickWindow *>(previewDialog), QStringLiteral("renamePreviewRenameButton")));

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

void TagEditorSmokeTest::saveDetailsReadAsAReport_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("default font") << 0.0;
    QTest::newRow("24 pt") << 24.0;
}

// The details of a save that failed for one file read as a report (not a
// plain text dump in a window 522 px tall for six lines): the
// editor's summary, semibold, then each section's heading over its files,
// what failed first, semibold in the negative colour, its file's name
// semibold on a line of its own over the error, secondary, then what
// succeeded quieter, each file 16 px in, more with a larger font, the lines
// a long error wraps to under its own start; the window as tall as all that.
// The save is a real one, into a folder the user may not write to.
void TagEditorSmokeTest::saveDetailsReadAsAReport() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // In an album's folder, whose names have spaces and hyphens, as a
    // library's do: the path is longer than a line.
    QDir root(QDir(dir.path()).filePath(QStringLiteral("The Quiet Meridian/2005 - Small Hours (Deluxe Edition)")));
    QVERIFY(root.mkpath(QStringLiteral("writable")) && root.mkpath(QStringLiteral("read-only")));
    const QString writable = root.filePath(QStringLiteral("writable/01 Brief Signal.flac"));
    const QString readOnly = root.filePath(QStringLiteral("read-only/03 Long Exhale.flac"));
    for (const QString &path : {writable, readOnly}) {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray flac = flac_fixture::flacWithComments({QStringLiteral("TITLE=Old")});
        QCOMPARE(file.write(flac), qint64(flac.size()));
    }
    const QString readOnlyFolder = root.filePath(QStringLiteral("read-only"));
    QVERIFY(QFile::setPermissions(readOnlyFolder, QFile::ReadOwner | QFile::ExeOwner));
    const auto restorePermissions = qScopeGuard([&] {
        QFile::setPermissions(readOnlyFolder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    });
    if (QFileInfo(readOnlyFolder).isWritable()) {
        QSKIP("The user may write to any folder, such as root.");
    }

    TagEditorController editor{nullptr};
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowWithTagEditor(engine, libraryModel, editor);
    QVERIFY(window != nullptr);
    QObject *tagsDialog = window->findChild<QObject *>(QStringLiteral("tagEditorDialog"));
    QObject *details = window->findChild<QObject *>(QStringLiteral("tagEditorStatusDetailsDialog"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(tagsDialog != nullptr && details != nullptr && palette != nullptr);
    QVERIFY(editor.openForPaths({writable, readOnly}));
    QTRY_VERIFY(!editor.loading() && tagsDialog->property("opened").toBool());
    QCOMPARE(editor.m_tableModel.rowCount(), 2);
    editor.setCell(0, QStringLiteral("title"), QStringLiteral("New"));
    editor.setCell(1, QStringLiteral("title"), QStringLiteral("New"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QCOMPARE(editor.statusText(), QStringLiteral("Saved 1 file; 1 failed."));

    QQuickWindow *tags = dialogWindowOf(tagsDialog);
    QVERIFY(tags != nullptr);
    QTRY_VERIFY(clickVisibleButton(tags, QStringLiteral("tagEditorDetailsButton")));
    QTRY_VERIFY(details->property("opened").toBool());
    QQuickWindow *detailsWindow = dialogWindowOf(details);
    QVERIFY(QTest::qWaitForWindowExposed(detailsWindow));

    // The summary first, semibold, in the text colour.
    QQuickItem *summary = visibleItemNamed(detailsWindow, QStringLiteral("tagEditorStatusDetailsSummary"));
    QVERIFY(summary != nullptr);
    QCOMPARE(summary->property("text").toString(), QStringLiteral("Saved 1 file; 1 failed."));
    QCOMPARE(summary->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(summary->property("color").value<QColor>(), palette->property("text").value<QColor>());

    // What failed first (after what succeeded, its file's name ran into the
    // error on one line): the file's name, then the error under it.
    QQuickItem *text = visibleItemNamed(detailsWindow, QStringLiteral("tagEditorStatusDetailsText"));
    QVERIFY(text != nullptr);
    QCOMPARE(text->property("textFormat").toInt(), int(Qt::RichText));
    auto *quickDocument = qvariant_cast<QQuickTextDocument *>(text->property("textDocument"));
    QVERIFY(quickDocument != nullptr);
    QTextDocument *document = quickDocument->textDocument();
    QVERIFY(document != nullptr);
    QStringList blocks;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        blocks << block.text();
    }
    QCOMPARE(blocks.size(), 6);
    QCOMPARE(blocks.mid(0, 2), (QStringList{QStringLiteral("Failed:"), QStringLiteral("03 Long Exhale.flac")}));
    // Why, in plain words, then the system's text as it gave it (the only
    // explanation was the system's, which read as a log).
    QCOMPARE(blocks[2], QStringLiteral("You don’t have permission to write in this folder."));
    QVERIFY2(blocks[3].contains(QStringLiteral("ermission denied")), qPrintable(blocks[3]));
    QVERIFY2(!blocks[3].contains(QStringLiteral("03 Long Exhale.flac: ")), qPrintable(blocks[3]));
    QCOMPARE(blocks.mid(4), (QStringList{QStringLiteral("Succeeded:"), QStringLiteral("01 Brief Signal.flac")}));
    const qreal indent = std::max(16.0, std::round(16.0 * std::max(1.0, palette->property("basePointSize").toReal() / 10.0)));
    const auto blockAt = [&](int index) { return document->findBlockByNumber(index); };
    // The headings on the summary's edge: what succeeded regular, in the
    // secondary colour; what failed semibold, in the negative colour (when
    // both were semibold, as the summary, it no longer led).
    const auto colourOf = [&](int index) {
        const QTextBlock block = blockAt(index);
        return block.begin().fragment().charFormat().foreground().color().rgb();
    };
    const auto weightOf = [&](int index) {
        return blockAt(index).begin().fragment().charFormat().fontWeight();
    };
    for (int index : {0, 4}) {
        QCOMPARE(blockAt(index).blockFormat().leftMargin(), 0.0);
    }
    QVERIFY(weightOf(4) < int(QFont::DemiBold));
    QCOMPARE(colourOf(4), palette->property("textSecondary").value<QColor>().rgb());
    QCOMPARE(weightOf(0), int(QFont::DemiBold));
    QCOMPARE(colourOf(0), palette->property("negative").value<QColor>().rgb());
    // The files indented: the failed one's name semibold in the text's
    // colour, why under it, regular in the same colour, the system's text
    // under that secondary, the other's name regular.
    for (int index : {1, 2, 3, 5}) {
        QCOMPARE(blockAt(index).blockFormat().leftMargin(), indent);
    }
    QCOMPARE(weightOf(1), int(QFont::DemiBold));
    QCOMPARE(colourOf(1), palette->property("text").value<QColor>().rgb());
    QVERIFY(weightOf(2) < int(QFont::DemiBold));
    QCOMPARE(colourOf(2), palette->property("text").value<QColor>().rgb());
    QVERIFY(weightOf(3) < int(QFont::DemiBold));
    QCOMPARE(colourOf(3), palette->property("textSecondary").value<QColor>().rgb());
    QVERIFY(weightOf(5) < int(QFont::DemiBold));
    QCOMPARE(colourOf(5), palette->property("text").value<QColor>().rgb());
    // 12 px between the sections, more with a larger font, not a grid
    // unit, 18 px.
    const qreal sectionGap = std::max(12.0, std::round(12.0 * std::max(1.0, palette->property("basePointSize").toReal() / 10.0)));
    QCOMPARE(blockAt(0).blockFormat().topMargin(), 0.0);
    QCOMPARE(blockAt(4).blockFormat().topMargin(), sectionGap);
    // The error wraps, and its lines start under the file's name, the
    // indent right of the heading.
    const QTextLayout *errorLayout = blockAt(3).layout();
    QVERIFY2(errorLayout->lineCount() > 1, qPrintable(QString::number(errorLayout->lineCount())));
    const auto lineLeft = [](const QTextBlock &block, int line) {
        return block.layout()->position().x() + block.layout()->lineAt(line).x();
    };
    for (int line = 0; line < errorLayout->lineCount(); ++line) {
        QCOMPARE(lineLeft(blockAt(3), line), lineLeft(blockAt(0), 0) + indent);
    }
    QCOMPARE(lineLeft(blockAt(1), 0), lineLeft(blockAt(0), 0) + indent);
    QCOMPARE(lineLeft(blockAt(5), 0), lineLeft(blockAt(0), 0) + indent);
    // As drawn too: each line of the long error, the first and those it
    // wraps to, starts its ink at the file's indent, with nothing drawn
    // between the headings' edge and it (a review read the wrapped line as
    // fallen back past the indent).
    QTRY_VERIFY(text->height() > 0);
    const QImage shown = detailsWindow->grabWindow().convertToFormat(QImage::Format_RGB32);
    QVERIFY(!shown.isNull());
    const qreal scale = shown.devicePixelRatio();
    const QPointF documentOrigin = text->mapToScene(
        QPointF(text->property("leftPadding").toReal(), text->property("topPadding").toReal()));
    // The leftmost drawn pixel of `line` of `block`, in scene pixels, from
    // the headings' edge to the line's end; -1 with none.
    const auto inkLeft = [&](const QTextBlock &block, int line) {
        const QTextLine textLine = block.layout()->lineAt(line);
        const QPointF position = block.layout()->position();
        const int top = int(std::ceil((documentOrigin.y() + position.y() + textLine.y()) * scale));
        const int bottom = int(std::floor((documentOrigin.y() + position.y() + textLine.y() + textLine.height()) * scale)) - 1;
        const int from = int(std::floor((documentOrigin.x() + lineLeft(blockAt(0), 0)) * scale));
        const int to = int(std::ceil((documentOrigin.x() + position.x() + textLine.x() + textLine.naturalTextWidth()) * scale));
        const QColor background(shown.pixel(from, (top + bottom) / 2));
        for (int x = from; x < std::min(to, shown.width()); ++x) {
            for (int y = top; y <= std::min(bottom, shown.height() - 1); ++y) {
                const QColor pixel(shown.pixel(x, y));
                if (std::abs(pixel.red() - background.red()) + std::abs(pixel.green() - background.green())
                        + std::abs(pixel.blue() - background.blue()) > 90) {
                    return x / scale;
                }
            }
        }
        return -1.0;
    };
    const qreal expectedInk = documentOrigin.x() + lineLeft(blockAt(0), 0) + indent;
    // The lines in view: at 24 pt the details may scroll.
    QQuickItem *view = text->parentItem();
    while (view != nullptr && !view->clip()) {
        view = view->parentItem();
    }
    QVERIFY(view != nullptr);
    for (int line = 0; line < errorLayout->lineCount(); ++line) {
        const QTextLine textLine = errorLayout->lineAt(line);
        if (documentOrigin.y() + errorLayout->position().y() + textLine.y() + textLine.height()
            > sceneRectOf(view).bottom()) {
            break;
        }
        const qreal ink = inkLeft(blockAt(3), line);
        QVERIFY2(ink >= expectedInk - 0.5 && ink <= expectedInk + 3,
                 qPrintable(QStringLiteral("line %1 of the error drawn from %2, the indent at %3")
                                .arg(line).arg(ink).arg(expectedInk)));
    }
    // The text on the summary's x.
    QCOMPARE(text->mapToScene(QPointF(text->property("leftPadding").toReal(), 0)).x(),
             sceneRectOf(summary).left());

    // As tall as all that, with a grid unit's room above the buttons, up to
    // 29 grid units scaled with the font, or the screen's height less a grid
    // unit above and below where that is less, past which the details
    // scroll. How tall the details are depends on the length of the
    // temporary folder's path, which the error names.
    QQuickItem *close = visibleItemNamed(detailsWindow, QStringLiteral("tagEditorStatusDetailsCloseButton"));
    QVERIFY(close != nullptr);
    const qreal gridUnit = details->property("contentPadding").toReal();
    QVariant scaledLimit;
    QVERIFY(QMetaObject::invokeMethod(qvariant_cast<QObject *>(details->property("uiPalette")), "scaledSize",
                                      Q_RETURN_ARG(QVariant, scaledLimit), Q_ARG(QVariant, QVariant(gridUnit * 29))));
    QVERIFY(scaledLimit.toReal() >= gridUnit * 29);
    const QScreen *screen = detailsWindow->screen();
    QVERIFY(screen != nullptr);
    const qreal screenRoom =
        std::min(screen->size().height(), screen->availableVirtualSize().height()) - 2 * gridUnit;
    const qreal limit = std::ceil(std::min(scaledLimit.toReal(), screenRoom));
    QTRY_VERIFY(text->height() > 0);
    const qreal textBottom = sceneRectOf(text).top() + text->property("contentHeight").toReal();
    const QString where = QStringLiteral("text to %1, Close from %2 in a window %3 px tall, at most %4")
        .arg(textBottom).arg(sceneRectOf(close).top()).arg(detailsWindow->height()).arg(limit);
    if (pointSize == 0 || detailsWindow->height() < limit) {
        QVERIFY2(detailsWindow->height() < limit, qPrintable(where));
        QVERIFY2(std::abs(sceneRectOf(close).top() - textBottom - gridUnit) <= 1.0, qPrintable(where));
        // With no scroll bar: the window was laid out hidden and small,
        // the bar shown, and the lines beside it took one more than the
        // window had room for, so it stayed.
        QQmlExpression barShown(qmlContext(text), text, QStringLiteral("detailsView.ScrollBar.vertical.visible"));
        QTRY_VERIFY2(!barShown.evaluate().toBool(), qPrintable(where));
    } else {
        QCOMPARE(detailsWindow->height(), int(limit));
        QVERIFY2(textBottom > sceneRectOf(close).top() - gridUnit, qPrintable(where));
        QQmlExpression barShown(qmlContext(text), text, QStringLiteral("detailsView.ScrollBar.vertical.visible"));
        QTRY_VERIFY2(barShown.evaluate().toBool(), qPrintable(where));
    }
    QCOMPARE(sceneRectOf(summary).top(), gridUnit);

    // Copy, from the keyboard or the context menu, copies the details as
    // they read, without the characters that only shape their lines.
    const auto plain = [](QString text) {
        return text.remove(QChar(0x200b)).remove(QChar(0x2060)).replace(QChar(0x00a0), QLatin1Char(' '));
    };
    QStringList documentText;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        documentText << plain(block.text());
    }
    const QString expectedCopy = documentText.join(QLatin1Char('\n'));
    QVERIFY2(expectedCopy.contains(QStringLiteral("(os error 13)")), qPrintable(expectedCopy));
    text->forceActiveFocus(Qt::OtherFocusReason);
    QTRY_VERIFY(text->hasActiveFocus());
    QVERIFY(QMetaObject::invokeMethod(text, "selectAll"));
    QGuiApplication::clipboard()->clear();
    QTest::keySequence(detailsWindow, QKeySequence::Copy);
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), expectedCopy);
    QGuiApplication::clipboard()->clear();
    QQmlExpression menuCopy(qmlContext(text), text, QStringLiteral("copy()"));
    menuCopy.evaluate();
    QVERIFY2(!menuCopy.hasError(), qPrintable(menuCopy.error().toString()));
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), expectedCopy);

    // The failed file's line breaks inside the folder's path only after a
    // slash, never inside a folder's name such as "2005 - Small Hours
    // (Deluxe Edition)" or "read-only", and never inside "(os error 13)"
    // (it broke in "ferrous-tagsave" at 24 pt). The narrowest
    // window spreads the path over lines; the selection goes as the text
    // is laid out anew, with no warning.
    const QString failedItem = editor.statusDetailSections().first().toMap().value(QStringLiteral("items"))
                                   .toList().value(0).toMap().value(QStringLiteral("error")).toString().trimmed();
    QVERIFY2(failedItem.contains(readOnlyFolder + QStringLiteral(": ")), qPrintable(failedItem));
    QVERIFY2(failedItem.endsWith(QStringLiteral("(os error 13)")), qPrintable(failedItem));
    const QFontMetricsF textMetrics(qvariant_cast<QFont>(text->property("font")));
    const qreal openWidth = text->width();
    detailsWindow->resize(detailsWindow->minimumWidth(), detailsWindow->height());
    QTRY_COMPARE(detailsWindow->width(), detailsWindow->minimumWidth());
    // What is wrong with the failed file's lines, once laid out at the
    // narrow width; nothing, where they break as they should.
    const qsizetype pathStart = failedItem.indexOf(readOnlyFolder);
    const qsizetype pathEnd = pathStart + readOnlyFolder.size();
    // The room a folder's name has whole: the line's, less the scroll
    // bar's, whether it shows or not.
    QQmlExpression nameRoomExpression(qmlContext(text), text,
                                      QStringLiteral("root.lineWidth - root.itemIndent"));
    const auto breakProblem = [&]() -> QString {
        const qreal nameRoom = nameRoomExpression.evaluate().toReal();
        const QTextBlock block = blockAt(3);
        const QTextLayout *layout = block.layout();
        QStringList lines;
        for (int line = 0; line < layout->lineCount(); ++line) {
            lines << plain(block.text().mid(layout->lineAt(line).textStart(), layout->lineAt(line).textLength()));
        }
        const QString shown = lines.join(QLatin1Char('|'));
        if (text->width() >= openWidth || lines.join(QString()) != failedItem) {
            return QStringLiteral("not laid out anew: ") + shown;
        }
        if (textMetrics.horizontalAdvance(readOnlyFolder) <= 1.5 * (text->width() - indent)) {
            return QStringLiteral("the path fits in less than two lines");
        }
        qsizetype lineEnd = 0;
        int breaksInPath = 0;
        for (qsizetype line = 0; line + 1 < lines.size(); ++line) {
            lineEnd += lines.at(line).size();
            if (lineEnd > pathStart + 1 && lineEnd < pathEnd) {
                ++breaksInPath;
                // Inside a name only where the name is wider than the line.
                const qsizetype nameStart = failedItem.lastIndexOf(QLatin1Char('/'), lineEnd - 1) + 1;
                const qsizetype slash = failedItem.indexOf(QLatin1Char('/'), lineEnd);
                const QString name = failedItem.mid(nameStart, (slash < 0 || slash > pathEnd ? pathEnd : slash) - nameStart);
                if (failedItem.at(lineEnd - 1) != QLatin1Char('/')
                    && textMetrics.horizontalAdvance(name + QLatin1Char('/')) <= nameRoom) {
                    return QStringLiteral("a break inside the folder name \"%1\": ").arg(name) + shown;
                }
            }
        }
        if (breaksInPath == 0) {
            return QStringLiteral("no break in the path: ") + shown;
        }
        if (std::none_of(lines.cbegin(), lines.cend(),
                         [](const QString &line) { return line.contains(QStringLiteral("(os error 13)")); })) {
            return QStringLiteral("the error's code broken: ") + shown;
        }
        return QString();
    };
    QTRY_COMPARE(breakProblem(), QString());

    QVERIFY(clickVisibleButton(detailsWindow, QStringLiteral("tagEditorStatusDetailsCloseButton")));
    QTRY_VERIFY(!details->property("visible").toBool());
}

namespace {

// Rename Files opens as tall as its summary and its list, a refused
// file's error wrapped under its name, with a grid unit's room above the
// buttons (four files opened it with 79 px of nothing under them), each
// time it opens.
void checkRenamePreviewOpensAsTallAsItsList(TagEditorWindow &fixture, QObject *previewDialog,
                                            const QByteArray &loadResponse) {
    QByteArray preview(R"({"results":[)");
    for (int i = 0; i < 4; ++i) {
        const QString path = QStringLiteral("/fixture/Album 000/%1 Song.flac").arg(i + 1, 2, 10, QLatin1Char('0'));
        const QString newPath = i == 1 ? QStringLiteral("/fixture/Album 000/02 Song (Live).flac") : path;
        // The last can't be renamed, for a reason long enough to wrap.
        const QString result = i == 3
            ? QStringLiteral(R"("ok":false,"error":"%1")")
                  .arg(QStringLiteral("the new name would replace a file that is not among the selected ones, "
                                      "so it keeps its name; rename or move that file first, then try again"))
            : QStringLiteral(R"("ok":true)");
        preview += QStringLiteral(R"(%1{"path":"%2","newPath":"%3",%4})")
                       .arg(i > 0 ? QStringLiteral(",") : QString(), path, newPath, result).toUtf8();
    }
    preview += R"(],"successfulPaths":[]})";
    fixture.editor.m_executeOperation = [=](TagEditorController::Operation operation, const QByteArray &,
                                            const TagEditorController::RenameGate &) {
        return operation == TagEditorController::Operation::PreviewRename ? preview : loadResponse;
    };
    for (int opening = 0; opening < 2; ++opening) {
        QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
        QTRY_VERIFY(previewDialog->property("opened").toBool());
        QQuickWindow *previewWindow = dialogWindowOf(previewDialog);
        QVERIFY(QTest::qWaitForWindowExposed(previewWindow));
        QQuickItem *list = visibleItemNamed(previewWindow, QStringLiteral("renamePreviewList"));
        QQuickItem *cancel = visibleItemNamed(previewWindow, QStringLiteral("renamePreviewCancelButton"));
        QVERIFY(list != nullptr && cancel != nullptr);
        QTRY_COMPARE(list->property("count").toInt(), 4);
        const qreal gridUnit = previewDialog->property("contentPadding").toReal();
        const qreal listBottom = sceneRectOf(list).top() + list->property("contentHeight").toReal();
        const QString where = QStringLiteral("opening %1: the list to %2, Cancel from %3 in a window %4 px tall")
            .arg(opening).arg(listBottom).arg(sceneRectOf(cancel).top()).arg(previewWindow->height());
        QVERIFY2(previewWindow->height() < gridUnit * 29, qPrintable(where));
        QVERIFY2(std::abs(sceneRectOf(cancel).top() - listBottom - gridUnit) <= 1.0, qPrintable(where));
        QVERIFY(clickVisibleButton(previewWindow, QStringLiteral("renamePreviewCancelButton")));
        QTRY_VERIFY(!previewDialog->property("visible").toBool());
    }
}

// Rename Files opens as wide as its widest line: a file's old name, arrow
// and new name in their columns, with the list's gutter, a sentence of the
// summary, or the buttons, within its
// minimum and 42 grid units scaled with the font, where longer lines elide
// in their middle; each time it opens, so long names widen it and short
// ones bring it back (a fixed 756 px held 390 px of lines at 10 pt, and
// 1814 px at 24 pt held 950).
void checkRenamePreviewOpensAsWideAsItsLines(TagEditorWindow &fixture, QObject *previewDialog,
                                             const QByteArray &loadResponse) {
    constexpr int kFiles = 12;
    // Every file renamed, to a short name or a long one; the last can't be.
    const auto previewOf = [](bool longNames) {
        QJsonArray results;
        for (int i = 0; i < kFiles; ++i) {
            const QString directory = QStringLiteral("/fixture/Album %1").arg(i / 10, 3, 10, QLatin1Char('0'));
            const QString path = QStringLiteral("%1/%2 Song.flac").arg(directory).arg(i % 10 + 1, 2, 10, QLatin1Char('0'));
            const QString name = longNames
                ? QStringLiteral("%1 An Unusually Long Song Title for a Preview Row, Extended Remaster with "
                                 "Bonus Material and Alternate Takes.flac").arg(i + 1, 2, 10, QLatin1Char('0'))
                : QStringLiteral("%1 Tune.flac").arg(i + 1, 2, 10, QLatin1Char('0'));
            QJsonObject result{{QStringLiteral("path"), path},
                               {QStringLiteral("newPath"), directory + QLatin1Char('/') + name},
                               {QStringLiteral("ok"), i < kFiles - 1}};
            if (i == kFiles - 1) {
                result.insert(QStringLiteral("error"), QStringLiteral("a file of that name is already there"));
            }
            results.push_back(result);
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("results"), results},
                                         {QStringLiteral("successfulPaths"), QJsonArray()}})
            .toJson(QJsonDocument::Compact);
    };
    const auto longNames = std::make_shared<bool>(false);
    fixture.editor.m_executeOperation = [=](TagEditorController::Operation operation, const QByteArray &,
                                            const TagEditorController::RenameGate &) {
        return operation == TagEditorController::Operation::PreviewRename ? previewOf(*longNames) : loadResponse;
    };
    QObject *palette = qvariant_cast<QObject *>(previewDialog->property("uiPalette"));
    QVERIFY(palette != nullptr);
    const qreal fontScale = palette->property("fontScale").toReal();
    const qreal gridUnit = previewDialog->property("contentPadding").toReal();
    const qreal limit = std::max(42 * gridUnit, qreal(std::round(42 * gridUnit * fontScale)));
    QCOMPARE(previewDialog->property("maximumDefaultWidth").toReal(), limit);
    const qreal gutter = previewDialog->property("listGutter").toReal();

    int shortWidth = 0;
    for (const bool longOpening : {false, true, false}) {
        *longNames = longOpening;
        const QString at = longOpening ? QStringLiteral("long names") : QStringLiteral("short names");
        QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
        QTRY_VERIFY(previewDialog->property("opened").toBool());
        QQuickWindow *previewWindow = dialogWindowOf(previewDialog);
        QVERIFY(QTest::qWaitForWindowExposed(previewWindow));
        QQuickItem *list = visibleItemNamed(previewWindow, QStringLiteral("renamePreviewList"));
        QQuickItem *summary = visibleItemNamed(previewWindow, QStringLiteral("renamePreviewSummary"));
        auto *footer = qobject_cast<QQuickItem *>(qvariant_cast<QObject *>(previewDialog->property("footer")));
        QVERIFY(list != nullptr && summary != nullptr && footer != nullptr);
        QTRY_COMPARE(list->property("count").toInt(), kFiles);
        QTRY_VERIFY(!visibleItemsNamed(list, QStringLiteral("renamePreviewNewName")).isEmpty());
        // Its widest line, measured as the labels draw them: the widest old
        // name, the arrow's column and the widest new name, or that a file
        // keeps its own, which start at one x in every row.
        const QFontMetricsF rowMetrics(qvariant_cast<QFont>(
            visibleItemsNamed(list, QStringLiteral("renamePreviewNewName")).first()->property("font")));
        const QVariantList rows = previewDialog->property("rows").toList();
        QCOMPARE(rows.size(), kFiles);
        const qreal arrowColumn = previewDialog->property("arrowColumnWidth").toReal();
        // The arrow has 8 px on either side with the default font, scaled
        // with a larger one.
        QCOMPARE(arrowColumn, std::ceil(rowMetrics.horizontalAdvance(previewDialog->property("arrowText").toString()))
                                  + 2 * std::max(qreal(8), qreal(std::round(8 * fontScale))));
        qreal widestOldName = 0;
        qreal widestNewName = 0;
        qreal widest = 0;
        for (const QVariant &row : rows) {
            QVariant text;
            QVERIFY(QMetaObject::invokeMethod(previewDialog, "newColumnText", Q_RETURN_ARG(QVariant, text),
                                              Q_ARG(QVariant, row)));
            widestOldName = std::max(widestOldName, rowMetrics.horizontalAdvance(
                                         row.toMap().value(QStringLiteral("oldName")).toString()));
            widestNewName = std::max(widestNewName, rowMetrics.horizontalAdvance(text.toString()));
        }
        widest = std::max(widest, std::ceil(widestOldName) + arrowColumn + std::ceil(widestNewName) + gutter);
        QVariant sentences;
        QVERIFY(QMetaObject::invokeMethod(previewDialog, "summarySentences", Q_RETURN_ARG(QVariant, sentences)));
        const QFontMetricsF summaryMetrics(qvariant_cast<QFont>(summary->property("font")));
        const QStringList summarySentences = sentences.value<QJSValue>().isArray()
            ? sentences.value<QJSValue>().toVariant().toStringList() : sentences.toStringList();
        QCOMPARE(summarySentences.size(), 2);
        for (const QString &sentence : summarySentences) {
            widest = std::max(widest, summaryMetrics.horizontalAdvance(sentence));
        }
        const qreal fitted = std::max(std::ceil(2 * gridUnit + widest), footer->implicitWidth());
        const qreal defaultWidth = previewDialog->property("defaultWidth").toReal();
        const QString where = QStringLiteral("%1: widest line %2, default width %3, window %4, limit %5")
            .arg(at).arg(widest).arg(defaultWidth).arg(previewWindow->width()).arg(limit);
        QCOMPARE(defaultWidth, std::min(limit, fitted));
        QCOMPARE(previewWindow->width(), qCeil(previewDialog->property("openingWidth").toReal()));
        // Each sentence of the summary on a line of its own, whole.
        QTRY_COMPARE(summary->property("lineCount").toInt(), summarySentences.size());
        if (longOpening) {
            // At its limit, or the screen's room, where the names elide.
            QVERIFY2(fitted > limit, qPrintable(where));
            QVERIFY2(previewWindow->width() > shortWidth, qPrintable(where));
        } else {
            // Short names are whole, in a window no wider than they need.
            QVERIFY2(fitted < limit, qPrintable(where));
            QVERIFY2(previewWindow->width() <= std::max(previewWindow->minimumWidth(), qCeil(fitted)),
                     qPrintable(where));
            QList<QQuickItem *> names = visibleItemsNamed(list, QStringLiteral("renamePreviewOldName"));
            names += visibleItemsNamed(list, QStringLiteral("renamePreviewNewName"));
            for (QQuickItem *name : std::as_const(names)) {
                QTRY_VERIFY2(!name->property("truncated").toBool(),
                         qPrintable(where + QStringLiteral(": ") + name->property("text").toString()));
            }
            if (shortWidth > 0) {
                QCOMPARE(previewWindow->width(), shortWidth);
            }
            shortWidth = previewWindow->width();
        }
        QVERIFY(clickVisibleButton(previewWindow, QStringLiteral("renamePreviewCancelButton")));
        QTRY_VERIFY(!previewDialog->property("visible").toBool());
    }
}

// Rename Files shows every file in three columns, in the table's order: its
// old name and the arrow muted, and its new name, which starts at one x in
// every row; a file that keeps its name has no arrow and says so, muted, in
// the new names' column, and one that can't be renamed is in the negative
// colour. Screen readers still read a row as one line. Where the window
// can't hold the names, the old names give way first, down to 40 % of the
// list, and the new names keep their one x (the arrows stood at seven
// places over 105 px, and the new names had no column).
void checkRenamePreviewShowsTheNewNamesInAColumn(TagEditorWindow &fixture, QObject *previewDialog,
                                                 const QByteArray &loadResponse, qreal pointSize) {
    constexpr int kFiles = 8;
    // Old names of several lengths; every third file keeps its name, and
    // the last can't be renamed.
    const auto previewOf = [](bool longNames) {
        const QStringList oldNames{QStringLiteral("track01.flac"), QStringLiteral("02 A Song.flac"),
                                   QStringLiteral("03.flac"), QStringLiteral("04 - Fourth Song Here.flac"),
                                   QStringLiteral("05 Fifth.flac"), QStringLiteral("06 - Kept.flac"),
                                   QStringLiteral("x7.flac"), QStringLiteral("08 Last.flac")};
        QJsonArray results;
        for (int i = 0; i < kFiles; ++i) {
            const QString directory = QStringLiteral("/fixture/Album");
            const QString oldName = longNames
                ? QStringLiteral("%1 An Old File Name Long Enough to Need Its Room in Any Window.flac").arg(i + 1)
                : oldNames.at(i);
            const bool kept = i % 3 == 2;
            const QString newName = kept ? oldName
                : longNames ? QStringLiteral("%1 - A New File Name Long Enough to Need Its Room in Any Window, "
                                             "Remastered.flac").arg(i + 1, 2, 10, QLatin1Char('0'))
                            : QStringLiteral("%1 - Song %2.flac").arg(i + 1, 2, 10, QLatin1Char('0')).arg(i + 1);
            QJsonObject result{{QStringLiteral("path"), directory + QLatin1Char('/') + oldName},
                               {QStringLiteral("newPath"), directory + QLatin1Char('/') + newName},
                               {QStringLiteral("ok"), i < kFiles - 1}};
            if (i == kFiles - 1) {
                result.insert(QStringLiteral("error"), QStringLiteral("a file of that name is already there"));
            }
            results.push_back(result);
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("results"), results},
                                         {QStringLiteral("successfulPaths"), QJsonArray()}})
            .toJson(QJsonDocument::Compact);
    };
    const auto longNames = std::make_shared<bool>(false);
    fixture.editor.m_executeOperation = [=](TagEditorController::Operation operation, const QByteArray &,
                                            const TagEditorController::RenameGate &) {
        return operation == TagEditorController::Operation::PreviewRename ? previewOf(*longNames) : loadResponse;
    };
    QObject *palette = qvariant_cast<QObject *>(previewDialog->property("uiPalette"));
    QVERIFY(palette != nullptr);
    const QRgb text = palette->property("text").value<QColor>().rgba();
    const QRgb secondary = palette->property("textSecondary").value<QColor>().rgba();
    const QRgb negative = palette->property("negative").value<QColor>().rgba();
    const qreal gutter = previewDialog->property("listGutter").toReal();
    const QString arrow = previewDialog->property("arrowText").toString();

    for (const bool longOpening : {false, true}) {
        *longNames = longOpening;
        const QString at = QStringLiteral("%1 pt, %2").arg(pointSize)
            .arg(longOpening ? QStringLiteral("long names") : QStringLiteral("short names"));
        QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
        QTRY_VERIFY(previewDialog->property("opened").toBool());
        QQuickWindow *previewWindow = dialogWindowOf(previewDialog);
        QVERIFY(QTest::qWaitForWindowExposed(previewWindow));
        QQuickItem *list = visibleItemNamed(previewWindow, QStringLiteral("renamePreviewList"));
        QVERIFY(list != nullptr);
        QTRY_COMPARE(list->property("count").toInt(), kFiles);
        const qreal line = list->width() - gutter;
        // Five renamed, two kept and one refused, all in view, in the
        // table's order, each in the three columns.
        QTRY_COMPARE(visibleItemsNamed(list, QStringLiteral("renamePreviewFileRow")).size(), kFiles);
        QList<QQuickItem *> fileRows = visibleItemsNamed(list, QStringLiteral("renamePreviewFileRow"));
        std::sort(fileRows.begin(), fileRows.end(), [list](QQuickItem *a, QQuickItem *b) {
            return a->mapToItem(list, QPointF()).y() < b->mapToItem(list, QPointF()).y();
        });
        const QVariantList rows = previewDialog->property("rows").toList();
        QCOMPARE(rows.size(), kFiles);
        QList<QQuickItem *> oldNames;
        QList<QQuickItem *> newNames;
        const qreal newNameX = previewDialog->property("oldNameColumnWidth").toReal()
            + previewDialog->property("arrowColumnWidth").toReal();
        for (int i = 0; i < kFiles; ++i) {
            QQuickItem *row = fileRows.at(i);
            QQuickItem *oldName = itemNamed(row, QStringLiteral("renamePreviewOldName"));
            QQuickItem *glyph = itemNamed(row, QStringLiteral("renamePreviewArrow"));
            QQuickItem *newName = itemNamed(row, QStringLiteral("renamePreviewNewName"));
            QVERIFY(oldName != nullptr && glyph != nullptr && newName != nullptr);
            const QVariantMap fields = rows.at(i).toMap();
            const bool refused = !fields.value(QStringLiteral("ok")).toBool();
            const bool kept = fields.value(QStringLiteral("unchanged")).toBool();
            const QRgb quiet = refused ? negative : secondary;
            QCOMPARE(oldName->property("text").toString(), fields.value(QStringLiteral("oldName")).toString());
            QCOMPARE(oldName->mapToItem(list, QPointF()).x(), 0.0);
            QCOMPARE(oldName->property("color").value<QColor>().rgba(), quiet);
            QCOMPARE(oldName->property("elide").toInt(), int(Qt::ElideMiddle));
            QCOMPARE(glyph->isVisible(), !kept);
            if (!kept) {
                QCOMPARE(glyph->property("text").toString(), arrow);
                QCOMPARE(glyph->property("color").value<QColor>().rgba(), quiet);
                QVERIFY(glyph->mapToItem(list, QPointF(glyph->width(), 0)).x() <= newNameX);
            }
            QCOMPARE(newName->property("text").toString(),
                     kept ? QStringLiteral("keeps its name") : fields.value(QStringLiteral("newName")).toString());
            // A name that stays may go on past its column, in its line.
            if (kept && longOpening) {
                QVERIFY2(newName->mapToItem(list, QPointF()).x() >= newNameX, qPrintable(at));
            } else {
                QCOMPARE(newName->mapToItem(list, QPointF()).x(), newNameX);
            }
            QCOMPARE(newName->property("color").value<QColor>().rgba(),
                     refused ? quiet : kept ? palette->property("textTertiary").value<QColor>().rgba() : text);
            QCOMPARE(newName->property("elide").toInt(), int(Qt::ElideMiddle));
            QVERIFY2(newName->mapToItem(list, QPointF()).x() + newName->width() <= line + 0.5, qPrintable(at));
            // A screen reader reads the row as one line.
            QVariant rowText;
            QVERIFY(QMetaObject::invokeMethod(previewDialog, "rowText", Q_RETURN_ARG(QVariant, rowText),
                                              Q_ARG(QVariant, rows.at(i))));
            QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(row);
            QVERIFY(accessible != nullptr);
            QCOMPARE(accessible->text(QAccessible::Name), rowText.toString());
            QVERIFY(rowText.toString().contains(kept ? QStringLiteral("keeps its name") : arrow));
            oldNames << oldName;
            newNames << newName;
        }

        const qreal oldColumn = previewDialog->property("oldNameColumnWidth").toReal();
        if (!longOpening) {
            // The window holds every name whole.
            for (QQuickItem *name : oldNames + newNames) {
                QTRY_VERIFY2(!name->property("truncated").toBool(),
                             qPrintable(at + QStringLiteral(": ") + name->property("text").toString()));
            }
        } else {
            // The old names give way to the new ones, down to 40 % of the
            // list: both elide in their middle.
            QVERIFY2(oldColumn >= std::floor(0.4 * line), qPrintable(at));
            QVERIFY2(oldColumn < 0.5 * line, qPrintable(at));
            QTRY_VERIFY2(oldNames.first()->property("truncated").toBool(), qPrintable(at));
        }
        QVERIFY(clickVisibleButton(previewWindow, QStringLiteral("renamePreviewCancelButton")));
        QTRY_VERIFY(!previewDialog->property("visible").toBool());
    }
}

} // namespace

void TagEditorSmokeTest::renamePreviewLaysOutItsRows_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// Rename Files as tall as its list, as wide as its lines, and with the new
// names in a column, in turn in one window.
void TagEditorSmokeTest::renamePreviewLaysOutItsRows() {
    QFETCH(qreal, pointSize);
    QFont font(QStringLiteral("Noto Sans"));
    font.setPointSizeF(pointSize);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QGuiApplication::setFont(font);
    constexpr int kFiles = 12;
    TagEditorWindow fixture;
    QVERIFY(fixture.open(kFiles, false));
    fixture.editor.selectAllRows();
    QObject *previewDialog = fixture.window->findChild<QObject *>(QStringLiteral("renamePreviewDialog"));
    QVERIFY(previewDialog != nullptr);
    const QByteArray loadResponse = tagEditorLoadResponse(kFiles);
    checkRenamePreviewOpensAsTallAsItsList(fixture, previewDialog, loadResponse);
    if (QTest::currentTestFailed()) {
        return;
    }
    checkRenamePreviewOpensAsWideAsItsLines(fixture, previewDialog, loadResponse);
    if (QTest::currentTestFailed()) {
        return;
    }
    checkRenamePreviewShowsTheNewNamesInAColumn(fixture, previewDialog, loadResponse, pointSize);
}

void TagEditorSmokeTest::renamePreviewALongNameThatStaysCutsNoNewName_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// The long name of a file that keeps it elides in the first column, which
// gives way to the new names: none of them is cut for it, and it runs under
// no arrow (it ran across the arrows and the new names, as one line).
void TagEditorSmokeTest::renamePreviewALongNameThatStaysCutsNoNewName() {
    QFETCH(qreal, pointSize);
    QFont font(QStringLiteral("Noto Sans"));
    font.setPointSizeF(pointSize);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QGuiApplication::setFont(font);
    TagEditorWindow fixture;
    QVERIFY(fixture.open(3, false));
    fixture.editor.selectAllRows();
    const QString kept = QStringLiteral("01 - An Overture for Strings, Brass, Two Pianos and a Very Distant "
                                        "Thunderstorm over the Northbound Harbour.flac");
    const QString renamed = QStringLiteral("02 - Morning Tide.flac");
    QJsonArray results;
    for (const auto &[from, to] : {std::pair{kept, kept}, std::pair{QStringLiteral("track02.flac"), renamed},
                                   std::pair{QStringLiteral("03 Short.flac"), QStringLiteral("03 Short.flac")}}) {
        results.push_back(QJsonObject{{QStringLiteral("path"), QStringLiteral("/fixture/Album/") + from},
                                      {QStringLiteral("newPath"), QStringLiteral("/fixture/Album/") + to},
                                      {QStringLiteral("ok"), true}});
    }
    const QByteArray preview =
        QJsonDocument(QJsonObject{{QStringLiteral("results"), results}}).toJson(QJsonDocument::Compact);
    fixture.editor.m_executeOperation = [preview](TagEditorController::Operation, const QByteArray &,
                                                  const TagEditorController::RenameGate &) { return preview; };
    QObject *previewDialog = fixture.window->findChild<QObject *>(QStringLiteral("renamePreviewDialog"));
    QVERIFY(previewDialog != nullptr);
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    QTRY_VERIFY(previewDialog->property("opened").toBool());
    QQuickWindow *previewWindow = dialogWindowOf(previewDialog);
    QVERIFY(QTest::qWaitForWindowExposed(previewWindow));
    QHash<QString, QQuickItem *> byText;
    QTRY_VERIFY(visibleItemsNamed(previewWindow->contentItem(), QStringLiteral("renamePreviewOldName")).size() == 3);
    for (const char *column : {"renamePreviewOldName", "renamePreviewNewName"}) {
        for (QQuickItem *item : visibleItemsNamed(previewWindow->contentItem(), QString::fromLatin1(column))) {
            byText.insert(item->property("text").toString(), item);
        }
    }
    QVERIFY(byText.contains(kept) && byText.contains(renamed) && byText.contains(QStringLiteral("keeps its name")));
    QTRY_VERIFY(byText.value(kept)->property("truncated").toBool());
    QTRY_VERIFY(!byText.value(renamed)->property("truncated").toBool());
    QVERIFY(!byText.value(QStringLiteral("track02.flac"))->property("truncated").toBool());
    // Within its own line, before what that says.
    QQuickItem *keeps = itemNamed(byText.value(kept)->parentItem(), QStringLiteral("renamePreviewNewName"));
    QVERIFY(keeps != nullptr);
    QVERIFY(sceneRectOf(byText.value(kept)).right() <= sceneRectOf(keeps).left());
    QVERIFY(clickVisibleButton(previewWindow, QStringLiteral("renamePreviewCancelButton")));
    QTRY_VERIFY(!previewDialog->property("visible").toBool());
}

// Several failed files are 8 px apart, each error 2 px under its file's
// name, and Copy keeps every message whole, in the order shown.
void TagEditorSmokeTest::saveDetailsSetFailedFilesApart() {
    TagEditorWindow fixture;
    QVERIFY(fixture.open(3, true));
    fixture.saves->setResponse(QByteArray(
        R"({"results":[{"path":"/fixture/Album 000/01 Song.flac","ok":true},)"
        R"json({"path":"/fixture/Album 000/02 Song.flac","ok":false,"error":"disk full (os error 28)","cause":"noSpace"},)json"
        R"json({"path":"/fixture/Album 000/03 Song.flac","ok":false,"error":"permission denied (os error 13)"}]})json"));
    QSignalSpy saved(&fixture.editor, &TagEditorController::saveFinished);
    QVERIFY(fixture.editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QObject *details = fixture.window->findChild<QObject *>(QStringLiteral("tagEditorStatusDetailsDialog"));
    QVERIFY(details != nullptr);
    QTRY_VERIFY(clickVisibleButton(fixture.tags(), QStringLiteral("tagEditorDetailsButton")));
    QTRY_VERIFY(details->property("opened").toBool());
    QQuickWindow *detailsWindow = dialogWindowOf(details);
    QVERIFY(QTest::qWaitForWindowExposed(detailsWindow));
    QQuickItem *text = visibleItemNamed(detailsWindow, QStringLiteral("tagEditorStatusDetailsText"));
    QVERIFY(text != nullptr);
    auto *quickDocument = qvariant_cast<QQuickTextDocument *>(text->property("textDocument"));
    QVERIFY(quickDocument != nullptr);
    const QTextDocument *document = quickDocument->textDocument();
    QStringList blocks;
    QList<qreal> gaps;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        blocks << block.text().replace(QChar(0x00a0), QLatin1Char(' '));
        gaps << block.blockFormat().topMargin();
    }
    // The cause under the name where the backend sent its code, and no line
    // where it sent none, though the error's words say "permission denied".
    QCOMPARE(blocks, (QStringList{QStringLiteral("Failed:"), QStringLiteral("02 Song.flac"),
                                  QStringLiteral("There isn’t enough free space on the disk."),
                                  QStringLiteral("disk full (os error 28)"), QStringLiteral("03 Song.flac"),
                                  QStringLiteral("permission denied (os error 13)"), QStringLiteral("Succeeded:"),
                                  QStringLiteral("01 Song.flac")}));
    const qreal scale = qMax(1.0, evaluateOn(text, QStringLiteral("root.uiPalette.fontScale")).toReal());
    QCOMPARE(gaps, (QList<qreal>{0, 0, qMax(2.0, std::round(2 * scale)), qMax(2.0, std::round(2 * scale)),
                                 qMax(8.0, std::round(8 * scale)),
                                 qMax(2.0, std::round(2 * scale)), qMax(12.0, std::round(12 * scale)), 0}));
    text->forceActiveFocus(Qt::OtherFocusReason);
    QVERIFY(QMetaObject::invokeMethod(text, "selectAll"));
    QGuiApplication::clipboard()->clear();
    evaluateOn(text, QStringLiteral("copy()"));
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), blocks.join(QLatin1Char('\n')));
    QVERIFY(clickVisibleButton(detailsWindow, QStringLiteral("tagEditorStatusDetailsCloseButton")));
    QTRY_VERIFY(!details->property("visible").toBool());
}

std::unique_ptr<QmlSmokeTest> makeTagEditorSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<TagEditorSmokeTest>(userDirectoriesParent);
}

void TagEditorSmokeTest::renamePreviewShowsANameThatStaysWhole_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// A file that keeps its name shows it whole where its line has room, past
// the old names' column that a long new name narrows (it was cut in its
// middle beside 350 px of nothing), and "keeps its name" follows it,
// quieter than the names, readable and on their baseline. The renamed
// files' columns stay where they are.
void TagEditorSmokeTest::renamePreviewShowsANameThatStaysWhole() {
    QFETCH(qreal, pointSize);
    QFont font(QStringLiteral("Noto Sans"));
    font.setPointSizeF(pointSize);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QGuiApplication::setFont(font);
    TagEditorWindow fixture;
    QVERIFY(fixture.open(3, false));
    fixture.editor.selectAllRows();
    const QString stays = QStringLiteral("01 - Overture for Strings and a Distant Summer Storm.flac");
    const QString longNew = QStringLiteral(
        "02 - Morning Tide Field (Live at the Harbour Hall, the Second Night of Two).flac");
    const QString shortNew = QStringLiteral("03 - Static.flac");
    QJsonArray results;
    for (const auto &[from, to] : {std::pair{stays, stays}, std::pair{QStringLiteral("track02.flac"), longNew},
                                   std::pair{QStringLiteral("track03.flac"), shortNew}}) {
        results.push_back(QJsonObject{{QStringLiteral("path"), QStringLiteral("/fixture/Album/") + from},
                                      {QStringLiteral("newPath"), QStringLiteral("/fixture/Album/") + to},
                                      {QStringLiteral("ok"), true}});
    }
    const QByteArray preview =
        QJsonDocument(QJsonObject{{QStringLiteral("results"), results}}).toJson(QJsonDocument::Compact);
    fixture.editor.m_executeOperation = [preview](TagEditorController::Operation, const QByteArray &,
                                                  const TagEditorController::RenameGate &) { return preview; };
    QObject *previewDialog = fixture.window->findChild<QObject *>(QStringLiteral("renamePreviewDialog"));
    QVERIFY(previewDialog != nullptr);
    QVERIFY(clickButton(fixture.window, QStringLiteral("tagEditorRenameButton")));
    QTRY_VERIFY(previewDialog->property("opened").toBool());
    QQuickWindow *previewWindow = dialogWindowOf(previewDialog);
    QVERIFY(QTest::qWaitForWindowExposed(previewWindow));
    QHash<QString, QQuickItem *> byText;
    QTRY_VERIFY(visibleItemsNamed(previewWindow->contentItem(), QStringLiteral("renamePreviewOldName")).size() == 3);
    for (const char *column : {"renamePreviewOldName", "renamePreviewNewName"}) {
        for (QQuickItem *item : visibleItemsNamed(previewWindow->contentItem(), QString::fromLatin1(column))) {
            byText.insert(item->property("text").toString(), item);
        }
    }
    QQuickItem *staying = byText.value(stays);
    QQuickItem *keeps = byText.value(QStringLiteral("keeps its name"));
    QQuickItem *renamed = byText.value(longNew);
    QQuickItem *other = byText.value(shortNew);
    QVERIFY(staying != nullptr && keeps != nullptr && renamed != nullptr && other != nullptr);
    // A line just long enough for the name that stays and what follows it.
    const qreal arrowColumn = previewDialog->property("arrowColumnWidth").toReal();
    const int lineWidth = qCeil(2 * previewDialog->property("contentPadding").toReal()
                               + previewDialog->property("listGutter").toReal() + std::ceil(staying->implicitWidth())
                               + arrowColumn + std::ceil(keeps->implicitWidth())) + 8;
    previewWindow->resize(lineWidth, previewWindow->height());
    QTRY_COMPARE(previewWindow->contentItem()->width(), qreal(lineWidth));
    // Wider than the old names' column, which the long new name narrows.
    const qreal column = previewDialog->property("oldNameColumnWidth").toReal();
    QVERIFY2(staying->implicitWidth() > column + 1, qPrintable(QString::number(column)));
    QTRY_VERIFY2(!staying->property("truncated").toBool(), qPrintable(describeRect(sceneRectOf(staying))));
    QTRY_VERIFY(!keeps->property("truncated").toBool());
    QTRY_VERIFY(qAbs(sceneRectOf(keeps).left() - sceneRectOf(staying).right() - arrowColumn) < 1);
    // The renamed files' new names at the column's end, one x.
    QCOMPARE(sceneRectOf(renamed).left(), sceneRectOf(other).left());
    QVERIFY(qAbs(sceneRectOf(other).left() - sceneRectOf(staying).left() - column - arrowColumn) < 0.5);
    // Quieter: the small font and the tertiary colour, 4.5:1 on the window.
    QObject *palette = qvariant_cast<QObject *>(previewDialog->property("uiPalette"));
    QVERIFY(palette != nullptr);
    QCOMPARE(keeps->property("color").value<QColor>().rgba(), palette->property("textTertiary").value<QColor>().rgba());
    QVERIFY(contrastRatio(keeps->property("color").value<QColor>(), palette->property("chrome").value<QColor>()) >= 4.5);
    QVERIFY(keeps->property("font").value<QFont>().pointSizeF() < staying->property("font").value<QFont>().pointSizeF());
    QTRY_VERIFY(qAbs(sceneRectOf(keeps).top() + keeps->baselineOffset()
                     - sceneRectOf(staying).top() - staying->baselineOffset()) <= 1);
    QVERIFY(clickVisibleButton(previewWindow, QStringLiteral("renamePreviewCancelButton")));
    QTRY_VERIFY(!previewDialog->property("visible").toBool());
}

#include "tst_qml_smoke_tag_editor.moc"
