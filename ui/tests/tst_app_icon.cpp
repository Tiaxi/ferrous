// SPDX-License-Identifier: GPL-3.0-or-later

// The app icon (#373): a drawing of its own for each size, built in for the
// window icon and installed in the hicolor icon theme with the desktop file.

#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QPixmap>
#include <QRect>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "AppIcon.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

constexpr QRgb kTile = 0x3a1490;
// The tile's 1 px rim, lighter than the tile, so that the tile holds its
// edge on a dark panel (#373 hill climb, H9), at 16 to 32 px only (H10). Within
// a trace's distance of the tile: no trace is that close to it.
constexpr QRgb kRim = 0x6a4ad0;
// Breeze Dark's window.
constexpr QRgb kBreezeDarkWindow = 0x202326;
// The chirp: the fundamental in amber, its 2nd harmonic in rose and its 3rd
// in magenta, lower on the colour map and each 3:1 or more on the tile. At 16
// and 22 px the fundamental and the 3rd harmonic only.
constexpr QRgb kGlide = 0xf2b035;
constexpr QRgb kHarmonic = 0xf25a7e;
constexpr QRgb kThirdHarmonic = 0xe055b4;

QImage render(const QString &path, int size) {
    QImageReader reader(path);
    reader.setScaledSize(QSize(size, size));
    return reader.read().convertToFormat(QImage::Format_ARGB32);
}

// An opaque pixel within `tolerance` of a colour, in each channel.
bool near(QRgb pixel, QRgb colour, int tolerance) {
    return qAlpha(pixel) == 255 && std::abs(qRed(pixel) - qRed(colour)) <= tolerance
           && std::abs(qGreen(pixel) - qGreen(colour)) <= tolerance
           && std::abs(qBlue(pixel) - qBlue(colour)) <= tolerance;
}

// How far a pixel is from the tile's colour, in its most different channel.
int distanceFromTile(QRgb pixel) {
    return std::max({std::abs(qRed(pixel) - qRed(kTile)), std::abs(qGreen(pixel) - qGreen(kTile)),
                     std::abs(qBlue(pixel) - qBlue(kTile))});
}

// A trace's ink over the tile: a pixel it covers by about half or more. The
// tile has almost no red or green, every trace plenty of red; two traces
// apart by a row's faint antialiased edge are seen apart.
bool traceInk(QRgb pixel) {
    return qAlpha(pixel) == 255 && distanceFromTile(pixel) >= 80;
}

// WCAG's contrast ratio between two opaque colours.
double contrastRatio(QRgb a, QRgb b) {
    const auto luminance = [](QRgb c) {
        const auto channel = [](int v) {
            const double s = v / 255.0;
            return s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(qRed(c)) + 0.7152 * channel(qGreen(c)) + 0.0722 * channel(qBlue(c));
    };
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

int colourDistance(QRgb a, QRgb b) {
    return std::abs(qRed(a) - qRed(b)) + std::abs(qGreen(a) - qGreen(b)) + std::abs(qBlue(a) - qBlue(b));
}

int countWhere(const QImage &image, const QRect &area, const std::function<bool(QRgb)> &test) {
    int count = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            count += test(image.pixel(x, y)) ? 1 : 0;
        }
    }
    return count;
}

// The tile's square, found from its opaque pixels.
QRect tileOf(const QImage &image) {
    QRect tile;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                tile |= QRect(x, y, 1, 1);
            }
        }
    }
    return tile;
}

// Breeze's app-icon margin around the tile at each size: 1 px at 16, 2 px at
// 22 and 32, 4 px at 48 and 6 px at 64, as Breeze's own app icons sit, and in
// proportion above.
int breezeMargin(int size) {
    switch (size) {
    case 16:
        return 1;
    case 22:
    case 32:
        return 2;
    case 48:
        return 4;
    default:
        return size * 6 / 64;
    }
}

// The sizes drawn on the pixel grid, in whole-pixel stairs with no
// antialiasing: there an antialiased stroke broke into dashes and dots.
bool onPixelGrid(int size) {
    return size <= 32;
}

// The traces' colours, lowest first: the fundamental and two harmonics from
// 32 px up, the fundamental and the magenta 3rd harmonic at 16 and 22 px,
// where a middle trace fused with its neighbours (#373 hill climb, H10).
std::vector<QRgb> traceColours(int size) {
    if (size >= 32) {
        return {kGlide, kHarmonic, kThirdHarmonic};
    }
    return {kGlide, kThirdHarmonic};
}

// How tall each of the pixel grid's stairs is in every column, lowest first:
// at 16 px two 2 px strokes; at 22 px a 3 px fundamental and a 2 px harmonic
// (about 2.5 px each); at 32 px a 3 px fundamental and 2 px harmonics, the
// whole-pixel taper nearest 1 : 0.8 : 0.65 (#373 hill climb, H10).
std::vector<int> stairHeights(int size) {
    switch (size) {
    case 16:
        return {2, 2};
    case 22:
        return {3, 2};
    default:
        return {3, 2, 2};
    }
}

// The columns a trace's end takes before its full width: at the pixel grid
// one, its end column; from 48 px its round cap, as far as the fundamental's
// half width (1.7 of the drawing's 64 units), rounded up.
int capColumns(int size) {
    return onPixelGrid(size) ? 1 : (size * 2 + 63) / 64;
}

