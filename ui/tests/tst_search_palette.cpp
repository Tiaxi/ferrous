// SPDX-License-Identifier: GPL-3.0-or-later

// The search palette of #373: its frame and place under the transport, the
// dim over the whole window, the sections in the order of their best match,
// the rows' columns, thumbnails and match emphasis, the selected, hovered
// and playing states, the key hints per state, the spelling suggestion, the
// recent searches, the fade, and the keyboard and mouse actions, with the
// contrast of the text as drawn in Breeze Dark and Breeze Light.

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>

#include "GlobalSearchResultsModel.h"
#include "Localization.h"
#include "TestColor.h"
#include "TestItems.h"
#include "TestMessages.h"
#include "TestMotion.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

// The colours of a Breeze scheme that UiPalette reads, from Breeze's
// .colors files.
struct Scheme {
    QString window;
    QString view;
    QString text;
    QString inactiveText;
    QString highlight;
    QString button;
};

const Scheme kBreezeDark{QStringLiteral("#202326"), QStringLiteral("#141618"), QStringLiteral("#fcfcfc"),
                         QStringLiteral("#a1a9b1"), QStringLiteral("#3daee9"), QStringLiteral("#292c30")};
const Scheme kBreezeLight{QStringLiteral("#eff0f1"), QStringLiteral("#ffffff"), QStringLiteral("#232629"),
                          QStringLiteral("#707d8a"), QStringLiteral("#3daee9"), QStringLiteral("#fcfcfc")};

// A window in `scheme`, `width` × `height`, with a 48 px transport band at
// the top and the palette over a fake bridge and the real controller and
// results model.
QByteArray harnessQml(const Scheme &scheme, int width, int height) {
    return QStringLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components
import "controllers" as Controllers
import "dialogs" as Dialogs

ApplicationWindow {
    id: window
    width: %7
    height: %8
    visible: true
    palette.window: "%1"
    palette.base: "%2"
    palette.text: "%3"
    palette.windowText: "%3"
    palette.button: "%6"
    palette.buttonText: "%3"
    property alias bridge: bridge
    property alias tokens: tokens
    property alias controller: controller
    property alias dialog: dialog
    property alias transport: transport

    QtObject {
        id: bridge
        property var globalSearchModel: searchModel
        property bool globalSearchBusy: false
        property bool globalSearchModelRetained: false
        property bool globalSearchCanExpand: true
        property var globalSearchTotals: [0, 0, 0]
        property string globalSearchSuggestion: ""
        property int libraryRootCount: 1
        property string currentTrackPath: ""
        property string playbackState: "Stopped"
        property var queries: []
        property var played: []
        property var queued: []
        property var expanded: []
        property var revealed: []
        function setGlobalSearchQuery(query) { queries = queries.concat([query]) }
        function expandGlobalSearch(rowType) { expanded = expanded.concat([rowType]) }
        function replaceWithPaths(paths) { played = played.concat(["track:" + paths.join(",")]) }
        function replaceAlbumByKey(artist, album) { played = played.concat(["album:" + album]) }
        function replaceArtistByName(artist) { played = played.concat(["artist:" + artist]) }
        function appendTrack(path) { queued = queued.concat(["track:" + path]) }
        function appendAlbumByKey(artist, album) { queued = queued.concat(["album:" + album]) }
        function appendArtistByName(artist) { queued = queued.concat(["artist:" + artist]) }
    }

    Controllers.GlobalSearchController {
        id: controller
        uiBridge: bridge
        globalSearchModelApi: searchModel
        requestLibraryRevealForSearchRow: function(row) { bridge.revealed = bridge.revealed.concat([row.label]) }
        focusLibraryViewForNavigation: function() {}
        requestOpenInFileBrowserForSearchRow: function(row) {}
    }

    Item {
        anchors.fill: parent
        Kirigami.Theme.inherit: false
        Kirigami.Theme.backgroundColor: "%1"
        Kirigami.Theme.textColor: "%3"
        Kirigami.Theme.disabledTextColor: "%4"
        Kirigami.Theme.highlightColor: "%5"

        Components.UiPalette {
            id: tokens
            windowRoot: window
        }

        Rectangle {
            id: transport
            objectName: "transportBar"
            width: parent.width
            height: 48
            color: tokens.chrome
            property int presses: 0
            property int wheels: 0

            MouseArea {
                objectName: "transportMouseArea"
                anchors.fill: parent
                hoverEnabled: true
                onPressed: transport.presses += 1
                onWheel: transport.wheels += 1
            }
        }
    }

    Dialogs.GlobalSearchDialog {
        id: dialog
        controller: controller
        uiPalette: tokens
        windowRoot: window
        anchorItem: transport
        snappyScrollFlickDeceleration: 18000
        snappyScrollMaxFlickVelocity: 1400
    }
}
)QML")
        .arg(scheme.window, scheme.view, scheme.text, scheme.inactiveText, scheme.highlight, scheme.button)
        .arg(width)
        .arg(height)
        .toUtf8();
}

QString formatErrors(const QList<QQmlError> &errors) {
    QStringList lines;
    for (const QQmlError &error : errors) {
        lines << error.toString();
    }
    return lines.join(QLatin1Char('\n'));
}

using Row = GlobalSearchResultsModel::SearchDisplayRow;

Row result(const QString &type, const QString &label, float score) {
    Row row;
    row.kind = QStringLiteral("item");
    row.rowType = type;
    row.label = label;
    row.score = score;
    return row;
}

// Results for "meridian": the tracks match best, then the artist, then the
// albums. `coverUrl` is the albums' and tracks' cover.
QVector<Row> meridianRows(const QString &coverUrl) {
    QVector<Row> rows;
    Row artist = result(QStringLiteral("artist"), QStringLiteral("The Quiet Meridian"), 2.0f);
    artist.count = 4;
    rows << artist;
    for (const auto &[name, year] : {std::pair{QStringLiteral("Small Hours"), 2005}, {QStringLiteral("Grey Lines"), 2009}}) {
        Row album = result(QStringLiteral("album"), name, 3.0f);
        album.artist = QStringLiteral("The Quiet Meridian");
        album.year = year;
        album.coverUrl = coverUrl;
        rows << album;
    }
    const auto track = [&coverUrl](const QString &title, const QString &album, float score, const QString &path) {
        Row row = result(QStringLiteral("track"), title, score);
        row.artist = QStringLiteral("The Quiet Meridian");
        row.album = album;
        row.coverUrl = coverUrl;
        row.trackPath = path;
        row.lengthText = QStringLiteral("3:49");
        return row;
    };
    rows << track(QStringLiteral("Meridian Line"), QStringLiteral("Grey Lines"), 1.0f, QStringLiteral("/fixture/line.flac"))
         << track(QStringLiteral("Meridian Drift"), QStringLiteral("Afterimage"), 1.0f, QStringLiteral("/fixture/drift.flac"))
         << track(QStringLiteral("Morning Tide Field"), QStringLiteral("Small Hours"), 4.0f,
                  QStringLiteral("/fixture/morning.flac"));
    // No cover: the no-cover tile.
    Row bare = track(QStringLiteral("Lantern Signal"), QStringLiteral("Small Hours"), 4.0f, QStringLiteral("/fixture/lantern.flac"));
    bare.coverUrl.clear();
    rows << bare;
    return rows;
}

void visit(QQuickItem *item, const std::function<void(QQuickItem *)> &function) {
    function(item);
    for (QQuickItem *child : item->childItems()) {
        visit(child, function);
    }
}

QList<QQuickItem *> visibleNamed(QQuickItem *root, const QString &name) {
    QList<QQuickItem *> items;
    visit(root, [&](QQuickItem *item) {
        if (item->objectName() == name && item->isVisible()) items << item;
    });
    return items;
}

QQuickItem *visibleChildNamed(QQuickItem *root, const QString &name) {
    const auto items = visibleNamed(root, name);
    return items.isEmpty() ? nullptr : items.first();
}

// A result's context as drawn: whole, in one text.
QList<QQuickItem *> contextTexts(QQuickItem *row) {
    return visibleNamed(row, QStringLiteral("globalSearchResultContext"));
}

// Where a result's context is drawn: the box of all its runs.
QRectF contextBoxOf(QQuickItem *row) {
    QRectF rect;
    for (QQuickItem *text : contextTexts(row)) {
        const QRectF box = text->mapRectToScene(QRectF(0, 0, text->width(), text->height()));
        rect = rect.isNull() ? box : rect.united(box);
    }
    return rect;
}

// The words of a context's text, without the colour of its matches.
QString contextWords(QQuickItem *context) {
    QString text = context->property("text").toString();
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return text;
}

// A result's context, put together from its runs.
QString contextText(QQuickItem *row) {
    QString text;
    for (QQuickItem *item : contextTexts(row)) text += contextWords(item);
    return text;
}

// The runs of a result's context that are in `color`, as its markup says.
QStringList contextRunsIn(QQuickItem *row, const QColor &color) {
    QStringList runs;
    const QRegularExpression run(QStringLiteral("<font color=\"([^\"]*)\">([^<]*)</font>"));
    for (QQuickItem *item : contextTexts(row)) {
        const QString text = item->property("text").toString();
        for (auto found = run.globalMatch(text); found.hasNext();) {
            const auto match = found.next();
            if (QColor(match.captured(1)) == color) runs << match.captured(2);
        }
    }
    return runs;
}

// A row's height at the default font, by its kind and its place: a heading
// that follows a section has 8 px more.
qreal rowHeightFor(const QString &kind, int index) {
    if (kind == QStringLiteral("item")) return 32.0;
    if (kind.isEmpty()) return 0.0;
    return kind == QStringLiteral("section") && index > 0 ? 36.0 : 28.0;
}

QColor colorOf(QQuickItem *item) {
    return item->property("color").value<QColor>();
}

// The harness window, shown and active, with a results model of the test's
// own and the palette open.
struct Harness {
    GlobalSearchResultsModel model;
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QObject *bridge = nullptr;
    QObject *tokens = nullptr;
    QObject *controller = nullptr;
    QObject *dialog = nullptr;
    QTemporaryDir covers;
    QString coverUrl;

    bool load(const Scheme &scheme, QString *errorText, int width = 1600, int height = 1000) {
        if (!covers.isValid()) {
            *errorText = QStringLiteral("no temporary directory");
            return false;
        }
        QImage cover(48, 48, QImage::Format_RGB32);
        cover.fill(QColor(QStringLiteral("#b07a5a")));
        const QString coverPath = covers.filePath(QStringLiteral("cover.png"));
        cover.save(coverPath);
        coverUrl = QUrl::fromLocalFile(coverPath).toString();
        engine.rootContext()->setContextProperty(QStringLiteral("searchModel"), &model);
        QQmlComponent component(&engine);
        component.setData(harnessQml(scheme, width, height),
                          QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/SearchPaletteHarness.qml")));
        root.reset(component.create());
        if (!root) {
            *errorText = formatErrors(component.errors());
            return false;
        }
        window = qobject_cast<QQuickWindow *>(root.get());
        bridge = root->property("bridge").value<QObject *>();
        tokens = root->property("tokens").value<QObject *>();
        controller = root->property("controller").value<QObject *>();
        dialog = root->property("dialog").value<QObject *>();
        if (window == nullptr || bridge == nullptr || tokens == nullptr || controller == nullptr || dialog == nullptr
            || !QTest::qWaitForWindowExposed(window)) {
            *errorText = QStringLiteral("the harness did not show");
            return false;
        }
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window)) {
            *errorText = QStringLiteral("the harness did not become active");
            return false;
        }
        return true;
    }

    // Opens the palette, without a fade.
    bool open() {
        dialog->setProperty("fadeDuration", 0);
        QMetaObject::invokeMethod(controller, "openDialog");
        return QTest::qWaitFor([this] { return dialog->property("opened").toBool(); }) && field()->hasActiveFocus();
    }

    // Types `query` and shows `rows` as its results.
    void search(const QString &query, const QVector<Row> &rows, const QVector<int> &totals = {}, bool canShowMore = true) {
        field()->setProperty("text", query);
        model.presentSearchRows(rows, totals, canShowMore);
        settleRows();
    }

    // Waits for the model's batched insertion of a first result list.
    void settleRows() {
        int unchanged = 0;
        int count = model.rowCount();
        while (unchanged < 3) {
            QTest::qWait(1);
            unchanged = model.rowCount() == count ? unchanged + 1 : 0;
            count = model.rowCount();
        }
    }

    // An item drawn in the palette, found in the item tree.
    QQuickItem *drawn(const QString &name) const {
        return visibleChildNamed(palette(), name);
    }

    QQuickItem *item(const QString &name) const {
        return root->findChild<QQuickItem *>(name);
    }

    QQuickItem *palette() const {
        return dialog->property("contentItem").value<QQuickItem *>()->parentItem();
    }

    QQuickItem *field() const {
        return item(QStringLiteral("globalSearchQueryField"));
    }

    QQuickItem *view() const {
        return item(QStringLiteral("globalSearchResultsView"));
    }

    QQuickItem *row(int index) const {
        QQuickItem *row = nullptr;
        QMetaObject::invokeMethod(view(), "forceLayout");
        QMetaObject::invokeMethod(view(), "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
        return row;
    }

    int selection() const {
        return controller->property("selectedDisplayIndex").toInt();
    }

    QColor token(const char *name) const {
        return tokens->property(name).value<QColor>();
    }

    // The footer's hints as "Key Action" strings.
    QStringList hints() const {
        QStringList list;
        QQuickItem *footer = item(QStringLiteral("globalSearchFooter"));
        visit(footer, [&](QQuickItem *hint) {
            if (hint->objectName() == QStringLiteral("globalSearchKeyHint") && hint->isVisible()) {
                list << hint->property("keyText").toString() + QLatin1Char(' ') + hint->property("actionText").toString();
            }
        });
        return list;
    }

    // The title of a row, from its runs.
    QString title(QQuickItem *row) const {
        QString text;
        for (QQuickItem *run : visibleNamed(row, QStringLiteral("globalSearchResultTitle"))) {
            text += run->property("text").toString();
        }
        return text;
    }

    QStringList strings(const char *property) const {
        QStringList list;
        for (const QVariant &value : bridge->property(property).toList()) list << value.toString();
        return list;
    }
};

} // namespace

class SearchPaletteTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void paletteSitsUnderTheTransportOverADim_data();
    void paletteSitsUnderTheTransportOverADim();
    void sectionsFollowTheirBestMatch();
    void rowsThatChangeKindRedrawInPlace();
    void tooltipFollowsARowThatChangesKindUnderAStillPointer();
    void tooltipShowsOnlyWhatARowDoesNot();
    void tooltipKeepsClearOfTheField();
    void rowsShareTheirColumnsAndThumbnails();
    void matchesAreEmphasisedInTheTitleOnly();
    void contextSaysWhyARowMatched();
    void selectedHoveredAndPlayingRowsAreDrawn();
    void footerNamesTheKeysForEachState();
    void suggestionIsASelectableRow();
    void recentSearchesShowBeforeTyping();
    void moreRowShowsTheWholeSectionAndKeepsTheSelection();
    void resultsListEndsOnAWholeRow_data();
    void resultsListEndsOnAWholeRow();
    void scrolledResultsTakeANewRowCountWithoutABindingLoop_data();
    void scrolledResultsTakeANewRowCountWithoutABindingLoop();
    void modelNotifiesItsCountAsRowsChange();
    void keyboardSelectsAndActs();
    void mouseSelectsAndActs();
    void menuKeyOpensTheSelectedResultsMenu();
    void resultMenuNamesWhatItActsOn();
    void staleAndBusyResultsDoNotAct();
    void typingKeepsTheResultsInPlaceAsTheyAre();
    void onlyASlowSearchDimsTheResults();
    void reopeningWithKeptTextShowsNoEmptySearch();
    void paletteAndDimFadeTogether();
    void clearUsesTheBreezeIconAndNoTextGlyphs();
    void textMeetsItsContrastAsDrawn_data();
    void textMeetsItsContrastAsDrawn();
    void largeFontRowsAndFooterHoldTheirText_data();
    void largeFontRowsAndFooterHoldTheirText();
    void glyphsTargetsAndThumbnailsFollowTheFont_data();
    void glyphsTargetsAndThumbnailsFollowTheFont();
    void rowColumnsNeverMeet_data();
    void rowColumnsNeverMeet();
    void headingsStartTheirSections_data();
    void headingsStartTheirSections();
    void tooltipStaysOffTheOtherResults_data();
    void tooltipStaysOffTheOtherResults();
    void tooltipFadesWhereItWasAsTheRowsChange();
    void clearGlyphIsQuietUntilPointedAt_data();
    void clearGlyphIsQuietUntilPointedAt();
    void tintsEndAtTheScrollbarsLaneAndTheTextStaysPut();
    void noCoverTileIsTheLibrarys_data();
    void noCoverTileIsTheLibrarys();
};

void SearchPaletteTest::initTestCase() {
    startCapturingMessages();
    QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                     QStringLiteral("icons/breeze/index.theme"))
                  .isEmpty(),
             "The Breeze icon theme is missing: install breeze-icon-theme (docs/INSTALL.md)");
}

void SearchPaletteTest::cleanup() {
    const QStringList list = takeCapturedMessages();
    QVERIFY2(list.isEmpty(), qPrintable(list.join(QLatin1Char('\n'))));
}

void SearchPaletteTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void SearchPaletteTest::paletteSitsUnderTheTransportOverADim_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::newRow("dark 1600") << true << 1600 << 1000;
    QTest::newRow("light 1000") << false << 1000 << 720;
    QTest::newRow("dark narrow") << true << 700 << 500;
}

