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
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
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
    void transportToolTipsNameTheShortcut();
    void seekWaveformMeetsItsContrastAsDrawn_data();
    void seekWaveformMeetsItsContrastAsDrawn();
    void seekHoverDrawsALineAndANativeToolTip();
    void seekFocusDrawsOneRingAroundTheBar_data();
    void seekFocusDrawsOneRingAroundTheBar();
    void seekWithoutATrackIsALineAndNoTime();
    void timeReadoutShowsPositionAndLength();
    void volumeDrawsGrooveHoverAndFocus_data();
    void volumeDrawsGrooveHoverAndFocus();
    void statusBarShowsTheNeutralPlaybackState();
    void statusBarShowsRepeatAndShuffleInWords();
    void statusBarShowsTheFormatGroupWithoutThePosition();
    void statusBarShowsTheScanWithItsBar();
    void statusBarScanBarFillsOnlyOnceTheRescanEnds();
    void statusBarScanWithoutATotalIsBusy();
    void lastFmSlotShowsEachState();
    void lastFmSlotCountsTheTimeLeftFromTheListenedTime();
    void lastFmSlotPausedLooksStruckThrough_data();
    void lastFmSlotPausedLooksStruckThrough();
    void lastFmSlotToolTipShowsOnKeyboardFocus();
    void issuesButtonShowsNewSeenAndOpen_data();
    void issuesButtonShowsNewSeenAndOpen();
    void statusBarHasNoDividers();
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
// from 4 px; the seek bar 16 px after Next; the time 72 px, 16 px after it;
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
        QCOMPARE(rect.left(), 4.0 + i * 36.0);
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
    QCOMPARE(seekRect.left(), 4.0 + 4 * 32.0 + 3 * 4.0 + 16.0);
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
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QCOMPARE(label->property("text").toString(), QStringLiteral("Paused"));
    QCOMPARE(glyph->property("source").toString(), QStringLiteral("media-playback-pause"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QCOMPARE(label->property("text").toString(), QStringLiteral("Stopped"));
    QVERIFY(!glyph->isVisible());
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
    QQuickItem *busy = harness.item(QStringLiteral("statusBarScanBarBusy"));
    QVERIFY(count && fill && busy);
    harness.bridge->setProperty("libraryScanTotal", 1000);
    harness.bridge->setProperty("libraryScanProcessed", 0);
    harness.bridge->setProperty("libraryScanDiscovered", 0);
    harness.bridge->setProperty("libraryScanInProgress", true);
    QTRY_VERIFY(fill->isVisible());
    QVERIFY(!busy->isVisible());
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

// The first scan of a folder cannot tell its total: a busy bar and the
// files scanned so far, until the scan can tell.
void TransportStatusTest::statusBarScanWithoutATotalIsBusy() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *scan = harness.item(QStringLiteral("statusBarScan"));
    QQuickItem *count = harness.item(QStringLiteral("statusBarScanCount"));
    QQuickItem *bar = harness.item(QStringLiteral("statusBarScanBar"));
    QQuickItem *fill = harness.item(QStringLiteral("statusBarScanBarFill"));
    QQuickItem *busy = harness.item(QStringLiteral("statusBarScanBarBusy"));
    QVERIFY(scan && count && bar && fill && busy);
    harness.bridge->setProperty("libraryScanTotal", -1);
    harness.bridge->setProperty("libraryScanProcessed", 0);
    harness.bridge->setProperty("libraryScanDiscovered", 0);
    harness.bridge->setProperty("libraryScanInProgress", true);
    QTRY_VERIFY(scan->isVisible());
    QVERIFY(bar->isVisible());
    QVERIFY(busy->isVisible());
    QVERIFY(!fill->isVisible());
    QCOMPARE(busy->width(), 24.0);
    QVERIFY(!count->isVisible());

    harness.bridge->setProperty("libraryScanProcessed", 1);
    harness.bridge->setProperty("libraryScanDiscovered", 1);
    QCOMPARE(count->property("text").toString(), QStringLiteral("1 file"));
    harness.bridge->setProperty("libraryScanProcessed", 1204);
    harness.bridge->setProperty("libraryScanDiscovered", 1500);
    QCOMPARE(count->property("text").toString(), QStringLiteral("1,204 files"));
    QVERIFY(busy->isVisible());
    QVERIFY(!fill->isVisible());

    // The walk has ended: the files it found are the total.
    harness.bridge->setProperty("libraryScanTotal", 1500);
    QVERIFY(!busy->isVisible());
    QVERIFY(fill->isVisible());
    QCOMPARE(count->property("text").toString(), QStringLiteral("1,204 / 1,500"));
    QCOMPARE(fill->width(), std::floor(72.0 * 1204 / 1500));
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

    // Listening: the glyph in the secondary colour inside the ring, which
    // shows the listening done, and the time left in the tooltip.
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    bridge->setProperty("lastFmScrobbleListenedMs", 77000);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(!label->isVisible());
    QCOMPARE(box->size(), QSizeF(20, 20));
    QCOMPARE(glyph->width(), 12.0);
    QVERIFY(ring->isVisible());
    QVERIFY(std::abs(progress() - 77.0 / 115.0) < 0.02);
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(sameColor(slot->property("ringColor").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 0:38 to scrobble"));
    QCOMPARE(accessibleText(slot, QAccessible::Name), QStringLiteral("Last.fm: 0:38 to scrobble"));
    QCOMPARE(accessibleText(slot, QAccessible::Description), QStringLiteral("Press to pause scrobbling."));
    // Until the track's length is known, there is no scrobble point.
    bridge->setProperty("lastFmScrobbleThresholdMs", 0);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: listening"));
    QCOMPARE(progress(), 0.0);
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);

    // A seek forward left too little of the track: the slot says so, the
    // ring is empty and the tooltip says why.
    bridge->setProperty("lastFmScrobbleState", 6);
    bridge->setProperty("lastFmScrobbleIneligibility", 3);
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Won't scrobble"));
    QVERIFY(label->isVisible());
    QVERIFY(ring->isVisible());
    QCOMPARE(progress(), 0.0);
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textTertiary")));
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(toolTipText(slot),
             QStringLiteral("Last.fm: this play won't scrobble. The rest of the track is shorter than the "
                            "0:38 of listening it still needs."));
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

    // Scrobbled, and while it is sent: the ring closed, in the link colour.
    for (const int state : {2, 3}) {
        bridge->setProperty("lastFmScrobbleState", state);
        QTRY_VERIFY2(slot->isVisible(), qPrintable(QString::number(state)));
        QVERIFY(ring->isVisible());
        QCOMPARE(progress(), 1.0);
        QVERIFY(sameColor(slot->property("ringColor").value<QColor>(), harness.token("link")));
        QVERIFY(!label->isVisible());
    }
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: scrobbled"));
    bridge->setProperty("lastFmScrobbleState", 2);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: sending the scrobble"));
    QVERIFY2(contrastRatio(harness.token("link"), harness.token("chrome")) >= 3.0,
             qPrintable(describe(QStringLiteral("scrobbled ring"), harness.token("link"), harness.token("chrome"))));

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

    // Failed: the words and the glyph in the negative colour, at least
    // 3:1 on the chrome.
    bridge->setProperty("lastFmPendingScrobbleCount", 0);
    bridge->setProperty("lastFmScrobbleState", 4);
    bridge->setProperty("lastFmScrobbleMessage", QStringLiteral("Invalid session key"));
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Scrobble failed"));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm.svg")));
    QCOMPARE(glyph->width(), 16.0);
    const QColor negative = harness.token("negative");
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
    QCOMPARE(progressMoved.count(), 1);
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
