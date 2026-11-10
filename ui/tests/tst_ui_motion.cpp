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
#include <QScopeGuard>
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

using Window = std::pair<double, double>;

// A line of the now-playing panel as it draws: the playing track's text
// whole while nothing fades; while it fades, the playing track's text in
// windows where it stays still and, fading in as one, in windows where it
// changes, and under that the lines before fading out piece by piece. Looked
// up anew each time: a fade that starts again makes its pieces again.
struct PanelLine {
    QQuickItem *panel = nullptr;
    QString name;

    QQuickItem *stillText() const { return itemNamed(panel, QStringLiteral("nowPlaying") + name); }
    QQuickItem *still() const { return stillText()->parentItem(); }
    QQuickItem *line() const { return still()->parentItem(); }
    QQuickItem *incoming() const { return itemNamed(line(), QStringLiteral("nowPlayingIncoming") + name); }
    bool valid() const { return stillText() != nullptr; }
    // The pieces of the lines before, oldest first.
    QList<QQuickItem *> parts() const {
        return itemsNamed(line(), QStringLiteral("nowPlayingOutgoing") + name + QStringLiteral("Part"));
    }
    QString text() const { return stillText()->property("text").toString(); }
    // The oldest line before's text, or "" when none fades.
    QString outgoingText() const {
        const QList<QQuickItem *> layers =
            itemsNamed(line(), QStringLiteral("nowPlayingOutgoing") + name + QStringLiteral("Layer"));
        return layers.isEmpty() ? QString() : layers.first()->property("text").toString();
    }
    // The opacity of the piece of the lines before that shows most, 0 when
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
    QFont font() const { return stillText()->property("font").value<QFont>(); }
    // Where the playing track's text shows whole: all of the line while
    // nothing fades.
    QList<Window> stillWindows() const {
        QList<Window> windows;
        if (!still()->clip()) {
            windows.push_back({0.0, line()->width()});
        }
        for (QQuickItem *window : itemsNamed(line(), QStringLiteral("nowPlayingStill") + name)) {
            windows.push_back({window->x(), window->x() + window->width()});
        }
        return windows;
    }
    // Where the playing track's text fades in.
    QList<Window> incomingWindows() const {
        QList<Window> windows;
        if (incoming()->isVisible()) {
            for (QQuickItem *window :
                 itemsNamed(incoming(), QStringLiteral("nowPlayingIncoming") + name + QStringLiteral("Window"))) {
                windows.push_back({window->x(), window->x() + window->width()});
            }
        }
        return windows;
    }
    bool stillAt(double x) const {
        const QList<Window> windows = stillWindows();
        return std::any_of(windows.begin(), windows.end(),
                           [x](const Window &window) { return x >= window.first && x < window.second; });
    }
    // What of the playing track's text shows in each still window: the
    // glyphs wholly in it, from the first to the last.
    QStringList stillTexts() const {
        const QFontMetricsF metrics(font());
        const QString shown = text();
        QStringList texts;
        for (const auto &[from, to] : stillWindows()) {
            qsizetype first = -1;
            qsizetype last = -1;
            double left = 0.0;
            for (qsizetype index = 0; index < shown.size(); ++index) {
                const double right = metrics.horizontalAdvance(shown.left(index + 1));
                if (!shown.at(index).isSpace() && left >= from - 1e-6 && right <= to + 1e-6) {
                    first = first < 0 ? index : first;
                    last = index;
                }
                left = right;
            }
            texts.push_back(first < 0 ? QString() : shown.mid(first, last - first + 1));
        }
        return texts;
    }
    // The opacities the line before and the playing track's line show with
    // at `x`, in the line: both whole where the line stays still.
    std::pair<double, double> opacitiesAt(double x) const {
        if (stillAt(x)) {
            return {1.0, 1.0};
        }
        for (QQuickItem *part : parts()) {
            if (x >= part->x() && x < part->x() + part->width()) {
                return {part->opacity(), incomingOpacity()};
            }
        }
        return {0.0, incomingOpacity()};
    }
    // Where the pieces of the lines before start, past the first.
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
    if (!line.valid()) {
        return false;
    }
    const QList<Window> windows = line.stillWindows();
    return windows.size() == 1 && qFuzzyIsNull(windows.first().first)
        && qFuzzyCompare(windows.first().second + 1.0, line.line()->width() + 1.0)
        && qFuzzyCompare(opacityUpTo(line.line(), panel), 1.0);
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

// What draws at a point of a line of the now-playing panel: a glyph of one
// of its layers and the opacity it draws with.
struct Drawn {
    QChar glyph;
    double opacity = 0.0;
};

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

// Where an edge of what `line` draws cuts a glyph in two, or "": each edge
// of a still window in a gap of every text the line shows, each edge of a
// window the playing track's text fades in in a gap of that text, and each
// edge of a piece of a line before in a gap of its own text. The ends of
// the line need no gap.
QString cutGlyph(const PanelLine &line) {
    const QFontMetricsF metrics(line.font());
    const double width = line.line()->width();
    QStringList texts{line.text()};
    for (QQuickItem *layer : layersOf(line)) {
        texts.push_back(layer->property("text").toString());
    }
    QString cut;
    const auto check = [&](double x, const QStringList &in, const QString &what) {
        if (!cut.isEmpty() || x <= 0.0 || x >= width) {
            return;
        }
        for (const QString &text : in) {
            const QChar glyph = glyphAt(metrics, text, x);
            if (!glyph.isNull() && !glyph.isSpace()) {
                cut = QStringLiteral("%1 edge at %2 px cuts \"%3\" of \"%4\"").arg(what).arg(x).arg(glyph).arg(text);
                return;
            }
        }
    };
    for (const auto &[from, to] : line.stillWindows()) {
        check(from, texts, QStringLiteral("still"));
        check(to, texts, QStringLiteral("still"));
    }
    for (const auto &[from, to] : line.incomingWindows()) {
        check(from, {line.text()}, QStringLiteral("incoming"));
        check(to, {line.text()}, QStringLiteral("incoming"));
    }
    for (QQuickItem *layer : layersOf(line)) {
        const QString text = layer->property("text").toString();
        for (QQuickItem *part : partsOf(line, layer)) {
            check(part->x(), {text}, QStringLiteral("part"));
            check(part->x() + part->width(), {text}, QStringLiteral("part"));
        }
    }
    return cut;
}

// What `line` draws at `x`, bottom to top: the playing track's text where
// it stays still, each piece of the lines before, oldest first, then the
// playing track's text fading in; leaving out what is at opacity 0. The
// glyph is null past the end of its text.
QList<Drawn> drawnAt(const PanelLine &line, double x) {
    const QFontMetricsF metrics(line.font());
    QList<Drawn> drawn;
    const auto add = [&](const QString &text, double opacity) {
        if (opacity > 0) {
            drawn.push_back({glyphAt(metrics, text, x), opacity});
        }
    };
    if (line.stillAt(x)) {
        add(line.text(), 1.0);
    }
    for (QQuickItem *layer : layersOf(line)) {
        for (QQuickItem *part : partsOf(line, layer)) {
            if (x >= part->x() && x < part->x() + part->width()) {
                add(partText(line, part), part->opacity());
            }
        }
    }
    for (const auto &[from, to] : line.incomingWindows()) {
        if (x >= from && x < to) {
            add(line.text(), line.incoming()->opacity());
        }
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

// The now-playing fade's curves, as NowPlayingPanel has them: its handover
// runs over the fade's time, linearly; the lines before go out over its
// first half and the playing track's come in over its second, so that no
// frame shows both (#373 H1); the covers before go out over all of it. Each
// is on the OutCubic curve.
double eased(double progress) {
    const double rest = 1.0 - std::clamp(progress, 0.0, 1.0);
    return 1.0 - rest * rest * rest;
}

double linesOut(double at) {
    return 1.0 - eased(2.0 * at);
}

double linesIn(double at) {
    return eased(2.0 * at - 1.0);
}

double coversOut(double at) {
    return 1.0 - eased(at);
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

// Plays a track as BridgeClient tells it, the path, then the metadata, with
// details of `year`, `genre`, and the number of `total`; a year or a number
// of 0, or an empty genre, are not known.
void tellDetails(QObject *bridge, const QString &path, const QString &title, const QString &album, int year,
                 const QString &genre, int number, int total) {
    bridge->setProperty("currentTrackPath", path);
    bridge->setProperty("currentTrackTitle", title);
    bridge->setProperty("currentTrackAlbum", album);
    bridge->setProperty("currentTrackYear", year > 0 ? QVariant(year) : QVariant());
    bridge->setProperty("currentTrackGenre", genre);
    bridge->setProperty("currentTrackNumber", number);
    bridge->setProperty("currentTrackTotal", total);
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
    void nowPlayingStillRunsAreFieldsAndWordsInPlace();
    void nowPlayingStillRunsAreFieldsAndWordsInPlace_data();
    void nowPlayingOnlyTheNumberThatChangesFades();
    void nowPlayingOnlyTheNumberThatChangesFades_data();
    void nowPlayingFieldThatMovesFades();
    void nowPlayingFieldThatMovesFades_data();
    void nowPlayingElidedLineKeepsWhatStaysStillAfterAChange();
    void nowPlayingRightToLeftDetailsFadeWhole();
    void nowPlayingInterruptionKeepsWhatStaysStill();
    void nowPlayingInterruptionKeepsWhatStaysStill_data();
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
    void nowPlayingNoFrameShowsTwoTextsInALine();
    void nowPlayingNoFrameShowsTwoTextsInALine_data();
    void visualizationModeSwitchKeepsAPlotOnScreen();
    void visualizationModeSwitchKeepsAPlotOnScreen_data();
    void visualizationPaneIsTheViewUntilTheFirstData();
    void visualizationPaneIsTheViewUntilTheFirstData_data();
    void viewerControlsFadeWithTheStrip();
    void viewerControlsFadeWithTheStrip_data();
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
// fades its tick out.
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
    // again, and Album B's tick fades out. A collapsed row shows no glyph.
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    QVERIFY(albumB != nullptr);
    playTreeTrack(bridge, kTrackB1, kAlbumBKey);
    const RowLook album = libraryRowLook(albumB);
    QQuickItem *albumTick = itemNamed(albumB, QStringLiteral("libraryRowNowTick"));
    QVERIFY(albumTick != nullptr);
    QVERIFY(albumTick->isVisible());
    QCOMPARE(albumTick->opacity(), 1.0);
    QVERIFY(!album.glyph->isVisible());
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QVERIFY(albumTick->isVisible());
    QCOMPARE(albumTick->opacity(), 1.0);
    QCOMPARE(colorOf(albumTick), nowHex(window));
    QVERIFY(!album.glyph->isVisible());
    QVERIFY(!album.fadingGlyph->isVisible());
    // Its title is never amber.
    QVERIFY(colorOf(album.title) != nowHex(window));
    QCOMPARE(colorOf(libraryRowLook(libraryRowWithPath(window, kTrackA1)).title), nowHex(window));
    QVERIFY(QTest::qWaitFor([&]() {
        return albumTick->opacity() < 1.0 || !albumTick->isVisible();
    }));
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    QVERIFY(!albumTick->isVisible());
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
// what changes fades, in place, over shortDuration: the title, and the
// track number in the details, between "Track" and "of 2", which stay still
// with the year and the genre. What changes goes out, then comes in: no
// frame shows both texts (#373 H1). The artist and the album stay at full
// opacity in every frame, and no edge cuts a glyph.
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
    const QStringList stillDetails = narrow
        ? QStringList{QStringLiteral("Classical · Track"), QStringLiteral("of 2")}
        : QStringList{QStringLiteral("2005 · Classical · Track"), QStringLiteral("of 2")};
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
            if (line.incomingOpacity() > 0 && line.outgoingOpacity() > 0) {
                failure = QStringLiteral("%1 fades in at %2 and out at %3 in check %4")
                              .arg(name).arg(line.incomingOpacity()).arg(line.outgoingOpacity()).arg(checks);
            }
        }
        const PanelLine titleLine = panelLine(panel, title);
        if (titleLine.fades() && !titleLine.stillTexts().isEmpty()) {
            failure = QStringLiteral("the title keeps %1 still").arg(titleLine.stillTexts().join(QLatin1Char('|')));
        }
        const PanelLine detailsLine = panelLine(panel, details);
        if (detailsLine.fades() && detailsLine.stillTexts() != stillDetails) {
            failure = QStringLiteral("the details keep %1 still").arg(detailsLine.stillTexts().join(QLatin1Char('|')));
        }
        for (const QString &name : {title, details}) {
            const QString cut = cutGlyph(panelLine(panel, name));
            if (!cut.isEmpty()) {
                failure = name + QStringLiteral(": ") + cut;
            }
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
    QCOMPARE(easingOf(handover), QEasingCurve::Linear);
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
    QCOMPARE(detailsLine.stillTexts(), stillDetails);
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
        QCOMPARE(titleLine.outgoingOpacity(), linesOut(at));
        QCOMPARE(detailsLine.outgoingOpacity(), linesOut(at));
        QCOMPARE(titleLine.incomingOpacity(), linesIn(at));
        QCOMPARE(detailsLine.incomingOpacity(), linesIn(at));
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

// Another album: every line that differs fades with the cover over
// longDuration, the cover cross-fading over all of it, the lines going out
// over its first half and coming in over its second; the artist both share
// stays still, and so does what is between the year and the number.
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
    QCOMPARE(easingOf(handover), QEasingCurve::Linear);
    QQuickItem *outgoingCover = itemNamed(panel, QStringLiteral("albumArtOutgoing"));
    QVERIFY(outgoingCover->isVisible());
    // Every point of the fade: the cover on its curve, every line on its.
    const auto onTheCurves = [&](double at) {
        for (const QString &name : kWideLines) {
            const PanelLine line = panelLine(panel, name);
            if (!sameOpacity(line.outgoingOpacity(), linesOut(at))
                || !sameOpacity(line.incomingOpacity(), linesIn(at))) {
                return false;
            }
        }
        return sameOpacity(outgoingCover->opacity(), coversOut(at));
    };
    QVERIFY(onTheCurves(0.0));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.25, 0.5, 0.75}) {
        panel->setProperty("handover", at);
        QVERIFY2(onTheCurves(at), qPrintable(QString::number(at)));
    }
    panel->setProperty("handover", 0.25);
    QCOMPARE(textOf(panel, "nowPlayingOutgoingTitle"), QStringLiteral("Second"));
    QCOMPARE(textOf(panel, "nowPlayingOutgoingArtistAlbum"), QStringLiteral("Artist A — Album A"));
    QCOMPARE(textOf(panel, "nowPlayingOutgoingDetails"), QStringLiteral("2005 · Track 2"));
    QCOMPARE(panelLine(panel, QStringLiteral("Title")).stillTexts(), QStringList());
    // The year and the number change in place, in tabular figures: what is
    // between them stays still.
    QCOMPARE(panelLine(panel, QStringLiteral("Details")).stillTexts(), QStringList{QStringLiteral("· Track")});
    QCOMPARE(panelLine(panel, QStringLiteral("ArtistAlbum")).stillTexts(), QStringList{QStringLiteral("Artist A —")});
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
    // Laid out: a line draws what fades in pieces of its width.
    QTRY_VERIFY(panelLine(panel, QStringLiteral("Title")).line()->width() > 0);
    showTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"));
    QVERIFY(handover->property("running").toBool());
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    const PanelLine artistAlbum = panelLine(panel, QStringLiteral("ArtistAlbum"));
    // Part of the way.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.45);
    QCOMPARE(title.outgoingOpacity(), linesOut(0.45));
    QVERIFY(title.outgoingOpacity() > 0.0);
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
    QCOMPARE(artistAlbum.stillTexts(), QStringList());
    QCOMPARE(artistAlbum.outgoingText(), QStringLiteral("Artist A — Album A"));
    QCOMPARE(artistAlbum.text(), QStringLiteral("Artist B — Album A"));
    QCOMPARE(title.outgoingOpacity(), titleBefore);
    QCOMPARE(title.outgoingText(), QStringLiteral("First"));

    // Then each goes one way only, on its curve, to the playing track.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    double titleOut = titleBefore;
    double lineOut = 1.0;
    for (const double at : {0.25, 0.5, 0.9}) {
        panel->setProperty("handover", at);
        const double nextTitle = title.outgoingOpacity();
        const double nextLine = artistAlbum.outgoingOpacity();
        QVERIFY(nextTitle <= titleOut);
        QVERIFY(nextLine < lineOut || nextLine == 0.0);
        QCOMPARE(nextTitle, titleBefore * linesOut(at));
        QCOMPARE(nextLine, linesOut(at));
        QCOMPARE(artistAlbum.incomingOpacity(), linesIn(at));
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
// it showed, and so does what stayed still after it: it does not take on
// the rest's progress, and nothing jumps. The playing track's text fades in
// as one where the line no longer stays still, once all that goes out is
// out, so none of its glyphs is cut where the line before's pieces are,
// however long its new field.
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
    QCOMPARE(details.stillTexts(), QStringList{QStringLiteral("2005 · Rock · Track")});
    // A quarter of the way: the number before going out, the next not in.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.25);
    const double numberLeft = linesOut(0.25);
    QVERIFY(numberLeft > 0.0);
    QCOMPARE(linesIn(0.25), 0.0);
    const QFontMetricsF metrics(details.font());
    const double inGenre = metrics.horizontalAdvance(QStringLiteral("2005 · R"));
    const double inTrack = metrics.horizontalAdvance(QStringLiteral("2005 · Rock · Tr"));
    const double inNumber = (metrics.horizontalAdvance(QStringLiteral("2005 · Rock · Track "))
                             + metrics.horizontalAdvance(QStringLiteral("2005 · Rock · Track 1"))) / 2;
    QCOMPARE(details.opacitiesAt(inGenre), std::make_pair(1.0, 1.0));
    QCOMPARE(details.opacitiesAt(inTrack), std::make_pair(1.0, 1.0));
    QCOMPARE(details.opacitiesAt(inNumber), std::make_pair(numberLeft, 0.0));
    QCOMPARE(details.opacitiesAt(metrics.horizontalAdvance(QStringLiteral("20"))), std::make_pair(1.0, 1.0));

    // The file's genre replaces the queue's.
    bridge->setProperty("currentTrackGenre", genre);
    QCOMPARE(details.text(), QStringLiteral("2005 · %1 · Track 2").arg(genre));
    QCOMPARE(details.outgoingText(), QStringLiteral("2005 · Rock · Track 1"));
    // At once: the genre before, and "Track" after it, which moves or might,
    // show whole, as they did, and fade out from there; the number before
    // goes on from where it was, and the playing track's text, not in yet,
    // comes in once they are out.
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(details.opacitiesAt(inGenre), std::make_pair(1.0, 0.0));
    QCOMPARE(details.opacitiesAt(inTrack), std::make_pair(1.0, 0.0));
    QCOMPARE(details.opacitiesAt(inNumber), std::make_pair(numberLeft, 0.0));
    QCOMPARE(details.opacitiesAt(metrics.horizontalAdvance(QStringLiteral("20"))), std::make_pair(1.0, 1.0));
    // The playing track's text is cut only where both share a gap, after
    // "2005 ·", wherever its own fields are; the lines before's pieces only
    // in their own gaps.
    QCOMPARE(details.stillTexts(), QStringList{QStringLiteral("2005 ·")});
    QCOMPARE(itemsNamed(details.line(), QStringLiteral("nowPlayingIncomingDetails")).size(), 1);
    const QList<Window> incoming = details.incomingWindows();
    QCOMPARE(incoming.size(), 1);
    QCOMPARE(incoming.first().first, details.stillWindows().first().second);
    QCOMPARE(incoming.first().second, details.line()->width());
    QCOMPARE(details.parts().size(), 2);
    QVERIFY2(cutGlyph(details).isEmpty(), qPrintable(cutGlyph(details)));

    // Then each goes on, on its curve: out, then in.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.25, 0.6}) {
        panel->setProperty("handover", at);
        QCOMPARE(details.opacitiesAt(inGenre), std::make_pair(linesOut(at), linesIn(at)));
        QCOMPARE(details.opacitiesAt(inTrack), std::make_pair(linesOut(at), linesIn(at)));
        QCOMPARE(details.opacitiesAt(inNumber), std::make_pair(numberLeft * linesOut(at), linesIn(at)));
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(allLinesStill(panel));
}

// A line with right-to-left text is not laid out from the left: none of it
// stays still, and the whole of it fades out, then in, as one.
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
        QCOMPARE(artistAlbum.stillWindows(), QList<Window>());
        const QList<QQuickItem *> parts = artistAlbum.parts();
        QCOMPARE(parts.size(), 1);
        QCOMPARE(parts.first()->x(), 0.0);
        QCOMPARE(parts.first()->width(), artistAlbum.line()->width());
        QCOMPARE(parts.first()->opacity(), linesOut(at));
        QCOMPARE(artistAlbum.incomingWindows(), (QList<Window>{{0.0, artistAlbum.line()->width()}}));
        QCOMPARE(artistAlbum.incomingOpacity(), linesIn(at));
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(allLinesStill(panel));
}

// A line that comes to have right-to-left text while it fades is not laid
// out from the left any more: none of it stays still. What it showed goes
// out, from the opacity it had, the start that showed whole from there, and
// the playing track's text then comes in as one: no frame shows both.
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
    QCOMPARE(artistAlbum.stillTexts(), QStringList{QStringLiteral("Artist A —")});
    // Three quarters of the way: the album before out, the next coming in.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.75);
    const double cameIn = linesIn(0.75);
    QVERIFY(cameIn > 0.0 && cameIn < 1.0);
    QCOMPARE(artistAlbum.incomingOpacity(), cameIn);
    QCOMPARE(artistAlbum.outgoingOpacity(), 0.0);

    // The file names the artist in Hebrew.
    bridge->setProperty("currentTrackArtist", QStringLiteral("להקת הים"));
    QCOMPARE(artistAlbum.text(), QStringLiteral("להקת הים — Album Bee"));
    // What showed goes out: the album as far as it came in, the artist,
    // which was still, whole.
    QCOMPARE(artistAlbum.outgoingText(), QStringLiteral("Artist A — Album Bee"));
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(artistAlbum.stillWindows(), QList<Window>());
    QCOMPARE(itemsNamed(artistAlbum.line(), QStringLiteral("nowPlayingIncomingArtistAlbum")).size(), 1);
    QCOMPARE(artistAlbum.incomingWindows(), (QList<Window>{{0.0, artistAlbum.line()->width()}}));
    QCOMPARE(artistAlbum.incomingOpacity(), 0.0);
    const QFontMetricsF metrics(artistAlbum.font());
    const double inArtist = metrics.horizontalAdvance(QStringLiteral("Art"));
    const double inAlbum = metrics.horizontalAdvance(QStringLiteral("Artist A — Al"));
    QCOMPARE(artistAlbum.opacitiesAt(inArtist), std::make_pair(1.0, 0.0));
    QCOMPARE(artistAlbum.opacitiesAt(inAlbum), std::make_pair(cameIn, 0.0));
    QVERIFY2(cutGlyph(artistAlbum).isEmpty(), qPrintable(cutGlyph(artistAlbum)));

    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.25, 0.75}) {
        panel->setProperty("handover", at);
        QCOMPARE(artistAlbum.opacitiesAt(inArtist), std::make_pair(linesOut(at), linesIn(at)));
        QCOMPARE(artistAlbum.opacitiesAt(inAlbum), std::make_pair(cameIn * linesOut(at), linesIn(at)));
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(allLinesStill(panel));
}

