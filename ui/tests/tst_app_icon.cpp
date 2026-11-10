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
#include <optional>
#include <vector>

#include "AppIcon.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

constexpr QRgb kTile = 0x3a1490;
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

// At least half the glide's amber, over the tile or the floor: the glide's
// edge pixels count too.
bool glideInk(QRgb pixel) {
    return qRed(pixel) >= 150 && qGreen(pixel) >= 100;
}

// Either stroke over the tile or the floor, edge pixels included: the tile and
// the floor have almost no green, the glide and the harmonic plenty.
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

// The first row of the floor band, down the tile's middle.
std::optional<int> floorTopOf(const QImage &image, const QRect &tile) {
    for (int y = tile.top(); y <= tile.bottom(); ++y) {
        if (near(image.pixel(tile.center().x(), y), kFloor, 8)) {
            return y;
        }
    }
    return std::nullopt;
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
    void smallSizesRiseFromTheFloorAndStayOffTheCorner_data();
    void smallSizesRiseFromTheFloorAndStayOffTheCorner();
    void harmonicAtEverySize_data() { addSizeRows(); }
    void harmonicAtEverySize();
    void strokesRiseTogetherFromTheFloor_data() { addSizeRows(); }
    void strokesRiseTogetherFromTheFloor();
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

    // The tile fills most of the square; the tile, the floor and the glide
    // each have pixels of their own colour.
    const QRect all = image.rect();
    const int opaque = countWhere(image, all, [](QRgb p) { return qAlpha(p) == 255; });
    QVERIFY2(opaque >= size * size * 3 / 5, qPrintable(QString::number(opaque)));
    QVERIFY(countWhere(image, all, [](QRgb p) { return near(p, kTile, 4); }) >= size * size / 3);
    QVERIFY(countWhere(image, all, [](QRgb p) { return near(p, kFloor, 4); }) >= size);
    QVERIFY(countWhere(image, all, [](QRgb p) { return near(p, kGlide, 24); }) >= size / 2);
    // Centred, with a margin.
    const QRect tile = tileOf(image);
    QCOMPARE(tile.left(), all.right() - tile.right());
    QCOMPARE(tile.top(), all.bottom() - tile.bottom());
    QVERIFY(tile.left() >= 1);

    QVERIFY(QFile::exists(FerrousAppIcon::resourcePath(0)));
    QCOMPARE(FerrousAppIcon::resourcePath(24), QStringLiteral(":/icons/hicolor/scalable/apps/ferrous.svg"));
}

void AppIconTest::smallSizesRiseFromTheFloorAndStayOffTheCorner_data() {
    QTest::addColumn<int>("size");
    QTest::addRow("16px") << 16;
    QTest::addRow("22px") << 22;
    QTest::addRow("32px") << 32;
}

void AppIconTest::smallSizesRiseFromTheFloorAndStayOffTheCorner() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const std::optional<int> floorTop = floorTopOf(image, tile);
    QVERIFY(floorTop.has_value());
    // The floor is a band: at most a seventh of the tile.
    QVERIFY2(tile.bottom() + 1 - *floorTop <= tile.height() / 7 + 1, qPrintable(QString::number(*floorTop)));

    // Corner to corner reads as a slash: nothing of the glide in the tile's
    // top-right corner.
    const int cornerSide = std::lround(tile.width() * 0.3);
    const QRect corner(tile.right() + 1 - cornerSide, tile.top(), cornerSide, cornerSide);
    QCOMPARE(countWhere(image, corner, glideInk), 0);
    // At 22 and 16 px it ends at about 70% of the width.
    int rightmost = -1;
    for (int y = tile.top(); y < *floorTop; ++y) {
        for (int x = tile.left(); x <= tile.right(); ++x) {
            if (glideInk(image.pixel(x, y))) {
                rightmost = std::max(rightmost, x);
            }
        }
    }
    if (size < 32) {
        QVERIFY2(rightmost + 1 <= std::lround(size * 0.84), qPrintable(QString::number(rightmost)));
        QVERIFY2(rightmost + 1 >= std::lround(size * 0.6), qPrintable(QString::number(rightmost)));
        // At 22 and 16 px the glide starts at the floor band: its round cap
        // sits on the band's top row, in the tile's left half.
        const QRect onFloor(tile.left(), *floorTop - 1, tile.width() / 2, 1);
        QVERIFY(countWhere(image, onFloor, glideInk) >= 1);
    }
}

void AppIconTest::harmonicAtEverySize() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    // The harmonic's red-orange, distinct from the glide's amber; at 16 and
    // 22 px a stair of whole pixels.
    const int harmonic = countWhere(image, image.rect(), [](QRgb p) { return near(p, kHarmonic, 40); });
    QVERIFY2(harmonic >= 4, qPrintable(QString::number(harmonic)));
}

// The glide and its harmonic are one sweep growing out of the floor band, not
// strokes floating in the tile (#373 hill climb): their ink is one shape, it
// stands on the band, and it starts in the tile's left third.
void AppIconTest::strokesRiseTogetherFromTheFloor() {
    QFETCH(int, size);
    const QImage image = render(FerrousAppIcon::resourcePath(size), size);
    const QRect tile = tileOf(image);
    const std::optional<int> floorTop = floorTopOf(image, tile);
    QVERIFY(floorTop.has_value());

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
    // It reaches the band's top row, at its start in the tile's left third.
    QVERIFY2(lowest >= *floorTop - 1, qPrintable(QStringLiteral("%1 above %2").arg(lowest).arg(*floorTop)));
    QVERIFY2(lowestX < tile.left() + tile.width() / 3, qPrintable(QString::number(lowestX)));
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
