// SPDX-License-Identifier: GPL-3.0-or-later

// The motion of the main window (#373): the hand-over of amber on a track
// change in the queue and the library tree, the now-playing panel's
// cross-fades, the tree's expand and collapse, and the queue's reorder by
// drag. Each uses Kirigami's durations with an OutCubic curve, is instant
// with Plasma's animations off, and leaves nothing running once it is done.

#include <QAbstractAnimation>
#include <QApplication>
#include <QColor>
#include <QEasingCurve>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>
#include <qqml.h>

#include <Kirigami/Platform/Units>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include "../src/LevelMeterItem.h"
#include "../src/LibraryTreeModel.h"
#include "../src/Localization.h"
#include "../src/QueueRows.h"
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
    const QString text = capturedMessages().join(QLatin1Char('\n'));
    capturedMessages().clear();
    return text;
}

QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel) {
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
    window->resize(1600, 1000);
    if (!QTest::qWaitFor([&]() { return window->width() == 1600 && window->height() == 1000; })) {
        return nullptr;
    }
    return window;
}

QObject *bridgeOf(QQuickWindow *window) {
    return qvariant_cast<QObject *>(window->property("uiBridge"));
}

QString hex(const QColor &color) {
    return color.name(QColor::HexArgb);
}

QString nowHex(QQuickWindow *window) {
    auto *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    return hex(palette->property("now").value<QColor>());
}

QString colorOf(const QObject *object) {
    return hex(object->property("color").value<QColor>());
}

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

QQuickItem *named(QQuickWindow *window, const char *name) {
    return itemNamed(window->contentItem(), QString::fromLatin1(name));
}

QQuickItem *itemAtIndex(QQuickItem *view, int index) {
    QQuickItem *row = nullptr;
    QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem *, row), Q_ARG(int, index));
    return row;
}

// The engine's Kirigami.Units, whose durations Plasma's animation speed
// sets: 0 with animations off.
Kirigami::Platform::Units *unitsOf(QQuickWindow *window) {
    QQmlExpression expression(qmlContext(window), window, QStringLiteral("Kirigami.Units"));
    const QVariant value = expression.evaluate();
    return qobject_cast<Kirigami::Platform::Units *>(qvariant_cast<QObject *>(value));
}

QEasingCurve::Type easingOf(const QObject *animation) {
    return animation->property("easing").value<QEasingCurve>().type();
}

QObject *animationOf(QObject *owner) {
    return qvariant_cast<QObject *>(owner->property("animation"));
}

// The queue: five tracks of one album.
QVariantList queueRows() {
    QVariantList rows;
    for (int i = 0; i < 5; ++i) {
        rows.push_back(QVariantMap{
            {QStringLiteral("title"), QStringLiteral("Track %1").arg(i)},
            {QStringLiteral("artist"), QStringLiteral("Artist")},
            {QStringLiteral("album"), QStringLiteral("Album")},
            {QStringLiteral("lengthText"), QStringLiteral("1:00")},
            {QStringLiteral("path"), QStringLiteral("/music/%1.flac").arg(i)},
        });
    }
    return rows;
}

QString queuePath(int index) {
    return QStringLiteral("/music/%1.flac").arg(index);
}

