// SPDX-License-Identifier: GPL-3.0-or-later

// The main window as one sheet (#373): the content on the view colour with
// four seams, the now-playing sidebar in its three layouts, the region
// headers of the library and the queue, the shared rows of the tree and the
// queue, the one-step library tree with its playing marks, the queue's
// columns, and the first run.

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QJSValue>
#include <QMutex>
#include <QMutexLocker>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlListReference>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUrl>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>

#include "../src/InputModality.h"
#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/SpectrogramItem.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"
#include "../src/WindowPresence.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

QMutex &messageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
}

QtMessageHandler &previousHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void captureMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker locker(&messageMutex());
        capturedMessages().push_back(message);
    }
    if (previousHandler()) {
        previousHandler()(type, context, message);
    }
}

QString takeMessages() {
    QMutexLocker locker(&messageMutex());
    // A window resize below the minimum size; the offscreen platform warns
    // that it does not pass size hints on.
    capturedMessages().removeAll(QStringLiteral("This plugin does not support propagateSizeHints()"));
    const QString text = capturedMessages().join(QLatin1Char('\n'));
    capturedMessages().clear();
    return text;
}

// Main.qml with its fallback bridge, `width` by `height`, shown and active.
QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel,
                             int width = 1600, int height = 1000) {
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    window->resize(width, height);
    window->requestActivate();
    if (!QTest::qWaitForWindowActive(window)
        || !QTest::qWaitFor([&]() { return window->width() == width && window->height() == height; })) {
        return nullptr;
    }
    return window;
}

QObject *bridgeOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("uiBridge"));
}

QObject *paletteOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("uiPalette"));
}

QColor tokenOf(QQuickWindow *window, const char *name) {
    return paletteOf(window)->property(name).value<QColor>();
}

QString hex(const QColor &color) {
    return color.name(QColor::HexArgb);
}

QString colorOf(const QObject *object) {
    return hex(object->property("color").value<QColor>());
}

QString tokenHex(QQuickWindow *window, const char *name) {
    return hex(tokenOf(window, name));
}

// The items named `name` under `root`, depth first.
QList<QQuickItem *> itemsNamed(QQuickItem *root, const QString &name) {
    QList<QQuickItem *> found;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (item->objectName() == name) {
            found.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    if (root != nullptr) {
        visit(root);
    }
    return found;
}

QQuickItem *itemNamed(QQuickItem *root, const QString &name) {
    const QList<QQuickItem *> found = itemsNamed(root, name);
    return found.isEmpty() ? nullptr : found.first();
}

// The first item named `name` in the window's item tree, which holds what
// repeaters and views make too.
QQuickItem *named(QQuickWindow *window, const char *name) {
    return itemNamed(window->contentItem(), QString::fromLatin1(name));
}

// Whether `item` and every item above it are visible.
bool shown(QQuickItem *item) {
    return item != nullptr && item->isVisible();
}

QRectF sceneRect(QQuickItem *item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

// The first row of the library tree whose type is `rowType`, as the view
// shows it (not one of its pooled delegates).
QQuickItem *libraryRow(QQuickWindow *window, const QString &rowType) {
    QQuickItem *view = named(window, "libraryView");
    const int count = view != nullptr ? view->property("count").toInt() : 0;
    for (int index = 0; index < count; ++index) {
        QQuickItem *row = nullptr;
        QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
        if (row != nullptr && row->property("rowTypeResolved").toString() == rowType) {
            return row;
        }
    }
    return nullptr;
}

// Row `index` of a list view, if it has one in place.
QQuickItem *viewRowAt(QQuickItem *view, int index) {
    QQuickItem *row = nullptr;
    QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
    return row;
}

// The rows of a list view that show the current-row ring.
QList<int> ringedRowsOf(QQuickItem *view) {
    QList<int> rows;
    for (int index = 0; index < view->property("count").toInt(); ++index) {
        QQuickItem *row = viewRowAt(view, index);
        QQuickItem *tint = row != nullptr ? itemNamed(row, QStringLiteral("rowBackgroundTint")) : nullptr;
        if (shown(tint) && QQmlProperty(tint, QStringLiteral("border.width")).read().toReal() > 0) {
            rows.push_back(index);
        }
    }
    return rows;
}

// Whether row `index` of a list view shows the selection tint.
bool rowTinted(QQuickItem *view, int index) {
    QQuickItem *row = viewRowAt(view, index);
    QQuickItem *tint = row != nullptr ? itemNamed(row, QStringLiteral("rowBackgroundTint")) : nullptr;
    return shown(tint) && tint->property("color").value<QColor>().alpha() > 0;
}

template <typename T>
void appendLe(QByteArray &out, T value) {
    const T little = qToLittleEndian(value);
    out.append(reinterpret_cast<const char *>(&little), static_cast<int>(sizeof(T)));
}

void appendUtf8(QByteArray &out, const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    appendLe<quint32>(out, static_cast<quint32>(utf8.size()));
    out.append(utf8);
}

const QString kTrackPath = QStringLiteral("/music/Artist A/Album A/02.flac");

const QString kRootKey = QStringLiteral("root|/music");
const QString kArtistKey = QStringLiteral("artist|Artist A");
const QString kAlbumKey = QStringLiteral("album|Artist A|Album A");

// An artist with an album from 2005 of one track, number 2, in the bridge's
// tree format. The artist row's path is that of the folder of whichever of
// its tracks the backend met first.
QByteArray artistAlbumTrackTree(const QString &artistPath = QStringLiteral("/music/Artist A"),
                                const QString &albumTitle = QStringLiteral("Album A"),
                                const QString &rootTitle = QString()) {
    struct Row {
        quint8 rowType;
        quint16 depth;
        quint32 trackNumber;
        quint32 childCount;
        qint32 year;
        QString title;
        QString key;
        QString path;
        QString trackPath;
    };
    QList<Row> rows{
        {1, 0, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, artistPath, {}},
        {2, 1, 0, 1, 2005, albumTitle, kAlbumKey,
         QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 2, 2, 0, 0, QStringLiteral("Second"), QStringLiteral("track|") + kTrackPath, kTrackPath,
         kTrackPath},
    };
    // Under a library folder's row, as with more than one folder.
    if (!rootTitle.isEmpty()) {
        for (Row &row : rows) {
            ++row.depth;
        }
        rows.prepend({0, 0, 0, 1, 0, rootTitle, kRootKey, QStringLiteral("/music"), {}});
    }
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const Row &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, -1);
        appendLe<quint32>(out, row.trackNumber);
        appendLe<quint32>(out, row.childCount);
        appendLe<qint32>(out, row.year);
        appendUtf8(out, row.title);
        appendUtf8(out, row.key);
        appendUtf8(out, QStringLiteral("Artist A"));
        appendUtf8(out, row.path);
        appendUtf8(out, QString());
        appendLe<quint64>(out, 0);
        appendUtf8(out, row.trackPath);
        appendLe<quint32>(out, row.trackPath.isEmpty() ? 0 : 1);
        if (!row.trackPath.isEmpty()) {
            appendUtf8(out, row.trackPath);
        }
    }
    return out;
}

void showTree(LibraryTreeModel &model, const QStringList &expanded,
              const QString &artistPath = QStringLiteral("/music/Artist A"),
              const QString &albumTitle = QStringLiteral("Album A")) {
    model.setExpandedKeys(expanded);
    model.setLibraryTreeFromBinary(artistAlbumTrackTree(artistPath, albumTitle));
}

const QStringList kAllExpanded{kArtistKey, kAlbumKey};

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

// `color`, with its alpha, drawn over the opaque `background`.
QColor drawnOver(const QColor &color, const QColor &background) {
    const double alpha = color.alphaF();
    return QColor::fromRgbF(static_cast<float>(background.redF() * (1 - alpha) + color.redF() * alpha),
                            static_cast<float>(background.greenF() * (1 - alpha) + color.greenF() * alpha),
                            static_cast<float>(background.blueF() * (1 - alpha) + color.blueF() * alpha));
}

// How a collapsed row that holds the playing track shows it (#373 H5): its
// name amber and semibold, quieter while paused, and no glyph anywhere on
// the row, which read as a play button. Empty when it is so, else what is
// wrong.
QString nameMarksNow(QQuickWindow *window, QQuickItem *row, bool paused) {
    QQuickItem *title = itemNamed(row, QStringLiteral("libraryRowTitle"));
    if (title == nullptr) {
        return QStringLiteral("no title");
    }
    QObject *palette = paletteOf(window);
    QColor expected = palette->property("now").value<QColor>();
    if (paused) {
        expected.setAlphaF(static_cast<float>(palette->property("nowPausedOpacity").toReal()));
    }
    const QColor color = title->property("color").value<QColor>();
    if (hex(color) != hex(expected)) {
        return QStringLiteral("the name is %1, not %2").arg(hex(color), hex(expected));
    }
    if (title->property("font").value<QFont>().weight() != QFont::DemiBold) {
        return QStringLiteral("the name is not semibold");
    }
    if (itemNamed(row, QStringLiteral("libraryRowNowMark")) != nullptr) {
        return QStringLiteral("the row still has a now mark");
    }
    QQuickItem *state = itemNamed(row, QStringLiteral("libraryRowState"));
    if (shown(state)) {
        return QStringLiteral("the row shows a state glyph");
    }
    // 4.5:1 on every background the row can have.
    for (const char *background : {"view", "hoveredRow", "selection", "selectionInactive"}) {
        const QColor under = palette->property(background).value<QColor>();
        const double ratio = contrastRatio(drawnOver(color, under), under);
        if (ratio < 4.5) {
            return QStringLiteral("%1:1 on %2").arg(ratio).arg(QString::fromLatin1(background));
        }
    }
    return {};
}

QVariantList sampleQueueRows(int count) {
    QVariantList rows;
    for (int i = 0; i < count; ++i) {
        rows.push_back(QVariantMap{
            {QStringLiteral("title"), QStringLiteral("Track %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("Artist %1").arg(i)},
            {QStringLiteral("album"), QStringLiteral("Album")},
            {QStringLiteral("lengthText"), QStringLiteral("1:00")},
            {QStringLiteral("path"), QStringLiteral("/music/%1.flac").arg(i)},
        });
    }
    return rows;
}

// A cover for the playing track, made once in a temporary directory of the
// test's own: the panel takes its wide layout only for a track with one
// (#373 H8).
QString testCoverUrl() {
    static QTemporaryDir dir;
    static const QString url = [] {
        const QString path = dir.filePath(QStringLiteral("cover.png"));
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(QColor(200, 120, 40));
        image.save(path);
        return QUrl::fromLocalFile(path).toString();
    }();
    return url;
}

void playTrack(QObject *bridge, const QString &state = QStringLiteral("Playing")) {
    bridge->setProperty("currentTrackPath", kTrackPath);
    bridge->setProperty("currentTrackCoverPath", testCoverUrl());
    bridge->setProperty("currentTrackTitle", QStringLiteral("Morning Tide Field"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("The Quiet Meridian"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Small Hours"));
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    bridge->setProperty("currentTrackYear", 2005);
    bridge->setProperty("currentTrackNumber", 2);
    bridge->setProperty("currentTrackTotal", 8);
    // The rows that hold it, as the backend names them.
    bridge->setProperty("currentTrackTreeKeys", QStringList{QStringLiteral("root|/music"), kArtistKey, kAlbumKey});
    bridge->setProperty("playbackState", state);
}

// The playing track's tags all gone, as for a file without them.
void clearTrackTags(QObject *bridge) {
    for (const char *property : {"currentTrackTitle", "currentTrackArtist", "currentTrackAlbum", "currentTrackGenre"}) {
        bridge->setProperty(property, QString());
    }
    bridge->setProperty("currentTrackYear", QVariant());
    bridge->setProperty("currentTrackNumber", 0);
    bridge->setProperty("currentTrackTotal", 0);
}

// Clears playback, as when the queue is cleared: nothing loaded.
void clearPlayback(QObject *bridge) {
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    for (const char *property : {"currentTrackPath", "currentTrackTitle", "currentTrackArtist", "currentTrackAlbum",
                                 "currentTrackCoverPath"}) {
        bridge->setProperty(property, QString());
    }
}

// Whether the now-playing panel shows the playing track's layout whole: the
// first track grows it from the compact row of nothing loaded (#373 H7),
// which this waits out.
bool grown(QQuickWindow *window) {
    QQuickItem *panel = named(window, "nowPlayingPanel");
    return panel != nullptr
        && QTest::qWaitFor([&]() { return panel->property("presence").toDouble() == 1.0; }, 5000);
}

// Why the now-playing panel is not the compact row of nothing loaded, or ""
// (#373 H7): one list row's height and the seam, the library's header right
// under it; a 16 px disc mark, larger with a larger font, at the 12 px
// inset, and "Nothing playing" where a library row's text starts, 6 px
// after it, both in the secondary text colour and centred in the row; no
// cover's square and none of a track's lines.
QString compactRowProblem(QQuickWindow *window) {
    QObject *palette = paletteOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *row = named(window, "nowPlayingIdleRow");
    QQuickItem *glyph = named(window, "nowPlayingIdleGlyph");
    QQuickItem *label = named(window, "nowPlayingIdleLabel");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    if (!panel || !row || !glyph || !label || !header || !seam) {
        return QStringLiteral("an item is missing");
    }
    const qreal rowHeight = palette->property("rowHeight").toReal();
    const QRectF block = sceneRect(panel);
    const QRectF rowRect = sceneRect(row);
    const QRectF glyphRect = sceneRect(glyph);
    const QRectF labelRect = sceneRect(label);
    const auto at = [](qreal got, qreal want) { return std::abs(got - want) <= 0.5; };
    if (!panel->property("idle").toBool()) {
        return QStringLiteral("not idle");
    }
    if (!at(panel->height(), rowHeight + 1)) {
        return QStringLiteral("panel %1 px high").arg(panel->height());
    }
    if (!at(sceneRect(header).top(), block.bottom()) || !at(sceneRect(seam).bottom(), block.bottom())) {
        return QStringLiteral("the library is not right under the seam");
    }
    if (!shown(row) || row->opacity() != 1.0 || !at(rowRect.top(), block.top()) || !at(rowRect.height(), rowHeight)) {
        return QStringLiteral("row at %1, %2 px high").arg(rowRect.top() - block.top()).arg(rowRect.height());
    }
    if (shown(named(window, "nowPlayingCoverArea")) || shown(named(window, "nowPlayingCover"))) {
        return QStringLiteral("the cover's square shows");
    }
    for (const char *name : {"nowPlayingWideText", "nowPlayingNarrowText"}) {
        if (shown(named(window, name))) {
            return QStringLiteral("%1 shows").arg(QString::fromLatin1(name));
        }
    }
    const qreal glyphSize = palette->property("toolIconSize").toReal();
    if (!shown(glyph) || glyph->width() != glyphSize || glyph->height() != glyphSize) {
        return QStringLiteral("glyph %1 px, not %2").arg(glyph->width()).arg(glyphSize);
    }
    if (!at(glyphRect.left(), block.left() + 12) || !at(glyphRect.center().y(), rowRect.center().y())) {
        return QStringLiteral("glyph at %1, %2").arg(glyphRect.left() - block.left()).arg(glyphRect.top() - block.top());
    }
    if (glyph->property("source").toString() != QStringLiteral("media-optical-audio-symbolic")
        || !glyph->property("isMask").toBool() || colorOf(glyph) != tokenHex(window, "textSecondary")) {
        return QStringLiteral("glyph %1 in %2").arg(glyph->property("source").toString(), colorOf(glyph));
    }
    if (!shown(label) || label->property("text").toString() != QStringLiteral("Nothing playing")
        || colorOf(label) != tokenHex(window, "textSecondary")
        || label->property("font").value<QFont>().weight() != QFont::Normal
        || label->property("truncated").toBool()) {
        return QStringLiteral("label \"%1\" in %2").arg(label->property("text").toString(), colorOf(label));
    }
    if (!at(labelRect.left(), glyphRect.right() + 6) || labelRect.right() > block.right() - 12 + 0.5
        || !at(labelRect.center().y(), rowRect.center().y())) {
        return QStringLiteral("label at %1, %2").arg(labelRect.left() - block.left()).arg(labelRect.top() - block.top());
    }
    return {};
}

// Breeze Dark's window, view and text for the application, until the guard
// it returns goes.
auto useDarkPalette(bool dark) {
    const QPalette before = QGuiApplication::palette();
    if (dark) {
        QPalette palette = before;
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#202326")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#141618")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#fcfcfc")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#fcfcfc")));
        QGuiApplication::setPalette(palette);
    }
    return qScopeGuard([before]() { QGuiApplication::setPalette(before); });
}

// Why the drawn now-playing tile is not one surface in the view colour, its
// mark aside, with no frame or slab of another colour on it, or "".
QString placeholderSurfaceProblem(QQuickWindow *window, QQuickItem *cover, QQuickItem *mark) {
    const QImage image = window->grabWindow();
    const qreal ratio = image.devicePixelRatio();
    const QColor view = tokenOf(window, "view");
    const QRectF tile = sceneRect(cover);
    // The mark, and a pixel around it for its edge's smoothing.
    const QRectF markRect = sceneRect(mark).adjusted(-1, -1, 1, 1);
    int checked = 0;
    for (int y = int(std::ceil(tile.top() * ratio)); y < int(std::floor(tile.bottom() * ratio)); ++y) {
        for (int x = int(std::ceil(tile.left() * ratio)); x < int(std::floor(tile.right() * ratio)); ++x) {
            if (markRect.contains(QPointF((x + 0.5) / ratio, (y + 0.5) / ratio))) {
                continue;
            }
            const QColor pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - view.red()) > 1 || std::abs(pixel.green() - view.green()) > 1
                || std::abs(pixel.blue() - view.blue()) > 1) {
                return QStringLiteral("%1 at %2,%3, not the view's %4").arg(pixel.name()).arg(x).arg(y).arg(view.name());
            }
            ++checked;
        }
    }
    return checked > 0 ? QString() : QStringLiteral("no pixel of the tile checked");
}

} // namespace

class UiMainWindowTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void contentIsOneSheetWithFourSeams();
    void sidebarWidthFollowsTheWindowAndTheFont_data();
    void sidebarWidthFollowsTheWindowAndTheFont();
    void nowPlayingHasItsWideNarrowAndEmptyLayouts();
    void nothingPlayingShowsWholeAtALargeFont_data();
    void nothingPlayingShowsWholeAtALargeFont();
    void nowPlayingTitleIsAStepOverItsLines();
    void nowPlayingTitleIsAStepOverItsLines_data();
    void nowPlayingNarrowShowsEveryLineAtAnyFontSize();
    void nowPlayingNarrowShowsEveryLineAtAnyFontSize_data();
    void nowPlayingWithoutCoverOrTagsLeavesNoBlankLines_data();
    void nowPlayingWithoutCoverOrTagsLeavesNoBlankLines();
    void nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort_data();
    void nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort();
    void nowPlayingKeepsTheArtistAndTheAlbumAtAnyFontSize();
    void nowPlayingKeepsTheArtistAndTheAlbumAtAnyFontSize_data();
    void nowPlayingMarksFollowTheFont();
    void nowPlayingMarksFollowTheFont_data();
    void nowPlayingShowsTheFileNameOfATrackWithoutTitleOrArtist();
    void nothingPlayingIsACompactRow_data();
    void nothingPlayingIsACompactRow();
    void nowPlayingCoverIsAKeyboardStop();
    void libraryFoldersAreFolderHeaders();
    void regionHeadersNameTheRegionAndItsTotals();
    void libraryTreeHasOneIndentStep();
    void libraryTreeChevronsGrowWithALargeFont();
    void libraryTreeMarksThePlayingTrackAndItsCollapsedAncestors();
    void collapsedRowNameElidesBeforeTheYear();
    void collapsedArtistMarkFollowsTheBackendsGroupingNotItsPath();
    void libraryRowsShowHoverSelectionAndTheCurrentRow();
    void queueRowTintsRunToThePaneEdgeUnderTheScrollBar();
    void listsRingARowAsTheKeyboardComesIn();
    void listsRingTheCurrentRowOnlyForTheKeyboard();
    void unreadableTracksAreMarkedFromTheIssueHistory();
    void queueHeaderLabelsOnlyArtistAlbumAndLength();
    void narrowQueueDropsAlbumBeforeTitle();
    void queueColumnsGiveTheirSlackToTheTitle();
    void narrowWindowAtALargeFontFitsTheQueue();
    void queueKeepsArtistAtALargeFont_data();
    void queueKeepsArtistAtALargeFont();
    void visualizationTakesNearlyHalfTheRightColumn_data();
    void visualizationTakesNearlyHalfTheRightColumn();
    void firstRunShowsOneMessage();
    void tabGoesFromTheLibraryToTheQueueRowsBeforeItsHandles();
    void transportStartsAndEndsAtTheContentInset();
    void viewersInTheWindowFadeAsTheStylesPopupsDo();
};

void UiMainWindowTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(captureMessage);
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterType<WindowPresence>("FerrousUi", 1, 0, "WindowPresence");
    InputModality::registerQmlType();
}