// The tile has a 1 px rim lighter than itself at 16 to 32 px, where it holds
// the tile's edge on Breeze Dark; from 48 px the tile is plain, as Breeze's
// app icons are (#373 hill climb, H9, H10).
bool hasRim(int size) {
    return size <= 32;
}

// The tile inside its rim, or the whole tile at the sizes without one.
QRect insideRim(const QRect &tile, int size) {
    return hasRim(size) ? tile.adjusted(1, 1, -1, -1) : tile;
}

// How much of a pixel a trace of a colour covers over the tile, 0 to 1, from
// the red channel, where every trace is far from the tile.
double coverage(QRgb pixel, QRgb colour) {
    const double span = qRed(colour) - qRed(kTile);
    return std::clamp((qRed(pixel) - qRed(kTile)) / span, 0.0, 1.0);
}

// The bounding box of the pixels that pass a test.
QRect boundsWhere(const QImage &image, const std::function<bool(QRgb)> &test) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (test(image.pixel(x, y))) {
                bounds |= QRect(x, y, 1, 1);
            }
        }
    }
    return bounds;
}

// The largest difference in any channel between two images of a size.
int maxDifference(const QImage &a, const QImage &b) {
    if (a.size() != b.size()) {
        return 256;
    }
    int largest = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y);
            const QRgb q = b.pixel(x, y);
            largest = std::max({largest, std::abs(qRed(p) - qRed(q)), std::abs(qGreen(p) - qGreen(q)),
                                std::abs(qBlue(p) - qBlue(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return largest;
}

// One trace of the chirp: a shape of touching ink pixels, corners included.
struct Trace {
    std::vector<QPoint> pixels;
    QRect bounds;

    // The ink-weighted height of the trace's centre in a column.
    double centreIn(const QImage &image, int x) const {
        double weight = 0;
        double sum = 0;
        for (const QPoint &p : pixels) {
            if (p.x() == x) {
                const double w = distanceFromTile(image.pixel(p));
                weight += w;
                sum += w * (p.y() + 0.5);
            }
        }
        return weight > 0 ? sum / weight : -1;
    }

    // How much of a column the trace of a colour covers, in whole pixels.
    double coverageIn(const QImage &image, int x, QRgb colour) const {
        double sum = 0;
        for (const QPoint &p : pixels) {
            if (p.x() == x) {
                sum += coverage(image.pixel(p), colour);
            }
        }
        return sum;
    }

    // Its most saturated pixel: the trace's own colour where it covers a
    // whole pixel, or nearest to it.
    QRgb strongest(const QImage &image) const {
        QRgb best = kTile;
        for (const QPoint &p : pixels) {
            if (distanceFromTile(image.pixel(p)) > distanceFromTile(best)) {
                best = image.pixel(p);
            }
        }
        return best;
    }
};

// The traces, lowest first.
std::vector<Trace> tracesOf(const QImage &image) {
    const int size = image.width();
    std::vector<int> shape(static_cast<size_t>(size * size), -1);
    std::vector<Trace> traces;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!traceInk(image.pixel(x, y)) || shape[static_cast<size_t>(y * size + x)] >= 0) {
                continue;
            }
            Trace trace;
            std::vector<QPoint> pending{QPoint(x, y)};
            shape[static_cast<size_t>(y * size + x)] = static_cast<int>(traces.size());
            while (!pending.empty()) {
                const QPoint p = pending.back();
                pending.pop_back();
                trace.pixels.push_back(p);
                trace.bounds |= QRect(p, QSize(1, 1));
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const QPoint q(p.x() + dx, p.y() + dy);
                        if (!image.rect().contains(q) || !traceInk(image.pixel(q))
                            || shape[static_cast<size_t>(q.y() * size + q.x())] >= 0) {
                            continue;
                        }
                        shape[static_cast<size_t>(q.y() * size + q.x())] = static_cast<int>(traces.size());
                        pending.push_back(q);
                    }
                }
            }
            traces.push_back(std::move(trace));
        }
    }
    std::sort(traces.begin(), traces.end(),
              [](const Trace &a, const Trace &b) { return a.bounds.bottom() > b.bounds.bottom(); });
    return traces;
}

QString installPrefix() {
    return qEnvironmentVariable("FERROUS_APP_ICON_INSTALL_PREFIX");
}

QString installedIcon(const QString &directory) {
    return QDir(installPrefix()).filePath(QStringLiteral("share/icons/hicolor/%1/apps/ferrous.svg").arg(directory));
}

QByteArray contentsOf(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void addSizeRows() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        QTest::addRow("%dpx", size) << size;
    }
}

} // namespace

class AppIconTest : public QObject {
    Q_OBJECT

private slots:
    void everySizeIsBuiltIn_data() { addSizeRows(); }
    void everySizeIsBuiltIn();
    void tileHasALighterRimAtSmallSizes_data() { addSizeRows(); }
    void tileHasALighterRimAtSmallSizes();
    void tracesStandOnThePlainTile_data() { addSizeRows(); }
    void tracesStandOnThePlainTile();
    void harmonicsAreOneSweepOnALogAxis_data() { addSizeRows(); }
    void harmonicsAreOneSweepOnALogAxis();
    void smallSizesAreDrawnOnThePixelGrid_data();
    void smallSizesAreDrawnOnThePixelGrid();
    void smallestSizesHaveTwoStrokesApart_data();
    void smallestSizesHaveTwoStrokesApart();
    void largeSizesTaperWithRoundCaps_data();
    void largeSizesTaperWithRoundCaps();
    void windowIconHasEachSizesDrawing();
    void everySizeInstalls_data();
    void everySizeInstalls();
    void iconThemeGivesEachSizeItsDrawing_data() { addSizeRows(); }
    void iconThemeGivesEachSizeItsDrawing();
    void desktopFileNamesTheIcon();
};

