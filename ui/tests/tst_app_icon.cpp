// SPDX-License-Identifier: GPL-3.0-or-later

// The app icon (#373): a drawing of its own for each size, built in for the
// window icon and installed in the hicolor icon theme with the desktop file.
// The drawings are locked; the tests check the rules every drawing follows,
// so that a change to one fails on the rule it breaks.

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
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "AppIcon.h"
#include "TestColor.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

constexpr QRgb kTile = 0x3a1490;
// The tile's rim: a quiet mid-violet hairline, lighter than the tile, so that
// the tile holds its edge on a dark panel (paletteHoldsApart).
constexpr QRgb kRim = 0x5a3fb0;
// Breeze's panels: Dark's window and view, Light's window and view.
constexpr QRgb kBreezeDarkWindow = 0x202326;
constexpr QRgb kBreezeDarkView = 0x31363b;
constexpr QRgb kBreezeLightWindow = 0xeff0f1;
constexpr QRgb kBreezeLightView = 0xffffff;
// The chirp: the fundamental in amber, its 2nd harmonic in rose, its 3rd in
// magenta and, at 128 and 256 px, a faint 4th in violet, a step of value
// darker each. The drawings on the pixel grid at 1x draw the magenta a step
// lighter: their 45 degree climbs cover a row and two half rows a column, and
// in the other drawings' magenta they render dimmer than those do.
constexpr QRgb kGlide = 0xf2b035;
constexpr QRgb kHarmonic = 0xff7a92;
constexpr QRgb kThirdHarmonic = 0xe055b4;
constexpr QRgb kPixelGridThirdHarmonic = 0xea6cc2;
constexpr QRgb kFourthHarmonic = 0xab55df;

// Rec. 709 luma, 0 to 255.
double luma(QRgb c) {
    return 0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c);
}

QImage render(const QString &path, int size) {
    QImageReader reader(path);
    reader.setScaledSize(QSize(size, size));
    return reader.read().convertToFormat(QImage::Format_ARGB32);
}

QByteArray contentsOf(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// An opaque pixel within `tolerance` of a colour, in each channel.
bool near(QRgb pixel, QRgb colour, int tolerance) {
    return qAlpha(pixel) == 255 && channelDistance(QColor(pixel), QColor(colour)) <= tolerance;
}

// A trace's ink over the tile: a pixel it covers by about half or more. The
// tile has almost no red or green, every trace plenty of red.
bool traceInk(QRgb pixel) {
    return qAlpha(pixel) == 255 && channelDistance(QColor(pixel), QColor(kTile)) >= 80;
}

QString hex(QRgb colour) {
    return QStringLiteral("#%1").arg(colour & 0xffffff, 6, 16, QLatin1Char('0'));
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

// The first pixel of an area, row by row, that fails a test, if any.
std::optional<QPoint> firstFailing(const QRect &area, const std::function<bool(int, int)> &passes) {
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x) {
            if (!passes(x, y)) {
                return QPoint(x, y);
            }
        }
    }
    return std::nullopt;
}

QString at(const std::optional<QPoint> &point) {
    return point ? QStringLiteral("at %1, %2").arg(point->x()).arg(point->y()) : QString();
}

// The tile's square, rim included, found from its opaque pixels.
QRect tileOf(const QImage &image) {
    return boundsWhere(image, [](QRgb p) { return qAlpha(p) > 0; });
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
            largest = std::max({largest, channelDistance(QColor(p), QColor(q)), std::abs(qAlpha(p) - qAlpha(q))});
        }
    }
    return largest;
}

// The traces of a rendered drawing, as shapes of touching ink pixels, corners
// included: a trace's most saturated pixel for each, lowest first.
std::vector<QRgb> traceCores(const QImage &image) {
    const int size = image.width();
    std::vector<bool> seen(static_cast<size_t>(size * size), false);
    std::vector<std::pair<int, QRgb>> traces;
    const auto distance = [](QRgb p) { return channelDistance(QColor(p), QColor(kTile)); };
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!traceInk(image.pixel(x, y)) || seen[static_cast<size_t>(y * size + x)]) {
                continue;
            }
            int bottom = y;
            QRgb core = image.pixel(x, y);
            std::vector<QPoint> pending{QPoint(x, y)};
            seen[static_cast<size_t>(y * size + x)] = true;
            while (!pending.empty()) {
                const QPoint p = pending.back();
                pending.pop_back();
                bottom = std::max(bottom, p.y());
                core = distance(image.pixel(p)) > distance(core) ? image.pixel(p) : core;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const QPoint q(p.x() + dx, p.y() + dy);
                        const size_t at = static_cast<size_t>(q.y() * size + q.x());
                        if (image.rect().contains(q) && !seen[at] && traceInk(image.pixel(q))) {
                            seen[at] = true;
                            pending.push_back(q);
                        }
                    }
                }
            }
            traces.emplace_back(bottom, core);
        }
    }
    std::sort(traces.begin(), traces.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<QRgb> cores;
    for (const auto &trace : traces) {
        cores.push_back(trace.second);
    }
    return cores;
}

// A drawing's file with only some of its lines, rendered at a size.
QImage renderLines(const QString &path, int size, const std::function<bool(const QString &)> &keep) {
    QStringList lines = QString::fromUtf8(contentsOf(path)).split(QLatin1Char('\n'));
    lines.removeIf([&](const QString &line) { return !keep(line); });
    QByteArray bytes = lines.join(QLatin1Char('\n')).toUtf8();
    QBuffer buffer(&bytes);
    QImageReader reader(&buffer, "svg");
    reader.setScaledSize(QSize(size, size));
    return reader.read().convertToFormat(QImage::Format_ARGB32);
}

bool isShape(const QString &line) {
    return line.contains(QLatin1String("<rect")) || line.contains(QLatin1String("<path "));
}