// 800 px wide (less in a narrow window), centred, 32 px under the
// transport and its seam, on the view colour with a 6 px radius; the field
// 44 px with the glyph at 16 px and the text at 52; the footer 28 px under
// a hairline; and the dim over the whole window.
void SearchPaletteTest::paletteSitsUnderTheTransportOverADim() {
    QFETCH(bool, dark);
    QFETCH(int, width);
    QFETCH(int, height);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, &errorText, width, height), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});

    QQuickItem *palette = harness.palette();
    const QRectF frame = sceneRect(palette);
    QCOMPARE(frame.width(), std::min(800.0, width - 32.0));
    QCOMPARE(frame.left(), std::round((width - frame.width()) / 2));
    QCOMPARE(frame.top(), 48.0 + 1.0 + 32.0);
    QVERIFY(frame.bottom() <= height - 32.0);

    QQuickItem *background = harness.item(QStringLiteral("globalSearchBackground"));
    QVERIFY(background != nullptr);
    QCOMPARE(hex(colorOf(background)), hex(harness.token("view")));
    QCOMPARE(background->property("radius").toReal(), 6.0);
    QCOMPARE(background->property("border").value<QObject *>()->property("width").toReal(), 1.0);
    // Its 1 px outline is the seam, which stands out from both the palette
    // and the dimmed window around it (without it, on dark, the palette
    // all but merged into the window).
    const QColor outline = background->property("border").value<QObject *>()->property("color").value<QColor>();
    QCOMPARE(hex(outline), hex(harness.token("seam")));

    QQuickItem *icon = harness.item(QStringLiteral("globalSearchFieldIcon"));
    QCOMPARE(sceneRect(icon).left() - frame.left(), 16.0);
    QCOMPARE(icon->width(), 16.0);
    QCOMPARE(icon->property("source").toString(), QStringLiteral("search"));
    QCOMPARE(sceneRect(harness.field()).left() - frame.left(), 52.0);
    QCOMPARE(harness.field()->height(), 44.0);
    QCOMPARE(harness.field()->property("font").value<QFont>().pointSizeF(),
             harness.tokens->property("basePointSize").toReal());
    QCOMPARE(hex(colorOf(harness.item(QStringLiteral("globalSearchFieldSeam")))), hex(harness.token("seam")));

    QQuickItem *footer = harness.item(QStringLiteral("globalSearchFooter"));
    QCOMPARE(footer->height(), 28.0);
    QCOMPARE(sceneRect(footer).bottom(), frame.bottom());
    QQuickItem *hairline = harness.item(QStringLiteral("globalSearchFooterHairline"));
    QCOMPARE(hex(colorOf(hairline)), hex(harness.token("hairline")));
    QCOMPARE(sceneRect(hairline).bottom(), sceneRect(footer).top());

    // The dim covers the whole window, the visualization pane too.
    QQuickItem *dim = nullptr;
    visit(harness.window->contentItem(), [&](QQuickItem *item) {
        if (item->objectName() == QStringLiteral("globalSearchDim")) dim = item;
    });
    QVERIFY(dim != nullptr);
    QTRY_COMPARE(dim->opacity(), 1.0);
    QCOMPARE(sceneRect(dim), QRectF(0, 0, width, height));
    QCOMPARE(hex(colorOf(dim)), hex(harness.token("dim")));
    // Stronger on dark, where the window behind is nearly the palette's
    // colour.
    QCOMPARE(qRound(colorOf(dim).alphaF() * 100), dark ? 65 : 25);
    // On dark, the outline parts the palette from the dimmed window, its
    // chrome and its views; on light, the white palette stands well clear
    // of the dimmed window by itself.
    for (const QColor &window : {harness.token("chrome"), harness.token("view")}) {
        const QColor dimmed = drawnOver(colorOf(dim), window);
        if (dark) {
            QVERIFY2(contrastRatio(outline, dimmed) >= 1.5,
                     qPrintable(describeContrast(QStringLiteral("outline"), outline, dimmed)));
            QVERIFY2(contrastRatio(outline, harness.token("view")) >= 1.5,
                     qPrintable(describeContrast(QStringLiteral("outline"), outline, harness.token("view"))));
        } else {
            QVERIFY2(contrastRatio(harness.token("view"), dimmed) >= 1.5,
                     qPrintable(describeContrast(QStringLiteral("palette"), harness.token("view"), dimmed)));
        }
    }
}

// Each type's results under its heading, in the order of the best match of
// each; a 28 px heading, and 36 px ones after a section, in 12 px semibold
// secondary text at the title column, 32 px rows; the first result selected.
void SearchPaletteTest::sectionsFollowTheirBestMatch() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());

    QStringList headings;
    QList<int> headingRows;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        const QVariantMap row = harness.model.rowDataAt(i);
        if (row.value(QStringLiteral("kind")) == QStringLiteral("section")) {
            QQuickItem *rowItem = harness.row(i);
            QVERIFY(rowItem != nullptr);
            QQuickItem *heading = visibleChildNamed(rowItem, QStringLiteral("globalSearchSectionHeading"));
            QVERIFY(heading != nullptr);
            headings << heading->property("text").toString();
            headingRows << i;
            QCOMPARE(rowItem->height(), i == 0 ? 28.0 : 36.0);
            const QFont font = heading->property("font").value<QFont>();
            QCOMPARE(font.weight(), QFont::DemiBold);
            QCOMPARE(font.pointSizeF(), harness.tokens->property("smallPointSize").toReal());
            QCOMPARE(hex(colorOf(heading)), hex(harness.token("textSecondary")));
            QCOMPARE(sceneRect(heading).left() - sceneRect(harness.palette()).left(), 52.0);
            // No rule under it.
            QVERIFY(visibleChildNamed(rowItem, QStringLiteral("rowBackgroundTint")) == nullptr);
        } else {
            QQuickItem *rowItem = harness.row(i);
            QVERIFY(rowItem != nullptr);
            QCOMPARE(rowItem->height(), 32.0);
        }
    }
    QCOMPARE(headings, (QStringList{QStringLiteral("Tracks"), QStringLiteral("Artists"), QStringLiteral("Albums")}));
    QCOMPARE(harness.selection(), 1);
    QCOMPARE(harness.title(harness.row(1)), QStringLiteral("Meridian Drift"));
    // Headings are not selectable.
    QVERIFY(!harness.model.isSelectableIndex(headingRows.first()));

    // A stronger album match moves its section first; on a tie, albums go
    // before tracks, and tracks before artists.
    auto rows = meridianRows(harness.coverUrl);
    rows[1].score = 0.5f;
    harness.search(QStringLiteral("small"), rows);
    QCOMPARE(harness.model.rowDataAt(0).value(QStringLiteral("rowType")).toString(), QStringLiteral("album"));
    QCOMPARE(harness.model.rowDataAt(1).value(QStringLiteral("label")).toString(), QStringLiteral("Small Hours"));
    QVector<Row> tied{result(QStringLiteral("artist"), QStringLiteral("Blue"), 0.0f),
                      result(QStringLiteral("track"), QStringLiteral("Blue"), 0.0f),
                      result(QStringLiteral("album"), QStringLiteral("Blue"), 0.0f)};
    harness.search(QStringLiteral("blue"), tied);
    QStringList order;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("section")) {
            order << harness.model.rowDataAt(i).value(QStringLiteral("rowType")).toString();
        }
    }
    QCOMPARE(order, (QStringList{QStringLiteral("album"), QStringLiteral("track"), QStringLiteral("artist")}));
}

// A query whose rows change kind or type where the old ones were (a track
// result becoming an album, a heading or an artist) updates them in place,
// and each recycled row draws as its new kind. The model used to remove and
// insert such rows for a DelegateChooser, which the palette no longer has.
void SearchPaletteTest::rowsThatChangeKindRedrawInPlace() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    const auto track = [](const QString &title, const QString &path) {
        Row row = result(QStringLiteral("track"), title, 1.0f);
        row.artist = QStringLiteral("The Quiet Meridian");
        row.album = QStringLiteral("Small Hours");
        row.trackPath = path;
        row.lengthText = QStringLiteral("3:49");
        return row;
    };
    harness.search(QStringLiteral("tide"),
                   {track(QStringLiteral("Tide Line"), QStringLiteral("/fixture/line.flac")),
                    track(QStringLiteral("Tide Drift"), QStringLiteral("/fixture/drift.flac")),
                    track(QStringLiteral("Tide Field"), QStringLiteral("/fixture/field.flac"))},
                   {0, 0, 3});
    QTRY_VERIFY(harness.view()->isVisible());
    QCOMPARE(harness.model.rowCount(), 4);
    QList<QPointer<QQuickItem>> before;
    for (int i = 0; i < 4; ++i) {
        before << harness.row(i);
        QVERIFY(before.last() != nullptr);
    }
    QVERIFY(visibleChildNamed(before[1], QStringLiteral("globalSearchCoverPlaceholder")) != nullptr);

    // An album, then an artist: a heading, a result, a heading, a result.
    Row album = result(QStringLiteral("album"), QStringLiteral("Small Hours"), 0.0f);
    album.artist = QStringLiteral("The Quiet Meridian");
    album.year = 2005;
    album.coverUrl = harness.coverUrl;
    Row artist = result(QStringLiteral("artist"), QStringLiteral("The Quiet Meridian"), 1.0f);
    artist.count = 4;
    QSignalSpy removed(&harness.model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy inserted(&harness.model, &QAbstractItemModel::rowsInserted);
    QSignalSpy changed(&harness.model, &QAbstractItemModel::dataChanged);
    harness.search(QStringLiteral("small"), {album, artist}, {1, 1, 0});
    QCOMPARE(harness.model.rowCount(), 4);
    QCOMPARE(removed.count(), 0);
    QCOMPARE(inserted.count(), 0);
    QCOMPARE(changed.count(), 1);

    // The same items, each drawn as its new row.
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(harness.row(i), before[i].data());
    }
    QQuickItem *albumHeading = visibleChildNamed(harness.row(0), QStringLiteral("globalSearchSectionHeading"));
    QVERIFY(albumHeading != nullptr);
    QCOMPARE(albumHeading->property("text").toString(), QStringLiteral("Albums"));

    QQuickItem *albumRow = harness.row(1);
    QCOMPARE(albumRow->height(), 32.0);
    QCOMPARE(harness.title(albumRow), QStringLiteral("Small Hours"));
    QVERIFY(contextText(albumRow).startsWith(QStringLiteral("The Quiet Meridian · ")));
    QVERIFY(visibleChildNamed(albumRow, QStringLiteral("globalSearchThumbnail")) != nullptr);
    QVERIFY(visibleChildNamed(albumRow, QStringLiteral("globalSearchCoverPlaceholder")) == nullptr);
    QVERIFY(visibleChildNamed(albumRow, QStringLiteral("globalSearchResultLength")) == nullptr);

    QQuickItem *artistHeadingRow = harness.row(2);
    QCOMPARE(artistHeadingRow->height(), 36.0);
    QQuickItem *artistHeading = visibleChildNamed(artistHeadingRow, QStringLiteral("globalSearchSectionHeading"));
    QVERIFY(artistHeading != nullptr);
    QCOMPARE(artistHeading->property("text").toString(), QStringLiteral("Artists"));
    QVERIFY(visibleNamed(artistHeadingRow, QStringLiteral("globalSearchResultTitle")).isEmpty());
    QVERIFY(visibleChildNamed(artistHeadingRow, QStringLiteral("globalSearchCoverPlaceholder")) == nullptr);
    QVERIFY(!harness.model.isSelectableIndex(2));

    QQuickItem *artistRow = harness.row(3);
    QCOMPARE(artistRow->height(), 32.0);
    QCOMPARE(harness.title(artistRow), QStringLiteral("The Quiet Meridian"));
    QCOMPARE(contextText(artistRow), QStringLiteral("4 albums"));
    QVERIFY(visibleChildNamed(artistRow, QStringLiteral("globalSearchArtistGlyph")) != nullptr);
    QVERIFY(visibleChildNamed(artistRow, QStringLiteral("globalSearchCoverPlaceholder")) == nullptr);
    QVERIFY(visibleChildNamed(artistRow, QStringLiteral("globalSearchThumbnail")) == nullptr);
    QVERIFY(visibleChildNamed(artistRow, QStringLiteral("globalSearchResultLength")) == nullptr);
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(artistRow);
    QVERIFY(accessible != nullptr);
    QVERIFY2(accessible->text(QAccessible::Name).contains(QStringLiteral("The Quiet Meridian")),
             qPrintable(accessible->text(QAccessible::Name)));
}

// New results that turn the result under a still pointer into a "Show
// all" link take the result's tooltip away, and give it back when the row
// is a result again.
void SearchPaletteTest::tooltipFollowsARowThatChangesKindUnderAStillPointer() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    QVector<Row> tracks;
    for (const QString &title : {QStringLiteral("Tide Line"), QStringLiteral("Tide Drift"), QStringLiteral("Tide Field")}) {
        Row row = result(QStringLiteral("track"), title, 1.0f);
        row.artist = QStringLiteral("The Quiet Meridian");
        row.trackPath = QStringLiteral("/fixture/") + title + QStringLiteral(".flac");
        tracks << row;
    }
    harness.search(QStringLiteral("tide"), tracks, {0, 0, 3});
    QTRY_VERIFY(harness.view()->isVisible());
    QCOMPARE(harness.model.rowCount(), 4);
    QQuickItem *row = harness.row(3);
    QVERIFY(row != nullptr);
    QTest::mouseMove(harness.window, sceneCentre(row));
    QTRY_COMPARE(harness.dialog->property("hoveredResultRow").value<QQuickItem *>(), row);

    // Two of five tracks: the row under the pointer is the "Show all" link.
    harness.search(QStringLiteral("tide"), tracks.mid(0, 2), {0, 0, 5});
    QCOMPARE(harness.model.rowCount(), 4);
    QCOMPARE(harness.model.rowDataAt(3).value(QStringLiteral("kind")).toString(), QStringLiteral("more"));
    QCOMPARE(harness.row(3), row);
    QVERIFY(row->property("hovered").toBool());
    QCOMPARE(harness.dialog->property("hoveredResultRow").value<QQuickItem *>(), nullptr);
    QObject *toolTip = harness.root->findChild<QObject *>(QStringLiteral("globalSearchResultToolTip"));
    QVERIFY(toolTip != nullptr);
    QVERIFY(!toolTip->property("visible").toBool());

    // A result again.
    harness.search(QStringLiteral("tide"), tracks, {0, 0, 3});
    QCOMPARE(harness.row(3), row);
    QCOMPARE(harness.dialog->property("hoveredResultRow").value<QQuickItem *>(), row);
    QTest::mouseMove(harness.window, QPoint(2, 2));
    QTRY_COMPARE(harness.dialog->property("hoveredResultRow").value<QQuickItem *>(), nullptr);
}

// A result has a tooltip only where it adds to the row: the row is
// elided, or there is more to say (its genre, its folder where there are
// several). It repeated the row.
void SearchPaletteTest::tooltipShowsOnlyWhatARowDoesNot() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeLight, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    auto rows = meridianRows(harness.coverUrl);
    for (Row &row : rows) row.rootLabel = QStringLiteral("Shelf");
    rows[1].genre = QStringLiteral("Ambient");
    rows[3].label = QStringLiteral("A Remarkably Long Track Title That Will Certainly Not Fit In Its Column, "
                                   "However Wide the Palette Makes It");
    rows[4].album = QStringLiteral("An Album Whose Name Runs On Well Past the Room Its Column Has for It");
    harness.search(QStringLiteral("meridian"), rows, {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    QObject *toolTip = harness.root->findChild<QObject *>(QStringLiteral("globalSearchResultToolTip"));
    QVERIFY(toolTip != nullptr);
    toolTip->setProperty("delay", 0);
    const auto shows = [&](const QString &label) {
        for (int i = 0; i < harness.model.rowCount(); ++i) {
            if (harness.model.rowDataAt(i).value(QStringLiteral("label")) != label) continue;
            QQuickItem *row = harness.row(i);
            if (row == nullptr) return QStringLiteral("no row");
            QTest::mouseMove(harness.window, sceneCentre(row));
            if (!QTest::qWaitFor([&] {
                    return harness.dialog->property("hoveredResultRow").value<QQuickItem *>() == row;
                })) {
                return QStringLiteral("no hover");
            }
            QTest::qWait(50);
            // At rest: open, or gone once its fade has ended.
            if (!QTest::qWaitFor([&] {
                    return toolTip->property("opened").toBool() || !toolTip->property("visible").toBool();
                })) {
                return QStringLiteral("no rest");
            }
            return toolTip->property("opened").toBool() ? toolTip->property("text").toString() : QString();
        }
        return QStringLiteral("no result");
    };

    // All of the row shows, and one folder's name says nothing.
    QCOMPARE(shows(QStringLiteral("The Quiet Meridian")), QString());
    QCOMPARE(shows(QStringLiteral("Morning Tide Field")), QString());
    // More to say.
    QVERIFY(shows(QStringLiteral("Small Hours")).contains(QStringLiteral("Ambient")));
    QVERIFY(!shows(QStringLiteral("Small Hours")).contains(QStringLiteral("Shelf")));
    // The elided title and the elided context.
    QVERIFY(shows(rows[3].label).contains(rows[3].label));
    QVERIFY(shows(rows[4].label).contains(rows[4].album));
    // The folder, where there are several.
    harness.bridge->setProperty("libraryRootCount", 2);
    QVERIFY(shows(QStringLiteral("The Quiet Meridian")).contains(QStringLiteral("Shelf")));
}

// A result's tooltip is under its row or the palette, or over the row
// where the footer is nearer, and never over the query field (it opened
// over the field), also as it fades out.
void SearchPaletteTest::tooltipKeepsClearOfTheField() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeLight, &errorText, 1000, 560), qPrintable(errorText));
    QVERIFY(harness.open());
    auto rows = meridianRows(harness.coverUrl);
    for (Row &row : rows) row.genre = QStringLiteral("Ambient");
    harness.search(QStringLiteral("meridian"), rows, {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    QObject *toolTip = harness.root->findChild<QObject *>(QStringLiteral("globalSearchResultToolTip"));
    QVERIFY(toolTip != nullptr);
    toolTip->setProperty("delay", 0);
    auto *content = toolTip->property("contentItem").value<QQuickItem *>();
    QVERIFY(content != nullptr);
    const QRectF field = sceneRect(harness.item(QStringLiteral("globalSearchQueryField")));
    const QRectF view = sceneRect(harness.view());
    QList<QQuickItem *> results;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        QQuickItem *row = harness.row(i);
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("item") && row != nullptr
            && sceneRect(row).bottom() <= view.bottom() + 0.5) {
            results << row;
        }
    }
    QVERIFY(results.size() >= 3);
    bool below = false;
    for (QQuickItem *row : std::as_const(results)) {
        QTest::mouseMove(harness.window, sceneCentre(row));
        QTRY_VERIFY(toolTip->property("opened").toBool());
        QTRY_COMPARE(harness.dialog->property("toolTipRow").value<QQuickItem *>(), row);
        const auto tip = [&] { return sceneRect(content->parentItem()); };
        QTRY_VERIFY2(tip().top() >= sceneRect(row).bottom() || tip().bottom() <= sceneRect(row).top(),
                     "the tooltip is over its row");
        QVERIFY2(tip().top() >= field.bottom(), "the tooltip is over the field");
        QVERIFY(tip().bottom() <= harness.window->height());
        if (row == results.first()) below = tip().top() >= sceneRect(row).bottom();
    }
    QVERIFY(below);

    // Back on the first row, then off the list: it fades where it was.
    QTest::mouseMove(harness.window, sceneCentre(results.first()));
    QTRY_COMPARE(harness.dialog->property("toolTipRow").value<QQuickItem *>(), results.first());
    QTRY_VERIFY(toolTip->property("opened").toBool());
    const QRectF shownAt = sceneRect(content->parentItem());
    QTest::mouseMove(harness.window, QPoint(2, 2));
    QTRY_COMPARE(harness.dialog->property("hoveredResultRow").value<QQuickItem *>(), nullptr);
    while (toolTip->property("visible").toBool() && content->parentItem()->opacity() > 0.2) {
        QVERIFY2(std::abs(sceneRect(content->parentItem()).top() - shownAt.top()) < 12, "the fading tooltip moved");
        QVERIFY2(sceneRect(content->parentItem()).top() >= field.bottom(), "the fading tooltip is over the field");
        QTest::qWait(8);
    }
}

