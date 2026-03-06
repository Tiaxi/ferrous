#include <QtTest/QtTest>

#include "../src/AppInstanceController.h"

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

    bool forwardToPrimary(const QStringList &paths, QString *errorMessage) override {
        ++forwardCalls;
        forwardedPaths = paths;
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

QTEST_MAIN(AppInstanceControllerTest)

#include "tst_app_instance_controller.moc"
