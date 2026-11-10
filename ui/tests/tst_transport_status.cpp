// SPDX-License-Identifier: GPL-3.0-or-later

// The transport and the status bar of #373: the flat transport buttons and
// the one Play/Pause button, the seek waveform's colours, contrast and
// states as drawn, the time readout, the volume slider's hover and focus,
// and the status bar's playback, repeat, format, scan, Last.fm and issue
// states.

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <Kirigami/Platform/Units>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "Localization.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"
#include "WaveformItem.h"

namespace {

QMutex &messageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &warnings() {
    static QStringList list;
    return list;
}

QtMessageHandler &previousHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void recordWarnings(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg) {
        QMutexLocker locker(&messageMutex());
        warnings().push_back(message);
    }
    if (previousHandler()) {
        previousHandler()(type, context, message);
    }
}

QStringList takeWarnings() {
    QMutexLocker locker(&messageMutex());
    QStringList list = warnings();
    warnings().clear();
    return list;
}

// The colours of a Breeze scheme that UiPalette reads, from Breeze's
// .colors files.
struct Scheme {
    QString window;
    QString view;
    QString text;
    QString inactiveText;
    QString highlight;
    QString negative;
    QString button;
};

const Scheme kBreezeDark{QStringLiteral("#202326"), QStringLiteral("#141618"), QStringLiteral("#fcfcfc"),
                         QStringLiteral("#a1a9b1"), QStringLiteral("#3daee9"), QStringLiteral("#da4453"),
                         QStringLiteral("#292c30")};
// A dark scheme with a darker accent, whose waveform needs more than 50%
// to keep 3:1.
const Scheme kPurpleDark{QStringLiteral("#2a2e32"), QStringLiteral("#1b1e20"), QStringLiteral("#eff0f1"),
                         QStringLiteral("#a1a9b1"), QStringLiteral("#9b59b6"), QStringLiteral("#da4453"),
                         QStringLiteral("#31363b")};
const Scheme kBreezeLight{QStringLiteral("#eff0f1"), QStringLiteral("#ffffff"), QStringLiteral("#232629"),
                          QStringLiteral("#707d8a"), QStringLiteral("#3daee9"), QStringLiteral("#da4453"),
                          QStringLiteral("#fcfcfc")};

// A window in `scheme` with the transport and the status bar over a fake
// bridge and an error controller.
QByteArray harnessQml(const Scheme &scheme, int width) {
    return QStringLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components
import "controllers" as Controllers
import "panes" as Panes

ApplicationWindow {
    id: window
    width: %8
    height: 200
    visible: true
    palette.window: "%1"
    palette.base: "%2"
    palette.text: "%3"
    palette.windowText: "%3"
    palette.button: "%7"
    palette.buttonText: "%3"
    property alias bridge: bridge
    property alias tokens: tokens
    property alias errors: errors
    property alias startButton: startButton
    property alias statusBar: statusBar
    property real positionSeconds: 77
    property string positionText: "01:17"
    property var seeks: []

    QtObject {
        id: bridge
        property bool connected: true
        property string backendFailure: ""
        property real durationSeconds: 229
        property string durationText: "03:49"
        property string playbackState: "Playing"
        property real volume: 0.8
        property var waveformPeaksPacked: ""
        property real waveformCoverageSeconds: 0
        property bool waveformComplete: false
        property int repeatMode: 0
        property bool shuffleEnabled: false
        property string currentTrackFormatLabel: "FLAC"
        property string currentTrackChannelLayoutText: "Stereo"
        property int currentTrackBitDepth: 16
        property int currentTrackSampleRateHz: 44100
        property int currentTrackCurrentBitrateKbps: 557
        property bool libraryScanInProgress: false
        property int libraryScanProcessed: 0
        property int libraryScanDiscovered: 0
        property int libraryScanTotal: -1
        property var libraryRootEntries: []
        property bool lastFmShowScrobbleProgress: true
        property bool lastFmScrobblingEnabled: true
        property int lastFmAuthState: 2
        property bool lastFmScrobblingPaused: false
        property int lastFmScrobbleState: 0
        property int lastFmScrobbleIneligibility: 0
        property int lastFmScrobbleListenedMs: 0
        property int lastFmScrobbleThresholdMs: 0
        property string lastFmScrobbleMessage: ""
        property int lastFmPendingScrobbleCount: 0
        function setLastFmScrobblingPaused(paused) { lastFmScrobblingPaused = paused }
        signal trackIdentityChanged()
    }

    Action { id: previousAction; text: "P&revious"; icon.name: "media-skip-backward"; shortcut: "Ctrl+Left" }
    Action { id: playAction; text: "Play"; icon.name: "media-playback-start" }
    Action { id: pauseAction; text: "Pause"; icon.name: "media-playback-pause" }
    Action { id: stopAction; text: "&Stop"; icon.name: "media-playback-stop"; shortcut: "Ctrl+Shift+Space" }
    Action { id: nextAction; text: "&Next"; icon.name: "media-skip-forward"; shortcut: "Ctrl+Right" }

    Controllers.ErrorController {
        id: errors
    }

    Item {
        anchors.fill: parent
        Kirigami.Theme.inherit: false
        Kirigami.Theme.backgroundColor: "%1"
        Kirigami.Theme.textColor: "%3"
        Kirigami.Theme.disabledTextColor: "%4"
        Kirigami.Theme.highlightColor: "%5"
        Kirigami.Theme.negativeTextColor: "%6"

        Components.UiPalette {
            id: tokens
            windowRoot: window
        }

        Column {
            anchors.fill: parent

            Button {
                id: startButton
                text: "Start"
            }

            Panes.TransportBar {
                objectName: "transportBar"
                width: parent.width
                uiBridge: bridge
                uiPalette: tokens
                previousAction: previousAction
                playAction: playAction
                pauseAction: pauseAction
                stopAction: stopAction
                nextAction: nextAction
                volumeMuted: false
                keyboardSeekStepSeconds: 5
                displayedPositionSeconds: window.positionSeconds
                positionText: window.positionText
                toggleMutedVolume: function() {}
                setAppVolume: function(value) { bridge.volume = value }
                normalizedVolumeValue: function(value) { return value }
                seekCommitted: function(value) { window.seeks = window.seeks.concat([value]) }
            }

            Panes.StatusBar {
                id: statusBar
                objectName: "statusBar"
                width: parent.width
                uiBridge: bridge
                uiPalette: tokens
                themeIsDark: tokens.themeIsDark
                errorController: errors
            }
        }
    }
}
)QML")
        .arg(scheme.window, scheme.view, scheme.text, scheme.inactiveText, scheme.highlight, scheme.negative,
             scheme.button)
        .arg(width)
        .toUtf8();
}

QString formatErrors(const QList<QQmlError> &errors) {
    QStringList lines;
    for (const QQmlError &error : errors) {
        lines << error.toString();
    }
    return lines.join(QLatin1Char('\n'));
}

// The harness window, shown and active, with the focus on its start button.
struct Harness {
    QQmlEngine engine;
    std::unique_ptr<QObject> root;
    QQuickWindow *window = nullptr;
    QObject *bridge = nullptr;
    QObject *tokens = nullptr;
    QObject *errors = nullptr;

    bool load(const Scheme &scheme, int width, QString *errorText) {
        QQmlComponent component(&engine);
        component.setData(harnessQml(scheme, width),
                          QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/TransportStatusHarness.qml")));
        root.reset(component.create());
        if (!root) {
            *errorText = formatErrors(component.errors());
            return false;
        }
        window = qobject_cast<QQuickWindow *>(root.get());
        bridge = root->property("bridge").value<QObject *>();
        tokens = root->property("tokens").value<QObject *>();
        errors = root->property("errors").value<QObject *>();
        if (window == nullptr || bridge == nullptr || tokens == nullptr || errors == nullptr
            || !QTest::qWaitForWindowExposed(window)) {
            *errorText = QStringLiteral("the harness did not show");
            return false;
        }
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window)) {
            *errorText = QStringLiteral("the harness did not become active");
            return false;
        }
        auto *start = root->property("startButton").value<QQuickItem *>();
        start->forceActiveFocus(Qt::TabFocusReason);
        return start->hasActiveFocus();
    }

    QQuickItem *item(const QString &name) const {
        return root->findChild<QQuickItem *>(name);
    }

    QColor token(const char *name) const {
        return tokens->property(name).value<QColor>();
    }
};

QVariant evaluateOn(QQuickItem *item, const QString &expression) {
    QQmlExpression evaluated(qmlContext(item), item, expression);
    bool undefined = false;
    const QVariant value = evaluated.evaluate(&undefined);
    return evaluated.hasError() || undefined ? QVariant() : value;
}

// The engine's Kirigami.Units, whose durations Plasma's animation speed
// sets: with animations off, short is 0 and long is clamped to 1 ms.
Kirigami::Platform::Units *unitsOf(QQuickItem *item) {
    return qobject_cast<Kirigami::Platform::Units *>(
        evaluateOn(item, QStringLiteral("Kirigami.Units")).value<QObject *>());
}

QString toolTipText(QQuickItem *item) {
    return evaluateOn(item, QStringLiteral("ToolTip.text")).toString();
}

bool toolTipShown(QQuickItem *item) {
    return evaluateOn(item, QStringLiteral("ToolTip.visible")).toBool();
}

QString accessibleText(QQuickItem *item, QAccessible::Text text) {
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(item);
    return accessible != nullptr ? accessible->text(text) : QString();
}

QRectF sceneRect(QQuickItem *item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

QPoint sceneCentre(QQuickItem *item) {
    return sceneRect(item).center().toPoint();
}

// `top` drawn over the opaque `bottom`.
QColor drawnOver(const QColor &top, const QColor &bottom) {
    const double alpha = top.alphaF();
    return QColor::fromRgbF(static_cast<float>(top.redF() * alpha + bottom.redF() * (1.0 - alpha)),
                            static_cast<float>(top.greenF() * alpha + bottom.greenF() * (1.0 - alpha)),
                            static_cast<float>(top.blueF() * alpha + bottom.blueF() * (1.0 - alpha)));
}

QColor mixed(const QColor &a, const QColor &b, double amount) {
    return QColor::fromRgbF(static_cast<float>(a.redF() * (1.0 - amount) + b.redF() * amount),
                            static_cast<float>(a.greenF() * (1.0 - amount) + b.greenF() * amount),
                            static_cast<float>(a.blueF() * (1.0 - amount) + b.blueF() * amount));
}

QColor withAlpha(const QColor &color, double alpha) {
    QColor result = color;
    result.setAlphaF(static_cast<float>(alpha));
    return result;
}

// The largest difference of the colours' 8-bit channels.
int channelDistance(const QColor &a, const QColor &b) {
    const auto channel = [](double value) { return static_cast<int>(std::lround(value * 255.0)); };
    return std::max({std::abs(channel(a.redF()) - channel(b.redF())),
                     std::abs(channel(a.greenF()) - channel(b.greenF())),
                     std::abs(channel(a.blueF()) - channel(b.blueF()))});
}

bool sameColor(const QColor &a, const QColor &b) {
    return channelDistance(a, b) == 0 && std::lround(a.alphaF() * 255.0) == std::lround(b.alphaF() * 255.0);
}

// WCAG's relative luminance and contrast ratio of opaque colours.
double relativeLuminance(const QColor &color) {
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}

double contrastRatio(const QColor &a, const QColor &b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QString describe(const QString &what, const QColor &a, const QColor &b) {
    return QStringLiteral("%1: %2 on %3 is %4:1")
        .arg(what, a.name(QColor::HexArgb), b.name(QColor::HexArgb))
        .arg(contrastRatio(a, b), 0, 'f', 2);
}

QColor pixelAt(const QImage &image, const QPointF &scenePoint, qreal scale) {
    return image.pixelColor(static_cast<int>(std::floor(scenePoint.x() * scale)),
                            static_cast<int>(std::floor(scenePoint.y() * scale)));
}

// Full-scale peaks: the waveform fills its ±12 px everywhere.
QByteArray fullScalePeaks() {
    return QByteArray(800, static_cast<char>(255));
}

// The window drawn, with the scale of its image to the scene.
QImage grabbed(QQuickWindow *window, qreal *scale) {
    QTest::qWait(50);
    const QImage image = window->grabWindow();
    *scale = window->width() > 0 && !image.isNull() ? static_cast<qreal>(image.width()) / window->width() : 1.0;
    return image;
}

// The scene x of the playhead's line at `fraction` of the way along the
// seek bar: its left edge moves over the bar's whole device pixels less
// the line's two.
qreal playheadLeftAt(QQuickItem *seek, double fraction) {
    const QRectF bar = sceneRect(seek);
    return bar.left() + std::round(fraction * (std::floor(bar.width() + 1e-6) - 2.0));
}

} // namespace

class TransportStatusTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void transportLaysOutFlatButtonsSeekTimeAndVolume();
    void narrowTransportShortensTheVolume();
    void playPauseIsOneButtonThatShowsTheState();
    void playPauseGlyphsNeverDrawTogether();
    void transportToolTipsNameTheShortcut();
    void seekWaveformMeetsItsContrastAsDrawn_data();
    void seekWaveformMeetsItsContrastAsDrawn();
    void seekHoverDrawsALineAndANativeToolTip();
    void seekFocusDrawsOneRingAroundTheBar_data();
    void seekFocusDrawsOneRingAroundTheBar();
    void transportButtonsDrawTheAppsFocusRing_data();
    void transportButtonsDrawTheAppsFocusRing();
    void seekWithoutATrackIsALineAndNoTime();
    void seekPlayheadMovesAcrossAShortTrack_data();
    void seekPlayheadMovesAcrossAShortTrack();
    void seekPlayheadWaitsAtTheStartWithoutALength_data();
    void seekPlayheadWaitsAtTheStartWithoutALength();
    void seekPlayheadThroughAZeroLengthFileThatFails();
    void timeReadoutShowsPositionAndLength();
    void volumeDrawsGrooveHoverAndFocus_data();
    void volumeDrawsGrooveHoverAndFocus();
    void statusBarShowsTheNeutralPlaybackState();
    void statusBarShowsRepeatAndShuffleInWords();
    void statusBarShowsTheFormatGroupWithoutThePosition();
    void statusBarShowsTheScanWithItsBar();
    void statusBarScanBarFillsOnlyOnceTheRescanEnds();
    void statusBarScanWithoutATotalIsStill();
    void lastFmSlotShowsEachState();
    void lastFmSlotCountsTheTimeLeftFromTheListenedTime();
    void lastFmSlotPausedLooksStruckThrough_data();
    void lastFmSlotPausedLooksStruckThrough();
    void lastFmSlotRingTrackReadsAndScrobbledCloses_data();
    void lastFmSlotRingTrackReadsAndScrobbledCloses();
    void lastFmSlotRingProgressReadsAtOneX_data();
    void lastFmSlotRingProgressReadsAtOneX();
    void lastFmSlotToolTipShowsOnKeyboardFocus();
    void issuesButtonShowsNewSeenAndOpen_data();
    void issuesButtonShowsNewSeenAndOpen();
    void statusBarHasNoDividers();
    void transportAndStatusBarGrowWithALargeFont();
};

void TransportStatusTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(recordWarnings);
    QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                     QStringLiteral("icons/breeze/index.theme"))
                  .isEmpty(),
             "The Breeze icon theme is missing: install breeze-icon-theme (docs/INSTALL.md)");
}

void TransportStatusTest::cleanup() {
    const QStringList list = takeWarnings();
    QVERIFY2(list.isEmpty(), qPrintable(list.join(QLatin1Char('\n'))));
}

void TransportStatusTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

// The sizes of the design at 1×: a 48 px band; 32 px buttons 4 px apart
// from 12 px, the content's inset; the seek bar 16 px after Next; the time 72 px, 16 px after it;
// the mute button 16 px after that and the 96 px slider 4 px after it,
// 12 px from the end.
void TransportStatusTest::transportLaysOutFlatButtonsSeekTimeAndVolume() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QVERIFY(transport != nullptr);
    QCOMPARE(transport->height(), 48.0);
    const qreal top = sceneRect(transport).top();

    const QStringList names{QStringLiteral("previous"), QStringLiteral("playPause"), QStringLiteral("stop"),
                            QStringLiteral("next")};
    const QList<int> glyphs{16, 20, 14, 16};
    for (int i = 0; i < names.size(); ++i) {
        QQuickItem *button = harness.item(QStringLiteral("transportButton_") + names[i]);
        QVERIFY2(button != nullptr, qPrintable(names[i]));
        const QRectF rect = sceneRect(button);
        QCOMPARE(rect.size(), QSizeF(32, 32));
        QCOMPARE(rect.left(), 12.0 + i * 36.0);
        QCOMPARE(rect.top() - top, 8.0);
        QVERIFY(button->property("flat").toBool());
        QCOMPARE(button->property("glyphSize").toInt(), glyphs[i]);
    }
    // One Play/Pause button: no Play and Pause buttons of their own.
    QVERIFY(harness.item(QStringLiteral("transportButton_play")) == nullptr);
    QVERIFY(harness.item(QStringLiteral("transportButton_pause")) == nullptr);

    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *time = harness.item(QStringLiteral("transportTime"));
    QQuickItem *mute = harness.item(QStringLiteral("muteButton"));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QVERIFY(seek && time && mute && volume && waveform);
    const QRectF seekRect = sceneRect(seek);
    QCOMPARE(seekRect.left(), 12.0 + 4 * 32.0 + 3 * 4.0 + 16.0);
    QCOMPARE(seekRect.height(), 28.0);
    QCOMPARE(seekRect.top() - top, 10.0);
    // The waveform is ±12 px about the bar's middle, 2 px from its edges.
    QCOMPARE(waveform->height(), 24.0);
    QCOMPARE(sceneRect(waveform).center().y(), seekRect.center().y());
    QCOMPARE(sceneRect(waveform).top() - seekRect.top(), 2.0);
    QCOMPARE(sceneRect(waveform).width(), seekRect.width());
    const QRectF timeRect = sceneRect(time);
    QCOMPARE(timeRect.left(), seekRect.right() + 16.0);
    QCOMPARE(timeRect.width(), 72.0);
    const QRectF muteRect = sceneRect(mute);
    QCOMPARE(muteRect.left(), timeRect.right() + 16.0);
    QCOMPARE(muteRect.size(), QSizeF(32, 32));
    const QRectF volumeRect = sceneRect(volume);
    QCOMPARE(volumeRect.left(), muteRect.right() + 4.0);
    QCOMPARE(volumeRect.width(), 96.0);
    QCOMPARE(sceneRect(transport).right() - volumeRect.right(), 12.0);
}

void TransportStatusTest::narrowTransportShortensTheVolume() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1000, &errorText), qPrintable(errorText));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QVERIFY(volume != nullptr);
    QCOMPARE(volume->width(), 72.0);
}

void TransportStatusTest::playPauseIsOneButtonThatShowsTheState() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *button = harness.item(QStringLiteral("transportButton_playPause"));
    QQuickItem *play = harness.item(QStringLiteral("transportPlayPauseGlyph_play"));
    QQuickItem *pause = harness.item(QStringLiteral("transportPlayPauseGlyph_pause"));
    QVERIFY(button && play && pause);
    const auto actionText = [button]() {
        auto *action = button->property("action").value<QObject *>();
        return action != nullptr ? action->property("text").toString() : QString();
    };

    // Playing: Pause.
    QCOMPARE(actionText(), QStringLiteral("Pause"));
    QCOMPARE(accessibleText(button, QAccessible::Name), QStringLiteral("Pause"));
    QTRY_VERIFY(pause->opacity() == 1.0 && !play->isVisible());

    // Paused and stopped: Play. The glyphs cross-fade.
    for (const QString &state : {QStringLiteral("Paused"), QStringLiteral("Stopped")}) {
        harness.bridge->setProperty("playbackState", state);
        QCOMPARE(actionText(), QStringLiteral("Play"));
        QCOMPARE(accessibleText(button, QAccessible::Name), QStringLiteral("Play"));
        QTRY_VERIFY(play->opacity() == 1.0 && !pause->isVisible());
    }
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QCOMPARE(actionText(), QStringLiteral("Pause"));
    QTRY_VERIFY(pause->opacity() == 1.0 && !play->isVisible());

    // A click triggers what it shows.
    auto *pauseAction = button->property("action").value<QObject *>();
    QSignalSpy paused(pauseAction, SIGNAL(triggered(QObject*)));
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(button));
    QTRY_COMPARE(paused.count(), 1);
}

// The Play/Pause glyph swaps without a frame of both shapes at once: the
// glyph shown fades out over the first half of shortDuration, then the other
// fades in over the second (#373 H1: a mashed glyph for a frame or two). A
// change back while the glyph fades out brings it back; with animations off
// the swap is instant.
void TransportStatusTest::playPauseGlyphsNeverDrawTogether() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *play = harness.item(QStringLiteral("transportPlayPauseGlyph_play"));
    QQuickItem *pause = harness.item(QStringLiteral("transportPlayPauseGlyph_pause"));
    QVERIFY(play && pause);
    auto *swap = play->parentItem()->property("swap").value<QObject *>();
    auto *units = unitsOf(play);
    QVERIFY(swap != nullptr && units != nullptr);
    QCOMPARE(swap->objectName(), QStringLiteral("transportPlayPauseSwap"));
    // Long enough for frames in each half.
    units->setShortDuration(600);
    QTRY_VERIFY(pause->opacity() == 1.0 && !play->isVisible());

    const auto drawn = [](QQuickItem *glyph) { return glyph->isVisible() && glyph->opacity() > 0.0; };
    QString failure;
    int frames = 0;
    const QMetaObject::Connection connection =
        QObject::connect(harness.window, &QQuickWindow::frameSwapped, harness.window, [&]() {
            ++frames;
            if (failure.isEmpty() && drawn(play) && drawn(pause)) {
                failure = QStringLiteral("both glyphs at frame %1: play %2, pause %3")
                              .arg(frames).arg(play->opacity()).arg(pause->opacity());
            }
        });
    const auto check = [&](const char *where) {
        QVERIFY2(!(drawn(play) && drawn(pause)), where);
    };

    // Playing to paused: Pause goes out, then Play comes in.
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QVERIFY(swap->property("running").toBool());
    check("at the start");
    QVERIFY(drawn(pause));
    QTRY_VERIFY(!drawn(pause));
    check("half-way");
    QTRY_VERIFY(!swap->property("running").toBool());
    QCOMPARE(play->opacity(), 1.0);
    QVERIFY(!pause->isVisible());

    // Playing again, and back to paused while Play fades out: Play comes
    // back, and Pause never shows.
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_VERIFY(play->opacity() < 1.0);
    QVERIFY(drawn(play));
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QVERIFY(!swap->property("running").toBool());
    QTRY_COMPARE(play->opacity(), 1.0);
    QVERIFY(!pause->isVisible());
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(frames > 0);

    // Animations off: at once.
    units->setShortDuration(0);
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QVERIFY(!swap->property("running").toBool());
    QCOMPARE(pause->opacity(), 1.0);
    QVERIFY(!play->isVisible());
}

void TransportStatusTest::transportToolTipsNameTheShortcut() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *next = harness.item(QStringLiteral("transportButton_next"));
    QQuickItem *stop = harness.item(QStringLiteral("transportButton_stop"));
    QQuickItem *playPause = harness.item(QStringLiteral("transportButton_playPause"));
    QVERIFY(next && stop && playPause);
    QCOMPARE(toolTipText(next), QStringLiteral("Next · Ctrl+Right"));
    QCOMPARE(toolTipText(stop), QStringLiteral("Stop · Ctrl+Shift+Space"));
    QCOMPARE(toolTipText(playPause), QStringLiteral("Pause"));
    // Keyboard focus shows it, as hover does.
    next->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(toolTipShown(next));
}

void TransportStatusTest::seekWaveformMeetsItsContrastAsDrawn_data() {
    QTest::addColumn<int>("scheme");
    QTest::addColumn<double>("measured");
    QTest::newRow("breeze dark") << 0 << 3.35;
    QTest::newRow("breeze light") << 1 << 3.16;
    // Not measured by the design: the opacity rises to keep 3:1.
    QTest::newRow("dark, purple accent") << 2 << 0.0;
}

