// SPDX-License-Identifier: GPL-3.0-or-later

// The app icon (#373): a drawing of its own for each size, built in for the
// window icon and installed in the hicolor icon theme with the desktop file.

#include <QBuffer>
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
#include <QtMath>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "AppIcon.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

constexpr QRgb kTile = 0x3a1490;
// The tile's rim, lighter than the tile, so that the tile holds its edge on a
// dark panel (#373 hill climb, H9), the same rim at every size (H11), and a
// quiet mid-violet since H18: H13's #6a4ad0 was the icon's loudest edge, and
// a near-tile #24096b vanished on Breeze Dark. Within a trace's distance of
// the tile: no trace is that close to it.
constexpr QRgb kRim = 0x5a3fb0;
constexpr QRgb kLoudRim = 0x6a4ad0;
// Breeze's panels: Dark's window and view, Light's window and view.
constexpr QRgb kBreezeDarkWindow = 0x202326;
constexpr QRgb kBreezeDarkView = 0x31363b;
constexpr QRgb kBreezeLightWindow = 0xeff0f1;
constexpr QRgb kBreezeLightView = 0xffffff;
// The chirp: the fundamental in amber, its 2nd harmonic in rose and its 3rd
// in magenta, lower on the colour map and each 3:1 or more on the tile, a
// step darker each (H20: the rose was as dark as the magenta). At 16 and
// 22 px the fundamental and the 3rd harmonic only. One magenta at every size
// (#373 hill climb, H23: H22's deeper one at 32 to 48 px made the top trace
// the dimmest there, and its colour changed with the size); the rose a step
// lighter instead, its hue kept, so that the three hold apart in greyscale.
constexpr QRgb kGlide = 0xf2b035;
constexpr QRgb kHarmonic = 0xff7a92;
constexpr QRgb kThirdHarmonic = 0xe055b4;
// H22's deeper magenta at 32 to 48 px, and its rose: no file has them now.
constexpr QRgb kDeepThirdHarmonic = 0xd442ab;
constexpr QRgb kDarkerHarmonic = 0xf87089;
// The faint 4th harmonic of the 128 and 256 px drawings, a violet a step
// darker again (#373 hill climb, H21); H21's, darker still, whose cut on the
// tile's top inner edge fused with the rim (H24); and H24's, 2.9:1 on the
// tile, with little reserve over the tile (H25).
constexpr QRgb kFourthHarmonic = 0xab55df;
constexpr QRgb kDarkerFourthHarmonic = 0x9a4bd0;
constexpr QRgb kH24FourthHarmonic = 0xa456d6;

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

// The sizes drawn as a smaller size's drawing in a larger canvas, as Breeze
// draws its 24 px icons: 24 px the 22 px drawing moved a pixel in (#373 hill
// climb, H22). (36 px was the 32 px one moved two until H24, a tile 72 % of
// the slot; it has its own drawing now, taskManagerSize.) The size whose
// drawing a size has: its own, or that one.
int drawnAs(int size) {
    return size == 24 ? 22 : size;
}

// The task manager's sizes, 32 and 36 px: one drawing on each grid, with
// the same strokes, taper and starts' spacing (#373 hill climb, H24).
bool taskManagerSize(int size) {
    return size == 32 || size == 36;
}

// Breeze's app-icon margin around the tile at each size: 1 px at 16, 2 px at
// 22 and 32, 4 px at 48 and 6 px at 64, as Breeze's own app icons sit, and in
// proportion above; 3 px at 24, the 22 px drawing's 2 and the pixel it moves
// in, and 3 at 36, a 30 px rim (#373 hill climb, H24: H22's 32 px drawing
// moved two pixels in left a 28 px rim).
int breezeMargin(int size) {
    switch (size) {
    case 16:
        return 1;
    case 24:
    case 36:
        return 3;
    case 22:
    case 32:
        return 2;
    case 48:
        return 4;
    default:
        return size * 6 / 64;
    }
}

// The sizes drawn on the pixel grid, 16, 22 and 24 px: each trace a closed shape
// with every vertex on a whole pixel, so that its flats are crisp and only
// its climbs, a row a column at most, are antialiased (#373 hill climb,
// H18). From 32 px, where the third trace comes in, the traces are smooth
// curves (H20: H19's whole-pixel stairs at 32 and 48 px read as aliased
// beside Breeze's icons, and changed idiom between 48 and 64 px).
bool onPixelGrid(int size) {
    return size <= 24;
}

// The traces' colours, lowest first: the fundamental and two harmonics from
// 32 px up, the fundamental and the magenta 3rd harmonic at 16 and 22 px,
// where a middle trace fused with its neighbours (#373 hill climb, H10, H18).
std::vector<QRgb> traceColours(int size) {
    if (size >= 32) {
        return {kGlide, kHarmonic, kThirdHarmonic};
    }
    return {kGlide, kThirdHarmonic};
}

// The sizes with a detail tier: at 128 and 256 px a faint 4th harmonic over
// the three traces (#373 hill climb, H21: the 64 px drawing enlarged left the
// tile's upper left half empty beside Breeze's large icons). 64 px and the
// scalable drawing, which serves 40 to 60 device px, keep three.
bool hasDetailTier(int size) {
    return size == 128 || size == 256;
}

// The ids of the series the tests check together, in drawing order, the top
// harmonic first: the fundamental and its 2nd and 3rd harmonics, or at 16
// and 22 px the fundamental and the 3rd.
QStringList seriesIds(int size) {
    if (size >= 32) {
        return {QStringLiteral("h3"), QStringLiteral("h2"), QStringLiteral("fundamental")};
    }
    return {QStringLiteral("h3"), QStringLiteral("fundamental")};
}

// Every trace's id in the file, in drawing order: the 4th harmonic first
// where there is one.
QStringList traceIds(int size) {
    QStringList ids = seriesIds(size);
    if (hasDetailTier(size)) {
        ids.prepend(QStringLiteral("h4"));
    }
    return ids;
}

// How tall each trace is on the pixel grid, lowest first: two 2 px strokes
// at 16 and 22 px (H18: a 1 px harmonic broke into dots), the fundamental
// 3 px at 22 px, so that the amber outweighs the magenta harmonic that rises
// across more of the tile (#373 hill climb, H21), and so at 24 px, the 22 px
// drawing, and at 16 px from its 4th column (H23: at 2 px the amber read
// lighter than the magenta over it; hintedSizesAreClosedShapesOnThePixelGrid).
std::vector<int> strokeHeights() {
    return {3, 2};
}

// How tall a trace is in a column on the pixel grid, `offset` columns from
// its start: its height (strokeHeights), but at 16 px the fundamental's
// first three columns, which start it at 2 px, a row clear of the harmonic.
int strokeHeightAt(int size, size_t trace, int offset) {
    return size == 16 && trace == 0 && offset < 3 ? 2 : strokeHeights()[trace];
}

// The width the 64-unit drawing's harmonics taper from, in its units: 3.8 at
// 64 px and 3.4 from 128 px (H19's 3.0 read thin beside 128 px icons), the
// rose 0.8 of it and the magenta 0.65, as a harmonic's energy falls with its
// number. The fundamental is heavier (fundamentalWidth).
double taperReference(int size) {
    return size == 64 ? 3.8 : 3.4;
}

// The fundamental's width in the 64-unit drawing's units: the focal stroke,
// clearly heavier than the harmonics over it, 5 units at 64 px and 4.5 from
// 128 (#373 hill climb, H25: at 3.8 and 3.4, 1 : 0.8 : 0.65 read as three
// near-equal stripes). Its added weight is on its underside: its top edge is
// where the series puts it (fundamentalUnderside).
double fundamentalWidth(int size) {
    return size == 64 ? 5.0 : 4.5;
}

// How far the fundamental's centre sits under the series' centre line, in
// the 64-unit drawing's units: half its added weight.
double fundamentalUnderside(int size) {
    return (fundamentalWidth(size) - taperReference(size)) / 2;
}

// The smooth strokes' widths across, in device pixels, lowest first: the
// fundamental the heaviest, the harmonics tapering above it, and never under
// 2 px (H20). At 32 and 36 px 4 / 2.5 / 2.25 (#373 hill climb, H25: H24's
// 3 / 2.5 / 2 read as three near-equal stripes in the task manager), the rose
// and the magenta eased to 2 px into their ends (easedWidth); at 48 px
// 4 / 2.5 / 2.25 (H25: the fundamental 3 px read as one of three equal
// stripes), the top trace 2.25 px
// over most of its run, eased to 2 px into its end (topTraceWidths); from
// 64 px the fundamental's fundamentalWidth and the harmonics 0.8 and 0.65 of
// taperReference.
std::vector<double> strokeWidths(int size) {
    switch (drawnAs(size)) {
    case 32:
    case 36:
    case 48:
        return {4.0, 2.5, 2.25};
    default: {
        const double unit = size / 64.0;
        const double reference = taperReference(size);
        return {fundamentalWidth(size) * unit, reference * 0.8 * unit, reference * 0.65 * unit};
    }
    }
}

// The 4th harmonic's width across at 128 and 256 px, in device pixels: half
// the 3.4 unit stroke the harmonics taper from (H21), faint beside the
// fundamental.
double fourthWidth(int size) {
    return taperReference(size) / 2 * size / 64.0;
}

// The top trace's width across at 48 px, at its start and at its end (#373
// hill climb, H23: a 2 px stroke read as the thinnest and dimmest trace, and
// beaded over its steep start): 2.25 px to x 38, eased to 2 over its last
// 5 px only, into its end's whole rows (H24: eased by x 20, it thinned to
// 2 px over most of its run).
std::pair<double, double> topTraceWidths() {
    return {2.25, 2.0};
}

// Where the 48 px top trace starts to ease to its end's width.
constexpr int kTopTraceHeldTo = 38;

// A trace's width across as drawn at `x`, lowest first, `edge` the tile's
// inner edge: its width (strokeWidths), but at 32 and 36 px the rose's
// 2.5 px and the magenta's 2.25 eased to 2 px over their last 4 px, so that
// they run out on two whole rows each (#373 hill climb, H24, H25), and at
// 32 px the fundamental's 4 px eased to 3 over its last 4 px, its top edge
// a row lower at its end, so that it opens apart from the rose, whose exit
// is a row lower too (H26).
double easedWidth(int size, size_t trace, double x, double edge) {
    const double width = strokeWidths(size)[trace];
    if (!taskManagerSize(size) || (trace == 0 && size != 32)) {
        return width;
    }
    const double end = trace == 0 ? 3.0 : 2.0;
    const double t = std::clamp((x - (edge - 4)) / 4, 0.0, 1.0);
    return width + (end - width) * t * t * (3 - 2 * t);
}

// The columns a trace's start takes before its full width: none at 16 and
// 22 px, where it starts square; from 32 px its round cap, as far as the
// fundamental's half width, rounded up. The other end has no cap: the
// traces run out to the tile's edge.
int capColumns(int size) {
    if (onPixelGrid(size)) {
        return 0;
    }
    return static_cast<int>(std::ceil(strokeWidths(size).front() / 2));
}

// The tile's rim, lighter than itself, so that it holds the tile's edge on
// Breeze Dark (#373 hill climb, H9, H11: at H10 only 16 to 32 px had it): a
// hairline, 1 device px at every size (H18: H13's 32nd of the icon, 4 and 8
// px at 128 and 256, framed the large sizes in a thick band; H21: H18's 2 px
// at 128 and 256 read as a halo on Breeze Light). The scalable drawing's is
// 1.5 units of its 64 (scalableDrawingKeepsADevicePixelRim).
constexpr int kRimWidth = 1;