// Each trace of a drawing alone, by its id, rendered at a size without the
// tile, the rim or the other traces: each pixel's alpha is how much of it the
// trace covers.
QHash<QString, QImage> traceCoverage(const QString &path, int size) {
    static const QRegularExpression pathId(QStringLiteral("<path id=\"(\\w+)\""));
    QHash<QString, QImage> traces;
    for (const QString &line : QString::fromUtf8(contentsOf(path)).split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch match = pathId.match(line);
        if (match.hasMatch()) {
            traces.insert(match.captured(1), renderLines(path, size, [&](const QString &other) { return other == line || !isShape(other); }));
        }
    }
    return traces;
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

// Breeze's app-icon margin around the tile at each size: 1 px at 16, 2 px at
// 22 and 32, 4 px at 48 and 6 px at 64, as Breeze's own app icons sit, and in
// proportion above; 3 px at 24, the 22 px drawing's 2 and the pixel it moves
// in, and 3 at 36, a 30 px rim.
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

// The sizes with a drawing of their own for their slot at 2x scaling: 16 to
// 48 px their 1x drawings doubled, 64 px its drawing doubled. None for 24 px,
// whose slot at 2x takes the 22 px slot's (hicolorsOwnIndexPicksTheSlotsDrawings).
constexpr std::array<int, 6> kDoubledSizes{16, 22, 32, 36, 48, 64};

// The drawing for a size's slot at 2x scaling.
QString doubledPath(int size) {
    return QStringLiteral(":/icons/hicolor/%1x%1@2/apps/ferrous.svg").arg(size);
}

// A drawing as the tests check it: its file, the slot it is drawn for and its
// scale there, and its rim in device pixels: 1 at 1x, 2 at 2x. The scalable
// drawing is checked at 128 px, where its 1.5 unit rim is 3 whole device
// pixels, with the 128 px drawing's strokes and without its detail tier.
struct Drawing {
    QString path;
    int slot = 0;
    int scale = 1;
    int rim = 1;
    bool scalable = false;

    int device() const { return slot * scale; }

    // 16, 22 and 24 px at 1x are drawn on the pixel grid: each trace a closed
    // shape with every vertex on a whole pixel, crisp flats and climbs of a
    // row a column at most. The others are smooth curves.
    bool onPixelGrid() const { return scale == 1 && slot <= 24; }

    // At 128 and 256 px a detail tier: a faint 4th harmonic over the three
    // traces (the 64 px drawing enlarged leaves the tile's upper left empty
    // beside Breeze's large icons).
    bool hasDetailTier() const { return scale == 1 && slot >= 128 && !scalable; }

    // The ids of the series, top harmonic first, as drawn: from 32 px the
    // fundamental and its 2nd and 3rd harmonics; in the 16 to 24 px slots the
    // fundamental and the 3rd (a middle trace there fuses with its neighbours).
    QStringList seriesIds() const {
        if (slot >= 32) {
            return {QStringLiteral("h3"), QStringLiteral("h2"), QStringLiteral("fundamental")};
        }
        return {QStringLiteral("h3"), QStringLiteral("fundamental")};
    }

    // Every trace's id in the file, in drawing order.
    QStringList ids() const {
        QStringList ids = seriesIds();
        if (hasDetailTier()) {
            ids.prepend(QStringLiteral("h4"));
        }
        return ids;
    }

    // The harmonic number of a trace.
    static int number(const QString &id) { return id == QLatin1String("fundamental") ? 1 : id.mid(1).toInt(); }

    QRgb colour(const QString &id) const {
        switch (number(id)) {
        case 1:
            return kGlide;
        case 2:
            return kHarmonic;
        case 3:
            return onPixelGrid() ? kPixelGridThirdHarmonic : kThirdHarmonic;
        default:
            return kFourthHarmonic;
        }
    }

    // The series' strokes' widths across, in device pixels, lowest first: on
    // the pixel grid two strokes 3 and 2 px tall (a 1 px harmonic breaks into
    // dots), the 16 px fundamental 2 px over its first three columns; at 32 to
    // 48 px 4, 2.5 and 2.25 px (3, 2.5 and 2 read as three near-equal
    // stripes), the top two eased to 2 px into their ends; from 64 px the
    // fundamental 5 units of the 64-unit drawing and the harmonics 0.8 and 0.65
    // of 3.8 units, and from 128 px and in the scalable drawing 4.5 and 3.4
    // units (3.0 reads thin beside 128 px icons). At 2x twice the 1x widths.
    std::vector<double> strokeWidths() const {
        std::vector<double> widths;
        if (slot <= 24) {
            widths = {3, 2};
        } else if (slot <= 48) {
            widths = {4, 2.5, 2.25};
        } else {
            const double unit = slot / 64.0;
            const double fundamental = slot == 64 ? 5.0 : 4.5;
            const double taper = slot == 64 ? 3.8 : 3.4;
            widths = {fundamental * unit, 0.8 * taper * unit, 0.65 * taper * unit};
        }
        for (double &width : widths) {
            width *= scale;
        }
        return widths;
    }

    // The columns a trace's start takes before its full width: none on the
    // pixel grid, where it starts square; else its round cap, as far as the
    // fundamental's half width, rounded up.
    int capColumns() const { return onPixelGrid() ? 0 : static_cast<int>(std::ceil(strokeWidths().front() / 2)); }
};

} // namespace

Q_DECLARE_METATYPE(Drawing)

namespace {

// Every drawing: each size's, each doubled slot's, and the scalable one, for
// the slots no other drawing matches.
void addDrawingRows() {
    QTest::addColumn<Drawing>("drawing");
    for (const int size : FerrousAppIcon::kSizes) {
        QTest::addRow("%dpx", size) << Drawing{FerrousAppIcon::resourcePath(size), size, 1, 1};
    }
    for (const int size : kDoubledSizes) {
        QTest::addRow("%dpx@2", size) << Drawing{doubledPath(size), size, 2, 2};
    }
    QTest::addRow("scalable") << Drawing{FerrousAppIcon::resourcePath(0), 128, 1, 3, true};
}

// The tile inside its rim.
QRect insideRim(const QImage &image, const Drawing &drawing) {
    return tileOf(image).adjusted(drawing.rim, drawing.rim, -drawing.rim, -drawing.rim);
}

// A drawing's traces drawn alone, lowest first, by Drawing::ids.
std::vector<QImage> tracesOf(const Drawing &drawing, const QStringList &ids) {
    const QHash<QString, QImage> cover = traceCoverage(drawing.path, drawing.device());
    std::vector<QImage> traces;
    for (auto id = ids.crbegin(); id != ids.crend(); ++id) {
        traces.push_back(cover.value(*id));
    }
    return traces;
}

// How bright a 3rd harmonic on the pixel grid renders on the tile where it
// climbs and where it runs flat, as contrast ratios on the tile: in each
// column, its colour over the tile in the mean coverage of its brightest run
// of rows as tall as its ink there; the median over the columns where its top
// row moves from the column before, and over those where it stays.
std::pair<double, double> magentaBrightness(const QImage &trace, QRgb colour) {
    const int size = trace.width();
    std::vector<double> climbs;
    std::vector<double> flats;
    int previousTop = -1;
    for (int x = 0; x < size; ++x) {
        const Column column = columnOf(trace, x);
        if (column.sum < 1) {
            previousTop = -1;
            continue;
        }
        const int rows = std::max(1, static_cast<int>(std::lround(column.sum)));
        double best = 0;
        for (int y = 0; y + rows <= size; ++y) {
            double run = 0;
            for (int r = 0; r < rows; ++r) {
                run += alphaAt(trace, x, y + r);
            }
            best = std::max(best, run / rows);
        }
        const QColor shown = mixed(QColor(kTile), QColor(colour), best);
        (previousTop >= 0 && column.top != previousTop ? climbs : flats).push_back(contrastRatio(shown, QColor(kTile)));
        previousTop = column.top;
    }
    const auto median = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values.empty() ? 0.0 : values[values.size() / 2];
    };
    return {median(climbs), median(flats)};
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

QString installPrefix() {
    return qEnvironmentVariable("FERROUS_APP_ICON_INSTALL_PREFIX");
}

QString installedIcon(const QString &directory) {
    return QDir(installPrefix()).filePath(QStringLiteral("share/icons/hicolor/%1/apps/ferrous.svg").arg(directory));
}

// The theme icon's pixmap for a slot, with an index.theme for the hicolor
// theme of these directories, each with its keys, over the test's install.
QImage themeIconAt(const QList<std::pair<QString, QString>> &directories, int size, qreal scale) {
    QTemporaryDir index;
    QStringList names;
    QString text;
    for (const auto &[name, keys] : directories) {
        names << name;
        text += QStringLiteral("\n[%1]\n%2Context=Applications\n").arg(name, keys);
    }
    text += QStringLiteral("\n[scalable/apps]\nMinSize=1\nSize=128\nMaxSize=256\nContext=Applications\nType=Scalable\n");
    names << QStringLiteral("scalable/apps");
    text.prepend(QStringLiteral("[Icon Theme]\nName=Hicolor\nDirectories=%1\n").arg(names.join(QLatin1Char(','))));
    QFile theme(index.filePath(QStringLiteral("hicolor/index.theme")));
    if (!index.isValid() || !QDir(index.path()).mkpath(QStringLiteral("hicolor")) || !theme.open(QIODevice::WriteOnly)
        || theme.write(text.toUtf8()) <= 0) {
        return {};
    }
    theme.close();
    // A theme icon looks itself up again when the theme changes: render it
    // before the theme is restored.
    const QStringList searchPaths = QIcon::themeSearchPaths();
    const QString themeName = QIcon::themeName();
    QIcon::setThemeSearchPaths({index.path(), QDir(installPrefix()).filePath(QStringLiteral("share/icons"))});
    QIcon::setThemeName(QStringLiteral("hicolor"));
    const QImage shown = QIcon::fromTheme(QStringLiteral("ferrous")).pixmap(QSize(size, size), scale).toImage();
    QIcon::setThemeName(themeName);
    QIcon::setThemeSearchPaths(searchPaths);
    return shown.convertToFormat(QImage::Format_ARGB32);
}

} // namespace