// The lead at 16 px (a 24 px thumbnail with a 2 px radius, the artist
// glyph, or the no-cover tile), the title at 52 px in a 380 px
// column, and the context at one x for every row, whatever its title.
void SearchPaletteTest::rowsShareTheirColumnsAndThumbnails() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeLight, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    auto rows = meridianRows(harness.coverUrl);
    rows[3].label = QStringLiteral("A Remarkably Long Track Title That Will Certainly Not Fit In Its Column");
    harness.search(QStringLiteral("meridian"), rows, {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    const qreal left = sceneRect(harness.palette()).left();

    QList<qreal> contextXs;
    int thumbnails = 0;
    int placeholders = 0;
    int artistGlyphs = 0;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) != QStringLiteral("item")) continue;
        QQuickItem *row = harness.row(i);
        QVERIFY(row != nullptr);
        const auto runs = visibleNamed(row, QStringLiteral("globalSearchResultTitle"));
        QVERIFY(!runs.isEmpty());
        QCOMPARE(sceneRect(runs.first()).left() - left, 52.0);
        QVERIFY(sceneRect(runs.last()).right() - left <= 52.0 + 380.0 + 0.5);
        const auto context = contextTexts(row);
        QVERIFY(!context.isEmpty());
        contextXs << contextBoxOf(row).left() - left;
        for (QQuickItem *text : context) {
            QCOMPARE(text->property("font").value<QFont>().pointSizeF(), harness.tokens->property("smallPointSize").toReal());
        }
        if (QQuickItem *thumbnail = visibleChildNamed(row, QStringLiteral("globalSearchThumbnail"))) {
            ++thumbnails;
            QCOMPARE(sceneRect(thumbnail), QRectF(left + 16, sceneRect(row).top() + 4, 24, 24));
            QCOMPARE(thumbnail->property("radius").toReal(), 2.0);
        }
        if (QQuickItem *glyph = visibleChildNamed(row, QStringLiteral("globalSearchCoverPlaceholder"))) {
            ++placeholders;
            QCOMPARE(sceneRect(glyph).center().x() - left, 28.0);
        }
        if (QQuickItem *glyph = visibleChildNamed(row, QStringLiteral("globalSearchArtistGlyph"))) {
            ++artistGlyphs;
            QCOMPARE(glyph->property("source").toString(), QStringLiteral("view-media-artist"));
            QCOMPARE(contextText(row), QStringLiteral("4 albums"));
        }
        const QString type = harness.model.rowDataAt(i).value(QStringLiteral("rowType")).toString();
        if (type == QStringLiteral("track")) {
            QQuickItem *length = visibleChildNamed(row, QStringLiteral("globalSearchResultLength"));
            QVERIFY(length != nullptr);
            // 16 px in, and clear of the scrollbar's lane whether it shows
            // or not.
            QCOMPARE(sceneRect(length).right(), sceneRect(row).right() - 16 - 8);
        }
        if (type == QStringLiteral("album")) {
            QVERIFY(contextText(row).startsWith(QStringLiteral("The Quiet Meridian · ")));
        }
    }
    QCOMPARE(contextXs.size(), 7);
    QVERIFY(std::all_of(contextXs.cbegin(), contextXs.cend(), [](qreal x) { return x == 444.0; }));
    QCOMPARE(thumbnails, 5);
    QCOMPARE(placeholders, 1);
    QCOMPARE(artistGlyphs, 1);

    // The long title elides inside its column.
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (!harness.model.rowDataAt(i).value(QStringLiteral("label")).toString().startsWith(QStringLiteral("A Remarkably"))) continue;
        QQuickItem *row = harness.row(i);
        const auto runs = visibleNamed(row, QStringLiteral("globalSearchResultTitle"));
        QVERIFY(std::any_of(runs.cbegin(), runs.cend(), [](QQuickItem *run) { return run->property("truncated").toBool(); }));
    }
}

// "Meridian Line" for "meridian": "Meridian" semibold, " Line" regular; the
// context has no emphasis. A row emphasises its matches in its title only
// (the context of an album found by its artist had its match semibold,
// and outweighed the title).
void SearchPaletteTest::matchesAreEmphasisedInTheTitleOnly() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    const auto rowLabelled = [&harness](const QString &label) -> QQuickItem * {
        for (int i = 0; i < harness.model.rowCount(); ++i) {
            if (harness.model.rowDataAt(i).value(QStringLiteral("label")) == label) return harness.row(i);
        }
        return nullptr;
    };
    // The semibold texts of `name` in `row`.
    const auto emphasisedIn = [](QQuickItem *row, const QString &name) {
        QStringList texts;
        for (QQuickItem *run : visibleNamed(row, name)) {
            if (run->property("font").value<QFont>().weight() == QFont::DemiBold) texts << run->property("text").toString();
        }
        return texts;
    };
    const QString titleRun = QStringLiteral("globalSearchResultTitle");
    // The row's context: one text, whole and regular.
    const auto wholeContext = [](QQuickItem *row) -> QString {
        const QList<QQuickItem *> texts = contextTexts(row);
        if (texts.size() != 1 || texts.first()->property("font").value<QFont>().weight() != QFont::Normal) return {};
        return contextWords(texts.first());
    };

    QQuickItem *row = rowLabelled(QStringLiteral("Meridian Line"));
    QVERIFY(row != nullptr);
    const auto runs = visibleNamed(row, titleRun);
    QCOMPARE(runs.size(), 2);
    QCOMPARE(runs[0]->property("text").toString(), QStringLiteral("Meridian"));
    QCOMPARE(runs[0]->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(runs[1]->property("text").toString(), QStringLiteral(" Line"));
    QCOMPARE(runs[1]->property("font").value<QFont>().weight(), QFont::Normal);
    // The second run starts where the first ends: the space stays.
    QVERIFY(sceneRect(runs[1]).left() > sceneRect(runs[0]).left() + 40);
    QCOMPARE(runs[0]->property("textFormat").toInt(), int(Qt::PlainText));

    // Its context matches too, but the title has the emphasis: the context
    // is whole and regular.
    QQuickItem *context = visibleChildNamed(row, QStringLiteral("globalSearchResultContext"));
    QVERIFY(context != nullptr);
    QCOMPARE(context->property("text").toString(), QStringLiteral("The Quiet Meridian — Grey Lines"));
    QCOMPARE(context->property("font").value<QFont>().weight(), QFont::Normal);
    QCOMPARE(wholeContext(row), QStringLiteral("The Quiet Meridian — Grey Lines"));

    // An album found by its artist: its title has no match, so nothing is
    // emphasised; its context, which has the match, stays whole and
    // regular, where the context starts, in the context's size and colour.
    row = rowLabelled(QStringLiteral("Small Hours"));
    QVERIFY(row != nullptr);
    QVERIFY(emphasisedIn(row, titleRun).isEmpty());
    QCOMPARE(wholeContext(row), QStringLiteral("The Quiet Meridian · 2005"));
    context = visibleChildNamed(row, QStringLiteral("globalSearchResultContext"));
    QCOMPARE(sceneRect(context).left() - sceneRect(harness.palette()).left(), 444.0);
    QCOMPARE(context->property("font").value<QFont>().pointSizeF(), harness.tokens->property("smallPointSize").toReal());
    QCOMPARE(hex(colorOf(context)), hex(harness.token("textSecondary")));

    // Two matches in a context: still whole and regular.
    harness.search(QStringLiteral("quiet small"), meridianRows(harness.coverUrl));
    row = rowLabelled(QStringLiteral("Lantern Signal"));
    QVERIFY(row != nullptr);
    QVERIFY(emphasisedIn(row, titleRun).isEmpty());
    QCOMPARE(wholeContext(row), QStringLiteral("The Quiet Meridian — Small Hours"));
    // A title that matches keeps every one of its matches, and its context
    // none.
    row = rowLabelled(QStringLiteral("Small Hours"));
    QVERIFY(row != nullptr);
    QCOMPARE(emphasisedIn(row, titleRun), QStringList{QStringLiteral("Small")});
    QVERIFY(!wholeContext(row).isEmpty());

    // Two words, two matches; the rest regular.
    harness.search(QStringLiteral("quiet line"), meridianRows(harness.coverUrl));
    row = rowLabelled(QStringLiteral("The Quiet Meridian"));
    QVERIFY(row != nullptr);
    QCOMPARE(emphasisedIn(row, titleRun), QStringList{QStringLiteral("Quiet")});
    QVERIFY(!wholeContext(row).isEmpty());

    // The playing track's title is all semibold: its context stays whole.
    harness.bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/lantern.flac"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    harness.search(QStringLiteral("quiet"), meridianRows(harness.coverUrl));
    row = rowLabelled(QStringLiteral("Lantern Signal"));
    QVERIFY(row != nullptr);
    QVERIFY(!wholeContext(row).isEmpty());
}

// A row whose title has no match says why it was found: the matched run of
// its context in the text colour, as regular as the rest (12 of 13 rows for
// "mer", found by their artist, showed no match at all). Metadata that
// looks like markup stays text.
void SearchPaletteTest::contextSaysWhyARowMatched() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    const QColor text = harness.token("text");

    auto rows = meridianRows(harness.coverUrl);
    rows[0].count = 2;
    // Quotes in a title are no match.
    rows[6].label = QStringLiteral("The \"Lantern\" Signal");
    // Metadata that looks like markup, and a match at the context's start.
    rows[5].artist = QStringLiteral("Meridian <b>&amp; Sons</b> <img src=x>");
    harness.search(QStringLiteral("mer"), rows, {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    const auto rowLabelled = [&harness](const QString &label) -> QQuickItem * {
        for (int i = 0; i < harness.model.rowCount(); ++i) {
            if (harness.model.rowDataAt(i).value(QStringLiteral("label")) == label) return harness.row(i);
        }
        return nullptr;
    };

    for (const QString &label : {QStringLiteral("Small Hours"), QStringLiteral("The \"Lantern\" Signal")}) {
        QQuickItem *row = rowLabelled(label);
        QVERIFY(row != nullptr);
        QCOMPARE(contextRunsIn(row, text), QStringList{QStringLiteral("Mer")});
        QQuickItem *context = contextTexts(row).first();
        QCOMPARE(context->property("font").value<QFont>().weight(), QFont::Normal);
        QCOMPARE(hex(colorOf(context)), hex(harness.token("textSecondary")));
        // The words and the spaces between them keep their room.
        const QFontMetricsF metrics(context->property("font").value<QFont>());
        const qreal plain = metrics.horizontalAdvance(contextWords(context));
        QVERIFY2(std::abs(context->implicitWidth() - plain) <= 3,
                 qPrintable(QStringLiteral("%1 for %2").arg(context->implicitWidth()).arg(plain)));
    }
    QQuickItem *row = rowLabelled(QStringLiteral("Morning Tide Field"));
    QVERIFY(row != nullptr);
    QCOMPARE(contextRunsIn(row, text), QStringList{QStringLiteral("Mer")});
    QCOMPARE(contextText(row), QStringLiteral("Meridian <b>&amp; Sons</b> <img src=x> — Small Hours"));
    QVERIFY(!contextTexts(row).first()->property("text").toString().contains(QStringLiteral("<img")));
    QVERIFY(!contextTexts(row).first()->property("text").toString().contains(QStringLiteral("<b>")));

    // A title that has the match keeps it: its context stays plain.
    row = rowLabelled(QStringLiteral("Meridian Line"));
    QVERIFY(row != nullptr);
    QVERIFY(contextRunsIn(row, text).isEmpty());

    // As drawn: the run is brighter than the rest of the context.
    row = rowLabelled(QStringLiteral("Small Hours"));
    QVERIFY(row != nullptr);
    const auto brightestIn = [&](const QString &before, const QString &run) {
        QQuickItem *context = contextTexts(row).first();
        const QFontMetricsF metrics(context->property("font").value<QFont>());
        const QRectF box = sceneRect(context);
        const QImage image = harness.window->grabWindow();
        const qreal scale = static_cast<qreal>(image.width()) / harness.window->width();
        const qreal from = box.left() + metrics.horizontalAdvance(before);
        int brightest = 0;
        for (qreal x = from + 1; x < from + metrics.horizontalAdvance(run) - 1; x += 0.5) {
            for (qreal y = box.top(); y < box.bottom(); y += 0.5) {
                brightest = std::max(brightest, qGray(image.pixel(qFloor(x * scale), qFloor(y * scale))));
            }
        }
        return brightest;
    };
    QTRY_VERIFY(brightestIn(QStringLiteral("The Quiet "), QStringLiteral("Mer"))
                > brightestIn(QString(), QStringLiteral("The Quiet")) + 30);

    // What the search doesn't read is never a match: an artist's count.
    harness.search(QStringLiteral("album"), rows, {1, 2, 4});
    row = rowLabelled(QStringLiteral("The Quiet Meridian"));
    QVERIFY(row != nullptr);
    QVERIFY(contextRunsIn(row, text).isEmpty());
    QCOMPARE(contextText(row), QStringLiteral("2 albums"));

    // Escaped without a match too, and as rows are reused: a tag that
    // reached the markup warns, which cleanup() catches.
    harness.search(QStringLiteral("tide"), rows, {1, 2, 4});
    row = rowLabelled(QStringLiteral("Morning Tide Field"));
    QVERIFY(row != nullptr);
    QVERIFY(contextRunsIn(row, text).isEmpty());
    QCOMPARE(contextText(row), QStringLiteral("Meridian <b>&amp; Sons</b> <img src=x> — Small Hours"));
    QVERIFY(!contextTexts(row).first()->property("text").toString().contains(QStringLiteral("<img")));
}

// Selected: the selection tint; hovered: the hover tint, which differs from
// it; playing: the amber glyph over the thumbnail and an amber semibold
// title; paused: the pause glyph; stopped: neither.
void SearchPaletteTest::selectedHoveredAndPlayingRowsAreDrawn() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());

    QQuickItem *selected = harness.row(harness.selection());
    QQuickItem *tint = visibleChildNamed(selected, QStringLiteral("rowBackgroundTint"));
    QVERIFY(tint != nullptr);
    QCOMPARE(hex(colorOf(tint)), hex(harness.token("selection")));
    QCOMPARE(tint->property("border").value<QObject *>()->property("width").toReal(), 0.0);
    // The selected tint spans the palette when no scrollbar shows.
    QCOMPARE(sceneRect(tint).width(), sceneRect(harness.palette()).width());

    QQuickItem *other = harness.row(harness.selection() + 1);
    QQuickItem *hover = nullptr;
    visit(other, [&](QQuickItem *item) {
        if (item->objectName() == QStringLiteral("rowBackgroundHover")) hover = item;
    });
    QVERIFY(hover != nullptr);
    QVERIFY(!hover->isVisible());
    {
        // The hover's fade in animation time, to its end.
        SteppedAnimationDriver animations;
        QTest::mouseMove(harness.window, sceneCentre(other));
        (void)animations.step(1000);
        QCOMPARE(hover->opacity(), 1.0);
    }
    const QColor view = harness.token("view");
    const QColor hoverDrawn = drawnOver(colorOf(hover), view);
    const QColor selectionDrawn = harness.token("selection");
    QVERIFY2(hoverDrawn != selectionDrawn, qPrintable(describeContrast(QStringLiteral("hover vs selection"), hoverDrawn, selectionDrawn)));
    // Visibly different: neither reads as the other.
    QVERIFY2(std::abs(relativeLuminance(hoverDrawn) - relativeLuminance(selectionDrawn)) > 0.005
                 || std::abs(hoverDrawn.hueF() - selectionDrawn.hueF()) > 0.1,
             qPrintable(describeContrast(QStringLiteral("hover vs selection"), hoverDrawn, selectionDrawn)));
    QVERIFY(visibleChildNamed(other, QStringLiteral("rowBackgroundTint")) == nullptr);
    QTest::mouseMove(harness.window, QPoint(2, 2));

    // Playing.
    int morning = -1;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("label")) == QStringLiteral("Morning Tide Field")) morning = i;
    }
    QVERIFY(morning >= 0);
    QQuickItem *row = harness.row(morning);
    QVERIFY(visibleChildNamed(row, QStringLiteral("globalSearchNowGlyph")) == nullptr);
    harness.bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/morning.flac"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QQuickItem *glyph = visibleChildNamed(row, QStringLiteral("globalSearchNowGlyph"));
    QVERIFY(glyph != nullptr);
    QCOMPARE(glyph->property("source").toString(), QStringLiteral("media-playback-start"));
    QCOMPARE(hex(glyph->property("color").value<QColor>()), hex(harness.token("now")));
    QCOMPARE(sceneRect(glyph).center(), sceneRect(visibleChildNamed(row, QStringLiteral("globalSearchThumbnail"))).center());
    QVERIFY(visibleChildNamed(row, QStringLiteral("globalSearchNowScrim")) != nullptr);
    for (QQuickItem *run : visibleNamed(row, QStringLiteral("globalSearchResultTitle"))) {
        QCOMPARE(hex(colorOf(run)), hex(harness.token("now")));
        QCOMPARE(run->property("font").value<QFont>().weight(), QFont::DemiBold);
    }
    // Only that row.
    QVERIFY(visibleChildNamed(harness.row(harness.selection()), QStringLiteral("globalSearchNowGlyph")) == nullptr);
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QCOMPARE(glyph->property("source").toString(), QStringLiteral("media-playback-pause"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QVERIFY(visibleChildNamed(row, QStringLiteral("globalSearchNowGlyph")) == nullptr);
    for (QQuickItem *run : visibleNamed(row, QStringLiteral("globalSearchResultTitle"))) {
        QCOMPARE(hex(colorOf(run)), hex(harness.token("text")));
    }
}

// The hints name what the keys do for the selected row: a result's
// actions, "Show All" for a more row, "Search" for a recent search,
// "Search Instead" for a suggestion, and only Esc otherwise. The key in the
// text colour, the action in the secondary one; 6 px from a key to its
// action, more than a word space, and 18 px between pairs (at 4 px, a word
// space, each pair ran together into one phrase).
void SearchPaletteTest::footerNamesTheKeysForEachState() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    // Before typing, with no recent searches: the field and the footer,
    // with only the field's seam between them.
    QTRY_COMPARE(harness.hints(), QStringList{QStringLiteral("Esc Close")});
    QVERIFY(!harness.item(QStringLiteral("globalSearchFooterHairline"))->isVisible());
    QCOMPARE(sceneRect(harness.palette()).height(), 44.0 + 1 + 1 + 28);

    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 26});
    QTRY_COMPARE(harness.hints(), (QStringList{QStringLiteral("Enter Play Track"), QStringLiteral("Ctrl+Enter Add to Queue"),
                                               QStringLiteral("Alt+Enter Show in Library"), QStringLiteral("Esc Close")}));
    const auto selectLabel = [&](const QString &label) {
        for (int i = 0; i < harness.model.rowCount(); ++i) {
            if (harness.model.rowDataAt(i).value(QStringLiteral("label")) == label) {
                QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, i));
            }
        }
    };
    selectLabel(QStringLiteral("The Quiet Meridian"));
    QTRY_COMPARE(harness.hints().first(), QStringLiteral("Enter Play Artist"));
    selectLabel(QStringLiteral("Grey Lines"));
    QTRY_COMPARE(harness.hints().first(), QStringLiteral("Enter Play Album"));
    // The more row at the end of the tracks.
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("more")) {
            QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, i));
        }
    }
    QTRY_COMPARE(harness.hints(), (QStringList{QStringLiteral("Enter Show All"), QStringLiteral("Esc Close")}));

    // The pairs' colours and spacing.
    QQuickItem *footer = harness.item(QStringLiteral("globalSearchFooter"));
    const auto keys = visibleNamed(footer, QStringLiteral("globalSearchKeyHintKey"));
    const auto actions = visibleNamed(footer, QStringLiteral("globalSearchKeyHintAction"));
    QCOMPARE(keys.size(), 2);
    QCOMPARE(hex(colorOf(keys.first())), hex(harness.token("text")));
    QCOMPARE(hex(colorOf(actions.first())), hex(harness.token("textSecondary")));
    QCOMPARE(keys.first()->property("font").value<QFont>().pointSizeF(), harness.tokens->property("smallPointSize").toReal());
    QTRY_COMPARE(sceneRect(keys.last()).left() - sceneRect(actions.first()).right(), 18.0);
    QCOMPARE(sceneRect(actions.first()).left() - sceneRect(keys.first()).right(), 6.0);
    QCOMPARE(sceneRect(actions.last()).left() - sceneRect(keys.last()).right(), 6.0);
    {
        const QFontMetricsF small(actions.first()->property("font").value<QFont>());
        QVERIFY2(6.0 >= 1.4 * small.horizontalAdvance(QLatin1Char(' ')),
                 qPrintable(QString::number(small.horizontalAdvance(QLatin1Char(' ')))));
    }
    QCOMPARE(sceneRect(keys.first()).left() - sceneRect(harness.palette()).left(), 52.0);

    // No results.
    harness.search(QStringLiteral("zzzz"), {});
    QTRY_COMPARE(harness.hints(), QStringList{QStringLiteral("Esc Close")});
}

