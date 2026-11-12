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
#include <QProcess>
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
#include <cstdio>
#include <functional>
#include <utility>

#include "../src/InputModality.h"
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
    // A larger font raises the window's minimum size; the offscreen
    // platform warns that it does not pass size hints on.
    capturedMessages().removeAll(QStringLiteral("This plugin does not support propagateSizeHints()"));
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
// bridge's tree format. Without `albumATracks`, Album A's tracks are left
// out, as the backend leaves out the children of a row it has not been
// told is expanded.
QByteArray libraryTree(bool albumATracks = true) {
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
    QList<Row> sent;
    for (const Row &row : rows) {
        if (albumATracks || !row.trackPath.startsWith(QStringLiteral("/music/Artist A/Album A/"))) {
            sent.push_back(row);
        }
    }
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(sent.size()));
    for (const Row &row : sent) {
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

// One cover every track shows unless a test gives it another: the panel
// takes its wide layout only for a track with a cover (#373 H8), and a
// cover that stays the same changes nothing of a track change's fade.
QString steadyCover() {
    static QTemporaryDir dir;
    static const QString url = [] {
        const QString path = dir.filePath(QStringLiteral("steady.png"));
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(QColor(90, 90, 90));
        image.save(path);
        return QUrl::fromLocalFile(path).toString();
    }();
    return url;
}

// The order a track's properties change in. The backend changes them in one
// snapshot, but BridgeClient tells the track's identity (its path) before
// its metadata (title, album, cover); the fallback bridge tells each as it
// is set.
enum class Order { PathFirst, CoverFirst };

// Plays a track for the now-playing panel, by default in BridgeClient's
// order.
void showTrack(QObject *bridge, const QString &path, const QString &title, const QString &album,
               const QString &cover = steadyCover(), Order order = Order::PathFirst) {
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

// The ink `line` draws with at `x`, as a share of one whole text's, and the
// sum of the opacities there: whole where the line stays still; elsewhere
// each piece of the lines before and the playing track's text fading in,
// one over the other, as the eye blends them over the line (#373 H6).
std::pair<double, double> inkAt(const PanelLine &line, double x) {
    if (line.stillAt(x)) {
        return {1.0, 1.0};
    }
    double clear = 1.0;
    double sum = 0.0;
    const auto add = [&](double opacity) {
        clear *= 1.0 - opacity;
        sum += opacity;
    };
    for (QQuickItem *part : line.parts()) {
        if (x >= part->x() && x < part->x() + part->width()) {
            add(part->opacity());
        }
    }
    for (const auto &[from, to] : line.incomingWindows()) {
        if (x >= from && x < to) {
            add(line.incoming()->opacity());
        }
    }
    return {1.0 - clear, sum};
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
// runs over the fade's time, linearly; the lines cross-fade over all of it
// on one OutCubic progress, the lines before going out as the playing
// track's come in, so a line that changes never drops below about 0.6 of
// its ink (#373 H6); the covers before go out over all of it, on the same
// curve.
double eased(double progress) {
    const double rest = 1.0 - std::clamp(progress, 0.0, 1.0);
    return 1.0 - rest * rest * rest;
}

double linesOut(double at) {
    return 1.0 - eased(at);
}

double linesIn(double at) {
    return eased(at);
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

// Whether the panel shows the playing track's layout whole: the first
// track grows it from the compact row of nothing playing (#373 H7), which
// this waits out.
bool grown(QQuickItem *panel) {
    return QTest::qWaitFor([&]() { return panel->property("presence").toDouble() == 1.0; }, 5000);
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
    // A track without a cover has no cover's place, and its mark whole.
    if (panel->property("layoutMode").toString() == QStringLiteral("bare")) {
        QQuickItem *mark = itemNamed(panel, QStringLiteral("nowPlayingBareMark"));
        return !itemNamed(panel, QStringLiteral("nowPlayingCoverArea"))->isVisible() && mark->isVisible()
            && qFuzzyCompare(mark->opacity(), 1.0);
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
    bridge->setProperty("currentTrackCoverPath", steadyCover());
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
    void nowPlayingGrowsFromTheCompactRowOnTheFirstPlay_data();
    void nowPlayingGrowsFromTheCompactRowOnTheFirstPlay();
    void nowPlayingGrowsAtOnceWithAnimationsOff();
    void nowPlayingCoverComesAndGoesAsOneChange_data();
    void nowPlayingCoverComesAndGoesAsOneChange();
    void nowPlayingCoverLayoutChangesAtOnceWithAnimationsOff();
    void nowPlayingShrinksBackIntoTheCompactRow();
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
    void treeToggleOpensChildrenThatArriveLater();
    void treeToggleIsInstantWithAnimationsOff();
    void treeChangesOtherThanAToggleAreInstant();
    void queueRowsADragDisplacesSlide();
    void queueRowsTheKeyboardMovesMoveAtOnce();
    void animationsOffMakeEveryChangeInstant();
    void nothingAnimatesOnceTheMotionIsDone();
    void nowPlayingKeepsTheCoverUntilTheNextTracksIsKnown();
    void nowPlayingChangedLinesCrossFadeWithoutABlink();
    void nowPlayingChangedLinesCrossFadeWithoutABlink_data();
    void nowPlayingLineFadesWholeUnlessOnlyAWordChanges();
    void nowPlayingLineFadesWholeUnlessOnlyAWordChanges_data();
    void nowPlayingAnotherAlbumLeavesNoWordAlone();
    void nowPlayingAnotherAlbumLeavesNoWordAlone_data();
    void nowPlayingLinesCrossFadeOnOneProgress();
    void visualizationModeSwitchKeepsAPlotOnScreen();
    void visualizationModeSwitchKeepsAPlotOnScreen_data();
    void visualizationPaneFloorFollowsTheMode();
    void visualizationPaneFloorFollowsTheMode_data();
    void viewerControlsFadeWithTheStrip();
    void viewerControlsFadeWithTheStrip_data();
    void viewerPlatesAndGlyphsRevealOnOneLevel();
    void windowedViewerOpensOverThePanesLastFrame();
    void viewerTakesTheVisualizationWholeIntoEitherWindow();
    void viewerTakesTheVisualizationWholeIntoEitherWindow_data();
    void viewerPlatesAndGlyphsRevealOnOneLevel_data();
    void albumArtViewerFadesWithItsArt_data();
    void albumArtViewerFadesWithItsArt();
    void windowedViewerClosesOverItsLastFrame();
    void albumArtViewerControlsFadeOnOneLevel_data();
    void albumArtViewerControlsFadeOnOneLevel();
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
    InputModality::registerQmlType();
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

// The old row's amber glyph and title fade out as the new row's fade in,
// over the same shortDuration on the same OutCubic curve: the two rows'
// amber never adds up to more than one row's, so no frame shows two fully
// amber rows.
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

    // At once, before a frame: the old row is amber, the new one not yet.
    QVERIFY(animationOf(fade)->property("running").toBool());
    QVERIFY(oldRow.fadingGlyph->isVisible());
    QCOMPARE(oldRow.fadingGlyph->property("source").toString(), QStringLiteral("media-playback-start"));
    QCOMPARE(colorOf(oldRow.fadingGlyph), nowHex(window));
    QCOMPARE(oldRow.fadingGlyph->opacity(), 1.0);
    QCOMPARE(colorOf(oldRow.title), nowHex(window));
    QVERIFY(colorOf(newRow.title) != nowHex(window));
    QCOMPARE(colorOf(newRow.glyph), nowHex(window));
    QCOMPARE(newRow.glyph->opacity(), 0.0);
    QVERIFY(!newRow.fadingGlyph->isVisible());
    // The weights go with the amber, not the state: the new row is not
    // semibold in the text colour, nor the old one regular in amber.
    const auto weightOf = [](const RowLook &row) {
        return row.title->property("font").value<QFont>().weight();
    };
    QCOMPARE(weightOf(newRow), QFont::Normal);
    QCOMPARE(weightOf(oldRow), QFont::DemiBold);
    QVERIFY(!oldRow.glyph->isVisible());

    // Every frame of the hand-over: the glyphs' opacities add up to one,
    // and so do the titles' shares of the amber, so never are two rows
    // fully amber.
    const QColor now(nowHex(window));
    const QColor plain = qvariant_cast<QObject *>(window->property("uiPalette"))
        ->property("text").value<QColor>();
    // How far a title is from the text colour towards the amber, 0 to 1,
    // along the channel where the two differ most.
    const auto amberShare = [&](QQuickItem *title) {
        const QColor color = title->property("color").value<QColor>();
        const qreal channels[3][3] = {{plain.redF(), now.redF(), color.redF()},
                                      {plain.greenF(), now.greenF(), color.greenF()},
                                      {plain.blueF(), now.blueF(), color.blueF()}};
        int widest = 0;
        for (int channel = 1; channel < 3; ++channel) {
            if (std::abs(channels[channel][1] - channels[channel][0])
                > std::abs(channels[widest][1] - channels[widest][0])) {
                widest = channel;
            }
        }
        return (channels[widest][2] - channels[widest][0]) / (channels[widest][1] - channels[widest][0]);
    };
    // Whether a title's weight agrees with its colour in this frame: it is
    // semibold while it is mostly amber and regular while it is mostly the
    // text colour, so the two change in the same frame.
    const auto weightFollowsAmber = [&](const RowLook &row) {
        const qreal share = amberShare(row.title);
        return weightOf(row) == QFont::DemiBold ? share >= 0.48 : share <= 0.52;
    };
    QVERIFY(weightFollowsAmber(oldRow) && weightFollowsAmber(newRow));
    int frames = 0;
    qreal worstGlyphs = 0;
    qreal worstTitles = 0;
    int weightMismatches = 0;
    QVERIFY(QTest::qWaitFor([&]() {
        if (!animationOf(fade)->property("running").toBool()) {
            return true;
        }
        ++frames;
        worstGlyphs = std::max(worstGlyphs,
                               std::abs(oldRow.fadingGlyph->opacity() + newRow.glyph->opacity() - 1.0));
        worstTitles = std::max(worstTitles, amberShare(oldRow.title) + amberShare(newRow.title) - 1.0);
        weightMismatches += (weightFollowsAmber(oldRow) ? 0 : 1) + (weightFollowsAmber(newRow) ? 0 : 1);
        return false;
    }, 2000));
    QVERIFY(frames > 0);
    QVERIFY2(worstGlyphs < 0.02, qPrintable(QString::number(worstGlyphs)));
    QVERIFY2(worstTitles < 0.03, qPrintable(QString::number(worstTitles)));
    QCOMPARE(weightMismatches, 0);

    // Then the old row is plain, the new one wholly amber, and nothing runs.
    QVERIFY(!oldRow.fadingGlyph->isVisible());
    QVERIFY(colorOf(oldRow.title) != nowHex(window));
    QVERIFY(fade->property("target").isNull() || !fade->property("target").value<QJSValue>().isObject());
    QCOMPARE(colorOf(newRow.title), nowHex(window));
    QCOMPARE(newRow.glyph->opacity(), 1.0);
    QCOMPARE(weightOf(newRow), QFont::DemiBold);
    QCOMPARE(weightOf(oldRow), QFont::Normal);
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
// fades its mark out.
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
    // The weights go with the amber: the new row semibold at once, as it is
    // amber at once, and the old one semibold while it is mostly amber.
    const auto weightOf = [](const RowLook &row) {
        return row.title->property("font").value<QFont>().weight();
    };
    QCOMPARE(weightOf(second), QFont::DemiBold);
    QCOMPARE(weightOf(first), QFont::DemiBold);
    QQuickItem *firstTitle = first.title;
    int weightMismatches = 0;
    bridge->setProperty("currentTrackTreeKeys", QStringList{kArtistKey, kAlbumAKey});
    QVERIFY(QTest::qWaitFor([&]() {
        if (!animationOf(fade)->property("running").toBool()) {
            return true;
        }
        const qreal amber = firstTitle->property("amber").toReal();
        const bool semibold = weightOf(first) == QFont::DemiBold;
        weightMismatches += semibold == (amber >= 0.5) ? 0 : 1;
        return false;
    }));
    QCOMPARE(weightMismatches, 0);
    QVERIFY(!first.fadingGlyph->isVisible());
    QVERIFY(colorOf(first.title) != nowHex(window));
    QCOMPARE(weightOf(first), QFont::Normal);

    // Album B is collapsed and holds the next track: its name is amber,
    // with no glyph on the row (#373 H5). Then Album A plays again, and
    // Album B's name fades back, its weight with its colour, as a track
    // row's does.
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    QVERIFY(albumB != nullptr);
    playTreeTrack(bridge, kTrackB1, kAlbumBKey);
    const RowLook album = libraryRowLook(albumB);
    QVERIFY(itemNamed(albumB, QStringLiteral("libraryRowNowMark")) == nullptr);
    QCOMPARE(colorOf(album.title), nowHex(window));
    QCOMPARE(weightOf(album), QFont::DemiBold);
    QVERIFY(!album.glyph->isVisible());
    QVERIFY(QTest::qWaitFor([&]() { return !animationOf(fade)->property("running").toBool(); }));
    playTreeTrack(bridge, kTrackA1, kAlbumAKey);
    QVERIFY(animationOf(fade)->property("running").toBool());
    QCOMPARE(colorOf(album.title), nowHex(window));
    QVERIFY(!album.glyph->isVisible());
    QVERIFY(!album.fadingGlyph->isVisible());
    QCOMPARE(colorOf(libraryRowLook(libraryRowWithPath(window, kTrackA1)).title), nowHex(window));
    QQuickItem *albumTitle = album.title;
    weightMismatches = 0;
    bool faded = false;
    QVERIFY(QTest::qWaitFor([&]() {
        if (!animationOf(fade)->property("running").toBool()) {
            return true;
        }
        const qreal amber = albumTitle->property("amber").toReal();
        faded = faded || (amber > 0 && amber < 1);
        weightMismatches += (weightOf(album) == QFont::DemiBold) == (amber >= 0.5) ? 0 : 1;
        return false;
    }));
    QVERIFY(faded);
    QCOMPARE(weightMismatches, 0);
    QVERIFY(colorOf(album.title) != nowHex(window));
    QCOMPARE(weightOf(album), QFont::Normal);
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
// with the year and the genre. What changes cross-fades, the text before
// going out as the playing track's comes in, on one progress (#373 H6).
// The artist and the album stay at full opacity in every frame, and no
// edge cuts a glyph.
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
    bridge->setProperty("currentTrackCoverPath", steadyCover());
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
            if (std::abs(line.incomingOpacity() + line.outgoingOpacity() - 1.0) > 1e-9) {
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
// longDuration, the cover and the lines cross-fading over all of it; the
// artist both share
// stays still, and the details, whose year and number change, fade whole.
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
    QVERIFY(grown(panel));

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
    // The year and the number change, in two places: the line fades whole,
    // with nothing between them left alone (#373 H2).
    QCOMPARE(panelLine(panel, QStringLiteral("Details")).stillTexts(), QStringList());
    QCOMPARE(panelLine(panel, QStringLiteral("Details")).incomingWindows(),
             (QList<Window>{{0.0, panelLine(panel, QStringLiteral("Details")).line()->width()}}));
    // Only the album changes, and most of the line stays: the artist stays
    // still.
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
    panel->setProperty("handover", 0.3);
    QCOMPARE(title.outgoingOpacity(), linesOut(0.3));
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
// it showed, and so does all else that stayed still: the line now changes
// in two places, so it fades whole (#373 H2), with no word left alone. What
// stayed still does not take on the rest's progress, and nothing jumps. The
// playing track's text fades in as one, from nothing, as all that goes out
// goes, so none of its glyphs is cut where the line before's pieces are,
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
    // A quarter of the way: the number before going out as the next comes
    // in.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.25);
    const double numberLeft = linesOut(0.25);
    const double numberIn = linesIn(0.25);
    QVERIFY(numberLeft > 0.0 && numberIn > 0.0);
    const QFontMetricsF metrics(details.font());
    const double inGenre = metrics.horizontalAdvance(QStringLiteral("2005 · R"));
    const double inTrack = metrics.horizontalAdvance(QStringLiteral("2005 · Rock · Tr"));
    const double inNumber = (metrics.horizontalAdvance(QStringLiteral("2005 · Rock · Track "))
                             + metrics.horizontalAdvance(QStringLiteral("2005 · Rock · Track 1"))) / 2;
    QCOMPARE(details.opacitiesAt(inGenre), std::make_pair(1.0, 1.0));
    QCOMPARE(details.opacitiesAt(inTrack), std::make_pair(1.0, 1.0));
    QCOMPARE(details.opacitiesAt(inNumber), std::make_pair(numberLeft, numberIn));
    QCOMPARE(details.opacitiesAt(metrics.horizontalAdvance(QStringLiteral("20"))), std::make_pair(1.0, 1.0));

    // The file's genre replaces the queue's.
    bridge->setProperty("currentTrackGenre", genre);
    QCOMPARE(details.text(), QStringLiteral("2005 · %1 · Track 2").arg(genre));
    QCOMPARE(details.outgoingText(), QStringLiteral("2005 · Rock · Track 1"));
    // At once: the year, the genre before and "Track" after it show whole,
    // as they did, and fade out from there; the number before goes on from
    // where it was, and so does the playing track's number, which had begun
    // to come in, in a layer of its own: the playing track's text then comes
    // in again, from nothing, as one.
    const double inYear = metrics.horizontalAdvance(QStringLiteral("20"));
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(details.opacitiesAt(inYear), std::make_pair(1.0, 0.0));
    QCOMPARE(details.opacitiesAt(inGenre), std::make_pair(1.0, 0.0));
    QCOMPARE(details.opacitiesAt(inTrack), std::make_pair(1.0, 0.0));
    QCOMPARE(details.opacitiesAt(inNumber), std::make_pair(numberLeft, 0.0));
    const QList<Drawn> number = drawnAt(details, inNumber);
    QCOMPARE(number.size(), 2);
    QCOMPARE(number.at(0).glyph, QLatin1Char('1'));
    QCOMPARE(number.at(0).opacity, numberLeft);
    QCOMPARE(number.at(1).glyph, QLatin1Char('2'));
    QCOMPARE(number.at(1).opacity, numberIn);
    // Nothing stays still: the playing track's text comes in as one, and
    // the line before's pieces are cut only in its own gaps.
    QCOMPARE(details.stillTexts(), QStringList());
    QCOMPARE(itemsNamed(details.line(), QStringLiteral("nowPlayingIncomingDetails")).size(), 1);
    QCOMPARE(details.incomingWindows(), (QList<Window>{{0.0, details.line()->width()}}));
    // The number before; and the playing track's line before, whole where
    // it stayed still and at where its number had come in to elsewhere.
    QCOMPARE(details.parts().size(), 3);
    QVERIFY2(cutGlyph(details).isEmpty(), qPrintable(cutGlyph(details)));

    // Then each goes on, on its curve, out as the playing track's comes in.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.25, 0.6}) {
        panel->setProperty("handover", at);
        QCOMPARE(details.opacitiesAt(inYear), std::make_pair(linesOut(at), linesIn(at)));
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
// the playing track's text comes in as one, from nothing, as it goes.
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
    // Three quarters of the way: the album before nearly out, the next
    // nearly in.
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.75);
    const double cameIn = linesIn(0.75);
    const double albumLeft = linesOut(0.75);
    QVERIFY(cameIn > 0.0 && cameIn < 1.0);
    QCOMPARE(artistAlbum.incomingOpacity(), cameIn);
    QCOMPARE(artistAlbum.outgoingOpacity(), albumLeft);

    // The file names the artist in Hebrew.
    bridge->setProperty("currentTrackArtist", QStringLiteral("להקת הים"));
    QCOMPARE(artistAlbum.text(), QStringLiteral("להקת הים — Album Bee"));
    // What showed goes out: the album before from where it was, the
    // playing track's as far as it came in, and the artist, which was
    // still, whole.
    QCOMPARE(artistAlbum.outgoingText(), QStringLiteral("Artist A — Album A"));
    QCOMPARE(layersOf(artistAlbum).last()->property("text").toString(), QStringLiteral("Artist A — Album Bee"));
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    QCOMPARE(artistAlbum.stillWindows(), QList<Window>());
    QCOMPARE(itemsNamed(artistAlbum.line(), QStringLiteral("nowPlayingIncomingArtistAlbum")).size(), 1);
    QCOMPARE(artistAlbum.incomingWindows(), (QList<Window>{{0.0, artistAlbum.line()->width()}}));
    QCOMPARE(artistAlbum.incomingOpacity(), 0.0);
    const QFontMetricsF metrics(artistAlbum.font());
    const double inArtist = metrics.horizontalAdvance(QStringLiteral("Art"));
    const double inAlbum = metrics.horizontalAdvance(QStringLiteral("Artist A — Al"));
    // The opacities `line` draws with at `x`, bottom to top.
    const auto opacities = [](const PanelLine &line, double x) {
        QList<double> drawn;
        for (const Drawn &each : drawnAt(line, x)) {
            drawn.push_back(each.opacity);
        }
        return drawn;
    };
    QCOMPARE(opacities(artistAlbum, inArtist), QList<double>{1.0});
    QCOMPARE(opacities(artistAlbum, inAlbum), (QList<double>{albumLeft, cameIn}));
    QVERIFY2(cutGlyph(artistAlbum).isEmpty(), qPrintable(cutGlyph(artistAlbum)));

    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.25, 0.75}) {
        panel->setProperty("handover", at);
        QCOMPARE(opacities(artistAlbum, inArtist), (QList<double>{linesOut(at), linesIn(at)}));
        QCOMPARE(opacities(artistAlbum, inAlbum),
                 (QList<double>{albumLeft * linesOut(at), cameIn * linesOut(at), linesIn(at)}));
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
    // A late genre: the number before, and the playing track's line before
    // in two pieces, split after "Track", which stayed still with the genre
    // before.
    bridge->setProperty("currentTrackGenre", QStringLiteral("Jazz"));
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    QCOMPARE(details.parts().size(), 3);
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
    QVERIFY(grown(panel));
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
    QTest::newRow("a number with a digit more") << "Classical" << "Classical" << 2005 << 9 << 10 << 12
                                                << QStringList{QStringLiteral("2005 · Classical · Track")}
                                                << "2005 · Classical · Track 9 o" << "2005 · Classical · Track 10 o";
    // Less than half of it would stay: it fades whole (#373 H2).
    QTest::newRow("a wider genre") << "Rock" << "Electronic" << 2005 << 2 << 3 << 8 << QStringList{}
                                   << "2005 · Rock · Tr" << "2005 · Electronic · Tr";
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
    QVERIFY(grown(panel));
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

// A line elided ever shorter, where what stays still is on both sides of
// the number that changes: no edge falls where the ellipsis is, or past the
// gap it stands for, and none cuts a glyph. What stays still stays still up
// to a gap while the line shows one after it, and to the end of the line
// when every text is cut off within it alike; the number, where it shows,
// always fades.
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
    tellDetails(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), 2019, genre, 5, 8);
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    panel->setProperty("handover", 0.25);
    const QString before = details.outgoingText();
    const QString after = details.text();
    const QFontMetricsF metrics(details.font());
    const QString upToNumber = QStringLiteral("2019 · ") + genre + QStringLiteral(" · Track ");
    const double inNumber = metrics.horizontalAdvance(upToNumber) + 1.0;
    const double pastNumber = metrics.horizontalAdvance(upToNumber + QLatin1Char('5'));
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
        if (shownUpTo(before, width) >= pastNumber && shownUpTo(after, width) >= pastNumber) {
            QVERIFY2(!details.stillAt(inNumber), qPrintable(QStringLiteral("the number is still") + where));
            QVERIFY2(details.opacitiesAt(inNumber) == std::make_pair(linesOut(0.25), linesIn(0.25)),
                     qPrintable(QStringLiteral("number") + where));
        }
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
                     (QStringList{QStringLiteral("2019 · ") + genre + QStringLiteral(" · Track"), QStringLiteral("of 8")}));
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
    // What stays still from B's arrival, and from C's: C's genre changes
    // too, so its line fades whole, with no lone "·" left (#373 H2).
    QTest::addColumn<QStringList>("stillForB");
    QTest::addColumn<QStringList>("stillForC");
    // Where a word that stayed still and changes for C is.
    QTest::addColumn<QString>("givenUp");
    QTest::newRow("the number") << QList<int>{0, 0, 0} << QStringList{QString(), QString(), QString()}
                                << QList<int>{4, 5, 6} << 8 << QStringList{"Track", "of 8"}
                                << QStringList{"Track", "of 8"} << QString();
    QTest::newRow("a field after the year") << QList<int>{2019, 2020, 2021}
                                            << QStringList{"Synth-pop", "Synth-pop", "Pop"} << QList<int>{0, 0, 0}
                                            << 0 << QStringList{"· Synth-pop"} << QStringList{}
                                            << "2019 · Syn";
}

// A → B, then C while A goes out or while B comes in (#463 with #537):
// every layer goes on from what it showed, what stays still for all three
// stays still, at full opacity, all along, and what C no longer has in
// place goes out whole, in B's layer, as it showed. C's text comes in as
// one, from nothing, as they go out. Nothing cuts a glyph.
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
        // A as far as it has gone out, B as far as it has come in: the two
        // cross-fade.
        const double leftOfA = linesOut(interruptAt);
        const double inOfB = linesIn(interruptAt);
        QVERIFY2(leftOfA > 0.0 && inOfB > 0.0 && sameOpacity(leftOfA + inOfB, 1.0), qPrintable(where));
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
    QVERIFY(panel->property("idle").toBool());
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
    QVERIFY(grown(panel));
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
    // C comes early in the cross-fade of A and B, and late.
    QTest::newRow("early") << 0.3;
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
    QVERIFY(grown(panel));
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
    // A's title as far as it went out and B's as far as it came in, where
    // they were; C's over them, to come in from nothing as they go out.
    const QList<QQuickItem *> layers = layersOf(title);
    QCOMPARE(layers.size(), 2);
    QCOMPARE(partText(title, partsOf(title, layers.first()).first()), QStringLiteral("First"));
    QCOMPARE(partsOf(title, layers.first()).first()->opacity(), linesOut(at));
    QCOMPARE(partText(title, partsOf(title, layers.last()).first()), QStringLiteral("Second"));
    QCOMPARE(partsOf(title, layers.last()).first()->opacity(), linesIn(at));
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

QRectF sceneRectOf(QQuickItem *item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

// Clears playback, as when the queue is cleared: nothing loaded.
void clearPlayback(QObject *bridge) {
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    for (const char *property : {"currentTrackPath", "currentTrackTitle", "currentTrackArtist", "currentTrackAlbum",
                                 "currentTrackCoverPath"}) {
        bridge->setProperty(property, QString());
    }
}

// The panel's height at `presence` of the way from the compact row to the
// playing track's layout of `full` px.
double grownHeight(QQuickItem *panel, double presence, double full) {
    const double rest = panel->property("restHeight").toDouble();
    return rest + (full - rest) * presence;
}

// Whether the library's header is laid out right under the panel.
bool libraryFollows(QQuickItem *panel, QQuickItem *header) {
    return std::abs(sceneRectOf(header).top() - sceneRectOf(panel).bottom()) < 0.01
        && std::abs(panel->height() - panel->implicitHeight()) < 0.01;
}

void UiMotionTest::nowPlayingGrowsFromTheCompactRowOnTheFirstPlay_data() {
    QTest::addColumn<bool>("narrow");
    QTest::newRow("wide") << false;
    QTest::newRow("narrow") << true;
}

// The first track after nothing loaded grows the cover's place from the
// compact row to its full size over longDuration, OutCubic (#373 H7): the
// panel's height, the cover's band and its opacity follow one progress, the
// lines and the library's header move down with it, the compact row fades
// out over the first half, and the lines show whole all along, with no
// blink. Nothing jumps: the header goes down frame by frame, one way, to
// where the full layout puts it.
void UiMotionTest::nowPlayingGrowsFromTheCompactRowOnTheFirstPlay() {
    QFETCH(bool, narrow);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    if (narrow) {
        window->resize(1000, 720);
        QTRY_COMPARE(window->width(), 1000);
    }
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    // Long enough for many frames.
    units->setLongDuration(1000);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *area = named(window, "nowPlayingCoverArea");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *row = named(window, "nowPlayingIdleRow");
    QVERIFY(panel && header && area && cover && row);
    auto *growth = qvariant_cast<QObject *>(panel->property("presenceAnimation"));
    QVERIFY(growth != nullptr);
    QTRY_COMPARE(panel->property("layoutMode").toString(), narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QVERIFY(panel->property("idle").toBool());
    QTRY_VERIFY(libraryFollows(panel, header));
    const double rest = panel->property("restHeight").toDouble();
    const double rowHeight = panel->property("idleRowHeight").toDouble();
    QCOMPARE(panel->height(), rest);
    QVERIFY(!area->isVisible());
    QVERIFY(row->isVisible());
    const double idleTop = sceneRectOf(header).top();

    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    QVERIFY(!panel->property("idle").toBool());
    QVERIFY(growth->property("running").toBool());
    QCOMPARE(growth->property("duration").toInt(), 1000);
    QCOMPARE(easingOf(growth), QEasingCurve::OutCubic);
    QCOMPARE(growth->property("from").toDouble(), 0.0);
    QCOMPARE(growth->property("to").toDouble(), 1.0);

    // Frame by frame: down, one way, from near where it was, to the end.
    QList<double> tops;
    while (growth->property("running").toBool()) {
        QTest::qWait(5);
        if (libraryFollows(panel, header)) {
            tops.push_back(sceneRectOf(header).top());
        }
    }
    QVERIFY(panel->property("presence").toDouble() == 1.0);
    QTRY_VERIFY(libraryFollows(panel, header));
    const double full = panel->property("layoutHeight").toDouble();
    const double fullTop = idleTop + full - rest;
    QVERIFY(full > rest + 60);
    QCOMPARE(panel->height(), full);
    QCOMPARE(sceneRectOf(header).top(), fullTop);
    QVERIFY2(tops.size() >= 5, qPrintable(QString::number(tops.size())));
    QVERIFY2(tops.first() < idleTop + (fullTop - idleTop) / 2, qPrintable(QString::number(tops.first())));
    for (qsizetype index = 1; index < tops.size(); ++index) {
        QVERIFY(tops.at(index) >= tops.at(index - 1));
    }
    const double side = narrow ? 96.0 : panel->width();
    QCOMPARE(area->height(), side);
    QCOMPARE(cover->height(), side);
    QCOMPARE(sceneRectOf(cover), sceneRectOf(area));
    QCOMPARE(cover->opacity(), 1.0);
    QVERIFY(!row->isVisible());

    // Every point of the growth, paused there.
    clearPlayback(bridge);
    QVERIFY(QMetaObject::invokeMethod(growth, "complete"));
    QTRY_VERIFY(libraryFollows(panel, header));
    QCOMPARE(sceneRectOf(header).top(), idleTop);
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    QVERIFY(growth->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(growth, "pause"));
    const QStringList lines = narrow ? kNarrowLines : kWideLines;
    for (const double at : {0.1, 0.25, 0.5, 0.75}) {
        panel->setProperty("presence", at);
        const QString where = QString::number(at);
        QTRY_VERIFY2(libraryFollows(panel, header), qPrintable(where));
        // On whole pixels.
        QVERIFY2(std::abs(panel->height() - grownHeight(panel, at, full)) <= 0.5, qPrintable(where));
        QCOMPARE(panel->height(), std::round(panel->height()));
        QVERIFY2(std::abs(area->height() - (rowHeight + (side - rowHeight) * at)) <= 0.5, qPrintable(where));
        QVERIFY2(qFuzzyCompare(cover->opacity(), at), qPrintable(where));
        // The middle of the cover in its band.
        QVERIFY2(std::abs(sceneRectOf(cover).center().y() - sceneRectOf(area).center().y()) <= 0.5,
                 qPrintable(where));
        QVERIFY2(area->clip(), qPrintable(where));
        QVERIFY2(qFuzzyCompare(1.0 + row->opacity(), 1.0 + std::max(0.0, 1.0 - 2.0 * at)), qPrintable(where));
        QCOMPARE(row->isVisible(), at < 0.5);
        // The lines whole, the title under the cover's band or, beside the
        // narrow cover, under the compact row while that shows.
        for (const QString &name : lines) {
            QVERIFY2(lineStill(panel, name), qPrintable(name + QLatin1Char(' ') + where));
        }
        const PanelLine title = panelLine(panel, lines.first());
        if (narrow) {
            QVERIFY2(row->opacity() <= 0.3
                         || sceneRectOf(title.line()).top() >= sceneRectOf(row).bottom(),
                     qPrintable(where));
        } else {
            QCOMPARE(sceneRectOf(title.line()).top(), sceneRectOf(area).bottom() + 12);
        }
    }
    QVERIFY(QMetaObject::invokeMethod(growth, "resume"));
    QVERIFY(grown(panel));
    QVERIFY(panelIdle(panel, lines));
}

// With animations off, the first track shows the full layout at once, and
// nothing loaded the compact row.
void UiMotionTest::nowPlayingGrowsAtOnceWithAnimationsOff() {
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
    QQuickItem *header = named(window, "libraryHeader");
    auto *growth = qvariant_cast<QObject *>(panel->property("presenceAnimation"));
    QVERIFY(growth != nullptr);
    QTRY_VERIFY(libraryFollows(panel, header));
    const double rest = panel->property("restHeight").toDouble();
    QCOMPARE(panel->height(), rest);

    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"));
    QVERIFY(!growth->property("running").toBool());
    QCOMPARE(panel->property("presence").toDouble(), 1.0);
    QCOMPARE(panel->implicitHeight(), panel->property("layoutHeight").toDouble());
    QTRY_VERIFY(libraryFollows(panel, header));
    QCOMPARE(panel->height(), panel->property("layoutHeight").toDouble());
    QVERIFY(!named(window, "nowPlayingIdleRow")->isVisible());

    clearPlayback(bridge);
    QVERIFY(!growth->property("running").toBool());
    QCOMPARE(panel->property("presence").toDouble(), 0.0);
    QCOMPARE(panel->implicitHeight(), rest);
    QTRY_VERIFY(libraryFollows(panel, header));
    QVERIFY(named(window, "nowPlayingIdleRow")->isVisible());
    QVERIFY(!named(window, "nowPlayingCoverArea")->isVisible());
}

void UiMotionTest::nowPlayingCoverComesAndGoesAsOneChange_data() {
    QTest::addColumn<bool>("narrow");
    QTest::addColumn<bool>("nextTrack");
    for (const bool narrow : {false, true}) {
        QTest::addRow("%s, the next track has no cover", narrow ? "1000x720" : "1600x1000") << narrow << true;
        QTest::addRow("%s, the track's cover goes", narrow ? "1000x720" : "1600x1000") << narrow << false;
    }
}

// Between a track with a cover and one without, wide or narrow, the panel
// changes as one (#373 H9: the cover shrank while the two layouts' lines
// cross-faded over it, the no-cover mark drew over the cover going out,
// the lines overlapped for 117 ms, and the column still moved 83 ms after
// the change's stated end): over longDuration, the lines before go out
// over the first half; the cover's place moves only from geometryStart,
// shrinking or growing at the top left, with the library's header, one
// way; the mark never shows while a cover does, and no placeholder shows
// in the cover's place; the lines before and the playing track's overlap
// at most 45 ms at Kirigami's speed and add up to at least half a line in
// every frame; the cover's image keeps its size, so it is not loaded again.
// At speed, nothing moves after the change ends, and the handover of the
// lines ends with it.
void UiMotionTest::nowPlayingCoverComesAndGoesAsOneChange() {
    QFETCH(bool, narrow);
    QFETCH(bool, nextTrack);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    if (narrow) {
        window->resize(1000, 720);
        QTRY_COMPARE(window->width(), 1000);
    }
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    const int duration = units->longDuration();
    QVERIFY(duration > 0);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *area = named(window, "nowPlayingCoverArea");
    QQuickItem *cover = named(window, "nowPlayingCover");
    QQuickItem *mark = named(window, "nowPlayingBareMark");
    QVERIFY(panel && header && area && cover && mark);
    QQuickItem *placeholder = itemNamed(cover, QStringLiteral("albumArtPlaceholderGlyph"));
    QVERIFY(placeholder != nullptr);
    QObject *growth = qvariant_cast<QObject *>(panel->property("coverGrowthAnimation"));
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    QVERIFY(growth && handover);
    const QStringList lines = narrow ? kNarrowLines : kWideLines;
    const QString artLayout = narrow ? QStringLiteral("narrow") : QStringLiteral("wide");
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    QVERIFY(grown(panel));
    QTRY_COMPARE(panel->property("layoutMode").toString(), artLayout);
    QTRY_VERIFY(libraryFollows(panel, header));
    QTRY_VERIFY(panelIdle(panel, lines));
    const double sourceSize = cover->property("coverSourceSize").toDouble();
    const double geometryStart = panel->property("geometryStart").toDouble();
    QVERIFY(geometryStart > 0.3 && geometryStart < 0.6);

    // The track with a cover, then one without, then one with, ...
    bool second = false;
    const auto change = [&](bool toCover) {
        second = !second;
        if (nextTrack) {
            showTrack(bridge, second ? kTrackA2 : kTrackA1, second ? QStringLiteral("Second") : QStringLiteral("First"),
                      QStringLiteral("Album A"), toCover ? covers.first : QString());
        } else {
            bridge->setProperty("currentTrackCoverPath", toCover ? covers.first : QString());
        }
    };
    const auto coverShown = [&]() { return area->isVisible() && cover->isVisible() && cover->opacity() > 0; };
    const auto markShown = [&]() { return mark->isVisible() && mark->opacity() > 0; };

    // Sampled through the change, paused at each point.
    const auto sampled = [&](bool toCover) {
        const double startTop = sceneRectOf(header).top();
        const double startSide = coverShown() ? cover->width() : 0.0;
        change(toCover);
        QCOMPARE(panel->property("layoutMode").toString(), toCover ? artLayout : QStringLiteral("bare"));
        QVERIFY(growth->property("running").toBool());
        QCOMPARE(growth->property("duration").toInt(), duration);
        QCOMPARE(easingOf(growth), QEasingCurve::Linear);
        const bool fades = handover->property("running").toBool();
        QCOMPARE(fades, nextTrack);
        if (fades) {
            // The lines' fade runs with the change, from its start.
            QCOMPARE(handover->property("duration").toInt(), duration);
            QCOMPARE(panel->property("handover").toDouble(), 0.0);
            QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        }
        QVERIFY(QMetaObject::invokeMethod(growth, "pause"));
        const int steps = 200;
        int overlapping = 0;
        double lastTop = startTop;
        for (int step = 0; step <= steps; ++step) {
            const double at = double(step) / steps;
            const QString where = QStringLiteral("%1 at %2").arg(toCover ? "to a cover" : "to none").arg(at);
            panel->setProperty("layoutStep", at);
            if (fades) {
                panel->setProperty("handover", at);
            }
            QVERIFY2(QTest::qWaitFor([&]() { return libraryFollows(panel, header); }), qPrintable(where));
            // The mark never with a cover, and no placeholder in its place.
            QVERIFY2(!(markShown() && coverShown()), qPrintable(where));
            if (coverShown()) {
                QVERIFY2(!placeholder->isVisible(), qPrintable(where));
                QCOMPARE(cover->childItems().first()->property("source").toUrl().toString(), covers.first);
            }
            QVERIFY2(!shownIn(panel, "albumArtOutgoing"), qPrintable(where));
            QCOMPARE(cover->property("coverSourceSize").toDouble(), sourceSize);
            // Nothing moves until the lines before are nearly gone.
            const double top = sceneRectOf(header).top();
            if (at < geometryStart) {
                QVERIFY2(qFuzzyCompare(top, startTop), qPrintable(where));
                QVERIFY2(!coverShown() || qFuzzyCompare(cover->width(), startSide), qPrintable(where));
            }
            // Then one way.
            QVERIFY2(toCover ? top >= lastTop : top <= lastTop, qPrintable(where));
            lastTop = top;
            // The lines: whole for the same track; the lines before and the
            // playing track's adding up to at least half a line, and both
            // over 30 % only briefly.
            bool both = false;
            for (const QString &name : lines) {
                const PanelLine line = panelLine(panel, name);
                if (!line.line()->isVisible() || line.text().isEmpty()) {
                    continue;
                }
                const double column = opacityUpTo(line.line(), panel);
                if (!fades) {
                    QVERIFY2(lineStill(panel, name), qPrintable(name + QLatin1Char(' ') + where));
                    continue;
                }
                const double before = line.outgoingOpacity() * column;
                const double playing = line.incomingOpacity() * column;
                QVERIFY2(before + playing >= 0.5,
                         qPrintable(QStringLiteral("%1 %2: %3 + %4").arg(name, where).arg(before).arg(playing)));
                both = both || (before > 0.3 && playing > 0.3 && line.fades());
            }
            overlapping += both && step < steps ? 1 : 0;
        }
        QVERIFY2(overlapping * duration / double(steps) <= 45.0,
                 qPrintable(QStringLiteral("%1 ms").arg(overlapping * duration / double(steps))));
        QVERIFY(QMetaObject::invokeMethod(growth, "resume"));
        if (fades) {
            QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
        }
        QVERIFY(QTest::qWaitFor([&]() {
            return !growth->property("running").toBool() && !handover->property("running").toBool();
        }, 5000));
        QTRY_VERIFY(libraryFollows(panel, header));
        QVERIFY(panelIdle(panel, lines));
        QCOMPARE(markShown(), !toCover);
        QCOMPARE(coverShown(), toCover);
        if (toCover) {
            QCOMPARE(cover->width(), narrow ? 96.0 : panel->width());
        }
    };
    sampled(false);
    sampled(true);

    // At speed: everything ends with the change, and nothing moves after.
    for (const bool toCover : {false, true}) {
        change(toCover);
        QVERIFY(growth->property("running").toBool());
        const bool fades = handover->property("running").toBool();
        QVERIFY(QTest::qWaitFor([&]() { return !growth->property("running").toBool(); }, 5000));
        QVERIFY2(!fades || !handover->property("running").toBool(), "the lines' fade ends after the change");
        QVERIFY(!qvariant_cast<QObject *>(panel->property("presenceAnimation"))->property("running").toBool());
        QTRY_VERIFY(libraryFollows(panel, header));
        const QQuickItem *column = itemNamed(panel, narrow ? QStringLiteral("nowPlayingNarrowText")
                                                           : QStringLiteral("nowPlayingWideText"));
        const QRectF settled[] = {sceneRectOf(header), sceneRectOf(const_cast<QQuickItem *>(column)),
                                  sceneRectOf(mark), sceneRectOf(cover)};
        const double markOpacity = mark->opacity();
        const double coverOpacity = cover->opacity();
        QTest::qWait(3 * duration / 2);
        QCOMPARE(sceneRectOf(header), settled[0]);
        QCOMPARE(sceneRectOf(const_cast<QQuickItem *>(column)), settled[1]);
        QCOMPARE(sceneRectOf(mark), settled[2]);
        QCOMPARE(sceneRectOf(cover), settled[3]);
        QCOMPARE(mark->opacity(), markOpacity);
        QCOMPARE(cover->opacity(), coverOpacity);
        QCOMPARE(markShown(), !toCover);
        QCOMPARE(cover->property("coverSourceSize").toDouble(), sourceSize);
    }
}

// With animations off, a track without a cover takes its layout, with no
// cover's place, at once, and one with a cover the wide one.
void UiMotionTest::nowPlayingCoverLayoutChangesAtOnceWithAnimationsOff() {
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
    QQuickItem *cover = named(window, "nowPlayingCover");
    auto *growth = qvariant_cast<QObject *>(panel->property("coverGrowthAnimation"));
    QVERIFY(growth != nullptr);
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("wide"));
    QCOMPARE(cover->width(), panel->width());

    QQuickItem *mark = named(window, "nowPlayingBareMark");
    QQuickItem *area = named(window, "nowPlayingCoverArea");
    QVERIFY(mark && area);
    QVERIFY(!mark->isVisible());

    showTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), QString());
    QVERIFY(!growth->property("running").toBool());
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("bare"));
    QCOMPARE(panel->property("coverGrowth").toDouble(), 0.0);
    QVERIFY(!area->isVisible());
    QVERIFY(mark->isVisible());
    QCOMPARE(mark->opacity(), 1.0);
    QCOMPARE(panel->implicitHeight(), panel->property("bareLayoutHeight").toDouble());
    QVERIFY(panelIdle(panel, kWideLines));

    bridge->setProperty("currentTrackCoverPath", covers.second);
    QVERIFY(!growth->property("running").toBool());
    QCOMPARE(panel->property("coverGrowth").toDouble(), 1.0);
    QCOMPARE(cover->width(), panel->width());
    QCOMPARE(cover->opacity(), 1.0);
    QVERIFY(!mark->isVisible());
    QCOMPARE(panel->implicitHeight(), panel->property("wideLayoutHeight").toDouble());
}