class AppIconTest : public QObject {
    Q_OBJECT

private slots:
    void everySizeIsBuiltIn_data();
    void everySizeIsBuiltIn();
    void paletteHoldsApart();
    void drawingsAreFilledShapesInTheirColours_data() { addDrawingRows(); }
    void drawingsAreFilledShapesInTheirColours();
    void scalableDrawingKeepsADevicePixelRim();
    void tileSitsInBreezesMarginInsideItsRim_data() { addDrawingRows(); }
    void tileSitsInBreezesMarginInsideItsRim();
    void tracesRunFromOneStartOutToTheTilesEdge_data() { addDrawingRows(); }
    void tracesRunFromOneStartOutToTheTilesEdge();
    void tracesAreOneGlidesHarmonicSeries_data() { addDrawingRows(); }
    void tracesAreOneGlidesHarmonicSeries();
    void strokesTaperFromTheFundamental_data() { addDrawingRows(); }
    void strokesTaperFromTheFundamental();
    void fundamentalThinsSmoothlyWithSize();
    void detailTierAddsAFaint4thHarmonic_data();
    void detailTierAddsAFaint4thHarmonic();
    void doubledSlotsDrawTheir1xCompositionAtTwice_data();
    void doubledSlotsDrawTheir1xCompositionAtTwice();
    void drawing24pxIsThe22pxMovedIn();
    void windowIconHasEachSizesDrawing();
    void everySizeInstalls_data();
    void everySizeInstalls();
    void noDrawingForA24pxSlotAt2x();
    void iconThemeGivesEachSizeItsDrawing_data();
    void iconThemeGivesEachSizeItsDrawing();
    void hicolorsOwnIndexPicksTheSlotsDrawings_data();
    void hicolorsOwnIndexPicksTheSlotsDrawings();
    void desktopFileNamesTheIcon();
    void commentsDescribeTheDrawingNotItsHistory_data() { addDrawingRows(); }
    void commentsDescribeTheDrawingNotItsHistory();
};

void AppIconTest::everySizeIsBuiltIn_data() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        QTest::addRow("%dpx", size) << size;
    }
}

void AppIconTest::everySizeIsBuiltIn() {
    QFETCH(int, size);
    const QString path = FerrousAppIcon::resourcePath(size);
    QVERIFY2(QFile::exists(path), qPrintable(path));
    // Drawn at its own size, as declared in the file.
    QImageReader reader(path);
    QCOMPARE(reader.size(), QSize(size, size));
    const QImage image = render(path, size);
    QVERIFY2(!image.isNull(), qPrintable(reader.errorString()));
    // Any other size gets the scalable drawing.
    QVERIFY(QFile::exists(FerrousAppIcon::resourcePath(0)));
    QCOMPARE(FerrousAppIcon::resourcePath(40), QStringLiteral(":/icons/hicolor/scalable/apps/ferrous.svg"));
}

// The colours' rules, which each drawing's fills follow
// (drawingsAreFilledShapesInTheirColours).
void AppIconTest::paletteHoldsApart() {
    const auto ratio = [](QRgb a, QRgb b) { return contrastRatio(QColor(a), QColor(b)); };
    // The rim holds the tile's edge on Breeze Dark's window and view, where
    // the indigo tile alone is 1.3:1 on the window, and the silhouette on
    // Breeze Light's; and it is quiet, 1.75:1 or less on the tile, quieter
    // than any trace, and no trace's ink.
    QVERIFY(ratio(kTile, kBreezeDarkWindow) < 1.5);
    QVERIFY2(ratio(kRim, kBreezeDarkWindow) >= 2.0, qPrintable(QString::number(ratio(kRim, kBreezeDarkWindow))));
    for (const QRgb panel : {kBreezeDarkWindow, kBreezeDarkView}) {
        QVERIFY2(ratio(kRim, panel) >= ratio(kTile, panel) + 0.5, qPrintable(hex(panel)));
    }
    for (const QRgb panel : {kBreezeLightWindow, kBreezeLightView}) {
        QVERIFY2(ratio(kRim, panel) >= 4.5, qPrintable(hex(panel)));
    }
    QVERIFY2(ratio(kRim, kTile) <= 1.75, qPrintable(QString::number(ratio(kRim, kTile))));
    QVERIFY(!traceInk(kRim | 0xff000000));

    // Each trace 3:1 or more on the tile, and a step of value from each to
    // the one above it, so that they hold apart in greyscale: the amber some
    // 6.6:1, the rose 5:1, the magenta 3.6:1, in Rec. 709 luma some 30 levels
    // apart each.
    for (const QRgb trace : {kGlide, kHarmonic, kThirdHarmonic, kPixelGridThirdHarmonic, kFourthHarmonic}) {
        QVERIFY2(ratio(trace, kTile) >= 3.0, qPrintable(hex(trace)));
        QVERIFY(ratio(kRim, kTile) < ratio(trace, kTile));
    }
    QVERIFY2(std::abs(ratio(kHarmonic, kTile) - 5.0) <= 0.1, qPrintable(QString::number(ratio(kHarmonic, kTile))));
    QVERIFY(ratio(kGlide, kTile) >= ratio(kHarmonic, kTile) + 1.5);
    QVERIFY(ratio(kHarmonic, kTile) >= ratio(kThirdHarmonic, kTile) + 1.2);
    QVERIFY2(luma(kHarmonic) - luma(kThirdHarmonic) >= 30, qPrintable(QString::number(luma(kHarmonic) - luma(kThirdHarmonic))));
    QVERIFY2(luma(kGlide) - luma(kHarmonic) >= 28, qPrintable(QString::number(luma(kGlide) - luma(kHarmonic))));

    // The pixel grid's magenta a step lighter, its hue kept, still under the
    // rose.
    QVERIFY(luma(kPixelGridThirdHarmonic) > luma(kThirdHarmonic) && luma(kPixelGridThirdHarmonic) < luma(kHarmonic));
    QVERIFY(std::abs(QColor(kPixelGridThirdHarmonic).hue() - QColor(kThirdHarmonic).hue()) <= 3);

    // The 4th harmonic faint: a step of value under the magenta, 3:1 on the
    // tile with little to spare, and 8 luma levels or more under the magenta;
    // yet light enough that its cut on the tile's top inner edge reads
    // against the rim, 1.7:1 or more on it.
    QVERIFY2(ratio(kFourthHarmonic, kTile) <= 3.1, qPrintable(QString::number(ratio(kFourthHarmonic, kTile))));
    QVERIFY(ratio(kThirdHarmonic, kTile) >= ratio(kFourthHarmonic, kTile) + 0.6);
    QVERIFY(luma(kThirdHarmonic) - luma(kFourthHarmonic) >= 8);
    QVERIFY2(ratio(kFourthHarmonic, kRim) >= 1.7, qPrintable(QString::number(ratio(kFourthHarmonic, kRim))));
}

