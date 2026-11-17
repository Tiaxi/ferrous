// A probe of Qt's and KDE's icon lookup; built by build_probes.sh (shoot_round.sh runs it).
// Which hicolor directory QIcon::fromTheme("ferrous") takes per logical size
// and device pixel ratio. Each directory holds a solid SVG of its own colour.
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QTextStream>
#include <cstdio>

// PROBE_MODE=kiconloader asks KIconLoader directly, the loader behind QIcon::fromTheme under Plasma
// (KDEPlasmaPlatformTheme6 creates a KIconEngine on KIconLoader::global()). Fedora has no kf6-kiconthemes-devel here,
// so the two public KIconThemes calls are declared by hand to match their exported symbols
// (_ZN11KIconLoader6globalEv, _ZNK11KIconLoader14loadScaledIconERK7QStringNS_5GroupEdiiRK5QListIS0_EPS0_b);
// both are non-virtual, so no class layout is needed. Linked with -l:libKF6IconThemes.so.6.
class KIconLoader {
public:
    enum Group { NoGroup = -1, Desktop = 0 };
    static KIconLoader *global();
    QPixmap loadScaledIcon(const QString &name, Group group, qreal scale, int size, int state,
                           const QStringList &overlays, QString *path_store, bool canReturnNull) const;
};

int main(int argc, char **argv) {
    // argv[1]: theme root (contains icons/hicolor), argv[2]: comma list of dirs
    const QString root = QString::fromLocal8Bit(argv[1]);
    const QStringList dirs = QString::fromLocal8Bit(argv[2]).split(',');
    QApplication app(argc, argv);
    if (qEnvironmentVariable("PROBE_MODE") != "kde") {
        QIcon::setThemeSearchPaths({root + "/icons"});
        QIcon::setThemeName("hicolor");
    }
    // Harness: PROBE_ICON names the icon, so an installed Ferrous in /usr/share cannot answer for the probe's files.
    const QString iconName = qEnvironmentVariableIsSet("PROBE_ICON") ? qEnvironmentVariable("PROBE_ICON") : QStringLiteral("ferrous");
    QIcon icon = QIcon::fromTheme(iconName);
    if (qEnvironmentVariable("PROBE_MODE") == "pixmap") {
        // FerrousAppIcon::icon(): each 1x directory's file as a pixmap of its size
        icon = QIcon();
        for (const QString &d : dirs) {
            if (d.contains('@') || d == "scalable") continue;
            const int s = d.section('x', 0, 0).toInt();
            QImage im(s, s, QImage::Format_ARGB32);
            im.fill(qRgba(dirs.indexOf(d) * 8, 0x80, 0x40, 255));
            icon.addPixmap(QPixmap::fromImage(im));
        }
    }
    std::printf("engine-null=%d theme=%s\n", icon.isNull(), qPrintable(QIcon::themeName()));
    // Harness: PROBE_SIZES / PROBE_DPRS (comma lists) override the probe's own lists.
    QList<int> sizes = {16, 22, 24, 32, 36, 40, 44, 48, 56, 64, 96, 128};
    QList<double> dprs = {1, 1.25, 1.5, 2};
    if (qEnvironmentVariableIsSet("PROBE_SIZES")) {
        sizes.clear();
        for (const QString &v : qEnvironmentVariable("PROBE_SIZES").split(',')) sizes << v.toInt();
    }
    if (qEnvironmentVariableIsSet("PROBE_DPRS")) {
        dprs.clear();
        for (const QString &v : qEnvironmentVariable("PROBE_DPRS").split(',')) dprs << v.toDouble();
    }
    std::printf("size");
    for (double d : dprs) std::printf("\t@%g", d);
    std::printf("\n");
    for (int s : sizes) {
        std::printf("%d", s);
        for (double d : dprs) {
            QPixmap pm;
            if (qEnvironmentVariable("PROBE_MODE") == "kiconloader") {
                QString path;
                pm = KIconLoader::global()->loadScaledIcon(iconName, KIconLoader::Desktop, d, s, 0, {}, &path, true);
                // the file it took, by its folder (…/hicolor/<folder>/apps/<name>.svg)
                const QStringList parts = path.split('/');
                const QString folder = parts.size() >= 3 ? parts[parts.size() - 3] : QStringLiteral("?");
                std::printf("\t%s(%dpx)", qPrintable(dirs.contains(folder) ? folder : QStringLiteral("?") + path),
                            pm.toImage().width());
                continue;
            }
            pm = icon.pixmap(QSize(s, s), d);
            const QImage im = pm.toImage().convertToFormat(QImage::Format_ARGB32);
            const QRgb p = im.pixel(im.width() / 2, im.height() / 2);
            const int idx = qRed(p);  // red channel encodes index*8
            QString name = "?";
            if (qAlpha(p) == 255 && idx % 8 == 0 && idx / 8 < dirs.size()) name = dirs[idx / 8];
            std::printf("\t%s(%dpx)", qPrintable(name), im.width());
        }
        std::printf("\n");
    }
    return 0;
}
