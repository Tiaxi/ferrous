// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

#include <utility>

// The Last.fm scrobble progress indicator and pausing scrobbling.
class ScrobbleSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void scrobbleIndicatorShowsEveryState();
    void scrobbleIndicatorGlyphIsCentredOnItsBlock();
    void scrobbleIndicatorBarMovesBySecondsWhileListening();
    void scrobbleIndicatorFillsTheBarWithoutAWaitingFrame();
    void scrobbleIndicatorClickAndMenuPauseScrobbling();
    void scrobbleIndicatorHidesWithoutLastFmOrWhenTurnedOff();
    void lastFmPageShowsTheScrobbleProgressSettingWhileConnected();
    void scrobbleIndicatorPausesScrobbling();
};

namespace {

// A status bar with a stand-in bridge whose Last.fm account is connected,
// and two errors, so the error indicator shows too.
const QByteArray kScrobbleStatusBarQml = QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components
import "panes" as Panes

ApplicationWindow {
    id: harness
    width: 900
    height: 120
    visible: true

    property int settingsRequests: 0
    readonly property color secondaryColor: palette.textSecondary
    readonly property color tertiaryColor: palette.textTertiary
    readonly property color negativeColor: palette.negative
    readonly property color textColor: palette.text

    QtObject {
        id: bridge
        objectName: "bridge"
        property bool connected: true
        property string backendFailure: ""
        property string playbackState: "Playing"
        property string durationText: "04:00"
        property int queueLength: 1
        property string queueDurationText: "04:00"
        property string currentTrackChannelLayoutText: ""
        property string currentTrackChannelLayoutIconKey: ""
        property string currentTrackFormatLabel: ""
        property int currentTrackBitDepth: 0
        property int currentTrackSampleRateHz: 0
        property int currentTrackCurrentBitrateKbps: 0
        property bool lastFmShowScrobbleProgress: true
        property bool lastFmScrobblingEnabled: true
        property int lastFmAuthState: 2
        property bool lastFmScrobblingPaused: false
        property int lastFmScrobbleState: 1
        property int lastFmScrobbleIneligibility: 0
        property int lastFmScrobbleListenedMs: 0
        property int lastFmScrobbleThresholdMs: 120000
        property string lastFmScrobbleMessage: ""
        property int lastFmPendingScrobbleCount: 0
        property int pauseRequests: 0
        function setLastFmScrobblingPaused(value) {
            lastFmScrobblingPaused = value
            pauseRequests += 1
        }
    }

    QtObject {
        id: errors
        property int count: 2
        property int unseenCount: 0
        property bool latestUnseen: false
        property var latest: null
        signal historyRequested()
    }

    Components.UiPalette {
        id: palette
        windowRoot: harness
    }

    footer: Panes.StatusBar {
        objectName: "statusBar"
        uiBridge: bridge
        uiPalette: palette
        themeIsDark: palette.themeIsDark
        errorController: errors
        onLastFmSettingsRequested: harness.settingsRequests += 1
    }
}
)QML");

struct ScrobbleHarness {
    QQmlApplicationEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window{nullptr};
    QObject *bridge{nullptr};
    QQuickItem *indicator{nullptr};
};

// Makes the harness of kScrobbleStatusBarQml; `error` says what failed.
bool makeScrobbleHarness(ScrobbleHarness &harness, QString *error) {
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    harness.root.reset(createQmlObjectFromSource(harness.engine, kScrobbleStatusBarQml, baseUrl, error));
    harness.window = qobject_cast<QQuickWindow *>(harness.root.data());
    if (harness.window == nullptr || !QTest::qWaitForWindowExposed(harness.window)) {
        return false;
    }
    harness.bridge = harness.root->findChild<QObject *>(QStringLiteral("bridge"));
    harness.indicator = harness.root->findChild<QQuickItem *>(QStringLiteral("scrobbleIndicator"));
    return harness.bridge != nullptr && harness.indicator != nullptr;
}

// What `expression` evaluates to on `item`, or nothing.
QVariant evaluateOn(QQuickItem *item, const QString &expression) {
    QQmlExpression evaluated(qmlContext(item), item, expression);
    bool undefined = false;
    const QVariant value = evaluated.evaluate(&undefined);
    return evaluated.hasError() || undefined ? QVariant() : value;
}

} // namespace