// Plays queue row `index`, as the backend says it at once: its index, its
// path and the state. A gapless change sets them while playing.
void playQueueRow(QObject *bridge, int index, const QString &state = QStringLiteral("Playing")) {
    bridge->setProperty("currentTrackPath", queuePath(index));
    bridge->setProperty("playingQueueIndex", index);
    bridge->setProperty("playbackState", state);
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

const QString kArtistKey = QStringLiteral("artist|Artist A");
const QString kAlbumAKey = QStringLiteral("album|Artist A|Album A");
const QString kAlbumBKey = QStringLiteral("album|Artist A|Album B");
const QString kTrackA1 = QStringLiteral("/music/Artist A/Album A/01.flac");
const QString kTrackA2 = QStringLiteral("/music/Artist A/Album A/02.flac");
const QString kTrackB1 = QStringLiteral("/music/Artist A/Album B/01.flac");

// An artist with two albums: Album A of two tracks, Album B of one, in the
// bridge's tree format.
QByteArray libraryTree() {
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
    const QList<Row> rows{
        {1, 0, 0, 2, 0, QStringLiteral("Artist A (2)"), kArtistKey, QStringLiteral("/music/Artist A"), {}},
        {2, 1, 0, 2, 2005, QStringLiteral("Album A"), kAlbumAKey, QStringLiteral("/music/Artist A/Album A"), {}},
        {4, 2, 1, 0, 0, QStringLiteral("First"), QStringLiteral("track|") + kTrackA1, kTrackA1, kTrackA1},
        {4, 2, 2, 0, 0, QStringLiteral("Second"), QStringLiteral("track|") + kTrackA2, kTrackA2, kTrackA2},
        {2, 1, 0, 1, 2007, QStringLiteral("Album B"), kAlbumBKey, QStringLiteral("/music/Artist A/Album B"), {}},
        {4, 2, 1, 0, 0, QStringLiteral("Third"), QStringLiteral("track|") + kTrackB1, kTrackB1, kTrackB1},
    };
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

void showTree(LibraryTreeModel &model, const QStringList &expanded) {
    model.setExpandedKeys(expanded);
    model.setLibraryTreeFromBinary(libraryTree());
}

// Plays a library track, as the backend says it: the path and state at
// once, the rows that hold it a moment later with its metadata.
void playTreeTrack(QObject *bridge, const QString &path, const QString &albumKey) {
    bridge->setProperty("currentTrackPath", path);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    bridge->setProperty("currentTrackTreeKeys", QStringList{kArtistKey, albumKey});
}

// The row of the library tree that shows `key`, as the view shows it.
QQuickItem *libraryRowWithKey(QQuickWindow *window, const QString &key) {
    QQuickItem *view = named(window, "libraryView");
    const int count = view != nullptr ? view->property("count").toInt() : 0;
    for (int index = 0; index < count; ++index) {
        QQuickItem *row = itemAtIndex(view, index);
        if (row != nullptr && row->property("selectionKeyResolved").toString() == key) {
            return row;
        }
    }
    return nullptr;
}

QQuickItem *libraryRowWithPath(QQuickWindow *window, const QString &path) {
    QQuickItem *view = named(window, "libraryView");
    const int count = view != nullptr ? view->property("count").toInt() : 0;
    for (int index = 0; index < count; ++index) {
        QQuickItem *row = itemAtIndex(view, index);
        if (row != nullptr && row->property("trackPathResolved").toString() == path) {
            return row;
        }
    }
    return nullptr;
}

// What a queue or tree row shows of its state: its live glyph, the glyph
// fading out, and its title.
struct RowLook {
    QQuickItem *glyph = nullptr;
    QQuickItem *fadingGlyph = nullptr;
    QQuickItem *title = nullptr;
};

RowLook queueRowLook(QQuickItem *row) {
    return {itemNamed(row, QStringLiteral("queueRowState")),
            itemNamed(row, QStringLiteral("queueRowFadingState")),
            itemNamed(row, QStringLiteral("queueCell_title"))};
}

RowLook libraryRowLook(QQuickItem *row) {
    return {itemNamed(row, QStringLiteral("libraryRowState")),
            itemNamed(row, QStringLiteral("libraryRowFadingState")),
            itemNamed(row, QStringLiteral("libraryRowTitle"))};
}

// Whether a row of a list shows its children's y as they are once its
// transitions are done: `y` of row `index` is `index` rows down.
bool rowsSettled(QQuickItem *view, int count) {
    const double rowHeight = view->property("rowHeight").toDouble();
    for (int index = 0; index < count; ++index) {
        QQuickItem *row = itemAtIndex(view, index);
        if (row == nullptr || !qFuzzyCompare(row->y() + 1.0, index * rowHeight + 1.0)) {
            return false;
        }
    }
    return true;
}

// The time of every animation of the GUI thread, while it lives: it stands
// still but for the steps the test takes, however busy the machine. For the
// lists' transitions, whose animations cannot be paused or set to a point.
// Qt takes it only while no other driver is installed: main() keeps Qt
// Quick on the basic render loop, whose window installs none.
class SteppedAnimationDriver : public QAnimationDriver {
public:
    SteppedAnimationDriver() { install(); }
    ~SteppedAnimationDriver() override { uninstall(); }
    SteppedAnimationDriver(const SteppedAnimationDriver &) = delete;
    SteppedAnimationDriver &operator=(const SteppedAnimationDriver &) = delete;

    qint64 elapsed() const override { return m_elapsed; }

    // Lets `ms` of animation time pass, a frame of 16 ms at a time, as the
    // window would draw them. Whether this driver ran the animations: Qt
    // starts the installed driver once one runs, so it is false when Qt
    // left it out, and the animations ran on real time instead.
    [[nodiscard]] bool step(int ms) {
        bool drove = false;
        for (int done = 0; done < ms; done += kFrame) {
            // The animations that started since: they run from the next
            // frame on.
            QCoreApplication::processEvents();
            drove = drove || isRunning();
            m_elapsed += std::min(kFrame, ms - done);
            advance();
        }
        return drove;
    }

private:
    static constexpr int kFrame = 16;
    qint64 m_elapsed = 0;
};

// The frames `window` draws over `ms` of wall time.
int framesOver(QQuickWindow *window, int ms) {
    int frames = 0;
    const QMetaObject::Connection connection = QObject::connect(
        window, &QQuickWindow::frameSwapped, window, [&frames]() { ++frames; });
    QTest::qWait(ms);
    QObject::disconnect(connection);
    return frames;
}

// Covers, as image files of the test's own, each of one colour: `first`
// and `second`, then more for the tracks after them, all square; and
// `landscape`, twice as wide as it is high.
struct Covers {
    QTemporaryDir dir;
    QString first;
    QString second;
    QStringList all;
    QString landscape;

    Covers() {
        const QList<QColor> colors{QColor(200, 40, 40), QColor(40, 40, 200), QColor(40, 160, 40),
                                   QColor(200, 160, 40), QColor(120, 40, 160)};
        for (qsizetype index = 0; index < colors.size(); ++index) {
            const QString name = QStringLiteral("cover%1.png").arg(index);
            QImage image(64, 64, QImage::Format_RGB32);
            image.fill(colors.at(index));
            image.save(dir.filePath(name));
            all.push_back(QUrl::fromLocalFile(dir.filePath(name)).toString());
        }
        first = all.at(0);
        second = all.at(1);
        QImage wide(128, 64, QImage::Format_RGB32);
        wide.fill(QColor(40, 160, 160));
        wide.save(dir.filePath(QStringLiteral("landscape.png")));
        landscape = QUrl::fromLocalFile(dir.filePath(QStringLiteral("landscape.png"))).toString();
    }
};

// The order a track's properties change in. The backend changes them in one
// snapshot, but BridgeClient tells the track's identity (its path) before
// its metadata (title, album, cover); the fallback bridge tells each as it
// is set.
enum class Order { PathFirst, CoverFirst };

// Plays a track for the now-playing panel, by default in BridgeClient's
// order.
void showTrack(QObject *bridge, const QString &path, const QString &title, const QString &album,
               const QString &cover = QString(), Order order = Order::PathFirst) {
    if (order == Order::CoverFirst) {
        bridge->setProperty("currentTrackCoverPath", cover);
    }
    bridge->setProperty("currentTrackPath", path);
    bridge->setProperty("currentTrackTitle", title);
    bridge->setProperty("currentTrackAlbum", album);
    bridge->setProperty("currentTrackCoverPath", cover);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
}

QString textOf(QQuickItem *panel, const char *name) {
    return itemNamed(panel, QString::fromLatin1(name))->property("text").toString();
}

bool shownIn(QQuickItem *panel, const char *name) {
    return itemNamed(panel, QString::fromLatin1(name))->isVisible();
}

// The lines of the now-playing panel, by their items' names.
const QStringList kWideLines{QStringLiteral("Title"), QStringLiteral("ArtistAlbum"), QStringLiteral("Details")};
const QStringList kNarrowLines{QStringLiteral("NarrowTitle"), QStringLiteral("NarrowArtist"),
                               QStringLiteral("NarrowAlbum"), QStringLiteral("NarrowDetails")};

// A line of the now-playing panel as it draws: the playing track's text
// where it stays still, the rest of it fading in as one, and over that the
// line before fading out part by part. Looked up anew each time: a fade
// that starts again makes its parts again.
struct PanelLine {
    QQuickItem *panel = nullptr;
    QString name;

    QQuickItem *stillText() const { return itemNamed(panel, QStringLiteral("nowPlaying") + name); }
    QQuickItem *still() const { return stillText()->parentItem(); }
    QQuickItem *line() const { return still()->parentItem(); }
    QQuickItem *incoming() const { return itemNamed(line(), QStringLiteral("nowPlayingIncoming") + name); }
    bool valid() const { return stillText() != nullptr; }
    // The parts of the line before, in order.
    QList<QQuickItem *> parts() const {
        return itemsNamed(line(), QStringLiteral("nowPlayingOutgoing") + name + QStringLiteral("Part"));
    }
    QString text() const { return stillText()->property("text").toString(); }
    // The line before's text, or "" when it does not fade.
    QString outgoingText() const {
        const QList<QQuickItem *> all = parts();
        return all.isEmpty()
            ? QString()
            : itemNamed(all.first(), QStringLiteral("nowPlayingOutgoing") + name)->property("text").toString();
    }
    // The opacity of the part of the line before that shows most, 0 when
    // none fades.
    double outgoingOpacity() const {
        double opacity = 0.0;
        for (QQuickItem *part : parts()) {
            opacity = std::max(opacity, part->opacity());
        }
        return opacity;
    }
    double incomingOpacity() const { return incoming()->isVisible() ? incoming()->opacity() : 1.0; }
    bool fades() const { return outgoingOpacity() > 0; }
    double stillWidth() const { return still()->width(); }
    QFont font() const { return stillText()->property("font").value<QFont>(); }
    // The opacities the line before and the playing track's line show with
    // at `x`, in the line: both whole where the line stays still.
    std::pair<double, double> opacitiesAt(double x) const {
        if (x < stillWidth()) {
            return {1.0, 1.0};
        }
        for (QQuickItem *part : parts()) {
            if (x >= part->x() && x < part->x() + part->width()) {
                return {part->opacity(), incomingOpacity()};
            }
        }
        return {0.0, incomingOpacity()};
    }
    // Where the parts of the line before start, past the first.
    QList<double> innerEdges() const {
        QList<double> edges;
        const QList<QQuickItem *> all = parts();
        for (qsizetype index = 1; index < all.size(); ++index) {
            edges.push_back(all.at(index)->x());
        }
        return edges;
    }
};

PanelLine panelLine(QQuickItem *panel, const QString &name) {
    return PanelLine{panel, name};
}

// The opacity `item` draws with, up to `top`.
double opacityUpTo(QQuickItem *item, QQuickItem *top) {
    double opacity = 1.0;
    for (QQuickItem *at = item; at != nullptr && at != top; at = at->parentItem()) {
        opacity *= at->opacity();
    }
    return opacity;
}

// Whether `name` shows the playing track's line whole, at full opacity,
// with nothing of the line before over it: none fades, or none of the
// line is left to.
bool lineStill(QQuickItem *panel, const QString &name) {
    const PanelLine line = panelLine(panel, name);
    return line.valid() && qFuzzyCompare(line.stillWidth() + 1.0, line.line()->width() + 1.0)
        && qFuzzyCompare(opacityUpTo(line.stillText(), panel), 1.0);
}

bool anyLineFades(QQuickItem *panel) {
    for (const QString &name : kWideLines + kNarrowLines) {
        if (panelLine(panel, name).fades()) {
            return true;
        }
    }
    return false;
}

bool allLinesStill(QQuickItem *panel) {
    for (const QString &name : kWideLines + kNarrowLines) {
        if (!lineStill(panel, name)) {
            return false;
        }
    }
    return true;
}

// Whether `line` keeps `start`, all of its leading fields that do not
// change, still: up to the space after it, and no further.
bool keepsStill(const PanelLine &line, const QString &start) {
    const QFontMetricsF metrics(line.font());
    const double width = line.stillWidth();
    return width >= metrics.horizontalAdvance(start)
        && width <= metrics.horizontalAdvance(start + QLatin1Char(' '));
}

// What draws at a point of a line of the now-playing panel: a glyph of one
// of its layers and the opacity it draws with.
struct Drawn {
    QChar glyph;
    double opacity = 0.0;
};

// The glyph of `text`, laid out from the left, whose advance takes in `x`;
// null past its end.
QChar glyphAt(const QFontMetricsF &metrics, const QString &text, double x) {
    double left = 0.0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const double right = metrics.horizontalAdvance(text.left(index + 1));
        if (x >= left && x < right) {
            return text.at(index);
        }
        left = right;
    }
    return {};
}

// The lines before of `line`, oldest first.
QList<QQuickItem *> layersOf(const PanelLine &line) {
    return itemsNamed(line.line(), QStringLiteral("nowPlayingOutgoing") + line.name + QStringLiteral("Layer"));
}

QList<QQuickItem *> partsOf(const PanelLine &line, QQuickItem *layer) {
    return itemsNamed(layer, QStringLiteral("nowPlayingOutgoing") + line.name + QStringLiteral("Part"));
}

QString partText(const PanelLine &line, QQuickItem *part) {
    return itemNamed(part, QStringLiteral("nowPlayingOutgoing") + line.name)->property("text").toString();
}

// What `line` draws at `x`, bottom to top: the still start, each part of the
// lines before, oldest first, then the playing track's text fading in;
// leaving out what is at opacity 0. The glyph is null past the end of its
// text.
QList<Drawn> drawnAt(const PanelLine &line, double x) {
    const QFontMetricsF metrics(line.font());
    QList<Drawn> drawn;
    const auto add = [&](const QString &text, double opacity) {
        if (opacity > 0) {
            drawn.push_back({glyphAt(metrics, text, x), opacity});
        }
    };
    if (x < line.stillWidth()) {
        add(line.text(), 1.0);
    }
    for (QQuickItem *layer : layersOf(line)) {
        for (QQuickItem *part : partsOf(line, layer)) {
            if (x >= part->x() && x < part->x() + part->width()) {
                add(partText(line, part), part->opacity());
            }
        }
    }
    QQuickItem *incoming = line.incoming();
    if (incoming->isVisible() && x >= incoming->x() && x < incoming->x() + incoming->width()) {
        add(line.text(), incoming->opacity());
    }
    return drawn;
}

// What the panel draws, line by line at points 2 px apart across each,
// then the cover: each line's draws and the cover's layers, bottom to top.
struct PanelLook {
    QList<QList<QList<Drawn>>> lines;
    // Each cover that shows, by its image ("" for the placeholder), from the
    // one that hides all under it up.
    QList<std::pair<QString, double>> cover;
};

QList<std::pair<QString, double>> coverLook(QQuickItem *panel) {
    QQuickItem *tile = itemNamed(panel, QStringLiteral("nowPlayingCover"));
    QList<std::pair<QString, double>> layers{
        {tile->childItems().first()->property("source").toUrl().toString(), 1.0}};
    QList<QQuickItem *> outgoing = itemsNamed(tile, QStringLiteral("albumArtOutgoing"));
    std::stable_sort(outgoing.begin(), outgoing.end(),
                     [](QQuickItem *left, QQuickItem *right) { return left->z() < right->z(); });
    for (QQuickItem *cover : outgoing) {
        if (cover->isVisible()) {
            layers.push_back({cover->childItems().first()->property("source").toUrl().toString(),
                              cover->opacity()});
        }
    }
    // A cover image draws on the colour behind the tile, around it too: one
    // at full opacity hides all under it, whatever its shape. The
    // placeholder is a faint tint, which hides nothing.
    qsizetype whole = 0;
    for (qsizetype index = 0; index < layers.size(); ++index) {
        if (!layers.at(index).first.isEmpty() && qFuzzyCompare(layers.at(index).second, 1.0)) {
            whole = index;
        }
    }
    return layers.mid(whole);
}

PanelLook panelLook(QQuickItem *panel, const QStringList &names) {
    PanelLook look;
    for (const QString &name : names) {
        const PanelLine line = panelLine(panel, name);
        QList<QList<Drawn>> points;
        for (double x = 0.5; x < line.line()->width(); x += 2.0) {
            points.push_back(drawnAt(line, x));
        }
        look.lines.push_back(points);
    }
    look.cover = coverLook(panel);
    return look;
}

bool sameOpacity(double left, double right) {
    return std::abs(left - right) < 1e-9;
}

// Where `after` differs from `before`, or "": the same layers in the same
// order, at the same opacities; with `glyphs`, the same glyphs at the same
// opacities, leaving out the layers that draw none there, and with
// `images`, the covers the same images.
QString lookChange(const PanelLook &before, const PanelLook &after, const QStringList &names, bool glyphs,
                   bool images) {
    for (qsizetype line = 0; line < names.size(); ++line) {
        for (qsizetype point = 0; point < before.lines.at(line).size(); ++point) {
            const auto drawing = [glyphs](QList<Drawn> drawn) {
                if (glyphs) {
                    drawn.removeIf([](const Drawn &each) { return each.glyph.isNull() || each.glyph.isSpace(); });
                }
                return drawn;
            };
            const QList<Drawn> was = drawing(before.lines.at(line).at(point));
            const QList<Drawn> now = drawing(after.lines.at(line).at(point));
            bool same = was.size() == now.size();
            for (qsizetype index = 0; same && index < was.size(); ++index) {
                same = sameOpacity(was.at(index).opacity, now.at(index).opacity)
                    && (!glyphs || was.at(index).glyph == now.at(index).glyph);
            }
            if (!same) {
                const auto describe = [](const QList<Drawn> &drawn) {
                    QStringList all;
                    for (const Drawn &each : drawn) {
                        all.push_back(QStringLiteral("%1 %2").arg(each.glyph).arg(each.opacity));
                    }
                    return all.join(QStringLiteral(", "));
                };
                return QStringLiteral("%1 at %2 px: [%3] -> [%4]")
                    .arg(names.at(line)).arg(0.5 + 2.0 * point).arg(describe(was), describe(now));
            }
        }
    }
    bool same = before.cover.size() == after.cover.size();
    for (qsizetype index = 0; same && index < before.cover.size(); ++index) {
        same = sameOpacity(before.cover.at(index).second, after.cover.at(index).second)
            && (!images || before.cover.at(index).first == after.cover.at(index).first);
    }
    if (!same) {
        QStringList was;
        QStringList now;
        for (const auto &[source, opacity] : before.cover) {
            was.push_back(QStringLiteral("%1 %2").arg(QFileInfo(QUrl(source).toLocalFile()).fileName()).arg(opacity));
        }
        for (const auto &[source, opacity] : after.cover) {
            now.push_back(QStringLiteral("%1 %2").arg(QFileInfo(QUrl(source).toLocalFile()).fileName()).arg(opacity));
        }
        return QStringLiteral("cover: [%1] -> [%2]").arg(was.join(QStringLiteral(", ")), now.join(QStringLiteral(", ")));
    }
    return {};
}

// Whether the panel shows the playing track alone: every line whole and
// still, with no line before; the cover alone, at full opacity.
bool panelIdle(QQuickItem *panel, const QStringList &names) {
    for (const QString &name : names) {
        const PanelLine line = panelLine(panel, name);
        if (!lineStill(panel, name) || !layersOf(line).isEmpty() || line.incoming()->isVisible()) {
            return false;
        }
    }
    QQuickItem *tile = itemNamed(panel, QStringLiteral("nowPlayingCover"));
    for (QQuickItem *cover : itemsNamed(tile, QStringLiteral("albumArtOutgoing"))) {
        if (cover->isVisible()) {
            return false;
        }
    }
    return qFuzzyCompare(opacityUpTo(tile->childItems().first(), panel), 1.0);
}

// Plays a track as BridgeClient tells it: the path, then the metadata, then
// the cover.
void tellTrack(QObject *bridge, const QString &path, const QString &title, const QString &album, int year,
               int number, const QString &cover) {
    bridge->setProperty("currentTrackPath", path);
    bridge->setProperty("currentTrackTitle", title);
    bridge->setProperty("currentTrackAlbum", album);
    bridge->setProperty("currentTrackYear", year);
    bridge->setProperty("currentTrackNumber", number);
    bridge->setProperty("currentTrackCoverPath", cover);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
}

} // namespace

class UiMotionTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();
    void cleanupTestCase();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void queueTrackChangeHandsTheAmberOver();
    void queueTrackChangeHandsTheAmberOver_data();
    void queueRepeatOneKeepsTheRowAmber();
    void queueEditsMovingThePlayingRowFadeNothing();
    void treeTrackChangeHandsTheAmberOver();
    void reusedTreeRowsShowNoStaleFade();
    void nowPlayingNextTrackOfAnAlbumFadesOnlyWhatChanges();
    void nowPlayingNextTrackOfAnAlbumFadesOnlyWhatChanges_data();
    void nowPlayingCrossFadesCoverAndTextForAnotherAlbum();
    void nowPlayingLineThatComesToDifferJoinsTheFade();
    void nowPlayingStillFieldThatChangesLateFadesFromWhole();
    void nowPlayingStillFieldThatChangesLateFadesFromWhole_data();
    void nowPlayingRightToLeftLineFadesWhole();
    void nowPlayingLineThatTurnsRightToLeftFadesInWhole();
    void nowPlayingElidedLineKeepsEdgesOffTheEllipsis();
    void nowPlayingLineCutOffWithinWhatItSharesStaysStill();
    void nowPlayingSharedStartIsWholeFields();
    void nowPlayingSharedStartIsWholeFields_data();
    void nowPlayingClearedAndReloadedMidFadeShowsNoStaleTrack();
    void nowPlayingLateCoverJoinsTheFadeOfTheLines();
    void nowPlayingRapidChangesFadeFromWhatShows();
    void nowPlayingRapidChangesFadeFromWhatShows_data();
    void nowPlayingCoverAfterTheFadeFadesAlone();
    void nowPlayingLeavesNothingPlayingWithoutAFade();
    void nowPlayingLeavesNothingPlayingWithoutAFade_data();
    void nowPlayingLateCoverKeepsEveryLayerContinuous();
    void nowPlayingLateCoverKeepsEveryLayerContinuous_data();
    void nowPlayingThirdTrackGoesOnFromEveryLayer();
    void nowPlayingThirdTrackGoesOnFromEveryLayer_data();
    void nowPlayingSkippingThroughTracksKeepsEveryLayer();
    void nowPlayingRepeatOneDuringAJoinedFadeChangesNothing();
    void nowPlayingThirdTrackWithAnimationsOffIsInstant();
    void nowPlayingInterruptedFadeAddsNoPlaceholderOverItself();
    void nowPlayingAnotherAlbumWithoutCoversFadesItsLinesLong();
    void nowPlayingCoverOfAnotherShapeHidesWhatIsUnderIt();
    void treeToggleOpensDownwardAndKeepsTheRowPut();
    void treeChangesOtherThanAToggleAreInstant();
    void queueRowsADragDisplacesSlide();
    void queueRowsTheKeyboardMovesMoveAtOnce();
    void animationsOffMakeEveryChangeInstant();
    void nothingAnimatesOnceTheMotionIsDone();
    void nowPlayingKeepsTheCoverUntilTheNextTracksIsKnown();
};

void UiMotionTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(captureMessage);
    FerrousLocalization::setUpApplicationDomain();
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterType<WindowPresence>("FerrousUi", 1, 0, "WindowPresence");
}

void UiMotionTest::init() {
    takeMessages();
}

void UiMotionTest::cleanup() {
    const QString messages = takeMessages();
    QVERIFY2(messages.isEmpty(), qPrintable(messages));
}

void UiMotionTest::cleanupTestCase() {
    qInstallMessageHandler(previousHandler());
    previousHandler() = nullptr;
}

void UiMotionTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void UiMotionTest::queueTrackChangeHandsTheAmberOver_data() {
    QTest::addColumn<bool>("gapless");
    // A track the user starts: the state goes on playing either way.
    QTest::newRow("next") << false;
    // The next track starting at the end of this one, gaplessly.
    QTest::newRow("gapless") << true;
}

// The new playing row is amber at once; only the old row's amber glyph and
// title fade out, over shortDuration with OutCubic.
void UiMotionTest::queueTrackChangeHandsTheAmberOver() {
    QFETCH(bool, gapless);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    playQueueRow(bridge, 1);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 2) != nullptr; }));
    QQuickItem *pane = named(window, "mainQueuePane");
    QObject *fade = qvariant_cast<QObject *>(pane->property("nowFade"));
    QVERIFY(fade != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QVERIFY(units->shortDuration() > 0);
    QCOMPARE(animationOf(fade)->property("duration").toInt(), units->shortDuration());
    QCOMPARE(easingOf(animationOf(fade)), QEasingCurve::OutCubic);

    const RowLook oldRow = queueRowLook(itemAtIndex(view, 1));
    const RowLook newRow = queueRowLook(itemAtIndex(view, 2));
    QCOMPARE(colorOf(oldRow.title), nowHex(window));
    QVERIFY(!oldRow.fadingGlyph->isVisible());

    if (gapless) {
        // The backend moves on in one step: index and path together.
        bridge->setProperty("playingQueueIndex", 2);
        bridge->setProperty("currentTrackPath", queuePath(2));
    } else {
        playQueueRow(bridge, 2);
    }

    // At once, before a frame: the new row is amber, the old one fades.
    QCOMPARE(colorOf(newRow.title), nowHex(window));
    QCOMPARE(newRow.title->property("font").value<QFont>().weight(), QFont::DemiBold);
    QVERIFY(newRow.glyph->isVisible());
    QCOMPARE(colorOf(newRow.glyph), nowHex(window));
    QVERIFY(!newRow.fadingGlyph->isVisible());
    QVERIFY(animationOf(fade)->property("running").toBool());
    QVERIFY(oldRow.fadingGlyph->isVisible());
    QCOMPARE(oldRow.fadingGlyph->property("source").toString(), QStringLiteral("media-playback-start"));
    QCOMPARE(colorOf(oldRow.fadingGlyph), nowHex(window));
    QCOMPARE(oldRow.fadingGlyph->opacity(), 1.0);
    QCOMPARE(colorOf(oldRow.title), nowHex(window));
    // Its weight goes with the state, at once.
    QCOMPARE(oldRow.title->property("font").value<QFont>().weight(), QFont::Normal);
    QVERIFY(!oldRow.glyph->isVisible());

    // Then it is plain, and nothing runs.
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    QVERIFY(!oldRow.fadingGlyph->isVisible());
    QVERIFY(colorOf(oldRow.title) != nowHex(window));
    QVERIFY(fade->property("target").isNull() || !fade->property("target").value<QJSValue>().isObject());
    QCOMPARE(colorOf(newRow.title), nowHex(window));
}

// Repeat-One plays the same row again: it stays amber, nothing fades.
void UiMotionTest::queueRepeatOneKeepsTheRowAmber() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    bridge->setProperty("repeatMode", 1);
    playQueueRow(bridge, 3);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 3) != nullptr; }));
    QObject *fade = qvariant_cast<QObject *>(named(window, "mainQueuePane")->property("nowFade"));

    // The backend says the same track again, as at the start of a repeat.
    playQueueRow(bridge, 3);
    const RowLook row = queueRowLook(itemAtIndex(view, 3));
    QCOMPARE(colorOf(row.title), nowHex(window));
    QVERIFY(row.glyph->isVisible());
    QVERIFY(!row.fadingGlyph->isVisible());
    QVERIFY(!animationOf(fade)->property("running").toBool());
}

// A queue edit that moves the playing row changes its index but not the
// track: no other row takes its amber, not even for a moment.
void UiMotionTest::queueEditsMovingThePlayingRowFadeNothing() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    playQueueRow(bridge, 3);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 4) != nullptr; }));

    // Row 1 is removed: the playing track is row 2 now, and row 3 is the
    // track that followed it.
    QVariantList rows = queueRows();
    rows.removeAt(1);
    bridge->setProperty("queueRows", rows);
    bridge->setProperty("queueLength", 4);
    bridge->setProperty("playingQueueIndex", 2);
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 3) != nullptr; }));
    for (int index = 0; index < 4; ++index) {
        QVERIFY2(!queueRowLook(itemAtIndex(view, index)).fadingGlyph->isVisible(),
                 qPrintable(QStringLiteral("row %1 fades").arg(index)));
    }
    QCOMPARE(colorOf(queueRowLook(itemAtIndex(view, 2)).title), nowHex(window));
}

// The tree hands the amber over as the queue does: the new track row at
// once, the old one fading; a collapsed album that held the old track
// fades its 60% glyph out.
void UiMotionTest::treeTrackChangeHandsTheAmberOver() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QVERIFY(QTest::qWaitFor([&]() { return libraryRowWithPath(window, kTrackA2) != nullptr; }));
    QQuickItem *pane = named(window, "libraryPane");
    QObject *fade = qvariant_cast<QObject *>(pane->property("nowFade"));
    QVERIFY(fade != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QCOMPARE(animationOf(fade)->property("duration").toInt(), units->shortDuration());
    QCOMPARE(easingOf(animationOf(fade)), QEasingCurve::OutCubic);

    const RowLook first = libraryRowLook(libraryRowWithPath(window, kTrackA1));
    const RowLook second = libraryRowLook(libraryRowWithPath(window, kTrackA2));
    QCOMPARE(colorOf(first.title), nowHex(window));

    // The next track of the album: its row is amber at once. The backend
    // names the rows that hold it with its metadata, after its path.
    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTreeKeys", QStringList{});
    QCOMPARE(colorOf(second.title), nowHex(window));
    QVERIFY(second.glyph->isVisible());
    QVERIFY(!second.fadingGlyph->isVisible());
    QVERIFY(first.fadingGlyph->isVisible());
    QCOMPARE(first.fadingGlyph->opacity(), 1.0);
    QCOMPARE(colorOf(first.title), nowHex(window));
    bridge->setProperty("currentTrackTreeKeys", QStringList{kArtistKey, kAlbumAKey});
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    QVERIFY(!first.fadingGlyph->isVisible());
    QVERIFY(colorOf(first.title) != nowHex(window));

    // Album B is collapsed and holds the next track; then Album A plays
    // again, and Album B's dim mark fades out.
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    QVERIFY(albumB != nullptr);
    playTreeTrack(bridge, kTrackB1, kAlbumBKey);
    const RowLook album = libraryRowLook(albumB);
    QVERIFY(album.glyph->isVisible());
    QCOMPARE(album.glyph->opacity(), 0.6);
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QVERIFY(!album.glyph->isVisible());
    QVERIFY(album.fadingGlyph->isVisible());
    QCOMPARE(album.fadingGlyph->opacity(), 0.6);
    QCOMPARE(colorOf(album.fadingGlyph), nowHex(window));
    // Its title is never amber.
    QVERIFY(colorOf(album.title) != nowHex(window));
    QCOMPARE(colorOf(libraryRowLook(libraryRowWithPath(window, kTrackA1)).title), nowHex(window));
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    QVERIFY(!album.fadingGlyph->isVisible());
}

// The tree reuses its rows: a row that comes back for another track never
// shows the fade of the track it showed, nor a transition's leftovers.
void UiMotionTest::reusedTreeRowsShowNoStaleFade() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QVERIFY(QTest::qWaitFor([&]() { return libraryRowWithPath(window, kTrackA2) != nullptr; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller"));
    QObject *fade = qvariant_cast<QObject *>(named(window, "libraryPane")->property("nowFade"));

    // A1's amber fades; Album A closes while it does, and Album B opens in
    // its rows.
    playTreeTrack(bridge, kTrackB1, kAlbumBKey);
    QVERIFY(animationOf(fade)->property("running").toBool());
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumBKey));
    QVERIFY(QTest::qWaitFor([&]() { return libraryRowWithPath(window, kTrackB1) != nullptr; }));
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(QTest::qWaitFor([&]() { return rowsSettled(view, view->property("count").toInt()); }));
    for (int index = 0; index < view->property("count").toInt(); ++index) {
        QQuickItem *row = itemAtIndex(view, index);
        QVERIFY(row != nullptr);
        QCOMPARE(row->opacity(), 1.0);
        QCOMPARE(row->z(), 1.0);
        const bool fadingA1 = row->property("trackPathResolved").toString() == kTrackA1;
        QVERIFY2(fadingA1 || !libraryRowLook(row).fadingGlyph->isVisible(),
                 qPrintable(QStringLiteral("row %1 shows a fade").arg(index)));
    }
    QCOMPARE(colorOf(libraryRowLook(libraryRowWithPath(window, kTrackB1)).title), nowHex(window));
}

void UiMotionTest::nowPlayingNextTrackOfAnAlbumFadesOnlyWhatChanges_data() {
    QTest::addColumn<bool>("narrow");
    QTest::newRow("wide") << false;
    QTest::newRow("narrow") << true;
}

