// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <memory>

#include "../src/DisplaySleepInhibitor.h"

namespace {

struct BackendState {
    QList<bool> requests;
};

class FakeBackend final : public DisplaySleepInhibitorBackend {
public:
    explicit FakeBackend(BackendState *state)
        : m_state(state) {}

    void setInhibited(bool inhibited) override {
        m_state->requests.push_back(inhibited);
    }

private:
    BackendState *m_state;
};

// Answers the backend's D-Bus calls in-process instead of using a session bus.
struct FakeScreenSaver {
    QList<QDBusMessage> calls;
    quint32 cookie{41};
    bool failInhibit{false};

    ScreenSaverInhibitorBackend::DbusCall caller() {
        return [this](const QDBusMessage &message) {
            calls.push_back(message);
            if (message.member() == QStringLiteral("Inhibit")) {
                return QDBusPendingCall::fromCompletedCall(failInhibit
                    ? message.createErrorReply(QDBusError::ServiceUnknown, QStringLiteral("no idle service"))
                    : message.createReply(QVariant::fromValue(cookie)));
            }
            return QDBusPendingCall::fromCompletedCall(message.createReply());
        };
    }

    QStringList members() const {
        QStringList out;
        for (const QDBusMessage &call : calls) {
            out.push_back(call.member());
        }
        return out;
    }
};

// Replies to completed calls reach the watcher through queued events.
void deliverReplies() {
    QTest::qWait(20);
}

void verifyScreenSaverEndpoint(const QDBusMessage &message) {
    QCOMPARE(message.service(), QStringLiteral("org.freedesktop.ScreenSaver"));
    QCOMPARE(message.path(), QStringLiteral("/org/freedesktop/ScreenSaver"));
    QCOMPARE(message.interface(), QStringLiteral("org.freedesktop.ScreenSaver"));
}

} // namespace

class DisplaySleepInhibitorTest : public QObject {
    Q_OBJECT

private slots:
    void forwardsOnlyStateTransitionsToBackend();
    void inhibitsIdleOnlyThroughScreenSaver();
    void releasesCookieThatArrivesAfterRelease();
    void keepsOneRequestWhenToggledBeforeReply();
    void destructionReleasesHeldCookie();
    void failedInhibitHoldsNoCookie();
};

void DisplaySleepInhibitorTest::forwardsOnlyStateTransitionsToBackend() {
    BackendState backendState;
    DisplaySleepInhibitor inhibitor(std::make_unique<FakeBackend>(&backendState));
    QSignalSpy changedSpy(&inhibitor, &DisplaySleepInhibitor::inhibitedChanged);

    QVERIFY(!inhibitor.inhibited());
    inhibitor.setInhibited(false);
    QCOMPARE(backendState.requests, QList<bool>{});
    QCOMPARE(changedSpy.count(), 0);

    inhibitor.setInhibited(true);
    QVERIFY(inhibitor.inhibited());
    QCOMPARE(backendState.requests, QList<bool>{true});
    QCOMPARE(changedSpy.count(), 1);

    inhibitor.setInhibited(true);
    QCOMPARE(backendState.requests, QList<bool>{true});
    QCOMPARE(changedSpy.count(), 1);

    inhibitor.setInhibited(false);
    QVERIFY(!inhibitor.inhibited());
    QCOMPARE(backendState.requests, (QList<bool>{true, false}));
    QCOMPARE(changedSpy.count(), 2);
}

void DisplaySleepInhibitorTest::inhibitsIdleOnlyThroughScreenSaver() {
    FakeScreenSaver screenSaver;
    ScreenSaverInhibitorBackend backend(screenSaver.caller());

    backend.setInhibited(true);
    QCOMPARE(screenSaver.calls.size(), 1);
    const QDBusMessage inhibit = screenSaver.calls.first();
    verifyScreenSaverEndpoint(inhibit);
    QCOMPARE(inhibit.member(), QStringLiteral("Inhibit"));
    QCOMPARE(inhibit.arguments().size(), 2);
    QCOMPARE(inhibit.arguments().at(0).toString(), QStringLiteral("Ferrous"));
    QVERIFY(!inhibit.arguments().at(1).toString().isEmpty());
    deliverReplies();

    backend.setInhibited(false);
    QCOMPARE(screenSaver.members(), (QStringList{QStringLiteral("Inhibit"), QStringLiteral("UnInhibit")}));
    const QDBusMessage release = screenSaver.calls.last();
    verifyScreenSaverEndpoint(release);
    QCOMPARE(release.arguments(), QVariantList{QVariant::fromValue(quint32{41})});
    for (const QDBusMessage &call : std::as_const(screenSaver.calls)) {
        QVERIFY(!call.service().contains(QStringLiteral("PowerManagement")));
    }
}

void DisplaySleepInhibitorTest::releasesCookieThatArrivesAfterRelease() {
    FakeScreenSaver screenSaver;
    ScreenSaverInhibitorBackend backend(screenSaver.caller());

    backend.setInhibited(true);
    backend.setInhibited(false);
    QCOMPARE(screenSaver.members(), QStringList{QStringLiteral("Inhibit")});
    deliverReplies();
    QCOMPARE(screenSaver.calls.size(), 2);
    QCOMPARE(screenSaver.calls.last().member(), QStringLiteral("UnInhibit"));
    QCOMPARE(screenSaver.calls.last().arguments(), QVariantList{QVariant::fromValue(quint32{41})});
}

void DisplaySleepInhibitorTest::keepsOneRequestWhenToggledBeforeReply() {
    FakeScreenSaver screenSaver;
    ScreenSaverInhibitorBackend backend(screenSaver.caller());

    backend.setInhibited(true);
    backend.setInhibited(false);
    backend.setInhibited(true);
    deliverReplies();
    QCOMPARE(screenSaver.members(), QStringList{QStringLiteral("Inhibit")});

    backend.setInhibited(false);
    QCOMPARE(screenSaver.members(), (QStringList{QStringLiteral("Inhibit"), QStringLiteral("UnInhibit")}));
}

void DisplaySleepInhibitorTest::destructionReleasesHeldCookie() {
    FakeScreenSaver screenSaver;
    {
        ScreenSaverInhibitorBackend backend(screenSaver.caller());
        backend.setInhibited(true);
        deliverReplies();
    }
    QCOMPARE(screenSaver.members(), (QStringList{QStringLiteral("Inhibit"), QStringLiteral("UnInhibit")}));
}

void DisplaySleepInhibitorTest::failedInhibitHoldsNoCookie() {
    FakeScreenSaver screenSaver;
    screenSaver.failInhibit = true;
    ScreenSaverInhibitorBackend backend(screenSaver.caller());

    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Unable to inhibit display sleep")));
    backend.setInhibited(true);
    deliverReplies();
    backend.setInhibited(false);
    QCOMPARE(screenSaver.members(), QStringList{QStringLiteral("Inhibit")});

    // A later request tries again.
    screenSaver.failInhibit = false;
    backend.setInhibited(true);
    QCOMPARE(screenSaver.members(), (QStringList{QStringLiteral("Inhibit"), QStringLiteral("Inhibit")}));
}

QTEST_GUILESS_MAIN(DisplaySleepInhibitorTest)

#include "tst_display_sleep_inhibitor.moc"