// A line whose parts fade on their own, elided ever shorter: no edge, of
// the still windows or between the pieces, falls where the ellipsis is, or
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
    panel->setProperty("handover", 0.25);
    // A late genre: two pieces, split after "Track", which stayed still
    // with the genre before.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Jazz"));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    QCOMPARE(details.parts().size(), 2);
    const QString before = details.outgoingText();
    const QString shared = QStringLiteral("2005 ·");
    const QString split = QStringLiteral("2005 · ") + genre + QStringLiteral(" · Track");
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
        const QList<double> edges = details.innerEdges();
        for (const double edge : edges) {
            QVERIFY2(edge <= shownUpTo(before, width), qPrintable(QStringLiteral("part edge %1").arg(edge) + where));
            ++kept;
        }
        // An edge whose gap the ellipsis took is at the end, where the
        // piece after it has nothing left to draw.
        if (edges.isEmpty() && splitEdge < width) {
            ++moved;
        }
        for (const Window &window : details.stillWindows()) {
            const double still = window.second;
            if (still < width) {
                QVERIFY2(still <= shownUpTo(before, width) && still <= shownUpTo(details.text(), width),
                         qPrintable(QStringLiteral("still edge %1").arg(still) + where));
                QCOMPARE(details.stillTexts(), QStringList{shared});
            }
        }
        QVERIFY2(cutGlyph(details).isEmpty(), qPrintable(cutGlyph(details) + where));
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

