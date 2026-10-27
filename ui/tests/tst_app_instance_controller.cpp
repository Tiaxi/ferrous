// SPDX-License-Identifier: GPL-3.0-or-later

#include <QCloseEvent>
#include <QDBusAbstractAdaptor>
#include <QGuiApplication>
#include <QWindow>
#include <QtTest/QtTest>

#include "../src/AppInstanceController.h"
#include "../src/WindowActivation.h"
#include "TestPlatform.h"

#include <memory>

namespace {

class FakeTransport final : public AppInstanceTransport {
public:
    bool available{true};
    bool startPrimaryResult{true};
    bool remoteExists{false};
    bool forwardResult{false};
    int startPrimaryCalls{0};
    int remoteExistsCalls{0};
    int forwardCalls{0};
    QStringList forwardedPaths;
    QString forwardedActivationToken;
    QObject *primaryObject{nullptr};
    QString startError{QStringLiteral("start failed")};
    QString forwardError{QStringLiteral("forward failed")};

    bool isAvailable() const override { return available; }

    bool startPrimary(QObject *object, QString *errorMessage) override {
        ++startPrimaryCalls;
        primaryObject = object;
        if (startPrimaryResult) {
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = startError;
        }
        return false;
    }

    bool remoteServiceExists() const override {
        ++const_cast<FakeTransport *>(this)->remoteExistsCalls;
        return remoteExists;
    }

    bool forwardToPrimary(
        const QStringList &paths,
        const QString &activationToken,
        QString *errorMessage) override {
        ++forwardCalls;
        forwardedPaths = paths;
        forwardedActivationToken = activationToken;
        if (forwardResult) {
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = forwardError;
        }
        return false;
    }
};

} // namespace

class AppInstanceControllerTest : public QObject {
    Q_OBJECT

private slots:
    void sanitizeOpenTargetsDropsBlankEntries();
    void initializeBuffersStartupTargetsUntilHandlerIsBound();
    void initializeForwardsToExistingPrimary();
    void initializeErrorsWhenForwardToPrimaryFails();
    void openPathsDispatchesImmediatelyWhenHandlerExists();
    void initializeForwardsActivationTokenToPrimary();
    void dbusOpenActivatesWithForwardedToken();
    void pendingRequestsKeepTheirActivationToken();
    void activationRaisesMainWindowNotHiddenViewer();
    void closeMainWindowSendsTheMainWindowACloseEvent();
    void activationTokenReachesOnlyWayland();
    void drawsOffscreenUnlessAPlatformIsChosen();
};

void AppInstanceControllerTest::sanitizeOpenTargetsDropsBlankEntries() {
    const QStringList raw{
        QStringLiteral(""),
        QStringLiteral("   "),
        QStringLiteral("file:///tmp/song.flac"),
        QStringLiteral("/tmp/album"),
    };
    const QStringList expected{
        QStringLiteral("file:///tmp/song.flac"),
        QStringLiteral("/tmp/album"),
    };
    QCOMPARE(AppInstanceController::sanitizeOpenTargets(raw), expected);
}

void AppInstanceControllerTest::initializeBuffersStartupTargetsUntilHandlerIsBound() {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *transportPtr = transport.get();
    AppInstanceController controller(std::move(transport));

    QCOMPARE(
        controller.initialize(QStringList{QStringLiteral("file:///tmp/song.flac")}),
        AppInstanceController::StartupResult::ContinuePrimary);
    QCOMPARE(transportPtr->startPrimaryCalls, 1);
    QVERIFY(transportPtr->primaryObject == &controller);

    QStringList dispatched;
    controller.setOpenHandler([&dispatched](const QStringList &paths) { dispatched = paths; });
    QCOMPARE(dispatched, QStringList{QStringLiteral("file:///tmp/song.flac")});
}

