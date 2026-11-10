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
#include <QScopeGuard>
#include <QTemporaryDir>
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
// The magenta of the H1 drawing's floor band. The band read as a stripe
// glued on and is gone; at 16 and 22 px a 1 px floor line in its colour is
// what the glide rises off (#373 hill climb).
constexpr QRgb kFloor = 0xa6006e;
constexpr QRgb kGlide = 0xffb000;
constexpr QRgb kHarmonic = 0xf0602a;

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

// At least half the glide's amber, over the tile: the glide's edge pixels
// count too.
bool glideInk(QRgb pixel) {
    return qRed(pixel) >= 150 && qGreen(pixel) >= 100;
}

// Either stroke over the tile, edge pixels included: the tile has almost no
// green, the glide and the harmonic plenty.
bool strokeInk(QRgb pixel) {
    return qAlpha(pixel) == 255 && qGreen(pixel) >= 48;
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
    void smallSizesRiseOffAFloor_data();
    void smallSizesRiseOffAFloor();
    void harmonicFrom32Px_data();
    void harmonicFrom32Px();
    void strokesRiseTogetherFromOneOrigin_data() { addSizeRows(); }
    void strokesRiseTogetherFromOneOrigin();
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

    // The tile fills most of the square, and the tile and the glide each
    // have pixels of their own colour. From 32 px up there is no floor: the
    // two strokes carry the drawing.
    const QRect all = image.rect();
    const int opaque = countWhere(image, all, [](QRgb p) { return qAlpha(p) == 255; });
    QVERIFY2(opaque >= size * size / 2, qPrintable(QString::number(opaque)));
    QVERIFY(countWhere(image, all, [](QRgb p) { return near(p, kTile, 4); }) >= size * size / 3);
    if (size >= 32) {
        QCOMPARE(countWhere(image, all, [](QRgb p) { return near(p, kFloor, 24); }), 0);
    }
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

void AppIconTest::smallSizesRiseOffAFloor_data() {
    QTest::addColumn<int>("size");
    QTest::addRow("16px") << 16;
    QTest::addRow("22px") << 22;
}

// At 16 and 22 px a lone glide read as a slash or a "no" mark, and a thin
// harmonic drawn as a line beside it smeared into it. There the drawing is
// set on the pixel grid: a heavy glide rising off a 1 px floor line, and the
// harmonic a crisp 1 px diagonal that leaves it (#373 hill climb).
void AppIconTest::smallSizesRiseOffAFloor() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const auto full = [](QRgb p) { return near(p, kGlide, 24); };

    // Heavy: every column through the glide's middle holds at least two
    // pixels of its full amber.
    const QRect ink = boundsWhere(image, full);
    QVERIFY(ink.isValid());
    QVERIFY2(ink.width() >= tile.width() / 2, qPrintable(QString::number(ink.width())));
    for (int x = ink.left() + ink.width() / 4; x <= ink.right() - ink.width() / 4; ++x) {
        const int count = countWhere(image, QRect(x, tile.top(), 1, tile.height()), full);
        QVERIFY2(count >= 2, qPrintable(QStringLiteral("column %1: %2").arg(x).arg(count)));
    }
    // It rises: its left end is lower than its right.
    const QRect left(ink.left(), tile.top(), 2, tile.height());
    const QRect right(ink.right() - 1, tile.top(), 2, tile.height());
    QVERIFY(boundsWhere(image.copy(left), glideInk).center().y()
            > boundsWhere(image.copy(right), glideInk).center().y());

    // The floor: one row of whole pixels across most of the tile, inset from
    // its sides, so a line and not a band; the glide's left end stands on it.
    const QRect floor = boundsWhere(image, [](QRgb p) { return near(p, kFloor, 16); });
    QVERIFY(floor.isValid());
    QCOMPARE(floor.height(), 1);
    QCOMPARE(countWhere(image, floor, [](QRgb p) { return near(p, kFloor, 16); }), floor.width());
    QVERIFY2(floor.width() >= tile.width() * 2 / 3, qPrintable(QString::number(floor.width())));
    QVERIFY(floor.left() > tile.left());
    QVERIFY(floor.right() < tile.right());
    QVERIFY2(floor.bottom() < tile.bottom() - 1, qPrintable(QString::number(floor.bottom())));
    QCOMPARE(ink.bottom(), floor.top() - 1);
    QVERIFY(full(image.pixel(ink.left(), floor.top() - 1)));
    QVERIFY(floor.left() <= ink.left() && floor.right() >= ink.right());

    // The harmonic: whole pixels of its red-orange, steeper than the glide,
    // and clear of the glide's amber over its upper half, not smeared into it.
    std::vector<QPoint> harmonic;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (near(image.pixel(x, y), kHarmonic, 16)) {
                harmonic.emplace_back(x, y);
            }
        }
    }
    QVERIFY2(static_cast<int>(harmonic.size()) >= size / 3, qPrintable(QString::number(harmonic.size())));
    const int harmonicTop = harmonic.front().y();
    const int harmonicBottom = harmonic.back().y();
    QVERIFY2(harmonicTop < ink.top() - 1, qPrintable(QStringLiteral("%1, %2").arg(harmonicTop).arg(ink.top())));
    for (const QPoint &p : harmonic) {
        if (p.y() > (harmonicTop + harmonicBottom) / 2) {
            continue;
        }
        const QRect around = QRect(p.x() - 1, p.y() - 1, 3, 3) & image.rect();
        QVERIFY2(countWhere(image, around, glideInk) == 0,
                 qPrintable(QStringLiteral("%1,%2").arg(p.x()).arg(p.y())));
    }
}