// The played part is `wave` mixed 12% toward the chrome (dark) or 20%
// toward the text (light); the unplayed part is it at 50% or 62%, which is
// at least 3:1 on the chrome. The playhead is a 2 px amber line across the
// 28 px bar, past the ±12 px waveform, with a casing on each side that
// stands apart from the played part. Measured in the drawn window.
void TransportStatusTest::seekWaveformMeetsItsContrastAsDrawn() {
    QFETCH(int, scheme);
    QFETCH(double, measured);
    const Scheme &colors = scheme == 0 ? kBreezeDark : (scheme == 1 ? kBreezeLight : kPurpleDark);
    const bool dark = scheme != 1;
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(colors, 1600, &errorText), qPrintable(errorText));
    harness.bridge->setProperty("waveformPeaksPacked", fullScalePeaks());
    harness.bridge->setProperty("waveformComplete", true);
    harness.bridge->setProperty("waveformCoverageSeconds", 229.0);
    harness.root->setProperty("positionSeconds", 229.0 / 2);
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QQuickItem *line = harness.item(QStringLiteral("seekPlayheadLine"));
    QQuickItem *casing = harness.item(QStringLiteral("seekPlayhead"));
    QQuickItem *unplayed = harness.item(QStringLiteral("seekUnplayed"));
    QQuickItem *emptyLine = harness.item(QStringLiteral("seekEmptyLine"));
    QVERIFY(transport && waveform && line && casing && unplayed && emptyLine);
    QTRY_VERIFY(!emptyLine->isVisible());
    QVERIFY(unplayed->isVisible() && line->isVisible());

    const QColor chrome = harness.token("chrome");
    const QColor wave = harness.token("wave");
    const QColor text = harness.token("text");
    const QColor expectedPlayed = dark ? mixed(wave, chrome, 0.12) : mixed(wave, text, 0.2);
    QVERIFY2(channelDistance(transport->property("seekPlayedColor").value<QColor>(), expectedPlayed) <= 1,
             qPrintable(transport->property("seekPlayedColor").value<QColor>().name()));
    const qreal opacity = transport->property("seekUnplayedOpacity").toReal();
    if (measured > 0) {
        QCOMPARE(opacity, dark ? 0.5 : 0.62);
    } else {
        QVERIFY2(opacity > 0.5 && opacity < 0.7, qPrintable(QString::number(opacity)));
    }
    QVERIFY(sameColor(line->property("color").value<QColor>(), harness.token("now")));
    QVERIFY(sameColor(casing->property("color").value<QColor>(),
                      dark ? QColor::fromRgbF(0, 0, 0, 0.6f) : QColor::fromRgbF(1, 1, 1, 0.85f)));
    QCOMPARE(line->height(), 28.0);
    QCOMPARE(line->width(), 2.0);

    QTest::qWait(50);
    const QImage image = harness.window->grabWindow();
    QVERIFY(!image.isNull());
    const qreal scale = harness.window->width() > 0
        ? static_cast<qreal>(image.width()) / harness.window->width()
        : 1.0;
    const QRectF wave_ = sceneRect(waveform);
    const qreal middle = wave_.center().y();
    const QColor drawnChrome = pixelAt(image, QPointF(wave_.left() + 10, wave_.top() - 3), scale);
    const QColor drawnPlayed = pixelAt(image, QPointF(wave_.left() + wave_.width() * 0.25, middle), scale);
    const QColor drawnUnplayed = pixelAt(image, QPointF(wave_.left() + wave_.width() * 0.75, middle), scale);
    const QRectF lineRect = sceneRect(line);
    const QColor drawnLine = pixelAt(image, QPointF(lineRect.left() + 0.5, middle), scale);
    const QColor drawnCasingLeft = pixelAt(image, QPointF(lineRect.left() - 0.5, middle), scale);
    const QColor drawnCasingRight = pixelAt(image, QPointF(lineRect.right() + 0.5, middle), scale);

    QVERIFY2(channelDistance(drawnChrome, chrome) <= 1, qPrintable(drawnChrome.name()));
    QVERIFY2(channelDistance(drawnPlayed, expectedPlayed) <= 2,
             qPrintable(drawnPlayed.name() + QStringLiteral(" vs ") + expectedPlayed.name()));
    const QColor expectedUnplayed = mixed(chrome, expectedPlayed, opacity);
    QVERIFY2(channelDistance(drawnUnplayed, expectedUnplayed) <= 2,
             qPrintable(drawnUnplayed.name() + QStringLiteral(" vs ") + expectedUnplayed.name()));
    QVERIFY2(channelDistance(drawnLine, harness.token("now")) <= 1, qPrintable(drawnLine.name()));
    // Full-scale peaks reach the waveform's top and bottom rows, played
    // and unplayed, and the bar is the chrome just outside them.
    for (const qreal y : {wave_.top() + 0.5, wave_.bottom() - 0.5}) {
        const QColor playedEdge = pixelAt(image, QPointF(wave_.left() + wave_.width() * 0.25, y), scale);
        const QColor unplayedEdge = pixelAt(image, QPointF(wave_.left() + wave_.width() * 0.75, y), scale);
        QVERIFY2(channelDistance(playedEdge, expectedPlayed) <= 2, qPrintable(playedEdge.name()));
        QVERIFY2(channelDistance(unplayedEdge, mixed(chrome, expectedPlayed, opacity)) <= 2,
                 qPrintable(unplayedEdge.name()));
    }
    for (const qreal y : {wave_.top() - 0.5, wave_.bottom() + 0.5}) {
        const QColor outside = pixelAt(image, QPointF(wave_.left() + wave_.width() * 0.25, y), scale);
        QVERIFY2(channelDistance(outside, chrome) <= 1, qPrintable(outside.name()));
    }

    // The unplayed part is at least 3:1 on the chrome, as the design
    // measured it (3.35:1 dark, 3.16:1 light), and the played part more.
    const double unplayedContrast = contrastRatio(drawnUnplayed, drawnChrome);
    QVERIFY2(unplayedContrast >= 3.0, qPrintable(describe(QStringLiteral("unplayed"), drawnUnplayed, drawnChrome)));
    QVERIFY2(measured <= 0 || std::abs(unplayedContrast - measured) < 0.1,
             qPrintable(describe(QStringLiteral("unplayed"), drawnUnplayed, drawnChrome)));
    QVERIFY2(contrastRatio(drawnPlayed, drawnChrome) >= 4.5,
             qPrintable(describe(QStringLiteral("played"), drawnPlayed, drawnChrome)));
    // The casing separates the line from the played part by luminance.
    for (const QColor &casingColor : {drawnCasingLeft, drawnCasingRight}) {
        QVERIFY2(channelDistance(casingColor, drawnOver(casing->property("color").value<QColor>(), expectedPlayed)) <= 2
                     || channelDistance(casingColor,
                                        drawnOver(casing->property("color").value<QColor>(), expectedUnplayed)) <= 2,
                 qPrintable(casingColor.name()));
        QVERIFY2(contrastRatio(casingColor, drawnLine) >= 3.0,
                 qPrintable(describe(QStringLiteral("line on its casing"), drawnLine, casingColor)));
    }
    QVERIFY2(contrastRatio(drawnCasingLeft, drawnPlayed) >= 3.0,
             qPrintable(describe(QStringLiteral("casing on the played part"), drawnCasingLeft, drawnPlayed)));
    QVERIFY2(contrastRatio(drawnLine, drawnChrome) >= 3.0,
             qPrintable(describe(QStringLiteral("playhead"), drawnLine, drawnChrome)));
}

void TransportStatusTest::seekHoverDrawsALineAndANativeToolTip() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *hoverLine = harness.item(QStringLiteral("seekHoverLine"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(seek && hoverLine && tip);
    QVERIFY(tip->inherits("QQuickToolTip"));
    QVERIFY(!hoverLine->isVisible());
    QVERIFY(!tip->property("visible").toBool());

    // Over the bar, at 62% of its width.
    const QRectF bar = sceneRect(seek);
    const QPointF at(bar.left() + std::round(bar.width() * 0.62), bar.center().y());
    QTest::mouseMove(harness.window, at.toPoint());
    QTRY_VERIFY(hoverLine->isVisible());
    QCOMPARE(sceneRect(hoverLine).left(), std::floor(at.x()));
    QCOMPARE(hoverLine->width(), 1.0);
    QCOMPARE(hoverLine->height(), 28.0);
    QVERIFY(sameColor(hoverLine->property("color").value<QColor>(), withAlpha(harness.token("text"), 0.7)));
    QTRY_VERIFY(tip->property("visible").toBool());
    const double seconds = seek->property("hoverSecond").toInt();
    const QString expected = QStringLiteral("%1:%2")
                                 .arg(static_cast<int>(seconds) / 60)
                                 .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'));
    QCOMPARE(tip->property("text").toString(), expected);
    QVERIFY2(!expected.startsWith(QLatin1Char('0')), qPrintable(expected));
    // Under the bar, centred on the pointer.
    const qreal tipX = tip->property("x").toReal();
    const qreal tipWidth = tip->property("width").toReal();
    QVERIFY2(std::abs(tipX + tipWidth / 2 - (at.x() - bar.left())) <= 1.0,
             qPrintable(QStringLiteral("%1 + %2 / 2").arg(tipX).arg(tipWidth)));
    QVERIFY(tip->property("y").toReal() >= seek->height());

    // Away from the bar, both go.
    QTest::mouseMove(harness.window, QPoint(static_cast<int>(at.x()), static_cast<int>(bar.bottom() + 60)));
    QTRY_VERIFY(!hoverLine->isVisible());
    QTRY_VERIFY(!tip->property("visible").toBool());
}

void TransportStatusTest::seekFocusDrawsOneRingAroundTheBar_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

void TransportStatusTest::seekFocusDrawsOneRingAroundTheBar() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *ring = harness.item(QStringLiteral("seekFocusRing"));
    QVERIFY(seek && ring);
    QVERIFY(!ring->isVisible());
    seek->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(ring->isVisible());
    // A 2 px ring whose inside is 3 px clear of the bar.
    QCOMPARE(evaluateOn(ring, QStringLiteral("border.width")).toInt(), 2);
    const QRectF bar = sceneRect(seek);
    const QRectF outer = sceneRect(ring);
    QCOMPARE(outer, bar.adjusted(-5, -5, 5, 5));
    const QColor ringColor = evaluateOn(ring, QStringLiteral("border.color")).value<QColor>();
    QVERIFY(sameColor(ringColor, harness.token("focusRing")));
    QVERIFY2(contrastRatio(ringColor, harness.token("chrome")) >= 3.0,
             qPrintable(describe(QStringLiteral("focus ring"), ringColor, harness.token("chrome"))));
    // Left and Right seek by 5 s.
    QTest::keyClick(harness.window, Qt::Key_Right);
    const QVariantList seeks = harness.root->property("seeks").value<QJSValue>().toVariant().toList();
    QCOMPARE(seeks.size(), 1);
    QCOMPARE(seeks.first().toDouble(), 82.0);
}

