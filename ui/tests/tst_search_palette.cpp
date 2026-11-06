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
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "GlobalSearchResultsModel.h"
#include "Localization.h"
#include "TestPlatform.h"

namespace {

QMutex &messageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &warnings() {
    static QStringList list;
    return list;
}

QtMessageHandler &previousHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void recordWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg) {
        QMutexLocker locker(&messageMutex());
        warnings().push_back(message);
    }
    if (previousHandler()) {
        previousHandler()(type, context, message);
    }
}

QStringList takeWarnings() {
    QMutexLocker locker(&messageMutex());
    QStringList list = warnings();
    warnings().clear();
    return list;
}

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
    // No cover: a placeholder glyph.
    Row bare = track(QStringLiteral("Lantern Signal"), QStringLiteral("Small Hours"), 4.0f, QStringLiteral("/fixture/lantern.flac"));
    bare.coverUrl.clear();
    rows << bare;
    return rows;
}

QRectF sceneRect(QQuickItem *item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

QPoint sceneCentre(QQuickItem *item) {
    return sceneRect(item).center().toPoint();
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

// `top` drawn over the opaque `bottom`.
QColor drawnOver(const QColor &top, const QColor &bottom) {
    const double alpha = top.alphaF();
    return QColor::fromRgbF(static_cast<float>(top.redF() * alpha + bottom.redF() * (1.0 - alpha)),
                            static_cast<float>(top.greenF() * alpha + bottom.greenF() * (1.0 - alpha)),
                            static_cast<float>(top.blueF() * alpha + bottom.blueF() * (1.0 - alpha)));
}

// WCAG's relative luminance and contrast ratio of opaque colours.
double relativeLuminance(const QColor &color) {
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}

double contrastRatio(const QColor &a, const QColor &b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QString describe(const QString &what, const QColor &a, const QColor &b) {
    return QStringLiteral("%1: %2 on %3 is %4:1")
        .arg(what, a.name(QColor::HexArgb), b.name(QColor::HexArgb))
        .arg(contrastRatio(a, b), 0, 'f', 2);
}

QColor colorOf(QQuickItem *item) {
    return item->property("color").value<QColor>();
}

// A colour as #aarrggbb, to compare colours whatever their spec.
QString hex(const QColor &color) {
    return color.name(QColor::HexArgb);
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
    void rowsShareTheirColumnsAndThumbnails();
    void titleMatchIsSemiboldAndContextStaysRegular();
    void selectedHoveredAndPlayingRowsAreDrawn();
    void footerNamesTheKeysForEachState();
    void suggestionIsASelectableRow();
    void recentSearchesShowBeforeTyping();
    void moreRowShowsTheWholeSectionAndKeepsTheSelection();
    void keyboardSelectsAndActs();
    void mouseSelectsAndActs();
    void staleAndBusyResultsDoNotAct();
    void paletteAndDimFadeTogether();
    void clearUsesTheBreezeIconAndNoTextGlyphs();
    void textMeetsItsContrastAsDrawn_data();
    void textMeetsItsContrastAsDrawn();
};

void SearchPaletteTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(recordWarnings);
    QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                     QStringLiteral("icons/breeze/index.theme"))
                  .isEmpty(),
             "The Breeze icon theme is missing: install breeze-icon-theme (docs/INSTALL.md)");
}

void SearchPaletteTest::cleanup() {
    const QStringList list = takeWarnings();
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
    QCOMPARE(qRound(colorOf(dim).alphaF() * 100), dark ? 50 : 25);
}

// Each type's results under its heading, in the order of the best match of
// each; 28 px headings in 12 px semibold secondary text at the title
// column, 32 px rows; the first result selected.
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
            QCOMPARE(rowItem->height(), 28.0);
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