// Back to nothing loaded, as when the queue is cleared, the full layout
// shrinks into the compact row over longDuration, from the height it showed,
// so the library does not jump; and so does a growth cut short.
void UiMotionTest::nowPlayingShrinksBackIntoTheCompactRow() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setLongDuration(1000);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QQuickItem *header = named(window, "libraryHeader");
    QQuickItem *row = named(window, "nowPlayingIdleRow");
    auto *growth = qvariant_cast<QObject *>(panel->property("presenceAnimation"));
    QVERIFY(growth != nullptr);
    QTRY_VERIFY(libraryFollows(panel, header));
    const double rest = panel->property("restHeight").toDouble();
    const double idleTop = sceneRectOf(header).top();
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first);
    bridge->setProperty("currentTrackArtist", QStringLiteral("Artist A"));
    QVERIFY(grown(panel));
    QTRY_VERIFY(libraryFollows(panel, header));
    const double full = panel->height();
    const double fullTop = sceneRectOf(header).top();

    // From the height it showed, though the lines that gave it are gone.
    clearPlayback(bridge);
    QVERIFY(panel->property("idle").toBool());
    QVERIFY(growth->property("running").toBool());
    QCOMPARE(growth->property("to").toDouble(), 0.0);
    QCOMPARE(easingOf(growth), QEasingCurve::OutCubic);
    QCOMPARE(panel->implicitHeight(), full);
    QVERIFY(QMetaObject::invokeMethod(growth, "pause"));
    QTRY_VERIFY(libraryFollows(panel, header));
    QCOMPARE(sceneRectOf(header).top(), fullTop);
    for (const double at : {0.75, 0.5, 0.25}) {
        panel->setProperty("presence", at);
        const QString where = QString::number(at);
        QTRY_VERIFY2(libraryFollows(panel, header), qPrintable(where));
        QVERIFY2(std::abs(panel->height() - grownHeight(panel, at, full)) <= 0.5, qPrintable(where));
        QCOMPARE(row->isVisible(), at < 0.5);
    }
    QVERIFY(QMetaObject::invokeMethod(growth, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !growth->property("running").toBool(); }, 5000));
    QTRY_VERIFY(libraryFollows(panel, header));
    QCOMPARE(panel->height(), rest);
    QCOMPARE(sceneRectOf(header).top(), idleTop);
    QVERIFY(row->isVisible());
    QCOMPARE(row->opacity(), 1.0);
    QVERIFY(!named(window, "nowPlayingCoverArea")->isVisible());

    // A growth cut short shrinks from where it got to.
    showTrack(bridge, kTrackA2, QStringLiteral("Second"), QStringLiteral("Album A"), covers.first);
    QVERIFY(QMetaObject::invokeMethod(growth, "pause"));
    panel->setProperty("presence", 0.6);
    QTRY_VERIFY(libraryFollows(panel, header));
    const double partway = panel->height();
    QVERIFY(partway > rest && partway < full);
    clearPlayback(bridge);
    QVERIFY(growth->property("running").toBool());
    QCOMPARE(growth->property("from").toDouble(), 0.6);
    QCOMPARE(panel->implicitHeight(), partway);
    QVERIFY(QMetaObject::invokeMethod(growth, "complete"));
    QCOMPARE(panel->implicitHeight(), rest);
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
    QVERIFY(panel->property("idle").toBool());

    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), covers.first,
              coverFirst ? Order::CoverFirst : Order::PathFirst);
    QVERIFY(!handover->property("running").toBool());
    QVERIFY(!shownIn(panel, "albumArtOutgoing"));
    QVERIFY(!anyLineFades(panel));
    QVERIFY(allLinesStill(panel));
    QVERIFY(grown(panel));

    bridge->setProperty("currentTrackCoverPath", covers.second);
    QVERIFY(handover->property("running").toBool());
    QVERIFY(shownIn(panel, "albumArtOutgoing"));
    QVERIFY(!anyLineFades(panel));
}