void AppIconTest::everySizeIsBuiltIn() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);
    QVERIFY2(QFile::exists(path), qPrintable(path));
    // Drawn at its own size, as declared in the file.
    QImageReader reader(path);
    QCOMPARE(reader.size(), QSize(size, size));
    const QImage image = render(path, size);
    QVERIFY2(!image.isNull(), qPrintable(reader.errorString()));

    // The tile fills most of the square, and the tile and the fundamental
    // each have pixels of their own colour.
    const QRect all = image.rect();
    const int opaque = countWhere(image, all, [](QRgb p) { return qAlpha(p) == 255; });
    QVERIFY2(opaque >= size * size / 2, qPrintable(QString::number(opaque)));
    QVERIFY(countWhere(image, all, [](QRgb p) { return near(p, kTile, 4); }) >= size * size / 4);
    QVERIFY(countWhere(image, all, [](QRgb p) { return near(p, kGlide, 24); }) >= size / 2);
    // Centred, in Breeze's app-icon margin.
    const QRect tile = tileOf(image);
    QCOMPARE(tile.left(), breezeMargin(size));
    QCOMPARE(tile.top(), breezeMargin(size));
    QCOMPARE(tile.left(), all.right() - tile.right());
    QCOMPARE(tile.top(), all.bottom() - tile.bottom());

    QVERIFY(QFile::exists(FerrousAppIcon::resourcePath(0)));
    QCOMPARE(FerrousAppIcon::resourcePath(24), QStringLiteral(":/icons/hicolor/scalable/apps/ferrous.svg"));
}

// The tile has a 1 px rim lighter than itself just inside its edge at 16 to
// 32 px, so that it holds its edge on a dark panel (#373 hill climb, H9: on
// Breeze Dark the indigo tile was 1.3:1 on the window); the tile's own
// colour is inside it. From 48 px the tile is large enough to hold its edge
// and plain, as Breeze's app icons have no outline (H10).
void AppIconTest::tileHasALighterRimAtSmallSizes() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const QPoint middle = tile.center();
    for (const QPoint &edge : {QPoint(middle.x(), tile.top()), QPoint(middle.x(), tile.bottom()),
                               QPoint(tile.left(), middle.y()), QPoint(tile.right(), middle.y())}) {
        QVERIFY2(near(image.pixel(edge), hasRim(size) ? kRim : kTile, 2),
                 qPrintable(QStringLiteral("#%1 at %2, %3")
                                .arg(image.pixel(edge) & 0xffffff, 6, 16, QLatin1Char('0'))
                                .arg(edge.x())
                                .arg(edge.y())));
    }
    // One pixel: inside it, at the bottom's middle, the tile.
    QVERIFY(near(image.pixel(middle.x(), tile.bottom() - 1), kTile, 2));
    // No rim colour anywhere from 48 px.
    if (!hasRim(size)) {
        QCOMPARE(countWhere(image, image.rect(), [](QRgb p) { return near(p, kRim, 24); }), 0);
    }
    // Lighter than the tile, and it holds the edge on Breeze Dark where the
    // tile alone doesn't; no trace.
    QVERIFY(contrastRatio(kTile, kBreezeDarkWindow) < 1.5);
    QVERIFY2(contrastRatio(kRim, kBreezeDarkWindow) >= 2.5,
             qPrintable(QString::number(contrastRatio(kRim, kBreezeDarkWindow))));
    QVERIFY(contrastRatio(kRim, kBreezeDarkWindow) > contrastRatio(kTile, kBreezeDarkWindow));
    QVERIFY(!traceInk(qRgba(qRed(kRim), qGreen(kRim), qBlue(kRim), 255)));
}

// The traces stand on the plain tile: H1 to H5 drew the spectrogram's floor
// under them as a magenta band, which read as an unrelated underline or a
// progress bar, so it is gone at every size (#373 hill climb, H5).
void AppIconTest::tracesStandOnThePlainTile() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const std::vector<Trace> traces = tracesOf(image);
    QVERIFY(!traces.empty());

    // Nothing but the tile under the lowest trace, down to the tile's rim:
    // no band, no line. From 48 px up the fundamental's antialiased edge
    // reaches a row below its ink; the stairs at 16 to 32 px have none; at
    // 22 px a row of the tile is left above the rim, and at 16 px they fill
    // the tile to its rim.
    int lowest = 0;
    for (const Trace &trace : traces) {
        lowest = std::max(lowest, trace.bounds.bottom());
    }
    const int top = lowest + (onPixelGrid(size) ? 1 : 2);
    const int corner = std::max(2, size / 8);
    const QRect below(tile.left() + corner, top, tile.width() - 2 * corner, insideRim(tile, size).bottom() - top + 1);
    QVERIFY2(below.height() >= (size > 22 ? 2 : size > 16 ? 1 : 0), qPrintable(QString::number(lowest)));
    QCOMPARE(countWhere(image, below, [](QRgb p) { return near(p, kTile, 4); }), below.width() * below.height());

    // Every mark rises: none is a flat band or line under the traces.
    for (const Trace &trace : traces) {
        const double left = trace.centreIn(image, trace.bounds.left());
        const double right = trace.centreIn(image, trace.bounds.right());
        QVERIFY2(left - right >= 1.0, qPrintable(QStringLiteral("a flat mark at rows %1 to %2")
                                                     .arg(trace.bounds.top())
                                                     .arg(trace.bounds.bottom())));
    }

    // Every trace is inside the tile's rim.
    for (const Trace &trace : traces) {
        QVERIFY2(insideRim(tile, size).contains(trace.bounds), qPrintable(QStringLiteral("%1,%2 %3x%4")
                                                                          .arg(trace.bounds.x())
                                                                          .arg(trace.bounds.y())
                                                                          .arg(trace.bounds.width())
                                                                          .arg(trace.bounds.height())));
    }

    // The traces sit in the middle of the tile, to within a pixel or so.
    const QRect ink = boundsWhere(image, traceInk);
    const double slack = std::max(1.0, size / 32.0);
    const double dx = (ink.left() + ink.right()) / 2.0 - (tile.left() + tile.right()) / 2.0;
    const double dy = (ink.top() + ink.bottom()) / 2.0 - (tile.top() + tile.bottom()) / 2.0;
    QVERIFY2(std::abs(dx) <= slack && std::abs(dy) <= slack,
             qPrintable(QStringLiteral("off centre by %1, %2").arg(dx).arg(dy)));
}