// The tile inside its rim.
QRect insideRim(const QRect &tile) {
    return tile.adjusted(kRimWidth, kRimWidth, -kRimWidth, -kRimWidth);
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

// A trace's weight over the columns from `start` to `end`: its coverage in
// each, across the stroke. A slanting stroke covers more of a column the
// steeper it runs, so its coverage is taken across its slope.
double strokeWeight(const QImage &image, const Trace &trace, QRgb colour, int start, int end) {
    double sum = 0;
    for (int x = start; x <= end; ++x) {
        const double slope = (trace.centreIn(image, x + 1) - trace.centreIn(image, x - 1)) / 2;
        sum += trace.coverageIn(image, x, colour) / std::hypot(1.0, slope);
    }
    return sum;
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

// A drawing rendered at a size without its 4th harmonic, if it has one: the
// series the tests check as a whole (detailTierAddsAFaint4thHarmonic checks
// the 4th).
QImage renderSeries(const QString &path, int size) {
    QStringList lines = QString::fromUtf8(contentsOf(path)).split(QLatin1Char('\n'));
    lines.removeIf([](const QString &line) { return line.contains(QLatin1String("<path id=\"h4\"")); });
    QByteArray bytes = lines.join(QLatin1Char('\n')).toUtf8();
    QBuffer buffer(&bytes);
    QImageReader reader(&buffer, "svg");
    reader.setScaledSize(QSize(size, size));
    return reader.read().convertToFormat(QImage::Format_ARGB32);
}

// Each trace of a drawing alone, by its id: the drawing without its tile,
// its rim or its other traces, rendered at a size, so that each pixel's
// alpha is how much of it the trace covers.
QHash<QString, QImage> traceCoverage(const QString &path, int size) {
    const QStringList lines = QString::fromUtf8(contentsOf(path)).split(QLatin1Char('\n'));
    static const QRegularExpression pathId(QStringLiteral("<path id=\"(\\w+)\""));
    QHash<QString, QImage> traces;
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = pathId.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        QStringList alone;
        for (const QString &other : lines) {
            if (!other.contains(QLatin1String("<rect")) && (other == line || !other.contains(QLatin1String("<path ")))) {
                alone << other;
            }
        }
        QByteArray bytes = alone.join(QLatin1Char('\n')).toUtf8();
        QBuffer buffer(&bytes);
        QImageReader reader(&buffer, "svg");
        reader.setScaledSize(QSize(size, size));
        traces.insert(match.captured(1), reader.read().convertToFormat(QImage::Format_ARGB32));
    }
    return traces;
}

// The tile inside the rim alone, rendered at a size: each pixel's alpha is
// how much of it the tile covers, so that the rim's rounded corners count out.
QImage tileCoverage(const QString &path, int size) {
    QStringList lines = QString::fromUtf8(contentsOf(path)).split(QLatin1Char('\n'));
    lines.removeIf([](const QString &line) {
        return line.contains(QLatin1String("<path ")) || line.contains(QLatin1String("<rect id=\"rim\""));
    });
    QByteArray bytes = lines.join(QLatin1Char('\n')).toUtf8();
    QBuffer buffer(&bytes);
    QImageReader reader(&buffer, "svg");
    reader.setScaledSize(QSize(size, size));
    return reader.read().convertToFormat(QImage::Format_ARGB32);
}

// How much of a pixel a trace drawn alone covers, 0 to 1.
double alphaAt(const QImage &trace, int x, int y) {
    return qAlpha(trace.pixel(x, y)) / 255.0;
}

// A pixel a trace touches, past its faintest antialiasing.
constexpr double kTouched = 0.04;

// A trace drawn alone, column by column: how much of the column it covers,
// the rows it touches and the height of its centre.
struct Column {
    double sum = 0;
    int top = -1;
    int bottom = -1;
    double centre = -1;
};

Column columnOf(const QImage &trace, int x) {
    Column column;
    double moment = 0;
    for (int y = 0; y < trace.height(); ++y) {
        const double c = alphaAt(trace, x, y);
        if (c > kTouched) {
            column.top = column.top < 0 ? y : column.top;
            column.bottom = y;
        }
        column.sum += c;
        moment += c * (y + 0.5);
    }
    column.centre = column.sum > 0 ? moment / column.sum : -1;
    return column;
}

// The columns a trace drawn alone touches.
std::pair<int, int> columnsOf(const QImage &trace) {
    int first = -1;
    int last = -1;
    for (int x = 0; x < trace.width(); ++x) {
        if (columnOf(trace, x).top >= 0) {
            first = first < 0 ? x : first;
            last = x;
        }
    }
    return {first, last};
}

// A trace's width across its stroke, in device pixels, on average over the
// columns from `start` to `end`: its coverage of each column across its
// slope there.
double widthAcross(const QImage &trace, int start, int end) {
    double sum = 0;
    for (int x = start; x <= end; ++x) {
        const double slope = (columnOf(trace, x + 1).centre - columnOf(trace, x - 1).centre) / 2;
        sum += columnOf(trace, x).sum / std::hypot(1.0, slope);
    }
    return sum / (end - start + 1);
}


// The sizes with a drawing of their own for their slot at 2x scaling: 16, 22,
// 32 and 36 px doubled (#373 hill climb, H26: 32 and 36 px had the 64 px
// composition, lighter than their 1x drawings), 48 px with the 64 px
// composition (H21: Qt's theme took the scalable drawing there, with a 1
// device px rim where the 16 and 22 px slots had 2), and 64 px its drawing
// doubled (H22). None for 24 px, whose slot at 2x
// takes the 22 px slot's (hicolorsOwnIndexPicksTheSlotsDrawings; H23: H22's
// 24x24@2 was never chosen).
constexpr std::array<int, 6> kDoubledSizes{16, 22, 32, 36, 48, 64};

// The drawing for a size's slot at 2x scaling.
QString doubledPath(int size) {
    return QStringLiteral(":/icons/hicolor/%1x%1@2/apps/ferrous.svg").arg(size);
}

// The fundamental's width in a 2x slot's drawing, in the 64-unit composition's
// units, and how far its centre sits under the series' centre line, half its
// added weight: the focal stroke, as at 1x (#373 hill climb, H25), 4.7 units
// over the 48 px slot's 3.6 and 5 over the 64 px slot's 3.8.
std::pair<double, double> doubledFundamental(int size) {
    return size == 48 ? std::pair<double, double>{4.7, (4.7 - 3.6) / 2} : std::pair<double, double>{5.0, (5.0 - 3.8) / 2};
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
    void tileHasTheSameLighterRimAtEverySize_data() { addSizeRows(); }
    void tileHasTheSameLighterRimAtEverySize();
    void scalableDrawingKeepsADevicePixelRim();
    void tracesStandOnThePlainTile_data() { addSizeRows(); }
    void tracesStandOnThePlainTile();
    void harmonicsFanOutFromOneStart_data() { addSizeRows(); }
    void harmonicsFanOutFromOneStart();
    void hintedSizesAreClosedShapesOnThePixelGrid_data();
    void hintedSizesAreClosedShapesOnThePixelGrid();
    void hintedTracesKeepClearOfEachOtherAndTheRim_data();
    void hintedTracesKeepClearOfEachOtherAndTheRim();
    void tracesRunOutFlushToTheTilesInnerEdge_data();
    void tracesRunOutFlushToTheTilesInnerEdge();
    void largeSizesTaperFromARoundStart_data();
    void largeSizesTaperFromARoundStart();
    void smallCurvesAreEvenAndEndOnWholeRows_data();
    void smallCurvesAreEvenAndEndOnWholeRows();
    void fundamentalThinsSmoothlyWithSize();
    void fundamentalLeadsTheHarmonics_data();
    void fundamentalLeadsTheHarmonics();
    void oneMagentaAtEverySize_data();
    void oneMagentaAtEverySize();
    void topTraceClearsTheCornerFrom64px_data();
    void topTraceClearsTheCornerFrom64px();
    void detailTierAddsAFaint4thHarmonic_data();
    void detailTierAddsAFaint4thHarmonic();
    void doubledSlotsDrawTheSmallCompositionsAtTwice_data();
    void doubledSlotsDrawTheSmallCompositionsAtTwice();
    void doubledTaskManagerSlotsDoubleTheir1xDrawings_data();
    void doubledTaskManagerSlotsDoubleTheir1xDrawings();
    void doubledSlotsFrom48pxDrawThe64pxComposition_data();
    void doubledSlotsFrom48pxDrawThe64pxComposition();
    void sizesDrawnAsASmallerSize_data();
    void sizesDrawnAsASmallerSize();
    void windowIconHasEachSizesDrawing();
    void everySizeInstalls_data();
    void everySizeInstalls();
    void noDrawingForA24pxSlotAt2x();
    void iconThemeGivesEachSizeItsDrawing_data();
    void iconThemeGivesEachSizeItsDrawing();
    void hicolorsOwnIndexPicksTheSlotsDrawings_data();
    void hicolorsOwnIndexPicksTheSlotsDrawings();
    void desktopFileNamesTheIcon();
    void commentsDescribeTheDrawingNotItsHistory_data();
    void commentsDescribeTheDrawingNotItsHistory();
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
    QCOMPARE(FerrousAppIcon::resourcePath(40), QStringLiteral(":/icons/hicolor/scalable/apps/ferrous.svg"));
}

// The tile has a rim lighter than itself just inside its edge, so that it
// holds its edge on a dark panel (#373 hill climb, H9: on Breeze Dark the
// indigo tile was 1.3:1 on the window); the tile's own colour is inside it.
// It is the same rim at every size (H11: from 48 px the tile was plain at
// H10, and the sizes differed), a quiet hairline (H18).
void AppIconTest::tileHasTheSameLighterRimAtEverySize() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const QPoint middle = tile.center();
    const int rim = kRimWidth;
    // Across each edge's middle, inwards: the rim's colour for its width,
    // then no longer (the tile's, or at 16 px a trace's end on the right).
    const QPoint inwards[] = {QPoint(0, 1), QPoint(0, -1), QPoint(1, 0), QPoint(-1, 0)};
    const QPoint edges[] = {QPoint(middle.x(), tile.top()), QPoint(middle.x(), tile.bottom()),
                            QPoint(tile.left(), middle.y()), QPoint(tile.right(), middle.y())};
    for (int side = 0; side < 4; ++side) {
        for (int step = 0; step <= rim; ++step) {
            const QPoint p = edges[side] + inwards[side] * step;
            const bool inRim = near(image.pixel(p), kRim, step < rim ? 2 : 24);
            QVERIFY2(inRim == (step < rim),
                     qPrintable(QStringLiteral("#%1 at %2, %3, %4 px in")
                                    .arg(image.pixel(p) & 0xffffff, 6, 16, QLatin1Char('0'))
                                    .arg(p.x())
                                    .arg(p.y())
                                    .arg(step)));
        }
    }
    // Inside it, at the bottom's middle, the tile.
    QVERIFY(near(image.pixel(middle.x(), tile.bottom() - rim), kTile, 2));
    // Nothing inside the rim has its colour, away from its rounded inner
    // corners: no trace or mark on the tile. The magenta harmonic's faint
    // antialiased edge over the tile comes within 24 of the mid-violet rim in
    // each channel, though never within 12.
    const QRect inside = insideRim(tile);
    const int corner = size / 16 + 1;
    for (const QRect &band : {inside.adjusted(corner, 0, -corner, 0), inside.adjusted(0, corner, 0, -corner)}) {
        QCOMPARE(countWhere(image, band, [](QRgb p) { return near(p, kRim, 12); }), 0);
    }
    // Lighter than the tile, and it holds the edge on Breeze Dark's window
    // and view where the tile alone doesn't, and the silhouette on Breeze
    // Light's; no trace.
    QVERIFY(contrastRatio(kTile, kBreezeDarkWindow) < 1.5);
    QVERIFY2(contrastRatio(kRim, kBreezeDarkWindow) >= 2.0,
             qPrintable(QString::number(contrastRatio(kRim, kBreezeDarkWindow))));
    for (const QRgb panel : {kBreezeDarkWindow, kBreezeDarkView}) {
        QVERIFY2(contrastRatio(kRim, panel) >= contrastRatio(kTile, panel) + 0.5,
                 qPrintable(QStringLiteral("#%1: %2:1").arg(panel, 6, 16, QLatin1Char('0')).arg(contrastRatio(kRim, panel))));
    }
    for (const QRgb panel : {kBreezeLightWindow, kBreezeLightView}) {
        QVERIFY2(contrastRatio(kRim, panel) >= 4.5,
                 qPrintable(QStringLiteral("#%1: %2:1").arg(panel, 6, 16, QLatin1Char('0')).arg(contrastRatio(kRim, panel))));
    }
    // Quiet: nearer the tile than H13's light rim, the loudest edge of the
    // icon then, and quieter on the tile than any trace.
    QVERIFY2(contrastRatio(kRim, kTile) <= 1.75, qPrintable(QString::number(contrastRatio(kRim, kTile))));
    QVERIFY(contrastRatio(kRim, kTile) < contrastRatio(kLoudRim, kTile));
    for (const QRgb trace : {kGlide, kHarmonic, kThirdHarmonic}) {
        QVERIFY(contrastRatio(kRim, kTile) < contrastRatio(trace, kTile));
    }
    QVERIFY(!traceInk(qRgba(qRed(kRim), qGreen(kRim), qBlue(kRim), 255)));
}

// The scalable drawing serves the slots no drawing of the theme's matches,
// 40 to 60 device px or so: there its rim is 1.5 units of its 64, about a
// device pixel, where a 64th of the icon, H21's, was 0.6 to 0.9 px, a
// grey-violet smudge rather than an edge (#373 hill climb, H22). The tile
// inside the rim, 1.5 units in from it all round.
void AppIconTest::scalableDrawingKeepsADevicePixelRim() {
    const QString path = FerrousAppIcon::resourcePath(96);
    QCOMPARE(path, QStringLiteral(":/icons/hicolor/scalable/apps/ferrous.svg"));
    const QString source = QString::fromUtf8(contentsOf(path));
    QVERIFY(source.contains(QLatin1String("<rect id=\"rim\" fill=\"#5a3fb0\" x=\"6\" y=\"6\" width=\"52\" height=\"52\" rx=\"5\"/>")));
    QVERIFY(source.contains(QLatin1String("<rect id=\"tile\" fill=\"#3a1490\" x=\"7.5\" y=\"7.5\" width=\"49\" height=\"49\" rx=\"3.5\"/>")));
    // At 128 and 256 px whole device pixels of it, 3 and 6, then the tile.
    for (const int size : {128, 256}) {
        const QImage image = render(path, size);
        const QRect tile = tileOf(image);
        const QPoint middle = tile.center();
        const int rim = size * 3 / 128;
        for (int step = 0; step <= rim; ++step) {
            QVERIFY2(near(image.pixel(middle.x(), tile.bottom() - step), step < rim ? kRim : kTile, 2),
                     qPrintable(QStringLiteral("%1 px: %2").arg(size).arg(step)));
        }
    }
    // From 40 px about a device pixel of it: the rim drawn alone covers
    // that much more of a column across the bottom edge than the tile does.
    const QStringList lines = source.split(QLatin1Char('\n'));
    const auto alone = [&](const QString &id, int size) {
        QStringList kept;
        for (const QString &line : lines) {
            if (!line.contains(QLatin1String("<rect")) && !line.contains(QLatin1String("<path"))) {
                kept << line;
            } else if (line.contains(QStringLiteral("id=\"%1\"").arg(id))) {
                kept << line;
            }
        }
        QByteArray bytes = kept.join(QLatin1Char('\n')).toUtf8();
        QBuffer buffer(&bytes);
        QImageReader reader(&buffer, "svg");
        reader.setScaledSize(QSize(size, size));
        return reader.read().convertToFormat(QImage::Format_ARGB32);
    };
    for (const int size : {40, 44, 56}) {
        const Column rim = columnOf(alone(QStringLiteral("rim"), size), size / 2);
        const Column tile = columnOf(alone(QStringLiteral("tile"), size), size / 2);
        const double width = (rim.sum - tile.sum) / 2;
        QVERIFY2(width >= 0.9 && std::abs(width - 1.5 * size / 64) <= 0.05,
                 qPrintable(QStringLiteral("%1 px: a %2 px rim").arg(size).arg(width)));
    }
}

// The traces stand on the plain tile: H1 to H5 drew the spectrogram's floor
// under them as a magenta band, which read as an unrelated underline or a
// progress bar, so it is gone at every size (#373 hill climb, H5).
void AppIconTest::tracesStandOnThePlainTile() {
    QFETCH(int, size);
    const QImage image = renderSeries(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const std::vector<Trace> traces = tracesOf(image);
    QVERIFY(!traces.empty());

    // Nothing but the tile under the lowest trace, down to the tile's rim:
    // no band, no line. From 64 px up the fundamental's antialiased edge
    // reaches a row below its ink; on the pixel grid it has none; a row of
    // the tile or more is left above the rim (H19: at 16 px the fundamental
    // sat on it).
    int lowest = 0;
    for (const Trace &trace : traces) {
        lowest = std::max(lowest, trace.bounds.bottom());
    }
    const int top = lowest + (onPixelGrid(size) ? 1 : 2);
    const int corner = std::max(2, size / 8);
    const QRect below(tile.left() + corner, top, tile.width() - 2 * corner, insideRim(tile).bottom() - top + 1);
    // At 32 and 36 px a row, under the fundamental's round start (#373 hill
    // climb, H25: its added weight on its underside).
    QVERIFY2(below.height() >= (onPixelGrid(size) || taskManagerSize(size) ? 1 : 2), qPrintable(QString::number(lowest)));
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
        QVERIFY2(insideRim(tile).contains(trace.bounds), qPrintable(QStringLiteral("%1,%2 %3x%4")
                                                                          .arg(trace.bounds.x())
                                                                          .arg(trace.bounds.y())
                                                                          .arg(trace.bounds.width())
                                                                          .arg(trace.bounds.height())));
    }

    // The traces sit in the middle of the tile's height, to within a pixel
    // or so; across it they start in its left part and run out to its edge
    // (tracesRunOutFlushToTheTilesInnerEdge).
    const QRect ink = boundsWhere(image, traceInk);
    const double slack = std::max(1.0, size / 32.0);
    const double dy = (ink.top() + ink.bottom()) / 2.0 - (tile.top() + tile.bottom()) / 2.0;
    QVERIFY2(std::abs(dy) <= slack, qPrintable(QStringLiteral("off centre by %1").arg(dy)));
}