void UiMainWindowTest::init() {
    takeMessages();
}

void UiMainWindowTest::cleanup() {
    const QString messages = takeMessages();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void UiMainWindowTest::cleanupTestCase() {
    qInstallMessageHandler(previousHandler());
    previousHandler() = nullptr;
}

void UiMainWindowTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void UiMainWindowTest::contentIsOneSheetWithFourSeams() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *transport = named(window, "transportBar");
    QQuickItem *transportSeam = named(window, "transportSeam");
    QQuickItem *mainSplit = named(window, "mainSplit");
    QQuickItem *sidebar = named(window, "sidebarPane");
    QQuickItem *rightSplit = named(window, "rightSplit");
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *pane = named(window, "spectrogramPane");
    QQuickItem *statusSeam = named(window, "statusBarSeam");
    QVERIFY(transport && transportSeam && mainSplit && sidebar && rightSplit && queue && pane && statusSeam);

    // Content on the view colour; the visualization on its mode's floor,
    // the spectrogram's black also before any audio (tst_ui_motion's
    // visualizationPaneFloorFollowsTheMode).
    QCOMPARE(colorOf(sidebar), tokenHex(window, "view"));
    QCOMPARE(colorOf(queue), tokenHex(window, "view"));
    QCOMPARE(colorOf(pane), QStringLiteral("#ff000000"));

    // Transport | content: a chrome line right under the transport, and
    // the content right under it.
    QCOMPARE(transportSeam->height(), 1.0);
    QCOMPARE(colorOf(transportSeam), tokenHex(window, "chromeLine"));
    QCOMPARE(sceneRect(transportSeam).top(), sceneRect(transport).bottom());
    QCOMPARE(sceneRect(mainSplit).top(), sceneRect(transportSeam).bottom());
    // Content | status bar: a chrome line on the status bar's top edge.
    QCOMPARE(statusSeam->height(), 1.0);
    QCOMPARE(colorOf(statusSeam), tokenHex(window, "chromeLine"));
    QVERIFY(std::abs(sceneRect(statusSeam).top() - sceneRect(mainSplit).bottom()) <= 0.5);

    // The two split handles are 1 px seams, with the panes on either side
    // and no gap.
    const QList<QQuickItem *> handles = itemsNamed(window->contentItem(), QStringLiteral("seamHandle"));
    QCOMPARE(handles.size(), 2);
    QQuickItem *columnSeam = handles.at(0)->width() == 1.0 ? handles.at(0) : handles.at(1);
    QQuickItem *queueSeam = columnSeam == handles.at(0) ? handles.at(1) : handles.at(0);
    QCOMPARE(columnSeam->width(), 1.0);
    QCOMPARE(queueSeam->height(), 1.0);
    for (QQuickItem *seam : {columnSeam, queueSeam}) {
        QCOMPARE(colorOf(seam), tokenHex(window, "seam"));
    }
    QCOMPARE(sceneRect(columnSeam).left(), sceneRect(sidebar).right());
    QCOMPARE(sceneRect(rightSplit).left(), sceneRect(columnSeam).right());
    QCOMPARE(sceneRect(queueSeam).top(), sceneRect(queue).bottom());
    QCOMPARE(sceneRect(pane).top(), sceneRect(queueSeam).bottom());

    // A seam can be grabbed a few pixels either side of its line.
    const QRectF columnRect = sceneRect(columnSeam);
    QVERIFY(columnSeam->contains(columnSeam->mapFromScene(QPointF(columnRect.left() - 2, 300))));
    QVERIFY(columnSeam->contains(columnSeam->mapFromScene(QPointF(columnRect.left() + 3, 300))));
}

void UiMainWindowTest::sidebarWidthFollowsTheWindowAndTheFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::addColumn<qreal>("expectedWidth");
    QTest::addColumn<qreal>("expectedMinimum");
    QTest::addColumn<bool>("narrow");
    QTest::newRow("10 pt, 1600 by 1000") << 10.0 << 1600 << 1000 << 360.0 << 250.0 << false;
    QTest::newRow("10 pt, 1000 by 720") << 10.0 << 1000 << 720 << 260.0 << 250.0 << true;
    // The panel narrow at 24 pt in 1000 px of height: the wide layout would
    // leave the library about three rows (#373 H6).
    QTest::newRow("24 pt, 1600 by 1000") << 24.0 << 1600 << 1000 << 408.0 << 360.0 << true;
    QTest::newRow("24 pt, 1600 by 1400") << 24.0 << 1600 << 1400 << 408.0 << 360.0 << false;
    QTest::newRow("24 pt, 1000 by 720") << 24.0 << 1000 << 720 << 360.0 << 360.0 << true;
}

// The sidebar is 360 px wide in a 1600 px window and 260 px in a 1000 px one
// with the default font. With a large font it is wider, so the library's
// names and the now-playing lines are more than fragments (#373 H5): at
// 24 pt, 360 px in the narrow window, where the now-playing panel stays
// narrow and the queue keeps its title and length, and 408 px in the wide
// one, where the panel is wide where the window is tall enough for it. Its least width follows the font too, and
// the window still fits 1000 px. A width the user dragged it to is kept.
void UiMainWindowTest::sidebarWidthFollowsTheWindowAndTheFont() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    QFETCH(qreal, expectedWidth);
    QFETCH(qreal, expectedMinimum);
    QFETCH(bool, narrow);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont font = applicationFont;
    font.setPointSizeF(pointSize);
    QGuiApplication::setFont(font);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, windowWidth, windowHeight);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    QQuickItem *sidebar = named(window, "sidebarPane");
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *queue = named(window, "mainQueuePane");
    QVERIFY(sidebar && panel && queue);
    QTRY_COMPARE(sidebar->width(), expectedWidth);
    QCOMPARE(QQmlProperty(sidebar, QStringLiteral("SplitView.minimumWidth"), qmlContext(sidebar)).read().toReal(),
             expectedMinimum);
    QTRY_COMPARE(panel->property("layoutMode").toString(), narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QVERIFY2(window->minimumWidth() <= 1000, qPrintable(QString::number(window->minimumWidth())));
    // The queue keeps its title column, at least its minimum, and Length.
    QVERIFY(queue->width() >= queue->property("minimumContentWidth").toReal() - 0.5);
    QTRY_VERIFY(sceneRect(queue).right() <= window->width() + 0.5);

    // A width the user dragged it to stays, as a share of the split, where
    // the window has room for it.
    if (windowWidth < 1600) {
        return;
    }
    QObject *layout = window->findChild<QObject *>(QStringLiteral("windowLayoutController"));
    QVERIFY(layout != nullptr);
    const qreal splitWidth = named(window, "mainSplit")->width();
    const qreal dragged = expectedWidth + 40;
    QVERIFY(QMetaObject::invokeMethod(layout, "noteSidebarWidth", Q_ARG(QVariant, dragged),
                                      Q_ARG(QVariant, splitWidth)));
    QTRY_COMPARE(sidebar->width(), dragged);
}

void UiMainWindowTest::nothingPlayingShowsWholeAtALargeFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::newRow("default font") << 0.0 << 1600 << 1000;
    QTest::newRow("default font, narrow") << 0.0 << 1000 << 720;
    QTest::newRow("24 pt") << 24.0 << 1600 << 1000;
    QTest::newRow("24 pt, narrow") << 24.0 << 1000 << 720;
}

// "Nothing playing" shows whole in the compact row, at any font and in
// either window, and the library's header is right under the panel (#373
// H7). At 24 pt the layout the panel would take for a track is narrow in
// both windows, the wide one too short for its wide layout (#373 H6); the
// row is the same in both.
void UiMainWindowTest::nothingPlayingShowsWholeAtALargeFont() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont large = applicationFont;
        large.setPointSizeF(pointSize);
        QGuiApplication::setFont(large);
    }
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, windowWidth, windowHeight);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *label = named(window, "nowPlayingIdleLabel");
    QVERIFY(panel && label);
    QVERIFY(panel->property("idle").toBool());
    const bool narrow = windowWidth == 1000 || pointSize > 0;
    QTRY_COMPARE(panel->property("layoutMode").toString(), narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QTRY_VERIFY(label->width() > 0);
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    QVERIFY(label->implicitHeight() <= label->height() + 0.5);
    QVERIFY(sceneRect(label).bottom() <= sceneRect(panel).bottom());
    const qreal scale = std::max(1.0, paletteOf(window)->property("basePointSize").toReal() / 10.0);
    QCOMPARE(named(window, "nowPlayingIdleGlyph")->width(), std::max<qreal>(16, std::round(16 * scale)));
}

void UiMainWindowTest::nowPlayingHasItsWideNarrowAndEmptyLayouts() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *palette = paletteOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QVERIFY(panel && cover && header);

    // Nothing loaded: the compact row (#373 H7; nothingPlayingIsACompactRow
    // has the rest of it).
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QVERIFY(panel->property("idle").toBool());
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    QVERIFY(!shown(cover));
    // A seam where the library starts.
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QVERIFY(shown(seam));
    QCOMPARE(seam->height(), 1.0);
    QCOMPARE(seam->width(), panel->width());
    QCOMPARE(colorOf(seam), tokenHex(window, "seam"));
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());

    // Playing, wide: the cover edge to edge and square, then the text 12 px
    // in and 12 px under it, with no card around it.
    playTrack(bridge);
    QVERIFY(grown(window));
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QTRY_COMPARE(cover->width(), panel->width());
    QCOMPARE(cover->height(), cover->width());
    QCOMPARE(sceneRect(cover).left(), sceneRect(panel).left());
    QQuickItem *title = named(window, "nowPlayingTitle");
    QQuickItem *artistAlbum = named(window, "nowPlayingArtistAlbum");
    QQuickItem *details = named(window, "nowPlayingDetails");
    QVERIFY(title && artistAlbum && details);
    QCOMPARE(sceneRect(title).left() - sceneRect(panel).left(), 12.0);
    QCOMPARE(sceneRect(title).top() - sceneRect(cover).bottom(), 12.0);
    QCOMPARE(title->property("text").toString(), QStringLiteral("Morning Tide Field"));
    QCOMPARE(artistAlbum->property("text").toString(), QStringLiteral("The Quiet Meridian — Small Hours"));
    QCOMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2 of 8"));
    // Without a total the backend is sure of, the number alone.
    bridge->setProperty("currentTrackTotal", 0);
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2"));
    bridge->setProperty("currentTrackTotal", 8);
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2 of 8"));
    // The title a step over the lines, 12 pt DemiBold with a 10 pt font;
    // the artist and the album in the text colour, the details secondary.
    const QFont titleFont = title->property("font").value<QFont>();
    QCOMPARE(titleFont.weight(), QFont::DemiBold);
    QCOMPARE(titleFont.pointSizeF(), palette->property("basePointSize").toReal() * 1.2);
    QCOMPARE(colorOf(title), tokenHex(window, "text"));
    QCOMPARE(colorOf(artistAlbum), tokenHex(window, "text"));
    QCOMPARE(colorOf(details), tokenHex(window, "textTertiary"));
    QCOMPARE(details->property("font").value<QFont>().pointSizeF(),
             palette->property("smallPointSize").toReal());
    QVERIFY(!panel->property("idle").toBool());
    // Then 14 px, about 16 px under the details' text with their descent,
    // and the seam into the library: two regions of one sheet, with no dead
    // band between them (#373 H4).
    QTRY_COMPARE(sceneRect(seam).top() - sceneRect(details).bottom(), 14.0);
    const QFontMetricsF detailsMetrics(details->property("font").value<QFont>());
    const qreal underText = sceneRect(seam).top() - (sceneRect(details).top() + detailsMetrics.ascent());
    QVERIFY2(underText >= 14.0 && underText <= 20.0, qPrintable(QString::number(underText)));
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(panel).bottom());

    // Missing values are left out, not shown as dashes.
    bridge->setProperty("currentTrackGenre", QString());
    bridge->setProperty("currentTrackYear", QVariant());
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("Track 2 of 8"));

    // A title that does not fit is elided, with the whole of it in the
    // tooltip; nothing scrolls.
    const QString longTitle = QStringLiteral("A Remarkably Long Track Title That Will Certainly Need Eliding");
    bridge->setProperty("currentTrackTitle", longTitle);
    QTRY_VERIFY(title->property("truncated").toBool());
    QVERIFY(panel->property("textTruncated").toBool());
    QCOMPARE(title->property("x").toReal(), 0.0);

    // Narrow: a 96 px cover with the lines beside it, then 12 px and the
    // seam before the library.
    window->resize(1000, 720);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QTRY_COMPARE(cover->width(), 96.0);
    QCOMPARE(cover->height(), 96.0);
    QVERIFY(!panel->property("narrowStacked").toBool());
    QQuickItem *narrowTitle = named(window, "nowPlayingNarrowTitle");
    QVERIFY(shown(narrowTitle));
    QCOMPARE(sceneRect(narrowTitle).left() - sceneRect(cover).right(), 12.0);
    QCOMPARE(narrowTitle->property("font").value<QFont>().pointSizeF(),
             palette->property("basePointSize").toReal() * 1.2);
    QCOMPARE(colorOf(named(window, "nowPlayingNarrowArtist")), tokenHex(window, "text"));
    QCOMPARE(colorOf(named(window, "nowPlayingNarrowAlbum")), tokenHex(window, "text"));
    QCOMPARE(colorOf(named(window, "nowPlayingNarrowDetails")), tokenHex(window, "textTertiary"));
    QTRY_COMPARE(sceneRect(seam).top() - sceneRect(cover).bottom(), 12.0);
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());

    // The narrow lines have the year with the album and the total with the
    // number, or the number alone without a total.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    bridge->setProperty("currentTrackYear", 2005);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(),
                 QStringLiteral("Classical · Track 2 of 8"));
    QCOMPARE(panel->property("narrowAlbumText").toString(), QStringLiteral("Small Hours (2005)"));
    bridge->setProperty("currentTrackTotal", 0);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · Track 2"));
    // Nor is a total below the number shown.
    bridge->setProperty("currentTrackTotal", 1);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · Track 2"));
}