// Icon B (#373 hill climb, H7 vote P9, drawn in H8): a linear chirp on a
// log-frequency axis. The fundamental rises steeply and bends over, and its
// harmonics are the same curve shifted up by log(2) and log(3): parallel
// traces, the gap to the 3rd harmonic 0.585 of the gap to the 2nd. They are
// measured traces, not a swoosh: they start and end on one time line, each
// at least 3:1 on the tile, in strokes tapering from the fundamental up
// (H10), and fill the plate.
void AppIconTest::harmonicsAreOneSweepOnALogAxis() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const std::vector<Trace> traces = tracesOf(image);

    // Separate traces, three from 32 px up. At 16 and 22 px three were busy
    // and broke into dashes: there the fundamental and one harmonic.
    const size_t expected = size >= 32 ? 3 : 2;
    QVERIFY2(traces.size() == expected, qPrintable(QString::number(traces.size())));

    // Lowest first, each in its own colour, and each 3:1 or more on the
    // tile (A's crimson top stroke dropped out at 16 to 22 px).
    const std::vector<QRgb> colours = traceColours(size);
    for (size_t i = 0; i < traces.size(); ++i) {
        const QRgb strongest = traces[i].strongest(image);
        for (size_t j = 0; j < colours.size(); ++j) {
            QVERIFY2(j == i || colourDistance(strongest, colours[i]) < colourDistance(strongest, colours[j]),
                     qPrintable(QStringLiteral("trace %1 is #%2").arg(i).arg(strongest & 0xffffff, 6, 16, QLatin1Char('0'))));
        }
        QVERIFY2(contrastRatio(strongest, kTile) >= 3.0,
                 qPrintable(QStringLiteral("trace %1: %2:1").arg(i).arg(contrastRatio(strongest, kTile))));
    }
    for (const QRgb colour : {kGlide, kHarmonic, kThirdHarmonic}) {
        QVERIFY2(contrastRatio(colour, kTile) >= 3.0, qPrintable(QString::number(colour, 16)));
    }

    // One time line at either end: the traces start in one column and end
    // in one column, in the tile's left and right fifths. From 48 px their
    // round caps reach past the time lines by half their tapered widths, so
    // the fundamental's a little further than its harmonics'.
    const int capSlack = onPixelGrid(size) ? 0 : 1 + size / 64;
    for (const Trace &trace : traces) {
        QVERIFY2(std::abs(trace.bounds.left() - traces.front().bounds.left()) <= capSlack,
                 qPrintable(QStringLiteral("starts at %1, not %2").arg(trace.bounds.left()).arg(traces.front().bounds.left())));
        QVERIFY2(std::abs(trace.bounds.right() - traces.front().bounds.right()) <= capSlack,
                 qPrintable(QStringLiteral("ends at %1, not %2").arg(trace.bounds.right()).arg(traces.front().bounds.right())));
    }
    // The columns every trace crosses at its full width, past the caps.
    int start = 0;
    int end = size;
    for (const Trace &trace : traces) {
        start = std::max(start, trace.bounds.left() + capColumns(size));
        end = std::min(end, trace.bounds.right() - capColumns(size));
    }
    QVERIFY(traces.front().bounds.left() < tile.left() + tile.width() / 5);
    QVERIFY(traces.front().bounds.right() > tile.right() - tile.width() / 5);

    // Parallel: in every column each harmonic is the same height above the
    // fundamental, log(3)/log(2) times as high for the 3rd as for the 2nd.
    const double slack = std::max(0.75, size / 48.0);
    std::vector<double> offsets(traces.size(), 0.0);
    for (size_t n = 1; n < traces.size(); ++n) {
        offsets[n] = traces[0].centreIn(image, start) - traces[n].centreIn(image, start);
        QVERIFY2(offsets[n] >= 3.0, qPrintable(QString::number(offsets[n])));
        for (int x = start; x <= end; ++x) {
            const double offset = traces[0].centreIn(image, x) - traces[n].centreIn(image, x);
            QVERIFY2(std::abs(offset - offsets[n]) <= slack,
                     qPrintable(QStringLiteral("harmonic %1 at column %2: %3 above, not %4")
                                    .arg(n + 1).arg(x).arg(offset).arg(offsets[n])));
        }
    }
    if (traces.size() == 3) {
        const double ratio = offsets[2] / offsets[1];
        QVERIFY2(std::abs(ratio - std::log(3.0) / std::log(2.0)) <= 0.12, qPrintable(QString::number(ratio)));
    }

    // A log sweep: steep at first, bending over. The fundamental rises
    // twice as far or more in its first third as in its last, never falls,
    // and rises a quarter of the tile inside its rim or more.
    const auto centre = [&](int x) { return traces[0].centreIn(image, x); };
    const int third = (end - start) / 3;
    const double first = centre(start) - centre(start + third);
    const double last = centre(end - third) - centre(end);
    QVERIFY2(last > 0 && first >= 2.0 * last, qPrintable(QStringLiteral("%1 then %2").arg(first).arg(last)));
    for (int x = start + 1; x <= end; ++x) {
        QVERIFY2(centre(x) <= centre(x - 1) + 0.25, qPrintable(QString::number(x)));
    }
    QVERIFY2(centre(start) - centre(end) >= insideRim(tile, size).height() / 4.0,
             qPrintable(QString::number(centre(start) - centre(end))));

    // The strokes taper from the fundamental up, as a harmonic's energy
    // falls with its number: none heavier than the one below it, and from
    // 22 px the fundamental heavier than the top harmonic (H10; H8 and H9
    // drew three equal strokes). The ratios at the smooth sizes are checked
    // in largeSizesTaperWithRoundCaps.
    std::vector<double> weights;
    for (size_t n = 0; n < traces.size(); ++n) {
        double sum = 0;
        for (int x = start; x <= end; ++x) {
            sum += traces[n].coverageIn(image, x, colours[n]);
        }
        weights.push_back(sum);
    }
    for (size_t n = 1; n < weights.size(); ++n) {
        QVERIFY2(weights[n] <= weights[n - 1] * 1.02,
                 qPrintable(QStringLiteral("trace %1: %2 against %3").arg(n).arg(weights[n]).arg(weights[n - 1])));
    }
    if (size >= 22) {
        QVERIFY2(weights.back() < weights.front() * 0.8,
                 qPrintable(QStringLiteral("%1 against %2").arg(weights.back()).arg(weights.front())));
    }

    // They fill the plate: across 70 % of the tile or more each way, with ink
    // in its top-left quarter (A left that empty).
    const QRect ink = boundsWhere(image, traceInk);
    QVERIFY2(ink.width() >= tile.width() * 0.7 && ink.height() >= tile.height() * 0.7,
             qPrintable(QStringLiteral("%1x%2 of %3").arg(ink.width()).arg(ink.height()).arg(tile.width())));
    const QRect topLeft(tile.left(), tile.top(), tile.width() / 2, tile.height() / 2);
    QVERIFY(countWhere(image, topLeft, traceInk) > 0);

    // Nothing of the fundamental in the tile's top-right corner: it bends
    // over, it does not slash from corner to corner.
    const int cornerSide = std::lround(tile.width() * 0.2);
    const QRect corner(tile.right() + 1 - cornerSide, tile.top(), cornerSide, cornerSide);
    QCOMPARE(countWhere(image, corner, [](QRgb p) { return near(p, kGlide, 48); }), 0);
}