// Icon B (#373 hill climb, H7 vote P9, drawn in H8; a fan since H13): a
// glide and its harmonics on a linear frequency axis. Each harmonic is the
// glide's frequency times its number, so from a common start at the lower
// left the three rise 1 : 2 : 3 and fan out across the tile's diagonal (H12
// drew the same curve shifted up a log axis: three parallel strokes that
// read as swooshes or a trend line). They are measured traces: they start
// on one time line and run out to the tile's edge (H19), each at least 3:1
// on the tile, in strokes tapering from the fundamental up (H10), bending
// over as a glide does, and fill the plate.
void AppIconTest::harmonicsFanOutFromOneStart() {
    QFETCH(int, size);
    const QImage image = renderSeries(FerrousAppIcon::resourcePath(size), size);
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
    // A step of value from each trace to the one above it, so that the
    // three hold apart in greyscale (#373 hill climb, H20: the rose was
    // 3.9:1 on the tile, the magenta 3.6:1): the amber some 6.6:1, the rose
    // 5:1 (H23: 4.6:1 before, a step lighter), the magenta 3.6:1
    // (oneMagentaAtEverySize).
    QVERIFY2(contrastRatio(kHarmonic, kTile) >= 4.9 && contrastRatio(kHarmonic, kTile) <= 5.1,
             qPrintable(QString::number(contrastRatio(kHarmonic, kTile))));
    QVERIFY(contrastRatio(kGlide, kTile) >= contrastRatio(kHarmonic, kTile) + 1.5);
    QVERIFY(contrastRatio(kHarmonic, kTile) >= contrastRatio(kThirdHarmonic, kTile) + 1.2);

    // One time line at the start: the traces start in one column, in the
    // tile's left fifth. From 64 px their round caps reach past it by half
    // their tapered widths, so the fundamental's a little further than its
    // harmonics'. They end in one column, the tile's edge.
    const int capSlack = onPixelGrid(size) ? 0 : 1 + size / 64;
    for (const Trace &trace : traces) {
        QVERIFY2(std::abs(trace.bounds.left() - traces.front().bounds.left()) <= capSlack,
                 qPrintable(QStringLiteral("starts at %1, not %2").arg(trace.bounds.left()).arg(traces.front().bounds.left())));
        QVERIFY2(std::abs(trace.bounds.right() - traces.front().bounds.right()) <= capSlack,
                 qPrintable(QStringLiteral("ends at %1, not %2").arg(trace.bounds.right()).arg(traces.front().bounds.right())));
    }
    // The columns every trace crosses at its full width, past the caps: the
    // caps share a centre, and the fundamental's, the widest, reaches
    // furthest back from it. (A harmonic's faint cap tip, as ink, depends on
    // its colour, so its first column is not compared.)
    const int start = traces.front().bounds.left() + capColumns(size);
    int end = size;
    for (const Trace &trace : traces) {
        end = std::min(end, trace.bounds.right() - 1);
    }
    QVERIFY(traces.front().bounds.left() <= tile.left() + tile.width() / 5);
    QCOMPARE(traces.front().bounds.right(), insideRim(tile).right());

    // The harmonic numbers of the traces, lowest first.
    const std::vector<double> numbers = traces.size() == 3 ? std::vector<double>{1, 2, 3} : std::vector<double>{1, 3};
    const auto centre = [&](size_t n, int x) { return traces[n].centreIn(image, x); };
    const double slack = std::max(0.75, size / 48.0);

    // A harmonic series of one glide: each trace rises its harmonic number
    // times as far as the fundamental, to within the pixel grid's rounding.
    // (At 16 px the fundamental's centre rises 1.5 px: its top edge climbs
    // two rows and its bottom one, as it grows from 2 to 3 px; H23.)
    // At 32 px the ends sit up to 1.5 px higher than the series from the
    // starts would put them, by the fundamental's (#373 hill climb, H24: the
    // fan opened wider to ends 2 px under the top inner edge, from the same
    // starts), so the harmonics rise up to 1.25 px less than their share there.
    const double fundamentalRise = centre(0, start) - centre(0, end);
    QVERIFY2(fundamentalRise >= (size == 16 ? 1.5 : 2.0), qPrintable(QString::number(fundamentalRise)));
    for (size_t n = 1; n < traces.size(); ++n) {
        const double rise = centre(n, start) - centre(n, end);
        QVERIFY2(std::abs(rise / numbers[n] - fundamentalRise) <= (size == 32 ? 1.25 : slack),
                 qPrintable(QStringLiteral("harmonic %1 rises %2, the fundamental %3")
                                .arg(numbers[n]).arg(rise).arg(fundamentalRise)));
    }

    // They fan out: the height between neighbours' centres grows from the
    // start to the end, by half or more, and never shrinks along the way by
    // more than a row of the grid's rounding (H12's traces kept one height
    // apart), at 32 px too, measured past the caps, where the traces have
    // opened already (#373 hill climb, H24: by a third, then two fifths,
    // which read as nearly parallel traces).
    const double fan = 1.5;
    for (size_t n = 1; n < traces.size(); ++n) {
        const auto apart = [&](int x) { return centre(n - 1, x) - centre(n, x); };
        QVERIFY2(apart(end) >= fan * apart(start),
                 qPrintable(QStringLiteral("trace %1: %2 apart at the start, %3 at the end")
                                .arg(n).arg(apart(start)).arg(apart(end))));
        for (int x = start + 1; x <= end; ++x) {
            QVERIFY2(apart(x) >= apart(x - 1) - std::max(1.0, slack),
                     qPrintable(QStringLiteral("trace %1 at column %2: %3 after %4").arg(n).arg(x).arg(apart(x)).arg(apart(x - 1))));
        }
    }
    // On a linear frequency axis the middle harmonic is midway between its
    // neighbours in every column.
    if (traces.size() == 3) {
        for (int x = start; x <= end; ++x) {
            const double off = centre(1, x) - (centre(0, x) + centre(2, x)) / 2;
            QVERIFY2(std::abs(off) <= std::max(1.25, slack),
                     qPrintable(QStringLiteral("column %1: the 2nd harmonic %2 off the middle").arg(x).arg(off)));
        }
    }

    // A glide: steep at first, bending over. The top harmonic, which rises
    // furthest, rises twice as far or more in its first third as in its last
    // (at 32 px 1.75 times: its round cap's two columns, left out, take the
    // steepest of its rise), and a third of the tile inside its rim or more
    // (at 32 px 0.3, where its cap's columns are left out; H24: a quarter
    // before); none falls.
    const size_t top = traces.size() - 1;
    const int third = (end - start) / 3;
    const double first = centre(top, start) - centre(top, start + third);
    const double last = centre(top, end - third) - centre(top, end);
    QVERIFY2(last > 0 && first >= (taskManagerSize(size) ? 1.75 : 2.0) * last, qPrintable(QStringLiteral("%1 then %2").arg(first).arg(last)));
    for (size_t n = 0; n < traces.size(); ++n) {
        for (int x = start + 1; x <= end; ++x) {
            QVERIFY2(centre(n, x) <= centre(n, x - 1) + 0.25, qPrintable(QStringLiteral("trace %1 at %2").arg(n).arg(x)));
        }
    }
    QVERIFY2(centre(top, start) - centre(top, end) >= insideRim(tile).height() * (size == 32 ? 0.3 : 1.0 / 3),
             qPrintable(QString::number(centre(top, start) - centre(top, end))));

    // The strokes taper from the fundamental up, as a harmonic's energy
    // falls with its number: none heavier than the one below it, and from
    // 32 px the fundamental heavier than the top harmonic (H10; H8 and H9
    // drew three equal strokes). At 16 and 22 px two 2 px strokes since H18,
    // as wide across where they climb as on a flat since H19, so the steeper
    // harmonic, the longer one, comes within a twentieth of the
    // fundamental's weight. The ratios at the smooth sizes are checked in
    // largeSizesTaperFromARoundStart.
    std::vector<double> weights;
    for (size_t n = 0; n < traces.size(); ++n) {
        weights.push_back(strokeWeight(image, traces[n], colours[n], start, end));
    }
    for (size_t n = 1; n < weights.size(); ++n) {
        QVERIFY2(weights[n] <= weights[n - 1] * (size >= 32 ? 1.02 : 1.05),
                 qPrintable(QStringLiteral("trace %1: %2 against %3").arg(n).arg(weights[n]).arg(weights[n - 1])));
    }
    if (size >= 32) {
        QVERIFY2(weights.back() < weights.front() * 0.8,
                 qPrintable(QStringLiteral("%1 against %2").arg(weights.back()).arg(weights.front())));
    }

    // They fill the plate along its diagonal: across 70 % of the tile or more
    // each way (H24: at 32 px two thirds of its height before, with 4 px of
    // air over the top trace), from its
    // bottom-left quarter, with ink in its top-left quarter (A left that
    // empty) and its top-right one.
    const QRect ink = boundsWhere(image, traceInk);
    QVERIFY2(ink.width() >= tile.width() * 0.7 && ink.height() >= tile.height() * 0.7,
             qPrintable(QStringLiteral("%1x%2 of %3").arg(ink.width()).arg(ink.height()).arg(tile.width())));
    const int half = tile.width() / 2;
    QVERIFY(countWhere(image, QRect(tile.left(), tile.top(), half, half), traceInk) > 0);
    QVERIFY(countWhere(image, QRect(tile.right() + 1 - half, tile.top(), half, half), traceInk) > 0);
    QVERIFY(countWhere(image, QRect(tile.left(), tile.bottom() + 1 - half, half, half), traceInk) > 0);

    // Nothing of the fundamental in the tile's top-right corner: it bends
    // over, it does not slash from corner to corner.
    const int cornerSide = std::lround(tile.width() * 0.2);
    const QRect corner(tile.right() + 1 - cornerSide, tile.top(), cornerSide, cornerSide);
    QCOMPARE(countWhere(image, corner, [](QRgb p) { return near(p, kGlide, 48); }), 0);
}

void AppIconTest::hintedSizesAreClosedShapesOnThePixelGrid_data() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        if (onPixelGrid(size)) {
            QTest::addRow("%dpx", size) << size;
        }
    }
}

// At 16 and 22 px two hinted strokes (#373 hill climb, H18, H19): each trace
// is a closed shape with every vertex on a whole pixel and every edge level,
// upright or at 45 degrees: crisp flats, and climbs of a row a column at
// most, antialiased only there. Each covers its full height in every column
// (strokeHeights), at most a row more where it climbs, as a broad nib does.
void AppIconTest::hintedSizesAreClosedShapesOnThePixelGrid() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);

    // The file: one filled closed path a trace, every coordinate whole and
    // every edge level, upright or at 45 degrees.
    QXmlStreamReader svg(contentsOf(path));
    QStringList ids;
    const QRegularExpression number(QStringLiteral("-?[0-9]*\\.?[0-9]+"));
    const QRegularExpression otherCommand(QStringLiteral("[A-LN-Ya-z]"));
    while (!svg.atEnd()) {
        if (svg.readNext() != QXmlStreamReader::StartElement || svg.name() != QLatin1String("path")) {
            continue;
        }
        const QXmlStreamAttributes attributes = svg.attributes();
        ids << attributes.value(QLatin1String("id")).toString();
        QVERIFY2(!attributes.hasAttribute(QLatin1String("stroke")), qPrintable(ids.last()));
        QVERIFY(!attributes.value(QLatin1String("fill")).isEmpty());
        QVERIFY(attributes.value(QLatin1String("fill")) != QLatin1String("none"));
        const QString d = attributes.value(QLatin1String("d")).toString();
        // A polygon: a move, its vertices, closed.
        QVERIFY2(d.startsWith(QLatin1Char('M')) && d.endsWith(QLatin1Char('Z')) && !d.contains(otherCommand),
                 qPrintable(d));
        std::vector<int> values;
        for (auto numbers = number.globalMatch(d); numbers.hasNext();) {
            const QString value = numbers.next().captured();
            QVERIFY2(!value.contains(QLatin1Char('.')), qPrintable(QStringLiteral("%1: %2").arg(ids.last(), value)));
            values.push_back(value.toInt());
        }
        QVERIFY(values.size() >= 8 && values.size() % 2 == 0);
        const size_t vertices = values.size() / 2;
        for (size_t i = 0; i < vertices; ++i) {
            const size_t j = (i + 1) % vertices;
            const int dx = std::abs(values[2 * j] - values[2 * i]);
            const int dy = std::abs(values[2 * j + 1] - values[2 * i + 1]);
            QVERIFY2(dx == 0 || dy == 0 || dx == dy,
                     qPrintable(QStringLiteral("%1: an edge %2 by %3").arg(ids.last()).arg(dx).arg(dy)));
        }
    }
    QVERIFY2(!svg.hasError(), qPrintable(svg.errorString()));
    QCOMPARE(ids, traceIds(size));

    // Rendered: inside the rim, away from its rounded corners, every pixel
    // is the tile or the tile under one trace, so no two touch.
    const QImage image = render(path, size);
    const QRect inside = insideRim(tileOf(image));
    const QHash<QString, QImage> cover = traceCoverage(path, size);
    const int corner = std::max(2, size / 8);
    const std::vector<QRgb> colours = traceColours(size);
    std::vector<QImage> traces;
    for (auto id = ids.crbegin(); id != ids.crend(); ++id) {
        traces.push_back(cover.value(*id));
    }
    for (int y = inside.top(); y <= inside.bottom(); ++y) {
        for (int x = inside.left(); x <= inside.right(); ++x) {
            const bool inCorner = (x < inside.left() + corner || x > inside.right() - corner)
                                  && (y < inside.top() + corner || y > inside.bottom() - corner);
            int touching = 0;
            QRgb expected = kTile;
            for (size_t n = 0; n < traces.size(); ++n) {
                const double c = alphaAt(traces[n], x, y);
                if (c > 0) {
                    ++touching;
                    expected = qRgb(qRound(qRed(kTile) + c * (qRed(colours[n]) - qRed(kTile))),
                                    qRound(qGreen(kTile) + c * (qGreen(colours[n]) - qGreen(kTile))),
                                    qRound(qBlue(kTile) + c * (qBlue(colours[n]) - qBlue(kTile))));
                }
            }
            QVERIFY2(touching <= 1, qPrintable(QStringLiteral("traces meet at %1, %2").arg(x).arg(y)));
            QVERIFY2(inCorner || near(image.pixel(x, y), expected, 4),
                     qPrintable(QStringLiteral("#%1 at %2, %3")
                                    .arg(image.pixel(x, y) & 0xffffff, 6, 16, QLatin1Char('0'))
                                    .arg(x)
                                    .arg(y)));
        }
    }

    // Column by column, from the common start to the tile's inner edge:
    // each trace one run of rows, its height or a row more, whole pixels but
    // where an edge climbs across the column, and climbing a row a column at
    // most.
    const int start = columnsOf(traces.front()).first;
    const std::vector<int> heights = strokeHeights();
    for (size_t n = 0; n < traces.size(); ++n) {
        const auto [first, last] = columnsOf(traces[n]);
        QCOMPARE(first, start);
        QCOMPARE(last, inside.right());
        int crisp = 0;
        double before = -1;
        for (int x = first; x <= last; ++x) {
            const Column column = columnOf(traces[n], x);
            bool whole = true;
            for (int y = column.top; y <= column.bottom; ++y) {
                const double c = alphaAt(traces[n], x, y);
                QVERIFY2(c > kTouched, qPrintable(QStringLiteral("trace %1, column %2: broken").arg(n).arg(x)));
                const bool edge = y == column.top || y == column.bottom;
                QVERIFY2(c > 0.97 || (edge && std::abs(c - 0.5) <= 0.1),
                         qPrintable(QStringLiteral("trace %1 at %2, %3: %4").arg(n).arg(x).arg(y).arg(c)));
                whole = whole && c > 0.97;
            }
            crisp += whole ? 1 : 0;
            const int height = strokeHeightAt(size, n, x - first);
            QVERIFY2(column.sum >= height - 0.05 && column.sum <= height + 1.05,
                     qPrintable(QStringLiteral("trace %1, column %2: %3 px tall").arg(n).arg(x).arg(column.sum)));
            QVERIFY2(before < 0 || (column.centre <= before + 0.01 && before - column.centre <= 1.01),
                     qPrintable(QStringLiteral("trace %1, column %2: from %3 to %4").arg(n).arg(x).arg(before).arg(column.centre)));
            before = column.centre;
        }
        // Crisp, every pixel whole, in a third of the fundamental's columns
        // or more, and in the flats of a harmonic, which climbs over most
        // of its length.
        QVERIFY2(crisp >= (n == 0 ? (last - first + 1) / 3 : 3),
                 qPrintable(QStringLiteral("trace %1: %2 crisp columns").arg(n).arg(crisp)));
    }
    // The harmonic's start no heavier than its stroke: its first columns
    // its height and no more (#373 hill climb, H21: a 3 px shoulder where it
    // turned from its first climb weighed on the start).
    for (int x = start; x < start + 3; ++x) {
        const double tall = columnOf(traces.back(), x).sum;
        QVERIFY2(std::abs(tall - heights.back()) <= 0.05, qPrintable(QStringLiteral("the harmonic's column %1: %2 px tall").arg(x).arg(tall)));
    }
    // At 16 px the fundamental starts at 2 px, a clear row under the
    // harmonic's start, for two columns, and from its 4th column on it is
    // 3 px, its height, or a half row more where an edge climbs, to the
    // tile's edge, with a row of the tile under it (#373 hill climb, H23: at
    // 2 px all along the amber read lighter than the magenta over it; H22: a
    // block three rows tall at its start).
    if (size == 16) {
        for (int x = start; x < start + 2; ++x) {
            const double tall = columnOf(traces.front(), x).sum;
            QVERIFY2(std::abs(tall - 2) <= 0.05, qPrintable(QStringLiteral("the fundamental's column %1: %2 px tall").arg(x).arg(tall)));
        }
        for (int x = start + 3; x <= inside.right(); ++x) {
            const Column column = columnOf(traces.front(), x);
            QVERIFY2(column.sum >= 2.95 && column.sum <= 3.55 && column.bottom < inside.bottom(),
                     qPrintable(QStringLiteral("the fundamental's column %1: %2 px tall, to row %3").arg(x).arg(column.sum).arg(column.bottom)));
        }
    }
}