// Nothing found, with a correction: "No matches for “merdian”", then
// "Search ‘meridian’ instead" as a selected row with the selection tint;
// Enter, or a click, searches the correction.
void SearchPaletteTest::suggestionIsASelectableRow() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeLight, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl));
    harness.bridge->setProperty("globalSearchSuggestion", QStringLiteral("meridian"));
    harness.search(QStringLiteral("merdian"), {});
    QQuickItem *title = harness.item(QStringLiteral("globalSearchNoResultsTitle"));
    QTRY_VERIFY(title->isVisible());
    QCOMPARE(title->property("text").toString(), QStringLiteral("No matches for “merdian”"));
    QCOMPARE(sceneRect(title).left() - sceneRect(harness.palette()).left(), 52.0);
    QVERIFY(!harness.view()->isVisible());
    QQuickItem *row = harness.drawn(QStringLiteral("globalSearchSuggestionRow"));
    QVERIFY(row != nullptr);
    QCOMPARE(row->height(), 32.0);
    QCOMPARE(visibleChildNamed(row, QStringLiteral("globalSearchAuxTitle"))->property("text").toString(),
             QStringLiteral("Search ‘meridian’ instead"));
    QQuickItem *tint = visibleChildNamed(row, QStringLiteral("rowBackgroundTint"));
    QVERIFY(tint != nullptr);
    QCOMPARE(hex(colorOf(tint)), hex(harness.token("selection")));
    QCOMPARE(harness.hints(), (QStringList{QStringLiteral("Enter Search Instead"), QStringLiteral("Esc Close")}));
    // 8 px between the row and the footer's hairline.
    QTRY_COMPARE(sceneRect(harness.item(QStringLiteral("globalSearchFooterHairline"))).top() - sceneRect(row).bottom(), 8.0);
    // Ctrl+Enter and Alt+Enter do nothing here.
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::AltModifier);
    QCOMPARE(harness.field()->property("text").toString(), QStringLiteral("merdian"));
    QTest::keyClick(harness.window, Qt::Key_Return);
    QCOMPARE(harness.field()->property("text").toString(), QStringLiteral("meridian"));
    QCOMPARE(harness.strings("queries").last(), QStringLiteral("meridian"));
    QVERIFY(harness.dialog->property("visible").toBool());
    QVERIFY(harness.field()->hasActiveFocus());

    // Without a correction: a hint, nothing selectable.
    harness.bridge->setProperty("globalSearchSuggestion", QString());
    harness.search(QStringLiteral("zzzz"), {});
    QTRY_VERIFY(harness.item(QStringLiteral("globalSearchNoResultsHint"))->isVisible());
    QVERIFY(harness.drawn(QStringLiteral("globalSearchSuggestionRow")) == nullptr);

    // A click on the suggestion.
    harness.bridge->setProperty("globalSearchSuggestion", QStringLiteral("lantern"));
    harness.search(QStringLiteral("lantren"), {});
    QQuickItem *clickRow = nullptr;
    QTRY_VERIFY((clickRow = harness.drawn(QStringLiteral("globalSearchSuggestionRow"))) != nullptr);
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(clickRow));
    QCOMPARE(harness.field()->property("text").toString(), QStringLiteral("lantern"));
}

// The searches whose results were used show before typing, newest first;
// Enter searches the selected one again.
void SearchPaletteTest::recentSearchesShowBeforeTyping() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl));
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    harness.search(QStringLiteral("lantern"), meridianRows(harness.coverUrl));
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    harness.search(QStringLiteral("Meridian"), meridianRows(harness.coverUrl));
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(harness.controller->property("recentQueries").toStringList(),
             (QStringList{QStringLiteral("Meridian"), QStringLiteral("lantern")}));

    harness.field()->setProperty("text", QString());
    QQuickItem *heading = harness.item(QStringLiteral("globalSearchRecentHeading"));
    QTRY_VERIFY(heading->isVisible());
    QCOMPARE(heading->property("text").toString(), QStringLiteral("Recent Searches"));
    QVERIFY(harness.item(QStringLiteral("globalSearchFooterHairline"))->isVisible());
    const auto rows = visibleNamed(harness.palette(), QStringLiteral("globalSearchRecentRow"));
    QCOMPARE(rows.size(), 2);
    QVERIFY(visibleChildNamed(rows[0], QStringLiteral("rowBackgroundTint")) != nullptr);
    QCOMPARE(harness.hints(), (QStringList{QStringLiteral("Enter Search"), QStringLiteral("Esc Close")}));
    QTest::keyClick(harness.window, Qt::Key_Down);
    QVERIFY(visibleChildNamed(rows[1], QStringLiteral("rowBackgroundTint")) != nullptr);
    QTest::keyClick(harness.window, Qt::Key_Down); // Wraps.
    QVERIFY(visibleChildNamed(rows[0], QStringLiteral("rowBackgroundTint")) != nullptr);
    QTest::keyClick(harness.window, Qt::Key_Up);
    QTest::keyClick(harness.window, Qt::Key_Return);
    QCOMPARE(harness.field()->property("text").toString(), QStringLiteral("lantern"));
}

// "Show all 26 tracks" ends a section that shows fewer; it is selectable,
// Enter or a click expands that section, and the selection moves to the
// first track it revealed. At the most a search shows, a note says how many
// show.
void SearchPaletteTest::moreRowShowsTheWholeSectionAndKeepsTheSelection() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 26});
    QTRY_VERIFY(harness.view()->isVisible());
    int moreIndex = -1;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("more")) moreIndex = i;
    }
    QVERIFY(moreIndex > 0);
    QCOMPARE(harness.model.rowDataAt(moreIndex - 1).value(QStringLiteral("rowType")).toString(), QStringLiteral("track"));
    QQuickItem *more = harness.row(moreIndex);
    QQuickItem *link = visibleChildNamed(more, QStringLiteral("globalSearchMoreLink"));
    QVERIFY(link != nullptr);
    QCOMPARE(link->property("text").toString(), QStringLiteral("Show all 26 tracks"));
    QCOMPARE(hex(colorOf(link)), hex(harness.token("link")));
    QCOMPARE(more->height(), 28.0);

    // Hover underlines the link; only results take the hover tint.
    QTest::mouseMove(harness.window, sceneCentre(more));
    QTRY_VERIFY(link->property("font").value<QFont>().underline());
    QQuickItem *moreHover = nullptr;
    visit(more, [&](QQuickItem *item) {
        if (item->objectName() == QStringLiteral("rowBackgroundHover")) moreHover = item;
    });
    QVERIFY(moreHover != nullptr && !moreHover->isVisible());
    QTest::mouseMove(harness.window, QPoint(2, 2));

    // Keyboard: select it and press Enter.
    QVERIFY(harness.model.isSelectableIndex(moreIndex));
    QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, moreIndex));
    QCOMPARE(hex(colorOf(link)), hex(harness.token("text")));
    QTest::keyClick(harness.window, Qt::Key_Return);
    QCOMPARE(harness.strings("expanded"), QStringList{QStringLiteral("track")});
    QVERIFY(harness.dialog->property("visible").toBool());

    // The expanded results arrive: the fifth track is selected.
    auto rows = meridianRows(harness.coverUrl);
    for (int i = 0; i < 22; ++i) {
        Row extra = result(QStringLiteral("track"), QStringLiteral("Extra %1").arg(i, 2, 10, QLatin1Char('0')), 5.0f);
        extra.trackPath = QStringLiteral("/fixture/extra%1.flac").arg(i);
        rows << extra;
    }
    harness.model.presentSearchRows(rows, {1, 2, 26}, true);
    harness.settleRows();
    const QVariantMap selected = harness.model.rowDataAt(harness.selection());
    QCOMPARE(selected.value(QStringLiteral("label")).toString(), QStringLiteral("Extra 00"));
    QCOMPARE(harness.model.indexOfResult(QStringLiteral("track"), 4), harness.selection());

    // Mouse: a click on the link expands.
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {12, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("more")) moreIndex = i;
    }
    QQuickItem *artistMore = harness.row(moreIndex);
    QCOMPARE(visibleChildNamed(artistMore, QStringLiteral("globalSearchMoreLink"))->property("text").toString(),
             QStringLiteral("Show all 12 artists"));
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(artistMore));
    QCOMPARE(harness.strings("expanded").last(), QStringLiteral("artist"));

    // At the most: a note, not selectable.
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 5312}, false);
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("truncated")) moreIndex = i;
    }
    QVERIFY(!harness.model.isSelectableIndex(moreIndex));
    QQuickItem *note = visibleChildNamed(harness.row(moreIndex), QStringLiteral("globalSearchTruncatedNote"));
    QVERIFY(note != nullptr);
    QCOMPARE(note->property("text").toString(),
             QStringLiteral("Showing 4 of 5,312 tracks. Add words to narrow the search."));
    QCOMPARE(hex(colorOf(note)), hex(harness.token("textSecondary")));
}

void SearchPaletteTest::resultsListEndsOnAWholeRow_data() {
    QTest::addColumn<int>("windowHeight");
    QTest::newRow("1000 px") << 1000;
    QTest::newRow("760 px") << 760;
    QTest::newRow("701 px") << 701;
    QTest::newRow("640 px") << 640;
}

// With more results than the palette has room for, the list is as tall as
// the rows that fit it whole, from the first, and the rest scroll (as tall
// as its room, it ended in half a row). The rows are
// of two heights, results and headings, so the height is no multiple of
// either. With room for them all, they all show, with 6 px under the last.
void SearchPaletteTest::resultsListEndsOnAWholeRow() {
    QFETCH(int, windowHeight);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText, 1600, windowHeight), qPrintable(errorText));
    QVERIFY(harness.open());
    auto rows = meridianRows(harness.coverUrl);
    for (int i = 0; i < 22; ++i) {
        Row extra = result(QStringLiteral("track"), QStringLiteral("Extra %1").arg(i, 2, 10, QLatin1Char('0')), 5.0f);
        extra.trackPath = QStringLiteral("/fixture/extra%1.flac").arg(i);
        rows << extra;
    }
    harness.search(QStringLiteral("meridian"), rows, {1, 2, 26});
    QTRY_VERIFY(harness.view()->isVisible());
    harness.model.presentSearchRows(rows, {1, 2, 26}, true);
    harness.settleRows();
    QQuickItem *view = harness.view();
    QQuickItem *footer = harness.item(QStringLiteral("globalSearchFooter"));
    QVERIFY(footer != nullptr);

    // Each row's height, by its kind.
    QList<qreal> heights;
    qreal all = 0;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        const QString kind = harness.model.rowDataAt(i).value(QStringLiteral("kind")).toString();
        heights << rowHeightFor(kind, i);
        all += heights.last();
    }
    QVERIFY2(all > 16 * 32.0, qPrintable(QString::number(all)));
    // The room: 16 results' height at the most, and the window's height
    // less the palette's top, 32 px under it, the field, the footer and
    // their two seams.
    const qreal paletteTop = sceneRect(harness.palette()).top();
    const qreal room = std::min(16 * 32.0, windowHeight - paletteTop - 32 - 44 - 2 - footer->height());
    qreal whole = 0;
    int shown = 0;
    while (shown < heights.size() && whole + heights.at(shown) <= room) {
        whole += heights.at(shown);
        ++shown;
    }
    QVERIFY2(shown > 2 && shown < heights.size(), qPrintable(QStringLiteral("%1 rows in %2 px").arg(shown).arg(room)));
    QTRY_COMPARE(view->height(), whole);
    // The rest scroll under the app's lane scrollbar, as the library's and
    // the queue's (Breeze's own was another hand): 8 px at the
    // view's right edge, its thumb alone in that lane: the rows' tints end
    // there and their text keeps clear of it.
    QQuickItem *scrollBar = harness.item(QStringLiteral("globalSearchScrollBar"));
    QVERIFY(scrollBar != nullptr);
    QTRY_VERIFY(scrollBar->isVisible());
    QCOMPARE(scrollBar->width(), 8.0);
    QCOMPARE(sceneRect(scrollBar).right(), sceneRect(view).right());
    QVERIFY(visibleChildNamed(scrollBar, QStringLiteral("laneScrollBarThumb")) != nullptr);
    QCOMPARE(view->property("scrollbarLane").toReal(), 8.0);
    QQuickItem *selectedRow = harness.row(harness.selection());
    QVERIFY(selectedRow != nullptr);
    QQuickItem *selectedTint = visibleChildNamed(selectedRow, QStringLiteral("rowBackgroundTint"));
    QVERIFY(selectedTint != nullptr);
    QCOMPARE(sceneRect(selectedTint).right(), sceneRect(scrollBar).left());
    QQuickItem *length = visibleChildNamed(selectedRow, QStringLiteral("globalSearchResultLength"));
    if (length != nullptr) {
        QVERIFY(sceneRect(length).right() <= sceneRect(scrollBar).left() - 16 + 0.5);
    }
    // The footer's seam follows the last whole row.
    QTRY_COMPARE(sceneRect(footer).top() - 1, sceneRect(view).bottom());
    QQuickItem *last = harness.row(shown - 1);
    QVERIFY(last != nullptr);
    QTRY_COMPARE(sceneRect(last).bottom(), sceneRect(view).bottom());
    // No row with an item is cut at either end of the view.
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        QQuickItem *item = harness.row(i);
        if (item == nullptr || !item->isVisible() || item->height() <= 0) {
            continue;
        }
        const QRectF rect = sceneRect(item);
        const bool inside = rect.top() >= sceneRect(view).top() - 0.5 && rect.bottom() <= sceneRect(view).bottom() + 0.5;
        const bool outside = rect.top() >= sceneRect(view).bottom() - 0.5 || rect.bottom() <= sceneRect(view).top() + 0.5;
        QVERIFY2(inside || outside, qPrintable(QStringLiteral("row %1 at %2..%3 in %4..%5")
                                                   .arg(i).arg(rect.top()).arg(rect.bottom())
                                                   .arg(sceneRect(view).top()).arg(sceneRect(view).bottom())));
    }

    // A few results: all of them, and 6 px under the last.
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});
    qreal few = 0;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        const QString kind = harness.model.rowDataAt(i).value(QStringLiteral("kind")).toString();
        few += rowHeightFor(kind, i);
    }
    if (few + 6 <= room) {
        QTRY_COMPARE(view->height(), few + 6);
    }
}

void SearchPaletteTest::scrolledResultsTakeANewRowCountWithoutABindingLoop_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowHeight");
    QTest::newRow("default font") << 0.0 << 1000;
    QTest::newRow("default font, short window") << 0.0 << 420;
    QTest::newRow("24 pt") << 24.0 << 1000;
    QTest::newRow("24 pt, short window") << 24.0 << 520;
}

// Typing on with the results scrolled down changes how many rows there
// are, and with them the palette's height. The list, scrolled past its new
// end, scrolls back into bounds as it takes that height, and lays itself
// out; its count, which the height followed, notified from there, so the
// height was worked out again from within itself: a binding loop on
// resultsHeight and the body's implicitHeight (#619). The palette's height
// follows the model's rows instead. The test fails on the warning, in
// cleanup(). With a large font, and in a window short enough that its room
// limits the palette, the list still ends on a whole row over the footer.
void SearchPaletteTest::scrolledResultsTakeANewRowCountWithoutABindingLoop() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowHeight);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont large = applicationFont;
        large.setPointSizeF(pointSize);
        QGuiApplication::setFont(large);
    }
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText, 1600, windowHeight), qPrintable(errorText));
    QVERIFY(harness.open());
    // Results shaped as the backend's: a section of each type, each with
    // its heading and a more row, as many as Rust's limits for a short
    // query.
    const auto rowsFor = [](int artists, int albums, int tracks) {
        QVector<Row> rows;
        for (int i = 0; i < artists; ++i) {
            rows << result(QStringLiteral("artist"), QStringLiteral("Artist %1").arg(i), 1.0f + float(i));
        }
        for (int i = 0; i < albums; ++i) {
            rows << result(QStringLiteral("album"), QStringLiteral("Album %1").arg(i), 2.0f + float(i));
        }
        for (int i = 0; i < tracks; ++i) {
            Row track = result(QStringLiteral("track"), QStringLiteral("Track %1").arg(i), 0.5f + float(i));
            track.trackPath = QStringLiteral("/fixture/track%1.flac").arg(i);
            track.lengthText = QStringLiteral("3:00");
            rows << track;
        }
        return rows;
    };
    struct Step {
        QString query;
        int artists;
        int albums;
        int tracks;
    };
    const QList<Step> steps{
        {QStringLiteral("m"), 5, 10, 20},  {QStringLiteral("me"), 3, 6, 12}, {QStringLiteral("mer"), 1, 2, 3},
        {QStringLiteral("me"), 3, 6, 12}, {QStringLiteral("m"), 5, 10, 20},
    };
    QQuickItem *view = harness.view();
    QQuickItem *footer = harness.item(QStringLiteral("globalSearchFooter"));
    QVERIFY(footer != nullptr);
    for (const Step &step : steps) {
        // To the end of the list, which scrolls it where it is longer than
        // its room.
        for (int i = 0; i < 4; ++i) QTest::keyClick(harness.window, Qt::Key_PageDown);
        if (view->isVisible() && view->property("contentHeight").toReal() > view->height()) {
            QVERIFY2(view->property("contentY").toReal() > 0, qPrintable(step.query));
        }
        harness.bridge->setProperty("globalSearchBusy", true);
        harness.field()->setProperty("text", step.query);
        harness.model.presentSearchRows(rowsFor(step.artists, step.albums, step.tracks),
                                        {2 * step.artists, 2 * step.albums, 2 * step.tracks}, true);
        harness.bridge->setProperty("globalSearchBusy", false);
        harness.settleRows();
        QTRY_VERIFY(view->isVisible());
        QTRY_COMPARE(sceneRect(footer).top() - 1, sceneRect(view).bottom());
        QVERIFY(sceneRect(harness.palette()).bottom() <= windowHeight - 32 + 0.5);
    }
}