void AppIconTest::smallSizesAreDrawnOnThePixelGrid_data() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        if (onPixelGrid(size)) {
            QTest::addRow("%dpx", size) << size;
        }
    }
}

// At 16 and 22 px antialiased traces broke into stepped stripes or merged
// into soft blobs, and at 32 px the thinner harmonics broke into dots. Each
// is a drawing of its own on the pixel grid: filled whole-pixel stairs of a
// height of their own in every column (stairHeights), with clear rows between
// neighbours along their whole length and no two touching even at a corner;
// two traces at 16 and 22 px, three at 32 (#373 hill climb, H6, H8, H9, H10).
void AppIconTest::smallSizesAreDrawnOnThePixelGrid() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);

    // The file: the tile and one filled path a trace, every coordinate whole.
    QXmlStreamReader svg(contentsOf(path));
    QStringList ids;
    const QRegularExpression number(QStringLiteral("-?[0-9]*\\.?[0-9]+"));
    while (!svg.atEnd()) {
        if (svg.readNext() != QXmlStreamReader::StartElement || svg.name() != QLatin1String("path")) {
            continue;
        }
        const QXmlStreamAttributes attributes = svg.attributes();
        ids << attributes.value(QLatin1String("id")).toString();
        QVERIFY2(!attributes.hasAttribute(QLatin1String("stroke")), qPrintable(ids.last()));
        QVERIFY(!attributes.value(QLatin1String("fill")).isEmpty());
        const QString d = attributes.value(QLatin1String("d")).toString();
        QVERIFY(!d.isEmpty());
        auto numbers = number.globalMatch(d);
        while (numbers.hasNext()) {
            const QString value = numbers.next().captured();
            QVERIFY2(!value.contains(QLatin1Char('.')), qPrintable(QStringLiteral("%1: %2").arg(ids.last(), value)));
        }
    }
    QVERIFY2(!svg.hasError(), qPrintable(svg.errorString()));
    const QStringList expected = size >= 32
        ? QStringList{QStringLiteral("h3"), QStringLiteral("h2"), QStringLiteral("fundamental")}
        : QStringList{QStringLiteral("h3"), QStringLiteral("fundamental")};
    QCOMPARE(ids, expected);

    // Rendered: away from the tile's rounded corners, every pixel is the
    // tile's colour, its rim's or a trace's, with no antialiased edge
    // between.
    const QImage image = render(path, size);
    const QRect tile = tileOf(image);
    const int corner = std::max(2, size / 8);
    const QRgb colours[] = {kTile, kRim, kGlide, kHarmonic, kThirdHarmonic};
    for (int y = tile.top(); y <= tile.bottom(); ++y) {
        for (int x = tile.left(); x <= tile.right(); ++x) {
            const bool inCorner = (x < tile.left() + corner || x > tile.right() - corner)
                                  && (y < tile.top() + corner || y > tile.bottom() - corner);
            if (inCorner) {
                continue;
            }
            const QRgb pixel = image.pixel(x, y);
            QVERIFY2(std::any_of(std::begin(colours), std::end(colours), [pixel](QRgb c) { return near(pixel, c, 1); }),
                     qPrintable(QStringLiteral("#%1 at %2, %3").arg(pixel & 0xffffff, 6, 16, QLatin1Char('0')).arg(x).arg(y)));
        }
    }

    // Each trace as tall as its stair in every column, and a clear row of
    // the tile between neighbours in every column. That they are found as separate
    // traces at all means no two touch, even at a corner.
    const std::vector<Trace> traces = tracesOf(image);
    QCOMPARE(traces.size(), static_cast<size_t>(expected.size()));
    for (const Trace &trace : traces) {
        QCOMPARE(trace.bounds.left(), traces.front().bounds.left());
        QCOMPARE(trace.bounds.right(), traces.front().bounds.right());
    }
    std::vector<std::vector<int>> tops(traces.size());
    std::vector<int> gaps(traces.size(), -1);
    for (int x = traces.front().bounds.left(); x <= traces.front().bounds.right(); ++x) {
        int below = image.height();
        for (size_t n = 0; n < traces.size(); ++n) {
            const Trace &trace = traces[n];
            int top = image.height();
            int bottom = -1;
            int count = 0;
            for (const QPoint &p : trace.pixels) {
                if (p.x() == x) {
                    top = std::min(top, p.y());
                    bottom = std::max(bottom, p.y());
                    ++count;
                }
            }
            QVERIFY2(count == stairHeights(size)[n] && bottom == top + count - 1,
                     qPrintable(QStringLiteral("column %1: %2 px").arg(x).arg(count)));
            // The clear rows under each trace, the same in every column: the
            // harmonics are the fundamental's stair shifted up.
            if (n > 0) {
                const int gap = below - bottom - 1;
                QVERIFY2(gap >= 1, qPrintable(QStringLiteral("column %1: no clear row").arg(x)));
                gaps[n] = gaps[n] < 0 ? gap : gaps[n];
                QCOMPARE(gap, gaps[n]);
            }
            below = top;
            tops[n].push_back(top);
        }
    }
    // A pixel log sweep: each stair climbs a row a column at first, never
    // two (where a stair would break at its corners), and its runs on one
    // row only lengthen as it bends over, apart from the last, which the
    // time line cuts. Five runs or more at 16 px, six from 22.
    for (const std::vector<int> &stair : tops) {
        std::vector<int> runs{1};
        for (size_t c = 1; c < stair.size(); ++c) {
            const int rise = stair[c - 1] - stair[c];
            QVERIFY2(rise == 0 || rise == 1, qPrintable(QStringLiteral("column %1: %2 rows").arg(c).arg(rise)));
            if (rise == 0) {
                ++runs.back();
            } else {
                runs.push_back(1);
            }
        }
        QCOMPARE(runs.front(), 1);
        QVERIFY2(runs.size() >= (size > 16 ? 6u : 5u), qPrintable(QString::number(runs.size())));
        for (size_t r = 1; r + 1 < runs.size(); ++r) {
            QVERIFY2(runs[r] >= runs[r - 1], qPrintable(QStringLiteral("run %1: %2 after %3").arg(r).arg(runs[r]).arg(runs[r - 1])));
        }
        QVERIFY2(runs[runs.size() - 2] >= 2, qPrintable(QString::number(runs[runs.size() - 2])));
    }
}