void UiMainWindowTest::nowPlayingTitleIsAStepOverItsLines_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// The title is a clear step over the lines under it at any font size
// (#373 H2): 1.2 times the font, DemiBold, as a level 2 Kirigami.Heading
// is; "Artist — Album" in the text colour at the font's size; the details
// smaller, in the tertiary colour. The block then has its 28 px and the
// seam before the library.
void UiMainWindowTest::nowPlayingTitleIsAStepOverItsLines() {
    QFETCH(qreal, pointSize);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    // Tall enough for the wide layout at 24 pt.
    QQuickWindow *window = loadMainWindow(engine, libraryModel, 1600, 1400);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QFont font = window->property("font").value<QFont>();
    font.setPointSizeF(pointSize);
    QVERIFY(window->setProperty("font", font));
    QObject *palette = paletteOf(window);
    QTRY_COMPARE(palette->property("basePointSize").toReal(), pointSize);
    playTrack(bridgeOf(window));
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QQuickItem *title = named(window, "nowPlayingTitle");
    QQuickItem *artistAlbum = named(window, "nowPlayingArtistAlbum");
    QQuickItem *details = named(window, "nowPlayingDetails");
    QVERIFY(title && artistAlbum && details);

    const QFont titleFont = title->property("font").value<QFont>();
    const QFont lineFont = artistAlbum->property("font").value<QFont>();
    QCOMPARE(titleFont.pointSizeF(), pointSize * 1.2);
    QCOMPARE(titleFont.weight(), QFont::DemiBold);
    QCOMPARE(lineFont.pointSizeF(), pointSize);
    QCOMPARE(lineFont.weight(), QFont::Normal);
    QCOMPARE(details->property("font").value<QFont>().pointSizeF(), pointSize * 0.9);
    QCOMPARE(colorOf(details), tokenHex(window, "textTertiary"));
    // The step shows: its capitals are a fifth taller, at least.
    QVERIFY(QFontMetricsF(titleFont).capHeight() >= 1.15 * QFontMetricsF(lineFont).capHeight());
    // Each line takes the height of its font: none overlaps the next.
    QTRY_VERIFY(title->height() >= QFontMetricsF(titleFont).height() - 1);
    // "Artist — Album", or, where that would be cut off, a line each.
    QQuickItem *firstLine = shown(artistAlbum) ? artistAlbum : named(window, "nowPlayingWideArtist");
    QQuickItem *lastLine = shown(artistAlbum) ? artistAlbum : named(window, "nowPlayingWideAlbum");
    QVERIFY(shown(firstLine) && shown(lastLine));
    QCOMPARE(colorOf(firstLine), tokenHex(window, "text"));
    QCOMPARE(colorOf(lastLine), tokenHex(window, "text"));
    QTRY_VERIFY(sceneRect(firstLine).top() >= sceneRect(title).bottom());
    QTRY_VERIFY(sceneRect(details).top() >= sceneRect(lastLine).bottom());

    QQuickItem *seam = named(window, "nowPlayingSeam");
    QQuickItem *header = named(window, "libraryHeader");
    QTRY_COMPARE(sceneRect(seam).top() - sceneRect(details).bottom(), 14.0);
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());
}

void UiMainWindowTest::nowPlayingNarrowShowsEveryLineAtAnyFontSize_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<bool>("stacked");
    QTest::newRow("10 pt") << 10.0 << false;
    QTest::newRow("14 pt") << 14.0 << false;
    QTest::newRow("24 pt") << 24.0 << true;
}

// In a narrow window, at any font size, the now-playing lines all show,
// each with at least 8 characters of its text (#373 H2, 24 pt in 1000 by
// 720): beside the 96 px cover, the block as tall as the cover or as its
// lines, whichever is taller; under the cover, on the whole width, once the
// room beside it is under about 12 characters. Then 12 px and the seam
// before the library.
void UiMainWindowTest::nowPlayingNarrowShowsEveryLineAtAnyFontSize() {
    QFETCH(qreal, pointSize);
    QFETCH(bool, stacked);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QFont font = window->property("font").value<QFont>();
    font.setPointSizeF(pointSize);
    QVERIFY(window->setProperty("font", font));
    QObject *palette = paletteOf(window);
    QTRY_COMPARE(palette->property("basePointSize").toReal(), pointSize);
    window->resize(1000, 720);
    QTRY_COMPARE(window->width(), 1000);
    QObject *bridge = bridgeOf(window);
    playTrack(bridge);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Lantern Signal"));
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QCOMPARE(panel->property("narrowStacked").toBool(), stacked);
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QQuickItem *header = named(window, "libraryHeader");
    QTRY_COMPARE(cover->width(), 96.0);
    QVERIFY(grown(window));
    // The lines laid out for the font, one under the other.
    QTRY_VERIFY(sceneRect(named(window, "nowPlayingNarrowArtist")).top()
                    >= sceneRect(named(window, "nowPlayingNarrowTitle")).bottom()
                && sceneRect(named(window, "nowPlayingNarrowAlbum")).top()
                    >= sceneRect(named(window, "nowPlayingNarrowArtist")).bottom()
                && sceneRect(named(window, "nowPlayingNarrowDetails")).top()
                    >= sceneRect(named(window, "nowPlayingNarrowAlbum")).bottom());
    const QRectF tile = sceneRect(cover);
    const QRectF block = sceneRect(panel);

    qreal bottom = tile.bottom();
    for (const char *name : {"nowPlayingNarrowTitle", "nowPlayingNarrowArtist", "nowPlayingNarrowAlbum",
                             "nowPlayingNarrowDetails"}) {
        QQuickItem *line = named(window, name);
        QVERIFY2(shown(line), name);
        const QRectF rect = sceneRect(line);
        const QString text = line->property("text").toString();
        QVERIFY2(!text.isEmpty(), name);
        // In the panel, clear of the cover, with its font's height.
        QVERIFY2(rect.left() >= block.left() + 12 && rect.right() <= block.right() - 12, name);
        QVERIFY2(!rect.intersects(tile), name);
        QVERIFY2(rect.height() >= QFontMetricsF(line->property("font").value<QFont>()).height() - 1, name);
        if (stacked) {
            QVERIFY2(rect.top() >= tile.bottom() + 12, name);
            QCOMPARE(rect.left(), block.left() + 12);
        } else {
            QCOMPARE(rect.left(), tile.right() + 12);
        }
        // Whole, or at least 8 characters of it and the ellipsis.
        const QFontMetricsF metrics(line->property("font").value<QFont>());
        QVERIFY2(metrics.horizontalAdvance(text) <= rect.width()
                     || metrics.horizontalAdvance(text.left(8) + QStringLiteral("…")) <= rect.width(),
                 qPrintable(QStringLiteral("%1: %2 in %3 px").arg(QString::fromLatin1(name), text).arg(rect.width())));
        bottom = std::max(bottom, rect.bottom());
    }
    // The block follows its lines: the seam 12 px under the lowest of them
    // or the cover, whichever is lower.
    QTRY_COMPARE(sceneRect(seam).top() - bottom, 12.0);
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());
    if (!stacked) {
        return;
    }

    // Under the cover, the lines are 4 px apart, and a title or an artist
    // too long for one line wraps onto a second rather than be cut off
    // (#373 H5); the others stay on one line. The block grows with them.
    QQuickItem *title = named(window, "nowPlayingNarrowTitle");
    QQuickItem *artist = named(window, "nowPlayingNarrowArtist");
    QQuickItem *album = named(window, "nowPlayingNarrowAlbum");
    QQuickItem *details = named(window, "nowPlayingNarrowDetails");
    const auto lineText = [](QQuickItem *line) {
        return itemNamed(line, line->objectName());
    };
    const auto gapsAreFour = [&]() {
        const auto four = [](qreal gap) { return std::abs(gap - 4.0) < 0.01; };
        return four(sceneRect(artist).top() - sceneRect(title).bottom())
            && four(sceneRect(album).top() - sceneRect(artist).bottom())
            && four(sceneRect(details).top() - sceneRect(album).bottom());
    };
    QTRY_VERIFY(gapsAreFour());
    for (QQuickItem *line : {title, artist, album, details}) {
        QCOMPARE(lineText(line) != nullptr ? lineText(line)->property("lineCount").toInt() : 0, 1);
    }
    bridge->setProperty("currentTrackTitle", QStringLiteral("Lantern Signal Over Harbour"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("The Quiet Meridian Chamber Ensemble"));
    for (QQuickItem *line : {title, artist}) {
        QQuickItem *text = lineText(line);
        QVERIFY(text != nullptr);
        QTRY_COMPARE(text->property("lineCount").toInt(), 2);
        QTRY_VERIFY2(!line->property("truncated").toBool(), qPrintable(line->objectName()));
        const QFontMetricsF metrics(text->property("font").value<QFont>());
        QTRY_VERIFY2(line->height() >= 2 * metrics.height() - 1, qPrintable(line->objectName()));
    }
    QTRY_VERIFY(gapsAreFour());
    QTRY_COMPARE(sceneRect(seam).top() - sceneRect(details).bottom(), 12.0);
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());
    // A title too long even for two lines is cut off at the second's end.
    bridge->setProperty("currentTrackTitle", QStringLiteral("A Remarkably Long Track Title ").repeated(4));
    QTRY_VERIFY(title->property("truncated").toBool());
    QCOMPARE(lineText(title)->property("lineCount").toInt(), 2);
}

void UiMainWindowTest::nowPlayingWithoutCoverOrTagsLeavesNoBlankLines_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

// A track without a cover takes the narrow layout in the wide window too
// (#373 H8: the empty 360 px square was a dead slab), its 96 px tile a
// placeholder (#373 H6): the view colour, with no lighter slab and no
// frame, and a disc mark at its centre, a neutral grey from the tertiary
// colour at 3:1 on the view, 32 px, a third of the tile (#373 H7: at 64 px
// and 2:1 it was faint). A track without tags shows its file name for a title,
// and its lines with no text take no room: no blank lines are left in the
// block (#373 H6, a file without tags left two).
void UiMainWindowTest::nowPlayingWithoutCoverOrTagsLeavesNoBlankLines() {
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QCOMPARE(paletteOf(window)->property("themeIsDark").toBool(), dark);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QVERIFY(panel && cover && header && seam);
    QQuickItem *mark = itemNamed(cover, QStringLiteral("albumArtPlaceholderGlyph"));
    QVERIFY(mark != nullptr);

    for (const bool narrow : {false, true}) {
        if (narrow) {
            window->resize(1000, 720);
            QTRY_COMPARE(window->width(), 1000);
        }
        playTrack(bridge);
        bridge->setProperty("currentTrackCoverPath", QString());
        QVERIFY(grown(window));
        QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
        QTRY_COMPARE(cover->width(), 96.0);
        QTRY_COMPARE(panel->property("coverGrowth").toReal(), 0.0);
        const QRectF tagged = sceneRect(header);
        const QRectF tileRect = sceneRect(cover);

        // No cover: the view colour, as the sidebar, and the quiet mark.
        QVERIFY(bridge->property("currentTrackCoverPath").toString().isEmpty());
        QCOMPARE(colorOf(cover), tokenHex(window, "view"));
        QVERIFY(shown(mark));
        QCOMPARE(mark->width(), 32.0);
        QCOMPARE(mark->height(), mark->width());
        QCOMPARE(sceneRect(mark).center(), tileRect.center());
        QCOMPARE(mark->property("source").toString(), QStringLiteral("media-optical-audio-symbolic"));
        QVERIFY(mark->property("valid").toBool());
        QVERIFY(mark->property("isMask").toBool());
        // At least 3:1 on the view, and not much over it: a quiet mark.
        const QColor markColor = mark->property("color").value<QColor>();
        QCOMPARE(hex(markColor), hex(panel->property("placeholderMarkColor").value<QColor>()));
        const double markContrast = contrastRatio(markColor, tokenOf(window, "view"));
        QVERIFY2(markContrast >= 3.0 && markContrast <= 3.6, qPrintable(QString::number(markContrast)));
        QCOMPARE(markColor.alphaF(), 1.0f);
        QTRY_VERIFY2(placeholderSurfaceProblem(window, cover, mark).isEmpty(),
                     qPrintable(placeholderSurfaceProblem(window, cover, mark)));

        // No tags: the file name for a title, and no line for the rest.
        clearTrackTags(bridge);
        QTRY_COMPARE(panel->property("artistAlbumText").toString(), QString());
        QCOMPARE(panel->property("detailsText").toString(), QString());
        QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
        QCOMPARE(sceneRect(cover), tileRect);
        QQuickItem *title = named(window, "nowPlayingNarrowTitle");
        QVERIFY(shown(title));
        QTRY_COMPARE(title->property("text").toString(), QFileInfo(kTrackPath).fileName());
        for (const char *name : {"nowPlayingNarrowArtist", "nowPlayingNarrowAlbum", "nowPlayingNarrowDetails"}) {
            QQuickItem *line = named(window, name);
            QVERIFY2(line != nullptr, name);
            QTRY_VERIFY2(!line->isVisible(), name);
        }
        // Beside the cover, which is taller than the title: the block keeps
        // the cover's height.
        QTRY_COMPARE(sceneRect(seam).top() - tileRect.bottom(), 12.0);
        QCOMPARE(sceneRect(header), tagged);
        QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());

        // The tags back: the lines back, where they were.
        playTrack(bridge);
        bridge->setProperty("currentTrackCoverPath", QString());
        QTRY_COMPARE(sceneRect(header), tagged);
    }
}

void UiMainWindowTest::nowPlayingKeepsTheArtistAndTheAlbumAtAnyFontSize_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<bool>("narrow");
    for (const bool narrow : {false, true}) {
        for (const qreal pointSize : {10.0, 14.0, 24.0}) {
            QTest::addRow("%s, %g pt", narrow ? "narrow" : "wide", pointSize) << pointSize << narrow;
        }
    }
}