void ScrobbleSmokeTest::scrobbleIndicatorShowsEveryState() {
    ScrobbleHarness harness;
    QString error;
    QVERIFY2(makeScrobbleHarness(harness, &error), qPrintable(error));
    QQuickItem *indicator = harness.indicator;
    QObject *bridge = harness.bridge;
    QObject *root = harness.root.data();

    // At the right end of the status bar, 16 px left of the issues.
    auto *errorsButton = harness.root->findChild<QQuickItem *>(QStringLiteral("statusBarErrorsButton"));
    QVERIFY(errorsButton != nullptr);
    QTRY_VERIFY(indicator->isVisible() && errorsButton->isVisible());
    const qreal indicatorRight = indicator->mapToScene(QPointF(indicator->width(), 0)).x();
    const qreal errorsLeft = errorsButton->mapToScene(QPointF(0, 0)).x();
    QVERIFY2(std::abs(errorsLeft - indicatorRight - 16.0) < 0.5,
             qPrintable(QStringLiteral("%1 %2").arg(indicatorRight).arg(errorsLeft)));

    const QColor secondary = root->property("secondaryColor").value<QColor>();
    const QColor tertiary = root->property("tertiaryColor").value<QColor>();
    const QColor negative = root->property("negativeColor").value<QColor>();
    const QColor text = root->property("textColor").value<QColor>();

    // The slot shows the glyph, with its bar while listening and once
    // scrobbled, words and a glyph when the user should know, and nothing
    // otherwise.
    struct Case {
        const char *name;
        int state;
        int reason;
        bool paused;
        int pending;
        const char *look;
        QString label;
        const char *glyph;
        QColor glyphColor;
        QString tooltip;
    };
    const QList<Case> cases{
        {"idle", 0, 0, false, 0, "empty", QString(), "", QColor(), QString()},
        {"listening", 1, 0, false, 0, "listening", QString(), "/lastfm.svg", secondary,
         QStringLiteral("Last.fm: 2:00 to scrobble")},
        {"sending", 2, 0, false, 0, "scrobbled", QString(), "/lastfm.svg", secondary,
         QStringLiteral("Last.fm: sending the scrobble")},
        // Accepted: the glyph settled in the secondary colour.
        {"accepted", 3, 0, false, 0, "scrobbled", QString(), "/lastfm.svg", secondary,
         QStringLiteral("Last.fm: scrobbled")},
        {"failed", 4, 0, false, 0, "failed", QStringLiteral("Scrobble failed"), "/lastfm.svg", negative,
         QStringLiteral("Last.fm didn't accept the scrobble: refused for testing")},
        {"waiting", 5, 0, false, 2, "queued", QStringLiteral("2 scrobbles waiting"), "status-offline.svg",
         secondary,
         QStringLiteral("Last.fm can't be reached (refused for testing). 2 scrobbles are waiting to be sent.")},
        {"queued while listening", 1, 0, false, 1, "queued", QStringLiteral("1 scrobble waiting"),
         "status-offline.svg", secondary,
         QStringLiteral("Last.fm can't be reached (refused for testing). 1 scrobble is waiting to be sent.")},
        {"too short", 6, 1, false, 0, "empty", QString(), "", QColor(), QString()},
        {"without tags", 6, 2, false, 0, "empty", QString(), "", QColor(), QString()},
        {"ended early", 6, 3, false, 0, "notScrobbled", QStringLiteral("Not scrobbled"), "/lastfm.svg",
         tertiary,
         QStringLiteral("Last.fm: not scrobbled. The play ended after 0:00 of the 2:00 of listening it needed.")},
        {"skipped", 7, 0, false, 0, "empty", QString(), "", QColor(), QString()},
        {"paused", 1, 0, true, 3, "paused", QString(), "/lastfm-paused.svg", tertiary,
         QStringLiteral("Last.fm: scrobbling is paused")},
    };
    bridge->setProperty("lastFmScrobbleListenedMs", 0);
    bridge->setProperty("lastFmScrobbleMessage", QStringLiteral("refused for testing"));
    QQuickItem *glyph = findQuickItemByObjectName(indicator, QStringLiteral("scrobbleIndicatorGlyph"));
    QQuickItem *label = findQuickItemByObjectName(indicator, QStringLiteral("scrobbleIndicatorLabel"));
    QVERIFY(glyph != nullptr && label != nullptr);
    for (const Case &c : cases) {
        bridge->setProperty("lastFmScrobblingPaused", c.paused);
        bridge->setProperty("lastFmPendingScrobbleCount", c.pending);
        bridge->setProperty("lastFmScrobbleIneligibility", c.reason);
        bridge->setProperty("lastFmScrobbleState", c.state);
        QTRY_COMPARE(indicator->property("look").toString(), QString::fromLatin1(c.look));
        const bool shown = qstrlen(c.glyph) > 0;
        QTRY_COMPARE(indicator->isVisible(), shown);
        if (!shown) {
            continue;
        }
        QVERIFY2(glyph->property("source").toString().endsWith(QString::fromLatin1(c.glyph)), c.name);
        QCOMPARE(glyph->property("color").value<QColor>(), c.glyphColor);
        QCOMPARE(label->isVisible(), !c.label.isEmpty());
        QCOMPARE(label->property("text").toString(), c.label);
        QCOMPARE(evaluateOn(indicator, QStringLiteral("ToolTip.text")).toString(), c.tooltip);
        QCOMPARE(QAccessible::queryAccessibleInterface(indicator)->text(QAccessible::Name), c.tooltip);
    }

    // Listening, being sent and scrobbled look apart by shape (#373 H8): a
    // 2 px bar growing in the text colour, the full bar at half strength,
    // and no bar under the glyph in the secondary colour. The glyph is the
    // same in all three.
    QQuickItem *bar = findQuickItemByObjectName(indicator, QStringLiteral("scrobbleIndicatorBar"));
    QVERIFY(bar != nullptr);
    bridge->setProperty("lastFmScrobblingPaused", false);
    bridge->setProperty("lastFmPendingScrobbleCount", 0);
    bridge->setProperty("lastFmScrobbleIneligibility", 0);
    bridge->setProperty("lastFmScrobbleListenedMs", 60000);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_COMPARE(indicator->property("look").toString(), QStringLiteral("listening"));
    QTRY_VERIFY(bar->isVisible());
    // Under the ink's 14 px: 6 px from the start, and half the other 8.
    QCOMPARE(bar->height(), 2.0);
    QCOMPARE(bar->width(), 10.0);
    QCOMPARE(bar->opacity(), 1.0);
    QCOMPARE(bar->property("color").value<QColor>(), text);
    bridge->setProperty("lastFmScrobbleState", 2);
    QTRY_COMPARE(indicator->property("look").toString(), QStringLiteral("scrobbled"));
    QVERIFY(bar->isVisible());
    QCOMPARE(bar->height(), 2.0);
    QCOMPARE(bar->width(), 14.0);
    QCOMPARE(bar->opacity(), 0.5);
    QCOMPARE(bar->property("color").value<QColor>(), text);
    QCOMPARE(glyph->property("color").value<QColor>(), secondary);
    QCOMPARE(glyph->opacity(), 1.0);
    bridge->setProperty("lastFmScrobbleState", 3);
    QTRY_VERIFY(!bar->isVisible());
    QVERIFY(indicator->isVisible());
    QCOMPARE(glyph->property("color").value<QColor>(), secondary);
    QCOMPARE(glyph->opacity(), 1.0);

    // Listening has its bar from the start, three times as long as it is
    // tall, so that it never looks like scrobbled, the glyph alone (#373
    // H9: at 0 % both were the bare glyph), and a little listening reads
    // as a bar (#373 H10: at 25 % the bar was a speck).
    for (const auto &[listenedMs, width] : {std::pair{0, 6.0}, std::pair{15000, 7.0}, std::pair{30000, 8.0}}) {
        bridge->setProperty("lastFmScrobbleListenedMs", listenedMs);
        bridge->setProperty("lastFmScrobbleState", 1);
        QTRY_COMPARE(indicator->property("look").toString(), QStringLiteral("listening"));
        QTRY_VERIFY(bar->isVisible());
        QCOMPARE(bar->height(), 2.0);
        QCOMPARE(bar->width(), width);
        QCOMPARE(bar->x(), 1.0);
        bridge->setProperty("lastFmScrobbleState", 3);
        QTRY_VERIFY(!bar->isVisible());
    }
}

