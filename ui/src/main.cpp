// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QIcon>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QScreen>
#include <QStandardPaths>
#include <QWindow>

#include <ki18n_version.h>

#include <cstdio>
#include <cstdlib>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

#include "BridgeClient.h"
#include "AppIcon.h"
#include "AboutComponents.h"
#include "AppInstanceController.h"
#include "ConsoleTee.h"
#include "CoverImageProvider.h"
#include "DiagnosticsLog.h"
#include "DisplaySleepInhibitor.h"
#include "HeapAllocator.h"
#include "HeapTrimmer.h"
#include "LibraryTreeModel.h"
#include "Localization.h"
#include "MprisController.h"
#include "QmlTypes.h"
#include "TagEditorController.h"
#include "WindowActivation.h"
#include "WindowPresence.h"

namespace {

bool hasInstalledDesktopEntry(const QString &desktopFileName) {
    return !QStandardPaths::locate(
                QStandardPaths::ApplicationsLocation,
                desktopFileName,
                QStandardPaths::LocateFile)
                .isEmpty();
}

// Logs what the UI runs with, for a bug report: the versions, the session,
// the platform and the style. The backend logs its own versions and folders.
void logStartup(BridgeClient &bridge, const QString &logPath) {
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    bridge.logInfo(
        QStringLiteral("app"),
        QStringLiteral("Ferrous %1 started: Qt %2 (built with %3), KDE Frameworks %4, "
                       "desktop %5, platform %6, style %7, icon theme %8, locale %9; log \"%10\".")
            .arg(QStringLiteral(FERROUS_APP_VERSION),
                 QString::fromLatin1(qVersion()),
                 QStringLiteral(QT_VERSION_STR),
                 QStringLiteral(KI18N_VERSION_STRING),
                 desktop.isEmpty() ? QStringLiteral("none") : desktop,
                 QGuiApplication::platformName(),
                 QQuickStyle::name(),
                 QIcon::themeName(),
                 QLocale().name(),
                 logPath));
    if (!BridgeClient::debugLoggingEnabled()) {
        return;
    }
    QStringList screens;
    for (const QScreen *screen : QGuiApplication::screens()) {
        screens.push_back(QStringLiteral("%1 %2x%3 at %4x")
                              .arg(screen->name())
                              .arg(screen->size().width())
                              .arg(screen->size().height())
                              .arg(screen->devicePixelRatio()));
    }
    bridge.logDebug(
        QStringLiteral("app"),
        QStringLiteral("Platform theme %1, screens: %2.")
            .arg(qEnvironmentVariable("QT_QPA_PLATFORMTHEME"), screens.join(QStringLiteral(", "))));
}

} // namespace

int main(int argc, char *argv[]) {
    // Before Qt, GStreamer or the Rust library start a thread.
    FerrousHeap::configureAllocator(std::getenv("GLIBC_TUNABLES"));
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    QApplication app(argc, argv);
    FerrousLocalization::setUpApplicationDomain();
    QApplication::setApplicationName(QStringLiteral("Ferrous"));
    QApplication::setApplicationDisplayName(QStringLiteral("Ferrous"));
    QApplication::setWindowIcon(FerrousAppIcon::icon());
    if (hasInstalledDesktopEntry(QStringLiteral("ferrous.desktop"))) {
        QApplication::setDesktopFileName(QStringLiteral("ferrous"));
    }
    ConsoleTee consoleTee(DiagnosticsLog::defaultLogPath());
#if defined(Q_OS_UNIX)
    consoleTee.install(STDOUT_FILENO, QStringLiteral("stdout"));
    consoleTee.install(STDERR_FILENO, QStringLiteral("stderr"));
#endif
    AppInstanceController instanceController;
    QString instanceError;
    switch (instanceController.initialize(
        QCoreApplication::arguments().mid(1),
        &instanceError,
        qEnvironmentVariable("XDG_ACTIVATION_TOKEN"))) {
    case AppInstanceController::StartupResult::ExitAfterForward:
        return 0;
    case AppInstanceController::StartupResult::ExitWithError:
        if (!instanceError.trimmed().isEmpty()) {
            std::fprintf(stderr, "%s\n", qPrintable(instanceError));
        }
        return 1;
    case AppInstanceController::StartupResult::ContinuePrimary:
        break;
    }
    BridgeClient bridge;
    instanceController.setOpenHandler([&bridge](const QStringList &paths) {
        bridge.replaceWithPaths(paths);
    });
    // Trims after the releases that leave much free memory in the arenas.
    HeapTrimmer heapTrimmer;
    QObject::connect(&bridge, &BridgeClient::trackIdentityChanged,
        &heapTrimmer, &HeapTrimmer::requestTrim);
    QObject::connect(&bridge, &BridgeClient::snapshotChanged, &heapTrimmer,
        [&heapTrimmer, &bridge]() { heapTrimmer.setQueueLength(bridge.queueLength()); });
    MprisController mpris(&bridge);
    DisplaySleepInhibitor displaySleepInhibitor;
    LibraryTreeModel libraryModel;
    TagEditorController tagEditor(&bridge);
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("covers"), new CoverImageProvider);
    registerFerrousQmlTypes();
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &tagEditor);
    engine.rootContext()->setContextProperty(
        QStringLiteral("displaySleepInhibitor"),
        &displaySleepInhibitor);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"),
        QStringLiteral(FERROUS_APP_VERSION));
    engine.rootContext()->setContextProperty(QStringLiteral("appComponents"), aboutComponents());

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(1); },
        Qt::QueuedConnection);

    engine.loadFromModule("FerrousUi", "Main");
    logStartup(bridge, DiagnosticsLog::defaultLogPath());
    if (!engine.rootObjects().isEmpty()) {
        auto *mainWindow = qobject_cast<QWindow *>(engine.rootObjects().constFirst());
        setMainActivationWindow(mainWindow);
        if (mainWindow != nullptr) {
            WindowPresence *presence = WindowPresence::of(mainWindow);
            QObject::connect(presence, &WindowPresence::shownChanged, &heapTrimmer,
                [&heapTrimmer, presence]() { heapTrimmer.setWindowShown(presence->shown()); });
            heapTrimmer.setWindowShown(presence->shown());
        }
    }
    return app.exec();
}