// The artist and the album both show at any font size (#373 H3: at 24 pt
// "Artist — Album" was cut off before the album, with room under it): wide,
// on one line where it fits, and otherwise on a line each, 4 px apart, 12 px
// in from both sides, each whole where it fits on the width; narrow, on
// lines of their own as always. Nothing else moves: the details under them,
// then 14 px to the seam.
void UiMainWindowTest::nowPlayingKeepsTheArtistAndTheAlbumAtAnyFontSize() {
    QFETCH(qreal, pointSize);
    QFETCH(bool, narrow);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    // Tall enough for the wide layout at 24 pt.
    QQuickWindow *window = loadMainWindow(engine, libraryModel, 1600, 1400);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QFont font = window->property("font").value<QFont>();
    font.setPointSizeF(pointSize);
    QVERIFY(window->setProperty("font", font));
    QObject *palette = paletteOf(window);
    QTRY_COMPARE(palette->property("basePointSize").toReal(), pointSize);
    if (narrow) {
        window->resize(1000, 720);
        QTRY_COMPARE(window->width(), 1000);
    }
    QObject *bridge = bridgeOf(window);
    playTrack(bridge);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QTRY_COMPARE(panel->property("layoutMode").toString(),
                 narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    const QRectF block = sceneRect(panel);
    const QString artist = QStringLiteral("The Quiet Meridian");
    const QString album = QStringLiteral("Small Hours");

    // Why `line` does not show all of `text` within the panel's 12 px
    // inset, or "".
    const auto notWhole = [&](QQuickItem *line, const QString &text) -> QString {
        const QRectF rect = sceneRect(line);
        const QFontMetricsF metrics(line->property("font").value<QFont>());
        if (shown(line) && line->property("text").toString() == text && !line->property("truncated").toBool()
            && metrics.horizontalAdvance(text) <= rect.width() && rect.left() >= block.left() + 12
            && rect.right() <= block.right() - 12) {
            return {};
        }
        return QStringLiteral("%1: \"%2\" (%3 px, truncated %4) in %5 to %6 of %7 to %8")
            .arg(line->objectName(), line->property("text").toString())
            .arg(metrics.horizontalAdvance(text)).arg(line->property("truncated").toBool())
            .arg(rect.left()).arg(rect.right()).arg(block.left()).arg(block.right());
    };

    if (narrow) {
        QQuickItem *artistLine = named(window, "nowPlayingNarrowArtist");
        QQuickItem *albumLine = named(window, "nowPlayingNarrowAlbum");
        QVERIFY(artistLine && albumLine);
        QVERIFY(shown(artistLine) && shown(albumLine));
        QCOMPARE(artistLine->property("text").toString(), artist);
        QVERIFY(albumLine->property("text").toString().startsWith(album));
        QTRY_VERIFY(sceneRect(albumLine).top() >= sceneRect(artistLine).bottom());
        return;
    }

    QQuickItem *title = named(window, "nowPlayingTitle");
    QQuickItem *combined = named(window, "nowPlayingArtistAlbum");
    QQuickItem *artistLine = named(window, "nowPlayingWideArtist");
    QQuickItem *albumLine = named(window, "nowPlayingWideAlbum");
    QQuickItem *details = named(window, "nowPlayingDetails");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QVERIFY(title && combined && artistLine && albumLine && details && seam);
    const QString both = artist + QStringLiteral(" — ") + album;
    const QFontMetricsF metrics(combined->property("font").value<QFont>());
    const qreal width = block.width() - 24;
    const bool fits = metrics.horizontalAdvance(both) <= width;
    // The default font fits it on one line; 24 pt does not (Sol, H3).
    if (pointSize == 10.0) {
        QVERIFY(fits);
    }
    if (pointSize == 24.0) {
        QVERIFY(!fits);
    }
    QCOMPARE(panel->property("wideArtistAlbumSplit").toBool(), !fits);
    QQuickItem *lastLine = nullptr;
    if (fits) {
        QTRY_VERIFY2(notWhole(combined, both).isEmpty(), qPrintable(notWhole(combined, both)));
        QVERIFY(!artistLine->isVisible() && !albumLine->isVisible());
        lastLine = combined;
    } else {
        QVERIFY(!combined->isVisible());
        QTRY_VERIFY(shown(artistLine) && shown(albumLine));
        QCOMPARE(artistLine->property("text").toString(), artist);
        QCOMPARE(albumLine->property("text").toString(), album);
        // Each whole, as both fit the width on their own.
        QTRY_VERIFY2(notWhole(artistLine, artist).isEmpty(), qPrintable(notWhole(artistLine, artist)));
        QVERIFY2(notWhole(albumLine, album).isEmpty(), qPrintable(notWhole(albumLine, album)));
        QCOMPARE(sceneRect(artistLine).left(), block.left() + 12);
        QCOMPARE(sceneRect(albumLine).left(), block.left() + 12);
        QVERIFY(sceneRect(artistLine).top() >= sceneRect(title).bottom());
        QTRY_COMPARE(sceneRect(albumLine).top() - sceneRect(artistLine).bottom(), 4.0);
        QCOMPARE(colorOf(artistLine), tokenHex(window, "text"));
        QCOMPARE(colorOf(albumLine), tokenHex(window, "text"));
        lastLine = albumLine;
    }
    QTRY_VERIFY(sceneRect(details).top() >= sceneRect(lastLine).bottom());
    QTRY_COMPARE(sceneRect(seam).top() - sceneRect(details).bottom(), 14.0);

    // A track whose artist and album fit on one line goes back to it.
    bridge->setProperty("currentTrackArtist", QStringLiteral("Ash"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Dusk"));
    QTRY_VERIFY(!panel->property("wideArtistAlbumSplit").toBool());
    QTRY_VERIFY2(notWhole(combined, QStringLiteral("Ash — Dusk")).isEmpty(),
                 qPrintable(notWhole(combined, QStringLiteral("Ash — Dusk"))));
    QVERIFY(!artistLine->isVisible() && !albumLine->isVisible());
}

void UiMainWindowTest::nowPlayingMarksFollowTheFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("14 pt") << 14.0;
    QTest::newRow("24 pt") << 24.0;
}

// The marks of the now-playing panel follow the font as the transport's
// glyphs do (#373 H3): the disc mark of a track without a cover, which
// takes the narrow layout in either window (#373 H8), 32 px, never over a
// third of the tile, which keeps its size, and even, at the tile's centre
// on whole pixels; and the 16 px mark of nothing playing's compact row
// (#373 H7).
void UiMainWindowTest::nowPlayingMarksFollowTheFont() {
    QFETCH(qreal, pointSize);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    // Tall enough for the wide layout at 24 pt.
    QQuickWindow *window = loadMainWindow(engine, libraryModel, 1600, 1400);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QFont font = window->property("font").value<QFont>();
    font.setPointSizeF(pointSize);
    QVERIFY(window->setProperty("font", font));
    QObject *palette = paletteOf(window);
    QTRY_COMPARE(palette->property("basePointSize").toReal(), pointSize);
    const qreal scale = std::max(1.0, pointSize / 10.0);
    const auto scaled = [&](int size) { return std::max<qreal>(size, std::round(size * scale)); };
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QVERIFY(panel && cover);
    QTRY_VERIFY(panel->property("layoutMode").toString() == QStringLiteral("wide"));

    // Nothing playing: the compact row's mark.
    QVERIFY(panel->property("idle").toBool());
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    QCOMPARE(named(window, "nowPlayingIdleGlyph")->width(), scaled(16));

    // A track without a cover: the mark at the tile's centre.
    QQuickItem *mark = itemNamed(cover, QStringLiteral("albumArtPlaceholderGlyph"));
    QVERIFY(mark != nullptr);
    for (const bool narrow : {false, true}) {
        if (narrow) {
            window->resize(1000, 720);
            QTRY_COMPARE(window->width(), 1000);
        }
        playTrack(bridgeOf(window));
        bridgeOf(window)->setProperty("currentTrackCoverPath", QString());
        QVERIFY(grown(window));
        QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
        QTRY_COMPARE(cover->width(), 96.0);
        QVERIFY(shown(mark));
        // Even, so that it sits on whole pixels at the tile's centre.
        const qreal expected = 2 * std::round(std::min(std::round(cover->width() / 3), scaled(48)) / 2);
        QCOMPARE(expected, 32.0);
        QTRY_COMPARE(mark->width(), expected);
        QCOMPARE(mark->height(), mark->width());
        QCOMPARE(sceneRect(mark).center(), sceneRect(cover).center());
        QCOMPARE(mark->property("source").toString(), QStringLiteral("media-optical-audio-symbolic"));
        QCOMPARE(colorOf(mark), hex(panel->property("placeholderMarkColor").value<QColor>()));
        // The tile keeps the size a cover has.
        QCOMPARE(cover->height(), cover->width());
    }
}

// A track whose tag lacks its title and artist shows its file name as the
// title and its album alone, with no stand-in for the artist (#560).
void UiMainWindowTest::nowPlayingShowsTheFileNameOfATrackWithoutTitleOrArtist() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    playTrack(bridge);
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist A/Album A/untitled take.flac"));
    bridge->setProperty("currentTrackTitle", QString());
    bridge->setProperty("currentTrackArtist", QString());
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QVERIFY(panel != nullptr);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QQuickItem *title = named(window, "nowPlayingTitle");
    QQuickItem *artistAlbum = named(window, "nowPlayingArtistAlbum");
    QVERIFY(title && artistAlbum);
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("untitled take.flac"));
    QTRY_COMPARE(artistAlbum->property("text").toString(), QStringLiteral("Small Hours"));
}

void UiMainWindowTest::nothingPlayingIsACompactRow_data() {
    QTest::addColumn<bool>("narrow");
    QTest::addColumn<bool>("dark");
    QTest::newRow("wide, light") << false << false;
    QTest::newRow("wide, dark") << false << true;
    QTest::newRow("narrow, light") << true << false;
    QTest::newRow("narrow, dark") << true << true;
}

// With nothing loaded, as on the first run, the panel is one compact row
// (#373 H7: the empty 360 px square, with "Nothing playing" far under it,
// took the library's room): the disc mark and "Nothing playing" in a list
// row's height, with no cover's square, in the wide window and the narrow
// one alike; the library has the rest of the sidebar. The first Play grows
// the track's layout from it (nowPlayingGrowsFromTheCompactRowOnTheFirstPlay):
// the library then starts as far down as the layout is taller than the row.
void UiMainWindowTest::nothingPlayingIsACompactRow() {
    QFETCH(bool, narrow);
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, narrow ? 1000 : 1600, narrow ? 720 : 1000);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QCOMPARE(paletteOf(window)->property("themeIsDark").toBool(), dark);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *library = named(window, "libraryView");
    QVERIFY(panel && cover && header && library);
    QTRY_COMPARE(panel->property("layoutMode").toString(), narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    // Nothing to open or show from the keyboard while nothing plays.
    QQuickItem *stop = itemNamed(cover, QStringLiteral("albumArtFocusTarget"));
    QVERIFY(stop != nullptr);
    QVERIFY(!stop->isEnabled());
    // The row's text is what a screen reader reads of it.
    QQuickItem *label = named(window, "nowPlayingIdleLabel");
    QCOMPARE(QQmlProperty(label, QStringLiteral("Accessible.name"), qmlContext(label)).read().toString(),
             QStringLiteral("Nothing playing"));
    const qreal idleTop = sceneRect(header).top();
    const qreal idleLibrary = library->height();

    // The first Play: the track's layout, and the library under it.
    playTrack(bridgeOf(window));
    QTRY_VERIFY(!panel->property("idle").toBool());
    QVERIFY(grown(window));
    QTRY_COMPARE(cover->width(), narrow ? 96.0 : panel->width());
    QCOMPARE(cover->height(), cover->width());
    QVERIFY(shown(cover));
    QCOMPARE(sceneRect(cover).topLeft(), sceneRect(panel).topLeft());
    QVERIFY(!shown(named(window, "nowPlayingIdleRow")));
    QQuickItem *title = named(window, narrow ? "nowPlayingNarrowTitle" : "nowPlayingTitle");
    QVERIFY(shown(title));
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("Morning Tide Field"));
    const qreal grownBy = panel->height() - panel->property("restHeight").toReal();
    QTRY_COMPARE(sceneRect(header).top(), sceneRect(panel).bottom());
    QCOMPARE(sceneRect(header).top(), idleTop + grownBy);
    // The library's view is whole rows high: within a row of it.
    QTRY_VERIFY2(std::abs(idleLibrary - library->height() - grownBy)
                     < paletteOf(window)->property("rowHeight").toReal(),
                 qPrintable(QStringLiteral("%1 to %2").arg(idleLibrary).arg(library->height())));
    // A track without a cover: the narrow layout, and the placeholder's
    // surface, with its mark.
    bridgeOf(window)->setProperty("currentTrackCoverPath", QString());
    QTRY_COMPARE(cover->width(), 96.0);
    QQuickItem *mark = itemNamed(cover, QStringLiteral("albumArtPlaceholderGlyph"));
    QVERIFY(shown(mark));
    QCOMPARE(mark->width(), 32.0);
    QCOMPARE(colorOf(cover), tokenHex(window, "view"));
    QTRY_VERIFY2(placeholderSurfaceProblem(window, cover, mark).isEmpty(),
                 qPrintable(placeholderSurfaceProblem(window, cover, mark)));

    // Back to nothing loaded, as when the queue is cleared: the row again.
    clearPlayback(bridgeOf(window));
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    QTRY_COMPARE(sceneRect(header).top(), idleTop);
}

void UiMainWindowTest::nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("shortHeight");
    QTest::addColumn<int>("tallHeight");
    // The default font: wide in 1000 px of height, narrow in 700.
    QTest::newRow("default font") << 0.0 << 700 << 1000;
    // 24 pt: narrow in 1000 px of height (#373 H6), wide in 1400.
    QTest::newRow("24 pt") << 24.0 << 1000 << 1400;
}

// Where the wide layout would leave the library fewer than six rows under
// its header, as with a large font or in a short window, the now-playing
// panel takes its narrow layout, with its 96 px cover (#373 H6: at 24 pt in
// 1600 by 1000 the library showed three rows). It is decided by the font,
// the sidebar's width and height, not by the layout it chooses nor by the
// track, so it does not switch back and forth: once as the window gets
// short, once as it gets tall again, never for a track.
void UiMainWindowTest::nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort() {
    QFETCH(qreal, pointSize);
    QFETCH(int, shortHeight);
    QFETCH(int, tallHeight);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont large = applicationFont;
        large.setPointSizeF(pointSize);
        QGuiApplication::setFont(large);
    }
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, 1600, tallHeight);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *palette = paletteOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *library = named(window, "libraryView");
    QVERIFY(panel && cover && library);
    const qreal rowHeight = palette->property("rowHeight").toReal();
    // How many library rows show.
    const auto rows = [&]() { return library->height() / rowHeight; };

    // Tall: wide, and nothing playing the compact row, whatever the
    // layout (#373 H7).
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QVERIFY(!panel->property("shortHeight").toBool());
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    QSignalSpy switches(panel, SIGNAL(layoutModeChanged()));
    QVERIFY(switches.isValid());

    // A track: the library has its rows. The height the switch is decided
    // by is the wide layout's own, all its lines counted.
    QObject *bridge = bridgeOf(window);
    playTrack(bridge);
    // An artist and album that share a line at any font.
    bridge->setProperty("currentTrackArtist", QStringLiteral("Ann"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Dusk"));
    QVERIFY(grown(window));
    QTRY_COMPARE(cover->width(), panel->width());
    const qreal reserved = panel->property("wideReservedHeight").toReal();
    QTRY_VERIFY2(std::abs(panel->height() - reserved) <= 1.0,
                 qPrintable(QStringLiteral("%1 against %2").arg(panel->height()).arg(reserved)));
    QVERIFY2(rows() >= 6, qPrintable(QString::number(rows())));

    // Short: narrow, the 96 px cover, and at least six rows of the library.
    window->resize(1600, shortHeight);
    QTRY_COMPARE(window->height(), shortHeight);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QVERIFY(panel->property("shortHeight").toBool());
    QTRY_COMPARE(cover->width(), 96.0);
    QCOMPARE(panel->property("wideReservedHeight").toReal(), reserved);
    QTRY_VERIFY2(rows() >= 6, qPrintable(QString::number(rows())));
    QTest::qWait(100);
    QCOMPARE(switches.count(), 1);

    // A track, with tags and then without, switches nothing; nor does
    // nothing playing, which is the compact row in it too.
    clearTrackTags(bridge);
    QTRY_COMPARE(panel->property("artistAlbumText").toString(), QString());
    playTrack(bridge);
    clearPlayback(bridge);
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    playTrack(bridge);
    QVERIFY(grown(window));
    QTest::qWait(100);
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QCOMPARE(switches.count(), 1);

    // Tall again: wide again, once.
    window->resize(1600, tallHeight);
    QTRY_COMPARE(window->height(), tallHeight);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QTRY_COMPARE(cover->width(), panel->width());
    QTest::qWait(100);
    QCOMPARE(switches.count(), 2);
}

// The cover is a keyboard stop while a track plays (#373 H5): Tab reaches it
// after the transport and leaves it for the library, a 1 px ring shows its
// focus only while the keyboard is in use, the Menu key and Shift+F10 open
// its context menu, and the pointer lays a faint veil on it.
void UiMainWindowTest::nowPlayingCoverIsAKeyboardStop() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, kAllExpanded);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    playTrack(bridge);
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *stop = named(window, "albumArtFocusTarget");
    QQuickItem *volume = named(window, "volumeSlider");
    QQuickItem *library = named(window, "libraryView");
    QQuickItem *ring = named(window, "albumArtFocusRing");
    QQuickItem *veil = named(window, "albumArtHoverVeil");
    QVERIFY(cover && stop && volume && library && ring && veil);
    QTRY_VERIFY(stop->isEnabled());
    QVERIFY(grown(window));
    QVERIFY(stop->activeFocusOnTab());
    QCOMPARE(sceneRect(stop), sceneRect(cover));

    // After the transport's last control, before the library.
    volume->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(volume->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY2(stop->hasActiveFocus(),
                 qPrintable(QStringLiteral("Tab went to %1")
                                .arg(window->activeFocusItem() ? window->activeFocusItem()->objectName()
                                                               : QStringLiteral("nothing"))));
    // The ring: 1 px, inside the tile, in the focus ring's colour.
    QTRY_VERIFY(shown(ring));
    QCOMPARE(QQmlProperty(ring, QStringLiteral("border.width")).read().toReal(), 1.0);
    QCOMPARE(hex(QQmlProperty(ring, QStringLiteral("border.color")).read().value<QColor>()),
             tokenHex(window, "focusRing"));
    QCOMPARE(sceneRect(ring), sceneRect(cover));
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(library->hasActiveFocus());
    QVERIFY(!shown(ring));
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY(stop->hasActiveFocus());
    QTRY_VERIFY(shown(ring));

    // Its context menu from the keyboard: the Menu key, then Shift+F10.
    const auto openMenu = [&]() -> QObject * {
        for (QObject *menu : window->findChildren<QObject *>(QStringLiteral("albumArtMenu"))) {
            if (menu->property("opened").toBool()) {
                return menu;
            }
        }
        return nullptr;
    };
    for (const auto &[key, modifiers] : {std::pair{Qt::Key_Menu, Qt::NoModifier},
                                         std::pair{Qt::Key_F10, Qt::ShiftModifier}}) {
        QTest::keyClick(window, key, modifiers);
        QObject *menu = nullptr;
        QTRY_VERIFY((menu = openMenu()) != nullptr);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(openMenu() == nullptr);
        QTRY_VERIFY(stop->hasActiveFocus());
    }

    // The pointer: no ring, as the keyboard is not in use, and the veil, in
    // the hover tint, over the whole tile; gone once the pointer leaves.
    QVERIFY(!shown(veil));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(cover).center().toPoint());
    QTRY_VERIFY(!shown(ring));
    QTRY_VERIFY(shown(veil));
    QTRY_COMPARE(veil->opacity(), 1.0);
    QCOMPARE(colorOf(veil), tokenHex(window, "hover"));
    QCOMPARE(sceneRect(veil), sceneRect(cover));
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 60));
    QTRY_VERIFY(!shown(veil));

    // Playback ends while the cover has the keyboard's focus: the idle tile
    // shows no ring and opens no menu, and Tab passes it by.
    library->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(library->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY(stop->hasActiveFocus());
    QTRY_VERIFY(shown(ring));
    for (const char *property : {"currentTrackPath", "currentTrackTitle", "currentTrackArtist", "currentTrackAlbum"}) {
        bridge->setProperty(property, QString());
    }
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_VERIFY(named(window, "nowPlayingPanel")->property("idle").toBool());
    QVERIFY(!shown(ring));
    QVERIFY(!stop->hasActiveFocus());
    QVERIFY(!stop->isEnabled());
    volume->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(volume->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY(library->hasActiveFocus());
}

// A library folder's row, as with more than one folder, is a folder header
// (#373 H5): a folder glyph in the column where an album has its thumbnail,
// its name semibold after it, where an album's starts, and its track count
// at the right end in the tertiary colour, where an album's year is. An
// artist's row under it is as any other.
void UiMainWindowTest::libraryFoldersAreFolderHeaders() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music"), QStringLiteral("/shelf")});
    bridge->setProperty("libraryRootEntries",
                        QVariantList{QVariantMap{{QStringLiteral("path"), QStringLiteral("/music")},
                                                 {QStringLiteral("name"), QStringLiteral("Shelf One")},
                                                 {QStringLiteral("trackCount"), 180}},
                                     QVariantMap{{QStringLiteral("path"), QStringLiteral("/shelf")},
                                                 {QStringLiteral("trackCount"), 1}}});
    libraryModel.setExpandedKeys({kRootKey, kArtistKey});
    libraryModel.setLibraryTreeFromBinary(artistAlbumTrackTree(QStringLiteral("/music/Artist A"),
                                                               QStringLiteral("Album A"), QStringLiteral("Shelf One")));
    QQuickItem *folder = nullptr;
    QQuickItem *artist = nullptr;
    QQuickItem *album = nullptr;
    QTRY_VERIFY((folder = libraryRow(window, QStringLiteral("root"))) != nullptr
                && (artist = libraryRow(window, QStringLiteral("artist"))) != nullptr
                && (album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    const auto xOf = [](QQuickItem *row, const char *name) {
        QQuickItem *item = itemNamed(row, QString::fromLatin1(name));
        return shown(item) ? item->mapToItem(row, QPointF()).x() : -1.0;
    };

    QQuickItem *chevron = itemNamed(folder, QStringLiteral("libraryRowChevron"));
    QQuickItem *glyph = itemNamed(folder, QStringLiteral("libraryRowFolderGlyph"));
    QQuickItem *title = itemNamed(folder, QStringLiteral("libraryRowTitle"));
    QQuickItem *count = itemNamed(folder, QStringLiteral("libraryRowCount"));
    QVERIFY(shown(chevron) && shown(glyph) && title && shown(count));
    // The chevron at the inset, then the glyph centred in the 20 px column
    // after it, then the name at 62, as an album's one step in is at 78.
    QCOMPARE(xOf(folder, "libraryRowChevron"), 12.0);
    QCOMPARE(glyph->property("source").toString(), QStringLiteral("folder"));
    QVERIFY(glyph->property("valid").toBool());
    QCOMPARE(glyph->width(), 16.0);
    QCOMPARE(xOf(folder, "libraryRowFolderGlyph"), 12.0 + 16 + 6 + 2);
    QVERIFY(std::abs(glyph->y() + glyph->height() / 2 - folder->height() / 2) <= 0.5);
    QCOMPARE(colorOf(glyph), tokenHex(window, "textSecondary"));
    QCOMPARE(xOf(folder, "libraryRowTitle"), 12.0 + 16 + 6 + 20 + 8);
    QCOMPARE(xOf(album, "libraryRowTitle") - xOf(folder, "libraryRowTitle"), 2 * 16.0);
    QCOMPARE(title->property("text").toString(), QStringLiteral("Shelf One"));
    QCOMPARE(title->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(colorOf(title), tokenHex(window, "text"));
    // The count where an album's year is, tertiary and small.
    QCOMPARE(count->property("text").toString(), QStringLiteral("180 tracks"));
    QCOMPARE(count->x() + count->width(), folder->width() - 8 - 12);
    QCOMPARE(xOf(album, "libraryRowYear") + itemNamed(album, QStringLiteral("libraryRowYear"))->width(),
             count->x() + count->width());
    QCOMPARE(colorOf(count), tokenHex(window, "textTertiary"));
    QCOMPARE(count->property("font").value<QFont>().pointSizeF(),
             paletteOf(window)->property("smallPointSize").toReal());
    QCOMPARE(title->x() + title->width(), count->x() - 8);
    QVERIFY(itemNamed(folder, QStringLiteral("libraryRowYear")) == nullptr);
    // An artist's row: no glyph or count, its name as any other's.
    QVERIFY(!shown(itemNamed(artist, QStringLiteral("libraryRowFolderGlyph"))));
    QVERIFY(itemNamed(artist, QStringLiteral("libraryRowCount")) == nullptr);
    QCOMPARE(itemNamed(artist, QStringLiteral("libraryRowTitle"))->property("font").value<QFont>().weight(),
             QFont::Normal);

    // Collapsed and holding the playing track: its name amber.
    playTrack(bridge);
    libraryModel.toggleKey(kRootKey);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("artist")) == nullptr);
    folder = libraryRow(window, QStringLiteral("root"));
    QVERIFY(folder != nullptr);
    QTRY_VERIFY2(nameMarksNow(window, folder, false).isEmpty(), qPrintable(nameMarksNow(window, folder, false)));
    QVERIFY(shown(itemNamed(folder, QStringLiteral("libraryRowFolderGlyph"))));
}

void UiMainWindowTest::regionHeadersNameTheRegionAndItsTotals() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *libraryHeader = named(window, "libraryHeader");
    QQuickItem *queueHeader = named(window, "queueHeader");
    QVERIFY(libraryHeader && queueHeader);
    QQuickItem *libraryTitle = itemNamed(libraryHeader, QStringLiteral("regionHeaderTitle"));
    QQuickItem *libraryTotals = itemNamed(libraryHeader, QStringLiteral("regionHeaderTotals"));
    QQuickItem *queueTitle = itemNamed(queueHeader, QStringLiteral("regionHeaderTitle"));
    QQuickItem *queueTotals = itemNamed(queueHeader, QStringLiteral("regionHeaderTotals"));
    QVERIFY(libraryTitle && libraryTotals && queueTitle && queueTotals);

    bridge->setProperty("libraryRootCount", 1);
    bridge->setProperty("libraryArtistCount", 12);
    bridge->setProperty("libraryAlbumCount", 28);
    bridge->setProperty("libraryTrackCount", 176);
    QTRY_COMPARE(libraryTotals->property("text").toString(),
                 QStringLiteral("12 artists · 28 albums · 176 tracks"));
    QCOMPARE(libraryTitle->property("text").toString(), QStringLiteral("Library"));
    QVERIFY(!libraryTitle->property("truncated").toBool());
    QCOMPARE(colorOf(libraryTotals), tokenHex(window, "textTertiary"));
    bridge->setProperty("libraryArtistCount", 1);
    QTRY_COMPARE(libraryTotals->property("text").toString(),
                 QStringLiteral("1 artist · 28 albums · 176 tracks"));
    // A scan shows its progress in the status bar, not after the totals.
    QQuickItem *scanCount = named(window, "statusBarScanCount");
    QVERIFY(scanCount != nullptr);
    // The status bar shows the scan of a connected bridge.
    bridge->setProperty("connected", true);
    bridge->setProperty("libraryScanProcessed", 41);
    bridge->setProperty("libraryScanDiscovered", 41);
    bridge->setProperty("libraryScanTotal", 120);
    bridge->setProperty("libraryScanInProgress", true);
    QTRY_VERIFY(shown(scanCount));
    QCOMPARE(scanCount->property("text").toString(), QStringLiteral("41 / 120"));
    QCOMPARE(libraryTotals->property("text").toString(),
             QStringLiteral("1 artist · 28 albums · 176 tracks"));
    bridge->setProperty("libraryScanInProgress", false);
    QTRY_VERIFY(!shown(scanCount));

    // The queue's name and totals.
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    bridge->setProperty("queueDurationText", QStringLiteral("3:00"));
    QTRY_COMPARE(queueTotals->property("text").toString(), QStringLiteral("3 tracks · 3:00"));
    QCOMPARE(queueTitle->property("text").toString(), QStringLiteral("Queue"));

    // Narrow: the library's totals keep the tracks only.
    window->resize(1000, 720);
    QTRY_COMPARE(libraryTotals->property("text").toString(), QStringLiteral("176 tracks"));
    QVERIFY(!libraryTitle->property("truncated").toBool());
}