void TransportStatusTest::transportButtonsDrawTheAppsFocusRing_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The transport's buttons draw their own background: the hover tint, and
// for keyboard focus the seek bar's and the volume's 2 px ring, 3 px out,
// which meets 3:1 on the chrome in both themes. The style's frame, 2.18:1
// on the light chrome, is not drawn: the background is the app's.
void TransportStatusTest::transportButtonsDrawTheAppsFocusRing() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    const QColor chrome = harness.token("chrome");
    for (const QString &name : {QStringLiteral("transportButton_previous"), QStringLiteral("transportButton_playPause"),
                                QStringLiteral("transportButton_stop"), QStringLiteral("transportButton_next"),
                                QStringLiteral("muteButton")}) {
        QQuickItem *button = harness.item(name);
        QVERIFY2(button != nullptr, qPrintable(name));
        auto *background = button->property("background").value<QQuickItem *>();
        QVERIFY2(background != nullptr, qPrintable(name));
        const QList<QQuickItem *> rings = background->findChildren<QQuickItem *>(
            QStringLiteral("transportButtonFocusRing"));
        const QList<QQuickItem *> hovers = background->findChildren<QQuickItem *>(
            QStringLiteral("transportButtonHover"));
        // The app's background, with nothing else in it: no style frame.
        QCOMPARE(rings.size(), 1);
        QCOMPARE(hovers.size(), 1);
        QCOMPARE(background->childItems().size(), 2);
        QQuickItem *ring = rings.first();
        QQuickItem *hover = hovers.first();
        QVERIFY2(!ring->isVisible(), qPrintable(name));

        // Hover: the tint, and no ring. The buttons follow the platform's
        // hover effects, which the offscreen platform turns off.
        button->setProperty("hoverEnabled", true);
        QTest::mouseMove(harness.window, sceneCentre(button));
        QTRY_VERIFY2(hover->isVisible() && hover->opacity() == 1.0, qPrintable(name));
        QVERIFY(sameColor(hover->property("color").value<QColor>(), harness.token("hover")));
        QVERIFY(!ring->isVisible());
        QTest::mouseMove(harness.window, QPoint(10, harness.window->height() - 10));
        QTRY_VERIFY2(!hover->isVisible(), qPrintable(name));

        // Keyboard focus: one 2 px ring, 3 px out from the button.
        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY2(ring->isVisible(), qPrintable(name));
        QCOMPARE(evaluateOn(ring, QStringLiteral("border.width")).toInt(), 2);
        QCOMPARE(sceneRect(ring), sceneRect(button).adjusted(-3, -3, 3, 3));
        const QColor ringColor = evaluateOn(ring, QStringLiteral("border.color")).value<QColor>();
        QVERIFY(sameColor(ringColor, harness.token("focusRing")));
        QVERIFY2(contrastRatio(ringColor, chrome) >= 3.0,
                 qPrintable(describe(name, ringColor, chrome)));
        harness.item(QStringLiteral("seekSlider"))->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY2(!ring->isVisible(), qPrintable(name));
    }
    // The mute glyph, which the style's background drew before, is drawn.
    QQuickItem *muteGlyph = harness.item(QStringLiteral("muteButtonGlyph"));
    QVERIFY(muteGlyph != nullptr && muteGlyph->isVisible());
    QVERIFY(sameColor(muteGlyph->property("color").value<QColor>(), harness.token("text")));
    QCOMPARE(muteGlyph->width(), 16.0);
}