void UiMotionTest::nowPlayingLateCoverKeepsEveryLayerContinuous_data() {
    QTest::addColumn<double>("at");
    // The cover comes early in the cross-fade of the lines, and late.
    QTest::newRow("going out") << 0.25;
    QTest::newRow("coming in") << 0.75;
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
    QVERIFY(grown(panel));
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
    // lines before out as the playing track's come in, from where they were.
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
        QVERIFY(sameOpacity(nextLines + nextLive, 1.0));
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
    // C comes early in the cross-fade of A's lines and B's, and late.
    QTest::newRow("going out") << 0.3;
    QTest::newRow("coming in") << 0.7;
}

// A → B, B's own cover late, then C during that joined fade (#463): C's fade
// starts from what shows. A's lines go on out from where they got to; B's,
// as far as they came in, go out as layers of their own, each part from
// where it got to and cut in B's own gaps; what stayed still and C's lines
// change, B's go out whole; B's cover goes out whole, as it showed, under
// A's. No layer jumps, each then goes one way only, C's lines come in from
// nothing as the others go out, and the fade ends on C alone.
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
    QVERIFY(grown(panel));
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
    // A's lines, from where they were when the cover came, going out as
    // B's come in, on one progress.
    const double a = linesOut(0.25) * linesOut(at);
    const double b = 1.0 - a;
    QVERIFY(a > 0.0 && b > 0.0);
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

    // The title: A's as far as it went out and B's as far as it came in;
    // C's over them, from 0.
    QCOMPARE(title.text(), QStringLiteral("Third"));
    const QList<QQuickItem *> titleLayers = layersOf(title);
    QCOMPARE(titleLayers.size(), 2);
    QCOMPARE(partText(title, partsOf(title, titleLayers.first()).first()), QStringLiteral("First"));
    QCOMPARE(partsOf(title, titleLayers.first()).first()->opacity(), a);
    QCOMPARE(partText(title, partsOf(title, titleLayers.last()).first()), QStringLiteral("Second"));
    QCOMPARE(partsOf(title, titleLayers.last()).first()->opacity(), b);
    QCOMPARE(title.incomingOpacity(), 0.0);
    // C's details have another year and another number: they change in two
    // places, so the line fades whole (#373 H2), with nothing of it left
    // still. What A's and B's kept still goes out whole, in B's layer, as it
    // showed, and B's number as far as it came in; A's draws nothing there,
    // which B's covers, and A's number goes on where it was.
    QCOMPARE(details.stillTexts(), QStringList());
    const QList<QQuickItem *> detailsLayers = layersOf(details);
    QCOMPARE(detailsLayers.size(), 2);
    const QList<QQuickItem *> partsA = partsOf(details, detailsLayers.first());
    const QList<QQuickItem *> partsB = partsOf(details, detailsLayers.last());
    QCOMPARE(partsA.size(), 1);
    QCOMPARE(partsB.size(), 2);
    QCOMPARE(partText(details, partsB.first()), QStringLiteral("2005 · Rock · Track 2"));
    const QFontMetricsF metrics(details.font());
    const QString shared = QStringLiteral("2005 · Rock · Track");
    // Where a number starts: in the gap after what A and B share.
    const auto inTheNumbersGap = [&](double x) {
        return x > metrics.horizontalAdvance(shared) && x < metrics.horizontalAdvance(shared + QLatin1Char(' '));
    };
    QCOMPARE(partsB.at(0)->x(), 0.0);
    QVERIFY(inTheNumbersGap(partsB.at(0)->x() + partsB.at(0)->width()));
    QCOMPARE(partsB.at(0)->opacity(), 1.0);
    QVERIFY(inTheNumbersGap(partsB.at(1)->x()));
    QCOMPARE(partsB.at(1)->opacity(), b);
    QCOMPARE(partText(details, partsA.first()), QStringLiteral("2005 · Rock · Track 1"));
    QVERIFY(inTheNumbersGap(partsA.first()->x()));
    QCOMPARE(partsA.first()->opacity(), a);
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
        QCOMPARE(partsOf(title, titleLayers.first()).first()->opacity(), a * level);
        QCOMPARE(partsOf(title, titleLayers.last()).first()->opacity(), b * level);
        QCOMPARE(title.incomingOpacity(), linesIn(next));
        QCOMPARE(partsB.at(0)->opacity(), level);
        QCOMPARE(partsB.at(1)->opacity(), b * level);
        QCOMPARE(partsA.first()->opacity(), a * level);
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
        // Late in C's fade: A's lines nearly out, and C's go out from where
        // they got to; the covers are two already.
        {QStringLiteral("/music/Artist A/Album D/01.flac"), QStringLiteral("Fourth"), QStringLiteral("Album D"), 2004,
         0.75, true, false},
        // Early in D's fade: the lines fade out two layers already, so D's
        // own takes E's text at the opacity it has.
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
            QVERIFY(grown(panel));
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
        const QStringList expected = index <= 2
            ? QStringList{QStringLiteral("First")}
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

// A, with a cover, changes to B, without one, and C, of another album and
// without a cover either, comes during that change: the change to no
// cover goes on as it was, A's cover shrinking away in its place, with no
// placeholder in it and no cover fading over another, and the lines go on
// from what they show. In the narrow window too.
void UiMotionTest::nowPlayingInterruptedFadeAddsNoPlaceholderOverItself() {
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    window->resize(1000, 720);
    QTRY_COMPARE(window->width(), 1000);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    QObject *growth = qvariant_cast<QObject *>(panel->property("coverGrowthAnimation"));
    QQuickItem *tile = itemNamed(panel, QStringLiteral("nowPlayingCover"));
    QQuickItem *placeholder = itemNamed(tile, QStringLiteral("albumArtPlaceholderGlyph"));
    QVERIFY(handover && growth && tile && placeholder);
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, covers.first);
    QTRY_VERIFY(panelLine(panel, QStringLiteral("NarrowTitle")).line()->width() > 0);
    QVERIFY(grown(panel));
    tellTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2007, 1, QString());
    QVERIFY(handover->property("running").toBool());
    QVERIFY(growth->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    QVERIFY(QMetaObject::invokeMethod(growth, "pause"));
    panel->setProperty("handover", 0.45);
    panel->setProperty("layoutStep", 0.45);
    const double growthAt = panel->property("coverGrowth").toDouble();
    QVERIFY(growthAt > 0 && growthAt < 1);
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{{covers.first, 1.0}}));
    QVERIFY(!placeholder->isVisible());
    const PanelLook before = panelLook(panel, kNarrowLines);

    tellTrack(bridge, QStringLiteral("/music/Artist A/Album C/01.flac"), QStringLiteral("Third"),
              QStringLiteral("Album C"), 2009, 1, QString());
    QVERIFY(handover->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    QCOMPARE(panel->property("handover").toDouble(), 0.0);
    // The change to no cover goes on where it was.
    QCOMPARE(panel->property("layoutStep").toDouble(), 0.45);
    QCOMPARE(panel->property("coverGrowth").toDouble(), growthAt);
    const QString lineChange = lookChange(before, panelLook(panel, kNarrowLines), kNarrowLines, true, true);
    QVERIFY2(lineChange.isEmpty(), qPrintable(lineChange));
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{{covers.first, 1.0}}));
    QVERIFY(!placeholder->isVisible());
    QVERIFY(!shownIn(panel, "albumArtOutgoing"));

    QVERIFY(QMetaObject::invokeMethod(growth, "resume"));
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    bool placeholderShown = false;
    QVERIFY(QTest::qWaitFor([&]() {
        placeholderShown = placeholderShown || (placeholder->isVisible() && tile->isVisible());
        return !handover->property("running").toBool() && !growth->property("running").toBool();
    }, 5000));
    QVERIFY(!placeholderShown);
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("bare"));
    QVERIFY(panelIdle(panel, kNarrowLines));
}

