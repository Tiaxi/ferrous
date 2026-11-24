// SPDX-License-Identifier: GPL-3.0-or-later

// The main window as one sheet (#373): the content on the view colour with
// four seams, the now-playing sidebar in its three layouts, the region
// headers of the library and the queue, the shared rows of the tree and the
// queue, the one-step library tree with its playing marks, the queue's
// columns, and the first run.

#include <QAccessible>
#include <QHash>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QJSValue>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlListReference>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QStyleHints>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QUrl>
#include <QtEndian>
#include <QtQuickTest/quicktest.h>
#include <QtTest/QtTest>
#include <qqml.h>

#include <algorithm>
#include <utility>
#include <cmath>
#include <functional>
#include <tuple>
#include <initializer_list>
#include <optional>

#include "../src/DiagnosticsLogView.h"
#include "../src/InputModality.h"
#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/SpectrogramItem.h"
#include "../src/StandardKeys.h"
#include "../src/WaveformEditorItem.h"
#include "../src/WaveformItem.h"
#include "../src/WindowPresence.h"
#include "TestColor.h"
#include "TestItems.h"
#include "TestMainWindow.h"
#include "TestMessages.h"
#include "TestMotion.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"
#include "TestShard.h"

namespace {

// A test's window size: the default window's, 1600 by 1000, unless given.
MainWindowOptions sized(int width = 1600, int height = 1000) {
    MainWindowOptions options;
    options.width = width;
    options.height = height;
    return options;
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

QString colorOf(const QObject *object) {
    return hex(object->property("color").value<QColor>());
}

QString tokenHex(QQuickWindow *window, const char *name) {
    return hex(tokenOf(window, name));
}

// The palette's scaledSize(size): a glyph's size at the window's font.
int scaledSize(QQuickWindow *window, int size) {
    QVariant scaled;
    QMetaObject::invokeMethod(paletteOf(window), "scaledSize", Q_RETURN_ARG(QVariant, scaled), Q_ARG(QVariant, size));
    return scaled.toInt();
}

// A state glyph's icon: a theme icon's name, or "status-error" or
// "status-warning" for the app's own error and warning marks, the status
// bar's.
QString stateGlyphName(QQuickItem *glyph) {
    const QString source = glyph->property("source").toString();
    for (const QString &own : {QStringLiteral("status-error"), QStringLiteral("status-warning")}) {
        if (source.endsWith(QStringLiteral("/assets/") + own + QStringLiteral(".svg"))) {
            return own;
        }
    }
    return source;
}

// Where the now-playing cover is: at the panel's corner, and in the
// large-font header at the 12 px inset, on its lines' left edge.
QPointF coverCorner(QQuickItem *panel) {
    const qreal inset = panel->property("largeHeader").toBool() ? 12.0 : 0.0;
    return panel->mapToScene(QPointF(inset, inset));
}

// Whether `item` and every item above it are visible.
bool shown(QQuickItem *item) {
    return item != nullptr && item->isVisible();
}

// The opacity `item` shows at within `top`: its own and its ancestors' up
// to it.
qreal opacityOf(QQuickItem *item, QQuickItem *top) {
    qreal opacity = 1.0;
    for (QQuickItem *at = item; at != nullptr && at != top; at = at->parentItem()) {
        opacity *= at->opacity();
    }
    return opacity;
}

// The window as drawn, read at points of its own coordinates.
class Drawn {
public:
    explicit Drawn(QQuickWindow *window)
        : m_image(window->grabWindow().convertToFormat(QImage::Format_RGB32)),
          m_scale(static_cast<qreal>(m_image.width()) / window->width()) {}

    QColor at(const QPointF &point) const {
        return m_image.pixelColor(static_cast<int>(std::floor(point.x() * m_scale)),
                                  static_cast<int>(std::floor(point.y() * m_scale)));
    }
    QColor at(qreal x, qreal y) const { return at(QPointF(x, y)); }
    qreal scale() const { return m_scale; }

private:
    QImage m_image;
    qreal m_scale;
};

// The no-cover mark's one colour at a list's size: a quiet violet grey on
// the plot's floor in a dark theme (no-art-mark.svg), a darker indigo on
// the light tile in a light one (no-art-mark-light.svg).
const QColor kNoCoverMark = QColor::fromRgb(0x8c, 0x82, 0xa3);
const QColor kNoCoverMarkLight = QColor::fromRgb(0x57, 0x4a, 0x7a);

// Whether two drawn colours are the same, give or take the rounding of
// blending.
bool near(const QColor &a, const QColor &b) {
    return channelDistance(a, b) <= 2;
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

// Gives `view` the keyboard's focus as Tab would, from the transport.
void tabInto(QQuickWindow *window, QQuickItem *view) {
    named(window, "transportButton_playPause")->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(!view->hasActiveFocus());
    view->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(view->hasActiveFocus());
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

// A row of the library tree, in the bridge's tree format.
struct TreeRow {
    quint8 rowType;
    quint16 depth;
    quint32 trackNumber;
    quint32 childCount;
    qint32 year;
    QString title;
    QString key;
    QString artist;
    QString path;
    QString trackPath;
};

QByteArray treeOf(const QList<TreeRow> &rows) {
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const TreeRow &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, -1);
        appendLe<quint32>(out, row.trackNumber);
        appendLe<quint32>(out, row.childCount);
        appendLe<qint32>(out, row.year);
        appendUtf8(out, row.title);
        appendUtf8(out, row.key);
        appendUtf8(out, row.artist);
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
    const QString artist = QStringLiteral("Artist A");
    QList<TreeRow> rows{
        {1, 0, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, artist, artistPath, {}},
        {2, 1, 0, 1, 2005, albumTitle, kAlbumKey, artist, QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 2, 2, 0, 0, QStringLiteral("Second"), QStringLiteral("track|") + kTrackPath, artist, kTrackPath,
         kTrackPath},
    };
    // Under a library folder's row, as with more than one folder.
    if (!rootTitle.isEmpty()) {
        for (TreeRow &row : rows) {
            ++row.depth;
        }
        rows.prepend({0, 0, 0, 1, 0, rootTitle, kRootKey, artist, QStringLiteral("/music"), {}});
    }
    return treeOf(rows);
}

void showTree(LibraryTreeModel &model, const QStringList &expanded,
              const QString &artistPath = QStringLiteral("/music/Artist A"),
              const QString &albumTitle = QStringLiteral("Album A")) {
    model.setExpandedKeys(expanded);
    model.setLibraryTreeFromBinary(artistAlbumTrackTree(artistPath, albumTitle));
}

const QStringList kAllExpanded{kArtistKey, kAlbumKey};

// `count` artists, collapsed, each with one album, in the bridge's tree
// format: a tree longer than the library's view.
QByteArray manyArtistsTree(int count) {
    QList<TreeRow> rows;
    for (int index = 0; index < count; ++index) {
        const QString name = QStringLiteral("Artist %1").arg(index, 2, 10, QLatin1Char('0'));
        rows.push_back({1, 0, 0, 1, 0, name, QStringLiteral("artist|") + name, name, QStringLiteral("/music/") + name,
                        {}});
    }
    return treeOf(rows);
}

// How a collapsed row that holds the playing track shows it: its name amber
// and semibold, quieter while paused, and no glyph anywhere on the row,
// which read as a play button. Empty when it is so, else what is wrong.
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

void setQueue(QObject *bridge, const QVariantList &rows) {
    bridge->setProperty("queueRows", rows);
    bridge->setProperty("queueLength", static_cast<int>(rows.size()));
}

// A cover for the playing track, made once in a temporary directory of the
// test's own: the panel takes its wide layout only for a track with one.
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

QObject *panelAnimation(QQuickItem *panel, const char *name) {
    return qvariant_cast<QObject *>(panel->property(name));
}

bool running(QQuickItem *panel, const char *animation) {
    QObject *object = panelAnimation(panel, animation);
    return object != nullptr && object->property("running").toBool();
}

// Ends the now-playing panel's growth or cross-fade where one runs, as it
// would end, for checks of where it leaves the panel: tst_ui_motion has
// their motion.
void finishMotion(QQuickItem *panel) {
    for (const char *name : {"presenceAnimation", "handoverAnimation"}) {
        if (running(panel, name)) {
            QMetaObject::invokeMethod(panelAnimation(panel, name), "complete");
        }
    }
}

// Whether the now-playing panel shows the playing track's layout whole: the
// first track grows it from the compact row of nothing loaded, which this
// ends, and the sidebar has laid it out at its whole height. The growth's
// last step sets `presence` to 1 after the polish of its frame, so the
// panel's whole height reaches the sidebar's layout only at the next one:
// until then the panel, and the library under it, are where the last step
// put them, as at 456 px of 457.
bool grown(QQuickWindow *window) {
    QQuickItem *panel = named(window, "nowPlayingPanel");
    if (panel == nullptr) {
        return false;
    }
    if (running(panel, "presenceAnimation")) {
        QMetaObject::invokeMethod(panelAnimation(panel, "presenceAnimation"), "complete");
    }
    return QTest::qWaitFor([&]() {
        return panel->property("presence").toDouble() == 1.0 && !layoutPending(panel);
    }, 5000);
}

// Whether the now-playing panel has settled: its growth and cross-fade
// ended, and laid out where they leave it.
bool settled(QQuickItem *panel) {
    finishMotion(panel);
    return QTest::qWaitFor([&]() {
        return !running(panel, "presenceAnimation") && !running(panel, "handoverAnimation")
            && !layoutPending(panel);
    }, 5000);
}

// Why the now-playing panel is not the compact row of nothing loaded, or "":
// one list row's height and the seam, the library's header right under it;
// Ferrous's 16 px mark, larger with a larger font, at the 12 px inset, and
// "Nothing playing" where a library row's text starts, 6 px after it, that
// gap too larger with a larger font (a fixed 6 px pressed the label against
// the 32 px mark at 24 pt), both in the secondary text colour and centred in
// the row; no cover's square and none of a track's lines.
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
    // Drawn for its size: two traces at 20 px and under, three above.
    const QString drawing = glyphSize <= 20 ? QStringLiteral("/ferrous-mark-small-symbolic.svg")
                                            : QStringLiteral("/ferrous-mark-symbolic.svg");
    if (!glyph->property("source").toUrl().toString().endsWith(drawing)
        || !glyph->property("isMask").toBool() || colorOf(glyph) != tokenHex(window, "textSecondary")) {
        return QStringLiteral("glyph %1 in %2").arg(glyph->property("source").toString(), colorOf(glyph));
    }
    if (!shown(label) || label->property("text").toString() != QStringLiteral("Nothing playing")
        || colorOf(label) != tokenHex(window, "textSecondary")
        || label->property("font").value<QFont>().weight() != QFont::Normal
        || label->property("truncated").toBool() || label->implicitHeight() > label->height() + 0.5) {
        return QStringLiteral("label \"%1\" in %2").arg(label->property("text").toString(), colorOf(label));
    }
    const qreal fontScale = std::max(1.0, palette->property("basePointSize").toReal() / 10.0);
    const qreal gap = std::max<qreal>(6, std::round(6 * fontScale));
    if (!at(labelRect.left(), glyphRect.right() + gap) || labelRect.right() > block.right() - 12 + 0.5
        || !at(labelRect.center().y(), rowRect.center().y()) || labelRect.bottom() > block.bottom()) {
        return QStringLiteral("label at %1, %2").arg(labelRect.left() - block.left()).arg(labelRect.top() - block.top());
    }
    return {};
}

// Why the compact row's glyph, at 16 px, does not read as Ferrous's mark,
// or "" (the stock disc was unreadable at 16 px): as drawn at 1:1, two
// traces across its right half, the glide and its 3rd harmonic (three 1.5 px
// strokes read as a slanted hatch at 16 px), two runs of ink apart in each
// column with 2 clear rows or more between them, each rising to the right
// and fanning apart, solid where they are, and no ink outside its box.
QString idleGlyphProblem(QQuickWindow *window) {
    constexpr int traces = 2;
    QQuickItem *glyph = named(window, "nowPlayingIdleGlyph");
    if (glyph == nullptr) {
        return QStringLiteral("no glyph");
    }
    const Drawn drawn(window);
    if (drawn.scale() != 1.0) {
        return QStringLiteral("drawn at %1:1").arg(drawn.scale());
    }
    const QRect box = sceneRect(glyph).toRect();
    if (box.size() != QSize(16, 16)) {
        return QStringLiteral("a %1 by %2 px glyph").arg(box.width()).arg(box.height());
    }
    const QColor background = drawn.at(box.left() - 4, box.center().y());
    const auto difference = [&](int x, int y) {
        const QColor pixel = drawn.at(box.left() + x, box.top() + y);
        return std::abs(pixel.red() - background.red()) + std::abs(pixel.green() - background.green())
            + std::abs(pixel.blue() - background.blue());
    };
    // How much of its ink a pixel has, 0 to 1: from the row's background
    // to the glyph's strongest pixel. (Its colour is the item's, which
    // compactRowProblem checks: the offscreen platform draws a mask
    // unrecoloured.)
    int strongest = 1;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            strongest = std::max(strongest, difference(x, y));
        }
    }
    if (strongest < 30) {
        return QStringLiteral("no ink: %1").arg(strongest);
    }
    const auto coverage = [&](int x, int y) { return double(difference(x, y)) / strongest; };
    QString rows;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const double c = coverage(x, y);
            rows += c > 0.66 ? QLatin1Char('#') : c > 0.25 ? QLatin1Char('+') : QLatin1Char('.');
        }
        rows += QLatin1Char('\n');
    }
    // Each column of its right half: two runs of ink, with 2 clear rows or
    // more between them; their middles in each column.
    QList<QList<double>> middles;
    for (int x = 8; x < 16; ++x) {
        QList<double> runs;
        int start = -1;
        int end = -1;
        int leastClear = 16;
        for (int y = 0; y <= 16; ++y) {
            const bool on = y < 16 && coverage(x, y) > 0.25;
            if (on && start < 0) {
                if (end >= 0) {
                    leastClear = std::min(leastClear, y - end);
                }
                start = y;
            } else if (!on && start >= 0) {
                runs.push_back((start + y - 1) / 2.0);
                start = -1;
                end = y;
            }
        }
        if (runs.size() != traces || leastClear < 2) {
            return QStringLiteral("column %1:\n%2").arg(x).arg(rows);
        }
        middles.push_back(runs);
    }
    // Rising to the right, each trace, and fanning apart.
    for (int trace = 0; trace < traces; ++trace) {
        if (middles.last().at(trace) >= middles.first().at(trace)) {
            return QStringLiteral("trace %1 does not rise:\n%2").arg(trace).arg(rows);
        }
    }
    if (middles.last().at(traces - 1) - middles.last().at(0)
        <= middles.first().at(traces - 1) - middles.first().at(0) - 0.01) {
        return QStringLiteral("the traces do not fan apart:\n%1").arg(rows);
    }
    // Solid where the strokes are, not a haze.
    int solid = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            solid += coverage(x, y) > 0.8 ? 1 : 0;
        }
    }
    if (solid < 20) {
        return QStringLiteral("%1 solid pixels:\n%2").arg(solid).arg(rows);
    }
    // Ink within the box only: the row around it is the background.
    for (int x = -2; x < 18; ++x) {
        if (coverage(x, -2) >= 0.1 || coverage(x, 17) >= 0.1) {
            return QStringLiteral("ink outside the box:\n%1").arg(rows);
        }
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

// Why the wide now-playing block is not on the queue's row grid, or "":
// with the cover it is a whole number of the list's rows high, so that the
// seam at its foot is on a line of the queue's rows; its lines start 8 px
// under the cover, and what the grid leaves over is under them, at least
// 8 px and less than that and a row.
QString wideBlockProblem(QQuickWindow *window) {
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QQuickItem *title = named(window, "nowPlayingTitle");
    QQuickItem *details = named(window, "nowPlayingDetails");
    QQuickItem *header = named(window, "libraryHeader");
    if (!panel || !cover || !seam || !title || !details || !header) {
        return QStringLiteral("an item is missing");
    }
    if (panel->property("layoutMode").toString() != QStringLiteral("wide")
        || panel->property("presence").toReal() != 1.0) {
        return QStringLiteral("not the wide layout, whole");
    }
    const qreal rowHeight = paletteOf(window)->property("rowHeight").toReal();
    const qreal block = sceneRect(seam).top() - sceneRect(cover).bottom();
    const qreal rows = (cover->height() + block) / rowHeight;
    if (std::abs(rows - std::round(rows)) > 0.001) {
        return QStringLiteral("the cover and the %1 px block are %2 rows of %3 px").arg(block).arg(rows).arg(rowHeight);
    }
    if (std::abs(sceneRect(title).top() - sceneRect(cover).bottom() - 8.0) > 0.01) {
        return QStringLiteral("the title %1 px under the cover").arg(sceneRect(title).top() - sceneRect(cover).bottom());
    }
    // The details' row, kept empty for a track without them.
    if (!shown(details)) {
        return QStringLiteral("the details' row is not kept");
    }
    const qreal under = sceneRect(seam).top() - sceneRect(details).bottom();
    if (under < 8.0 - 0.01 || under >= 8.0 + rowHeight) {
        return QStringLiteral("%1 px under the lines").arg(under);
    }
    if (std::abs(sceneRect(seam).bottom() - sceneRect(header).top()) > 0.01
        || std::abs(sceneRect(seam).bottom() - sceneRect(panel).bottom()) > 0.01) {
        return QStringLiteral("the library is not right under the seam");
    }
    return {};
}

// Why the details line of the now-playing panel is not one text in one
// colour, the tertiary one, at full opacity, or "": also once a track
// change has faded part of it.
QString detailsColourProblem(QQuickWindow *window, QQuickItem *details) {
    QQuickItem *line = details->parentItem()->parentItem();
    int texts = 0;
    const std::function<QString(QQuickItem *)> visit = [&](QQuickItem *item) -> QString {
        if (!item->isVisible()) {
            return {};
        }
        if (item->metaObject()->indexOfProperty("lineCount") >= 0 && item->width() > 0
            && !item->property("text").toString().isEmpty()) {
            ++texts;
            if (colorOf(item) != tokenHex(window, "textTertiary") || opacityOf(item, line) != 1.0) {
                return QStringLiteral("a text in %1 at %2").arg(colorOf(item)).arg(opacityOf(item, line));
            }
        }
        for (QQuickItem *child : item->childItems()) {
            const QString problem = visit(child);
            if (!problem.isEmpty()) {
                return problem;
            }
        }
        return {};
    };
    const QString problem = visit(line);
    return !problem.isEmpty() ? problem : (texts == 1 ? QString() : QStringLiteral("%1 texts in the line").arg(texts));
}

// Why the details line of playTrack()'s track, of `genre`, is cut off, or
// "": where it has no room for all of its details it leaves out what
// matters least rather than be cut off (not "Classical · Track 2 o…"): the
// word "Track" after the genre, then the genre, keeping the album position,
// then the album's track total, then the year; the tooltip has the whole,
// as it has every line's that is cut off (`lines`). Under the wide
// cover the details have the year; the narrow lines have it with the
// album, and their details are "Genre · n of m" on every track, never with
// "Track" after the genre. The panel measures them with tabular figures,
// every digit as wide as the widest.
QString detailsCutProblem(QQuickItem *panel, QQuickItem *details, bool wide, const QString &genre,
                          const QList<QQuickItem *> &lines) {
    const QString year = wide ? QStringLiteral("2005 · ") : QString();
    const QString whole = year + genre + (wide ? QStringLiteral(" · Track 2 of 8") : QStringLiteral(" · 2 of 8"));
    QStringList candidates{whole, year + genre + QStringLiteral(" · 2 of 8"),
                           year + QStringLiteral("Track 2 of 8"), year + QStringLiteral("Track 2"),
                           QStringLiteral("Track 2")};
    candidates.removeDuplicates();
    const QString wholeText = panel->property(wide ? "detailsText" : "narrowDetailsText").toString();
    const QString shownText = details->property("text").toString();
    if (wholeText != whole) {
        return QStringLiteral("the details are \"%1\"").arg(wholeText);
    }
    if (details->property("elided").toBool() || !candidates.contains(shownText)) {
        return QStringLiteral("\"%1\" shows").arg(shownText);
    }
    // The longest of them that fits the line.
    const QFontMetricsF metrics(details->property("font").value<QFont>());
    QChar widest = QLatin1Char('0');
    for (char digit = '1'; digit <= '9'; ++digit) {
        if (metrics.horizontalAdvance(QLatin1Char(digit)) > metrics.horizontalAdvance(widest)) {
            widest = QLatin1Char(digit);
        }
    }
    const auto tabular = [&](QString text) {
        return metrics.horizontalAdvance(text.replace(QRegularExpression(QStringLiteral("[0-9]")), widest));
    };
    for (const QString &candidate : candidates) {
        if (candidate == shownText) {
            break;
        }
        if (tabular(candidate) <= details->width() - 1.0) {
            return QStringLiteral("\"%1\" shows, but \"%2\" fits").arg(shownText, candidate);
        }
    }
    if (tabular(shownText) > details->width() + 1.0) {
        return QStringLiteral("\"%1\" does not fit %2 px").arg(shownText).arg(details->width());
    }
    bool cut = shownText != whole;
    for (QQuickItem *line : lines) {
        cut = cut || line->property("elided").toBool();
    }
    if (panel->property("textTruncated").toBool() != cut) {
        return QStringLiteral("the tooltip's whole text %1").arg(cut ? "missing" : "for nothing cut off");
    }
    return {};
}

// A genre too long for the details line at any font size.
const QString kLongGenre = QStringLiteral("Contemporary Classical and Ambient Electronic Crossover");

// The lines the panel shows, top to bottom: the sidebar's own, wide or
// narrow, that have text.
QList<QQuickItem *> shownLines(QQuickWindow *window) {
    QList<QQuickItem *> lines;
    for (const char *name : {"nowPlayingTitle", "nowPlayingArtistAlbum", "nowPlayingDetails", "nowPlayingNarrowTitle",
                             "nowPlayingNarrowArtist", "nowPlayingNarrowAlbum", "nowPlayingNarrowDetails"}) {
        QQuickItem *line = named(window, name);
        if (shown(line) && !line->property("text").toString().isEmpty()) {
            lines.push_back(line);
        }
    }
    std::sort(lines.begin(), lines.end(),
              [](QQuickItem *a, QQuickItem *b) { return sceneRect(a).top() < sceneRect(b).top(); });
    return lines;
}

// Why the now-playing panel does not show the stand-in for a cover, or "":
// the cover's place as for a cover, whole, in the plot's floor colour,
// #14062e in both themes, with one static mark, the app icon's curves at
// its own aspect, not stretched to 2:1, from a quarter of the way in to the
// tile's right edge, the wide tile's of four curves (with the faint 4th
// harmonic, as the icon's 128 and 256 px drawings) and the small one's of
// two, and no text at all (the lines under it give the album); a 1 px edge
// in both themes on the sides where the tile meets the sidebar's view
// colour, as drawn (without it the small tiles' sides dissolved into the
// dark sidebar, and the wide dark tile had no foot over its title), and
// none on a side that meets the panel's top, the window's edge or a seam
// (all round, it doubled them: a frame inside the flush narrow tile, a 2 px
// seam under it, a double edge at the light wide tile's top and right):
// the wide tile's foot alone, the narrow tile's side facing the lines, and
// all four of the large-font header's inset tile. No disc glyph, and no
// cover. As drawn, the mark's middle third is not all floor.
QString standInProblem(QQuickWindow *window) {
    QObject *palette = paletteOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *area = named(window, "nowPlayingCoverArea");
    QQuickItem *cover = named(window, "nowPlayingCover");
    if (!panel || !area || !cover) {
        return QStringLiteral("an item is missing");
    }
    QQuickItem *standIn = itemNamed(cover, QStringLiteral("albumArtStandIn"));
    QQuickItem *mark = itemNamed(cover, QStringLiteral("albumArtStandInMark"));
    QQuickItem *edge = itemNamed(cover, QStringLiteral("albumArtStandInEdge"));
    if (!standIn || !mark || !edge) {
        return QStringLiteral("a part of the stand-in is missing");
    }
    // The tile has no disc mark of its own any more.
    if (itemNamed(cover, QStringLiteral("albumArtPlaceholderGlyph")) != nullptr) {
        return QStringLiteral("the tile still has its disc mark");
    }
    for (const char *name : {"albumArtStandInTitle", "albumArtStandInSubtitle"}) {
        if (itemNamed(cover, QString::fromLatin1(name)) != nullptr) {
            return QStringLiteral("the stand-in still has %1").arg(QString::fromLatin1(name));
        }
    }
    if (palette->property("displayPointSize").isValid()) {
        return QStringLiteral("the palette still has the stand-in title's size");
    }
    if (panel->property("standInTitle").isValid()) {
        return QStringLiteral("the panel still has a stand-in title");
    }
    if (panel->property("presence").toReal() != 1.0) {
        return QStringLiteral("still growing");
    }
    const bool wide = panel->property("layoutMode").toString() == QStringLiteral("wide");
    // The narrow layout's 96 px, or the compact one's.
    const qreal side = wide ? panel->width() : panel->property("narrowLayoutCoverSize").toReal();
    const qreal narrowSide = panel->property("largeHeader").toBool() ? panel->property("headerCoverSize").toReal()
        : panel->property("narrowBeside").toBool() ? panel->property("besideCoverSize").toReal() : 96.0;
    if (!wide && (side < 96.0 || side != narrowSide)) {
        return QStringLiteral("the narrow cover's side %1").arg(side);
    }
    // At the panel's corner, wide or narrow, but in the large-font header
    // at its inset.
    const QPointF corner = coverCorner(panel);
    if (!shown(area) || !shown(cover) || cover->opacity() != 1.0 || cover->width() != side
        || cover->height() != side || sceneRect(cover).topLeft() != corner) {
        return QStringLiteral("the cover's place %1 by %2 at %3").arg(cover->width()).arg(cover->height())
            .arg(cover->opacity());
    }
    if (colorOf(cover) != QStringLiteral("#ff14062e") || colorOf(cover) != tokenHex(window, "plotFloor")) {
        return QStringLiteral("the stand-in in %1").arg(colorOf(cover));
    }
    if (!shown(standIn) || cover->property("idle").toBool() || cover->property("hasCover").toBool()) {
        return QStringLiteral("the stand-in does not show, or the tile at rest or a cover does");
    }
    // The mark: its paths' square box over the whole tile, clipped to it;
    // the small tile's of three curves. Its paths run on past the tile's right
    // side, cut flush there (standInMarkRunsOut).
    const QRectF tile = sceneRect(cover);
    const QRectF markRect = sceneRect(mark);
    const QSizeF box = mark->property("box").toSizeF();
    if (box != QSizeF(1000, 1000)) {
        return QStringLiteral("the mark's paths in a %1 by %2 box").arg(box.width()).arg(box.height());
    }
    if (!shown(mark) || !mark->clip() || markRect != tile) {
        return QStringLiteral("the mark %1 by %2 at %3, %4").arg(markRect.width()).arg(markRect.height())
            .arg(markRect.x()).arg(markRect.y());
    }
    const bool small = cover->property("standInCompact").toBool();
    if (small != (side < 180)) {
        return QStringLiteral("a %1 px tile has the %2 mark").arg(side).arg(small ? "small" : "wide");
    }
    if (mark->property("status").toInt() != 1 /* Shape.Ready */) {
        return QStringLiteral("the mark has not drawn");
    }
    if (mark->opacity() != 1.0) {
        return QStringLiteral("the mark at %1").arg(mark->opacity());
    }
    // Its strokes in pixels, not scaled with the tile: on the wide mark a
    // trace of the window's precision, the glide 5 px and no trace under
    // 2.5 (the glide of 8.5 px was the window's loudest amber); on the small
    // mark, the 96 and 152 px tiles', 4, 3.5 and 3 px (strokes of 3 and
    // 2.5 px read as hairlines). The glide 1.3 times the stroke, the focal
    // one, as in the app icon (at the harmonics' taper the curves read as
    // near-equal stripes).
    const qreal base = small ? 4.0 : 3.9;
    const qreal glide = mark->property("glideStroke").toReal();
    if (std::abs(glide - 1.3 * base) > 0.01 || (!small && (glide < 4.8 || glide > 5.5))) {
        return QStringLiteral("the glide's stroke %1 px on a %2 px tile").arg(glide).arg(side);
    }
    const qreal high = mark->property("highHarmonicStroke").toReal();
    const qreal harmonic = mark->property("harmonicStroke").toReal();
    if (std::abs(harmonic - (small ? 3.5 : 0.875 * base)) > 0.01 || std::abs(high - (small ? 3.0 : 0.75 * base)) > 0.01
        || glide < 1.45 * harmonic) {
        return QStringLiteral("the harmonics %1 and %2 px beside the glide's %3").arg(mark->property("harmonicStroke").toReal())
            .arg(high).arg(glide);
    }
    // The wide mark's 4th harmonic the thinnest, and 2.5 px.
    const qreal fourth = mark->property("fourthHarmonicStroke").toReal();
    if (std::abs(fourth - 0.645 * base) > 0.01 || (!small && (fourth < 2.5 || fourth > high))) {
        return QStringLiteral("the 4th harmonic %1 px beside the glide's %2").arg(mark->property("fourthHarmonicStroke").toReal())
            .arg(glide);
    }
    // The colours as drawn on the floor: the amber at its value as at 68 %,
    // some 5.1:1, but an amber, its hue and saturation the app icon's
    // (#ab7a33, mixed toward the floor, read as ochre), the 3rd
    // harmonic's magenta 3.6 to 3.9:1 on the floor, and the 2nd harmonic's
    // rose a step lighter, between it and the amber, as in the app icon
    // (at one value the two harmonics read as one).
    const QColor floor(QStringLiteral("#14062e"));
    const QColor glideColour = cover->property("standInGlideColor").value<QColor>();
    if (hex(glideColour) != QStringLiteral("#ffb07a1a")) {
        return QStringLiteral("the glide in %1").arg(hex(glideColour));
    }
    const QColor iconAmber(QStringLiteral("#f2b035"));
    const QColor ochre(QStringLiteral("#ab7a33"));
    const double glideRatio = contrastRatio(glideColour, floor);
    if (std::abs(glideRatio - contrastRatio(ochre, floor)) > 0.1 || std::abs(glideColour.hsvHue() - iconAmber.hsvHue()) > 2
        || glideColour.hsvSaturationF() < iconAmber.hsvSaturationF() || ochre.hsvSaturationF() >= iconAmber.hsvSaturationF()) {
        return QStringLiteral("the glide at %1:1, hue %2, saturation %3").arg(glideRatio).arg(glideColour.hsvHue())
            .arg(glideColour.hsvSaturationF());
    }
    const double harmonicRatio = contrastRatio(cover->property("standInHarmonicColor").value<QColor>(), floor);
    const double highRatio = contrastRatio(cover->property("standInHighHarmonicColor").value<QColor>(), floor);
    if (highRatio < 3.6 || highRatio > 3.9) {
        return QStringLiteral("the 3rd harmonic at %1:1 on the floor").arg(highRatio);
    }
    if (harmonicRatio < highRatio + 0.4 || harmonicRatio > glideRatio - 0.4) {
        return QStringLiteral("the 2nd harmonic at %1:1 on the floor, between %2 and %3").arg(harmonicRatio).arg(highRatio)
            .arg(glideRatio);
    }
    // The 4th harmonic a dim violet, some 3.2:1, the dimmest trace, half a
    // step or more below the 3rd (2.9:1 read faint at 360 px).
    const double fourthRatio = contrastRatio(cover->property("standInFourthHarmonicColor").value<QColor>(), floor);
    if (fourthRatio < 3.1 || fourthRatio > 3.25 || fourthRatio > highRatio - 0.5) {
        return QStringLiteral("the 4th harmonic at %1:1 on the floor").arg(fourthRatio);
    }
    // A 1 px edge of plotFloorEdge on each side that meets the view, none
    // on the others, as drawn.
    const QColor edgeColour = tokenOf(window, "plotFloorEdge");
    if (edgeColour.alpha() != 255) {
        return QStringLiteral("the edge colour is %1").arg(tokenHex(window, "plotFloorEdge"));
    }
    QQuickItem *seam = named(window, "nowPlayingSeam");
    if (!shown(edge) || seam == nullptr) {
        return QStringLiteral("no edge");
    }
    const QRectF tileRect = sceneRect(cover);
    const QRectF panelRect = sceneRect(panel);
    const struct {
        const char *side;
        bool meetsView;
        QPointF inside;
        QRectF line;
    } sides[] = {
        {"top", tileRect.top() > panelRect.top() + 0.5, QPointF(tileRect.center().x(), tileRect.top() + 0.5),
         QRectF(tileRect.left(), tileRect.top(), tileRect.width(), 1)},
        {"left", tileRect.left() > panelRect.left() + 0.5, QPointF(tileRect.left() + 0.5, tileRect.center().y()),
         QRectF(tileRect.left(), tileRect.top(), 1, tileRect.height())},
        {"right", tileRect.right() < panelRect.right() - 0.5, QPointF(tileRect.right() - 0.5, tileRect.top() + 0.1 * tileRect.height()),
         QRectF(tileRect.right() - 1, tileRect.top(), 1, tileRect.height())},
        {"bottom", tileRect.bottom() < sceneRect(seam).top() - 0.5, QPointF(tileRect.center().x(), tileRect.bottom() - 0.5),
         QRectF(tileRect.left(), tileRect.bottom() - 1, tileRect.width(), 1)},
    };
    const QString layout = panel->property("layoutMode").toString()
        + (panel->property("largeHeader").toBool() ? QStringLiteral(" header") : QString());
    const Drawn drawn(window);
    for (const auto &side : sides) {
        QQuickItem *line = itemNamed(cover, QStringLiteral("albumArtStandInEdge_") + QLatin1String(side.side));
        if (line == nullptr || shown(line) != side.meetsView) {
            return QStringLiteral("the %1 edge %2 in the %3 layout").arg(QLatin1String(side.side))
                .arg(shown(line) ? QStringLiteral("shows") : QStringLiteral("is missing")).arg(layout);
        }
        if (side.meetsView
            && (sceneRect(line) != side.line || channelDistance(drawn.at(side.inside), edgeColour) > 3)) {
            return QStringLiteral("the %1 edge drawn %2 at %3,%4").arg(QLatin1String(side.side))
                .arg(hex(drawn.at(side.inside))).arg(sceneRect(line).x()).arg(sceneRect(line).y());
        }
    }
    // Under the mark: where a trace runs out through an edged side, the
    // side's pixel is not the edge's colour (drawn over the mark, the edge
    // capped the traces' ends on the right with a 1 px bar). Each pixel
    // along the side with a trace solid 1 px inside it; the narrow and
    // header tiles' right sides have some.
    const QColor floorColour = tokenOf(window, "plotFloor");
    int crossings = 0;
    for (const auto &side : sides) {
        if (!side.meetsView) {
            continue;
        }
        const bool across = side.line.width() > side.line.height();
        const QPointF inward = across ? QPointF(0, side.line.top() <= tileRect.top() + 0.5 ? 1 : -1)
                                      : QPointF(side.line.left() <= tileRect.left() + 0.5 ? 1 : -1, 0);
        const qreal length = across ? side.line.width() : side.line.height();
        for (qreal along = 2.5; along < length - 2; along += 1) {
            const QPointF onLine = side.line.topLeft() + (across ? QPointF(along, 0.5) : QPointF(0.5, along));
            const QColor inside = drawn.at(onLine + inward);
            if (channelDistance(inside, floorColour) < 80 || channelDistance(inside, edgeColour) < 80) {
                continue;
            }
            ++crossings;
            const QColor atEdge = drawn.at(onLine);
            if (channelDistance(atEdge, edgeColour) <= 8) {
                return QStringLiteral("the %1 edge %2 over the trace %3 at %4 in the %5 layout")
                    .arg(QLatin1String(side.side), hex(atEdge), hex(inside)).arg(along).arg(layout);
            }
        }
    }
    if (crossings == 0 && sides[2].meetsView) {
        return QStringLiteral("no trace runs out through the right side in the %1 layout").arg(layout);
    }
    // The wide tileRect edges its foot alone, in both themes.
    if (wide && (!shown(itemNamed(cover, QStringLiteral("albumArtStandInEdge_bottom")))
                 || shown(itemNamed(cover, QStringLiteral("albumArtStandInEdge_top"))))) {
        return QStringLiteral("the wide tileRect's edges");
    }
    return {};
}

// How much of the stand-in's mark is drawn: the share of the pixels of
// its rect, as grabbed, that are not the floor.
qreal standInMarkInk(QQuickWindow *window) {
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *mark = cover ? itemNamed(cover, QStringLiteral("albumArtStandInMark")) : nullptr;
    if (mark == nullptr) {
        return 0;
    }
    const QImage image = window->grabWindow();
    const qreal scale = static_cast<qreal>(image.width()) / window->width();
    const QRect rect = QRectF(sceneRect(mark).topLeft() * scale, sceneRect(mark).size() * scale).toRect();
    const QColor floor(QStringLiteral("#14062e"));
    int ink = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (std::max({std::abs(pixel.red() - floor.red()), std::abs(pixel.green() - floor.green()),
                          std::abs(pixel.blue() - floor.blue())}) > 24) {
                ++ink;
            }
        }
    }
    return static_cast<qreal>(ink) / std::max(1, rect.width() * rect.height());
}

// Why the stand-in's mark does not run out to the tile's right edge, cut
// flush there, as the app icon's traces run out to its tile's edge, or ""
// (ending in round caps inside the mark, the series read as a line chart):
// as grabbed, the column just inside the tile's right edge crosses each of
// every one of its curves, and they start some 24 % in: the columns before
// a fifth of the tile are floor, and a column at 30 % crosses them all, the
// wide mark's four or the small mark's three. None leaves through the top
// edge, so the crop reads as meant (the wide mark's 4th alone did): the row
// just inside it is floor.
QString standInMarkRunsOut(QQuickWindow *window) {
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *mark = cover ? itemNamed(cover, QStringLiteral("albumArtStandInMark")) : nullptr;
    if (mark == nullptr) {
        return QStringLiteral("no mark");
    }
    const QImage image = window->grabWindow();
    const qreal scale = static_cast<qreal>(image.width()) / window->width();
    const QRectF rect = sceneRect(cover);
    // Inside the light theme's 1 px edge.
    const int top = static_cast<int>(std::floor(rect.top() * scale)) + 2;
    const int bottom = static_cast<int>(std::ceil(rect.bottom() * scale)) - 2;
    const int left = static_cast<int>(std::floor(rect.left() * scale));
    const int right = static_cast<int>(std::ceil(rect.right() * scale));
    const int width = right - left;
    const QColor floor(QStringLiteral("#14062e"));
    const auto inked = [&](int x, int y) {
        const QColor pixel = image.pixelColor(x, y);
        return std::max({std::abs(pixel.red() - floor.red()), std::abs(pixel.green() - floor.green()),
                         std::abs(pixel.blue() - floor.blue())})
               > 24;
    };
    const auto runs = [&](int x) {
        int count = 0;
        bool in = false;
        for (int y = top; y < bottom; ++y) {
            const bool ink = inked(x, y);
            count += ink && !in ? 1 : 0;
            in = ink;
        }
        return count;
    };
    const bool small = cover->property("standInCompact").toBool();
    const int curves = small ? 3 : 4;
    if (runs(right - 2) != curves) {
        return QStringLiteral("%1 curves at the tile's right edge, not %2").arg(runs(right - 2)).arg(curves);
    }
    // The row just inside the top edge.
    int first = -1;
    int last = -1;
    int topRuns = 0;
    for (int x = left + 2; x < right - 2; ++x) {
        if (inked(x, top)) {
            topRuns += last == x - 1 ? 0 : 1;
            first = first < 0 ? x : first;
            last = x;
        }
    }
    if (topRuns != 0) {
        return QStringLiteral("%1 runs in the top row, from %2 to %3 of %4 px").arg(topRuns).arg(first - left).arg(last - left).arg(width);
    }
    for (int x = left + 2; x < left + width / 5; ++x) {
        if (runs(x) != 0) {
            return QStringLiteral("a curve %1 px into the tile").arg(x - left);
        }
    }
    if (runs(left + width * 3 / 10) != curves) {
        return QStringLiteral("%1 curves at 30 %% of the tile").arg(runs(left + width * 3 / 10));
    }
    return {};
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
    void seekTimeStaysOverItsLine_data();
    void seekTimeStaysOverItsLine();
    void seekTimeBoxIsCrispWithoutAShadow_data();
    void seekTimeBoxIsCrispWithoutAShadow();
    void transportRowSitsBalancedInItsBand_data();
    void transportRowSitsBalancedInItsBand();
    void contentIsOneSheetWithFourSeams();
    void sidebarWidthFollowsTheWindowAndTheFont_data();
    void sidebarWidthFollowsTheWindowAndTheFont();
    void nothingPlayingIsACompactRow_data();
    void nothingPlayingIsACompactRow();
    void nowPlayingHasItsWideNarrowAndEmptyLayouts();
    void nowPlayingWideBlockAtAnyFontSize_data();
    void nowPlayingWideBlockAtAnyFontSize();
    void nowPlayingNarrowShowsEveryLineAtAnyFontSize_data();
    void nowPlayingNarrowShowsEveryLineAtAnyFontSize();
    void nowPlayingNarrowBlockIsOneHeightForEveryTrack_data();
    void nowPlayingNarrowBlockIsOneHeightForEveryTrack();
    void nowPlayingCompactKeepsTheAlbumAndItsYear();
    void nowPlayingLargeFontCompactIsAHeaderRow_data();
    void nowPlayingLargeFontCompactIsAHeaderRow();
    void nowPlayingTitleIsOneSizePerLayout_data();
    void nowPlayingTitleIsOneSizePerLayout();
    void nowPlayingEllipsisFollowsTheTextWithoutASpace();
    void nowPlayingNarrowDetailsKeepTheAlbumPosition();
    void levelMeterAtRestShowsItsMarks_data();
    void levelMeterAtRestShowsItsMarks();
    void nowPlayingWithoutCoverOrTagsLeavesNoBlankLines_data();
    void nowPlayingWithoutCoverOrTagsLeavesNoBlankLines();
    void nowPlayingHoldsItsHeightOnEveryFrameOfACoverChange_data();
    void nowPlayingHoldsItsHeightOnEveryFrameOfACoverChange();
    void nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort_data();
    void nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort();
    void showInLibraryBringsThePlayingRowIntoView();
    void showInLibraryOpensTheCollapsedLibraryFolderOfTheTrack();
    void showInLibraryGivesWayToTheUserBrowsingTheLibrary();
    void showInLibraryLandsOnTheRowGrid_data();
    void showInLibraryLandsOnTheRowGrid();
    void nowPlayingShowsTheFileNameOfATrackWithoutTitleOrArtist();
    void nowPlayingCoverIsAKeyboardStop();
    void libraryFoldersAreFolderHeaders();
    void libraryFoldersAfterTheFirstHaveARuleOnTop();
    void libraryCollapseAllIsEnabledOnlyWithAnExpandedRow();
    void libraryCollapseAllLeavesNoRowDrawn();
    void regionHeadersNameTheRegionAndItsTotals();
    void libraryHeaderSaysItHasNoFolders_data();
    void libraryHeaderSaysItHasNoFolders();
    void libraryHeaderShortensToItsTrackTotal_data();
    void libraryHeaderShortensToItsTrackTotal();
    void selectedRowsDrawTheirMarksForTheSelection();
    void selectedQueueRowsNumberIsReadableAsDrawn_data();
    void selectedQueueRowsNumberIsReadableAsDrawn();
    void libraryTreeHasOneIndentStep();
    void libraryAlbumWithoutACoverShowsAPlaceholderTile();
    void libraryNoCoverMarkIsOneQuietColour_data();
    void libraryNoCoverMarkIsOneQuietColour();
    void libraryFocusRingStopsAtTheScrollBarLane();
    void libraryTreeChevronsGrowWithALargeFont();
    void libraryThumbnailsFollowTheFont_data();
    void libraryThumbnailsFollowTheFont();
    void libraryChevronsKeepBreezesStroke_data();
    void libraryChevronsKeepBreezesStroke();
    void libraryTreeMarksThePlayingTrackAndItsCollapsedAncestors();
    void collapsedRowNameElidesBeforeTheYear();
    void libraryRowShowsItsCutTitleWholeInItsToolTip();
    void collapsedArtistMarkFollowsTheBackendsGroupingNotItsPath();
    void libraryRowsShowHoverSelectionAndTheCurrentRow();
    void librarySelectedRowStepsUpItsChevron();
    void librarySelectedRowAnswersHoverAsTheQueues_data();
    void librarySelectedRowAnswersHoverAsTheQueues();
    void queueFocusRingStopsAtTheScrollBarLane_data();
    void queueFocusRingStopsAtTheScrollBarLane();
    void queueRowHoverIsTheListsTint();
    void rowStatesShareOneRightEdge_data();
    void rowStatesShareOneRightEdge();
    void transportIsWindowChromeInTheHeaderColours();
    void listsRingARowAsTheKeyboardComesIn();
    void tabIntoTheQueueFocusesItsCurrentRow();
    void tabIntoTheLibraryFocusesItsCurrentRow();
    void listsRingTheCurrentRowOnlyForTheKeyboard();
    void unreadableTracksAreMarkedFromTheIssueHistory();
    void aTrackThatFailsToPlayShowsTheWarningWhileCurrent();
    void anUnreadableRowTellsWhyToThePointerAndTheKeyboard();
    void unreadableRowsTellWhatFailedNotWhatPlaybackDid();
    void unreadableRowsReasonOpensBelowClearOfThePlayingRow();
    void unreadableRowsHaveTheErrorMarkNotTheWarnings();
    void queueStopMarkWeighsNoMoreThanPause_data();
    void queueStopMarkWeighsNoMoreThanPause();
    void queueHeaderLabelsOnlyArtistAlbumAndLength();
    void queueRightPaddingIsTheLaneAndAnInset_data();
    void queueRightPaddingIsTheLaneAndAnInset();
    void pressedQueueRowHasItsOwnColour();
    void queueNameKeepsTheTitleColumnsPlace_data();
    void queueNameKeepsTheTitleColumnsPlace();
    void narrowQueueDropsAlbumBeforeTitle();
    void queueColumnsGiveTheirSlackToTheTitle();
    void narrowWindowAtALargeFontFitsTheQueue();
    void narrowQueueAtALargeFontGivesTheNamesTheRoom_data();
    void narrowQueueAtALargeFontGivesTheNamesTheRoom();
    void queueKeepsArtistAtALargeFont_data();
    void queueKeepsArtistAtALargeFont();
    void visualizationTakesNearlyHalfTheRightColumn_data();
    void visualizationTakesNearlyHalfTheRightColumn();
    void firstRunShowsOneMessage();
    void nothingSaysFirstRunBeforeTheLibraryLoads();
    void tabGoesThroughTheQueueHeadersHandlesBeforeItsRows();
    void focusedColumnHandleHasAFrameAndSaysItsKeys_data();
    void focusedColumnHandleHasAFrameAndSaysItsKeys();
    void columnHandlesShowAQuietMarkAtRest_data();
    void columnHandlesShowAQuietMarkAtRest();
    void transportStartsAndEndsAtTheContentInset();
    void firstRunsButtonIsThePrimaryOne();
    void viewersInTheWindowFadeAsTheStylesPopupsDo();
};

void UiMainWindowTest::initTestCase() {
    startCapturingMessages();
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<SpectrogramViewFollow>("FerrousUi", 1, 0, "SpectrogramViewFollow");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<DiagnosticsLogView>("FerrousUi", 1, 0, "DiagnosticsLogView");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterType<WindowPresence>("FerrousUi", 1, 0, "WindowPresence");
    InputModality::registerQmlTypes();
    StandardKeys::registerQmlTypes();
}

void UiMainWindowTest::init() {
    clearCapturedMessages();
}

void UiMainWindowTest::cleanup() {
    // A window resize below the minimum size: the offscreen platform warns
    // that it does not pass size hints on.
    dropCapturedMessages(offscreenSizeHintsWarning());
    const QString messages = takeCapturedMessagesText();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void UiMainWindowTest::cleanupTestCase() {
    stopCapturingMessages();
}

void UiMainWindowTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void UiMainWindowTest::contentIsOneSheetWithFourSeams() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
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
    // the spectrogram's indigo also before any audio (tst_ui_motion's
    // visualizationPaneFloorFollowsTheMode).
    QCOMPARE(colorOf(sidebar), tokenHex(window, "view"));
    QCOMPARE(colorOf(queue), tokenHex(window, "view"));
    QCOMPARE(colorOf(pane), QStringLiteral("#ff14062e"));
    QCOMPARE(colorOf(pane), tokenHex(window, "plotFloor"));

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

void UiMainWindowTest::seekTimeStaysOverItsLine_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::addColumn<bool>("dark");
    // The default window and the narrow one, at 10 pt and at 24 pt, whose
    // time's box is widest on the narrow window's shortest bar, and whose
    // titles run past the sidebar in the wide window.
    QTest::newRow("10 pt, 1600 by 1000, dark") << 10.0 << 1600 << 1000 << true;
    QTest::newRow("10 pt, 1000 by 720, light") << 10.0 << 1000 << 720 << false;
    QTest::newRow("24 pt, 1600 by 1000, light") << 24.0 << 1600 << 1000 << false;
    QTest::newRow("24 pt, 1000 by 720, dark") << 24.0 << 1000 << 720 << true;
}

// The seek bar's time has one place: a lane above the bar, in the
// transport's band, centred on the line it reads, wherever the pointer is
// on the bar, and within the bar at either end. It covers none of the
// waveform, no menu title, nothing of the now-playing card or the queue's
// header (it flipped under the bar near the start, ~54 px down and over
// the now-playing title at 24 pt), and stays 3 px clear of the bar's and
// the buttons' focus rings. The band grows by no more than the lane needs,
// and the buttons, the bar, the time and the volume keep one centre line.
void UiMainWindowTest::seekTimeStaysOverItsLine() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    QCOMPARE(window->size(), QSize(windowWidth, windowHeight));
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge != nullptr);
    bridge->setProperty("durationText", QStringLiteral("03:49"));
    bridge->setProperty("durationSeconds", 229.0);
    playTrack(bridge);
    QQuickItem *transport = named(window, "transportBar");
    QQuickItem *seek = named(window, "seekSlider");
    QQuickItem *waveform = named(window, "seekWaveform");
    QQuickItem *seekRing = named(window, "seekFocusRing");
    QQuickItem *menuBar = named(window, "mainMenuBar");
    QQuickItem *nowPlaying = named(window, "nowPlayingPanel");
    QQuickItem *queueHeader = named(window, "queueHeader");
    QQuickItem *seam = named(window, "transportSeam");
    QQuickItem *playPause = named(window, "transportButton_playPause");
    QQuickItem *mute = named(window, "muteButton");
    QQuickItem *volume = named(window, "volumeSlider");
    QQuickItem *time = named(window, "transportTime");
    QObject *tip = window->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(transport && seek && waveform && seekRing && menuBar && nowPlaying && queueHeader && seam && playPause
            && mute && volume && time && tip);
    QTRY_VERIFY(seek->isEnabled());
    // Laid out in the window's width.
    QTRY_VERIFY(sceneRect(transport).right() <= windowWidth);
    auto *tipBox = tip->property("background").value<QQuickItem *>();
    QVERIFY(tipBox != nullptr);
    const QRectF bar = sceneRect(seek);
    const QRectF band = sceneRect(transport);

    // One centre line for the row.
    for (QQuickItem *item : {playPause, mute, volume, time}) {
        QVERIFY2(std::abs(sceneRect(item).center().y() - bar.center().y()) <= 0.5,
                 qPrintable(item->objectName() + QLatin1Char(' ') + describeRect(sceneRect(item))
                            + QStringLiteral(", the bar ") + describeRect(bar)));
    }
    // The band ends at its seam, and holds the row with 8 px under the
    // buttons, or more where the band's 48 px leave more.
    QCOMPARE(sceneRect(seam).top(), band.bottom());
    QVERIFY(band.bottom() - sceneRect(playPause).bottom() >= 8 - 0.5);

    QList<QRectF> titles;
    for (int i = 0; i < menuBar->property("count").toInt(); ++i) {
        QQuickItem *title = nullptr;
        QVERIFY(QMetaObject::invokeMethod(menuBar, "itemAt", Q_RETURN_ARG(QQuickItem *, title), Q_ARG(int, i)));
        QVERIFY(title != nullptr);
        titles.append(sceneRect(title));
    }
    QVERIFY(titles.size() >= 6);
    QVERIFY(bar.left() < titles.constLast().right());
    // Under the bar where it flipped before: the now-playing card and the
    // queue's header.
    const QRectF card = sceneRect(nowPlaying);
    const QRectF header = sceneRect(queueHeader);

    // Every few pixels across the titles, then a few points beyond.
    QList<qreal> xs;
    for (qreal x = bar.left(); x < std::min(titles.constLast().right() + 100, bar.right() - 1); x += 7) {
        xs.append(x);
    }
    for (const qreal fraction : {0.3, 0.5, 0.9, 0.98, 1.0}) {
        xs.append(std::min(bar.left() + bar.width() * fraction, bar.right() - 1));
    }
    std::optional<qreal> boxTop;
    for (const qreal x : xs) {
        const QPoint at = QPointF(x, bar.center().y()).toPoint();
        QTest::mouseMove(window, at);
        QTRY_VERIFY2(tip->property("opened").toBool(),
                     qPrintable(QStringLiteral("at %1, %2: shown %3, the bar %4 to %5")
                                    .arg(at.x()).arg(at.y()).arg(tip->property("shown").toBool())
                                    .arg(bar.left()).arg(bar.right())));
        QTRY_VERIFY(std::abs(tip->property("anchorX").toReal() - (at.x() - bar.left())) < 1.0);
        const QRectF box = sceneRect(tipBox);
        const QString where =
            QStringLiteral("at %1: %2, the bar %3").arg(at.x()).arg(describeRect(box), describeRect(bar));
        // In one place: every box at the same height, in the band, over the
        // bar and its focus ring, and in the window.
        if (!boxTop) {
            boxTop = box.top();
        }
        QVERIFY2(std::abs(box.top() - *boxTop) <= 0.5, qPrintable(where));
        QVERIFY2(box.top() >= band.top() - 0.5 && box.bottom() < bar.top(), qPrintable(where));
        QVERIFY2(!box.intersects(sceneRect(waveform)), qPrintable(where));
        QVERIFY2(box.left() >= 0.0 && box.right() <= windowWidth, qPrintable(where));
        for (const QRectF &title : titles) {
            QVERIFY2(!box.intersects(title), qPrintable(where + QStringLiteral(", a title ") + describeRect(title)));
        }
        QVERIFY2(!box.intersects(card), qPrintable(where + QStringLiteral(", the card ") + describeRect(card)));
        QVERIFY2(!box.intersects(header), qPrintable(where));
        // Centred on its line, or, where that would hang past the bar's
        // end, flush with that end and still over the line.
        const qreal half = box.width() / 2;
        const qreal placedLeft = std::clamp(at.x() - half, bar.left(), bar.right() - box.width());
        QVERIFY2(std::abs(box.left() - placedLeft) <= 0.5, qPrintable(where));
        QVERIFY2(box.left() <= at.x() && box.right() >= at.x(), qPrintable(where));
    }
    QVERIFY(boxTop.has_value());
    const QRectF box = sceneRect(tipBox);
    // 3 px clear of the bar's focus ring and of the buttons' (3 px outside
    // them), with the ring shown.
    QVERIFY(box.bottom() <= playPause->mapToScene(QPointF(0, 0)).y() - 3 - 3 + 0.5);
    seek->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(seekRing->isVisible());
    QVERIFY2(sceneRect(tipBox).bottom() <= sceneRect(seekRing).top() - 3 + 0.5,
             qPrintable(describeRect(sceneRect(tipBox)) + QStringLiteral(" over ") + describeRect(sceneRect(seekRing))));
    QCOMPARE(sceneRect(tipBox).top(), box.top());
    // The band grew by no more than the lane needs: the box is at its top
    // where the room above the bar was too little for it.
    QVERIFY2(box.top() - band.top() <= std::max(0.0, bar.top() - band.top() - box.height()) + 1.0,
             qPrintable(describeRect(box) + QStringLiteral(" in ") + describeRect(band)));

    // Every tooltip of the transport shares the lane: one line on the time
    // box's line, as tall as it, in the band and the window, none of the
    // menu's titles covered (the buttons' opened under the band, over the
    // cover and the queue's header, and Mute's did).
    QList<std::pair<QQuickItem *, QObject *>> laneTips;
    for (const char *name : {"transportButton_previous", "transportButton_playPause", "transportButton_stop",
                             "transportButton_next", "muteButton"}) {
        QQuickItem *button = named(window, name);
        QVERIFY2(button != nullptr, name);
        QObject *buttonTip = button->findChild<QObject *>(QStringLiteral("transportButtonToolTip"));
        QVERIFY2(buttonTip != nullptr, name);
        // Those that can act: one that cannot takes no focus.
        if (button->isEnabled()) {
            laneTips.append({button, buttonTip});
        }
    }
    QObject *volumeTip = window->findChild<QObject *>(QStringLiteral("volumeToolTip"));
    QVERIFY(volumeTip != nullptr);
    laneTips.append({volume, volumeTip});
    QVERIFY(laneTips.size() >= 4);
    for (const auto &[control, laneTip] : laneTips) {
        control->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY2(laneTip->property("opened").toBool(), qPrintable(control->objectName()));
        const QRectF drawn = sceneRect(laneTip->property("background").value<QQuickItem *>());
        const QString where = control->objectName() + QLatin1Char(' ') + describeRect(drawn)
            + QStringLiteral(", the time box ") + describeRect(box);
        QVERIFY2(std::abs(drawn.bottom() - box.bottom()) <= 0.5 && std::abs(drawn.height() - box.height()) <= 0.5,
                 qPrintable(where));
        QVERIFY2(drawn.top() >= band.top() && drawn.left() >= 0 && drawn.right() <= windowWidth, qPrintable(where));
        for (const QRectF &title : titles) {
            QVERIFY2(!drawn.intersects(title), qPrintable(where + QStringLiteral(", a title ") + describeRect(title)));
        }
    }
}

void UiMainWindowTest::seekTimeBoxIsCrispWithoutAShadow_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

// The seek bar's time box is one style in both themes: the tooltip's fill
// with a crisp 1 px edge, and no shadow around it (light's soft shadow
// spread into the gap between the box and the waveform, and its edge was
// lost on the Header chrome, so it read as a white card where the dark
// box read as a framed readout). The edge stands out as much on the
// chrome as on the fill.
void UiMainWindowTest::seekTimeBoxIsCrispWithoutAShadow() {
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    MainWindowFixture fixture(sized(1600, 1000));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QVERIFY(bridge != nullptr);
    bridge->setProperty("durationText", QStringLiteral("03:49"));
    bridge->setProperty("durationSeconds", 229.0);
    playTrack(bridge);
    QQuickItem *transport = named(window, "transportBar");
    QQuickItem *seek = named(window, "seekSlider");
    QObject *tip = window->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(transport && seek && tip);
    QTRY_VERIFY(seek->isEnabled());
    const QRectF bar = sceneRect(seek);
    // Into the bar from under the band, as a pointer comes.
    QTest::mouseMove(window, QPointF(bar.left() + bar.width() * 0.4, sceneRect(transport).bottom() + 40).toPoint());
    QTest::mouseMove(window, QPointF(bar.left() + bar.width() * 0.4, bar.center().y()).toPoint());
    QTRY_VERIFY(tip->property("opened").toBool());
    QTRY_COMPARE(tip->property("opacity").toReal(), 1.0);
    auto *box = tip->property("background").value<QQuickItem *>();
    QVERIFY(box != nullptr);
    // No shadow at all: the style's box, a Kirigami ShadowedRectangle, casts
    // one that an offscreen grab doesn't draw.
    QVERIFY2(!box->property("shadow").isValid(), box->metaObject()->className());
    const QRectF drawnBox = sceneRect(box);

    const Drawn drawn(window);
    const QColor chrome = transport->property("chromeColor").value<QColor>();
    const QColor fill = transport->property("laneBoxFill").value<QColor>();
    // Square on the band: the chrome all round it, 1 px off its edge, in
    // the gap over the bar and 2 px further down, beside it, and over it.
    const qreal middle = drawnBox.center().x();
    const QList<QPointF> around{
        {middle, drawnBox.bottom() + 0.5},        {middle, drawnBox.bottom() + 2.5},
        {middle - 20, drawnBox.bottom() + 1.5},   {drawnBox.left() - 1.5, drawnBox.center().y()},
        {drawnBox.right() + 1.5, drawnBox.center().y()}, {middle, drawnBox.top() - 0.5}};
    for (const QPointF &point : around) {
        if (point.y() >= bar.top() || point.y() < sceneRect(transport).top()) {
            continue;
        }
        QVERIFY2(channelDistance(drawn.at(point), chrome) <= 2,
                 qPrintable(QStringLiteral("at %1, %2: %3, the chrome %4, the box %5")
                                .arg(point.x()).arg(point.y())
                                .arg(drawn.at(point).name(), chrome.name(), describeRect(drawnBox))));
    }
    // A 1 px edge, the same at the top and the bottom, then the fill.
    const QColor top = drawn.at(middle, drawnBox.top() + 0.5);
    const QColor bottom = drawn.at(middle, drawnBox.bottom() - 0.5);
    QVERIFY2(channelDistance(top, bottom) <= 2, qPrintable(top.name() + QStringLiteral(" and ") + bottom.name()));
    QVERIFY2(channelDistance(drawn.at(middle, drawnBox.top() + 1.5), fill) <= 2,
             qPrintable(drawn.at(middle, drawnBox.top() + 1.5).name() + QStringLiteral(", the fill ") + fill.name()));
    // At least 1.8:1 on both the fill and the chrome, as the style's edge
    // is in Breeze Dark, whose fill is the chrome (in Breeze Light it was
    // 1.5:1 on the chrome, and soft without the shadow).
    const double least = transport->property("laneBoxEdgeContrast").toDouble();
    QCOMPARE(least, 1.8);
    const double onFill = contrastRatio(top, fill);
    const double onChrome = contrastRatio(top, chrome);
    QVERIFY2(onFill >= least - 0.05 && onChrome >= least - 0.05,
             qPrintable(QStringLiteral("the edge %1: %2:1 on the fill %3, %4:1 on the chrome %5")
                            .arg(top.name()).arg(onFill).arg(fill.name()).arg(onChrome).arg(chrome.name())));

    // The buttons' tooltips in the same lane have the same box.
    QQuickItem *playPause = named(window, "transportButton_playPause");
    QVERIFY(playPause != nullptr);
    QObject *buttonTip = playPause->findChild<QObject *>(QStringLiteral("transportButtonToolTip"));
    QVERIFY(buttonTip != nullptr);
    auto *buttonBox = buttonTip->property("background").value<QQuickItem *>();
    QVERIFY(buttonBox != nullptr);
    QCOMPARE(buttonBox->objectName(), box->objectName());
    QVERIFY(!buttonBox->property("shadow").isValid());
}

void UiMainWindowTest::transportRowSitsBalancedInItsBand_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::addColumn<bool>("dark");
    QTest::newRow("10 pt, 1600 by 1000, dark") << 10.0 << 1600 << 1000 << true;
    QTest::newRow("10 pt, 1000 by 720, light") << 10.0 << 1000 << 720 << false;
    QTest::newRow("24 pt, 1600 by 1000, light") << 24.0 << 1600 << 1000 << false;
    QTest::newRow("24 pt, 1000 by 720, dark") << 24.0 << 1000 << 720 << true;
}

// The transport's row sits balanced in its band, the lane of its popups
// above it, empty at rest, and room under it: from the menu's titles down
// to the buttons' glyphs, as drawn, at most 3:2 of the band under the
// glyphs (the lane over the row and 8 px under it sank the controls onto
// the seam, 44 px against 17 px at 10 pt, 76 px against 21 px at 24 pt).
// In the least window, at 24 pt in 1000 by 720, the room under the row
// gives way to the panes' minimums, so the window still fits; the row
// still sits higher than it did (76 px against 21). There the lane's
// 3 px clear of the focus rings, 2 px more than it had, come out of the
// room under the row: 74 px against 32.
void UiMainWindowTest::transportRowSitsBalancedInItsBand() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    QCOMPARE(window->size(), QSize(windowWidth, windowHeight));
    QVERIFY2(window->minimumHeight() <= 720, qPrintable(QString::number(window->minimumHeight())));
    QQuickItem *transport = named(window, "transportBar");
    QQuickItem *seam = named(window, "transportSeam");
    QQuickItem *menuBar = named(window, "mainMenuBar");
    QQuickItem *previous = named(window, "transportButton_previous");
    QQuickItem *next = named(window, "transportButton_next");
    QVERIFY(transport && seam && menuBar && previous && next);
    const QRectF band = sceneRect(transport);
    QCOMPARE(sceneRect(seam).top(), band.bottom());

    // The room under the row: what balances the lane, or in the least
    // window what it has beyond the panes' minimums.
    const qreal wanted = transport->property("wantedBalanceRoom").toReal();
    const qreal room = transport->property("balanceRoom").toReal();
    QVERIFY(wanted > 0);
    QCOMPARE(band.height(), transport->property("leastHeight").toReal() + room);
    const bool least = pointSize > 10 && windowHeight == 720;
    if (least) {
        QVERIFY2(room < wanted && room >= 0, qPrintable(QStringLiteral("%1 of %2").arg(room).arg(wanted)));
        QCOMPARE(room, qreal(windowHeight - window->minimumHeight()));
    } else {
        QCOMPARE(room, wanted);
    }

    // As drawn: the lowest ink of the titles, and the glyphs' ink, read
    // against the chrome at the band's right end.
    const Drawn drawn(window);
    const QColor chrome = drawn.at(band.right() - 2, band.bottom() - 2);
    const auto inked = [&](qreal x, qreal y) {
        const QColor c = drawn.at(x, y);
        return std::abs(c.red() - chrome.red()) + std::abs(c.green() - chrome.green())
            + std::abs(c.blue() - chrome.blue()) > 60;
    };
    const auto inkRows = [&](const QRectF &area) {
        std::optional<qreal> top;
        std::optional<qreal> bottom;
        for (qreal y = std::floor(area.top()); y < area.bottom(); ++y) {
            for (qreal x = std::floor(area.left()); x < area.right(); ++x) {
                if (inked(x + 0.5, y + 0.5)) {
                    top = top ? top : y;
                    bottom = y + 1;
                    break;
                }
            }
        }
        return std::pair{top, bottom};
    };
    const QRectF menu = sceneRect(menuBar);
    const auto [menuTop, menuBottom] = inkRows(QRectF(menu.left(), menu.top(), std::min(menu.width(), 400.0), menu.height()));
    const QRectF buttons = sceneRect(previous).united(sceneRect(next));
    const auto [glyphTop, glyphBottom] = inkRows(buttons);
    QVERIFY(menuTop && menuBottom && glyphTop && glyphBottom);
    const qreal above = *glyphTop - *menuBottom;
    const qreal below = band.bottom() - *glyphBottom;
    const QString proportions = QStringLiteral("%1 px over the glyphs, %2 px under them, the band %3")
        .arg(above).arg(below).arg(describeRect(band));
    QVERIFY2(below > 0 && above > 0, qPrintable(proportions));
    QVERIFY2(above <= (least ? 2.35 : 1.5) * below, qPrintable(proportions));
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
    // leave the library about three rows.
    QTest::newRow("24 pt, 1600 by 1000") << 24.0 << 1600 << 1000 << 408.0 << 360.0 << true;
    QTest::newRow("24 pt, 1000 by 720") << 24.0 << 1000 << 720 << 360.0 << 360.0 << true;
    // Wider windows allow the pane more at a large font: what
    // the queue does not need for its columns, up to 22 grid units of the
    // font, 968 px at 24 pt; 0 here for that, worked out in the test. The
    // wide layout's cover would be as wide: the panel is narrow.
    QTest::newRow("24 pt, 1920 by 1200") << 24.0 << 1920 << 1200 << 0.0 << 360.0 << true;
    QTest::newRow("24 pt, 2560 by 1400") << 24.0 << 2560 << 1400 << 968.0 << 360.0 << true;
}

// The sidebar is 360 px wide in a 1600 px window and 260 px in a 1000 px
// one with the default font. With a large font it is wider, so the
// library's names and the now-playing lines are more than fragments: at
// 24 pt, 360 px in the narrow window, where the now-playing panel stays
// narrow and the queue keeps its title and length, and 408 px in the wide
// one. Its least width follows the font too, and the window still fits
// 1000 px. A width the user dragged it to is kept.
void UiMainWindowTest::sidebarWidthFollowsTheWindowAndTheFont() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    QFETCH(qreal, expectedWidth);
    QFETCH(qreal, expectedMinimum);
    QFETCH(bool, narrow);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    QQuickItem *sidebar = named(window, "sidebarPane");
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *queue = named(window, "mainQueuePane");
    QVERIFY(sidebar && panel && queue);
    const qreal gridUnit = paletteOf(window)->property("gridUnit").toReal();
    if (expectedWidth == 0.0) {
        // What the window leaves once the queue has all its columns, and
        // the 1 px seam: more than the 408 px of its share, less than the
        // 22 grid units.
        QCOMPARE(gridUnit, 44.0);
        expectedWidth = std::floor(windowWidth - queue->property("widthWithAllColumns").toReal() - 1);
        QVERIFY2(expectedWidth > 408.0 && expectedWidth < 22 * gridUnit, qPrintable(QString::number(expectedWidth)));
    }
    QTRY_COMPARE(sidebar->width(), expectedWidth);
    if (pointSize > 10.0) {
        // The queue keeps every column where the pane is wider than its
        // share, and the pane is never more than 22 grid units of the font.
        QVERIFY(sidebar->width() <= std::max(360.0, 22 * gridUnit));
        QVERIFY(sidebar->width() == 408.0 || sidebar->width() == 360.0
                || queue->property("albumColumnVisible").toBool());
    }
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
    const qreal dragged = expectedWidth > 500 ? expectedWidth - 40 : expectedWidth + 40;
    QVERIFY(QMetaObject::invokeMethod(layout, "noteSidebarWidth", Q_ARG(QVariant, dragged),
                                      Q_ARG(QVariant, splitWidth)));
    QTRY_COMPARE(sidebar->width(), dragged);
}

void UiMainWindowTest::nothingPlayingIsACompactRow_data() {
    QTest::addColumn<bool>("narrow");
    QTest::addColumn<bool>("dark");
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("wide, light") << false << false << 0.0;
    QTest::newRow("narrow, dark") << true << true << 0.0;
    QTest::newRow("wide, light, 24 pt") << false << false << 24.0;
    QTest::newRow("narrow, dark, 24 pt") << true << true << 24.0;
}

// With nothing loaded, as on the first run, the panel is one compact row
// (the empty 360 px square, with "Nothing playing" far under it, took the
// library's room): Ferrous's mark and "Nothing playing" in a list row's
// height, with no cover's square, in the wide window and the narrow one
// alike, at any font size; the library has the rest of the sidebar. The
// mark, ferrous-mark-symbolic.svg, is the now-playing stand-in's glide and
// its 2nd and 3rd harmonics drawn for 16 px (the stock disc was unreadable
// at 16 px), in either theme; it follows the font as the transport's glyphs
// do. The first Play grows the track's layout from the row: the library
// then starts as far down as the layout is taller than the row.
void UiMainWindowTest::nothingPlayingIsACompactRow() {
    QFETCH(bool, narrow);
    QFETCH(bool, dark);
    QFETCH(qreal, pointSize);
    // The mark follows one rule for its optical size: two round-capped
    // strokes of 1.75 px for 20 px and under, the glide and its 3rd
    // harmonic, as the library's 20 px tile; three for 24 px and up, the
    // glide the heaviest.
    for (const auto &[name, strokes] : {std::pair{"ferrous-mark-small-symbolic.svg", 2},
                                        std::pair{"ferrous-mark-symbolic.svg", 3}}) {
        QFile file(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/assets/") + QLatin1String(name));
        QVERIFY2(file.open(QIODevice::ReadOnly), name);
        const QString svg = QString::fromUtf8(file.readAll());
        QVERIFY2(svg.contains(QStringLiteral("viewBox=\"0 0 16 16\"")), name);
        QCOMPARE(svg.count(QStringLiteral("<path ")), strokes);
        QCOMPARE(svg.count(QStringLiteral("stroke-linecap=\"round\"")), strokes);
        QCOMPARE(svg.count(QStringLiteral("stroke-width=\"1.75\"")), strokes == 2 ? 2 : 1);
    }

    const auto restorePalette = useDarkPalette(dark);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(narrow ? sized(1000, 720) : sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QCOMPARE(paletteOf(window)->property("themeIsDark").toBool(), dark);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *library = named(window, "libraryView");
    QQuickItem *glyph = named(window, "nowPlayingIdleGlyph");
    QVERIFY(panel && cover && header && library && glyph);
    // The layout a track would take: at 24 pt narrow in both windows, the
    // wide one too short for the wide layout. The row is the same in all.
    const bool narrowLayout = narrow || pointSize > 0;
    QTRY_COMPARE(panel->property("layoutMode").toString(),
                 narrowLayout ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    const qreal scale = std::max(1.0, paletteOf(window)->property("basePointSize").toReal() / 10.0);
    QCOMPARE(glyph->width(), std::max<qreal>(16, std::round(16 * scale)));
    if (pointSize == 0) {
        QTRY_VERIFY2(idleGlyphProblem(window).isEmpty(), qPrintable(idleGlyphProblem(window)));
    }
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
    QTRY_COMPARE(cover->width(), narrowLayout ? panel->property("narrowLayoutCoverSize").toReal() : panel->width());
    QCOMPARE(cover->height(), cover->width());
    QVERIFY(shown(cover));
    QCOMPARE(sceneRect(cover).topLeft(), coverCorner(panel));
    QVERIFY(!shown(named(window, "nowPlayingIdleRow")));
    QQuickItem *title = named(window, narrowLayout ? "nowPlayingNarrowTitle" : "nowPlayingTitle");
    QVERIFY(shown(title));
    QTRY_COMPARE(title->property("text").toString(), QStringLiteral("Morning Tide Field"));
    const qreal grownBy = panel->height() - panel->property("restHeight").toReal();
    QTRY_COMPARE(sceneRect(header).top(), sceneRect(panel).bottom());
    QCOMPARE(sceneRect(header).top(), idleTop + grownBy);
    // The library's view is whole rows high: within a row of it.
    QTRY_VERIFY2(std::abs(idleLibrary - library->height() - grownBy)
                     < paletteOf(window)->property("rowHeight").toReal(),
                 qPrintable(QStringLiteral("%1 to %2").arg(idleLibrary).arg(library->height())));
    // A track without a cover: the stand-in in the cover's place, and the
    // library where it was.
    const qreal headerTop = sceneRect(header).top();
    bridgeOf(window)->setProperty("currentTrackCoverPath", QString());
    QVERIFY(settled(panel));
    QTRY_VERIFY2(standInProblem(window).isEmpty(), qPrintable(standInProblem(window)));
    QCOMPARE(sceneRect(header).top(), headerTop);

    // Back to nothing loaded, as when the queue is cleared: the row again.
    clearPlayback(bridgeOf(window));
    QVERIFY(settled(panel));
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    QTRY_COMPARE(sceneRect(header).top(), idleTop);
}

void UiMainWindowTest::nowPlayingHasItsWideNarrowAndEmptyLayouts() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *palette = paletteOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QVERIFY(panel && cover && header);

    // Nothing loaded: the compact row (nothingPlayingIsACompactRow has the
    // rest of it).
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

    // Playing, wide: the cover edge to edge and square, then the text at
    // the 12 px inset and 8 px under it, with no card around it.
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
    QCOMPARE(sceneRect(title).top() - sceneRect(cover).bottom(), 8.0);
    QCOMPARE(title->property("text").toString(), QStringLiteral("Morning Tide Field"));
    QCOMPARE(artistAlbum->property("text").toString(), QStringLiteral("The Quiet Meridian — Small Hours"));
    QCOMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2 of 8"));
    // Without a total the backend is sure of, the number alone.
    bridge->setProperty("currentTrackTotal", 0);
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2"));
    bridge->setProperty("currentTrackTotal", 8);
    QTRY_COMPARE(details->property("text").toString(), QStringLiteral("2005 · Classical · Track 2 of 8"));
    // The title over the lines (nowPlayingWideBlockAtAnyFontSize has its
    // sizes); the artist and the album in the text colour, the details
    // tertiary.
    QCOMPARE(title->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(colorOf(title), tokenHex(window, "text"));
    QCOMPARE(colorOf(artistAlbum), tokenHex(window, "text"));
    QCOMPARE(colorOf(details), tokenHex(window, "textTertiary"));
    QCOMPARE(details->property("font").value<QFont>().pointSizeF(),
             palette->property("smallPointSize").toReal());
    QVERIFY(!panel->property("idle").toBool());
    // Then the seam into the library, the block with the cover a whole
    // number of rows high: two regions of one sheet, with no dead band
    // between them.
    QVERIFY(settled(panel));
    QTRY_VERIFY2(wideBlockProblem(window).isEmpty(), qPrintable(wideBlockProblem(window)));
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
    QTRY_VERIFY(title->property("elided").toBool());
    QVERIFY(panel->property("textTruncated").toBool());
    QCOMPARE(title->property("x").toReal(), 0.0);

    // Narrow: the cover with the lines beside it, as high as the block,
    // flush with its top, its left and the seam (a 96 px cover left a 25 px
    // band of the view under it), the block as high as the 96 px cover's or
    // the lines' room, then 12 px.
    window->resize(1000, 720);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QVERIFY(!panel->property("narrowStacked").toBool());
    QVERIFY(panel->property("narrowBeside").toBool());
    QVERIFY(grown(window));
    QVERIFY(settled(panel));
    {
        const qreal block = std::max(96.0, panel->property("narrowTextTop").toReal()
                                               + panel->property("narrowReservedHeight").toReal()) + 12;
        QTRY_COMPARE(panel->height(), block + 1);
        QTRY_COMPARE(cover->width(), block);
        QCOMPARE(cover->height(), block);
        QVERIFY2(block > 96.0 + 12, qPrintable(QString::number(block)));
        QCOMPARE(sceneRect(cover).topLeft(), sceneRect(panel).topLeft());
        QCOMPARE(sceneRect(cover).bottom(), sceneRect(seam).top());
        // The lines' top, one for every track, lower than 10 px down.
        const qreal top = panel->property("centredTextTop").toReal();
        QVERIFY2(top > 10.0, qPrintable(QString::number(top)));
        QCOMPARE(sceneRect(named(window, "nowPlayingNarrowTitle")).top() - sceneRect(panel).top(), top);
    }
    QQuickItem *narrowTitle = named(window, "nowPlayingNarrowTitle");
    QVERIFY(shown(narrowTitle));
    QCOMPARE(sceneRect(narrowTitle).left() - sceneRect(cover).right(), 12.0);
    QCOMPARE(narrowTitle->property("font").value<QFont>().pointSizeF(),
             palette->property("basePointSize").toReal() * 1.2);
    QCOMPARE(colorOf(named(window, "nowPlayingNarrowArtist")), tokenHex(window, "text"));
    QCOMPARE(colorOf(named(window, "nowPlayingNarrowAlbum")), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(named(window, "nowPlayingNarrowDetails")), tokenHex(window, "textTertiary"));
    // Beside the cover the long title wraps onto a second line, where it
    // is cut off (on one line, beside the cover at 24 pt, most of a title
    // was), and the seam is 6 px or more under the lowest of the lines, the
    // cover flush with it.
    QTRY_COMPARE(narrowTitle->property("lineCount").toInt(), 2);
    QVERIFY(narrowTitle->property("elided").toBool());
    QVERIFY(QQuickTest::qWaitForPolish(window));
    const qreal leastFoot = panel->property("besideLeastFoot").toReal();
    QVERIFY(leastFoot >= 6.0);
    QTRY_VERIFY(sceneRect(seam).top() - sceneRect(named(window, "nowPlayingNarrowDetails")).bottom() >= leastFoot - 0.5);
    QCOMPARE(sceneRect(cover).bottom(), sceneRect(seam).top());
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());
    // A title that fits stays on one line, and the block keeps the room of
    // two (it shrank to the cover's, so a track change moved the library).
    const qreal narrowSeam = sceneRect(seam).top();
    bridge->setProperty("currentTrackTitle", QStringLiteral("Tide"));
    QTRY_COMPARE(narrowTitle->property("lineCount").toInt(), 1);
    QVERIFY(QQuickTest::qWaitForPolish(window));
    QCOMPARE(sceneRect(seam).top(), narrowSeam);
    bridge->setProperty("currentTrackTitle", longTitle);

    // The narrow lines have the year with the album and the total with the
    // number, or the number alone without a total.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    bridge->setProperty("currentTrackYear", 2005);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(),
                 QStringLiteral("Classical · 2 of 8"));
    QCOMPARE(panel->property("narrowAlbumText").toString(), QStringLiteral("Small Hours (2005)"));
    bridge->setProperty("currentTrackTotal", 0);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · Track 2"));
    // Nor is a total below the number shown.
    bridge->setProperty("currentTrackTotal", 1);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · Track 2"));
}

void UiMainWindowTest::nowPlayingWideBlockAtAnyFontSize_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowHeight");
    // Each in a window tall enough for the wide layout at its font.
    QTest::newRow("10 pt, 1600 by 1000") << 10.0 << 1000;
    QTest::newRow("14 pt, 1600 by 1200") << 14.0 << 1200;
    QTest::newRow("24 pt, 1600 by 1400") << 24.0 << 1400;
}

// The wide layout's text block at any font size. The title is a clear step
// over the lines under it: a level 1 Kirigami heading's 1.35 times the
// font, DemiBold; "Artist — Album" at the font in the text colour, on one
// line, whole where it fits and elided where it does not, with the whole in
// the tooltip, 12 px in from both sides (a line each for the two made the
// block taller for some tracks); the details smaller, one text in the
// tertiary colour, never cut off (detailsCutProblem). The block is on the
// queue's row grid, as high as makes it, with the cover, a whole number of
// the list's rows, so the seam at its foot and the hairline under the
// library's header are on lines of the queue's rows; in the default window
// the library header's hairline continues the seam between the queue and
// the visualization. It is one height for every track (an untagged
// track's was 56 px, a tagged one's 82 and one with a long album's 108, so
// the library's header moved by up to 26 px on a track change): a line the
// track has no text for keeps its row, empty. A track without a cover has
// the wide tile's stand-in.
void UiMainWindowTest::nowPlayingWideBlockAtAnyFontSize() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowHeight);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(1600, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *palette = paletteOf(window);
    QTRY_COMPARE(palette->property("basePointSize").toReal(), pointSize);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *title = named(window, "nowPlayingTitle");
    QQuickItem *artistAlbum = named(window, "nowPlayingArtistAlbum");
    QQuickItem *details = named(window, "nowPlayingDetails");
    QVERIFY(panel && cover && seam && header && queue && title && artistAlbum && details);
    playTrack(bridge);
    QVERIFY(grown(window));
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QVERIFY(settled(panel));
    // The sidebar at the width the font gives it, which a larger one
    // widens, and the panel at its height.
    QTRY_COMPARE(panel->width(), named(window, "sidebarPane")->property("splitPreferredWidth").toReal());
    QTRY_COMPARE(panel->height(), panel->implicitHeight());
    const QRectF block = sceneRect(panel);

    // The title a step over the lines: its capitals three tenths taller at
    // least, each line its font's height, none over the next.
    const QFont titleFont = title->property("font").value<QFont>();
    const QFont lineFont = artistAlbum->property("font").value<QFont>();
    QCOMPARE(titleFont.pointSizeF(), pointSize * 1.35);
    QCOMPARE(titleFont.weight(), QFont::DemiBold);
    QCOMPARE(lineFont.pointSizeF(), pointSize);
    QCOMPARE(lineFont.weight(), QFont::Normal);
    QCOMPARE(details->property("font").value<QFont>().pointSizeF(), pointSize * 0.9);
    QVERIFY(QFontMetricsF(titleFont).capHeight() >= 1.3 * QFontMetricsF(lineFont).capHeight());
    QTRY_VERIFY(title->height() >= QFontMetricsF(titleFont).height() - 1);
    QTRY_VERIFY(sceneRect(artistAlbum).top() >= sceneRect(title).bottom());
    QTRY_VERIFY(sceneRect(details).top() >= sceneRect(artistAlbum).bottom());

    // "Artist — Album": one line, always.
    for (const char *gone : {"nowPlayingWideArtist", "nowPlayingWideAlbum"}) {
        QVERIFY2(named(window, gone) == nullptr, gone);
    }
    QVERIFY(!panel->property("wideArtistAlbumSplit").isValid());
    const QString both = QStringLiteral("The Quiet Meridian — Small Hours");
    const bool fits = QFontMetricsF(lineFont).horizontalAdvance(both) <= block.width() - 24;
    // The default font fits it on one line; 24 pt does not.
    if (pointSize == 10.0) {
        QVERIFY(fits);
    } else if (pointSize == 24.0) {
        QVERIFY(!fits);
    }
    QVERIFY(shown(artistAlbum));
    QCOMPARE(artistAlbum->property("text").toString(), both);
    QCOMPARE(artistAlbum->property("maximumLineCount").toInt(), 1);
    QCOMPARE(artistAlbum->property("wrapMode").toInt(), 0 /* Text.NoWrap */);
    QCOMPARE(colorOf(artistAlbum), tokenHex(window, "text"));
    QTRY_COMPARE(artistAlbum->property("elided").toBool(), !fits);
    QVERIFY(sceneRect(artistAlbum).left() >= block.left() + 12);
    QVERIFY(sceneRect(artistAlbum).right() <= block.right() - 12);
    if (fits) {
        QVERIFY(QFontMetricsF(lineFont).horizontalAdvance(both) <= artistAlbum->width());
    }

    // The details.
    QTRY_VERIFY2(detailsColourProblem(window, details).isEmpty(), qPrintable(detailsColourProblem(window, details)));
    {
        const QString cut = detailsCutProblem(panel, details, true, QStringLiteral("Classical"), {title, artistAlbum});
        QVERIFY2(cut.isEmpty(), qPrintable(cut));
    }
    if (pointSize == 10.0) {
        // Room for all of it.
        QCOMPARE(details->property("text").toString(), panel->property("detailsText").toString());
    }
    // With a genre too long for the line, what fits of the rest.
    bridge->setProperty("currentTrackGenre", kLongGenre);
    QTRY_COMPARE(panel->property("detailsText").toString(),
                 QStringLiteral("2005 · %1 · Track 2 of 8").arg(kLongGenre));
    QVERIFY(settled(panel));
    {
        const QString cut = detailsCutProblem(panel, details, true, kLongGenre, {title, artistAlbum});
        QVERIFY2(cut.isEmpty(), qPrintable(cut));
    }
    QTRY_VERIFY2(detailsColourProblem(window, details).isEmpty(), qPrintable(detailsColourProblem(window, details)));
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    QVERIFY(settled(panel));

    // On the queue's grid: its rows are as high as the palette's, and the
    // library's header under the block is a row, with its hairline at its
    // foot, a whole number of rows under the column's top.
    QTRY_VERIFY2(wideBlockProblem(window).isEmpty(), qPrintable(wideBlockProblem(window)));
    const qreal rowHeight = palette->property("rowHeight").toReal();
    QCOMPARE(queue->property("rowPitch").toReal(), rowHeight);
    const qreal blockHeight = sceneRect(seam).top() - sceneRect(cover).bottom();
    QCOMPARE(blockHeight, panel->property("wideBlockHeight").toReal());
    QCOMPARE(std::fmod(sceneRect(header).bottom() - block.top() - 1, rowHeight), 0.0);
    if (windowHeight == 1000) {
        // The default window: the queue's list ends on the same line, and
        // the seam under it is the hairline's row of pixels.
        const QList<QQuickItem *> handles = itemsNamed(window->contentItem(), QStringLiteral("seamHandle"));
        QCOMPARE(handles.size(), 2);
        QQuickItem *queueSeam = handles.at(0)->height() == 1.0 ? handles.at(0) : handles.at(1);
        QCOMPARE(sceneRect(queue).top(), block.top());
        QCOMPARE(sceneRect(queueSeam).top(), sceneRect(header).bottom() - 1);
        QCOMPARE(sceneRect(queueSeam).bottom(), sceneRect(header).bottom());
    }

    // One height for every track: the block from the cover's foot to the
    // seam, the library's header and each line's row stay.
    const auto geometry = [&]() {
        return QList<QRectF>{QRectF(0, sceneRect(cover).bottom(), 1, sceneRect(seam).top() - sceneRect(cover).bottom()),
                             sceneRect(header), sceneRect(title), sceneRect(artistAlbum), sceneRect(details)};
    };
    const QList<QRectF> tagged = geometry();
    // The next track of the album: once its number has faded in, one text
    // in one colour again.
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist A/Album A/03.flac"));
    bridge->setProperty("currentTrackTitle", QStringLiteral("Low Sun"));
    bridge->setProperty("currentTrackNumber", 3);
    QVERIFY(settled(panel));
    QTRY_VERIFY2(detailsColourProblem(window, details).isEmpty(), qPrintable(detailsColourProblem(window, details)));
    QVERIFY(details->property("text").toString().contains(QStringLiteral("Track 3")));
    QCOMPARE(geometry(), tagged);
    // Untagged: the file name for a title, its folder in the artist's row,
    // the details' row empty, kept.
    bridge->setProperty("currentTrackPath", kTrackPath);
    clearTrackTags(bridge);
    QTRY_COMPARE(panel->property("artistAlbumText").toString(), QString());
    QTRY_COMPARE(panel->property("detailsText").toString(), QString());
    QVERIFY(settled(panel));
    QVERIFY(shown(artistAlbum) && shown(details));
    QCOMPARE(artistAlbum->property("text").toString(), QFileInfo(kTrackPath).dir().dirName());
    QCOMPARE(details->property("text").toString(), QString());
    QCOMPARE(geometry(), tagged);
    QTRY_VERIFY2(wideBlockProblem(window).isEmpty(), qPrintable(wideBlockProblem(window)));
    // A long album: one line, elided, with the whole in the tooltip.
    playTrack(bridge);
    bridge->setProperty("currentTrackAlbum",
                        QStringLiteral("A Remarkably Long Album Title That Runs On Past The Cover"));
    QTRY_VERIFY(artistAlbum->property("elided").toBool());
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), tagged);
    QVERIFY(panel->property("textTruncated").toBool());
    // A title alone, no artist or album: the same.
    bridge->setProperty("currentTrackArtist", QString());
    bridge->setProperty("currentTrackAlbum", QString());
    QTRY_COMPARE(panel->property("artistAlbumText").toString(), QString());
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), tagged);

    // A track without a cover: the stand-in in the cover's place, the wide
    // tile's mark at this font, and the block where it was.
    playTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QString());
    QVERIFY(settled(panel));
    QTRY_VERIFY2(standInProblem(window).isEmpty(), qPrintable(standInProblem(window)));
    QVERIFY(!cover->property("standInCompact").toBool());
    QCOMPARE(geometry(), tagged);
}

void UiMainWindowTest::nowPlayingNarrowShowsEveryLineAtAnyFontSize_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<QString>("arrangement");
    QTest::addColumn<qreal>("coverSide");
    // Beside the cover, which fills the block where the lines keep a dozen
    // characters beside it (0: the block's height less the seam), and is
    // 96 px where they would not.
    QTest::newRow("10 pt") << 10.0 << QStringLiteral("beside") << 0.0;
    QTest::newRow("14 pt") << 14.0 << QStringLiteral("beside") << 96.0;
    QTest::newRow("18 pt") << 18.0 << QStringLiteral("stacked") << 96.0;
    // (-1: two lines of the title.)
    QTest::newRow("24 pt") << 24.0 << QStringLiteral("header") << -1.0;
}

// In a narrow window, at any font size, the now-playing lines all show,
// each with at least 8 characters of its text (24 pt in 1000 by 720 too),
// the artist and the album on lines of their own: beside the cover, the
// block as tall as the 96 px cover or as its lines, whichever is taller,
// the cover filling it where the lines keep a dozen characters beside it;
// under the 96 px cover, on the whole width, once the room beside it is
// under about 12 characters; and with a large font, the header: the title
// beside a cover two of its lines high, the rest under it on the whole
// width. Then 12 px and the seam before the library.
void UiMainWindowTest::nowPlayingNarrowShowsEveryLineAtAnyFontSize() {
    QFETCH(qreal, pointSize);
    QFETCH(QString, arrangement);
    QFETCH(qreal, coverSide);
    const bool stacked = arrangement == QStringLiteral("stacked");
    const bool headerRow = arrangement == QStringLiteral("header");
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(1000, 720));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    QObject *bridge = bridgeOf(window);
    playTrack(bridge);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Lantern Signal"));
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QCOMPARE(panel->property("narrowStacked").toBool(), stacked);
    QCOMPARE(panel->property("largeHeader").toBool(), headerRow);
    QCOMPARE(panel->property("narrowBeside").toBool(), arrangement == QStringLiteral("beside"));
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *title = named(window, "nowPlayingNarrowTitle");
    QQuickItem *artist = named(window, "nowPlayingNarrowArtist");
    QQuickItem *album = named(window, "nowPlayingNarrowAlbum");
    QQuickItem *details = named(window, "nowPlayingNarrowDetails");
    QVERIFY(cover && seam && header && title && artist && album && details);
    QVERIFY(grown(window));
    QVERIFY(settled(panel));
    QTRY_VERIFY(QQuickTest::qWaitForPolish(window));
    if (coverSide == 0.0) {
        coverSide = panel->height() - 1;
    } else if (coverSide < 0) {
        const QFontMetricsF titleMetrics(title->property("font").value<QFont>());
        coverSide = panel->property("headerCoverSize").toReal();
        // Two lines of the title, never under 96 px (at the base size in a
        // 360 px sidebar, 96 px).
        QVERIFY2(coverSide >= 96.0 && std::abs(coverSide - std::max(96.0, 2 * titleMetrics.height())) <= 6.0,
                 qPrintable(QString::number(coverSide)));
    }
    QTRY_COMPARE(cover->width(), coverSide);
    QCOMPARE(cover->height(), coverSide);
    // The artist and the album, each on its own line, laid out for the
    // font, one under the other.
    QCOMPARE(artist->property("text").toString(), QStringLiteral("The Quiet Meridian"));
    QVERIFY(album->property("text").toString().startsWith(QStringLiteral("Small Hours")));
    QTRY_VERIFY(sceneRect(artist).top() >= sceneRect(title).bottom()
                && sceneRect(album).top() >= sceneRect(artist).bottom()
                && sceneRect(details).top() >= sceneRect(album).bottom());
    const QRectF tile = sceneRect(cover);
    const QRectF block = sceneRect(panel);
    QCOMPARE(tile.topLeft(), coverCorner(panel));

    qreal bottom = 0;
    for (QQuickItem *line : {title, artist, album, details}) {
        const QByteArray name = line->objectName().toLatin1();
        QVERIFY2(shown(line), name);
        const QRectF rect = sceneRect(line);
        const QString text = line->property("text").toString();
        QVERIFY2(!text.isEmpty(), name);
        // In the panel, clear of the cover, with its font's height; the
        // large-font header's title to 8 px from its right side.
        const qreal end = line == title && panel->property("largeHeader").toBool() ? 8 : 12;
        QVERIFY2(rect.left() >= block.left() + 12 && rect.right() <= block.right() - end, name);
        QVERIFY2(!rect.intersects(tile), name);
        QVERIFY2(rect.height() >= QFontMetricsF(line->property("font").value<QFont>()).height() - 1, name);
        if (stacked) {
            QVERIFY2(rect.top() >= tile.bottom() + 12, name);
            QCOMPARE(rect.left(), block.left() + 12);
        } else if (headerRow && line != title) {
            // Under the header, 8 px under the cover, at the inset, on the
            // whole width.
            QVERIFY2(rect.top() >= tile.bottom() + 8, name);
            QCOMPARE(rect.left(), block.left() + 12);
            QCOMPARE(rect.width(), block.width() - 24);
        } else {
            // 12 px after the cover, in the header too.
            QCOMPARE(rect.left(), tile.right() + 12);
        }
        // Whole, or at least 8 characters of it and the ellipsis.
        const QFontMetricsF metrics(line->property("font").value<QFont>());
        QVERIFY2(metrics.horizontalAdvance(text) <= rect.width()
                     || metrics.horizontalAdvance(text.left(8) + QStringLiteral("…")) <= rect.width(),
                 qPrintable(QStringLiteral("%1: %2 in %3 px").arg(line->objectName(), text).arg(rect.width())));
        bottom = std::max(bottom, rect.bottom());
    }
    // The seam 12 px or more under the lowest of them, and under the cover
    // but where the cover fills the block beside the lines, flush with the
    // seam: the block keeps the room of a title on two lines, whatever the
    // track (nowPlayingNarrowBlockIsOneHeightForEveryTrack).
    const bool fillsBlock = arrangement == QStringLiteral("beside") && coverSide > 96.0;
    QTRY_VERIFY(sceneRect(seam).top() - bottom >= (fillsBlock ? 6.0 : 12.0) - 0.5);
    if (fillsBlock) {
        // A title on one line and the three under it centred on the cover,
        // as far as the room for a title's second line lets them (10 px
        // down, they had some 14 px over and 36 under).
        const qreal above = sceneRect(title).top() - tile.top();
        const qreal below = tile.bottom() - sceneRect(details).bottom();
        QVERIFY2(above > 10.0 && below - above <= 14.0 && below >= above - 1.0,
                 qPrintable(QStringLiteral("%1 over and %2 under").arg(above).arg(below)));
        QCOMPARE(tile.bottom(), sceneRect(seam).top());
    } else {
        QVERIFY(sceneRect(seam).top() - tile.bottom() >= 12.0);
    }
    QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());
    const qreal seamAt = sceneRect(seam).top();
    if (headerRow) {
        // One block: the cover and the lines on one left edge, the title
        // centred on the cover, within it; the artist the panel's 12 px
        // inset under the cover, the album and the details 4 px apart, then
        // 12 px.
        const QRectF titleRect = sceneRect(title);
        QCOMPARE(tile.left(), sceneRect(artist).left());
        QVERIFY(std::abs(titleRect.center().y() - tile.center().y()) <= 1.0 && titleRect.top() >= tile.top());
        QTRY_COMPARE(sceneRect(artist).top(), tile.bottom() + 12);
        QCOMPARE(sceneRect(album).top(), sceneRect(artist).bottom() + 4);
        QCOMPARE(sceneRect(details).top(), sceneRect(album).bottom() + 4);
        QCOMPARE(seamAt - sceneRect(details).bottom(), 12.0);
        return;
    }
    if (!stacked) {
        // Beside the cover, the title 4 px over the artist (at 2 px it ran
        // into the small lines), and the block no taller for it.
        QCOMPARE(sceneRect(artist).top() - sceneRect(title).bottom(), 4.0);
        return;
    }

    // Under the cover, the lines are 4 px apart, and a title too long for
    // one line wraps onto a second rather than be cut off; under a title
    // on one line, an artist too long for one does, or else an album; the
    // details stay on one line. The block keeps its height.
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
    const auto wraps = [&](QQuickItem *line) {
        QQuickItem *text = lineText(line);
        QVERIFY(text != nullptr);
        QTRY_COMPARE(text->property("lineCount").toInt(), 2);
        QTRY_VERIFY2(!line->property("elided").toBool(), qPrintable(line->objectName()));
        const QFontMetricsF metrics(text->property("font").value<QFont>());
        // (Text lays a line out on whole pixels, up to one under the
        // font's height.)
        QTRY_VERIFY2(line->height() >= 2 * metrics.height() - 2, qPrintable(line->objectName()));
    };
    const auto gapsText = [&]() {
        return QStringLiteral("%1 %2 %3").arg(sceneRect(artist).top() - sceneRect(title).bottom())
            .arg(sceneRect(album).top() - sceneRect(artist).bottom()).arg(sceneRect(details).top() - sceneRect(album).bottom());
    };
    const auto blockKept = [&]() {
        QVERIFY(settled(panel));
        QTRY_VERIFY2(gapsAreFour(), qPrintable(gapsText()));
        QVERIFY(QQuickTest::qWaitForPolish(window));
        QCOMPARE(sceneRect(seam).top(), seamAt);
        QVERIFY(sceneRect(seam).top() - sceneRect(details).bottom() >= 12.0);
        QCOMPARE(sceneRect(seam).bottom(), sceneRect(header).top());
    };
    bridge->setProperty("currentTrackArtist", QStringLiteral("The Quiet Meridian Chamber Ensemble"));
    wraps(artist);
    blockKept();
    // Under a title on two lines, the artist takes one, elided, with the
    // whole of it in the tooltip.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Lantern Signal Over Harbour"));
    wraps(title);
    QTRY_COMPARE(lineText(artist)->property("lineCount").toInt(), 1);
    QTRY_VERIFY(artist->property("elided").toBool());
    QVERIFY(panel->property("textTruncated").toBool());
    blockKept();
    // A long album wraps under a title and an artist on one line each, its
    // year onto the second line with its end, rather than keep a fifth of
    // its text.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Lantern Signal"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("The Quiet Meridian"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Small Hours at the Harbour"));
    QTRY_COMPARE(lineText(album)->property("lineCount").toInt(), 2);
    QTRY_VERIFY(!album->property("elided").toBool());
    QVERIFY(lineText(album)->property("wholeText").toString().endsWith(QStringLiteral("(2005)")));
    blockKept();
    // A title too long even for two lines is cut off at the second's end.
    bridge->setProperty("currentTrackTitle", QStringLiteral("A Remarkably Long Track Title ").repeated(4));
    QTRY_VERIFY(title->property("elided").toBool());
    QCOMPARE(lineText(title)->property("lineCount").toInt(), 2);
}

void UiMainWindowTest::nowPlayingNarrowBlockIsOneHeightForEveryTrack_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::addColumn<QString>("arrangement");
    QTest::newRow("beside, default font, 1000x720") << 10.0 << 1000 << 720 << QStringLiteral("beside");
    QTest::newRow("beside, 12 pt, 1000x720") << 12.0 << 1000 << 720 << QStringLiteral("beside");
    QTest::newRow("beside, 14 pt, 1000x720") << 14.0 << 1000 << 720 << QStringLiteral("beside");
    // In a 270 px sidebar, too narrow for the title beside the header's
    // cover.
    QTest::newRow("stacked, 18 pt, 1000x720") << 18.0 << 1000 << 720 << QStringLiteral("stacked");
    QTest::newRow("header, 16 pt, 1000x720") << 16.0 << 1000 << 720 << QStringLiteral("header");
    QTest::newRow("header, 24 pt, 1000x720") << 24.0 << 1000 << 720 << QStringLiteral("header");
    QTest::newRow("compact, default font, 1600x700") << 10.0 << 1600 << 700 << QStringLiteral("compact");
    QTest::newRow("header, 24 pt, 1600x1000") << 24.0 << 1600 << 1000 << QStringLiteral("header");
}

// The narrow block is one height for every track, in each of its
// arrangements, so a track change never moves the library (at 24 pt in a
// 1000 by 720 window the lines under the cover took as many lines as the
// track's text needed, and a longer title moved the library's header by
// 28 px): an untagged track's, a title's alone, a common one's and those
// with a long title, artist or album take the same height, the lines that
// wrap wrapping within the room the block keeps for them.
void UiMainWindowTest::nowPlayingNarrowBlockIsOneHeightForEveryTrack() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    QFETCH(QString, arrangement);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QQuickItem *details = named(window, "nowPlayingNarrowDetails");
    QVERIFY(panel && header && seam && details);
    playTrack(bridge);
    QVERIFY(grown(window));
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QCOMPARE(panel->property("narrowStacked").toBool(), arrangement == QStringLiteral("stacked"));
    QCOMPARE(panel->property("compactLayout").toBool(),
             (arrangement == QStringLiteral("compact") || arrangement == QStringLiteral("header"))
                 && !panel->property("narrow").toBool());
    QCOMPARE(panel->property("largeHeader").toBool(), arrangement == QStringLiteral("header"));
    QCOMPARE(panel->property("narrowBeside").toBool(), arrangement == QStringLiteral("beside"));
    // The lines laid out (the column places them when it is polished),
    // the block at the height they give it, and the sidebar at that.
    const auto settle = [&]() {
        QVERIFY(settled(panel));
        QVERIFY(QQuickTest::qWaitForPolish(window));
        QTRY_COMPARE(panel->height(), panel->implicitHeight());
        QVERIFY(QQuickTest::qWaitForPolish(window));
    };
    settle();
    if (QTest::currentTestFailed()) {
        return;
    }
    const auto sample = [&]() {
        return QStringLiteral("panel %1 px, library header at %2").arg(panel->height()).arg(sceneRect(header).top());
    };
    const QString common = sample();
    // The large-font header's block, from the fonts alone: 12 px, its
    // cover, as high as two lines of the title and never under 96 px, 12 px,
    // the artist, the album and the details, 4 px apart, then 12 px and the
    // seam.
    if (arrangement == QStringLiteral("header")) {
        QQuickItem *title = named(window, "nowPlayingNarrowTitle");
        const qreal titleLine = QFontMetricsF(title->property("font").value<QFont>()).height();
        const qreal side = panel->property("headerCoverSize").toReal();
        QVERIFY2(side >= 96.0 && side >= std::round(2 * titleLine) - 1 && side <= std::max(96.0, 2 * titleLine + 6),
                 qPrintable(QStringLiteral("%1 for a title line of %2").arg(side).arg(titleLine)));
        QCOMPARE(named(window, "nowPlayingCover")->width(), side);
        QCOMPARE(panel->height(), 12 + side + 12 + 3 * details->height() + 2 * 4 + 12 + 1);
        QCOMPARE(panel->height(), panel->property("largeHeaderHeight").toReal() + 12 + 1);
    }
    // Ranked in every arrangement: the artist primary, the album secondary
    // (both primary, they read as one name that wrapped), the details
    // tertiary.
    QCOMPARE(colorOf(named(window, "nowPlayingNarrowArtist")), tokenHex(window, "text"));
    QCOMPARE(colorOf(named(window, "nowPlayingNarrowAlbum")), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(details), tokenHex(window, "textTertiary"));
    QVERIFY(tokenHex(window, "textSecondary") != tokenHex(window, "text"));
    // The lines stay in the block, 12 px or more over the seam, or 6 beside
    // a cover that fills the block, whose middle they keep to; and their
    // top is the same for every track, so a track change moves no title.
    const qreal coverSide = named(window, "nowPlayingCover")->width();
    const bool fillsBlock = arrangement == QStringLiteral("beside") && coverSide > 96.0;
    const qreal leastFoot = fillsBlock ? 5.0 : 11.0;
    QQuickItem *titleLine = named(window, "nowPlayingNarrowTitle");
    const qreal titleTop = sceneRect(titleLine).top();
    const auto linesInside = [&]() {
        return sceneRect(seam).top() - sceneRect(details).bottom() >= leastFoot
            && (!fillsBlock || sceneRect(titleLine).top() == titleTop);
    };
    QVERIFY(linesInside());

    // Under the cover, a title on one line leaves its second line to the
    // artist, or else to the album, which wrap into it.
    const bool stacked = arrangement == QStringLiteral("stacked");
    QQuickItem *artist = named(window, "nowPlayingNarrowArtist");
    QQuickItem *album = named(window, "nowPlayingNarrowAlbum");
    QVERIFY(artist && album);
    struct Track {
        const char *name;
        std::function<void()> set;
        QQuickItem *wraps = nullptr;
    };
    const QString longTitle = QStringLiteral("Lantern Signal Over The Harbour At Night");
    const QString longArtist = QStringLiteral("The Quiet Meridian Chamber Ensemble Of The North");
    const QString longAlbum = QStringLiteral("Small Hours at the Harbour Before the Morning Tide");
    const auto playWith = [&](std::initializer_list<std::pair<const char *, QString>> tags) {
        playTrack(bridge);
        for (const auto &[property, value] : tags) {
            bridge->setProperty(property, value);
        }
    };
    const QList<Track> tracks{
        {"untagged", [&]() { clearTrackTags(bridge); }},
        {"title alone",
         [&]() {
             clearTrackTags(bridge);
             bridge->setProperty("currentTrackTitle", QStringLiteral("Morning Tide Field"));
         }},
        {"long title", [&]() { playWith({{"currentTrackTitle", longTitle}}); }},
        {"long artist", [&]() { playWith({{"currentTrackArtist", longArtist}}); }},
        {"long album", [&]() { playWith({{"currentTrackAlbum", longAlbum}}); }},
        {"short title, long artist",
         [&]() {
             playWith({{"currentTrackTitle", QStringLiteral("Tide")},
                       {"currentTrackArtist", QStringLiteral("The Quiet Meridian Chamber Ensemble")}});
         },
         stacked ? artist : nullptr},
        {"short title, long album",
         [&]() {
             playWith({{"currentTrackTitle", QStringLiteral("Tide")},
                       {"currentTrackAlbum", QStringLiteral("Small Hours at the Harbour")}});
         },
         stacked ? album : nullptr},
        {"all long",
         [&]() {
             playWith({{"currentTrackTitle", longTitle.repeated(2)},
                       {"currentTrackArtist", longArtist.repeated(2)},
                       {"currentTrackAlbum", longAlbum.repeated(2)}});
         }},
        {"no cover", [&]() { playWith({{"currentTrackCoverPath", QString()}}); }},
        {"common again", [&]() { playTrack(bridge); }},
    };
    for (const Track &track : tracks) {
        track.set();
        settle();
        if (QTest::currentTestFailed()) {
            return;
        }
        QVERIFY2(sample() == common,
                 qPrintable(QStringLiteral("%1: %2, a common track's %3")
                                .arg(QString::fromLatin1(track.name), sample(), common)));
        QVERIFY2(linesInside(), track.name);
        if (track.wraps != nullptr) {
            QVERIFY2(track.wraps->property("lineCount").toInt() == 2, track.name);
            QVERIFY2(!track.wraps->property("elided").toBool(), track.name);
        }
    }
}

// Beside the cover in the compact layout, the narrow one in a sidebar wide
// enough for the wide one, for the sidebar's height alone, with the default
// font in a short window: the title and the album take two lines where they
// need them, the artist one, elided, with the whole of it in the tooltip,
// and the block keeps the room of them all, six lines, so no track moves
// the library (on one line a long album was cut off and its year lost; two
// lines for the artist too, seven in all, left some 145 px blank under a
// common track). The cover is about three of the lines high: the title's
// first, the gap and two small lines, from 96 to 132 px. Where the album is
// still cut off, its year stays whole after the ellipsis. With a large font
// the compact layout is a header row instead
// (nowPlayingLargeFontCompactIsAHeaderRow).
void UiMainWindowTest::nowPlayingCompactKeepsTheAlbumAndItsYear() {
    const ScopedApplicationFont font(10.0);
    MainWindowFixture fixture(sized(1600, 700));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *title = named(window, "nowPlayingNarrowTitle");
    QQuickItem *artist = named(window, "nowPlayingNarrowArtist");
    QQuickItem *album = named(window, "nowPlayingNarrowAlbum");
    QQuickItem *details = named(window, "nowPlayingNarrowDetails");
    QVERIFY(panel && cover && header && title && artist && album && details);
    playTrack(bridge);
    QVERIFY(grown(window));
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QVERIFY(panel->property("compactLayout").toBool());
    QVERIFY(!panel->property("narrowStacked").toBool());
    QVERIFY(settled(panel));
    const auto geometry = [&]() { return QList<QRectF>{sceneRect(panel), sceneRect(header), sceneRect(cover)}; };
    const QList<QRectF> before = geometry();
    // The room of two lines each for the title and the album, and one each
    // for the artist and the details: six lines.
    const auto lineHeight = [](QQuickItem *line) {
        return QFontMetricsF(line->property("font").value<QFont>()).height();
    };
    const qreal reserved = panel->property("compactLinesHeight").toReal();
    // (Text lays a line out a pixel or two taller than the font's height.)
    const qreal sixLines = 10 + 2 * lineHeight(title) + 4 + 4 * lineHeight(artist);
    QVERIFY2(reserved >= sixLines - 2 && reserved <= sixLines + 4 * 3
                 && reserved < sixLines + lineHeight(artist) - 2,
             qPrintable(QStringLiteral("%1 for six lines of %2").arg(reserved).arg(sixLines)));
    // The cover: the title's line, the gap and two small lines, from 96 to
    // 132 px; at the panel's corner, the lines 12 px after it.
    const qreal coverSide = std::clamp(
        static_cast<qreal>(std::round(lineHeight(title) + 4 + 2 * lineHeight(artist))), 96.0, 132.0);
    QCOMPARE(panel->property("narrowLayoutCoverSize").toReal(), coverSide);
    QCOMPARE(cover->width(), coverSide);
    QCOMPARE(cover->height(), coverSide);
    QCOMPARE(sceneRect(cover).topLeft(), sceneRect(panel).topLeft());
    QCOMPARE(sceneRect(title).left(), sceneRect(cover).right() + 12);
    QCOMPARE(panel->height(), std::max(coverSide, reserved) + 12 + 1);

    // A long title, artist and album: two lines each for the title and the
    // album, the artist on one, elided; the block as it was, which these
    // six lines fill.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Lantern Signal Over The Harbour At Night"));
    const QString longArtist = QStringLiteral("The Quiet Meridian Chamber Ensemble Of The North");
    bridge->setProperty("currentTrackArtist", longArtist);
    const QString longAlbum = QStringLiteral("A Remarkably Long Album Title That Runs On ").repeated(3).trimmed();
    bridge->setProperty("currentTrackAlbum", longAlbum);
    bridge->setProperty("currentTrackYear", 2024);
    for (QQuickItem *line : {title, album}) {
        QTRY_COMPARE(line->property("lineCount").toInt(), 2);
    }
    QTRY_VERIFY(artist->property("elided").toBool());
    QCOMPARE(artist->property("lineCount").toInt(), 1);
    QCOMPARE(artist->property("elide").toInt(), static_cast<int>(Qt::ElideRight));
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), before);
    QVERIFY(sceneRect(details).bottom() <= sceneRect(panel).bottom() - 12);
    QTRY_VERIFY2(std::abs(sceneRect(details).bottom() - (sceneRect(panel).top() + reserved)) <= 1.0,
                 qPrintable(QStringLiteral("details end at %1, the reserved lines at %2")
                                .arg(sceneRect(details).bottom() - sceneRect(panel).top()).arg(reserved)));
    // The whole artist in the tooltip.
    QVERIFY(panel->property("textTruncated").toBool());
    // The album cut off before its year, which shows whole after it, on
    // its second line, within the inset.
    QTRY_VERIFY(album->property("elided").toBool());
    QCOMPARE(album->property("text").toString(), longAlbum);
    QCOMPARE(album->property("wholeText").toString(), longAlbum + QStringLiteral(" (2024)"));
    QQuickItem *tail = itemNamed(album, QStringLiteral("nowPlayingLineTail"));
    QVERIFY(tail != nullptr);
    QTRY_VERIFY(shown(tail));
    QCOMPARE(tail->property("text").toString(), QStringLiteral(" (2024)"));
    const QRectF tailRect = sceneRect(tail);
    const QRectF albumRect = sceneRect(album);
    QVERIFY2(tailRect.right() <= sceneRect(panel).right() - 12 + 1,
             qPrintable(QStringLiteral("%1 past %2").arg(tailRect.right()).arg(sceneRect(panel).right() - 12)));
    // Cut off just before the tail, which ends near the line's end.
    QVERIFY2(tailRect.right() > albumRect.left() + 0.75 * albumRect.width(),
             qPrintable(QStringLiteral("%1 of %2").arg(tailRect.right() - albumRect.left()).arg(albumRect.width())));
    QCOMPARE(tailRect.top(), albumRect.top() + albumRect.height() / 2);
    // Read whole: the album and its year.
    QCOMPARE(QQmlProperty(album, QStringLiteral("Accessible.name"), qmlContext(album)).read().toString(),
             longAlbum + QStringLiteral(" (2024)"));

    // An album that fits with its year: the year right after it.
    const QFontMetricsF metrics(album->property("font").value<QFont>());
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Dusk"));
    QTRY_COMPARE(album->property("wholeText").toString(), QStringLiteral("Dusk (2024)"));
    QCOMPARE(album->property("text").toString(), QStringLiteral("Dusk"));
    QVERIFY(shown(tail) && !album->property("elided").toBool());
    QCOMPARE(album->property("lineCount").toInt(), 1);
    QTRY_VERIFY2(std::abs(sceneRect(tail).left() - sceneRect(album).left()
                          - metrics.horizontalAdvance(QStringLiteral("Dusk"))) <= 1.0,
                 qPrintable(QString::number(sceneRect(tail).left() - sceneRect(album).left())));
    QCOMPARE(sceneRect(tail).top(), sceneRect(album).top());

    // An album that fills its first line: the year at the start of the
    // next, as a wrapped "Album (Year)" would have it.
    const qreal width = album->width();
    QString filling = QStringLiteral("Harbour");
    for (const QString &word : {QStringLiteral(" Harbour"), QStringLiteral(" i")}) {
        while (metrics.horizontalAdvance(filling + word) < width - 2) {
            filling += word;
        }
    }
    QVERIFY(metrics.horizontalAdvance(filling + QStringLiteral(" (2024)")) > width);
    bridge->setProperty("currentTrackAlbum", filling);
    QTRY_COMPARE(album->property("text").toString(), filling);
    QTRY_COMPARE(album->property("lineCount").toInt(), 1);
    QVERIFY(!album->property("elided").toBool());
    QTRY_COMPARE(tail->property("text").toString(), QStringLiteral("(2024)"));
    QCOMPARE(sceneRect(tail).left(), sceneRect(album).left());
    QTRY_VERIFY(sceneRect(tail).top() >= sceneRect(album).bottom() - 1);
    // The line counts its second row; the details are under it.
    QTRY_VERIFY(sceneRect(details).top() >= sceneRect(tail).bottom());
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), before);

    // Untagged: the same block.
    clearTrackTags(bridge);
    QTRY_COMPARE(panel->property("artistAlbumText").toString(), QString());
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), before);
}

void UiMainWindowTest::nowPlayingLargeFontCompactIsAHeaderRow_data() {
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    // The compact layout, in a sidebar wide enough for the wide one.
    QTest::newRow("1600x1000") << 1600 << 1000;
    QTest::newRow("1920x1000") << 1920 << 1000;
    // A narrow sidebar, 360 px (the 96 px cover stood alone on a row over
    // the lines).
    QTest::newRow("1000x720") << 1000 << 720;
}

// With a large font, the narrow layouts are a header row and lines on the
// whole width, in a narrow sidebar and in the compact layout alike (beside
// the cover, the lines left up to 264 px of the panel empty, or cut the
// artist off): the cover at the panel's corner, as high as two lines of the
// title, and the title, on up to two lines, beside it, 12 px after it and
// from its top; 8 px under the cover, at the 12 px inset and on the whole
// width, the artist, "Album (Year)", the album cut off before its year, and
// the details, a line each, 4 px apart, the details one text in one colour
// that leaves out what matters least rather than be cut off. The block is
// one height whatever the track has, so the library's header never moves
// on a track change.
void UiMainWindowTest::nowPlayingLargeFontCompactIsAHeaderRow() {
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    const ScopedApplicationFont font(24.0);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QQuickItem *title = named(window, "nowPlayingNarrowTitle");
    QQuickItem *artist = named(window, "nowPlayingNarrowArtist");
    QQuickItem *album = named(window, "nowPlayingNarrowAlbum");
    QQuickItem *details = named(window, "nowPlayingNarrowDetails");
    QVERIFY(panel && cover && header && seam && title && artist && album && details);
    playTrack(bridge);
    QVERIFY(grown(window));
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QCOMPARE(panel->property("compactLayout").toBool(), !panel->property("narrow").toBool());
    QCOMPARE(panel->property("narrow").toBool(), windowWidth == 1000);
    QVERIFY(panel->property("largeHeader").toBool());
    QVERIFY(!panel->property("narrowStacked").toBool());
    QVERIFY(settled(panel));
    const QRectF panelRect = sceneRect(panel);
    const auto lineHeight = [](QQuickItem *line) {
        return QFontMetricsF(line->property("font").value<QFont>()).height();
    };

    // The details of a common track, and with a genre too long for the
    // line; "Morning Tide Field" beside the cover, whole on up to two lines.
    QTRY_VERIFY2(detailsColourProblem(window, details).isEmpty(), qPrintable(detailsColourProblem(window, details)));
    for (const QString &genre : {QStringLiteral("Classical"), kLongGenre}) {
        bridge->setProperty("currentTrackGenre", genre);
        QTRY_COMPARE(panel->property("narrowDetailsText").toString(), genre + QStringLiteral(" · 2 of 8"));
        QVERIFY(settled(panel));
        const QString cut = detailsCutProblem(panel, details, false, genre, {title, artist, album});
        QVERIFY2(cut.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(genre, cut)));
    }
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    QVERIFY(settled(panel));
    QVERIFY2(!title->property("elided").toBool(), qPrintable(title->property("text").toString()));
    QVERIFY(title->property("lineCount").toInt() <= 2);

    // A title on two lines: the cover spans them.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Lantern Signal Over The Harbour At Night"));
    QTRY_COMPARE(title->property("lineCount").toInt(), 2);
    QVERIFY(settled(panel));
    const qreal side = panel->property("headerCoverSize").toReal();
    QVERIFY2(side >= 96.0, qPrintable(QString::number(side)));
    QVERIFY2(std::abs(side - std::max(96.0, 2 * lineHeight(title))) <= 6.0,
             qPrintable(QStringLiteral("%1 for %2").arg(side).arg(lineHeight(title))));
    QCOMPARE(cover->width(), side);
    QCOMPARE(cover->height(), side);
    const QRectF coverRect = sceneRect(cover);
    // One block at the panel's inset: 12 px from its top and left, and the
    // title the inset after the cover, 12 px (8 px read cramped at 24 pt).
    QCOMPARE(coverRect.topLeft(), panelRect.topLeft() + QPointF(12, 12));
    QTRY_COMPARE(sceneRect(title).left(), coverRect.right() + 12);
    // Its two lines span the cover, or, at the base size beside a 96 px
    // cover, are centred on it.
    QVERIFY2(std::abs(sceneRect(title).top() - (coverRect.top() + std::max(0.0, side - sceneRect(title).height()) / 2))
                 <= 1.0,
             qPrintable(QStringLiteral("the title from %1, %2 px high").arg(sceneRect(title).top())
                            .arg(sceneRect(title).height())));
    // To 8 px from the panel's right side, a ragged edge.
    QCOMPARE(sceneRect(title).right(), panelRect.right() - 8);
    // A text of the title, on one line or two, is centred on the cover (a
    // title on one line hung on the cover's foot, with some 60 px empty
    // over it).
    const auto seatedProblem = [&](QQuickItem *text) -> QString {
        const QRectF rect = sceneRect(text);
        if (rect.left() != coverRect.right() + 12 || std::abs(rect.center().y() - coverRect.center().y()) > 1.0
            || rect.top() < coverRect.top()) {
            return QStringLiteral("\"%1\" on %2 line(s) at %3,%4 to %5, the cover %6 to %7")
                .arg(text->property("text").toString()).arg(text->property("lineCount").toInt())
                .arg(rect.left()).arg(rect.top()).arg(rect.bottom()).arg(coverRect.top()).arg(coverRect.bottom());
        }
        return {};
    };
    QVERIFY2(seatedProblem(title).isEmpty(), qPrintable(seatedProblem(title)));
    // At least a dozen characters beside the cover.
    QVERIFY2(title->width() >= panel->property("narrowLineMinimum").toReal(), qPrintable(QString::number(title->width())));
    // Under the cover, the panel's 12 px inset down (8 px read tight under
    // the 24 pt title and the cover), at the inset and on the whole width;
    // beside it the title stays 8 px after it (seatedProblem), so a long
    // title keeps its words on the line.
    const auto checkFootLines = [&]() {
        QCOMPARE(sceneRect(artist).topLeft(), QPointF(panelRect.left() + 12, coverRect.bottom() + 12));
        QCOMPARE(sceneRect(artist).top() - coverRect.bottom(), coverRect.left() - panelRect.left());
        QCOMPARE(artist->width(), panelRect.width() - 24);
        QCOMPARE(sceneRect(album).topLeft(), QPointF(panelRect.left() + 12, sceneRect(artist).bottom() + 4));
        QCOMPARE(album->width(), panelRect.width() - 24);
        QCOMPARE(sceneRect(details).topLeft(), QPointF(panelRect.left() + 12, sceneRect(album).bottom() + 4));
        QCOMPARE(details->width(), panelRect.width() - 24);
        QCOMPARE(sceneRect(seam).top() - sceneRect(details).bottom(), 12.0);
    };
    checkFootLines();
    QTRY_COMPARE(album->property("wholeText").toString(), QStringLiteral("Small Hours (2005)"));
    QCOMPARE(album->property("lineCount").toInt(), 1);
    QQuickItem *tail = itemNamed(album, QStringLiteral("nowPlayingLineTail"));
    QVERIFY(tail != nullptr && shown(tail));
    QCOMPARE(sceneRect(tail).top(), sceneRect(album).top());
    // The details on one line, never elided.
    QCOMPARE(details->property("lineCount").toInt(), 1);
    QVERIFY(!details->property("elided").toBool());
    const auto geometry = [&]() {
        return QList<QRectF>{sceneRect(panel), sceneRect(header), sceneRect(cover), sceneRect(album)};
    };
    const QList<QRectF> before = geometry();
    QCOMPARE(panel->height(), panel->property("largeHeaderHeight").toReal() + 12 + 1);

    // A long artist and album: the artist on one line, elided; the album
    // cut off before its year, which shows whole after it on the line.
    const QString longArtist = QStringLiteral("The Quiet Meridian Chamber Ensemble Of The North");
    bridge->setProperty("currentTrackArtist", longArtist);
    const QString longAlbum = QStringLiteral("A Remarkably Long Album Title That Runs On ").repeated(3).trimmed();
    bridge->setProperty("currentTrackAlbum", longAlbum);
    bridge->setProperty("currentTrackYear", 2024);
    QTRY_VERIFY(artist->property("elided").toBool());
    QCOMPARE(artist->property("lineCount").toInt(), 1);
    QTRY_VERIFY(album->property("elided").toBool());
    QCOMPARE(album->property("lineCount").toInt(), 1);
    // "Album (Year)" a step below the artist.
    QCOMPARE(colorOf(album), tokenHex(window, "textSecondary"));
    QTRY_COMPARE(tail->property("text").toString(), QStringLiteral(" (2024)"));
    QVERIFY(shown(tail));
    QCOMPARE(sceneRect(tail).top(), sceneRect(album).top());
    QVERIFY2(sceneRect(tail).right() <= panelRect.right() - 12 + 1,
             qPrintable(QStringLiteral("%1 past %2").arg(sceneRect(tail).right()).arg(panelRect.right() - 12)));
    QVERIFY(sceneRect(tail).left() > sceneRect(album).left() + album->width() / 2);
    QVERIFY(panel->property("textTruncated").toBool());
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), before);
    checkFootLines();

    // Under a title on one line, a long artist stays on its own line under
    // the cover, elided, and the title's second line beside the cover stays
    // empty; the block stays.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Dusk"));
    bridge->setProperty("currentTrackArtist", longArtist);
    QTRY_COMPARE(title->property("lineCount").toInt(), 1);
    QTRY_VERIFY(artist->property("elided").toBool());
    QCOMPARE(artist->property("lineCount").toInt(), 1);
    QVERIFY(settled(panel));
    QVERIFY2(seatedProblem(title).isEmpty(), qPrintable(seatedProblem(title)));
    QVERIFY(sceneRect(title).top() > coverRect.top() + side / 5);
    QCOMPARE(geometry(), before);
    checkFootLines();

    // A track change between a title on one line and one on two fades each
    // where it showed: the one going out keeps its place, so nothing in
    // the fade jumps. Once it has faded in, the details are one text in one
    // colour again.
    {
        QObject *animation = panelAnimation(panel, "handoverAnimation");
        const auto incomingTitle = [&]() -> QQuickItem * {
            QQuickItem *window = itemNamed(panel, QStringLiteral("nowPlayingIncomingNarrowTitleWindow"));
            return window != nullptr && !window->childItems().isEmpty() ? window->childItems().first() : nullptr;
        };
        const QString twoLines = QStringLiteral("Lantern Signal Over The Harbour At Night");
        for (const auto &[path, next, was] :
             {std::tuple{QStringLiteral("/music/Artist A/Album A/03.flac"), twoLines, QStringLiteral("Dusk")},
              std::tuple{kTrackPath, QStringLiteral("Dusk"), twoLines}}) {
            bridge->setProperty("currentTrackPath", path);
            bridge->setProperty("currentTrackTitle", next);
            QTRY_VERIFY(animation->property("running").toBool());
            QVERIFY(QMetaObject::invokeMethod(animation, "pause"));
            panel->setProperty("handover", 0.2);
            QQuickItem *outgoing = itemNamed(panel, QStringLiteral("nowPlayingOutgoingNarrowTitle"));
            QQuickItem *incoming = incomingTitle();
            QVERIFY(outgoing != nullptr && incoming != nullptr);
            QCOMPARE(outgoing->property("wholeText").toString(), was);
            QCOMPARE(incoming->property("wholeText").toString(), next);
            QCOMPARE(outgoing->property("lineCount").toInt(), was == twoLines ? 2 : 1);
            QCOMPARE(incoming->property("lineCount").toInt(), next == twoLines ? 2 : 1);
            QVERIFY2(seatedProblem(outgoing).isEmpty(), qPrintable(seatedProblem(outgoing)));
            QVERIFY2(seatedProblem(incoming).isEmpty(), qPrintable(seatedProblem(incoming)));
            QVERIFY(QMetaObject::invokeMethod(animation, "resume"));
            QVERIFY(settled(panel));
            QVERIFY2(seatedProblem(title).isEmpty(), qPrintable(seatedProblem(title)));
            QCOMPARE(geometry(), before);
            QTRY_VERIFY2(detailsColourProblem(window, details).isEmpty(),
                         qPrintable(detailsColourProblem(window, details)));
        }
    }

    // A title alone, a short title, and none of the tags: the same block.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Dusk"));
    for (const char *property : {"currentTrackArtist", "currentTrackAlbum", "currentTrackGenre"}) {
        bridge->setProperty(property, QString());
    }
    bridge->setProperty("currentTrackYear", QVariant());
    bridge->setProperty("currentTrackNumber", 0);
    QTRY_COMPARE(panel->property("narrowAlbumText").toString(), QString());
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), before);
    QVERIFY2(seatedProblem(title).isEmpty(), qPrintable(seatedProblem(title)));
    clearTrackTags(bridge);
    QTRY_COMPARE(panel->property("artistAlbumText").toString(), QString());
    QVERIFY(settled(panel));
    // (checkFootLines is a tagged track's: the album's row stays, empty.)
    QCOMPARE(geometry(), before);

    // A file without tags: its folder and its facts in the first two rows,
    // where a tagged track's artist and album are, footGap under the cover
    // and stackedLineGap apart, with the spare row at the foot (shifted to
    // the middle of the three, the folder stood ~30 px under the cover and
    // moved on a change to a tagged track); in a fade to a tagged track
    // each keeps its place as it goes out, where the tagged track's line
    // comes in.
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Unsorted/track07.flac"));
    bridge->setProperty("currentTrackTitle", QStringLiteral("track07"));
    bridge->setProperty("currentTrackFormatLabel", QStringLiteral("FLAC"));
    bridge->setProperty("durationSeconds", 40.0);
    QTRY_VERIFY(panel->property("untagged").toBool());
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), before);
    QQuickItem *folderText = itemNamed(panel, QStringLiteral("nowPlayingNarrowArtist"));
    QQuickItem *factsText = itemNamed(panel, QStringLiteral("nowPlayingNarrowAlbum"));
    QVERIFY(folderText && factsText);
    QTRY_COMPARE(itemNamed(panel, QStringLiteral("nowPlayingNarrowDetails"))->property("text").toString(), QString());
    QTRY_COMPARE(folderText->property("text").toString(), QStringLiteral("Unsorted"));
    QTRY_COMPARE(factsText->property("text").toString(), QStringLiteral("FLAC \u00b7 0:40"));
    QCOMPARE(QQmlProperty::read(factsText, QStringLiteral("Accessible.name"), qmlContext(factsText)).toString(),
             QStringLiteral("FLAC \u00b7 0:40"));
    QCOMPARE(colorOf(factsText), tokenHex(window, "textTertiary"));
    const QRectF folderRect = sceneRect(folderText);
    const QRectF factsRect = sceneRect(factsText);
    const qreal over = folderRect.top() - (coverRect.bottom() + 12);
    const qreal under = sceneRect(seam).top() - 12 - factsRect.bottom();
    QVERIFY2(std::abs(over) <= 0.5 && under >= folderRect.height(),
             qPrintable(QStringLiteral("%1 over and %2 under").arg(over).arg(under)));
    QVERIFY2(std::abs(factsRect.top() - folderRect.bottom() - 4) <= 1.5,
             qPrintable(QString::number(factsRect.top() - folderRect.bottom())));
    {
        playTrack(bridge);
        QObject *animation = panelAnimation(panel, "handoverAnimation");
        QTRY_VERIFY(animation->property("running").toBool());
        QVERIFY(QMetaObject::invokeMethod(animation, "pause"));
        panel->setProperty("handover", 0.2);
        QQuickItem *goingFolder = itemNamed(panel, QStringLiteral("nowPlayingOutgoingNarrowArtist"));
        QQuickItem *goingFacts = itemNamed(panel, QStringLiteral("nowPlayingOutgoingNarrowAlbum"));
        QVERIFY(goingFolder && goingFacts);
        QCOMPARE(sceneRect(goingFolder).top(), folderRect.top());
        QCOMPARE(sceneRect(goingFacts).top(), factsRect.top());
        // The tagged track's artist and album come in where they went out.
        QCOMPARE(sceneRect(folderText).top(), folderRect.top());
        QCOMPARE(sceneRect(factsText).top(), factsRect.top());
        // Whole inside the windows they go out through.
        for (QQuickItem *text : {goingFolder, goingFacts}) {
            const QRectF through = sceneRect(text->parentItem());
            QVERIFY(through.top() <= sceneRect(text).top() && through.bottom() >= sceneRect(text).bottom());
        }
        QVERIFY(QMetaObject::invokeMethod(animation, "resume"));
    }
    QVERIFY(settled(panel));
    QCOMPARE(geometry(), before);
    checkFootLines();
}

void UiMainWindowTest::nowPlayingTitleIsOneSizePerLayout_data() {
    QTest::addColumn<QSize>("windowSize");
    // Whether the layout's titles are at the base size (headerTitleSmall).
    QTest::addColumn<bool>("small");
    // A title on two lines, cut off, and the words its size fits on its
    // first line.
    QTest::addColumn<QString>("longTitle");
    QTest::addColumn<QString>("firstLine");
    QTest::newRow("1000x720") << QSize(1000, 720) << true
                              << QStringLiteral("A Remarkably Long Title For The Evening Tide")
                              << QStringLiteral("A Remarkably");
    QTest::newRow("1600x1000") << QSize(1600, 1000) << false
                               << QStringLiteral("Morning Tide Field Over The Harbour At Night")
                               << QStringLiteral("Morning Tide");
}

// The large-font header's title is one size for every track of a layout,
// never per track (a long title that stepped down to the base size on its
// own ranked over the artist by its weight alone). Where the room beside the
// heading-size header's cover is under a dozen characters of the
// heading-size title, as at 24 pt in a 1000x720 window, every title is at
// the base size, DemiBold, and the cover spans its two lines, 96 px or
// more; otherwise, as at 24 pt in a 1600x1000 window, at the heading's.
// A short, a medium and a long title, and a title going out in a fade,
// are at that size, centred on the cover, the panel's height and the
// library's place the same. A long title wraps between words; a single
// word wider than the line breaks with a hyphen, U+2010, at least 4
// characters before it and 3 after, the first line full; never anywhere.
void UiMainWindowTest::nowPlayingTitleIsOneSizePerLayout() {
    QFETCH(QSize, windowSize);
    QFETCH(bool, small);
    QFETCH(QString, longTitle);
    QFETCH(QString, firstLine);
    const ScopedApplicationFont font(24.0);
    MainWindowFixture fixture(sized(windowSize.width(), windowSize.height()));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *title = named(window, "nowPlayingNarrowTitle");
    QQuickItem *artist = named(window, "nowPlayingNarrowArtist");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QVERIFY(panel && title && artist && cover && header);
    playTrack(bridge);
    QVERIFY(grown(window));
    QVERIFY(panel->property("largeHeader").toBool());
    QCOMPARE(panel->property("headerTitleSmall").toBool(), small);
    QObject *handover = panelAnimation(panel, "handoverAnimation");
    QVERIFY(handover != nullptr);
    constexpr int kWrap = 4;  // Text.Wrap
    const qreal headingSize = panel->property("titlePointSize").toReal();
    const qreal baseSize = paletteOf(window)->property("basePointSize").toReal();
    QVERIFY(headingSize > baseSize);
    const qreal titleSize = small ? baseSize : headingSize;
    const auto pointSize = [](QQuickItem *text) { return text->property("font").value<QFont>().pointSizeF(); };
    QVERIFY(settled(panel));
    QCOMPARE(pointSize(title), titleSize);
    QFont titleFont = title->property("font").value<QFont>();
    QCOMPARE(titleFont.weight(), QFont::DemiBold);
    const QFontMetricsF metrics(titleFont);
    const qreal width = title->width();
    // 12 px after the cover, the panel's inset, and to 8 px from its right
    // side: the 12 px take no room from the title, which keeps the room it
    // had 8 px after the cover (with the inset on the right too, "A
    // Remarkably" no longer fitted the line, "A / Remarkably…").
    QCOMPARE(sceneRect(title).left(), sceneRect(cover).right() + 12);
    QCOMPARE(sceneRect(title).right(), sceneRect(panel).right() - 8);
    QCOMPARE(width, panel->width() - 12 - cover->width() - 8 - 12);
    // The rule, from the fonts and the width: the room beside the cover a
    // heading-size title would have, against 12 of its average characters.
    QFont headingFont = titleFont;
    headingFont.setPointSizeF(headingSize);
    const qreal headingCover = std::max(96.0, qreal(std::round(2 * QFontMetricsF(headingFont).height())));
    const qreal roomBesideHeading = panel->width() - headingCover - 2 * 12;
    QVERIFY2((roomBesideHeading < 12 * QFontMetricsF(headingFont).averageCharWidth()) == small,
             qPrintable(QStringLiteral("%1 px beside a %2 px cover").arg(roomBesideHeading).arg(headingCover)));
    // The cover spans the title's two lines, and is never under 96 px.
    const qreal side = panel->property("headerCoverSize").toReal();
    QVERIFY2(side >= 96.0 && std::abs(side - std::max(96.0, 2 * metrics.height())) <= 6.0,
             qPrintable(QStringLiteral("%1 for a title line of %2").arg(side).arg(metrics.height())));
    QCOMPARE(cover->width(), side);
    // The title over the artist: larger than it, or as large and heavier.
    QVERIFY(titleSize > pointSize(artist));
    // The seat and the block, which no title changes.
    const QRectF coverRect = sceneRect(cover);
    const qreal panelHeight = panel->height();
    const qreal headerTop = sceneRect(header).top();
    const auto seatedProblem = [&](QQuickItem *text) -> QString {
        const QRectF rect = sceneRect(text);
        if (std::abs(rect.center().y() - coverRect.center().y()) > 1.0 || rect.top() < coverRect.top()) {
            return QStringLiteral("\"%1\" on %2 line(s) from %3 to %4, the cover to %5")
                .arg(text->property("text").toString()).arg(text->property("lineCount").toInt())
                .arg(rect.top()).arg(rect.bottom()).arg(coverRect.bottom());
        }
        return {};
    };
    const auto blockProblem = [&]() -> QString {
        if (panel->height() != panelHeight || sceneRect(header).top() != headerTop
            || sceneRect(cover) != coverRect) {
            return QStringLiteral("the panel %1 px, the library at %2, the cover %3 px")
                .arg(panel->height()).arg(sceneRect(header).top()).arg(sceneRect(cover).height());
        }
        return {};
    };
    // How far the first line's ink reaches from the title's left edge.
    const auto firstLineInk = [&]() {
        const QImage image = window->grabWindow().convertToFormat(QImage::Format_RGB32);
        const qreal scale = qreal(image.width()) / window->width();
        const QRectF rect = sceneRect(title);
        const qreal lineHeight = rect.height() / std::max(1, title->property("lineCount").toInt());
        const QColor background = image.pixelColor(int((rect.right() + 4) * scale), int(rect.top() * scale) + 2);
        int right = -1;
        for (int y = int(rect.top() * scale); y < int((rect.top() + lineHeight) * scale); ++y) {
            for (int x = int(rect.left() * scale); x < int((rect.right() + 1) * scale) && x < image.width(); ++x) {
                const QColor pixel = image.pixelColor(x, y);
                if (std::max({std::abs(pixel.red() - background.red()), std::abs(pixel.green() - background.green()),
                              std::abs(pixel.blue() - background.blue())}) > 60) {
                    right = std::max(right, x);
                }
            }
        }
        return right < 0 ? 0.0 : (right + 1) / scale - rect.left();
    };

    // A short, a medium and a long title, each of another track: one size.
    const QString nextWord = longTitle.mid(firstLine.size() + 1).section(QLatin1Char(' '), 0, 0);
    QVERIFY2(metrics.horizontalAdvance(firstLine) <= width
                 && metrics.horizontalAdvance(firstLine + QLatin1Char(' ') + nextWord) > width,
             qPrintable(QStringLiteral("the column %1 px").arg(width)));
    const QStringList titles{QStringLiteral("Shore"), QStringLiteral("Lantern Signal"), longTitle};
    for (int index = 0; index < titles.size(); ++index) {
        const QString &next = titles.at(index);
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist A/Album A/%1.flac").arg(index + 10));
        bridge->setProperty("currentTrackTitle", next);
        QTRY_COMPARE(title->property("wholeText").toString(), next);
        QVERIFY(settled(panel));
        QVERIFY2(pointSize(title) == titleSize, qPrintable(next));
        QCOMPARE(title->property("font").value<QFont>().weight(), QFont::DemiBold);
        QCOMPARE(title->property("wrapMode").toInt(), kWrap);
        QVERIFY2(seatedProblem(title).isEmpty(), qPrintable(seatedProblem(title)));
        QVERIFY2(blockProblem().isEmpty(), qPrintable(blockProblem()));
    }
    // The long one on two lines, cut off, its first line its first words,
    // whole.
    QCOMPARE(title->property("lineCount").toInt(), 2);
    QVERIFY(!title->property("text").toString().contains(QChar(0x2010)));
    const qreal wanted = metrics.horizontalAdvance(firstLine);
    QTRY_VERIFY2(std::abs(firstLineInk() - wanted) <= 8.0,
                 qPrintable(QStringLiteral("line 0 %1 px, \"%2\" %3").arg(firstLineInk()).arg(firstLine).arg(wanted)));

    // In a fade to a short title, both are at the layout's size.
    const QString shortTitle = QStringLiteral("Dusk");
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist A/Album A/03.flac"));
    bridge->setProperty("currentTrackTitle", shortTitle);
    QTRY_VERIFY(handover->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.2);
    {
        QQuickItem *outgoing = itemNamed(panel, QStringLiteral("nowPlayingOutgoingNarrowTitle"));
        QQuickItem *incomingWindow = itemNamed(panel, QStringLiteral("nowPlayingIncomingNarrowTitleWindow"));
        QVERIFY(outgoing != nullptr && incomingWindow != nullptr && !incomingWindow->childItems().isEmpty());
        QQuickItem *incoming = incomingWindow->childItems().first();
        QCOMPARE(outgoing->property("wholeText").toString(), longTitle);
        QCOMPARE(pointSize(outgoing), titleSize);
        QCOMPARE(incoming->property("text").toString(), shortTitle);
        QCOMPARE(pointSize(incoming), titleSize);
        QVERIFY2(seatedProblem(outgoing).isEmpty(), qPrintable(seatedProblem(outgoing)));
        QVERIFY2(seatedProblem(incoming).isEmpty(), qPrintable(seatedProblem(incoming)));
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(settled(panel));
    QCOMPARE(title->property("text").toString(), shortTitle);
    QCOMPARE(pointSize(title), titleSize);
    QVERIFY2(blockProblem().isEmpty(), qPrintable(blockProblem()));

    // A single very long word: hyphenated where its first line fills, the
    // rest on the second, elided where it does not fit.
    const QString word = QStringLiteral("Supercalifragilisticexpialidociously");
    QVERIFY(metrics.horizontalAdvance(word) > width);
    bridge->setProperty("currentTrackPath", kTrackPath);
    bridge->setProperty("currentTrackTitle", word);
    QTRY_COMPARE(title->property("wholeText").toString(), word);
    QVERIFY(settled(panel));
    QCOMPARE(pointSize(title), titleSize);
    QCOMPARE(title->property("wrapMode").toInt(), kWrap);
    const QString laidOut = title->property("text").toString();
    const QString hyphen = QStringLiteral("‐\n");
    QVERIFY2(laidOut.count(hyphen) == 1, qPrintable(laidOut));
    const QString head = laidOut.section(hyphen, 0, 0);
    const QString rest = laidOut.section(hyphen, 1);
    QVERIFY2(word.startsWith(head + rest.chopped(rest.endsWith(QChar(0x2026)) ? 1 : 0)), qPrintable(laidOut));
    QVERIFY2(head.size() >= 4 && word.size() - head.size() >= 3, qPrintable(laidOut));
    // The first line full: one more character with the hyphen would not fit.
    QVERIFY(metrics.horizontalAdvance(head + QStringLiteral("‐")) <= width);
    QVERIFY(metrics.horizontalAdvance(word.left(head.size() + 1) + QStringLiteral("‐")) > width);
    QCOMPARE(title->property("lineCount").toInt(), 2);
    // The rest elided where it does not fit the second line.
    QCOMPARE(title->property("elided").toBool(), metrics.horizontalAdvance(word.mid(head.size())) > width);
    QCOMPARE(QQmlProperty::read(title, QStringLiteral("Accessible.name"), qmlContext(title)).toString(), word);
    QVERIFY2(seatedProblem(title).isEmpty(), qPrintable(seatedProblem(title)));
    QVERIFY2(blockProblem().isEmpty(), qPrintable(blockProblem()));
}

// A line the panel cuts off has no space before its ellipsis (not "Track
// Title That …"): where Text would elide after a space, the panel elides
// the line itself, "Title That…", on one line under the wide cover and on
// the second of the large-font header's title, its first line as it was;
// the whole of it stays the accessible name and the tooltip's. A line cut
// within a word is elided as Text elides it, "Long Track T…".
// The narrow details keep the track's place on its album before they drop
// its total ("Classical · Track 1" lost it in the narrow sidebar, with the
// room for "Classical · 1 of 4"): the whole, then "Genre · n of m", then
// "Track n of m", then "Track n", each where the one before does not fit;
// "n of m" only after a genre and with a total. The narrow and compact
// layouts' whole is "Genre · n of m" on every track, "Track n of m" with
// no genre (dropping "Track" by width alone, the line read "Jazz · Track 1
// of 8" on one track and "Classical · 1 of 4" on the next); the wide
// layout's keeps "Track". In the narrow sidebar, without a cover, the line
// shows the longest of them that fits.
void UiMainWindowTest::nowPlayingNarrowDetailsKeepTheAlbumPosition() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *details = named(window, "nowPlayingNarrowDetails");
    QVERIFY(panel && details);
    playTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QString());
    window->resize(1000, 720);
    QVERIFY(grown(window));
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QVERIFY(settled(panel));
    QCOMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · 2 of 8"));

    const QFontMetricsF metrics(details->property("font").value<QFont>());
    QChar widest = QLatin1Char('0');
    for (char digit = '1'; digit <= '9'; ++digit) {
        if (metrics.horizontalAdvance(QLatin1Char(digit)) > metrics.horizontalAdvance(widest)) {
            widest = QLatin1Char(digit);
        }
    }
    const auto tabular = [&](QString text) {
        return std::ceil(metrics.horizontalAdvance(text.replace(QRegularExpression(QStringLiteral("[0-9]")), widest)));
    };
    const auto fitting = [&](double width) {
        QVariant result;
        const bool called = QMetaObject::invokeMethod(panel, "_fittingDetails", Q_RETURN_ARG(QVariant, result),
                                                      Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral(" · ")),
                                                      Q_ARG(QVariant, width), Q_ARG(QVariant, false));
        return called ? result.toString() : QStringLiteral("(no call)");
    };
    const QStringList candidates{QStringLiteral("Classical · Track 2 of 8"), QStringLiteral("Classical · 2 of 8"),
                                 QStringLiteral("Track 2 of 8"), QStringLiteral("Track 2")};
    for (qsizetype index = 0; index < candidates.size(); ++index) {
        const double width = tabular(candidates.at(index));
        QCOMPARE(fitting(width), candidates.at(index));
        QCOMPARE(fitting(width - 1.0), candidates.at(std::min(index + 1, candidates.size() - 1)));
    }
    // Without a genre, no bare "2 of 8": the number with "Track".
    bridge->setProperty("currentTrackGenre", QString());
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Track 2 of 8"));
    QCOMPARE(fitting(tabular(QStringLiteral("Track 2 of 8")) - 1.0), QStringLiteral("Track 2"));
    // Without a total, the number alone after the genre.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    bridge->setProperty("currentTrackTotal", 0);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · Track 2"));
    QCOMPARE(fitting(tabular(QStringLiteral("Classical · Track 2")) - 1.0), QStringLiteral("Track 2"));

    // One grammar on every track: a short genre, whose line would fit
    // with "Track", is as a long one's, "Genre · n of m".
    bridge->setProperty("currentTrackTotal", 8);
    for (const QString &each : {QStringLiteral("Jazz"), QStringLiteral("Classical")}) {
        bridge->setProperty("currentTrackGenre", each);
        QTRY_COMPARE(panel->property("narrowDetailsText").toString(), each + QStringLiteral(" · 2 of 8"));
        QVERIFY(settled(panel));
        QCOMPARE(details->property("text").toString(), each + QStringLiteral(" · 2 of 8"));
    }
    QVERIFY(tabular(QStringLiteral("Jazz · Track 2 of 8")) <= details->width());
    QCOMPARE(panel->property("shownNarrowDetailsText").toString(), panel->property("narrowDetailsText").toString());

    // In the narrow sidebar, a genre whose line with "Track" would not fit,
    // but does without: the same line, the album position kept.
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), QStringLiteral("Classical · 2 of 8"));
    QVERIFY(settled(panel));
    const double width = details->width();
    QString genre;
    for (const QString &each : {QStringLiteral("Ambient"), QStringLiteral("Classical"), QStringLiteral("Chamber Music"),
                                QStringLiteral("Modern Classical"), QStringLiteral("Contemporary Jazz"),
                                QStringLiteral("Progressive Rock Fusion")}) {
        if (tabular(each + QStringLiteral(" · Track 2 of 8")) > width && tabular(each + QStringLiteral(" · 2 of 8")) <= width) {
            genre = each;
            break;
        }
    }
    QVERIFY2(!genre.isEmpty(), qPrintable(QString::number(width)));
    bridge->setProperty("currentTrackGenre", genre);
    QTRY_COMPARE(panel->property("narrowDetailsText").toString(), genre + QStringLiteral(" · 2 of 8"));
    QVERIFY(settled(panel));
    QCOMPARE(details->property("text").toString(), genre + QStringLiteral(" · 2 of 8"));
    QVERIFY(!details->property("elided").toBool());
    // Nothing of them left out.
    QCOMPARE(panel->property("shownNarrowDetailsText").toString(), panel->property("narrowDetailsText").toString());
}

void UiMainWindowTest::nowPlayingEllipsisFollowsTheTextWithoutASpace() {
    const QString ellipsis(QChar(0x2026));
    // A run of narrow letters, so that some length of it puts the cut
    // right after "That ".
    const auto findLead = [&](const QFontMetricsF &metrics, qreal width, const QString &before) {
        for (int length = 1; length < 120; ++length) {
            const QString lead(length, QLatin1Char('i'));
            if (!before.isEmpty() && metrics.horizontalAdvance(before + QLatin1Char(' ') + lead) <= width) {
                continue;
            }
            const QString line = lead + QStringLiteral(" Title That Wandering Line");
            if (metrics.elidedText(line, Qt::ElideRight, width) == lead + QStringLiteral(" Title That ") + ellipsis) {
                return lead;
            }
        }
        return QString();
    };

    // Under the wide cover, one line.
    {
        MainWindowFixture fixture(sized());
        QQuickWindow *window = fixture.window;
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        QObject *bridge = bridgeOf(window);
        QQuickItem *panel = named(window, "nowPlayingPanel");
        QQuickItem *title = named(window, "nowPlayingTitle");
        QVERIFY(panel && title);
        playTrack(bridge);
        QVERIFY(grown(window));
        QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
        const QFontMetricsF metrics(title->property("font").value<QFont>());
        const qreal width = title->width();
        const QString lead = findLead(metrics, width, QString());
        QVERIFY2(!lead.isEmpty(), qPrintable(QString::number(width)));
        const QString whole = lead + QStringLiteral(" Title That Wandering Line");
        bridge->setProperty("currentTrackTitle", whole);
        QTRY_COMPARE(title->property("wholeText").toString(), whole);
        QTRY_COMPARE(title->property("text").toString(), lead + QStringLiteral(" Title That") + ellipsis);
        QVERIFY(title->property("elided").toBool());
        QVERIFY(panel->property("textTruncated").toBool());
        QCOMPARE(QQmlProperty::read(title, QStringLiteral("Accessible.name"), qmlContext(title)).toString(), whole);
        QVERIFY(metrics.horizontalAdvance(title->property("text").toString()) <= width);

        // Cut within a word: as Text elides it.
        const QString withinAWord = QStringLiteral("A Remarkably Long Track Title That Will Certainly Need Eliding");
        QVERIFY(!metrics.elidedText(withinAWord, Qt::ElideRight, width).endsWith(QLatin1Char(' ') + ellipsis));
        bridge->setProperty("currentTrackTitle", withinAWord);
        QTRY_COMPARE(title->property("text").toString(), withinAWord);
        QTRY_VERIFY(title->property("truncated").toBool());
        QVERIFY(title->property("elided").toBool());
    }

    // The large-font header's title, on its second line.
    const ScopedApplicationFont font(24.0);
    MainWindowFixture fixture(sized(1000, 720));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *title = named(window, "nowPlayingNarrowTitle");
    QVERIFY(panel && title);
    playTrack(bridge);
    QVERIFY(grown(window));
    QVERIFY(panel->property("largeHeader").toBool());
    const QFontMetricsF metrics(title->property("font").value<QFont>());
    const qreal width = title->width();
    const QString first = QStringLiteral("Remarkably A");
    QVERIFY(metrics.horizontalAdvance(first) <= width);
    const QString lead = findLead(metrics, width, first);
    QVERIFY2(!lead.isEmpty(), qPrintable(QString::number(width)));
    const QString whole = first + QLatin1Char(' ') + lead + QStringLiteral(" Title That Wandering Line");
    bridge->setProperty("currentTrackTitle", whole);
    QTRY_COMPARE(title->property("wholeText").toString(), whole);
    QTRY_COMPARE(title->property("text").toString(),
                 first + QLatin1Char('\n') + lead + QStringLiteral(" Title That") + ellipsis);
    QCOMPARE(title->property("lineCount").toInt(), 2);
    QVERIFY(title->property("elided").toBool());
    QVERIFY(panel->property("textTruncated").toBool());
    QCOMPARE(title->property("font").value<QFont>().pointSizeF(),
             paletteOf(window)->property("basePointSize").toReal());
    QCOMPARE(QQmlProperty::read(title, QStringLiteral("Accessible.name"), qmlContext(title)).toString(), whole);
}

void UiMainWindowTest::levelMeterAtRestShowsItsMarks_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

// With nothing loaded, the visualization's level meter is its trough with
// the -6, -12 and -24 dB marks' ticks on its outer side, each beside its
// 2 px gap and as tall, where they are while it plays, and no lamp, fill or
// peak tick (the blank trough
// of the first run read as a scrollbar's): in either mode and theme, with
// the same geometry. Before any audio, the pane at rest, the trough is the
// plot's floor and the marks faint, on the spectrogram #14062e and
// #433858, on the waveform 1.7:1 on the view, a step over the rest lines,
// as short marks need it to show (the lit
// #30244a trough was the one instrument switched on); with a track, paused
// or stopped, the lit trough, its unlit ladder on the body (a bare trough
// read as a scrollbar's track, H26) and its ticks; and with the queue
// cleared, the floor again.
void UiMainWindowTest::levelMeterAtRestShowsItsMarks() {
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    QCOMPARE(bridge->property("currentTrackPath").toString(), QString());
    QVERIFY(bridge->setProperty("showLevelMeter", true));
    QObject *palette = paletteOf(window);
    const auto visibleMeter = [&]() -> LevelMeterItem * {
        for (QQuickItem *item : itemsNamed(window->contentItem(), QStringLiteral("levelMeter"))) {
            if (item->isVisible() && item->width() > 0 && item->height() > 0) {
                return qobject_cast<LevelMeterItem *>(item);
            }
        }
        return nullptr;
    };
    // The column's track and its marks as drawn, against `trough` and
    // `tick` drawn over it.
    // `unlit` is the unlit ladder's colour, or invalid for none.
    const auto marksProblem = [&](LevelMeterItem *meter, const QColor &trough, const QColor &tick,
                                  const QColor &unlit) -> QString {
        if (meter->property("troughColor").value<QColor>() != trough
            || meter->property("tickColor").value<QColor>() != tick) {
            return QStringLiteral("the trough %1, the ticks %2")
                .arg(hex(meter->property("troughColor").value<QColor>()),
                     hex(meter->property("tickColor").value<QColor>()));
        }
        const QColor ticked = drawnOver(tick, trough);
        const Drawn drawn(window);
        const QRectF rect = sceneRect(meter);
        // On the marks' outer 4 px, and on the body, where no mark crosses.
        const double x = rect.left() + LevelMeterItem::kMarkLeft + LevelMeterItem::kMarkWidth / 2;
        const double bodyX = rect.left() + LevelMeterItem::kBodyLeft + LevelMeterItem::kBodyWidth / 2;
        for (int lane = 0; lane < meter->laneCount(); ++lane) {
            const auto [top, bottom] = LevelMeterItem::laneRows(lane, meter->laneCount(), meter->height());
            const double meterTop = rect.top() + top + LevelMeterItem::kInset;
            const double meterBottom = rect.top() + bottom - LevelMeterItem::kInset;
            const double scaleHeight = meterBottom - meterTop - LevelMeterItem::kLampHeight - 1;
            const std::vector<int> gaps = LevelMeterItem::markGaps(scaleHeight);
            if (gaps.size() != std::size_t(bottom - top < LevelMeterItem::kFullScaleLaneHeight ? 2 : 3)) {
                return QStringLiteral("lane %1 has %2 marks").arg(lane).arg(gaps.size());
            }
            int unlitRows = 0;
            for (double y = meterTop + 0.5; y < meterBottom; y += 1) {
                const bool onTick = std::any_of(gaps.begin(), gaps.end(), [&](int gap) {
                    return y > meterBottom - gap - LevelMeterItem::kMarkGap && y < meterBottom - gap;
                });
                if (!near(drawn.at(x, y), onTick ? ticked : trough)) {
                    return QStringLiteral("lane %1, y %2: %3, wanted %4")
                        .arg(lane).arg(y - rect.top()).arg(hex(drawn.at(x, y)), hex(onTick ? ticked : trough));
                }
                // The body: the trough, or an unlit rung, never beside a
                // mark's tick.
                const bool unlitRung = unlit.isValid() && !onTick && near(drawn.at(bodyX, y), unlit);
                unlitRows += unlitRung ? 1 : 0;
                if (!unlitRung && !near(drawn.at(bodyX, y), trough)) {
                    return QStringLiteral("lane %1, y %2, on the body: %3, wanted %4")
                        .arg(lane).arg(y - rect.top()).arg(hex(drawn.at(bodyX, y)), hex(trough));
                }
            }
            if (unlit.isValid() && unlitRows == 0) {
                return QStringLiteral("lane %1 has no unlit ladder").arg(lane);
            }
        }
        return {};
    };
    // The visible meter of each mode, its plot's colours, and the marks it
    // shows, the lit ones or those at rest.
    const auto checkModes = [&](const QString &phase, bool lit) {
        for (const int mode : {0, 1}) {
            QVERIFY(viewer->setProperty("visualizationMode", mode));
            auto *plotColors =
                qvariant_cast<QObject *>(palette->property(mode == 0 ? "spectrogramPlot" : "waveformPlot"));
            QVERIFY(plotColors != nullptr);
            const QColor trough = plotColors->property(lit ? "meterTrough" : "meterRestTrough").value<QColor>();
            const QColor tick = plotColors->property(lit ? "meterTick" : "meterRestTick").value<QColor>();
            // Before any audio, no ladder.
            const QColor unlit = lit ? plotColors->property("meterUnlit").value<QColor>() : QColor();
            LevelMeterItem *meter = nullptr;
            QTRY_VERIFY((meter = visibleMeter()) != nullptr
                        && meter->property("troughColor").value<QColor>() == trough);
            QVERIFY2(!meter->active(), qPrintable(phase));
            if (!lit) {
                // The plot's floor, and faint marks on it.
                QCOMPARE(trough, plotColors->property("plotBg").value<QColor>());
                if (mode == 0) {
                    QCOMPARE(trough, QColor(QStringLiteral("#14062e")));
                    QCOMPARE(tick.name(), QStringLiteral("#433858"));
                } else {
                    QCOMPARE(trough, palette->property("view").value<QColor>());
                    // The rest lines' 1.3:1 is too faint for short marks:
                    // 1.7:1, as on the spectrogram. Checked in light only:
                    // this fixture's dark palette leaves Kirigami's text
                    // colour light-theme's, which no tint lifts off the view.
                    const double ratio = contrastRatio(tick, trough);
                    const double lineRatio = contrastRatio(palette->property("waveformRestLine").value<QColor>(), trough);
                    QVERIFY2(ratio >= lineRatio && (dark || (ratio >= 1.7 && ratio < 1.85 && ratio > lineRatio)),
                             qPrintable(QStringLiteral("rest marks %1:1, the rest lines %2:1").arg(ratio).arg(lineRatio)));
                }
            } else {
                QVERIFY(trough != plotColors->property("meterRestTrough").value<QColor>());
            }
            QTRY_VERIFY2(marksProblem(meter, trough, tick, unlit).isEmpty(),
                         qPrintable(QStringLiteral("%1, mode %2: %3").arg(phase).arg(mode)
                                        .arg(marksProblem(meter, trough, tick, unlit))));
            // Under the queue's seam the column draws no seam of its own:
            // over the waveform's meter one made the seam 2 px thick there.
            {
                const Drawn drawn(window);
                const QRectF rect = sceneRect(meter);
                const QColor seam = palette->property("seam").value<QColor>();
                const double x = rect.left() + LevelMeterItem::kTrackLeft + LevelMeterItem::kTrackWidth / 2;
                QVERIFY2(near(drawn.at(x, rect.top() - 0.5), seam),
                         qPrintable(QStringLiteral("%1, mode %2: over the meter %3")
                                        .arg(phase).arg(mode).arg(hex(drawn.at(x, rect.top() - 0.5)))));
                QVERIFY2(!near(drawn.at(x, rect.top() + 0.5), seam),
                         qPrintable(QStringLiteral("%1, mode %2: the meter's top row %3, the seam's colour")
                                        .arg(phase).arg(mode).arg(hex(drawn.at(x, rect.top() + 0.5)))));
            }
        }
    };

    // Before any audio: the floor and the rest lines' colour.
    checkModes(QStringLiteral("before audio"), false);
    if (QTest::currentTestFailed()) {
        return;
    }
    // A track, paused and stopped: the lit trough and its ticks.
    for (const QString &state : {QStringLiteral("Paused"), QStringLiteral("Stopped")}) {
        playTrack(bridge, state);
        checkModes(state, true);
        if (QTest::currentTestFailed()) {
            return;
        }
    }
    // The queue cleared: the pane at rest again.
    bridge->setProperty("currentTrackPath", QString());
    checkModes(QStringLiteral("cleared"), false);
}

void UiMainWindowTest::nowPlayingWithoutCoverOrTagsLeavesNoBlankLines_data() {
    QTest::addColumn<bool>("narrow");
    QTest::addColumn<bool>("dark");
    QTest::addColumn<qreal>("pointSize");
    // Each layout once: wide, narrow beside the cover, and the large-font
    // header in the compact layout and in a narrow sidebar; each theme in
    // the wide tile's layout or a small one's.
    QTest::newRow("1600x1000, light, default font") << false << false << 0.0;
    QTest::newRow("1600x1000, dark, default font") << false << true << 0.0;
    QTest::newRow("1000x720, dark, default font") << true << true << 0.0;
    QTest::newRow("1600x1000, dark, 24 pt") << false << true << 24.0;
    QTest::newRow("1000x720, light, 24 pt") << true << false << 24.0;
}

// A track without a cover keeps the cover's place, with a stand-in in it
// (with a layout of its own, a cover that came or went moved the library
// by its height): the plot's floor colour, #14062e in both themes, with
// the same static mark for every track and no text (showing the album, it
// was blank without one), with a 1 px edge in a light theme. A cover that
// goes or comes changes nothing of the panel's geometry: its height, the
// library's place under it, and where its lines are. A track without tags
// shows its file name for a title, and its lines with no text take no room
// beside the narrow cover and keep their rows, empty, under the wide one.
void UiMainWindowTest::nowPlayingWithoutCoverOrTagsLeavesNoBlankLines() {
    QFETCH(bool, narrow);
    QFETCH(bool, dark);
    QFETCH(qreal, pointSize);
    const auto restorePalette = useDarkPalette(dark);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(narrow ? sized(1000, 720) : sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QCOMPARE(paletteOf(window)->property("themeIsDark").toBool(), dark);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QVERIFY(panel && cover && header);

    playTrack(bridge);
    QVERIFY(grown(window));
    const QString artLayout = panel->property("layoutMode").toString();
    const bool wide = artLayout == QStringLiteral("wide");
    // The narrow window, and 24 pt, leave the wide layout too short a
    // library (nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort).
    QCOMPARE(wide, !narrow && pointSize == 0);
    QTRY_VERIFY(cover->property("hasCover").toBool());
    QVERIFY(settled(panel));
    // The panel with a cover: its height, the library under it, the
    // cover's place and every line.
    const auto geometry = [&]() {
        QList<QRectF> rects{sceneRect(panel), sceneRect(header), sceneRect(cover)};
        for (QQuickItem *line : shownLines(window)) {
            rects.push_back(sceneRect(line));
        }
        return rects;
    };
    const QList<QRectF> withCover = geometry();
    QCOMPARE(shownLines(window).size(), wide ? 3 : 4);
    const auto standIn = [&]() {
        return standInProblem(window);
    };

    // The cover gone: its stand-in, the mark, and nothing moves. Drawn, in
    // the wide tile and the small one, at any font size, once the cover
    // has gone out: a mark, never the bare floor (it was blank for three
    // tracks in four).
    bridge->setProperty("currentTrackCoverPath", QString());
    QVERIFY(settled(panel));
    QTRY_VERIFY2(standIn().isEmpty(), qPrintable(standIn()));
    QCOMPARE(panel->property("layoutMode").toString(), artLayout);
    QCOMPARE(geometry(), withCover);
    const qreal ink = standInMarkInk(window);
    QVERIFY2(ink > 0.02 && ink < 0.5, qPrintable(QString::number(ink)));
    QVERIFY2(standInMarkRunsOut(window).isEmpty(), qPrintable(standInMarkRunsOut(window)));

    // A long album, an album named as the track, and none: the same mark
    // for every one, and never text.
    for (const QString &album : {QStringLiteral("An Album With A Remarkably Long Name ").repeated(6).trimmed(),
                                 QStringLiteral("Morning Tide Field"), QString()}) {
        bridge->setProperty("currentTrackAlbum", album);
        QTRY_COMPARE(panel->property("albumText").toString(), album);
        QVERIFY(settled(panel));
        QTRY_VERIFY2(standIn().isEmpty(), qPrintable(standIn()));
        QCOMPARE(colorOf(cover), tokenHex(window, "plotFloor"));
        QCOMPARE(standInMarkInk(window), ink);
    }

    // No tags: the file name for a title, one line, and under it, in the
    // artist's row, the name of its folder, in textSecondary, as a fact of
    // the file rather than a tag (at 24 pt the file name stood alone
    // beside the cover); the same mark.
    clearTrackTags(bridge);
    QTRY_COMPARE(panel->property("artistAlbumText").toString(), QString());
    QCOMPARE(panel->property("detailsText").toString(), QString());
    const QString fileName = QFileInfo(kTrackPath).fileName();
    const QString folderName = QFileInfo(kTrackPath).dir().dirName();
    QCOMPARE(folderName, QStringLiteral("Album A"));
    QVERIFY(settled(panel));
    QTRY_VERIFY2(standIn().isEmpty(), qPrintable(standIn()));
    QCOMPARE(standInMarkInk(window), ink);
    const QList<QQuickItem *> lines = shownLines(window);
    QCOMPARE(lines.size(), 2);
    QTRY_COMPARE(lines.first()->property("text").toString(), fileName);
    QCOMPARE(panel->property("folderText").toString(), folderName);
    QQuickItem *folderLine = lines.at(1);
    QCOMPARE(folderLine->objectName(), wide ? QStringLiteral("nowPlayingArtistAlbum")
                                            : QStringLiteral("nowPlayingNarrowArtist"));
    QCOMPARE(folderLine->property("text").toString(), folderName);
    QCOMPARE(colorOf(folderLine), tokenHex(window, "textSecondary"));
    QCOMPARE(QAccessible::queryAccessibleInterface(folderLine)->text(QAccessible::Name), folderName);
    // As the app gives it: the title is the file's name, with or without
    // its extension, never empty (keyed to an empty title, the folder never
    // showed).
    for (const QString &nameTitle : {QFileInfo(kTrackPath).completeBaseName(), fileName}) {
        bridge->setProperty("currentTrackTitle", nameTitle);
        QTRY_COMPARE(panel->property("folderText").toString(), folderName);
    }
    // A real title, even one without any other tag: no folder.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Morning Tide Field"));
    QTRY_COMPARE(panel->property("folderText").toString(), QString());
    bridge->setProperty("currentTrackTitle", QString());
    QTRY_COMPARE(panel->property("folderText").toString(), folderName);
    QVERIFY(settled(panel));
    QCOMPARE(sceneRect(cover), withCover.at(2));
    if (wide) {
        // Under the cover, the block keeps its height, the empty lines
        // their rows.
        const auto sameBlock = [&]() {
            return sceneRect(panel) == withCover.at(0) && sceneRect(header) == withCover.at(1);
        };
        QTRY_VERIFY2(sameBlock(),
                     qPrintable(QStringLiteral("panel %1 px high (with the cover %2), layout %3, block %4, header at %5")
                                    .arg(panel->height()).arg(withCover.at(0).height())
                                    .arg(panel->property("layoutHeight").toReal())
                                    .arg(panel->property("wideBlockHeight").toReal()).arg(sceneRect(header).top())));
    } else {
        // Beside it, never shorter than the cover and the space under it.
        QVERIFY(panel->height() <= withCover.at(0).height());
        QVERIFY(panel->height() >= cover->height());
    }

    // A track with any tag the lines show never shows its folder: an album
    // alone, a genre alone, a track number alone, a year alone, a title
    // alone.
    const auto noFolderShown = [&]() -> QString {
        if (!panel->property("folderText").toString().isEmpty()) {
            return QStringLiteral("folder text %1").arg(panel->property("folderText").toString());
        }
        for (QQuickItem *line : shownLines(window)) {
            const bool secondary = colorOf(line) == tokenHex(window, "textSecondary")
                && line->objectName() != QStringLiteral("nowPlayingNarrowAlbum");
            if (line->property("text").toString() == folderName || secondary) {
                return QStringLiteral("%1 shows \"%2\"").arg(line->objectName(), line->property("text").toString());
            }
        }
        return {};
    };
    const std::pair<const char *, QVariant> singleTags[] = {
        {"currentTrackAlbum", QStringLiteral("Small Hours")}, {"currentTrackGenre", QStringLiteral("Classical")},
        {"currentTrackNumber", 3}, {"currentTrackYear", 2005}, {"currentTrackTitle", QStringLiteral("Dusk")}};
    for (const auto &[property, value] : singleTags) {
        clearTrackTags(bridge);
        QTRY_COMPARE(panel->property("folderText").toString(), folderName);
        bridge->setProperty(property, value);
        QTRY_VERIFY2(noFolderShown().isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(property, noFolderShown())));
        QVERIFY(settled(panel));
        QVERIFY2(noFolderShown().isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(property, noFolderShown())));
        QCOMPARE(sceneRect(panel), withCover.at(0));
        QCOMPARE(sceneRect(header), withCover.at(1));
    }

    // A track change from a tagged track to a file without tags fades each
    // text in its own colour: the artist goes out in the text colour as
    // the folder comes in in textSecondary.
    {
        playTrack(bridge);
        QVERIFY(settled(panel));
        QObject *handover = panelAnimation(panel, "handoverAnimation");
        const QString line = wide ? QStringLiteral("ArtistAlbum") : QStringLiteral("NarrowArtist");
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/Loose Files/track07.flac"));
        clearTrackTags(bridge);
        QTRY_COMPARE(panel->property("folderText").toString(), QStringLiteral("Loose Files"));
        QTRY_VERIFY(handover->property("running").toBool());
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        panel->setProperty("handover", 0.2);
        QQuickItem *outgoing = itemNamed(panel, QStringLiteral("nowPlayingOutgoing") + line);
        QQuickItem *incomingWindow = itemNamed(panel, QStringLiteral("nowPlayingIncoming%1Window").arg(line));
        QVERIFY(outgoing != nullptr && incomingWindow != nullptr && !incomingWindow->childItems().isEmpty());
        QQuickItem *incoming = incomingWindow->childItems().first();
        QVERIFY(outgoing->property("text").toString().startsWith(QStringLiteral("The Quiet Meridian")));
        QCOMPARE(colorOf(outgoing), tokenHex(window, "text"));
        QCOMPARE(incoming->property("text").toString(), QStringLiteral("Loose Files"));
        QCOMPARE(colorOf(incoming), tokenHex(window, "textSecondary"));
        QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
        QVERIFY(settled(panel));
        QCOMPARE(named(window, qPrintable(QStringLiteral("nowPlaying") + line))->property("text").toString(),
                 QStringLiteral("Loose Files"));
        QCOMPARE(sceneRect(panel), withCover.at(0));
        bridge->setProperty("currentTrackPath", kTrackPath);
    }

    // The tags back, then the cover: all as it was with it.
    playTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QString());
    QVERIFY(settled(panel));
    QTRY_VERIFY2(standIn().isEmpty(), qPrintable(standIn()));
    QTRY_COMPARE(geometry(), withCover);
    bridge->setProperty("currentTrackCoverPath", testCoverUrl());
    QTRY_VERIFY(cover->property("hasCover").toBool());
    QVERIFY(settled(panel));
    QVERIFY(!shown(itemNamed(cover, QStringLiteral("albumArtStandIn"))));
    QCOMPARE(geometry(), withCover);
}

void UiMainWindowTest::nowPlayingHoldsItsHeightOnEveryFrameOfACoverChange_data() {
    nowPlayingWithoutCoverOrTagsLeavesNoBlankLines_data();
}

// The now-playing panel is one height, and the library under it in one
// place, on every frame drawn while a cover goes or comes, as the stand-in
// and the cover cross-fade: for the playing track and for a track without
// one after a track with one, and back, in the wide layout and the narrow
// one, at any font size. Once grown, the panel is as high as its layout
// (the growth's last step reached the sidebar a frame late, and a panel
// read in between was 1 px short of it).
void UiMainWindowTest::nowPlayingHoldsItsHeightOnEveryFrameOfACoverChange() {
    QFETCH(bool, narrow);
    QFETCH(bool, dark);
    QFETCH(qreal, pointSize);
    const auto restorePalette = useDarkPalette(dark);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(narrow ? sized(1000, 720) : sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *header = named(window, "libraryHeader");
    QVERIFY(panel && cover && header);
    QObject *handover = panelAnimation(panel, "handoverAnimation");
    QVERIFY(handover != nullptr);

    // Grown, the panel is as high as its layout, and the library under it.
    // The growth ends here between two frames, as its last step does after
    // its frame's polish: the sidebar lays the whole height out only at
    // the next one, which grown() waits for.
    QObject *growth = panelAnimation(panel, "presenceAnimation");
    QVERIFY(growth != nullptr);
    playTrack(bridge);
    QTRY_VERIFY(panel->property("presence").toDouble() > 0.0);
    QVERIFY(growth->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(growth, "complete"));
    QCOMPARE(panel->property("presence").toDouble(), 1.0);
    QVERIFY(grown(window));
    QCOMPARE(panel->height(), panel->property("layoutHeight").toReal());
    const QRectF panelRect = sceneRect(panel);
    const qreal headerTop = sceneRect(header).top();
    QCOMPARE(headerTop, panelRect.bottom());
    QTRY_VERIFY(cover->property("hasCover").toBool());
    QVERIFY(settled(panel));
    QCOMPARE(sceneRect(panel), panelRect);

    int frames = 0;
    QStringList moved;
    const FrameSampler sampler(window, [&]() {
        ++frames;
        const QRectF now = sceneRect(panel);
        const qreal top = sceneRect(header).top();
        if (now != panelRect || top != headerTop) {
            moved.push_back(QStringLiteral("frame %1: the panel %2 px high at %3, the library's header at %4")
                                .arg(frames).arg(now.height()).arg(now.top()).arg(top));
        }
    });
    // Each change cross-fades over frames of its own, every one sampled.
    const auto crossFaded = [&](int before) {
        return QTest::qWaitFor([&]() { return !handover->property("running").toBool() && frames > before + 2; },
                               5000);
    };
    // The cover gone, then back; another track without a cover after the
    // one with it (its tags the same: the narrow layout's height follows
    // the lines' text, which this leaves out); and the track with the
    // cover after it.
    const std::pair<const char *, std::function<void()>> changes[] = {
        {"the cover gone", [&]() { bridge->setProperty("currentTrackCoverPath", QString()); }},
        {"the cover back", [&]() { bridge->setProperty("currentTrackCoverPath", testCoverUrl()); }},
        {"a track without one",
         [&]() {
             bridge->setProperty("currentTrackPath", QStringLiteral("/music/Artist A/Album A/03.flac"));
             bridge->setProperty("currentTrackCoverPath", QString());
         }},
        {"the track with one", [&]() { playTrack(bridge); }},
    };
    bool hasCover = true;
    for (const auto &[change, apply] : changes) {
        const int before = frames;
        apply();
        hasCover = !hasCover;
        QVERIFY2(handover->property("running").toBool(), change);
        QVERIFY2(crossFaded(before), change);
        QCOMPARE(cover->property("hasCover").toBool(), hasCover);
    }

    QVERIFY2(moved.isEmpty(), qPrintable(moved.join(QLatin1Char('\n'))));
    QCOMPARE(sceneRect(panel), panelRect);
    QCOMPARE(sceneRect(header).top(), headerTop);
}

void UiMainWindowTest::nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("shortHeight");
    QTest::addColumn<int>("tallHeight");
    // The default font: wide in 1000 px of height, narrow in 700.
    QTest::newRow("default font") << 0.0 << 1600 << 700 << 1000;
    // 24 pt: narrow in 1000 px of height, wide in 1400.
    QTest::newRow("24 pt") << 24.0 << 1600 << 1000 << 1400;
    // The wide panel's larger cover at 1920 px, under a band whose lane
    // keeps 3 px clear of the focus rings: wide from 1440.
    QTest::newRow("24 pt, 1920 px") << 24.0 << 1920 << 1000 << 1440;
}

// Where the wide layout would leave the library fewer than six rows under
// its header, as with a large font or in a short window, the now-playing
// panel takes its narrow layout, with its 96 px cover (at 24 pt in 1600 by
// 1000 the library showed three rows). It is decided by the font, the
// sidebar's width and height, not by the layout it chooses nor by the
// track, so it does not switch back and forth: once as the window gets
// short, once as it gets tall again, never for a track. The sidebar being
// wide, the cover sits beside the lines, however far past its foot they
// run, not on a row of its own above them (at 24 pt it stood alone, with
// its width empty beside it).
void UiMainWindowTest::nowPlayingTakesTheNarrowLayoutWhereTheLibraryWouldBeShort() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, shortHeight);
    QFETCH(int, tallHeight);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, tallHeight));
    QQuickWindow *window = fixture.window;
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
    // layout.
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
    QVERIFY(settled(panel));
    QTRY_VERIFY2(std::abs(panel->height() - reserved) <= 1.0,
                 qPrintable(QStringLiteral("%1 against %2").arg(panel->height()).arg(reserved)));
    QVERIFY2(rows() >= 6, qPrintable(QString::number(rows())));

    // Short: narrow, the compact cover, 96 px with the default font and
    // with a large one the header's, as high as two lines of the title,
    // and at least six rows of the library.
    window->resize(windowWidth, shortHeight);
    QTRY_COMPARE(window->height(), shortHeight);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QVERIFY(panel->property("shortHeight").toBool());
    const qreal coverSide = panel->property("narrowLayoutCoverSize").toReal();
    QVERIFY2(pointSize > 0 ? coverSide > 96.0 : coverSide == 96.0, qPrintable(QString::number(coverSide)));
    QCOMPARE(panel->property("largeHeader").toBool(), pointSize > 0);
    QTRY_COMPARE(cover->width(), coverSide);
    QCOMPARE(panel->property("wideReservedHeight").toReal(), reserved);
    QTRY_VERIFY2(rows() >= 6, qPrintable(QString::number(rows())));
    QTest::qWait(100);
    QCOMPARE(switches.count(), 1);
    // The lines beside the cover, 12 px after it, and the seam 12 px under
    // the lowest of them or the cover. The cover is at the panel's corner,
    // as in a narrow sidebar and as the wide one is, at every font size.
    QVERIFY(!panel->property("narrow").toBool());
    QVERIFY(!panel->property("narrowStacked").toBool());
    QVERIFY(grown(window));
    QVERIFY(settled(panel));
    const QRectF tile = sceneRect(cover);
    QQuickItem *seam = named(window, "nowPlayingSeam");
    QVERIFY(seam != nullptr);
    qreal lowest = tile.bottom();
    for (const char *name : {"nowPlayingNarrowTitle", "nowPlayingNarrowArtist", "nowPlayingNarrowAlbum",
                             "nowPlayingNarrowDetails"}) {
        QQuickItem *line = named(window, name);
        QVERIFY2(line != nullptr && shown(line), name);
        // With a large font, all but the title under the header row, at
        // the panel's inset.
        const bool under = pointSize > 0 && qstrcmp(name, "nowPlayingNarrowTitle") != 0;
        QTRY_COMPARE(sceneRect(line).left(), under ? sceneRect(panel).left() + 12 : tile.right() + 12);
        // The large-font header's title to 8 px from the panel's right side.
        QVERIFY2(sceneRect(line).right() <= sceneRect(panel).right() - (pointSize > 0 && !under ? 8 : 12), name);
        lowest = std::max(lowest, sceneRect(line).bottom());
    }
    QQuickItem *narrowTitle = named(window, "nowPlayingNarrowTitle");
    // Beside the large-font header's cover, centred on it, a title on one
    // line too; beside the narrow cover, from its top.
    if (pointSize > 0) {
        QVERIFY(sceneRect(narrowTitle).top() >= sceneRect(panel).top());
        QVERIFY2(std::abs(sceneRect(narrowTitle).center().y() - tile.center().y()) <= 1.0,
                 qPrintable(QStringLiteral("%1 to %2").arg(sceneRect(narrowTitle).bottom()).arg(tile.bottom())));
    } else {
        QCOMPARE(sceneRect(narrowTitle).top(), sceneRect(panel).top() + panel->property("narrowTextTop").toReal());
    }
    QVERIFY(sceneRect(narrowTitle).top() < tile.bottom());
    QCOMPARE(tile.topLeft(), coverCorner(panel));
    QCOMPARE(sceneRect(narrowTitle).left(), tile.right() + 12);
    if (pointSize > 0) {
        // The lines run past the cover's foot at 24 pt.
        QVERIFY2(lowest > tile.bottom(), qPrintable(QString::number(lowest - tile.bottom())));
    }
    // The block keeps the room of every line at its most, two lines for
    // the title and the album and one for the artist, then 12 px.
    QVERIFY(panel->property("compactLayout").toBool());
    const qreal reservedLines = pointSize > 0 ? panel->property("largeHeaderHeight").toReal()
                                              : std::max(coverSide, panel->property("compactLinesHeight").toReal());
    QTRY_COMPARE(sceneRect(seam).top(), sceneRect(panel).top() + reservedLines + 12);
    QVERIFY(sceneRect(seam).top() - lowest >= 12.0);

    // A track, with tags and then without, switches nothing; nor does
    // nothing playing, which is the compact row in it too.
    clearTrackTags(bridge);
    QTRY_COMPARE(panel->property("artistAlbumText").toString(), QString());
    playTrack(bridge);
    clearPlayback(bridge);
    QVERIFY(settled(panel));
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    playTrack(bridge);
    QVERIFY(grown(window));
    QTest::qWait(100);
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("narrow"));
    QCOMPARE(switches.count(), 1);

    // Tall again: wide again, once.
    window->resize(windowWidth, tallHeight);
    QTRY_COMPARE(window->height(), tallHeight);
    QTRY_COMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QTRY_COMPARE(cover->width(), panel->width());
    QTest::qWait(100);
    QCOMPARE(switches.count(), 2);
}

// Show in Library brings the playing track's row into the library's view
// (at 24 pt in a 1920 by 1200 window the tree opened but the row stayed out
// of sight, as the view held fewer rows than the tree had above it). The
// backend sends the rows of a collapsed artist only once it is told to open
// it, so the reveal ends as that tree is applied: the row is centred then,
// and the view's place from before the tree, which a tree otherwise keeps,
// must not be put back over it.
void UiMainWindowTest::showInLibraryBringsThePlayingRowIntoView() {
    const ScopedApplicationFont font(24.0);
    MainWindowFixture fixture(sized(1920, 1200));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    LibraryTreeModel &libraryModel = fixture.libraryModel;
    QObject *bridge = bridgeOf(window);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(view != nullptr);

    // Sixty artists of one album each, collapsed, as the backend sends
    // them: an artist's album and its track only once `open` says so.
    constexpr int artists = 60;
    constexpr int playing = 47;
    const auto artistKey = [](int index) { return QStringLiteral("artist|Artist %1").arg(index, 2, 10, QLatin1Char('0')); };
    const QString albumKey = QStringLiteral("album|Artist %1|Album").arg(playing);
    const QString trackPath = QStringLiteral("/music/Artist %1/Album/01.flac").arg(playing);
    const QString trackKey = QStringLiteral("track|") + trackPath;
    const auto tree = [&](bool open) {
        QList<TreeRow> rows;
        const QString artist = QStringLiteral("Artist");
        for (int index = 0; index < artists; ++index) {
            const QString folder = QStringLiteral("/music/Artist %1").arg(index);
            rows.push_back(
                {1, 0, 0, 1, 0, QStringLiteral("Artist %1 (1)").arg(index), artistKey(index), artist, folder, {}});
            if (open && index == playing) {
                rows.push_back(
                    {2, 1, 0, 1, 0, QStringLiteral("Album"), albumKey, artist, folder + QStringLiteral("/Album"), {}});
                rows.push_back({4, 2, 1, 0, 0, QStringLiteral("First"), trackKey, artist, trackPath, trackPath});
            }
        }
        return treeOf(rows);
    };
    const auto frame = [&](int version, bool open) {
        return QMetaObject::invokeMethod(bridge, "libraryTreeFrameReceived", Q_ARG(int, version),
                                         Q_ARG(QVariant, QVariant(tree(open))));
    };
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    // The track plays, and the now-playing panel has its place.
    bridge->setProperty("currentTrackPath", trackPath);
    bridge->setProperty("currentTrackTitle", QStringLiteral("First"));
    bridge->setProperty("currentTrackTreeKeys", QStringList{artistKey(playing), albumKey});
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QVERIFY(grown(window));
    QVERIFY(frame(1, false));
    QTRY_COMPARE(view->property("count").toInt(), artists);
    const qreal rowHeight = paletteOf(window)->property("rowHeight").toReal();
    // The view shows the tree's start, and far fewer rows than are above
    // the playing artist's.
    QCOMPARE(view->property("contentY").toReal(), 0.0);
    QVERIFY2(view->height() < (playing - 4) * rowHeight, qPrintable(QString::number(view->height())));

    // Show in Library asks for the playing track's row: its artist and
    // album open, which the model does not have the rows of yet.
    QVERIFY(QMetaObject::invokeMethod(bridge, "libraryRevealRequested",
                                      Q_ARG(QVariant, QVariant(QStringList{artistKey(playing), albumKey})),
                                      Q_ARG(QString, trackKey)));
    QCOMPARE(libraryModel.indexForSelectionKey(trackKey), -1);
    QVERIFY(libraryModel.property("revealPending").toBool());
    // The backend's answer: the tree with them.
    QVERIFY(frame(2, true));
    QTRY_VERIFY(libraryModel.indexForSelectionKey(trackKey) >= 0);
    QTRY_VERIFY(!libraryModel.property("parsing").toBool());
    const int row = libraryModel.indexForSelectionKey(trackKey);
    QCOMPARE(row, playing + 2);

    // The row is in the view, whole, now and once the tree has settled.
    const auto inView = [&]() {
        const qreal top = row * rowHeight - view->property("contentY").toReal();
        return top >= 0 && top + rowHeight <= view->height() + 0.5;
    };
    QTRY_VERIFY2(inView(), qPrintable(QStringLiteral("contentY %1, the row at %2, the view %3 high")
                                          .arg(view->property("contentY").toReal()).arg(row * rowHeight)
                                          .arg(view->height())));
    QTest::qWait(200);
    QVERIFY2(inView(), qPrintable(QStringLiteral("contentY %1 after the tree settled")
                                      .arg(view->property("contentY").toReal())));
    // As drawn: the row's item is the playing track's, amber.
    QQuickItem *item = viewRowAt(view, row);
    QVERIFY(item != nullptr);
    QTRY_VERIFY(item->property("isNow").toBool());
    QVERIFY(sceneRect(item).top() >= sceneRect(view).top() - 0.5);
    QVERIFY(sceneRect(item).bottom() <= sceneRect(view).bottom() + 0.5);
    // And selected, as Show in Library leaves it.
    QVERIFY(item->property("rowSelected").toBool());
}

// Show in Library on a track in a library folder the user collapsed opens
// that folder, and leaves the track's row selected in view (#629).
void UiMainWindowTest::showInLibraryOpensTheCollapsedLibraryFolderOfTheTrack() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    LibraryTreeModel &libraryModel = fixture.libraryModel;
    QObject *bridge = bridgeOf(window);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(view != nullptr);

    const QString rootA = QStringLiteral("root|/music-a");
    const QString rootB = QStringLiteral("root|/music-b");
    const QString artistB = QStringLiteral("artist|/music-b|Artist B");
    const QString albumB = QStringLiteral("album|/music-b|Artist B|Album");
    const QString trackPath = QStringLiteral("/music-b/Artist B/Album/01.flac");
    const QString trackKey = QStringLiteral("track|") + trackPath;
    // The backend sends artist B's rows once it is told to open it.
    const auto frame = [&](int version, bool open) {
        QList<TreeRow> rows{
            {0, 0, 0, 1, 0, QStringLiteral("/music-a"), rootA, {}, QStringLiteral("/music-a"), {}},
            {1, 1, 0, 1, 0, QStringLiteral("Artist A (1)"), QStringLiteral("artist|/music-a|Artist A"),
             QStringLiteral("Artist A"), QStringLiteral("/music-a/Artist A"), {}},
            {0, 0, 0, 1, 0, QStringLiteral("/music-b"), rootB, {}, QStringLiteral("/music-b"), {}},
            {1, 1, 0, 1, 0, QStringLiteral("Artist B (1)"), artistB, QStringLiteral("Artist B"),
             QStringLiteral("/music-b/Artist B"), {}},
        };
        if (open) {
            rows.push_back({2, 2, 0, 1, 0, QStringLiteral("Album"), albumB, QStringLiteral("Artist B"),
                            QStringLiteral("/music-b/Artist B/Album"), {}});
            rows.push_back({4, 3, 1, 0, 0, QStringLiteral("First"), trackKey, QStringLiteral("Artist B"),
                            trackPath, trackPath});
        }
        return QMetaObject::invokeMethod(bridge, "libraryTreeFrameReceived", Q_ARG(int, version),
                                         Q_ARG(QVariant, QVariant(treeOf(rows))));
    };
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music-a"), QStringLiteral("/music-b")});
    QVERIFY(frame(1, false));
    QTRY_COMPARE(view->property("count").toInt(), 4);
    // The user collapses both library folders.
    libraryModel.toggleKey(rootA);
    libraryModel.toggleKey(rootB);
    QTRY_COMPARE(view->property("count").toInt(), 2);

    // Show in Library asks with the keys the backend holds expanded.
    QVERIFY(QMetaObject::invokeMethod(bridge, "libraryRevealRequested",
                                      Q_ARG(QVariant, QVariant(QStringList{artistB, albumB})),
                                      Q_ARG(QString, trackKey)));
    QVERIFY(libraryModel.property("revealPending").toBool());
    QVERIFY(frame(2, true));
    QTRY_VERIFY(libraryModel.indexForSelectionKey(trackKey) >= 0);
    QTRY_VERIFY(!libraryModel.property("parsing").toBool());
    const int row = libraryModel.indexForSelectionKey(trackKey);
    // Folder B opened; folder A stayed collapsed above it.
    QCOMPARE(row, 4);
    QCOMPARE(libraryModel.indexForSelectionKey(QStringLiteral("artist|/music-a|Artist A")), -1);

    QTRY_VERIFY(viewRowAt(view, row) != nullptr);
    QQuickItem *item = viewRowAt(view, row);
    QTRY_VERIFY(sceneRect(item).top() >= sceneRect(view).top() - 0.5
                && sceneRect(item).bottom() <= sceneRect(view).bottom() + 0.5);
    QTRY_VERIFY(item->property("rowSelected").toBool());

    // Collapsing the folder again still works.
    libraryModel.toggleKey(rootB);
    QTRY_COMPARE(view->property("count").toInt(), 2);
}

void UiMainWindowTest::showInLibraryLandsOnTheRowGrid_data() {
    QTest::addColumn<qreal>("points");
    QTest::addColumn<bool>("fromSearch");
    QTest::newRow("the queue's, 10 pt") << 10.0 << false;
    QTest::newRow("the queue's, 24 pt") << 24.0 << false;
    QTest::newRow("the search's, 10 pt") << 10.0 << true;
    QTest::newRow("the search's, 24 pt") << 24.0 << true;
}

// Show in Library, the queue's, which centres the row, and the search's,
// which brings it into the view, lands on the row grid: the top row is
// whole under the header (6 px of it was cut at 10 pt, and at 24 pt it was
// cut through its letters), the revealed row whole in the view. The tree's
// last row is the exception: to show it whole the view is at its end.
void UiMainWindowTest::showInLibraryLandsOnTheRowGrid() {
    QFETCH(qreal, points);
    QFETCH(bool, fromSearch);
    const ScopedApplicationFont font(points);
    MainWindowFixture fixture(sized(1600, 1026));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *view = named(window, "libraryView");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(view != nullptr && controller != nullptr);
    constexpr int count = 90;
    fixture.libraryModel.setLibraryTreeFromBinary(manyArtistsTree(count));
    QTRY_COMPARE(view->property("count").toInt(), count);
    const qreal rowHeight = paletteOf(window)->property("rowHeight").toReal();
    QVERIFY2(std::fmod(view->height(), rowHeight) >= 1.0, "the view holds whole rows, which proves nothing");

    const auto reveal = [&](int index) {
        const QString key = QStringLiteral("artist|Artist %1").arg(index, 2, 10, QLatin1Char('0'));
        return fromSearch
            ? QMetaObject::invokeMethod(controller, "requestRevealForSearchRow",
                                        Q_ARG(QVariant, QVariant(QVariantMap{{QStringLiteral("artistKey"), key}})))
            : QMetaObject::invokeMethod(controller, "requestBridgeReveal", Q_ARG(QVariant, QVariant(QStringList{})),
                                        Q_ARG(QVariant, key));
    };
    const auto wholeInView = [&](int index) {
        QQuickItem *row = viewRowAt(view, index);
        return row != nullptr && sceneRect(row).top() >= sceneRect(view).top() - 0.5
            && sceneRect(row).bottom() <= sceneRect(view).bottom() + 0.5;
    };
    // Down from the top, up again, near the end and one before the last.
    for (const int index : {40, 12, 83, count - 2, 1}) {
        QVERIFY(reveal(index));
        QTRY_VERIFY2(wholeInView(index), qPrintable(QString::number(index)));
        // After what the reveal runs once the view has laid out.
        QCoreApplication::processEvents();
        QVERIFY(wholeInView(index));
        const qreal above = sceneRect(viewRowAt(view, index)).top() - sceneRect(view).top();
        const qreal off = above - rowHeight * std::round(above / rowHeight);
        QVERIFY2(std::abs(off) <= 0.5,
                 qPrintable(QStringLiteral("row %1 is %2 px off the row grid").arg(index).arg(off)));
    }
    QVERIFY(reveal(count - 1));
    QTRY_VERIFY(wholeInView(count - 1));
}

// Show in Library still waiting for its track's rows gives way to the user
// browsing the library: the user closes the track's library folder and
// opens an album in another, and when the rows come, the view stays where
// the user is, the folder stays closed, and the track is not selected
// (#649).
void UiMainWindowTest::showInLibraryGivesWayToTheUserBrowsingTheLibrary() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    LibraryTreeModel &libraryModel = fixture.libraryModel;
    QObject *bridge = bridgeOf(window);
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(controller != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(view != nullptr);

    const QString rootA = QStringLiteral("root|/music-a");
    const QString artistA = QStringLiteral("artist|/music-a|Artist A");
    const QString albumA = QStringLiteral("album|/music-a|Artist A|Album");
    const QString trackPathA = QStringLiteral("/music-a/Artist A/Album/01.flac");
    const QString trackKeyA = QStringLiteral("track|") + trackPathA;
    const QString rootB = QStringLiteral("root|/music-b");
    const QString artistB = QStringLiteral("artist|/music-b|Artist B");
    const QString albumB = QStringLiteral("album|/music-b|Artist B|Album");
    const QString trackPathB = QStringLiteral("/music-b/Artist B/Album/01.flac");
    constexpr int kFillerArtists = 40;
    // Folder B, with more artists than the view shows, above folder A. The
    // backend sends the rows of each artist and album it was told is open.
    const auto frame = [&](int version, bool openA, bool openArtistB, bool openAlbumB) {
        QList<TreeRow> rows{
            {0, 0, 0, 1, 0, QStringLiteral("/music-b"), rootB, {}, QStringLiteral("/music-b"), {}},
            {1, 1, 0, 1, 0, QStringLiteral("Artist B (1)"), artistB, QStringLiteral("Artist B"),
             QStringLiteral("/music-b/Artist B"), {}},
        };
        if (openArtistB) {
            rows.push_back({2, 2, 0, 1, 0, QStringLiteral("Album"), albumB, QStringLiteral("Artist B"),
                            QStringLiteral("/music-b/Artist B/Album"), {}});
            if (openAlbumB) {
                rows.push_back({4, 3, 1, 0, 0, QStringLiteral("First"), QStringLiteral("track|") + trackPathB,
                                QStringLiteral("Artist B"), trackPathB, trackPathB});
            }
        }
        for (int i = 0; i < kFillerArtists; ++i) {
            const QString name = QStringLiteral("Other Artist %1").arg(i, 2, 10, QLatin1Char('0'));
            rows.push_back({1, 1, 0, 1, 0, name, QStringLiteral("artist|/music-b|") + name, name,
                            QStringLiteral("/music-b/") + name, {}});
        }
        rows.push_back({0, 0, 0, 1, 0, QStringLiteral("/music-a"), rootA, {}, QStringLiteral("/music-a"), {}});
        rows.push_back({1, 1, 0, 1, 0, QStringLiteral("Artist A (1)"), artistA, QStringLiteral("Artist A"),
                        QStringLiteral("/music-a/Artist A"), {}});
        if (openA) {
            rows.push_back({2, 2, 0, 1, 0, QStringLiteral("Album"), albumA, QStringLiteral("Artist A"),
                            QStringLiteral("/music-a/Artist A/Album"), {}});
            rows.push_back({4, 3, 1, 0, 0, QStringLiteral("First"), trackKeyA, QStringLiteral("Artist A"),
                            trackPathA, trackPathA});
        }
        return QMetaObject::invokeMethod(bridge, "libraryTreeFrameReceived", Q_ARG(int, version),
                                         Q_ARG(QVariant, QVariant(treeOf(rows))));
    };
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music-b"), QStringLiteral("/music-a")});
    QVERIFY(frame(1, false, false, false));
    QTRY_COMPARE(view->property("count").toInt(), kFillerArtists + 4);
    QTRY_VERIFY(!libraryModel.property("parsing").toBool());
    QCOMPARE(view->property("contentY").toReal(), 0.0);

    // Show in Library on track A, whose rows the backend has not sent yet.
    QVERIFY(QMetaObject::invokeMethod(bridge, "libraryRevealRequested",
                                      Q_ARG(QVariant, QVariant(QStringList{artistA, albumA})),
                                      Q_ARG(QString, trackKeyA)));
    QVERIFY(libraryModel.property("revealPending").toBool());

    // The user closes folder A, then opens artist B and its album, and the
    // trees with their rows, and those of track A, come.
    QVERIFY(QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, rootA)));
    QVERIFY(!libraryModel.property("revealPending").toBool());
    QCOMPARE(controller->property("pendingRevealKey").toString(), QString());
    QVERIFY(QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, artistB)));
    QVERIFY(frame(2, true, true, false));
    QTRY_VERIFY(libraryModel.indexForSelectionKey(albumB) >= 0);
    QVERIFY(QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, albumB)));
    QVERIFY(frame(3, true, true, true));
    QTRY_VERIFY(libraryModel.indexForSelectionKey(QStringLiteral("track|") + trackPathB) >= 0);
    QTRY_VERIFY(!libraryModel.property("parsing").toBool());

    // Folder A stays closed, the view stays at the top, where the user is,
    // and track A is not selected.
    QCOMPARE(view->property("count").toInt(), kFillerArtists + 5);
    QCOMPARE(libraryModel.indexForSelectionKey(artistA), -1);
    QCOMPARE(libraryModel.indexForSelectionKey(trackKeyA), -1);
    QVERIFY(!libraryModel.property("revealPending").toBool());
    QVERIFY(!libraryModel.isKeySelected(trackKeyA));
    QVERIFY(controller->property("selectedSelectionKey").toString() != trackKeyA);
    QCOMPARE(view->property("contentY").toReal(), 0.0);
}

// A track whose tag lacks its title and artist shows its file name as the
// title and its album alone, with no stand-in for the artist (#560).
void UiMainWindowTest::nowPlayingShowsTheFileNameOfATrackWithoutTitleOrArtist() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
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

// The cover is a keyboard stop while a track plays: Tab reaches it
// after the transport and leaves it for the library, a 2 px ring with a
// 1 px casing inside it shows its focus, over any art, only while the
// keyboard is in use, the Menu key and Shift+F10 open
// its context menu, and the pointer lays a faint veil on it.
void UiMainWindowTest::nowPlayingCoverIsAKeyboardStop() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(fixture.libraryModel, kAllExpanded);
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
    // The ring: 2 px, inside the tile, in the focus ring's colour, with
    // 1 px of the view's inside it.
    QTRY_VERIFY(shown(ring));
    QCOMPARE(QQmlProperty(ring, QStringLiteral("border.width")).read().toReal(), 2.0);
    QCOMPARE(hex(QQmlProperty(ring, QStringLiteral("border.color")).read().value<QColor>()),
             tokenHex(window, "focusRing"));
    QCOMPARE(sceneRect(ring), sceneRect(cover));
    QQuickItem *casing = named(window, "albumArtFocusCasing");
    QVERIFY(shown(casing));
    QCOMPARE(sceneRect(casing), sceneRect(cover).adjusted(2, 2, -2, -2));
    QCOMPARE(QQmlProperty(casing, QStringLiteral("border.width")).read().toReal(), 1.0);
    QCOMPARE(hex(QQmlProperty(casing, QStringLiteral("border.color")).read().value<QColor>()),
             tokenHex(window, "view"));
    // On art of the ring's own colour (a 1 px ring alone was 1.2:1 on such
    // art): the casing parts the two by 3:1, as drawn.
    const QColor ringColour = tokenOf(window, "focusRing");
    QTemporaryDir artDir;
    QImage sameAsRing(64, 64, QImage::Format_RGB32);
    sameAsRing.fill(ringColour);
    QVERIFY(sameAsRing.save(artDir.filePath(QStringLiteral("ring.png"))));
    bridge->setProperty("currentTrackCoverPath",
                        QUrl::fromLocalFile(artDir.filePath(QStringLiteral("ring.png"))).toString());
    QVERIFY(settled(named(window, "nowPlayingPanel")));
    const QRectF tile = sceneRect(cover);
    QTRY_VERIFY2(near(Drawn(window).at(tile.center()), ringColour), qPrintable(hex(Drawn(window).at(tile.center()))));
    {
        const Drawn drawn(window);
        const qreal y = tile.center().y();
        QVERIFY(near(drawn.at(tile.left() + 0.5, y), ringColour));
        QVERIFY(near(drawn.at(tile.left() + 1.5, y), ringColour));
        const QColor line = drawn.at(tile.left() + 2.5, y);
        QVERIFY(near(line, tokenOf(window, "view")));
        QVERIFY2(contrastRatio(line, ringColour) >= 3.0, qPrintable(QString::number(contrastRatio(line, ringColour))));
    }
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

    // A track without a cover: its stand-in is the same stop, with the
    // menu from the keyboard and the right button, as the mark of such a
    // track had; the pointer lays no veil on it and a double-click opens no
    // viewer, as there is no art to show.
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(viewer != nullptr);
    bridge->setProperty("currentTrackCoverPath", QString());
    QTRY_VERIFY(!cover->property("hasCover").toBool());
    QVERIFY(stop->isEnabled());
    QVERIFY(stop->activeFocusOnTab());
    QCOMPARE(sceneRect(stop), sceneRect(cover));
    const QPoint onStandIn = sceneRect(cover).center().toPoint();
    QTest::mouseMove(window, onStandIn);
    QTRY_VERIFY(cover->property("hovered").toBool());
    QVERIFY(!shown(veil));
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, onStandIn);
    QTest::qWait(50);
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(!shown(veil));
    // Past the double-click's time, so that the press is one of its own.
    QTest::mouseMove(window, onStandIn, QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, onStandIn);
    QObject *pointerMenu = nullptr;
    QTRY_VERIFY((pointerMenu = openMenu()) != nullptr);
    QTest::keyClick(window, Qt::Key_Escape);
    // Gone, not only closing, before the keyboard opens it again.
    QTRY_VERIFY(!pointerMenu->property("visible").toBool());
    library->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(library->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY(stop->hasActiveFocus());
    QTRY_VERIFY(shown(ring));
    QTest::keyClick(window, Qt::Key_Menu);
    QObject *keyboardMenu = nullptr;
    QTRY_VERIFY((keyboardMenu = openMenu()) != nullptr);
    QTest::keyClick(window, Qt::Key_Escape);
    // Gone, not only closing: a menu gives the focus back to where it was
    // when its window goes, after the focus moves on below.
    QTRY_VERIFY(!keyboardMenu->property("visible").toBool());
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 60));
    bridge->setProperty("currentTrackCoverPath", testCoverUrl());
    QTRY_VERIFY(cover->property("hasCover").toBool());

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

// The x of item `name` in `row`, from the row's left edge; -1 where it does
// not show.
qreal xIn(QQuickItem *row, const char *name) {
    QQuickItem *item = itemNamed(row, QString::fromLatin1(name));
    return shown(item) ? item->mapToItem(row, QPointF()).x() : -1.0;
}

// Where a tree row's text ends, as the view lays it out: 4 px in from its
// right end, which is the 12 px content inset clear of the scrollbar's
// 8 px lane.
qreal textEndIn(QQuickItem *row) {
    return row->width() - 8 - 12 - 4;
}

// A library folder's row, as with more than one folder, is a folder header:
// its name semibold a gap after its chevron, with no glyph's slot before
// it, so it starts left of its own artists' (a folder glyph's slot put it
// right of them), and its track count at the right end in the tertiary
// colour, where an album's year is. An artist's row under it is as any
// other.
void UiMainWindowTest::libraryFoldersAreFolderHeaders() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    LibraryTreeModel &libraryModel = fixture.libraryModel;
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

    QQuickItem *chevron = itemNamed(folder, QStringLiteral("libraryRowChevron"));
    QQuickItem *title = itemNamed(folder, QStringLiteral("libraryRowTitle"));
    QQuickItem *count = itemNamed(folder, QStringLiteral("libraryRowCount"));
    QVERIFY(shown(chevron) && title && shown(count));
    QVERIFY(itemNamed(folder, QStringLiteral("libraryRowFolderGlyph")) == nullptr);
    // The chevron at the content inset and the name a gap after it, one
    // indent step before its artists'.
    QCOMPARE(xIn(folder, "libraryRowChevron"), 12.0);
    const qreal step = xIn(album, "libraryRowChevron") - xIn(artist, "libraryRowChevron");
    QCOMPARE(step, chevron->width());
    QCOMPARE(xIn(folder, "libraryRowTitle"), 12.0 + step + scaledSize(window, 6));
    QCOMPARE(xIn(artist, "libraryRowTitle") - xIn(folder, "libraryRowTitle"), step);
    QCOMPARE(title->property("text").toString(), QStringLiteral("Shelf One"));
    QCOMPARE(title->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(colorOf(title), tokenHex(window, "text"));
    // The count where an album's year is, tertiary and small, the name's
    // box a gap before it.
    QCOMPARE(count->property("text").toString(), QStringLiteral("180 tracks"));
    QCOMPARE(count->x() + count->width(), textEndIn(folder));
    QCOMPARE(xIn(album, "libraryRowYear") + itemNamed(album, QStringLiteral("libraryRowYear"))->width(),
             count->x() + count->width());
    QCOMPARE(colorOf(count), tokenHex(window, "textTertiary"));
    QCOMPARE(count->property("font").value<QFont>().pointSizeF(),
             paletteOf(window)->property("smallPointSize").toReal());
    QCOMPARE(title->x() + title->width(), count->x() - 8);
    QVERIFY(itemNamed(folder, QStringLiteral("libraryRowYear")) == nullptr);
    // An artist's row: no count, its name as any other's.
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
    QVERIFY(shown(itemNamed(folder, QStringLiteral("libraryRowCount"))));
}

// A library folder's row after the first has a 1 px rule in the hairline
// colour across its top, inside its row, as wide as the row's states: the
// rows keep their height and places. The first folder's row and the other
// rows have none; the selection covers it.
void UiMainWindowTest::libraryFoldersAfterTheFirstHaveARuleOnTop() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music"), QStringLiteral("/shelf")});
    const QString first = QStringLiteral("/music/Artist A/Album A/01.flac");
    const QString second = QStringLiteral("/shelf/Artist B/Album B/01.flac");
    fixture.libraryModel.setExpandedKeys({kRootKey, QStringLiteral("root|/shelf"), kArtistKey});
    fixture.libraryModel.setLibraryTreeFromBinary(treeOf({
        {0, 0, 0, 1, 0, QStringLiteral("Music"), kRootKey, QString(), QStringLiteral("/music"), {}},
        {1, 1, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A"), {}},
        {2, 2, 0, 1, 2005, QStringLiteral("Album A"), kAlbumKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 3, 1, 0, 0, QStringLiteral("First"), QStringLiteral("track|") + first, QStringLiteral("Artist A"), first,
         first},
        {0, 0, 0, 1, 0, QStringLiteral("Shelf"), QStringLiteral("root|/shelf"), QString(), QStringLiteral("/shelf"), {}},
        {1, 1, 0, 1, 0, QStringLiteral("Artist B (1)"), QStringLiteral("artist|Artist B"), QStringLiteral("Artist B"),
         QStringLiteral("/shelf/Artist B"), {}},
        {2, 2, 0, 1, 2010, QStringLiteral("Album B"), QStringLiteral("album|Artist B|Album B"),
         QStringLiteral("Artist B"), QStringLiteral("/shelf/Artist B/Album B"), {}},
        {4, 3, 1, 0, 0, QStringLiteral("Other"), QStringLiteral("track|") + second, QStringLiteral("Artist B"), second,
         second},
    }));
    QQuickItem *view = named(window, "libraryView");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(view && controller);
    QTRY_COMPARE(view->property("count").toInt(), 5);
    QTRY_VERIFY(viewRowAt(view, 4) != nullptr);
    QQuickItem *shelf = viewRowAt(view, 3);
    QVERIFY(shelf != nullptr && shelf->property("isRootRow").toBool());
    const qreal rowHeight = view->property("rowHeight").toReal();
    const auto rule = [](QQuickItem *row) { return itemNamed(row, QStringLiteral("rowBackgroundRule")); };
    QQuickItem *music = viewRowAt(view, 0);
    QVERIFY(music != nullptr && music->property("isRootRow").toBool());
    QVERIFY(!shown(rule(music)));
    QVERIFY(!shown(rule(viewRowAt(view, 1))) && !shown(rule(viewRowAt(view, 2))) && !shown(rule(viewRowAt(view, 4))));
    QQuickItem *shelfRule = rule(shelf);
    QVERIFY(shown(shelfRule));
    QCOMPARE(colorOf(shelfRule), tokenHex(window, "hairline"));
    QCOMPARE(shelfRule->height(), 1.0);
    QCOMPARE(sceneRect(shelfRule).top(), sceneRect(shelf).top());
    QCOMPARE(sceneRect(shelfRule).left(), sceneRect(shelf).left());
    QCOMPARE(sceneRect(shelfRule).right(), sceneRect(itemNamed(shelf, QStringLiteral("rowBackgroundTint"))).right());
    // The rows keep their height and their places on the grid.
    for (int index = 0; index < 5; ++index) {
        QCOMPARE(viewRowAt(view, index)->height(), rowHeight);
        QCOMPARE(viewRowAt(view, index)->y(), index * rowHeight);
    }
    // As drawn, at the row's left end, where it has no text: the rule, and
    // the view under it.
    const QPointF onRule = sceneRect(shelf).topLeft() + QPointF(4.5, 0.5);
    QTRY_VERIFY2(near(Drawn(window).at(onRule), tokenOf(window, "hairline")),
                 qPrintable(Drawn(window).at(onRule).name()));
    QVERIFY(near(Drawn(window).at(onRule + QPointF(0, 1)), tokenOf(window, "view")));
    QVERIFY(tokenHex(window, "hairline") != tokenHex(window, "view"));
    // Selected: the selection covers it.
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 3)));
    QTRY_VERIFY(rowTinted(view, 3));
    const QColor tint = itemNamed(shelf, QStringLiteral("rowBackgroundTint"))->property("color").value<QColor>();
    QTRY_VERIFY2(near(Drawn(window).at(onRule), tint), qPrintable(Drawn(window).at(onRule).name()));
}

// The library's Collapse All is enabled only while a row shows expanded: an
// artist's, an album's or a library folder's, which shows expanded unless
// it was collapsed. With every row collapsed, or none, it would do nothing
// (it was enabled whenever the tree had rows).
void UiMainWindowTest::libraryCollapseAllIsEnabledOnlyWithAnExpandedRow() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    LibraryTreeModel &model = fixture.libraryModel;
    QObject *item = window->findChild<QObject *>(QStringLiteral("libraryCollapseAllItem"));
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(item && controller);
    const auto enabled = [item]() { return item->property("enabled").toBool(); };
    QVERIFY(!model.hasExpandedRows());
    QVERIFY(!enabled());

    QSignalSpy changes(&model, &LibraryTreeModel::hasExpandedRowsChanged);
    showTree(model, kAllExpanded);
    QTRY_COMPARE(model.rowCount(), 3);
    QVERIFY(model.hasExpandedRows());
    QVERIFY(enabled());
    QCOMPARE(changes.count(), 1);
    QVERIFY(QMetaObject::invokeMethod(controller, "collapseAll"));
    QTRY_COMPARE(model.rowCount(), 1);
    QVERIFY(!model.hasExpandedRows());
    QVERIFY(!enabled());
    // One row opened, and closed again.
    model.toggleKey(kArtistKey);
    QTRY_COMPARE(model.rowCount(), 2);
    QVERIFY(enabled());
    model.toggleKey(kArtistKey);
    QTRY_COMPARE(model.rowCount(), 1);
    QVERIFY(!enabled());
    QCOMPARE(changes.count(), 4);

    // Library folders show expanded until collapsed, the rows under them
    // collapsed.
    model.setExpandedKeys({});
    model.setLibraryTreeFromBinary(artistAlbumTrackTree(QStringLiteral("/music/Artist A"), QStringLiteral("Album A"),
                                                        QStringLiteral("Music")));
    QTRY_COMPARE(model.rowCount(), 2);
    QVERIFY(enabled());
    QVERIFY(QMetaObject::invokeMethod(controller, "collapseAll"));
    QTRY_COMPARE(model.rowCount(), 1);
    QVERIFY(!enabled());
}

// Where row `index` of a list view would be drawn, but no row is, the view
// shows its own colour: empty when it does, else the first pixel that
// does not.
QString slotIsBlank(QQuickWindow *window, QQuickItem *view, int index) {
    const qreal rowHeight = view->property("rowHeight").toReal();
    const QRectF viewRect = sceneRect(view);
    const qreal top = viewRect.top() + index * rowHeight - view->property("contentY").toReal();
    const QColor background = tokenOf(window, "view");
    const Drawn drawn(window);
    for (qreal y = top + 0.5; y < top + rowHeight; y += 1.0) {
        for (qreal x = viewRect.left() + 0.5; x < viewRect.right(); x += 1.0) {
            if (!near(drawn.at(x, y), background)) {
                return QStringLiteral("row slot %1 drawn at (%2, %3): %4")
                    .arg(index)
                    .arg(x)
                    .arg(y)
                    .arg(drawn.at(x, y).name());
            }
        }
    }
    return {};
}

// Collapse All takes out the rows of each library folder in a run of its
// own. The rows it took out leave the view, also those of a later folder:
// the view displaced them for the earlier run and then pooled them, and Qt
// left a pooled row it had marked to move drawn where it was (it hides a
// pooled row only when no transition is due, and the library's view has
// transitions for its toggles). Collapsing one row with Left, and opening
// the rows again from the pool, draw the rows where they are.
void UiMainWindowTest::libraryCollapseAllLeavesNoRowDrawn() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    bridgeOf(window)->setProperty("libraryRoots", QStringList{QStringLiteral("/music"), QStringLiteral("/shelf")});
    const QString first = QStringLiteral("/music/Artist A/Album A/01.flac");
    const QString second = QStringLiteral("/shelf/Artist B/Album B/01.flac");
    const QString artistB = QStringLiteral("artist|Artist B");
    const QStringList allExpanded{kRootKey, QStringLiteral("root|/shelf"), kArtistKey, kAlbumKey, artistB,
                                  QStringLiteral("album|Artist B|Album B")};
    LibraryTreeModel &model = fixture.libraryModel;
    model.setExpandedKeys(allExpanded);
    model.setLibraryTreeFromBinary(treeOf({
        {0, 0, 0, 1, 0, QStringLiteral("Music"), kRootKey, QString(), QStringLiteral("/music"), {}},
        {1, 1, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A"), {}},
        {2, 2, 0, 1, 2005, QStringLiteral("Album A"), kAlbumKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 3, 1, 0, 0, QStringLiteral("First"), QStringLiteral("track|") + first, QStringLiteral("Artist A"), first,
         first},
        {0, 0, 0, 1, 0, QStringLiteral("Shelf"), QStringLiteral("root|/shelf"), QString(), QStringLiteral("/shelf"), {}},
        {1, 1, 0, 1, 0, QStringLiteral("Artist B (1)"), artistB, QStringLiteral("Artist B"),
         QStringLiteral("/shelf/Artist B"), {}},
        {2, 2, 0, 1, 2010, QStringLiteral("Album B"), QStringLiteral("album|Artist B|Album B"),
         QStringLiteral("Artist B"), QStringLiteral("/shelf/Artist B/Album B"), {}},
        {4, 3, 1, 0, 0, QStringLiteral("Other"), QStringLiteral("track|") + second, QStringLiteral("Artist B"), second,
         second},
    }));
    QQuickItem *view = named(window, "libraryView");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QObject *collapseAll = window->findChild<QObject *>(QStringLiteral("libraryCollapseAllItem"));
    QVERIFY(view && controller && collapseAll);
    const qreal rowHeight = view->property("rowHeight").toReal();
    // Each row in its place, and nothing below the last.
    const auto drawnAsTheRows = [&](int count) -> QString {
        if (view->property("count").toInt() != count) {
            return QStringLiteral("%1 rows").arg(view->property("count").toInt());
        }
        for (int index = 0; index < count; ++index) {
            QQuickItem *row = viewRowAt(view, index);
            if (row == nullptr || !shown(row) || row->y() != index * rowHeight) {
                return QStringLiteral("row %1 out of place").arg(index);
            }
        }
        for (int index = count; index < 8; ++index) {
            const QString drawn = slotIsBlank(window, view, index);
            if (!drawn.isEmpty()) {
                return drawn;
            }
        }
        return {};
    };
    QTRY_COMPARE(drawnAsTheRows(8), QString());

    // From the library's menu: the two folders' rows alone.
    QVERIFY(collapseAll->property("enabled").toBool());
    QVERIFY(QMetaObject::invokeMethod(collapseAll, "click"));
    QTRY_COMPARE(drawnAsTheRows(2), QString());
    QCOMPARE(viewRowAt(view, 1)->property("rowTitle").toString(), QStringLiteral("Shelf"));

    // Opened again, from the pool, and collapsed by the controller as the
    // shortcut does.
    model.setExpandedKeys(allExpanded);
    QTRY_COMPARE(drawnAsTheRows(8), QString());
    QVERIFY(QMetaObject::invokeMethod(controller, "collapseAll"));
    QTRY_COMPARE(drawnAsTheRows(2), QString());

    // Left on one artist: its album and track leave, the rows below close
    // up over them.
    model.setExpandedKeys(allExpanded);
    QTRY_COMPARE(drawnAsTheRows(8), QString());
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 1)));
    view->forceActiveFocus();
    QTRY_VERIFY(view->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_COMPARE(drawnAsTheRows(6), QString());
    QCOMPARE(viewRowAt(view, 2)->property("rowTitle").toString(), QStringLiteral("Shelf"));
}

void UiMainWindowTest::regionHeadersNameTheRegionAndItsTotals() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
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
    QCOMPARE(scanCount->property("text").toString(), QStringLiteral("34%"));
    QCOMPARE(libraryTotals->property("text").toString(),
             QStringLiteral("1 artist · 28 albums · 176 tracks"));
    bridge->setProperty("libraryScanInProgress", false);
    QTRY_VERIFY(!shown(scanCount));

    // The queue's name and totals.
    setQueue(bridge, sampleQueueRows(3));
    bridge->setProperty("queueDurationText", QStringLiteral("3:00"));
    QTRY_COMPARE(queueTotals->property("text").toString(), QStringLiteral("3 tracks · 3:00"));
    QCOMPARE(queueTitle->property("text").toString(), QStringLiteral("Queue"));

    // Narrow: the library's totals keep the tracks only.
    window->resize(1000, 720);
    QTRY_COMPARE(libraryTotals->property("text").toString(), QStringLiteral("176 tracks"));
    QVERIFY(!libraryTitle->property("truncated").toBool());
}

void UiMainWindowTest::libraryHeaderSaysItHasNoFolders_data() {
    QTest::addColumn<qreal>("points");
    QTest::addColumn<bool>("narrow");
    QTest::newRow("10 pt") << 10.0 << false;
    QTest::newRow("10 pt, narrow") << 10.0 << true;
    QTest::newRow("24 pt") << 24.0 << false;
}

// Without a library folder the library's header says so where its totals
// go, in their quiet colour, and nothing else: no line under it and no
// button (the empty header read as a library that had not loaded); the
// queue's first-run message stays the one call to action. With a folder,
// the totals as ever, none while it has no tracks.
void UiMainWindowTest::libraryHeaderSaysItHasNoFolders() {
    QFETCH(qreal, points);
    QFETCH(bool, narrow);
    const ScopedApplicationFont font(points);
    MainWindowFixture fixture(narrow ? sized(1000, 720) : sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QCOMPARE(bridge->property("libraryRootCount").toInt(), 0);
    QQuickItem *header = named(window, "libraryHeader");
    QVERIFY(header != nullptr);
    QQuickItem *title = itemNamed(header, QStringLiteral("regionHeaderTitle"));
    QQuickItem *totals = itemNamed(header, QStringLiteral("regionHeaderTotals"));
    QVERIFY(title && totals);
    QTRY_VERIFY(shown(totals));
    QCOMPARE(totals->property("text").toString(), QStringLiteral("No folders"));
    QVERIFY(!totals->property("truncated").toBool());
    QVERIFY(!title->property("truncated").toBool());
    QCOMPARE(colorOf(totals), tokenHex(window, "textTertiary"));
    QVERIFY(!named(window, "libraryEmptyStateLabel")->isVisible());
    QVERIFY(shown(named(window, "queueFirstRunMessage")));

    // A folder, scanning: no totals until it has tracks.
    bridge->setProperty("libraryRootCount", 1);
    QTRY_COMPARE(totals->property("text").toString(), QString());
    bridge->setProperty("libraryArtistCount", 1);
    bridge->setProperty("libraryAlbumCount", 2);
    bridge->setProperty("libraryTrackCount", 9);
    // (At 24 pt the header shows the track total when they don't all fit.)
    QTRY_COMPARE(header->property("totals").toString(),
                 narrow ? QStringLiteral("9 tracks") : QStringLiteral("1 artist · 2 albums · 9 tracks"));
    // The folder removed again.
    bridge->setProperty("libraryRootCount", 0);
    bridge->setProperty("libraryTrackCount", 0);
    QTRY_COMPARE(totals->property("text").toString(), QStringLiteral("No folders"));
}

void UiMainWindowTest::libraryHeaderShortensToItsTrackTotal_data() {
    QTest::addColumn<qreal>("points");
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("whole");
    QTest::newRow("10 pt, wide") << 10.0 << 1600 << true;
    QTest::newRow("10 pt, narrow") << 10.0 << 1000 << false;
    QTest::newRow("24 pt, wide") << 24.0 << 1600 << false;
    QTest::newRow("24 pt, very wide") << 24.0 << 2400 << true;
    QTest::newRow("24 pt, narrow") << 24.0 << 1000 << false;
}

// When the library's totals don't fit whole, its header shows the track
// total, at any width and font, as the narrow sidebar does. At a large font
// in a wide window it dropped totals from the end and kept "13 artists",
// which said less than "180 tracks" and differed from the narrow window.
void UiMainWindowTest::libraryHeaderShortensToItsTrackTotal() {
    QFETCH(qreal, points);
    QFETCH(int, width);
    QFETCH(bool, whole);
    const ScopedApplicationFont font(points);
    MainWindowFixture fixture(sized(width, 1000));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *header = named(window, "libraryHeader");
    QVERIFY(header != nullptr);
    QQuickItem *title = itemNamed(header, QStringLiteral("regionHeaderTitle"));
    QQuickItem *totals = itemNamed(header, QStringLiteral("regionHeaderTotals"));
    QVERIFY(title && totals);
    bridge->setProperty("libraryRootCount", 1);
    bridge->setProperty("libraryArtistCount", 13);
    bridge->setProperty("libraryAlbumCount", 29);
    bridge->setProperty("libraryTrackCount", 180);
    const QString all = QStringLiteral("13 artists · 29 albums · 180 tracks");
    const QString shortened = QStringLiteral("180 tracks");
    QTRY_VERIFY(shown(totals));
    QTRY_COMPARE(totals->property("text").toString(), whole ? all : shortened);
    QVERIFY(!totals->property("truncated").toBool());
    QVERIFY(!title->property("truncated").toBool());
    // The heading still names every total where there is room for them.
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(header);
    QVERIFY(accessible != nullptr);
    QVERIFY2(accessible->text(QAccessible::Name).endsWith(width > 1000 ? all : shortened),
             qPrintable(accessible->text(QAccessible::Name)));
}

void UiMainWindowTest::selectedQueueRowsNumberIsReadableAsDrawn_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

// A selected queue row's track number, measured on the window as drawn,
// on the focused list's selection and on the selection of a list without
// the focus: its strongest pixel is over 4.5:1 on the row's tint. (A
// review measured 3.40:1, the plain tertiary colour on the selection: the
// queue row it read was under the search palette's selected result.)
void UiMainWindowTest::selectedQueueRowsNumberIsReadableAsDrawn() {
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    setQueue(bridgeOf(window), sampleQueueRows(3));
    QObject *controller = window->findChild<QObject *>(QStringLiteral("queueController"));
    QQuickItem *view = named(window, "playlistView");
    QQuickItem *library = named(window, "libraryView");
    QVERIFY(controller && view && library);
    QTRY_VERIFY(viewRowAt(view, 2) != nullptr);
    QVERIFY(QMetaObject::invokeMethod(controller, "setSingleSelection", Q_ARG(QVariant, 1)));
    QTRY_VERIFY(rowTinted(view, 1));

    const auto measure = [&](const QString &what) {
        QQuickItem *cell = itemNamed(viewRowAt(view, 1), QStringLiteral("queueCell_order"));
        QVERIFY(cell != nullptr);
        QCOMPARE(colorOf(cell), tokenHex(window, "textTertiarySelected"));
        QCOMPARE(cell->property("text").toString(), QStringLiteral("2"));
        const QImage image = window->grabWindow().convertToFormat(QImage::Format_RGB32);
        const qreal scale = static_cast<qreal>(image.width()) / window->width();
        const QRectF rect = cell->mapRectToScene(QRectF(0, 0, cell->width(), cell->height()));
        const QRect device(QPoint(static_cast<int>(std::ceil(rect.left() * scale)),
                                  static_cast<int>(std::ceil(rect.top() * scale))),
                           QPoint(static_cast<int>(std::floor(rect.right() * scale)) - 1,
                                  static_cast<int>(std::floor(rect.bottom() * scale)) - 1));
        // The row's tint is the colour most of the cell is drawn in.
        QHash<QRgb, int> counts;
        for (int y = device.top(); y <= device.bottom(); ++y) {
            for (int x = device.left(); x <= device.right(); ++x) {
                ++counts[image.pixel(x, y)];
            }
        }
        QRgb tint = 0;
        int most = 0;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            if (it.value() > most) {
                most = it.value();
                tint = it.key();
            }
        }
        const QColor background = QColor::fromRgb(tint);
        QVERIFY2(!near(background, tokenOf(window, "view")),
                 qPrintable(what + QStringLiteral(": the row is not tinted")));
        QColor strongest = background;
        for (int y = device.top(); y <= device.bottom(); ++y) {
            for (int x = device.left(); x <= device.right(); ++x) {
                const QColor candidate = image.pixelColor(x, y);
                if (contrastRatio(candidate, background) > contrastRatio(strongest, background)) {
                    strongest = candidate;
                }
            }
        }
        qInfo("%s", qPrintable(describeContrast(what, strongest, background)));
        QVERIFY2(contrastRatio(strongest, background) >= 4.5,
                 qPrintable(describeContrast(what, strongest, background)));
    };

    // The queue has the focus: the selection's own tint.
    view->forceActiveFocus(Qt::MouseFocusReason);
    QTRY_VERIFY(view->hasActiveFocus());
    measure(QStringLiteral("focused list"));
    // Another list has it: the inactive selection's tint.
    library->forceActiveFocus(Qt::MouseFocusReason);
    QTRY_VERIFY(!view->hasActiveFocus());
    measure(QStringLiteral("list without the focus"));
}

// A selected row of the queue draws its length in the selected row's
// secondary colour and its artist and album in its column colour, which
// stay over its number's contrast on the selection, under the pointer as
// well; an unselected one in the plain ones. A selected playing row's
// title and glyph take the amber made readable on the selection (on dark
// the map's amber is 4.18:1 on the hovered selection, and it is lighter:
// UiFoundationsTest holds the colours), an unselected one the map's own.
void UiMainWindowTest::selectedRowsDrawTheirMarksForTheSelection() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    setQueue(bridgeOf(window), sampleQueueRows(3));
    QObject *controller = window->findChild<QObject *>(QStringLiteral("queueController"));
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(controller && view);
    QTRY_VERIFY(viewRowAt(view, 2) != nullptr);
    QVERIFY(QMetaObject::invokeMethod(controller, "setSingleSelection", Q_ARG(QVariant, 1)));
    QTRY_VERIFY(rowTinted(view, 1));
    const auto lengthColor = [view](int row) {
        return colorOf(itemNamed(viewRowAt(view, row), QStringLiteral("queueCell_length")));
    };
    QCOMPARE(lengthColor(1), tokenHex(window, "textSecondarySelected"));
    QCOMPARE(lengthColor(2), tokenHex(window, "textSecondary"));
    // The artist and album likewise, over the row's number.
    const auto artistColor = [view](int row) {
        return colorOf(itemNamed(viewRowAt(view, row), QStringLiteral("queueCell_artist")));
    };
    QCOMPARE(artistColor(1), tokenHex(window, "textColumnSelected"));
    QCOMPARE(artistColor(2), tokenHex(window, "textColumn"));

    QObject *bridge = bridgeOf(window);
    bridge->setProperty("playingQueueIndex", 1);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    const auto cellColor = [view](int row, const char *cell) {
        return colorOf(itemNamed(viewRowAt(view, row), QLatin1String(cell)));
    };
    QTRY_COMPARE(cellColor(1, "queueCell_title"), tokenHex(window, "nowSelected"));
    QCOMPARE(cellColor(1, "queueRowState"), tokenHex(window, "nowSelected"));
    QVERIFY(QMetaObject::invokeMethod(controller, "setSingleSelection", Q_ARG(QVariant, 2)));
    QTRY_VERIFY(rowTinted(view, 2));
    QTRY_COMPARE(cellColor(1, "queueCell_title"), tokenHex(window, "now"));
    QCOMPARE(cellColor(1, "queueRowState"), tokenHex(window, "now"));
}

// An album row without a cover keeps the 20 px place a cover's thumbnail
// has, so its name lines up with its neighbours' (it left an empty 20 px
// hole), with the now-playing stand-in's mark in it, drawn for 20 px, on a
// tile of the plot's floor with a 1 px edge (the floor's swatch alone read
// as a failed load). Once a cover has loaded, the cover alone; one that has
// not loaded yet has the hover tint meanwhile, and no tile.
void UiMainWindowTest::libraryAlbumWithoutACoverShowsAPlaceholderTile() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(fixture.libraryModel, kAllExpanded);
    QQuickItem *album = nullptr;
    QTRY_VERIFY((album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    QQuickItem *thumbnail = itemNamed(album, QStringLiteral("libraryRowThumbnail"));
    QQuickItem *title = itemNamed(album, QStringLiteral("libraryRowTitle"));
    QQuickItem *chevron = itemNamed(album, QStringLiteral("libraryRowChevron"));
    QVERIFY(thumbnail && title && chevron);
    QVERIFY(album->property("coverPath").toString().isEmpty());
    QQuickItem *glyph = itemNamed(album, QStringLiteral("libraryRowCoverPlaceholder"));
    QVERIFY(glyph != nullptr);
    // The app's default palette is light: the light tile.
    QVERIFY(!paletteOf(window)->property("themeIsDark").toBool());
    const QColor floor = tokenOf(window, "noCoverTile");

    // The thumbnail's place and size, and the name after it, with a cover
    // or without: 6 px after the chevron, centred in the row, the name
    // 8 px after it.
    const auto checkPlace = [&]() {
        QVERIFY(shown(thumbnail));
        QCOMPARE(xIn(album, "libraryRowThumbnail"), xIn(album, "libraryRowChevron") + chevron->width() + 6);
        QCOMPARE(thumbnail->size(), QSizeF(20, 20));
        QCOMPARE(thumbnail->mapToItem(album, QPointF()).y(), std::round((album->height() - 20) / 2));
        QCOMPARE(thumbnail->property("radius").toReal(), 2.0);
        QCOMPARE(xIn(album, "libraryRowTitle"), xIn(album, "libraryRowThumbnail") + 20 + 8);
    };

    // Without a cover: the thumbnail's own fill none, and the no-art tile
    // over it in its place, at its size and radius, of the floor with a
    // 1 px edge, with the 20 px mark filling it.
    checkPlace();
    QCOMPARE(thumbnail->property("color").value<QColor>().alpha(), 0);
    QCOMPARE(QQmlProperty(thumbnail, QStringLiteral("border.width")).read().toReal(), 0.0);
    QVERIFY(shown(glyph));
    QCOMPARE(sceneRect(glyph), sceneRect(thumbnail));
    QCOMPARE(glyph->property("radius").toReal(), 2.0);
    QCOMPARE(colorOf(glyph), tokenHex(window, "noCoverTile"));
    QCOMPARE(QQmlProperty(glyph, QStringLiteral("border.width")).read().toReal(), 1.0);
    QCOMPARE(hex(QQmlProperty(glyph, QStringLiteral("border.color")).read().value<QColor>()),
             tokenHex(window, "noCoverTileEdge"));
    QQuickItem *mark = itemNamed(glyph, QStringLiteral("noCoverTileMark"));
    QVERIFY(shown(mark));
    QCOMPARE(mark->size(), QSizeF(20, 20));
    QVERIFY(mark->property("source").toUrl().toString().endsWith(QStringLiteral("/no-art-mark-20-light.svg")));
    QTRY_COMPARE(mark->property("status").toInt(), 1); // Image.Ready
    // As drawn: the edge, the floor inside it, the glide and the harmonic
    // in the mark's colour, on their pixels (no-art-mark-20.svg); both run
    // out to the tile's inner edge, as the app icon's traces run out to
    // its tile's: the column inside the edge has them, crisp, on their
    // rows.
    const QColor edge = tokenOf(window, "noCoverTileEdge");
    const auto markProblem = [&]() -> QString {
        const Drawn drawn(window);
        const QPointF origin = sceneRect(thumbnail).topLeft();
        const auto at = [&](qreal x, qreal y) { return drawn.at(origin + QPointF(x, y)); };
        const auto marked = [](const QColor &c) { return channelDistance(c, kNoCoverMarkLight) <= 3; };
        const struct {
            qreal x;
            qreal y;
            std::function<bool(const QColor &)> is;
        } pixels[] = {
            {0.5, 10.5, [&](const QColor &c) { return channelDistance(c, edge) <= 3; }},
            {10.5, 19.5, [&](const QColor &c) { return channelDistance(c, edge) <= 3; }},
            {2.5, 2.5, [&](const QColor &c) { return channelDistance(c, floor) <= 3; }},
            {13.5, 13.5, marked},
            {15.5, 5.5, marked},
            {18.5, 12.5, marked},
            {18.5, 3.5, marked},
        };
        for (const auto &pixel : pixels) {
            if (!pixel.is(at(pixel.x, pixel.y))) {
                return QStringLiteral("%1 at %2, %3").arg(at(pixel.x, pixel.y).name()).arg(pixel.x).arg(pixel.y);
            }
        }
        return {};
    };
    QTRY_VERIFY2(markProblem().isEmpty(), qPrintable(markProblem()));

    // A cover loaded: the tile goes; the cover, nothing under it, in the
    // same place.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage coverImage(40, 40, QImage::Format_RGB32);
    const QColor coverColor(200, 120, 40);
    coverImage.fill(coverColor);
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QVERIFY(coverImage.save(coverPath));
    thumbnail->setProperty("source", QUrl::fromLocalFile(coverPath));
    QTRY_COMPARE(thumbnail->property("status").toInt(), 1); // Image.Ready
    QCOMPARE(thumbnail->property("color").value<QColor>().alpha(), 0);
    checkPlace();
    QVERIFY(!shown(glyph));
    QTRY_VERIFY(channelDistance(Drawn(window).at(sceneRect(thumbnail).center()), coverColor) <= 2);
}

void UiMainWindowTest::libraryNoCoverMarkIsOneQuietColour_data() {
    QTest::addColumn<qreal>("points");
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark, 10 pt, the stepped mark") << 10.0 << true;
    QTest::newRow("dark, 24 pt, the curved mark") << 24.0 << true;
    QTest::newRow("light, 10 pt, the stepped mark") << 10.0 << false;
    QTest::newRow("light, 24 pt, the curved mark") << 24.0 << false;
}

// At a list's size the no-cover mark is one quiet colour on its floor, at
// full strength and at least 4.5:1 on it, in both drawings: no amber, which
// is only what is playing, and no magenta (repeated down a run of albums
// without covers, the two outshone everything in the tree). Every pixel
// inside the tile's edge is the floor, the mark or a blend of them. In
// Breeze Dark the floor is the plot's, with its edge; in a light theme a
// light indigo tile with a darker rim (the plot's floor there made a run of
// albums without covers a column of near-black squares down a white list).
void UiMainWindowTest::libraryNoCoverMarkIsOneQuietColour() {
    QFETCH(qreal, points);
    QFETCH(bool, dark);
    const auto palette = useDarkPalette(dark);
    const ScopedApplicationFont font(points);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QCOMPARE(paletteOf(window)->property("themeIsDark").toBool(), dark);
    showTree(fixture.libraryModel, kAllExpanded);
    QQuickItem *album = nullptr;
    QTRY_VERIFY((album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    QQuickItem *tile = itemNamed(album, QStringLiteral("libraryRowCoverPlaceholder"));
    QVERIFY(shown(tile));
    const QColor floor = tokenOf(window, "noCoverTile");
    const QColor edge = QQmlProperty(tile, QStringLiteral("border.color")).read().value<QColor>();
    const QColor view = tokenOf(window, "view");
    if (dark) {
        // As it was: the plot's floor and edge.
        QCOMPARE(hex(floor), tokenHex(window, "plotFloor"));
        QCOMPARE(hex(edge), tokenHex(window, "plotFloorEdge"));
    } else {
        QCOMPARE(hex(floor), QStringLiteral("#ffebe7f4"));
        QCOMPARE(hex(edge), QStringLiteral("#ffc9c2dc"));
        // A light tile on the light view, its rim darker than it.
        QVERIFY2(contrastRatio(floor, view) < 1.5, qPrintable(describeContrast(QStringLiteral("tile"), floor, view)));
        QVERIFY(relativeLuminance(edge) < relativeLuminance(floor));
    }
    const QColor markColor = dark ? kNoCoverMark : kNoCoverMarkLight;
    QVERIFY2(contrastRatio(markColor, floor) >= 4.5,
             qPrintable(describeContrast(QStringLiteral("mark"), markColor, floor)));
    QQuickItem *mark = itemNamed(tile, QStringLiteral("noCoverTileMark"));
    QVERIFY(shown(mark));
    QVERIFY2(mark->property("source").toUrl().toString().endsWith(dark ? QStringLiteral(".svg") : QStringLiteral("-light.svg")),
             qPrintable(mark->property("source").toUrl().toString()));

    const auto problem = [&]() -> QString {
        const Drawn drawn(window);
        const QRectF box = sceneRect(tile);
        int marked = 0;
        for (qreal y = 2.5; y < box.height() - 2; ++y) {
            for (qreal x = 2.5; x < box.width() - 2; ++x) {
                const QColor pixel = drawn.at(box.topLeft() + QPointF(x, y));
                const double amount =
                    std::clamp((pixel.greenF() - floor.greenF()) / double(markColor.greenF() - floor.greenF()), 0.0, 1.0);
                if (channelDistance(pixel, mixed(floor, markColor, amount)) > 6) {
                    return QStringLiteral("%1 at %2, %3").arg(pixel.name()).arg(x).arg(y);
                }
                marked += channelDistance(pixel, markColor) <= 3 ? 1 : 0;
            }
        }
        return marked >= box.width() / 2 ? QString() : QStringLiteral("%1 pixels of the mark").arg(marked);
    };
    QTRY_VERIFY2(problem().isEmpty(), qPrintable(problem()));
}

// The tree has one indent step, a chevron's width: the artist's chevron at
// the content inset, its name 6 px after it; an album one step in, then
// its 20 px thumbnail and its name 8 px after that, the year at the text's
// right end, tertiary, clear of the scrollbar lane and the inset; a track's
// number under the thumbnail, right-aligned, and its title where the
// album's name starts.
void UiMainWindowTest::libraryTreeHasOneIndentStep() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(fixture.libraryModel, kAllExpanded);
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
    const auto text = [](QQuickItem *row, const char *name) {
        QQuickItem *item = itemNamed(row, QString::fromLatin1(name));
        return item != nullptr ? item->property("text").toString() : QString();
    };
    QQuickItem *chevron = itemNamed(artist, QStringLiteral("libraryRowChevron"));
    QVERIFY(chevron != nullptr);
    const qreal step = chevron->width();
    QCOMPARE(step, qreal(scaledSize(window, 16)));

    // Artist: the chevron at the inset, its name 6 px after; no album count.
    QCOMPARE(xIn(artist, "libraryRowChevron"), 12.0);
    QCOMPARE(xIn(artist, "libraryRowTitle"), 12.0 + step + 6);
    QCOMPARE(text(artist, "libraryRowTitle"), QStringLiteral("Artist A"));
    QCOMPARE(chevron->property("source").toString(), QStringLiteral("arrow-down"));
    // Album: one step in, then the 20 px thumbnail 6 px after its chevron,
    // then its name 8 px after that; the year at the text's right end: the
    // mark a collapsed row that holds the playing track has follows the
    // name, not the year.
    const qreal albumChevron = xIn(album, "libraryRowChevron");
    QCOMPARE(albumChevron, 12.0 + step);
    QCOMPARE(xIn(album, "libraryRowThumbnail"), albumChevron + step + 6);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowThumbnail"))->width(), 20.0);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowThumbnail"))->property("radius").toReal(), 2.0);
    const qreal nameX = xIn(album, "libraryRowThumbnail") + 20 + 8;
    QCOMPARE(xIn(album, "libraryRowTitle"), nameX);
    QCOMPARE(text(album, "libraryRowTitle"), QStringLiteral("Album A"));
    QQuickItem *year = itemNamed(album, QStringLiteral("libraryRowYear"));
    QVERIFY(shown(year));
    QCOMPARE(year->property("text").toString(), QStringLiteral("2005"));
    QCOMPARE(year->x() + year->width(), textEndIn(album));
    QCOMPARE(colorOf(year), tokenHex(window, "textTertiary"));
    // Track: its number under the thumbnail, right-aligned, and its title
    // where the album's name starts; no number in the title.
    QQuickItem *number = itemNamed(track, QStringLiteral("libraryRowNumber"));
    QVERIFY(shown(number));
    QCOMPARE(xIn(track, "libraryRowNumber"), xIn(album, "libraryRowThumbnail"));
    QCOMPARE(number->width(), 20.0);
    QCOMPARE(number->property("horizontalAlignment").toInt(), int(Qt::AlignRight));
    QCOMPARE(number->property("text").toString(), QStringLiteral("2"));
    QCOMPARE(colorOf(number), tokenHex(window, "textTertiary"));
    QCOMPARE(xIn(track, "libraryRowTitle"), nameX);
    QCOMPARE(text(track, "libraryRowTitle"), QStringLiteral("Second"));
    QVERIFY(!shown(itemNamed(track, QStringLiteral("libraryRowChevron"))));
    QVERIFY(!shown(itemNamed(track, QStringLiteral("libraryRowYear"))));

    // A collapsed album points right.
    fixture.libraryModel.toggleKey(kAlbumKey);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    album = libraryRow(window, QStringLiteral("album"));
    QVERIFY(album != nullptr);
    QCOMPARE(itemNamed(album, QStringLiteral("libraryRowChevron"))->property("source").toString(),
             QStringLiteral("arrow-right"));
}

// At 24 pt the tree's chevron columns and state glyphs grow with the font,
// as the palette scales its 16 and 12 px glyphs, and the indent step with
// the columns, so that each level's chevron is where its parent's ends. The
// chevron itself stops at Breeze's 22 px arrow, centred in its column
// (libraryChevronsKeepBreezesStroke).
void UiMainWindowTest::libraryTreeChevronsGrowWithALargeFont() {
    const ScopedApplicationFont font(24.0);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(fixture.libraryModel, kAllExpanded);
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
    // 2.4 times the default font's: 38 and 29 px.
    const qreal chevronSize = scaledSize(window, 16);
    QCOMPARE(chevronSize, std::round(16 * 2.4));
    QCOMPARE(artistChevron->size(), QSizeF(22, 22));
    QCOMPARE(albumChevron->x() - artistChevron->x(), chevronSize);
    QTRY_VERIFY(state->isVisible());
    QCOMPARE(state->size(), QSizeF(scaledSize(window, 12), scaledSize(window, 12)));
    QCOMPARE(state->width(), std::round(12 * 2.4));
    // Each inside its row.
    for (QQuickItem *item : {artistChevron, albumChevron, state}) {
        QQuickItem *row = item->parentItem();
        QVERIFY(item->y() >= 0 && item->y() + item->height() <= row->height());
    }
    // The title after the chevron and its gap, clear of it.
    QVERIFY(itemNamed(artist, QStringLiteral("libraryRowTitle"))->x()
            >= artist->property("chevronX").toReal() + chevronSize + 6);
    // A collapsed album that holds the playing track has its name amber at
    // a large font too.
    fixture.libraryModel.toggleKey(kAlbumKey);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    QTRY_VERIFY2(nameMarksNow(window, libraryRow(window, QStringLiteral("album")), false).isEmpty(),
                 qPrintable(nameMarksNow(window, libraryRow(window, QStringLiteral("album")), false)));
}

void UiMainWindowTest::libraryThumbnailsFollowTheFont_data() {
    QTest::addColumn<qreal>("points");
    QTest::newRow("8 pt") << 8.0;
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("14 pt") << 14.0;
    QTest::newRow("24 pt") << 24.0;
}

// An album's thumbnail, the gaps before and after it, a track's number
// column under it and the no-cover mark in it follow the font: 20, 6 and
// 8 px with the default font or a smaller one, and at 24 pt 2.4 times
// those, the thumbnail inside its row (it stayed 20 px in a 52 px row,
// beside 32 px text).
void UiMainWindowTest::libraryThumbnailsFollowTheFont() {
    QFETCH(qreal, points);
    const ScopedApplicationFont font(points);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(fixture.libraryModel, kAllExpanded);
    QQuickItem *album = nullptr;
    QQuickItem *track = nullptr;
    QTRY_VERIFY((album = libraryRow(window, QStringLiteral("album"))) != nullptr
                && (track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    QQuickItem *thumbnail = itemNamed(album, QStringLiteral("libraryRowThumbnail"));
    QQuickItem *chevron = itemNamed(album, QStringLiteral("libraryRowChevron"));
    QQuickItem *number = itemNamed(track, QStringLiteral("libraryRowNumber"));
    QQuickItem *tile = itemNamed(album, QStringLiteral("libraryRowCoverPlaceholder"));
    QQuickItem *mark = itemNamed(album, QStringLiteral("noCoverTileMark"));
    QVERIFY(thumbnail && chevron && number && shown(tile) && shown(mark));
    const qreal size = thumbnail->width();
    QCOMPARE(thumbnail->height(), size);
    if (points <= 10.0) {
        QCOMPARE(size, 20.0);
    } else {
        QVERIFY2(size >= std::min<qreal>(scaledSize(window, 20), album->height() - 6) - 1 && size <= album->height() - 6,
                 qPrintable(QStringLiteral("%1 in a %2 px row").arg(size).arg(album->height())));
    }
    if (points == 24.0) {
        QVERIFY2(size >= 38 && size <= 46, qPrintable(QString::number(size)));
    }
    QCOMPARE(std::fmod(size, 2.0), 0.0);
    const qreal radius = thumbnail->property("radius").toReal();
    QCOMPARE(radius, std::max(2.0, std::round(size / 10)));
    const qreal thumbnailX = xIn(album, "libraryRowThumbnail");
    // After the chevron's column, which the chevron is centred in.
    QCOMPARE(thumbnailX, album->property("chevronX").toReal() + scaledSize(window, 16) + scaledSize(window, 6));
    const qreal nameX = thumbnailX + size + scaledSize(window, 8);
    QCOMPARE(xIn(album, "libraryRowTitle"), nameX);
    QCOMPARE(xIn(track, "libraryRowTitle"), nameX);
    QCOMPARE(xIn(track, "libraryRowNumber"), thumbnailX);
    QCOMPARE(number->width(), size);
    QCOMPARE(sceneRect(tile), sceneRect(thumbnail));
    QCOMPARE(tile->property("radius").toReal(), radius);
    // The mark is drawn at its size: on its 20 px grid, crisp, and in
    // smooth curves above it, where the grid's steps made a staircase.
    const QString drawing = mark->property("source").toUrl().toString();
    // The app's default palette is light: the light drawings.
    QVERIFY2(drawing.endsWith(size == 20.0 ? QStringLiteral("/no-art-mark-20-light.svg") : QStringLiteral("/no-art-mark-light.svg")),
             qPrintable(drawing));
    QTRY_COMPARE(mark->property("status").toInt(), 1); // Image.Ready
    QCOMPARE(mark->size(), QSizeF(size, size));
    QCOMPARE(QQmlProperty(mark, QStringLiteral("sourceSize.width")).read().toReal(), size);
    QCOMPARE(mark->property("smooth").toBool(), size != 20.0);
}

void UiMainWindowTest::libraryChevronsKeepBreezesStroke_data() {
    QTest::addColumn<qreal>("points");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("14 pt") << 14.0;
    QTest::newRow("24 pt") << 24.0;
}

// The tree's chevrons are Breeze's arrows at one of Breeze's sizes, at most
// its 22 px one, centred in their column, which grows with the font as the
// indent step: Breeze draws its arrows in a 1 px stroke at every size, and
// the 32 px arrow a 24 pt font rounded to was a wide hairline V beside text
// of a heavier stroke. The default font's 16 px chevron is as it was.
void UiMainWindowTest::libraryChevronsKeepBreezesStroke() {
    QFETCH(qreal, points);
    const ScopedApplicationFont font(points);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(fixture.libraryModel, kAllExpanded);
    QQuickItem *artist = nullptr;
    QQuickItem *album = nullptr;
    QTRY_VERIFY((artist = libraryRow(window, QStringLiteral("artist"))) != nullptr
                && (album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    const qreal column = scaledSize(window, 16);
    const qreal glyph = std::min<qreal>(column, 22);
    if (points == 24.0) QCOMPARE(glyph, 22.0);
    for (QQuickItem *row : {artist, album}) {
        QQuickItem *chevron = itemNamed(row, QStringLiteral("libraryRowChevron"));
        QVERIFY(shown(chevron));
        QCOMPARE(chevron->width(), glyph);
        QCOMPARE(chevron->height(), glyph);
        // A size Breeze draws, at that size.
        QVERIFY(chevron->property("roundToIconSize").toBool());
        const qreal columnX = row->property("chevronX").toReal();
        QCOMPARE(xIn(row, "libraryRowChevron"), columnX + std::round((column - glyph) / 2));
        QVERIFY(std::abs(chevron->mapToItem(row, QPointF(0, glyph / 2)).y() - row->height() / 2) <= 0.5);
        if (points <= 10.0) QCOMPARE(xIn(row, "libraryRowChevron"), columnX);
    }
    // The indent step is still the column.
    QCOMPARE(album->property("chevronX").toReal() - artist->property("chevronX").toReal(), column);

    // As drawn: the arrow's ink no wider than Breeze's 22 px drawing.
    const auto inkWidth = [&]() {
        const QImage image = window->grabWindow();
        const qreal scale = qreal(image.width()) / window->width();
        const QRectF box = sceneRect(artist);
        const qreal columnLeft = artist->mapToScene(QPointF(artist->property("chevronX").toReal(), 0)).x();
        const QColor background = image.pixelColor(QPointF((box.right() - 30) * scale, (box.top() + 2) * scale).toPoint());
        int left = image.width();
        int right = -1;
        for (int y = qFloor(box.top() * scale); y < qFloor(box.bottom() * scale); ++y) {
            for (int x = qFloor(columnLeft * scale); x < qFloor((columnLeft + column) * scale); ++x) {
                if (channelDistance(image.pixelColor(x, y), background) > 40) {
                    left = std::min(left, x);
                    right = std::max(right, x);
                }
            }
        }
        return right < left ? 0.0 : (right - left + 1) / scale;
    };
    QTRY_VERIFY(inkWidth() > 0);
    QVERIFY2(inkWidth() <= glyph, qPrintable(QStringLiteral("%1 px of ink").arg(inkWidth())));
}

void UiMainWindowTest::libraryTreeMarksThePlayingTrackAndItsCollapsedAncestors() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    LibraryTreeModel &libraryModel = fixture.libraryModel;
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

    // Playing: an amber glyph in the album's chevron column, centred in it,
    // and a semibold amber title.
    playTrack(bridge);
    QQuickItem *track = libraryRow(window, QStringLiteral("track"));
    QTRY_COMPARE(glyphName(track), QStringLiteral("media-playback-start"));
    QCOMPARE(colorOf(glyph(track)), tokenHex(window, "now"));
    QCOMPARE(glyph(track)->width(), qreal(scaledSize(window, 12)));
    const qreal chevronSize = scaledSize(window, 16);
    QCOMPARE(xIn(track, "libraryRowState") + glyph(track)->width() / 2,
             xIn(libraryRow(window, QStringLiteral("album")), "libraryRowChevron") + chevronSize / 2);
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
    // read as a play button. Its year stays tertiary.
    const auto checkName = [&](QQuickItem *row, bool paused) {
        const QString wrong = nameMarksNow(window, row, paused);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
    };
    libraryModel.toggleKey(kAlbumKey);
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
    // The row's drawing, under the clip it has while it folds.
    const QList<QQuickItem *> children = itemNamed(album, QStringLiteral("libraryRowContent"))->childItems();
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
// place, and turns amber as it is, with nothing after it.
void UiMainWindowTest::collapsedRowNameElidesBeforeTheYear() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    const QString longName = QStringLiteral("An Album Whose Name Runs On ") + QStringLiteral("And On ").repeated(30);
    showTree(fixture.libraryModel, {kArtistKey}, QStringLiteral("/music/Artist A"), longName);
    QQuickItem *album = nullptr;
    QTRY_VERIFY((album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    QQuickItem *title = itemNamed(album, QStringLiteral("libraryRowTitle"));
    QQuickItem *year = itemNamed(album, QStringLiteral("libraryRowYear"));
    QVERIFY(title != nullptr && shown(year));
    QVERIFY(title->property("implicitWidth").toReal() > title->width());
    // The year at the text's right end, the name's box a gap before it.
    QCOMPARE(year->x() + year->width(), textEndIn(album));
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
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
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
        showTree(fixture.libraryModel, {}, artistPath);
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

// Why the selected row `index` of a list `view` does not show its focus
// ring and tint as one rectangle beside the scrollbar, or "": while the
// scrollbar shows, both end `clearance` px short of its 8 px lane, with the
// thumb alone in it on the view colour; without a scrollbar, both reach the
// edge. As drawn, on the row's middle: the ring's right side just inside
// where the states end, or at the edge; past it, the view or the thumb,
// never the ring or the tint.
QString focusRingProblem(QQuickWindow *window, QQuickItem *view, int index, bool scrolls, qreal clearance = 0) {
    QQuickItem *row = viewRowAt(view, index);
    QQuickItem *tint = row != nullptr ? itemNamed(row, QStringLiteral("rowBackgroundTint")) : nullptr;
    if (!shown(tint) || QQmlProperty(tint, QStringLiteral("border.width")).read().toReal() != 1.0
        || colorOf(tint) != tokenHex(window, "selection")) {
        return QStringLiteral("no ringed, selected row %1").arg(index);
    }
    const qreal edge = sceneRect(view).right();
    const qreal lane = scrolls ? 8.0 : 0.0;
    const qreal end = scrolls ? lane + clearance : 0.0;
    if (sceneRect(tint).right() != edge - end) {
        return QStringLiteral("the ring to %1, the edge %2").arg(sceneRect(tint).right()).arg(edge);
    }
    const Drawn drawn(window);
    const qreal y = sceneRect(row).center().y();
    if (scrolls) {
        // Either side of the thumb, and between the ring and the lane.
        for (qreal x = edge - end + 0.5; x < edge - lane + 1; x += 1.0) {
            if (!near(drawn.at(x, y), tokenOf(window, "view"))) {
                return QStringLiteral("%1 past the ring at %2 of %3").arg(drawn.at(x, y).name()).arg(x).arg(edge);
            }
        }
        if (!near(drawn.at(edge - 0.5, y), tokenOf(window, "view"))) {
            return QStringLiteral("%1 at the edge").arg(drawn.at(edge - 0.5, y).name());
        }
    }
    const QColor ring = tokenOf(window, "focusRing");
    if (!near(drawn.at(edge - end - 0.5, y), ring)) {
        return QStringLiteral("%1 where the ring ends").arg(drawn.at(edge - end - 0.5, y).name());
    }
    for (qreal x = edge - end + 0.5; x < edge; x += 1.0) {
        if (near(drawn.at(x, y), ring)) {
            return QStringLiteral("the ring at %1 of %2").arg(x).arg(edge);
        }
    }
    return {};
}

// The library's focus ring and selection tint end 2 px short of the
// scrollbar's lane while it shows, as the queue's do, so the ring keeps 3 px
// clear of the thumb even at its widest, under the pointer (at the lane it
// was 1 px off it, and the two read as touching); without a scrollbar they
// reach the edge.
void UiMainWindowTest::libraryFocusRingStopsAtTheScrollBarLane() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *view = named(window, "libraryView");
    QQuickItem *scrollBar = named(window, "libraryScrollBar");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(view && scrollBar && controller);
    QCOMPARE(scrollBar->property("lane").toReal(), 8.0);

    for (const bool longTree : {true, false}) {
        if (longTree) {
            fixture.libraryModel.setLibraryTreeFromBinary(manyArtistsTree(60));
        } else {
            showTree(fixture.libraryModel, {});
        }
        QTRY_COMPARE(scrollBar->isVisible(), longTree);
        tabInto(window, view);
        QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 0)));
        QTRY_VERIFY2(focusRingProblem(window, view, 0, longTree, 2).isEmpty(),
                     qPrintable(focusRingProblem(window, view, 0, longTree, 2)));
        if (longTree) {
            QQuickItem *tint = itemNamed(viewRowAt(view, 0), QStringLiteral("rowBackgroundTint"));
            QQuickItem *thumb = itemNamed(scrollBar, QStringLiteral("laneScrollBarThumb"));
            QVERIFY(tint != nullptr && thumb != nullptr);
            QCOMPARE(sceneRect(thumb).left() - sceneRect(tint).right(), 4.0);
            QTest::mouseMove(window, sceneRect(scrollBar).center().toPoint());
            QTRY_VERIFY(scrollBar->property("engaged").toBool());
            QTRY_COMPARE(sceneRect(thumb).left() - sceneRect(tint).right(), 3.0);
            QTest::mouseMove(window, QPoint(2, window->height() - 2));
            QTRY_VERIFY(!scrollBar->property("engaged").toBool());
        }
    }
}

void UiMainWindowTest::libraryRowsShowHoverSelectionAndTheCurrentRow() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(fixture.libraryModel, kAllExpanded);
    QQuickItem *album = nullptr;
    QTRY_VERIFY((album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(view && controller);
    QQuickItem *tint = itemNamed(album, QStringLiteral("rowBackgroundTint"));
    QQuickItem *hover = itemNamed(album, QStringLiteral("rowBackgroundHover"));
    QVERIFY(tint && hover);
    // The tints run to the pane's right edge (they stopped 8 px short of
    // it); the scrollbar shows only when the tree is longer than the view.
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
    // Hover: the lists' hover tint (about 1.35:1 on dark), and the year stepped up
    // to stay 4.6:1 on it.
    QTest::mouseMove(window, sceneRect(album).center().toPoint());
    QTRY_VERIFY(hover->isVisible() && hover->opacity() == 1.0);
    QCOMPARE(colorOf(hover), tokenHex(window, "rowHover"));
    QCOMPARE(colorOf(itemNamed(album, QStringLiteral("libraryRowYear"))), tokenHex(window, "textTertiaryHovered"));
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
    QCOMPARE(colorOf(year), tokenHex(window, "textTertiarySelected"));
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
    QCOMPARE(colorOf(itemNamed(track, QStringLiteral("libraryRowNumber"))), tokenHex(window, "textTertiarySelected"));
}

// A selected row's chevron steps up with the row's other secondary marks,
// to the selected row's secondary colour (it stayed the plain one on the
// selection); an unselected row's keeps the plain one.
void UiMainWindowTest::librarySelectedRowStepsUpItsChevron() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    showTree(fixture.libraryModel, kAllExpanded);
    QQuickItem *artist = nullptr;
    QQuickItem *album = nullptr;
    QTRY_VERIFY((artist = libraryRow(window, QStringLiteral("artist"))) != nullptr
                && (album = libraryRow(window, QStringLiteral("album"))) != nullptr);
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(controller != nullptr);
    QQuickItem *artistChevron = itemNamed(artist, QStringLiteral("libraryRowChevron"));
    QQuickItem *albumChevron = itemNamed(album, QStringLiteral("libraryRowChevron"));
    QVERIFY(shown(artistChevron) && shown(albumChevron));
    QCOMPARE(colorOf(albumChevron), tokenHex(window, "textSecondary"));
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 1)));
    QTRY_VERIFY(album->property("rowSelected").toBool());
    QCOMPARE(colorOf(albumChevron), tokenHex(window, "textSecondarySelected"));
    QCOMPARE(colorOf(artistChevron), tokenHex(window, "textSecondary"));
    QVERIFY(tokenHex(window, "textSecondarySelected") != tokenHex(window, "textSecondary"));
}

void UiMainWindowTest::librarySelectedRowAnswersHoverAsTheQueues_data() {
    QTest::addColumn<bool>("queue");
    QTest::addColumn<bool>("focused");
    QTest::newRow("library") << false << true;
    QTest::newRow("library, without focus") << false << false;
    QTest::newRow("queue") << true << true;
    QTest::newRow("queue, without focus") << true << false;
}

// A selected library row under the pointer steps its selection tint toward
// the text colour, as a selected queue row does, in the same colours, with
// focus and without it (the library's kept the plain tint, so the pointer
// went unanswered on it); off it, the plain tint again.
void UiMainWindowTest::librarySelectedRowAnswersHoverAsTheQueues() {
    QFETCH(bool, queue);
    QFETCH(bool, focused);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *view = named(window, queue ? "playlistView" : "libraryView");
    QQuickItem *other = named(window, queue ? "libraryView" : "playlistView");
    QObject *controller = window->findChild<QObject *>(queue ? QStringLiteral("queueController")
                                                             : QStringLiteral("libraryController"));
    QVERIFY(view && other && controller);
    if (queue) {
        setQueue(bridgeOf(window), sampleQueueRows(3));
        QTRY_COMPARE(view->property("count").toInt(), 3);
    } else {
        showTree(fixture.libraryModel, kAllExpanded);
        QTRY_COMPARE(view->property("count").toInt(), 3);
    }
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRowAt(view, 1)) != nullptr);
    QVERIFY(QMetaObject::invokeMethod(controller, queue ? "setSingleSelection" : "selectIndex",
                                      Q_ARG(QVariant, 1)));
    QTRY_VERIFY(rowTinted(view, 1));
    if (focused) {
        view->forceActiveFocus();
        QTRY_VERIFY(view->hasActiveFocus());
    } else {
        other->forceActiveFocus();
        QTRY_VERIFY(!view->hasActiveFocus());
    }
    QQuickItem *tint = itemNamed(row, QStringLiteral("rowBackgroundTint"));
    QVERIFY(tint != nullptr);
    const QString plain = tokenHex(window, focused ? "selection" : "selectionInactive");
    const QString hovered = tokenHex(window, focused ? "selectionHovered" : "selectionInactiveHovered");
    QVERIFY(plain != hovered);
    QTRY_COMPARE(colorOf(tint), plain);
    QTest::mouseMove(window, sceneRect(row).center().toPoint());
    QTRY_COMPARE(colorOf(tint), hovered);
    // As drawn, at the row's left end, where it has no text.
    const QPointF corner = sceneRect(row).topLeft() + QPointF(2.5, 2.5);
    const QColor drawnHovered = tokenOf(window, focused ? "selectionHovered" : "selectionInactiveHovered");
    QTRY_VERIFY2(near(Drawn(window).at(corner), drawnHovered),
                 qPrintable(Drawn(window).at(corner).name() + QStringLiteral(" for ") + drawnHovered.name()));
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 60));
    QTRY_COMPARE(colorOf(tint), plain);
}

void UiMainWindowTest::queueFocusRingStopsAtTheScrollBarLane_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
}

// The queue's focus ring and selection tint are one rectangle that ends 2 px
// short of the scrollbar's lane while the scrollbar shows
// (focusRingProblem): the thumb never runs over the ring (it ran under the
// thumb), no tint runs on past it (the tint went on under the thumb, a tab
// on the ring's end), and the ring keeps 3 px clear of the thumb even at its
// widest, under the pointer (at the lane it was 1 px off it, and the two
// read as touching). The thumb, 4 px wide and 2 px from the edge, is alone
// in the lane. A short queue, with no scrollbar, keeps both to the edge
// (they stopped 8 px short of it).
void UiMainWindowTest::queueFocusRingStopsAtTheScrollBarLane() {
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QCOMPARE(paletteOf(window)->property("themeIsDark").toBool(), dark);
    QObject *bridge = bridgeOf(window);
    QQuickItem *queueView = named(window, "playlistView");
    QQuickItem *scrollBar = named(window, "queueScrollBar");
    QObject *queue = window->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(queueView && scrollBar && queue);
    QCOMPARE(scrollBar->property("lane").toReal(), 8.0);

    for (const int count : {80, 3}) {
        const bool scrolls = count > 3;
        setQueue(bridge, sampleQueueRows(count));
        QTRY_COMPARE(queueView->property("count").toInt(), count);
        QTRY_COMPARE(scrollBar->isVisible(), scrolls);
        // The keyboard in the library, as at launch.
        named(window, "libraryView")->forceActiveFocus();
        QTRY_VERIFY(!queueView->hasActiveFocus());
        // The second row, beside the thumb, which starts a little below the
        // first row's middle.
        QVERIFY(QMetaObject::invokeMethod(queue, "setSingleSelection", Q_ARG(QVariant, 1)));
        QTRY_VERIFY(rowTinted(queueView, 1));
        // Without the keyboard: the tint alone, as drawn to the same edge,
        // the thumb past it.
        QQuickItem *tint = itemNamed(viewRowAt(queueView, 1), QStringLiteral("rowBackgroundTint"));
        const qreal lane = scrolls ? 8.0 + 2.0 : 0.0;
        QCOMPARE(sceneRect(tint).right(), sceneRect(queueView).right() - lane);
        const QColor tintColor = tint->property("color").value<QColor>();
        const qreal right = sceneRect(queueView).right();
        const qreal y = sceneRect(tint).center().y();
        const auto tintProblem = [&]() -> QString {
            const Drawn drawn(window);
            if (!near(drawn.at(right - lane - 0.5, y), tintColor) || !near(drawn.at(right - 16, y), tintColor)) {
                return QStringLiteral("%1 where the tint ends, %2 before it, the tint %3")
                    .arg(drawn.at(right - lane - 0.5, y).name(), drawn.at(right - 16, y).name(), tintColor.name());
            }
            if (scrolls && !near(drawn.at(right - 0.5, y), tokenOf(window, "view"))) {
                return QStringLiteral("%1 past the thumb").arg(drawn.at(right - 0.5, y).name());
            }
            if (near(drawn.at(right - 4, y), tintColor) == scrolls) {
                return QStringLiteral("%1 where the thumb is").arg(drawn.at(right - 4, y).name());
            }
            return {};
        };
        QTRY_VERIFY2(tintProblem().isEmpty(), qPrintable(tintProblem()));
        // From the keyboard: the ring too.
        tabInto(window, queueView);
        QTRY_VERIFY2(focusRingProblem(window, queueView, 1, scrolls, 2).isEmpty(),
                     qPrintable(focusRingProblem(window, queueView, 1, scrolls, 2)));
        if (scrolls) {
            // The thumb at its widest, under the pointer: 3 px clear.
            QQuickItem *thumb = itemNamed(scrollBar, QStringLiteral("laneScrollBarThumb"));
            QVERIFY(thumb != nullptr);
            const qreal restingGap = sceneRect(thumb).left() - sceneRect(tint).right();
            QCOMPARE(restingGap, 4.0);
            QTest::mouseMove(window, sceneRect(scrollBar).center().toPoint());
            QTRY_VERIFY(scrollBar->property("engaged").toBool());
            QTRY_COMPARE(sceneRect(thumb).left() - sceneRect(tint).right(), 3.0);
            QTest::mouseMove(window, QPoint(2, window->height() - 2));
        }
    }
}

// A queue row under the pointer takes the lists' hover tint, about 1.35:1 on
// a dark view (at 1.3:1 it was faint), as drawn, with its number stepped up
// to stay 4.6:1 on it; a selected row keeps its selection tint over it, and
// the playing row its amber title.
void UiMainWindowTest::queueRowHoverIsTheListsTint() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *queueView = named(window, "playlistView");
    QObject *queue = window->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(queueView != nullptr && queue != nullptr);
    setQueue(bridge, sampleQueueRows(3));
    QTRY_COMPARE(queueView->property("count").toInt(), 3);
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRowAt(queueView, 0)) != nullptr);
    QQuickItem *hover = itemNamed(row, QStringLiteral("rowBackgroundHover"));
    QQuickItem *number = itemNamed(row, QStringLiteral("queueCell_order"));
    QVERIFY(hover != nullptr && number != nullptr);
    QVERIFY(!hover->isVisible());
    QCOMPARE(colorOf(number), tokenHex(window, "textTertiary"));

    QTest::mouseMove(window, sceneRect(row).center().toPoint());
    QTRY_VERIFY(hover->isVisible() && hover->opacity() == 1.0);
    QCOMPARE(colorOf(hover), tokenHex(window, "rowHover"));
    QCOMPARE(colorOf(number), tokenHex(window, "textTertiaryHovered"));
    // As drawn, at the row's left end, where it has no text.
    const QPointF corner = sceneRect(row).topLeft() + QPointF(2.5, 2.5);
    const QColor hovered = tokenOf(window, "hoveredRow");
    QTRY_VERIFY2(near(Drawn(window).at(corner), hovered),
                 qPrintable(Drawn(window).at(corner).name() + QStringLiteral(" for ") + hovered.name()));

    // Selected under the pointer: the selection tint over the hover's.
    QVERIFY(QMetaObject::invokeMethod(queue, "setSingleSelection", Q_ARG(QVariant, 0)));
    QTRY_VERIFY(rowTinted(queueView, 0));
    QQuickItem *tint = itemNamed(row, QStringLiteral("rowBackgroundTint"));
    QVERIFY(tint != nullptr && shown(tint));
    QVERIFY(tint->z() >= hover->z());
    QCOMPARE(tint->property("color").value<QColor>().alpha(), 255);
    QCOMPARE(colorOf(number), tokenHex(window, "textTertiarySelected"));
    QTest::mouseMove(window, QPoint(window->width() - 10, window->height() - 60));
    QTRY_VERIFY(!hover->isVisible());
}

void UiMainWindowTest::rowStatesShareOneRightEdge_data() {
    QTest::addColumn<bool>("queue");
    QTest::addColumn<bool>("scrolls");
    QTest::newRow("library, scrolling") << false << true;
    QTest::newRow("library") << false << false;
    QTest::newRow("queue, scrolling") << true << true;
    QTest::newRow("queue") << true << false;
}

// A selected range, its current row's ring and a hovered row have one
// straight right edge, in the library and in the queue: while the
// scrollbar shows, 2 px short of its lane, so the ring keeps clear of the
// thumb; at the pane's edge otherwise.
// The focused row was a box with a tail under the thumb.
void UiMainWindowTest::rowStatesShareOneRightEdge() {
    QFETCH(bool, queue);
    QFETCH(bool, scrolls);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    const int count = scrolls ? 80 : 6;
    QQuickItem *view = named(window, queue ? "playlistView" : "libraryView");
    QVERIFY(view != nullptr);
    if (queue) {
        setQueue(bridgeOf(window), sampleQueueRows(count));
    } else {
        fixture.libraryModel.setLibraryTreeFromBinary(manyArtistsTree(count));
    }
    QTRY_COMPARE(view->property("count").toInt(), count);
    tabInto(window, view);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
    QList<int> selected;
    QTRY_VERIFY([&] {
        selected.clear();
        for (int index = 0; index < 5; ++index) {
            if (rowTinted(view, index)) selected << index;
        }
        return selected.size() == 2;
    }());
    QCOMPARE(ringedRowsOf(view), QList<int>{selected.last()});

    QQuickItem *hoveredRow = viewRowAt(view, 4);
    QVERIFY(hoveredRow != nullptr && !selected.contains(4));
    // Two moves: the pointer may have been there in the row before, in
    // another window, and a move that goes nowhere is none.
    QTest::mouseMove(window, sceneRect(hoveredRow).center().toPoint() - QPoint(8, 0));
    QTest::mouseMove(window, sceneRect(hoveredRow).center().toPoint());
    QQuickItem *hover = itemNamed(hoveredRow, QStringLiteral("rowBackgroundHover"));
    QVERIFY(hover != nullptr);
    QTRY_VERIFY(hover->isVisible() && hover->opacity() == 1.0);

    const qreal edge = sceneRect(view).right();
    const qreal end = edge - (scrolls ? 8.0 + 2.0 : 0.0);
    QCOMPARE(sceneRect(hover).right(), end);
    for (const int index : std::as_const(selected)) {
        QCOMPARE(sceneRect(itemNamed(viewRowAt(view, index), QStringLiteral("rowBackgroundTint"))).right(), end);
    }
    if (!scrolls) {
        return;
    }
    const auto laneProblem = [&]() -> QString {
        const Drawn drawn(window);
        for (const int index : {selected.first(), selected.last(), 4}) {
            const qreal y = sceneRect(viewRowAt(view, index)).center().y();
            for (const qreal x : {end + 0.5, edge - 0.5}) {
                if (!near(drawn.at(x, y), tokenOf(window, "view"))) {
                    return QStringLiteral("%1 in the lane beside row %2").arg(drawn.at(x, y).name()).arg(index);
                }
            }
        }
        return {};
    };
    QTRY_VERIFY2(laneProblem().isEmpty(), qPrintable(laneProblem()));
}

// The transport is window chrome in the theme's Header colour set, as
// Breeze's toolbars and the menu bar above it are: its background, and the
// seek bar's and waveform's, are the menu bar's, from the platform theme
// rather than a fixed colour.
void UiMainWindowTest::transportIsWindowChromeInTheHeaderColours() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *transport = named(window, "transportBar");
    QQuickItem *menuBar = named(window, "mainMenuBar");
    QQuickItem *background = named(window, "transportBackground");
    QQuickItem *seekBackground = named(window, "seekBackground");
    QVERIFY(transport && menuBar && background && seekBackground);
    QQmlExpression colorSet(qmlContext(transport), transport,
                            QStringLiteral("Kirigami.Theme.colorSet === Kirigami.Theme.Header && !Kirigami.Theme.inherit"));
    bool undefined = false;
    const QVariant inHeader = colorSet.evaluate(&undefined);
    QVERIFY2(!colorSet.hasError() && !undefined, qPrintable(colorSet.error().toString()));
    QVERIFY(inHeader.toBool());

    auto *menuBackground = menuBar->property("background").value<QQuickItem *>();
    QVERIFY(menuBackground != nullptr);
    const QColor header = menuBackground->property("color").value<QColor>();
    QCOMPARE(transport->property("chromeColor").value<QColor>(), header);
    QCOMPARE(background->property("color").value<QColor>(), header);
    QCOMPARE(seekBackground->property("color").value<QColor>(), header);
    // The waveform lets the bar's chrome show through it.
    QCOMPARE(named(window, "seekWaveform")->property("bgColor").value<QColor>().alpha(), 0);
    // As drawn: one band from the menu bar through the transport.
    const QRectF menuRect = sceneRect(menuBar);
    const QRectF transportRect = sceneRect(transport);
    const auto bandProblem = [&]() -> QString {
        const Drawn drawn(window);
        const QColor menu = drawn.at(menuRect.right() - 4.5, menuRect.center().y());
        const QColor bar = drawn.at(transportRect.center().x(), transportRect.top() + 2.5);
        return menu.rgb() == header.rgb() && bar.rgb() == header.rgb()
            ? QString()
            : QStringLiteral("the menu bar %1, the transport %2, the header %3")
                  .arg(menu.name(), bar.name(), header.name());
    };
    QTRY_VERIFY2(bandProblem().isEmpty(), qPrintable(bandProblem()));
}

// Tab into the queue gives the focus to the row the keyboard is on, the
// one with the ring, so a screen reader reads its number, title, artist and
// state, and the keys go on from it. The bare list had the focus when the
// queue's rows came after the window opened, and a screen reader read only
// "Queue, list"; otherwise its first row did, wherever the ring was.
void UiMainWindowTest::tabIntoTheQueueFocusesItsCurrentRow() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    const bool accessibilityWasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    const auto restoreAccessibility = qScopeGuard([accessibilityWasActive] {
        QAccessible::setActive(accessibilityWasActive);
    });
    QObject *bridge = bridgeOf(window);
    QQuickItem *queueView = named(window, "playlistView");
    QObject *queue = window->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(queueView && queue);
    // What the focus is on, as a screen reader is told: the item with the
    // keyboard's focus, or the nearest item above it with a role.
    const auto focused = [window]() -> QAccessibleInterface * {
        for (QQuickItem *item = window->activeFocusItem(); item != nullptr; item = item->parentItem()) {
            QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(item);
            if (accessible != nullptr && accessible->role() != QAccessible::NoRole
                && accessible->role() != QAccessible::Client) {
                return accessible;
            }
        }
        return nullptr;
    };
    const auto focusedName = [&focused]() {
        QAccessibleInterface *accessible = focused();
        return accessible != nullptr ? accessible->text(QAccessible::Name) : QString();
    };

    // The rows come after the window has opened; the fourth plays.
    setQueue(bridge, sampleQueueRows(20));
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/3.flac"));
    bridge->setProperty("playingQueueIndex", 3);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_COMPARE(queueView->property("count").toInt(), 20);
    tabInto(window, queueView);
    QTRY_COMPARE(ringedRowsOf(queueView), QList<int>{3});
    QTRY_COMPARE(window->activeFocusItem(), viewRowAt(queueView, 3));
    QVERIFY(focused() != nullptr);
    QCOMPARE(focused()->role(), QAccessible::ListItem);
    QCOMPARE(focusedName(), QStringLiteral("4., Track 3, Artist 3, Album, 1:00, playing"));
    QVERIFY(focused()->state().focused);

    // Down goes on from it, and the focus with the ring.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(queue->property("selectedIndices").toList(), QVariantList{4});
    QTRY_COMPARE(ringedRowsOf(queueView), QList<int>{4});
    QTRY_COMPARE(window->activeFocusItem(), viewRowAt(queueView, 4));
    QCOMPARE(focusedName(), QStringLiteral("5., Track 4, Artist 4, Album, 1:00"));

    // Emptied, the list itself; filled again, stopped, the first row.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    bridge->setProperty("playingQueueIndex", -1);
    setQueue(bridge, QVariantList{});
    QVERIFY(QMetaObject::invokeMethod(queue, "clearSelection"));
    queue->setProperty("focusRowIndex", -1);
    QTRY_COMPARE(queueView->property("count").toInt(), 0);
    QTRY_VERIFY(queueView->hasActiveFocus());
    QCOMPARE(window->activeFocusItem(), queueView);
    QCOMPARE(focused()->role(), QAccessible::List);
    setQueue(bridge, sampleQueueRows(5));
    QTRY_COMPARE(queueView->property("count").toInt(), 5);
    tabInto(window, queueView);
    QTRY_COMPARE(ringedRowsOf(queueView), QList<int>{0});
    QTRY_COMPARE(window->activeFocusItem(), viewRowAt(queueView, 0));
    QCOMPARE(focusedName(), QStringLiteral("1., Track 0, Artist 0, Album, 1:00"));
}

// Tab into the library gives the focus to the row the keyboard is on, the
// one with the ring, so a screen reader reads its title and kind, and the
// keys go on from it. Its first row had the focus wherever the ring was:
// on the playing track's row, a clicked row, or one Show in Library chose.
// The focus stays with the row as rows above it open and close, as the row
// moves in a new tree, and through Collapse All.
void UiMainWindowTest::tabIntoTheLibraryFocusesItsCurrentRow() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    const bool accessibilityWasActive = QAccessible::isActive();
    QAccessible::setActive(true);
    const auto restoreAccessibility = qScopeGuard([accessibilityWasActive] {
        QAccessible::setActive(accessibilityWasActive);
    });
    QObject *bridge = bridgeOf(window);
    LibraryTreeModel &model = fixture.libraryModel;
    QQuickItem *view = named(window, "libraryView");
    QObject *library = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(view && library);
    // What the focus is on, as a screen reader is told: the item with the
    // keyboard's focus, or the nearest item above it with a role.
    const auto focused = [window]() -> QAccessibleInterface * {
        for (QQuickItem *item = window->activeFocusItem(); item != nullptr; item = item->parentItem()) {
            QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(item);
            if (accessible != nullptr && accessible->role() != QAccessible::NoRole
                && accessible->role() != QAccessible::Client) {
                return accessible;
            }
        }
        return nullptr;
    };
    // Empty when the focus is on row `index`, whose title is `title`, and
    // it has the ring; else what is wrong.
    const auto focusIsOn = [&](int index, const QString &title) -> QString {
        QQuickItem *row = viewRowAt(view, index);
        if (row == nullptr) {
            return QStringLiteral("no row %1").arg(index);
        }
        if (window->activeFocusItem() != row) {
            const QQuickItem *item = window->activeFocusItem();
            return QStringLiteral("the focus is on %1 (%2), not row %3")
                .arg(item != nullptr ? item->property("rowTitle").toString() : QStringLiteral("nothing"),
                     item != nullptr ? QString::fromLatin1(item->metaObject()->className()) : QString())
                .arg(index);
        }
        QAccessibleInterface *accessible = focused();
        if (accessible == nullptr || accessible->role() != QAccessible::TreeItem) {
            return QStringLiteral("not a tree item");
        }
        if (accessible->text(QAccessible::Name) != title) {
            return QStringLiteral("read as \"%1\"").arg(accessible->text(QAccessible::Name));
        }
        if (!accessible->state().focused) {
            return QStringLiteral("not read as focused");
        }
        if (ringedRowsOf(view) != QList<int>{index}) {
            return QStringLiteral("the ring is elsewhere");
        }
        return {};
    };

    // Two library folders, each an artist with an album of one track.
    const QString first = QStringLiteral("/music/Artist A/Album A/01.flac");
    const QString second = QStringLiteral("/shelf/Artist B/Album B/01.flac");
    const QString shelfKey = QStringLiteral("root|/shelf");
    const QString artistB = QStringLiteral("artist|Artist B");
    const QString albumB = QStringLiteral("album|Artist B|Album B");
    const QString firstKey = QStringLiteral("track|") + first;
    const QList<TreeRow> music{
        {0, 0, 0, 1, 0, QStringLiteral("Music"), kRootKey, QString(), QStringLiteral("/music"), {}},
        {1, 1, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A"), {}},
        {2, 2, 0, 1, 2005, QStringLiteral("Album A"), kAlbumKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 3, 1, 0, 0, QStringLiteral("First"), firstKey, QStringLiteral("Artist A"), first, first},
    };
    const QList<TreeRow> shelf{
        {0, 0, 0, 1, 0, QStringLiteral("Shelf"), shelfKey, QString(), QStringLiteral("/shelf"), {}},
        {1, 1, 0, 1, 0, QStringLiteral("Artist B (1)"), artistB, QStringLiteral("Artist B"),
         QStringLiteral("/shelf/Artist B"), {}},
        {2, 2, 0, 1, 2010, QStringLiteral("Album B"), albumB, QStringLiteral("Artist B"),
         QStringLiteral("/shelf/Artist B/Album B"), {}},
        {4, 3, 1, 0, 0, QStringLiteral("Other"), QStringLiteral("track|") + second, QStringLiteral("Artist B"), second,
         second},
    };
    const QStringList allExpanded{kRootKey, shelfKey, kArtistKey, kAlbumKey, artistB, albumB};
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music"), QStringLiteral("/shelf")});
    model.setExpandedKeys(allExpanded);
    model.setLibraryTreeFromBinary(treeOf(music + shelf));
    QTRY_COMPARE(view->property("count").toInt(), 8);

    // The second folder's track plays: Tab lands on its row.
    bridge->setProperty("currentTrackPath", second);
    bridge->setProperty("currentTrackTreeKeys", QStringList{shelfKey, artistB, albumB});
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    tabInto(window, view);
    QTRY_COMPARE(focusIsOn(7, QStringLiteral("Other")), QString());
    // Up goes on from it, and the focus with the ring.
    QTest::keyClick(window, Qt::Key_Up);
    QTRY_COMPARE(library->property("selectedSelectionKey").toString(), albumB);
    QTRY_COMPARE(focusIsOn(6, QStringLiteral("Album B")), QString());

    // Rows above it close and open: the focus stays on it.
    model.toggleKey(kArtistKey);
    QTRY_COMPARE(view->property("count").toInt(), 6);
    QTRY_COMPARE(focusIsOn(4, QStringLiteral("Album B")), QString());
    model.toggleKey(kArtistKey);
    QTRY_COMPARE(view->property("count").toInt(), 8);
    QTRY_COMPARE(focusIsOn(6, QStringLiteral("Album B")), QString());

    // A clicked row, Tab coming back to the library.
    QQuickItem *clicked = viewRowAt(view, 2);
    QVERIFY(clicked != nullptr);
    const QRectF clickedRect = sceneRect(clicked);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      QPointF(clickedRect.right() - 48, clickedRect.center().y()).toPoint());
    QTRY_COMPARE(library->property("selectedSelectionKey").toString(), kAlbumKey);
    tabInto(window, view);
    QTRY_COMPARE(focusIsOn(2, QStringLiteral("Album A")), QString());

    // Show in Library, from elsewhere.
    named(window, "transportButton_playPause")->forceActiveFocus(Qt::TabFocusReason);
    QVERIFY(QMetaObject::invokeMethod(bridge, "libraryRevealRequested",
                                      Q_ARG(QVariant, QVariant(QStringList{kRootKey, kArtistKey, kAlbumKey})),
                                      Q_ARG(QString, firstKey)));
    QTRY_COMPARE(library->property("selectedSelectionKey").toString(), firstKey);
    tabInto(window, view);
    QTRY_COMPARE(focusIsOn(3, QStringLiteral("First")), QString());

    // New trees: the shelf gains an album, then comes first. Its rows
    // stay, and the music folder's leave and come back after them, the
    // row with the focus among them.
    const QString third = QStringLiteral("/shelf/Artist B/Album C/01.flac");
    const QString albumC = QStringLiteral("album|Artist B|Album C");
    QList<TreeRow> largerShelf = shelf;
    largerShelf[1].childCount = 2;
    largerShelf.push_back({2, 2, 0, 1, 2012, QStringLiteral("Album C"), albumC, QStringLiteral("Artist B"),
                           QStringLiteral("/shelf/Artist B/Album C"), {}});
    largerShelf.push_back({4, 3, 1, 0, 0, QStringLiteral("Third"), QStringLiteral("track|") + third,
                           QStringLiteral("Artist B"), third, third});
    model.setExpandedKeys(allExpanded + QStringList{albumC});
    model.setLibraryTreeFromBinary(treeOf(music + largerShelf));
    QTRY_COMPARE(view->property("count").toInt(), 10);
    QTRY_COMPARE(focusIsOn(3, QStringLiteral("First")), QString());
    model.setLibraryTreeFromBinary(treeOf(largerShelf + music));
    QTRY_COMPARE(model.indexForSelectionKey(firstKey), 9);
    QTRY_COMPARE(focusIsOn(9, QStringLiteral("First")), QString());

    // Collapse All leaves the folders' rows; the row went with its
    // folder's children, so Tab rings one of them, and the focus is on it.
    QVERIFY(QMetaObject::invokeMethod(library, "collapseAll"));
    QTRY_COMPARE(view->property("count").toInt(), 2);
    library->setProperty("focusSelectionKey", QString());
    tabInto(window, view);
    QTRY_COMPARE(ringedRowsOf(view).size(), 1);
    const int ringed = ringedRowsOf(view).constFirst();
    QTRY_COMPARE(focusIsOn(ringed, viewRowAt(view, ringed)->property("rowTitle").toString()), QString());
}

// Tab into the library or the queue with no row selected: a row it shows
// becomes the current one and has the 1 px ring, so the keyboard's place
// shows. Nothing is selected and the view stays where it is.
void UiMainWindowTest::listsRingARowAsTheKeyboardComesIn() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    LibraryTreeModel &libraryModel = fixture.libraryModel;
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(libraryModel, kAllExpanded);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    QQuickItem *libraryView = named(window, "libraryView");
    QQuickItem *queueView = named(window, "playlistView");
    QObject *library = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QObject *queue = window->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(libraryView && queueView && library && queue);
    // Whether row `index` of `view` has the ring alone: no selection tint.
    const auto ringedAlone = [](QQuickItem *view, int index) {
        QQuickItem *row = viewRowAt(view, index);
        QQuickItem *tint = row != nullptr ? itemNamed(row, QStringLiteral("rowBackgroundTint")) : nullptr;
        return shown(tint) && QQmlProperty(tint, QStringLiteral("border.width")).read().toReal() == 1.0
            && tint->property("color").value<QColor>().alpha() == 0;
    };

    // The library: its first row, with nothing selected.
    tabInto(window, libraryView);
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{0});
    QVERIFY(ringedAlone(libraryView, 0));
    QCOMPARE(library->property("selectedSelectionKey").toString(), QString());
    QCOMPARE(libraryModel.selectedKeys(), QStringList());
    // The keys go on from it: Down selects the next row.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(library->property("selectedSelectionKey").toString(),
                 viewRowAt(libraryView, 1)->property("selectionKeyResolved").toString());
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{1});
    QVERIFY(!ringedAlone(libraryView, 1));
    // Deselected, the row stays the current one.
    QVERIFY(QMetaObject::invokeMethod(library, "clearPrimarySelection"));
    libraryModel.clearSelection();
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{1});
    QVERIFY(ringedAlone(libraryView, 1));

    // Playing, a fresh tree rings the playing track's row,
    playTrack(bridge);
    library->setProperty("focusSelectionKey", QString());
    tabInto(window, libraryView);
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{2});
    QVERIFY(ringedAlone(libraryView, 2));
    // or the collapsed row that holds it.
    library->setProperty("focusSelectionKey", QString());
    libraryModel.toggleKey(kAlbumKey);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) == nullptr);
    tabInto(window, libraryView);
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{1});
    QCOMPARE(libraryModel.selectedKeys(), QStringList());
    // Without the keyboard, no ring.
    tabInto(window, queueView);
    QTRY_COMPARE(ringedRowsOf(libraryView), QList<int>{});

    // The queue, scrolled down: its first row shown, and it stays put.
    setQueue(bridge, sampleQueueRows(80));
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(queueView->property("count").toInt(), 80);
    const qreal rowHeight = queueView->property("rowHeight").toReal();
    QVERIFY(rowHeight > 0);
    named(window, "transportButton_playPause")->forceActiveFocus(Qt::TabFocusReason);
    queueView->setProperty("contentY", 10 * rowHeight);
    QTRY_VERIFY(viewRowAt(queueView, 10) != nullptr);
    tabInto(window, queueView);
    QTRY_COMPARE(ringedRowsOf(queueView), QList<int>{10});
    QVERIFY(ringedAlone(queueView, 10));
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    QCOMPARE(queue->property("selectedIndices").toList().size(), 0);
    // Enter plays the ringed row.
    QJSValue played = fixture.engine.newObject();
    queue->setProperty("playAt", QVariant::fromValue(
        fixture.engine.evaluate(QStringLiteral("(function(played) { return function(index) { played.index = index } })"))
            .call({played})));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(played.property(QStringLiteral("index")).toInt(), 10);
    // The playing row, when it shows,
    bridge->setProperty("playingQueueIndex", 14);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    queue->setProperty("focusRowIndex", -1);
    tabInto(window, queueView);
    QTRY_COMPARE(ringedRowsOf(queueView), QList<int>{14});
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    // but not one off the view: the queue does not move to it.
    bridge->setProperty("playingQueueIndex", 2);
    queue->setProperty("focusRowIndex", -1);
    tabInto(window, queueView);
    QTRY_COMPARE(ringedRowsOf(queueView), QList<int>{10});
    QCOMPARE(queueView->property("contentY").toReal(), 10 * rowHeight);
    QCOMPARE(queue->property("selectedIndices").toList().size(), 0);
    // Down goes on from it, selecting the next row.
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_COMPARE(queue->property("selectedIndices").toList(), QVariantList{11});
    QTRY_COMPARE(ringedRowsOf(queueView), QList<int>{11});
}

// The current-row ring shows where the keyboard is, so it shows only
// while the keyboard is in use: not at launch, where the library has the
// focus, and not after a click, which selects with the tint alone. Tab or a
// key press brings it back.
void UiMainWindowTest::listsRingTheCurrentRowOnlyForTheKeyboard() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(fixture.libraryModel, kAllExpanded);
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    setQueue(bridge, sampleQueueRows(5));
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
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(errors != nullptr);
    showTree(fixture.libraryModel, kAllExpanded);
    QVariantList rows = sampleQueueRows(2);
    QVariantMap second = rows.at(1).toMap();
    second.insert(QStringLiteral("path"), kTrackPath);
    rows[1] = second;
    setQueue(bridge, rows);
    QQuickItem *track = nullptr;
    QTRY_VERIFY((track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    const auto unreadablePaths = [errors]() { return errors->property("unreadablePaths").toMap(); };

    // A warning, or an error about another kind of thing, marks nothing.
    // Category 1 is playback, 2 the library; severity 0 a warning, 1 an error.
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, kTrackPath), Q_ARG(QVariant, QStringLiteral("Slow")),
                                      Q_ARG(QVariant, QString())));
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 2), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath), Q_ARG(QVariant, QStringLiteral("Scan")),
                                      Q_ARG(QVariant, QString())));
    QVERIFY(unreadablePaths().isEmpty());
    // A playback error about the file marks its rows: the status bar's
    // error mark at its size, in the negative colour, centred where the
    // other marks are, and the title in the secondary colour.
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath),
                                      Q_ARG(QVariant, QStringLiteral("The file cannot be read.")),
                                      Q_ARG(QVariant, QString())));
    QCOMPARE(unreadablePaths().value(kTrackPath).toString(), QStringLiteral("The file cannot be read."));
    QQuickItem *treeGlyph = itemNamed(track, QStringLiteral("libraryRowState"));
    QTRY_VERIFY(shown(treeGlyph));
    QCOMPARE(stateGlyphName(treeGlyph), QStringLiteral("status-error"));
    QCOMPARE(colorOf(treeGlyph), tokenHex(window, "negative"));
    QVERIFY(treeGlyph->property("isMask").toBool());
    const int markSize = scaledSize(window, 16);
    QCOMPARE(treeGlyph->width(), qreal(markSize));
    QCOMPARE(treeGlyph->height(), qreal(markSize));
    // In the middle of the row, in the chevron column it fills, clear of
    // the thumbnail after it.
    QVERIFY(std::abs(treeGlyph->y() + treeGlyph->height() / 2 - track->height() / 2) <= 0.5);
    QCOMPARE(treeGlyph->x(), track->property("stateX").toReal());
    QQuickItem *thumbnail = itemNamed(track, QStringLiteral("libraryRowThumbnail"));
    QVERIFY(thumbnail != nullptr);
    QVERIFY2(treeGlyph->x() + treeGlyph->width() < thumbnail->x(),
             qPrintable(QStringLiteral("%1 + %2 vs %3").arg(treeGlyph->x()).arg(treeGlyph->width()).arg(thumbnail->x())));
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
    QCOMPARE(stateGlyphName(queueGlyph), QStringLiteral("status-error"));
    QCOMPARE(colorOf(queueGlyph), tokenHex(window, "negative"));
    QVERIFY(queueGlyph->property("isMask").toBool());
    QCOMPARE(queueGlyph->width(), qreal(markSize));
    QCOMPARE(queueGlyph->height(), qreal(markSize));
    // Centred in its column and the row, on whole pixels, clear of the
    // number column.
    QQuickItem *indicatorCell = itemNamed(queueRow, QStringLiteral("queueCell_indicator"));
    QVERIFY(indicatorCell != nullptr);
    const QPointF glyphCentre = queueGlyph->mapToItem(queueRow, QPointF(markSize / 2.0, markSize / 2.0));
    QVERIFY(std::abs(glyphCentre.x() - (indicatorCell->x() + indicatorCell->width() / 2)) <= 0.5);
    QVERIFY(std::abs(glyphCentre.y() - queueRow->height() / 2) <= 0.5);
    QCOMPARE(queueGlyph->x(), std::round(queueGlyph->x()));
    QCOMPARE(queueGlyph->y(), std::round(queueGlyph->y()));
    QVERIFY(queueGlyph->mapToItem(queueRow, QPointF(markSize, 0)).x()
            <= itemNamed(queueRow, QStringLiteral("queueCell_order"))->x());
    // The reason is the row's, for the pointer anywhere on it and for the
    // keyboard (anUnreadableRowTellsWhyToThePointerAndTheKeyboard).
    QObject *reason = nullptr;
    QTRY_VERIFY((reason = queueRow->findChild<QObject *>(QStringLiteral("queueRowReason"))) != nullptr);
    QCOMPARE(reason->property("message").toString(), QStringLiteral("The file cannot be read."));
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_title"))), tokenHex(window, "textSecondary"));
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_artist"))), tokenHex(window, "textSecondary"));
    QVERIFY(QQmlProperty(queueRow, QStringLiteral("Accessible.name"), qmlContext(queueRow))
                .read().toString().endsWith(QStringLiteral(", The file cannot be read.")));
    // Selected, the unreadable titles step up with the rows' other secondary
    // text, over their figures (selectedRowTextsNeverReadUnderTheirFigures).
    QObject *queueController = window->findChild<QObject *>(QStringLiteral("queueController"));
    QVERIFY(queueController != nullptr);
    QVERIFY(QMetaObject::invokeMethod(queueController, "setSingleSelection", Q_ARG(QVariant, 1)));
    QTRY_COMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_title"))),
                 tokenHex(window, "textSecondarySelected"));
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_artist"))), tokenHex(window, "textSecondarySelected"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, sceneRect(track).center().toPoint());
    QTRY_VERIFY(track->property("rowSelected").toBool());
    QCOMPARE(colorOf(itemNamed(track, QStringLiteral("libraryRowTitle"))), tokenHex(window, "textSecondarySelected"));
    // Clearing the history clears the marks.
    QVERIFY(QMetaObject::invokeMethod(errors, "clear"));
    QTRY_VERIFY(!treeGlyph->isVisible());
    QTRY_VERIFY(!queueGlyph->isVisible());
    // Still selected.
    QCOMPARE(colorOf(itemNamed(queueRow, QStringLiteral("queueCell_artist"))), tokenHex(window, "textColumnSelected"));
}

// A file that fails to play stays the current row, stopped, as the backend
// leaves it when it was the queue's last: the queue's row and the tree's
// show the unreadable error mark, whose tooltip gives the reason, not the
// stop mark (the mark came only once another row played). Played again, the
// row shows what the playback does; a readable track that stops keeps the
// stop mark.
void UiMainWindowTest::aTrackThatFailsToPlayShowsTheWarningWhileCurrent() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QQuickItem *queueView = named(window, "playlistView");
    QVERIFY(bridge && errors && queueView);
    showTree(fixture.libraryModel, kAllExpanded);
    QVariantList rows = sampleQueueRows(4);
    QVariantMap last = rows.at(3).toMap();
    last.insert(QStringLiteral("path"), kTrackPath);
    rows[3] = last;
    setQueue(bridge, rows);
    QQuickItem *track = nullptr;
    QTRY_VERIFY((track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    QTRY_COMPARE(queueView->property("count").toInt(), 4);
    const auto queueGlyph = [queueView](int index) {
        QQuickItem *row = viewRowAt(queueView, index);
        QQuickItem *glyph = row != nullptr ? itemNamed(row, QStringLiteral("queueRowState")) : nullptr;
        return shown(glyph) ? stateGlyphName(glyph) : QString();
    };
    QQuickItem *treeGlyph = itemNamed(track, QStringLiteral("libraryRowState"));
    QVERIFY(treeGlyph != nullptr);
    const auto treeGlyphName = [treeGlyph]() {
        return shown(treeGlyph) ? stateGlyphName(treeGlyph) : QString();
    };

    // The last row starts to play, and fails: the bridge's playback error
    // about the file, and playback stops with the row still current.
    bridge->setProperty("currentTrackPath", kTrackPath);
    bridge->setProperty("playingQueueIndex", 3);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_COMPARE(queueGlyph(3), QStringLiteral("media-playback-start"));
    QTRY_COMPARE(treeGlyphName(), QStringLiteral("media-playback-start"));
    // What playback did is the message's, why the file failed its system
    // text; the row tells only the latter (unreadableRowsTellWhatFailedNotWhatPlaybackDid).
    QVERIFY(QMetaObject::invokeMethod(bridge, "bridgeError",
                                      Q_ARG(QString, QStringLiteral("Couldn't play “02.flac”; playback stopped.")),
                                      Q_ARG(int, 1), Q_ARG(int, 1), Q_ARG(QString, kTrackPath),
                                      Q_ARG(QString, QStringLiteral("No valid frames found before end of stream"))));
    const QString reason = QStringLiteral("Couldn't play “02.flac” (No valid frames found before end of stream).");
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(queueGlyph(3), QStringLiteral("status-error"));
    QCOMPARE(treeGlyphName(), QStringLiteral("status-error"));
    QQuickItem *row = viewRowAt(queueView, 3);
    QVERIFY(row != nullptr && row->property("isCurrentQueueRow").toBool());
    QQuickItem *glyph = itemNamed(row, QStringLiteral("queueRowState"));
    QCOMPARE(colorOf(glyph), tokenHex(window, "negative"));
    QCOMPARE(glyph->width(), qreal(scaledSize(window, 16)));
    QCOMPARE(colorOf(itemNamed(row, QStringLiteral("queueCell_title"))), tokenHex(window, "textSecondary"));
    QCOMPARE(row->property("unreadableReason").toString(), reason);
    QCOMPARE(colorOf(treeGlyph), tokenHex(window, "negative"));
    // The other rows show nothing.
    for (int index = 0; index < 3; ++index) {
        QCOMPARE(queueGlyph(index), QString());
    }

    // Another row plays: the failed one keeps its mark.
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/2.flac"));
    bridge->setProperty("playingQueueIndex", 2);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_COMPARE(queueGlyph(2), QStringLiteral("media-playback-start"));
    QCOMPARE(queueGlyph(3), QStringLiteral("status-error"));
    QCOMPARE(treeGlyphName(), QStringLiteral("status-error"));
    // A readable track that stops shows the stop mark.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(queueGlyph(2), QStringLiteral("media-playback-stop"));
    QCOMPARE(queueGlyph(3), QStringLiteral("status-error"));

    // Played again, say once the file is fixed, the row shows the playback
    // until it stops.
    bridge->setProperty("currentTrackPath", kTrackPath);
    bridge->setProperty("playingQueueIndex", 3);
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_COMPARE(queueGlyph(3), QStringLiteral("media-playback-pause"));
    QCOMPARE(colorOf(itemNamed(viewRowAt(queueView, 3), QStringLiteral("queueRowState"))), tokenHex(window, "now"));
    // The transport's marks keep their size.
    QCOMPARE(itemNamed(viewRowAt(queueView, 3), QStringLiteral("queueRowState"))->width(),
             qreal(scaledSize(window, 12)));
    QTRY_COMPARE(treeGlyphName(), QStringLiteral("media-playback-pause"));
    QCOMPARE(queueGlyph(2), QString());

    // With the issue cleared, a stopped current row is the stop mark again.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(queueGlyph(3), QStringLiteral("status-error"));
    QVERIFY(QMetaObject::invokeMethod(errors, "clear"));
    QTRY_COMPARE(queueGlyph(3), QStringLiteral("media-playback-stop"));
    QTRY_COMPARE(treeGlyphName(), QStringLiteral("media-playback-stop"));
}

// An unreadable file's rows have the error's mark, the ring with a cross
// that the status bar and Recent Issues give the same failure, in the
// negative colour, not the warning's triangle: the triangle in red on the
// row and grey in Recent Issues gave one event two marks and the triangle
// two meanings.
void UiMainWindowTest::unreadableRowsHaveTheErrorMarkNotTheWarnings() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QQuickItem *queueView = named(window, "playlistView");
    QVERIFY(bridge && errors && queueView);
    showTree(fixture.libraryModel, kAllExpanded);
    QVariantList rows = sampleQueueRows(3);
    QVariantMap failed = rows.at(1).toMap();
    failed.insert(QStringLiteral("path"), kTrackPath);
    rows[1] = failed;
    setQueue(bridge, rows);
    QTRY_COMPARE(queueView->property("count").toInt(), 3);
    QVERIFY(QMetaObject::invokeMethod(bridge, "bridgeError",
                                      Q_ARG(QString, QStringLiteral("Couldn't play “02.flac”; playback stopped.")),
                                      Q_ARG(int, 1), Q_ARG(int, 1), Q_ARG(QString, kTrackPath),
                                      Q_ARG(QString, QStringLiteral("Resource not found"))));
    QQuickItem *statusGlyph = nullptr;
    QTRY_VERIFY(shown(statusGlyph = itemNamed(window->contentItem(), QStringLiteral("statusBarErrorsGlyph"))));
    QVERIFY(statusGlyph->property("source").toString().endsWith(QStringLiteral("/assets/status-error.svg")));
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRowAt(queueView, 1)) != nullptr && row->property("unreadable").toBool());
    QQuickItem *queueGlyph = itemNamed(row, QStringLiteral("queueRowState"));
    QTRY_VERIFY(shown(queueGlyph));
    QCOMPARE(queueGlyph->property("source").toString(), statusGlyph->property("source").toString());
    QCOMPARE(colorOf(queueGlyph), tokenHex(window, "negative"));
    QCOMPARE(colorOf(queueGlyph), colorOf(statusGlyph));
    QCOMPARE(queueGlyph->width(), statusGlyph->width());
    QQuickItem *track = nullptr;
    QTRY_VERIFY((track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    QQuickItem *treeGlyph = itemNamed(track, QStringLiteral("libraryRowState"));
    QTRY_VERIFY(shown(treeGlyph));
    QCOMPARE(treeGlyph->property("source").toString(), statusGlyph->property("source").toString());
    QCOMPARE(colorOf(treeGlyph), tokenHex(window, "negative"));
    QCOMPARE(treeGlyph->width(), statusGlyph->width());
    // Selected, the negative colour for the selection, the same mark.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      sceneCentre(itemNamed(row, QStringLiteral("queueCell_title"))));
    QTRY_VERIFY(rowTinted(queueView, 1));
    QCOMPARE(stateGlyphName(queueGlyph), QStringLiteral("status-error"));
    QCOMPARE(colorOf(queueGlyph), tokenHex(window, "negativeSelected"));
}

// The tooltip that says why the row `row` of a list is marked unreadable,
// or null while it has none.
QObject *rowReasonOf(QQuickItem *row, const QString &name) {
    return row != nullptr ? row->findChild<QObject *>(name) : nullptr;
}

bool reasonOpen(QObject *reason) {
    return reason != nullptr && reason->property("opened").toBool();
}

// Where the tooltip `reason` of the row `row` is, in the window.
QRectF reasonSceneRect(QQuickItem *row, QObject *reason) {
    return row->mapRectToScene(QRectF(reason->property("x").toReal(), reason->property("y").toReal(),
                                      reason->property("width").toReal(), reason->property("height").toReal()));
}

// The label of the tooltip `reason`, which wraps its text.
QQuickItem *reasonLabelOf(QObject *reason) {
    return qvariant_cast<QQuickItem *>(reason->property("contentItem"));
}

// An unreadable row of the queue and of the library tells why it is marked
// to the pointer anywhere on the row and to the keyboard on it, not only on
// its 16 px glyph, where the pointer had to find it and the keyboard never
// could. The reason sits over the row from its title, and goes while the
// pointer holds the row down, and when the pointer or the keyboard leaves
// it. A list gives one reason at a time: the keyboard's row gives way to
// the pointer's. A readable row has none.
void UiMainWindowTest::anUnreadableRowTellsWhyToThePointerAndTheKeyboard() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QQuickItem *queueView = named(window, "playlistView");
    QQuickItem *libraryView = named(window, "libraryView");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(bridge && errors && queueView && libraryView && controller);
    showTree(fixture.libraryModel, kAllExpanded);
    QVariantList rows = sampleQueueRows(4);
    QVariantMap last = rows.at(3).toMap();
    last.insert(QStringLiteral("path"), kTrackPath);
    rows[3] = last;
    setQueue(bridge, rows);
    QTRY_COMPARE(queueView->property("count").toInt(), 4);
    QQuickItem *track = nullptr;
    QTRY_VERIFY((track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    const QString why = QStringLiteral("The file cannot be read.");
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath), Q_ARG(QVariant, why),
                                      Q_ARG(QVariant, QString())));
    QQuickItem *row = viewRowAt(queueView, 3);
    QTRY_VERIFY(row != nullptr && row->property("unreadable").toBool());
    QObject *reason = nullptr;
    QTRY_VERIFY((reason = rowReasonOf(row, QStringLiteral("queueRowReason"))) != nullptr);
    QCOMPARE(reason->property("message").toString(), why);
    QCOMPARE(reason->property("text").toString(), why);
    for (int index = 0; index < 3; ++index) {
        QCOMPARE(rowReasonOf(viewRowAt(queueView, index), QStringLiteral("queueRowReason")), nullptr);
    }
    const QPoint away(window->width() - 10, window->height() - 60);
    QTest::mouseMove(window, away);
    QVERIFY(!reasonOpen(reason));

    // The pointer on the title, far from the glyph: the reason, from the
    // title's start, below the row, where the view has room
    // (unreadableRowsReasonOpensBelowClearOfThePlayingRow).
    QQuickItem *title = itemNamed(row, QStringLiteral("queueCell_title"));
    QVERIFY(title != nullptr);
    const QPoint onTitle = sceneRect(title).center().toPoint();
    QTest::mouseMove(window, onTitle - QPoint(8, 0));
    QTest::mouseMove(window, onTitle);
    QTRY_VERIFY(reasonOpen(reason));
    QCOMPARE(reason->property("x").toReal(), title->x());
    QCOMPARE(reason->property("y").toReal(), row->height() + 3);
    // Held down, as a drag starts: none; let go: again.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, onTitle);
    QTRY_VERIFY(!reasonOpen(reason));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, onTitle);
    QTRY_VERIFY(reasonOpen(reason));
    QTest::mouseMove(window, away);
    QTRY_VERIFY(!reasonOpen(reason));

    // The keyboard on the row: the reason; off it: none.
    tabInto(window, queueView);
    QTest::keyClick(window, Qt::Key_Home);
    QTRY_VERIFY(viewRowAt(queueView, 0)->property("rowCurrent").toBool());
    QVERIFY(!reasonOpen(reason));
    QTest::keyClick(window, Qt::Key_End);
    QTRY_VERIFY(row->property("rowCurrent").toBool());
    QTRY_VERIFY(reasonOpen(reason));
    QTest::keyClick(window, Qt::Key_Up);
    QTRY_VERIFY(!reasonOpen(reason));

    // The same file twice: the pointer on the one, the keyboard on the
    // other. The pointer's reason alone, then, off it, the keyboard's.
    QVariantMap first = rows.at(1).toMap();
    first.insert(QStringLiteral("path"), kTrackPath);
    rows[1] = first;
    setQueue(bridge, rows);
    QQuickItem *other = nullptr;
    QTRY_VERIFY((other = viewRowAt(queueView, 1)) != nullptr && other->property("unreadable").toBool());
    row = viewRowAt(queueView, 3);
    QObject *otherReason = nullptr;
    QTRY_VERIFY((otherReason = rowReasonOf(other, QStringLiteral("queueRowReason"))) != nullptr);
    QTRY_VERIFY((reason = rowReasonOf(row, QStringLiteral("queueRowReason"))) != nullptr);
    QTest::keyClick(window, Qt::Key_End);
    QTRY_VERIFY(row->property("rowCurrent").toBool());
    QTRY_VERIFY(reasonOpen(reason));
    const QPoint onOther = sceneRect(itemNamed(other, QStringLiteral("queueCell_title"))).center().toPoint();
    QTest::mouseMove(window, onOther - QPoint(8, 0));
    QTest::mouseMove(window, onOther);
    QTRY_VERIFY(reasonOpen(otherReason));
    QTRY_VERIFY(!reasonOpen(reason));
    QVERIFY(row->property("rowCurrent").toBool());
    const QPoint onReadable = sceneRect(itemNamed(viewRowAt(queueView, 0), QStringLiteral("queueCell_title")))
                                  .center().toPoint();
    QTest::mouseMove(window, onReadable);
    QTRY_VERIFY(!reasonOpen(otherReason));
    QTRY_VERIFY(reasonOpen(reason));
    QTest::mouseMove(window, away);

    // The library's track row, alike.
    QObject *treeReason = nullptr;
    QTRY_VERIFY((treeReason = rowReasonOf(track, QStringLiteral("libraryRowReason"))) != nullptr);
    QCOMPARE(treeReason->property("message").toString(), why);
    QCOMPARE(rowReasonOf(libraryRow(window, QStringLiteral("album")), QStringLiteral("libraryRowReason")), nullptr);
    QQuickItem *treeTitle = itemNamed(track, QStringLiteral("libraryRowTitle"));
    QVERIFY(treeTitle != nullptr);
    const QPoint onTreeTitle = sceneRect(treeTitle).center().toPoint();
    QTest::mouseMove(window, onTreeTitle - QPoint(8, 0));
    QTest::mouseMove(window, onTreeTitle);
    QTRY_VERIFY(reasonOpen(treeReason));
    QCOMPARE(treeReason->property("x").toReal(), track->property("titleX").toReal());
    QTest::mouseMove(window, away);
    QTRY_VERIFY(!reasonOpen(treeReason));
    int trackIndex = -1;
    for (int index = 0; index < libraryView->property("count").toInt(); ++index) {
        if (viewRowAt(libraryView, index) == track) {
            trackIndex = index;
        }
    }
    QVERIFY(trackIndex > 0);
    tabInto(window, libraryView);
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 0)));
    QTRY_VERIFY(!track->property("rowCurrent").toBool());
    QVERIFY(!reasonOpen(treeReason));
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, trackIndex)));
    QTRY_VERIFY(track->property("rowCurrent").toBool());
    QTRY_VERIFY(reasonOpen(treeReason));
}

// The reason on an unreadable file's rows says what failed and why, the
// system's text that came apart from the message, and not what playback
// did then: "playback stopped" was stale once another row played, beside
// that row's play mark. Recent Issues keeps the whole message.
void UiMainWindowTest::unreadableRowsTellWhatFailedNotWhatPlaybackDid() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QQuickItem *queueView = named(window, "playlistView");
    QVERIFY(bridge && errors && queueView);
    showTree(fixture.libraryModel, kAllExpanded);
    QVariantList rows = sampleQueueRows(4);
    QVariantMap failed = rows.at(1).toMap();
    failed.insert(QStringLiteral("path"), kTrackPath);
    rows[1] = failed;
    setQueue(bridge, rows);
    QTRY_COMPARE(queueView->property("count").toInt(), 4);
    QVERIFY(QMetaObject::invokeMethod(bridge, "bridgeError",
                                      Q_ARG(QString, QStringLiteral("Couldn't play “02.flac”; playback stopped.")),
                                      Q_ARG(int, 1), Q_ARG(int, 1), Q_ARG(QString, kTrackPath),
                                      Q_ARG(QString, QStringLiteral("No valid frames found before end of stream"))));
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/2.flac"));
    bridge->setProperty("playingQueueIndex", 2);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    const QString why = QStringLiteral("Couldn't play “02.flac” (No valid frames found before end of stream).");
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRowAt(queueView, 1)) != nullptr && row->property("unreadable").toBool());
    QObject *reason = nullptr;
    QTRY_VERIFY((reason = rowReasonOf(row, QStringLiteral("queueRowReason"))) != nullptr);
    QCOMPARE(reason->property("message").toString(), why);
    QVERIFY(QQmlProperty(row, QStringLiteral("Accessible.name"), qmlContext(row))
                .read().toString().endsWith(QStringLiteral(", ") + why));
    QQuickItem *track = nullptr;
    QTRY_VERIFY((track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    QObject *treeReason = nullptr;
    QTRY_VERIFY((treeReason = rowReasonOf(track, QStringLiteral("libraryRowReason"))) != nullptr);
    QCOMPARE(treeReason->property("message").toString(), why);
    // Recent Issues has the message as it came, and the system's text.
    QCOMPARE(errors->property("count").toInt(), 1);
    QObject *latest = qvariant_cast<QObject *>(errors->property("latest"));
    QVERIFY(latest != nullptr);
    QCOMPARE(latest->property("message").toString(), QStringLiteral("Couldn't play “02.flac”; playback stopped."));
    QCOMPARE(latest->property("detail").toString(), QStringLiteral("No valid frames found before end of stream"));
}

// An unreadable row's reason opens below the row, from its title, where it
// fits in the view, and above it only near the view's bottom: above, it hid
// the titles of every row over a queue's failed row, the playing one's
// among them, with the view's room below unused. It keeps the playing row
// clear, above the view's top if need be. It is as wide as its text up to
// 66 average characters of its font, a line or two for a typical reason (the style's
// tooltip wrapped it to three narrow lines at any font size), within the
// row.
void UiMainWindowTest::unreadableRowsReasonOpensBelowClearOfThePlayingRow() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QQuickItem *queueView = named(window, "playlistView");
    QQuickItem *libraryView = named(window, "libraryView");
    QObject *controller = window->findChild<QObject *>(QStringLiteral("libraryController"));
    QVERIFY(bridge && errors && queueView && libraryView && controller);
    const QString later = QStringLiteral("/music/Artist A/Album A/03.flac");
    fixture.libraryModel.setExpandedKeys(kAllExpanded);
    fixture.libraryModel.setLibraryTreeFromBinary(treeOf({
        {1, 0, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A"), {}},
        {2, 1, 0, 2, 2005, QStringLiteral("Album A"), kAlbumKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 2, 2, 0, 0, QStringLiteral("Second"), QStringLiteral("track|") + kTrackPath, QStringLiteral("Artist A"),
         kTrackPath, kTrackPath},
        {4, 2, 3, 0, 0, QStringLiteral("Third"), QStringLiteral("track|") + later, QStringLiteral("Artist A"), later,
         later},
    }));
    QVariantList rows = sampleQueueRows(60);
    QVariantMap failed = rows.at(3).toMap();
    failed.insert(QStringLiteral("path"), kTrackPath);
    rows[3] = failed;
    QVariantMap last = rows.at(59).toMap();
    last.insert(QStringLiteral("path"), kTrackPath);
    rows[59] = last;
    setQueue(bridge, rows);
    QTRY_COMPARE(queueView->property("count").toInt(), 60);
    const QString detail = QStringLiteral("No valid frames found before end of stream");
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath),
                                      Q_ARG(QVariant, QStringLiteral("Couldn't play “02.flac”; playback stopped.")),
                                      Q_ARG(QVariant, detail)));
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRowAt(queueView, 3)) != nullptr && row->property("unreadable").toBool());
    QObject *reason = nullptr;
    QTRY_VERIFY((reason = rowReasonOf(row, QStringLiteral("queueRowReason"))) != nullptr);
    QQuickItem *title = itemNamed(row, QStringLiteral("queueCell_title"));
    QVERIFY(title != nullptr);
    const qreal below = row->height() + 3;
    const auto above = [reason]() { return -3 - reason->property("height").toReal(); };

    // The keyboard on the row: below it, with the playing row above.
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/2.flac"));
    bridge->setProperty("playingQueueIndex", 2);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    tabInto(window, queueView);
    QTest::keyClick(window, Qt::Key_Home);
    for (int step = 0; step < 3; ++step) {
        QTest::keyClick(window, Qt::Key_Down);
    }
    QTRY_VERIFY(row->property("rowCurrent").toBool());
    QTRY_VERIFY(reasonOpen(reason));
    QCOMPARE(reason->property("x").toReal(), title->x());
    QCOMPARE(reason->property("y").toReal(), below);
    // The text on a line or two, within the row's states.
    QQuickItem *label = reasonLabelOf(reason);
    QVERIFY(label != nullptr);
    QVERIFY2(label->property("lineCount").toInt() <= 2, qPrintable(QString::number(label->property("lineCount").toInt())));
    const QRectF tip = reasonSceneRect(row, reason);
    const QRectF rowRect = sceneRect(row);
    QVERIFY2(tip.right() <= rowRect.right() - queueView->property("rowStatesEnd").toReal(),
             qPrintable(QStringLiteral("%1 vs %2").arg(tip.right()).arg(rowRect.right())));
    QVERIFY(!tip.intersects(sceneRect(viewRowAt(queueView, 2))));
    QVERIFY(!tip.intersects(sceneRect(viewRowAt(queueView, 0))));

    // A reason too long for a line or two, from a newer error about the
    // file, wraps at 66 average characters of its font, inside the row.
    const auto reportWhy = [errors](const QString &why) {
        return QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 1),
                                         Q_ARG(QVariant, kTrackPath),
                                         Q_ARG(QVariant, QStringLiteral("Couldn't play “02.flac”; playback stopped.")),
                                         Q_ARG(QVariant, why));
    };
    QVERIFY(reportWhy(detail.repeated(4)));
    QTRY_VERIFY(label->property("lineCount").toInt() > 2);
    const qreal capped = reason->property("maximumTextWidth").toReal();
    QVERIFY(capped > 0);
    QCOMPARE(label->width(), capped);
    QVERIFY(reasonSceneRect(row, reason).right() <= rowRect.right());
    QVERIFY(reportWhy(detail));
    QTRY_VERIFY(label->property("lineCount").toInt() <= 2);

    // The playing row just below: above, clear of it; stopped, below again.
    bridge->setProperty("playingQueueIndex", 4);
    QTRY_COMPARE(reason->property("y").toReal(), above());
    QVERIFY(!reasonSceneRect(row, reason).intersects(sceneRect(viewRowAt(queueView, 4))));
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(reason->property("y").toReal(), below);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("playingQueueIndex", 2);
    QTRY_COMPARE(reason->property("y").toReal(), below);

    // The last row, at the view's bottom: above, as nothing fits below.
    QTest::keyClick(window, Qt::Key_End);
    QQuickItem *lastRow = nullptr;
    QTRY_VERIFY((lastRow = viewRowAt(queueView, 59)) != nullptr && lastRow->property("rowCurrent").toBool());
    QObject *lastReason = nullptr;
    QTRY_VERIFY((lastReason = rowReasonOf(lastRow, QStringLiteral("queueRowReason"))) != nullptr);
    QTRY_VERIFY(reasonOpen(lastReason));
    QCOMPARE(lastReason->property("y").toReal(), -3 - lastReason->property("height").toReal());
    QVERIFY(reasonSceneRect(lastRow, lastReason).bottom() <= sceneRect(queueView).bottom());

    // The library's track row, alike: below it, unless the playing track
    // is the next row, then above, clear of it, if need be over the pane's
    // header, which the view's top row leaves too little room under.
    QQuickItem *track = nullptr;
    QTRY_VERIFY((track = libraryRow(window, QStringLiteral("track"))) != nullptr);
    QObject *treeReason = nullptr;
    QTRY_VERIFY((treeReason = rowReasonOf(track, QStringLiteral("libraryRowReason"))) != nullptr);
    tabInto(window, libraryView);
    QVERIFY(QMetaObject::invokeMethod(controller, "selectIndex", Q_ARG(QVariant, 2)));
    QTRY_VERIFY(track->property("rowCurrent").toBool());
    QTRY_VERIFY(reasonOpen(treeReason));
    QCOMPARE(treeReason->property("x").toReal(), track->property("titleX").toReal());
    QCOMPARE(treeReason->property("y").toReal(), track->height() + 3);
    bridge->setProperty("currentTrackPath", later);
    QQuickItem *playing = viewRowAt(libraryView, 3);
    QTRY_VERIFY(playing != nullptr && playing->property("isNow").toBool());
    QTRY_COMPARE(treeReason->property("y").toReal(), -3 - treeReason->property("height").toReal());
    QVERIFY(!reasonSceneRect(track, treeReason).intersects(sceneRect(playing)));
}

// A library row whose title is cut short shows it whole to the pointer
// anywhere on the row, after the style's delay, in the tooltip an
// unreadable row's reason has, at its place: from the title's start, below
// the row, never over it. A title that fits has none, and an unreadable
// row's reason takes the title's place. The accessible name has the whole
// title as well.
void UiMainWindowTest::libraryRowShowsItsCutTitleWholeInItsToolTip() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *errors = window->findChild<QObject *>(QStringLiteral("errorController"));
    QVERIFY(bridge && errors);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    const QString longAlbum = QStringLiteral("An Album Whose Name Runs On ") + QStringLiteral("And On ").repeated(12);
    const QString longTrack = QStringLiteral("A Track Whose Title Runs On ") + QStringLiteral("And On ").repeated(12);
    const QString later = QStringLiteral("/music/Artist A/Album A/03.flac");
    fixture.libraryModel.setExpandedKeys(kAllExpanded);
    fixture.libraryModel.setLibraryTreeFromBinary(treeOf({
        {1, 0, 0, 1, 0, QStringLiteral("Artist A (1)"), kArtistKey, QStringLiteral("Artist A"),
         QStringLiteral("/music/Artist A"), {}},
        {2, 1, 0, 2, 2005, longAlbum, kAlbumKey, QStringLiteral("Artist A"), QStringLiteral("/music/Artist A/Album A"),
         {}},
        {4, 2, 2, 0, 0, longTrack, QStringLiteral("track|") + kTrackPath, QStringLiteral("Artist A"), kTrackPath,
         kTrackPath},
        {4, 2, 3, 0, 0, QStringLiteral("Third"), QStringLiteral("track|") + later, QStringLiteral("Artist A"), later,
         later},
    }));
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(view != nullptr);
    QQuickItem *artist = nullptr;
    QQuickItem *album = nullptr;
    QQuickItem *track = nullptr;
    QTRY_VERIFY((artist = viewRowAt(view, 0)) != nullptr && (album = viewRowAt(view, 1)) != nullptr
                && (track = viewRowAt(view, 2)) != nullptr && viewRowAt(view, 3) != nullptr);
    QQuickItem *albumTitle = itemNamed(album, QStringLiteral("libraryRowTitle"));
    QVERIFY(albumTitle != nullptr);
    QVERIFY(albumTitle->property("truncated").toBool());
    QVERIFY(!itemNamed(artist, QStringLiteral("libraryRowTitle"))->property("truncated").toBool());
    QCOMPARE(QQmlProperty(album, QStringLiteral("Accessible.name"), qmlContext(album)).read().toString(), longAlbum);
    // None made before the pointer comes.
    QCOMPARE(rowReasonOf(album, QStringLiteral("libraryRowTitleToolTip")), nullptr);
    const QPoint away(window->width() - 10, window->height() - 60);

    // On the row's year, away from the title: after the style's delay, the
    // whole title, from the title's start, below the row.
    QQuickItem *year = itemNamed(album, QStringLiteral("libraryRowYear"));
    QVERIFY(shown(year));
    QTest::mouseMove(window, sceneRect(year).center().toPoint() - QPoint(4, 0));
    QTest::mouseMove(window, sceneRect(year).center().toPoint());
    QObject *tip = nullptr;
    QTRY_VERIFY((tip = rowReasonOf(album, QStringLiteral("libraryRowTitleToolTip"))) != nullptr);
    QVERIFY(tip->property("delay").toInt() > 100);
    QTest::qWait(100);
    QVERIFY(!reasonOpen(tip));
    QTRY_VERIFY(reasonOpen(tip));
    QCOMPARE(tip->property("message").toString(), longAlbum);
    QCOMPARE(tip->property("text").toString(), longAlbum);
    QCOMPARE(tip->property("x").toReal(), album->property("titleX").toReal());
    QCOMPARE(tip->property("y").toReal(), album->height() + 3);
    QVERIFY(!reasonSceneRect(album, tip).intersects(sceneRect(album)));
    QVERIFY(reasonSceneRect(album, tip).right() <= sceneRect(view).right());
    // Held down, as a drag starts: none.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, sceneRect(year).center().toPoint());
    QTRY_VERIFY(!reasonOpen(tip));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, sceneRect(year).center().toPoint());

    // A title that fits: none; the cut one's fades out.
    const QPoint onArtist = sceneRect(artist).center().toPoint();
    QTest::mouseMove(window, onArtist);
    QTRY_VERIFY(!reasonOpen(tip));
    QTest::qWait(1000);
    QVERIFY(!tip->property("visible").toBool());
    QCOMPARE(rowReasonOf(artist, QStringLiteral("libraryRowTitleToolTip")), nullptr);
    QCOMPARE(rowReasonOf(artist, QStringLiteral("libraryRowReason")), nullptr);

    // Another cut title: its own, and the view keeps one.
    QVERIFY(itemNamed(track, QStringLiteral("libraryRowTitle"))->property("truncated").toBool());
    QTest::mouseMove(window, sceneRect(track).center().toPoint());
    QObject *trackTip = nullptr;
    QTRY_VERIFY((trackTip = rowReasonOf(track, QStringLiteral("libraryRowTitleToolTip"))) != nullptr);
    QTRY_VERIFY(reasonOpen(trackTip));
    QCOMPARE(trackTip->property("message").toString(), longTrack);
    QCOMPARE(rowReasonOf(album, QStringLiteral("libraryRowTitleToolTip")), nullptr);
    QTest::mouseMove(window, away);
    QTRY_VERIFY(!reasonOpen(trackTip));

    // An unreadable row cut short: its reason, not its title.
    const QString why = QStringLiteral("The file cannot be read.");
    QVERIFY(QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, kTrackPath), Q_ARG(QVariant, why), Q_ARG(QVariant, QString())));
    QObject *reason = nullptr;
    QTRY_VERIFY((reason = rowReasonOf(track, QStringLiteral("libraryRowReason"))) != nullptr);
    QTest::mouseMove(window, sceneRect(track).center().toPoint());
    QTRY_VERIFY(reasonOpen(reason));
    QCOMPARE(reason->property("message").toString(), why);
    QCOMPARE(rowReasonOf(track, QStringLiteral("libraryRowTitleToolTip")), nullptr);
    QTest::mouseMove(window, away);
    QTRY_VERIFY(!reasonOpen(reason));
}

void UiMainWindowTest::queueStopMarkWeighsNoMoreThanPause_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// The now row's stop square is drawn smaller than its play and pause marks,
// 10/16 of their box as the transport's Stop is of its, so that its solid
// ink, as drawn, is no more than the pause mark's, give or take its soft
// edges (at their size it was half again the pause mark's, the heaviest of
// the three).
void UiMainWindowTest::queueStopMarkWeighsNoMoreThanPause() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    QObject *bridge = bridgeOf(window);
    QQuickItem *queueView = named(window, "playlistView");
    QVERIFY(bridge && queueView);
    setQueue(bridge, sampleQueueRows(4));
    QTRY_COMPARE(queueView->property("count").toInt(), 4);
    bridge->setProperty("playingQueueIndex", 1);
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRowAt(queueView, 1)) != nullptr);
    QQuickItem *glyph = itemNamed(row, QStringLiteral("queueRowState"));
    QQuickItem *cell = itemNamed(row, QStringLiteral("queueCell_indicator"));
    QVERIFY(glyph && cell);

    // The ink of the mark, as drawn: each pixel of the cell by how far it is
    // from the row's colour, in parts of the farthest.
    const auto inkOf = [&](const QString &state, const QString &name) {
        bridge->setProperty("playbackState", state);
        if (!QTest::qWaitFor([&] { return stateGlyphName(glyph) == name && glyph->opacity() == 1.0; })) {
            return -1.0;
        }
        QTest::qWait(300);
        const QImage image = window->grabWindow().convertToFormat(QImage::Format_RGB32);
        const qreal scale = window->effectiveDevicePixelRatio();
        const QRectF area = sceneRect(cell);
        const QColor background = image.pixelColor(QPointF(area.left() + 1, area.top() + 1).toPoint() * scale);
        QList<int> distances;
        for (int y = int(area.top()); y < int(area.bottom()); ++y) {
            for (int x = int(area.left()); x < int(area.right()); ++x) {
                distances.append(channelDistance(image.pixelColor(QPoint(x, y) * scale), background));
            }
        }
        const int farthest = *std::max_element(distances.cbegin(), distances.cend());
        double ink = 0;
        for (const int distance : distances) {
            ink += farthest > 0 ? double(distance) / farthest : 0.0;
        }
        return ink;
    };
    const double playing = inkOf(QStringLiteral("Playing"), QStringLiteral("media-playback-start"));
    const double paused = inkOf(QStringLiteral("Paused"), QStringLiteral("media-playback-pause"));
    const double stopped = inkOf(QStringLiteral("Stopped"), QStringLiteral("media-playback-stop"));
    const QString inks = QStringLiteral("playing %1, paused %2, stopped %3").arg(playing).arg(paused).arg(stopped);
    QVERIFY2(playing > 0 && paused > 0 && stopped > 0, qPrintable(inks));
    QVERIFY2(stopped <= 1.05 * paused, qPrintable(inks));
    QCOMPARE(glyph->width(), qreal(scaledSize(window, 10)));
    QCOMPARE(glyph->height(), glyph->width());
}

void UiMainWindowTest::queueRightPaddingIsTheLaneAndAnInset_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::addColumn<bool>("dark");
    QTest::newRow("10 pt, 1600 by 1000, dark") << 10.0 << 1600 << 1000 << true;
    QTest::newRow("10 pt, 1000 by 720, light") << 10.0 << 1000 << 720 << false;
    QTest::newRow("24 pt, 1600 by 1000, light") << 24.0 << 1600 << 1000 << false;
    QTest::newRow("24 pt, 1000 by 720, dark") << 24.0 << 1000 << 720 << true;
}

// Right of Length, the queue keeps the scrollbar's lane, 8 px at any font,
// and a 16 px inset that grows with the font to half as much again: 24 px
// at 10 pt, 32 px at 24 pt. Scaled as one with the font, the two made a
// 48 px gutter at 24 pt, twice the lane's beside Album and Artist dropped
// for want of room. Length's text stays clear of the scrollbar's thumb by
// the inset.
void UiMainWindowTest::queueRightPaddingIsTheLaneAndAnInset() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    QFETCH(bool, dark);
    const auto restorePalette = useDarkPalette(dark);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    setQueue(bridgeOf(window), sampleQueueRows(80));
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *queueView = named(window, "playlistView");
    QQuickItem *scrollBar = named(window, "queueScrollBar");
    QVERIFY(queue && queueView && scrollBar);
    QTRY_VERIFY(scrollBar->isVisible());
    const qreal lane = scrollBar->property("lane").toReal();
    QCOMPARE(lane, 8.0);
    const qreal scale = queue->property("sizeScale").toReal();
    const qreal inset = std::round(16 * std::min(scale, 1.5));
    const qreal padding = queue->property("rightPadding").toReal();
    QCOMPARE(padding, lane + inset);
    QVERIFY2(padding <= lane + 24, qPrintable(QString::number(padding)));
    if (pointSize > 20) {
        QVERIFY2(scale > 1.5, qPrintable(QString::number(scale)));
    } else {
        QCOMPARE(padding, 24.0);
    }
    // The header's Length and a row's end where the padding starts, the
    // inset clear of the scrollbar's lane.
    QQuickItem *length = named(window, "queueHeader_length");
    QVERIFY(shown(length));
    QCOMPARE(sceneRect(queue).right() - sceneRect(length).right(), padding);
    QQuickItem *row = nullptr;
    QTRY_VERIFY((row = viewRowAt(queueView, 0)) != nullptr);
    QQuickItem *cell = itemNamed(row, QStringLiteral("queueCell_length"));
    QVERIFY(shown(cell));
    QCOMPARE(sceneRect(scrollBar).left() - sceneRect(cell).right(), inset);
}

void UiMainWindowTest::queueHeaderLabelsOnlyArtistAlbumAndLength() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *palette = paletteOf(window);
    setQueue(bridge, sampleQueueRows(3));
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
    // right-aligned 24 px (at 10 pt) from the queue's right edge.
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
    // The scrollbar's lane and the inset (queueRightPaddingIsTheLaneAndAnInset).
    QCOMPARE(queue->property("rightPadding").toReal(),
             8 + std::round(16 * std::min(palette->property("rowHeight").toReal() / 26, 1.5)));
    // The resize handles show no line until the pointer or the keyboard
    // finds them.
    QQuickItem *handle = named(window, "queueColumnHandle_album");
    QVERIFY(shown(handle));
    QQuickItem *line = itemNamed(handle, QStringLiteral("queueColumnHandleLine"));
    QVERIFY(line != nullptr && !line->isVisible());
    handle->forceActiveFocus(Qt::TabFocusReason);
    // With focus, a frame the header's height, not a short tick.
    QQuickItem *frame = itemNamed(handle, QStringLiteral("queueColumnHandleFocus"));
    QVERIFY(frame != nullptr);
    QTRY_VERIFY(frame->isVisible());
    // 2 px clear of the seam above the header and of its hairline.
    QCOMPARE(sceneRect(frame).top(), sceneRect(queueHeader).top() + 2);
    QCOMPARE(sceneRect(frame).height(), sceneRect(queueHeader).height() - 5);
}

// A queue row held down by the pointer takes the pressed selection, apart
// from the hovered selection it shows before and after (they were one
// colour); a drag that takes the row away ends it.
void UiMainWindowTest::pressedQueueRowHasItsOwnColour() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    setQueue(bridgeOf(window), sampleQueueRows(5));
    QQuickItem *row = nullptr;
    QTRY_VERIFY(([&]() {
        for (QQuickItem *candidate : itemsNamed(named(window, "playlistView"), QStringLiteral("queueRow"))) {
            if (candidate->property("queueRowIndex").toInt() == 1) {
                row = candidate;
            }
        }
        return row != nullptr;
    })());
    QQuickItem *tint = itemNamed(row, QStringLiteral("rowBackgroundTint"));
    QVERIFY(tint != nullptr);
    const QPoint centre = row->mapToScene(QPointF(row->width() / 3, row->height() / 2)).toPoint();
    QTest::mouseMove(window, centre);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, centre);
    QTRY_VERIFY(row->property("rowSelected").toBool());
    QCOMPARE(colorOf(tint), tokenHex(window, "selectionPressed"));
    QVERIFY(colorOf(tint) != tokenHex(window, "selectionHovered"));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, centre);
    QTRY_COMPARE(colorOf(tint), tokenHex(window, "selectionHovered"));

    // Pressed again, then dragged off: the row goes with the pointer, and
    // stops showing the press.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, centre);
    QCOMPARE(colorOf(tint), tokenHex(window, "selectionPressed"));
    for (int step = 1; step <= 6; ++step) {
        QTest::mouseMove(window, centre + QPoint(0, step * 8));
    }
    QTRY_VERIFY(named(window, "playlistView")->property("draggingRow").value<QObject *>() != nullptr);
    QVERIFY(colorOf(tint) != tokenHex(window, "selectionPressed"));
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, centre + QPoint(0, 48));
}

void UiMainWindowTest::queueNameKeepsTheTitleColumnsPlace_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("1600x1000") << 1600 << 1000 << 10.0;
    QTest::newRow("1000x720, 24 pt") << 1000 << 720 << 24.0;
}

// "Queue" is the Title column's heading, and stays at that column's x
// whether the queue is empty or filled (at the panes' 12 px inset while
// empty, it jumped 54 px on the first Play).
void UiMainWindowTest::queueNameKeepsTheTitleColumnsPlace() {
    QFETCH(int, width);
    QFETCH(int, height);
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(width, height));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *library = named(window, "libraryPane");
    QQuickItem *queueHeader = named(window, "queueHeader");
    QQuickItem *libraryHeader = named(window, "libraryHeader");
    QVERIFY(queue && library && queueHeader && libraryHeader);
    QQuickItem *name = itemNamed(queueHeader, QStringLiteral("regionHeaderTitle"));
    QQuickItem *libraryName = itemNamed(libraryHeader, QStringLiteral("regionHeaderTitle"));
    QQuickItem *title = itemNamed(queueHeader, QStringLiteral("queueHeader_title"));
    QVERIFY(name && libraryName && title);
    QCOMPARE(bridge->property("queueLength").toInt(), 0);
    const auto inset = [](QQuickItem *item, QQuickItem *pane) {
        return item->mapToScene(QPointF()).x() - pane->mapToScene(QPointF()).x();
    };
    QTRY_COMPARE(name->mapToScene(QPointF()).x(), title->mapToScene(QPointF()).x());
    const qreal emptyInset = inset(name, queue);
    QVERIFY2(emptyInset > 12.0, qPrintable(QString::number(emptyInset)));
    QCOMPARE(inset(libraryName, library), 12.0);
    QVERIFY(!name->property("truncated").toBool());

    setQueue(bridge, sampleQueueRows(50));
    QTRY_COMPARE(name->mapToScene(QPointF()).x(), title->mapToScene(QPointF()).x());
    QCOMPARE(inset(name, queue), emptyInset);

    setQueue(bridge, {});
    QTRY_COMPARE(name->mapToScene(QPointF()).x(), title->mapToScene(QPointF()).x());
    QCOMPARE(inset(name, queue), emptyInset);
}

void UiMainWindowTest::narrowQueueDropsAlbumBeforeTitle() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    // As narrow as the window goes; with the default 10 pt font a 1000 px
    // window is narrow enough.
    window->resize(window->minimumWidth(), 720);
    QTRY_COMPARE(window->width(), window->minimumWidth());
    QObject *bridge = bridgeOf(window);
    setQueue(bridge, sampleQueueRows(3));
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *album = named(window, "queueHeader_album");
    QQuickItem *albumHandle = named(window, "queueColumnHandle_album");
    QQuickItem *title = named(window, "queueHeader_title");
    QVERIFY(queue && album && albumHandle && title);
    QTRY_VERIFY(!album->isVisible());
    QVERIFY(!albumHandle->isVisible());
    QVERIFY(!albumHandle->activeFocusOnTab());
    QVERIFY(title->width() >= queue->property("minimumTitleColumnWidth").toReal() - 1);
    // The minimum stays where it was before Artist's default share grew:
    // it is counted with Artist at 26% of the width.
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
    // them way.
    QQuickItem *artist = named(window, "queueHeader_artist");
    const qreal textRoom = queue->property("defaultTextColumnsRoom").toReal();
    QVERIFY2(artist->width() >= textRoom * minimumShare - 1
                 && artist->width() <= std::max(textRoom * queue->property("defaultArtistColumnShare").toReal(),
                                                queue->property("minimumArtistColumnWidth").toReal()) + 1,
             qPrintable(QStringLiteral("artist %1 of %2").arg(artist->width()).arg(textRoom)));
    // Wider, Album comes back.
    window->resize(1600, 1000);
    QTRY_VERIFY(album->isVisible());
    QVERIFY(albumHandle->isVisible());
}

// The queue's columns at 1600 and 1000 px: Length has a fixed room from the
// font, about 56 px; of what is left for the text, the title has a little
// under half, Artist a little over a quarter and Album a little less (half
// the room left a gap after short titles, and Artist about 24 px narrower
// at 1600 px). In the 1000 px window Album drops before Artist, and Length
// keeps its room.
void UiMainWindowTest::queueColumnsGiveTheirSlackToTheTitle() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    setQueue(bridge, sampleQueueRows(3));
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *title = named(window, "queueHeader_title");
    QQuickItem *artist = named(window, "queueHeader_artist");
    QQuickItem *album = named(window, "queueHeader_album");
    QQuickItem *length = named(window, "queueHeader_length");
    QVERIFY(queue && title && artist && album && length);
    QTRY_VERIFY(shown(album));

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
// within the queue, 24 px clear of its sides. In 1000 px at 24 pt the
// title at its minimum leaves Artist less than the least it is shown at
// (queueKeepsArtistAtALargeFont) with the gaps of the font, so the
// columns are compact, and Artist, short of ten characters there too,
// gives the title its room (narrowQueueAtALargeFontGivesTheNamesTheRoom).
void UiMainWindowTest::narrowWindowAtALargeFontFitsTheQueue() {
    const ScopedApplicationFont font(24.0);
    MainWindowFixture fixture(sized(1000, 720));
    QQuickWindow *window = fixture.window;
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
                 qPrintable(QStringLiteral("%1: %2 in %3")
                                .arg(item->objectName(), describeRect(rect), describeRect(pane))));
    }
    QVERIFY2(explanation->property("lineCount").toInt() > 1,
             qPrintable(QString::number(explanation->property("lineCount").toInt())));

    // Fifty tracks: Title and Length fit in the compact columns; Album
    // and Artist drop, as the sidebar is wider at a large font, and the
    // title keeps more than its minimum.
    setQueue(bridge, sampleQueueRows(50));
    QQuickItem *view = named(window, "playlistView");
    QTRY_VERIFY(viewRowAt(view, 0) != nullptr);
    QTRY_VERIFY(!shown(named(window, "queueHeader_album")));
    QQuickItem *title = named(window, "queueHeader_title");
    QQuickItem *artist = named(window, "queueHeader_artist");
    QQuickItem *length = named(window, "queueHeader_length");
    QVERIFY(title && artist && length);
    QVERIFY(shown(length) && !shown(artist));
    QVERIFY(queue->property("compactColumns").toBool());
    const qreal titleMinimum = queue->property("minimumTitleColumnWidth").toReal();
    QVERIFY2(queue->width() - queue->property("standardReservedColumnsWidth").toReal() - titleMinimum
                     - queue->property("standardColumnGap").toReal()
                 < queue->property("minimumShownTextColumnWidth").toReal(),
             qPrintable(QString::number(queue->width())));
    QVERIFY2(title->width() > titleMinimum, qPrintable(QString::number(title->width())));
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
    QVERIFY2(title->width() >= titleMinimum - 1, qPrintable(QString::number(title->width())));
    // The window's minimum is the panes' and fits 1000 px beside the wider
    // sidebar: the queue's counts Length at its least width, as Length
    // gives up the two characters' room before it in a queue that narrow,
    // rather than its default.
    QVERIFY(length->width() >= queue->property("minimumLengthColumnWidth").toReal() - 0.5);
    QVERIFY(queue->property("minimumTitleOnlyWidth").toReal()
            < queue->property("minimumWidthLimit").toReal() + 0.5);
    const qreal panesMinimum = named(window, "sidebarPane")->property("minimumWidth").toReal()
        + queue->property("minimumContentWidth").toReal() + 1;
    QCOMPARE(window->minimumWidth(), static_cast<int>(std::ceil(panesMinimum)));
}

void UiMainWindowTest::narrowQueueAtALargeFontGivesTheNamesTheRoom_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("compact");
    QTest::newRow("10 pt, 1000 by 720, dark") << 10.0 << 1000 << 720 << true << false;
    QTest::newRow("10 pt, 1600 by 1000, light") << 10.0 << 1600 << 1000 << false << false;
    QTest::newRow("24 pt, 1000 by 720, dark") << 24.0 << 1000 << 720 << true << true;
    QTest::newRow("24 pt, 1000 by 720, light") << 24.0 << 1000 << 720 << false << true;
    QTest::newRow("24 pt, 1600 by 1000, dark") << 24.0 << 1600 << 1000 << true << false;
    QTest::newRow("24 pt, 1600 by 1000, light") << 24.0 << 1600 << 1000 << false << false;
}

// In the narrow window at 24 pt, the state glyph's column, the number's
// and the gap before the title took 132 px for a one-digit number, and
// Artist, kept at 120 px, showed "The Q…" on every row beside elided
// titles. There the columns are compact and the room goes to the names:
// the state glyph's column is the error mark's with 2 px at either side,
// the number's its widest number of two digits or more with 6 px before
// it, the gaps 12 px and Length its least width. The title keeps its
// usual minimum, and Artist shows only where it has ten characters of a
// name; otherwise the title has the row. The header and the rows share
// the widths. Elsewhere, at 10 pt or in a wider window, the columns keep
// the gaps and widths of the font.
void UiMainWindowTest::narrowQueueAtALargeFontGivesTheNamesTheRoom() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    QFETCH(bool, dark);
    QFETCH(bool, compact);
    const auto restorePalette = useDarkPalette(dark);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QTRY_COMPARE(paletteOf(window)->property("basePointSize").toReal(), pointSize);
    QCOMPARE(window->size(), QSize(windowWidth, windowHeight));
    setQueue(bridgeOf(window), sampleQueueRows(50));
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(queue && view);
    QQuickItem *indicator = named(window, "queueHeader_indicator");
    QQuickItem *order = named(window, "queueHeader_order");
    QQuickItem *title = named(window, "queueHeader_title");
    QQuickItem *artist = named(window, "queueHeader_artist");
    QQuickItem *length = named(window, "queueHeader_length");
    QVERIFY(indicator && order && title && artist && length);
    QTRY_VERIFY(viewRowAt(view, 0) != nullptr);
    QTRY_COMPARE(queue->property("compactColumns").toBool(), compact);

    const qreal scale = queue->property("sizeScale").toReal();
    const qreal gap = queue->property("columnGap").toReal();
    const QRectF pane = sceneRect(queue);
    const qreal padding = queue->property("rightPadding").toReal();
    const qreal titleMinimum = queue->property("minimumTitleColumnWidth").toReal();
    const auto widths = [&] {
        return QStringLiteral("queue %1: state %2, number %3, title %4, artist %5, length %6, gap %7")
            .arg(queue->width()).arg(indicator->width()).arg(order->width()).arg(title->width())
            .arg(shown(artist) ? artist->width() : 0.0).arg(length->width()).arg(gap);
    };
    QTRY_VERIFY2(std::abs(pane.right() - sceneRect(length).right() - padding) <= 0.5, qPrintable(widths()));
    const qreal leading = sceneRect(title).left() - pane.left();
    if (compact) {
        // The gutters of the names' columns: 132 px before the title at
        // 24 pt with the font's, under 100 px here.
        QVERIFY2(scale >= 2, qPrintable(QString::number(scale)));
        const qreal errorGlyph = queue->property("errorGlyphSize").toReal();
        QCOMPARE(indicator->width(), errorGlyph + 4);
        const qreal widestNumber = QFontMetricsF(itemNamed(viewRowAt(view, 0), QStringLiteral("queueCell_order"))
                                                     ->property("font").value<QFont>())
                                       .horizontalAdvance(QStringLiteral("50"));
        QCOMPARE(order->width(), std::ceil(widestNumber) + 6);
        QCOMPARE(gap, 12.0);
        QCOMPARE(queue->property("titleGap").toReal(), 12.0);
        QCOMPARE(leading, indicator->width() + order->width() + 12);
        QVERIFY2(leading < 100, qPrintable(widths()));
        QCOMPARE(length->width(), queue->property("minimumLengthColumnWidth").toReal());
        // Artist would have under ten characters: the title has the row.
        QVERIFY2(!shown(artist), qPrintable(widths()));
        QCOMPARE(sceneRect(length).left() - sceneRect(title).right(), gap);
        QVERIFY2(title->width() >= titleMinimum + 120, qPrintable(widths()));

        // Wider, where ten characters fit beside the title at its usual
        // minimum, Artist shows with them, still compact.
        window->resize(windowWidth + 100, windowHeight);
        QTRY_COMPARE(window->width(), windowWidth + 100);
        QTRY_VERIFY2(shown(artist), qPrintable(widths()));
        QVERIFY(queue->property("compactColumns").toBool());
        const qreal tenCharacters = std::ceil(QFontMetricsF(QGuiApplication::font()).averageCharWidth() * 10);
        QVERIFY2(artist->width() >= std::max(120.0, tenCharacters) - 0.5, qPrintable(widths()));
        QVERIFY2(title->width() >= titleMinimum - 0.5, qPrintable(widths()));
        QTRY_VERIFY2(std::abs(sceneRect(artist).left() - sceneRect(title).right() - gap) <= 1, qPrintable(widths()));
        QVERIFY2(std::abs(sceneRect(length).left() - sceneRect(artist).right() - gap) <= 1, qPrintable(widths()));
    } else {
        QTRY_VERIFY(shown(artist));
        QCOMPARE(gap, std::round(16 * scale));
        QCOMPARE(indicator->width(), queue->property("playlistIndicatorColumnWidth").toReal());
        QCOMPARE(order->width(), queue->property("playlistOrderColumnWidth").toReal());
        QCOMPARE(length->width(), queue->property("measuredLengthColumnWidth").toReal());
        QVERIFY2(title->width() >= titleMinimum - 1, qPrintable(widths()));
        QVERIFY2(std::abs(sceneRect(artist).left() - sceneRect(title).right() - gap) <= 1, qPrintable(widths()));
    }
    // The rows' cells on the header's columns.
    QQuickItem *row = viewRowAt(view, 0);
    QList<const char *> columns{"indicator", "order", "title", "length"};
    if (shown(artist)) {
        columns.append("artist");
    }
    for (const char *column : columns) {
        QQuickItem *cell = itemNamed(row, QStringLiteral("queueCell_%1").arg(QString::fromLatin1(column)));
        QQuickItem *header = named(window, QStringLiteral("queueHeader_%1").arg(QString::fromLatin1(column)));
        QVERIFY2(shown(cell), column);
        QVERIFY2(std::abs(sceneRect(cell).left() - sceneRect(header).left()) <= 0.5
                     && std::abs(sceneRect(cell).width() - header->width()) <= 0.5,
                 qPrintable(QStringLiteral("%1: %2 under %3").arg(QString::fromLatin1(column),
                     describeRect(sceneRect(cell)), describeRect(sceneRect(header)))));
    }
    QCOMPARE(shown(itemNamed(row, QStringLiteral("queueCell_artist"))), shown(artist));
    // The window's least width is the same with the compact columns: they
    // change the widths inside the queue, not its minimum.
    QVERIFY2(window->minimumWidth() <= 1000, qPrintable(QString::number(window->minimumWidth())));
}

void UiMainWindowTest::queueKeepsArtistAtALargeFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    // (queueColumnsGiveTheirSlackToTheTitle has the default window, and
    // narrowWindowAtALargeFontFitsTheQueue 24 pt in 1000 px.)
    QTest::newRow("default font, 1000 by 720") << 0.0 << 1000 << 720;
    QTest::newRow("24 pt, 1600 by 1000") << 24.0 << 1600 << 1000;
}

// Artist has at least sixteen characters of the font by default where the
// title, given way down to its minimum, leaves them (at 24 pt in 1600 px it
// was cut to eleven beside a title column twice as wide as its titles).
// With the default font Artist's share is wider, and stays.
void UiMainWindowTest::queueKeepsArtistAtALargeFont() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
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
    setQueue(bridge, rows);
    QQuickItem *queue = named(window, "mainQueuePane");
    QQuickItem *title = named(window, "queueHeader_title");
    QQuickItem *artist = named(window, "queueHeader_artist");
    QVERIFY(queue && title && artist);
    QTRY_VERIFY(sceneRect(queue).right() <= windowWidth + 0.5 && sceneRect(queue).right() >= windowWidth - 1);
    QTRY_VERIFY(sceneRect(named(window, "sidebarPane")).right() + 1 >= sceneRect(queue).left() - 0.5);
    const QFont baseFont = paletteOf(window)->property("baseFont").value<QFont>();
    const qreal minimum = std::ceil(QFontMetricsF(baseFont).averageCharWidth() * 16);
    QTRY_COMPARE(queue->property("minimumArtistColumnWidth").toReal(), minimum);
    const qreal titleMinimum = queue->property("minimumTitleColumnWidth").toReal();
    const qreal gap = queue->property("columnGap").toReal();
    const qreal reserved = queue->property("reservedColumnsWidth").toReal();
    const QString widths = QStringLiteral("queue %1, title %2 (least %3), artist %4 (least %5)")
        .arg(queue->width()).arg(title->width()).arg(titleMinimum).arg(artist->width()).arg(minimum);
    QVERIFY2(queue->width() - reserved - titleMinimum - gap >= queue->property("minimumShownTextColumnWidth").toReal(),
             qPrintable(widths));
    QTRY_VERIFY2(shown(artist), qPrintable(widths));
    QTRY_VERIFY2(artist->width() >= minimum - 1, qPrintable(widths));
    QVERIFY2(title->width() >= titleMinimum - 1, qPrintable(widths));
    const qreal share = queue->property("defaultTextColumnsRoom").toReal()
        * queue->property("defaultArtistColumnShare").toReal();
    if (pointSize > 0) {
        // The title gave way, and the name shows whole.
        QVERIFY2(artist->width() > share + 1, qPrintable(widths));
        QQuickItem *view = named(window, "playlistView");
        QTRY_VERIFY(viewRowAt(view, 0) != nullptr);
        QQuickItem *cell = itemNamed(viewRowAt(view, 0), QStringLiteral("queueCell_artist"));
        QVERIFY(shown(cell));
        QTRY_COMPARE(cell->property("text").toString(), artistName);
        QVERIFY2(!cell->property("truncated").toBool(), qPrintable(widths));
    } else {
        // With the default font, its share, as before.
        QVERIFY2(std::abs(artist->width() - share) <= 1, qPrintable(widths));
    }
}

void UiMainWindowTest::visualizationTakesNearlyHalfTheRightColumn_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<int>("windowHeight");
    QTest::newRow("default font, 1600 by 1000") << 0.0 << 1600 << 1000;
    QTest::newRow("24 pt, 1600 by 1000") << 24.0 << 1600 << 1000;
    QTest::newRow("24 pt, 1000 by 720") << 24.0 << 1000 << 720;
}

// Before the user drags the split, the visualization takes 46% of the right
// column (at 42% the pane read as a footer under a playlist): 411 px at
// 1600 by 1000, where the queue keeps 16 rows and more with the default
// font. The queue keeps its header and six rows first, and each pane its
// minimum, the queue's its header and four rows: at 24 pt in a 720 px
// window the visualization has its minimum and the queue the rest, and the
// window's least height still fits 720 px. The queue's list is a whole
// number of its rows, the visualization taking the rest (not a row cut at
// the seam). A split the user dragged wins, exactly where it was put, on
// the grid or not.
void UiMainWindowTest::visualizationTakesNearlyHalfTheRightColumn() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(int, windowHeight);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized(windowWidth, windowHeight));
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QVERIFY2(window->minimumHeight() <= 720, qPrintable(QString::number(window->minimumHeight())));
    QObject *bridge = bridgeOf(window);
    setQueue(bridge, sampleQueueRows(50));
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
    // The queue's list on the grid of its rows, as tall as a delegate is.
    QQuickItem *list = named(window, "playlistView");
    QQuickItem *row = named(window, "queueRow");
    QVERIFY(list != nullptr && row != nullptr);
    QCOMPARE(row->height(), rowHeight);
    const qreal rows = list->height() / row->height();
    QVERIFY2(list->height() > 0 && std::abs(rows - std::round(rows)) < 1e-6,
             qPrintable(heights + QStringLiteral(", list %1").arg(list->height())));
    if (pointSize > 0 && windowHeight == 720) {
        // The visualization at its minimum, and what the grid leaves over;
        // the queue the rest.
        QVERIFY2(pane->height() >= 220 - 0.5 && pane->height() < 220 + rowHeight, qPrintable(heights));
    } else {
        // Its share, and what the grid leaves over.
        const qreal wanted = std::round(split->height() * 0.46);
        QVERIFY2((pane->height() >= wanted - 1.5 && pane->height() < wanted + rowHeight) || share < 0.46,
                 qPrintable(heights));
        // The queue's header and six rows at least.
        QVERIFY2(queue->height() >= 7 * rowHeight - 0.5, qPrintable(heights));
    }
    if (pointSize == 0) {
        QVERIFY2(share >= 0.45 && share <= 0.50, qPrintable(heights));
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

    // One off the grid stays exactly there. The least window, at 24 pt in
    // 720 px, has no room for one: the transport's band takes what the
    // panes' minimums leave over, the queue at its least on the grid.
    const qreal gridRows = std::ceil((queueMinimum - header->height()) / rowHeight);
    if (split->height() < header->height() + gridRows * rowHeight + 7 + 220 + 1) {
        QCOMPARE(split->height(), queueMinimum + 220 + 1);
        QCOMPARE(queueMinimum, header->height() + gridRows * rowHeight);
        return;
    }
    const qreal offGrid = header->height() + gridRows * rowHeight + 7;
    QVERIFY(QMetaObject::invokeMethod(layout, "noteQueueHeight", Q_ARG(QVariant, offGrid),
                                      Q_ARG(QVariant, split->height())));
    QTRY_COMPARE(queue->height(), offGrid);
    QCOMPARE(list->height(), gridRows * rowHeight + 7);
}

// The window opens before the backend has read the library, whose counts
// are zero until then: neither the queue's first-run message nor its empty
// line, and no "No folders", flash at the start of a library that has
// folders. Once the library is loaded, a new install's first run shows.
void UiMainWindowTest::nothingSaysFirstRunBeforeTheLibraryLoads() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryLoaded", false);
    QCOMPARE(bridge->property("libraryRootCount").toInt(), 0);
    QCOMPARE(bridge->property("queueLength").toInt(), 0);
    QQuickItem *firstRun = named(window, "queueFirstRunMessage");
    QQuickItem *queueEmpty = named(window, "queueEmptyMessage");
    QQuickItem *totals = itemNamed(named(window, "libraryHeader"),
                                   QStringLiteral("regionHeaderTotals"));
    QVERIFY(firstRun && queueEmpty && totals);
    QTRY_VERIFY(!firstRun->isVisible());
    QVERIFY(!queueEmpty->isVisible());
    QCOMPARE(totals->property("text").toString(), QString());
    QVERIFY(!named(window, "libraryEmptyStateLabel")->isVisible());
    // A restored queue as well: nothing over its rows.
    bridge->setProperty("queueLength", 3);
    QVERIFY(!firstRun->isVisible());
    QVERIFY(!queueEmpty->isVisible());
    bridge->setProperty("queueLength", 0);

    // A new install: the library is loaded and has no folders.
    bridge->setProperty("libraryLoaded", true);
    QTRY_VERIFY(shown(firstRun));
    QVERIFY(!queueEmpty->isVisible());
    QCOMPARE(totals->property("text").toString(), QStringLiteral("No folders"));

    // A library with folders, loaded with them: the empty queue's line.
    bridge->setProperty("libraryLoaded", false);
    bridge->setProperty("libraryRootCount", 1);
    QTRY_VERIFY(!firstRun->isVisible());
    QVERIFY(!queueEmpty->isVisible());
    bridge->setProperty("libraryLoaded", true);
    QTRY_VERIFY(shown(queueEmpty));
    QVERIFY(!firstRun->isVisible());
}

void UiMainWindowTest::firstRunShowsOneMessage() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
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
    // No other message: the library shows its header, whose totals say
    // it has no folders (libraryHeaderSaysItHasNoFolders), with nothing
    // under it, and the queue no empty line; the pane, before any audio, is
    // its mode's floor, the spectrogram's black.
    QVERIFY(!named(window, "libraryEmptyStateLabel")->isVisible());
    QVERIFY(!named(window, "queueEmptyLabel")->isVisible());
    QVERIFY(shown(named(window, "libraryHeader")));
    QCOMPARE(itemNamed(named(window, "libraryHeader"), QStringLiteral("regionHeaderTotals"))
                 ->property("text").toString(),
             QStringLiteral("No folders"));
    QCOMPARE(itemNamed(named(window, "queueHeader"), QStringLiteral("regionHeaderTotals"))
                 ->property("text").toString(),
             QString());
    QCOMPARE(colorOf(named(window, "spectrogramPane")), tokenHex(window, "plotFloor"));
    QVERIFY(!shown(named(window, "plotAnalysingText")));
    QVERIFY(named(window, "nowPlayingPanel")->property("idle").toBool());
    QTRY_VERIFY2(compactRowProblem(window).isEmpty(), qPrintable(compactRowProblem(window)));
    // The empty queue's header keeps its column labels and handles, with
    // only its count left out (theEmptyQueuesHeaderKeepsItsColumns).
    for (const char *column : {"queueHeader_artist", "queueHeader_album", "queueHeader_length"}) {
        QCOMPARE(named(window, column)->opacity(), 1.0);
    }
    QVERIFY(named(window, "queueColumnHandle_artist")->activeFocusOnTab());

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
    // Queued tracks replace the line; the columns keep their labels.
    setQueue(bridge, sampleQueueRows(1));
    QTRY_VERIFY(!queueEmpty->isVisible());
    QCOMPARE(named(window, "queueHeader_artist")->opacity(), 1.0);
    QVERIFY(named(window, "queueColumnHandle_artist")->activeFocusOnTab());
}

// Tab goes through the queue in reading order: from the library to the
// column handles of the queue's header, in the columns' order, then to its
// rows, and on to the visualization. The handles came after the rows, so
// the focus went down to the rows and back up to the header, and three
// stops stood between the rows and the plot.
void UiMainWindowTest::tabGoesThroughTheQueueHeadersHandlesBeforeItsRows() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(fixture.libraryModel, kAllExpanded);
    setQueue(bridge, sampleQueueRows(3));
    QTRY_VERIFY(libraryRow(window, QStringLiteral("track")) != nullptr);
    QQuickItem *library = named(window, "libraryView");
    QQuickItem *queue = named(window, "playlistView");
    QQuickItem *artistHandle = named(window, "queueColumnHandle_artist");
    QVERIFY(library && queue && artistHandle);
    QTRY_VERIFY(artistHandle->activeFocusOnTab());
    const auto focusName = [window]() {
        return window->activeFocusItem() != nullptr ? window->activeFocusItem()->objectName() : QStringLiteral("nothing");
    };

    library->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(library->hasActiveFocus());
    // The handles, in the columns' order, which Left and Right still move.
    for (const char *column : {"queueColumnHandle_artist", "queueColumnHandle_album", "queueColumnHandle_length"}) {
        QTest::keyClick(window, Qt::Key_Tab);
        QTRY_VERIFY2(named(window, column)->hasActiveFocus(),
                     qPrintable(QStringLiteral("%1: Tab went to %2").arg(QLatin1String(column), focusName())));
    }
    const qreal lengthWidth = named(window, "queueHeader_length")->width();
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_VERIFY(named(window, "queueHeader_length")->width() > lengthWidth);
    QTest::keyClick(window, Qt::Key_Home);
    QTRY_COMPARE(named(window, "queueHeader_length")->width(), lengthWidth);
    // Then the rows,
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY2(queue->hasActiveFocus(), qPrintable(QStringLiteral("Tab went to %1").arg(focusName())));
    // and on to the visualization, past the queue.
    QTest::keyClick(window, Qt::Key_Tab);
    QTRY_VERIFY2(!queue->hasActiveFocus() && !named(window, "queueColumnHandles")->isAncestorOf(window->activeFocusItem())
                     && !library->hasActiveFocus(),
                 qPrintable(QStringLiteral("Tab went to %1").arg(focusName())));
    // Shift+Tab goes back from the rows to the last handle, and from the
    // first handle to the library.
    queue->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(queue->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY(named(window, "queueColumnHandle_length")->hasActiveFocus());
    artistHandle->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(artistHandle->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    QTRY_VERIFY2(library->hasActiveFocus(), qPrintable(QStringLiteral("Shift+Tab went to %1").arg(focusName())));
}

void UiMainWindowTest::columnHandlesShowAQuietMarkAtRest_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// In a populated header, each column's resize boundary shows a quiet mark at
// rest, so a pointer user can see where to resize (it showed nothing until
// hovered): a 1 px stroke in the seam colour, 10 px at 10 pt and growing with
// the font, centred on the labels, where the divider is. It gives way to the
// divider under the pointer and with focus. An empty queue's header, with
// its labels, shows it too.
void UiMainWindowTest::columnHandlesShowAQuietMarkAtRest() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *header = named(window, "queueHeader");
    QVERIFY(header);
    const auto restOf = [window](const char *column) {
        return itemNamed(named(window, QStringLiteral("queueColumnHandle_") + QLatin1String(column)),
                         QStringLiteral("queueColumnHandleRest"));
    };
    QVERIFY(restOf("length") != nullptr);
    QVERIFY(restOf("length")->isVisible());

    setQueue(bridgeOf(window), sampleQueueRows(3));
    for (const char *column : {"artist", "album", "length"}) {
        QQuickItem *handle = named(window, QStringLiteral("queueColumnHandle_") + QLatin1String(column));
        QQuickItem *label = named(window, QStringLiteral("queueHeader_") + QLatin1String(column));
        QVERIFY2(handle && label, column);
        QQuickItem *rest = restOf(column);
        QQuickItem *line = itemNamed(handle, QStringLiteral("queueColumnHandleLine"));
        QVERIFY(rest && line);
        // A narrow queue drops its Album column.
        if (!label->isVisible()) {
            QVERIFY(!rest->isVisible());
            continue;
        }
        QTRY_VERIFY2(rest->isVisible(), column);
        QVERIFY(!line->isVisible());
        QCOMPARE(rest->width(), 1.0);
        QCOMPARE(rest->height(), qreal(scaledSize(window, 10)));
        QCOMPARE(colorOf(rest), tokenHex(window, "seam"));
        const QRectF mark = sceneRect(rest);
        QCOMPARE(mark.left(), std::round(mark.left()));
        QCOMPARE(mark.top(), std::round(mark.top()));
        QCOMPARE(mark.left(), sceneRect(line).left());
        // Short of the header's top and its hairline, and centred on the label.
        QVERIFY(mark.top() > sceneRect(header).top() && mark.bottom() < sceneRect(header).bottom() - 1);
        QVERIFY2(std::abs(mark.center().y() - sceneRect(label).center().y()) <= 1.0,
                 qPrintable(QStringLiteral("%1: mark centred at %2, label at %3")
                                .arg(QLatin1String(column))
                                .arg(mark.center().y())
                                .arg(sceneRect(label).center().y())));
        // Left of the label it belongs to.
        QVERIFY(mark.right() < sceneRect(label).left());
    }

    // Under the pointer, the divider alone.
    QQuickItem *handle = named(window, "queueColumnHandle_length");
    QQuickItem *line = itemNamed(handle, QStringLiteral("queueColumnHandleLine"));
    // From elsewhere: a test before this one may have left the pointer there.
    QTest::mouseMove(window, QPoint(window->width() / 2, window->height() - 60));
    QTest::mouseMove(window, handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint());
    QTRY_VERIFY(line->isVisible());
    QVERIFY(!restOf("length")->isVisible());
    QVERIFY(restOf("artist")->isVisible());
    QTest::mouseMove(window, QPoint(window->width() / 2, window->height() - 60));
    QTRY_VERIFY(!line->isVisible());
    QVERIFY(restOf("length")->isVisible());
    // With keyboard focus, the ring around the divider alone.
    handle->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(line->isVisible());
    QVERIFY(!restOf("length")->isVisible());

    // Emptied, the header keeps its marks.
    named(window, "playlistView")->forceActiveFocus(Qt::TabFocusReason);
    setQueue(bridgeOf(window), QVariantList{});
    QTRY_VERIFY(restOf("length")->isVisible());
    QVERIFY(restOf("artist")->isVisible());
}

void UiMainWindowTest::focusedColumnHandleHasAFrameAndSaysItsKeys_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// A focused column handle has a frame in the app's focus ring around its
// divider, within the gap between the columns: 3 px clear of the label after
// it, and 2 px clear of the seam above the header and of its hairline (a
// 12 px box 2 px off "Artist" and flush on the seam, with no divider in it,
// read as an empty checkbox). Its tooltip says what it is and its keys. The
// pointer's hover is the divider alone, in the tertiary text colour, which
// reads on the header (the seam colour, 1.5:1, barely showed).
void UiMainWindowTest::focusedColumnHandleHasAFrameAndSaysItsKeys() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    setQueue(bridgeOf(window), sampleQueueRows(3));
    QQuickItem *header = named(window, "queueHeader");
    QVERIFY(header);
    for (const char *column : {"artist", "album", "length"}) {
        QQuickItem *handle = named(window, QStringLiteral("queueColumnHandle_") + QLatin1String(column));
        QQuickItem *label = named(window, QStringLiteral("queueHeader_") + QLatin1String(column));
        QVERIFY2(handle && label, column);
        // A narrow queue drops its Album column.
        if (!label->isVisible()) {
            QVERIFY(!handle->isVisible());
            continue;
        }
        QTRY_VERIFY(handle->activeFocusOnTab());
        QQuickItem *frame = itemNamed(handle, QStringLiteral("queueColumnHandleFocus"));
        QQuickItem *line = itemNamed(handle, QStringLiteral("queueColumnHandleLine"));
        QVERIFY(frame && line);
        QVERIFY(!frame->isVisible());
        QVERIFY(!line->isVisible());

        handle->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(frame->isVisible());
        QCOMPARE(QQmlProperty(frame, QStringLiteral("border.width")).read().toReal(), 2.0);
        QCOMPARE(hex(QQmlProperty(frame, QStringLiteral("border.color")).read().value<QColor>()),
                 tokenHex(window, "focusRing"));
        const QRectF ring = sceneRect(frame);
        QVERIFY2(ring.width() >= 9.0, qPrintable(QString::number(ring.width())));
        QCOMPARE(ring.top(), sceneRect(header).top() + 2);
        QCOMPARE(ring.bottom(), sceneRect(header).bottom() - 1 - 2);
        // 3 px clear of the label after it.
        QVERIFY2(ring.right() <= sceneRect(label).left() - 3,
                 qPrintable(QStringLiteral("%1: ring ends at %2, label at %3")
                                .arg(QLatin1String(column)).arg(ring.right()).arg(sceneRect(label).left())));
        QCOMPARE(ring.left(), std::round(ring.left()));
        // The divider stays drawn, in the middle of the ring and inside it.
        QVERIFY2(line->isVisible(), column);
        QCOMPARE(line->width(), 1.0);
        const QRectF divider = sceneRect(line);
        QCOMPARE(divider.left() - ring.left(), ring.right() - divider.right());
        QVERIFY(divider.left() - ring.left() >= 2 + 2);
        QVERIFY(divider.top() >= ring.top() + 2 + 2);
        QVERIFY(divider.bottom() <= ring.bottom() - 2 - 2);
        QCOMPARE(colorOf(line), tokenHex(window, "textTertiary"));
    }

    QQuickItem *handle = named(window, "queueColumnHandle_length");
    QQuickItem *frame = itemNamed(handle, QStringLiteral("queueColumnHandleFocus"));
    QQuickItem *line = itemNamed(handle, QStringLiteral("queueColumnHandleLine"));
    handle->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(frame->isVisible());
    const QString says = evaluateOn(handle, QStringLiteral("ToolTip.text")).toString();
    QCOMPARE(says, QStringLiteral("Resize the Length column · Left or Right to resize, Home for the default width"));
    // After the desktop's tooltip delay, as the other tooltips' on focus.
    QTRY_VERIFY(evaluateOn(handle, QStringLiteral("ToolTip.visible")).toBool());

    named(window, "playlistView")->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(!frame->isVisible());
    QVERIFY(!line->isVisible());
    // Once it has faded out.
    QTRY_VERIFY(!evaluateOn(handle, QStringLiteral("ToolTip.visible")).toBool());
    QTest::mouseMove(window, handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2)).toPoint());
    QTRY_VERIFY(line->isVisible());
    QVERIFY(!frame->isVisible());
    QCOMPARE(line->width(), 1.0);
    // The hover divider reads on the header: 3:1 at least.
    const QColor ink(colorOf(line));
    const QColor view(tokenHex(window, "view"));
    QVERIFY2(contrastRatio(ink, view) >= 3.0, qPrintable(describeContrast(QStringLiteral("hover divider"), ink, view)));
}

// The transport's first glyph starts at the content's 12 px inset, where
// the library's chevrons and the status bar's text do, and its volume ends
// the same 12 px from the right edge: both measured to what is drawn (the
// first button's frame was at 12 px, its glyph at 20).
void UiMainWindowTest::transportStartsAndEndsAtTheContentInset() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("libraryRoots", QStringList{QStringLiteral("/music")});
    showTree(fixture.libraryModel, {});
    QQuickItem *artist = nullptr;
    QTRY_VERIFY((artist = libraryRow(window, QStringLiteral("artist"))) != nullptr);
    QQuickItem *previous = named(window, "transportButton_previous");
    QQuickItem *volume = named(window, "volumeSlider");
    QQuickItem *chevron = itemNamed(artist, QStringLiteral("libraryRowChevron"));
    QQuickItem *previousGlyph = previous != nullptr ? itemNamed(previous, QStringLiteral("transportButtonGlyph")) : nullptr;
    QVERIFY(previous && previousGlyph && volume && chevron);
    QTRY_COMPARE(sceneRect(previousGlyph).left(), 12.0);
    QCOMPARE(sceneRect(chevron).left(), sceneRect(previousGlyph).left());
    QCOMPARE(sceneRect(volume).right(), window->width() - 12.0);
}

// First run's one action is drawn as the default button, as OK is in
// Configure: the desktop style draws that from the button's being marked
// default (`highlighted` alone drew it as any button).
void UiMainWindowTest::firstRunsButtonIsThePrimaryOne() {
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QQuickItem *button = named(window, "firstRunAddFolderButton");
    QVERIFY(shown(button));
    QVERIFY(evaluateOn(button, QStringLiteral("Accessible.defaultButton")).toBool());
    auto *drawnBy = button->property("background").value<QQuickItem *>();
    QVERIFY(drawnBy != nullptr);
    QCOMPARE(drawnBy->property("activeControl").toString(), QStringLiteral("default"));
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
    MainWindowFixture fixture(sized());
    QQuickWindow *window = fixture.window;
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(bridge != nullptr && viewer != nullptr);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());

    // The style's popup, for its transitions.
    QQmlComponent stylePopup(&fixture.engine);
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
    returnFocusFromClosedPopups();
    UiMainWindowTest test;
    return qExecShard(&test, argc, argv);
}

#include "tst_ui_main_window.moc"