// The palette's height follows the model's count, which notifies as the
// rows are inserted or removed, with the new count already read back:
// fewer rows, more rows, and none.
void SearchPaletteTest::modelNotifiesItsCountAsRowsChange() {
    GlobalSearchResultsModel model;
    QList<int> counts;
    QObject::connect(&model, &GlobalSearchResultsModel::countChanged, &model,
                     [&] { counts << model.property("count").toInt(); });
    const auto rows = [](int n) {
        QVector<Row> list;
        for (int i = 0; i < n; ++i) list << result(QStringLiteral("track"), QStringLiteral("Track %1").arg(i), 1.0f);
        return list;
    };
    model.replaceRows(rows(5));
    model.replaceRows(rows(3));
    model.replaceRows(rows(3));
    model.replaceRows(rows(8));
    model.replaceRows({});
    QCOMPARE(counts, (QList<int>{5, 3, 8, 0}));
    QCOMPARE(model.property("count").toInt(), 0);
}

// Up and Down move between results (wrapping, skipping headings), Page Up
// and Down by a page, Home and End to the ends in the list; Enter plays
// and closes, Ctrl+Enter adds to the queue and stays open, Alt+Enter shows
// the result in the library and closes, Esc closes.
void SearchPaletteTest::keyboardSelectsAndActs() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    auto rows = meridianRows(harness.coverUrl);
    for (int i = 0; i < 40; ++i) {
        Row extra = result(QStringLiteral("track"), QStringLiteral("Extra %1").arg(i, 2, 10, QLatin1Char('0')), 5.0f);
        extra.trackPath = QStringLiteral("/fixture/extra%1.flac").arg(i);
        rows << extra;
    }
    harness.search(QStringLiteral("meridian"), rows);
    QTRY_VERIFY(harness.view()->isVisible());
    // Many results: at most 16 result rows tall, and as tall as the rows
    // that fit that whole (resultsListEndsOnAWholeRow): here a heading and
    // 15 results.
    QTRY_COMPARE(sceneRect(harness.palette()).height(), 44.0 + 1 + (28 + 15 * 32) + 1 + 28);
    QCOMPARE(harness.selection(), 1);
    const int first = harness.selection();
    QTest::keyClick(harness.window, Qt::Key_Up); // Wraps to the last.
    const int last = harness.selection();
    QVERIFY(last > first);
    QCOMPARE(harness.model.rowDataAt(last).value(QStringLiteral("rowType")).toString(), QStringLiteral("album"));
    QTest::keyClick(harness.window, Qt::Key_Down); // Wraps to the first.
    QCOMPARE(harness.selection(), first);
    // Down skips the next heading.
    int tracks = 0;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("rowType")) == QStringLiteral("track")
            && harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("item")) ++tracks;
    }
    for (int i = 1; i < tracks; ++i) QTest::keyClick(harness.window, Qt::Key_Down);
    QTest::keyClick(harness.window, Qt::Key_Down);
    QCOMPARE(harness.model.rowDataAt(harness.selection()).value(QStringLiteral("rowType")).toString(), QStringLiteral("artist"));
    QCOMPARE(harness.model.rowDataAt(harness.selection() - 1).value(QStringLiteral("kind")).toString(), QStringLiteral("section"));
    QTest::keyClick(harness.window, Qt::Key_PageUp);
    const int afterPageUp = harness.selection();
    QVERIFY(afterPageUp < harness.model.indexOfResult(QStringLiteral("artist"), 0));
    QTest::keyClick(harness.window, Qt::Key_PageDown);
    QVERIFY(harness.selection() > afterPageUp);
    // The selection stays in view.
    QQuickItem *view = harness.view();
    const auto selectionVisible = [&] {
        QQuickItem *row = harness.row(harness.selection());
        return row != nullptr && sceneRect(row).top() >= sceneRect(view).top() - 1
            && sceneRect(row).bottom() <= sceneRect(view).bottom() + 1;
    };
    QTRY_VERIFY(selectionVisible());
    // Home and End in the field move the cursor; in the list, the selection.
    QTest::keyClick(harness.window, Qt::Key_Home);
    QCOMPARE(harness.field()->property("cursorPosition").toInt(), 0);
    QTest::keyClick(harness.window, Qt::Key_End);
    QCOMPARE(harness.field()->property("cursorPosition").toInt(), 8);
    view->forceActiveFocus();
    QTest::keyClick(harness.window, Qt::Key_End);
    QCOMPARE(harness.selection(), last);
    QTRY_VERIFY(selectionVisible());
    QTest::keyClick(harness.window, Qt::Key_Home);
    QCOMPARE(harness.selection(), first);
    QTRY_VERIFY(selectionVisible());
    // Ctrl+F goes back to the query.
    QTest::keyClick(harness.window, Qt::Key_F, Qt::ControlModifier);
    QVERIFY(harness.field()->hasActiveFocus());
    QCOMPARE(harness.field()->property("selectedText").toString(), QStringLiteral("meridian"));

    // Ctrl+Enter: the track goes to the queue; the palette stays.
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    QCOMPARE(harness.strings("queued"), QStringList{QStringLiteral("track:/fixture/drift.flac")});
    QVERIFY(harness.strings("played").isEmpty());
    QVERIFY(harness.dialog->property("visible").toBool());
    // Alt+Enter: shown in the library; the palette closes.
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::AltModifier);
    QCOMPARE(harness.strings("revealed"), QStringList{QStringLiteral("Meridian Drift")});
    QVERIFY(harness.strings("played").isEmpty());
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());

    // Enter: plays the artist; the palette closes.
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), rows);
    QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex",
                              Q_ARG(QVariant, harness.model.indexOfResult(QStringLiteral("artist"), 0)));
    QTest::keyClick(harness.window, Qt::Key_Enter);
    QCOMPARE(harness.strings("played"), QStringList{QStringLiteral("artist:The Quiet Meridian")});
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());

    // Esc closes.
    QVERIFY(harness.open());
    QTest::keyClick(harness.window, Qt::Key_Escape);
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());
}

// A click selects; a double click plays; a right click opens the result's
// menu, whose window shows whole the first time (#646).
void SearchPaletteTest::mouseSelectsAndActs() {
    PopupShowSizes popupSizes;
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl));
    QTRY_VERIFY(harness.view()->isVisible());
    const int album = harness.model.indexOfResult(QStringLiteral("album"), 0);
    QQuickItem *row = harness.row(album);
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(row));
    QCOMPARE(harness.selection(), album);
    QVERIFY(harness.strings("played").isEmpty());
    QTest::mouseDClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(row));
    QCOMPARE(harness.strings("played"), QStringList{QStringLiteral("album:Grey Lines")});
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());

    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl));
    QTRY_VERIFY(harness.view()->isVisible());
    const int track = harness.model.indexOfResult(QStringLiteral("track"), 2);
    // The menu's fades in animation time, each to its end.
    SteppedAnimationDriver animations;
    const auto settle = [&animations] { (void)animations.step(1000); };
    // Near the row's start: the menu is a window of its own, kept on the
    // screen, and the offscreen screen is 800 px wide, so a click further
    // right opened it to the click's left.
    const QRectF trackRect = sceneRect(harness.row(track));
    const QPoint clickAt = QPointF(trackRect.left() + 40, trackRect.center().y()).toPoint();
    QTest::mouseClick(harness.window, Qt::RightButton, Qt::NoModifier, clickAt);
    settle();
    QCOMPARE(harness.selection(), track);
    QCOMPARE(harness.dialog->property("contextRowData").toMap().value(QStringLiteral("label")).toString(),
             QStringLiteral("Lantern Signal"));
    QQmlExpression menuOpened(qmlContext(harness.field()), harness.field(), QStringLiteral("resultMenu.opened"));
    QTRY_VERIFY2(menuOpened.evaluate().toBool(), qPrintable(menuOpened.error().toString() + QStringLiteral(" visible=")
        + QQmlExpression(qmlContext(harness.field()), harness.field(), QStringLiteral("resultMenu.visible")).evaluate().toString()));
    // It opens where the click was, not at the window's corner.
    QQmlExpression menuItem(qmlContext(harness.field()), harness.field(), QStringLiteral("resultMenu.contentItem"));
    auto *menuContent = qvariant_cast<QQuickItem *>(menuItem.evaluate());
    QVERIFY(menuContent != nullptr);
    const QPointF clicked = harness.window->mapToGlobal(QPointF(clickAt));
    const QPointF menuAt = menuContent->mapToGlobal(QPointF(0, 0));
    QVERIFY2(std::abs(menuAt.x() - clicked.x()) <= 16 && std::abs(menuAt.y() - clicked.y()) <= 16,
             qPrintable(QStringLiteral("menu at %1,%2, clicked at %3,%4")
                            .arg(menuAt.x()).arg(menuAt.y()).arg(clicked.x()).arg(clicked.y())));
    // Its window, the first the palette opens, shows at the size the menu
    // ends at: on Wayland the compositor kept the size a menu window was
    // first shown at, and the menu was an empty strip.
    QQuickWindow *menuWindow = menuContent->window();
    QVERIFY(menuWindow != nullptr && menuWindow != harness.window);
    QTRY_VERIFY(menuWindow->isExposed());
    QCOMPARE(popupSizes.lastShown(menuWindow), menuWindow->size());
    QQmlExpression menuSize(qmlContext(harness.field()), harness.field(),
                            QStringLiteral("Qt.size(Math.ceil(resultMenu.implicitWidth), Math.ceil(resultMenu.implicitHeight))"));
    QCOMPARE(menuSize.evaluate().toSizeF().toSize(), menuWindow->size());
    // Show in Library has the queue menu's icon.
    QQmlExpression showInLibraryIcon(qmlContext(harness.field()), harness.field(),
                                     QStringLiteral("resultMenu.itemAt(3).text + '|' + resultMenu.itemAt(3).icon.name"));
    QCOMPARE(showInLibraryIcon.evaluate().toString(), QStringLiteral("Show in Library|view-list-tree"));
    QTest::keyClick(harness.window, Qt::Key_Escape);
    settle();
    QTRY_VERIFY(!menuOpened.evaluate().toBool());
    // The palette stays open behind its menu.
    QVERIFY(harness.dialog->property("visible").toBool());
}

// The Menu key and Shift+F10 open the selected result's menu under its row,
// from the field and from the list, as the queue's and the library's do
// (#583), and the field's own text menu stays closed; with a more row
// selected they open nothing.
void SearchPaletteTest::menuKeyOpensTheSelectedResultsMenu() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 26});
    QTRY_VERIFY(harness.view()->isVisible());
    QVERIFY(harness.field()->hasActiveFocus());
    // The menu's fades in animation time, each to its end.
    SteppedAnimationDriver animations;
    const auto settle = [&animations] { (void)animations.step(1000); };
    QQmlExpression menuOpened(qmlContext(harness.field()), harness.field(), QStringLiteral("resultMenu.opened"));
    QQmlExpression menuItem(qmlContext(harness.field()), harness.field(), QStringLiteral("resultMenu.contentItem"));

    const int track = harness.model.indexOfResult(QStringLiteral("track"), 2);
    QVERIFY(QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, track)));
    QTest::keyClick(harness.window, Qt::Key_Menu);
    settle();
    QTRY_VERIFY(menuOpened.evaluate().toBool());
    QCOMPARE(harness.dialog->property("contextRowData").toMap().value(QStringLiteral("label")).toString(),
             QStringLiteral("Lantern Signal"));
    // Under the selected row, at its start.
    auto *menuContent = qvariant_cast<QQuickItem *>(menuItem.evaluate());
    QVERIFY(menuContent != nullptr);
    QQuickItem *trackRow = harness.row(track);
    QVERIFY(trackRow != nullptr);
    const QRectF row = sceneRect(trackRow);
    const QPointF rowCorner = harness.window->mapToGlobal(QPointF(row.left(), row.bottom()));
    const QPointF menuAt = menuContent->mapToGlobal(QPointF(0, 0));
    QVERIFY2(std::abs(menuAt.x() - rowCorner.x()) <= 16 && std::abs(menuAt.y() - rowCorner.y()) <= 16,
             qPrintable(QStringLiteral("menu at %1,%2, row's corner at %3,%4")
                            .arg(menuAt.x()).arg(menuAt.y()).arg(rowCorner.x()).arg(rowCorner.y())));
    // Escape closes the menu only, and the field keeps the keys.
    QTest::keyClick(harness.window, Qt::Key_Escape);
    settle();
    QTRY_VERIFY(!menuOpened.evaluate().toBool());
    QVERIFY(harness.dialog->property("visible").toBool());
    QTRY_VERIFY(harness.field()->hasActiveFocus());

    // Shift+F10 from the list; its actions act on that result.
    harness.view()->forceActiveFocus();
    QTest::keyClick(harness.window, Qt::Key_F10, Qt::ShiftModifier);
    settle();
    QTRY_VERIFY(menuOpened.evaluate().toBool());
    QTest::keyClick(harness.window, Qt::Key_Down);
    QTest::keyClick(harness.window, Qt::Key_Down); // Add to Queue.
    QTest::keyClick(harness.window, Qt::Key_Return);
    settle();
    QCOMPARE(harness.strings("queued"), QStringList{QStringLiteral("track:/fixture/lantern.flac")});
    QTRY_VERIFY(!menuOpened.evaluate().toBool());

    // A more row has no menu (in the field, the key would be the field's).
    harness.view()->forceActiveFocus();
    int moreIndex = -1;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("more")) moreIndex = i;
    }
    QVERIFY(moreIndex > 0);
    QVERIFY(QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, moreIndex)));
    QCOMPARE(harness.selection(), moreIndex);
    QTest::keyClick(harness.window, Qt::Key_Menu);
    settle();
    QVERIFY(!menuOpened.evaluate().toBool());
    QVERIFY(harness.dialog->property("visible").toBool());
}

// The result's menu is one of the app's context menus: it names what it
// acts on as the footer does, with the footer's keys, has the library's
// icons, and groups Show in Library with the folder as the queue's does.
void SearchPaletteTest::resultMenuNamesWhatItActsOn() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    SteppedAnimationDriver animations;
    const auto settle = [&animations] { (void)animations.step(1000); };
    QQmlExpression menuOpened(qmlContext(harness.field()), harness.field(), QStringLiteral("resultMenu.opened"));
    QQmlExpression menuRows(qmlContext(harness.field()), harness.field(), QStringLiteral(R"JS((function() {
        const rows = []
        for (let i = 0; i < resultMenu.count; ++i) {
            const item = resultMenu.itemAt(i)
            rows.push(item.text === undefined ? "sep"
                : item.text + "|" + (item.keyHint || "") + "|" + item.icon.name)
        }
        return rows.join("\n")
    })())JS"));

    const struct {
        QString type;
        QString play;
        QString add;
    } kinds[] = {
        {QStringLiteral("artist"), QStringLiteral("Play Artist"), QStringLiteral("Add Artist to Queue")},
        {QStringLiteral("album"), QStringLiteral("Play Album"), QStringLiteral("Add Album to Queue")},
        {QStringLiteral("track"), QStringLiteral("Play Track"), QStringLiteral("Add Track to Queue")},
    };
    for (const auto &kind : kinds) {
        const int index = harness.model.indexOfResult(kind.type, 0);
        QVERIFY(QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, index)));
        QTRY_VERIFY(harness.field()->hasActiveFocus());
        QVERIFY(harness.hints().contains(QStringLiteral("Enter ") + kind.play));
        QTest::keyClick(harness.window, Qt::Key_Menu);
        settle();
        QTRY_VERIFY(menuOpened.evaluate().toBool());
        const QString rows = menuRows.evaluate().toString();
        QVERIFY2(!menuRows.hasError(), qPrintable(menuRows.error().toString()));
        QCOMPARE(rows.split(QLatin1Char('\n')),
                 (QStringList{
                     kind.play + QStringLiteral("|Enter|media-playback-start"),
                     kind.add + QStringLiteral("|Ctrl+Enter|media-playlist-append"),
                     QStringLiteral("sep"),
                     QStringLiteral("Show in Library|Alt+Enter|view-list-tree"),
                     kind.type == QStringLiteral("track") ? QStringLiteral("Open Containing Folder||folder-open")
                                                          : QStringLiteral("Open Folder||folder-open"),
                 }));
        QTest::keyClick(harness.window, Qt::Key_Escape);
        settle();
        QTRY_VERIFY(!menuOpened.evaluate().toBool());
    }
}

// Results kept from an earlier query, or shown while a newer one runs, do
// nothing; a new query drops the selection and the pending expansion.
void SearchPaletteTest::staleAndBusyResultsDoNotAct() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 26});
    QTRY_VERIFY(harness.view()->isVisible());
    harness.bridge->setProperty("globalSearchBusy", true);
    // The pointer neither selects nor acts on a row, nor scrolls the list.
    QVERIFY(!harness.view()->property("interactive").toBool());
    const int selected = harness.selection();
    const int other = harness.model.indexOfResult(QStringLiteral("album"), 0);
    QVERIFY(other >= 0 && other != selected);
    const QPoint otherAt = sceneCentre(harness.row(other));
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, otherAt);
    QTest::mouseDClick(harness.window, Qt::LeftButton, Qt::NoModifier, otherAt);
    QCOMPARE(harness.selection(), selected);
    QVERIFY(harness.field()->hasActiveFocus());
    // Nor do the list's own keys move the selection.
    harness.view()->forceActiveFocus();
    QTest::keyClick(harness.window, Qt::Key_End);
    QTest::keyClick(harness.window, Qt::Key_Down);
    QCOMPARE(harness.selection(), selected);
    harness.field()->forceActiveFocus();
    QTest::keyClick(harness.window, Qt::Key_Return);
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::ControlModifier);
    QTest::keyClick(harness.window, Qt::Key_Return, Qt::AltModifier);
    QVERIFY(harness.strings("played").isEmpty());
    QVERIFY(harness.strings("queued").isEmpty());
    QVERIFY(harness.strings("revealed").isEmpty());
    QVERIFY(harness.dialog->property("visible").toBool());
    harness.bridge->setProperty("globalSearchBusy", false);

    harness.bridge->setProperty("globalSearchModelRetained", true);
    QVERIFY(!harness.view()->isVisible());
    QTest::keyClick(harness.window, Qt::Key_Return);
    QVERIFY(harness.strings("played").isEmpty());
    harness.bridge->setProperty("globalSearchModelRetained", false);

    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("more")) {
            QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, i));
        }
    }
    QTest::keyClick(harness.window, Qt::Key_Return);
    QCOMPARE(harness.strings("expanded"), QStringList{QStringLiteral("track")});
    QVERIFY(harness.controller->property("pendingExpansion").toMap().contains(QStringLiteral("rowType")));
    harness.field()->setProperty("text", QStringLiteral("other"));
    QVERIFY(!harness.controller->property("pendingExpansion").toMap().contains(QStringLiteral("rowType")));
    QCOMPARE(harness.strings("queries").last(), QStringLiteral("other"));
}