void UiMotionTest::nowPlayingStillRunsAreFieldsAndWordsInPlace_data() {
    QTest::addColumn<QString>("before");
    QTest::addColumn<QString>("after");
    QTest::addColumn<QString>("separator");
    QTest::addColumn<bool>("tabular");
    QTest::addColumn<QStringList>("still");
    const QString dot = QStringLiteral(" · ");
    QTest::newRow("the number") << "2005 · Rock · Track 1" << "2005 · Rock · Track 2" << dot << true
                                << QStringList{"2005 · Rock · Track"};
    QTest::newRow("the number of a total") << "Track 4 of 8" << "Track 5 of 8" << dot << true
                                           << QStringList{"Track", "of 8"};
    QTest::newRow("the year") << "2019 · Synth-pop" << "2020 · Synth-pop" << dot << true
                              << QStringList{"· Synth-pop"};
    QTest::newRow("the year and the number") << "2019 · Synth-pop · Track 3 of 10"
                                             << "2020 · Synth-pop · Track 1 of 12" << dot << true
                                             << QStringList{"· Synth-pop · Track", "of"};
    QTest::newRow("the year, not in tabular figures") << "2019 · Synth-pop" << "2020 · Synth-pop" << dot << false
                                                      << QStringList{};
    QTest::newRow("a number that gets wider") << "Track 9 of 12" << "Track 10 of 12" << dot << true
                                              << QStringList{"Track"};
    QTest::newRow("a field that gets wider") << "2005 · Rock · Track 2" << "2005 · Electronic · Track 2" << dot
                                             << true << QStringList{"2005 ·"};
    QTest::newRow("a field more") << "2005 · Rock" << "2005 · Rock · Track 2" << dot << true
                                  << QStringList{"2005 · Rock"};
    QTest::newRow("a field fewer") << "2005 · Rock · Track 1" << "2005 · Rock" << dot << true
                                   << QStringList{"2005 · Rock"};
    QTest::newRow("a word that is not a number") << "Album A (2005)" << "Album B (2005)" << "" << true
                                                 << QStringList{};
    QTest::newRow("the year with the album") << "Album A (2005)" << "Album A (2007)" << "" << true
                                             << QStringList{"Album A"};
    QTest::newRow("the artist") << "Artist — Album A" << "Artist — Album B" << " — " << false
                                << QStringList{"Artist —"};
    QTest::newRow("the same") << "2005 · Rock" << "2005 · Rock" << dot << true << QStringList{"2005 · Rock"};
    QTest::newRow("one field, the same") << "First" << "First" << "" << false << QStringList{"First"};
    QTest::newRow("one field, another") << "First" << "Firsts" << "" << false << QStringList{};
    QTest::newRow("right to left") << "להקה — שירים" << "להקה — שירים נוספים" << " — " << false << QStringList{};
    QTest::newRow("right to left later") << "Artist — שירים" << "Artist — Album" << " — " << false
                                         << QStringList{};
    QTest::newRow("right to left, the year") << "2019 · רוק" << "2020 · רוק" << dot << true << QStringList{};
    QTest::newRow("right to left, the same") << "להקה" << "להקה" << "" << false << QStringList{};
}

// What stays still of a line that changes: its fields that are the same and
// do not move, and in a field where only numbers change, its words that are
// the same and do not move. A number keeps its width in tabular figures
// when it keeps its number of digits. Nothing stays still in right-to-left
// text.
void UiMotionTest::nowPlayingStillRunsAreFieldsAndWordsInPlace() {
    QFETCH(QString, before);
    QFETCH(QString, after);
    QFETCH(QString, separator);
    QFETCH(bool, tabular);
    QFETCH(QStringList, still);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QVariant result;
    QVERIFY(QMetaObject::invokeMethod(named(window, "nowPlayingPanel"), "_stillRuns",
                                      Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, before),
                                      Q_ARG(QVariant, after), Q_ARG(QVariant, separator),
                                      Q_ARG(QVariant, tabular)));
    QStringList runs;
    for (const QVariant &run : result.toList()) {
        const QVariantList range = run.toList();
        QCOMPARE(range.size(), 2);
        const int start = range.at(0).toInt();
        const int end = range.at(1).toInt();
        // The same, in the same place, in both.
        QCOMPARE(before.mid(start, end - start), after.mid(start, end - start));
        runs.push_back(after.mid(start, end - start));
    }
    QCOMPARE(runs, still);
}

void UiMotionTest::nowPlayingOnlyTheNumberThatChangesFades_data() {
    QTest::addColumn<bool>("narrow");
    QTest::addColumn<int>("yearBefore");
    QTest::addColumn<int>("yearAfter");
    QTest::addColumn<QString>("genre");
    QTest::addColumn<int>("numberBefore");
    QTest::addColumn<int>("numberAfter");
    QTest::addColumn<int>("total");
    // Where the word that changes starts, and what it is before and after.
    QTest::addColumn<QString>("prefix");
    QTest::addColumn<QString>("was");
    QTest::addColumn<QString>("is");
    QTest::newRow("the number") << false << 0 << 0 << QString() << 4 << 5 << 8 << "Track " << "4" << "5";
    QTest::newRow("the number, narrow") << true << 0 << 0 << QString() << 4 << 5 << 8 << "Track " << "4" << "5";
    // Another album of the artist, a year later, of the same genre.
    QTest::newRow("the year") << false << 2019 << 2020 << "Synth-pop" << 0 << 0 << 0 << "" << "2019" << "2020";
}