// Each drawing is the tile's rim, the tile and its traces, in that order and
// in the palette's colours, each trace a filled outline, closed: on the pixel
// grid a polygon with every vertex on a whole pixel and every edge level,
// upright or at 45 degrees; else curves with one arc, the round cap of its
// start (a flat cut reads as clipped; the other end has no cap, it runs out
// to the tile's edge). The tile sits inside the rim by the rim's width.
void AppIconTest::drawingsAreFilledShapesInTheirColours() {
    QFETCH(Drawing, drawing);
    QXmlStreamReader svg(contentsOf(drawing.path));
    QStringList ids;
    QList<QRectF> rects;
    double scale = 0;
    static const QRegularExpression number(QStringLiteral("-?[0-9]*\\.?[0-9]+"));
    static const QRegularExpression polygonCommand(QStringLiteral("^M[-0-9 ]+Z$"));
    while (!svg.atEnd()) {
        if (svg.readNext() != QXmlStreamReader::StartElement) {
            continue;
        }
        const QXmlStreamAttributes attributes = svg.attributes();
        const auto value = [&](const char *name) { return attributes.value(QLatin1String(name)).toString(); };
        if (svg.name() == QLatin1String("svg")) {
            scale = drawing.device() / value("viewBox").split(QLatin1Char(' ')).value(2).toDouble();
            continue;
        }
        if (svg.name() == QLatin1String("rect")) {
            const QString id = value("id");
            QCOMPARE(id, rects.isEmpty() ? QStringLiteral("rim") : QStringLiteral("tile"));
            QCOMPARE(value("fill"), hex(id == QLatin1String("rim") ? kRim : kTile));
            rects << QRectF(value("x").toDouble(), value("y").toDouble(), value("width").toDouble(), value("height").toDouble());
            continue;
        }
        if (svg.name() != QLatin1String("path")) {
            continue;
        }
        ids << value("id");
        QCOMPARE(value("fill"), hex(drawing.colour(ids.last())));
        QVERIFY2(!attributes.hasAttribute(QLatin1String("stroke")), qPrintable(ids.last()));
        const QString d = value("d");
        if (!drawing.onPixelGrid()) {
            QVERIFY2(d.startsWith(QLatin1Char('M')) && d.endsWith(QLatin1Char('Z')) && d.count(QLatin1Char('A')) == 1
                         && d.contains(QLatin1Char('C')),
                     qPrintable(ids.last()));
            continue;
        }
        QVERIFY2(polygonCommand.match(d).hasMatch(), qPrintable(d));
        std::vector<int> values;
        for (const QRegularExpressionMatch &match : number.globalMatch(d)) {
            values.push_back(match.captured().toInt());
        }
        QVERIFY(values.size() >= 8 && values.size() % 2 == 0);
        const size_t vertices = values.size() / 2;
        for (size_t i = 0; i < vertices; ++i) {
            const size_t j = (i + 1) % vertices;
            const int dx = std::abs(values[2 * j] - values[2 * i]);
            const int dy = std::abs(values[2 * j + 1] - values[2 * i + 1]);
            QVERIFY2(dx == 0 || dy == 0 || dx == dy, qPrintable(QStringLiteral("%1: an edge %2 by %3").arg(ids.last()).arg(dx).arg(dy)));
        }
    }
    QVERIFY2(!svg.hasError(), qPrintable(svg.errorString()));
    QCOMPARE(ids, drawing.ids());
    QCOMPARE(rects.size(), 2);
    const double inset = drawing.rim / scale;
    QVERIFY2(rects[1] == rects[0].adjusted(inset, inset, -inset, -inset),
             qPrintable(QStringLiteral("the tile %1,%2 %3x%4 in the rim")
                            .arg(rects[1].x()).arg(rects[1].y()).arg(rects[1].width()).arg(rects[1].height())));
}

// The scalable drawing serves the slots no drawing of the theme's matches,
// 40 to 60 device px or so: there its rim of 1.5 units of its 64 is about a
// device pixel (a 64th of the icon is 0.6 to 0.9 px, a smudge rather than an
// edge).
void AppIconTest::scalableDrawingKeepsADevicePixelRim() {
    const QString path = FerrousAppIcon::resourcePath(96);
    QCOMPARE(path, QStringLiteral(":/icons/hicolor/scalable/apps/ferrous.svg"));
    const auto shapes = shapesOf(path);
    QVERIFY(shapes.size() >= 2 && shapes[0].first == QLatin1String("rect#rim") && shapes[1].first == QLatin1String("rect#tile"));
    const double rim = shapes[1].second.front().value - shapes[0].second.front().value;
    QVERIFY2(rim * 40 / 64 >= 0.9 && rim * 60 / 64 <= 1.5, qPrintable(QString::number(rim)));
}

// The tile in Breeze's app-icon margin (breezeMargin, twice it at 2x),
// centred, with its rim just inside its edge, the rim's width in the rim's
// colour on every side, then the tile's own; nothing else on the tile in the
// rim's colour, away from its rounded corners.
void AppIconTest::tileSitsInBreezesMarginInsideItsRim() {
    QFETCH(Drawing, drawing);
    const QImage image = render(drawing.path, drawing.device());
    const QRect tile = tileOf(image);
    const int margin = breezeMargin(drawing.slot) * drawing.scale;
    QCOMPARE(tile, image.rect().adjusted(margin, margin, -margin, -margin));
    const QPoint middle = tile.center();
    const QPoint inwards[] = {QPoint(0, 1), QPoint(0, -1), QPoint(1, 0), QPoint(-1, 0)};
    const QPoint edges[] = {QPoint(middle.x(), tile.top()), QPoint(middle.x(), tile.bottom()), QPoint(tile.left(), middle.y()),
                            QPoint(tile.right(), middle.y())};
    for (int side = 0; side < 4; ++side) {
        for (int step = 0; step <= drawing.rim; ++step) {
            const QPoint p = edges[side] + inwards[side] * step;
            QVERIFY2(near(image.pixel(p), kRim, step < drawing.rim ? 2 : 24) == (step < drawing.rim),
                     qPrintable(QStringLiteral("%1 at %2, %3, %4 px in").arg(hex(image.pixel(p))).arg(p.x()).arg(p.y()).arg(step)));
        }
    }
    QVERIFY(near(image.pixel(middle.x(), tile.bottom() - drawing.rim), kTile, 2));
    // A trace's faint antialiased edge over the tile comes within 24 of the
    // rim in each channel, though never within 12.
    const QRect inside = insideRim(image, drawing);
    const int corner = drawing.device() / 16 + 1;
    for (const QRect &band : {inside.adjusted(corner, 0, -corner, 0), inside.adjusted(0, corner, 0, -corner)}) {
        QCOMPARE(countWhere(image, band, [](QRgb p) { return near(p, kRim, 12); }), 0);
    }
}