void UiMainWindowTest::libraryTreeHasOneIndentStep() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(libraryModel, kAllExpanded);
    QQuickItem *artist = nullptr;
    QQuickItem *album = nullptr;
    QQuickItem *track = nullptr;
    QTRY_VERIFY((artist = libraryRow(window, QStringLiteral("artist"))) != nullptr
                && (album = libraryRow(window, QStringLiteral("album"))) != nullptr
                && (track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    const qreal rowHeight = paletteOf(window)->property("rowHeight").toReal();
    for (QQuickItem *row : {artist, album, track}) {
        QCOMPARE(row->height(), rowHeight);
    }
    // The x of `name` in `row`, from the row's left edge.
    const auto xOf = [](QQuickItem *row, const char *name) {
        QQuickItem *item = itemNamed(row, QString::fromLatin1(name));
        return shown(item) ? item->mapToItem(row, QPointF()).x() : -1.0;
    };
    const auto text = [](QQuickItem *row, const char *name) {
        QQuickItem *item = itemNamed(row, QString::fromLatin1(name));
        return item != nullptr ? item->property("text").toString() : QString();
    };

    // Artist: the chevron at 12, its name 6 px after; no album count.
    QCOMPARE(xOf(artist, "libraryRowChevron"), 12.0);
    QCOMPARE(xOf(artist, "libraryRowTitle"), 34.0);
    QCOMPARE(text(artist, "libraryRowTitle"), QStringLiteral("Artist A"));
    QCOMPARE(itemNamed(artist, QStringLiteral("libraryRowChevron"))->property("source").toString(),
             QStringLiteral("arrow-down"));
    // Album: one step in, then the 20 px thumbnail, then the name at 78;
    // the year at the right end, tertiary, clear of the scrollbar lane and
    // the inset: the mark a collapsed row that holds the playing track has
    // follows the name, not the year (#373 H4).
    QCOMPARE(xOf(album, "libraryRowChevron"), 28.0);
    QCOMPARE(xOf(album, "libraryRowThumbnail"), 50.0);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowThumbnail"))->width(), 20.0);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowThumbnail"))->property("radius").toReal(), 2.0);
    QCOMPARE(xOf(album, "libraryRowTitle"), 78.0);
    QCOMPARE(text(album, "libraryRowTitle"), QStringLiteral("Album A"));
    QQuickItem *year = itemNamed(album, QStringLiteral("libraryRowYear"));
    QVERIFY(shown(year));
    QCOMPARE(year->property("text").toString(), QStringLiteral("2005"));
    QCOMPARE(year->x() + year->width(), album->width() - 8 - 12);
    QCOMPARE(colorOf(year), tokenHex(window, "textTertiary"));
    // Track: its number under the thumbnail, right-aligned, and its title
    // where the album's name starts; no number in the title.
    QQuickItem *number = itemNamed(track, QStringLiteral("libraryRowNumber"));
    QVERIFY(shown(number));
    QCOMPARE(xOf(track, "libraryRowNumber"), 50.0);
    QCOMPARE(number->width(), 20.0);
    QCOMPARE(number->property("horizontalAlignment").toInt(), int(Qt::AlignRight));
    QCOMPARE(number->property("text").toString(), QStringLiteral("2"));
    QCOMPARE(colorOf(number), tokenHex(window, "textTertiary"));
    QCOMPARE(xOf(track, "libraryRowTitle"), 78.0);
    QCOMPARE(text(track, "libraryRowTitle"), QStringLiteral("Second"));
    QVERIFY(!shown(itemNamed(track, QStringLiteral("libraryRowChevron"))));
    QVERIFY(!shown(itemNamed(track, QStringLiteral("libraryRowYear"))));

    // A collapsed album points right.
    libraryModel.toggleKey(QStringLiteral("album|Artist A|Album A"));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    album = libraryRow(window, QStringLiteral("album"));
    QVERIFY(album != nullptr);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowChevron"))->property("source").toString(),
             QStringLiteral("arrow-right"));
}

// At 24 pt the tree's chevrons and state glyphs grow with the font, 2.4
// times their 16 and 12 px, and the indent step with the chevrons, so that
// each level's chevron is where its parent's ends.
void UiMainWindowTest::libraryTreeChevronsGrowWithALargeFont() {
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont large = applicationFont;
    large.setPointSizeF(24);
    QGuiApplication::setFont(large);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(libraryModel, kAllExpanded);
    QQuickItem *artist = nullptr;
    QQuickItem *album = nullptr;
    QQuickItem *track = nullptr;
    QTRY_VERIFY((artist = libraryRow(window, QStringLiteral("artist"))) != nullptr
                && (album = libraryRow(window, QStringLiteral("album"))) != nullptr
                && (track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    playTrack(bridgeOf(window));
    QQuickItem *artistChevron = itemNamed(artist, QStringLiteral("libraryRowChevron"));
    QQuickItem *albumChevron = itemNamed(album, QStringLiteral("libraryRowChevron"));
    QQuickItem *state = itemNamed(track, QStringLiteral("libraryRowState"));
    QVERIFY(artistChevron && albumChevron && state);
    QCOMPARE(artistChevron->size(), QSizeF(38, 38));
    QCOMPARE(albumChevron->x() - artistChevron->x(), 38.0);
    QTRY_VERIFY(state->isVisible());
    QCOMPARE(state->size(), QSizeF(29, 29));
    // Each inside its row.
    for (QQuickItem *item : {artistChevron, albumChevron, state}) {
        QQuickItem *row = item->parentItem();
        QVERIFY(item->y() >= 0 && item->y() + item->height() <= row->height());
    }
    // The title after the chevron and its gap, clear of it.
    QVERIFY(itemNamed(artist, QStringLiteral("libraryRowTitle"))->x() >= artistChevron->x() + 38 + 6);
    // A collapsed album that holds the playing track has its name amber at
    // a large font too.
    libraryModel.toggleKey(kAlbumKey);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    QTRY_VERIFY2(nameMarksNow(window, libraryRow(window, QStringLiteral("album")), false).isEmpty(),
                 qPrintable(nameMarksNow(window, libraryRow(window, QStringLiteral("album")), false)));
}

void UiMainWindowTest::libraryTreeMarksThePlayingTrackAndItsCollapsedAncestors() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, kAllExpanded);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    const auto glyph = [](QQuickItem *row) { return itemNamed(row, QStringLiteral("libraryRowState")); };
    const auto glyphName = [&glyph](QQuickItem *row) {
        QQuickItem *item = row != nullptr ? glyph(row) : nullptr;
        return shown(item) ? item->property("source").toString() : QString();
    };
    const auto title = [](QQuickItem *row) { return itemNamed(row, QStringLiteral("libraryRowTitle")); };

    // Playing: an amber glyph in the album's chevron column and a semibold
    // amber title.
    playTrack(bridge);
    QQuickItem *track = libraryRow(window, QStringLiteral("track"));
    QTRY_COMPARE(glyphName(track), QStringLiteral("media-playback-start"));
    QCOMPARE(colorOf(glyph(track)), tokenHex(window, "now"));
    QCOMPARE(glyph(track)->width(), 12.0);
    QCOMPARE(glyph(track)->mapToItem(track, QPointF()).x(), 28.0 + 2.0);
    QCOMPARE(colorOf(title(track)), tokenHex(window, "now"));
    QCOMPARE(title(track)->property("font").value<QFont>().weight(), QFont::DemiBold);
    // Its expanded album and artist show nothing.
    QCOMPARE(glyphName(libraryRow(window, QStringLiteral("album"))), QString());
    QCOMPARE(glyphName(libraryRow(window, QStringLiteral("artist"))), QString());
    // Paused: the pause glyph, still amber.
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_COMPARE(glyphName(track), QStringLiteral("media-playback-pause"));
    QCOMPARE(colorOf(title(track)), tokenHex(window, "now"));
    // Stopped: a neutral stop mark, and the title as any other.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(glyphName(track), QStringLiteral("media-playback-stop"));
    QCOMPARE(colorOf(glyph(track)), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(title(track)), tokenHex(window, "text"));
    bridge->setProperty("playbackState", QStringLiteral("Playing"));

    // Expanded rows that hold it keep their names plain: the track shows.
    QTRY_COMPARE(colorOf(title(track)), tokenHex(window, "now"));
    QCOMPARE(colorOf(title(libraryRow(window, QStringLiteral("album")))), tokenHex(window, "text"));
    QCOMPARE(colorOf(title(libraryRow(window, QStringLiteral("artist")))), tokenHex(window, "text"));

    // A collapsed album that holds the playing track: its name amber and
    // semibold, as the queue's playing title, and no glyph at all, which
    // read as a play button (#373 H5). Its year stays tertiary.
    const auto checkName = [&](QQuickItem *row, bool paused) {
        const QString wrong = nameMarksNow(window, row, paused);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
    };
    libraryModel.toggleKey(QStringLiteral("album|Artist A|Album A"));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    QQuickItem *album = libraryRow(window, QStringLiteral("album"));
    QVERIFY(album != nullptr);
    QTRY_COMPARE(colorOf(title(album)), tokenHex(window, "now"));
    checkName(album, false);
    QCOMPARE(glyphName(album), QString());
    QCOMPARE(colorOf(itemNamed(album, QStringLiteral("libraryRowYear"))), tokenHex(window, "textTertiary"));
    // On a hovered and a selected row too: the same amber, over both tints.
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 1)));
    named(window, "libraryView")->forceActiveFocus();
    QTest::mouseMove(window, sceneRect(album).center().toPoint());
    QTRY_VERIFY(shown(itemNamed(album, QStringLiteral("rowBackgroundHover"))));
    QTRY_VERIFY(shown(itemNamed(album, QStringLiteral("rowBackgroundTint"))));
    checkName(album, false);
    const QList<QQuickItem *> children = album->childItems();
    QVERIFY(children.indexOf(title(album))
            > children.indexOf(itemNamed(album, QStringLiteral("rowBackgroundTint"))->parentItem()));
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 60));
    QVERIFY(QMetaObject::invokeMethod(controller, "clearPrimarySelection"));
    libraryModel.clearSelection();
    // So does a collapsed artist.
    libraryModel.collapseAll();
    QTRY_VERIFY(libraryRow(window, QStringLiteral("album")) == nullptr);
    QQuickItem *artist = libraryRow(window, QStringLiteral("artist"));
    QVERIFY(artist != nullptr);
    QTRY_COMPARE(colorOf(title(artist)), tokenHex(window, "now"));
    checkName(artist, false);
    // Paused: the same amber, quieter, still 4.5:1 on the row; stopped,
    // the name as any other: amber means now.
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_VERIFY(colorOf(title(artist)) != tokenHex(window, "now"));
    checkName(artist, true);
    const qreal pausedOpacity = paletteOf(window)->property("nowPausedOpacity").toReal();
    QVERIFY2(pausedOpacity >= 0.7 && pausedOpacity < 1.0, qPrintable(QString::number(pausedOpacity)));
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(colorOf(title(artist)), tokenHex(window, "text"));
    QCOMPARE(title(artist)->property("font").value<QFont>().weight(), QFont::Normal);
    // Another track, in other rows: nothing.
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_COMPARE(colorOf(title(artist)), tokenHex(window, "now"));
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist B/Album/01.flac"));
    bridge->setProperty("currentTrackTreeKeys", QStringList{QStringLiteral("artist|Artist B")});
    QTRY_COMPARE(colorOf(title(artist)), tokenHex(window, "text"));
}