void AppIconTest::hintedTracesKeepClearOfEachOtherAndTheRim_data() {
    hintedSizesAreClosedShapesOnThePixelGrid_data();
}

// On the pixel grid a clear device pixel or more between the traces
// everywhere, more towards the end as they fan out, and the tile around
// them: a clear row of it above the top trace and below the lowest, and a
// clear column before their start (#373 hill climb, H19: at 16 px the amber
// start sat on the rim). The fundamental's start lifted off the rim at 16 px
// left a row between the two strokes there.
void AppIconTest::hintedTracesKeepClearOfEachOtherAndTheRim() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);
    const QRect inside = insideRim(tileOf(render(path, size)));
    const QHash<QString, QImage> cover = traceCoverage(path, size);
    const QStringList ids = traceIds(size);
    std::vector<QImage> traces;
    for (auto id = ids.crbegin(); id != ids.crend(); ++id) {
        traces.push_back(cover.value(*id));
    }
    const auto [first, last] = columnsOf(traces.front());

    // In every column a clear row between neighbours, never a row fewer than
    // in the column before, and at the end twice as many as at the start or
    // more, and three rows more.
    for (size_t n = 1; n < traces.size(); ++n) {
        int firstGap = -1;
        int gap = -1;
        for (int x = first; x <= last; ++x) {
            const int between = columnOf(traces[n - 1], x).top - columnOf(traces[n], x).bottom - 1;
            QVERIFY2(between >= 1, qPrintable(QStringLiteral("trace %1, column %2: %3 clear rows").arg(n).arg(x).arg(between)));
            QVERIFY2(gap < 0 || between >= gap - 1,
                     qPrintable(QStringLiteral("trace %1, column %2: %3 clear rows after %4").arg(n).arg(x).arg(between).arg(gap)));
            firstGap = firstGap < 0 ? between : firstGap;
            gap = between;
        }
        QVERIFY2(gap >= 2 * firstGap && gap >= firstGap + 3,
                 qPrintable(QStringLiteral("trace %1: %2 clear rows, then %3").arg(n).arg(firstGap).arg(gap)));
    }

    // The nearest two neighbours come, from any pixel one touches to any the
    // other does, across columns too: a device pixel or more, even at an
    // antialiased corner.
    for (size_t n = 1; n < traces.size(); ++n) {
        double nearest = size;
        for (int y = inside.top(); y <= inside.bottom(); ++y) {
            for (int x = inside.left(); x <= inside.right(); ++x) {
                if (alphaAt(traces[n], x, y) <= kTouched) {
                    continue;
                }
                for (int v = inside.top(); v <= inside.bottom(); ++v) {
                    for (int u = inside.left(); u <= inside.right(); ++u) {
                        if (alphaAt(traces[n - 1], u, v) > kTouched) {
                            nearest = std::min(nearest, std::hypot(std::max(0, std::abs(x - u) - 1),
                                                                   std::max(0, std::abs(y - v) - 1)));
                        }
                    }
                }
            }
        }
        QVERIFY2(nearest >= 1.0, qPrintable(QStringLiteral("trace %1: %2 px from the one below").arg(n).arg(nearest)));
    }

    // The tile around them: a clear row above and below, inside the rim,
    // and a clear column before the start.
    int top = size;
    int bottom = -1;
    for (const QImage &trace : traces) {
        for (int x = first; x <= last; ++x) {
            const Column column = columnOf(trace, x);
            top = std::min(top, column.top);
            bottom = std::max(bottom, column.bottom);
        }
    }
    QVERIFY2(top > inside.top() && bottom < inside.bottom() && first > inside.left(),
             qPrintable(QStringLiteral("rows %1 to %2 from column %3 inside %4 to %5, from %6")
                            .arg(top).arg(bottom).arg(first).arg(inside.top()).arg(inside.bottom()).arg(inside.left())));
}

void AppIconTest::tracesRunOutFlushToTheTilesInnerEdge_data() {
    addSizeRows();
}

// The traces run out to the tile's inner edge on the right, cut flush on it
// with the rim intact, so that the tile reads as a crop of an endless
// spectrogram (#373 hill climb, H19: three series ending at a common x in
// round caps inside the tile read as a line chart). Each trace's last column
// is the one inside the rim, with no cap there: on the pixel grid whole
// pixels of the stroke's height, and from 64 px as covered as the column
// before it. Nothing of a trace is under the rim.
void AppIconTest::tracesRunOutFlushToTheTilesInnerEdge() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);
    const QRect tile = tileOf(render(path, size));
    const QRect inside = insideRim(tile);
    const QHash<QString, QImage> cover = traceCoverage(path, size);
    QCOMPARE(cover.size(), static_cast<qsizetype>(traceIds(size).size()));
    // The 4th harmonic runs out through the top inner edge instead
    // (detailTierAddsAFaint4thHarmonic).
    const QStringList ids = seriesIds(size);
    for (qsizetype n = 0; n < ids.size(); ++n) {
        const QString &id = ids[n];
        const QImage &trace = cover.value(id);
        const auto [first, last] = columnsOf(trace);
        QVERIFY2(last == inside.right(), qPrintable(QStringLiteral("%1 ends at column %2").arg(id).arg(last)));
        const Column end = columnOf(trace, last);
        if (onPixelGrid(size)) {
            const int height = strokeHeights()[static_cast<size_t>(ids.size() - 1 - n)];
            QVERIFY2(end.bottom - end.top + 1 == height && std::abs(end.sum - height) <= 0.05,
                     qPrintable(QStringLiteral("%1: %2 px at its end").arg(id).arg(end.sum)));
        } else {
            const double before = columnOf(trace, last - 1).sum;
            QVERIFY2(end.sum >= 0.9 * before,
                     qPrintable(QStringLiteral("%1: %2 px at its end, %3 before").arg(id).arg(end.sum).arg(before)));
        }
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                QVERIFY2(inside.contains(x, y) || alphaAt(trace, x, y) <= 0.01,
                         qPrintable(QStringLiteral("%1 at %2, %3").arg(id).arg(x).arg(y)));
            }
        }
        // It starts in the tile's left fifth, clear of the rim.
        QVERIFY2(first > inside.left() && first <= tile.left() + tile.width() / 5,
                 qPrintable(QStringLiteral("%1 starts at column %2").arg(id).arg(first)));
    }
}

void AppIconTest::largeSizesTaperFromARoundStart_data() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        if (!onPixelGrid(size)) {
            QTest::addRow("%dpx", size) << size;
        }
    }
}

// From 32 px the traces are smooth (#373 hill climb, H10 from 64 px, H20
// from 32 px): strokes tapering from the fundamental up (strokeWidths), as a
// harmonic's energy falls with its number, drawn as filled outlines with a
// round cap at the start, where H9's flat cut read as clipped, and none at
// the end, where they run out to the tile's edge (H19).
void AppIconTest::largeSizesTaperFromARoundStart() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);

    // The file: each trace a filled outline, closed, with one arc, its start's
    // round cap.
    QXmlStreamReader svg(contentsOf(path));
    QStringList ids;
    while (!svg.atEnd()) {
        if (svg.readNext() != QXmlStreamReader::StartElement || svg.name() != QLatin1String("path")) {
            continue;
        }
        const QXmlStreamAttributes attributes = svg.attributes();
        ids << attributes.value(QLatin1String("id")).toString();
        QVERIFY2(!attributes.hasAttribute(QLatin1String("stroke")), qPrintable(ids.last()));
        QVERIFY(attributes.value(QLatin1String("fill")) != QLatin1String("none"));
        const QString d = attributes.value(QLatin1String("d")).toString();
        QCOMPARE(d.count(QLatin1Char('A')), 1);
        QVERIFY(d.endsWith(QLatin1Char('Z')));
    }
    QVERIFY2(!svg.hasError(), qPrintable(svg.errorString()));
    QCOMPARE(ids, traceIds(size));

    const QImage image = render(path, size);
    const QRect inside = insideRim(tileOf(image));
    const QHash<QString, QImage> cover = traceCoverage(path, size);
    std::vector<QImage> traces;
    for (auto id = ids.crbegin(); id != ids.crend(); ++id) {
        traces.push_back(cover.value(*id));
    }
    QCOMPARE(tracesOf(image).size(), static_cast<size_t>(hasDetailTier(size) ? 4 : 3));

    // A round start, at 256 px, where a cap is pixels enough across to
    // tell: a trace's first column holds the tip of its cap and the column
    // after it less than the stroke's height, both short of a column of the
    // stroke's body just past the cap. A cut, as in H9, has the stroke's
    // whole height in the column after its end.
    if (size == 256) {
        for (size_t n = 0; n < traces.size(); ++n) {
            const int tip = columnsOf(traces[n]).first;
            const double body = columnOf(traces[n], tip + 2 * capColumns(size)).sum;
            const double last = columnOf(traces[n], tip).sum;
            const double next = columnOf(traces[n], tip + 1).sum;
            QVERIFY2(last < 0.6 * body && next < 0.85 * body,
                     qPrintable(QStringLiteral("trace %1 at column %2: %3, then %4, against %5")
                                    .arg(n).arg(tip).arg(last).arg(next).arg(body)));
        }
    }

    // The widths as drawn, across each stroke, over the columns every trace
    // crosses at its full width.
    const int start = columnsOf(traces.front()).first + capColumns(size);
    const int end = inside.right() - 1;
    const std::vector<double> expected = strokeWidths(size);
    for (size_t n = 0; n < expected.size(); ++n) {
        const double width = widthAcross(traces[n], start, end);
        QVERIFY2(std::abs(width - expected[n]) <= 0.08 * expected[n],
                 qPrintable(QStringLiteral("trace %1: %2 px across, not %3").arg(n).arg(width).arg(expected[n])));
    }
}

void AppIconTest::smallCurvesAreEvenAndEndOnWholeRows_data() {
    QTest::addColumn<int>("size");
    QTest::addRow("32px") << 32;
    QTest::addRow("36px") << 36;
    QTest::addRow("48px") << 48;
}