void AppIconTest::smallestSizesHaveTwoStrokesApart_data() {
    QTest::addColumn<int>("size");
    QTest::addRow("16px") << 16;
    QTest::addRow("22px") << 22;
}

// At 16 and 22 px H9's two upper strokes, rose and magenta 2 rows apart,
// fused into one pink slab (#373 hill climb, H9). Now two strokes only, the
// amber fundamental and the magenta 3rd harmonic, with 2 rows of the tile or
// more between them in every column, so at least one device pixel of indigo
// separates them wherever they run (H10).
void AppIconTest::smallestSizesHaveTwoStrokesApart() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect inside = insideRim(tileOf(image), size);
    const int corner = std::max(2, size / 8);

    // Exactly two stroke colours: inside the rim, away from its rounded
    // corners, every pixel is the tile, the amber or the magenta, and both
    // strokes are there.
    int amber = 0;
    int magenta = 0;
    for (int y = inside.top(); y <= inside.bottom(); ++y) {
        for (int x = inside.left(); x <= inside.right(); ++x) {
            const bool inCorner = (x < inside.left() + corner || x > inside.right() - corner)
                                  && (y < inside.top() + corner || y > inside.bottom() - corner);
            const QRgb pixel = image.pixel(x, y);
            amber += near(pixel, kGlide, 1) ? 1 : 0;
            magenta += near(pixel, kThirdHarmonic, 1) ? 1 : 0;
            QVERIFY2(inCorner || near(pixel, kTile, 1) || near(pixel, kGlide, 1) || near(pixel, kThirdHarmonic, 1),
                     qPrintable(QStringLiteral("#%1 at %2, %3").arg(pixel & 0xffffff, 6, 16, QLatin1Char('0')).arg(x).arg(y)));
        }
    }
    QVERIFY(amber > 0);
    QVERIFY(magenta > 0);

    // In every column the strokes cross, both of them, the magenta above the
    // amber, and only the tile between, 2 rows or more.
    const QRect ink = boundsWhere(image, traceInk);
    for (int x = ink.left(); x <= ink.right(); ++x) {
        int magentaBottom = -1;
        int amberTop = size;
        for (int y = inside.top(); y <= inside.bottom(); ++y) {
            const QRgb pixel = image.pixel(x, y);
            if (near(pixel, kThirdHarmonic, 1)) {
                QVERIFY2(amberTop == size, qPrintable(QStringLiteral("column %1: magenta under amber").arg(x)));
                magentaBottom = y;
            } else if (near(pixel, kGlide, 1)) {
                amberTop = std::min(amberTop, y);
            }
        }
        QVERIFY2(magentaBottom >= 0 && amberTop < size, qPrintable(QStringLiteral("column %1").arg(x)));
        const int gap = amberTop - magentaBottom - 1;
        QVERIFY2(gap >= 2, qPrintable(QStringLiteral("column %1: %2 rows between").arg(x).arg(gap)));
        for (int y = magentaBottom + 1; y < amberTop; ++y) {
            QVERIFY(near(image.pixel(x, y), kTile, 1));
        }
    }
}