void TransportStatusTest::seekWithoutATrackIsALineAndNoTime() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QQuickItem *emptyLine = harness.item(QStringLiteral("seekEmptyLine"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *time = harness.item(QStringLiteral("transportTime"));
    QQuickItem *position = harness.item(QStringLiteral("transportTimePosition"));
    QVERIFY(emptyLine && seek && time && position);
    QTRY_VERIFY(emptyLine->isVisible());
    QCOMPARE(emptyLine->height(), 1.0);
    QCOMPARE(emptyLine->width(), seek->width());
    QCOMPARE(sceneRect(emptyLine).top(), std::round(sceneRect(seek).center().y() - 0.5));
    QVERIFY(sameColor(emptyLine->property("color").value<QColor>(), withAlpha(harness.token("text"), 0.18)));
    for (const char *name : {"seekPlayhead", "seekUnplayed", "seekWaveform"}) {
        QVERIFY2(!harness.item(QString::fromLatin1(name))->isVisible(), name);
    }
    QCOMPARE(time->property("text").toString(), QStringLiteral("–:––"));
    QVERIFY(sameColor(position->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(!seek->isEnabled());
}

// #541: a clip of a second or less has a length, so the playhead is drawn
// and moves across the waveform, its length shows and the bar seeks.
void TransportStatusTest::seekPlayheadMovesAcrossAShortTrack_data() {
    QTest::addColumn<double>("duration");
    QTest::addColumn<QString>("durationText");
    QTest::addColumn<QString>("lengthShown");
    QTest::newRow("one second") << 1.0 << QStringLiteral("00:01") << QStringLiteral("0:01");
    QTest::newRow("under a second") << 0.4 << QStringLiteral("00:00") << QStringLiteral("0:00");
}

void TransportStatusTest::seekPlayheadMovesAcrossAShortTrack() {
    QFETCH(double, duration);
    QFETCH(QString, durationText);
    QFETCH(QString, lengthShown);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    harness.root->setProperty("positionSeconds", 0.0);
    harness.root->setProperty("positionText", QStringLiteral("00:00"));
    harness.bridge->setProperty("durationSeconds", duration);
    harness.bridge->setProperty("durationText", durationText);
    harness.bridge->setProperty("waveformPeaksPacked", fullScalePeaks());
    harness.bridge->setProperty("waveformCoverageSeconds", duration);
    harness.bridge->setProperty("waveformComplete", true);
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QQuickItem *casing = harness.item(QStringLiteral("seekPlayhead"));
    QQuickItem *line = harness.item(QStringLiteral("seekPlayheadLine"));
    QQuickItem *unplayed = harness.item(QStringLiteral("seekUnplayed"));
    QQuickItem *length = harness.item(QStringLiteral("transportTimeLength"));
    QVERIFY(seek && waveform && casing && line && unplayed && length);
    QVERIFY(seek->isEnabled());
    QCOMPARE(length->property("text").toString(), QStringLiteral(" / ") + lengthShown);

    qreal previousLeft = -1;
    for (const double fraction : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const QByteArray at = QByteArray::number(fraction);
        harness.root->setProperty("positionSeconds", duration * fraction);
        QVERIFY2(casing->isVisible() && line->isVisible() && unplayed->isVisible(), at.constData());
        QCOMPARE(line->width(), 2.0);
        QCOMPARE(line->height(), 28.0);
        const QRectF lineRect = sceneRect(line);
        const QRectF waveRect = sceneRect(waveform);
        QVERIFY2(qFuzzyCompare(lineRect.left() + 1, playheadLeftAt(seek, fraction) + 1),
                 qPrintable(QStringLiteral("%1 at %2").arg(lineRect.left()).arg(fraction)));
        QVERIFY2(lineRect.left() > previousLeft, at.constData());
        QVERIFY2(lineRect.left() >= waveRect.left() && lineRect.right() <= waveRect.right(), at.constData());
        // The unplayed part starts at the line.
        QCOMPARE(sceneRect(unplayed).left(), lineRect.left());
        previousLeft = lineRect.left();
    }
    // At the end, the line is at the bar's end, cut by nothing.
    QCOMPARE(sceneRect(line).right(), sceneRect(seek).left() + std::floor(sceneRect(seek).width() + 1e-6));

    // Drawn halfway, in the playhead's amber over the waveform.
    harness.root->setProperty("positionSeconds", duration / 2);
    qreal scale = 1.0;
    const QImage image = grabbed(harness.window, &scale);
    QVERIFY(!image.isNull());
    const QRectF lineRect = sceneRect(line);
    const QColor drawn = pixelAt(image, QPointF(lineRect.left() + 0.5, sceneRect(waveform).center().y()), scale);
    QVERIFY2(channelDistance(drawn, harness.token("now")) <= 1, qPrintable(drawn.name()));

    // The keys seek within the short track.
    seek->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(harness.window, Qt::Key_Right);
    const QVariantList seeks = harness.root->property("seeks").value<QJSValue>().toVariant().toList();
    QCOMPARE(seeks.size(), 1);
    QCOMPARE(seeks.first().toDouble(), duration);
}

// #541: until the length is known, or when what the engine reports is no
// length, the playhead stays drawn at the start with the whole waveform
// unplayed, the bar does not seek, and the length shows as unknown. When
// the length comes, the playhead moves to the position.
void TransportStatusTest::seekPlayheadWaitsAtTheStartWithoutALength_data() {
    QTest::addColumn<double>("duration");
    QTest::newRow("not yet known") << 0.0;
    QTest::newRow("not a number") << std::numeric_limits<double>::quiet_NaN();
    QTest::newRow("infinite") << std::numeric_limits<double>::infinity();
}

void TransportStatusTest::seekPlayheadWaitsAtTheStartWithoutALength() {
    QFETCH(double, duration);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    harness.bridge->setProperty("durationSeconds", duration);
    harness.bridge->setProperty("durationText", QStringLiteral("00:00"));
    harness.bridge->setProperty("waveformPeaksPacked", fullScalePeaks());
    harness.bridge->setProperty("waveformCoverageSeconds", 0.6);
    harness.bridge->setProperty("waveformComplete", false);
    harness.root->setProperty("positionSeconds", 1.0);
    harness.root->setProperty("positionText", QStringLiteral("00:01"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QQuickItem *casing = harness.item(QStringLiteral("seekPlayhead"));
    QQuickItem *line = harness.item(QStringLiteral("seekPlayheadLine"));
    QQuickItem *unplayed = harness.item(QStringLiteral("seekUnplayed"));
    QQuickItem *time = harness.item(QStringLiteral("transportTime"));
    QVERIFY(seek && waveform && casing && line && unplayed && time);

    QVERIFY(casing->isVisible() && line->isVisible());
    QCOMPARE(line->width(), 2.0);
    QCOMPARE(line->height(), 28.0);
    QCOMPARE(sceneRect(line).left(), sceneRect(seek).left());
    QVERIFY(unplayed->isVisible());
    QCOMPARE(sceneRect(unplayed).left(), sceneRect(waveform).left());
    QCOMPARE(sceneRect(unplayed).right(), sceneRect(waveform).right());
    QVERIFY(!seek->isEnabled());
    QCOMPARE(seek->property("value").toDouble(), 0.0);
    QCOMPARE(time->property("text").toString(), QStringLiteral("0:01 / –:––"));
    qreal scale = 1.0;
    const QImage image = grabbed(harness.window, &scale);
    QVERIFY(!image.isNull());
    const QColor drawn = pixelAt(image, QPointF(sceneRect(line).left() + 0.5, sceneRect(waveform).center().y()), scale);
    QVERIFY2(channelDistance(drawn, harness.token("now")) <= 1, qPrintable(drawn.name()));

    // The length comes: a 2 s track, 1 s in.
    harness.bridge->setProperty("durationSeconds", 2.0);
    harness.bridge->setProperty("durationText", QStringLiteral("00:02"));
    QVERIFY(seek->isEnabled());
    QCOMPARE(sceneRect(line).left(), playheadLeftAt(seek, 0.5));
    QCOMPARE(sceneRect(unplayed).left(), sceneRect(line).left());
    QCOMPARE(time->property("text").toString(), QStringLiteral("0:01 / 0:02"));
}

// #541: a zero-length file plays with no length and no waveform, so the
// playhead waits at the start over the empty line; when it fails, playback
// stops and the bar is empty; the next track's playhead is drawn again.
void TransportStatusTest::seekPlayheadThroughAZeroLengthFileThatFails() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    harness.bridge->setProperty("durationSeconds", 0.0);
    harness.bridge->setProperty("durationText", QStringLiteral("00:00"));
    harness.root->setProperty("positionSeconds", 0.0);
    harness.root->setProperty("positionText", QStringLiteral("00:00"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *emptyLine = harness.item(QStringLiteral("seekEmptyLine"));
    QQuickItem *casing = harness.item(QStringLiteral("seekPlayhead"));
    QQuickItem *line = harness.item(QStringLiteral("seekPlayheadLine"));
    QQuickItem *time = harness.item(QStringLiteral("transportTime"));
    QVERIFY(seek && emptyLine && casing && line && time);
    QVERIFY(emptyLine->isVisible());
    QVERIFY(casing->isVisible() && line->isVisible());
    QCOMPARE(sceneRect(line).left(), sceneRect(seek).left());
    QVERIFY(!seek->isEnabled());
    QCOMPARE(time->property("text").toString(), QStringLiteral("0:00 / –:––"));

    // It fails: playback stops.
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    for (const char *name : {"seekPlayhead", "seekUnplayed", "seekWaveform"}) {
        QVERIFY2(!harness.item(QString::fromLatin1(name))->isVisible(), name);
    }
    QVERIFY(emptyLine->isVisible());
    QCOMPARE(time->property("text").toString(), QStringLiteral("–:––"));

    // The next track, a second long, half played.
    harness.bridge->setProperty("durationSeconds", 1.0);
    harness.bridge->setProperty("durationText", QStringLiteral("00:01"));
    harness.root->setProperty("positionSeconds", 0.5);
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QVERIFY(casing->isVisible() && line->isVisible());
    QCOMPARE(sceneRect(line).left(), playheadLeftAt(seek, 0.5));
    QVERIFY(seek->isEnabled());
}

void TransportStatusTest::timeReadoutShowsPositionAndLength() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *time = harness.item(QStringLiteral("transportTime"));
    QQuickItem *position = harness.item(QStringLiteral("transportTimePosition"));
    QQuickItem *length = harness.item(QStringLiteral("transportTimeLength"));
    QVERIFY(time && position && length);
    QCOMPARE(time->property("text").toString(), QStringLiteral("1:17 / 3:49"));
    QVERIFY(sameColor(position->property("color").value<QColor>(), harness.token("text")));
    QVERIFY(sameColor(length->property("color").value<QColor>(), harness.token("textSecondary")));
    // Tabular figures, so the time does not shift as it counts.
    const QFont font = position->property("font").value<QFont>();
    QVERIFY(font.featureTags().contains(QFont::Tag("tnum")));
    // The clock's text changes once a second.
    harness.root->setProperty("positionText", QStringLiteral("01:18"));
    QCOMPARE(time->property("text").toString(), QStringLiteral("1:18 / 3:49"));
    // Hours: the readout grows to fit, and keeps the hours' leading digits.
    harness.root->setProperty("positionText", QStringLiteral("1:02:03"));
    harness.bridge->setProperty("durationText", QStringLiteral("1:20:00"));
    harness.bridge->setProperty("durationSeconds", 4800.0);
    QCOMPARE(time->property("text").toString(), QStringLiteral("1:02:03 / 1:20:00"));
    QTRY_VERIFY(time->width() >= time->childItems().first()->implicitWidth());
}

void TransportStatusTest::volumeDrawsGrooveHoverAndFocus_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

void TransportStatusTest::volumeDrawsGrooveHoverAndFocus() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *groove = harness.item(QStringLiteral("volumeGroove"));
    QQuickItem *fill = harness.item(QStringLiteral("volumeGrooveFill"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QQuickItem *ring = harness.item(QStringLiteral("volumeFocusRing"));
    QVERIFY(volume && groove && fill && thumb && ring);
    const QColor text = harness.token("text");
    const QColor chrome = harness.token("chrome");

    // A 2 px groove, filled at text 65% up to the thumb's centre, the rest
    // at 25%; a 14 px thumb.
    QCOMPARE(groove->height(), 2.0);
    QCOMPARE(groove->width(), volume->width());
    QVERIFY(sameColor(groove->property("color").value<QColor>(), withAlpha(text, 0.25)));
    QVERIFY(sameColor(fill->property("color").value<QColor>(), withAlpha(text, 0.65)));
    QCOMPARE(fill->width(), thumb->x() + thumb->width() / 2);
    QCOMPARE(thumb->width(), 14.0);
    QCOMPARE(thumb->height(), 14.0);
    QVERIFY2(contrastRatio(drawnOver(withAlpha(text, 0.65), chrome), chrome) >= 3.0,
             qPrintable(describe(QStringLiteral("filled groove"), drawnOver(withAlpha(text, 0.65), chrome), chrome)));
    const QColor restBorder = evaluateOn(thumb, QStringLiteral("border.color")).value<QColor>();
    const QColor restFill = thumb->property("color").value<QColor>();
    QVERIFY(sameColor(restBorder, withAlpha(text, 0.5)));

    // Hover tints the thumb and shows "Volume 80%".
    QTest::mouseMove(harness.window, sceneCentre(volume));
    QTRY_VERIFY(volume->property("hovered").toBool());
    QTRY_VERIFY(!sameColor(thumb->property("color").value<QColor>(), restFill));
    QVERIFY(sameColor(evaluateOn(thumb, QStringLiteral("border.color")).value<QColor>(), harness.token("accent")));
    QCOMPARE(toolTipText(volume), QStringLiteral("Volume 80%"));
    QTRY_VERIFY(toolTipShown(volume));
    QVERIFY(!ring->isVisible());

    // Away, the thumb is at rest again; keyboard focus draws a 2 px ring
    // around it and shows the tooltip too.
    QTest::mouseMove(harness.window, QPoint(10, harness.window->height() - 10));
    QTRY_VERIFY(!volume->property("hovered").toBool());
    QTRY_VERIFY(sameColor(thumb->property("color").value<QColor>(), restFill));
    volume->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(ring->isVisible());
    QCOMPARE(evaluateOn(ring, QStringLiteral("border.width")).toInt(), 2);
    QCOMPARE(sceneRect(ring), sceneRect(thumb).adjusted(-3, -3, 3, 3));
    QVERIFY(sameColor(evaluateOn(ring, QStringLiteral("border.color")).value<QColor>(), harness.token("focusRing")));
    QVERIFY2(contrastRatio(harness.token("focusRing"), chrome) >= 3.0,
             qPrintable(describe(QStringLiteral("volume focus"), harness.token("focusRing"), chrome)));
    QTRY_VERIFY(toolTipShown(volume));
    // A key's step is the slider's tenth.
    QTest::keyClick(harness.window, Qt::Key_Left);
    QTRY_COMPARE(toolTipText(volume), QStringLiteral("Volume 70%"));
}

void TransportStatusTest::statusBarShowsTheNeutralPlaybackState() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QQuickItem *label = harness.item(QStringLiteral("statusBarPlaybackStateLabel"));
    QQuickItem *glyph = harness.item(QStringLiteral("statusBarPlaybackGlyph"));
    QVERIFY(status && label && glyph);
    // 24 px of chrome under its 1 px seam; 12 px text, 12 px from the left.
    QCOMPARE(status->height(), 25.0);
    QCOMPARE(sceneRect(glyph).left(), 12.0);
    QCOMPARE(label->property("font").value<QFont>().pointSizeF(),
             harness.tokens->property("smallPointSize").toReal());

    QCOMPARE(label->property("text").toString(), QStringLiteral("Playing"));
    QVERIFY(glyph->isVisible());
    QCOMPARE(glyph->width(), 11.0);
    QCOMPARE(glyph->property("source").toString(), QStringLiteral("media-playback-start"));
    // Neutral: never amber or the accent.
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    const qreal labelLeft = sceneRect(label).left();
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QCOMPARE(label->property("text").toString(), QStringLiteral("Paused"));
    QCOMPARE(glyph->property("source").toString(), QStringLiteral("media-playback-pause"));
    QCOMPARE(sceneRect(label).left(), labelLeft);
    // Stopped has its glyph too, so the word starts at the same place in
    // every state.
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QCOMPARE(label->property("text").toString(), QStringLiteral("Stopped"));
    QVERIFY(glyph->isVisible());
    QCOMPARE(glyph->width(), 11.0);
    QCOMPARE(glyph->property("source").toString(), QStringLiteral("media-playback-stop"));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QTRY_COMPARE(sceneRect(label).left(), labelLeft);
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("textSecondary")));
}

void TransportStatusTest::statusBarShowsRepeatAndShuffleInWords() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *group = harness.item(QStringLiteral("statusBarRepeatShuffle"));
    QQuickItem *repeat = harness.item(QStringLiteral("statusBarRepeatLabel"));
    QQuickItem *shuffle = harness.item(QStringLiteral("statusBarShuffleGlyph"));
    QQuickItem *state = harness.item(QStringLiteral("statusBarPlaybackState"));
    QVERIFY(group && repeat && shuffle && state);
    // Off: nothing.
    QVERIFY(!group->isVisible());
    harness.bridge->setProperty("repeatMode", 1);
    QTRY_VERIFY(repeat->isVisible());
    QCOMPARE(repeat->property("text").toString(), QStringLiteral("Repeat One Track"));
    QVERIFY(!shuffle->isVisible());
    // 16 px after the playback state.
    QTRY_COMPARE(sceneRect(group).left(), sceneRect(state).right() + 16.0);
    harness.bridge->setProperty("repeatMode", 2);
    QCOMPARE(repeat->property("text").toString(), QStringLiteral("Repeat Whole Queue"));
    harness.bridge->setProperty("shuffleEnabled", true);
    QTRY_VERIFY(shuffle->isVisible());
    QCOMPARE(accessibleText(shuffle, QAccessible::Name), QStringLiteral("Shuffle On"));
    harness.bridge->setProperty("repeatMode", 0);
    QTRY_VERIFY(!repeat->isVisible());
    QVERIFY(group->isVisible() && shuffle->isVisible());
}

void TransportStatusTest::statusBarShowsTheFormatGroupWithoutThePosition() {
    {
        Harness harness;
        QString errorText;
        QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
        QQuickItem *format = harness.item(QStringLiteral("statusBarFormat"));
        QQuickItem *status = harness.item(QStringLiteral("statusBar"));
        QVERIFY(format && status);
        QCOMPARE(format->property("text").toString(),
                 QStringLiteral("FLAC · 16-bit / 44.1 kHz · Stereo · 557 kbps"));
        // The position is the transport's alone.
        const QList<QQuickItem *> labels = status->findChildren<QQuickItem *>();
        for (QQuickItem *item : labels) {
            const QString text = item->property("text").toString();
            QVERIFY2(!text.contains(QStringLiteral("1:17")) && !text.contains(QStringLiteral("3:49")),
                     qPrintable(text));
        }
        harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
        QCOMPARE(format->property("text").toString(), QString());
    }
    // Narrow: the format only.
    Harness narrow;
    QString errorText;
    QVERIFY2(narrow.load(kBreezeDark, 1000, &errorText), qPrintable(errorText));
    QCOMPARE(narrow.item(QStringLiteral("statusBarFormat"))->property("text").toString(), QStringLiteral("FLAC"));
}

void TransportStatusTest::statusBarShowsTheScanWithItsBar() {
    for (const int width : {1600, 1000}) {
        Harness harness;
        QString errorText;
        QVERIFY2(harness.load(kBreezeDark, width, &errorText), qPrintable(errorText));
        QQuickItem *scan = harness.item(QStringLiteral("statusBarScan"));
        QQuickItem *label = harness.item(QStringLiteral("statusBarScanLabel"));
        QQuickItem *count = harness.item(QStringLiteral("statusBarScanCount"));
        QQuickItem *bar = harness.item(QStringLiteral("statusBarScanBar"));
        QQuickItem *fill = harness.item(QStringLiteral("statusBarScanBarFill"));
        QVERIFY(scan && label && count && bar && fill);
        QVERIFY(!scan->isVisible());
        harness.bridge->setProperty("libraryRootEntries",
                                    QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Music")},
                                                             {QStringLiteral("path"), QStringLiteral("/fixture/music")}}});
        harness.bridge->setProperty("libraryScanProcessed", 412);
        harness.bridge->setProperty("libraryScanDiscovered", 413);
        harness.bridge->setProperty("libraryScanTotal", 1204);
        harness.bridge->setProperty("libraryScanInProgress", true);
        QTRY_VERIFY(scan->isVisible());
        // The files processed of those to scan, not of those found so far.
        QCOMPARE(count->property("text").toString(), QStringLiteral("412 / 1,204"));
        if (width == 1000) {
            QCOMPARE(label->property("text").toString(), QStringLiteral("Scanning"));
            QVERIFY(!bar->isVisible());
            continue;
        }
        QCOMPARE(label->property("text").toString(), QStringLiteral("Scanning “Music”"));
        QVERIFY(bar->isVisible());
        QCOMPARE(bar->size(), QSizeF(72, 4));
        // The bar moves with the batches, by whole pixels.
        QCOMPARE(fill->width(), std::floor(72.0 * 412 / 1204));
        harness.bridge->setProperty("libraryScanProcessed", 602);
        QCOMPARE(fill->width(), 36.0);
        // Two folders: no name.
        harness.bridge->setProperty(
            "libraryRootEntries",
            QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Music")}},
                         QVariantMap{{QStringLiteral("name"), QStringLiteral("More")}}});
        QCOMPARE(label->property("text").toString(), QStringLiteral("Scanning"));
        harness.bridge->setProperty("libraryScanInProgress", false);
        QTRY_VERIFY(!scan->isVisible());
    }
}