// The next track of the same album: the cover stays, and of the lines only
// what changes cross-fades, in place, over shortDuration: the title, and
// the track number in the details, past the year and the genre. The artist
// and the album stay at full opacity in every frame.
void UiMotionTest::nowPlayingNextTrackOfAnAlbumFadesOnlyWhatChanges() {
    QFETCH(bool, narrow);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    if (narrow) {
        window->resize(1000, 720);
    }
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("currentTrackPath", kTrackA1);
    bridge->setProperty("currentTrackTitle", QStringLiteral("First"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album A"));
    bridge->setProperty("currentTrackYear", 2005);
    bridge->setProperty("currentTrackGenre", QStringLiteral("Classical"));
    bridge->setProperty("currentTrackNumber", 1);
    bridge->setProperty("currentTrackTotal", 2);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_COMPARE(panel->property("layoutMode").toString(),
                 narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    QVERIFY(handover != nullptr);
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(allLinesStill(panel));
    QTRY_VERIFY(panelLine(panel, narrow ? QStringLiteral("NarrowDetails") : QStringLiteral("Details")).line()->width() > 0);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    // Long enough for some frames.
    units->setShortDuration(600);

    const QString title = narrow ? QStringLiteral("NarrowTitle") : QStringLiteral("Title");
    const QString details = narrow ? QStringLiteral("NarrowDetails") : QStringLiteral("Details");
    const QString sharedDetails = narrow ? QStringLiteral("Classical ·") : QStringLiteral("2005 · Classical ·");
    const QStringList stillLines = narrow
        ? QStringList{QStringLiteral("NarrowArtist"), QStringLiteral("NarrowAlbum")}
        : QStringList{QStringLiteral("ArtistAlbum")};
    QString failure;
    // The frames the window drew while the fade ran, and the looks
    // checked, at those frames and at the points the test sets.
    int frames = 0;
    int checks = 0;
    const auto checkFrame = [&]() {
        if (!failure.isEmpty() || !handover->property("running").toBool()) {
            return;
        }
        ++checks;
        for (const QString &name : stillLines) {
            if (!lineStill(panel, name)) {
                failure = QStringLiteral("%1 is not still in check %2").arg(name).arg(checks);
            }
        }
        for (const QString &name : {title, details}) {
            const PanelLine line = panelLine(panel, name);
            if (!qFuzzyCompare(line.incomingOpacity() + line.outgoingOpacity() + 1.0, 2.0)) {
                failure = QStringLiteral("%1 fades in at %2 and out at %3 in check %4")
                              .arg(name).arg(line.incomingOpacity()).arg(line.outgoingOpacity()).arg(checks);
            }
        }
        const PanelLine titleLine = panelLine(panel, title);
        if (titleLine.fades() && titleLine.stillWidth() != 0) {
            failure = QStringLiteral("the title keeps %1 px still").arg(titleLine.stillWidth());
        }
        const PanelLine detailsLine = panelLine(panel, details);
        if (detailsLine.fades() && !keepsStill(detailsLine, sharedDetails)) {
            failure = QStringLiteral("the details keep %1 px still").arg(detailsLine.stillWidth());
        }
    };
    const QMetaObject::Connection connection =
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [&]() {
            if (handover->property("running").toBool()) {
                ++frames;
            }
            checkFrame();
        });

    // As BridgeClient tells it: the path, then the metadata.
    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Second"));
    bridge->setProperty("currentTrackNumber", 2);

    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), 600);
    QCOMPARE(easingOf(handover), QEasingCurve::OutCubic);
    // The playing track's lines, coming in; the track before's going out
    // where they differ.
    const PanelLine titleLine = panelLine(panel, title);
    const PanelLine detailsLine = panelLine(panel, details);
    QCOMPARE(titleLine.text(), QStringLiteral("Second"));
    QVERIFY(titleLine.fades());
    QCOMPARE(titleLine.outgoingText(), QStringLiteral("First"));
    QVERIFY(detailsLine.fades());
    QCOMPARE(detailsLine.text(),
             narrow ? QStringLiteral("Classical · Track 2 of 2") : QStringLiteral("2005 · Classical · Track 2 of 2"));
    QCOMPARE(detailsLine.outgoingText(),
             narrow ? QStringLiteral("Classical · Track 1 of 2") : QStringLiteral("2005 · Classical · Track 1 of 2"));
    QVERIFY(keepsStill(detailsLine, sharedDetails));
    for (const QString &name : stillLines) {
        QVERIFY2(lineStill(panel, name), qPrintable(name));
    }
    QVERIFY(!itemNamed(panel, QStringLiteral("albumArtOutgoing"))->isVisible());

    // Along the way, as the fade stands at each point of it, and as the
    // window draws it there: held there, a frame comes whatever the load.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.25, 0.5, 0.75}) {
        const int framesBefore = frames;
        panel->setProperty("handover", at);
        checkFrame();
        QVERIFY(QTest::qWaitFor([&]() { return frames > framesBefore; }));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(titleLine.outgoingOpacity(), 1.0 - at);
        QCOMPARE(detailsLine.outgoingOpacity(), 1.0 - at);
    }
    panel->setProperty("handover", 0.0);
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));

    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(frames > 0);
    QVERIFY(allLinesStill(panel));

    // Metadata that comes later changes the lines in place.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Baroque"));
    QVERIFY(!handover->property("running").toBool());
    QCOMPARE(detailsLine.text(),
             narrow ? QStringLiteral("Baroque · Track 2 of 2") : QStringLiteral("2005 · Baroque · Track 2 of 2"));
    QVERIFY(allLinesStill(panel));

    // Repeat-One: the same track again, its metadata told again, fades
    // nothing.
    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Second"));
    bridge->setProperty("currentTrackNumber", 2);
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(allLinesStill(panel));
}

// Another album: every line that differs cross-fades, on one curve with
// the cover over longDuration; the artist both share stays still.
void UiMotionTest::nowPlayingCrossFadesCoverAndTextForAnotherAlbum() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Second"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album A"));
    bridge->setProperty("currentTrackYear", 2005);
    bridge->setProperty("currentTrackNumber", 2);
    bridge->setProperty("currentTrackCoverPath", covers.first);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    QTRY_VERIFY(panelLine(panel, QStringLiteral("ArtistAlbum")).line()->width() > 0);

    bridge->setProperty("currentTrackPath", kTrackB1);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Third"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album B"));
    bridge->setProperty("currentTrackYear", 2007);
    bridge->setProperty("currentTrackNumber", 1);
    bridge->setProperty("currentTrackCoverPath", covers.second);

    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QCOMPARE(easingOf(handover), QEasingCurve::OutCubic);
    QQuickItem *outgoingCover = itemNamed(panel, QStringLiteral("albumArtOutgoing"));
    QVERIFY(outgoingCover->isVisible());
    // One curve: the cover and every line going out are at the same point.
    const auto oneCurve = [&]() {
        for (const QString &name : kWideLines) {
            const PanelLine line = panelLine(panel, name);
            if (!line.fades() || line.outgoingOpacity() != outgoingCover->opacity()) {
                return false;
            }
        }
        return true;
    };
    QVERIFY(oneCurve());
    // Half-way, still on the one curve.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.5);
    QCOMPARE(outgoingCover->opacity(), 0.5);
    QVERIFY(oneCurve());
    QCOMPARE(textOf(panel, "nowPlayingOutgoingTitle"), QStringLiteral("Second"));
    QCOMPARE(textOf(panel, "nowPlayingOutgoingArtistAlbum"), QStringLiteral("Artist A — Album A"));
    QCOMPARE(textOf(panel, "nowPlayingOutgoingDetails"), QStringLiteral("2005 · Track 2"));
    QCOMPARE(panelLine(panel, QStringLiteral("Title")).stillWidth(), 0.0);
    QCOMPARE(panelLine(panel, QStringLiteral("Details")).stillWidth(), 0.0);
    QVERIFY(keepsStill(panelLine(panel, QStringLiteral("ArtistAlbum")), QStringLiteral("Artist A —")));
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QVERIFY(!outgoingCover->isVisible());
    QVERIFY(allLinesStill(panel));
}

// A line of the track change that was still and comes to differ while the
// others fade (its metadata comes late) joins the fade: the line before
// fades out from full opacity, the lines fading go on from where they are,
// and nothing jumps.
void UiMotionTest::nowPlayingLineThatComesToDifferJoinsTheFade() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"));
    showTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"));
    QVERIFY(handover->property("running").toBool());
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    const PanelLine artistAlbum = panelLine(panel, QStringLiteral("ArtistAlbum"));
    // Part of the way.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.45);
    QCOMPARE(title.outgoingOpacity(), 0.55);
    QVERIFY(lineStill(panel, QStringLiteral("ArtistAlbum")));
    const double titleBefore = title.outgoingOpacity();

    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist B"));
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->shortDuration());
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    // At once: the line before shows whole, as the line did; the title
    // where it was.
    QVERIFY(artistAlbum.fades());
    QCOMPARE(artistAlbum.outgoingOpacity(), 1.0);
    QCOMPARE(artistAlbum.incomingOpacity(), 0.0);
    QCOMPARE(artistAlbum.stillWidth(), 0.0);
    QCOMPARE(artistAlbum.outgoingText(), QStringLiteral("Artist A — Album A"));
    QCOMPARE(artistAlbum.text(), QStringLiteral("Artist B — Album A"));
    QCOMPARE(title.outgoingOpacity(), titleBefore);
    QCOMPARE(title.outgoingText(), QStringLiteral("First"));

    // Then each goes one way only, on the one curve, to the playing track.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    double titleOut = titleBefore;
    double lineOut = 1.0;
    for (const double at : {0.25, 0.5, 0.9}) {
        panel->setProperty("handover", at);
        const double nextTitle = title.outgoingOpacity();
        const double nextLine = artistAlbum.outgoingOpacity();
        QVERIFY(nextTitle < titleOut);
        QVERIFY(nextLine < lineOut);
        QCOMPARE(nextTitle, titleBefore * (1.0 - at));
        QCOMPARE(nextLine, 1.0 - at);
        QCOMPARE(artistAlbum.incomingOpacity(), at);
        titleOut = nextTitle;
        lineOut = nextLine;
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(allLinesStill(panel));
}

void UiMotionTest::nowPlayingStillFieldThatChangesLateFadesFromWhole_data() {
    QTest::addColumn<QString>("genre");
    QTest::newRow("as long") << QStringLiteral("Jazz");
    QTest::newRow("much longer") << QStringLiteral("Progressive Electronic Ambient");
}

// A field that stayed still while the rest of its line fades, and that the
// playing track's metadata changes later, fades out from full opacity, as
// it showed: it does not take on the rest's progress, and nothing jumps.
// The playing track's text fades in as one past what both still share, so
// none of its glyphs is cut where the line before's parts are, however
// long its new field.
void UiMotionTest::nowPlayingStillFieldThatChangesLateFadesFromWhole() {
    QFETCH(QString, genre);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setShortDuration(1500);
    bridge->setProperty("currentTrackYear", 2005);
    bridge->setProperty("currentTrackGenre", QStringLiteral("Rock"));
    bridge->setProperty("currentTrackNumber", 1);
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"));
    const PanelLine details = panelLine(panel, QStringLiteral("Details"));
    QTRY_VERIFY(details.line()->width() > 0);

    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Second"));
    bridge->setProperty("currentTrackNumber", 2);
    QVERIFY(details.fades());
    QVERIFY(keepsStill(details, QStringLiteral("2005 · Rock ·")));
    // Half-way.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.5);
    const QFontMetricsF metrics(details.font());
    const double inGenre = metrics.horizontalAdvance(QStringLiteral("2005 · R"));
    const double inNumber = metrics.horizontalAdvance(QStringLiteral("2005 · Rock · Tr"));
    QCOMPARE(details.opacitiesAt(inGenre), std::make_pair(1.0, 1.0));
    QCOMPARE(details.opacitiesAt(inNumber), std::make_pair(0.5, 0.5));
    QCOMPARE(details.opacitiesAt(metrics.horizontalAdvance(QStringLiteral("20"))), std::make_pair(1.0, 1.0));

    // The file's genre replaces the queue's.
    bridge->setProperty("currentTrackGenre", genre);
    QCOMPARE(details.text(), QStringLiteral("2005 · %1 · Track 2").arg(genre));
    QCOMPARE(details.outgoingText(), QStringLiteral("2005 · Rock · Track 1"));
    // At once: the genre before shows whole, as it did, and fades out from
    // there; the number before, and the playing track's text, go on from
    // where they were.
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(details.opacitiesAt(inGenre), std::make_pair(1.0, 0.5));
    QCOMPARE(details.opacitiesAt(inNumber), std::make_pair(0.5, 0.5));
    QCOMPARE(details.opacitiesAt(metrics.horizontalAdvance(QStringLiteral("20"))), std::make_pair(1.0, 1.0));
    // The playing track's text is cut only where both share a gap, after
    // "2005 ·", wherever its own fields are; the line before's parts only
    // in its own gaps.
    QVERIFY(keepsStill(details, QStringLiteral("2005 ·")));
    QCOMPARE(itemsNamed(details.line(), QStringLiteral("nowPlayingIncomingDetails")).size(), 1);
    QCOMPARE(details.incoming()->x(), details.stillWidth());
    QCOMPARE(details.incoming()->x() + details.incoming()->width(), details.line()->width());
    const QList<double> edges = details.innerEdges();
    QCOMPARE(edges.size(), 1);
    QVERIFY(edges.first() > metrics.horizontalAdvance(QStringLiteral("2005 · Rock ·")));
    QVERIFY(edges.first() < metrics.horizontalAdvance(QStringLiteral("2005 · Rock · ")));

    // Then each goes on, on the one curve.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.25, 0.6}) {
        panel->setProperty("handover", at);
        const double level = 1.0 - at;
        QCOMPARE(details.opacitiesAt(inGenre), std::make_pair(level, 1.0 - 0.5 * level));
        QCOMPARE(details.opacitiesAt(inNumber), std::make_pair(0.5 * level, 1.0 - 0.5 * level));
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(allLinesStill(panel));
}