void AppIconTest::largeSizesTaperWithRoundCaps_data() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        if (!onPixelGrid(size)) {
            QTest::addRow("%dpx", size) << size;
        }
    }
}

// From 48 px the traces are a deliberate drawing (#373 hill climb, H10): the
// strokes taper from the fundamental up, 1 : 0.8 : 0.65, as a harmonic's
// energy falls with its number, and end in round caps, not the flat cuts on
// the time lines that read as clipped in H9.
void AppIconTest::largeSizesTaperWithRoundCaps() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);

    // The file: each trace a stroke with round caps, its width tapering
    // from the fundamental's.
    QXmlStreamReader svg(contentsOf(path));
    QHash<QString, double> widths;
    while (!svg.atEnd()) {
        if (svg.readNext() != QXmlStreamReader::StartElement || svg.name() != QLatin1String("path")) {
            continue;
        }
        const QXmlStreamAttributes attributes = svg.attributes();
        const QString id = attributes.value(QLatin1String("id")).toString();
        QCOMPARE(attributes.value(QLatin1String("fill")).toString(), QStringLiteral("none"));
        QCOMPARE(attributes.value(QLatin1String("stroke-linecap")).toString(), QStringLiteral("round"));
        widths.insert(id, attributes.value(QLatin1String("stroke-width")).toDouble());
    }
    QVERIFY2(!svg.hasError(), qPrintable(svg.errorString()));
    QCOMPARE(widths.size(), 3);
    QVERIFY(widths.value(QStringLiteral("fundamental")) > 0);
    QVERIFY(std::abs(widths.value(QStringLiteral("h2")) / widths.value(QStringLiteral("fundamental")) - 0.8) < 0.01);
    QVERIFY(std::abs(widths.value(QStringLiteral("h3")) / widths.value(QStringLiteral("fundamental")) - 0.65) < 0.01);

    // Rendered: three traces, in their colours.
    const QImage image = render(path, size);
    const std::vector<Trace> traces = tracesOf(image);
    QCOMPARE(traces.size(), static_cast<size_t>(3));
    const std::vector<QRgb> colours = traceColours(size);

    // No flat cut on the time line, at 256 px, where a cap is pixels enough
    // across to tell: a trace's end column holds the tip of its cap and the
    // column inside it less than the stroke's height, both short of a column
    // of the stroke's body just inside the cap (its height at that end's
    // slope). A vertical cut, as in H9, has the stroke's whole height in the
    // column inside its end, whatever pixel the cut falls in.
    if (size == 256) {
        const int inward = 2 * capColumns(size);
        for (size_t n = 0; n < traces.size(); ++n) {
            const Trace &trace = traces[n];
            for (const int step : {1, -1}) {
                const int tip = step > 0 ? trace.bounds.left() : trace.bounds.right();
                const double body = trace.coverageIn(image, tip + step * inward, colours[n]);
                const double last = trace.coverageIn(image, tip, colours[n]);
                const double next = trace.coverageIn(image, tip + step, colours[n]);
                QVERIFY2(last < 0.6 * body && next < 0.85 * body,
                         qPrintable(QStringLiteral("trace %1 at column %2: %3, then %4, against %5")
                                        .arg(n).arg(tip).arg(last).arg(next).arg(body)));
            }
        }
    }

    // The taper as drawn: each trace's coverage summed over the columns
    // every trace crosses at its full width. The traces are parallel, so in
    // any column their heights keep their widths' ratio.
    int start = 0;
    int end = size;
    for (const Trace &trace : traces) {
        start = std::max(start, trace.bounds.left() + capColumns(size));
        end = std::min(end, trace.bounds.right() - capColumns(size));
    }
    std::vector<double> weights;
    for (size_t n = 0; n < traces.size(); ++n) {
        double sum = 0;
        for (int x = start; x <= end; ++x) {
            sum += traces[n].coverageIn(image, x, colours[n]);
        }
        weights.push_back(sum);
    }
    const double expected[] = {1.0, 0.8, 0.65};
    for (size_t n = 1; n < weights.size(); ++n) {
        const double ratio = weights[n] / weights[0];
        QVERIFY2(std::abs(ratio - expected[n]) <= 0.07,
                 qPrintable(QStringLiteral("trace %1: %2 of the fundamental").arg(n).arg(ratio)));
    }
}

