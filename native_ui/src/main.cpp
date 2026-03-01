#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <qqml.h>
#include <QQuickStyle>

#include "BridgeClient.h"
#include "SpectrogramItem.h"
#include "WaveformItem.h"

int main(int argc, char *argv[]) {
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
    QGuiApplication app(argc, argv);
    BridgeClient bridge;
    QQmlApplicationEngine engine;
    qmlRegisterType<SpectrogramItem>("FerrousNative", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousNative", 1, 0, "WaveformItem");
    engine.rootContext()->setContextProperty(QStringLiteral("bridge"), &bridge);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(1); },
        Qt::QueuedConnection);

    engine.loadFromModule("FerrousNative", "Main");
    return app.exec();
}
