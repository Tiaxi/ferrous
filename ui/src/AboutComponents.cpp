// SPDX-License-Identifier: GPL-3.0-or-later

#include "AboutComponents.h"

#include <QGuiApplication>
#include <QString>
#include <QVariantMap>
#include <QtGlobal>

#include <KLocalizedString>
#include <ki18n_version.h>

namespace {

QVariantMap component(const QString &name, const QString &version) {
    return QVariantMap{{QStringLiteral("name"), name}, {QStringLiteral("version"), version}};
}

QString windowingSystemName() {
    const QString platform = QGuiApplication::platformName();
    if (platform.startsWith(QLatin1String("wayland"))) {
        return QStringLiteral("Wayland");
    }
    if (platform == QLatin1String("xcb")) {
        return QStringLiteral("X11");
    }
    return platform;
}

} // namespace

QVariantList aboutComponents() {
    const QString runtimeQt = QString::fromLatin1(qVersion());
    const QString builtQt = QStringLiteral(QT_VERSION_STR);
    const QString qtVersion = runtimeQt == builtQt
        ? runtimeQt
        : i18nc("@info Qt version it runs on, version it was built with", "%1 (built against %2)",
                runtimeQt, builtQt);
    return {
        component(QStringLiteral("KDE Frameworks"), QStringLiteral(KI18N_VERSION_STRING)),
        component(QStringLiteral("Qt"), qtVersion),
        component(i18nc("@label", "Windowing system"), windowingSystemName()),
    };
}