// The issue's two cases (#537): "Track 4 of 8" to "Track 5 of 8", and
// "2019 · Synth-pop" to "2020 · Synth-pop". In tabular figures the new
// number takes the old one's place: only it cross-fades, and every other
// glyph of the line shows once, at full opacity, in every frame of the fade
// and at each point the test sets.
void UiMotionTest::nowPlayingOnlyTheNumberThatChangesFades() {
    QFETCH(bool, narrow);
    QFETCH(int, yearBefore);
    QFETCH(int, yearAfter);
    QFETCH(QString, genre);
    QFETCH(int, numberBefore);
    QFETCH(int, numberAfter);
    QFETCH(int, total);
    QFETCH(QString, prefix);
    QFETCH(QString, was);
    QFETCH(QString, is);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    if (narrow) {
        window->resize(1000, 720);
    }
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    // Long enough for some frames.
    units->setShortDuration(600);
    units->setLongDuration(600);
    const QString name = narrow ? QStringLiteral("NarrowDetails") : QStringLiteral("Details");
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    tellDetails(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), yearBefore, genre,
                numberBefore, total);
    QTRY_COMPARE(panel->property("layoutMode").toString(), narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    const PanelLine line = panelLine(panel, name);
    QTRY_VERIFY(line.line()->width() > 0);
    QVERIFY(panelIdle(panel, narrow ? kNarrowLines : kWideLines));
    const QString before = line.text();
    QCOMPARE(before.mid(prefix.size(), was.size()), was);

    // The word that changes, where it is in both lines.
    const QFontMetricsF metrics(line.font());
    const double from = metrics.horizontalAdvance(prefix);
    const double to = metrics.horizontalAdvance(prefix + was);
    // What draws other than the playing track's line at full opacity, with
    // only that word cross-fading on the curve at `at`, or "".
    const auto onlyTheWord = [&](double at) -> QString {
        const QString after = line.text();
        if (line.outgoingText() != before) {
            return QStringLiteral("the line before is \"%1\"").arg(line.outgoingText());
        }
        for (double x = 0.5; x < line.line()->width(); x += 1.0) {
            QList<Drawn> drawn = drawnAt(line, x);
            drawn.removeIf([](const Drawn &each) { return each.glyph.isNull() || each.glyph.isSpace(); });
            QList<Drawn> expected;
            const auto expect = [&](QChar glyph, double opacity) {
                if (!glyph.isNull() && !glyph.isSpace() && opacity > 0) {
                    expected.push_back({glyph, opacity});
                }
            };
            if (x >= from && x < to) {
                expect(glyphAt(metrics, before, x), linesOut(at));
                expect(glyphAt(metrics, after, x), linesIn(at));
            } else {
                expect(glyphAt(metrics, after, x), 1.0);
            }
            bool same = drawn.size() == expected.size();
            for (qsizetype index = 0; same && index < drawn.size(); ++index) {
                same = drawn.at(index).glyph == expected.at(index).glyph
                    && sameOpacity(drawn.at(index).opacity, expected.at(index).opacity);
            }
            if (!same) {
                QStringList shown;
                for (const Drawn &each : drawn) {
                    shown.push_back(QStringLiteral("%1 %2").arg(each.glyph).arg(each.opacity));
                }
                return QStringLiteral("at %1 px, at %2: [%3]").arg(x).arg(at).arg(shown.join(QStringLiteral(", ")));
            }
        }
        return cutGlyph(line);
    };
    QString failure;
    int frames = 0;
    const QMetaObject::Connection connection =
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [&]() {
            if (failure.isEmpty() && handover->property("running").toBool()) {
                ++frames;
                failure = onlyTheWord(panel->property("handover").toDouble());
            }
        });

    tellDetails(bridge, yearAfter != yearBefore ? kTrackB1 : kTrackA2, QStringLiteral("Second"),
                yearAfter != yearBefore ? QStringLiteral("Album B") : QStringLiteral("Album A"), yearAfter, genre,
                numberAfter, total);
    QVERIFY(handover->property("running").toBool());
    QVERIFY(line.fades());
    const QString after = line.text();
    QCOMPARE(after, QString(before).replace(prefix.size(), was.size(), is));
    // In tabular figures, in the same place.
    QCOMPARE(metrics.horizontalAdvance(prefix + is), to);
    QCOMPARE(metrics.horizontalAdvance(after), metrics.horizontalAdvance(before));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.0, 0.25, 0.5, 0.75}) {
        const int framesBefore = frames;
        panel->setProperty("handover", at);
        const QString each = onlyTheWord(at);
        QVERIFY2(each.isEmpty(), qPrintable(each));
        QVERIFY(QTest::qWaitFor([&]() { return frames > framesBefore; }));
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    panel->setProperty("handover", 0.0);
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(frames > 0);
    QVERIFY(panelIdle(panel, narrow ? kNarrowLines : kWideLines));
}

void UiMotionTest::nowPlayingFieldThatMovesFades_data() {
    QTest::addColumn<QString>("genreBefore");
    QTest::addColumn<QString>("genreAfter");
    QTest::addColumn<int>("year");
    QTest::addColumn<int>("numberBefore");
    QTest::addColumn<int>("numberAfter");
    QTest::addColumn<int>("total");
    QTest::addColumn<QStringList>("still");
    // A word that moves, as each line lays it out up to it.
    QTest::addColumn<QString>("movedBefore");
    QTest::addColumn<QString>("movedAfter");
    QTest::newRow("a number with a digit more") << QString() << QString() << 0 << 9 << 10 << 12
                                                << QStringList{QStringLiteral("Track")} << "Track 9 o"
                                                << "Track 10 o";
    QTest::newRow("a wider genre") << "Rock" << "Electronic" << 2005 << 2 << 3 << 8
                                   << QStringList{QStringLiteral("2005 ·")} << "2005 · Rock · Tr"
                                   << "2005 · Electronic · Tr";
}

// A field, or a word of one, that is the same but moves, because what is
// before it on the line changed its width, fades: what is after the change
// fades whole, and only what is before it can stay still.
void UiMotionTest::nowPlayingFieldThatMovesFades() {
    QFETCH(QString, genreBefore);
    QFETCH(QString, genreAfter);
    QFETCH(int, year);
    QFETCH(int, numberBefore);
    QFETCH(int, numberAfter);
    QFETCH(int, total);
    QFETCH(QStringList, still);
    QFETCH(QString, movedBefore);
    QFETCH(QString, movedAfter);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    tellDetails(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), year, genreBefore,
                numberBefore, total);
    const PanelLine details = panelLine(panel, QStringLiteral("Details"));
    QTRY_VERIFY(details.line()->width() > 0);
    tellDetails(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), year, genreAfter,
                numberAfter, total);
    QVERIFY(handover->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    const QFontMetricsF metrics(details.font());
    // In the word that moves, in each line.
    const double inBefore = metrics.horizontalAdvance(movedBefore) - 1.0;
    const double inAfter = metrics.horizontalAdvance(movedAfter) - 1.0;
    QVERIFY(std::abs(inBefore - inAfter) > 1.0);
    for (const double at : {0.0, 0.5, 0.8}) {
        panel->setProperty("handover", at);
        QCOMPARE(details.stillTexts(), still);
        QCOMPARE(details.opacitiesAt(inBefore), std::make_pair(linesOut(at), linesIn(at)));
        QCOMPARE(details.opacitiesAt(inAfter), std::make_pair(linesOut(at), linesIn(at)));
        QVERIFY2(cutGlyph(details).isEmpty(), qPrintable(cutGlyph(details)));
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kWideLines));
}

// A line elided ever shorter, where what stays still is after the number
// that changes: no edge falls where the ellipsis is, or past the gap it
// stands for, and none cuts a glyph. What stays still past the year stays
// still up to a gap while the line shows one after it, and to the end of
// the line when every text is cut off within it alike; the year always
// fades.
void UiMotionTest::nowPlayingElidedLineKeepsWhatStaysStillAfterAChange() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    const QString genre = QStringLiteral("Contemporary Classical Chamber Music");
    tellDetails(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2019, genre, 4, 8);
    const PanelLine details = panelLine(panel, QStringLiteral("Details"));
    QTRY_VERIFY(details.line()->width() > 0);
    tellDetails(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2020, genre, 5, 8);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.25);
    const QString before = details.outgoingText();
    const QString after = details.text();
    const QFontMetricsF metrics(details.font());
    const double inYear = metrics.horizontalAdvance(QStringLiteral("20"));
    // What of `text` shows before the ellipsis at `width`.
    const auto shownUpTo = [&](const QString &text, double width) {
        QString shown = metrics.elidedText(text, Qt::ElideRight, width);
        if (shown != text) {
            shown.chop(1);
        }
        return metrics.horizontalAdvance(shown);
    };

    int toAGap = 0;
    int toTheEnd = 0;
    for (int width = static_cast<int>(std::ceil(metrics.horizontalAdvance(before))) + 4; width > 40; --width) {
        // The sidebar narrowing as the window does.
        details.line()->setWidth(width);
        const QString where = QStringLiteral(" at %1 px").arg(width);
        QVERIFY2(!details.stillAt(inYear), qPrintable(QStringLiteral("the year is still") + where));
        QVERIFY2(details.opacitiesAt(inYear) == std::make_pair(linesOut(0.25), linesIn(0.25)),
                 qPrintable(QStringLiteral("year") + where));
        QVERIFY2(cutGlyph(details).isEmpty(), qPrintable(cutGlyph(details) + where));
        for (const Window &window : details.stillWindows()) {
            if (window.second < width) {
                QVERIFY2(window.second <= shownUpTo(before, width) && window.second <= shownUpTo(after, width),
                         qPrintable(QStringLiteral("still edge %1").arg(window.second) + where));
                ++toAGap;
            } else if (metrics.horizontalAdvance(after) > width) {
                // Cut off within it, as the line before is.
                QVERIFY2(metrics.horizontalAdvance(before) > width, qPrintable(QStringLiteral("cut off") + where));
                ++toTheEnd;
            }
        }
        if (width > metrics.horizontalAdvance(before)) {
            QCOMPARE(details.stillTexts(),
                     (QStringList{QStringLiteral("· ") + genre + QStringLiteral(" · Track"), QStringLiteral("of 8")}));
        }
    }
    QVERIFY(toAGap > 0);
    QVERIFY(toTheEnd > 0);
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
}

// A details line with a genre in Hebrew is laid out from the right: when
// only its year changes, none of it stays still, and the whole of it
// cross-fades, as one.
void UiMotionTest::nowPlayingRightToLeftDetailsFadeWhole() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    tellDetails(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2019, QStringLiteral("רוק"), 0, 0);
    const PanelLine details = panelLine(panel, QStringLiteral("Details"));
    QTRY_VERIFY(details.line()->width() > 0);
    tellDetails(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2020, QStringLiteral("רוק"), 0, 0);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(details.outgoingText(), QStringLiteral("2019 · רוק"));
    QCOMPARE(details.text(), QStringLiteral("2020 · רוק"));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.0, 0.3, 0.7}) {
        panel->setProperty("handover", at);
        QCOMPARE(details.stillWindows(), QList<Window>());
        const QList<QQuickItem *> parts = details.parts();
        QCOMPARE(parts.size(), 1);
        QCOMPARE(parts.first()->x(), 0.0);
        QCOMPARE(parts.first()->width(), details.line()->width());
        QCOMPARE(parts.first()->opacity(), linesOut(at));
        QCOMPARE(details.incomingWindows(), (QList<Window>{{0.0, details.line()->width()}}));
        QCOMPARE(details.incomingOpacity(), linesIn(at));
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kWideLines));
}

void UiMotionTest::nowPlayingInterruptionKeepsWhatStaysStill_data() {
    QTest::addColumn<QList<int>>("years");
    QTest::addColumn<QStringList>("genres");
    QTest::addColumn<QList<int>>("numbers");
    QTest::addColumn<int>("total");
    // What stays still from B's arrival, and from C's.
    QTest::addColumn<QStringList>("stillForB");
    QTest::addColumn<QStringList>("stillForC");
    // Where a word that stayed still and changes for C is.
    QTest::addColumn<QString>("givenUp");
    QTest::newRow("the number") << QList<int>{0, 0, 0} << QStringList{QString(), QString(), QString()}
                                << QList<int>{4, 5, 6} << 8 << QStringList{"Track", "of 8"}
                                << QStringList{"Track", "of 8"} << QString();
    QTest::newRow("a field after the year") << QList<int>{2019, 2020, 2021}
                                            << QStringList{"Synth-pop", "Synth-pop", "Pop"} << QList<int>{0, 0, 0}
                                            << 0 << QStringList{"· Synth-pop"} << QStringList{"·"}
                                            << "2019 · Syn";
}