// At 32 and 48 px the traces are antialiased curves, the 64-unit drawing on
// their grid (#373 hill climb, H20: H19 drew them as whole-pixel stairs,
// which read as aliased and beaded beside Breeze's smooth icons): no stair
// runs, an even weight along each stroke, never under 2 px across, apart
// from each other everywhere, and the ends cut flush on the tile's inner
// edge on whole rows. H21: on the 64 px composition, the magenta's end 5 px
// under the top inner edge at 48 px and 4 at 32 (H20: 3 and 2, with the
// 32 px end under the corner, where Plasma's badge cuts it), the ends
// 1 : 2 : 3 above the baseline, from starts as far apart. H25: at 32 and
// 36 px the fundamental the focal stroke, 4 px, its weight on its underside,
// and the fan opened, the caps' tips in one column (H24: 3 / 2.5 / 2, exits
// 1.6 and 1.8 times as far apart as the starts, read as three near-parallel
// stripes). H26: the magenta's exit 3 rows under the top inner edge and the
// rose's a row lower, so that the top trace keeps 2 px of the tile over it
// up to the rounded corner, where Plasma's playing badge sits (H25's, a row
// under the edge, ran into the corner); the exits some 1.75 times as far
// apart as the starts, from starts a little closer, the traces as far apart
// as in H25. 36 px a drawing of its own, the same strokes on its grid
// (H24); and 1.5 px or more of the tile between neighbours everywhere,
// 2.25 px at 30 % of the way across.
void AppIconTest::smallCurvesAreEvenAndEndOnWholeRows() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);
    const QRect inside = insideRim(tileOf(render(path, size)));
    const QHash<QString, QImage> cover = traceCoverage(path, size);
    const QStringList ids = traceIds(size);
    // Each trace's end, its top and bottom on the inner edge, lowest first.
    using Ends = std::vector<std::pair<double, double>>;
    const Ends ends = size == 48 ? Ends{{30, 34}, {20, 22.5}, {10, 12}}
                                 : (size == 36 ? Ends{{22, 26}, {15, 17}, {7, 9}} : Ends{{21, 24}, {13, 15}, {6, 8}});
    // The start caps' centres, lowest first: at 48 px a pixel above H20's,
    // the fundamental's half a pixel lower, its added weight on its underside.
    const std::vector<double> starts = size == 48 ? std::vector<double>{37.25, 31.75, 26.75}
                                                  : (size == 36 ? std::vector<double>{27.75, 23.0, 18.8}
                                                                : std::vector<double>{25.1, 20.45, 16.3});
    const std::vector<double> widths = strokeWidths(size);
    const QString source = QString::fromUtf8(contentsOf(path));
    std::vector<double> startCentres;
    std::vector<QImage> traces;
    for (auto id = ids.crbegin(); id != ids.crend(); ++id) {
        traces.push_back(cover.value(*id));
    }
    for (size_t n = 0; n < traces.size(); ++n) {
        const QImage &trace = traces[n];
        const QString &id = ids[static_cast<qsizetype>(ids.size() - 1 - n)];
        // The file: the outline's end on the tile's inner edge, x = 43 at
        // 48 px and 29 at 32, from the end's top row to its bottom.
        const QString end = QStringLiteral("%1 %2 L%1 %3")
                                .arg(inside.right() + 1)
                                .arg(ends[n].first)
                                .arg(ends[n].second);
        QVERIFY2(source.contains(end), qPrintable(QStringLiteral("%1: no '%2'").arg(id, end)));
        // Its round start: the cap's arc from the stroke's lower edge back
        // to its upper one, centred on the start.
        static const QRegularExpression cap(QStringLiteral("d=\"M([0-9.]+) ([0-9.]+) .* ([0-9.]+) ([0-9.]+) A[0-9.]+ [0-9.]+ 0 0 1 \\1 \\2Z\""));
        const QRegularExpressionMatch arc = cap.match(source.section(QStringLiteral("<path id=\"%1\"").arg(id), 1, 1).section(QLatin1Char('\n'), 0, 0));
        QVERIFY2(arc.hasMatch(), qPrintable(id));
        const double startCentre = (arc.captured(2).toDouble() + arc.captured(4).toDouble()) / 2;
        QVERIFY2(std::abs(startCentre - starts[n]) <= 0.01, qPrintable(QStringLiteral("%1 starts at %2").arg(id).arg(startCentre)));
        startCentres.push_back(startCentre);

        // Rendered, its last column covers the end's rows and nothing else,
        // to within the slope's change across the column.
        const auto [first, last] = columnsOf(trace);
        QCOMPARE(last, inside.right());
        for (int y = inside.top(); y <= inside.bottom(); ++y) {
            const double expected = std::clamp(std::min<double>(y + 1, ends[n].second) - std::max<double>(y, ends[n].first), 0.0, 1.0);
            QVERIFY2(std::abs(alphaAt(trace, last, y) - expected) <= 0.3,
                     qPrintable(QStringLiteral("%1 at its end, row %2: %3, not %4").arg(id).arg(y).arg(alphaAt(trace, last, y)).arg(expected)));
        }

        // A curve: its centre moves on smoothly from column to column, with
        // no run of level columns broken by a whole row's step, as a stair
        // has; and it never falls. Its body from the first column past the
        // fundamental's cap and its own.
        const double capCentre = (arc.captured(1).toDouble() + arc.captured(3).toDouble()) / 2;
        const double capRadius = std::hypot(arc.captured(1).toDouble() - arc.captured(3).toDouble(),
                                            arc.captured(2).toDouble() - arc.captured(4).toDouble()) / 2;
        const int body = std::max(first + capColumns(size), static_cast<int>(std::ceil(capCentre + capRadius)));
        for (int x = body + 1; x < last; ++x) {
            const double bend = columnOf(trace, x + 1).centre - 2 * columnOf(trace, x).centre + columnOf(trace, x - 1).centre;
            QVERIFY2(std::abs(bend) <= 0.3, qPrintable(QStringLiteral("%1, column %2: bends %3").arg(id).arg(x).arg(bend)));
            QVERIFY2(columnOf(trace, x).centre <= columnOf(trace, x - 1).centre + 0.01,
                     qPrintable(QStringLiteral("%1, column %2: falls").arg(id).arg(x)));
        }
        // An even weight: across its slope, every column of it past the cap
        // is within an eighth of its width as drawn there (easedWidth), which
        // is 2 px or more; no beads. At 48 px the top trace from its start's
        // 2.25 px to its end's 2, never wider than the column before by more
        // than a rendering's few hundredths, and its start's width to x 38
        // (topTraceWidths). Every column of every trace past the caps has a
        // pixel of its whole colour, 0.9 of it or more: no dip in its
        // contrast along its run.
        QVERIFY(widths[n] >= 2.0);
        const bool top = n + 1 == traces.size() && size == 48;
        const auto [startWidth, endWidth] = topTraceWidths();
        double before = startWidth + 0.15;
        for (int x = body; x < last; ++x) {
            double peak = 0;
            for (int y = inside.top(); y <= inside.bottom(); ++y) {
                peak = std::max(peak, alphaAt(trace, x, y));
            }
            QVERIFY2(peak >= 0.9, qPrintable(QStringLiteral("%1, column %2: %3 of a pixel at most").arg(id).arg(x).arg(peak)));
            const double across = widthAcross(trace, x, x);
            if (top) {
                QVERIFY2(across >= endWidth - 0.1 && across <= before + 0.05 && (x + 1 > kTopTraceHeldTo || across >= startWidth - 0.1),
                         qPrintable(QStringLiteral("%1, column %2: %3 px across after %4").arg(id).arg(x).arg(across).arg(before)));
                before = across;
                continue;
            }
            const double wanted = easedWidth(size, n, x + 0.5, inside.right() + 1);
            QVERIFY2(std::abs(across - wanted) <= wanted / 8,
                     qPrintable(QStringLiteral("%1, column %2: %3 px across, not %4").arg(id).arg(x).arg(across).arg(wanted)));
        }
    }

    // Apart everywhere: no pixel that two traces touch.
    for (int y = inside.top(); y <= inside.bottom(); ++y) {
        for (int x = inside.left(); x <= inside.right(); ++x) {
            int touching = 0;
            for (const QImage &trace : traces) {
                touching += alphaAt(trace, x, y) > kTouched ? 1 : 0;
            }
            QVERIFY2(touching <= 1, qPrintable(QStringLiteral("traces meet at %1, %2").arg(x).arg(y)));
        }
    }
    // The fan opens: the exits' centres 1.7 times or more as far apart as
    // the starts' at 32 and 36 px, and twice at 48.
    const double exits = (ends.front().first + ends.front().second - ends.back().first - ends.back().second) / 2;
    const double fan = exits / (startCentres.front() - startCentres.back());
    QVERIFY2(fan >= (size == 48 ? 2.0 : 1.7), qPrintable(QString::number(fan)));
    // The tile clear above the top trace: 5 rows at 48 px, 3 at 32 and 36
    // (#373 hill climb, H26: 1 before, and in its last column, under the
    // tile's rounded corner, none: the trace ran into the corner where
    // Plasma's playing badge sits).
    for (int y = inside.top(); y < inside.top() + (size == 48 ? 5 : 3); ++y) {
        for (int x = inside.left(); x <= inside.right(); ++x) {
            QVERIFY2(alphaAt(traces.back(), x, y) <= 0.01, qPrintable(QStringLiteral("%1, %2").arg(x).arg(y)));
        }
    }
    // In each of the top trace's last 4 columns, 2 px or more of the tile
    // over it, the rounded corner's rim counted out.
    if (taskManagerSize(size)) {
        const QImage tile = tileCoverage(path, size);
        for (int x = inside.right() - 3; x <= inside.right(); ++x) {
            double air = 0;
            for (int y = 0; y <= columnOf(traces.back(), x).top; ++y) {
                air += std::max(0.0, alphaAt(tile, x, y) - alphaAt(traces.back(), x, y));
            }
            QVERIFY2(air >= 2.0, qPrintable(QStringLiteral("column %1: %2 px of the tile over the top trace").arg(x).arg(air)));
        }
    }
    // Apart by the tile: in every column both cross past their caps' first
    // columns, 1.5 px or more of it between their centres, from what each
    // leaves of a row, and 2.25 px at 30 % of the way across (H20: at 32 px
    // the rose came within a row of the amber there).
    const int thirty = inside.left() + inside.width() * 3 / 10;
    for (size_t n = 1; n < traces.size(); ++n) {
        const int from = std::max(columnsOf(traces[n - 1]).first, columnsOf(traces[n]).first) + 1;
        for (int x = from; x < inside.right(); ++x) {
            const int upper = static_cast<int>(columnOf(traces[n], x).centre);
            const int lower = static_cast<int>(columnOf(traces[n - 1], x).centre);
            double clear = 0;
            for (int y = upper; y <= lower; ++y) {
                clear += std::max(0.0, 1 - alphaAt(traces[n], x, y) - alphaAt(traces[n - 1], x, y));
            }
            QVERIFY2(clear >= (x == thirty ? 2.25 : 1.5),
                     qPrintable(QStringLiteral("traces %1 and %2, column %3: %4 px of the tile between").arg(n - 1).arg(n).arg(x).arg(clear)));
        }
    }
}

// The ink thins smoothly as the icon grows: the fundamental's width across,
// a share of the tile inside the rim, falls from each size to the next
// (#373 hill climb, H19: at 48 px it dipped below 64 px's, and 32 px read
// bolder than 48). From 22 px: at 16 px the grid's 2 px is the least a
// stroke can be, and 22 px's 3 px is a larger share (H21). A size drawn as a
// smaller size's drawing, moved in, has that one's (sizesDrawnAsASmallerSize).
void AppIconTest::fundamentalThinsSmoothlyWithSize() {
    double before = -1;
    for (const int size : FerrousAppIcon::kSizes) {
        if (size == 16 || drawnAs(size) != size) {
            continue;
        }
        const QString path = FerrousAppIcon::resourcePath(size);
        const QRect inside = insideRim(tileOf(render(path, size)));
        const QImage fundamental = traceCoverage(path, size).value(QStringLiteral("fundamental"));
        const auto [first, last] = columnsOf(fundamental);
        const double share = widthAcross(fundamental, first + capColumns(size), last - 1) / inside.width();
        QVERIFY2(before < 0 || (share < before && share > before * 0.6),
                 qPrintable(QStringLiteral("%1 px: %2 of the tile after %3").arg(size).arg(share).arg(before)));
        before = share;
    }
}

void AppIconTest::fundamentalLeadsTheHarmonics_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        if (size >= 32) {
            QTest::addRow("%dpx", size) << FerrousAppIcon::resourcePath(size) << size;
        }
    }
    for (const int size : {32, 36, 48, 64}) {
        QTest::addRow("%dpx@2", size) << doubledPath(size) << 2 * size;
    }
    QTest::addRow("scalable") << FerrousAppIcon::resourcePath(0) << 64;
}

// The fundamental is the focal stroke, as it is at 16 and 22 px: half as
// heavy again as the rose over it, or more, and three quarters again as the
// magenta, across their strokes over the columns all three cross at their
// full widths (#373 hill climb, H25: from 32 px they tapered 1 : 0.83 : 0.67
// or 1 : 0.8 : 0.65 and read as three near-equal stripes).
void AppIconTest::fundamentalLeadsTheHarmonics() {
    QFETCH(QString, path);
    QFETCH(int, size);
    const QHash<QString, QImage> cover = traceCoverage(path, size);
    const QImage &fundamental = cover.value(QStringLiteral("fundamental"));
    const QImage &second = cover.value(QStringLiteral("h2"));
    const QImage &third = cover.value(QStringLiteral("h3"));
    const auto [first, last] = columnsOf(fundamental);
    const int start = first + static_cast<int>(std::ceil(widthAcross(fundamental, last - 2, last - 1)));
    const double heavy = widthAcross(fundamental, start, last - 1);
    const double rose = widthAcross(second, start, last - 1);
    const double magenta = widthAcross(third, start, last - 1);
    QVERIFY2(heavy >= 1.5 * rose && heavy >= 1.75 * magenta,
             qPrintable(QStringLiteral("%1, %2 and %3 px across").arg(heavy).arg(rose).arg(magenta)));
}

void AppIconTest::oneMagentaAtEverySize_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        QTest::addRow("%dpx", size) << FerrousAppIcon::resourcePath(size) << size;
    }
    for (const int size : kDoubledSizes) {
        QTest::addRow("%dpx@2", size) << doubledPath(size) << 2 * size;
    }
    QTest::addRow("scalable") << FerrousAppIcon::resourcePath(0) << 48;
}

// The 3rd harmonic is one magenta in every file (#373 hill climb, H23: H22
// deepened it at 32 to 48 px and in their 2x drawings, so that it held apart
// from the rose in greyscale, and there the top trace was the thinnest and
// dimmest trace, 2.6:1 as drawn, its hue changing from 48 to 64 px). The
// rose is a step lighter instead, its hue kept: in Rec. 709 luma at their
// full colours some 30 levels over the magenta and as many under the amber
// (H22: 21 over the magenta), and nothing of H22's colours in any file.
// Rendered, down the middle column, each trace's centre is that far apart
// in grey, to within what a 2 px stroke's centre row loses to the tile at
// 32 px, and the magenta's 3:1 or more on the tile there.
void AppIconTest::oneMagentaAtEverySize() {
    QFETCH(QString, path);
    QFETCH(int, size);
    const auto luma = [](QRgb c) { return 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c); };
    QVERIFY2(luma(kHarmonic) - luma(kThirdHarmonic) >= 30, qPrintable(QString::number(luma(kHarmonic) - luma(kThirdHarmonic))));
    QVERIFY2(luma(kGlide) - luma(kHarmonic) >= 28, qPrintable(QString::number(luma(kGlide) - luma(kHarmonic))));
    QVERIFY(luma(kDarkerHarmonic) - luma(kThirdHarmonic) < 25);
    // The lighter rose keeps the rose's hue, a few degrees off at most.
    QVERIFY(std::abs(QColor(kHarmonic).hue() - QColor(kDarkerHarmonic).hue()) <= 3);
    const QString source = QString::fromUtf8(contentsOf(path));
    const auto fill = [](QRgb colour) { return QStringLiteral("fill=\"#%1\"").arg(colour & 0xffffff, 6, 16, QLatin1Char('0')); };
    const QString magenta = QStringLiteral("<path id=\"h3\" ") + fill(kThirdHarmonic);
    QVERIFY2(source.contains(magenta), qPrintable(magenta));
    QVERIFY(!source.contains(fill(kDeepThirdHarmonic)));
    QVERIFY(!source.contains(fill(kDarkerHarmonic)));
    // Rendered where there are three traces: from 32 px, and in the 2x
    // drawings from the 32 px slot's.
    if (!source.contains(QLatin1String("<path id=\"h2\""))) {
        QVERIFY(size < 32 || path.contains(QLatin1String("@2")));
        return;
    }
    QVERIFY(source.contains(QStringLiteral("<path id=\"h2\" ") + fill(kHarmonic)));
    const QImage image = renderSeries(path, size);
    const int x = size / 2;
    std::vector<QRgb> centres;
    for (int y = 0; y < size;) {
        if (!traceInk(image.pixel(x, y))) {
            ++y;
            continue;
        }
        const int top = y;
        while (y < size && traceInk(image.pixel(x, y))) {
            ++y;
        }
        centres.push_back(image.pixel(x, (top + y) / 2));
    }
    QCOMPARE(centres.size(), static_cast<size_t>(3));
    const double magentaToRose = luma(centres[1]) - luma(centres[0]);
    const double roseToAmber = luma(centres[2]) - luma(centres[1]);
    QVERIFY2(magentaToRose >= 24 && roseToAmber >= 28,
             qPrintable(QStringLiteral("%1, %2, %3").arg(luma(centres[0])).arg(luma(centres[1])).arg(luma(centres[2]))));
    QVERIFY2(contrastRatio(centres[0], kTile) >= 3.0, qPrintable(QString::number(contrastRatio(centres[0], kTile))));
}