// A name too long for its row elides before the year, which keeps its
// place, and turns amber as it is, with nothing after it (#373 H5).
void UiMainWindowTest::collapsedRowNameElidesBeforeTheYear() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    const QString longName = QStringLiteral("An Album Whose Name Runs On ") + QStringLiteral("And On ").repeated(30);
    showTree(libraryModel, {kArtistKey}, QStringLiteral("/music/Artist A"), longName);
    QQuickItem *album = nullptr;
    QTRY_VERIFY((album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    QQuickItem *title = itemNamed(album, QStringLiteral("libraryRowTitle"));
    QQuickItem *year = itemNamed(album, QStringLiteral("libraryRowYear"));
    QVERIFY(title != nullptr && shown(year));
    QVERIFY(title->property("implicitWidth").toReal() > title->width());
    // The year at the right end, the name's box a gap before it.
    QCOMPARE(year->x() + year->width(), album->width() - 8 - 12);
    QCOMPARE(title->x() + title->width(), year->x() - 8);
    const QRectF yearRect = sceneRect(year);

    playTrack(bridge);
    QTRY_COMPARE(colorOf(title), tokenHex(window, "now"));
    const QString wrong = nameMarksNow(window, album, false);
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
    QVERIFY(title->property("truncated").toBool());
    QCOMPARE(title->x() + title->width(), year->x() - 8);
    QCOMPARE(sceneRect(year), yearRect);
}

void UiMainWindowTest::collapsedArtistMarkFollowsTheBackendsGroupingNotItsPath() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    const auto artistMark = [&]() {
        QQuickItem *artist = libraryRow(window, QStringLiteral("artist"));
        QQuickItem *title = artist != nullptr ? itemNamed(artist, QStringLiteral("libraryRowTitle")) : nullptr;
        return title != nullptr && colorOf(title) == tokenHex(window, "now");
    };
    // One artist gathers a loose file right in the library folder, tagged
    // with its name, and an album track in its own folder. Its row has the
    // path of whichever the backend met first, which a rescan can change.
    for (const QString &artistPath : {QStringLiteral("/music"), QStringLiteral("/music/Artist A")}) {
        showTree(libraryModel, {}, artistPath);
        QTRY_VERIFY(libraryRow(window, QStringLiteral("artist")) != nullptr);
        QCOMPARE(libraryRow(window, QStringLiteral("artist"))->property("openPathResolved").toString(),
                 artistPath);
        // The loose file.
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/single.flac"));
        bridge->setProperty("currentTrackTreeKeys", QStringList{QStringLiteral("root|/music"), kArtistKey});
        QTRY_VERIFY(artistMark());
        // The album track.
        bridge->setProperty("currentTrackPath", kTrackPath);
        bridge->setProperty("currentTrackTreeKeys",
                            QStringList{QStringLiteral("root|/music"), kArtistKey, kAlbumKey});
        QTRY_VERIFY(artistMark());
        // A track the library does not have: no rows hold it.
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist A/elsewhere.flac"));
        bridge->setProperty("currentTrackTreeKeys", QStringList{});
        QTRY_VERIFY(!artistMark());
    }
}

void UiMainWindowTest::libraryRowsShowHoverSelectionAndTheCurrentRow() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(libraryModel, kAllExpanded);
    QQuickItem *album = nullptr;
    QTRY_VERIFY((album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(view && controller);
    QQuickItem *tint = itemNamed(album, QStringLiteral("rowBackgroundTint"));
    QQuickItem *hover = itemNamed(album, QStringLiteral("rowBackgroundHover"));
    QVERIFY(tint && hover);
    // The tints run to the pane's right edge (#373 H6: they stopped 8 px
    // short of it); the scrollbar shows only when the tree is longer than
    // the view.
    QCOMPARE(tint->width(), album->width());
    QCOMPARE(tint->mapToItem(album, QPointF()).x(), 0.0);
    QCOMPARE(sceneRect(album).right(), sceneRect(view).right());
    QQuickItem *scrollBar = named(window, "libraryScrollBar");
    QVERIFY(scrollBar != nullptr);
    QVERIFY(!scrollBar->isVisible());
    QCOMPARE(scrollBar->width(), 8.0);

    // At rest: neither tint.
    QVERIFY(!tint->isVisible());
    QVERIFY(!hover->isVisible());
    // Hover: the hover tint.
    QTest::mouseMove(window, sceneRect(album).center().toPoint());
    QTRY_VERIFY(hover->isVisible() && hover->opacity() == 1.0);
    QCOMPARE(colorOf(hover), tokenHex(window, "hover"));
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 60));
    QTRY_VERIFY(!hover->isVisible());

    // Selected without focus: the inactive tint, no ring; the year steps up
    // to the secondary colour.
    QQuickItem *year = itemNamed(album, QStringLiteral("libraryRowYear"));
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 1)));
    named(window, "playlistView")->forceActiveFocus();
    QTRY_VERIFY(!view->hasActiveFocus());
    QTRY_VERIFY(tint->isVisible());
    QCOMPARE(colorOf(tint), tokenHex(window, "selectionInactive"));
    QCOMPARE(QQmlProperty(tint, QStringLiteral("border.width")).read().toReal(), 0.0);
    QCOMPARE(colorOf(year), tokenHex(window, "textSecondary"));
    // With focus from the keyboard: the tint, and the ring on the current
    // row as one rectangle.
    view->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(view->hasActiveFocus());
    QTRY_COMPARE(colorOf(tint), tokenHex(window, "selection"));
    QCOMPARE(QQmlProperty(tint, QStringLiteral("border.width")).read().toReal(), 1.0);
    QCOMPARE(hex(QQmlProperty(tint, QStringLiteral("border.color")).read().value<QColor>()),
             tokenHex(window, "focusRing"));
    // Down moves the current row, and the ring with it.
    QTest::keyClick(window, Qt::Key_Down);
    QQuickItem *track = libraryRow(window, QStringLiteral("track"));
    QQuickItem *trackTint = itemNamed(track, QStringLiteral("rowBackgroundTint"));
    QTRY_COMPARE(QQmlProperty(trackTint, QStringLiteral("border.width")).read().toReal(), 1.0);
    QTRY_VERIFY(!tint->isVisible());
    QCOMPARE(colorOf(itemNamed(track, QStringLiteral("libraryRowNumber"))), tokenHex(window, "textSecondary"));
}

// The queue's selection tint runs to the pane's right edge, with no
// scrollbar and with one, whose thumb draws over it (#373 H6: the tints
// stopped 8 px short of the edge).
void UiMainWindowTest::queueRowTintsRunToThePaneEdgeUnderTheScrollBar() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, 1600, 1000);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *bridge = bridgeOf(window);
    QQuickItem *queueView = named(window, "playlistView");
    QObject *queue = window->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(queueView != nullptr && queue != nullptr);
    QQuickItem *scrollBar = nullptr;
    for (QQuickItem *child : queueView->childItems()) {
        if (QString::fromLatin1(child->metaObject()->className()).contains(QStringLiteral("ScrollBar"))) {
            scrollBar = child;
        }
    }
    QVERIFY(scrollBar != nullptr);
    for (const int count : {3, 80}) {
        bridge->setProperty("queueRows", sampleQueueRows(count));
        bridge->setProperty("queueLength", count);
        QTRY_COMPARE(queueView->property("count").toInt(), count);
        QTRY_COMPARE(scrollBar->isVisible(), count > 3);
        QVERIFY(QMetaObject::invokeMethod(queue, "setSingleSelection", Q_ARG(QVariant, 1)));
        QQuickItem *row = nullptr;
        // The second row, beside the thumb, which starts a little below the
        // first row's middle.
        QTRY_VERIFY((row = viewRowAt(queueView, 1)) != nullptr && rowTinted(queueView, 1));
        QQuickItem *tint = itemNamed(row, QStringLiteral("rowBackgroundTint"));
        QCOMPARE(sceneRect(tint).right(), sceneRect(queueView).right());
        const QColor tintColor = tint->property("color").value<QColor>();
        const QImage image = window->grabWindow();
        const qreal scale = image.width() / qreal(window->width());
        const QPointF middle(0, sceneRect(row).center().y());
        const auto pixel = [&](qreal x) {
            return image.pixelColor(int(std::floor(x * scale)), int(std::floor(middle.y() * scale)));
        };
        const qreal right = sceneRect(queueView).right();
        const auto sameAsTint = [&](const QColor &color) {
            return std::abs(color.red() - tintColor.red()) <= 2 && std::abs(color.green() - tintColor.green()) <= 2
                && std::abs(color.blue() - tintColor.blue()) <= 2;
        };
        // The tint in the scrollbar's lane, at the edge.
        QVERIFY2(sameAsTint(pixel(right - 0.5)),
                 qPrintable(QStringLiteral("%1 at the edge, tint %2").arg(pixel(right - 0.5).name(), tintColor.name())));
        QVERIFY2(sameAsTint(pixel(right - 16)), qPrintable(pixel(right - 16).name()));
        // The thumb, 4 px wide, 2 px from the edge, over the tint.
        QCOMPARE(!sameAsTint(pixel(right - 4)), count > 3);
    }
}

// Tab into the library or the queue with no row selected: a row it shows
// becomes the current one and has the 1 px ring, so the keyboard's place
// shows. Nothing is selected and the view stays where it is.
void UiMainWindowTest::listsRingARowAsTheKeyboardComesIn() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, kAllExpanded);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    QQuickItem *libraryView = named(window, "libraryView");
    QQuickItem *queueView = named(window, "playlistView");
    QObject *library = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QObject *queue = window->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(libraryView && queueView && library && queue);
    const auto rowAt = [](QQuickItem *view, int index) {
        QQuickItem *row = nullptr;
        QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
        return row;
    };
    // Whether row `index` of `view` has the ring alone: no selection tint.
    const auto ringedAlone = [&rowAt](QQuickItem *view, int index) {
        QQuickItem *row = rowAt(view, index);
        QQuickItem *tint = row != nullptr ? itemNamed(row, QStringLiteral("rowBackgroundTint")) : nullptr;
        return shown(tint) && QQmlProperty(tint, QStringLiteral("border.width")).read().toReal() == 1.0
            && tint->property("color").value<QColor>().alpha() == 0;
    };
    const auto ringedRows = [&rowAt](QQuickItem *view) {
        QList<int> rows;
        for (int index = 0; index < view->property("count").toInt(); ++index) {
            QQuickItem *row = rowAt(view, index);
            QQuickItem *tint = row != nullptr ? itemNamed(row, QStringLiteral("rowBackgroundTint")) : nullptr;
            if (shown(tint) && QQmlProperty(tint, QStringLiteral("border.width")).read().toReal() > 0) {
                rows.push_back(index);
            }
        }
        return rows;
    };
    const auto tabInto = [window](QQuickItem *view) {
        named(window, "transportButton_playPause")->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(!view->hasActiveFocus());
        view->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(view->hasActiveFocus());
    };

    // The library: its first row, with nothing selected.
    tabInto(libraryView);
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{0});
    QVERIFY(ringedAlone(libraryView, 0));
    QCOMPARE(library->property("selectedSelectionKey").toString(), QString());
    QCOMPARE(libraryModel.selectedKeys(), QStringList());
    // The keys go on from it: Down selects the next row.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(library->property("selectedSelectionKey").toString(),
                 rowAt(libraryView, 1)->property("selectionKeyResolved").toString());
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{1});
    QVERIFY(!ringedAlone(libraryView, 1));
    // Deselected, the row stays the current one.
    QVERIFY(QMetaObject::invokeMethod(library, "clearPrimarySelection"));
    libraryModel.clearSelection();
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{1});
    QVERIFY(ringedAlone(libraryView, 1));

    // Playing, a fresh tree rings the playing track's row,
    playTrack(bridge);
    library->setProperty("focusSelectionKey", QString());
    tabInto(libraryView);
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{2});
    QVERIFY(ringedAlone(libraryView, 2));
    // or the collapsed row that holds it.
    library->setProperty("focusSelectionKey", QString());
    libraryModel.toggleKey(kAlbumKey);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    tabInto(libraryView);
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{1});
    QCOMPARE(libraryModel.selectedKeys(), QStringList());
    // Without the keyboard, no ring.
    tabInto(queueView);
    QTRY_COMPARE(ringedRows(libraryView), QList<int>{});

    // The queue, scrolled down: its first row shown, and it stays put.
    bridge->setProperty("queueRows", sampleQueueRows(80));
    bridge->setProperty("queueLength", 80);
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(queueView->property("count").toInt(), 80);
    const qreal rowHeight = queueView->property("rowHeight").toReal();
    QVERIFY(rowHeight > 0);
    named(window, "transportButton_playPause")->forceActiveFocus(Qt::TabFocusReason);
    queueView->setProperty("contentY", 10 * rowHeight);
    QTRY_VERIFY(rowAt(queueView, 10) != nullptr);
    tabInto(queueView);
    QTRY_COMPARE(ringedRows(queueView), QList<int>{10});
    QVERIFY(ringedAlone(queueView, 10));
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    QCOMPARE(queue->property("selectedIndices").toList().size(), 0);
    // Enter plays the ringed row.
    QJSValue played = engine.newObject();
    queue->setProperty("playAt", QVariant::fromValue(
        engine.evaluate(QStringLiteral("(function(played) { return function(index) { played.index = index } })"))
            .call({played})));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(played.property(QStringLiteral("index")).toInt(), 10);
    // The playing row, when it shows,
    bridge->setProperty("playingQueueIndex", 14);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    queue->setProperty("focusRowIndex", -1);
    tabInto(queueView);
    QTRY_COMPARE(ringedRows(queueView), QList<int>{14});
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    // but not one off the view: the queue does not move to it.
    bridge->setProperty("playingQueueIndex", 2);
    queue->setProperty("focusRowIndex", -1);
    tabInto(queueView);
    QTRY_COMPARE(ringedRows(queueView), QList<int>{10});
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    QCOMPARE(queue->property("selectedIndices").toList().size(), 0);
    // Down goes on from it, selecting the next row.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(queue->property("selectedIndices").toList(), QVariantList{11});
    QTRY_COMPARE(ringedRows(queueView), QList<int>{11});
}

// The current-row ring shows where the keyboard is, so it shows only
// while the keyboard is in use: not at launch, where the library has the
// focus, and not after a click, which selects with the tint alone. Tab or a
// key press brings it back.
void UiMainWindowTest::listsRingTheCurrentRowOnlyForTheKeyboard() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, kAllExpanded);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    bridge->setProperty("queueRows", sampleQueueRows(5));
    bridge->setProperty("queueLength", 5);
    QQuickItem *libraryView = named(window, "libraryView");
    QQuickItem *queueView = named(window, "playlistView");
    QVERIFY(libraryView && queueView);
    QTRY_COMPARE(queueView->property("count").toInt(), 5);
    const auto clickRow = [window](QQuickItem *view, int index, const char *cell) {
        QQuickItem *row = viewRowAt(view, index);
        QVERIFY(row != nullptr);
        QQuickItem *target = itemNamed(row, QString::fromLatin1(cell));
        QVERIFY(target != nullptr);
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(target).center().toPoint());
    };

    // At launch: the library has the focus and a current row, but no ring.
    QTRY_VERIFY(libraryView->hasActiveFocus());
    QCOMPARE(ringedRowsOf(libraryView), QList<int>{});
    QCOMPARE(ringedRowsOf(queueView), QList<int>{});

    // A click selects with the tint alone.
    clickRow(libraryView, 1, "libraryRowTitle");
    QTRY_VERIFY(rowTinted(libraryView, 1));
    QVERIFY(libraryView->hasActiveFocus());
    QCOMPARE(ringedRowsOf(libraryView), QList<int>{});
    // A key in the list rings the row it moves to.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{2});
    // A modifier alone, as for a Ctrl+click, is not the keyboard taking over.
    clickRow(libraryView, 1, "libraryRowTitle");
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{});
    QTest::keyClick(window, Qt::Key_Control);
    QCOMPARE(ringedRowsOf(libraryView), QList<int>{});

    // Tab into the queue: its current row is ringed, and the library's
    // ring is gone with the focus.
    queueView->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(queueView->hasActiveFocus());
    QTRY_COMPARE(ringedRowsOf(queueView), QList<int>{0});
    QCOMPARE(ringedRowsOf(libraryView), QList<int>{});
    // A click on a queue row selects it with the tint alone.
    clickRow(queueView, 3, "queueCell_title");
    QTRY_VERIFY(rowTinted(queueView, 3));
    QCOMPARE(ringedRowsOf(queueView), QList<int>{});
    // Backtab to the library: its selected row is ringed again.
    libraryView->forceActiveFocus(Qt::BacktabFocusReason);
    QTRY_VERIFY(libraryView->hasActiveFocus());
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{1});
    QVERIFY(rowTinted(libraryView, 1));
}