// The glyph's ink, as drawn in device pixels, is centred on its block, the
// slot, both ways, and the slot on the status bar under its seam (#373
// H10: the glyph sat 2.5 px above the middle, over the bar's place). The
// bar under it is 2 px thick and, a second into a 2 min scrobble point, a
// 6 px dash from the ink's left edge, not a speck. The ctest runs this at
// 1x and at 2x.
void ScrobbleSmokeTest::scrobbleIndicatorGlyphIsCentredOnItsBlock() {
    ScrobbleHarness harness;
    QString error;
    QVERIFY2(makeScrobbleHarness(harness, &error), qPrintable(error));
    QQuickItem *indicator = harness.indicator;
    harness.bridge->setProperty("lastFmScrobbleListenedMs", 1000);
    QTRY_VERIFY(indicator->isVisible());
    auto *statusBar = harness.root->findChild<QQuickItem *>(QStringLiteral("statusBar"));
    QQuickItem *glyph = findQuickItemByObjectName(indicator, QStringLiteral("scrobbleIndicatorGlyph"));
    QVERIFY(statusBar != nullptr && glyph != nullptr);
    QTest::qWait(100);
    const QImage image = harness.window->grabWindow();
    const qreal scale = harness.window->effectiveDevicePixelRatio();
    QVERIFY(!image.isNull());

    // On whole device pixels.
    const QPointF glyphTopLeft = glyph->mapToScene(QPointF(0, 0)) * scale;
    QCOMPARE(glyphTopLeft, QPointF(std::round(glyphTopLeft.x()), std::round(glyphTopLeft.y())));

    // The slot in device pixels, and which of its rows and columns have
    // ink: anything that differs from the status bar's chrome beside it.
    const QRectF slotRect = QRectF(indicator->mapToScene(QPointF(0, 0)), indicator->size());
    const QRect slot(QPoint(int(std::round(slotRect.left() * scale)), int(std::round(slotRect.top() * scale))),
                     QSize(int(std::round(slotRect.width() * scale)), int(std::round(slotRect.height() * scale))));
    QCOMPARE(QRectF(slot), QRectF(slotRect.topLeft() * scale, slotRect.size() * scale));
    const QColor chrome = image.pixelColor(slot.left() - 2, slot.center().y());
    const auto inked = [&](int x, int y) {
        const QColor pixel = image.pixelColor(x, y);
        return std::max({std::abs(pixel.red() - chrome.red()), std::abs(pixel.green() - chrome.green()),
                         std::abs(pixel.blue() - chrome.blue())})
            > 24;
    };
    QList<int> inkRows;
    for (int y = slot.top(); y <= slot.bottom(); ++y) {
        for (int x = slot.left(); x <= slot.right(); ++x) {
            if (inked(x, y)) {
                inkRows.append(y);
                break;
            }
        }
    }
    QVERIFY(!inkRows.isEmpty());
    // Two runs of rows: the glyph, then the bar under a clear gap.
    qsizetype split = 1;
    while (split < inkRows.size() && inkRows[split] == inkRows[split - 1] + 1) {
        ++split;
    }
    QVERIFY2(split < inkRows.size(), "no bar under the glyph");
    const int glyphTop = inkRows.first();
    const int glyphBottom = inkRows[split - 1];
    const int barTop = inkRows[split];
    const int barBottom = inkRows.last();
    int glyphLeft = slot.right();
    int glyphRight = slot.left();
    for (int y = glyphTop; y <= glyphBottom; ++y) {
        for (int x = slot.left(); x <= slot.right(); ++x) {
            if (inked(x, y)) {
                glyphLeft = std::min(glyphLeft, x);
                glyphRight = std::max(glyphRight, x);
            }
        }
    }
    int barLeft = slot.right();
    int barRight = slot.left();
    for (int x = slot.left(); x <= slot.right(); ++x) {
        if (inked(x, barTop)) {
            barLeft = std::min(barLeft, x);
            barRight = std::max(barRight, x);
        }
    }
    const QString where = QStringLiteral("glyph %1..%2 x %3..%4, bar %5..%6 x %7..%8, slot %9")
                              .arg(glyphLeft).arg(glyphRight).arg(glyphTop).arg(glyphBottom)
                              .arg(barLeft).arg(barRight).arg(barTop).arg(barBottom)
                              .arg(QStringLiteral("%1,%2 %3x%4").arg(slot.x()).arg(slot.y())
                                       .arg(slot.width()).arg(slot.height()));
    // The ink's middle is the slot's, both ways: 10 by 14 px of ink in a
    // 22 by 22 px slot leaves 6 px above and below it, 4 px either side.
    QVERIFY2((glyphTop - slot.top()) == (slot.bottom() - glyphBottom), qPrintable(where));
    QVERIFY2((glyphLeft - slot.left()) == (slot.right() - glyphRight), qPrintable(where));
    QCOMPARE(glyphBottom - glyphTop + 1, int(std::round(10 * scale)));
    // And the slot's middle is the status bar's, under its 1 px seam.
    const QRectF bar = QRectF(statusBar->mapToScene(QPointF(0, 1)), QSizeF(statusBar->width(), statusBar->height() - 1));
    QCOMPARE(slotRect.center().y(), bar.center().y());
    // The bar: 2 px thick, 2 px under the ink, from its left edge, and 6 px
    // long a second into the listening.
    QVERIFY2(barBottom - barTop + 1 == int(std::round(2 * scale)), qPrintable(where));
    QVERIFY2(barTop - glyphBottom - 1 == int(std::round(2 * scale)), qPrintable(where));
    QVERIFY2(barLeft == glyphLeft, qPrintable(where));
    QVERIFY2(barRight - barLeft + 1 == int(std::round(6 * scale)), qPrintable(where));
}