// Another album, neither with a cover: the placeholder stays as it is, with
// nothing over it, and the lines fade over longDuration all the same, in
// the narrow layout a track without a cover takes (#373 H8).
void UiMotionTest::nowPlayingAnotherAlbumWithoutCoversFadesItsLinesLong() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    // The narrow layout, which a track without a cover takes (#373 H8),
    // whatever the cover.
    window->resize(1000, 720);
    QTRY_COMPARE(window->width(), 1000);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    tellTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"), 2005, 1, QString());
    QTRY_VERIFY(panelLine(panel, QStringLiteral("NarrowTitle")).line()->width() > 0);
    tellTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"), 2007, 1, QString());
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), units->longDuration());
    QVERIFY(panelLine(panel, QStringLiteral("NarrowTitle")).fades());
    QVERIFY(!shownIn(panel, "albumArtOutgoing"));
    QCOMPARE(coverLook(panel), (QList<std::pair<QString, double>>{{QString(), 1.0}}));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(panelIdle(panel, kNarrowLines));
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
// the rows below slide down from where they were over longDuration with
// OutCubic, uncovering the children. Collapsing is the same played back, on
// the same duration and curve (#373 H7: the expand was instant beside the
// collapse's fade). The children show only above the fold, the top of the
// rows sliding, at full opacity: no child's text is ever under a sliding
// row's, as it was while they faded under them (#373 H9 T5).
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
    auto *fold = qvariant_cast<QObject *>(view->property("foldAnimation"));
    QVERIFY(open != nullptr && close != nullptr && fold != nullptr);
    for (QObject *animation : {open, close, fold}) {
        QCOMPARE(animation->property("duration").toInt(), units->longDuration());
        QCOMPARE(easingOf(animation), QEasingCurve::OutCubic);
    }

    QQuickItem *albumA = libraryRowWithKey(window, kAlbumAKey);
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = view->property("rowHeight").toDouble();
    const double albumAY = albumA->y();
    QCOMPARE(albumB->y(), 2 * rowHeight);
    const auto shown = [](QQuickItem *row) {
        auto *clip = row->findChild<QQuickItem *>(QStringLiteral("libraryRowClip"));
        return clip != nullptr && clip->isVisible() ? clip->height() : 0.0;
    };
    // The children's drawn part ends at the top of the row sliding, or
    // above it, and they never fade.
    const auto checkFold = [&](const QList<QQuickItem *> &children, const QString &where) {
        for (QQuickItem *child : children) {
            QVERIFY2(shown(child) == 0.0 || child->y() + shown(child) <= albumB->y() + 0.01,
                     qPrintable(QStringLiteral("%1: a child to %2 under Album B at %3")
                                    .arg(where).arg(child->y() + shown(child)).arg(albumB->y())));
            QCOMPARE(child->opacity(), 1.0);
        }
        QCOMPARE(albumB->opacity(), 1.0);
    };

    SteppedAnimationDriver driver;
    const int duration = units->longDuration();
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    // Laid out at once: the children are in place, Album A has not moved,
    // and Album B starts where it was, over the children, which show none
    // of themselves yet.
    QCOMPARE(view->property("count").toInt(), 5);
    QCOMPARE(albumA->y(), albumAY);
    QQuickItem *firstChild = libraryRowWithPath(window, kTrackA1);
    QQuickItem *secondChild = libraryRowWithPath(window, kTrackA2);
    QVERIFY(firstChild != nullptr && secondChild != nullptr);
    QCOMPARE(firstChild->y(), 2 * rowHeight);
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QVERIFY(albumB->z() > firstChild->z());
    QCOMPARE(shown(firstChild), 0.0);
    QCOMPARE(shown(secondChild), 0.0);
    // Frame by frame, Album B on its way down uncovers the children.
    int expandMs = 0;
    bool halfShown = false;
    while (albumB->y() < 4 * rowHeight || view->property("folding").toBool()) {
        QVERIFY(driver.step(16));
        expandMs += 16;
        QVERIFY2(expandMs <= duration + 48, qPrintable(QString::number(albumB->y())));
        checkFold({firstChild, secondChild}, QStringLiteral("expand at %1 ms").arg(expandMs));
        QCOMPARE(albumA->y(), albumAY);
        halfShown = halfShown || (shown(firstChild) > 0 && shown(secondChild) < rowHeight);
    }
    QVERIFY(halfShown);
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QCOMPARE(albumB->z(), 1.0);
    QCOMPARE(shown(firstChild), rowHeight);
    QCOMPARE(shown(secondChild), rowHeight);

    // Collapsing: Album B slides back up over the children, the expand
    // played back, over the same time.
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QCOMPARE(view->property("count").toInt(), 3);
    QCOMPARE(albumA->y(), albumAY);
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QCOMPARE(shown(firstChild), rowHeight);
    int collapseMs = 0;
    halfShown = false;
    while (albumB->y() > 2 * rowHeight || view->property("folding").toBool()) {
        QVERIFY(driver.step(16));
        collapseMs += 16;
        QVERIFY2(collapseMs <= duration + 48, qPrintable(QString::number(albumB->y())));
        if (firstChild->isVisible() && secondChild->isVisible()) {
            checkFold({firstChild, secondChild}, QStringLiteral("collapse at %1 ms").arg(collapseMs));
            halfShown = halfShown || (shown(firstChild) > 0 && shown(secondChild) < rowHeight);
        }
    }
    QVERIFY(halfShown);
    // As long, to the test's frame.
    QVERIFY2(std::abs(collapseMs - expandMs) <= 16,
             qPrintable(QStringLiteral("collapse %1 ms, expand %2 ms").arg(collapseMs).arg(expandMs)));
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QCOMPARE(albumA->y(), albumAY);
}

// The backend sends a collapsed row's children only once the row is
// expanded, with a later tree: they open then as a toggle's, Album B
// sliding down from where it was and uncovering them, not at once (#373
// H10: in the app, expanding was instant while collapsing slid). A tree
// without them, sent before the backend was asked, moves nothing.
void UiMotionTest::treeToggleOpensChildrenThatArriveLater() {
    LibraryTreeModel libraryModel;
    libraryModel.setExpandedKeys({kArtistKey});
    libraryModel.setLibraryTreeFromBinary(libraryTree(false));
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(QTest::qWaitFor([&]() { return view->property("count").toInt() == 3; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QQuickItem *albumA = libraryRowWithKey(window, kAlbumAKey);
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = view->property("rowHeight").toDouble();
    const double albumAY = albumA->y();
    QCOMPARE(albumB->y(), 2 * rowHeight);
    const auto shown = [](QQuickItem *row) {
        auto *clip = row->findChild<QQuickItem *>(QStringLiteral("libraryRowClip"));
        return clip != nullptr && clip->isVisible() ? clip->height() : 0.0;
    };
    // A tree as the bridge delivers it.
    const auto applyTree = [&](int version, bool albumATracks) {
        return QMetaObject::invokeMethod(controller, "requestTreeApply", Q_ARG(QVariant, version),
                                         Q_ARG(QVariant, QVariant(libraryTree(albumATracks))));
    };

    SteppedAnimationDriver driver;
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    // Nothing to open yet.
    QCOMPARE(view->property("count").toInt(), 3);
    QVERIFY(!view->property("folding").toBool());
    QCOMPARE(albumB->y(), 2 * rowHeight);

    // A tree from before the backend was asked: still nothing.
    QVERIFY(applyTree(2, false));
    QCOMPARE(view->property("count").toInt(), 3);
    QVERIFY(!view->property("folding").toBool());

    // The children come: in place at once, under Album B, which starts
    // where it was.
    QVERIFY(applyTree(3, true));
    QCOMPARE(view->property("count").toInt(), 5);
    QCOMPARE(albumA->y(), albumAY);
    QQuickItem *firstChild = libraryRowWithPath(window, kTrackA1);
    QQuickItem *secondChild = libraryRowWithPath(window, kTrackA2);
    QVERIFY(firstChild != nullptr && secondChild != nullptr);
    QCOMPARE(firstChild->y(), 2 * rowHeight);
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QVERIFY(view->property("folding").toBool());
    QCOMPARE(shown(firstChild), 0.0);
    QCOMPARE(shown(secondChild), 0.0);
    // Then as a toggle with its children loaded: over longDuration, the
    // children never under Album B.
    int expandMs = 0;
    bool halfShown = false;
    while (albumB->y() < 4 * rowHeight || view->property("folding").toBool()) {
        QVERIFY(driver.step(16));
        expandMs += 16;
        QVERIFY2(expandMs <= units->longDuration() + 48, qPrintable(QString::number(albumB->y())));
        for (QQuickItem *child : {firstChild, secondChild}) {
            QVERIFY(shown(child) == 0.0 || child->y() + shown(child) <= albumB->y() + 0.01);
        }
        halfShown = halfShown || (shown(firstChild) > 0 && shown(secondChild) < rowHeight);
    }
    QVERIFY(halfShown);
    QVERIFY(expandMs >= units->longDuration() - 16);
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QCOMPARE(shown(secondChild), rowHeight);

    // The next tree, the same rows again, moves nothing.
    QVERIFY(applyTree(4, true));
    QVERIFY(!view->property("folding").toBool());
    QCOMPARE(albumB->y(), 4 * rowHeight);
}

// With animations off, a toggle moves the rows at once, both ways, and the
// children show whole (#373 H9 T5).
void UiMotionTest::treeToggleIsInstantWithAnimationsOff() {
    LibraryTreeModel libraryModel;
    showTree(libraryModel, {kArtistKey});
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setShortDuration(0);
    units->setLongDuration(0);
    QQuickItem *view = named(window, "libraryView");
    QVERIFY(QTest::qWaitFor([&]() { return view->property("count").toInt() == 3; }));
    QObject *controller = qvariant_cast<QObject *>(named(window, "libraryPane")->property("controller"));
    QQuickItem *albumB = libraryRowWithKey(window, kAlbumBKey);
    const double rowHeight = view->property("rowHeight").toDouble();

    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QCOMPARE(view->property("count").toInt(), 5);
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QVERIFY(!view->property("folding").toBool());
    QQuickItem *firstChild = libraryRowWithPath(window, kTrackA1);
    QVERIFY(firstChild != nullptr);
    auto *clip = firstChild->findChild<QQuickItem *>(QStringLiteral("libraryRowClip"));
    QVERIFY(clip != nullptr && clip->isVisible());
    QCOMPARE(clip->height(), rowHeight);
    QCOMPARE(firstChild->opacity(), 1.0);

    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QCOMPARE(view->property("count").toInt(), 3);
    QCOMPARE(albumB->y(), 2 * rowHeight);
    QVERIFY(!view->property("folding").toBool());
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
    // And open again: the children whole at once, as the collapse left
    // nothing to fade.
    QMetaObject::invokeMethod(controller, "toggleNode", Q_ARG(QVariant, kAlbumAKey));
    QCOMPARE(albumB->y(), 4 * rowHeight);
    QVERIFY(rowsSettled(tree, tree->property("count").toInt()));
    QQuickItem *child = libraryRowWithPath(window, kTrackA1);
    QVERIFY(child != nullptr);
    QCOMPARE(child->opacity(), 1.0);
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
// curve; the mark of a track without a cover shows only for a track known
// to have none, once the cover before is gone (#373 H9), and no placeholder
// shows in the cover's place.
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
    QQuickItem *glyph = itemNamed(panel, QStringLiteral("nowPlayingBareMark"));
    QQuickItem *placeholder = itemNamed(panel, QStringLiteral("albumArtPlaceholderGlyph"));
    QVERIFY(placeholder != nullptr);
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

    // A track known to have no cover: the mark shows once its metadata
    // says so, and not before, after the cover before is gone.
    showTrack(bridge, kTrackA2, QStringLiteral("Third"), QStringLiteral("Album A"), covers.second);
    QVERIFY(!glyph->isVisible());
    QCOMPARE(placeholderShown, 0);
    bridge->setProperty("currentTrackCoverPath", QString());
    QCOMPARE(panel->property("layoutMode").toString(), QStringLiteral("bare"));
    QVERIFY(!glyph->isVisible());
    bool tilePlaceholder = false;
    QVERIFY(QTest::qWaitFor([&]() {
        tilePlaceholder = tilePlaceholder || placeholder->isVisible();
        return glyph->isVisible();
    }, 5000));
    QVERIFY(!tilePlaceholder);
    QVERIFY(!named(window, "nowPlayingCover")->isVisible() || named(window, "nowPlayingCover")->opacity() == 0.0);
    QCOMPARE(placeholderShown, 1);
    QObject::disconnect(connection);
}

namespace {

// The tests of one shard, with FERROUS_TEST_SHARD=<shard>/<shards>. CTest
// runs each shard in a process of its own, all in parallel
// (ui/CMakeLists.txt). A test is a row of a data-driven function, or a
// function without data: a few functions have many rows that take seconds
// each, and shards of whole functions came out unevenly. In the order of
// `-datatags`, the declaration order, a shard has every shards-th test from
// the shard-th. The rule needs no list kept up to date, and the rows of a
// function, alike in cost, go to every shard. Without the variable,
// `sharded` is false and every test runs.
struct TestShard {
    bool sharded = false;
    // The lines of `-datatags` for the shard's tests: "<class> <function>"
    // or "<class> <function> <tag>".
    QStringList listing;
    // The tests as QTest takes them on its command line: "<function>" or
    // "<function>:<tag>".
    QStringList tests;
    QString error;
};

TestShard testShardFromEnvironment() {
    TestShard shard;
    const QString value = qEnvironmentVariable("FERROUS_TEST_SHARD");
    if (value.isEmpty()) {
        return shard;
    }
    shard.sharded = true;
    const QStringList parts = value.split(QLatin1Char('/'));
    bool indexValid = false;
    bool countValid = false;
    const int index = parts.size() == 2 ? parts.at(0).toInt(&indexValid) : 0;
    const int count = parts.size() == 2 ? parts.at(1).toInt(&countValid) : 0;
    if (!indexValid || !countValid || index < 1 || index > count) {
        shard.error = QStringLiteral("FERROUS_TEST_SHARD is \"%1\", not <shard>/<shards> with 1 <= shard <= shards")
                          .arg(value);
        return shard;
    }
    // QTest lists the tests, rows and all, only on its standard output and
    // only to exit after: ask this binary, unsharded, in a process of its own.
    QProcess listing;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("FERROUS_TEST_SHARD"));
    listing.setProcessEnvironment(environment);
    listing.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-datatags")});
    if (!listing.waitForFinished(30000) || listing.exitStatus() != QProcess::NormalExit
        || listing.exitCode() != 0) {
        shard.error = QStringLiteral("Could not list the tests with -datatags: %1 %2")
                          .arg(listing.errorString(), QString::fromLocal8Bit(listing.readAllStandardError()));
        return shard;
    }
    const QStringList lines = QString::fromLocal8Bit(listing.readAllStandardOutput()).split(QLatin1Char('\n'));
    int position = 0;
    for (const QString &line : lines) {
        const QStringList fields = line.split(QLatin1Char(' '));
        if (fields.size() < 2) {
            continue;
        }
        if (position++ % count != index - 1) {
            continue;
        }
        shard.listing << line;
        const QString tag = line.section(QLatin1Char(' '), 2);
        shard.tests << (tag.isEmpty() ? fields.at(1) : fields.at(1) + QLatin1Char(':') + tag);
    }
    if (shard.tests.isEmpty()) {
        shard.error = QStringLiteral("Shard %1 has no tests").arg(value);
    }
    return shard;
}

} // namespace

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
    const TestShard shard = testShardFromEnvironment();
    if (!shard.error.isEmpty()) {
        // To standard error, not with qCritical(): before qExec(), Qt logs to
        // the journal where standard error is not a terminal, as under CTest.
        std::fprintf(stderr, "%s\n", qPrintable(shard.error));
        return 1;
    }
    if (!shard.sharded) {
        return QTest::qExec(&test, argc, argv);
    }
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments << QString::fromLocal8Bit(argv[i]);
    }
    // QTest's -datatags lists every test: list the shard's, which
    // check_test_shards.cmake checks against it.
    if (arguments.contains(QStringLiteral("-datatags"))) {
        for (const QString &line : shard.listing) {
            std::printf("%s\n", qPrintable(line));
        }
        return 0;
    }
    return QTest::qExec(&test, arguments + shard.tests);
}

void UiMotionTest::nowPlayingChangedLinesCrossFadeWithoutABlink_data() {
    QTest::addColumn<bool>("narrow");
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("wide") << false << 0.0;
    QTest::newRow("narrow") << true << 0.0;
    // 24 pt: the lines under the cover, the title and the artist wrapping.
    QTest::newRow("narrow, stacked") << true << 24.0;
}