void AppIconTest::windowIconHasEachSizesDrawing() {
    const QIcon icon = FerrousAppIcon::icon();
    QVERIFY(!icon.isNull());
    const QList<QSize> available = icon.availableSizes();
    for (const int size : FerrousAppIcon::kSizes) {
        QVERIFY2(available.contains(QSize(size, size)), qPrintable(QString::number(size)));
        // At each size, that size's drawing, not another scaled.
        const QImage expected = render(FerrousAppIcon::resourcePath(size), size);
        const QImage shown = icon.pixmap(QSize(size, size), 1.0).toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(maxDifference(shown, expected) <= 2, qPrintable(QString::number(size)));
    }
}

void AppIconTest::everySizeInstalls_data() {
    QTest::addColumn<QString>("directory");
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        QTest::addRow("%dpx", size) << QStringLiteral("%1x%1").arg(size) << size;
    }
    QTest::addRow("scalable") << QStringLiteral("scalable") << 0;
}

void AppIconTest::everySizeInstalls() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QFETCH(QString, directory);
    QFETCH(int, size);
    const QString path = installedIcon(directory);
    QVERIFY2(QFile::exists(path), qPrintable(path));
    // The installed file is the built-in drawing.
    const QByteArray installed = contentsOf(path);
    QVERIFY(!installed.isEmpty());
    QCOMPARE(installed, contentsOf(FerrousAppIcon::resourcePath(size)));
    const int drawn = size > 0 ? size : 64;
    const QImage image = render(path, drawn);
    QVERIFY(!image.isNull());
    QVERIFY(countWhere(image, image.rect(), [](QRgb p) { return near(p, kGlide, 24); }) >= drawn / 2);
}

void AppIconTest::iconThemeGivesEachSizeItsDrawing() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QFETCH(int, size);
    // The hicolor theme's index, as the hicolor-icon-theme package has it:
    // the fixed sizes first, then the scalable directory.
    QTemporaryDir index;
    QVERIFY(index.isValid());
    QVERIFY(QDir(index.path()).mkpath(QStringLiteral("hicolor")));
    {
        QString text;
        QStringList directories;
        for (const int fixed : FerrousAppIcon::kSizes) {
            const QString directory = QStringLiteral("%1x%1/apps").arg(fixed);
            directories << directory;
            text += QStringLiteral("[%1]\nSize=%2\nContext=Applications\nType=Fixed\n\n").arg(directory).arg(fixed);
        }
        directories << QStringLiteral("scalable/apps");
        text += QStringLiteral("[scalable/apps]\nMinSize=1\nSize=128\nMaxSize=256\nContext=Applications\nType=Scalable\n");
        text.prepend(QStringLiteral("[Icon Theme]\nName=Hicolor\nDirectories=%1\n\n").arg(directories.join(QLatin1Char(','))));
        QFile theme(index.filePath(QStringLiteral("hicolor/index.theme")));
        QVERIFY(theme.open(QIODevice::WriteOnly));
        QVERIFY(theme.write(text.toUtf8()) > 0);
    }
    // A theme icon looks itself up again when the theme changes: keep the
    // theme until the test ends.
    const QStringList searchPaths = QIcon::themeSearchPaths();
    const QString themeName = QIcon::themeName();
    const auto restoreTheme = qScopeGuard([&]() {
        QIcon::setThemeName(themeName);
        QIcon::setThemeSearchPaths(searchPaths);
    });
    QIcon::setThemeSearchPaths({index.path(), QDir(installPrefix()).filePath(QStringLiteral("share/icons"))});
    QIcon::setThemeName(QStringLiteral("hicolor"));
    const QIcon icon = QIcon::fromTheme(QStringLiteral("ferrous"));
    QVERIFY(!icon.isNull());
    const QImage shown = icon.pixmap(QSize(size, size), 1.0).toImage().convertToFormat(QImage::Format_ARGB32);
    const QImage expected = render(installedIcon(QStringLiteral("%1x%1").arg(size)), size);
    QVERIFY2(maxDifference(shown, expected) <= 2, qPrintable(QString::number(maxDifference(shown, expected))));
}

void AppIconTest::desktopFileNamesTheIcon() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    const QString path = QDir(installPrefix()).filePath(QStringLiteral("share/applications/ferrous.desktop"));
    QVERIFY2(QFile::exists(path), qPrintable(path));
    const QStringList lines = QString::fromUtf8(contentsOf(path)).split(QLatin1Char('\n'));
    QVERIFY(lines.contains(QStringLiteral("Icon=ferrous")));
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    // No desktop session's platform theme, whose icon loader could answer
    // for the theme; TestSessionIsolation.h.
    isolateFromDesktopSession();
    QGuiApplication app(argc, argv);
    AppIconTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_app_icon.moc"