void AppInstanceControllerTest::initializeForwardsToExistingPrimary() {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *transportPtr = transport.get();
    transportPtr->startPrimaryResult = false;
    transportPtr->remoteExists = true;
    transportPtr->forwardResult = true;
    AppInstanceController controller(std::move(transport));

    QCOMPARE(
        controller.initialize(QStringList{QStringLiteral("/tmp/song.flac")}),
        AppInstanceController::StartupResult::ExitAfterForward);
    QCOMPARE(transportPtr->startPrimaryCalls, 1);
    QCOMPARE(transportPtr->remoteExistsCalls, 1);
    QCOMPARE(transportPtr->forwardCalls, 1);
    QCOMPARE(transportPtr->forwardedPaths, QStringList{QStringLiteral("/tmp/song.flac")});
}

void AppInstanceControllerTest::initializeErrorsWhenForwardToPrimaryFails() {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *transportPtr = transport.get();
    transportPtr->startPrimaryResult = false;
    transportPtr->remoteExists = true;
    transportPtr->forwardResult = false;
    transportPtr->forwardError = QStringLiteral("handoff failed");
    AppInstanceController controller(std::move(transport));

    QString errorMessage;
    QCOMPARE(
        controller.initialize(QStringList{QStringLiteral("/tmp/song.flac")}, &errorMessage),
        AppInstanceController::StartupResult::ExitWithError);
    QCOMPARE(errorMessage, QStringLiteral("handoff failed"));
}

void AppInstanceControllerTest::openPathsDispatchesImmediatelyWhenHandlerExists() {
    auto transport = std::make_unique<FakeTransport>();
    AppInstanceController controller(std::move(transport));

    QStringList dispatched;
    controller.setOpenHandler([&dispatched](const QStringList &paths) { dispatched = paths; });
    QVERIFY(controller.openPaths(QStringList{QStringLiteral("/tmp/song.flac")}));
    QCOMPARE(dispatched, QStringList{QStringLiteral("/tmp/song.flac")});
}

void AppInstanceControllerTest::initializeForwardsActivationTokenToPrimary() {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport *transportPtr = transport.get();
    transportPtr->startPrimaryResult = false;
    transportPtr->remoteExists = true;
    transportPtr->forwardResult = true;
    AppInstanceController controller(std::move(transport));

    QCOMPARE(
        controller.initialize(
            QStringList{QStringLiteral("/tmp/song.flac")},
            nullptr,
            QStringLiteral("token-123")),
        AppInstanceController::StartupResult::ExitAfterForward);
    QCOMPARE(transportPtr->forwardedActivationToken, QStringLiteral("token-123"));
}

void AppInstanceControllerTest::dbusOpenActivatesWithForwardedToken() {
    AppInstanceController controller(std::make_unique<FakeTransport>());
    QStringList events;
    controller.setOpenHandler([&events](const QStringList &paths) {
        events.push_back(QStringLiteral("open:") + paths.join(QLatin1Char(',')));
    });
    controller.setActivationHandler([&events](const QString &token) {
        events.push_back(QStringLiteral("activate:") + token);
    });

    auto *adaptor = controller.findChild<QDBusAbstractAdaptor *>();
    QVERIFY(adaptor != nullptr);
    bool accepted = false;
    QVariantMap platformData;
    platformData.insert(QStringLiteral("activation-token"), QStringLiteral("wayland-token"));
    QVERIFY(QMetaObject::invokeMethod(
        adaptor,
        "Open",
        Q_RETURN_ARG(bool, accepted),
        Q_ARG(QStringList, QStringList{QStringLiteral("/tmp/song.flac")}),
        Q_ARG(QVariantMap, platformData)));
    QVERIFY(accepted);
    QCOMPARE(events, (QStringList{
        QStringLiteral("open:/tmp/song.flac"),
        QStringLiteral("activate:wayland-token"),
    }));

    // Activation without files still raises the window with the token.
    events.clear();
    QVERIFY(QMetaObject::invokeMethod(
        adaptor,
        "Open",
        Q_RETURN_ARG(bool, accepted),
        Q_ARG(QStringList, QStringList{}),
        Q_ARG(QVariantMap, platformData)));
    QCOMPARE(events, QStringList{QStringLiteral("activate:wayland-token")});

    // The older method stays available for second instances of older builds.
    events.clear();
    QVERIFY(QMetaObject::invokeMethod(
        adaptor,
        "OpenPaths",
        Q_RETURN_ARG(bool, accepted),
        Q_ARG(QStringList, QStringList{QStringLiteral("/tmp/other.flac")})));
    QCOMPARE(events, (QStringList{QStringLiteral("open:/tmp/other.flac"), QStringLiteral("activate:")}));
}