// While a newer query runs, the rows it replaces are drawn as they were:
// at full contrast, glyphs too, with the bold runs of the query they were
// found by (they dropped to half opacity and lost their bold runs on every
// keystroke, a flash of about 57 ms).
void SearchPaletteTest::typingKeepsTheResultsInPlaceAsTheyAre() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("me"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    const int track = harness.model.indexOfResult(QStringLiteral("track"), 0);
    const auto emphasised = [&] {
        QStringList texts;
        for (QQuickItem *run : visibleNamed(harness.row(track), QStringLiteral("globalSearchResultTitle"))) {
            if (run->property("font").value<QFont>().weight() == QFont::DemiBold) texts << run->property("text").toString();
        }
        return texts;
    };
    QCOMPARE(emphasised(), QStringList{QStringLiteral("Me")});
    const auto drawnResults = [&] {
        const QImage image = harness.window->grabWindow();
        const qreal scale = static_cast<qreal>(image.width()) / harness.window->width();
        const QRectF list = sceneRect(harness.view());
        return image.copy(QRectF(list.topLeft() * scale, list.size() * scale).toAlignedRect());
    };
    // The selection stays on the first result, as the app keeps it.
    const int first = harness.selection();
    QVERIFY(first >= 0);
    QImage before;
    // Once the covers have loaded.
    QTRY_VERIFY([&] {
        const QImage last = before;
        before = drawnResults();
        return !last.isNull() && last == before;
    }());

    harness.bridge->setProperty("globalSearchBusy", true);
    harness.field()->setProperty("text", QStringLiteral("mer"));
    QVERIFY(QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, first)));
    QCOMPARE(harness.view()->opacity(), 1.0);
    QCOMPARE(emphasised(), QStringList{QStringLiteral("Me")});
    QVERIFY2(drawnResults() == before, "the results changed as the query was typed");

    // The new query's rows have its runs.
    harness.model.presentSearchRows(meridianRows(harness.coverUrl), {1, 2, 4});
    harness.bridge->setProperty("globalSearchBusy", false);
    harness.settleRows();
    QCOMPARE(emphasised(), QStringList{QStringLiteral("Mer")});
}

// The results dim only for a search still running 250 ms after its query,
// the threshold of a busy indicator: each new query starts the wait again,
// the dim fades in, and the results come back at once.
void SearchPaletteTest::onlyASlowSearchDimsTheResults() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("me"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    QQuickItem *view = harness.view();

    // The time of the last query, and of the first dim after it.
    QElapsedTimer clock;
    clock.start();
    qint64 queried = 0;
    qint64 dimmedAfter = -1;
    const auto connection = QObject::connect(view, &QQuickItem::opacityChanged, view, [&] {
        if (dimmedAfter < 0 && view->opacity() < 1.0) dimmedAfter = clock.elapsed() - queried;
    });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });

    harness.bridge->setProperty("globalSearchBusy", true);
    for (const QString &query : {QStringLiteral("mer"), QStringLiteral("meri"), QStringLiteral("merid")}) {
        queried = clock.elapsed();
        harness.field()->setProperty("text", query);
        QCOMPARE(view->opacity(), 1.0);
        if (query != QStringLiteral("merid")) QTest::qWait(100);
    }
    QTRY_VERIFY(dimmedAfter >= 0);
    // Less what an animation may take of the frame it started in.
    QVERIFY2(dimmedAfter >= 190, qPrintable(QStringLiteral("dimmed %1 ms after the last query").arg(dimmedAfter)));
    QTRY_COMPARE(view->opacity(), 0.5);

    // A query typed while it is dimmed keeps it so; the answer ends it at once.
    harness.field()->setProperty("text", QStringLiteral("meridi"));
    QCOMPARE(view->opacity(), 0.5);
    harness.bridge->setProperty("globalSearchBusy", false);
    QCOMPARE(view->opacity(), 1.0);
}

// Reopened with the text it kept, the palette searches it again before it
// shows: "No results" never shows between opening and the results for that
// text (#647).
void SearchPaletteTest::reopeningWithKeptTextShowsNoEmptySearch() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());

    // Closing clears the search, and its empty results arrive; the field
    // keeps its text.
    QTest::keyClick(harness.window, Qt::Key_Escape);
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());
    QCOMPARE(harness.strings("queries").last(), QString());
    harness.model.presentSearchRows({}, {0, 0, 0}, false);
    harness.settleRows();
    QCOMPARE(harness.model.rowCount(), 0);
    QCOMPARE(harness.field()->property("text").toString(), QStringLiteral("meridian"));

    // As the bridge is: busy from a query until its results arrive.
    QQmlExpression busyWhileSearching(qmlContext(harness.root.get()), harness.root.get(), QStringLiteral(R"JS(
        bridge.queriesChanged.connect(function() {
            bridge.globalSearchBusy = bridge.queries[bridge.queries.length - 1].trim().length > 0
        }))JS"));
    busyWhileSearching.evaluate();
    QVERIFY2(!busyWhileSearching.hasError(), qPrintable(busyWhileSearching.error().toString()));

    QQuickItem *noResults = harness.item(QStringLiteral("globalSearchNoResults"));
    QVERIFY(noResults != nullptr);
    QVERIFY(!noResults->isVisible());
    bool noResultsShown = false;
    const auto connection = QObject::connect(noResults, &QQuickItem::visibleChanged, noResults, [&] {
        noResultsShown = noResultsShown || noResults->isVisible();
    });
    const auto disconnect = qScopeGuard([&] { QObject::disconnect(connection); });

    // Reopened with a fade, as it is by default: the frames of the fade
    // come before the results.
    harness.dialog->setProperty("fadeDuration", 150);
    QMetaObject::invokeMethod(harness.controller, "openDialog");
    QVERIFY(harness.dialog->property("visible").toBool());
    QVERIFY(!noResults->isVisible());
    QTRY_VERIFY(harness.dialog->property("opened").toBool());
    QCOMPARE(harness.strings("queries").last(), QStringLiteral("meridian"));
    QVERIFY(harness.bridge->property("globalSearchBusy").toBool());
    QVERIFY(!noResultsShown);

    harness.model.presentSearchRows(meridianRows(harness.coverUrl), {1, 2, 4}, true);
    harness.bridge->setProperty("globalSearchBusy", false);
    harness.settleRows();
    QTRY_VERIFY(harness.view()->isVisible());
    QVERIFY(harness.model.rowCount() > 0);
    QVERIFY(!noResultsShown);
}

// The palette and the dim fade in and out together over the short
// duration, and open at once when animations are off. Fading out, the
// palette is closed: the window under it takes presses, and Find opens it
// again.
void SearchPaletteTest::paletteAndDimFadeTogether() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    // By default, Kirigami's short duration.
    QQmlExpression shortDuration(qmlContext(harness.dialog), harness.dialog,
                                 QStringLiteral("Kirigami.Units.shortDuration"));
    const QVariant unit = shortDuration.evaluate();
    QVERIFY2(!shortDuration.hasError(), qPrintable(shortDuration.error().toString()));
    QVERIFY(unit.toInt() > 0);
    QCOMPARE(harness.dialog->property("fadeDuration").toInt(), unit.toInt());

    QQuickItem *palette = harness.palette();
    const auto findDim = [&] {
        QQuickItem *dim = nullptr;
        visit(harness.window->contentItem(), [&](QQuickItem *item) {
            if (item->objectName() == QStringLiteral("globalSearchDim")) dim = item;
        });
        return dim;
    };
    // The fades in animation time: part of the way, and to their end.
    std::optional<SteppedAnimationDriver> animations(std::in_place);
    harness.dialog->setProperty("fadeDuration", 400);
    QMetaObject::invokeMethod(harness.controller, "openDialog");
    QTRY_VERIFY(findDim() != nullptr);
    QQuickItem *dim = findDim();
    // The palette draws the dim, which fades with it.
    QVERIFY(palette->isAncestorOf(dim));
    QVERIFY(animations->step(96));
    QVERIFY2(palette->opacity() > 0 && palette->opacity() < 1, qPrintable(QString::number(palette->opacity())));
    (void)animations->step(400);
    QCOMPARE(palette->opacity(), 1.0);
    QCOMPARE(dim->opacity(), 1.0);

    // Open, the window under the dim takes no hover, wheel or presses.
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *transportMouse = harness.item(QStringLiteral("transportMouseArea"));
    QVERIFY(transport != nullptr && transportMouse != nullptr);
    const QPoint overTransport(20, 20);
    QVERIFY(!sceneRect(palette).contains(overTransport));
    QTest::mouseMove(harness.window, overTransport);
    QTest::mouseMove(harness.window, overTransport + QPoint(4, 0));
    QVERIFY(!transportMouse->property("containsMouse").toBool());
    QWheelEvent wheel(overTransport, harness.window->mapToGlobal(QPointF(overTransport)), QPoint(), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(harness.window, &wheel);
    QCOMPARE(transport->property("wheels").toInt(), 0);
    // A press on the palette, here its footer, leaves it open.
    QTest::mouseClick(harness.window, Qt::LeftButton, {},
                      sceneCentre(harness.item(QStringLiteral("globalSearchFooter"))));
    (void)animations->step(48);
    QVERIFY(harness.dialog->property("opened").toBool());
    // A press outside closes it, and the window takes nothing.
    QTest::mouseClick(harness.window, Qt::LeftButton, {}, overTransport);
    QCOMPARE(transport->property("presses").toInt(), 0);
    QVERIFY(animations->step(48));
    QVERIFY(harness.dialog->property("visible").toBool());
    (void)animations->step(400);
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());

    // Closing, the dim fades out with the palette, which already takes
    // nothing from the window: a click reaches it.
    QMetaObject::invokeMethod(harness.controller, "openDialog");
    (void)animations->step(400);
    QTRY_VERIFY(harness.dialog->property("opened").toBool());
    QTest::keyClick(harness.window, Qt::Key_Escape);
    QVERIFY(animations->step(96));
    QVERIFY(harness.dialog->property("visible").toBool());
    QVERIFY2(palette->opacity() > 0 && palette->opacity() < 1, qPrintable(QString::number(palette->opacity())));
    QVERIFY(palette->isAncestorOf(findDim()));
    QTest::mouseClick(harness.window, Qt::LeftButton, {}, overTransport);
    QCOMPARE(transport->property("presses").toInt(), 1);

    // Find opens it again while it fades out.
    QMetaObject::invokeMethod(harness.controller, "openDialog");
    (void)animations->step(400);
    QTRY_VERIFY(harness.dialog->property("opened").toBool());
    QCOMPARE(palette->opacity(), 1.0);
    QTRY_VERIFY(harness.field()->hasActiveFocus());
    QTest::keyClick(harness.window, Qt::Key_Escape);
    (void)animations->step(400);
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());
    animations.reset();

    // Animations off: at once.
    harness.dialog->setProperty("fadeDuration", 0);
    QMetaObject::invokeMethod(harness.controller, "openDialog");
    QTRY_VERIFY(harness.dialog->property("visible").toBool());
    QCOMPARE(palette->opacity(), 1.0);
    dim = findDim();
    QVERIFY(dim != nullptr);
    QCOMPARE(dim->opacity(), 1.0);
    QTest::keyClick(harness.window, Qt::Key_Escape);
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());
}

// Clear is a tool button with the Breeze edit-clear icon, shown with text;
// help has the help icon; no "×" or "?" text glyph anywhere.
void SearchPaletteTest::clearUsesTheBreezeIconAndNoTextGlyphs() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    QQuickItem *clear = harness.item(QStringLiteral("globalSearchClearButton"));
    QQuickItem *help = harness.item(QStringLiteral("globalSearchHelpButton"));
    QVERIFY(clear != nullptr && help != nullptr);
    QVERIFY(!clear->isVisible());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl));
    QVERIFY(clear->isVisible());
    QQmlExpression iconName(qmlContext(clear), clear, QStringLiteral("icon.name"));
    QCOMPARE(iconName.evaluate().toString(), QStringLiteral("edit-clear"));
    QQmlExpression helpIcon(qmlContext(help), help, QStringLiteral("icon.name"));
    QCOMPARE(helpIcon.evaluate().toString(), QStringLiteral("question"));
    QCOMPARE(clear->property("display").toInt(), 0); // AbstractButton.IconOnly
    // Quiet glyphs: help's in the field's secondary text colour, the solid
    // clear's under it (clearGlyphIsQuietUntilPointedAt).
    QQmlExpression helpColor(qmlContext(help), help, QStringLiteral("icon.color"));
    QCOMPARE(hex(helpColor.evaluate().value<QColor>()), hex(harness.token("textSecondary")));
    QCOMPARE(QAccessible::queryAccessibleInterface(clear)->text(QAccessible::Name), QStringLiteral("Clear"));
    // No text glyphs drawn as icons.
    visit(harness.palette(), [&](QQuickItem *item) {
        const QString text = item->property("text").toString();
        if (item->isVisible() && !item->inherits("QQuickAbstractButton")) {
            QVERIFY2(text != QStringLiteral("×") && text != QStringLiteral("?"), qPrintable(item->objectName()));
        }
    });
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(clear));
    QCOMPARE(harness.field()->property("text").toString(), QString());
    QVERIFY(harness.field()->hasActiveFocus());
    QVERIFY(!clear->isVisible());
}

void SearchPaletteTest::textMeetsItsContrastAsDrawn_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("Breeze Dark") << true;
    QTest::newRow("Breeze Light") << false;
}

// Text ≥4.5:1 and the glyphs ≥3:1, on the view, the hover tint and the
// selection, with the colours the items draw with.
void SearchPaletteTest::textMeetsItsContrastAsDrawn() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/drift.flac"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 26});
    QTRY_VERIFY(harness.view()->isVisible());

    const QColor view = colorOf(harness.item(QStringLiteral("globalSearchBackground")));
    const QColor hover = drawnOver(harness.token("hover"), view);
    const QColor selection = harness.token("selection");
    QVERIFY(selection.alphaF() == 1.0f);
    QVERIFY(harness.model.rowDataAt(harness.selection()).value(QStringLiteral("trackPath")) == QStringLiteral("/fixture/drift.flac"));

    QStringList failures;
    const auto check = [&](const QString &what, const QColor &foreground, const QColor &background, double minimum) {
        if (contrastRatio(foreground, background) < minimum) failures << describeContrast(what, foreground, background);
    };
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        QQuickItem *row = harness.row(i);
        QVERIFY(row != nullptr);
        const bool selected = i == harness.selection();
        // Only results take the hover tint.
        const bool result = harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("item");
        const QList<QColor> backgrounds = selected ? QList<QColor>{selection}
                                                   : (result ? QList<QColor>{view, hover} : QList<QColor>{view});
        for (const QColor &background : backgrounds) {
            for (QQuickItem *run : visibleNamed(row, QStringLiteral("globalSearchResultTitle"))) {
                check(QStringLiteral("title of row %1").arg(i), colorOf(run), background, 4.5);
            }
            for (const QString &name : {QStringLiteral("globalSearchResultContext"),
                                        QStringLiteral("globalSearchResultLength"), QStringLiteral("globalSearchMoreLink"),
                                        QStringLiteral("globalSearchSectionHeading")}) {
                for (QQuickItem *text : visibleNamed(row, name)) {
                    check(QStringLiteral("%1 of row %2").arg(name).arg(i), colorOf(text),
                          name == QStringLiteral("globalSearchSectionHeading") ? view : background, 4.5);
                }
            }
            // The no-cover tile's mark is checked on its own floor
            // (noCoverTileIsTheLibrarys).
            if (QQuickItem *glyph = visibleChildNamed(row, QStringLiteral("globalSearchArtistGlyph"))) {
                check(QStringLiteral("artist glyph of row %1").arg(i), colorOf(glyph), background, 3.0);
            }
            // The amber glyph on its scrim over a mid cover.
            if (QQuickItem *glyph = visibleChildNamed(row, QStringLiteral("globalSearchNowGlyph"))) {
                const QColor scrim = drawnOver(colorOf(visibleChildNamed(row, QStringLiteral("globalSearchNowScrim"))),
                                               QColor(QStringLiteral("#b07a5a")));
                check(QStringLiteral("playing glyph"), colorOf(glyph), scrim, 3.0);
            }
        }
    }
    // The more link selected.
    int moreIndex = -1;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) == QStringLiteral("more")) moreIndex = i;
    }
    QMetaObject::invokeMethod(harness.controller, "selectDisplayIndex", Q_ARG(QVariant, moreIndex));
    check(QStringLiteral("selected more link"),
          colorOf(visibleChildNamed(harness.row(moreIndex), QStringLiteral("globalSearchMoreLink"))), selection, 4.5);
    // The footer, the field and its placeholder.
    QQuickItem *footer = harness.item(QStringLiteral("globalSearchFooter"));
    for (QQuickItem *text : visibleNamed(footer, QStringLiteral("globalSearchKeyHintKey"))
                                + visibleNamed(footer, QStringLiteral("globalSearchKeyHintAction"))) {
        check(QStringLiteral("key hint"), colorOf(text), view, 4.5);
    }
    check(QStringLiteral("query"), colorOf(harness.field()), view, 4.5);
    check(QStringLiteral("placeholder"), harness.field()->property("placeholderTextColor").value<QColor>(), view, 4.5);
    check(QStringLiteral("field glyph"), colorOf(harness.item(QStringLiteral("globalSearchFieldIcon"))), view, 3.0);
    // The messages and the suggestion row.
    harness.bridge->setProperty("globalSearchSuggestion", QStringLiteral("meridian"));
    harness.search(QStringLiteral("merdian"), {});
    QTRY_VERIFY(harness.item(QStringLiteral("globalSearchNoResultsTitle"))->isVisible());
    check(QStringLiteral("no results"), colorOf(harness.item(QStringLiteral("globalSearchNoResultsTitle"))), view, 4.5);
    QQuickItem *suggestion = harness.drawn(QStringLiteral("globalSearchSuggestionRow"));
    QVERIFY(suggestion != nullptr);
    check(QStringLiteral("suggestion"), colorOf(visibleChildNamed(suggestion, QStringLiteral("globalSearchAuxTitle"))), selection, 4.5);
    harness.bridge->setProperty("globalSearchSuggestion", QString());
    harness.search(QStringLiteral("zzzz"), {});
    QTRY_VERIFY(harness.item(QStringLiteral("globalSearchNoResultsHint"))->isVisible());
    check(QStringLiteral("no results hint"), colorOf(harness.item(QStringLiteral("globalSearchNoResultsHint"))), view, 4.5);

    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
}