void AppIconTest::topTraceClearsTheCornerFrom64px_data() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        if (size >= 64) {
            QTest::addRow("%dpx", size) << size;
        }
    }
}

// From 64 px the traces' ends sit 0.94 as high above the 64-unit drawing's
// baseline, y 55.5, as in H19, their starts where they were, so that the top
// trace runs out to the tile's edge some 3 units below its rounded corner
// rather than into it (#373 hill climb, H20): at the edge their centres are
// at 55.5 - 0.94 k 14.17 units, 1 : 2 : 3 above the baseline (the
// fundamental's lower by its fundamentalUnderside), and the top
// trace's ink starts 2.5 units or more below the corner's arc, which ends 4
// units below the tile's inner edge.
void AppIconTest::topTraceClearsTheCornerFrom64px() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);
    const double unit = size / 64.0;
    const QRect inside = insideRim(tileOf(render(path, size)));
    const QHash<QString, QImage> cover = traceCoverage(path, size);
    const QStringList ids = seriesIds(size);
    for (qsizetype n = 0; n < ids.size(); ++n) {
        const int k = static_cast<int>(ids.size() - n);
        const QImage &trace = cover.value(ids[n]);
        const Column end = columnOf(trace, inside.right());
        const double centre = end.centre / unit;
        const double expected = 55.5 - 0.94 * k * (55.5 - 13) / 3 + (k == 1 ? fundamentalUnderside(size) : 0);
        QVERIFY2(std::abs(centre - expected) <= 0.4,
                 qPrintable(QStringLiteral("%1 ends at %2 units, not %3").arg(ids[n]).arg(centre).arg(expected)));
    }
    const Column top = columnOf(cover.value(QStringLiteral("h3")), inside.right());
    const double cornerEnd = inside.top() + 4 * unit;
    QVERIFY2(top.top >= cornerEnd + 2.5 * unit,
             qPrintable(QStringLiteral("the top trace from row %1, the corner to %2").arg(top.top).arg(cornerEnd)));
}

void AppIconTest::detailTierAddsAFaint4thHarmonic_data() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        if (!onPixelGrid(size)) {
            QTest::addRow("%dpx", size) << size;
        }
    }
    // The scalable drawing, for the sizes between.
    QTest::addRow("scalable") << 96;
}

// At 128 and 256 px a detail tier (#373 hill climb, H21, P29 amended: the 64
// px drawing enlarged left the upper left of the tile empty beside Breeze's
// large icons): the faint 4th harmonic of the same glide, in violet, half
// the width the harmonics taper from (fourthWidth), from the common start, a
// true 4th harmonic: as far above the 3rd as the 3rd is above the 2nd in
// every column, rising 4/3 as far as the 3rd, flattening as the others do
// (#373 hill climb, H26: H25 bent its last stretch up to leave at 40
// degrees, the only reversed curvature, and spaced it wider than the series),
// and cropped by the tile's top inner edge as the others are by its right
// one, where the series puts it: from where its upper edge meets that edge,
// x 44 or more of the 64-unit drawing, its lower edge runs on along its curve
// to the same row, by 50, and the cut runs along the edge between them (H23:
// H22 cut it square across the stroke, a corner on the rim with a wedge of
// the tile under the cut), so that nothing of it is in the tile's rounded
// corner, whose arc starts at x 53, where Plasma draws a task's badge (x over
// 51, y under 20). Below 128 px, and in the scalable drawing, three traces.
void AppIconTest::detailTierAddsAFaint4thHarmonic() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);
    const QString source = QString::fromUtf8(contentsOf(path));
    QCOMPARE(source.contains(QLatin1String("<path id=\"h4\"")), hasDetailTier(size));
    if (!hasDetailTier(size)) {
        return;
    }
    QVERIFY(source.contains(QLatin1String("<path id=\"h4\" fill=\"#ab55df\"")));
    // Faint: a step of value below the 3rd harmonic, as each trace is below
    // the one under it, and fainter than any of the three, 3:1 on the tile
    // (#373 hill climb, H25: H24's 2.9:1 had little reserve), 8 grey levels or
    // more under the magenta in Rec. 709 luma. Yet light enough that its cut
    // on the top inner edge reads against the rim, 1.7:1 or more on it (H24:
    // H21's #9a4bd0, 2.55:1 on the tile, was 1.55:1 on the rim, and the cut
    // fused with it); the hue kept.
    const double ratio = contrastRatio(kFourthHarmonic, kTile);
    QVERIFY2(ratio >= 3.0 && ratio <= 3.1, qPrintable(QString::number(ratio)));
    QVERIFY(contrastRatio(kH24FourthHarmonic, kTile) < 3.0);
    QVERIFY(!source.contains(QLatin1String("#a456d6")));
    QVERIFY(contrastRatio(kThirdHarmonic, kTile) >= ratio + 0.6);
    const auto luma = [](QRgb c) { return 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c); };
    QVERIFY2(luma(kThirdHarmonic) - luma(kFourthHarmonic) >= 8, qPrintable(QString::number(luma(kFourthHarmonic))));
    QVERIFY2(contrastRatio(kFourthHarmonic, kRim) >= 1.7, qPrintable(QString::number(contrastRatio(kFourthHarmonic, kRim))));
    QVERIFY(contrastRatio(kDarkerFourthHarmonic, kRim) < 1.6);
    QVERIFY(std::abs(QColor(kFourthHarmonic).hue() - QColor(kDarkerFourthHarmonic).hue()) <= 3);
    QVERIFY(std::abs(QColor(kFourthHarmonic).hue() - QColor(kH24FourthHarmonic).hue()) <= 3);
    QVERIFY(!source.contains(QLatin1String("#9a4bd0")));

    const double unit = size / 64.0;
    const QRect inside = insideRim(tileOf(render(path, size)));
    const QHash<QString, QImage> cover = traceCoverage(path, size);
    const QImage &fourth = cover.value(QStringLiteral("h4"));
    const QImage &third = cover.value(QStringLiteral("h3"));
    const QImage &second = cover.value(QStringLiteral("h2"));
    const QImage &fundamental = cover.value(QStringLiteral("fundamental"));
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            QVERIFY2(inside.contains(x, y) || alphaAt(fourth, x, y) <= 0.01, qPrintable(QStringLiteral("%1, %2").arg(x).arg(y)));
        }
    }

    // From the common start, its round cap within the others' slack.
    const auto [first, last] = columnsOf(fourth);
    QVERIFY2(std::abs(first - columnsOf(fundamental).first) <= 1 + size / 64,
             qPrintable(QStringLiteral("starts at %1, the fundamental at %2").arg(first).arg(columnsOf(fundamental).first)));
    const int start = std::max(first, columnsOf(fundamental).first) + capColumns(size);
    // On the harmonic series there: as far above the 3rd harmonic as the
    // 3rd is above the 2nd, to within a unit.
    const auto centre = [](const QImage &trace, int x) { return columnOf(trace, x).centre; };
    const double step = centre(second, start) - centre(third, start);
    const double above = centre(third, start) - centre(fourth, start);
    QVERIFY2(std::abs(above - step) <= unit, qPrintable(QStringLiteral("%1 above the 3rd, which is %2 above the 2nd").arg(above).arg(step)));

    // Cropped by the top inner edge: in the file, the outline's end runs
    // from its upper edge's end on the edge, along the edge, to its lower
    // edge's end, as far on as the stroke's width across the edge's slant
    // there, to within a tenth.
    static const QRegularExpression endCut(QStringLiteral(
        "<path id=\"h4\"[^>]* C[0-9.]+ [0-9.]+ ([0-9.]+) ([0-9.]+) ([0-9.]+) ([0-9.]+) L([0-9.]+) ([0-9.]+) C"));
    const QRegularExpressionMatch cut = endCut.match(source);
    QVERIFY(cut.hasMatch());
    const QPointF control(cut.captured(1).toDouble(), cut.captured(2).toDouble());
    const QPointF upper(cut.captured(3).toDouble(), cut.captured(4).toDouble());
    const QPointF lower(cut.captured(5).toDouble(), cut.captured(6).toDouble());
    // The tile inside a rim of one device pixel.
    const double edge = 6 + 64.0 / size;
    QVERIFY2(std::abs(upper.y() - edge) <= 0.001 && std::abs(lower.y() - edge) <= 0.001,
             qPrintable(QStringLiteral("%1, %2").arg(upper.y()).arg(lower.y())));
    // Where the series puts it, clear of the corner: the cut from x 44 to 50
    // (88 to 100 of 128 px).
    QVERIFY2(upper.x() >= 44 && lower.x() <= 50, qPrintable(QStringLiteral("cut from %1 to %2 units").arg(upper.x()).arg(lower.x())));
    const QPointF along = upper - control;
    const double slant = std::abs(along.y()) / std::hypot(along.x(), along.y());
    // At the series' slope there, some 23 degrees to the edge: not bent up
    // (H25's met it at 40).
    const double angle = qRadiansToDegrees(std::asin(slant));
    QVERIFY2(angle >= 21 && angle <= 26, qPrintable(QStringLiteral("%1 degrees").arg(angle)));
    const double across = fourthWidth(size) / unit;
    QVERIFY2(std::abs((lower.x() - upper.x()) * slant - across) <= 0.1 * across,
             qPrintable(QStringLiteral("a %1 unit cut, a %2 unit stroke").arg(lower.x() - upper.x()).arg(across)));
    // Rendered, it reaches the tile's top row inside the rim in every column
    // the cut crosses, with no wedge of the tile between it and the rim, and
    // runs along it for the cut's length, to within a device pixel.
    for (int x = static_cast<int>(std::ceil(upper.x() * unit)) + 1; x < static_cast<int>(lower.x() * unit) - 1; ++x) {
        QVERIFY2(columnOf(fourth, x).top == inside.top() && alphaAt(fourth, x, inside.top()) >= 0.5,
                 qPrintable(QStringLiteral("column %1: from row %2, %3 of its top row").arg(x).arg(columnOf(fourth, x).top)
                                .arg(alphaAt(fourth, x, inside.top()))));
    }
    double topRow = 0;
    for (int x = inside.left(); x <= inside.right(); ++x) {
        topRow += alphaAt(fourth, x, inside.top());
    }
    QVERIFY2(std::abs(topRow - (lower.x() - upper.x()) * unit) <= 1.0,
             qPrintable(QStringLiteral("%1 px of it in the top row, a %2 px cut").arg(topRow).arg((lower.x() - upper.x()) * unit)));
    // Nothing in the badge's corner.
    for (int y = 0; y < 20 * unit; ++y) {
        for (int x = static_cast<int>(51 * unit); x < size; ++x) {
            QVERIFY2(alphaAt(fourth, x, y) <= kTouched, qPrintable(QStringLiteral("%1, %2").arg(x).arg(y)));
        }
    }

    // 4/3 as steep as the 3rd harmonic: their rises over two thirds of the
    // way from the start to the cut.
    const int cutStart = static_cast<int>(upper.x() * unit);
    const int middle = start + (cutStart - start) * 2 / 3;
    const double rises = (centre(fourth, start) - centre(fourth, middle)) / (centre(third, start) - centre(third, middle));
    QVERIFY2(rises >= 1.3 && rises <= 1.36, qPrintable(QString::number(rises)));
    // On the series in every column up to the cut: as far above the 3rd as
    // the 3rd is above the 2nd, to within a unit.
    for (int x = start; x < cutStart - 1; ++x) {
        const double gap = centre(third, x) - centre(fourth, x);
        const double series = centre(second, x) - centre(third, x);
        QVERIFY2(std::abs(gap - series) <= unit,
                 qPrintable(QStringLiteral("column %1: %2 above the 3rd, which is %3 above the 2nd").arg(x).arg(gap).arg(series)));
    }
    // One flattening curve: over each quarter of the way to the cut it rises
    // less than over the quarter before, and over the last one still rises.
    const int quarter = (cutStart - 1 - start) / 4;
    double before = size;
    for (int q = 0; q < 4; ++q) {
        const double rise = centre(fourth, start + q * quarter) - centre(fourth, start + (q + 1) * quarter);
        QVERIFY2(rise > 0 && rise < before, qPrintable(QStringLiteral("quarter %1: rises %2 after %3").arg(q).arg(rise).arg(before)));
        before = rise;
    }
    // Half the width the harmonics taper from, over the same columns.
    const double width = widthAcross(fourth, start, middle);
    const double wanted = fourthWidth(size);
    QVERIFY2(std::abs(width - wanted) <= 0.12 * wanted, qPrintable(QStringLiteral("%1 px across, not %2").arg(width).arg(wanted)));

    // Clear of the 3rd harmonic everywhere: in every column both cross, a
    // clear device pixel or more between them.
    for (int x = start; x <= last; ++x) {
        const Column over = columnOf(fourth, x);
        const Column under = columnOf(third, x);
        if (over.top >= 0 && under.top >= 0) {
            QVERIFY2(under.top - over.bottom - 1 >= 1, qPrintable(QStringLiteral("column %1: rows %2 and %3").arg(x).arg(over.bottom).arg(under.top)));
        }
    }
}

void AppIconTest::doubledSlotsDrawTheSmallCompositionsAtTwice_data() {
    QTest::addColumn<int>("size");
    QTest::addRow("16px@2") << 16;
    QTest::addRow("22px@2") << 22;
}