// The traces: separate shapes, each of its own colour 3:1 or more on the
// tile as rendered, and nothing else on the tile (a floor drawn under them
// reads as an underline or a progress bar). They start on one time line, in
// the tile's left fifth, clear of the rim, and run out flush to the tile's
// inner edge on the right with the rim intact, a crop of an endless
// spectrogram (series ending at a common x in round caps inside the tile read
// as a line chart): each one's last column as covered as the one before it,
// on the pixel grid its stroke's whole height. Apart everywhere, and clear of
// the rim: a row of the tile or more under the lowest trace and over the top
// one. The fundamental bends over: nothing of it in the tile's top-right
// corner.
void AppIconTest::tracesRunFromOneStartOutToTheTilesEdge() {
    QFETCH(Drawing, drawing);
    const int size = drawing.device();
    const QImage image = render(drawing.path, size);
    const QRect tile = tileOf(image);
    const QRect inside = insideRim(image, drawing);
    const QStringList ids = drawing.ids();
    const std::vector<QImage> traces = tracesOf(drawing, ids);

    // Separate traces, each its colour's ink at its core.
    const std::vector<QRgb> cores = traceCores(image);
    QCOMPARE(cores.size(), traces.size());
    for (size_t n = 0; n < cores.size(); ++n) {
        const QRgb colour = drawing.colour(ids[ids.size() - 1 - static_cast<qsizetype>(n)]);
        QVERIFY2(channelDistance(QColor(cores[n]), QColor(colour)) <= 48 && contrastRatio(QColor(cores[n]), QColor(kTile)) >= 3.0,
                 qPrintable(QStringLiteral("trace %1 is %2 at most, not %3").arg(n).arg(hex(cores[n]), hex(colour))));
    }

    // Nothing on the tile but the traces: inside the rim, away from its
    // rounded corners, each pixel the tile under the traces' coverage.
    const int corner = std::max(2, size / 8);
    std::vector<QRgb> colours;
    for (auto id = ids.crbegin(); id != ids.crend(); ++id) {
        colours.push_back(drawing.colour(*id));
    }
    const auto composited = [&](int x, int y) {
        std::array<double, 3> channels{double(qRed(kTile)), double(qGreen(kTile)), double(qBlue(kTile))};
        for (size_t n = traces.size(); n-- > 0;) {
            const double a = alphaAt(traces[n], x, y);
            const std::array<int, 3> colour{qRed(colours[n]), qGreen(colours[n]), qBlue(colours[n])};
            for (size_t c = 0; c < 3; ++c) {
                channels[c] += a * (colour[c] - channels[c]);
            }
        }
        return qRgb(qRound(channels[0]), qRound(channels[1]), qRound(channels[2]));
    };
    const auto onlyTraces = firstFailing(inside, [&](int x, int y) {
        const bool inCorner = (x < inside.left() + corner || x > inside.right() - corner)
                              && (y < inside.top() + corner || y > inside.bottom() - corner);
        return inCorner || near(image.pixel(x, y), composited(x, y), 4);
    });
    // QVERIFY2 builds its message even when the check holds.
    if (onlyTraces) {
        QFAIL(qPrintable(QStringLiteral("%1 %2, not %3")
                             .arg(hex(image.pixel(*onlyTraces)), at(onlyTraces),
                                  hex(composited(onlyTraces->x(), onlyTraces->y())))));
    }

    const int capSlack = drawing.onPixelGrid() ? 0 : 1 + size / 64;
    const int start = columnsOf(traces.front()).first;
    int top = size;
    int bottom = -1;
    for (size_t n = 0; n < traces.size(); ++n) {
        const QString &id = ids[ids.size() - 1 - static_cast<qsizetype>(n)];
        const QImage &trace = traces[n];
        // Inside the rim.
        const auto outside = firstFailing(image.rect(), [&](int x, int y) { return inside.contains(x, y) || alphaAt(trace, x, y) <= 0.01; });
        QVERIFY2(!outside, qPrintable(QStringLiteral("%1 %2").arg(id, at(outside))));
        // From one start, clear of the rim, in the tile's left fifth.
        const auto [first, last] = columnsOf(trace);
        QVERIFY2(std::abs(first - start) <= capSlack && first > inside.left() && first <= tile.left() + tile.width() / 5,
                 qPrintable(QStringLiteral("%1 starts at column %2, the fundamental at %3").arg(id).arg(first).arg(start)));
        for (int x = first; x <= last; ++x) {
            const Column column = columnOf(trace, x);
            if (column.top >= 0) {
                top = id == QLatin1String("h4") ? top : std::min(top, column.top);
                bottom = std::max(bottom, column.bottom);
            }
        }
        // The series runs out flush to the tile's inner edge (the 4th
        // harmonic through its top edge: detailTierAddsAFaint4thHarmonic).
        if (id == QLatin1String("h4")) {
            continue;
        }
        QVERIFY2(last == inside.right(), qPrintable(QStringLiteral("%1 ends at column %2").arg(id).arg(last)));
        const Column end = columnOf(trace, last);
        const double before = columnOf(trace, last - 1).sum;
        if (drawing.onPixelGrid()) {
            const int height = static_cast<int>(drawing.strokeWidths()[n]);
            QVERIFY2(end.bottom - end.top + 1 == height && std::abs(end.sum - height) <= 0.05,
                     qPrintable(QStringLiteral("%1: %2 px at its end").arg(id).arg(end.sum)));
        } else {
            QVERIFY2(end.sum >= 0.9 * before, qPrintable(QStringLiteral("%1: %2 px at its end, %3 before").arg(id).arg(end.sum).arg(before)));
        }
    }
    QVERIFY2(top > inside.top() && bottom < inside.bottom(),
             qPrintable(QStringLiteral("rows %1 to %2 inside %3 to %4").arg(top).arg(bottom).arg(inside.top()).arg(inside.bottom())));

    // Apart: no pixel two neighbours touch, and in every column both cross
    // past their first, on the pixel grid a clear row between them, else
    // 1.5 px or more of the tile between their centres, from what each leaves
    // of a row.
    for (size_t n = 1; n < traces.size(); ++n) {
        for (int x = std::max(columnsOf(traces[n - 1]).first, columnsOf(traces[n]).first) + 1; x <= inside.right(); ++x) {
            const Column under = columnOf(traces[n - 1], x);
            const Column over = columnOf(traces[n], x);
            double clear = 0;
            bool meet = false;
            for (int y = inside.top(); y <= inside.bottom(); ++y) {
                const double a = alphaAt(traces[n], x, y);
                const double b = alphaAt(traces[n - 1], x, y);
                meet = meet || (a > kTouched && b > kTouched);
                if (y >= static_cast<int>(over.centre) && y <= static_cast<int>(under.centre)) {
                    clear += std::max(0.0, 1 - a - b);
                }
            }
            QVERIFY2(!meet, qPrintable(QStringLiteral("traces %1 and %2 meet in column %3").arg(n - 1).arg(n).arg(x)));
            QVERIFY2(drawing.onPixelGrid() ? under.top - over.bottom > 1 : clear >= 1.5,
                     qPrintable(QStringLiteral("traces %1 and %2, column %3: rows %4 and %5, %6 px of the tile between")
                                    .arg(n - 1).arg(n).arg(x).arg(over.bottom).arg(under.top).arg(clear)));
        }
    }

    // The top of the series clear of the tile's rounded top-right corner,
    // where Plasma draws a task's badge: from 32 px, in each of its last 4
    // columns, 6 % of the tile's height or more of the tile over it, the
    // rim's corner counted out (a row or so, and it runs into the corner).
    if (drawing.slot >= 32) {
        const QImage tileAlone = renderLines(drawing.path, size, [](const QString &line) {
            return !isShape(line) || line.contains(QLatin1String("<rect id=\"tile\""));
        });
        const QImage &topTrace = traces[drawing.seriesIds().size() - 1];
        for (int x = inside.right() - 3; x <= inside.right(); ++x) {
            double air = 0;
            for (int y = 0; y <= columnOf(topTrace, x).top; ++y) {
                air += std::max(0.0, alphaAt(tileAlone, x, y) - alphaAt(topTrace, x, y));
            }
            QVERIFY2(air >= 0.06 * inside.height(), qPrintable(QStringLiteral("column %1: %2 px of the tile over the top trace").arg(x).arg(air)));
        }
    }

    QCOMPARE(countWhere(image, QRect(tile.right() + 1 - tile.width() / 5, tile.top(), tile.width() / 5, tile.width() / 5),
                        [](QRgb p) { return near(p, kGlide, 48); }),
             0);
}