// The bar and the tooltip move by whole seconds of listening: of the
// snapshots of the listened time, which arrive every 40 ms while the track
// plays, at most one a second draws anything. The bar also moves only by
// whole device pixels, so how often it moves depends on the scale; the
// ctest runs this at 1x and at 2x.
void ScrobbleSmokeTest::scrobbleIndicatorBarMovesBySecondsWhileListening() {
    ScrobbleHarness harness;
    QString error;
    QVERIFY2(makeScrobbleHarness(harness, &error), qPrintable(error));
    QQuickItem *indicator = harness.indicator;
    QObject *bridge = harness.bridge;
    bridge->setProperty("lastFmScrobbleThresholdMs", 60000);
    QTRY_VERIFY(indicator->isVisible());
    QCOMPARE(evaluateOn(indicator, QStringLiteral("ToolTip.text")).toString(),
             QStringLiteral("Last.fm: 1:00 to scrobble"));
    int frames = 0;
    connect(harness.window, &QQuickWindow::frameSwapped, harness.window, [&frames]() { ++frames; });
    // Lets the window draw what it has to.
    const auto settle = [&frames]() {
        int seen = -1;
        for (int wait = 0; wait < 50 && seen != frames; ++wait) {
            seen = frames;
            QTest::qWait(100);
        }
        return seen == frames;
    };
    QVERIFY(settle());

    // The second of listening at each move of the bar, and where it went.
    QList<int> movedAtSecond;
    QList<double> movedTo;
    QSignalSpy progressMoved(indicator, SIGNAL(barProgressChanged()));
    QSignalSpy textChanged(indicator, SIGNAL(stateTextChanged()));

    // Ten seconds of 40 ms snapshots towards a 60 s scrobble point: the
    // bar's 8 px past its start cross a device pixel every 7.5 s at 1x,
    // every 3.75 s at 2x.
    frames = 0;
    for (int listened = 40; listened <= 10000; listened += 40) {
        const int movesBefore = progressMoved.count();
        bridge->setProperty("lastFmScrobbleListenedMs", listened);
        if (progressMoved.count() != movesBefore) {
            movedAtSecond.push_back(listened / 1000);
            movedTo.push_back(indicator->property("barProgress").toDouble());
        }
        QCoreApplication::processEvents();
    }
    QVERIFY(settle());
    // It moves, forward, at most once in each second of listening.
    QVERIFY(!movedTo.isEmpty());
    for (const double moved : movedTo) {
        const double devicePixels = moved * 8.0 * std::max(1.0, harness.window->devicePixelRatio());
        QVERIFY2(std::abs(devicePixels - std::round(devicePixels)) < 1e-9, qPrintable(QString::number(moved)));
    }
    QVERIFY2(movedTo.size() <= 10, qPrintable(QString::number(movedTo.size())));
    for (qsizetype i = 1; i < movedTo.size(); ++i) {
        QVERIFY2(movedAtSecond[i] > movedAtSecond[i - 1], qPrintable(QString::number(movedAtSecond[i])));
        QVERIFY2(movedTo[i] > movedTo[i - 1], qPrintable(QString::number(movedTo[i])));
    }
    QVERIFY(movedTo.last() <= 10.0 / 60.0);
    QCOMPARE(textChanged.count(), 10);
    QVERIFY2(frames <= 10, qPrintable(QString::number(frames)));
    QCOMPARE(evaluateOn(indicator, QStringLiteral("ToolTip.text")).toString(),
             QStringLiteral("Last.fm: 0:50 to scrobble"));

    // While the count holds, as while the track is paused, nothing moves.
    frames = 0;
    progressMoved.clear();
    textChanged.clear();
    for (int snapshot = 0; snapshot < 25; ++snapshot) {
        bridge->setProperty("lastFmScrobbleListenedMs", 10000);
        QCoreApplication::processEvents();
    }
    QVERIFY(settle());
    QCOMPARE(progressMoved.count(), 0);
    QCOMPARE(textChanged.count(), 0);
    QCOMPARE(frames, 0);

    // Its scrobble being sent fills the bar at once; once accepted, the bar
    // goes.
    bridge->setProperty("lastFmScrobbleState", 2);
    QCOMPARE(indicator->property("look").toString(), QStringLiteral("scrobbled"));
    QCOMPARE(indicator->property("barProgress").toDouble(), 1.0);
    QVERIFY(indicator->isVisible());
    bridge->setProperty("lastFmScrobbleState", 3);
    QCOMPARE(indicator->property("barProgress").toDouble(), 0.0);
    QVERIFY(indicator->isVisible());
}