// A → B, then C while A goes out or while B comes in (#463 with #537):
// every layer goes on from what it showed, what stays still for all three
// stays still, at full opacity, all along, and what C no longer has in
// place goes out whole, in B's layer, as it showed. A layer at 0 is
// dropped. C's text comes in once all of it is out. Nothing cuts a glyph.
void UiMotionTest::nowPlayingInterruptionKeepsWhatStaysStill() {
    QFETCH(QList<int>, years);
    QFETCH(QStringList, genres);
    QFETCH(QList<int>, numbers);
    QFETCH(int, total);
    QFETCH(QStringList, stillForB);
    QFETCH(QStringList, stillForC);
    QFETCH(QString, givenUp);
    for (const double interruptAt : {0.25, 0.75}) {
        const QString where = QStringLiteral(" (C at %1)").arg(interruptAt);
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
        units->setLongDuration(1000);
        const QStringList paths{kTrackA1, kTrackA2, QStringLiteral("/music/Artist A/Album A/03.flac")};
        const QStringList titles{QStringLiteral("First"), QStringLiteral("Second"), QStringLiteral("Third")};
        // Another year is another album.
        const auto album = [&](int index) {
            return years.at(index) == years.first() ? QStringLiteral("Album A")
                                                    : QStringLiteral("Album %1").arg(index);
        };
        const auto tell = [&](int index) {
            tellDetails(bridge, paths.at(index), titles.at(index), album(index), years.at(index),
                        genres.at(index), numbers.at(index), total);
        };
        tell(0);
        const PanelLine details = panelLine(panel, QStringLiteral("Details"));
        QTRY_VERIFY(details.line()->width() > 0);
        tell(1);
        QVERIFY(handover->property("running").toBool());
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        QCOMPARE(details.stillTexts(), stillForB);
        panel->setProperty("handover", interruptAt);
        // A as far as it has gone out, B as far as it has come in.
        const double leftOfA = linesOut(interruptAt);
        const double inOfB = linesIn(interruptAt);
        QVERIFY2((leftOfA > 0.0) != (inOfB > 0.0), qPrintable(where));
        const PanelLook before = panelLook(panel, kWideLines);
        const QString textB = details.text();

        tell(2);
        QVERIFY(handover->property("running").toBool());
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        QCOMPARE(panel->property("handover").toDouble(), 0.0);
        // At once: every layer where it was, drawing what it drew.
        const QString change = lookChange(before, panelLook(panel, kWideLines), kWideLines, true, false);
        QVERIFY2(change.isEmpty(), qPrintable(change + where));
        QCOMPARE(details.stillTexts(), stillForC);
        // A's layer while it still shows; B's while it shows. The newest
        // draws what goes out whole: what stayed still is the same in both;
        // B's holds it when A's is gone.
        const QList<QQuickItem *> layers = layersOf(details);
        const bool hasA = leftOfA > 0.0;
        const bool hasB = inOfB > 0.0 || (!givenUp.isEmpty() && !hasA);
        QCOMPARE(layers.size(), (hasA ? 1 : 0) + (hasB ? 1 : 0));
        if (hasB) {
            QCOMPARE(layers.last()->property("text").toString(), textB);
        }
        QCOMPARE(details.incomingOpacity(), 0.0);
        QVERIFY2(cutGlyph(details).isEmpty(), qPrintable(cutGlyph(details) + where));
        const QFontMetricsF metrics(details.font());
        const double inGivenUp = metrics.horizontalAdvance(givenUp);
        for (const double at : {0.25, 0.5, 0.9}) {
            panel->setProperty("handover", at);
            const double level = linesOut(at);
            // A's and B's pieces from where they were, and C's text from 0.
            if (!givenUp.isEmpty()) {
                QCOMPARE(details.opacitiesAt(inGivenUp), std::make_pair(level, linesIn(at)));
            }
            for (qsizetype index = 0; index < layers.size(); ++index) {
                const double from = hasA && index == 0 ? leftOfA : inOfB;
                for (QQuickItem *part : partsOf(details, layers.at(index))) {
                    const bool whole = index == layers.size() - 1 && !givenUp.isEmpty()
                        && inGivenUp >= part->x() && inGivenUp < part->x() + part->width();
                    QVERIFY(sameOpacity(part->opacity(), (whole ? 1.0 : from) * level));
                }
            }
            QCOMPARE(details.incomingOpacity(), linesIn(at));
            QCOMPARE(details.stillTexts(), stillForC);
            QVERIFY2(cutGlyph(details).isEmpty(), qPrintable(cutGlyph(details) + where));
        }
        QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
        QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
        QVERIFY(panelIdle(panel, kWideLines));
    }
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
// lines: the cover joins them, over longDuration, the cover on its curve
// over all of it, the lines on theirs.
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
    // Laid out: a line draws what fades in pieces of its width.
    QTRY_VERIFY(panelLine(panel, QStringLiteral("Title")).line()->width() > 0);
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
    // Along the way, together.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.25, 0.5, 0.75}) {
        panel->setProperty("handover", at);
        QCOMPARE(outgoingCover->opacity(), coversOut(at));
        QCOMPARE(title.outgoingOpacity(), linesOut(at));
        QCOMPARE(title.incomingOpacity(), linesIn(at));
    }
    // The album line is the same: it stays still all along.
    QVERIFY(lineStill(panel, QStringLiteral("ArtistAlbum")));
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }));
    QVERIFY(!outgoingCover->isVisible());
    QVERIFY(allLinesStill(panel));
}

void UiMotionTest::nowPlayingRapidChangesFadeFromWhatShows_data() {
    QTest::addColumn<double>("at");
    // C comes while A goes out.
    QTest::newRow("early") << 0.3;
    // C comes while B comes in.
    QTest::newRow("late") << 0.7;
}

// A → B → C within one fade: the next fade starts from what shows, A's
// layers or B's as far as each got, never from a track that was gone, and
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
    // A's title as far as it went out, or B's as far as it came in, where
    // it was; C's over it, to come in once it is out.
    const QList<QQuickItem *> layers = layersOf(title);
    QCOMPARE(layers.size(), 1);
    const bool early = linesOut(at) > 0.0;
    QCOMPARE(partText(title, partsOf(title, layers.first()).first()),
             early ? QStringLiteral("First") : QStringLiteral("Second"));
    QCOMPARE(partsOf(title, layers.first()).first()->opacity(), early ? linesOut(at) : linesIn(at));
    QCOMPARE(title.incomingOpacity(), 0.0);
    // B's cover whole, as it showed, under A's.
    const QList<std::pair<QString, double>> cover = coverLook(panel);
    QCOMPARE(cover.size(), 2);
    QCOMPARE(cover.at(0).first, covers.second);
    QCOMPARE(cover.at(0).second, 1.0);
    QCOMPARE(cover.at(1).first, covers.first);
    QCOMPARE(cover.at(1).second, coversOut(at));

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
    QCOMPARE(easingOf(handover), QEasingCurve::Linear);
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
    QTest::addColumn<double>("at");
    // The cover comes while the lines before go out.
    QTest::newRow("before half") << 0.25;
    // The cover comes while the playing track's lines come in.
    QTest::newRow("after half") << 0.75;
}

// The next track's own cover comes while its lines fade in: no layer jumps.
// The lines go on from the opacity they have onto the cover's long curve;
// the cover before, which showed whole, fades out from 1.
void UiMotionTest::nowPlayingLateCoverKeepsEveryLayerContinuous() {
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
    QVERIFY(units->longDuration() != units->shortDuration());
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    // Laid out: a line draws what fades in pieces of its width.
    QTRY_VERIFY(panelLine(panel, QStringLiteral("Title")).line()->width() > 0);
    showTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), covers.first);
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->shortDuration());
    const PanelLine title = panelLine(panel, QStringLiteral("Title"));
    QQuickItem *outgoingCover = itemNamed(panel, QStringLiteral("albumArtOutgoing"));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", at);
    const double outgoingBefore = title.outgoingOpacity();
    const double liveBefore = title.incomingOpacity();
    QCOMPARE(outgoingBefore, linesOut(at));
    QCOMPARE(liveBefore, linesIn(at));
    QVERIFY(!outgoingCover->isVisible());

    bridge->setProperty("currentTrackCoverPath", covers.second);
    // At once: every layer where it was; the cover before shows whole over
    // the new one, as it did before.
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(title.fades(), outgoingBefore > 0.0);
    if (title.fades()) {
        QCOMPARE(textOf(panel, "nowPlayingOutgoingTitle"), QStringLiteral("First"));
    }
    QCOMPARE(title.outgoingOpacity(), outgoingBefore);
    QCOMPARE(title.incomingOpacity(), liveBefore);
    QVERIFY(outgoingCover->isVisible());
    QCOMPARE(outgoingCover->opacity(), 1.0);
    QCOMPARE(outgoingCover->childItems().first()->property("source").toUrl().toString(), covers.first);

    // Then each goes one way only, on its curve, to the playing track: the
    // lines before out, then the playing track's in from where they were.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    double lines = outgoingBefore;
    double live = liveBefore;
    double cover = 1.0;
    for (const double next : {0.25, 0.5, 0.9}) {
        panel->setProperty("handover", next);
        const double nextLines = title.outgoingOpacity();
        const double nextLive = title.incomingOpacity();
        QVERIFY(outgoingCover->isVisible());
        const double nextCover = outgoingCover->opacity();
        QVERIFY(nextLines <= lines);
        QVERIFY(nextLive >= live);
        QVERIFY(nextCover < cover);
        QCOMPARE(nextLines, outgoingBefore * linesOut(next));
        QCOMPARE(nextLive, 1.0 - (1.0 - liveBefore) * (1.0 - linesIn(next)));
        QCOMPARE(nextCover, coversOut(next));
        QVERIFY(nextLines == 0.0 || nextLive == 0.0);
        QVERIFY(lineStill(panel, QStringLiteral("ArtistAlbum")));
        lines = nextLines;
        live = nextLive;
        cover = nextCover;
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(!outgoingCover->isVisible());
    QVERIFY(allLinesStill(panel));
}

void UiMotionTest::nowPlayingThirdTrackGoesOnFromEveryLayer_data() {
    QTest::addColumn<double>("at");
    // C comes while A's lines go out.
    QTest::newRow("before half") << 0.3;
    // C comes while B's lines come in.
    QTest::newRow("after half") << 0.7;
}