// A line with right-to-left text is not laid out from the left: none of it
// stays still, and the whole of it cross-fades, as one.
void UiMotionTest::nowPlayingRightToLeftLineFadesWhole() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setLongDuration(1500);
    // An artist named in Hebrew, with two albums of different lengths.
    bridge->setProperty("currentTrackArtist", QStringLiteral("להקת הים"));
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("שירים"));
    const PanelLine artistAlbum = panelLine(panel, QStringLiteral("ArtistAlbum"));
    QTRY_VERIFY(artistAlbum.line()->width() > 0);

    showTrack(bridge, kTrackB1, QStringLiteral("Third"), QStringLiteral("שירים ארוכים מן הים"));
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(artistAlbum.outgoingText(), QStringLiteral("להקת הים — שירים"));
    QCOMPARE(artistAlbum.text(), QStringLiteral("להקת הים — שירים ארוכים מן הים"));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.0, 0.3, 0.7}) {
        panel->setProperty("handover", at);
        QCOMPARE(artistAlbum.stillWidth(), 0.0);
        const QList<QQuickItem *> parts = artistAlbum.parts();
        QCOMPARE(parts.size(), 1);
        QCOMPARE(parts.first()->x(), 0.0);
        QCOMPARE(parts.first()->width(), artistAlbum.line()->width());
        QCOMPARE(parts.first()->opacity(), 1.0 - at);
        QCOMPARE(artistAlbum.incoming()->x(), 0.0);
        QCOMPARE(artistAlbum.incoming()->width(), artistAlbum.line()->width());
        QCOMPARE(artistAlbum.incomingOpacity(), at);
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(allLinesStill(panel));
}

// A line that comes to have right-to-left text while it fades is not laid
// out from the left any more: none of it stays still, and the playing
// track's text fades in as one, from the opacity it had; the line before's
// parts go on, the start that showed whole fading from there.
void UiMotionTest::nowPlayingLineThatTurnsRightToLeftFadesInWhole() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"));
    const PanelLine artistAlbum = panelLine(panel, QStringLiteral("ArtistAlbum"));
    QTRY_VERIFY(artistAlbum.line()->width() > 0);
    showTrack(bridge, kTrackB1, QStringLiteral("Third"), QStringLiteral("Album Bee"));
    QVERIFY(keepsStill(artistAlbum, QStringLiteral("Artist A —")));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.5);
    QCOMPARE(artistAlbum.incomingOpacity(), 0.5);

    // The file names the artist in Hebrew.
    bridge->setProperty("currentTrackArtist", QStringLiteral("להקת הים"));
    QCOMPARE(artistAlbum.text(), QStringLiteral("להקת הים — Album Bee"));
    QCOMPARE(artistAlbum.outgoingText(), QStringLiteral("Artist A — Album A"));
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(artistAlbum.stillWidth(), 0.0);
    const QList<QQuickItem *> incoming =
        itemsNamed(artistAlbum.line(), QStringLiteral("nowPlayingIncomingArtistAlbum"));
    QCOMPARE(incoming.size(), 1);
    QCOMPARE(incoming.first()->x(), 0.0);
    QCOMPARE(incoming.first()->width(), artistAlbum.line()->width());
    // No higher than it showed past the still start.
    QCOMPARE(artistAlbum.incomingOpacity(), 0.5);
    const QFontMetricsF metrics(artistAlbum.font());
    const double inArtist = metrics.horizontalAdvance(QStringLiteral("Art"));
    const double inAlbum = metrics.horizontalAdvance(QStringLiteral("Artist A — Al"));
    QCOMPARE(artistAlbum.opacitiesAt(inArtist), std::make_pair(1.0, 0.5));
    QCOMPARE(artistAlbum.opacitiesAt(inAlbum), std::make_pair(0.5, 0.5));

    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.5);
    QCOMPARE(artistAlbum.opacitiesAt(inArtist), std::make_pair(0.5, 0.75));
    QCOMPARE(artistAlbum.opacitiesAt(inAlbum), std::make_pair(0.25, 0.75));
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(allLinesStill(panel));
}

// A line whose parts fade on their own, elided ever shorter: no edge, of
// the still start or between the parts, falls where the ellipsis is, or
// past the gap it stands for.
void UiMotionTest::nowPlayingElidedLineKeepsEdgesOffTheEllipsis() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    const QString genre = QStringLiteral("Contemporary Classical Chamber Music");
    bridge->setProperty("currentTrackYear", 2005);
    bridge->setProperty("currentTrackGenre", genre);
    bridge->setProperty("currentTrackNumber", 1);
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"));
    const PanelLine details = panelLine(panel, QStringLiteral("Details"));
    QTRY_VERIFY(details.line()->width() > 0);
    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Second"));
    bridge->setProperty("currentTrackNumber", 2);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.5);
    // A late genre: two parts, split after the genre before.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Jazz"));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    QCOMPARE(details.parts().size(), 2);
    const QString before = details.outgoingText();
    const QString shared = QStringLiteral("2005 ·");
    const QString split = QStringLiteral("2005 · ") + genre + QStringLiteral(" ·");
    const QFontMetricsF metrics(details.font());
    const double splitEdge = (metrics.horizontalAdvance(split) + metrics.horizontalAdvance(split + QLatin1Char(' '))) / 2;
    // What of `text` shows before the ellipsis at `width`.
    const auto shownUpTo = [&](const QString &text, double width) {
        QString shown = metrics.elidedText(text, Qt::ElideRight, width);
        if (shown != text) {
            shown.chop(1);
        }
        return metrics.horizontalAdvance(shown);
    };

    int moved = 0;
    int kept = 0;
    for (int width = static_cast<int>(std::ceil(metrics.horizontalAdvance(before))) + 4; width > 40; --width) {
        // The sidebar narrowing as the window does.
        details.line()->setWidth(width);
        const QString where = QStringLiteral(" at %1 px").arg(width);
        for (const double edge : details.innerEdges()) {
            if (edge < width) {
                QVERIFY2(edge <= shownUpTo(before, width), qPrintable(QStringLiteral("part edge %1").arg(edge) + where));
                ++kept;
            } else if (splitEdge < width) {
                ++moved;
            }
        }
        const double still = details.stillWidth();
        if (still > 0 && still < width) {
            QVERIFY2(still <= shownUpTo(before, width) && still <= shownUpTo(details.text(), width),
                     qPrintable(QStringLiteral("still edge %1").arg(still) + where));
            QVERIFY(keepsStill(details, shared));
        }
    }
    // Both kinds came up: the edge in a gap that shows, and an edge whose
    // gap the ellipsis took, at the end.
    QVERIFY(kept > 0);
    QVERIFY(moved > 0);
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
}

// A line cut off within the fields the two share looks the same for both
// tracks: it stays still.
void UiMotionTest::nowPlayingLineCutOffWithinWhatItSharesStaysStill() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    const QString genre = QStringLiteral(
        "A Genre Named At Such Length That No Sidebar Will Ever Show All Of It, Not Even The Widest One");
    bridge->setProperty("currentTrackGenre", genre);
    bridge->setProperty("currentTrackNumber", 1);
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"));
    const PanelLine details = panelLine(panel, QStringLiteral("Details"));
    QTRY_VERIFY(details.stillText()->property("truncated").toBool());

    bridge->setProperty("currentTrackPath", kTrackA2);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Second"));
    bridge->setProperty("currentTrackNumber", 2);
    QVERIFY(handover->property("running").toBool());
    QVERIFY(panelLine(panel, QStringLiteral("Title")).fades());
    QCOMPARE(details.outgoingText(), genre + QStringLiteral(" · Track 1"));
    QVERIFY(lineStill(panel, QStringLiteral("Details")));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QVERIFY(allLinesStill(panel));
}

void UiMotionTest::nowPlayingSharedStartIsWholeFields_data() {
    QTest::addColumn<QString>("before");
    QTest::addColumn<QString>("after");
    QTest::addColumn<QString>("separator");
    QTest::addColumn<QString>("shared");
    QTest::newRow("the last field") << "2005 · Rock · Track 1" << "2005 · Rock · Track 2" << " · "
                                    << "2005 · Rock ·";
    QTest::newRow("a field more") << "2005 · Rock" << "2005 · Rock · Track 2" << " · " << "2005 · Rock";
    QTest::newRow("a field fewer") << "2005 · Rock · Track 1" << "2005 · Rock" << " · " << "2005 · Rock";
    QTest::newRow("the first field") << "1999 · Rock" << "2005 · Rock" << " · " << "";
    QTest::newRow("not part of a field") << "Track 1" << "Track 12" << " · " << "";
    QTest::newRow("the artist") << "Artist — Album A" << "Artist — Album B" << " — " << "Artist —";
    QTest::newRow("the same") << "2005 · Rock" << "2005 · Rock" << " · " << "2005 · Rock";
    QTest::newRow("one field, the same") << "First" << "First" << "" << "First";
    QTest::newRow("one field, another") << "First" << "Firsts" << "" << "";
    QTest::newRow("right to left") << "להקה — שירים" << "להקה — שירים נוספים" << " — " << "";
    QTest::newRow("right to left later") << "Artist — שירים" << "Artist — Album" << " — " << "";
    QTest::newRow("right to left, the same") << "להקה" << "להקה" << "" << "";
}

// What stays still of a line that changes: its whole leading fields that
// are the same, never part of one.
void UiMotionTest::nowPlayingSharedStartIsWholeFields() {
    QFETCH(QString, before);
    QFETCH(QString, after);
    QFETCH(QString, separator);
    QFETCH(QString, shared);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QVariant result;
    QVERIFY(QMetaObject::invokeMethod(named(window, "nowPlayingPanel"), "_sharedStart",
                                      Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, before),
                                      Q_ARG(QVariant, after), Q_ARG(QVariant, separator)));
    QCOMPARE(result.toString(), shared);
}

// Playback cleared and a track loaded while a fade runs: nothing of the
// tracks before comes back over the new one.
void UiMotionTest::nowPlayingClearedAndReloadedMidFadeShowsNoStaleTrack() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    showTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), covers.second);
    QVERIFY(handover->property("running").toBool());

    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    bridge->setProperty("currentTrackPath", QString());
    bridge->setProperty("currentTrackTitle", QString());
    bridge->setProperty("currentTrackAlbum", QString());
    bridge->setProperty("currentTrackCoverPath", QString());
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("empty"));
    QVERIFY(!handover->property("running").toBool());

    showTrack(bridge, kTrackA2, QStringLiteral("Third"), QStringLiteral("Album C"), covers.first);
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(!anyLineFades(panel));
    QVERIFY(!shownIn(panel, "albumArtOutgoing"));
    QCOMPARE(textOf(panel, "nowPlayingTitle"), QStringLiteral("Third"));
    QVERIFY(allLinesStill(panel));
    // And none comes later either.
    QTest::qWait(300);
    QVERIFY(!anyLineFades(panel));
    QVERIFY(!shownIn(panel, "albumArtOutgoing"));
}

// The next track of the album, whose own cover comes during the fade of the
// lines: the cover joins them, on one long curve.
void UiMotionTest::nowPlayingLateCoverJoinsTheFadeOfTheLines() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    showTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), covers.first);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->shortDuration());
    QVERIFY(!shownIn(panel, "albumArtOutgoing"));

    bridge->setProperty("currentTrackCoverPath", covers.second);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QQuickItem *outgoingCover = itemNamed(panel, QStringLiteral("albumArtOutgoing"));
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    QVERIFY(outgoingCover->isVisible());
    QVERIFY(title.fades());
    QCOMPARE(textOf(panel, "nowPlayingOutgoingTitle"), QStringLiteral("First"));
    QCOMPARE(outgoingCover->opacity(), title.outgoingOpacity());
    // Half-way, still together.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.5);
    QCOMPARE(outgoingCover->opacity(), 0.5);
    QCOMPARE(outgoingCover->opacity(), title.outgoingOpacity());
    // The album line is the same: it stays still all along.
    QVERIFY(lineStill(panel, QStringLiteral("ArtistAlbum")));
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QVERIFY(!outgoingCover->isVisible());
    QVERIFY(allLinesStill(panel));
}