void SearchPaletteTest::largeFontRowsAndFooterHoldTheirText_data() {
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<bool>("footerWraps");
    QTest::newRow("1600 px") << 1600 << false;
    QTest::newRow("narrow") << 560 << true;
}

// At 24 pt, the field, the rows and the headings are as tall as their text
// needs, and the palette, its title column and its help are as much wider
// as the font is larger (otherwise, at 800 px, titles were elided beside
// empty room and the footer wrapped). The footer's key hints stay on one
// line where the window has the room, and wrap inside the palette, which
// grows to hold their lines, where it has not. With the default font, the
// sizes stay those of the design, as the tests above pin.
void SearchPaletteTest::largeFontRowsAndFooterHoldTheirText() {
    QFETCH(int, windowWidth);
    QFETCH(bool, footerWraps);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont large = applicationFont;
    large.setPointSizeF(24);
    QGuiApplication::setFont(large);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText, windowWidth, 1000), qPrintable(errorText));
    QCOMPARE(harness.tokens->property("basePointSize").toReal(), 24.0);
    QVERIFY(harness.open());
    auto fixtureRows = meridianRows(harness.coverUrl);
    // A title too long for 300 px at 24 pt that fits the wider column.
    fixtureRows[3].label = QStringLiteral("Meridian Line, Second Take");
    harness.search(QStringLiteral("meridian"), fixtureRows, {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    const QRectF frame = sceneRect(harness.palette());
    QCOMPARE(frame.width(), std::min(std::round(800.0 * 2.4), windowWidth - 32.0));
    if (!footerWraps) {
        QCOMPARE(harness.dialog->property("titleWidth").toReal(), std::round(300.0 * 2.4));
    }
    // The syntax help as much wider, inside the palette.
    QObject *help = harness.dialog->findChild<QObject *>(QStringLiteral("globalSearchHelp"));
    QVERIFY(help != nullptr);
    QCOMPARE(help->property("width").toReal(), std::min(std::round(480.0 * 2.4), frame.width() - 32.0));

    // The field holds its line.
    const QFontMetricsF fieldMetrics(harness.field()->property("font").value<QFont>());
    QVERIFY2(harness.field()->height() >= fieldMetrics.height() + 8,
             qPrintable(QString::number(harness.field()->height())));

    // Every row in view holds its texts, centred in it; each heading holds
    // its text and the 4 px under it.
    int rows = 0;
    int headings = 0;
    qreal titleLeft = -1;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        QQuickItem *row = harness.row(i);
        if (row == nullptr || !row->isVisible() || row->height() <= 0) {
            continue;
        }
        const QString kind = harness.model.rowDataAt(i).value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("item") && titleLeft < 0) {
            titleLeft = sceneRect(visibleNamed(row, QStringLiteral("globalSearchResultTitle")).first()).left();
        }
        if (kind == QStringLiteral("item") && !footerWraps) {
            // No title is elided that its column could hold.
            for (QQuickItem *run : visibleNamed(row, QStringLiteral("globalSearchResultTitle"))) {
                QVERIFY2(!run->property("truncated").toBool(), qPrintable(run->property("text").toString()));
            }
        }
        if (kind == QStringLiteral("section")) {
            QQuickItem *heading = visibleChildNamed(row, QStringLiteral("globalSearchSectionHeading"));
            QVERIFY(heading != nullptr);
            QVERIFY2(row->height() >= heading->implicitHeight(),
                     qPrintable(QStringLiteral("a %1 px heading for %2 px of text")
                                    .arg(row->height()).arg(heading->implicitHeight())));
            QVERIFY(!heading->property("truncated").toBool());
            ++headings;
        } else if (kind == QStringLiteral("item")) {
            QList<QQuickItem *> texts = visibleNamed(row, QStringLiteral("globalSearchResultTitle"))
                + contextTexts(row) + visibleNamed(row, QStringLiteral("globalSearchResultLength"));
            QVERIFY(!texts.isEmpty());
            for (QQuickItem *text : texts) {
                QVERIFY2(row->height() >= text->implicitHeight() + 8,
                         qPrintable(QStringLiteral("a %1 px row for %2 px of text (%3)")
                                        .arg(row->height()).arg(text->implicitHeight())
                                        .arg(text->property("text").toString())));
                QCOMPARE(text->property("verticalAlignment").toInt(), int(Qt::AlignVCenter));
                QCOMPARE(sceneRect(text).center().y(), sceneRect(row).center().y());
            }
            ++rows;
        }
    }
    QVERIFY(rows >= 3);
    QVERIFY(headings >= 1);

    // The footer's four pairs on one line, or wrapped to more than one in
    // a narrow window, every pair whole and inside the palette, and the
    // footer is as tall as its lines.
    // Each line starts where the results' titles do.
    QTRY_COMPARE(harness.hints().size(), 4);
    QQuickItem *footer = harness.item(QStringLiteral("globalSearchFooter"));
    const QRectF footerRect = sceneRect(footer);
    QCOMPARE(footerRect.bottom(), frame.bottom());
    QList<qreal> lineTops;
    for (QQuickItem *hint : visibleNamed(footer, QStringLiteral("globalSearchKeyHint"))) {
        const QRectF rect = sceneRect(hint);
        QVERIFY2(within(rect, footerRect), qPrintable(hint->property("keyText").toString()));
        QVERIFY(rect.right() <= frame.right() - 16.0 + 0.5);
        for (QQuickItem *text : visibleNamed(hint, QStringLiteral("globalSearchKeyHintKey"))
                                    + visibleNamed(hint, QStringLiteral("globalSearchKeyHintAction"))) {
            QVERIFY(text->implicitHeight() <= rect.height());
            QVERIFY(within(sceneRect(text), footerRect));
        }
        if (!lineTops.contains(rect.top())) {
            lineTops << rect.top();
            QCOMPARE(rect.left(), titleLeft);
        }
        QVERIFY(rect.left() >= titleLeft);
        // A key stays further from its action than the action's words
        // from each other, at a large font too.
        {
            const auto keys = visibleNamed(hint, QStringLiteral("globalSearchKeyHintKey"));
            const auto actions = visibleNamed(hint, QStringLiteral("globalSearchKeyHintAction"));
            QVERIFY(keys.size() == 1 && actions.size() == 1);
            const QFontMetricsF small(actions.first()->property("font").value<QFont>());
            const qreal gap = sceneRect(actions.first()).left() - sceneRect(keys.first()).right();
            QVERIFY2(gap >= 1.4 * small.horizontalAdvance(QLatin1Char(' ')),
                     qPrintable(QStringLiteral("%1 vs a space of %2").arg(gap).arg(small.horizontalAdvance(QLatin1Char(' ')))));
        }
    }
    if (footerWraps) {
        QVERIFY2(lineTops.size() >= 2, qPrintable(QString::number(lineTops.size())));
    } else {
        QCOMPARE(lineTops.size(), 1);
    }
    QQuickItem *hairline = harness.item(QStringLiteral("globalSearchFooterHairline"));
    QCOMPARE(sceneRect(hairline).bottom(), footerRect.top());
    QVERIFY(frame.bottom() <= harness.window->height() - 32.0);
}

void SearchPaletteTest::glyphsTargetsAndThumbnailsFollowTheFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("default font") << 0.0;
    QTest::newRow("24 pt") << 24.0;
}

// The field's search glyph, its clear and help buttons and their glyphs,
// and the results' covers and glyphs are sized for the default font and
// grow with a larger one, as the transport's do (at 24 pt they
// stayed at their 1× sizes beside 32 px text). The covers keep 4 px clear
// of their row's edges and the buttons 8 px of the field's; the query and
// the titles start 12 px after the lead.
void SearchPaletteTest::glyphsTargetsAndThumbnailsFollowTheFont() {
    QFETCH(qreal, pointSize);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont large = applicationFont;
        large.setPointSizeF(pointSize);
        QGuiApplication::setFont(large);
    }
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    const bool large = pointSize > 0;
    const qreal scale = large ? pointSize / 10.0 : 1.0;
    // The sizes UiPalette.scaledSize gives.
    // Even, so the glyphs centre on whole pixels.
    const auto scaled = [scale](qreal size) { return 2 * std::round(std::max(size, std::round(size * scale)) / 2); };
    const auto evenBelow = [](qreal size) { return 2 * std::floor(size / 2); };
    const qreal glyph = scaled(16);
    const qreal fieldHeight = harness.field()->height();
    const qreal rowHeight = harness.dialog->property("resultHeight").toReal();
    const qreal lead = evenBelow(std::min(std::max(24.0, std::round(24 * scale)), rowHeight - 8));
    const qreal button = evenBelow(std::min(std::max(28.0, std::round(28 * scale)), fieldHeight - 16));
    if (large) {
        // 38 px glyphs, 46 px covers and 48 px buttons beside 32 px text.
        QCOMPARE(glyph, 38.0);
        QVERIFY2(lead >= 24 * 1.8 && button >= 28 * 1.6, qPrintable(QStringLiteral("%1 %2").arg(lead).arg(button)));
    } else {
        QCOMPARE(glyph, 16.0);
        QCOMPARE(lead, 24.0);
        QCOMPARE(button, 28.0);
    }
    const QRectF frame = sceneRect(harness.palette());
    QQuickItem *icon = harness.item(QStringLiteral("globalSearchFieldIcon"));
    QCOMPARE(icon->width(), glyph);
    QCOMPARE(icon->height(), glyph);
    QCOMPARE(sceneRect(icon).left() - frame.left(), 16.0);
    QVERIFY(std::abs(sceneRect(icon).center().y() - sceneRect(harness.field()).center().y()) <= 0.5);
    QCOMPARE(sceneRect(harness.field()).left() - frame.left(), 16.0 + lead + 12.0);
    QQuickItem *clear = harness.item(QStringLiteral("globalSearchClearButton"));
    QQuickItem *help = harness.item(QStringLiteral("globalSearchHelpButton"));
    QVERIFY(clear != nullptr && help != nullptr && clear->isVisible());
    for (QQuickItem *target : {clear, help}) {
        QCOMPARE(target->width(), button);
        QCOMPARE(target->height(), button);
        QVERIFY(sceneRect(target).top() - sceneRect(harness.field()).top() >= 8.0 - 0.5);
        QQmlExpression iconWidth(qmlContext(target), target, QStringLiteral("icon.width"));
        QQmlExpression iconHeight(qmlContext(target), target, QStringLiteral("icon.height"));
        QCOMPARE(iconWidth.evaluate().toReal(), glyph);
        QCOMPARE(iconHeight.evaluate().toReal(), glyph);
        QVERIFY(button >= glyph + 8);
    }
    QCOMPARE(frame.right() - sceneRect(help).right(), 8.0);
    QCOMPARE(sceneRect(clear).right(), sceneRect(help).left());

    int thumbnails = 0;
    int glyphs = 0;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) != QStringLiteral("item")) continue;
        QQuickItem *row = harness.row(i);
        if (row == nullptr || !row->isVisible()) continue;
        QCOMPARE(sceneRect(visibleNamed(row, QStringLiteral("globalSearchResultTitle")).first()).left() - frame.left(),
                 16.0 + lead + 12.0);
        if (QQuickItem *thumbnail = visibleChildNamed(row, QStringLiteral("globalSearchThumbnail"))) {
            ++thumbnails;
            QCOMPARE(thumbnail->width(), lead);
            QCOMPARE(thumbnail->height(), lead);
            QCOMPARE(sceneRect(thumbnail).left() - frame.left(), 16.0);
            QVERIFY(sceneRect(thumbnail).top() - sceneRect(row).top() >= 4.0 - 0.5);
            QVERIFY(sceneRect(row).bottom() - sceneRect(thumbnail).bottom() >= 4.0 - 0.5);
        }
        for (const QString &name : {QStringLiteral("globalSearchArtistGlyph"), QStringLiteral("globalSearchCoverPlaceholder")}) {
            if (QQuickItem *leadGlyph = visibleChildNamed(row, name)) {
                ++glyphs;
                // The no-cover tile is as large as a cover.
                QCOMPARE(leadGlyph->width(), name == QStringLiteral("globalSearchArtistGlyph") ? glyph : lead);
                QCOMPARE(sceneRect(leadGlyph).center().x() - frame.left(), 16.0 + lead / 2);
            }
        }
    }
    QVERIFY(thumbnails >= 2);
    QVERIFY(glyphs >= 1);

    // The playing glyph over its cover, 12 px at the default font.
    harness.bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/line.flac"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("label")) != QStringLiteral("Meridian Line")) continue;
        QQuickItem *row = harness.row(i);
        QVERIFY(row != nullptr);
        QQuickItem *now = visibleChildNamed(row, QStringLiteral("globalSearchNowGlyph"));
        QVERIFY(now != nullptr);
        QCOMPARE(now->width(), scaled(12));
        QCOMPARE(sceneRect(now).center(), sceneRect(visibleChildNamed(row, QStringLiteral("globalSearchThumbnail"))).center());
    }
}

void SearchPaletteTest::rowColumnsNeverMeet_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::newRow("default font") << 0.0 << 1600;
    QTest::newRow("default font, narrow") << 0.0 << 560;
    QTest::newRow("24 pt") << 24.0 << 1600;
    QTest::newRow("24 pt, 1000 px") << 24.0 << 1000;
    QTest::newRow("24 pt, narrow") << 24.0 << 560;
    QTest::newRow("24 pt, 1920 px") << 24.0 << 1920;
}

// A track's length holds its text whole at the right, and the title and
// the context keep 12 px clear of each other and of it, whatever the font
// and the palette's width: at 24 pt the length ran 14 px into the context.
void SearchPaletteTest::rowColumnsNeverMeet() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont large = applicationFont;
        large.setPointSizeF(pointSize);
        QGuiApplication::setFont(large);
    }
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText, windowWidth, 1000), qPrintable(errorText));
    QVERIFY(harness.open());
    auto rows = meridianRows(harness.coverUrl);
    // A length of over an hour, and a title and context too long for their
    // columns.
    rows[3].lengthText = QStringLiteral("1:05:22");
    rows[4].label = QStringLiteral("A Remarkably Long Track Title That Will Certainly Not Fit In Its Column");
    rows[4].album = QStringLiteral("An Album Whose Name Runs On Well Past the Room Its Column Has");
    harness.search(QStringLiteral("meridian"), rows, {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    const QRectF frame = sceneRect(harness.palette());
    const qreal gap = 12.0;

    int tracks = 0;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) != QStringLiteral("item")) continue;
        QQuickItem *row = harness.row(i);
        if (row == nullptr || !row->isVisible()) continue;
        const QString label = harness.model.rowDataAt(i).value(QStringLiteral("label")).toString();
        const QByteArray at = (label + QStringLiteral(" at ") + QString::number(pointSize) + QStringLiteral(" pt, ")
                               + QString::number(windowWidth) + QStringLiteral(" px")).toUtf8();
        const auto runs = visibleNamed(row, QStringLiteral("globalSearchResultTitle"));
        QVERIFY2(!runs.isEmpty(), at.constData());
        QVERIFY2(!contextTexts(row).isEmpty(), at.constData());
        const QRectF contextRect = contextBoxOf(row);
        // The title ends 12 px before the context starts.
        for (QQuickItem *run : runs) {
            QVERIFY2(sceneRect(run).right() <= contextRect.left() - gap + 0.5,
                     qPrintable(QStringLiteral("%1: title to %2, context from %3")
                                    .arg(QString::fromUtf8(at)).arg(sceneRect(run).right()).arg(contextRect.left())));
        }
        QVERIFY2(contextRect.right() <= frame.right() - 16.0 + 0.5, at.constData());
        QQuickItem *length = visibleChildNamed(row, QStringLiteral("globalSearchResultLength"));
        if (length == nullptr) continue;
        ++tracks;
        const QRectF lengthRect = sceneRect(length);
        // Its text whole inside its box, the box 16 px in from the right and
        // 12 px clear of the context.
        QVERIFY2(length->implicitWidth() <= length->width() + 0.5,
                 qPrintable(QStringLiteral("%1: %2 px of text in %3 px")
                                .arg(QString::fromUtf8(at)).arg(length->implicitWidth()).arg(length->width())));
        QVERIFY2(lengthRect.right() <= frame.right() - 16.0 + 0.5, at.constData());
        QVERIFY2(contextRect.right() <= lengthRect.left() - gap + 0.5,
                 qPrintable(QStringLiteral("%1: context to %2, length from %3")
                                .arg(QString::fromUtf8(at)).arg(contextRect.right()).arg(lengthRect.left())));
        // The usual lengths share one column.
        if (length->property("text").toString().size() <= 5) {
            QCOMPARE(length->width(), harness.dialog->property("lengthWidth").toReal());
        }
    }
    QVERIFY(tracks >= 2);
    if (pointSize == 0.0 && windowWidth == 1600) {
        // The design's sizes with the usual font.
        QCOMPARE(harness.dialog->property("lengthWidth").toReal(), 44.0);
        QCOMPARE(harness.dialog->property("titleWidth").toReal(), 380.0);
    }
    if (pointSize == 24.0 && windowWidth == 1920) {
        QCOMPARE(harness.dialog->property("titleWidth").toReal(), std::round(380.0 * 2.4));
    }
    // The title grows only into room the context can spare: the context
    // keeps 276 px, scaled, or where it had less beside a title of up to
    // 300 px, all of that.
    const qreal scale = pointSize > 0 ? pointSize / 10.0 : 1.0;
    const qreal title = harness.dialog->property("titleWidth").toReal();
    const qreal room = frame.width() - 16 - 8 - harness.dialog->property("lengthWidth").toReal() - 2 * gap
        - harness.dialog->property("titleX").toReal();
    const qreal before = std::min(std::round(300.0 * scale), std::round(room * 0.6));
    QVERIFY2(room - title >= std::min(room - before, std::round(276.0 * scale)) - 1,
             qPrintable(QStringLiteral("a title of %1 leaves the context %2").arg(title).arg(room - title)));
    QVERIFY(title >= before);
}

void SearchPaletteTest::headingsStartTheirSections_data() {
    QTest::addColumn<qreal>("points");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("14 pt") << 14.0;
    QTest::newRow("24 pt") << 24.0;
}