// A → B, B's own cover late, then C during that joined fade (#463): C's fade
// starts from what shows. A's lines go on out from where they got to; B's,
// as far as they came in, go out as layers of their own, each part from
// where it got to and cut in B's own gaps; what stayed still and C's lines
// change, B's go out whole; B's cover goes out whole, as it showed, under
// A's. No layer jumps, each then goes one way only, C's lines come in once
// the others are out, and the fade ends on C alone.
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
    panel->setProperty("handover", 0.25);
    // B's own cover, late: it joins the fade.
    bridge->setProperty("currentTrackCoverPath", covers.all.at(1));
    QCOMPARE(handover->property("duration").toInt(), 1500);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", at);
    // A's lines, from where they were when the cover came, going out; B's
    // coming in once they are out.
    const double a = linesOut(0.25) * linesOut(at);
    const double b = linesIn(at);
    QVERIFY((a > 0.0) != (b > 0.0));
    QCOMPARE(title.outgoingOpacity(), a);
    QCOMPARE(title.incomingOpacity(), b);
    QCOMPARE(details.stillTexts(), QStringList{QStringLiteral("2005 · Rock · Track")});
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

    // The title: A's as far as it went out, or B's as far as it came in;
    // C's over it, from 0.
    QCOMPARE(title.text(), QStringLiteral("Third"));
    const QList<QQuickItem *> titleLayers = layersOf(title);
    QCOMPARE(titleLayers.size(), 1);
    QCOMPARE(partText(title, partsOf(title, titleLayers.first()).first()),
             a > 0.0 ? QStringLiteral("First") : QStringLiteral("Second"));
    const double titleFrom = a > 0.0 ? a : b;
    QCOMPARE(partsOf(title, titleLayers.first()).first()->opacity(), titleFrom);
    QCOMPARE(title.incomingOpacity(), 0.0);
    // C's details have another year, of as many digits: what A's and B's
    // kept still past it stays still. B's year goes out whole, as it
    // showed, and B's number as far as it came in; A's year draws nothing,
    // which B's covers, and A's number goes on where it was.
    QCOMPARE(details.stillTexts(), QStringList{QStringLiteral("· Rock · Track")});
    const QList<QQuickItem *> detailsLayers = layersOf(details);
    QCOMPARE(detailsLayers.size(), a > 0.0 ? 2 : 1);
    const QList<QQuickItem *> partsA = a > 0.0 ? partsOf(details, detailsLayers.first()) : QList<QQuickItem *>();
    const QList<QQuickItem *> partsB = partsOf(details, detailsLayers.last());
    QCOMPARE(partsA.size(), a > 0.0 ? 1 : 0);
    QCOMPARE(partsB.size(), b > 0.0 ? 2 : 1);
    QCOMPARE(partText(details, partsB.first()), QStringLiteral("2005 · Rock · Track 2"));
    const QFontMetricsF metrics(details.font());
    const QString shared = QStringLiteral("2005 · Rock · Track");
    QCOMPARE(partsB.at(0)->x(), 0.0);
    QVERIFY(partsB.at(0)->width() > metrics.horizontalAdvance(QStringLiteral("2005")));
    QVERIFY(partsB.at(0)->width() < metrics.horizontalAdvance(QStringLiteral("2005 ")));
    QCOMPARE(partsB.at(0)->opacity(), 1.0);
    // Where a number starts: in the gap after what all share.
    const auto inTheNumbersGap = [&](QQuickItem *part) {
        return part->x() > metrics.horizontalAdvance(shared)
            && part->x() < metrics.horizontalAdvance(shared + QLatin1Char(' '));
    };
    if (b > 0.0) {
        QVERIFY(inTheNumbersGap(partsB.at(1)));
        QCOMPARE(partsB.at(1)->opacity(), b);
    }
    if (a > 0.0) {
        QCOMPARE(partText(details, partsA.first()), QStringLiteral("2005 · Rock · Track 1"));
        QVERIFY(inTheNumbersGap(partsA.first()));
        QCOMPARE(partsA.first()->opacity(), a);
    }
    QCOMPARE(details.incomingOpacity(), 0.0);
    QVERIFY2(cutGlyph(details).isEmpty(), qPrintable(cutGlyph(details)));
    // The album line was the same for A and B: B's goes out from full
    // opacity, past the artist they share with C's.
    QCOMPARE(layersOf(artistAlbum).size(), 1);
    QCOMPARE(artistAlbum.outgoingOpacity(), 1.0);
    QCOMPARE(artistAlbum.stillTexts(), QStringList{QStringLiteral("Artist A —")});
    // The covers: B's whole over C's, A's over it where it was.
    QList<std::pair<QString, double>> cover = coverLook(panel);
    QCOMPARE(cover.size(), 2);
    QCOMPARE(cover.at(0).first, covers.all.at(1));
    QCOMPARE(cover.at(1).first, covers.all.at(0));
    QCOMPARE(cover.at(1).second, coversOut(at));

    // Then each goes one way only, on its curve, to C.
    for (const double next : {0.25, 0.5, 0.9}) {
        panel->setProperty("handover", next);
        const double level = linesOut(next);
        QCOMPARE(partsOf(title, titleLayers.first()).first()->opacity(), titleFrom * level);
        QCOMPARE(title.incomingOpacity(), linesIn(next));
        QCOMPARE(partsB.at(0)->opacity(), level);
        if (b > 0.0) {
            QCOMPARE(partsB.at(1)->opacity(), b * level);
        }
        if (a > 0.0) {
            QCOMPARE(partsA.first()->opacity(), a * level);
        }
        QCOMPARE(details.incomingOpacity(), linesIn(next));
        QCOMPARE(artistAlbum.outgoingOpacity(), level);
        cover = coverLook(panel);
        QCOMPARE(cover.size(), 3);
        QCOMPARE(cover.at(0).first, covers.all.at(2));
        QCOMPARE(cover.at(1).first, covers.all.at(1));
        QCOMPARE(cover.at(1).second, coversOut(next));
        QCOMPARE(cover.at(2).second, coversOut(at) * coversOut(next));
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
        // While C's lines come in: A's are out, and C's go out from where
        // they got to; the covers are two already.
        {QStringLiteral("/music/Artist A/Album D/01.flac"), QStringLiteral("Fourth"), QStringLiteral("Album D"), 2004,
         0.75, true, false},
        // While C's lines go out: D's, at 0, are dropped.
        {QStringLiteral("/music/Artist A/Album E/01.flac"), QStringLiteral("Fifth"), QStringLiteral("Album E"), 2005,
         0.25, false, false},
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
        const QStringList expected =
            index <= 2 ? QStringList{QStringLiteral("First")} : QStringList{QStringLiteral("Third")};
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
    const QList<std::pair<QString, double>> fading{{QString(), 1.0}, {covers.first, coversOut(0.4)}};
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
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{
                                   {QString(), 1.0}, {covers.first, coversOut(0.4) * coversOut(0.5)}}));
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
    // A going out over B, around B's image over the colour behind it.
    const double leftOfA = coversOut(0.4);
    const QColor bare = mixed(squareColor, leftOfA, view);
    const QColor inImage = mixed(squareColor, leftOfA, wideColor);
    QTRY_VERIFY(near(drawn().first, bare) && near(drawn().second, inImage));

    // A again.
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, covers.first);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{{covers.landscape, 1.0}, {covers.first, leftOfA}}));
    // B's cover shows at once, not its backdrop: the image cache has it, as
    // it showed.
    QVERIFY(loaded());
    const auto [above, middle] = drawn();
    QVERIFY2(near(above, bare), qPrintable(QStringLiteral("around B's image: %1, not %2").arg(above.name(), bare.name())));
    QVERIFY2(near(middle, inImage), qPrintable(QStringLiteral("in B's image: %1, not %2").arg(middle.name(), inImage.name())));

    // Half-way: B's cover going out over A's own, A's before going out over it.
    panel->setProperty("handover", 0.5);
    const QColor overA = mixed(squareColor, leftOfA * coversOut(0.5), mixed(view, coversOut(0.5), squareColor));
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

void UiMotionTest::nowPlayingNoFrameShowsTwoTextsInALine_data() {
    QTest::addColumn<bool>("narrow");
    QTest::newRow("wide") << false;
    QTest::newRow("narrow") << true;
}

// A track change never draws two texts over each other in a line (#373 H1:
// "Hollow Arc" over "Moth Copper"): at every point of every line, at most
// one text shows, in every frame and at every point of the fade the test
// sets, through the next track of an album, another album, metadata that
// comes late, and a third track while the text before goes out or the next
// comes in.
void UiMotionTest::nowPlayingNoFrameShowsTwoTextsInALine() {
    QFETCH(bool, narrow);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    if (narrow) {
        window->resize(1000, 720);
    }
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    // Long enough for frames in each half.
    units->setShortDuration(400);
    units->setLongDuration(600);
    const QStringList names = narrow ? kNarrowLines : kWideLines;
    bridge->setProperty("currentTrackArtist", QStringLiteral("Lantern Signal"));
    bridge->setProperty("currentTrackGenre", QStringLiteral("Ambient"));
    tellTrack(bridge, kTrackA1, QStringLiteral("Moth Copper"), QStringLiteral("Album A"), 2005, 1, covers.all.at(0));
    QTRY_COMPARE(panel->property("layoutMode").toString(), narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QTRY_VERIFY(panelLine(panel, names.first()).line()->width() > 0);
    QVERIFY(panelIdle(panel, names));

    // Where a line shows two texts at once, or "".
    const auto twoTexts = [&]() -> QString {
        for (const QString &name : names) {
            const PanelLine line = panelLine(panel, name);
            for (double x = 0.5; x < line.line()->width(); x += 1.0) {
                QList<Drawn> drawn = drawnAt(line, x);
                drawn.removeIf([](const Drawn &each) { return each.glyph.isNull(); });
                if (drawn.size() > 1) {
                    QStringList shown;
                    for (const Drawn &each : drawn) {
                        shown.push_back(QStringLiteral("%1 %2").arg(each.glyph).arg(each.opacity));
                    }
                    return QStringLiteral("%1 at %2 px, handover %3: [%4]")
                        .arg(name).arg(x).arg(panel->property("handover").toDouble()).arg(shown.join(", "));
                }
            }
        }
        return {};
    };
    QString failure;
    int frames = 0;
    const QMetaObject::Connection connection =
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [&]() {
            if (failure.isEmpty() && handover->property("running").toBool()) {
                ++frames;
                failure = twoTexts();
            }
        });
    // Along a fade at the points the test sets, then to its end in frames.
    const auto followTheFade = [&](const char *what) {
        QVERIFY2(handover->property("running").toBool(), what);
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        for (const double at : {0.0, 0.1, 0.25, 0.45, 0.5, 0.55, 0.75, 0.95}) {
            panel->setProperty("handover", at);
            const QString each = twoTexts();
            QVERIFY2(each.isEmpty(), qPrintable(QString::fromLatin1(what) + QStringLiteral(": ") + each));
        }
        panel->setProperty("handover", 0.0);
        QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
        QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
        QVERIFY2(failure.isEmpty(), qPrintable(QString::fromLatin1(what) + QStringLiteral(": ") + failure));
        QVERIFY(panelIdle(panel, names));
    };

    // The next track of the album.
    tellTrack(bridge, kTrackA2, QStringLiteral("Hollow Arc"), QStringLiteral("Album A"), 2005, 2, covers.all.at(0));
    followTheFade("the next track");

    // Another album, with its cover.
    bridge->setProperty("currentTrackArtist", QStringLiteral("Mlantern Signal"));
    tellTrack(bridge, kTrackB1, QStringLiteral("Moth Copper"), QStringLiteral("Album B"), 2007, 1, covers.all.at(1));
    followTheFade("another album");

    // Metadata that comes late, in each half of the fade.
    for (const double lateAt : {0.25, 0.75}) {
        tellTrack(bridge, lateAt < 0.5 ? kTrackA1 : kTrackA2, QStringLiteral("Hollow Arc"), QStringLiteral("Album B"),
                  2007, lateAt < 0.5 ? 2 : 3, covers.all.at(1));
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        panel->setProperty("handover", lateAt);
        bridge->setProperty("currentTrackGenre", lateAt < 0.5 ? QStringLiteral("Drone") : QStringLiteral("Ambient"));
        followTheFade("late metadata");
    }

    // A third track, while the text before goes out, and while the next
    // comes in.
    for (const double thirdAt : {0.25, 0.75}) {
        tellTrack(bridge, kTrackA1, QStringLiteral("Lantern"), QStringLiteral("Album A"), 2005, 4, covers.all.at(0));
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        panel->setProperty("handover", thirdAt);
        bridge->setProperty("currentTrackArtist", QStringLiteral("Signal"));
        tellTrack(bridge, kTrackB1, QStringLiteral("Copper Moth"), QStringLiteral("Album C"), 2009, 5,
                  covers.all.at(2));
        followTheFade("a third track");
    }
    QObject::disconnect(connection);
    QVERIFY(frames > 0);
}