// A rescan, whose files are processed as soon as they are found: the bar
// runs from empty to full against the files to scan, and is full only once
// they all are.
void TransportStatusTest::statusBarScanBarFillsOnlyOnceTheRescanEnds() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *count = harness.item(QStringLiteral("statusBarScanCount"));
    QQuickItem *fill = harness.item(QStringLiteral("statusBarScanBarFill"));
    QVERIFY(count && fill);
    harness.bridge->setProperty("libraryScanTotal", 1000);
    harness.bridge->setProperty("libraryScanProcessed", 0);
    harness.bridge->setProperty("libraryScanDiscovered", 0);
    harness.bridge->setProperty("libraryScanInProgress", true);
    QTRY_VERIFY(fill->isVisible());
    QCOMPARE(fill->width(), 0.0);
    QCOMPARE(count->property("text").toString(), QStringLiteral("0 / 1,000"));

    double previous = 0.0;
    for (int processed = 1; processed < 1000; ++processed) {
        harness.bridge->setProperty("libraryScanProcessed", processed);
        harness.bridge->setProperty("libraryScanDiscovered", processed);
        const double width = fill->width();
        QVERIFY2(width >= previous, qPrintable(QString::number(processed)));
        QVERIFY2(width < 72.0, qPrintable(QStringLiteral("full at %1 of 1000").arg(processed)));
        previous = width;
    }
    QCOMPARE(count->property("text").toString(), QStringLiteral("999 / 1,000"));
    QCOMPARE(fill->width(), 71.0);
    harness.bridge->setProperty("libraryScanProcessed", 1000);
    QCOMPARE(fill->width(), 72.0);
    QCOMPARE(count->property("text").toString(), QStringLiteral("1,000 / 1,000"));
}

// The first scan of a folder cannot tell its total: the files scanned so
// far, and no bar until the scan can tell, for there is nothing to fill it
// against. Nothing in the scan moves on its own (#373: no indeterminate
// animation), with the desktop's animations on or off: Plasma's "off"
// leaves Kirigami's longDuration at 1 ms, which a guard on it would not
// stop.
void TransportStatusTest::statusBarScanWithoutATotalIsStill() {
    for (const int longDuration : {200, 1}) {
        Harness harness;
        QString errorText;
        QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
        QQuickItem *scan = harness.item(QStringLiteral("statusBarScan"));
        QQuickItem *count = harness.item(QStringLiteral("statusBarScanCount"));
        QQuickItem *bar = harness.item(QStringLiteral("statusBarScanBar"));
        QQuickItem *fill = harness.item(QStringLiteral("statusBarScanBarFill"));
        QVERIFY(scan && count && bar && fill);
        Kirigami::Platform::Units *units = unitsOf(scan);
        QVERIFY(units != nullptr);
        units->setLongDuration(longDuration);
        units->setShortDuration(longDuration == 1 ? 0 : 100);
        harness.bridge->setProperty("libraryScanTotal", -1);
        harness.bridge->setProperty("libraryScanProcessed", 0);
        harness.bridge->setProperty("libraryScanDiscovered", 0);
        harness.bridge->setProperty("libraryScanInProgress", true);
        QTRY_VERIFY(scan->isVisible());
        QVERIFY(!bar->isVisible());
        QVERIFY(!count->isVisible());

        harness.bridge->setProperty("libraryScanProcessed", 1);
        harness.bridge->setProperty("libraryScanDiscovered", 1);
        QCOMPARE(count->property("text").toString(), QStringLiteral("1 file"));
        harness.bridge->setProperty("libraryScanProcessed", 1204);
        harness.bridge->setProperty("libraryScanDiscovered", 1500);
        QCOMPARE(count->property("text").toString(), QStringLiteral("1,204 files"));
        QVERIFY(!bar->isVisible());

        // No animation in the scan, and over some frames nothing in it
        // moves.
        QList<QQuickItem *> parts{scan};
        for (qsizetype index = 0; index < parts.size(); ++index) {
            parts.append(parts.at(index)->childItems());
        }
        for (QObject *object : scan->findChildren<QObject *>()) {
            QVERIFY2(!object->inherits("QQuickAbstractAnimation"), object->metaObject()->className());
        }
        const auto geometry = [&parts]() {
            QList<QRectF> rects;
            for (QQuickItem *part : parts) {
                rects.push_back(sceneRect(part));
            }
            return rects;
        };
        // Once the window has drawn the row laid out for the count.
        QVERIFY(QTest::qWaitForWindowExposed(harness.window));
        int frames = 0;
        const QMetaObject::Connection counting = QObject::connect(
            harness.window, &QQuickWindow::frameSwapped, harness.window, [&frames]() { ++frames; });
        harness.window->update();
        QTRY_VERIFY(frames > 0);
        const QList<QRectF> before = geometry();
        QTest::qWait(300);
        QObject::disconnect(counting);
        QCOMPARE(geometry(), before);

        // The walk has ended: the files it found are the total, and the bar
        // fills against it.
        harness.bridge->setProperty("libraryScanTotal", 1500);
        QVERIFY(bar->isVisible());
        QVERIFY(fill->isVisible());
        QCOMPARE(count->property("text").toString(), QStringLiteral("1,204 / 1,500"));
        QCOMPARE(fill->width(), std::floor(72.0 * 1204 / 1500));
    }
}

void TransportStatusTest::lastFmSlotShowsEachState() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *box = harness.item(QStringLiteral("scrobbleIndicatorBox"));
    QQuickItem *ring = harness.item(QStringLiteral("scrobbleIndicatorRing"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QQuickItem *label = harness.item(QStringLiteral("scrobbleIndicatorLabel"));
    QVERIFY(slot && box && ring && glyph && label);
    QObject *bridge = harness.bridge;
    QVERIFY(!slot->isVisible());
    const auto progress = [slot]() { return slot->property("ringProgress").toDouble(); };

    // Listening: the glyph in the secondary colour inside the ring, whose
    // arc in the text colour shows the listening done, and the time left
    // in the tooltip.
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    bridge->setProperty("lastFmScrobbleListenedMs", 77000);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(!label->isVisible());
    QCOMPARE(box->size(), QSizeF(20, 20));
    // A 16 px ring, a 2 px arc over a 1 px track along its inner edge, and
    // a 10 px glyph inside it, so that the arc carries the shape (#373 H3).
    QCOMPARE(ring->size(), QSizeF(16, 16));
    QCOMPARE(glyph->width(), 10.0);
    QCOMPARE(slot->property("ringStroke").toReal(), 2.0);
    QCOMPARE(slot->property("ringTrackStroke").toReal(), 1.0);
    QCOMPARE(slot->property("ringRadius").toReal(), 7.0);
    QCOMPARE(slot->property("ringTrackRadius").toReal(), 6.5);
    QVERIFY(ring->isVisible());
    QVERIFY(std::abs(progress() - 77.0 / 115.0) < 0.02);
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm-ring.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(sameColor(slot->property("ringColor").value<QColor>(), harness.token("text")));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 0:38 to scrobble"));
    QCOMPARE(accessibleText(slot, QAccessible::Name), QStringLiteral("Last.fm: 0:38 to scrobble"));
    QCOMPARE(accessibleText(slot, QAccessible::Description), QStringLiteral("Press to pause scrobbling."));
    // Until the track's length is known, there is no scrobble point.
    bridge->setProperty("lastFmScrobbleThresholdMs", 0);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: listening"));
    QCOMPARE(progress(), 0.0);
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);

    // Once playback has stopped after a play that ended before its
    // scrobble point: the slot says so, the ring is empty and the tooltip
    // says how far the play got.
    bridge->setProperty("lastFmScrobbleState", 6);
    bridge->setProperty("lastFmScrobbleIneligibility", 3);
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Not scrobbled"));
    QVERIFY(label->isVisible());
    QVERIFY(ring->isVisible());
    QCOMPARE(progress(), 0.0);
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textTertiary")));
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(toolTipText(slot),
             QStringLiteral("Last.fm: not scrobbled. The play ended after 1:17 of the 1:55 of listening it "
                            "needed."));
    // Other plays that can't be scrobbled, and skipped ones: empty.
    for (const int reason : {1, 2}) {
        bridge->setProperty("lastFmScrobbleIneligibility", reason);
        QTRY_VERIFY2(!slot->isVisible(), qPrintable(QString::number(reason)));
    }
    bridge->setProperty("lastFmScrobbleIneligibility", 0);
    for (const int state : {7, 0}) {
        bridge->setProperty("lastFmScrobbleState", state);
        QTRY_VERIFY2(!slot->isVisible(), qPrintable(QString::number(state)));
    }

    // Scrobbled, and while it is sent: the ring closed in the arc's colour,
    // with no fill and no colour (#373 H2); the glyph is secondary while it
    // is sent, and the text colour once accepted.
    // Between the top of the ring and the glyph.
    const auto insideRing = [box]() { return box->mapToScene(QPointF(box->width() / 2, 5.5)); };
    qreal scale = 1;
    for (const int state : {2, 3}) {
        bridge->setProperty("lastFmScrobbleState", state);
        QTRY_VERIFY2(slot->isVisible(), qPrintable(QString::number(state)));
        QVERIFY(ring->isVisible());
        QCOMPARE(progress(), 1.0);
        QVERIFY(sameColor(slot->property("ringColor").value<QColor>(), harness.token("text")));
        QVERIFY(sameColor(glyph->property("color").value<QColor>(),
                          harness.token(state == 3 ? "text" : "textSecondary")));
        QVERIFY(!label->isVisible());
        // Inside the ring, left of the glyph: the status bar, unfilled.
        QTRY_VERIFY2(sameColor(pixelAt(grabbed(harness.window, &scale), insideRing(), scale),
                               harness.token("chrome")),
                     qPrintable(QString::number(state)));
    }
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: scrobbled"));
    bridge->setProperty("lastFmScrobbleState", 2);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: sending the scrobble"));

    // Queued: the words and the offline glyph, without the ring.
    bridge->setProperty("lastFmScrobbleState", 5);
    bridge->setProperty("lastFmPendingScrobbleCount", 3);
    QTRY_VERIFY(slot->isVisible());
    QCOMPARE(label->property("text").toString(), QStringLiteral("3 scrobbles waiting"));
    QVERIFY(label->isVisible());
    QVERIFY(!ring->isVisible());
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("status-offline.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(toolTipText(slot).startsWith(QStringLiteral("Last.fm can't be reached.")));
    bridge->setProperty("lastFmPendingScrobbleCount", 1);
    QCOMPARE(label->property("text").toString(), QStringLiteral("1 scrobble waiting"));

    // Failed: the words, and the closed ring and the glyph in the negative
    // colour, the slot's only colour, at least 3:1 on the chrome.
    bridge->setProperty("lastFmPendingScrobbleCount", 0);
    bridge->setProperty("lastFmScrobbleState", 4);
    bridge->setProperty("lastFmScrobbleMessage", QStringLiteral("Invalid session key"));
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Scrobble failed"));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm-ring.svg")));
    QCOMPARE(glyph->width(), 10.0);
    QVERIFY(ring->isVisible());
    QCOMPARE(progress(), 1.0);
    const QColor negative = harness.token("negative");
    QVERIFY(sameColor(slot->property("ringColor").value<QColor>(), negative));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), negative));
    QVERIFY2(contrastRatio(negative, harness.token("chrome")) >= 3.0,
             qPrintable(describe(QStringLiteral("failed glyph"), negative, harness.token("chrome"))));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm didn't accept the scrobble: Invalid session key"));

    // Paused by the user: the glyph struck through, quieter, without the
    // ring; a click resumes.
    bridge->setProperty("lastFmScrobbleState", 1);
    bridge->setProperty("lastFmScrobblingPaused", true);
    QTRY_COMPARE(toolTipText(slot), QStringLiteral("Last.fm: scrobbling is paused"));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm-paused.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textTertiary")));
    QVERIFY(!ring->isVisible());
    QCOMPARE(box->size(), QSizeF(20, 20));
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(slot));
    QTRY_VERIFY(!bridge->property("lastFmScrobblingPaused").toBool());

    // Signed out or turned off: empty.
    bridge->setProperty("lastFmAuthState", 0);
    QTRY_VERIFY(!slot->isVisible());
}