// The lead at 16 px (a 24 px thumbnail with a 2 px radius, the artist
// glyph, or a placeholder without a cover), the title at 52 px in a 300 px
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
        QVERIFY(sceneRect(runs.last()).right() - left <= 52.0 + 300.0 + 0.5);
        QQuickItem *context = visibleChildNamed(row, QStringLiteral("globalSearchResultContext"));
        QVERIFY(context != nullptr);
        contextXs << sceneRect(context).left() - left;
        QCOMPARE(context->property("font").value<QFont>().pointSizeF(), harness.tokens->property("smallPointSize").toReal());
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
            QCOMPARE(visibleChildNamed(row, QStringLiteral("globalSearchResultContext"))->property("text").toString(),
                     QStringLiteral("4 albums"));
        }
        const QString type = harness.model.rowDataAt(i).value(QStringLiteral("rowType")).toString();
        if (type == QStringLiteral("track")) {
            QQuickItem *length = visibleChildNamed(row, QStringLiteral("globalSearchResultLength"));
            QVERIFY(length != nullptr);
            QCOMPARE(sceneRect(length).right(), sceneRect(row).right() - 16);
        }
        if (type == QStringLiteral("album")) {
            QVERIFY(context->property("text").toString().startsWith(QStringLiteral("The Quiet Meridian · ")));
        }
    }
    QCOMPARE(contextXs.size(), 7);
    QVERIFY(std::all_of(contextXs.cbegin(), contextXs.cend(), [](qreal x) { return x == 364.0; }));
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
// context has no emphasis.
void SearchPaletteTest::titleMatchIsSemiboldAndContextStaysRegular() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    harness.search(QStringLiteral("meridian"), meridianRows(harness.coverUrl), {1, 2, 4});
    QTRY_VERIFY(harness.view()->isVisible());
    QQuickItem *row = nullptr;
    for (int i = 0; i < harness.model.rowCount() && row == nullptr; ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("label")) == QStringLiteral("Meridian Line")) row = harness.row(i);
    }
    QVERIFY(row != nullptr);
    const auto runs = visibleNamed(row, QStringLiteral("globalSearchResultTitle"));
    QCOMPARE(runs.size(), 2);
    QCOMPARE(runs[0]->property("text").toString(), QStringLiteral("Meridian"));
    QCOMPARE(runs[0]->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(runs[1]->property("text").toString(), QStringLiteral(" Line"));
    QCOMPARE(runs[1]->property("font").value<QFont>().weight(), QFont::Normal);
    // The second run starts where the first ends: the space stays.
    QVERIFY(sceneRect(runs[1]).left() > sceneRect(runs[0]).left() + 40);
    QCOMPARE(runs[0]->property("textFormat").toInt(), int(Qt::PlainText));

    QQuickItem *context = visibleChildNamed(row, QStringLiteral("globalSearchResultContext"));
    QCOMPARE(context->property("text").toString(), QStringLiteral("The Quiet Meridian — Grey Lines"));
    QCOMPARE(context->property("textFormat").toInt(), int(Qt::PlainText));
    QCOMPARE(context->property("font").value<QFont>().weight(), QFont::Normal);

    // Two words, two matches; the rest regular.
    harness.search(QStringLiteral("quiet line"), meridianRows(harness.coverUrl));
    QStringList emphasised;
    for (int i = 0; i < harness.model.rowCount(); ++i) {
        if (harness.model.rowDataAt(i).value(QStringLiteral("label")) != QStringLiteral("The Quiet Meridian")) continue;
        for (QQuickItem *run : visibleNamed(harness.row(i), QStringLiteral("globalSearchResultTitle"))) {
            if (run->property("font").value<QFont>().weight() == QFont::DemiBold) emphasised << run->property("text").toString();
        }
    }
    QCOMPARE(emphasised, QStringList{QStringLiteral("Quiet")});
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
    QTest::mouseMove(harness.window, sceneCentre(other));
    QTRY_COMPARE(hover->opacity(), 1.0);
    const QColor view = harness.token("view");
    const QColor hoverDrawn = drawnOver(colorOf(hover), view);
    const QColor selectionDrawn = harness.token("selection");
    QVERIFY2(hoverDrawn != selectionDrawn, qPrintable(describe(QStringLiteral("hover vs selection"), hoverDrawn, selectionDrawn)));
    // Visibly different: neither reads as the other.
    QVERIFY2(std::abs(relativeLuminance(hoverDrawn) - relativeLuminance(selectionDrawn)) > 0.005
                 || std::abs(hoverDrawn.hueF() - selectionDrawn.hueF()) > 0.1,
             qPrintable(describe(QStringLiteral("hover vs selection"), hoverDrawn, selectionDrawn)));
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
// text colour, the action in the secondary one, 18 px between pairs.
void SearchPaletteTest::footerNamesTheKeysForEachState() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, &errorText), qPrintable(errorText));
    QVERIFY(harness.open());
    // Before typing, with no recent searches.
    QTRY_COMPARE(harness.hints(), QStringList{QStringLiteral("Esc Close")});

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
// menu.
void SearchPaletteTest::mouseSelectsAndActs() {
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
    QTest::mouseClick(harness.window, Qt::RightButton, Qt::NoModifier, sceneCentre(harness.row(track)));
    QCOMPARE(harness.selection(), track);
    QCOMPARE(harness.dialog->property("contextRowData").toMap().value(QStringLiteral("label")).toString(),
             QStringLiteral("Lantern Signal"));
    QQmlExpression menuOpened(qmlContext(harness.field()), harness.field(), QStringLiteral("resultMenu.opened"));
    QTRY_VERIFY2(menuOpened.evaluate().toBool(), qPrintable(menuOpened.error().toString() + QStringLiteral(" visible=")
        + QQmlExpression(qmlContext(harness.field()), harness.field(), QStringLiteral("resultMenu.visible")).evaluate().toString()));
    QTest::keyClick(harness.window, Qt::Key_Escape);
    QTRY_VERIFY(!menuOpened.evaluate().toBool());
    // The palette stays open behind its menu.
    QVERIFY(harness.dialog->property("visible").toBool());
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
    QCOMPARE(harness.view()->opacity(), 0.5);
    QVERIFY(!harness.view()->isEnabled());
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

// The palette and the dim fade in together over the short duration, and
// open at once when animations are off.
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
    harness.dialog->setProperty("fadeDuration", 400);
    QMetaObject::invokeMethod(harness.controller, "openDialog");
    QTRY_VERIFY(findDim() != nullptr);
    QQuickItem *dim = findDim();
    QTest::qWait(60);
    QVERIFY2(palette->opacity() > 0 && palette->opacity() < 1, qPrintable(QString::number(palette->opacity())));
    QVERIFY2(dim->opacity() > 0 && dim->opacity() < 1, qPrintable(QString::number(dim->opacity())));
    QTRY_COMPARE(palette->opacity(), 1.0);
    QTRY_COMPARE(dim->opacity(), 1.0);
    QTest::keyClick(harness.window, Qt::Key_Escape);
    QTRY_VERIFY(!harness.dialog->property("visible").toBool());

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
    QCOMPARE(helpIcon.evaluate().toString(), QStringLiteral("help-contextual"));
    QCOMPARE(clear->property("display").toInt(), 0); // AbstractButton.IconOnly
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
        if (contrastRatio(foreground, background) < minimum) failures << describe(what, foreground, background);
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
            for (const QString &name : {QStringLiteral("globalSearchResultContext"), QStringLiteral("globalSearchResultLength"),
                                        QStringLiteral("globalSearchMoreLink"), QStringLiteral("globalSearchSectionHeading")}) {
                if (QQuickItem *text = visibleChildNamed(row, name)) {
                    check(QStringLiteral("%1 of row %2").arg(name).arg(i), colorOf(text),
                          name == QStringLiteral("globalSearchSectionHeading") ? view : background, 4.5);
                }
            }
            for (const QString &name : {QStringLiteral("globalSearchArtistGlyph"), QStringLiteral("globalSearchCoverPlaceholder")}) {
                if (QQuickItem *glyph = visibleChildNamed(row, name)) {
                    check(QStringLiteral("%1 of row %2").arg(name).arg(i), colorOf(glyph), background, 3.0);
                }
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

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // Accessible interfaces exist while assistive technology is active.
    qputenv("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1");
    // The app's controls style, which it needs to start (docs/INSTALL.md).
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
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
    // The same platform whatever the desktop the tests run in: without a
    // desktop, Qt loads no desktop's platform theme.
    qunsetenv("XDG_CURRENT_DESKTOP");
    qunsetenv("QT_QPA_PLATFORMTHEME");
    qputenv("LANGUAGE", "en_US");
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    QApplication app(argc, argv);
    QAccessible::setActive(true);
    // Breeze, as a KDE session's platform theme sets it.
    QIcon::setThemeName(QStringLiteral("breeze"));
    FerrousLocalization::registerQmlTypes();
    SearchPaletteTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_search_palette.moc"