void UiMotionTest::visualizationModeSwitchKeepsAPlotOnScreen_data() {
    QTest::addColumn<int>("displayMode");
    QTest::addColumn<int>("channels");
    QTest::addColumn<bool>("animated");
    QTest::newRow("rolling, stereo") << 0 << 2 << true;
    QTest::newRow("centered, stereo") << 1 << 2 << true;
    QTest::newRow("centered, 5.1") << 1 << 6 << true;
    QTest::newRow("rolling, 5.1") << 0 << 6 << true;
    QTest::newRow("animations off") << 1 << 2 << false;
}

// The visualization mode switch (#373 Motion; H1: a black frame on the way
// back to the spectrogram): in every frame, a plot that has its data shows
// whole. The chosen plot comes in over the one before, which stays whole
// under it until the chosen one can draw: the spectrogram, hidden, gives its
// columns up, and shown again waits for a fresh session's. Then it fades in
// over shortDuration on OutCubic, as one picture, and the plot before goes.
// With animations off it shows at once once it can draw, with no blank
// frame.
void UiMotionTest::visualizationModeSwitchKeepsAPlotOnScreen() {
    QFETCH(int, displayMode);
    QFETCH(int, channels);
    QFETCH(bool, animated);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    // Long enough for frames along the fade.
    units->setShortDuration(animated ? 400 : 0);
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QQuickItem *surface = named(window, "visualizationSurface");
    QQuickItem *spectrogram = named(window, "mainSpectrogramSurface");
    QQuickItem *waveform = named(window, "mainWaveformSurface");
    QVERIFY(viewer && surface && spectrogram && waveform);
    QObject *fade = qvariant_cast<QObject *>(surface->property("modeFadeAnimation"));
    QVERIFY(fade != nullptr);
    QCOMPARE(fade->property("duration").toInt(), units->shortDuration());
    QCOMPARE(easingOf(fade), QEasingCurve::OutCubic);

    bridge->setProperty("spectrogramDisplayMode", displayMode);
    bridge->setProperty("spectrogramViewMode", channels > 2 ? 1 : 0);
    bridge->setProperty("durationSeconds", 60.0);
    bridge->setProperty("currentTrackChannels", channels);
    playQueueRow(bridge, 0);
    // The waveform has its overview.
    bridge->setProperty("waveformCoverageSeconds", 60.0);
    bridge->setProperty("waveformComplete", true);
    QVERIFY(QMetaObject::invokeMethod(bridge, "precomputedSpectrogramChannelsReady", Q_ARG(int, channels),
                                      Q_ARG(bool, true)));
    // The spectrogram's lanes, and their columns.
    const auto panes = [&]() {
        QList<SpectrogramItem *> found;
        for (QQuickItem *item : itemsNamed(spectrogram, QStringLiteral("spectrogramPaneItem"))) {
            found.push_back(qobject_cast<SpectrogramItem *>(item));
        }
        return found;
    };
    QTRY_COMPARE(panes().size(), channels);
    const auto feedColumns = [&]() {
        constexpr int bins = 4'097;
        constexpr int columns = 200;
        const QList<SpectrogramItem *> lanes = panes();
        for (qsizetype index = 0; index < lanes.size(); ++index) {
            lanes.at(index)->feedPrecomputedChunk(QByteArray(bins * columns, '\x80'), bins,
                                                  static_cast<int>(index), columns, 0, 2'000, 48'000, 1'024,
                                                  false, true, 1, false);
        }
    };
    feedColumns();
    QTRY_VERIFY(!spectrogram->property("awaitingData").toBool());
    QVERIFY(!waveform->property("awaitingData").toBool());
    QTRY_VERIFY(spectrogram->isVisible() && !waveform->isVisible());
    QCOMPARE(spectrogram->opacity(), 1.0);

    // Whether a plot with data shows whole: visible, at full opacity.
    const auto whole = [](QQuickItem *plot) {
        return plot->isVisible() && plot->opacity() == 1.0 && !plot->property("awaitingData").toBool();
    };
    QString failure;
    int frames = 0;
    const QMetaObject::Connection connection =
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [&]() {
            ++frames;
            if (failure.isEmpty() && !whole(spectrogram) && !whole(waveform)) {
                failure = QStringLiteral("no plot whole at frame %1: spectrogram %2 at %3, waveform %4 at %5")
                              .arg(frames)
                              .arg(spectrogram->isVisible()).arg(spectrogram->opacity())
                              .arg(waveform->isVisible()).arg(waveform->opacity());
            }
        });
    const auto check = [&](const char *where) {
        QVERIFY2(whole(spectrogram) || whole(waveform), where);
        QVERIFY2(failure.isEmpty(), qPrintable(QString::fromLatin1(where) + QStringLiteral(": ") + failure));
    };

    // To the waveform, which has its data: it comes in over the
    // spectrogram at once, or with animations off replaces it.
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
    QVERIFY(waveform->isVisible());
    QCOMPARE(spectrogram->isVisible(), animated);
    check("to the waveform");
    if (animated) {
        QVERIFY(fade->property("running").toBool());
        QVERIFY(waveform->z() > spectrogram->z());
        QVERIFY(QMetaObject::invokeMethod(fade, "pause"));
        surface->setProperty("modeFade", 0.5);
        QCOMPARE(waveform->opacity(), 0.5);
        QCOMPARE(spectrogram->opacity(), 1.0);
        // Its floor and its lanes as one picture.
        QObject *layer = waveform->property("layer").value<QObject *>();
        QVERIFY(layer != nullptr && layer->property("enabled").toBool());
        QVERIFY(QMetaObject::invokeMethod(fade, "resume"));
        QTRY_VERIFY(!spectrogram->isVisible());
    } else {
        QVERIFY(!fade->property("running").toBool());
        QVERIFY(!spectrogram->isVisible());
    }
    QCOMPARE(waveform->opacity(), 1.0);
    check("on the waveform");

    // The spectrogram, hidden, gives its columns up, as BridgeClient has it
    // do when no pane shows.
    for (SpectrogramItem *pane : panes()) {
        pane->releaseStorage();
    }
    QTRY_VERIFY(spectrogram->property("awaitingData").toBool());

    // Back to the spectrogram: it shows, so that its analysis runs, but at
    // 0 under the waveform until its columns come.
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 0)));
    QVERIFY(spectrogram->isVisible());
    QCOMPARE(spectrogram->opacity(), 0.0);
    QVERIFY(whole(waveform));
    QVERIFY(!fade->property("running").toBool());
    for (SpectrogramItem *pane : panes()) {
        pane->retainStorage();
    }
    const int framesBefore = frames;
    window->update();
    QTRY_VERIFY(frames > framesBefore);
    QTest::qWait(100);
    check("waiting for the spectrogram");
    QVERIFY(whole(waveform));
    QCOMPARE(spectrogram->opacity(), 0.0);

    // Its first columns: it comes in over the waveform.
    feedColumns();
    QTRY_VERIFY(!spectrogram->property("awaitingData").toBool());
    check("the spectrogram's data");
    if (animated) {
        QVERIFY(fade->property("running").toBool());
        QVERIFY(spectrogram->z() > waveform->z());
        QVERIFY(QMetaObject::invokeMethod(fade, "pause"));
        surface->setProperty("modeFade", 0.5);
        QCOMPARE(spectrogram->opacity(), 0.5);
        QVERIFY(whole(waveform));
        QVERIFY(QMetaObject::invokeMethod(fade, "resume"));
        QTRY_VERIFY(!waveform->isVisible());
    } else {
        QVERIFY(!fade->property("running").toBool());
        QVERIFY(!waveform->isVisible());
    }
    QCOMPARE(spectrogram->opacity(), 1.0);
    check("on the spectrogram");

    // Back before the spectrogram can draw: the waveform, which never
    // left, stays, and the spectrogram goes.
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
    if (animated) {
        QTRY_VERIFY(!spectrogram->isVisible());
    }
    for (SpectrogramItem *pane : panes()) {
        pane->releaseStorage();
    }
    QTRY_VERIFY(spectrogram->property("awaitingData").toBool());
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 0)));
    QCOMPARE(spectrogram->opacity(), 0.0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
    QVERIFY(!spectrogram->isVisible());
    QVERIFY(whole(waveform));
    QVERIFY(!fade->property("running").toBool());
    check("back to the waveform");

    QObject::disconnect(connection);
    QVERIFY(frames > 0);

    // Stopped, nothing is waited for: the spectrogram, with no columns,
    // shows at once.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 0)));
    QVERIFY(spectrogram->isVisible());
    if (animated) {
        QVERIFY(fade->property("running").toBool());
    }
    QTRY_VERIFY(!waveform->isVisible());
    QCOMPARE(spectrogram->opacity(), 1.0);
}

void UiMotionTest::visualizationPaneIsTheViewUntilTheFirstData_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("animated");
    QTest::newRow("light") << false << true;
    QTest::newRow("light, animations off") << false << false;
    QTest::newRow("dark") << true << true;
}