void AppIconTest::harmonicFrom32Px_data() {
    QTest::addColumn<int>("size");
    for (const int size : FerrousAppIcon::kSizes) {
        if (size >= 32) {
            QTest::addRow("%dpx", size) << size;
        }
    }
}

void AppIconTest::harmonicFrom32Px() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    // The harmonic's red-orange, distinct from the glide's amber: from 32 px
    // up a stroke of its own, not a few pixels at the glide's edge. Near the
    // origin the glide covers it, so two thirds of the size is a stroke
    // about 2 px wide over half the tile.
    const int harmonic = countWhere(image, image.rect(), [](QRgb p) { return near(p, kHarmonic, 40); });
    QVERIFY2(harmonic >= size * 2 / 3, qPrintable(QString::number(harmonic)));
}

// The glide and its harmonic are one sweep from one origin, not strokes
// floating apart (#373 hill climb): their ink is one shape, it starts low in
// the tile's left third, and the drawing sits in the middle of the tile.
void AppIconTest::strokesRiseTogetherFromOneOrigin() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);

    // The strokes' ink, in shapes of touching pixels, corners included.
    std::vector<int> shape(static_cast<size_t>(size * size), -1);
    int shapes = 0;
    int lowest = -1;
    int lowestX = -1;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!strokeInk(image.pixel(x, y)) || shape[static_cast<size_t>(y * size + x)] >= 0) {
                continue;
            }
            std::vector<QPoint> pending{QPoint(x, y)};
            shape[static_cast<size_t>(y * size + x)] = shapes;
            while (!pending.empty()) {
                const QPoint p = pending.back();
                pending.pop_back();
                if (p.y() > lowest || (p.y() == lowest && p.x() < lowestX)) {
                    lowest = p.y();
                    lowestX = p.x();
                }
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const QPoint q(p.x() + dx, p.y() + dy);
                        if (!image.rect().contains(q) || !strokeInk(image.pixel(q))
                            || shape[static_cast<size_t>(q.y() * size + q.x())] >= 0) {
                            continue;
                        }
                        shape[static_cast<size_t>(q.y() * size + q.x())] = shapes;
                        pending.push_back(q);
                    }
                }
            }
            ++shapes;
        }
    }
    QCOMPARE(shapes, 1);
    // Its lowest point, the origin, is in the tile's left third and lower half.
    QVERIFY2(lowestX < tile.left() + tile.width() / 3, qPrintable(QString::number(lowestX)));
    QVERIFY2(lowest > tile.center().y(), qPrintable(QString::number(lowest)));
    // The drawing is centred in the tile, to within a pixel or so.
    const QRect ink = boundsWhere(image, strokeInk);
    const double slack = std::max(1.0, size / 32.0);
    const double dx = (ink.left() + ink.right()) / 2.0 - (tile.left() + tile.right()) / 2.0;
    const double dy = (ink.top() + ink.bottom()) / 2.0 - (tile.top() + tile.bottom()) / 2.0;
    QVERIFY2(std::abs(dx) <= slack && std::abs(dy) <= slack,
             qPrintable(QStringLiteral("off centre by %1, %2").arg(dx).arg(dy)));
    // Nothing of the glide in the tile's top-right corner: it rises, it does
    // not slash from corner to corner.
    const int cornerSide = std::lround(tile.width() * 0.2);
    const QRect corner(tile.right() + 1 - cornerSide, tile.top(), cornerSide, cornerSide);
    QCOMPARE(countWhere(image, corner, glideInk), 0);
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
