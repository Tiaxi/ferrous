// SPDX-License-Identifier: GPL-3.0-or-later

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

} // namespace

class DisplaySleepInhibitorTest : public QObject {
    Q_OBJECT

private slots:
    void forwardsOnlyStateTransitionsToBackend();
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

QTEST_GUILESS_MAIN(DisplaySleepInhibitorTest)

#include "tst_display_sleep_inhibitor.moc"