void UiMainWindowTest::unreadableTracksAreMarkedFromTheIssueHistory() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(errors != nullptr);
    showTree(libraryModel, kAllExpanded);
    QVariantList rows = sampleQueueRows(2);
    QVariantMap second = rows.at(1).toMap();
    second.insert(QStringLiteral("path"), kTrackPath);
    rows[1] = second;
    bridge->setProperty("queueRows", rows);
    bridge->setProperty("queueLength", 2);
    QQuickItem *track = nullptr;
    QTRY_VERIFY((track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    const auto unreadablePaths = [errors]() { return errors->property("unreadablePaths").toMap(); };

    // A warning, or an error about another kind of thing, marks nothing.
    // Category 1 is playback, 2 the library; severity 0 a warning, 1 an error.
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, kTrackPath), Q_ARG(QVariant, QStringLiteral("Slow"))));
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 2), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath), Q_ARG(QVariant, QStringLiteral("Scan"))));
    QVERIFY(unreadablePaths().isEmpty());
    // A playback error about the file marks its rows: a neutral warning
    // glyph, and the title in the secondary colour.
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath),
                                      Q_ARG(QVariant, QStringLiteral("The file cannot be read."))));
    QCOMPARE(unreadablePaths().value(kTrackPath).toString(), QStringLiteral("The file cannot be read."));
    QQuickItem *treeGlyph = itemNamed(track, QStringLiteral("libraryRowState"));
    QTRY_VERIFY(shown(treeGlyph));
    QCOMPARE(treeGlyph->property("source").toString(), QStringLiteral("dialog-warning"));
    QCOMPARE(colorOf(treeGlyph), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(itemNamed(track, QStringLiteral("libraryRowTitle"))), tokenHex(window, "textSecondary"));
    QCOMPARE(QQmlProperty(track, QStringLiteral("Accessible.description"), qmlContext(track)).read().toString(),
             QStringLiteral("Track, The file cannot be read."));
    QQuickItem *queueRow = nullptr;
    QTRY_VERIFY(([&]() {
        for (QQuickItem *row : itemsNamed(named(window, "playlistView"), QStringLiteral("queueRow"))) {
            if (row->property("queueRowIndex").toInt() == 1) {
                queueRow = row;
            }
        }
        return queueRow != nullptr;
    })());
    QQuickItem *queueGlyph = itemNamed(queueRow, QStringLiteral("queueRowState"));
    QTRY_VERIFY(shown(queueGlyph));
    QCOMPARE(queueGlyph->property("source").toString(), QStringLiteral("dialog-warning"));
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_title"))), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_artist"))), tokenHex(window, "textSecondary"));
    QVERIFY(QQmlProperty(queueRow, QStringLiteral("Accessible.name"), qmlContext(queueRow))
                .read().toString().endsWith(QStringLiteral(", The file cannot be read.")));
    // Clearing the history clears the marks.
    QVERIFY(QMetaObject::invokeMethod(errors, "clear"));
    QTRY_VERIFY(!treeGlyph->isVisible());
    QTRY_VERIFY(!queueGlyph->isVisible());
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_artist"))), tokenHex(window, "textColumn"));
}

void UiMainWindowTest::queueHeaderLabelsOnlyArtistAlbumAndLength() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *palette = paletteOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QQuickItem *queueHeader = named(window, "queueHeader");
    QVERIFY(queueHeader != nullptr);
    QCOMPARE(queueHeader->height(), palette->property("rowHeight").toReal());

    // The region's name sits where the Title label would be.
    QQuickItem *title = itemNamed(queueHeader, QStringLiteral("queueHeader_title"));
    QQuickItem *name = itemNamed(queueHeader, QStringLiteral("regionHeaderTitle"));
    QVERIFY(title && name);
    QTRY_COMPARE(name->mapToScene(QPointF()).x(), title->mapToScene(QPointF()).x());
    // The glyph and number columns have no label.
    for (const char *column : {"queueHeader_indicator", "queueHeader_order", "queueHeader_title"}) {
        QQuickItem *header = itemNamed(queueHeader, QString::fromLatin1(column));
        QVERIFY(header != nullptr);
        QVERIFY2(!header->property("text").isValid(), column);
        QVERIFY2(header->childItems().isEmpty(), column);
    }
    // Artist, Album and Length: small labels in the secondary colour, Length
    // right-aligned 24 px (at the default font) from the queue's right edge.
    QQuickItem *queue = named(window, "mainQueuePane");
    for (const char *column : {"queueHeader_artist", "queueHeader_album", "queueHeader_length"}) {
        QQuickItem *label = itemNamed(queueHeader, QString::fromLatin1(column));
        QVERIFY2(shown(label), column);
        QCOMPARE(colorOf(label), tokenHex(window, "textSecondary"));
        QCOMPARE(label->property("font").value<QFont>().pointSizeF(),
                 palette->property("smallPointSize").toReal());
    }
    QQuickItem *length = itemNamed(queueHeader, QStringLiteral("queueHeader_length"));
    QCOMPARE(length->property("horizontalAlignment").toInt(), int(Qt::AlignRight));
    QCOMPARE(sceneRect(queue).right() - sceneRect(length).right(), queue->property("rightPadding").toReal());
    QCOMPARE(queue->property("rightPadding").toReal(),
             std::round(24 * palette->property("rowHeight").toReal() / 26));
    // The resize handles show no line until the pointer or the keyboard
    // finds them.
    QQuickItem *handle = named(window, "queueColumnHandle_album");
    QVERIFY(shown(handle));
    QQuickItem *line = itemNamed(handle, QStringLiteral("queueColumnHandleLine"));
    QVERIFY(line != nullptr && !line->isVisible());
    handle->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(line->isVisible());
    QCOMPARE(colorOf(line), tokenHex(window, "focusRing"));
    // With focus, a 2 px line the header's full height, not a short tick
    // (#373 H6).
    QCOMPARE(line->width(), 2.0);
    QCOMPARE(sceneRect(line).top(), sceneRect(queueHeader).top());
    QCOMPARE(sceneRect(line).height(), sceneRect(queueHeader).height());
    QCOMPARE(sceneRect(line).height(), sceneRect(handle).height());
}

void UiMainWindowTest::narrowQueueDropsAlbumBeforeTitle() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    // As narrow as the window goes; with the default 10 pt font a 1000 px
    // window is narrow enough.
    window->resize(window->minimumWidth(), 720);
    QTRY_COMPARE(window->width(), window->minimumWidth());
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *album = named(window, "queueHeader_album");
    QQuickItem *albumHandle = named(window, "queueColumnHandle_album");
    QQuickItem *title = named(window, "queueHeader_title");
    QVERIFY(queue && album && albumHandle && title);
    QTRY_VERIFY(!album->isVisible());
    QVERIFY(!albumHandle->isVisible());
    QVERIFY(!albumHandle->activeFocusOnTab());
    QVERIFY(title->width() >= queue->property("minimumTitleColumnWidth").toReal() - 1);
    // The minimum stays where it was before Artist's default share grew
    // (#373 H6): it is counted with Artist at 26% of the width.
    const qreal minimumShare = queue->property("minimumArtistColumnShare").toReal();
    QCOMPARE(minimumShare, 0.26);
    QVERIFY(minimumShare < queue->property("defaultArtistColumnShare").toReal());
    QVERIFY(std::abs(queue->property("minimumWithArtistWidth").toReal()
                     - (queue->property("minimumTitleOnlyWidth").toReal()
                        - queue->property("minimumLengthColumnWidth").toReal()
                        + queue->property("defaultLengthColumnWidth").toReal())
                         / (1 - minimumShare)) < 0.01);
    // Artist has what the title's minimum leaves, about a quarter of the
    // text columns' room: the share the window's minimum width is counted
    // with, under its default, or sixteen characters where the title gives
    // them way (#373 H8).
    QQuickItem *artist = named(window, "queueHeader_artist");
    const qreal textRoom = queue->property("defaultTextColumnsRoom").toReal();
    QVERIFY2(artist->width() >= textRoom * queue->property("minimumArtistColumnShare").toReal() - 1
                 && artist->width() <= std::max(textRoom * queue->property("defaultArtistColumnShare").toReal(),
                                                queue->property("minimumArtistColumnWidth").toReal()) + 1,
             qPrintable(QStringLiteral("artist %1 of %2").arg(artist->width()).arg(textRoom)));
    // Wider, Album comes back.
    window->resize(1600, 1000);
    QTRY_VERIFY(album->isVisible());
    QVERIFY(albumHandle->isVisible());
}

// The queue's columns at 1600 and 1000 px (#373 H5): Length has a fixed
// room from the font, about 56 px; of what is left for the text, the
// title has a little under half, Artist a little over a quarter and Album
// a little less (#373 H6: half the room left a gap after short titles, and
// Artist about 24 px narrower at 1600 px). In the 1000 px window Album
// drops before Artist, and Length keeps its room.
void UiMainWindowTest::queueColumnsGiveTheirSlackToTheTitle() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, 1600, 1000);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *title = named(window, "queueHeader_title");
    QQuickItem *artist = named(window, "queueHeader_artist");
    QQuickItem *album = named(window, "queueHeader_album");
    QQuickItem *length = named(window, "queueHeader_length");
    QVERIFY(queue && title && artist && album && length);
    QTRY_VERIFY(shown(album));
    QTRY_COMPARE(window->width(), 1600);

    const qreal lengthWidth = queue->property("defaultLengthColumnWidth").toReal();
    QVERIFY2(lengthWidth >= 48 && lengthWidth <= 64, qPrintable(QString::number(lengthWidth)));
    QCOMPARE(length->width(), lengthWidth);
    QVERIFY(lengthWidth >= queue->property("minimumLengthColumnWidth").toReal());
    const qreal gap = queue->property("columnGap").toReal();
    const qreal room = queue->width() - queue->property("reservedColumnsWidth").toReal() - 2 * gap;
    const QString widths = QStringLiteral("title %1, artist %2, album %3, length %4 of %5 (room %6)")
        .arg(title->width()).arg(artist->width()).arg(album->width()).arg(length->width())
        .arg(queue->width()).arg(room);
    QVERIFY2(std::abs(artist->width() - 0.283 * room) <= 1, qPrintable(widths));
    QVERIFY2(std::abs(album->width() - 0.263 * room) <= 1, qPrintable(widths));
    QVERIFY2(std::abs(title->width() - 0.454 * room) <= 2, qPrintable(widths));
    // About 24 px more each than half the room for the title gave them.
    QVERIFY2(artist->width() - 0.26 * room >= 20 && album->width() - 0.24 * room >= 20, qPrintable(widths));
    QVERIFY2(title->width() > artist->width() && title->width() > album->width(), qPrintable(widths));
    // Album ends a gap before Length, which ends at the queue's inset.
    QVERIFY2(std::abs(sceneRect(length).left() - sceneRect(album).right() - gap) <= 1, qPrintable(widths));

    // 1000 px: Album drops, Artist and Length stay, and the title has the
    // rest: more than its minimum.
    window->resize(1000, 720);
    QTRY_COMPARE(window->width(), 1000);
    QTRY_VERIFY2(!shown(album), qPrintable(QStringLiteral("queue %1, room %2, reserved %3, title %4, artist %5, album %6")
        .arg(queue->width()).arg(queue->property("defaultTextColumnsRoom").toReal())
        .arg(queue->property("reservedColumnsWidth").toReal()).arg(title->width()).arg(artist->width()).arg(album->width())));
    QVERIFY(shown(artist) && shown(length));
    QCOMPARE(length->width(), lengthWidth);
    QVERIFY2(title->width() >= queue->property("minimumTitleColumnWidth").toReal() - 1,
             qPrintable(QString::number(title->width())));
    QVERIFY2(title->width() > artist->width(), qPrintable(QString::number(title->width())));
    QVERIFY(sceneRect(length).right() <= sceneRect(queue).right() + 0.5);
}

// A narrow window at a large font, 1000 x 720 at 24 pt: the window's
// minimum stays within it, and the queue's columns fit the queue. Length
// keeps its measured width, the title its minimum, Album drops first, and
// nothing runs past the queue's right end. An empty queue's hint wraps
// within the queue, 24 px clear of its sides.
void UiMainWindowTest::narrowWindowAtALargeFontFitsTheQueue() {
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont large = applicationFont;
    large.setPointSizeF(24);
    QGuiApplication::setFont(large);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, 1000, 720);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QVERIFY2(window->minimumWidth() <= 1000, qPrintable(QString::number(window->minimumWidth())));
    QObject *bridge = bridgeOf(window);
    QQuickItem *queue = named(window, "mainQueuePane");
    QVERIFY(queue != nullptr);
    QTRY_VERIFY(sceneRect(queue).right() <= window->width() + 0.5);

    // The empty queue's hint.
    bridge->setProperty("libraryRootCount", 1);
    QQuickItem *hint = named(window, "queueEmptyMessage");
    QQuickItem *explanation = named(window, "queueEmptyExplanation");
    QVERIFY(hint && explanation);
    QTRY_VERIFY(shown(hint));
    const QRectF pane = sceneRect(queue);
    for (QQuickItem *item : {hint, named(window, "queueEmptyLabel"), explanation}) {
        const QRectF rect = sceneRect(item);
        QVERIFY2(rect.left() >= pane.left() + 24 - 0.5 && rect.right() <= pane.right() - 24 + 0.5,
                 qPrintable(QStringLiteral("%1: %2..%3 in %4..%5")
                                .arg(item->objectName())
                                .arg(rect.left()).arg(rect.right()).arg(pane.left()).arg(pane.right())));
    }
    QVERIFY2(explanation->property("lineCount").toInt() > 1,
             qPrintable(QString::number(explanation->property("lineCount").toInt())));

    // Fifty tracks: Title and Length fit; Album and Artist drop, as the
    // sidebar is wider at a large font (#373 H5), and the title keeps more
    // than its minimum.
    bridge->setProperty("queueRows", sampleQueueRows(50));
    bridge->setProperty("queueLength", 50);
    QQuickItem *view = named(window, "playlistView");
    QTRY_VERIFY(viewRowAt(view, 0) != nullptr);
    QTRY_VERIFY(!shown(named(window, "queueHeader_album")));
    QQuickItem *title = named(window, "queueHeader_title");
    QQuickItem *artist = named(window, "queueHeader_artist");
    QQuickItem *length = named(window, "queueHeader_length");
    QVERIFY(title && artist && length);
    QVERIFY(shown(length) && !shown(artist));
    QVERIFY2(title->width() >= queue->property("minimumTitleColumnWidth").toReal() - 1,
             qPrintable(QString::number(title->width())));
    QVERIFY(length->width() >= queue->property("minimumLengthColumnWidth").toReal() - 0.5);
    QVERIFY(sceneRect(length).left() >= sceneRect(title).right());
    QVERIFY2(sceneRect(length).right() <= pane.right() - 8 + 0.5,
             qPrintable(QStringLiteral("%1 > %2").arg(sceneRect(length).right()).arg(pane.right() - 8)));
    // The row's cells are where the header's columns are.
    QQuickItem *row = viewRowAt(view, 0);
    for (const char *cell : {"queueCell_title", "queueCell_length"}) {
        QQuickItem *item = itemNamed(row, QString::fromLatin1(cell));
        QVERIFY2(shown(item) && sceneRect(item).right() <= pane.right() - 8 + 0.5, cell);
    }
    QVERIFY(!shown(itemNamed(row, QStringLiteral("queueCell_album"))));
    QVERIFY(!shown(itemNamed(row, QStringLiteral("queueCell_artist"))));

    // At the window's minimum, the queue at its own: Length still fits.
    window->resize(window->minimumWidth(), 720);
    QTRY_COMPARE(window->width(), window->minimumWidth());
    QTRY_VERIFY(queue->width() <= queue->property("minimumContentWidth").toReal() + 0.5);
    const QRectF narrowPane = sceneRect(queue);
    QVERIFY(narrowPane.right() <= window->width() + 0.5);
    QVERIFY(shown(length));
    QVERIFY2(sceneRect(length).right() <= narrowPane.right() - 8 + 0.5,
             qPrintable(QStringLiteral("%1 > %2").arg(sceneRect(length).right()).arg(narrowPane.right() - 8)));
    QVERIFY2(title->width() >= queue->property("minimumTitleColumnWidth").toReal() - 1,
             qPrintable(QString::number(title->width())));
    // The window's minimum is the panes' and fits 1000 px beside the wider
    // sidebar (#373 H6): the queue's counts Length at its least width, as
    // Length gives up the two characters' room before it in a queue that
    // narrow, rather than its default.
    QVERIFY(length->width() >= queue->property("minimumLengthColumnWidth").toReal() - 0.5);
    QVERIFY(queue->property("minimumTitleOnlyWidth").toReal()
            < queue->property("minimumWidthLimit").toReal() + 0.5);
    const qreal panesMinimum = named(window, "sidebarPane")->property("minimumWidth").toReal()
        + queue->property("minimumContentWidth").toReal() + 1;
    QCOMPARE(window->minimumWidth(), static_cast<int>(std::ceil(panesMinimum)));
}

void UiMainWindowTest::queueKeepsArtistAtALargeFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::newRow("default font, 1600 by 1000") << 0.0 << 1600 << 1000;
    QTest::newRow("default font, 1000 by 720") << 0.0 << 1000 << 720;
    QTest::newRow("24 pt, 1600 by 1000") << 24.0 << 1600 << 1000;
    QTest::newRow("24 pt, 1000 by 720") << 24.0 << 1000 << 720;
}

