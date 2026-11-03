// SPDX-License-Identifier: GPL-3.0-or-later

#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QWindow>

#include <cstdio>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

#include "BridgeClient.h"
#include "AppInstanceController.h"
#include "ConsoleTee.h"
#include "CoverImageProvider.h"
#include "DiagnosticsLog.h"
#include "DisplaySleepInhibitor.h"
#include "LibraryTreeModel.h"
#include "Localization.h"
#include "MprisController.h"
#include "QmlTypes.h"
#include "TagEditorController.h"
#include "WindowActivation.h"

namespace {

bool hasInstalledDesktopEntry(const QString &desktopFileName) {
    return !QStandardPaths::locate(
                QStandardPaths::ApplicationsLocation,
                desktopFileName,
                QStandardPaths::LocateFile)
                .isEmpty();
}

} // namespace

int main(int argc, char *argv[]) {
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    QApplication app(argc, argv);
    FerrousLocalization::setUpApplicationDomain();
    QApplication::setApplicationName(QStringLiteral("Ferrous"));
    QApplication::setApplicationDisplayName(QStringLiteral("Ferrous"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/assets/ferrous.svg")));
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

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(1); },
        Qt::QueuedConnection);

    engine.loadFromModule("FerrousUi", "Main");
    if (!engine.rootObjects().isEmpty()) {
        setMainActivationWindow(qobject_cast<QWindow *>(engine.rootObjects().constFirst()));
    }
    return app.exec();
}