// A 16 or 22 px slot at 2x scaling shows the drawing it shows at 1x, doubled
// (#373 hill climb, H20: there the theme fell back to the 32 px drawing, with
// three traces, or the scalable one, so a task manager's icon changed with the
// scale): the same two traces, from the same start to the same end, as smooth
// strokes twice the 1x heights with a round start, in a 2 px rim and twice
// the margin.
void AppIconTest::doubledSlotsDrawTheSmallCompositionsAtTwice() {
    QFETCH(int, size);
    const QString path = doubledPath(size);
    const int doubled = 2 * size;
    QVERIFY2(QFile::exists(path), qPrintable(path));
    QCOMPARE(QImageReader(path).size(), QSize(doubled, doubled));
    const QImage image = render(path, doubled);
    const QImage single = render(FerrousAppIcon::resourcePath(size), size);

    // Twice the 1x tile, its margin and its rim.
    const QRect tile = tileOf(image);
    const QRect singleTile = tileOf(single);
    QCOMPARE(tile, QRect(singleTile.topLeft() * 2, singleTile.size() * 2));
    const QPoint middle = tile.center();
    for (int step = 0; step <= 2; ++step) {
        QCOMPARE(near(image.pixel(middle.x(), tile.bottom() - step), kRim, 2), step < 2);
    }
    const QRect inside = tile.adjusted(2, 2, -2, -2);

    // The 1x drawing's two traces, in its colours and order, each a filled
    // outline of a smooth stroke with one arc, its start's round cap.
    QXmlStreamReader svg(contentsOf(path));
    QStringList ids;
    while (!svg.atEnd()) {
        if (svg.readNext() == QXmlStreamReader::StartElement && svg.name() == QLatin1String("path")) {
            ids << svg.attributes().value(QLatin1String("id")).toString();
            const QString d = svg.attributes().value(QLatin1String("d")).toString();
            QCOMPARE(d.count(QLatin1Char('A')), 1);
            QVERIFY(d.contains(QLatin1Char('C')));
        }
    }
    QCOMPARE(ids, traceIds(size));
    const std::vector<Trace> traces = tracesOf(image);
    const std::vector<Trace> singleTraces = tracesOf(single);
    QCOMPARE(traces.size(), static_cast<size_t>(2));
    QCOMPARE(singleTraces.size(), static_cast<size_t>(2));
    const QHash<QString, QImage> cover = traceCoverage(path, doubled);
    const QHash<QString, QImage> singleCover = traceCoverage(FerrousAppIcon::resourcePath(size), size);
    for (const QString &id : ids) {
        const double wide = 2.0 * strokeHeights()[id == QLatin1String("fundamental") ? 0 : 1];
        const QImage &trace = cover.value(id);
        const QImage &singleTrace = singleCover.value(id);
        const auto [first, last] = columnsOf(trace);
        const auto [singleFirst, singleLast] = columnsOf(singleTrace);
        // From the 1x start column, doubled, to the tile's inner edge.
        QCOMPARE(first, 2 * singleFirst);
        QCOMPARE(last, inside.right());
        QCOMPARE(last + 1, 2 * (singleLast + 1));
        // At the end the 1x end's rows, doubled: cut on whole rows on the
        // inner edge, so that the last column, which the stroke still climbs
        // across, is centred on them to within a few tenths of a pixel.
        const Column end = columnOf(trace, last);
        const Column singleEnd = columnOf(singleTrace, singleLast);
        QVERIFY2(std::abs(end.centre - 2 * singleEnd.centre) <= 0.3,
                 qPrintable(QStringLiteral("%1 ends at %2, the 1x at %3").arg(id).arg(end.centre).arg(singleEnd.centre)));
        QVERIFY2(std::abs(end.sum - wide) <= 0.15, qPrintable(QStringLiteral("%1: %2 px at its end").arg(id).arg(end.sum)));
        // Its centre at the start, past the cap, within a device pixel of the
        // 1x start's, doubled.
        const double start = columnOf(trace, first + 3).centre;
        const double singleStart = columnOf(singleTrace, singleFirst + 1).centre * 2;
        QVERIFY2(std::abs(start - singleStart) <= 1.5,
                 qPrintable(QStringLiteral("%1 starts at %2, the 1x at %3").arg(id).arg(start).arg(singleStart)));
        // Twice the 1x height across, evenly, past the cap, and smooth: no
        // step from column to column. At 16 px the fundamental from its 1x
        // 4th column, doubled, and before it twice the 1x start's 2 px.
        int full = first + static_cast<int>(wide) - 1;
        if (strokeHeightAt(size, 0, 0) != strokeHeights().front() && id == QLatin1String("fundamental")) {
            full = 2 * (singleFirst + 3) + 1;
            const double startWidth = widthAcross(trace, first + 3, first + 3);
            QVERIFY2(std::abs(startWidth - 2.0 * strokeHeightAt(size, 0, 0)) <= 0.5,
                     qPrintable(QStringLiteral("%1 px across at its start").arg(startWidth)));
        }
        for (int x = full; x < last; ++x) {
            const double across = widthAcross(trace, x, x);
            QVERIFY2(std::abs(across - wide) <= 0.5, qPrintable(QStringLiteral("%1, column %2: %3 px across").arg(id).arg(x).arg(across)));
            const double bend = columnOf(trace, x + 1).centre - 2 * columnOf(trace, x).centre + columnOf(trace, x - 1).centre;
            QVERIFY2(std::abs(bend) <= 0.3, qPrintable(QStringLiteral("%1, column %2: bends %3").arg(id).arg(x).arg(bend)));
        }
    }
}

void AppIconTest::doubledTaskManagerSlotsDoubleTheir1xDrawings_data() {
    QTest::addColumn<int>("size");
    QTest::addRow("32px@2") << 32;
    QTest::addRow("36px@2") << 36;
}

// The numbers of an SVG path's data, each with the command it belongs to and
// its place among that command's numbers.
struct PathNumber {
    QChar command;
    int place = 0;
    double value = 0;
};

std::vector<PathNumber> pathNumbers(const QString &data) {
    static const QRegularExpression token(QStringLiteral("[A-Za-z]|-?[0-9]+(?:\\.[0-9]+)?"));
    std::vector<PathNumber> numbers;
    QChar command;
    int place = 0;
    for (const QRegularExpressionMatch &match : token.globalMatch(data)) {
        const QString text = match.captured(0);
        if (text.front().isLetter()) {
            command = text.front();
            place = 0;
            continue;
        }
        numbers.push_back({command, place++, text.toDouble()});
    }
    return numbers;
}

// Each shape of a drawing, in order, by its element and id: a rect's
// numbers, or a path's data's.
QList<std::pair<QString, std::vector<PathNumber>>> shapesOf(const QString &path) {
    QList<std::pair<QString, std::vector<PathNumber>>> shapes;
    QXmlStreamReader svg(contentsOf(path));
    while (!svg.atEnd()) {
        if (svg.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        const QXmlStreamAttributes attributes = svg.attributes();
        const QString id = svg.name().toString() + QLatin1Char('#') + attributes.value(QLatin1String("id")).toString();
        if (svg.name() == QLatin1String("rect")) {
            std::vector<PathNumber> numbers;
            for (const char *name : {"x", "y", "width", "height", "rx"}) {
                numbers.push_back({QLatin1Char('R'), 0, attributes.value(QLatin1String(name)).toDouble()});
            }
            shapes.push_back({id, numbers});
        } else if (svg.name() == QLatin1String("path")) {
            shapes.push_back({id, pathNumbers(attributes.value(QLatin1String("d")).toString())});
        }
    }
    return shapes;
}

// A 32 or 36 px slot at 2x scaling, the task manager's at 2x, shows its 1x
// drawing doubled, as the 16 and 22 px slots do (#373 hill climb, H26: it
// had the 64 px composition, some 35 % lighter than the 1x drawing, so a
// slot's weight changed with the scale): every coordinate and width of the
// file twice the 1x file's, to within the files' rounding, the arcs' flags
// kept, its rim 2 px; rendered, each trace from twice its 1x start to the
// tile's inner edge, ending on twice its 1x end's rows, and twice as wide
// across.
void AppIconTest::doubledTaskManagerSlotsDoubleTheir1xDrawings() {
    QFETCH(int, size);
    const QString path = doubledPath(size);
    const QString single = FerrousAppIcon::resourcePath(size);
    const int doubled = 2 * size;
    QVERIFY2(QFile::exists(path), qPrintable(path));
    QCOMPARE(QImageReader(path).size(), QSize(doubled, doubled));

    const auto shapes = shapesOf(path);
    const auto singleShapes = shapesOf(single);
    QCOMPARE(shapes.size(), singleShapes.size());
    for (qsizetype n = 0; n < shapes.size(); ++n) {
        const auto &[id, numbers] = shapes[n];
        const auto &[singleId, singleNumbers] = singleShapes[n];
        QCOMPARE(id, singleId);
        QCOMPARE(numbers.size(), singleNumbers.size());
        for (size_t i = 0; i < numbers.size(); ++i) {
            QCOMPARE(numbers[i].command, singleNumbers[i].command);
            // An arc's rotation and flags, its 3rd to 5th numbers, are kept.
            const bool flag = numbers[i].command == QLatin1Char('A') && numbers[i].place >= 2 && numbers[i].place <= 4;
            const double wanted = flag ? singleNumbers[i].value : 2 * singleNumbers[i].value;
            QVERIFY2(std::abs(numbers[i].value - wanted) <= 0.0025,
                     qPrintable(QStringLiteral("%1, number %2: %3, not %4").arg(id).arg(i).arg(numbers[i].value).arg(wanted)));
        }
    }

    // Rendered: twice the 1x tile, in a 2 px rim.
    const QImage image = render(path, doubled);
    const QRect tile = tileOf(image);
    const QRect singleTile = tileOf(render(single, size));
    QCOMPARE(tile, QRect(singleTile.topLeft() * 2, singleTile.size() * 2));
    const QPoint middle = tile.center();
    for (int step = 0; step <= 2; ++step) {
        QCOMPARE(near(image.pixel(middle.x(), tile.bottom() - step), kRim, 2), step < 2);
    }
    const QRect inside = tile.adjusted(2, 2, -2, -2);
    QCOMPARE(tracesOf(image).size(), static_cast<size_t>(3));
    const QHash<QString, QImage> cover = traceCoverage(path, doubled);
    const QHash<QString, QImage> singleCover = traceCoverage(single, size);
    for (const QString &id : seriesIds(size)) {
        const QImage &trace = cover.value(id);
        const QImage &singleTrace = singleCover.value(id);
        const auto [first, last] = columnsOf(trace);
        const auto [singleFirst, singleLast] = columnsOf(singleTrace);
        QVERIFY2(std::abs(first - 2 * singleFirst) <= 1, qPrintable(QStringLiteral("%1 from %2, the 1x from %3").arg(id).arg(first).arg(singleFirst)));
        QCOMPARE(last, inside.right());
        QCOMPARE(last + 1, 2 * (singleLast + 1));
        const Column end = columnOf(trace, last);
        const Column singleEnd = columnOf(singleTrace, singleLast);
        QVERIFY2(std::abs(end.centre - 2 * singleEnd.centre) <= 0.3,
                 qPrintable(QStringLiteral("%1 ends at %2, the 1x at %3").arg(id).arg(end.centre).arg(singleEnd.centre)));
        // As wide across over the middle of its run as twice the 1x's.
        const int from = singleFirst + 2 * capColumns(size);
        const int to = singleLast - 5;
        const double width = widthAcross(trace, 2 * from, 2 * to + 1);
        const double singleWidth = widthAcross(singleTrace, from, to);
        QVERIFY2(std::abs(width - 2 * singleWidth) <= 0.05 * 2 * singleWidth,
                 qPrintable(QStringLiteral("%1: %2 px across, the 1x %3").arg(id).arg(width).arg(singleWidth)));
    }
}

void AppIconTest::doubledSlotsFrom48pxDrawThe64pxComposition_data() {
    QTest::addColumn<int>("size");
    QTest::addRow("48px@2") << 48;
    QTest::addRow("64px@2") << 64;
}

// A 48 px slot at 2x scaling has a drawing of its own (#373 hill climb,
// H21: the theme gave it the scalable drawing, with a 1 device px rim beside
// the 16 and 22 px slots' 2): the 64 px composition, three smooth traces
// rising 1 : 2 : 3 to the tile's inner edge, in twice the 1x tile and a 2 px
// rim, its traces where the 64 px drawing has them, in proportion to the
// tile inside the rim, and tapering as it does, from 3.6 units at 96 device
// px. The 64 px slot's (H22: the theme gave it the 128 or 256 px drawing,
// with the 4th harmonic, or the scalable one) is the 64 px drawing doubled,
// 3.8 units at 128 device px, without the 4th harmonic. (The 32 and 36 px
// slots' double their 1x drawings: doubledTaskManagerSlotsDoubleTheir1xDrawings.)
void AppIconTest::doubledSlotsFrom48pxDrawThe64pxComposition() {
    QFETCH(int, size);
    const QString path = doubledPath(size);
    const int doubled = 2 * size;
    QVERIFY2(QFile::exists(path), qPrintable(path));
    QCOMPARE(QImageReader(path).size(), QSize(doubled, doubled));
    const QImage image = render(path, doubled);

    // Twice the 1x tile, its margin and a 2 px rim.
    const QRect tile = tileOf(image);
    const QRect singleTile = tileOf(render(FerrousAppIcon::resourcePath(size), size));
    QCOMPARE(tile, QRect(singleTile.topLeft() * 2, singleTile.size() * 2));
    const QPoint middle = tile.center();
    for (int step = 0; step <= 2; ++step) {
        QCOMPARE(near(image.pixel(middle.x(), tile.bottom() - step), kRim, 2), step < 2);
        QCOMPARE(near(image.pixel(tile.left() + step, middle.y()), kRim, 2), step < 2);
    }
    const QRect inside = tile.adjusted(2, 2, -2, -2);

    // The three traces, each a filled outline with its start's round cap.
    QXmlStreamReader svg(contentsOf(path));
    QStringList ids;
    while (!svg.atEnd()) {
        if (svg.readNext() == QXmlStreamReader::StartElement && svg.name() == QLatin1String("path")) {
            ids << svg.attributes().value(QLatin1String("id")).toString();
            QCOMPARE(svg.attributes().value(QLatin1String("d")).toString().count(QLatin1Char('A')), 1);
        }
    }
    QCOMPARE(ids, seriesIds(size));
    QCOMPARE(tracesOf(image).size(), static_cast<size_t>(3));

    // Where the 64 px drawing has them, in proportion to the tile inside the
    // rim: each trace's start and end within a hundredth of its height.
    const QString reference = FerrousAppIcon::resourcePath(64);
    const QRect referenceInside = insideRim(tileOf(render(reference, 64)));
    const QHash<QString, QImage> cover = traceCoverage(path, doubled);
    const QHash<QString, QImage> referenceCover = traceCoverage(reference, 64);
    const std::vector<double> fundamental{3.8, 3.6};
    const double unit = inside.width() / 50.0;
    for (qsizetype n = 0; n < ids.size(); ++n) {
        const QImage &trace = cover.value(ids[n]);
        const QImage &referenceTrace = referenceCover.value(ids[n]);
        const auto [first, last] = columnsOf(trace);
        QCOMPARE(last, inside.right());
        // The fundamental's centre as the series puts it: less its added
        // weight's half on its underside, in each drawing's own units.
        const bool isFundamental = ids[n] == QLatin1String("fundamental");
        const double shift = isFundamental ? doubledFundamental(size).second / 50 : 0;
        const double referenceShift = isFundamental ? fundamentalUnderside(64) / 50 : 0;
        const auto at = [](const QImage &t, int x, const QRect &in) {
            return (columnOf(t, x).centre - in.top()) / in.height();
        };
        const int referenceFirst = columnsOf(referenceTrace).first;
        const double startShare = (referenceFirst + 3 - referenceInside.left() + 0.5) / referenceInside.width();
        const int start = inside.left() + static_cast<int>(std::lround(startShare * inside.width() - 0.5));
        QVERIFY2(std::abs(at(trace, start, inside) - shift - at(referenceTrace, referenceFirst + 3, referenceInside) + referenceShift) <= 0.015,
                 qPrintable(QStringLiteral("%1 starts at %2 of the tile, the 64 px drawing's at %3")
                                .arg(ids[n]).arg(at(trace, start, inside)).arg(at(referenceTrace, referenceFirst + 3, referenceInside))));
        QVERIFY2(std::abs(at(trace, last, inside) - shift - at(referenceTrace, referenceInside.right(), referenceInside) + referenceShift) <= 0.01,
                 qPrintable(QStringLiteral("%1 ends at %2 of the tile, the 64 px drawing's at %3")
                                .arg(ids[n]).arg(at(trace, last, inside)).arg(at(referenceTrace, referenceInside.right(), referenceInside))));
        // Its width across: the harmonics tapering 0.8 and 0.65 from the
        // composition's 3.8 units (3.6 in the 48 px slot's); the fundamental
        // heavier (doubledFundamental).
        const double taper[] = {0.65, 0.8, 1.0};
        const double wanted = (isFundamental ? doubledFundamental(size).first : fundamental[size == 48 ? 1 : 0] * taper[n]) * unit;
        const double width = widthAcross(trace, first + 2 * capColumns(64), last - 1);
        QVERIFY2(std::abs(width - wanted) <= 0.08 * wanted, qPrintable(QStringLiteral("%1: %2 px across, not %3").arg(ids[n]).arg(width).arg(wanted)));
    }
}

void AppIconTest::sizesDrawnAsASmallerSize_data() {
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("from");
    QTest::addColumn<int>("size");
    QTest::addColumn<int>("smaller");
    QTest::addRow("24px") << FerrousAppIcon::resourcePath(24) << FerrousAppIcon::resourcePath(22) << 24 << 22;
}

// The 24 px drawing is the 22 px one moved a pixel in on a 24 px canvas, as
// Breeze draws its 24 px icons, for the window icon's 24 px slot (#373 hill
// climb, H22: the slot took the 32 px drawing scaled down, three thin
// traces, or the 22 px one scaled up): pixel for pixel the same, moved, with
// nothing else on the canvas. (H22 drew 36 px and its 2x so too, from the
// 32 px slot's, until H24 gave 36 px a drawing of its own.)
void AppIconTest::sizesDrawnAsASmallerSize() {
    QFETCH(QString, path);
    QFETCH(QString, from);
    QFETCH(int, size);
    QFETCH(int, smaller);
    QCOMPARE(QImageReader(path).size(), QSize(size, size));
    const QImage image = render(path, size);
    const QImage source = render(from, smaller);
    const int offset = (size - smaller) / 2;
    int largest = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const QPoint at(x - offset, y - offset);
            const QRgb expected = source.rect().contains(at) ? source.pixel(at) : qRgba(0, 0, 0, 0);
            const QRgb shown = image.pixel(x, y);
            if (qAlpha(expected) == 0 && qAlpha(shown) == 0) {
                continue;
            }
            largest = std::max({largest, std::abs(qRed(shown) - qRed(expected)), std::abs(qGreen(shown) - qGreen(expected)),
                                std::abs(qBlue(shown) - qBlue(expected)), std::abs(qAlpha(shown) - qAlpha(expected))});
        }
    }
    QVERIFY2(largest <= 2, qPrintable(QString::number(largest)));
    // Its tile in the canvas's middle, the margin the moved pixels wider.
    QCOMPARE(tileOf(image), tileOf(source).translated(offset, offset));
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
    QTest::addColumn<QString>("resource");
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        QTest::addRow("%dpx", size) << QStringLiteral("%1x%1").arg(size) << FerrousAppIcon::resourcePath(size) << size;
    }
    for (const int size : kDoubledSizes) {
        QTest::addRow("%dpx@2", size) << QStringLiteral("%1x%1@2").arg(size) << doubledPath(size) << 2 * size;
    }
    QTest::addRow("scalable") << QStringLiteral("scalable") << FerrousAppIcon::resourcePath(0) << 0;
}