void UiMotionTest::nowPlayingRapidChangesFadeFromWhatShows_data() {
    QTest::addColumn<double>("at");
    // C comes while A, fading out, still shows more than B.
    QTest::newRow("early") << 0.3;
    // C comes once B shows more than A.
    QTest::newRow("late") << 0.7;
}

// A → B → C within one fade: the next fade starts from what shows, A's
// layers and B's as far as each got, never from a track that was gone, and
// ends on C alone.
void UiMotionTest::nowPlayingRapidChangesFadeFromWhatShows() {
    QFETCH(double, at);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    QTRY_VERIFY(title.line()->width() > 0);
    showTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), covers.second);
    QVERIFY(handover->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", at);
    const PanelLook before = panelLook(panel, kWideLines);

    showTrack(bridge, kTrackA2, QStringLiteral("Third"), QStringLiteral("Album C"), covers.first);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    const QString change = lookChange(before, panelLook(panel, kWideLines), kWideLines, true, true);
    QVERIFY2(change.isEmpty(), qPrintable(change));
    QCOMPARE(title.text(), QStringLiteral("Third"));
    // A's title at the bottom, then B's, each where it was; C's over them.
    const QList<QQuickItem *> layers = layersOf(title);
    QCOMPARE(layers.size(), 2);
    QCOMPARE(partText(title, partsOf(title, layers.at(0)).first()), QStringLiteral("First"));
    QCOMPARE(partsOf(title, layers.at(0)).first()->opacity(), 1.0 - at);
    QCOMPARE(partText(title, partsOf(title, layers.at(1)).first()), QStringLiteral("Second"));
    QCOMPARE(partsOf(title, layers.at(1)).first()->opacity(), at);
    QCOMPARE(title.incomingOpacity(), 0.0);
    // B's cover whole, as it showed, under A's.
    const QList<std::pair<QString, double>> cover = coverLook(panel);
    QCOMPARE(cover.size(), 2);
    QCOMPARE(cover.at(0).first, covers.second);
    QCOMPARE(cover.at(0).second, 1.0);
    QCOMPARE(cover.at(1).first, covers.first);
    QCOMPARE(cover.at(1).second, 1.0 - at);

    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(!anyLineFades(panel));
    QVERIFY(panelIdle(panel, kWideLines));
}

// A cover that comes after the fade of the lines is done fades in alone,
// over longDuration; the lines stay as they are.
void UiMotionTest::nowPlayingCoverAfterTheFadeFadesAlone() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    showTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), covers.first);
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));

    bridge->setProperty("currentTrackCoverPath", covers.second);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QCOMPARE(easingOf(handover), QEasingCurve::OutCubic);
    QQuickItem *outgoingCover = itemNamed(panel, QStringLiteral("albumArtOutgoing"));
    QVERIFY(outgoingCover->isVisible());
    QCOMPARE(outgoingCover->childItems().first()->property("source").toUrl().toString(), covers.first);
    // The lines did not change: they neither fade nor dim.
    QVERIFY(!anyLineFades(panel));
    QVERIFY(allLinesStill(panel));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QVERIFY(!outgoingCover->isVisible());
}

void UiMotionTest::nowPlayingLeavesNothingPlayingWithoutAFade_data() {
    QTest::addColumn<bool>("coverFirst");
    // BridgeClient's order: the path, then the metadata with the cover.
    QTest::newRow("path first") << false;
    QTest::newRow("cover first") << true;
}

// The first track after "Nothing playing" shows its cover and lines
// together, at once, whatever order they come in; a cover that changes
// after that is a late one and fades.
void UiMotionTest::nowPlayingLeavesNothingPlayingWithoutAFade() {
    QFETCH(bool, coverFirst);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("empty"));

    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first,
              coverFirst ? Order::CoverFirst : Order::PathFirst);
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(!shownIn(panel, "albumArtOutgoing"));
    QVERIFY(!anyLineFades(panel));
    QVERIFY(allLinesStill(panel));

    bridge->setProperty("currentTrackCoverPath", covers.second);
    QVERIFY(handover->property("running").toBool());
    QVERIFY(shownIn(panel, "albumArtOutgoing"));
    QVERIFY(!anyLineFades(panel));
}

void UiMotionTest::nowPlayingLateCoverKeepsEveryLayerContinuous_data() {
    QTest::addColumn<double>("linesLeft");
    // The cover comes while the lines before still show more.
    QTest::newRow("before half") << 0.7;
    // The cover comes once the playing track's lines show more.
    QTest::newRow("after half") << 0.3;
}

// The next track's own cover comes while its lines fade in: no layer jumps.
// The lines go on from the opacity they have onto the cover's long curve;
// the cover before, which showed whole, fades out from 1.
void UiMotionTest::nowPlayingLateCoverKeepsEveryLayerContinuous() {
    QFETCH(double, linesLeft);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QVERIFY(units->longDuration() != units->shortDuration());
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    showTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), covers.first);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->shortDuration());
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    QQuickItem *outgoingCover = itemNamed(panel, QStringLiteral("albumArtOutgoing"));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 1.0 - linesLeft);
    QCOMPARE(title.outgoingOpacity(), linesLeft);
    const double outgoingBefore = title.outgoingOpacity();
    const double liveBefore = title.incomingOpacity();
    QVERIFY(!outgoingCover->isVisible());

    bridge->setProperty("currentTrackCoverPath", covers.second);
    // At once: every layer where it was; the cover before shows whole over
    // the new one, as it did before.
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QVERIFY(title.fades());
    QCOMPARE(textOf(panel, "nowPlayingOutgoingTitle"), QStringLiteral("First"));
    QCOMPARE(title.outgoingOpacity(), outgoingBefore);
    QCOMPARE(title.incomingOpacity(), liveBefore);
    QVERIFY(outgoingCover->isVisible());
    QCOMPARE(outgoingCover->opacity(), 1.0);
    QCOMPARE(outgoingCover->childItems().first()->property("source").toUrl().toString(), covers.first);

    // Then each goes one way only, on the one curve, to the playing track.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    double lines = outgoingBefore;
    double cover = 1.0;
    for (const double at : {0.25, 0.5, 0.9}) {
        panel->setProperty("handover", at);
        const double nextLines = title.outgoingOpacity();
        QVERIFY(outgoingCover->isVisible());
        const double nextCover = outgoingCover->opacity();
        QVERIFY(nextLines < lines);
        QVERIFY(nextCover < cover);
        QCOMPARE(nextLines, outgoingBefore * (1.0 - at));
        QCOMPARE(nextCover, 1.0 - at);
        QCOMPARE(title.incomingOpacity() + nextLines, 1.0);
        QVERIFY(lineStill(panel, QStringLiteral("ArtistAlbum")));
        lines = nextLines;
        cover = nextCover;
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(!outgoingCover->isVisible());
    QVERIFY(allLinesStill(panel));
}

void UiMotionTest::nowPlayingThirdTrackGoesOnFromEveryLayer_data() {
    QTest::addColumn<double>("at");
    // C comes while A, fading out, still shows more than B.
    QTest::newRow("before half") << 0.3;
    // C comes once B shows more than A.
    QTest::newRow("after half") << 0.7;
}

// A → B, B's own cover late, then C during that joined fade (#463): C's fade
// starts from what shows. B's lines, part faded in, go out as layers of
// their own over A's, each part from where it got to and cut in B's own
// gaps; B's cover goes out whole, as it showed, under A's. No layer jumps,
// each then goes one way only, and the fade ends on C alone.
void UiMotionTest::nowPlayingThirdTrackGoesOnFromEveryLayer() {
    QFETCH(double, at);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setShortDuration(1000);
    units->setLongDuration(1500);
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    bridge->setProperty("currentTrackGenre", QStringLiteral("Rock"));
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, covers.all.at(0));
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    const PanelLine artistAlbum = panelLine(panel, QStringLiteral("ArtistAlbum"));
    const PanelLine details = panelLine(panel, QStringLiteral("Details"));
    QTRY_VERIFY(details.line()->width() > 0);
    QVERIFY(panelIdle(panel, kWideLines));

    // B, the next track of the album, with the album's cover for now.
    tellTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), 2005, 2, covers.all.at(0));
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), 1000);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.3);
    // B's own cover, late: it joins the fade.
    bridge->setProperty("currentTrackCoverPath", covers.all.at(1));
    QCOMPARE(handover->property("duration").toInt(), 1500);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", at);
    // A's lines, from 0.7 when the cover came, along the long curve.
    const double a = 0.7 * (1.0 - at);
    QCOMPARE(title.outgoingOpacity(), a);
    QCOMPARE(title.incomingOpacity(), 1.0 - a);
    QVERIFY(keepsStill(details, QStringLiteral("2005 · Rock ·")));
    QVERIFY(lineStill(panel, QStringLiteral("ArtistAlbum")));
    const PanelLook before = panelLook(panel, kWideLines);
    QCOMPARE(before.cover.size(), 2);

    // C, of another album, in BridgeClient's order.
    tellTrack(bridge, kTrackB1, QStringLiteral("Third"), QStringLiteral("Album C"), 2007, 3, covers.all.at(2));
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(handover->property("duration").toInt(), 1500);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    // At once: every layer where it was, drawing what it drew.
    const QString change = lookChange(before, panelLook(panel, kWideLines), kWideLines, true, true);
    QVERIFY2(change.isEmpty(), qPrintable(change));

    // The title: A's at the bottom, then B's as far as it came in, then
    // C's, from 0.
    QCOMPARE(title.text(), QStringLiteral("Third"));
    const QList<QQuickItem *> titleLayers = layersOf(title);
    QCOMPARE(titleLayers.size(), 2);
    QCOMPARE(partText(title, partsOf(title, titleLayers.at(0)).first()), QStringLiteral("First"));
    QCOMPARE(partText(title, partsOf(title, titleLayers.at(1)).first()), QStringLiteral("Second"));
    QCOMPARE(title.incomingOpacity(), 0.0);
    // The details share no start with C's: B's go out whole up to the gap
    // after the start A's and B's shared, measured in B's own text, and past
    // it as far as they came in; A's from that gap on.
    QCOMPARE(details.stillWidth(), 0.0);
    const QList<QQuickItem *> detailsLayers = layersOf(details);
    QCOMPARE(detailsLayers.size(), 2);
    const QList<QQuickItem *> partsA = partsOf(details, detailsLayers.at(0));
    const QList<QQuickItem *> partsB = partsOf(details, detailsLayers.at(1));
    QCOMPARE(partsA.size(), 1);
    QCOMPARE(partsB.size(), 2);
    QCOMPARE(partText(details, partsA.first()), QStringLiteral("2005 · Rock · Track 1"));
    QCOMPARE(partText(details, partsB.first()), QStringLiteral("2005 · Rock · Track 2"));
    const QFontMetricsF metrics(details.font());
    const QString shared = QStringLiteral("2005 · Rock ·");
    QCOMPARE(partsB.at(0)->x(), 0.0);
    QCOMPARE(partsB.at(0)->opacity(), 1.0);
    QVERIFY(partsB.at(1)->x() > metrics.horizontalAdvance(shared));
    QVERIFY(partsB.at(1)->x() < metrics.horizontalAdvance(shared + QLatin1Char(' ')));
    QCOMPARE(partsB.at(1)->opacity(), 1.0 - a);
    QCOMPARE(partsA.first()->x(), partsB.at(1)->x());
    QCOMPARE(partsA.first()->opacity(), a);
    // The album line was the same for A and B: B's goes out from full
    // opacity, past the artist they share with C's.
    QCOMPARE(layersOf(artistAlbum).size(), 1);
    QCOMPARE(artistAlbum.outgoingOpacity(), 1.0);
    QVERIFY(keepsStill(artistAlbum, QStringLiteral("Artist A —")));
    // The covers: B's whole over C's, A's over it where it was.
    QList<std::pair<QString, double>> cover = coverLook(panel);
    QCOMPARE(cover.size(), 2);
    QCOMPARE(cover.at(0).first, covers.all.at(1));
    QCOMPARE(cover.at(1).first, covers.all.at(0));
    QCOMPARE(cover.at(1).second, 1.0 - at);

    // Then each goes one way only, on the one curve, to C.
    for (const double next : {0.25, 0.5, 0.9}) {
        panel->setProperty("handover", next);
        const double level = 1.0 - next;
        QCOMPARE(partsOf(title, titleLayers.at(0)).first()->opacity(), a * level);
        QCOMPARE(partsOf(title, titleLayers.at(1)).first()->opacity(), (1.0 - a) * level);
        QCOMPARE(title.incomingOpacity(), next);
        QCOMPARE(partsB.at(0)->opacity(), level);
        QCOMPARE(partsB.at(1)->opacity(), (1.0 - a) * level);
        QCOMPARE(partsA.first()->opacity(), a * level);
        QCOMPARE(details.incomingOpacity(), next);
        QCOMPARE(artistAlbum.outgoingOpacity(), level);
        cover = coverLook(panel);
        QCOMPARE(cover.size(), 3);
        QCOMPARE(cover.at(0).first, covers.all.at(2));
        QCOMPARE(cover.at(1).first, covers.all.at(1));
        QCOMPARE(cover.at(1).second, level);
        QCOMPARE(cover.at(2).second, (1.0 - at) * level);
    }
    panel->setProperty("handover", 0.0);
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    // C alone: one layer of each line, and one cover, at full opacity.
    QVERIFY(panelIdle(panel, kWideLines));
    QCOMPARE(title.text(), QStringLiteral("Third"));
    cover = coverLook(panel);
    QCOMPARE(cover.size(), 1);
    QCOMPARE(cover.first().first, covers.all.at(2));
}