// The snapshots the backend sends as a play reaches its scrobble point,
// with each property set as BridgeClient sets them: the count of waiting
// scrobbles, then the play's state. A scrobble on its way doesn't wait, so
// the bar goes from filling to full with nothing in between; only a send
// that fails shows the scrobble waiting.
void ScrobbleSmokeTest::scrobbleIndicatorFillsTheBarWithoutAWaitingFrame() {
    struct Snapshot {
        int pending;
        int state;
        int listenedMs;
        const char *message;
    };
    const auto play = [](const QList<Snapshot> &snapshots, QStringList *looks, QString *error) {
        ScrobbleHarness harness;
        if (!makeScrobbleHarness(harness, error)) {
            return false;
        }
        QQuickItem *indicator = harness.indicator;
        QObject *bridge = harness.bridge;
        const auto set = [&](const char *name, const QVariant &value) {
            bridge->setProperty(name, value);
            const QString look = indicator->property("look").toString();
            if (looks->isEmpty() || looks->constLast() != look) {
                looks->append(look);
            }
        };
        for (const Snapshot &snapshot : snapshots) {
            set("lastFmPendingScrobbleCount", snapshot.pending);
            set("lastFmScrobbleState", snapshot.state);
            set("lastFmScrobbleListenedMs", snapshot.listenedMs);
            set("lastFmScrobbleMessage", QString::fromLatin1(snapshot.message));
        }
        return true;
    };

    // Listening, the tick that queues the scrobble, the service taking it
    // and sending it, and Last.fm's answer.
    QStringList looks;
    QString error;
    QVERIFY2(play({{0, 1, 119000, ""}, {0, 2, 120000, ""}, {0, 2, 120000, ""}, {0, 3, 120000, ""}},
                  &looks, &error),
             qPrintable(error));
    QCOMPARE(looks, (QStringList{QStringLiteral("listening"), QStringLiteral("scrobbled")}));

    // A send that fails leaves it waiting.
    looks.clear();
    QVERIFY2(play({{0, 1, 119000, ""}, {0, 2, 120000, ""}, {1, 5, 120000, "offline for testing"}}, &looks,
                  &error),
             qPrintable(error));
    QCOMPARE(looks, (QStringList{QStringLiteral("listening"), QStringLiteral("scrobbled"),
                                 QStringLiteral("queued")}));
}