// One glide and its harmonics on a linear frequency axis: each harmonic's
// frequency the glide's times its number, so that from the common start at
// the lower left they rise 1 : 2 : 3 and fan out across the tile's diagonal
// (one curve shifted up a log axis is parallel strokes, which read as
// swooshes or a trend line). Each rises its number times as far as the
// fundamental, to within the grid's rounding (doubled at 2x); neighbours grow
// apart from the start to the end by 45 % or more, measured past the caps,
// where they have opened already (by a third, they read as parallel
// stripes); the 2nd harmonic is midway between its neighbours in every
// column. A glide: steep at first, bending over, the top harmonic rising
// twice as far or more in its first third as in its last, three tenths of
// the tile or more in all; none falls. They fill the
// plate along its diagonal: 70 % of the tile each way, with ink in its
// bottom-left, top-left and top-right quarters.
void AppIconTest::tracesAreOneGlidesHarmonicSeries() {
    QFETCH(Drawing, drawing);
    const int size = drawing.device();
    const QImage image = render(drawing.path, size);
    const QRect tile = tileOf(image);
    const QRect inside = insideRim(image, drawing);
    const QStringList ids = drawing.seriesIds();
    const std::vector<QImage> traces = tracesOf(drawing, ids);
    const auto centre = [&](size_t n, int x) { return columnOf(traces[n], x).centre; };
    const auto number = [&](size_t n) { return Drawing::number(ids[ids.size() - 1 - static_cast<qsizetype>(n)]); };

    // Past the caps, to the column before the tile's edge.
    int start = 0;
    for (const QImage &trace : traces) {
        start = std::max(start, columnsOf(trace).first);
    }
    start += drawing.capColumns();
    const int end = inside.right() - 1;
    const double slack = std::max(0.75, size / 48.0);

    const double fundamentalRise = centre(0, start) - centre(0, end);
    QVERIFY2(fundamentalRise >= 1.5, qPrintable(QString::number(fundamentalRise)));
    for (size_t n = 1; n < traces.size(); ++n) {
        const double rise = centre(n, start) - centre(n, end);
        QVERIFY2(std::abs(rise / number(n) - fundamentalRise) <= std::max(1.25, slack) * drawing.scale,
                 qPrintable(QStringLiteral("harmonic %1 rises %2, the fundamental %3").arg(number(n)).arg(rise).arg(fundamentalRise)));
        const auto apart = [&](int x) { return centre(n - 1, x) - centre(n, x); };
        QVERIFY2(apart(end) >= 1.45 * apart(start),
                 qPrintable(QStringLiteral("trace %1: %2 apart at the start, %3 at the end").arg(n).arg(apart(start)).arg(apart(end))));
        for (int x = start + 1; x <= end; ++x) {
            QVERIFY2(apart(x) >= apart(x - 1) - std::max(1.0, slack),
                     qPrintable(QStringLiteral("trace %1 at column %2: %3 after %4").arg(n).arg(x).arg(apart(x)).arg(apart(x - 1))));
        }
    }
    if (traces.size() == 3) {
        for (int x = start; x <= end; ++x) {
            const double off = centre(1, x) - (centre(0, x) + centre(2, x)) / 2;
            QVERIFY2(std::abs(off) <= std::max(1.25, slack) * drawing.scale,
                     qPrintable(QStringLiteral("column %1: the 2nd harmonic %2 off the middle").arg(x).arg(off)));
        }
    }

    const size_t top = traces.size() - 1;
    const int third = (end - start) / 3;
    const double first = centre(top, start) - centre(top, start + third);
    const double last = centre(top, end - third) - centre(top, end);
    QVERIFY2(last > 0 && first >= 2.0 * last, qPrintable(QStringLiteral("%1 then %2").arg(first).arg(last)));
    QVERIFY2(centre(top, start) - centre(top, end) >= 0.29 * inside.height(), qPrintable(QString::number(centre(top, start) - centre(top, end))));
    for (size_t n = 0; n < traces.size(); ++n) {
        for (int x = start + 1; x <= end; ++x) {
            QVERIFY2(centre(n, x) <= centre(n, x - 1) + 0.25, qPrintable(QStringLiteral("trace %1 at %2").arg(n).arg(x)));
        }
    }

    const QRect ink = boundsWhere(image, traceInk);
    QVERIFY2(ink.width() >= tile.width() * 0.7 && ink.height() >= tile.height() * 0.7,
             qPrintable(QStringLiteral("%1x%2 of %3").arg(ink.width()).arg(ink.height()).arg(tile.width())));
    const int half = tile.width() / 2;
    QVERIFY(countWhere(image, QRect(tile.left(), tile.top(), half, half), traceInk) > 0);
    QVERIFY(countWhere(image, QRect(tile.right() + 1 - half, tile.top(), half, half), traceInk) > 0);
    QVERIFY(countWhere(image, QRect(tile.left(), tile.bottom() + 1 - half, half, half), traceInk) > 0);
}

// The strokes taper from the fundamental up, as a harmonic's energy falls
// with its number, in their widths (Drawing::strokeWidths): from 32 px the
// fundamental the focal stroke, half as heavy again as the rose over it or
// more and three quarters again as the magenta. On the pixel grid each trace
// is one run of whole pixels in every column, its height or a row more where
// it climbs, the climbing edges half covered, climbing a row a column at
// most, crisp in a third of the fundamental's columns; and its magenta's
// climbs render as bright as the 2x drawings' smooth magenta, some 3.3:1 or
// more on the tile, its flats well under the amber. Smooth strokes are curves
// of an even weight: past the caps no stair, as whole-pixel steps would read
// beside Breeze's icons, never under 2 px across at 1x or 4 at 2x (a thinner
// stroke beads and dims), and a pixel of the stroke's whole colour in every
// column.
void AppIconTest::strokesTaperFromTheFundamental() {
    QFETCH(Drawing, drawing);
    const int size = drawing.device();
    const QRect inside = insideRim(render(drawing.path, size), drawing);
    const QStringList ids = drawing.seriesIds();
    const std::vector<QImage> traces = tracesOf(drawing, ids);
    const std::vector<double> widths = drawing.strokeWidths();

    if (drawing.onPixelGrid()) {
        for (size_t n = 0; n < traces.size(); ++n) {
            const auto [first, last] = columnsOf(traces[n]);
            int crisp = 0;
            double before = -1;
            for (int x = first; x <= last; ++x) {
                const Column column = columnOf(traces[n], x);
                bool whole = true;
                for (int y = column.top; y <= column.bottom; ++y) {
                    const double c = alphaAt(traces[n], x, y);
                    const bool edge = y == column.top || y == column.bottom;
                    QVERIFY2(c > 0.97 || (edge && std::abs(c - 0.5) <= 0.1),
                             qPrintable(QStringLiteral("trace %1 at %2, %3: %4").arg(n).arg(x).arg(y).arg(c)));
                    whole = whole && c > 0.97;
                }
                crisp += whole ? 1 : 0;
                // The starts: at 16 px the fundamental 2 px tall over its first
                // two columns, a clear row under the harmonic's start, and 3 px
                // from its 4th (at 2 px all along the amber reads lighter than
                // the magenta over it; at 3 px from its start it is a block).
                // The harmonic starts on its first climb, as wide across its
                // 45 degree slope as on its flats: at 22 and 24 px 3 rows tall
                // over its first 5 columns (at two rows, 1.4 px across, the
                // climb reads thin beside the amber), at 16 px 2 rows over 3.
                const int offset = x - first;
                double height = widths[n];
                double extra = 1.05;
                if (drawing.slot == 16 && n == 0 && offset < 3) {
                    height = 2;
                    extra = offset < 2 ? 0.05 : 1.05;
                } else if (n == 1 && offset < (drawing.slot == 16 ? 3 : 5)) {
                    height = drawing.slot == 16 ? 2 : 3;
                    extra = 0.05;
                }
                QVERIFY2(column.sum >= height - 0.05 && column.sum <= height + extra,
                         qPrintable(QStringLiteral("trace %1, column %2: %3 px tall").arg(n).arg(x).arg(column.sum)));
                QVERIFY2(before < 0 || (column.centre <= before + 0.01 && before - column.centre <= 1.01),
                         qPrintable(QStringLiteral("trace %1, column %2: from %3 to %4").arg(n).arg(x).arg(before).arg(column.centre)));
                before = column.centre;
            }
            QVERIFY2(crisp >= (n == 0 ? (last - first + 1) / 3 : 3), qPrintable(QStringLiteral("trace %1: %2 crisp columns").arg(n).arg(crisp)));
        }
        static const QRegularExpression fill(QStringLiteral("<path id=\"h3\" fill=\"#([0-9a-f]{6})\""));
        const QRgb magenta = fill.match(QString::fromUtf8(contentsOf(drawing.path))).captured(1).toUInt(nullptr, 16);
        const auto [climbs, flats] = magentaBrightness(traces.back(), magenta);
        QVERIFY2(climbs >= 3.25, qPrintable(QStringLiteral("its climbs at %1:1").arg(climbs)));
        QVERIFY2(flats <= contrastRatio(QColor(kGlide), QColor(kTile)) - 2.0, qPrintable(QStringLiteral("its flats at %1:1").arg(flats)));
        return;
    }

    const int start = columnsOf(traces.front()).first + drawing.capColumns();
    const int end = inside.right() - 1;
    std::vector<double> across;
    for (size_t n = 0; n < traces.size(); ++n) {
        const QImage &trace = traces[n];
        across.push_back(widthAcross(trace, start, end));
        QVERIFY2(std::abs(across.back() - widths[n]) <= 0.08 * widths[n],
                 qPrintable(QStringLiteral("trace %1: %2 px across, not %3").arg(n).arg(across.back()).arg(widths[n])));
        const int body = std::max(start, columnsOf(trace).first + drawing.capColumns());
        for (int x = body; x < inside.right(); ++x) {
            const double width = widthAcross(trace, x, x);
            QVERIFY2(width >= 1.9 * drawing.scale, qPrintable(QStringLiteral("trace %1, column %2: %3 px across").arg(n).arg(x).arg(width)));
            double peak = 0;
            for (int y = inside.top(); y <= inside.bottom(); ++y) {
                peak = std::max(peak, alphaAt(trace, x, y));
            }
            QVERIFY2(peak >= 0.9, qPrintable(QStringLiteral("trace %1, column %2: %3 of a pixel at most").arg(n).arg(x).arg(peak)));
            const double bend = columnOf(trace, x + 1).centre - 2 * columnOf(trace, x).centre + columnOf(trace, x - 1).centre;
            QVERIFY2(x == body || std::abs(bend) <= 0.3, qPrintable(QStringLiteral("trace %1, column %2: bends %3").arg(n).arg(x).arg(bend)));
        }
    }
    if (across.size() >= 3) {
        QVERIFY2(across[0] >= 1.5 * across[1] && across[0] >= 1.75 * across[2],
                 qPrintable(QStringLiteral("%1, %2 and %3 px across").arg(across[0]).arg(across[1]).arg(across[2])));
    }
}