// The time left comes from the scrobbler's listened time alone: a seek
// either way and a pause leave it, and only a new play, whose count starts
// from zero, starts it over. The ring and the tooltip move by whole seconds
// of listening, and not at all while the count holds.
void TransportStatusTest::lastFmSlotCountsTheTimeLeftFromTheListenedTime() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QVERIFY(slot != nullptr);
    QObject *bridge = harness.bridge;
    harness.root->setProperty("positionSeconds", 30.0);
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    bridge->setProperty("lastFmScrobbleListenedMs", 30000);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_COMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:25 to scrobble"));
    const double atThirty = slot->property("ringProgress").toDouble();
    QVERIFY(atThirty > 0.0);

    // Seeks forward and back, and a pause and a resume: the position and
    // the playback state move, the count doesn't.
    for (const double position : {90.0, 10.0}) {
        harness.root->setProperty("positionSeconds", position);
        QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:25 to scrobble"));
        QCOMPARE(slot->property("ringProgress").toDouble(), atThirty);
    }
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:25 to scrobble"));
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:25 to scrobble"));
    QCOMPARE(slot->property("ringProgress").toDouble(), atThirty);

    // Snapshots within a second of listening change nothing.
    QSignalSpy progressMoved(slot, SIGNAL(ringProgressChanged()));
    QSignalSpy textChanged(slot, SIGNAL(stateTextChanged()));
    for (int listened = 30040; listened < 31000; listened += 40) {
        bridge->setProperty("lastFmScrobbleListenedMs", listened);
    }
    QCOMPARE(progressMoved.count(), 0);
    QCOMPARE(textChanged.count(), 0);
    bridge->setProperty("lastFmScrobbleListenedMs", 31000);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:24 to scrobble"));
    // The ring moves by whole device pixels of its arc: with the second,
    // if it crosses one.
    const double arcPixels = std::round(2 * M_PI * slot->property("ringRadius").toDouble()
                                        * std::max(1.0, harness.window->devicePixelRatio()));
    const bool crossesAPixel = std::floor(31.0 / 115.0 * arcPixels) != std::floor(30.0 / 115.0 * arcPixels);
    QCOMPARE(progressMoved.count(), crossesAPixel ? 1 : 0);
    QCOMPARE(textChanged.count(), 1);

    // A new play (a new track, a replay, Repeat One) counts from zero.
    bridge->setProperty("lastFmScrobbleListenedMs", 0);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:55 to scrobble"));
    QCOMPARE(slot->property("ringProgress").toDouble(), 0.0);
    // One with another scrobble point.
    bridge->setProperty("lastFmScrobbleThresholdMs", 240000);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 4:00 to scrobble"));
}

void TransportStatusTest::lastFmSlotPausedLooksStruckThrough_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// Paused differs from listening in its shape, not only its colour: what is
// drawn, ink against the chrome, differs in many pixels.
void TransportStatusTest::lastFmSlotPausedLooksStruckThrough() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *box = harness.item(QStringLiteral("scrobbleIndicatorBox"));
    QVERIFY(slot && box);
    QObject *bridge = harness.bridge;
    bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    bridge->setProperty("lastFmScrobbleListenedMs", 36000);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    const QColor chrome = harness.token("chrome");

    // Which pixels of the glyph box are drawn.
    const auto ink = [&]() {
        const QImage image = harness.window->grabWindow();
        const qreal scale = harness.window->effectiveDevicePixelRatio();
        const QRectF rect = sceneRect(box);
        QList<bool> drawn;
        for (int y = 0; y < int(rect.height()); ++y) {
            for (int x = 0; x < int(rect.width()); ++x) {
                const QColor pixel = pixelAt(image, rect.topLeft() + QPointF(x + 0.5, y + 0.5), scale);
                drawn.push_back(channelDistance(pixel, chrome) > 24);
            }
        }
        return drawn;
    };
    const QList<bool> listening = ink();
    bridge->setProperty("lastFmScrobblingPaused", true);
    QTRY_COMPARE(slot->property("look").toString(), QStringLiteral("paused"));
    QTRY_VERIFY(ink() != listening);
    const QList<bool> paused = ink();
    int differing = 0;
    int pausedInk = 0;
    for (qsizetype i = 0; i < paused.size(); ++i) {
        differing += paused[i] != listening[i] ? 1 : 0;
        pausedInk += paused[i] ? 1 : 0;
    }
    QVERIFY2(differing >= 40, qPrintable(QString::number(differing)));
    // The stroke crosses the box's middle, from corner to corner.
    const int side = int(sceneRect(box).width());
    QVERIFY(pausedInk > 0);
    QVERIFY(paused[(side / 2) * side + side / 2]);
    QVERIFY(paused[5 * side + 5]);
    QVERIFY(paused[(side - 6) * side + side - 6]);
}

void TransportStatusTest::lastFmSlotRingTrackReadsAndScrobbledCloses_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The ring (#373 H2). Its track, the listening still to come, is opaque and
// at least 3:1 on the status bar (it was 1.91:1 dark and 1.55:1 light),
// and quieter than the arc of the listening done. Scrobbled is the ring
// closed, with no fill: it differs from a ring nearly full by its shape,
// a gap or none, which is drawn and measured where the gap is.
void TransportStatusTest::lastFmSlotRingTrackReadsAndScrobbledCloses() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *box = harness.item(QStringLiteral("scrobbleIndicatorBox"));
    QVERIFY(slot && box);
    QObject *bridge = harness.bridge;
    const QColor chrome = harness.token("chrome");

    // At 0 %: the track alone, from the declared colours.
    bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    bridge->setProperty("lastFmScrobbleListenedMs", 0);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    QCOMPARE(slot->property("ringProgress").toDouble(), 0.0);
    const QColor track = slot->property("ringTrackColor").value<QColor>();
    const QColor arc = slot->property("ringColor").value<QColor>();
    QCOMPARE(track.alphaF(), 1.0f);
    QVERIFY2(contrastRatio(track, chrome) >= 3.0, qPrintable(describe(QStringLiteral("ring track"), track, chrome)));
    QVERIFY2(contrastRatio(arc, chrome) > contrastRatio(track, chrome) + 1.0,
             qPrintable(describe(QStringLiteral("ring arc"), arc, chrome) + QStringLiteral(", ")
                        + describe(QStringLiteral("ring track"), track, chrome)));

    // Where a ring at 90 % has its gap: the arc runs clockwise from the top,
    // so the gap is the last tenth before it, centred 18° left of the top;
    // on the track, which lies under the arc's inner half.
    const qreal radius = slot->property("ringTrackRadius").toReal();
    const double angle = (270.0 - 18.0) * M_PI / 180.0;
    // Found again for each grab: the slot moves as its state changes.
    qreal scale = 1;
    // The strongest pixel across the ring there, as the 1 px track falls
    // between pixels on a diagonal.
    const auto drawnInGap = [&]() {
        const QImage image = grabbed(harness.window, &scale);
        QColor strongest = chrome;
        for (const qreal across : {-0.5, 0.0, 0.5}) {
            const QColor drawn = pixelAt(image,
                                         box->mapToScene(QPointF(box->width() / 2 + (radius + across) * std::cos(angle),
                                                                 box->height() / 2 + (radius + across) * std::sin(angle))),
                                         scale);
            if (channelDistance(drawn, chrome) > channelDistance(strongest, chrome)) {
                strongest = drawn;
            }
        }
        return strongest;
    };
    const auto drawnInside = [&]() {
        const QImage image = grabbed(harness.window, &scale);
        return pixelAt(image, box->mapToScene(QPointF(box->width() / 2, 5.5)), scale);
    };
    // Whether the drawn pixel is nearer to `a` than to `b` and to `c`.
    const auto nearest = [](const QColor &drawn, const QColor &a, const QColor &b, const QColor &c) {
        return channelDistance(drawn, a) < channelDistance(drawn, b)
            && channelDistance(drawn, a) < channelDistance(drawn, c);
    };

    // Nearly done: the gap shows the track, not the arc. The 1 px track
    // spreads over two pixels on the diagonal, so it is drawn there between
    // its colour and the chrome's.
    bridge->setProperty("lastFmScrobbleListenedMs", 108000);
    QTRY_VERIFY(std::abs(slot->property("ringProgress").toDouble() - 0.9) < 0.02);
    QTRY_VERIFY2(channelDistance(drawnInGap(), track) < channelDistance(drawnInGap(), arc)
                     && !sameColor(drawnInGap(), chrome),
                 qPrintable(drawnInGap().name()));

    // Scrobbled, sent and accepted: no gap, and nothing filled inside.
    for (const int state : {2, 3}) {
        bridge->setProperty("lastFmScrobbleState", state);
        QTRY_COMPARE(slot->property("look").toString(), QStringLiteral("scrobbled"));
        const QColor closed = slot->property("ringColor").value<QColor>();
        QVERIFY2(contrastRatio(closed, chrome) >= 3.0,
                 qPrintable(describe(QStringLiteral("closed ring"), closed, chrome)));
        QTRY_VERIFY2(nearest(drawnInGap(), closed, track, chrome),
                     qPrintable(QStringLiteral("state %1: %2 in the gap, the ring %3, the track %4")
                                    .arg(state)
                                    .arg(drawnInGap().name(), closed.name(), track.name())));
        QVERIFY(sameColor(drawnInside(), chrome));
    }
    // No colour for a routine success: the accepted ring is the text's.
    QVERIFY(sameColor(slot->property("ringColor").value<QColor>(), harness.token("text")));
}

void TransportStatusTest::lastFmSlotRingProgressReadsAtOneX_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The ring's progress reads at 1x (#373 H3): a quarter, a half and nearly
// done drew nearly alike with a 1.5 px arc over a track of the same width.
// The 2 px arc in the text colour is drawn where the listening is done and
// the track elsewhere, so the share of the ring the arc takes, sampled
// along it, follows the progress; and the 1 px track, as drawn, is still
// 3:1 on the status bar.
void TransportStatusTest::lastFmSlotRingProgressReadsAtOneX() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QCOMPARE(harness.window->effectiveDevicePixelRatio(), 1.0);
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *ring = harness.item(QStringLiteral("scrobbleIndicatorRing"));
    QVERIFY(slot && ring);
    QObject *bridge = harness.bridge;
    const QColor chrome = harness.token("chrome");
    bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    bridge->setProperty("lastFmScrobbleListenedMs", 0);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    const QColor arc = slot->property("ringColor").value<QColor>();
    const QColor track = slot->property("ringTrackColor").value<QColor>();
    QVERIFY(sameColor(arc, harness.token("text")));
    // On the track, which the arc covers too.
    const qreal radius = slot->property("ringTrackRadius").toReal();

    // The empty ring, as drawn: the strongest pixel of the track above the
    // glyph, as the contrast check samples it.
    qreal scale = 1;
    {
        QTRY_VERIFY(ring->isVisible());
        const QImage image = grabbed(harness.window, &scale);
        const int aboveGlyph = int(ring->height() - slot->property("glyphSize").toInt()) / 2;
        double best = 1;
        for (int y = 0; y < aboveGlyph; ++y) {
            for (int x = 0; x < int(ring->width()); ++x) {
                const QColor drawn = pixelAt(image, ring->mapToScene(QPointF(x + 0.5, y + 0.5)), scale);
                best = std::max(best, contrastRatio(drawn, chrome));
            }
        }
        QVERIFY2(best >= 3.0, qPrintable(QStringLiteral("the drawn track is %1:1 on the chrome").arg(best)));
    }

    // Which of `samples` points along the ring, clockwise from the top, are
    // drawn in the arc's colour rather than the track's.
    constexpr int samples = 72;
    const auto arcShare = [&]() {
        const QImage image = grabbed(harness.window, &scale);
        int lit = 0;
        for (int i = 0; i < samples; ++i) {
            const double angle = (-90.0 + 360.0 * (i + 0.5) / samples) * M_PI / 180.0;
            const QPointF point(ring->width() / 2 + radius * std::cos(angle),
                                ring->height() / 2 + radius * std::sin(angle));
            const QColor drawn = pixelAt(image, ring->mapToScene(point), scale);
            lit += channelDistance(drawn, arc) < channelDistance(drawn, track) ? 1 : 0;
        }
        return double(lit) / samples;
    };
    QList<double> shares;
    for (const double done : {0.0, 0.25, 0.5, 0.9}) {
        bridge->setProperty("lastFmScrobbleListenedMs", int(done * 120000));
        QTRY_VERIFY(std::abs(slot->property("ringProgress").toDouble() - done) < 0.02);
        QTRY_VERIFY2(std::abs(arcShare() - done) <= 0.06,
                     qPrintable(QStringLiteral("%1 done drew %2 of the ring").arg(done).arg(arcShare())));
        shares.push_back(arcShare());
    }
    // Each step reads apart from the last.
    for (qsizetype i = 1; i < shares.size(); ++i) {
        QVERIFY2(shares[i] - shares[i - 1] >= 0.2, qPrintable(QString::number(shares[i] - shares[i - 1])));
    }
}