void AppIconTest::everySizeInstalls() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QFETCH(QString, directory);
    QFETCH(QString, resource);
    QFETCH(int, size);
    const QString path = installedIcon(directory);
    QVERIFY2(QFile::exists(path), qPrintable(path));
    // The installed file is the built-in drawing.
    const QByteArray installed = contentsOf(path);
    QVERIFY(!installed.isEmpty());
    QCOMPARE(installed, contentsOf(resource));
    const int drawn = size > 0 ? size : 64;
    const QImage image = render(path, drawn);
    QVERIFY(!image.isNull());
    QVERIFY(countWhere(image, image.rect(), [](QRgb p) { return near(p, kGlide, 24); }) >= drawn / 2);
}

// No 24x24@2 drawing, built in or installed (#373 hill climb, H23): the
// hicolor theme's 22 px directories serve 20 to 24 px and come first, so a
// 24 px slot at 2x takes 22x22@2 (hicolorsOwnIndexPicksTheSlotsDrawings), and
// the window icon has 1x pixmaps only. The 24 px drawing stays, for the
// window icon's 24 px slot (windowIconHasEachSizesDrawing), and says so, so
// that no one takes it for what the icon theme shows at 24 px (#373 hill
// climb, H24).
void AppIconTest::noDrawingForA24pxSlotAt2x() {
    QVERIFY(!QFile::exists(doubledPath(24)));
    QVERIFY(QFile::exists(FerrousAppIcon::resourcePath(24)));
    const QString note = QString::fromUtf8(contentsOf(FerrousAppIcon::resourcePath(24))).section(QStringLiteral("-->"), 0, 0);
    QVERIFY(note.contains(QLatin1String("serves only the built-in window icon's 24 px pixmap")));
    QVERIFY(note.contains(QLatin1String("hicolor's 22 px directories serve the 24 px slot")));
    QVERIFY(std::find(FerrousAppIcon::kSizes.cbegin(), FerrousAppIcon::kSizes.cend(), 24) != FerrousAppIcon::kSizes.cend());
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QVERIFY(QFile::exists(installedIcon(QStringLiteral("24x24"))));
    QVERIFY(!QDir(installPrefix()).exists(QStringLiteral("share/icons/hicolor/24x24@2")));
}

void AppIconTest::iconThemeGivesEachSizeItsDrawing_data() {
    QTest::addColumn<int>("size");
    QTest::addColumn<int>("scale");
    for (const int size : FerrousAppIcon::kSizes) {
        QTest::addRow("%dpx", size) << size << 1;
    }
    for (const int size : kDoubledSizes) {
        QTest::addRow("%dpx@2", size) << size << 2;
    }
}

// At 1x each size's drawing, and at 2x in a 16 to 48 px slot that slot's
// drawing at 2x.
void AppIconTest::iconThemeGivesEachSizeItsDrawing() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QFETCH(int, size);
    QFETCH(int, scale);
    // The hicolor theme's index, as the hicolor-icon-theme package has it:
    // each fixed size and its 2x, then the scalable directory.
    QTemporaryDir index;
    QVERIFY(index.isValid());
    QVERIFY(QDir(index.path()).mkpath(QStringLiteral("hicolor")));
    {
        QString text;
        QStringList directories;
        for (const int fixed : FerrousAppIcon::kSizes) {
            const QString directory = QStringLiteral("%1x%1/apps").arg(fixed);
            const QString doubled = QStringLiteral("%1x%1@2/apps").arg(fixed);
            directories << directory << doubled;
            text += QStringLiteral("[%1]\nSize=%2\nContext=Applications\nType=Fixed\n\n").arg(directory).arg(fixed);
            text += QStringLiteral("[%1]\nSize=%2\nScale=2\nContext=Applications\nType=Fixed\n\n").arg(doubled).arg(fixed);
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
    const QImage shown = icon.pixmap(QSize(size, size), scale).toImage().convertToFormat(QImage::Format_ARGB32);
    const QString directory = scale == 1 ? QStringLiteral("%1x%1").arg(size) : QStringLiteral("%1x%1@2").arg(size);
    const QImage expected = render(installedIcon(directory), size * scale);
    QVERIFY2(maxDifference(shown, expected) <= 2, qPrintable(QString::number(maxDifference(shown, expected))));
}

void AppIconTest::hicolorsOwnIndexPicksTheSlotsDrawings_data() {
    QTest::addColumn<int>("size");
    QTest::addColumn<qreal>("scale");
    QTest::addColumn<QString>("directory");
    for (const int size : {16, 22, 32, 48}) {
        QTest::addRow("%dpx", size) << size << 1.0 << QStringLiteral("%1x%1").arg(size);
        QTest::addRow("%dpx@2", size) << size << 2.0 << QStringLiteral("%1x%1@2").arg(size);
        QTest::addRow("%dpx@1.5", size) << size << 1.5 << QStringLiteral("%1x%1@2").arg(size);
    }
    // The 22 px directories reach to 24 px, and come first.
    QTest::addRow("24px") << 24 << 1.0 << QStringLiteral("22x22");
    QTest::addRow("24px@2") << 24 << 2.0 << QStringLiteral("22x22@2");
    // 36 px, between the 32 and 48 px directories' reach, its own.
    QTest::addRow("36px") << 36 << 1.0 << QStringLiteral("36x36");
    QTest::addRow("36px@2") << 36 << 2.0 << QStringLiteral("36x36@2");
    QTest::addRow("36px@1.5") << 36 << 1.5 << QStringLiteral("36x36@2");
    // 64 px at 1.25x to 2x, where KDE's lookup took the 256 px drawing and
    // Qt's the scalable one. (At 1x Qt's own lookup, unlike KDE's, takes
    // the 256 px directory before the 64 px one.)
    QTest::addRow("64px@2") << 64 << 2.0 << QStringLiteral("64x64@2");
    QTest::addRow("64px@1.5") << 64 << 1.5 << QStringLiteral("64x64@2");
    QTest::addRow("64px@1.25") << 64 << 1.25 << QStringLiteral("64x64@2");
    QTest::addRow("128px") << 128 << 1.0 << QStringLiteral("128x128");
}

// The hicolor theme's own index, as the hicolor-icon-theme package has it
// (0.18): its application directories are "Threshold" ones, each serving
// its size and 2 px either side, and its 256 and 512 px ones scalable from
// 64 px. Qt's theme lookup, and KDE's, which a Plasma session uses (the
// same slots, checked by hand), take the first directory that serves a
// size, at the scale rounded up: so a slot at 1.25x or 1.5x takes the 2x
// drawing. A 24 px slot takes the 22 px drawing (#373 hill climb, H22: the
// 24 px directories come after it), as Breeze's own application icons,
// which have no 24 px drawing, show theirs.
void AppIconTest::hicolorsOwnIndexPicksTheSlotsDrawings() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QFETCH(int, size);
    QFETCH(qreal, scale);
    QFETCH(QString, directory);
    QTemporaryDir index;
    QVERIFY(index.isValid());
    QVERIFY(QDir(index.path()).mkpath(QStringLiteral("hicolor")));
    {
        QString text;
        QStringList directories;
        for (const int fixed : {16, 22, 24, 32, 36, 48, 64, 72, 96, 128, 192, 256, 512}) {
            for (const int dirScale : {1, 2}) {
                const QString name = QStringLiteral("%1x%1%2/apps").arg(fixed).arg(dirScale == 2 ? QStringLiteral("@2") : QString());
                directories << name;
                text += QStringLiteral("[%1]\n").arg(name);
                if (fixed >= 256) {
                    text += QStringLiteral("MinSize=64\nSize=%1\nMaxSize=%1\n").arg(fixed);
                } else {
                    text += QStringLiteral("Size=%1\n").arg(fixed);
                }
                if (dirScale == 2) {
                    text += QStringLiteral("Scale=2\n");
                }
                text += QStringLiteral("Context=Applications\nType=%1\n\n").arg(fixed >= 256 ? QStringLiteral("Scalable") : QStringLiteral("Threshold"));
            }
        }
        directories << QStringLiteral("scalable/apps");
        text += QStringLiteral("[scalable/apps]\nMinSize=1\nSize=128\nMaxSize=256\nContext=Applications\nType=Scalable\n");
        text.prepend(QStringLiteral("[Icon Theme]\nName=Hicolor\nDirectories=%1\n\n").arg(directories.join(QLatin1Char(','))));
        QFile theme(index.filePath(QStringLiteral("hicolor/index.theme")));
        QVERIFY(theme.open(QIODevice::WriteOnly));
        QVERIFY(theme.write(text.toUtf8()) > 0);
    }
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
    const QImage shown = icon.pixmap(QSize(size, size), scale).toImage().convertToFormat(QImage::Format_ARGB32);
    const int device = qRound(size * scale);
    QCOMPARE(shown.size(), QSize(device, device));
    const QImage expected = render(installedIcon(directory), device);
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

void AppIconTest::commentsDescribeTheDrawingNotItsHistory_data() {
    QTest::addColumn<QString>("path");
    for (const int size : FerrousAppIcon::kSizes) {
        QTest::addRow("%dpx", size) << FerrousAppIcon::resourcePath(size);
    }
    for (const int size : kDoubledSizes) {
        QTest::addRow("%dpx@2", size) << doubledPath(size);
    }
    QTest::addRow("scalable") << FerrousAppIcon::resourcePath(0);
}

// Each drawing's comment says how it is built, the rule its size follows and
// which slot it serves, and nothing of the design rounds it came from: the
// drawings ship, and are judged blind.
void AppIconTest::commentsDescribeTheDrawingNotItsHistory() {
    QFETCH(QString, path);
    const QString source = QString::fromUtf8(contentsOf(path));
    const qsizetype open = source.indexOf(QLatin1String("<!--"));
    QVERIFY(open >= 0);
    const QString comment = source.mid(open, source.indexOf(QLatin1String("-->"), open) - open);
    QVERIFY(comment.contains(QLatin1String("Ferrous")));
    static const QRegularExpression history(QStringLiteral("#373|hill climb|\\bH[0-9]+\\b"));
    const QRegularExpressionMatch match = history.match(comment);
    QVERIFY2(!match.hasMatch(), qPrintable(match.captured(0)));
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