// The ink thins smoothly as the icon grows: the fundamental's width across,
// a share of the tile inside the rim, falls from each size to the next, so
// that no size reads bolder than a smaller one. From 22 px: at 16 px the
// grid's 2 px is the least a stroke can be, and 22 px's 3 px a larger share.
// The 24 px drawing is the 22 px one (drawing24pxIsThe22pxMovedIn).
void AppIconTest::fundamentalThinsSmoothlyWithSize() {
    double before = -1;
    for (const int size : FerrousAppIcon::kSizes) {
        if (size == 16 || size == 24) {
            continue;
        }
        const Drawing drawing{FerrousAppIcon::resourcePath(size), size, 1, 1};
        const QRect inside = insideRim(render(drawing.path, size), drawing);
        const QImage fundamental = traceCoverage(drawing.path, size).value(QStringLiteral("fundamental"));
        const auto [first, last] = columnsOf(fundamental);
        const double share = widthAcross(fundamental, first + drawing.capColumns(), last - 1) / inside.width();
        QVERIFY2(before < 0 || (share < before && share > before * 0.6),
                 qPrintable(QStringLiteral("%1 px: %2 of the tile after %3").arg(size).arg(share).arg(before)));
        before = share;
    }
}

void AppIconTest::detailTierAddsAFaint4thHarmonic_data() {
    QTest::addColumn<int>("size");
    QTest::addRow("128px") << 128;
    QTest::addRow("256px") << 256;
}

// The detail tier's 4th harmonic, in faint violet, half the width the
// harmonics taper from: from the common start, a true 4th harmonic of the
// same glide, as far above the 3rd as the 3rd is above the 2nd in every
// column to within a quarter unit of the 64-unit drawing, flattening as the
// others do (bent up at its end, it would be the only reversed curvature).
// The tile's top inner edge crops it as its right one does the others, the
// stroke run on along its curve past the edge: it meets the edge at the
// series' slope there, some 25 degrees, so that its cut along the edge is
// between twice and three times its width across, the tile's top row covered
// along the cut with no wedge of the tile under it (a cut square across the
// stroke leaves a corner on the rim), and nothing of it along the edge
// elsewhere. Nothing of it in the tile's rounded top-right corner, where
// Plasma draws a task's badge (x over 51 units, y under 20).
void AppIconTest::detailTierAddsAFaint4thHarmonic() {
    QFETCH(int, size);
    const Drawing drawing{FerrousAppIcon::resourcePath(size), size, 1, 1};
    const double unit = size / 64.0;
    const QRect inside = insideRim(render(drawing.path, size), drawing);
    const QHash<QString, QImage> cover = traceCoverage(drawing.path, size);
    const QImage &fourth = cover.value(QStringLiteral("h4"));
    const QImage &third = cover.value(QStringLiteral("h3"));
    const QImage &second = cover.value(QStringLiteral("h2"));
    const auto centre = [](const QImage &trace, int x) { return columnOf(trace, x).centre; };

    // Its cut on the top inner edge: the columns whose top row it covers by
    // half or more, from that row down.
    int cutFrom = -1;
    int cutTo = -1;
    double cut = 0;
    for (int x = inside.left(); x <= inside.right(); ++x) {
        const double c = alphaAt(fourth, x, inside.top());
        cut += c;
        if (c >= 0.5) {
            cutFrom = cutFrom < 0 ? x : cutFrom;
            cutTo = x;
        }
    }
    QVERIFY(cutFrom >= 0);
    for (int x = inside.left(); x <= inside.right(); ++x) {
        const double c = alphaAt(fourth, x, inside.top());
        if (x >= cutFrom && x <= cutTo) {
            QVERIFY2(c >= 0.5 && columnOf(fourth, x).top == inside.top(),
                     qPrintable(QStringLiteral("column %1: from row %2, %3 of its top row").arg(x).arg(columnOf(fourth, x).top).arg(c)));
        } else {
            // But the antialiased pixels where its upper edge comes in.
            QVERIFY2(c <= kTouched || (x >= cutFrom - 3 && x <= cutTo + 2),
                     qPrintable(QStringLiteral("column %1: %2 of the top row, the cut from %3 to %4").arg(x).arg(c).arg(cutFrom).arg(cutTo)));
        }
    }
    const double width = 3.4 / 2 * unit;
    QVERIFY2(cut >= 2 * width && cut <= 3 * width, qPrintable(QStringLiteral("a %1 px cut, a %2 px stroke").arg(cut).arg(width)));
    const QRect badge(QPoint(static_cast<int>(51 * unit), 0), QPoint(size - 1, static_cast<int>(20 * unit) - 1));
    const auto inBadge = firstFailing(badge, [&](int x, int y) { return alphaAt(fourth, x, y) <= kTouched; });
    QVERIFY2(!inBadge, qPrintable(at(inBadge)));

    // On the series from past the caps to where its upper edge comes in to
    // the cut.
    const int start = std::max(columnsOf(fourth).first, columnsOf(cover.value(QStringLiteral("fundamental"))).first) + drawing.capColumns();
    const int to = cutFrom - 3;
    for (int x = start; x < to; ++x) {
        const double gap = centre(third, x) - centre(fourth, x);
        const double series = centre(second, x) - centre(third, x);
        QVERIFY2(std::abs(gap - series) <= 0.25 * unit,
                 qPrintable(QStringLiteral("column %1: %2 above the 3rd, which is %3 above the 2nd").arg(x).arg(gap).arg(series)));
    }
    const int quarter = (to - start) / 4;
    double before = size;
    for (int q = 0; q < 4; ++q) {
        const double rise = centre(fourth, start + q * quarter) - centre(fourth, start + (q + 1) * quarter);
        QVERIFY2(rise > 0 && rise < before, qPrintable(QStringLiteral("quarter %1: rises %2 after %3").arg(q).arg(rise).arg(before)));
        before = rise;
    }
    const double across = widthAcross(fourth, start, start + (to - start) * 2 / 3);
    QVERIFY2(std::abs(across - width) <= 0.12 * width, qPrintable(QStringLiteral("%1 px across, not %2").arg(across).arg(width)));
}