// A heading groups with its section: at least 1.7 times the space above its
// text as under it, whether a result or a Show all link ends the section
// before (the headings sat some 30 px from the rows either side, less than
// the step between two results, so the sections ran on as one list). The
// first has no section to part from.
void SearchPaletteTest::headingsStartTheirSections() {
    QFETCH(qreal, points);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont font = applicationFont;
    font.setPointSizeF(points);
    QGuiApplication::setFont(font);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText, 1600, 1600), qPrintable(errorText));
    QVERIFY(harness.open());
    // More albums than show: that section ends in a link.
    auto rows = meridianRows(harness.coverUrl);
    rows[1].score = 0.5f;
    harness.search(QStringLiteral("meridian"), rows, {1, 9, 4});
    QTRY_VERIFY(harness.view()->isVisible());

    const auto textBox = [](QQuickItem *text) {
        const QRectF box = sceneRect(text);
        const qreal lines = text->property("contentHeight").toReal();
        const qreal top = text->property("verticalAlignment").toInt() == int(Qt::AlignBottom)
            ? box.bottom() - text->property("bottomPadding").toReal() - lines
            : box.center().y() - lines / 2;
        return QRectF(box.left(), top, box.width(), lines);
    };
    const auto mainText = [&](int index) -> QQuickItem * {
        QQuickItem *row = harness.row(index);
        if (row == nullptr) return nullptr;
        for (const char *name : {"globalSearchResultTitle", "globalSearchMoreLink", "globalSearchSectionHeading"}) {
            if (QQuickItem *text = visibleChildNamed(row, QString::fromLatin1(name))) return text;
        }
        return nullptr;
    };
    int parted = 0;
    QStringList before;
    for (int i = 1; i + 1 < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) != QStringLiteral("section")) continue;
        QQuickItem *previous = mainText(i - 1);
        QQuickItem *heading = mainText(i);
        QQuickItem *next = mainText(i + 1);
        QVERIFY(previous != nullptr && heading != nullptr && next != nullptr);
        before << harness.model.rowDataAt(i - 1).value(QStringLiteral("kind")).toString();
        const qreal above = textBox(heading).top() - textBox(previous).bottom();
        const qreal below = textBox(next).top() - textBox(heading).bottom();
        QVERIFY2(above >= 1.7 * below,
                 qPrintable(QStringLiteral("%1 px above row %2's heading, %3 under it").arg(above).arg(i).arg(below)));
        QVERIFY(textBox(heading).top() >= sceneRect(harness.row(i)).top());
        ++parted;
    }
    QCOMPARE(parted, 2);
    QVERIFY(before.contains(QStringLiteral("more")) && before.contains(QStringLiteral("item")));
}

void SearchPaletteTest::tooltipStaysOffTheOtherResults_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::addColumn<qreal>("points");
    QTest::addColumn<int>("extraTracks");
    QTest::addColumn<QString>("place");
    const QString beside = QStringLiteral("beside");
    const QString below = QStringLiteral("below");
    const QString inside = QStringLiteral("clear");
    QTest::newRow("1600, many") << 1600 << 1000 << 10.0 << 22 << beside;
    QTest::newRow("1600, few") << 1600 << 1000 << 10.0 << 0 << beside;
    QTest::newRow("1000, few, room under the palette") << 1000 << 720 << 10.0 << 0 << below;
    QTest::newRow("1000, many") << 1000 << 600 << 10.0 << 22 << inside;
    QTest::newRow("1000 at 24 pt") << 1000 << 720 << 24.0 << 22 << inside;
    QTest::newRow("1600 at 24 pt") << 1600 << 1000 << 24.0 << 22 << inside;
    QTest::newRow("1280, wrapped beside") << 1280 << 800 << 10.0 << 22 << beside;
}

// A result's tooltip leaves the results around it readable where the
// window has room outside the palette: beside it, level with the row, or
// under it (it covered the next two results, often to add only the
// genre). Without that room it is clear of its own row, the query field and
// the footer.
void SearchPaletteTest::tooltipStaysOffTheOtherResults() {
    QFETCH(int, width);
    QFETCH(int, height);
    QFETCH(qreal, points);
    QFETCH(int, extraTracks);
    QFETCH(QString, place);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont font = applicationFont;
    font.setPointSizeF(points);
    QGuiApplication::setFont(font);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText, width, height), qPrintable(errorText));
    QVERIFY(harness.open());
    auto rows = meridianRows(harness.coverUrl);
    for (int i = 0; i < extraTracks; ++i) {
        Row extra = result(QStringLiteral("track"), QStringLiteral("Extra %1").arg(i), 5.0f);
        extra.trackPath = QStringLiteral("/fixture/extra%1.flac").arg(i);
        rows << extra;
    }
    for (Row &row : rows) row.genre = QStringLiteral("Ambient");
    harness.search(QStringLiteral("meridian"), rows);
    QTRY_VERIFY(harness.view()->isVisible());
    QObject *toolTip = harness.root->findChild<QObject *>(QStringLiteral("globalSearchResultToolTip"));
    QVERIFY(toolTip != nullptr);
    toolTip->setProperty("delay", 0);
    auto *content = toolTip->property("contentItem").value<QQuickItem *>();
    QVERIFY(content != nullptr);

    const QRectF frame = sceneRect(harness.palette());
    const QRectF view = sceneRect(harness.view());
    const QRectF field = sceneRect(harness.item(QStringLiteral("globalSearchQueryField")));
    const QRectF footer = sceneRect(harness.item(QStringLiteral("globalSearchFooter")));
    int hovered = 0;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("kind")) != QStringLiteral("item")) continue;
        QQuickItem *row = harness.row(i);
        if (row == nullptr || sceneRect(row).bottom() > view.bottom() + 0.5) continue;
        QTest::mouseMove(harness.window, sceneCentre(row));
        QTRY_COMPARE(harness.dialog->property("toolTipRow").value<QQuickItem *>(), row);
        QTRY_VERIFY(toolTip->property("opened").toBool());
        QCoreApplication::processEvents();
        const QRectF tip = sceneRect(content->parentItem());
        const QRectF rowRect = sceneRect(row);
        const QByteArray at = QStringLiteral("row %1: the tooltip at %2,%3 %4x%5")
                                  .arg(i).arg(tip.left()).arg(tip.top()).arg(tip.width()).arg(tip.height()).toUtf8();
        QVERIFY2(tip.top() >= 0 && tip.bottom() <= height && tip.left() >= 0 && tip.right() <= width, at.constData());
        if (place == QStringLiteral("beside")) {
            QVERIFY2(tip.left() >= frame.right(), at.constData());
            QVERIFY2(std::abs(tip.center().y() - rowRect.center().y()) <= 1.0, at.constData());
        } else if (place == QStringLiteral("below")) {
            QVERIFY2(tip.top() >= frame.bottom(), at.constData());
        }
        QVERIFY2(!tip.intersects(rowRect.adjusted(1, 1, -1, -1)), at.constData());
        QVERIFY2(!tip.intersects(field.adjusted(1, 1, -1, -1)), at.constData());
        QVERIFY2(!tip.intersects(footer.adjusted(1, 1, -1, -1)), at.constData());
        ++hovered;
    }
    QVERIFY(hovered >= 3);
}

// As the rows change under it and the palette shrinks, the tooltip fades
// where it was, beside the palette and under it.
void SearchPaletteTest::tooltipFadesWhereItWasAsTheRowsChange() {
    for (const int width : {1600, 1000}) {
        Harness harness;
        QString errorText;
        QVERIFY2(harness.load(kBreezeLight, &errorText, width, 720), qPrintable(errorText));
        QVERIFY(harness.open());
        auto rows = meridianRows(harness.coverUrl);
        for (Row &row : rows) row.genre = QStringLiteral("Ambient");
        harness.search(QStringLiteral("meridian"), rows);
        QTRY_VERIFY(harness.view()->isVisible());
        QObject *toolTip = harness.root->findChild<QObject *>(QStringLiteral("globalSearchResultToolTip"));
        QVERIFY(toolTip != nullptr);
        toolTip->setProperty("delay", 0);
        auto *content = toolTip->property("contentItem").value<QQuickItem *>();
        QVERIFY(content != nullptr);
        QQuickItem *row = harness.row(harness.model.indexOfResult(QStringLiteral("album"), 1));
        QVERIFY(row != nullptr);
        QTest::mouseMove(harness.window, sceneCentre(row));
        QTRY_VERIFY(toolTip->property("opened").toBool());
        const QPointF shownAt(toolTip->property("x").toReal(), toolTip->property("y").toReal());
        const qreal height = harness.palette()->height();

        // Typed on: the results, and with them the palette, shrink.
        harness.bridge->setProperty("globalSearchBusy", true);
        harness.field()->setProperty("text", QStringLiteral("meridian l"));
        harness.model.presentSearchRows(rows.mid(0, 2), {1, 1, 0}, true);
        QCoreApplication::processEvents();
        QVERIFY(harness.palette()->height() < height);
        QCOMPARE(QPointF(toolTip->property("x").toReal(), toolTip->property("y").toReal()), shownAt);
        // And over nothing the window keeps clear, as drawn.
        QVERIFY(!sceneRect(content->parentItem())
                     .intersects(sceneRect(harness.item(QStringLiteral("globalSearchQueryField")))));
    }
}

void SearchPaletteTest::clearGlyphIsQuietUntilPointedAt_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
}

// Clear's solid glyph is quieter at rest than the line glyphs beside it,
// and still 3:1 on the view (in their colour it was the field row's
// heaviest mark); under the pointer and with the keyboard's focus it takes
// their colour.
void SearchPaletteTest::clearGlyphIsQuietUntilPointedAt() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl));
    QQuickItem *clear = harness.item(QStringLiteral("globalSearchClearButton"));
    QVERIFY(clear != nullptr && clear->isVisible());
    QTest::mouseMove(harness.window, QPoint(2, 2));
    QQmlExpression iconColor(qmlContext(clear), clear, QStringLiteral("icon.color"));
    const auto drawnIn = [&iconColor] { return iconColor.evaluate().value<QColor>(); };
    const QColor view = harness.token("view");
    const QColor pointedAt = harness.token("textSecondary");
    const double atRest = contrastRatio(drawnIn(), view);
    QVERIFY2(atRest >= 3.0, qPrintable(describeContrast(QStringLiteral("clear"), drawnIn(), view)));
    QVERIFY2(atRest <= 0.65 * contrastRatio(pointedAt, view),
             qPrintable(describeContrast(QStringLiteral("clear"), drawnIn(), view)));
    const QColor rest = drawnIn();
    // Breeze's style turns hover effects off without a desktop session.
    QVERIFY(clear->setProperty("hoverEnabled", true));
    QTest::mouseMove(harness.window, sceneCentre(clear));
    QTRY_VERIFY(channelDistance(drawnIn(), pointedAt) <= 1);
    QTest::mouseMove(harness.window, QPoint(2, 2));
    QTRY_VERIFY(channelDistance(drawnIn(), rest) <= 1);
    clear->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(channelDistance(drawnIn(), pointedAt) <= 1);
}

// Typing across the point where the results scroll, every frame: the
// selection and hover tints end at the scrollbar's lane while it shows (the
// thumb cut a pale piece out of the selected row) and at the edge without
// it, and nothing moves sideways: the field, the lengths and the context's
// room, which the lane took 8 px of as it appeared.
void SearchPaletteTest::tintsEndAtTheScrollbarsLaneAndTheTextStaysPut() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    const QVector<Row> few = meridianRows(harness.coverUrl);
    QVector<Row> many = few;
    for (int i = 0; i < 24; ++i) {
        Row extra = result(QStringLiteral("track"), QStringLiteral("Extra %1").arg(i), 5.0f);
        extra.artist = QStringLiteral("The Quiet Meridian");
        extra.lengthText = QStringLiteral("1:23");
        extra.trackPath = QStringLiteral("/fixture/extra%1.flac").arg(i);
        many << extra;
    }
    harness.search(QStringLiteral("m"), few);
    QTRY_VERIFY(harness.view()->isVisible());
    QQuickItem *view = harness.view();
    QQuickItem *scrollBar = harness.item(QStringLiteral("globalSearchScrollBar"));
    QVERIFY(scrollBar != nullptr && !scrollBar->isVisible());
    QQuickItem *selected = harness.row(harness.selection());
    QVERIFY(selected != nullptr);
    QQuickItem *length = visibleChildNamed(selected, QStringLiteral("globalSearchResultLength"));
    QVERIFY(length != nullptr);
    const qreal lengthRight = sceneRect(length).right();
    const qreal contextRight = contextBoxOf(selected).right();
    const QRectF field = sceneRect(harness.field());

    QStringList problems;
    int withBar = 0;
    int without = 0;
    const auto check = [&] {
        if (!view->isVisible()) return;
        const bool bar = scrollBar->isVisible();
        (bar ? withBar : without) += 1;
        const qreal end = sceneRect(view).right() - (bar ? 8 : 0);
        for (const char *name : {"rowBackgroundTint", "rowBackgroundHover"}) {
            for (QQuickItem *tint : visibleNamed(view, QString::fromLatin1(name))) {
                if (std::abs(sceneRect(tint).right() - end) > 0.5) {
                    problems << QStringLiteral("%1 to %2, not %3").arg(QLatin1String(name)).arg(sceneRect(tint).right()).arg(end);
                }
            }
        }
        for (QQuickItem *text : visibleNamed(view, QStringLiteral("globalSearchResultLength"))) {
            if (std::abs(sceneRect(text).right() - lengthRight) > 0.5) {
                problems << QStringLiteral("a length to %1, not %2").arg(sceneRect(text).right()).arg(lengthRight);
            }
            const qreal context = contextBoxOf(text->parentItem()).right();
            // A length of an hour or more is wider; these are not.
            if (std::abs(context - contextRight) > 0.5) {
                problems << QStringLiteral("a context to %1, not %2").arg(context).arg(contextRight);
            }
        }
        if (sceneRect(harness.field()) != field) problems << QStringLiteral("the field moved");
    };
    FrameSampler sampler(harness.window, check);
    // A pointer over another result, for the hover tint too.
    QQuickItem *other = harness.row(harness.selection() + 1);
    QVERIFY(other != nullptr);
    QTest::mouseMove(harness.window, sceneCentre(other));
    QTRY_VERIFY(visibleChildNamed(other, QStringLiteral("rowBackgroundHover")) != nullptr);
    check();
    harness.search(QStringLiteral("me"), many);
    QTRY_VERIFY(scrollBar->isVisible());
    QTest::qWait(50);
    check();
    harness.search(QStringLiteral("mer"), few);
    QTRY_VERIFY(!scrollBar->isVisible());
    QTest::qWait(50);
    check();
    sampler.stop();
    QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 4).join(QStringLiteral("; "))));
    QVERIFY(withBar > 0 && without > 0);
}

void SearchPaletteTest::noCoverTileIsTheLibrarys_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<qreal>("points");
    QTest::newRow("dark, 10 pt") << true << 10.0;
    QTest::newRow("light, 10 pt") << false << 10.0;
    QTest::newRow("dark, 24 pt") << true << 24.0;
    QTest::newRow("light, 24 pt") << false << 24.0;
}

// An album or a track without a cover has the library's tile in a cover's
// place, at its size: the floor, its edge and the quiet mark, as drawn, so
// it reads at 24 pt too (the track's glyph became a grey blob there, the
// album's a green one). While its track plays it has the amber glyph over a
// scrim, as a cover has.
void SearchPaletteTest::noCoverTileIsTheLibrarys() {
    QFETCH(bool, dark);
    QFETCH(qreal, points);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont font = applicationFont;
    font.setPointSizeF(points);
    QGuiApplication::setFont(font);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, &errorText, 1600, 1600), qPrintable(errorText));
    QVERIFY(harness.open());
    auto rows = meridianRows(harness.coverUrl);
    rows[1].coverUrl.clear();
    harness.search(QStringLiteral("meridian"), rows);
    QTRY_VERIFY(harness.view()->isVisible());
    QTest::mouseMove(harness.window, QPoint(2, 2));

    const qreal lead = harness.dialog->property("leadSize").toReal();
    const QColor floor = harness.token("plotFloor");
    const QColor mark = QColor::fromRgb(0x8c, 0x82, 0xa3);
    QVERIFY2(contrastRatio(mark, floor) >= 4.5, qPrintable(describeContrast(QStringLiteral("mark"), mark, floor)));
    QStringList types;
    QQuickItem *trackRow = nullptr;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        const QVariantMap data = harness.model.rowDataAt(i);
        QQuickItem *row = harness.row(i);
        if (row == nullptr) continue;
        QQuickItem *tile = visibleChildNamed(row, QStringLiteral("globalSearchCoverPlaceholder"));
        if (tile == nullptr) continue;
        types << data.value(QStringLiteral("rowType")).toString();
        if (types.last() == QStringLiteral("track")) trackRow = row;
        const QRectF box = sceneRect(tile);
        QCOMPARE(box.size(), QSizeF(lead, lead));
        QCOMPARE(box.left() - sceneRect(harness.palette()).left(), 16.0);
        QCOMPARE(tile->property("radius").toReal(), 2.0);
        QCOMPARE(hex(colorOf(tile)), hex(floor));
        QCOMPARE(hex(tile->property("border").value<QObject *>()->property("color").value<QColor>()),
                 hex(harness.token("plotFloorEdge")));
        const auto problem = [&]() -> QString {
            const QImage image = harness.window->grabWindow();
            const qreal scale = qreal(image.width()) / harness.window->width();
            int marked = 0;
            for (qreal y = 2.5; y < lead - 2; ++y) {
                for (qreal x = 2.5; x < lead - 2; ++x) {
                    const QColor pixel = image.pixelColor(((box.topLeft() + QPointF(x, y)) * scale).toPoint());
                    marked += channelDistance(pixel, mark) <= 12 ? 1 : 0;
                }
            }
            return marked >= lead ? QString() : QStringLiteral("%1 pixels of the mark").arg(marked);
        };
        QTRY_VERIFY2(problem().isEmpty(), qPrintable(problem()));
    }
    types.sort();
    QCOMPARE(types, (QStringList{QStringLiteral("album"), QStringLiteral("track")}));
    QVERIFY(trackRow != nullptr);

    harness.bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/lantern.flac"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QQuickItem *tile = visibleChildNamed(trackRow, QStringLiteral("globalSearchCoverPlaceholder"));
    QQuickItem *scrim = visibleChildNamed(trackRow, QStringLiteral("globalSearchNowScrim"));
    QQuickItem *now = visibleChildNamed(trackRow, QStringLiteral("globalSearchNowGlyph"));
    QVERIFY(tile != nullptr && scrim != nullptr && now != nullptr);
    QCOMPARE(sceneRect(scrim), sceneRect(tile));
    QVERIFY(std::abs(sceneRect(now).center().x() - sceneRect(tile).center().x()) <= 0.5);
    QVERIFY2(contrastRatio(colorOf(now), drawnOver(colorOf(scrim), mark)) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("playing glyph"), colorOf(now),
                                         drawnOver(colorOf(scrim), mark))));
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // Accessible interfaces exist while assistive technology is active.
    qputenv("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1");
    // The basic render loop, whatever the environment asks for: the threaded
    // one installs an animation driver of its own for the GUI thread, and Qt
    // then takes no SteppedAnimationDriver.
    qputenv("QSG_RENDER_LOOP", "basic");
    // The app's controls style and Breeze icons, whatever the desktop the
    // tests run in; TestSessionIsolation.h.
    isolateFromDesktopSessionWithAppLook();
    // Settings, data and caches of the test's own: no developer's icon
    // theme, colours or caches reach the tests.
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    qputenv("LANGUAGE", "en_US");
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    QApplication app(argc, argv);
    returnFocusFromClosedPopups();
    QAccessible::setActive(true);
    FerrousLocalization::registerQmlTypes();
    SearchPaletteTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_search_palette.moc"