// The empty pane (#373 H2, P1): in the light theme, before the session's
// first audio data, the pane is the view colour, not a slab of the
// spectrogram's black floor; what is drawn on it, the mode switch, takes
// the colours of a plot on the view. The first data fades it out over
// longDuration into the plot's floor and data, at once with animations
// off, and the floor stays the plot's from then on, also once playback
// stops. The dark theme's pane is its floor throughout.
void UiMotionTest::visualizationPaneIsTheViewUntilTheFirstData() {
    QFETCH(bool, dark);
    QFETCH(bool, animated);
    const QPalette before = QGuiApplication::palette();
    if (dark) {
        // Breeze Dark's window, view and text.
        QPalette palette = before;
        palette.setColor(QPalette::Window, QColor(QStringLiteral("#202326")));
        palette.setColor(QPalette::Base, QColor(QStringLiteral("#141618")));
        palette.setColor(QPalette::Text, QColor(QStringLiteral("#fcfcfc")));
        palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#fcfcfc")));
        QGuiApplication::setPalette(palette);
    }
    const auto restorePalette = qScopeGuard([&]() { QGuiApplication::setPalette(before); });
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setShortDuration(animated ? 150 : 0);
    units->setLongDuration(animated ? 400 : 1);
    auto *tokens = qvariant_cast<QObject *>(window->property("uiPalette"));
    QVERIFY(tokens != nullptr);
    QCOMPARE(tokens->property("themeIsDark").toBool(), dark);
    const QColor view = tokens->property("view").value<QColor>();
    QObject *bridge = bridgeOf(window);
    QQuickItem *pane = named(window, "spectrogramPane");
    QQuickItem *layer = named(window, "plotLayer");
    QQuickItem *cover = named(window, "plotViewFloor");
    QQuickItem *spectrogram = named(window, "mainSpectrogramSurface");
    QVERIFY(pane && layer && cover && spectrogram);
    auto *spectrogramSet = qvariant_cast<QObject *>(tokens->property("spectrogramPlot"));
    auto *waveformSet = qvariant_cast<QObject *>(tokens->property("waveformPlot"));
    const QColor black = spectrogramSet->property("plotBg").value<QColor>();
    QCOMPARE(black, QColor(Qt::black));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(viewer != nullptr);
    QCOMPARE(viewer->property("visualizationMode").toInt(), 0);

    // Clear of the mode switch in the top right corner.
    const auto drawnInPane = [&]() {
        const QImage image = window->grabWindow();
        const qreal scale = image.width() / static_cast<qreal>(window->width());
        const QPointF at = pane->mapToScene(QPointF(pane->width() * 0.25, pane->height() * 0.6)) * scale;
        return image.pixelColor(at.toPoint());
    };

    // Before any audio.
    QTest::qWait(50);
    if (dark) {
        QVERIFY(!cover->isVisible());
        QCOMPARE(pane->property("color").value<QColor>(), black);
        QCOMPARE(layer->property("plotColors").value<QObject *>(), spectrogramSet);
        QCOMPARE(drawnInPane(), black);
    } else {
        QVERIFY(cover->isVisible());
        QCOMPARE(cover->opacity(), 1.0);
        QCOMPARE(cover->property("color").value<QColor>(), view);
        QCOMPARE(pane->property("color").value<QColor>(), view);
        QCOMPARE(layer->property("plotColors").value<QObject *>(), waveformSet);
        QTRY_COMPARE(drawnInPane(), view);
        // The mode switch shows on it.
        QQuickItem *modeSwitch = itemNamed(layer, QStringLiteral("visualizationModeSwitch"));
        QVERIFY(modeSwitch != nullptr);
        QVERIFY(modeSwitch->isVisible());
        QVERIFY(modeSwitch->z() > cover->z());
    }

    // A track, before its data: the pane stays as it is.
    bridge->setProperty("durationSeconds", 60.0);
    bridge->setProperty("currentTrackChannels", 2);
    playQueueRow(bridge, 0);
    QTest::qWait(50);
    QCOMPARE(cover->isVisible(), !dark);

    // The first data: the spectrogram's columns.
    QVERIFY(QMetaObject::invokeMethod(bridge, "precomputedSpectrogramChannelsReady", Q_ARG(int, 2),
                                      Q_ARG(bool, true)));
    QList<SpectrogramItem *> lanes;
    QTRY_VERIFY([&]() {
        lanes.clear();
        for (QQuickItem *item : itemsNamed(spectrogram, QStringLiteral("spectrogramPaneItem"))) {
            lanes.push_back(qobject_cast<SpectrogramItem *>(item));
        }
        return lanes.size() == 2;
    }());
    constexpr int bins = 4'097;
    constexpr int columns = 200;
    for (qsizetype index = 0; index < lanes.size(); ++index) {
        lanes.at(index)->feedPrecomputedChunk(QByteArray(bins * columns, '\x80'), bins, static_cast<int>(index),
                                              columns, 0, 2'000, 48'000, 1'024, false, true, 1, false);
    }
    QTRY_VERIFY(!spectrogram->property("awaitingData").toBool());
    // The floor is the plot's, and what is drawn on it in the plot's
    // colours, at once.
    QCOMPARE(pane->property("color").value<QColor>(), black);
    QCOMPARE(layer->property("plotColors").value<QObject *>(), spectrogramSet);
    if (!dark && animated) {
        // The view fades out over longDuration.
        QVERIFY(cover->isVisible());
        QTRY_VERIFY(cover->opacity() > 0.05 && cover->opacity() < 0.95);
        QTRY_VERIFY_WITH_TIMEOUT(!cover->isVisible(), 2000);
    } else {
        QVERIFY(!cover->isVisible());
    }
    QCOMPARE(cover->opacity(), 0.0);
    QVERIFY(drawnInPane() != view || dark);

    // Stopped, with the track's data gone, the floor stays the plot's.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    bridge->setProperty("currentTrackPath", QString());
    for (SpectrogramItem *lane : lanes) {
        lane->releaseStorage();
    }
    QTRY_VERIFY(spectrogram->property("awaitingData").toBool());
    QTest::qWait(50);
    QVERIFY(!cover->isVisible());
    QCOMPARE(pane->property("color").value<QColor>(), black);
}

void UiMotionTest::viewerControlsFadeWithTheStrip_data() {
    QTest::addColumn<bool>("wholeScreen");
    QTest::newRow("whole screen") << true;
    QTest::newRow("in the window") << false;
}

// The viewer's controls hide and show together (#373 H1: the strip faded,
// the corner controls cut): the title strip over the whole screen, the
// corner plate in the window, the mode switch and the lanes' Mute and Solo
// all fade on one curve, at the same opacity in every frame, shown at once
// for the key that wakes them and hidden only once faded out.
void UiMotionTest::viewerControlsFadeWithTheStrip() {
    QFETCH(bool, wholeScreen);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    // Long enough for frames along the fade.
    units->setShortDuration(400);
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide = window->findChild<QObject *>(QStringLiteral("spectrogramFullscreenControlsAutoHide"));
    QQuickItem *spectrogram = named(window, "mainSpectrogramSurface");
    QVERIFY(viewer && autoHide && spectrogram);
    // Two named lanes, with Mute and Solo always shown.
    bridge->setProperty("viewerFullscreenMode", wholeScreen ? 1 : 0);
    bridge->setProperty("spectrogramViewMode", 1);
    bridge->setProperty("channelButtonsVisibility", 2);
    bridge->setProperty("currentTrackChannels", 2);
    playQueueRow(bridge, 0, QStringLiteral("Paused"));
    QVERIFY(QMetaObject::invokeMethod(bridge, "precomputedSpectrogramChannelsReady", Q_ARG(int, 2),
                                      Q_ARG(bool, true)));
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
    QQuickWindow *host = window;
    if (wholeScreen) {
        QTRY_VERIFY(spectrogram->window() != nullptr && spectrogram->window() != window);
        host = spectrogram->window();
        QVERIFY(QTest::qWaitForWindowExposed(host));
    } else {
        QTRY_VERIFY(itemNamed(window->contentItem(), QStringLiteral("spectrogramPopupControls")) != nullptr);
    }
    // The controls that hide with the viewer's: the strip, or the corner
    // plate, then the mode switch and the lanes' buttons.
    QQuickItem *controls = itemNamed(host->contentItem(), wholeScreen ? QStringLiteral("spectrogramFullscreenStrip")
                                                                       : QStringLiteral("spectrogramPopupControls"));
    QQuickItem *modeSwitch = itemNamed(host->contentItem(), QStringLiteral("visualizationModeSwitch"));
    QVERIFY(controls && modeSwitch);
    QList<QQuickItem *> followers{modeSwitch};
    QTRY_COMPARE(itemsNamed(host->contentItem(), QStringLiteral("plotLaneButtons")).size(), 2);
    followers.append(itemsNamed(host->contentItem(), QStringLiteral("plotLaneButtons")));
    // The pointer resting on the view, which reveals the mode switch.
    QTest::mouseMove(host, QPoint(host->width() / 2, host->height() / 2));
    QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    QTRY_COMPARE(controls->opacity(), 1.0);
    for (QQuickItem *follower : followers) {
        QTRY_COMPARE(follower->opacity(), 1.0);
        QVERIFY(follower->isVisible());
    }

    QString failure;
    int frames = 0;
    // Where a follower is not at the controls' opacity, or "".
    const auto apart = [&]() -> QString {
        for (QQuickItem *follower : followers) {
            if (std::abs(follower->opacity() - controls->opacity()) > 1e-9
                || follower->isVisible() != controls->isVisible()) {
                return QStringLiteral("%1 at %2 (%3), the controls at %4 (%5)")
                    .arg(follower->objectName()).arg(follower->opacity()).arg(follower->isVisible())
                    .arg(controls->opacity()).arg(controls->isVisible());
            }
        }
        return {};
    };
    const QMetaObject::Connection connection = QObject::connect(host, &QQuickWindow::frameSwapped, host, [&]() {
        ++frames;
        if (failure.isEmpty()) {
            failure = apart();
        }
    });

    // The pointer resting: they fade out together, and hide once out.
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QVERIFY(controls->isVisible());
    QTRY_VERIFY(controls->opacity() > 0.0 && controls->opacity() < 1.0);
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QTRY_VERIFY(!controls->isVisible());
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QVERIFY2(failure.isEmpty(), qPrintable(failure));

    // A key wakes them: shown at once, fading in together.
    autoHide->setProperty("hideDelay", 600000);
    QVERIFY(QMetaObject::invokeMethod(autoHide, "wake"));
    QVERIFY(controls->isVisible());
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QTRY_VERIFY(controls->opacity() > 0.0 && controls->opacity() < 1.0);
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QTRY_COMPARE(controls->opacity(), 1.0);
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(frames > 0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

#include "tst_ui_motion.moc"