// Skipping through five tracks, each during the fade before (or, for the
// third, at once): every change starts from what shows, and no layer jumps.
// A layer that is at 0 when the next track comes is dropped. A line, or the
// cover, fades out no more than two layers at once; past that, the playing
// track's own layer takes the next track's text, or cover, at the opacity
// it has. The fade ends on the last track alone.
void UiMotionTest::nowPlayingSkippingThroughTracksKeepsEveryLayer() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setLongDuration(1500);
    QCOMPARE(panel->property("maxOutgoingLayers").toInt(), 2);
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    struct Track {
        QString path;
        QString title;
        QString album;
        int year;
        // Where the fade before stands when it comes, or < 0 for at once.
        double at;
        // Whether it can add a layer to every line, and to the cover.
        bool linesLayer;
        bool coverLayer;
    };
    const QList<Track> tracks{
        {kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2001, 0.0, true, true},
        {kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2002, 0.0, true, true},
        // At once: B's lines, at 0, are dropped.
        {QStringLiteral("/music/Artist A/Album C/01.flac"), QStringLiteral("Third"), QStringLiteral("Album C"), 2003,
         -1.0, true, true},
        // A's and C's lines; the covers are two already.
        {QStringLiteral("/music/Artist A/Album D/01.flac"), QStringLiteral("Fourth"), QStringLiteral("Album D"), 2004,
         0.4, true, false},
        {QStringLiteral("/music/Artist A/Album E/01.flac"), QStringLiteral("Fifth"), QStringLiteral("Album E"), 2005,
         0.4, false, false},
    };
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    for (qsizetype index = 0; index < tracks.size(); ++index) {
        const Track &track = tracks.at(index);
        if (index > 1 && track.at >= 0) {
            QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
            panel->setProperty("handover", track.at);
        }
        const PanelLook before = index > 0 ? panelLook(panel, kWideLines) : PanelLook{};
        tellTrack(bridge, track.path, track.title, track.album, track.year, 1, covers.all.at(index));
        if (index == 0) {
            QTRY_VERIFY(title.line()->width() > 0);
            QVERIFY(panelIdle(panel, kWideLines));
            continue;
        }
        QVERIFY(handover->property("running").toBool());
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        QCOMPARE(panel->property("handover").toDouble(), 0.0);
        const QString change =
            lookChange(before, panelLook(panel, kWideLines), kWideLines, track.linesLayer, track.coverLayer);
        QVERIFY2(change.isEmpty(), qPrintable(track.title + QStringLiteral(": ") + change));
        QCOMPARE(title.text(), track.title);
        for (const QString &name : kWideLines) {
            QVERIFY2(layersOf(panelLine(panel, name)).size() <= 2, qPrintable(track.title + QStringLiteral(" ") + name));
        }
        // The title's lines before, oldest first.
        QStringList outgoing;
        for (QQuickItem *layer : layersOf(title)) {
            outgoing.push_back(partText(title, partsOf(title, layer).first()));
        }
        const QStringList expected = index == 1 ? QStringList{QStringLiteral("First")}
            : index == 2                        ? QStringList{QStringLiteral("First")}
                                                : QStringList{QStringLiteral("First"), QStringLiteral("Third")};
        QCOMPARE(outgoing, expected);
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kWideLines));
    QCOMPARE(title.text(), QStringLiteral("Fifth"));
    const QList<std::pair<QString, double>> cover = coverLook(panel);
    QCOMPARE(cover.size(), 1);
    QCOMPARE(cover.first().first, covers.all.at(4));
}

// Repeat-One during a joined fade: the same track told again, as the
// backend tells it when it starts over, changes nothing and does not start
// the fade again.
void UiMotionTest::nowPlayingRepeatOneDuringAJoinedFadeChangesNothing() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, covers.all.at(0));
    QTRY_VERIFY(panelLine(panel, QStringLiteral("Title")).line()->width() > 0);
    tellTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), 2005, 2, covers.all.at(0));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.3);
    bridge->setProperty("currentTrackCoverPath", covers.all.at(1));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.5);
    const PanelLook before = panelLook(panel, kWideLines);

    tellTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), 2005, 2, covers.all.at(1));
    QCOMPARE(panel->property("handover").toDouble(), 0.5);
    QVERIFY(handover->property("paused").toBool());
    const QString change = lookChange(before, panelLook(panel, kWideLines), kWideLines, true, true);
    QVERIFY2(change.isEmpty(), qPrintable(change));
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kWideLines));
}

// With Plasma's animations off, A → B, B's late cover and C are each
// instant: nothing fades, and C alone shows.
void UiMotionTest::nowPlayingThirdTrackWithAnimationsOffIsInstant() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setShortDuration(0);
    units->setLongDuration(0);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, covers.all.at(0));
    QTRY_VERIFY(title.line()->width() > 0);
    tellTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), 2005, 2, covers.all.at(0));
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(panelIdle(panel, kWideLines));
    bridge->setProperty("currentTrackCoverPath", covers.all.at(1));
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(panelIdle(panel, kWideLines));
    tellTrack(bridge, kTrackB1, QStringLiteral("Third"), QStringLiteral("Album C"), 2007, 3, covers.all.at(2));
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(panelIdle(panel, kWideLines));
    QCOMPARE(title.text(), QStringLiteral("Third"));
    const QList<std::pair<QString, double>> cover = coverLook(panel);
    QCOMPARE(cover.size(), 1);
    QCOMPARE(cover.first().first, covers.all.at(2));
}

// A, with a cover, fades to B, without one, and C, of another album and
// without a cover either, comes during that fade: A's cover goes on fading
// over the placeholder, and no second placeholder goes out over the
// playing one, which would double its tint at once. The placeholder is a
// translucent tint: the look composes every layer over it.
void UiMotionTest::nowPlayingInterruptedFadeAddsNoPlaceholderOverItself() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, covers.first);
    QTRY_VERIFY(panelLine(panel, QStringLiteral("Title")).line()->width() > 0);
    tellTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2007, 1, QString());
    QVERIFY(handover->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.4);
    const QList<std::pair<QString, double>> fading{{QString(), 1.0}, {covers.first, 0.6}};
    QCOMPARE(coverLook(panel), fading);
    const PanelLook before = panelLook(panel, kWideLines);

    tellTrack(bridge, QStringLiteral("/music/Artist A/Album C/01.flac"), QStringLiteral("Third"),
              QStringLiteral("Album C"), 2009, 1, QString());
    QVERIFY(handover->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    const QString change = lookChange(before, panelLook(panel, kWideLines), kWideLines, true, true);
    QVERIFY2(change.isEmpty(), qPrintable(change));
    // One placeholder, the playing one, under A's cover.
    QCOMPARE(coverLook(panel), fading);
    QQuickItem *tile = itemNamed(panel, QStringLiteral("nowPlayingCover"));
    int shown = 0;
    for (QQuickItem *cover : itemsNamed(tile, QStringLiteral("albumArtOutgoing"))) {
        shown += cover->isVisible() ? 1 : 0;
    }
    QCOMPARE(shown, 1);

    panel->setProperty("handover", 0.5);
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{{QString(), 1.0}, {covers.first, 0.3}}));
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kWideLines));
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{{QString(), 1.0}}));
}

// Another album, neither with a cover: the placeholder stays as it is, with
// nothing over it, and the lines fade over longDuration all the same.
void UiMotionTest::nowPlayingAnotherAlbumWithoutCoversFadesItsLinesLong() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, QString());
    QTRY_VERIFY(panelLine(panel, QStringLiteral("Title")).line()->width() > 0);
    tellTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2007, 1, QString());
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QVERIFY(panelLine(panel, QStringLiteral("Title")).fades());
    QVERIFY(!shownIn(panel, "albumArtOutgoing"));
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{{QString(), 1.0}}));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kWideLines));
}

// A, with a square cover, fades to B, with a landscape one, and A comes
// back during that fade. B's cover goes out whole, as it showed, over A's
// own: around B's image, where the tile shows the colour behind it, A's
// cover must not show through, or A would jump there from its opacity to
// full. Read from the drawn pixels, above B's image and in it, on both
// sides; idle, the space around a landscape cover is the sidebar's colour.
void UiMotionTest::nowPlayingCoverOfAnotherShapeHidesWhatIsUnderIt() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    QQuickItem *tile = itemNamed(panel, QStringLiteral("nowPlayingCover"));
    const QColor view =
        qvariant_cast<QObject *>(panel->property("uiPalette"))->property("view").value<QColor>();
    QCOMPARE(view.alpha(), 255);
    // Whether every cover image that shows has loaded.
    const auto loaded = [&]() {
        QList<QQuickItem *> images{tile->childItems().first()};
        for (QQuickItem *cover : itemsNamed(tile, QStringLiteral("albumArtOutgoing"))) {
            if (cover->isVisible()) {
                images.push_back(cover->childItems().first());
            }
        }
        return std::all_of(images.begin(), images.end(), [](QQuickItem *image) {
            return image->property("source").toUrl().isEmpty() || image->property("status").toInt() == 1;
        });
    };
    // The colours drawn above the middle of the tile, where a landscape
    // cover leaves it bare, and at its middle.
    const auto drawn = [&]() {
        const QImage frame = window->grabWindow();
        const qreal ratio = frame.devicePixelRatio();
        const auto at = [&](double x, double y) {
            const QPointF point = tile->mapToScene(QPointF(x, y)) * ratio;
            return frame.pixelColor(point.toPoint());
        };
        return std::pair{at(tile->width() / 2, tile->height() * 0.1), at(tile->width() / 2, tile->height() / 2)};
    };
    const auto near = [](const QColor &left, const QColor &right) {
        return std::abs(left.red() - right.red()) <= 3 && std::abs(left.green() - right.green()) <= 3
            && std::abs(left.blue() - right.blue()) <= 3;
    };
    const auto mixed = [](const QColor &over, double opacity, const QColor &under) {
        return QColor::fromRgbF(over.redF() * opacity + under.redF() * (1 - opacity),
                                over.greenF() * opacity + under.greenF() * (1 - opacity),
                                over.blueF() * opacity + under.blueF() * (1 - opacity));
    };
    const QColor squareColor(200, 40, 40);
    const QColor wideColor(40, 160, 160);

    tellTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2007, 1, covers.landscape);
    QTRY_VERIFY(tile->width() > 0 && loaded());
    // Idle, around a landscape cover: the sidebar's colour, as before.
    QTRY_VERIFY(near(drawn().first, view) && near(drawn().second, wideColor));

    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, covers.first);
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kWideLines));
    tellTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2007, 1, covers.landscape);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.4);
    QTRY_VERIFY(loaded());
    // A at 0.6 over B, around B's image over the colour behind it.
    const QColor bare = mixed(squareColor, 0.6, view);
    const QColor inImage = mixed(squareColor, 0.6, wideColor);
    QTRY_VERIFY(near(drawn().first, bare) && near(drawn().second, inImage));

    // A again.
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, covers.first);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{{covers.landscape, 1.0}, {covers.first, 0.6}}));
    // B's cover shows at once, not its backdrop: the image cache has it, as
    // it showed.
    QVERIFY(loaded());
    const auto [above, middle] = drawn();
    QVERIFY2(near(above, bare), qPrintable(QStringLiteral("around B's image: %1, not %2").arg(above.name(), bare.name())));
    QVERIFY2(near(middle, inImage), qPrintable(QStringLiteral("in B's image: %1, not %2").arg(middle.name(), inImage.name())));

    // Half-way: B's cover at 0.5 over A's own, A's going out at 0.3 over it.
    panel->setProperty("handover", 0.5);
    const QColor overA = mixed(squareColor, 0.3, mixed(view, 0.5, squareColor));
    QTRY_VERIFY(near(drawn().first, overA));
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kWideLines));
    QTRY_VERIFY(near(drawn().first, squareColor) && near(drawn().second, squareColor));
}