// A line that changes cross-fades (#373 H6: out, then in, left each line
// under half its ink for up to 50 ms): the line before goes out as the
// playing track's comes in, at the same time, so at every point of every
// line, in every frame and at every point of the fade the test sets, the
// two draw with at least 0.6 of a whole text's ink between them, and never
// more than one text's opacity in all. Both texts show at once, mid-fade;
// at its end the playing track's is at exactly 1 and the line before at 0.
// Lines that do not change stay still. Through the next track of an album,
// another album, metadata that comes late, and a third track early and
// late in the fade.
void UiMotionTest::nowPlayingChangedLinesCrossFadeWithoutABlink() {
    QFETCH(bool, narrow);
    QFETCH(qreal, pointSize);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    if (pointSize > 0) {
        QFont font = window->property("font").value<QFont>();
        font.setPointSizeF(pointSize);
        QVERIFY(window->setProperty("font", font));
        QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
        QVERIFY(palette != nullptr);
        QTRY_COMPARE(palette->property("basePointSize").toReal(), pointSize);
    }
    if (narrow) {
        window->resize(1000, 720);
    }
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    // Long enough for many frames of the fade.
    units->setShortDuration(400);
    units->setLongDuration(600);
    const QStringList names = narrow ? kNarrowLines : kWideLines;
    bridge->setProperty("currentTrackArtist", QStringLiteral("Lantern Signal"));
    bridge->setProperty("currentTrackGenre", QStringLiteral("Ambient"));
    tellTrack(bridge, kTrackA1, QStringLiteral("Moth Copper"), QStringLiteral("Album A"), 2005, 1, covers.all.at(0));
    QTRY_COMPARE(panel->property("layoutMode").toString(), narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QCOMPARE(panel->property("narrowStacked").toBool(), pointSize > 0);
    QTRY_VERIFY(panelLine(panel, names.first()).line()->width() > 0);
    QVERIFY(grown(panel));
    QVERIFY(panelIdle(panel, names));

    // The lines that must stay still through the fade at hand.
    QStringList stillNames;
    // Where a line that fades drops under 0.6 of its ink or draws more than
    // one text's, or a line that does not change does not stay still; or "".
    int crossFading = 0;
    const auto blink = [&]() -> QString {
        for (const QString &name : names) {
            const PanelLine line = panelLine(panel, name);
            if (stillNames.contains(name)) {
                if (!lineStill(panel, name)) {
                    return QStringLiteral("%1 does not stay still").arg(name);
                }
                continue;
            }
            if (!line.fades() && !line.incoming()->isVisible()) {
                continue;
            }
            if (line.outgoingOpacity() > 0 && line.incomingOpacity() > 0 && line.incomingOpacity() < 1) {
                ++crossFading;
            }
            for (double x = 0.5; x < line.line()->width(); x += 1.0) {
                const auto [ink, sum] = inkAt(line, x);
                if (ink < 0.6 || sum > 1.0 + 1e-9) {
                    return QStringLiteral("%1 at %2 px, handover %3: ink %4, opacities %5")
                        .arg(name).arg(x).arg(panel->property("handover").toDouble()).arg(ink).arg(sum);
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
                failure = blink();
            }
        });
    // Along a fade at the points the test sets, at its very end, then to
    // its end in frames.
    const auto followTheFade = [&](const char *what) {
        QVERIFY2(handover->property("running").toBool(), what);
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        crossFading = 0;
        for (const double at : {0.0, 0.05, 0.1, 0.25, 0.4, 0.5, 0.6, 0.75, 0.9, 0.95}) {
            panel->setProperty("handover", at);
            const QString each = blink();
            QVERIFY2(each.isEmpty(), qPrintable(QString::fromLatin1(what) + QStringLiteral(": ") + each));
        }
        QVERIFY2(crossFading > 0, what);
        // At its end: each text whole or gone, exactly.
        panel->setProperty("handover", 1.0);
        for (const QString &name : names) {
            const PanelLine line = panelLine(panel, name);
            for (QQuickItem *part : line.parts()) {
                QVERIFY2(part->opacity() == 0.0, qPrintable(QString::fromLatin1(what) + QLatin1Char(' ') + name));
            }
            QVERIFY2(line.incomingOpacity() == 1.0, qPrintable(QString::fromLatin1(what) + QLatin1Char(' ') + name));
        }
        panel->setProperty("handover", 0.0);
        QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
        QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
        QVERIFY2(failure.isEmpty(), qPrintable(QString::fromLatin1(what) + QStringLiteral(": ") + failure));
        QVERIFY(panelIdle(panel, names));
        stillNames.clear();
    };

    // The next track of the album: the artist and the album stay still.
    stillNames = narrow ? QStringList{QStringLiteral("NarrowArtist"), QStringLiteral("NarrowAlbum")}
                        : QStringList{QStringLiteral("ArtistAlbum")};
    tellTrack(bridge, kTrackA2, QStringLiteral("Hollow Arc"), QStringLiteral("Album A"), 2005, 2, covers.all.at(0));
    followTheFade("the next track");

    // Another album, with its cover.
    bridge->setProperty("currentTrackArtist", QStringLiteral("Mlantern Signal"));
    tellTrack(bridge, kTrackB1, QStringLiteral("Moth Copper"), QStringLiteral("Album B"), 2007, 1, covers.all.at(1));
    followTheFade("another album");

    // Metadata that comes late, early and late in the fade.
    for (const double lateAt : {0.25, 0.75}) {
        tellTrack(bridge, lateAt < 0.5 ? kTrackA1 : kTrackA2, QStringLiteral("Hollow Arc"), QStringLiteral("Album B"),
                  2007, lateAt < 0.5 ? 2 : 3, covers.all.at(1));
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        panel->setProperty("handover", lateAt);
        bridge->setProperty("currentTrackGenre", lateAt < 0.5 ? QStringLiteral("Drone") : QStringLiteral("Ambient"));
        followTheFade("late metadata");
    }

    // A third track, early and late in the fade.
    for (const double thirdAt : {0.25, 0.75}) {
        tellTrack(bridge, kTrackA1, QStringLiteral("Lantern"), QStringLiteral("Album A"), 2005, 4, covers.all.at(0));
        QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
        panel->setProperty("handover", thirdAt);
        const QString each = blink();
        QVERIFY2(each.isEmpty(), qPrintable(each));
        bridge->setProperty("currentTrackArtist", QStringLiteral("Signal"));
        tellTrack(bridge, kTrackB1, QStringLiteral("Copper Moth"), QStringLiteral("Album C"), 2009, 5,
                  covers.all.at(2));
        followTheFade("a third track");
    }
    QObject::disconnect(connection);
    QVERIFY(frames > 0);
}

void UiMotionTest::nowPlayingLineFadesWholeUnlessOnlyAWordChanges_data() {
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("before");
    QTest::addColumn<QString>("after");
    QTest::addColumn<QStringList>("still");
    QTest::newRow("the number") << "details" << "2005 · Classical · Track 1 of 2" << "2005 · Classical · Track 2 of 2"
                                << QStringList{"2005 · Classical · Track", "of 2"};
    QTest::newRow("the number of a total") << "narrowDetails" << "Track 1 of 8" << "Track 2 of 8"
                                           << QStringList{"Track", "of 8"};
    QTest::newRow("the year") << "details" << "2019 · Synth-pop" << "2020 · Synth-pop"
                              << QStringList{"· Synth-pop"};
    QTest::newRow("the album") << "artistAlbum" << "Artist A — Album A" << "Artist A — Album B"
                               << QStringList{"Artist A —"};
    // Another album: the year, the number and the total change, and
    // "· Classical · Track" and "of" would be left alone.
    QTest::newRow("in three places") << "details" << "2006 · Classical · Track 1 of 4"
                                     << "2005 · Classical · Track 8 of 8" << QStringList{};
    QTest::newRow("in two places") << "details" << "2005 · Track 2" << "2007 · Track 1" << QStringList{};
    QTest::newRow("most of it") << "artistAlbum" << "Lumen Drift — Afterimage"
                                << "Lumen Drift — The Quiet Meridian Hours" << QStringList{};
    QTest::newRow("a number that gets wider") << "narrowDetails" << "Track 9 of 12" << "Track 10 of 12"
                                              << QStringList{};
    QTest::newRow("a field more") << "details" << "2005 · Rock" << "2005 · Rock · Track 2" << QStringList{};
    QTest::newRow("a field fewer") << "details" << "2005 · Rock · Track 1" << "2005 · Rock" << QStringList{};
    // The number comes before the total: "Classical ·" would be left alone.
    QTest::newRow("the total goes") << "narrowDetails" << "Classical · Track 1 of 4" << "Classical · Track 8"
                                    << QStringList{};
    QTest::newRow("the total comes") << "narrowDetails" << "Classical · Track 8" << "Classical · Track 8 of 8"
                                     << QStringList{};
    QTest::newRow("the year with the album") << "narrowAlbum" << "Album A (2005)" << "Album A (2007)"
                                             << QStringList{"Album A"};
    QTest::newRow("the same") << "details" << "2005 · Rock" << "2005 · Rock" << QStringList{"2005 · Rock"};
}

// What stays still of a line that changes (#373 H2): where only a word or
// a number of it changes, the rest of it, as #537 has it; otherwise
// nothing, and the line fades whole: when it changes in more than one
// place, when less than half of it would stay, or when it has more or
// fewer fields. No word or separator is left alone in a line that is
// otherwise gone.
void UiMotionTest::nowPlayingLineFadesWholeUnlessOnlyAWordChanges() {
    QFETCH(QString, key);
    QFETCH(QString, before);
    QFETCH(QString, after);
    QFETCH(QStringList, still);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    const QVariantList whole{QVariant(QVariantList{0, static_cast<int>(std::max(before.size(), after.size()))})};
    QVariant result;
    QVERIFY(QMetaObject::invokeMethod(named(window, "nowPlayingPanel"), "_keptStill", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, key), Q_ARG(QVariant, whole), Q_ARG(QVariant, before),
                                      Q_ARG(QVariant, after)));
    QStringList runs;
    for (const QVariant &run : result.toList()) {
        const QVariantList range = run.toList();
        QCOMPARE(range.size(), 2);
        const int start = range.at(0).toInt();
        const int end = range.at(1).toInt();
        QCOMPARE(before.mid(start, end - start), after.mid(start, end - start));
        runs.push_back(after.mid(start, end - start));
    }
    QCOMPARE(runs, still);
}

void UiMotionTest::nowPlayingAnotherAlbumLeavesNoWordAlone_data() {
    QTest::addColumn<bool>("narrow");
    QTest::newRow("wide") << false;
    QTest::newRow("narrow") << true;
}

// Another album, told as BridgeClient tells it (#373 H2, the strip's "·
// Classical ·" left alone): the details, whose year, number and total
// change, and the artist and album, fade whole: in every frame and at each
// point the test sets, nothing of them stays still, every piece of the line
// before goes out at one opacity, the playing track's comes in as one, and
// the two cross-fade on one progress (#373 H6). The title fades whole too.
void UiMotionTest::nowPlayingAnotherAlbumLeavesNoWordAlone() {
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
    units->setLongDuration(600);
    bridge->setProperty("currentTrackArtist", QStringLiteral("Lumen Drift"));
    tellDetails(bridge, kTrackA1, QStringLiteral("Moth Copper"), QStringLiteral("Afterimage"), 2006,
                QStringLiteral("Classical"), 1, 4);
    bridge->setProperty("currentTrackCoverPath", covers.all.at(0));
    const QStringList names = narrow ? kNarrowLines : kWideLines;
    QTRY_COMPARE(panel->property("layoutMode").toString(), narrow ? QStringLiteral("narrow") : QStringLiteral("wide"));
    QTRY_VERIFY(panelLine(panel, names.last()).line()->width() > 0);
    QVERIFY(grown(panel));
    QVERIFY(panelIdle(panel, names));

    // Where a line that changes keeps a word still, goes out in pieces at
    // more than one opacity, or goes out and comes in on two progresses;
    // or "".
    const auto apart = [&]() -> QString {
        for (const QString &name : names) {
            const PanelLine line = panelLine(panel, name);
            if (!line.fades() && !line.incoming()->isVisible()) {
                continue;
            }
            if (!line.stillTexts().isEmpty()) {
                return QStringLiteral("%1 keeps %2 still").arg(name, line.stillTexts().join(QLatin1Char('|')));
            }
            if (line.incomingWindows() != QList<Window>{{0.0, line.line()->width()}}) {
                return QStringLiteral("%1 comes in in pieces").arg(name);
            }
            QList<double> levels;
            for (QQuickItem *part : line.parts()) {
                if (part->opacity() > 0 && !levels.contains(part->opacity())) {
                    levels.push_back(part->opacity());
                }
            }
            if (levels.size() > 1) {
                return QStringLiteral("%1 goes out at %2 opacities").arg(name).arg(levels.size());
            }
            if (std::abs(line.outgoingOpacity() + line.incomingOpacity() - 1.0) > 1e-9) {
                return QStringLiteral("%1 goes out and comes in apart").arg(name);
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
                failure = apart();
            }
        });

    // The path, then the metadata, the total after the number, then the
    // cover.
    bridge->setProperty("currentTrackPath", kTrackB1);
    bridge->setProperty("currentTrackTitle", QStringLiteral("Hollow Arc"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("The Quiet Meridian"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Small Hours"));
    bridge->setProperty("currentTrackYear", 2005);
    bridge->setProperty("currentTrackNumber", 8);
    bridge->setProperty("currentTrackTotal", 8);
    bridge->setProperty("currentTrackCoverPath", covers.all.at(1));
    QVERIFY(handover->property("running").toBool());
    QCOMPARE(handover->property("duration").toInt(), 600);
    for (const QString &name : names) {
        QVERIFY2(panelLine(panel, name).fades(), qPrintable(name));
    }
    QCOMPARE(panelLine(panel, narrow ? QStringLiteral("NarrowDetails") : QStringLiteral("Details")).text(),
             narrow ? QStringLiteral("Classical · Track 8 of 8") : QStringLiteral("2005 · Classical · Track 8 of 8"));

    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const double at : {0.0, 0.1, 0.2, 0.3, 0.39, 0.4, 0.41, 0.5, 0.7, 0.9}) {
        panel->setProperty("handover", at);
        const QString each = apart();
        QVERIFY2(each.isEmpty(), qPrintable(QStringLiteral("at %1: %2").arg(at).arg(each)));
    }
    panel->setProperty("handover", 0.0);
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(frames > 0);
    QVERIFY(panelIdle(panel, names));
}

// A line that changes cross-fades on one progress (#373 H6): over the fade,
// at points 1 % apart, the line before goes out as the playing track's
// comes in, their opacities adding up to one, so the line keeps at least
// 0.6 of its ink at every point; both show between its ends, and at them
// one text is whole and the other gone, exactly.
void UiMotionTest::nowPlayingLinesCrossFadeOnOneProgress() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QQuickItem *panel = named(window, "nowPlayingPanel");
    QObject *handover = qvariant_cast<QObject *>(panel->property("handoverAnimation"));
    showTrack(bridge, kTrackA1, QStringLiteral("First"), QStringLiteral("Album A"));
    QTRY_VERIFY(panelLine(panel, QStringLiteral("Title")).line()->width() > 0);
    showTrack(bridge, kTrackB1, QStringLiteral("Second"), QStringLiteral("Album B"));
    QVERIFY(handover->property("running").toBool());
    QVERIFY(QMetaObject::invokeMethod(handover, "pause"));
    for (const QString &name : {QStringLiteral("Title"), QStringLiteral("ArtistAlbum")}) {
        const PanelLine line = panelLine(panel, name);
        for (int step = 0; step <= 100; ++step) {
            panel->setProperty("handover", step / 100.0);
            const double out = line.outgoingOpacity();
            const double in = line.incomingOpacity();
            const QString where = QStringLiteral("%1 at %2: out %3, in %4").arg(name).arg(step).arg(out).arg(in);
            QVERIFY2(std::abs(out + in - 1.0) < 1e-9, qPrintable(where));
            QVERIFY2(1.0 - (1.0 - out) * (1.0 - in) >= 0.6, qPrintable(where));
            if (step == 0) {
                QVERIFY2(out == 1.0 && in == 0.0, qPrintable(where));
            } else if (step == 100) {
                QVERIFY2(out == 0.0 && in == 1.0, qPrintable(where));
            } else {
                QVERIFY2(out > 0.0 && in > 0.0, qPrintable(where));
            }
        }
    }
    QVERIFY(QMetaObject::invokeMethod(handover, "resume"));
    QVERIFY(QTest::qWaitFor([&]() { return !handover->property("running").toBool(); }, 5000));
    QVERIFY(allLinesStill(panel));
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
// whole. The chosen plot fades in over the one before, which stays whole
// under it, over shortDuration on OutCubic, as one picture, and the plot
// before goes. The spectrogram stays live under the waveform, at 0 and
// taking no input, so that switching back while playing comes in at the
// click with its whole view drawn (#373 H7: it waited about 250 ms for a
// fresh session's columns): the first frame after the click shows it, and
// no frame is darker than both plots. With animations off the chosen plot
// shows at once, with no blank frame.
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
    // The track's columns from `first` on, `count` of them, every lane's
    // in one chunk, as a session sends them: the first chunk of a session
    // resets it.
    constexpr int trackColumns = 2'000;
    const auto feedColumns = [&](int first, int count) {
        constexpr int bins = 4'097;
        const QList<SpectrogramItem *> lanes = panes();
        const QByteArray columns(bins * count * static_cast<int>(lanes.size()), '\x80');
        for (qsizetype index = 0; index < lanes.size(); ++index) {
            lanes.at(index)->feedPrecomputedChunk(columns, bins,
                                                  static_cast<int>(index), count, first, trackColumns,
                                                  48'000, 1'024, false, first == 0, 1, false);
        }
    };
    feedColumns(0, trackColumns);
    QTRY_VERIFY(!spectrogram->property("awaitingData").toBool());
    QVERIFY(!waveform->property("awaitingData").toBool());
    QTRY_VERIFY(spectrogram->isVisible() && !waveform->isVisible());
    QCOMPARE(spectrogram->opacity(), 1.0);

    // Whether a plot with data shows whole: visible, at full opacity.
    const auto whole = [](QQuickItem *plot) {
        return plot->isVisible() && plot->opacity() == 1.0 && !plot->property("awaitingData").toBool();
    };
    // The playheads drawn (H2: two through the fade): those of the plots
    // that show, at any opacity, and draw theirs. Every spectrogram lane
    // draws the same one.
    QQuickItem *waveformItem = itemNamed(waveform, QStringLiteral("waveformEditorItem"));
    QVERIFY(waveformItem != nullptr);
    const auto playheads = [&]() {
        int drawn = 0;
        if (spectrogram->isVisible() && spectrogram->opacity() > 0.0) {
            bool lanesDraw = false;
            for (SpectrogramItem *pane : panes()) {
                lanesDraw = lanesDraw || pane->playheadVisible();
            }
            drawn += lanesDraw ? 1 : 0;
        }
        if (waveform->isVisible() && waveform->opacity() > 0.0
            && waveformItem->property("playheadVisible").toBool()) {
            ++drawn;
        }
        return drawn;
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
            if (failure.isEmpty() && playheads() != 1) {
                failure = QStringLiteral("%1 playheads at frame %2").arg(playheads()).arg(frames);
            }
            // A spectrogram that comes in over the waveform shows its whole
            // view, never its black floor or part of its columns.
            if (failure.isEmpty() && waveform->isVisible() && spectrogram->isVisible()
                && spectrogram->opacity() > 0.0 && spectrogram->z() > waveform->z()
                && !spectrogram->property("viewFilled").toBool()) {
                failure = QStringLiteral("the spectrogram came in at %1 before it filled its view, at frame %2")
                              .arg(spectrogram->opacity()).arg(frames);
            }
        });
    const auto check = [&](const char *where) {
        QVERIFY2(whole(spectrogram) || whole(waveform), where);
        QCOMPARE(playheads(), 1);
        QVERIFY2(failure.isEmpty(), qPrintable(QString::fromLatin1(where) + QStringLiteral(": ") + failure));
    };

    // The time from the click to the first frame that showed the
    // spectrogram, in ms, or -1.
    QElapsedTimer sinceClick;
    qint64 firstShownMs = -1;
    const QMetaObject::Connection shownConnection =
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [&]() {
            if (sinceClick.isValid() && firstShownMs < 0 && spectrogram->isVisible()
                && spectrogram->opacity() > 0.0) {
                firstShownMs = sinceClick.elapsed();
            }
        });
    // Waits for `count` more frames.
    const auto waitFrames = [&](int count) {
        const int before = frames;
        window->update();
        return QTest::qWaitFor([&]() {
            if (frames < before + count) window->update();
            return frames >= before + count;
        }, 5000);
    };
    // The spectrogram live under the waveform: shown, so that its
    // analysis runs and each lane draws its view, at 0, and taking no
    // input; assistive technology skips it.
    const auto standing = [&]() {
        bool lanesShown = true;
        for (SpectrogramItem *pane : panes()) {
            lanesShown = lanesShown && pane->isVisible() && !pane->storageReleased();
        }
        return spectrogram->isVisible() && spectrogram->opacity() == 0.0 && !spectrogram->isEnabled()
            && spectrogram->property("standby").toBool() && lanesShown;
    };

    for (int round = 0; round < 2; ++round) {
        // To the waveform, which has its data: it comes in over the
        // spectrogram at once, or with animations off replaces it.
        QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
        QVERIFY(waveform->isVisible());
        QVERIFY(spectrogram->isVisible());
        check("to the waveform");
        if (animated) {
            QVERIFY(fade->property("running").toBool());
            QVERIFY(waveform->z() > spectrogram->z());
            if (round == 0) {
                QVERIFY(QMetaObject::invokeMethod(fade, "pause"));
                surface->setProperty("modeFade", 0.5);
                QCOMPARE(waveform->opacity(), 0.5);
                QCOMPARE(spectrogram->opacity(), 1.0);
                // One playhead: the incoming waveform's; the spectrogram's
                // went as the fade started.
                QVERIFY(waveformItem->property("playheadVisible").toBool());
                for (SpectrogramItem *pane : panes()) {
                    QVERIFY(!pane->playheadVisible());
                }
                // Its floor and its lanes as one picture.
                QObject *layer = waveform->property("layer").value<QObject *>();
                QVERIFY(layer != nullptr && layer->property("enabled").toBool());
                QVERIFY(QMetaObject::invokeMethod(fade, "resume"));
            }
            QTRY_VERIFY(!fade->property("running").toBool());
        } else {
            QVERIFY(!fade->property("running").toBool());
        }
        QVERIFY(standing());
        QCOMPARE(waveform->opacity(), 1.0);
        check("on the waveform");

        // Playing on, the spectrogram goes on drawing its view under the
        // waveform: its view is whole when it is chosen.
        QVERIFY(waitFrames(3));
        QTRY_VERIFY(spectrogram->property("viewFilled").toBool());
        QVERIFY(standing());
        check("the spectrogram under the waveform");

        // Back to the spectrogram: it comes in at the click, whole, with
        // nothing to wait for: its view is drawn, and the fade starts with
        // the click.
        QVERIFY(spectrogram->property("viewFilled").toBool());
        firstShownMs = -1;
        sinceClick.start();
        QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 0)));
        QVERIFY(spectrogram->isEnabled());
        if (animated) {
            QVERIFY(fade->property("running").toBool());
            QVERIFY(spectrogram->z() > waveform->z());
        } else {
            QVERIFY(!fade->property("running").toBool());
            QVERIFY(!waveform->isVisible());
            QCOMPARE(spectrogram->opacity(), 1.0);
        }
        window->update();
        QVERIFY(QTest::qWaitFor([&]() { return firstShownMs >= 0; }, 2000));
        // Frames, not a session's columns, away (H7: 251 ms): the bound
        // leaves room for a loaded test machine's software rendering.
        QVERIFY2(firstShownMs < 125, qPrintable(QStringLiteral("shown %1 ms after the click").arg(firstShownMs)));
        sinceClick.invalidate();
        check("the spectrogram's view");
        if (animated) {
            if (round == 0) {
                QVERIFY(QMetaObject::invokeMethod(fade, "pause"));
                surface->setProperty("modeFade", 0.5);
                QCOMPARE(spectrogram->opacity(), 0.5);
                QVERIFY(whole(waveform));
                QVERIFY(!waveformItem->property("playheadVisible").toBool());
                for (SpectrogramItem *pane : panes()) {
                    QVERIFY(pane->playheadVisible());
                }
                QVERIFY(QMetaObject::invokeMethod(fade, "resume"));
            }
            QTRY_VERIFY(!waveform->isVisible());
        }
        QCOMPARE(spectrogram->opacity(), 1.0);
        QVERIFY(!spectrogram->property("standby").toBool());
        check("on the spectrogram");
        // Settled, the plot under it draws its playhead again, for the next
        // switch.
        QVERIFY(waveformItem->property("playheadVisible").toBool());
    }

    QObject::disconnect(connection);
    QObject::disconnect(shownConnection);
    QVERIFY(frames > 0);

    // Cold, with no columns yet, as when the pane comes back with the
    // window and its analysis starts afresh: the spectrogram still comes
    // in at the click, with what it has, and its columns come in as they
    // would were it shown.
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
    QTRY_VERIFY(!fade->property("running").toBool());
    for (SpectrogramItem *pane : panes()) {
        pane->releaseStorage();
    }
    QTRY_VERIFY(spectrogram->property("awaitingData").toBool());
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 0)));
    if (animated) {
        QVERIFY(fade->property("running").toBool());
    }
    QTRY_VERIFY(!waveform->isVisible());
    QCOMPARE(spectrogram->opacity(), 1.0);
    for (SpectrogramItem *pane : panes()) {
        pane->retainStorage();
    }
    feedColumns(0, trackColumns);
    QTRY_VERIFY(!spectrogram->property("awaitingData").toBool());

    // Stopped, nothing is waited for: the waveform, with its overview,
    // shows at once.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, 1)));
    QVERIFY(waveform->isVisible());
    if (animated) {
        QVERIFY(fade->property("running").toBool());
    }
    QTRY_COMPARE(waveform->opacity(), 1.0);
    QTRY_VERIFY(spectrogram->property("standby").toBool());
    QCOMPARE(spectrogram->opacity(), 0.0);
}