void AppIconTest::doubledSlotsDrawTheir1xCompositionAtTwice_data() {
    QTest::addColumn<int>("size");
    for (const int size : kDoubledSizes) {
        QTest::addRow("%dpx@2", size) << size;
    }
}

// A slot at 2x scaling shows the drawing it shows at 1x, doubled (otherwise
// the theme falls back to a larger size's drawing, with more traces or a
// lighter composition, or to the scalable one, so that a slot's icon changes
// with the scale): twice the 1x tile, and each trace from twice its 1x start
// to the tile's inner edge, its end centred on twice the 1x end's rows. From
// 32 to 48 px every coordinate and width of the file is twice the 1x file's,
// to within the files' rounding, the arcs' flags kept. (The 16 and 22 px
// slots' are smooth curves of their 1x strokes' heights doubled, the 64 px
// slot's the 64 px strokes doubled: strokesTaperFromTheFundamental.)
void AppIconTest::doubledSlotsDrawTheir1xCompositionAtTwice() {
    QFETCH(int, size);
    const Drawing drawing{doubledPath(size), size, 2, 2};
    const Drawing single{FerrousAppIcon::resourcePath(size), size, 1, 1};
    QVERIFY2(QFile::exists(drawing.path), qPrintable(drawing.path));
    QCOMPARE(QImageReader(drawing.path).size(), QSize(2 * size, 2 * size));
    const QImage image = render(drawing.path, 2 * size);
    const QImage singleImage = render(single.path, size);
    const QRect singleTile = tileOf(singleImage);
    QCOMPARE(tileOf(image), QRect(singleTile.topLeft() * 2, singleTile.size() * 2));

    const QRect inside = insideRim(image, drawing);
    const QHash<QString, QImage> cover = traceCoverage(drawing.path, 2 * size);
    const QHash<QString, QImage> singleCover = traceCoverage(single.path, size);
    for (const QString &id : drawing.seriesIds()) {
        const QImage &trace = cover.value(id);
        const QImage &singleTrace = singleCover.value(id);
        const auto [first, last] = columnsOf(trace);
        const auto [singleFirst, singleLast] = columnsOf(singleTrace);
        QVERIFY2(std::abs(first - 2 * singleFirst) <= 1,
                 qPrintable(QStringLiteral("%1 from %2, the 1x from %3").arg(id).arg(first).arg(singleFirst)));
        QCOMPARE(last, inside.right());
        QCOMPARE(last + 1, 2 * (singleLast + 1));
        const double endCentre = columnOf(trace, last).centre;
        const double singleEnd = columnOf(singleTrace, singleLast).centre;
        QVERIFY2(std::abs(endCentre - 2 * singleEnd) <= 0.3,
                 qPrintable(QStringLiteral("%1 ends at %2, the 1x at %3").arg(id).arg(endCentre).arg(singleEnd)));
    }

    if (size < 32 || size > 48) {
        return;
    }
    const auto shapes = shapesOf(drawing.path);
    const auto singleShapes = shapesOf(single.path);
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
}

// The 24 px drawing is the 22 px one moved a pixel in on a 24 px canvas, as
// Breeze draws its 24 px icons, for the window icon's 24 px slot (otherwise
// the slot takes the 32 px drawing scaled down, three thin traces, or the
// 22 px one scaled up): pixel for pixel the same, moved, with nothing else on
// the canvas.
void AppIconTest::drawing24pxIsThe22pxMovedIn() {
    QCOMPARE(QImageReader(FerrousAppIcon::resourcePath(24)).size(), QSize(24, 24));
    const QImage image = render(FerrousAppIcon::resourcePath(24), 24);
    QImage moved(24, 24, QImage::Format_ARGB32);
    moved.fill(Qt::transparent);
    const QImage source = render(FerrousAppIcon::resourcePath(22), 22);
    for (int y = 0; y < 22; ++y) {
        for (int x = 0; x < 22; ++x) {
            moved.setPixel(x + 1, y + 1, source.pixel(x, y));
        }
    }
    QVERIFY2(maxDifference(image, moved) <= 2, qPrintable(QString::number(maxDifference(image, moved))));
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
    addDrawingRows();
}

// Each drawing installs in its hicolor directory, the built-in file.
void AppIconTest::everySizeInstalls() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QFETCH(Drawing, drawing);
    const QString path = installedIcon(drawing.path.section(QLatin1Char('/'), 3, 3));
    QVERIFY2(QFile::exists(path), qPrintable(path));
    const QByteArray installed = contentsOf(path);
    QVERIFY(!installed.isEmpty());
    QCOMPARE(installed, contentsOf(drawing.path));
}

// No 24x24@2 drawing, built in or installed: the hicolor theme's 22 px
// directories serve 20 to 24 px and come first, so a 24 px slot at 2x takes
// 22x22@2 (hicolorsOwnIndexPicksTheSlotsDrawings), and the window icon has
// 1x pixmaps only. The 24 px drawing stays, for the window icon's 24 px slot
// (windowIconHasEachSizesDrawing), and says so, so that no one takes it for
// what the icon theme shows at 24 px.
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

// With an index of each fixed size and its 2x, the theme gives each slot
// its drawing at 1x, and at 2x the slot's drawing at 2x.
void AppIconTest::iconThemeGivesEachSizeItsDrawing() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QFETCH(int, size);
    QFETCH(int, scale);
    QList<std::pair<QString, QString>> directories;
    for (const int fixed : FerrousAppIcon::kSizes) {
        directories.push_back({QStringLiteral("%1x%1/apps").arg(fixed), QStringLiteral("Size=%1\nType=Fixed\n").arg(fixed)});
        directories.push_back({QStringLiteral("%1x%1@2/apps").arg(fixed), QStringLiteral("Size=%1\nScale=2\nType=Fixed\n").arg(fixed)});
    }
    const QImage shown = themeIconAt(directories, size, scale);
    const QImage expected = render(installedIcon((scale == 1 ? QStringLiteral("%1x%1") : QStringLiteral("%1x%1@2")).arg(size)), size * scale);
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
// drawing. A 24 px slot takes the 22 px drawing (the 24 px directories come
// after it), as Breeze's own application icons, which have no 24 px drawing,
// show theirs.
void AppIconTest::hicolorsOwnIndexPicksTheSlotsDrawings() {
    if (installPrefix().isEmpty()) {
        QSKIP("FERROUS_APP_ICON_INSTALL_PREFIX names no install; CTest installs one");
    }
    QFETCH(int, size);
    QFETCH(qreal, scale);
    QFETCH(QString, directory);
    QList<std::pair<QString, QString>> directories;
    for (const int fixed : {16, 22, 24, 32, 36, 48, 64, 72, 96, 128, 192, 256, 512}) {
        for (const QString &suffix : {QString(), QStringLiteral("@2")}) {
            QString keys = fixed >= 256 ? QStringLiteral("MinSize=64\nSize=%1\nMaxSize=%1\nType=Scalable\n").arg(fixed)
                                        : QStringLiteral("Size=%1\nType=Threshold\n").arg(fixed);
            if (!suffix.isEmpty()) {
                keys += QStringLiteral("Scale=2\n");
            }
            directories.push_back({QStringLiteral("%1x%1%2/apps").arg(fixed).arg(suffix), keys});
        }
    }
    const QImage shown = themeIconAt(directories, size, scale);
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

// Each drawing's comment says how it is built, the rule its size follows and
// which slot it serves, and nothing of the design rounds it came from: the
// drawings ship, and are judged blind.
void AppIconTest::commentsDescribeTheDrawingNotItsHistory() {
    QFETCH(Drawing, drawing);
    const QString source = QString::fromUtf8(contentsOf(drawing.path));
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