void ScrobbleSmokeTest::scrobbleIndicatorClickAndMenuPauseScrobbling() {
    ScrobbleHarness harness;
    QString error;
    QVERIFY2(makeScrobbleHarness(harness, &error), qPrintable(error));
    QQuickItem *indicator = harness.indicator;
    QObject *bridge = harness.bridge;
    QQuickWindow *window = harness.window;
    QTRY_VERIFY(indicator->isVisible());

    // A click pauses scrobbling, and another resumes it.
    QTest::mouseClick(window, Qt::LeftButton, {}, centerInWindow(indicator));
    QTRY_VERIFY(bridge->property("lastFmScrobblingPaused").toBool());
    QCOMPARE(indicator->property("look").toString(), QStringLiteral("paused"));
    QTest::mouseClick(window, Qt::LeftButton, {}, centerInWindow(indicator));
    QTRY_VERIFY(!bridge->property("lastFmScrobblingPaused").toBool());
    QCOMPARE(bridge->property("pauseRequests").toInt(), 2);

    // The menu has the pause, checked as it is, and the Last.fm settings.
    QObject *menu = window->findChild<QObject *>(QStringLiteral("scrobbleIndicatorMenu"));
    QVERIFY(menu != nullptr);
    const auto menuItem = [&](const QString &name) -> QQuickItem * {
        QQuickItem *content = qvariant_cast<QQuickItem *>(menu->property("contentItem"));
        return content != nullptr ? findQuickItemByObjectName(content, name) : nullptr;
    };
    QTest::mouseClick(window, Qt::RightButton, {}, centerInWindow(indicator));
    QTRY_VERIFY(menu->property("opened").toBool());
    QQuickItem *pauseItem = menuItem(QStringLiteral("scrobbleIndicatorPauseItem"));
    QQuickItem *settingsItem = menuItem(QStringLiteral("scrobbleIndicatorSettingsItem"));
    QVERIFY(pauseItem != nullptr && settingsItem != nullptr);
    QVERIFY(pauseItem->property("checkable").toBool());
    QVERIFY(!pauseItem->property("checked").toBool());
    QTest::mouseClick(window, Qt::LeftButton, {}, centerInWindow(pauseItem));
    QTRY_VERIFY(bridge->property("lastFmScrobblingPaused").toBool());
    QTRY_VERIFY(!menu->property("visible").toBool());

    QTest::mouseClick(window, Qt::RightButton, {}, centerInWindow(indicator));
    QTRY_VERIFY(menu->property("opened").toBool());
    QVERIFY(pauseItem->property("checked").toBool());
    QTest::mouseClick(window, Qt::LeftButton, {}, centerInWindow(settingsItem));
    QTRY_COMPARE(harness.root->property("settingsRequests").toInt(), 1);
    QTRY_VERIFY(!menu->property("visible").toBool());
    QVERIFY(bridge->property("lastFmScrobblingPaused").toBool());

    // By keyboard: Tab reaches it, Space and Enter toggle it, and the
    // menu key opens its menu.
    QVERIFY(indicator->activeFocusOnTab());
    indicator->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(indicator->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(!bridge->property("lastFmScrobblingPaused").toBool());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(bridge->property("lastFmScrobblingPaused").toBool());
    QTest::keyClick(window, Qt::Key_Menu);
    QTRY_VERIFY(menu->property("opened").toBool());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!menu->property("visible").toBool());
}