void TransportStatusTest::lastFmSlotToolTipShowsOnKeyboardFocus() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *background = harness.item(QStringLiteral("scrobbleIndicatorBackground"));
    QVERIFY(slot && background);
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(slot->activeFocusOnTab());
    QVERIFY(!toolTipShown(slot));
    slot->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(toolTipShown(slot));
    QCOMPARE(evaluateOn(background, QStringLiteral("border.width")).toInt(), 2);
    QVERIFY(sameColor(evaluateOn(background, QStringLiteral("border.color")).value<QColor>(),
                      harness.token("focusRing")));
    // Space pauses scrobbling, not playback.
    QTest::keyClick(harness.window, Qt::Key_Space);
    QTRY_VERIFY(harness.bridge->property("lastFmScrobblingPaused").toBool());
}

void TransportStatusTest::issuesButtonShowsNewSeenAndOpen_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

void TransportStatusTest::issuesButtonShowsNewSeenAndOpen() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *button = harness.item(QStringLiteral("statusBarErrorsButton"));
    QQuickItem *glyph = harness.item(QStringLiteral("statusBarErrorsGlyph"));
    QQuickItem *label = harness.item(QStringLiteral("statusBarErrorsLabel"));
    QQuickItem *background = harness.item(QStringLiteral("statusBarErrorsBackground"));
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QVERIFY(button && glyph && label && background && status);
    const QColor chrome = harness.token("chrome");
    QVERIFY(!button->isVisible());

    // One old issue, seen; then two new ones.
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral("old"))));
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "markSeen"));
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral("first"))));
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "report", Q_ARG(QVariant, 2), Q_ARG(QVariant, 1),
                                      Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral("second"))));
    takeWarnings(); // The controller logs each report as a warning.
    QTRY_VERIFY(button->isVisible());
    QCOMPARE(label->property("text").toString(), QStringLiteral("2 new issues"));
    QCOMPARE(glyph->width(), 14.0);
    // An outlined triangle: Breeze's filled one loses its "!" as a mask.
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("status-warning.svg")));
    QCOMPARE(button->height(), 22.0);
    const QColor negative = harness.token("negative");
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), negative));
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("text")));
    QVERIFY2(contrastRatio(negative, chrome) >= 3.0, qPrintable(describe(QStringLiteral("new glyph"), negative, chrome)));
    // 6 px from the status bar's end.
    QTRY_COMPARE(sceneRect(status).right() - sceneRect(button).right(), 6.0);

    // Keyboard focus shows the tooltip, as hover does.
    button->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(toolTipShown(button));
    QCOMPARE(toolTipText(button), QStringLiteral("Show Recent Issues"));
    QCOMPARE(evaluateOn(background, QStringLiteral("border.width")).toInt(), 2);

    // Open: the same on a text-10% tint, on which the text keeps 4.5:1.
    QSignalSpy requested(harness.errors, SIGNAL(historyRequested()));
    QTest::keyClick(harness.window, Qt::Key_Space);
    QTRY_COMPARE(requested.count(), 1);
    harness.root->property("statusBar").value<QObject *>()->setProperty("issuesOpen", true);
    QTRY_VERIFY(sameColor(background->property("color").value<QColor>(), withAlpha(harness.token("text"), 0.10)));
    QCOMPARE(label->property("text").toString(), QStringLiteral("2 new issues"));
    const QColor onTint = drawnOver(withAlpha(harness.token("text"), 0.10), chrome);
    QVERIFY2(contrastRatio(harness.token("text"), onTint) >= 4.5,
             qPrintable(describe(QStringLiteral("issues on the tint"), harness.token("text"), onTint)));
    QVERIFY2(contrastRatio(negative, onTint) >= 3.0,
             qPrintable(describe(QStringLiteral("glyph on the tint"), negative, onTint)));

    // Seen: "3 issues" in the secondary colour.
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "markSeen"));
    harness.root->property("statusBar").value<QObject *>()->setProperty("issuesOpen", false);
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("3 issues"));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY2(contrastRatio(harness.token("textSecondary"), chrome) >= 4.5,
             qPrintable(describe(QStringLiteral("seen issues"), harness.token("textSecondary"), chrome)));

    // A repeat is new again.
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral("old"))));
    takeWarnings();
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("1 new issue"));
}

// Groups are set apart by space alone: no "|" labels, no 1 px rules other
// than the seam along the top.
void TransportStatusTest::statusBarHasNoDividers() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    harness.bridge->setProperty("repeatMode", 1);
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral("issue"))));
    takeWarnings();
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QVERIFY(status != nullptr);
    QTRY_VERIFY(harness.item(QStringLiteral("statusBarErrorsButton"))->isVisible());
    const QList<QQuickItem *> items = status->findChildren<QQuickItem *>();
    for (QQuickItem *item : items) {
        if (!item->isVisible()) {
            continue;
        }
        const QString text = item->property("text").toString();
        QVERIFY2(text.trimmed() != QStringLiteral("|"), qPrintable(item->objectName()));
        if (item->objectName() == QStringLiteral("statusBarSeam")) {
            continue;
        }
        const bool rule = item->inherits("QQuickRectangle") && item->width() > 0 && item->height() > 0
            && (item->width() <= 1.0 || item->height() <= 1.0);
        QVERIFY2(!rule, qPrintable(item->objectName() + QStringLiteral(" %1×%2").arg(item->width()).arg(item->height())));
    }
}

namespace {

// Whether `inner` lies within `outer`, give or take half a pixel.
bool within(const QRectF &inner, const QRectF &outer) {
    return inner.left() >= outer.left() - 0.5 && inner.right() <= outer.right() + 0.5
        && inner.top() >= outer.top() - 0.5 && inner.bottom() <= outer.bottom() + 0.5;
}

QString describeRect(const QRectF &rect) {
    return QStringLiteral("%1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
}

} // namespace

// At 24 pt, nothing in the transport or the status bar clips or overlaps:
// the glyphs and buttons grow with the font, the band with them, and the
// status bar is as tall as its text and 4 px above and below it.
void TransportStatusTest::transportAndStatusBarGrowWithALargeFont() {
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont large = applicationFont;
    large.setPointSizeF(24);
    QGuiApplication::setFont(large);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QCOMPARE(harness.tokens->property("basePointSize").toReal(), 24.0);

    // The transport: the glyphs 2.4 times the default font's, each inside
    // its button, the buttons side by side inside the band.
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QVERIFY(transport != nullptr);
    const QRectF band = sceneRect(transport);
    const QStringList names{QStringLiteral("previous"), QStringLiteral("playPause"), QStringLiteral("stop"),
                            QStringLiteral("next")};
    const QList<int> glyphs{38, 48, 34, 38};
    qreal previousRight = band.left();
    qreal buttonSize = 0;
    for (int i = 0; i < names.size(); ++i) {
        QQuickItem *button = harness.item(QStringLiteral("transportButton_") + names[i]);
        QVERIFY2(button != nullptr, qPrintable(names[i]));
        QCOMPARE(button->property("glyphSize").toInt(), glyphs[i]);
        const QRectF rect = sceneRect(button);
        buttonSize = rect.height();
        QVERIFY2(rect.width() >= glyphs[i] && rect.height() >= glyphs[i], qPrintable(names[i]));
        QVERIFY2(within(rect, band), qPrintable(names[i] + QLatin1Char(' ') + describeRect(rect)));
        QVERIFY2(rect.left() >= previousRight, qPrintable(names[i]));
        previousRight = rect.right();
    }
    QCOMPARE(band.height(), std::max(48.0, buttonSize + 16.0));
    for (const QString &name : {QStringLiteral("seekSlider"), QStringLiteral("transportTimePosition"),
                                QStringLiteral("transportTimeLength"), QStringLiteral("muteButton"),
                                QStringLiteral("volumeSlider")}) {
        QQuickItem *item = harness.item(name);
        QVERIFY2(item != nullptr, qPrintable(name));
        QVERIFY2(within(sceneRect(item), band), qPrintable(name + QLatin1Char(' ') + describeRect(sceneRect(item))));
    }
    QVERIFY(sceneRect(harness.item(QStringLiteral("seekSlider"))).left() >= previousRight);
    QVERIFY(sceneRect(harness.item(QStringLiteral("muteButton"))).left()
            >= sceneRect(harness.item(QStringLiteral("transportTime"))).right());

    // The status bar: its text and glyphs within the bar under the seam,
    // and centred in it.
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    harness.bridge->setProperty("lastFmScrobbleState", 6);
    harness.bridge->setProperty("lastFmScrobbleIneligibility", 3);
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral("first"))));
    takeWarnings(); // The controller logs each report as a warning.
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QQuickItem *label = harness.item(QStringLiteral("statusBarPlaybackStateLabel"));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *issues = harness.item(QStringLiteral("statusBarErrorsButton"));
    QVERIFY(status && label && slot && issues);
    QTRY_VERIFY(slot->isVisible() && issues->isVisible());
    // Once the bar has laid out the slot and the issues it shows again.
    QTRY_VERIFY(std::abs(sceneRect(slot).center().y() - sceneRect(status).adjusted(0, 1, 0, 0).center().y()) <= 1.0
                && sceneRect(slot).right() < sceneRect(issues).left());
    const QRectF statusRect = sceneRect(status);
    const QRectF bar = statusRect.adjusted(0, 1, 0, 0);
    QVERIFY2(bar.height() >= label->height() + 7,
             qPrintable(QStringLiteral("%1 for a %2 px line").arg(bar.height()).arg(label->height())));
    for (const QString &name : {QStringLiteral("statusBarPlaybackGlyph"), QStringLiteral("statusBarPlaybackStateLabel"),
                                QStringLiteral("scrobbleIndicator"), QStringLiteral("scrobbleIndicatorLabel"),
                                QStringLiteral("scrobbleIndicatorBox"), QStringLiteral("statusBarErrorsButton"),
                                QStringLiteral("statusBarErrorsLabel")}) {
        QQuickItem *item = harness.item(name);
        QVERIFY2(item != nullptr && item->isVisible(), qPrintable(name));
        const QRectF rect = sceneRect(item);
        QVERIFY2(within(rect, bar), qPrintable(name + QLatin1Char(' ') + describeRect(rect)));
        QVERIFY2(std::abs(rect.center().y() - bar.center().y()) <= 1.0, qPrintable(name + QLatin1Char(' ') + describeRect(rect) + QLatin1Char(' ') + describeRect(bar)));
    }
    // The glyphs grow with the font as the transport's do: the state's
    // 11 px and the issues' 14 px, 2.4 times.
    QCOMPARE(harness.item(QStringLiteral("statusBarPlaybackGlyph"))->size(), QSizeF(26, 26));
    QCOMPARE(harness.item(QStringLiteral("statusBarErrorsGlyph"))->size(), QSizeF(34, 34));
    // The format group's words, each whole and within the bar.
    QQuickItem *format = harness.item(QStringLiteral("statusBarFormat"));
    QVERIFY(format != nullptr);
    QCOMPARE(format->property("text").toString(), QStringLiteral("FLAC · 16-bit / 44.1 kHz · Stereo · 557 kbps"));
    QVERIFY(within(sceneRect(format), bar));
    // The Last.fm label's words inside the slot's background.
    QVERIFY(within(sceneRect(harness.item(QStringLiteral("scrobbleIndicatorLabel"))), sceneRect(slot)));
    QVERIFY(within(sceneRect(harness.item(QStringLiteral("statusBarErrorsLabel"))), sceneRect(issues)));
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // Accessible interfaces exist while assistive technology is active.
    qputenv("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1");
    // The app's controls style and Breeze icons, whatever the desktop the
    // tests run in; TestSessionIsolation.h.
    isolateFromDesktopSessionWithAppLook();
    // Settings, data and caches of the test's own: no developer's icon
    // theme, colours or caches reach the tests.
    QTemporaryDir fixtures;
    if (!fixtures.isValid()) {
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("config"))));
    qputenv("XDG_CONFIG_DIRS", QFile::encodeName(fixtures.filePath(QStringLiteral("config-dirs"))));
    qputenv("XDG_DATA_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("data"))));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(fixtures.filePath(QStringLiteral("cache"))));
    // English digit grouping for the counts: "1,204".
    qputenv("LANGUAGE", "en_US");
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    QApplication app(argc, argv);
    QAccessible::setActive(true);
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    TransportStatusTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_transport_status.moc"