// Expanding a row opens its children downward: the toggled row stays put,
// the rows below slide down from where they were over shortDuration with
// OutCubic. Collapsing slides them back up.
void UiMotionTest::treeToggleOpensDownwardAndKeepsTheRowPut() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(QTest::qWaitFor([&]() { return view->property("count").toInt() == 3; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    auto *open = qvariant_cast<QObject *>(view->property("openAnimation"));
    auto *close = qvariant_cast<QObject *>(view->property("closeAnimation"));
    auto *closeFade = qvariant_cast<QObject *>(view->property("closeFadeAnimation"));
    QVERIFY(open != nullptr && close != nullptr && closeFade != nullptr);
    for (QObject *animation : {open, close, closeFade}) {
        QCOMPARE(animation->property("duration").toInt(), units->shortDuration());
        QCOMPARE(easingOf(animation), QEasingCurve::OutCubic);
    }

    QQuickItem *albumA = libraryRowWithKey(window, kAlbumAKey);
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = view->property("rowHeight").toDouble();
    const double albumAY = albumA->y();
    QCOMPARE(albumB->y(), 2 * rowHeight);

    // The slide's time, in the test's steps.
    SteppedAnimationDriver driver;
    const int duration = units->shortDuration();
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    // Laid out at once: the children are in place, Album A has not moved,
    // and Album B starts where it was, over the children.
    QCOMPARE(view->property("count").toInt(), 5);
    QCOMPARE(albumA->y(), albumAY);
    QQuickItem *firstChild = libraryRowWithPath(window, kTrackA1);
    QVERIFY(firstChild != nullptr);
    QCOMPARE(firstChild->y(), 2 * rowHeight);
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QVERIFY(albumB->z() > firstChild->z());
    // Half-way, it is on its way down, still over the children.
    QVERIFY(driver.step(duration / 2));
    QVERIFY2(albumB->y() > 2 * rowHeight && albumB->y() < 4 * rowHeight, qPrintable(QString::number(albumB->y())));
    QVERIFY(albumB->z() > firstChild->z());
    QCOMPARE(albumA->y(), albumAY);
    QVERIFY(driver.step(duration));
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QCOMPARE(albumB->z(), 1.0);

    // Collapsing: Album B slides back up over the children as they fade.
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QCOMPARE(view->property("count").toInt(), 3);
    QCOMPARE(albumA->y(), albumAY);
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QVERIFY(driver.step(duration / 2));
    QVERIFY2(albumB->y() > 2 * rowHeight && albumB->y() < 4 * rowHeight, qPrintable(QString::number(albumB->y())));
    QVERIFY(driver.step(duration));
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QCOMPARE(albumA->y(), albumAY);
}

// Rows that change other than by a toggle, as when a scan or a restore
// brings them, move at once.
void UiMotionTest::treeChangesOtherThanAToggleAreInstant() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(QTest::qWaitFor([&]() { return view->property("count").toInt() == 3; }));
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = view->property("rowHeight").toDouble();

    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QVERIFY(QMetaObject::invokeMethod(view, "forceLayout"));
    QCOMPARE(view->property("count").toInt(), 5);
    QCOMPARE(albumB->y(), 4 * rowHeight);
}

// Rows a drag drops elsewhere: the rows they displace slide to their places
// over shortDuration with OutCubic, under the dropped row.
void UiMotionTest::queueRowsADragDisplacesSlide() {
    QueueRowsModel rows;
    QVector<QueueRowData> initial;
    for (int i = 0; i < 5; ++i) {
        QueueRowData data;
        data.title = QStringLiteral("Track %1").arg(i);
        data.path = queuePath(i);
        data.lengthText = QStringLiteral("1:00");
        initial.push_back(data);
    }
    rows.setRows(initial);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", QVariant::fromValue<QObject *>(&rows));
    bridge->setProperty("queueLength", 5);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 4) != nullptr; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "mainQueuePane")->property("controller"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    auto *slide = qvariant_cast<QObject *>(view->property("slideAnimation"));
    QVERIFY(slide != nullptr);
    QCOMPARE(slide->property("duration").toInt(), units->shortDuration());
    QCOMPARE(easingOf(slide), QEasingCurve::OutCubic);
    const double rowHeight = view->property("rowHeight").toDouble();
    // The slide's time, in the test's steps.
    SteppedAnimationDriver driver;

    // Row 0 dropped before row 3; the backend moves it there.
    QMetaObject::invokeMethod(controller, "dropQueueRows", Q_ARG(QVariant, 0), Q_ARG(QVariant, 3));
    QVERIFY(controller->property("movesSlide").toBool());
    QQuickItem *moved = itemAtIndex(view, 0);
    QQuickItem *displaced = itemAtIndex(view, 1);
    QVERIFY(rows.moveRow(0, 2));
    QVERIFY(QMetaObject::invokeMethod(view, "forceLayout"));
    // The dropped row is in place at once, over the row it displaces,
    // which starts where it was.
    QCOMPARE(moved->y(), 2 * rowHeight);
    QCOMPARE(displaced->y(), 1 * rowHeight);
    QVERIFY(moved->z() > displaced->z());
    QVERIFY(driver.step(units->shortDuration() / 2));
    QVERIFY2(displaced->y() < rowHeight && displaced->y() > 0, qPrintable(QString::number(displaced->y())));
    QVERIFY(driver.step(units->shortDuration()));
    QVERIFY(rowsSettled(view, 5));
    QCOMPARE(itemAtIndex(view, 2), moved);
}

// Moves by the keyboard are instant, so repeated moves never lag, also
// after a drag.
void UiMotionTest::queueRowsTheKeyboardMovesMoveAtOnce() {
    QueueRowsModel rows;
    QVector<QueueRowData> initial;
    for (int i = 0; i < 5; ++i) {
        QueueRowData data;
        data.title = QStringLiteral("Track %1").arg(i);
        data.path = queuePath(i);
        data.lengthText = QStringLiteral("1:00");
        initial.push_back(data);
    }
    rows.setRows(initial);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", QVariant::fromValue<QObject *>(&rows));
    bridge->setProperty("queueLength", 5);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 4) != nullptr; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "mainQueuePane")->property("controller"));
    const double rowHeight = view->property("rowHeight").toDouble();

    QMetaObject::invokeMethod(controller, "dropQueueRows", Q_ARG(QVariant, 0), Q_ARG(QVariant, 3));
    QVERIFY(controller->property("movesSlide").toBool());
    QMetaObject::invokeMethod(controller, "setSingleSelection", Q_ARG(QVariant, 3));
    QMetaObject::invokeMethod(controller, "moveSelected", Q_ARG(QVariant, -1));
    QVERIFY(!controller->property("movesSlide").toBool());
    QQuickItem *displaced = itemAtIndex(view, 2);
    QVERIFY(rows.moveRow(3, 2));
    QVERIFY(QMetaObject::invokeMethod(view, "forceLayout"));
    QCOMPARE(displaced->y(), 3 * rowHeight);
    QVERIFY(rowsSettled(view, 5));
}

// With Plasma's animations off, Kirigami's durations are 0: every change
// above is instant, and nothing is left to fade.
void UiMotionTest::animationsOffMakeEveryChangeInstant() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setShortDuration(0);
    units->setLongDuration(0);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    playQueueRow(bridge, 1);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Track 1"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album"));
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 2) != nullptr; }));
    QVERIFY(QTest::qWaitFor([&]() { return libraryRowWithPath(window, kTrackA2) != nullptr; }));

    // The queue and the tree.
    playQueueRow(bridge, 2);
    playTreeTrack(bridge, kTrackA2, kAlbumAKey);
    const RowLook oldQueueRow = queueRowLook(itemAtIndex(view, 1));
    QVERIFY(!oldQueueRow.fadingGlyph->isVisible());
    QVERIFY(colorOf(oldQueueRow.title) != nowHex(window));
    QCOMPARE(colorOf(queueRowLook(itemAtIndex(view, 2)).title), nowHex(window));
    const RowLook oldTreeRow = libraryRowLook(libraryRowWithPath(window, kTrackA1));
    QVERIFY(!oldTreeRow.fadingGlyph->isVisible());
    QVERIFY(colorOf(oldTreeRow.title) != nowHex(window));

    // Now playing, for another album.
    bridge->setProperty("currentTrackTitle", QStringLiteral("Third"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Album B"));
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    playQueueRow(bridge, 3);
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(!anyLineFades(panel));
    QVERIFY(!itemNamed(panel, QStringLiteral("albumArtOutgoing"))->isVisible());
    QVERIFY(allLinesStill(panel));

    // The tree's toggle.
    QQuickItem *tree = named(window, "libraryView");
    QObject *controller = qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller"));
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = tree->property("rowHeight").toDouble();
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QVERIFY(rowsSettled(tree, tree->property("count").toInt()));
}

// Once a change is done, nothing runs: no animation, no frame.
void UiMotionTest::nothingAnimatesOnceTheMotionIsDone() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey, kAlbumAKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    bridge->setProperty("queueRows", queueRows());
    bridge->setProperty("queueLength", 5);
    playQueueRow(bridge, 1);
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Track 1"));

    QQuickItem *view = named(window, "playlistView");
    QVERIFY(QTest::qWaitFor([&]() { return itemAtIndex(view, 2) != nullptr; }));

    playQueueRow(bridge, 2);
    playTreeTrack(bridge, kTrackA2, kAlbumAKey);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Track 2"));
    QMetaObject::invokeMethod(qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller")),
                              "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QObject *queueFade = qvariant_cast<QObject *>(named(window, "mainQueuePane")->property("nowFade"));
    QObject *treeFade = qvariant_cast<QObject *>(named(window, "libraryPane")->property("nowFade"));
    QObject *handover = qvariant_cast<QObject *>(named(window, "nowPlayingPanel")->property("handoverAnimation"));
    QVERIFY(animationOf(queueFade)->property("running").toBool());
    QVERIFY(QTest::qWaitFor([&]() {
        return !animationOf(queueFade)->property("running").toBool()
            && !animationOf(treeFade)->property("running").toBool()
            && !handover->property("running").toBool()
            && rowsSettled(named(window, "libraryView"), 3);
    }));
    // The last frames of the motion, and of the scrollbar the style shows
    // as the tree's height changes, then none.
    QTest::qWait(1000);
    QCOMPARE(framesOver(window, 500), 0);
}

// A track change in BridgeClient's order (#466): the new path and lines,
// with the cover before kept while the new track's is not known, then its
// cover. The cover before shows until the new one fades in over the long
// curve; the placeholder shows only for a track known to have no cover.
void UiMotionTest::nowPlayingKeepsTheCoverUntilTheNextTracksIsKnown() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QQuickItem *glyph = itemNamed(panel, QStringLiteral("albumArtPlaceholderGlyph"));
    QQuickItem *outgoingCover = itemNamed(panel, QStringLiteral("albumArtOutgoing"));
    QVERIFY(glyph != nullptr);
    QVERIFY(outgoingCover != nullptr);
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    QVERIFY(!glyph->isVisible());
    int placeholderShown = 0;
    const QMetaObject::Connection connection =
        QObject::connect(glyph, &QQuickItem::visibleChanged, glyph, [&]() {
            if (glyph->isVisible()) {
                ++placeholderShown;
            }
        });

    // Another album, whose cover the bridge does not know yet.
    showTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), covers.first);
    QVERIFY(handover->property("running").toBool());
    QVERIFY(!glyph->isVisible());
    QTest::qWait(units->shortDuration() / 2);
    QVERIFY(!glyph->isVisible());
    // Its cover comes and fades in over the cover before.
    bridge->setProperty("currentTrackCoverPath", covers.second);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QVERIFY(outgoingCover->isVisible());
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QVERIFY(!outgoingCover->isVisible());
    QCOMPARE(placeholderShown, 0);

    // A track known to have no cover: the placeholder shows once its
    // metadata says so, and not before.
    showTrack(bridge, kTrackA2, QStringLiteral("Third"), QStringLiteral("Album A"), covers.second);
    QVERIFY(!glyph->isVisible());
    QCOMPARE(placeholderShown, 0);
    bridge->setProperty("currentTrackCoverPath", QString());
    QVERIFY(glyph->isVisible());
    QCOMPARE(placeholderShown, 1);
    QObject::disconnect(connection);
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // The basic render loop, whatever the environment asks for: the threaded
    // one installs an animation driver of its own for the GUI thread, and Qt
    // then takes no SteppedAnimationDriver.
    qputenv("QSG_RENDER_LOOP", "basic");
    // The app's controls style and Breeze icons, whatever the desktop the
    // tests run in; TestSessionIsolation.h.
    isolateFromDesktopSessionWithAppLook();
    // Settings, data and caches of the test's own.
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    QApplication app(argc, argv);
    UiMotionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_motion.moc"
