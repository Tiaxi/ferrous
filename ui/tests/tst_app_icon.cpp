// SPDX-License-Identifier: GPL-3.0-or-later

// The app icon (#373): a drawing of its own for each size, built in for the
// window icon and installed in the hicolor icon theme with the desktop file.

#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
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
#include <vector>

#include "AppIcon.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

constexpr QRgb kTile = 0x3a1490;
// The chirp: the fundamental in amber, its 2nd harmonic in red-orange and
// its 3rd lower on the colour map, each thinner than the one below it.
constexpr QRgb kGlide = 0xffb000;
constexpr QRgb kHarmonic = 0xf0602a;
constexpr QRgb kThirdHarmonic = 0xd20040;

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

    // How much ink the trace has in a column, in whole pixels' worth.
    double inkIn(const QImage &image, int x, double full) const {
        double sum = 0;
        for (const QPoint &p : pixels) {
            if (p.x() == x) {
                sum += std::min(1.0, distanceFromTile(image.pixel(p)) / full);
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
    void tracesStandOnThePlainTile_data() { addSizeRows(); }
    void tracesStandOnThePlainTile();
    void harmonicsFanOutFromOneTime_data() { addSizeRows(); }
    void harmonicsFanOutFromOneTime();
    void smallSizesAreDrawnOnThePixelGrid_data();
    void smallSizesAreDrawnOnThePixelGrid();
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
    QVERIFY(countWhere(image, all, [](QRgb p) { return near(p, kTile, 4); }) >= size * size / 3);
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

// The traces stand on the plain tile: H1 to H5 drew the spectrogram's floor
// under them as a magenta band, which read as an unrelated underline or a
// progress bar, so it is gone at every size (#373 hill climb, H5).
void AppIconTest::tracesStandOnThePlainTile() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const std::vector<Trace> traces = tracesOf(image);
    QVERIFY(!traces.empty());

    // Nothing but the tile under the lowest trace, down to the tile's edge:
    // no band, no line. From 48 px up the fundamental's antialiased edge
    // reaches a row below its ink; the stairs at 16 to 32 px have none, and
    // at 16 px they fill the tile to a row of its edge.
    int lowest = 0;
    for (const Trace &trace : traces) {
        lowest = std::max(lowest, trace.bounds.bottom());
    }
    const int top = lowest + (onPixelGrid(size) ? 1 : 2);
    const int corner = std::max(2, size / 8);
    const QRect below(tile.left() + corner, top, tile.width() - 2 * corner, tile.bottom() - top + 1);
    QVERIFY2(below.height() >= (size > 16 ? 2 : 1), qPrintable(QString::number(lowest)));
    QCOMPARE(countWhere(image, below, [](QRgb p) { return near(p, kTile, 4); }), below.width() * below.height());

    // Every mark rises: none is a flat band or line under the traces.
    for (const Trace &trace : traces) {
        const double left = trace.centreIn(image, trace.bounds.left());
        const double right = trace.centreIn(image, trace.bounds.right());
        QVERIFY2(left - right >= 1.0, qPrintable(QStringLiteral("a flat mark at rows %1 to %2")
                                                     .arg(trace.bounds.top())
                                                     .arg(trace.bounds.bottom())));
    }

    // Every trace is inside the tile, clear of its edges.
    for (const Trace &trace : traces) {
        QVERIFY(trace.bounds.left() > tile.left() && trace.bounds.right() < tile.right());
        QVERIFY(trace.bounds.top() > tile.top() && trace.bounds.bottom() < tile.bottom());
    }

    // The traces sit in the middle of the tile, to within a pixel or so.
    const QRect ink = boundsWhere(image, traceInk);
    const double slack = std::max(1.0, size / 32.0);
    const double dx = (ink.left() + ink.right()) / 2.0 - (tile.left() + tile.right()) / 2.0;
    const double dy = (ink.top() + ink.bottom()) / 2.0 - (tile.top() + tile.bottom()) / 2.0;
    QVERIFY2(std::abs(dx) <= slack && std::abs(dy) <= slack,
             qPrintable(QStringLiteral("off centre by %1, %2").arg(dx).arg(dy)));
}

// The glide and one harmonic leaving it from a shared origin read as two
// pencils or a slash. The drawing is a harmonic chirp instead: the
// fundamental and its harmonics start at one time, at one, two and three
// times its height above the zero of frequency, and rise one, two and three
// times as fast, so they fan out, each thinner than the one below (#373 hill
// climb).
void AppIconTest::harmonicsFanOutFromOneTime() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const std::vector<Trace> traces = tracesOf(image);

    // Separate traces, three from 32 px up. At 16 and 22 px three were busy
    // and broke into dashes: there the fundamental and one harmonic, each
    // a solid stair, with a clear row between (#373 hill climb, H5).
    const size_t expected = size >= 32 ? 3 : 2;
    QVERIFY2(traces.size() == expected, qPrintable(QString::number(traces.size())));

    // Lowest first, each in its own colour.
    const QRgb colours[] = {kGlide, kHarmonic, kThirdHarmonic};
    for (size_t i = 0; i < traces.size(); ++i) {
        const QRgb strongest = traces[i].strongest(image);
        for (size_t j = 0; j < 3; ++j) {
            QVERIFY2(j == i || colourDistance(strongest, colours[i]) < colourDistance(strongest, colours[j]),
                     qPrintable(QStringLiteral("trace %1 is #%2").arg(i).arg(strongest & 0xffffff, 6, 16, QLatin1Char('0'))));
        }
    }
    QVERIFY(countWhere(image, image.rect(), [](QRgb p) { return near(p, kGlide, 24); }) >= size / 2);

    // They start at one time: their left ends agree to within their round
    // caps, in the tile's left third.
    const double slack = std::max(1.0, size / 32.0);
    int start = 0;
    int end = size;
    for (const Trace &trace : traces) {
        QVERIFY2(std::abs(trace.bounds.left() - traces.front().bounds.left()) <= slack,
                 qPrintable(QStringLiteral("%1, %2").arg(trace.bounds.left()).arg(traces.front().bounds.left())));
        QVERIFY(trace.bounds.left() < tile.left() + tile.width() / 3);
        start = std::max(start, trace.bounds.left());
        end = std::min(end, trace.bounds.right());
    }
    // Past the round caps from 48 px up, where each trace is drawn at its
    // full width; the pixel-grid stairs at 16 to 32 px have none.
    const int capped = onPixelGrid(size) ? 0 : std::max(1, size / 32);
    start += capped;
    end -= capped;
    QVERIFY(end - start >= tile.width() / 2);

    // At one, two and three times the fundamental's height above the zero
    // of frequency, and rising one, two and three times as far. The zero is
    // no longer drawn: it is where the traces' lines meet, the same height
    // from each harmonic and at either end, at or below the fundamental and
    // at the tile's foot. The pixel grid at 16 and 22 px allows some slack:
    // there the harmonic climbs a row every column and the fundamental one
    // every two, and the zero falls just past the tile's foot.
    std::vector<double> from;
    std::vector<double> to;
    for (const Trace &trace : traces) {
        from.push_back(trace.centreIn(image, start));
        to.push_back(trace.centreIn(image, end));
        QVERIFY(from.back() > 0 && to.back() > 0);
    }
    std::vector<double> zeros;
    for (size_t n = 1; n < traces.size(); ++n) {
        const double harmonic = static_cast<double>(n + 1);
        zeros.push_back((harmonic * from[0] - from[n]) / (harmonic - 1));
        zeros.push_back((harmonic * to[0] - to[n]) / (harmonic - 1));
    }
    double zero = 0;
    for (const double z : zeros) {
        zero += z / static_cast<double>(zeros.size());
    }
    for (const double z : zeros) {
        QVERIFY2(std::abs(z - zero) <= slack, qPrintable(QStringLiteral("zero at %1, %2").arg(z).arg(zero)));
    }
    QVERIFY2(zero > traces.front().bounds.bottom() && zero <= tile.bottom() + (size < 32 ? 3.0 : 1.0),
             qPrintable(QStringLiteral("zero at %1").arg(zero)));
    std::vector<double> heights;
    std::vector<double> rises;
    for (size_t n = 0; n < traces.size(); ++n) {
        heights.push_back(zero - from[n]);
        rises.push_back(from[n] - to[n]);
    }
    QVERIFY2(heights[0] >= 1.0 && rises[0] >= 1.0,
             qPrintable(QStringLiteral("height %1, rise %2").arg(heights[0]).arg(rises[0])));
    for (size_t n = 1; n < traces.size(); ++n) {
        const double harmonic = static_cast<double>(n + 1);
        const double height = heights[n] / heights[0];
        const double rise = rises[n] / rises[0];
        QVERIFY2(height >= harmonic * 0.8 && height <= harmonic * 1.25,
                 qPrintable(QStringLiteral("harmonic %1: height %2 times").arg(n + 1).arg(height)));
        QVERIFY2(rise >= harmonic * 0.8 && rise <= harmonic * 1.25,
                 qPrintable(QStringLiteral("harmonic %1: rise %2 times").arg(n + 1).arg(rise)));
    }

    // Each thinner than the one below, from the fundamental up; at 16 to
    // 32 px every stair is 2 px, which the pixel grid allows.
    const double full = distanceFromTile(kGlide) * 0.6;
    for (size_t n = 1; n < traces.size(); ++n) {
        double lower = 0;
        double upper = 0;
        for (int x = start; x <= end; ++x) {
            lower += traces[n - 1].inkIn(image, x, full);
            upper += traces[n].inkIn(image, x, full);
        }
        const bool thinner = onPixelGrid(size) ? upper <= lower * 1.1 : upper < lower * 0.95;
        QVERIFY2(thinner,
                 qPrintable(QStringLiteral("trace %1: %2 against %3").arg(n).arg(upper).arg(lower)));
    }

    // Nothing of the fundamental in the tile's top-right corner: it rises
    // gently, it does not slash from corner to corner.
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

// At 16 and 22 px the traces aliased into stepped stripes that read as a
// striped flag or lines of text, and at 32 px the thinner harmonics into
// dots. Each is a drawing of its own on the pixel grid: filled whole-pixel
// stairs, 2 px tall in every column, with a clear row between neighbours
// along their whole length; two traces at 16 and 22 px, three at 32 (#373
// hill climb, H6).
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
        : QStringList{QStringLiteral("h2"), QStringLiteral("fundamental")};
    QCOMPARE(ids, expected);

    // Rendered: away from the tile's rounded corners, every pixel is the
    // tile's colour or a trace's, with no antialiased edge between.
    const QImage image = render(path, size);
    const QRect tile = tileOf(image);
    const int corner = std::max(2, size / 8);
    const QRgb colours[] = {kTile, kGlide, kHarmonic, kThirdHarmonic};
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

    // Each trace 2 px tall in every column, and a clear row of the tile
    // between neighbours in every column. That they are found as separate
    // traces at all means no two touch, even at a corner.
    const std::vector<Trace> traces = tracesOf(image);
    QCOMPARE(traces.size(), static_cast<size_t>(expected.size()));
    for (const Trace &trace : traces) {
        QCOMPARE(trace.bounds.left(), traces.front().bounds.left());
        QCOMPARE(trace.bounds.right(), traces.front().bounds.right());
    }
    std::vector<int> run(traces.size(), 0);
    std::vector<int> longestRun(traces.size(), 0);
    std::vector<int> lastTop(traces.size(), -1);
    int firstGap = -1;
    int gap = 0;
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
            QVERIFY2(count == 2 && bottom == top + 1, qPrintable(QStringLiteral("column %1: %2 px").arg(x).arg(count)));
            QVERIFY2(bottom < below - 1, qPrintable(QStringLiteral("column %1: no clear row").arg(x)));
            if (n == 1) {
                gap = below - bottom - 1;
                firstGap = firstGap < 0 ? gap : firstGap;
            }
            below = top;
            run[n] = top == lastTop[n] ? run[n] + 1 : 1;
            longestRun[n] = std::max(longestRun[n], run[n]);
            lastTop[n] = top;
        }
    }
    if (size < 32) {
        // The clean pixel diagonals, not dashes: the fundamental climbs a row
        // every two columns and the harmonic every column. The clear rows
        // between them open from one to at least four, so they fan out from
        // one point rather than lie as stripes.
        QVERIFY2(longestRun[0] <= 2, qPrintable(QString::number(longestRun[0])));
        QCOMPARE(longestRun[1], 1);
        QCOMPARE(firstGap, 1);
        QVERIFY2(gap >= 4, qPrintable(QString::number(gap)));
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