void ScrobbleSmokeTest::scrobbleIndicatorHidesWithoutLastFmOrWhenTurnedOff() {
    ScrobbleHarness harness;
    QString error;
    QVERIFY2(makeScrobbleHarness(harness, &error), qPrintable(error));
    QQuickItem *indicator = harness.indicator;
    QObject *bridge = harness.bridge;
    QTRY_VERIFY(indicator->isVisible());

    // Not connected, or connecting.
    for (const int authState : {0, 1, 3, 4}) {
        bridge->setProperty("lastFmAuthState", authState);
        QTRY_VERIFY2(!indicator->isVisible(), qPrintable(QString::number(authState)));
    }
    bridge->setProperty("lastFmAuthState", 2);
    QTRY_VERIFY(indicator->isVisible());

    // Scrobbling turned off.
    bridge->setProperty("lastFmScrobblingEnabled", false);
    QTRY_VERIFY(!indicator->isVisible());
    bridge->setProperty("lastFmScrobblingEnabled", true);
    QTRY_VERIFY(indicator->isVisible());

    // The setting turned off.
    bridge->setProperty("lastFmShowScrobbleProgress", false);
    QTRY_VERIFY(!indicator->isVisible());
    bridge->setProperty("lastFmShowScrobbleProgress", true);
    QTRY_VERIFY(indicator->isVisible());
}

void ScrobbleSmokeTest::lastFmPageShowsTheScrobbleProgressSettingWhileConnected() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "preferences" as Preferences