void AppInstanceControllerTest::pendingRequestsKeepTheirActivationToken() {
    AppInstanceController controller(std::make_unique<FakeTransport>());
    QStringList tokens;
    controller.setActivationHandler([&tokens](const QString &token) { tokens.push_back(token); });

    QVERIFY(controller.openPaths(QStringList{QStringLiteral("/tmp/song.flac")}, QStringLiteral("early")));
    QVERIFY(tokens.isEmpty());

    QStringList dispatched;
    controller.setOpenHandler([&dispatched](const QStringList &paths) { dispatched = paths; });
    QCOMPARE(dispatched, QStringList{QStringLiteral("/tmp/song.flac")});
    QCOMPARE(tokens, QStringList{QStringLiteral("early")});
}

void AppInstanceControllerTest::activationRaisesMainWindowNotHiddenViewer() {
    // The viewer exists first, so it leads the top-level window list.
    QWindow viewer;
    QWindow mainWindow;
    viewer.create();
    mainWindow.create();
    const QWindowList windows = QGuiApplication::topLevelWindows();
    QVERIFY(windows.indexOf(&viewer) < windows.indexOf(&mainWindow));

    QVERIFY(!activateMainWindow());
    setMainActivationWindow(&mainWindow);
    QVERIFY(activateMainWindow());
    QVERIFY(mainWindow.isVisible());
    QVERIFY(!viewer.isVisible());

    setMainActivationWindow(nullptr);
}

void AppInstanceControllerTest::activationTokenReachesOnlyWayland() {
    qunsetenv("XDG_ACTIVATION_TOKEN");
    provideActivationToken(QStringLiteral("xcb"), QStringLiteral("token"));
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));
    provideActivationToken(QStringLiteral("wayland"), QString());
    QVERIFY(!qEnvironmentVariableIsSet("XDG_ACTIVATION_TOKEN"));

    provideActivationToken(QStringLiteral("wayland"), QStringLiteral("token"));
    QCOMPARE(qEnvironmentVariable("XDG_ACTIVATION_TOKEN"), QStringLiteral("token"));
    qunsetenv("XDG_ACTIVATION_TOKEN");
}

void AppInstanceControllerTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    QGuiApplication app(argc, argv);
    AppInstanceControllerTest test;
    return QTest::qExec(&test, argc, argv);
}

void AppInstanceControllerTest::closeMainWindowSendsTheMainWindowACloseEvent() {
    // Counts close events and refuses them, as a window still saving does.
    class CloseCountingWindow final : public QWindow {
    public:
        int closeEvents{0};

    protected:
        void closeEvent(QCloseEvent *event) override {
            ++closeEvents;
            event->ignore();
        }
    };
    QWindow viewer;
    CloseCountingWindow mainWindow;
    viewer.show();
    mainWindow.showMinimized();

    QVERIFY(!closeMainWindow());
    QCOMPARE(mainWindow.closeEvents, 0);

    setMainActivationWindow(&mainWindow);
    QVERIFY(closeMainWindow());
    QCOMPARE(mainWindow.closeEvents, 1);
    QVERIFY(mainWindow.isVisible());
    QVERIFY(viewer.isVisible());

    setMainActivationWindow(nullptr);
}

#include "tst_app_instance_controller.moc"