void UiMotionTest::visualizationPaneFloorFollowsTheMode_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("animated");
    QTest::newRow("light") << false << true;
    QTest::newRow("light, animations off") << false << false;
    QTest::newRow("dark") << true << true;
    QTest::newRow("dark, animations off") << true << false;
}

// The empty pane's floor follows the mode (#373 H4, P5; H2's P1 made the
// light theme's bare pane the view colour): before any audio and while the
// first track is analysed, the spectrogram's pane is its colour map's
// floor, black, as silence draws it, and the waveform's the view colour,
// in both themes. Switching modes on the empty pane switches the floor with
// the mode's cross-fade, and the first data never changes it. What is drawn
// on it, the mode switch, takes the mode's plot colours.
void UiMotionTest::visualizationPaneFloorFollowsTheMode() {
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
    QQuickItem *surface = named(window, "visualizationSurface");
    QQuickItem *spectrogram = named(window, "mainSpectrogramSurface");
    QQuickItem *waveform = named(window, "mainWaveformSurface");
    QVERIFY(pane && layer && surface && spectrogram && waveform);
    // H2's view-colour layer is gone.
    QVERIFY(itemNamed(window->contentItem(), QStringLiteral("plotViewFloor")) == nullptr);
    QObject *fade = qvariant_cast<QObject *>(surface->property("modeFadeAnimation"));
    QVERIFY(fade != nullptr);
    auto *spectrogramSet = qvariant_cast<QObject *>(tokens->property("spectrogramPlot"));
    auto *waveformSet = qvariant_cast<QObject *>(tokens->property("waveformPlot"));
    const QColor black(QStringLiteral("#000000"));
    QCOMPARE(spectrogramSet->property("plotBg").value<QColor>(), black);
    QCOMPARE(waveformSet->property("plotBg").value<QColor>(), view);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QVERIFY(viewer != nullptr);
    QCOMPARE(viewer->property("visualizationMode").toInt(), 0);
    QQuickItem *modeSwitch = itemNamed(layer, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(modeSwitch != nullptr);

    // Clear of the mode switch in the top right corner.
    const auto drawnInPane = [&]() {
        const QImage image = window->grabWindow();
        const qreal scale = image.width() / static_cast<qreal>(window->width());
        const QPointF at = pane->mapToScene(QPointF(pane->width() * 0.25, pane->height() * 0.6)) * scale;
        return image.pixelColor(at.toPoint());
    };
    // The mode's floor, as the pane says it, as the layer's colours say it,
    // and as drawn.
    const auto expectFloor = [&](int mode, const char *when) -> bool {
        const QColor wanted = mode == 1 ? view : black;
        QObject *set = mode == 1 ? waveformSet : spectrogramSet;
        if (pane->property("color").value<QColor>() != wanted) {
            qWarning() << when << "pane" << pane->property("color").value<QColor>() << "wanted" << wanted;
            return false;
        }
        if (layer->property("plotColors").value<QObject *>() != set
            || modeSwitch->property("plotColors").value<QObject *>() != set) {
            qWarning() << when << "the layer is not in the mode's plot colours";
            return false;
        }
        const QColor drawn = drawnInPane();
        if (drawn != wanted) {
            qWarning() << when << "drawn" << drawn << "wanted" << wanted;
            return false;
        }
        return true;
    };
    // Switches to `mode` on an empty pane: with animations, the chosen plot
    // comes in over the other, its floor over the other's; then it settles.
    const auto switchTo = [&](int mode) {
        QVERIFY(QMetaObject::invokeMethod(viewer, "setVisualizationMode", Q_ARG(QVariant, mode)));
        if (animated) {
            QVERIFY(fade->property("running").toBool());
            QVERIFY(QMetaObject::invokeMethod(fade, "pause"));
            surface->setProperty("modeFade", 0.5);
            // Halfway between the two floors.
            const QColor drawn = drawnInPane();
            const QColor from = mode == 1 ? black : view;
            const QColor to = mode == 1 ? view : black;
            const auto between = [](int value, int a, int b) {
                return value > std::min(a, b) && value < std::max(a, b);
            };
            QVERIFY2(between(drawn.red(), from.red(), to.red()) && between(drawn.blue(), from.blue(), to.blue()),
                     qPrintable(drawn.name()));
            QVERIFY(QMetaObject::invokeMethod(fade, "resume"));
        } else {
            QVERIFY(!fade->property("running").toBool());
        }
        QTRY_VERIFY(!surface->property("modeSwitching").toBool());
        if (mode == 1) {
            // The spectrogram stays live under the waveform, at 0.
            QTRY_COMPARE(spectrogram->opacity(), 0.0);
            QVERIFY(!spectrogram->isEnabled());
        } else {
            QTRY_VERIFY(!waveform->isVisible());
        }
    };

    // Before any audio: the spectrogram's floor, with the mode switch on it.
    QTest::qWait(50);
    QVERIFY(modeSwitch->isVisible());
    QTRY_VERIFY(expectFloor(0, "before audio"));

    // The waveform, on the empty pane: its floor, the view colour.
    switchTo(1);
    QTRY_VERIFY(expectFloor(1, "before audio, waveform"));
    // And back.
    switchTo(0);
    QTRY_VERIFY(expectFloor(0, "before audio, back to the spectrogram"));

    // A track, while it is analysed: the floor stays the mode's.
    bridge->setProperty("durationSeconds", 60.0);
    bridge->setProperty("currentTrackChannels", 2);
    playQueueRow(bridge, 0);
    QTest::qWait(50);
    QVERIFY(spectrogram->property("awaitingData").toBool());
    QVERIFY(expectFloor(0, "analysing"));

    // The first data: the spectrogram's columns, at the floor's level. The
    // floor is the same, at once and after.
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
        lanes.at(index)->feedPrecomputedChunk(QByteArray(bins * columns, '\0'), bins, static_cast<int>(index),
                                              columns, 0, 2'000, 48'000, 1'024, false, true, 1, false);
    }
    QTRY_VERIFY(!spectrogram->property("awaitingData").toBool());
    QCOMPARE(pane->property("color").value<QColor>(), black);
    QCOMPARE(layer->property("plotColors").value<QObject *>(), spectrogramSet);
    QTest::qWait(animated ? 450 : 50);
    QCOMPARE(pane->property("color").value<QColor>(), black);

    // Stopped, with the track's data gone: the mode's floor still, in each
    // mode.
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    bridge->setProperty("currentTrackPath", QString());
    for (SpectrogramItem *lane : lanes) {
        lane->releaseStorage();
    }
    QTRY_VERIFY(spectrogram->property("awaitingData").toBool());
    QTRY_VERIFY(expectFloor(0, "stopped"));
    switchTo(1);
    QTRY_VERIFY(expectFloor(1, "stopped, waveform"));
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
// for the key that wakes them and hidden only once faded out. The plots'
// crosshair and its readouts fade with them too (#373 H3: it went at once).
// All of them take the auto-hide's one level as their opacity, and the
// pointer hides once it is 0, not as the fade starts (#373 H4).
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
    bridge->setProperty("showSpectrogramCrosshair", true);
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
    // The crosshair, which each plot draws: the spectrogram's lanes, and
    // the waveform beside them.
    QList<QQuickItem *> crosshairs = itemsNamed(spectrogram, QStringLiteral("spectrogramPaneItem"));
    QVERIFY(!crosshairs.isEmpty());
    QQuickItem *waveform = itemNamed(spectrogram->parentItem(), QStringLiteral("waveformEditorItem"));
    QVERIFY(waveform != nullptr);
    crosshairs.append(waveform);
    for (QQuickItem *crosshair : crosshairs) {
        QCOMPARE(crosshair->property("crosshairOpacity").toDouble(), 1.0);
        QVERIFY(crosshair->property("crosshairEnabled").toBool());
    }

    // The pointer over each plot.
    QObject *waveformHover = waveform->findChild<QObject *>(QStringLiteral("waveformSurfaceHoverHandler"));
    QVERIFY(waveformHover != nullptr);

    QString failure;
    int frames = 0;
    int fadingFrames = 0;
    // Where a follower is not at the controls' opacity, or "".
    const auto apart = [&]() -> QString {
        const double level = autoHide->property("level").toDouble();
        if (std::abs(controls->opacity() - level) > 1e-9) {
            return QStringLiteral("the controls at %1, the level at %2").arg(controls->opacity()).arg(level);
        }
        // The pointer shows while anything does.
        const int shape = controls->isVisible() ? Qt::ArrowCursor : Qt::BlankCursor;
        if (spectrogram->property("pointerCursorShape").toInt() != shape
            || waveformHover->property("cursorShape").toInt() != shape) {
            return QStringLiteral("the pointer %1 / %2, the controls at %3 (%4)")
                .arg(spectrogram->property("pointerCursorShape").toInt())
                .arg(waveformHover->property("cursorShape").toInt())
                .arg(controls->opacity()).arg(controls->isVisible());
        }
        for (QQuickItem *follower : followers) {
            if (std::abs(follower->opacity() - controls->opacity()) > 1e-9
                || follower->isVisible() != controls->isVisible()) {
                return QStringLiteral("%1 at %2 (%3), the controls at %4 (%5)")
                    .arg(follower->objectName()).arg(follower->opacity()).arg(follower->isVisible())
                    .arg(controls->opacity()).arg(controls->isVisible());
            }
        }
        for (QQuickItem *crosshair : crosshairs) {
            const double opacity = crosshair->property("crosshairOpacity").toDouble();
            const bool enabled = crosshair->property("crosshairEnabled").toBool();
            if (std::abs(opacity - controls->opacity()) > 1e-9 || enabled != controls->isVisible()) {
                return QStringLiteral("the crosshair of %1 at %2 (%3), the controls at %4 (%5)")
                    .arg(crosshair->objectName()).arg(opacity).arg(enabled)
                    .arg(controls->opacity()).arg(controls->isVisible());
            }
        }
        return {};
    };
    const QMetaObject::Connection connection = QObject::connect(host, &QQuickWindow::frameSwapped, host, [&]() {
        ++frames;
        const double level = autoHide->property("level").toDouble();
        if (level > 0.0 && level < 1.0) {
            ++fadingFrames;
        }
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
    // The pointer stays while they fade.
    QCOMPARE(spectrogram->property("pointerCursorShape").toInt(), int(Qt::ArrowCursor));
    QTRY_VERIFY(!controls->isVisible());
    QCOMPARE(autoHide->property("level").toDouble(), 0.0);
    QCOMPARE(spectrogram->property("pointerCursorShape").toInt(), int(Qt::BlankCursor));
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    // Frames along the fade, not only its ends.
    QVERIFY2(fadingFrames >= 3, qPrintable(QString::number(fadingFrames)));

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

void UiMotionTest::viewerPlatesAndGlyphsRevealOnOneLevel_data() {
    QTest::addColumn<bool>("wholeScreen");
    QTest::newRow("whole screen") << true;
    QTest::newRow("in the window") << false;
}

namespace {

// How opaque `item` is drawn: its opacity and that of every item it is in,
// or 0 while any of them is hidden.
double drawnOpacity(const QQuickItem *item) {
    double opacity = 1.0;
    for (; item != nullptr; item = item->parentItem()) {
        if (!item->isVisible()) {
            return 0.0;
        }
        opacity *= item->opacity();
    }
    return opacity;
}

} // namespace

// The viewer's controls come back as one (#373 H5: the plates' fill came in
// about 130 ms before the lanes' M and S): with the pointer moving on a
// lane, the title strip or the corner plate, the mode switch and the lane's
// Mute and Solo, each plate's fill and each button's glyph, are at the
// auto-hide's one level in every frame, from hidden to shown. The lanes'
// buttons show on the pointer (the default), not always.
void UiMotionTest::viewerPlatesAndGlyphsRevealOnOneLevel() {
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
    bridge->setProperty("viewerFullscreenMode", wholeScreen ? 1 : 0);
    bridge->setProperty("spectrogramViewMode", 1);
    bridge->setProperty("channelButtonsVisibility", 1);
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
    }
    QQuickItem *controls = nullptr;
    QTRY_VERIFY((controls = itemNamed(host->contentItem(), wholeScreen ? QStringLiteral("spectrogramFullscreenStrip")
                                                                         : QStringLiteral("spectrogramPopupControls")))
                != nullptr);
    QTRY_VERIFY(spectrogram->window() == host);
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    if (!wholeScreen) {
        QTRY_VERIFY(popup->property("opened").toBool());
    }
    QQuickItem *modeSwitch = itemNamed(host->contentItem(), QStringLiteral("visualizationModeSwitch"));
    QVERIFY(modeSwitch != nullptr);
    QTRY_COMPARE(itemsNamed(host->contentItem(), QStringLiteral("plotLaneButtons")).size(), 2);
    QQuickItem *laneButtons = itemsNamed(host->contentItem(), QStringLiteral("plotLaneButtons")).first();
    // Each plate's fill and a glyph on it.
    QList<QPair<QString, QQuickItem *>> drawn{
        {QStringLiteral("the mode switch's plate"), itemNamed(modeSwitch, QStringLiteral("plotPlateFill"))},
        {QStringLiteral("the mode switch's icon"), itemNamed(modeSwitch, QStringLiteral("plotToolButtonIcon"))},
        {QStringLiteral("the lane's plate"), itemNamed(laneButtons, QStringLiteral("plotPlateFill"))},
        {QStringLiteral("the lane's M"), itemNamed(laneButtons, QStringLiteral("plotToolButtonGlyph"))},
        {QStringLiteral("the viewer's controls"), controls},
        {QStringLiteral("the viewer's Close"),
         itemNamed(itemNamed(controls, wholeScreen ? QStringLiteral("spectrogramFullscreenCloseButton")
                                                   : QStringLiteral("spectrogramPopupCloseButton")),
                   wholeScreen ? QStringLiteral("viewerToolButtonIcon") : QStringLiteral("plotToolButtonIcon"))},
    };
    if (!wholeScreen) {
        drawn.append({QStringLiteral("the corner plate"), itemNamed(controls, QStringLiteral("plotPlateFill"))});
    }
    for (const auto &[what, item] : drawn) {
        QVERIFY2(item != nullptr, qPrintable(what));
    }
    // The pointer resting on the first lane, away from every control.
    const QPoint onLane(host->width() / 2, host->height() / 4);
    QTest::mouseMove(host, onLane);
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QTRY_COMPARE(laneButtons->opacity(), 1.0);

    QString failure;
    int fadingFrames = 0;
    double popupOpacity = 1.0;
    // Where a plate or a glyph is not at the level, or "".
    const auto apart = [&]() -> QString {
        const double level = autoHide->property("level").toDouble() * popupOpacity;
        for (const auto &[what, item] : drawn) {
            const double opacity = drawnOpacity(item);
            if (std::abs(opacity - level) > 1e-9) {
                return QStringLiteral("%1 at %2, the level at %3").arg(what).arg(opacity).arg(level);
            }
        }
        return {};
    };
    if (!wholeScreen) {
        popupOpacity = drawnOpacity(qobject_cast<QQuickItem *>(controls->parentItem()));
    }
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));

    // Hidden, with the pointer resting.
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!controls->isVisible());
    QTRY_COMPARE(autoHide->property("level").toDouble(), 0.0);
    autoHide->setProperty("hideDelay", 600000);

    const QMetaObject::Connection connection = QObject::connect(host, &QQuickWindow::frameSwapped, host, [&]() {
        const double level = autoHide->property("level").toDouble();
        if (level > 0.0 && level < 1.0) {
            ++fadingFrames;
        }
        if (failure.isEmpty()) {
            failure = apart();
        }
    });
    // The pointer moving on the lane wakes them; then, hidden again, the
    // pointer comes back onto the lane from outside the view. In a viewer
    // the plates that show on the pointer show with its controls (#373 H6:
    // the mode switch, then the lane's buttons, came in on their own curves
    // after the strip), so the pointer leaving the view leaves them as they
    // are.
    for (const bool fromOutside : {false, true}) {
        if (fromOutside) {
            autoHide->setProperty("hideDelay", 1);
            QTRY_VERIFY(!controls->isVisible());
            QTest::mouseMove(host, QPoint(-20, -20));
            QTest::qWait(50);
            QCOMPARE(modeSwitch->property("revealLevel").toDouble(), 1.0);
            QCOMPARE(laneButtons->property("revealLevel").toDouble(), 1.0);
            autoHide->setProperty("hideDelay", 600000);
            fadingFrames = 0;
        }
        const QByteArray where = fromOutside ? "from outside" : "on the lane";
        QTest::mouseMove(host, onLane + QPoint(fromOutside ? 0 : 3, 0));
        QTRY_VERIFY2(autoHide->property("controlsVisible").toBool(), where.constData());
        QVERIFY2(apart().isEmpty(), qPrintable(where + ": " + apart()));
        QTRY_VERIFY(autoHide->property("level").toDouble() > 0.0 && autoHide->property("level").toDouble() < 1.0);
        QVERIFY2(apart().isEmpty(), qPrintable(where + ": " + apart()));
        QTRY_COMPARE(autoHide->property("level").toDouble(), 1.0);
        QVERIFY2(apart().isEmpty(), qPrintable(where + ": " + apart()));
        QVERIFY2(failure.isEmpty(), qPrintable(where + ": " + failure));
        QVERIFY2(fadingFrames >= 3, qPrintable(where + ": " + QString::number(fadingFrames)));
    }
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY2(fadingFrames >= 3, qPrintable(QString::number(fadingFrames)));

    // The controls shown, the pointer comes onto the lane from outside the
    // view, as when it reaches a viewer that has just opened: nothing comes
    // in on a curve of its own after them (#373 H6, 73 ms after the strip).
    QTest::mouseMove(host, QPoint(-20, -20));
    QVERIFY(QMetaObject::invokeMethod(autoHide, "wake"));
    QTRY_COMPARE(autoHide->property("level").toDouble(), 1.0);
    QVERIFY2(apart().isEmpty(), qPrintable(QStringLiteral("shown, the pointer outside: ") + apart()));
    // Sampled along what would be a fade, as no frame need be drawn.
    QTest::mouseMove(host, onLane);
    for (int sample = 0; sample < 20; ++sample) {
        QTest::qWait(units->shortDuration() / 10);
        QVERIFY2(apart().isEmpty(), qPrintable(QStringLiteral("the pointer arrived, sample %1: %2")
                                                   .arg(sample).arg(apart())));
    }
    QCOMPARE(modeSwitch->property("revealLevel").toDouble(), 1.0);
    QCOMPARE(laneButtons->property("revealLevel").toDouble(), 1.0);
    QVERIFY2(failure.isEmpty(), qPrintable(QStringLiteral("the pointer arrived: ") + failure));
    QObject::disconnect(connection);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

// The viewer in the window opens over the pane's last frame (#373 H5: the
// pane went blank for about 46 ms, as the visualization left it at once and
// the viewer faded in over the empty pane). In every frame from the open,
// the visualization is either still in the pane or the pane has kept its
// last frame; under the open viewer, the pane shows what it showed before.
// Closed, the visualization is back and the kept frame goes. The pane makes
// the frame only while it keeps one.
void UiMotionTest::windowedViewerOpensOverThePanesLastFrame() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QQuickItem *pane = named(window, "spectrogramPane");
    QQuickItem *surface = named(window, "visualizationSurface");
    const auto lastFrame = [window]() { return named(window, "spectrogramPaneLastFrame"); };
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QVERIFY(bridge && viewer && pane && surface && popup);
    bridge->setProperty("viewerFullscreenMode", 0);
    // Before any audio, the pane shows its mode switch on its floor.
    QQuickItem *modeSwitch = itemNamed(surface, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(modeSwitch != nullptr);
    QTRY_COMPARE(modeSwitch->opacity(), 1.0);
    QVERIFY(pane->isAncestorOf(surface));
    QVERIFY(lastFrame() == nullptr);

    // The window's layout settles first: the pane keeps its place for a
    // while.
    const auto paneSceneRect = [pane]() { return pane->mapRectToScene(QRectF(0, 0, pane->width(), pane->height())); };
    QRectF settled = paneSceneRect();
    QElapsedTimer still;
    still.start();
    QVERIFY(QTest::qWaitFor([&]() {
        if (paneSceneRect() != settled) {
            settled = paneSceneRect();
            still.restart();
        }
        return still.elapsed() >= 300;
    }, 5000));
    const QRectF paneRect = pane->mapRectToScene(QRectF(0, 0, pane->width(), pane->height()));
    const auto paneImage = [&]() {
        const QImage image = window->grabWindow();
        const double scale = double(image.width()) / window->width();
        return image.copy(QRectF(paneRect.topLeft() * scale, paneRect.size() * scale).toAlignedRect())
            .convertToFormat(QImage::Format_RGB32);
    };
    const QImage before = paneImage();
    QVERIFY(!before.isNull());
    const QColor floor = pane->property("color").value<QColor>();
    // The pane shows something besides its floor, which a blank pane would
    // not.
    int drawn = 0;
    for (int y = 0; y < before.height(); ++y) {
        for (int x = 0; x < before.width(); ++x) {
            const QColor at = before.pixelColor(x, y);
            drawn += std::abs(at.red() - floor.red()) + std::abs(at.green() - floor.green())
                    + std::abs(at.blue() - floor.blue()) > 24;
        }
    }
    QVERIFY2(drawn > 100, qPrintable(QString::number(drawn)));

    QString failure;
    int frames = 0;
    const QMetaObject::Connection connection = QObject::connect(window, &QQuickWindow::frameSwapped, window, [&]() {
        ++frames;
        if (failure.isEmpty() && !pane->isAncestorOf(surface)
            && !(lastFrame() != nullptr && lastFrame()->isVisible() && pane->property("lastFrameKept").toBool())) {
            failure = QStringLiteral("frame %1: the visualization gone from the pane, its frame not kept").arg(frames);
        }
    });
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    // The pane keeps the frame first; the visualization goes after.
    QVERIFY(pane->isAncestorOf(surface));
    QTRY_VERIFY(!pane->isAncestorOf(surface));
    QVERIFY(pane->property("lastFrameKept").toBool());
    QTRY_VERIFY(popup->property("opened").toBool());
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(frames > 0);

    // Under the viewer, the pane shows what it showed before.
    QCOMPARE(paneSceneRect(), paneRect);
    popup->setProperty("opacity", 0.0);
    const QImage under = paneImage();
    popup->setProperty("opacity", 1.0);
    QCOMPARE(under.size(), before.size());
    int differing = 0;
    for (int y = 0; y < under.height(); ++y) {
        for (int x = 0; x < under.width(); ++x) {
            const QColor a = under.pixelColor(x, y);
            const QColor b = before.pixelColor(x, y);
            differing += std::abs(a.red() - b.red()) > 3 || std::abs(a.green() - b.green()) > 3
                         || std::abs(a.blue() - b.blue()) > 3;
        }
    }
    QVERIFY2(differing <= under.width() * under.height() / 1000,
             qPrintable(QStringLiteral("%1 of %2 pixels differ").arg(differing).arg(under.width() * under.height())));

    // Closed: the pane's kept frame goes, and the visualization is back
    // once the closing viewer keeps a frame of its own.
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    QVERIFY(lastFrame() == nullptr);
    QTRY_VERIFY(pane->isAncestorOf(surface));
    QVERIFY(!pane->property("lastFrameKept").toBool());
    QVERIFY(!pane->property("holdsVisualization").toBool());

    // Opened again, it keeps a frame again.
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(!pane->isAncestorOf(surface));
    QVERIFY(lastFrame() != nullptr && lastFrame()->isVisible());
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
}

// The viewer takes the visualization whole into its window, whichever
// window it opened in before and when the setting changes while it is open
// (#373 H6: the pane's kept frame, a ShaderEffectSource of the
// visualization, kept it in the main window's scene when the whole-screen
// window took it; the two windows' render threads drew its nodes at once,
// and the app crashed. The setting comes from the backend after it is
// set, so a viewer opened just after it is set opens in the window first).
// Wherever the viewer shows the visualization, every item of it is in that
// window; the pane keeps no frame of it, and has no frame, unless it is in
// the main window.
void UiMotionTest::viewerTakesTheVisualizationWholeIntoEitherWindow_data() {
    QTest::addColumn<bool>("wholeScreenFirst");
    QTest::addColumn<bool>("whileOpen");
    QTest::newRow("window, then whole screen") << false << false;
    QTest::newRow("whole screen, then window") << true << false;
    QTest::newRow("window, whole screen while open") << false << true;
    QTest::newRow("whole screen, window while open") << true << true;
}

void UiMotionTest::viewerTakesTheVisualizationWholeIntoEitherWindow() {
    QFETCH(bool, wholeScreenFirst);
    QFETCH(bool, whileOpen);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QQuickItem *pane = named(window, "spectrogramPane");
    QQuickItem *surface = named(window, "visualizationSurface");
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QQuickItem *spectrogram = named(window, "mainSpectrogramSurface");
    QVERIFY(bridge && viewer && pane && surface && popup && spectrogram);
    // A track the spectrogram shows the columns of.
    bridge->setProperty("durationSeconds", 60.0);
    bridge->setProperty("currentTrackChannels", 2);
    bridge->setProperty("waveformCoverageSeconds", 60.0);
    bridge->setProperty("waveformComplete", true);
    playQueueRow(bridge, 0);
    QVERIFY(QMetaObject::invokeMethod(bridge, "precomputedSpectrogramChannelsReady", Q_ARG(int, 2),
                                      Q_ARG(bool, true)));
    QList<QQuickItem *> lanes;
    QTRY_COMPARE((lanes = itemsNamed(spectrogram, QStringLiteral("spectrogramPaneItem"))).size(), 2);
    for (qsizetype index = 0; index < lanes.size(); ++index) {
        constexpr int bins = 4'097;
        constexpr int columns = 200;
        qobject_cast<SpectrogramItem *>(lanes.at(index))
            ->feedPrecomputedChunk(QByteArray(bins * columns, '\x80'), bins, static_cast<int>(index), columns, 0,
                                   2'000, 48'000, 1'024, false, true, 1, false);
    }
    QTRY_VERIFY(!spectrogram->property("awaitingData").toBool());

    // Whether the visualization is whole in `host`: every item of it there,
    // and, in another window, no frame of the pane's.
    const auto whole = [&](QQuickWindow *host) -> QString {
        QString failure;
        const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
            if (failure.isEmpty() && item->window() != host) {
                failure = QStringLiteral("%1 (%2) in another window")
                              .arg(item->objectName(), QString::fromLatin1(item->metaObject()->className()));
            }
            for (QQuickItem *child : item->childItems()) {
                visit(child);
            }
        };
        visit(surface);
        if (failure.isEmpty() && host != window
            && (pane->property("holdsVisualization").toBool()
                || named(window, "spectrogramPaneLastFrame") != nullptr)) {
            failure = QStringLiteral("the pane keeps a frame of it");
        }
        return failure;
    };
    // The visualization is in the viewer, whole, for a few frames.
    const auto shownWhole = [&](bool wholeScreen, const QString &where) -> QString {
        if (!QTest::qWaitFor([&]() { return !pane->isAncestorOf(surface); })) {
            return where + QStringLiteral(": still in the pane");
        }
        QQuickWindow *host = window;
        if (wholeScreen) {
            if (!QTest::qWaitFor([&]() {
                    return surface->parentItem() != nullptr && surface->parentItem()->window() != nullptr
                           && surface->parentItem()->window() != window;
                })) {
                return where + QStringLiteral(": not in the whole-screen window");
            }
            host = surface->parentItem()->window();
            if (!QTest::qWaitForWindowExposed(host)) {
                return where + QStringLiteral(": the whole-screen window not shown");
            }
        } else if (!QTest::qWaitFor([&]() { return popup->property("opened").toBool(); })) {
            return where + QStringLiteral(": the viewer not open");
        }
        for (int frame = 0; frame < 3; ++frame) {
            QSignalSpy swapped(host, &QQuickWindow::frameSwapped);
            host->update();
            if (!swapped.wait(2000)) {
                return where + QStringLiteral(": no frame");
            }
        }
        const QString apart = whole(host);
        return apart.isEmpty() ? QString() : where + QStringLiteral(": ") + apart;
    };

    for (int round = 0; round < 2; ++round) {
        for (const bool wholeScreen : {wholeScreenFirst, !wholeScreenFirst}) {
            const QString where = QStringLiteral("round %1, %2").arg(round).arg(
                wholeScreen ? QStringLiteral("whole screen") : QStringLiteral("window"));
            bridge->setProperty("viewerFullscreenMode", (whileOpen ? !wholeScreen : wholeScreen) ? 1 : 0);
            QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
            QTRY_VERIFY(viewer->property("spectrogramViewerOpen").toBool());
            if (whileOpen) {
                // Open in the other mode, the setting then changes: once
                // the viewer shows, and then at once, while the pane may
                // still be keeping its frame.
                if (round == 0) {
                    const QString before = shownWhole(!wholeScreen, where + QStringLiteral(", before the change"));
                    QVERIFY2(before.isEmpty(), qPrintable(before));
                }
                bridge->setProperty("viewerFullscreenMode", wholeScreen ? 1 : 0);
            }
            const QString shown = shownWhole(wholeScreen, where);
            QVERIFY2(shown.isEmpty(), qPrintable(shown));

            QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
            QTRY_VERIFY(!viewer->property("spectrogramViewerOpen").toBool());
            QTRY_VERIFY2(pane->isAncestorOf(surface), qPrintable(where));
            QSignalSpy swapped(window, &QQuickWindow::frameSwapped);
            window->update();
            QVERIFY2(swapped.wait(2000), qPrintable(where));
            const QString back = whole(window);
            QVERIFY2(back.isEmpty(), qPrintable(where + QStringLiteral(", closed: ") + back));
            QVERIFY2(named(window, "spectrogramPaneLastFrame") == nullptr, qPrintable(where));
            QVERIFY(!pane->property("holdsVisualization").toBool());
        }
    }
}


namespace {

// The item a Popup draws itself and its content in.
QQuickItem *popupItemOf(QObject *popup) {
    auto *content = qvariant_cast<QQuickItem *>(popup->property("contentItem"));
    return content != nullptr ? content->parentItem() : nullptr;
}

} // namespace

void UiMotionTest::albumArtViewerFadesWithItsArt_data() {
    QTest::addColumn<bool>("opening");
    QTest::newRow("opening") << true;
    QTest::newRow("closing") << false;
}

// The album art viewer in the window fades in and out with its art and its
// details (#373 H8: as it closed, the art went at once and the viewer's
// black floor faded out alone, a blink of about 100 ms). In every frame the
// viewer draws, the art and the details are drawn at the viewer's own
// opacity, the art loaded: none of them shows before the viewer or goes
// before it.
void UiMotionTest::albumArtViewerFadesWithItsArt() {
    QFETCH(bool, opening);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    // Long enough for frames along the fade.
    units->setLongDuration(400);
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
    QQuickItem *surface = named(window, "albumArtSurface");
    QQuickItem *preload = named(window, "albumArtViewerPreloadImage");
    QVERIFY(bridge && viewer && popup && surface && preload);
    QQuickItem *mainHost = surface->parentItem();
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *title = itemNamed(surface, QStringLiteral("albumArtInfoTitle"));
    QVERIFY(mainHost && image && column && title);
    bridge->setProperty("viewerFullscreenMode", 0);
    showTrack(bridge, QStringLiteral("/music/0.flac"), QStringLiteral("Track"), QStringLiteral("Album"),
              covers.first);
    // The window holds the cover decoded as the viewer shows it.
    QTRY_COMPARE(preload->property("status").toInt(), 1);
    QVERIFY(!surface->isVisible());

    QQuickItem *popupItem = popupItemOf(popup);
    QVERIFY(popupItem != nullptr);
    QString failure;
    int frames = 0;
    int fadingFrames = 0;
    // What is not drawn at the viewer's opacity, or "".
    const auto apart = [&](bool details) -> QString {
        if (popupItem == nullptr || !popupItem->isVisible()) {
            return {};
        }
        const double shown = drawnOpacity(popupItem);
        if (!popupItem->isAncestorOf(surface)) {
            return QStringLiteral("the art out of the viewer, at %1").arg(shown);
        }
        if (shown > 0.0 && image->property("status").toInt() != 1) {
            return QStringLiteral("the art not loaded, the viewer at %1").arg(shown);
        }
        QList<QPair<QString, QQuickItem *>> drawn{{QStringLiteral("the art"), image}};
        if (details) {
            drawn.append({QStringLiteral("the details"), column});
            drawn.append({QStringLiteral("the file's name"), title});
        }
        for (const auto &[what, item] : drawn) {
            const double opacity = drawnOpacity(item);
            if (std::abs(opacity - shown) > 1e-9) {
                return QStringLiteral("%1 at %2, the viewer at %3").arg(what).arg(opacity).arg(shown);
            }
        }
        return {};
    };

    const auto sampleFade = [&](bool details) {
        const QMetaObject::Connection connection =
            QObject::connect(window, &QQuickWindow::frameSwapped, window, [&]() {
                ++frames;
                if (popupItem != nullptr && popupItem->isVisible()) {
                    const double shown = drawnOpacity(popupItem);
                    fadingFrames += shown > 0.0 && shown < 1.0;
                }
                if (failure.isEmpty()) {
                    failure = apart(details);
                }
            });
        return connection;
    };

    if (opening) {
        const QMetaObject::Connection connection = sampleFade(false);
        QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
        QTRY_VERIFY(popupItem->isVisible());
        // Halfway, the art is there, at the viewer's opacity.
        QTRY_VERIFY(drawnOpacity(popupItem) > 0.2 && drawnOpacity(popupItem) < 0.8);
        QVERIFY2(apart(false).isEmpty(), qPrintable(apart(false)));
        QVERIFY(drawnOpacity(image) > 0.2);
        QTRY_VERIFY(popup->property("opened").toBool() && drawnOpacity(popupItem) == 1.0);
        QObject::disconnect(connection);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY2(fadingFrames >= 3, qPrintable(QString::number(fadingFrames)));
        units->setLongDuration(0);
        QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
        QTRY_VERIFY(!popup->property("visible").toBool());
        return;
    }

    units->setLongDuration(0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    QTRY_VERIFY(popup->property("opened").toBool());
    QVERIFY(popupItem->isAncestorOf(surface));
    QVERIFY(QMetaObject::invokeMethod(viewer, "toggleAlbumArtInfoVisible", Q_ARG(QVariant, QVariant())));
    QTRY_VERIFY(column->isVisible());
    QTRY_COMPARE(image->property("status").toInt(), 1);
    QCOMPARE(drawnOpacity(popupItem), 1.0);
    units->setLongDuration(400);

    const QMetaObject::Connection connection = sampleFade(true);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    // At once, the art is still in the viewer, which goes on showing.
    QVERIFY2(apart(true).isEmpty(), qPrintable(apart(true)));
    // Halfway, the art and the details are there, at the viewer's opacity.
    QTRY_VERIFY(drawnOpacity(popupItem) > 0.2 && drawnOpacity(popupItem) < 0.8);
    QVERIFY2(apart(true).isEmpty(), qPrintable(apart(true)));
    QVERIFY(drawnOpacity(image) > 0.2 && drawnOpacity(column) > 0.2);
    // Faded out, the art leaves the viewer.
    QTRY_VERIFY(!popupItem->isVisible());
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY2(fadingFrames >= 3, qPrintable(QString::number(fadingFrames)));
    QTRY_VERIFY(surface->parentItem() == mainHost);
    QVERIFY(!surface->isVisible());
    QVERIFY(frames > 0);
}

// The visualization viewer in the window fades out over its last frame as
// it closes, while the pane under it shows the visualization live again
// (#373 H8: the visualization went back to the pane at once, and the
// viewer's black floor faded out alone over it). In every frame the viewer
// draws, it shows the visualization, live or as its kept frame, at its own
// opacity; once the frame is kept, the pane has the visualization. Faded
// out, the frame goes. Opened over the whole screen while it fades, the
// viewer gives the visualization up whole to that window.
void UiMotionTest::windowedViewerClosesOverItsLastFrame() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("spectrogramViewerPopup"));
    QQuickItem *pane = named(window, "spectrogramPane");
    QQuickItem *surface = named(window, "visualizationSurface");
    QQuickItem *spectrogram = named(window, "mainSpectrogramSurface");
    QObject *shell = window->findChild<QObject *>(QStringLiteral("spectrogramViewerShell"));
    QVERIFY(bridge && viewer && popup && pane && surface && spectrogram && shell);
    bridge->setProperty("viewerFullscreenMode", 0);
    // Columns the spectrogram shows, in colour.
    bridge->setProperty("durationSeconds", 60.0);
    bridge->setProperty("currentTrackChannels", 2);
    bridge->setProperty("waveformCoverageSeconds", 60.0);
    bridge->setProperty("waveformComplete", true);
    playQueueRow(bridge, 0, QStringLiteral("Paused"));
    QVERIFY(QMetaObject::invokeMethod(bridge, "precomputedSpectrogramChannelsReady", Q_ARG(int, 2),
                                      Q_ARG(bool, true)));
    QList<QQuickItem *> lanes;
    QTRY_COMPARE((lanes = itemsNamed(spectrogram, QStringLiteral("spectrogramPaneItem"))).size(), 2);
    for (qsizetype index = 0; index < lanes.size(); ++index) {
        constexpr int bins = 4'097;
        constexpr int columns = 200;
        qobject_cast<SpectrogramItem *>(lanes.at(index))
            ->feedPrecomputedChunk(QByteArray(bins * columns, '\x80'), bins, static_cast<int>(index), columns, 0,
                                   2'000, 48'000, 1'024, false, true, 1, false);
    }
    QTRY_VERIFY(!spectrogram->property("awaitingData").toBool());

    QQuickItem *popupItem = nullptr;
    QQuickItem *host = nullptr;
    const auto closingFrame = [&]() {
        return popupItem != nullptr ? itemNamed(popupItem, QStringLiteral("spectrogramViewerClosingFrame")) : nullptr;
    };
    const auto openInWindow = [&]() {
        units->setLongDuration(0);
        QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
        QTRY_VERIFY(popup->property("opened").toBool());
        QTRY_VERIFY(!pane->isAncestorOf(surface));
        popupItem = popupItemOf(popup);
        QVERIFY(popupItem != nullptr && popupItem->isAncestorOf(surface));
        host = surface->parentItem();
        QCOMPARE(drawnOpacity(popupItem), 1.0);
    };

    // What the viewer does not show at its opacity, or "".
    const auto apart = [&]() -> QString {
        if (!popupItem->isVisible()) {
            return {};
        }
        const double shown = drawnOpacity(popupItem);
        if (popupItem->isAncestorOf(surface)) {
            const double opacity = drawnOpacity(surface);
            return std::abs(opacity - shown) > 1e-9
                ? QStringLiteral("the visualization at %1, the viewer at %2").arg(opacity).arg(shown)
                : QString();
        }
        QQuickItem *frame = closingFrame();
        if (frame == nullptr || !frame->property("grabbed").toBool()) {
            return QStringLiteral("the visualization gone from the viewer, its frame not kept, at %1").arg(shown);
        }
        if (std::abs(drawnOpacity(frame) - shown) > 1e-9) {
            return QStringLiteral("the frame at %1, the viewer at %2").arg(drawnOpacity(frame)).arg(shown);
        }
        if (frame->size() != host->size()) {
            return QStringLiteral("the frame %1×%2, the view %3×%4").arg(frame->width()).arg(frame->height())
                .arg(host->width()).arg(host->height());
        }
        if (!pane->isAncestorOf(surface)) {
            return QStringLiteral("the visualization in neither the viewer nor the pane");
        }
        return {};
    };

    openInWindow();
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(closingFrame() == nullptr);
    units->setLongDuration(400);
    QString failure;
    int fadingFrames = 0;
    QMetaObject::Connection connection = QObject::connect(window, &QQuickWindow::frameSwapped, window, [&]() {
        const double shown = drawnOpacity(popupItem);
        fadingFrames += shown > 0.0 && shown < 1.0;
        if (failure.isEmpty()) {
            failure = apart();
        }
    });
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QVERIFY(!viewer->property("spectrogramViewerOpen").toBool());
    // At once, the visualization is still in the viewer; it goes back to
    // the pane once the viewer keeps its frame.
    QVERIFY(popupItem->isAncestorOf(surface));
    QTRY_VERIFY(pane->isAncestorOf(surface));
    QVERIFY(closingFrame() != nullptr && closingFrame()->property("grabbed").toBool());
    // Halfway, the frame shows at the viewer's opacity over the pane.
    QTRY_VERIFY(drawnOpacity(popupItem) > 0.2 && drawnOpacity(popupItem) < 0.8);
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QVERIFY(drawnOpacity(closingFrame()) > 0.2);
    // Faded out, the frame goes.
    QTRY_VERIFY(!popupItem->isVisible());
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY2(fadingFrames >= 3, qPrintable(QString::number(fadingFrames)));
    QTRY_VERIFY(closingFrame() == nullptr);
    QVERIFY(!shell->property("keepsVisualization").toBool());
    QVERIFY(pane->isAncestorOf(surface));

    // The kept frame shows what the viewer showed: with the fade too slow
    // to move, the window draws as it did before the viewer closed. The
    // viewer's controls stay while it is open, however slowly the test runs.
    QVERIFY(shell->setProperty("fullscreenControlsHideDelay", 600000));
    // With its scale, lanes at rest draw their lines and labels too, so the
    // frame has enough to compare (#373 H10 P18: without the scale, the
    // floor at rest is plain).
    bridge->setProperty("showSpectrogramScale", true);
    openInWindow();
    if (QTest::currentTestFailed()) {
        return;
    }
    // Back in the viewer, the plot's mode switch fades in with the viewer's
    // controls; once it has, nothing in the viewer moves. Grabbed before,
    // the window would miss what the kept frame shows of it.
    QQuickItem *modeSwitch = itemNamed(surface, QStringLiteral("visualizationModeSwitch"));
    QVERIFY(modeSwitch != nullptr);
    QTRY_COMPARE(drawnOpacity(modeSwitch), 1.0);
    window->update();
    QSignalSpy swapped(window, &QQuickWindow::frameSwapped);
    QVERIFY(swapped.wait(2000));
    const QImage before = window->grabWindow().convertToFormat(QImage::Format_RGB32);
    QColor floor(Qt::black);
    int drawn = 0;
    for (int y = 0; y < before.height(); y += 4) {
        for (int x = 0; x < before.width(); x += 4) {
            const QColor at = before.pixelColor(x, y);
            drawn += std::abs(at.red() - floor.red()) + std::abs(at.green() - floor.green())
                    + std::abs(at.blue() - floor.blue()) > 24;
        }
    }
    QVERIFY2(drawn > 1000, qPrintable(QString::number(drawn)));
    units->setLongDuration(600000);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(pane->isAncestorOf(surface));
    QVERIFY(closingFrame() != nullptr && closingFrame()->isVisible());
    const QImage after = window->grabWindow().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(after.size(), before.size());
    int differing = 0;
    // Where they differ, for the message.
    QRect differs;
    for (int y = 0; y < after.height(); ++y) {
        for (int x = 0; x < after.width(); ++x) {
            const QColor a = after.pixelColor(x, y);
            const QColor b = before.pixelColor(x, y);
            if (std::abs(a.red() - b.red()) > 8 || std::abs(a.green() - b.green()) > 8
                || std::abs(a.blue() - b.blue()) > 8) {
                ++differing;
                differs |= QRect(x, y, 1, 1);
            }
        }
    }
    QVERIFY2(differing <= after.width() * after.height() / 1000,
             qPrintable(QStringLiteral("%1 of %2 pixels differ, within %3×%4 at %5,%6")
                            .arg(differing)
                            .arg(after.width() * after.height())
                            .arg(differs.width())
                            .arg(differs.height())
                            .arg(differs.x())
                            .arg(differs.y())));

    // Opened over the whole screen while it fades: the viewer gives up its
    // frame, and the visualization goes whole into that window.
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openSpectrogramViewer"));
    QTRY_VERIFY(surface->window() != nullptr && surface->window() != window);
    QVERIFY(QTest::qWaitForWindowExposed(surface->window()));
    QVERIFY(closingFrame() == nullptr);
    QVERIFY(!shell->property("holdsVisualization").toBool());
    QVERIFY(!pane->property("holdsVisualization").toBool());
    units->setLongDuration(0);
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeSpectrogramViewer"));
    QTRY_VERIFY(pane->isAncestorOf(surface));
}

void UiMotionTest::albumArtViewerControlsFadeOnOneLevel_data() {
    QTest::addColumn<bool>("wholeScreen");
    QTest::newRow("whole screen") << true;
    QTest::newRow("in the window") << false;
}

// The album art viewer's controls hide while the pointer rests and come
// back as it moves, on the auto-hide's one level, as the visualization
// viewer's do: over the whole screen the title strip with its buttons, in
// the window the corner plate with its buttons (#373 H8: the corner plate
// never hid). Each is drawn at the level in every frame of the fade.
void UiMotionTest::albumArtViewerControlsFadeOnOneLevel() {
    QFETCH(bool, wholeScreen);
    Covers covers;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindow(engine, libraryModel);
    QVERIFY(window != nullptr);
    Kirigami::Platform::Units *units = unitsOf(window);
    QVERIFY(units != nullptr);
    units->setLongDuration(0);
    // Long enough for frames along the fade.
    units->setShortDuration(400);
    QObject *bridge = bridgeOf(window);
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *autoHide = window->findChild<QObject *>(QStringLiteral("albumArtFullscreenControlsAutoHide"));
    QObject *popup = window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
    QQuickItem *surface = named(window, "albumArtSurface");
    QVERIFY(viewer && autoHide && popup && surface);
    bridge->setProperty("viewerFullscreenMode", wholeScreen ? 1 : 0);
    showTrack(bridge, QStringLiteral("/music/0.flac"), QStringLiteral("Track"), QStringLiteral("Album"),
              covers.first);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QQuickWindow *host = window;
    if (wholeScreen) {
        QTRY_VERIFY(surface->window() != nullptr && surface->window() != window);
        host = surface->window();
        QVERIFY(QTest::qWaitForWindowExposed(host));
    } else {
        QTRY_VERIFY(popup->property("opened").toBool());
    }
    QVERIFY(autoHide->property("active").toBool());
    QQuickItem *controls = nullptr;
    QTRY_VERIFY((controls = itemNamed(host->contentItem(), wholeScreen ? QStringLiteral("albumArtFullscreenStrip")
                                                                         : QStringLiteral("albumArtPopupControls")))
                != nullptr);
    QList<QPair<QString, QQuickItem *>> drawn{
        {QStringLiteral("the controls"), controls},
        {QStringLiteral("Image Details"),
         itemNamed(controls, wholeScreen ? QStringLiteral("albumArtFullscreenInfoButton")
                                         : QStringLiteral("albumArtPopupInfoButton"))},
        {QStringLiteral("Close"),
         itemNamed(controls, wholeScreen ? QStringLiteral("albumArtFullscreenCloseButton")
                                         : QStringLiteral("albumArtPopupCloseButton"))},
    };
    if (!wholeScreen) {
        drawn.append({QStringLiteral("the corner plate"), itemNamed(controls, QStringLiteral("plotPlateFill"))});
    }
    for (const auto &[what, item] : drawn) {
        QVERIFY2(item != nullptr, qPrintable(what));
    }
    QQuickItem *container = wholeScreen ? host->contentItem() : popupItemOf(popup);
    QVERIFY(container != nullptr);
    // Where a control is not at the level, or "".
    const auto apart = [&]() -> QString {
        const double level = autoHide->property("level").toDouble() * drawnOpacity(container);
        for (const auto &[what, item] : drawn) {
            const double opacity = drawnOpacity(item);
            if (std::abs(opacity - level) > 1e-9) {
                return QStringLiteral("%1 at %2, the level at %3").arg(what).arg(opacity).arg(level);
            }
        }
        return {};
    };
    const QPoint onArt(host->width() / 3, host->height() / 2);
    QTest::mouseMove(host, onArt);
    QTRY_COMPARE(autoHide->property("level").toDouble(), 1.0);
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));

    QString failure;
    int fadingFrames = 0;
    const QMetaObject::Connection connection = QObject::connect(host, &QQuickWindow::frameSwapped, host, [&]() {
        const double level = autoHide->property("level").toDouble();
        fadingFrames += level > 0.0 && level < 1.0;
        if (failure.isEmpty()) {
            failure = apart();
        }
    });
    // The pointer resting: they fade out together, and hide once out.
    autoHide->setProperty("hideDelay", 1);
    QTRY_VERIFY(!autoHide->property("controlsVisible").toBool());
    QTRY_VERIFY(autoHide->property("level").toDouble() > 0.0 && autoHide->property("level").toDouble() < 1.0);
    QVERIFY(controls->isVisible());
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QTRY_VERIFY(!controls->isVisible());
    QCOMPARE(autoHide->property("level").toDouble(), 0.0);
    QVERIFY2(failure.isEmpty(), qPrintable(QStringLiteral("hiding: ") + failure));
    QVERIFY2(fadingFrames >= 3, qPrintable(QString::number(fadingFrames)));

    // The pointer moving brings them back, shown at once, fading in.
    autoHide->setProperty("hideDelay", 600000);
    fadingFrames = 0;
    QTest::mouseMove(host, onArt + QPoint(5, 0));
    QTRY_VERIFY(autoHide->property("controlsVisible").toBool());
    QVERIFY(controls->isVisible());
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QTRY_COMPARE(autoHide->property("level").toDouble(), 1.0);
    QObject::disconnect(connection);
    QVERIFY2(apart().isEmpty(), qPrintable(apart()));
    QVERIFY2(failure.isEmpty(), qPrintable(QStringLiteral("showing: ") + failure));
    QVERIFY2(fadingFrames >= 3, qPrintable(QString::number(fadingFrames)));
    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

#include "tst_ui_motion.moc"
