// SPDX-License-Identifier: GPL-3.0-or-later

#include "AppIcon.h"

#include <QImage>
#include <QImageReader>
#include <QPixmap>
#include <QSize>
#include <QtGlobal>

#include <algorithm>

namespace FerrousAppIcon {

QString resourcePath(int size) {
    if (std::find(kSizes.cbegin(), kSizes.cend(), size) == kSizes.cend()) {
        return QStringLiteral(":/icons/hicolor/scalable/apps/ferrous.svg");
    }
    return QStringLiteral(":/icons/hicolor/%1x%1/apps/ferrous.svg").arg(size);
}

QIcon icon() {
    // One SVG per size: QIcon's SVG engine keeps a single file for all sizes,
    // so the drawings go in as pixmaps of their own sizes.
    QIcon icon;
    for (const int size : kSizes) {
        QImageReader reader(resourcePath(size));
        reader.setScaledSize(QSize(size, size));
        const QImage image = reader.read();
        if (image.isNull()) {
            qWarning("Ferrous: the %dpx app icon did not load: %s", size, qPrintable(reader.errorString()));
            continue;
        }
        icon.addPixmap(QPixmap::fromImage(image));
    }
    return icon;
}

} // namespace FerrousAppIcon