// Artist has at least sixteen characters of the font by default where the
// title, given way down to its minimum, leaves them (#373 H8: at 24 pt in
// 1600 px it was cut to eleven beside a title column twice as wide as its
// titles). With the default font Artist's share is wider, and stays.
// In 1000 px at 24 pt the title at its minimum leaves Artist less than the
// least it is shown at, so Artist drops there and the title keeps its
// minimum.
void UiMainWindowTest::queueKeepsArtistAtALargeFont() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont large = applicationFont;
        large.setPointSizeF(pointSize);
        QGuiApplication::setFont(large);
    }
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, windowWidth, windowHeight);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal() >= 20, pointSize > 0);
    QObject *bridge = bridgeOf(window);
    QVariantList rows = sampleQueueRows(20);
    const QString artistName = QStringLiteral("The Quiet Meridian");
    for (QVariant &row : rows) {
        QVariantMap map = row.toMap();
        map.insert(QStringLiteral("artist"), artistName);
        row = map;
    }
    bridge->setProperty("queueRows", rows);
    bridge->setProperty("queueLength", rows.size());
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *title = named(window, "queueHeader_title");
    QQuickItem *artist = named(window, "queueHeader_artist");
    QVERIFY(queue && title && artist);
    window->resize(windowWidth, windowHeight);
    QTRY_COMPARE(window->width(), windowWidth);
    QTRY_VERIFY(sceneRect(queue).right() <= windowWidth + 0.5 && sceneRect(queue).right() >= windowWidth - 1);
    QTRY_VERIFY(sceneRect(named(window, "sidebarPane")).right() + 1 >= sceneRect(queue).left() - 0.5);
    const QFont font = paletteOf(window)->property("baseFont").value<QFont>();
    const qreal minimum = std::ceil(QFontMetricsF(font).averageCharWidth() * 16);
    QTRY_COMPARE(queue->property("minimumArtistColumnWidth").toReal(), minimum);
    const qreal titleMinimum = queue->property("minimumTitleColumnWidth").toReal();
    const qreal gap = queue->property("columnGap").toReal();
    const qreal reserved = queue->property("reservedColumnsWidth").toReal();
    const QString widths = QStringLiteral("queue %1, title %2 (least %3), artist %4 (least %5)")
        .arg(queue->width()).arg(title->width()).arg(titleMinimum).arg(artist->width()).arg(minimum);
    const bool roomForArtist = queue->width() - reserved - titleMinimum - gap
        >= queue->property("minimumShownTextColumnWidth").toReal();
    if (pointSize > 0 && windowWidth == 1000) {
        // 24 pt in 1000 px: no room beside the title's minimum.
        QVERIFY2(!roomForArtist, qPrintable(widths));
        QTRY_VERIFY2(!shown(artist), qPrintable(widths));
        QVERIFY2(title->width() >= titleMinimum - 1, qPrintable(widths));
        return;
    }
    QVERIFY2(roomForArtist, qPrintable(widths));
    QTRY_VERIFY2(shown(artist), qPrintable(widths));
    QTRY_VERIFY2(artist->width() >= minimum - 1, qPrintable(widths));
    QVERIFY2(title->width() >= titleMinimum - 1, qPrintable(widths));
    if (pointSize > 0) {
        // The title gave way, and the name shows whole.
        QVERIFY2(artist->width() > queue->property("defaultTextColumnsRoom").toReal()
                     * queue->property("defaultArtistColumnShare").toReal() + 1,
                 qPrintable(widths));
        QQuickItem *view = named(window, "playlistView");
        QTRY_VERIFY(viewRowAt(view, 0) != nullptr);
        QQuickItem *cell = itemNamed(viewRowAt(view, 0), QStringLiteral("queueCell_artist"));
        QVERIFY(shown(cell));
        QTRY_COMPARE(cell->property("text").toString(), artistName);
        QVERIFY2(!cell->property("truncated").toBool(), qPrintable(widths));
    } else {
        // With the default font, its share, as before.
        QVERIFY2(std::abs(artist->width() - queue->property("defaultTextColumnsRoom").toReal()
                                                * queue->property("defaultArtistColumnShare").toReal())
                     <= 1,
                 qPrintable(widths));
    }
}

void UiMainWindowTest::visualizationTakesNearlyHalfTheRightColumn_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::newRow("default font, 1600 by 1000") << 0.0 << 1600 << 1000;
    QTest::newRow("default font, 1000 by 720") << 0.0 << 1000 << 720;
    QTest::newRow("24 pt, 1600 by 1000") << 24.0 << 1600 << 1000;
    QTest::newRow("24 pt, 1000 by 720") << 24.0 << 1000 << 720;
}

// Before the user drags the split, the visualization takes 47% of the right
// column (#373 H8: at 42% the pane read as a footer under a playlist): 423
// px at 1600 by 1000, where the queue keeps 16 rows and more with the
// default font. The queue keeps its header and six rows first, and each
// pane its minimum, the queue's its header and four rows: at 24 pt in a
// 720 px window the visualization has its minimum and the queue the rest,
// and the window's least height still fits 720 px. A split the user
// dragged wins.
void UiMainWindowTest::visualizationTakesNearlyHalfTheRightColumn() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont large = applicationFont;
        large.setPointSizeF(pointSize);
        QGuiApplication::setFont(large);
    }
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel, windowWidth, windowHeight);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    window->resize(windowWidth, windowHeight);
    QTRY_COMPARE(window->height(), windowHeight);
    QVERIFY2(window->minimumHeight() <= 720, qPrintable(QString::number(window->minimumHeight())));
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", sampleQueueRows(50));
    bridge->setProperty("queueLength", 50);
    QQuickItem *split = named(window, "rightSplit");
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *pane = named(window, "spectrogramPane");
    QQuickItem *header = named(window, "queueHeader");
    QVERIFY(split && queue && pane && header);
    const qreal rowHeight = paletteOf(window)->property("rowHeight").toReal();
    // Laid out for the window's height: the split down to the status bar.
    QTRY_VERIFY(sceneRect(split).bottom() <= windowHeight - 10 && sceneRect(split).bottom() >= windowHeight - 80
                && std::abs(queue->height() + pane->height() + 1 - split->height()) <= 1);
    const qreal queueMinimum = std::max<qreal>(220, 5 * rowHeight);
    const QString heights = QStringLiteral("split %1, queue %2, visualization %3, row %4")
        .arg(split->height()).arg(queue->height()).arg(pane->height()).arg(rowHeight);
    QCOMPARE(QQmlProperty(queue, QStringLiteral("SplitView.minimumHeight"), qmlContext(queue)).read().toReal(),
             queueMinimum);
    QVERIFY2(queue->height() >= queueMinimum - 0.5 && pane->height() >= 220 - 0.5, qPrintable(heights));
    const qreal share = pane->height() / split->height();
    if (pointSize > 0 && windowHeight == 720) {
        // The visualization at its minimum; the queue the rest.
        QVERIFY2(std::abs(pane->height() - 220) <= 1, qPrintable(heights));
    } else {
        QVERIFY2(std::abs(pane->height() - std::round(split->height() * 0.47)) <= 1.5 || share < 0.47,
                 qPrintable(heights));
        // The queue's header and six rows at least.
        QVERIFY2(queue->height() >= 7 * rowHeight - 0.5, qPrintable(heights));
    }
    if (pointSize == 0 && windowHeight == 1000) {
        QVERIFY2(share >= 0.45 && share <= 0.48, qPrintable(heights));
        // 16 whole rows under the header, and more.
        QVERIFY2(std::floor((queue->height() - header->height()) / rowHeight) >= 16, qPrintable(heights));
    }

    // A split the user dragged, as the settings keep it, wins.
    QObject *layout = window->findChild<QObject *>(QStringLiteral("windowLayoutController"));
    QVERIFY(layout != nullptr);
    const qreal dragged = std::round(split->height() * 0.7);
    QVERIFY(QMetaObject::invokeMethod(layout, "noteQueueHeight", Q_ARG(QVariant, dragged),
                                      Q_ARG(QVariant, split->height())));
    // As a share of the split.
    const auto expected = [&]() { return std::min(split->height() * 0.7, split->height() - 220 - 1); };
    QTRY_VERIFY2(std::abs(queue->height() - expected()) <= 1,
                 qPrintable(QStringLiteral("%1, not %2").arg(queue->height()).arg(expected())));
}

void UiMainWindowTest::firstRunShowsOneMessage() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QCOMPARE(bridge->property("libraryRootCount").toInt(), 0);
    QCOMPARE(bridge->property("queueLength").toInt(), 0);

    // One message, the queue's: the app icon, the title, two lines and the
    // button that adds a library folder.
    QQuickItem *message = named(window, "queueFirstRunMessage");
    QVERIFY(shown(message));
    QQuickItem *icon = named(window, "queueFirstRunIcon");
    QCOMPARE(icon->width(), 48.0);
    // The app icon's own 48 px drawing, built in, so it shows whether or not
    // the icon is installed.
    QCOMPARE(icon->property("source").toUrl(), QUrl(QStringLiteral("qrc:/icons/hicolor/48x48/apps/ferrous.svg")));
    QTRY_VERIFY(icon->property("valid").toBool());
    QQuickItem *title = named(window, "queueFirstRunTitle");
    QCOMPARE(title->property("text").toString(), QStringLiteral("Start with your music folder"));
    QCOMPARE(title->property("font").value<QFont>().weight(), QFont::DemiBold);
    QQuickItem *explanation = named(window, "queueFirstRunExplanation");
    QCOMPARE(explanation->property("text").toString().count(QLatin1Char('\n')), 1);
    QCOMPARE(colorOf(explanation), tokenHex(window, "textSecondary"));
    QQuickItem *button = named(window, "firstRunAddFolderButton");
    QVERIFY(shown(button));
    QCOMPARE(qvariant_cast<QObject *>(button->property("action")),
             window->findChild<QObject *>(QStringLiteral("addLibraryFolderAction")));
    // Centred in the queue.
    QQuickItem *queue = named(window, "playlistView");
    QVERIFY(std::abs(sceneRect(message).center().x() - sceneRect(queue).center().x()) <= 1);
    // No other message: the library shows its header with nothing under it,
    // and the queue no empty line; the pane, before any audio, is its
    // mode's floor, the spectrogram's black.
    QVERIFY(!named(window, "libraryEmptyStateLabel")->isVisible());
    QVERIFY(!named(window, "queueEmptyLabel")->isVisible());
    QVERIFY(shown(named(window, "libraryHeader")));
    QCOMPARE(itemNamed(named(window, "libraryHeader"), QStringLiteral("regionHeaderTotals"))
                 ->property("text").toString(),
             QString());
    QCOMPARE(itemNamed(named(window, "queueHeader"), QStringLiteral("regionHeaderTotals"))
                 ->property("text").toString(),
             QString());
    QCOMPARE(colorOf(named(window, "spectrogramPane")), QStringLiteral("#ff000000"));
    QVERIFY(!shown(named(window, "plotAnalysingText")));
    QVERIFY(named(window, "nowPlayingPanel")->property("idle").toBool());
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    // The empty queue's header shows only its name, and Tab skips its
    // resize handles.
    for (const char *column : {"queueHeader_artist", "queueHeader_album", "queueHeader_length"}) {
        QCOMPARE(named(window, column)->opacity(), 0.0);
    }
    QVERIFY(!named(window, "queueColumnHandle_artist")->activeFocusOnTab());

    // With a library folder, the first run is over: the library's quiet
    // line, and the queue's two-line hint, centred as the first-run
    // message is, in its type: what is empty, then what fills it.
    bridge->setProperty("libraryRootCount", 1);
    QTRY_VERIFY(!message->isVisible());
    QQuickItem *queueEmptyMessage = named(window, "queueEmptyMessage");
    QQuickItem *queueEmpty = named(window, "queueEmptyLabel");
    QQuickItem *queueEmptyExplanation = named(window, "queueEmptyExplanation");
    QTRY_VERIFY(shown(queueEmpty));
    QVERIFY(shown(queueEmptyExplanation));
    QCOMPARE(queueEmpty->property("text").toString(), QStringLiteral("The queue is empty"));
    QCOMPARE(queueEmptyExplanation->property("text").toString(),
             QStringLiteral("Double-click music in the library to play it, or drag it here."));
    QCOMPARE(queueEmpty->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(queueEmpty->property("font").value<QFont>().pointSizeF(),
             title->property("font").value<QFont>().pointSizeF());
    QCOMPARE(colorOf(queueEmpty), tokenHex(window, "text"));
    QCOMPARE(colorOf(queueEmptyExplanation), tokenHex(window, "textSecondary"));
    QCOMPARE(queueEmptyExplanation->property("font").value<QFont>().pointSizeF(),
             explanation->property("font").value<QFont>().pointSizeF());
    QVERIFY(sceneRect(queueEmptyExplanation).top() >= sceneRect(queueEmpty).bottom());
    QVERIFY(std::abs(sceneRect(queueEmptyMessage).center().x() - sceneRect(queue).center().x()) <= 1);
    QVERIFY(std::abs(sceneRect(queueEmptyMessage).center().y() - sceneRect(queue).center().y()) <= 1);
    // Each line on one line in a window of the default size.
    const QFontMetricsF explanationMetrics(queueEmptyExplanation->property("font").value<QFont>());
    QVERIFY(queueEmptyExplanation->height() < 2 * explanationMetrics.lineSpacing());
    QQuickItem *libraryEmpty = named(window, "libraryEmptyStateLabel");
    QTRY_VERIFY(shown(libraryEmpty));
    QCOMPARE(libraryEmpty->property("text").toString(), QStringLiteral("Loading library…"));
    // Once the backend has sent its (empty) tree.
    QObject *libraryController = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(QMetaObject::invokeMethod(libraryController, "requestTreeApply", Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, QByteArray())));
    QTRY_COMPARE(libraryEmpty->property("text").toString(),
                 QStringLiteral("No music found in the library folders"));
    // Queued tracks replace the line, and the columns get their labels.
    bridge->setProperty("queueRows", sampleQueueRows(1));
    bridge->setProperty("queueLength", 1);
    QTRY_VERIFY(!queueEmpty->isVisible());
    QCOMPARE(named(window, "queueHeader_artist")->opacity(), 1.0);
    QVERIFY(named(window, "queueColumnHandle_artist")->activeFocusOnTab());
}

// Tab goes from the library to the queue's rows: the column handles over
// the queue's header come after them in the Tab chain, not before, and
// still take the keyboard there.
void UiMainWindowTest::tabGoesFromTheLibraryToTheQueueRowsBeforeItsHandles() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, kAllExpanded);
    bridge->setProperty("queueRows", sampleQueueRows(3));
    bridge->setProperty("queueLength", 3);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    QQuickItem *library = named(window, "libraryView");
    QQuickItem *queue = named(window, "playlistView");
    QQuickItem *artistHandle = named(window, "queueColumnHandle_artist");
    QVERIFY(library && queue && artistHandle);
    QTRY_VERIFY(artistHandle->activeFocusOnTab());

    library->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(library->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY2(queue->hasActiveFocus(),
                 qPrintable(QStringLiteral("Tab went to %1")
                                .arg(window->activeFocusItem() ? window->activeFocusItem()->objectName()
                                                               : QStringLiteral("nothing"))));
    // Then the handles, in the columns' order, which Left and Right still
    // move.
    for (const char *column : {"queueColumnHandle_artist", "queueColumnHandle_album", "queueColumnHandle_length"}) {
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY2(named(window, column)->hasActiveFocus(), column);
    }
    // And Shift+Tab goes back from the rows to the library.
    queue->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(queue->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY(library->hasActiveFocus());
}

// The transport's first button starts at the content's 12 px inset, where
// the library's chevrons and the status bar's text do, and its volume ends
// the same 12 px from the right edge.
void UiMainWindowTest::transportStartsAndEndsAtTheContentInset() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, {});
    QQuickItem *artist = nullptr;
    QTRY_VERIFY((artist = libraryRow(window, QStringLiteral("artist"))) != nullptr);
    QQuickItem *previous = named(window, "transportButton_previous");
    QQuickItem *volume = named(window, "volumeSlider");
    QQuickItem *chevron = itemNamed(artist, QStringLiteral("libraryRowChevron"));
    QVERIFY(previous && volume && chevron);
    QTRY_COMPARE(sceneRect(previous).left(), 12.0);
    QCOMPARE(sceneRect(chevron).left(), sceneRect(previous).left());
    QCOMPARE(sceneRect(volume).right(), window->width() - 12.0);
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // The app's controls style and Breeze icons, whatever the desktop the
    // tests run in; TestSessionIsolation.h.
    isolateFromDesktopSessionWithAppLook();
    // Settings, data and caches of the test's own: no developer's colours,
    // fonts, layout or caches reach the tests.
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    QApplication app(argc, argv);
    UiMainWindowTest test;
    return QTest::qExec(&test, argc, argv);
}

// The viewers in the main window open and close with the controls style's
// own popup transition, as every popup does (instant with animations off),
// and show at once rather than after a delay.
void UiMainWindowTest::viewersInTheWindowFadeAsTheStylesPopupsDo() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());

    // The style's popup, for its transitions.
    QQmlComponent stylePopup(&engine);
    stylePopup.setData("import QtQuick.Controls\nPopup {}\n", QUrl());
    QScopedPointer<QObject> reference(stylePopup.create());
    QVERIFY2(reference != nullptr, qPrintable(stylePopup.errorString()));
    const auto fadeDuration = [](QObject *popup, const char *transition) {
        auto *object = qvariant_cast<QObject *>(popup->property(transition));
        if (object == nullptr) {
            return -1;
        }
        QQmlListReference animations(object, "animations");
        return animations.count() > 0 ? animations.at(0)->property("duration").toInt() : -1;
    };
    // A popup makes its transitions as it first opens and closes.
    reference->setProperty("parent", QVariant::fromValue(window->contentItem()));
    QVERIFY(QMetaObject::invokeMethod(reference.data(), "open"));
    QTRY_VERIFY(reference->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(reference.data(), "close"));
    QTRY_VERIFY(!reference->property("visible").toBool());
    const int enterDuration = fadeDuration(reference.data(), "enter");
    const int exitDuration = fadeDuration(reference.data(), "exit");
    QVERIFY(enterDuration >= 0 && exitDuration >= 0);

    const QList<std::pair<const char *, const char *>> viewers{
        {"openSpectrogramViewer", "spectrogramViewerPopup"},
        {"openAlbumArtViewerForCurrentArt", "albumArtViewerPopup"},
    };
    for (const auto &[open, popupName] : viewers) {
        QObject *popup = window->findChild<QObject *>(QLatin1String(popupName));
        QVERIFY2(popup != nullptr, popupName);

        // Shown in the same turn of the event loop as the request.
        QVERIFY(QMetaObject::invokeMethod(viewer, open));
        QCoreApplication::processEvents();
        QVERIFY2(popup->property("visible").toBool(), popupName);
        QTRY_VERIFY2(popup->property("opened").toBool(), popupName);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY2(!popup->property("visible").toBool(), popupName);
        QCOMPARE(fadeDuration(popup, "enter"), enterDuration);
        QCOMPARE(fadeDuration(popup, "exit"), exitDuration);
    }
}

#include "tst_ui_main_window.moc"