ApplicationWindow {
    id: harness
    width: 720
    height: 640
    visible: true

    QtObject {
        id: bridge
        objectName: "bridge"
        property bool lastFmScrobblingEnabled: true
        property bool lastFmBuildConfigured: true
        property string lastFmUsername: ""
        property int lastFmAuthState: 0
        property int lastFmPendingScrobbleCount: 0
        property string lastFmStatusText: ""
        property bool lastFmShowScrobbleProgress: true
        property int settingChanges: 0
        function setLastFmScrobblingEnabled(value) {}
        function setLastFmShowScrobbleProgress(value) {
            lastFmShowScrobbleProgress = value
            settingChanges += 1
        }
        function beginLastFmAuth() {}
        function completeLastFmAuth() {}
        function disconnectLastFm() {}
    }

    Components.UiPalette {
        id: palette
        windowRoot: harness
    }

    // Applies each change at once, where Configure Ferrous waits for OK.
    QtObject {
        id: settings
        function value(key) { return bridge[key] }
        function setValue(key, value) {
            bridge["set" + key.charAt(0).toUpperCase() + key.slice(1)](value)
        }
    }

    Preferences.LastFmPage {
        anchors.fill: parent
        uiBridge: bridge
        uiPalette: palette
        settings: settings
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.data());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *bridge = root->findChild<QObject *>(QStringLiteral("bridge"));
    QQuickItem *checkBox = findQuickItemByObjectName(
        window->contentItem(), QStringLiteral("lastFmShowScrobbleProgressCheckBox"));
    QVERIFY(bridge != nullptr && checkBox != nullptr);

    // Only a connected account shows it.
    QVERIFY(!checkBox->isVisible());
    bridge->setProperty("lastFmUsername", QStringLiteral("tester"));
    bridge->setProperty("lastFmAuthState", 2);
    QTRY_VERIFY(checkBox->isVisible());
    QVERIFY(checkBox->property("checked").toBool());
    QCOMPARE(checkBox->property("text").toString(), QStringLiteral("Show the Last.fm status in the status bar"));

    QTest::mouseClick(window, Qt::LeftButton, {}, centerInWindow(checkBox));
    QTRY_COMPARE(bridge->property("settingChanges").toInt(), 1);
    QVERIFY(!bridge->property("lastFmShowScrobbleProgress").toBool());
    QVERIFY(!checkBox->property("checked").toBool());
    // The bridge's value wins.
    bridge->setProperty("lastFmShowScrobbleProgress", true);
    QTRY_VERIFY(checkBox->property("checked").toBool());

    bridge->setProperty("lastFmAuthState", 3);
    QTRY_VERIFY(!checkBox->isVisible());
}

void ScrobbleSmokeTest::scrobbleIndicatorPausesScrobbling() {
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    auto *indicator = window->findChild<QQuickItem *>(QStringLiteral("scrobbleIndicator"));
    QObject *pauseItem = window->findChild<QObject *>(QStringLiteral("scrobbleIndicatorPauseItem"));
    QVERIFY(bridge != nullptr && indicator != nullptr && pauseItem != nullptr);
    // The Last.fm slot pauses scrobbling; the Playback menu does not.
    QVERIFY(window->findChild<QObject *>(QStringLiteral("pauseScrobblingAction")) == nullptr);
    for (QObject *object : window->findChildren<QObject *>()) {
        QVERIFY(object->property("text").toString() != QStringLiteral("Pause Scrobbl&ing"));
    }

    // Without Last.fm, there is nothing to pause, and no indicator.
    QVERIFY(!indicator->isVisible());
    bridge->setProperty("lastFmScrobblingEnabled", true);
    bridge->setProperty("lastFmAuthState", 2);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(indicator->isVisible());
    QVERIFY(pauseItem->property("checkable").toBool());
    QVERIFY(!pauseItem->property("checked").toBool());

    QVERIFY(QMetaObject::invokeMethod(pauseItem, "toggle"));
    QVERIFY(QMetaObject::invokeMethod(pauseItem, "triggered"));
    QTRY_VERIFY(bridge->property("lastFmScrobblingPaused").toBool());
    QTRY_COMPARE(indicator->property("look").toString(), QStringLiteral("paused"));
    QTRY_VERIFY(pauseItem->property("checked").toBool());

    // A press on the slot resumes it, and the menu item follows.
    QVERIFY(QMetaObject::invokeMethod(indicator, "togglePaused"));
    QTRY_VERIFY(!bridge->property("lastFmScrobblingPaused").toBool());
    QTRY_VERIFY(!pauseItem->property("checked").toBool());

    // Its menu's settings item opens the preferences on the Last.fm page.
    QObject *preferences = window->findChild<QObject *>(QStringLiteral("preferencesDialog"));
    QVERIFY(preferences != nullptr);
    QVERIFY(QMetaObject::invokeMethod(indicator, "settingsRequested"));
    QTRY_VERIFY(preferences->property("opened").toBool());
    QCOMPARE(preferences->property("pageIndex").toInt(),
             preferences->property("lastFmPageIndex").toInt());
    QCOMPARE(preferences->property("pageTitles").toStringList().at(
                 preferences->property("lastFmPageIndex").toInt()),
             QStringLiteral("Last.fm"));
}

std::unique_ptr<QmlSmokeTest> makeScrobbleSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<ScrobbleSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_scrobble.moc"
