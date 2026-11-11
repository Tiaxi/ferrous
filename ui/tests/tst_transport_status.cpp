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
#include <QElapsedTimer>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QHash>
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
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <Kirigami/Platform/Units>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

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
    // A mute as the playback controller's: the volume put aside, and 0.
    property bool muted: false
    property real mutedVolume: 1.0

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
    // The transport reads only its shortcut's text, for the Play/Pause
    // tooltip; disabled, so that Space stays with the focused control.
    Action { id: playPauseAction; text: "&Pause"; shortcut: "Space"; enabled: false }
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
                playPauseAction: playPauseAction
                volumeMuted: window.muted
                mutedVolume: window.mutedVolume
                keyboardSeekStepSeconds: 5
                displayedPositionSeconds: window.positionSeconds
                positionText: window.positionText
                toggleMutedVolume: function() {
                    if (window.muted) {
                        window.muted = false
                        bridge.volume = window.mutedVolume
                    } else {
                        window.mutedVolume = bridge.volume
                        window.muted = true
                        bridge.volume = 0
                    }
                }
                setAppVolume: function(value) {
                    if (value > 0) {
                        window.muted = false
                    }
                    bridge.volume = value
                }
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

// Whether `inner` lies within `outer`, give or take half a pixel.
bool within(const QRectF &inner, const QRectF &outer) {
    return inner.left() >= outer.left() - 0.5 && inner.right() <= outer.right() + 0.5
        && inner.top() >= outer.top() - 0.5 && inner.bottom() <= outer.bottom() + 0.5;
}

QString describeRect(const QRectF &rect) {
    return QStringLiteral("%1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
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
    void playPauseGlyphsCrossFadeWithoutABlink();
    void transportToolTipsNameTheShortcut();
    void seekWaveformMeetsItsContrastAsDrawn_data();
    void seekWaveformMeetsItsContrastAsDrawn();
    void seekHoverDrawsALineAndANativeToolTip();
    void seekFocusDrawsOneRingAroundTheBar_data();
    void seekFocusDrawsOneRingAroundTheBar();
    void volumeGlyphHasTheTransportsWeight_data();
    void volumeGlyphHasTheTransportsWeight();
    void transportGlyphsDrawOnOneScale_data();
    void transportGlyphsDrawOnOneScale();
    void transportButtonsDrawTheAppsFocusRing_data();
    void transportButtonsDrawTheAppsFocusRing();
    void transportPressedReadsApartFromHover_data();
    void transportPressedReadsApartFromHover();
    void seekToolTipFadesInAtTheShortDuration();
    void seekWithoutATrackIsALineAndNoTime();
    void seekPlayheadMovesAcrossAShortTrack_data();
    void seekPlayheadMovesAcrossAShortTrack();
    void seekPlayheadWaitsAtTheStartWithoutALength_data();
    void seekPlayheadWaitsAtTheStartWithoutALength();
    void seekPlayheadThroughAZeroLengthFileThatFails();
    void timeReadoutShowsPositionAndLength();
    void volumeDrawsGrooveHoverAndFocus_data();
    void volumeDrawsGrooveHoverAndFocus();
    void volumeMuteKeepsTheThumb_data();
    void volumeMuteKeepsTheThumb();
    void volumeToolTipFollowsTheDrag();
    void statusBarShowsTheNeutralPlaybackState();
    void statusBarShowsRepeatAndShuffleInWords();
    void statusBarShowsTheFormatGroupWithoutThePosition();
    void statusBarFormatShowsWhatFitsWhole();
    void statusBarShowsTheScanWithItsBar();
    void statusBarScanBarFillsOnlyOnceTheRescanEnds();
    void statusBarScanWithoutATotalIsStill();
    void lastFmSlotShowsEachState();
    void lastFmSlotCountsTheTimeLeftFromTheListenedTime();
    void lastFmSlotPausedLooksStruckThrough_data();
    void lastFmSlotPausedLooksStruckThrough();
    void lastFmSlotBarFillsWithoutATrough_data();
    void lastFmSlotBarFillsWithoutATrough();
    void lastFmSlotToolTipShowsOnKeyboardFocus();
    void lastFmSlotSendingAndScrobbledLookApart_data();
    void lastFmSlotSendingAndScrobbledLookApart();
    void lastFmSlotGlyphReadsAtOneX_data();
    void lastFmSlotGlyphReadsAtOneX();
    void lastFmBarReadsApartFromTheScanBar_data();
    void lastFmBarReadsApartFromTheScanBar();
    void statusBarNamesAStoppedEngineInPlainWords();
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
// from 12 px, the content's inset; the seek bar 16 px after Next; then on
// one rhythm (#373 H8), between what is drawn: the time 16 px after the
// seek bar, as wide as "0:00 / 0:00", the volume glyph 24 px after it, and
// the 96 px slider's groove 8 px after the glyph, 12 px from the end. The
// volume glyph is the size of Previous and Next.
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
    // Play/Pause a step above its neighbours, in the same 32 px button, and
    // the others one size (#373 H9: 22, 16 and 14 read as three weights).
    const QList<int> glyphs{16, 20, 16, 16};
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
    // Both Play/Pause glyphs are drawn at 20 px, centred in the button.
    const QRectF playPauseRect = sceneRect(harness.item(QStringLiteral("transportButton_playPause")));
    for (const QString &name : {QStringLiteral("transportPlayPauseGlyph_play"),
                                QStringLiteral("transportPlayPauseGlyph_pause")}) {
        QQuickItem *glyph = harness.item(name);
        QVERIFY2(glyph != nullptr, qPrintable(name));
        QCOMPARE(glyph->size(), QSizeF(20, 20));
        QCOMPARE(sceneRect(glyph).center(), playPauseRect.center());
        QVERIFY2(within(sceneRect(glyph), playPauseRect), qPrintable(name));
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
    // The time's text ends where its box does: the box is as wide as a time
    // in minutes in the label's font.
    QQuickItem *position = harness.item(QStringLiteral("transportTimePosition"));
    QQuickItem *length = harness.item(QStringLiteral("transportTimeLength"));
    QVERIFY(position && length);
    QFont timeFont = position->property("font").value<QFont>();
    const qreal minutesWidth = QFontMetricsF(timeFont).horizontalAdvance(QStringLiteral("0:00 / 0:00"));
    QCOMPARE(timeRect.width(), std::ceil(minutesWidth));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_VERIFY(length->isVisible());
    QVERIFY2(std::abs(sceneRect(length).right() - timeRect.right()) < 1.0,
             qPrintable(QStringLiteral("%1 %2").arg(sceneRect(length).right()).arg(timeRect.right())));
    const QRectF muteRect = sceneRect(mute);
    QCOMPARE(muteRect.size(), QSizeF(32, 32));
    QQuickItem *muteGlyph = harness.item(QStringLiteral("muteButtonGlyph"));
    QVERIFY(muteGlyph != nullptr);
    const QRectF muteGlyphRect = sceneRect(muteGlyph);
    QCOMPARE(muteGlyphRect.size(), QSizeF(glyphs.first(), glyphs.first()));
    QCOMPARE(muteGlyphRect.center(), muteRect.center());
    QCOMPARE(muteGlyphRect.left() - timeRect.right(), 24.0);
    const QRectF volumeRect = sceneRect(volume);
    QQuickItem *groove = harness.item(QStringLiteral("volumeGroove"));
    QVERIFY(groove != nullptr);
    QCOMPARE(sceneRect(groove).left() - muteGlyphRect.right(), 8.0);
    QCOMPARE(volumeRect.width(), 96.0);
    // Stopped, "–:––" leaves the box as wide, and the volume where it is.
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_VERIFY(!length->isVisible());
    QCOMPARE(sceneRect(time), timeRect);
    QCOMPARE(sceneRect(muteGlyph), muteGlyphRect);
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
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

// The Play/Pause glyphs cross-fade: the one shown goes out as the other
// comes in, over shortDuration, so the button never drops below 0.6 of its
// ink (#373 H4: a fade out, then in, left it below half for 50 ms). At rest
// one glyph is whole and the other gone; a change back mid-fade reverses
// from where it is; with animations off the swap is instant.
void TransportStatusTest::playPauseGlyphsCrossFadeWithoutABlink() {
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
    // Long enough for many frames of the fade.
    units->setShortDuration(600);
    QTRY_VERIFY(pause->opacity() == 1.0 && !play->isVisible());

    const auto drawn = [](QQuickItem *glyph) { return glyph->isVisible() ? glyph->opacity() : 0.0; };
    // The ink where the glyphs overlap, one drawn over the other; where
    // only one has ink, the sum is what the eye blends over the button.
    // Both stay at 0.6 or above.
    const auto combined = [&]() {
        const qreal a = drawn(play);
        const qreal b = drawn(pause);
        return std::min(a + b, 1.0 - (1.0 - a) * (1.0 - b));
    };
    QString failure;
    int frames = 0;
    int framesWithBoth = 0;
    const QMetaObject::Connection connection =
        QObject::connect(harness.window, &QQuickWindow::frameSwapped, harness.window, [&]() {
            ++frames;
            if (drawn(play) > 0.0 && drawn(pause) > 0.0) {
                ++framesWithBoth;
            }
            if (failure.isEmpty() && combined() < 0.6) {
                failure = QStringLiteral("combined ink %1 at frame %2: play %3, pause %4")
                              .arg(combined()).arg(frames).arg(drawn(play)).arg(drawn(pause));
            }
        });
    const auto check = [&](const char *where) {
        QVERIFY2(combined() >= 0.6, where);
    };

    // Playing to paused: Pause goes out as Play comes in, at once.
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QVERIFY(swap->property("running").toBool());
    check("at the start");
    QTRY_VERIFY(drawn(play) > 0.0 && drawn(pause) > 0.0);
    check("both drawn");
    QTRY_VERIFY(!swap->property("running").toBool());
    QCOMPARE(play->opacity(), 1.0);
    QCOMPARE(pause->opacity(), 0.0);
    QVERIFY(!pause->isVisible());
    QVERIFY2(framesWithBoth > 0, "the glyphs never cross-faded");

    // Playing again, and back to paused mid-fade: the fade reverses from
    // where it is and settles on Play alone.
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_VERIFY(play->opacity() < 0.9 && pause->opacity() > 0.1);
    const qreal turnedAt = pause->opacity();
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QVERIFY(swap->property("running").toBool());
    // No jump: Pause goes back from where it was.
    QVERIFY(std::abs(pause->opacity() - turnedAt) < 0.1);
    check("reversed");
    QTRY_VERIFY(!swap->property("running").toBool());
    QCOMPARE(play->opacity(), 1.0);
    QCOMPARE(pause->opacity(), 0.0);
    QVERIFY(!pause->isVisible());

    // To Pause and all the way.
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_VERIFY(!swap->property("running").toBool());
    QCOMPARE(pause->opacity(), 1.0);
    QCOMPARE(play->opacity(), 0.0);
    QVERIFY(!play->isVisible());
    QObject::disconnect(connection);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QVERIFY(frames > 0);

    // Animations off: at once.
    units->setShortDuration(0);
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QVERIFY(!swap->property("running").toBool());
    QCOMPARE(play->opacity(), 1.0);
    QVERIFY(!pause->isVisible());
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
    // Play and Pause have no shortcut of their own: Space toggles them
    // (#373 H9: Previous named its shortcut and Pause none).
    QCOMPARE(toolTipText(playPause), QStringLiteral("Pause · Space"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_COMPARE(toolTipText(playPause), QStringLiteral("Play · Space"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
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
    // Within the bar's height, in the transport's band, so it covers
    // nothing under it such as the queue's header (#373 H5); left of the
    // line, where the pointer is not, 6 px clear of it.
    auto *tipBox = tip->property("background").value<QQuickItem *>();
    QVERIFY(tipBox != nullptr);
    const QRectF band = sceneRect(harness.item(QStringLiteral("transportBar")));
    QTRY_VERIFY(tip->property("opened").toBool());
    QRectF tipRect = sceneRect(tipBox);
    QVERIFY2(std::abs(tipRect.center().y() - bar.center().y()) <= 1.0, qPrintable(describeRect(tipRect)));
    QVERIFY2(tipRect.top() >= band.top() && tipRect.bottom() <= band.bottom(), qPrintable(describeRect(tipRect)));
    QCOMPARE(tipRect.right(), at.x() - 6.0);
    QVERIFY(!tipRect.intersects(sceneRect(hoverLine)));

    // Just right of the playhead, where on the left it would cover it: on
    // the right, clear of the pointer's arrow. It covers neither the hover
    // line nor the playhead.
    QQuickItem *playhead = harness.item(QStringLiteral("seekPlayhead"));
    QVERIFY(playhead != nullptr);
    const QRectF head = sceneRect(playhead);
    const QPointF nearHead(std::round(head.right() + 20), bar.center().y());
    QTest::mouseMove(harness.window, nearHead.toPoint());
    QTRY_COMPARE(sceneRect(hoverLine).left(), std::floor(nearHead.x()));
    QTRY_VERIFY(!tip->property("placedLeft").toBool());
    tipRect = sceneRect(tipBox);
    QCOMPARE(tipRect.left(), nearHead.x() + evaluateOn(seek, QStringLiteral("Kirigami.Units.gridUnit")).toReal());
    QVERIFY(!tipRect.intersects(sceneRect(hoverLine)));
    QVERIFY(!tipRect.intersects(head));
    QVERIFY2(std::abs(tipRect.center().y() - bar.center().y()) <= 1.0, qPrintable(describeRect(tipRect)));

    // At the bar's start, where the left has no room: on the right.
    const QPointF atStart(bar.left() + 3, bar.center().y());
    QTest::mouseMove(harness.window, atStart.toPoint());
    QTRY_COMPARE(sceneRect(hoverLine).left(), atStart.x());
    QTRY_VERIFY(!tip->property("placedLeft").toBool());
    QVERIFY(sceneRect(tipBox).left() >= atStart.x() + 1);

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

void TransportStatusTest::volumeGlyphHasTheTransportsWeight_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The volume glyph is drawn at the weight of Next beside it (#373 H8:
// Breeze's speaker, with 1 px waves, read lighter than Play and Next), in
// both of its looks: at least 80 % of Next's ink, where Breeze's speaker
// had 63 %, and in Next's box.
void TransportStatusTest::volumeGlyphHasTheTransportsWeight() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *next = harness.item(QStringLiteral("transportButton_next"));
    QQuickItem *muteGlyph = harness.item(QStringLiteral("muteButtonGlyph"));
    QVERIFY(next && muteGlyph);
    QQuickItem *nextGlyph = nullptr;
    QList<QQuickItem *> pending{next};
    while (!pending.isEmpty() && nextGlyph == nullptr) {
        QQuickItem *item = pending.takeFirst();
        if (item->objectName() == QStringLiteral("transportButtonGlyph")) {
            nextGlyph = item;
        }
        pending.append(item->childItems());
    }
    QVERIFY(nextGlyph != nullptr);
    QCOMPARE(muteGlyph->size(), nextGlyph->size());
    const QColor chrome = harness.token("chrome");
    // The pixels drawn at least half as strongly as the glyph's strongest,
    // so that how a mask is coloured doesn't count: offscreen, without the
    // desktop's platform theme, Kirigami draws a mask file uncoloured, which
    // is dark on the dark chrome.
    const auto ink = [&](QQuickItem *glyph) {
        qreal scale = 1;
        const QImage image = grabbed(harness.window, &scale);
        QList<int> distances;
        for (int y = 0; y < int(glyph->height()); ++y) {
            for (int x = 0; x < int(glyph->width()); ++x) {
                distances.append(
                    channelDistance(pixelAt(image, glyph->mapToScene(QPointF(x + 0.5, y + 0.5)), scale), chrome));
            }
        }
        const int strongest = *std::max_element(distances.cbegin(), distances.cend());
        return strongest < 16 ? 0
                              : int(std::count_if(distances.cbegin(), distances.cend(),
                                                  [strongest](int distance) { return distance * 2 >= strongest; }));
    };
    int nextInk = 0;
    QTRY_VERIFY((nextInk = ink(nextGlyph)) > 0);
    int volumeInk = 0;
    QTRY_VERIFY((volumeInk = ink(muteGlyph)) > 0);
    QVERIFY2(volumeInk * 5 >= nextInk * 4, qPrintable(QStringLiteral("%1 / %2").arg(volumeInk).arg(nextInk)));
    // Muted.
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier,
                      sceneCentre(harness.item(QStringLiteral("muteButton"))));
    QTRY_VERIFY(muteGlyph->property("source").toString().endsWith(QStringLiteral("/volume-muted.svg")));
    QTRY_VERIFY((volumeInk = ink(muteGlyph)) > 0);
    QVERIFY2(volumeInk * 10 >= nextInk * 7, qPrintable(QStringLiteral("%1 / %2").arg(volumeInk).arg(nextInk)));
}

void TransportStatusTest::transportGlyphsDrawOnOneScale_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("small");
    QTest::addColumn<int>("large");
    QTest::newRow("10 pt") << 10.0 << 16 << 20;
    QTest::newRow("24 pt") << 24.0 << 38 << 48;
}

// The transport's glyphs on one scale (#373 H9: Play/Pause 22, Previous and
// Next 16 and Stop 14 read as three weights): Previous, Stop, Next and the
// volume 16 px, their ink 12 px tall, and Play/Pause a step above at 20 px,
// its ink 14 px tall; Stop's and Pause's ink at most 1.4 times Next's. They
// grow with the font on whole pixels, to at least 0.9 of its pixel size at
// 24 pt, and are drawn at that size (H9: Kirigami rounded a 24 pt font's
// 38 px glyphs down to 32 px theme icons, and would draw a 20 px one at 16).
void TransportStatusTest::transportGlyphsDrawOnOneScale() {
    QFETCH(qreal, pointSize);
    QFETCH(int, small);
    QFETCH(int, large);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont font = applicationFont;
    font.setPointSizeF(pointSize);
    QGuiApplication::setFont(font);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QCOMPARE(harness.tokens->property("basePointSize").toReal(), pointSize);
    const auto glyphOf = [&harness](const QString &button) -> QQuickItem * {
        QList<QQuickItem *> pending{harness.item(QStringLiteral("transportButton_") + button)};
        while (!pending.isEmpty()) {
            QQuickItem *item = pending.takeFirst();
            if (item != nullptr && item->objectName() == QStringLiteral("transportButtonGlyph")) {
                return item;
            }
            if (item != nullptr) {
                pending.append(item->childItems());
            }
        }
        return nullptr;
    };
    struct Glyph {
        const char *name;
        QQuickItem *item;
        int side;
        int inkHeight;
    };
    // Playing: Pause shows.
    QQuickItem *pause = harness.item(QStringLiteral("transportPlayPauseGlyph_pause"));
    QTRY_VERIFY(pause != nullptr && pause->opacity() == 1.0);
    const QList<Glyph> glyphs{
        {"previous", glyphOf(QStringLiteral("previous")), small, small * 12 / 16},
        {"pause", pause, large, large * 14 / 20},
        {"stop", glyphOf(QStringLiteral("stop")), small, small * 12 / 16},
        {"next", glyphOf(QStringLiteral("next")), small, small * 12 / 16},
        {"volume", harness.item(QStringLiteral("muteButtonGlyph")), small, small * 12 / 16},
    };
    const int fontPixels = QFontInfo(font).pixelSize();
    for (const Glyph &glyph : glyphs) {
        QVERIFY2(glyph.item != nullptr, glyph.name);
        QCOMPARE(glyph.item->size(), QSizeF(glyph.side, glyph.side));
        QCOMPARE(glyph.item->property("paintedWidth").toReal(), qreal(glyph.side));
        QVERIFY2(glyph.side >= 0.9 * fontPixels || pointSize < 24,
                 qPrintable(QStringLiteral("%1: %2 px for a %3 px font").arg(glyph.name).arg(glyph.side).arg(fontPixels)));
    }
    QQuickItem *play = harness.item(QStringLiteral("transportPlayPauseGlyph_play"));
    QVERIFY(play != nullptr);
    QCOMPARE(play->size(), QSizeF(large, large));

    // As drawn: the pixels at least half as strong as the glyph's strongest,
    // so that how a mask is coloured offscreen doesn't count; their count
    // and the rows they span, in scene pixels.
    const QColor chrome = harness.token("chrome");
    struct Ink {
        int pixels = 0;
        int height = 0;
    };
    const auto inkOf = [&](QQuickItem *item) {
        qreal scale = 1;
        const QImage image = grabbed(harness.window, &scale);
        QList<int> distances;
        for (int y = 0; y < int(item->height()); ++y) {
            for (int x = 0; x < int(item->width()); ++x) {
                distances.append(
                    channelDistance(pixelAt(image, item->mapToScene(QPointF(x + 0.5, y + 0.5)), scale), chrome));
            }
        }
        Ink ink;
        const int strongest = *std::max_element(distances.cbegin(), distances.cend());
        if (strongest < 16) {
            return ink;
        }
        int top = -1;
        int bottom = -1;
        for (int i = 0; i < distances.size(); ++i) {
            if (distances[i] * 2 >= strongest) {
                ink.pixels += 1;
                const int y = i / int(item->width());
                top = top < 0 ? y : top;
                bottom = y;
            }
        }
        ink.height = bottom - top + 1;
        return ink;
    };
    QHash<QString, Ink> inks;
    for (const Glyph &glyph : glyphs) {
        Ink ink;
        QTRY_VERIFY((ink = inkOf(glyph.item)).pixels > 0);
        inks.insert(QString::fromLatin1(glyph.name), ink);
        QVERIFY2(std::abs(ink.height - glyph.inkHeight) <= 1,
                 qPrintable(QStringLiteral("%1: %2 px of ink in a %3 px glyph, not %4")
                                .arg(glyph.name).arg(ink.height).arg(glyph.side).arg(glyph.inkHeight)));
    }
    const int next = inks.value(QStringLiteral("next")).pixels;
    for (const char *name : {"previous", "pause", "stop"}) {
        const int pixels = inks.value(QString::fromLatin1(name)).pixels;
        QVERIFY2(pixels * 10 >= next * 8 && pixels * 5 <= next * 7,
                 qPrintable(QStringLiteral("%1: %2 px of ink, Next %3").arg(name).arg(pixels).arg(next)));
    }
    // Play, as Pause's step above, and its ink in the same rows.
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_VERIFY(play->opacity() == 1.0 && !pause->isVisible());
    Ink playInk;
    QTRY_VERIFY((playInk = inkOf(play)).pixels > 0);
    // Give or take the triangle's thin tips, at most a pixel at each end.
    QVERIFY2(std::abs(playInk.height - inks.value(QStringLiteral("pause")).height) <= 2,
             qPrintable(QStringLiteral("Play %1 px, Pause %2 px").arg(playInk.height)
                            .arg(inks.value(QStringLiteral("pause")).height)));
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

void TransportStatusTest::transportPressedReadsApartFromHover_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// A pressed transport button reads apart from a hovered one (#373 H5: the
// press, text 10%, was the hover's tint in the dark and lighter than it in
// the light): the pressed tint, 17% / 25% text, a clear step past the
// hover's 10% / 14%, as drawn on the chrome. The status bar's buttons
// share it.
void TransportStatusTest::transportPressedReadsApartFromHover() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    const QColor chrome = harness.token("chrome");
    const QColor hover = harness.token("hover");
    const QColor pressed = harness.token("pressed");
    QCOMPARE(std::lround(pressed.alphaF() * 100), dark ? 17L : 25L);
    QCOMPARE(std::lround(hover.alphaF() * 100), dark ? 10L : 14L);
    QVERIFY(sameColor(QColor::fromRgbF(pressed.redF(), pressed.greenF(), pressed.blueF()), harness.token("text")));
    const QColor hoverDrawn = drawnOver(hover, chrome);
    const QColor pressedDrawn = drawnOver(pressed, chrome);
    QVERIFY2(contrastRatio(pressedDrawn, hoverDrawn) >= 1.2,
             qPrintable(describe(QStringLiteral("pressed on hover"), pressedDrawn, hoverDrawn)));

    QQuickItem *button = harness.item(QStringLiteral("transportButton_previous"));
    QVERIFY(button != nullptr);
    button->setProperty("hoverEnabled", true);
    auto *background = button->property("background").value<QQuickItem *>();
    QVERIFY(background != nullptr);
    auto *tint = background->findChild<QQuickItem *>(QStringLiteral("transportButtonHover"));
    QVERIFY(tint != nullptr);
    // From elsewhere: a move to where the pointer last was is no move.
    QTest::mouseMove(harness.window, QPoint(10, harness.window->height() - 10));
    QTest::mouseMove(harness.window, sceneCentre(button));
    QTRY_VERIFY(tint->isVisible() && tint->opacity() == 1.0);
    QVERIFY(sameColor(tint->property("color").value<QColor>(), hover));
    qreal scale = 1;
    const QPointF corner = sceneRect(button).topLeft() + QPointF(4.5, 4.5);
    QTRY_VERIFY(channelDistance(pixelAt(grabbed(harness.window, &scale), corner, scale), hoverDrawn) <= 2);
    QTest::mousePress(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(button));
    QTRY_VERIFY(button->property("down").toBool());
    QVERIFY(sameColor(tint->property("color").value<QColor>(), pressed));
    QTRY_VERIFY(channelDistance(pixelAt(grabbed(harness.window, &scale), corner, scale), pressedDrawn) <= 2);
    QTest::mouseRelease(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(button));

    // The status bar's issues button and Last.fm slot take the same tint.
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "report", Q_ARG(QVariant, 1), Q_ARG(QVariant, 0),
                                      Q_ARG(QVariant, QString()), Q_ARG(QVariant, QStringLiteral("first"))));
    takeWarnings(); // The controller logs each report as a warning.
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    for (const auto &[name, backgroundName] :
         {std::pair{QStringLiteral("statusBarErrorsButton"), QStringLiteral("statusBarErrorsBackground")},
          std::pair{QStringLiteral("scrobbleIndicator"), QStringLiteral("scrobbleIndicatorBackground")}}) {
        QQuickItem *statusButton = harness.item(name);
        QQuickItem *statusBackground = harness.item(backgroundName);
        QVERIFY2(statusButton && statusBackground, qPrintable(name));
        QTRY_VERIFY2(statusButton->isVisible(), qPrintable(name));
        // No press of the issues button, which would open them.
        statusButton->setProperty("down", true);
        QTRY_VERIFY2(sameColor(statusBackground->property("color").value<QColor>(), pressed), qPrintable(name));
        // An invalid value resets it, to follow the press again.
        statusButton->setProperty("down", QVariant());
    }
}

// The seek bar's tooltip comes in over the window's short duration, as its
// other hovers do, not the style's long one (#373 H5: 200 ms against 100 ms
// elsewhere); it goes as the style's tooltips do.
void TransportStatusTest::seekToolTipFadesInAtTheShortDuration() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(seek && tip);
    Kirigami::Platform::Units *units = unitsOf(seek);
    QVERIFY(units != nullptr);
    // Far apart, so which one the fade takes shows in its time.
    units->setShortDuration(100);
    units->setLongDuration(3000);
    const QRectF bar = sceneRect(seek);
    QElapsedTimer timer;
    QTest::mouseMove(harness.window, QPoint(static_cast<int>(bar.left() + bar.width() * 0.62),
                                            static_cast<int>(bar.center().y())));
    timer.start();
    QTRY_VERIFY(tip->property("visible").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(tip->property("opacity").toReal() == 1.0, 1500);
    QVERIFY2(timer.elapsed() < 1500, qPrintable(QString::number(timer.elapsed())));
    // The fade out is the style's: the long duration, here still under way
    // after a frame or two.
    QTest::mouseMove(harness.window, QPoint(static_cast<int>(bar.left() + bar.width() * 0.62),
                                            static_cast<int>(bar.bottom() + 60)));
    QTRY_VERIFY(tip->property("opacity").toReal() < 1.0);
    QTest::qWait(100);
    QVERIFY(tip->property("opacity").toReal() > 0.0);
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

// The volume slider in the seek bar's language (#373 H5): a 3 px groove,
// filled up to the thumb in the seek bar's played colour and the rest in
// its unplayed one, opaque, and a solid 12 px thumb in the played colour,
// centred on the groove. Hover brightens the thumb to the text colour and
// shows "Volume 80%"; keyboard focus draws a 2 px ring around the thumb.
void TransportStatusTest::volumeDrawsGrooveHoverAndFocus() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *groove = harness.item(QStringLiteral("volumeGroove"));
    QQuickItem *fill = harness.item(QStringLiteral("volumeGrooveFill"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QQuickItem *ring = harness.item(QStringLiteral("volumeFocusRing"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("volumeToolTip"));
    QVERIFY(transport && volume && groove && fill && thumb && ring && tip);
    const QColor chrome = harness.token("chrome");
    const QColor played = transport->property("seekPlayedColor").value<QColor>();
    const QColor unplayed = mixed(chrome, played, transport->property("seekUnplayedOpacity").toDouble());

    QCOMPARE(groove->height(), 3.0);
    QCOMPARE(groove->width(), volume->width());
    QVERIFY(sameColor(groove->property("color").value<QColor>(), unplayed));
    QVERIFY(sameColor(fill->property("color").value<QColor>(), played));
    QCOMPARE(fill->width(), thumb->x() + thumb->width() / 2);
    QCOMPARE(thumb->width(), 12.0);
    QCOMPARE(thumb->height(), 12.0);
    QCOMPARE(sceneRect(thumb).center().y(), sceneRect(groove).center().y());
    // The groove's top on a whole pixel.
    QCOMPARE(sceneRect(groove).top(), std::floor(sceneRect(groove).top()));
    // Solid: the thumb is filled, with no border of another colour.
    const QColor restFill = thumb->property("color").value<QColor>();
    QVERIFY(sameColor(restFill, played));
    QCOMPARE(evaluateOn(thumb, QStringLiteral("border.width")).toInt(), 0);
    // As drawn: the unfilled groove 3:1 on the chrome, as the unplayed
    // waveform is, and the thumb and the filled part more.
    QVERIFY2(contrastRatio(unplayed, chrome) >= 3.0, qPrintable(describe(QStringLiteral("unfilled"), unplayed, chrome)));
    QVERIFY2(contrastRatio(played, chrome) > contrastRatio(unplayed, chrome),
             qPrintable(describe(QStringLiteral("filled"), played, chrome)));
    qreal scale = 1;
    const QImage image = grabbed(harness.window, &scale);
    const QRectF thumbRect = sceneRect(thumb);
    QVERIFY(channelDistance(pixelAt(image, thumbRect.center(), scale), played) <= 2);
    QVERIFY(channelDistance(pixelAt(image, QPointF(sceneRect(groove).right() - 2, sceneRect(groove).center().y()), scale),
                            unplayed) <= 2);
    QVERIFY(channelDistance(pixelAt(image, QPointF(sceneRect(groove).left() + 2, sceneRect(groove).center().y()), scale),
                            played) <= 2);

    // Hover brightens the thumb and shows "Volume 80%".
    QTest::mouseMove(harness.window, sceneCentre(volume));
    QTRY_VERIFY(volume->property("hovered").toBool());
    QTRY_VERIFY(sameColor(thumb->property("color").value<QColor>(), harness.token("text")));
    QCOMPARE(tip->property("text").toString(), QStringLiteral("Volume 80%"));
    QTRY_VERIFY(tip->property("visible").toBool());
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
    QTRY_VERIFY(tip->property("visible").toBool());
    // A key's step is the slider's tenth.
    QTest::keyClick(harness.window, Qt::Key_Left);
    QTRY_COMPARE(tip->property("text").toString(), QStringLiteral("Volume 70%"));
    QCOMPARE(harness.bridge->property("volume").toDouble(), 0.7);
}

void TransportStatusTest::volumeMuteKeepsTheThumb_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// A mute leaves the thumb at the volume it put aside (#373 H5: it threw the
// thumb to 0), dims the filled groove to the unfilled colour and shows the
// muted glyph; the tooltip says so. Unmuting brings the fill back.
void TransportStatusTest::volumeMuteKeepsTheThumb() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *mute = harness.item(QStringLiteral("muteButton"));
    QQuickItem *muteGlyph = harness.item(QStringLiteral("muteButtonGlyph"));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *groove = harness.item(QStringLiteral("volumeGroove"));
    QQuickItem *fill = harness.item(QStringLiteral("volumeGrooveFill"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("volumeToolTip"));
    QVERIFY(transport && mute && muteGlyph && volume && groove && fill && thumb && tip);
    const QColor played = transport->property("seekPlayedColor").value<QColor>();
    QCOMPARE(volume->property("value").toDouble(), 0.8);
    const qreal thumbX = thumb->x();
    const qreal fillWidth = fill->width();
    QVERIFY(muteGlyph->property("source").toString().endsWith(QStringLiteral("/volume.svg")));

    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(mute));
    QTRY_VERIFY(harness.root->property("muted").toBool());
    QCOMPARE(harness.bridge->property("volume").toDouble(), 0.0);
    QVERIFY(muteGlyph->property("source").toString().endsWith(QStringLiteral("/volume-muted.svg")));
    // The thumb and the fill stay; the fill dims to the groove's colour.
    QCOMPARE(volume->property("value").toDouble(), 0.8);
    QCOMPARE(thumb->x(), thumbX);
    QCOMPARE(fill->width(), fillWidth);
    QVERIFY(sameColor(fill->property("color").value<QColor>(), groove->property("color").value<QColor>()));
    QVERIFY(sameColor(thumb->property("color").value<QColor>(), played));
    QCOMPARE(tip->property("text").toString(), QStringLiteral("Volume 80%, muted"));
    QCOMPARE(accessibleText(volume, QAccessible::Description), QStringLiteral("80%"));

    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(mute));
    QTRY_VERIFY(!harness.root->property("muted").toBool());
    QCOMPARE(harness.bridge->property("volume").toDouble(), 0.8);
    QCOMPARE(thumb->x(), thumbX);
    QVERIFY(sameColor(fill->property("color").value<QColor>(), played));
    QCOMPARE(tip->property("text").toString(), QStringLiteral("Volume 80%"));
}

// The volume's tooltip shows the value it is dragged to, even when the drag
// starts before the hover's delay has shown it (#373 H5: "Volume 100%"
// with the thumb at 40%: the shared tooltip kept the text it had when the
// delay began).
void TransportStatusTest::volumeToolTipFollowsTheDrag() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("volumeToolTip"));
    QVERIFY(volume && thumb && tip);
    Kirigami::Platform::Units *units = unitsOf(volume);
    QVERIFY(units != nullptr);
    // A delay long enough that the drag starts and ends within it.
    units->setToolTipDelay(400);
    harness.bridge->setProperty("volume", 1.0);
    QTRY_COMPARE(volume->property("value").toDouble(), 1.0);

    // Onto the thumb, which starts the hover's delay; then a drag to 40%.
    const QRectF rect = sceneRect(volume);
    const QPoint start = sceneCentre(thumb);
    QTest::mouseMove(harness.window, start);
    QTRY_VERIFY(volume->property("hovered").toBool());
    QTest::mousePress(harness.window, Qt::LeftButton, Qt::NoModifier, start);
    const qreal travel = rect.width() - thumb->width();
    const QPoint end(static_cast<int>(std::round(rect.left() + thumb->width() / 2 + 0.4 * travel)),
                     start.y());
    for (int step = 1; step <= 8; ++step) {
        QTest::mouseMove(harness.window, start + (end - start) * step / 8);
    }
    QTRY_VERIFY(std::abs(volume->property("value").toDouble() - 0.4) < 0.02);
    QTRY_VERIFY(tip->property("opened").toBool());
    const QString expected = QStringLiteral("Volume %1%").arg(std::lround(volume->property("value").toDouble() * 100));
    QCOMPARE(tip->property("text").toString(), expected);
    // As drawn: the tooltip's label.
    auto *label = tip->property("contentItem").value<QQuickItem *>();
    QVERIFY(label != nullptr);
    const QList<QObject *> texts = label->findChildren<QObject *>();
    bool drawn = label->property("text").toString() == expected;
    for (QObject *text : texts) {
        drawn = drawn || text->property("text").toString() == expected;
    }
    QVERIFY2(drawn, qPrintable(expected));
    QTest::mouseRelease(harness.window, Qt::LeftButton, Qt::NoModifier, end);
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
    QQuickItem *shuffle = harness.item(QStringLiteral("statusBarShuffle"));
    QQuickItem *shuffleGlyph = harness.item(QStringLiteral("statusBarShuffleGlyph"));
    QQuickItem *shuffleLabel = harness.item(QStringLiteral("statusBarShuffleLabel"));
    QQuickItem *repeatGlyph = nullptr;
    QQuickItem *state = harness.item(QStringLiteral("statusBarPlaybackState"));
    QVERIFY(group && repeat && shuffle && shuffleGlyph && shuffleLabel && state);
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
    // Shuffle as repeat is (#373 H6: a glyph alone, named only in its
    // tooltip): its glyph and the words of its menu item, "Shuffle", in
    // the secondary colour, 16 px after repeat.
    harness.bridge->setProperty("shuffleEnabled", true);
    QTRY_VERIFY(shuffle->isVisible() && shuffleLabel->isVisible() && shuffleGlyph->isVisible());
    QCOMPARE(shuffleLabel->property("text").toString(), QStringLiteral("Shuffle"));
    QCOMPARE(accessibleText(shuffleLabel, QAccessible::Name), QStringLiteral("Shuffle"));
    QCOMPARE(shuffleGlyph->property("source").toString(), QStringLiteral("media-playlist-shuffle"));
    QVERIFY(sameColor(shuffleLabel->property("color").value<QColor>(),
                      repeat->property("color").value<QColor>()));
    QVERIFY(sameColor(shuffleLabel->property("color").value<QColor>(), harness.token("textSecondary")));
    QTRY_COMPARE(sceneRect(shuffle).left(), sceneRect(repeat).right() + 16.0);
    // The glyph and its words as repeat's: the same size, 6 px apart.
    for (QQuickItem *child : repeat->parentItem()->childItems()) {
        if (child != repeat && child->property("source").isValid()) {
            repeatGlyph = child;
        }
    }
    QVERIFY(repeatGlyph != nullptr);
    QCOMPARE(shuffleGlyph->size(), repeatGlyph->size());
    QCOMPARE(sceneRect(shuffleLabel).left() - sceneRect(shuffleGlyph).right(),
             sceneRect(repeat).left() - sceneRect(repeatGlyph).right());
    QCOMPARE(sceneRect(shuffleLabel).left() - sceneRect(shuffleGlyph).right(), 6.0);
    harness.bridge->setProperty("repeatMode", 0);
    QTRY_VERIFY(!repeat->isVisible());
    QVERIFY(group->isVisible() && shuffle->isVisible());
    QTRY_COMPARE(sceneRect(group).left(), sceneRect(state).right() + 16.0);
    QTRY_COMPARE(sceneRect(shuffle).left(), sceneRect(group).left());
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
}

// The format group shows what fits whole, not what the window's width
// allows (#373 H6: at 1000 px it showed "FLAC" alone, with most of the bar
// empty). With the room, all of it; with less, it drops parts from the end,
// each only once it no longer fits, and never cuts one.
void TransportStatusTest::statusBarFormatShowsWhatFitsWhole() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1000, &errorText), qPrintable(errorText));
    QQuickItem *format = harness.item(QStringLiteral("statusBarFormat"));
    QQuickItem *row = harness.item(QStringLiteral("statusBarFormatRow"));
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QQuickItem *stateLabel = harness.item(QStringLiteral("statusBarPlaybackStateLabel"));
    QVERIFY(format && row && status && stateLabel);
    const QStringList parts{QStringLiteral("FLAC"), QStringLiteral("16-bit / 44.1 kHz"), QStringLiteral("Stereo"),
                            QStringLiteral("557 kbps")};
    const QString separator = QStringLiteral(" · ");
    QTRY_COMPARE(format->property("text").toString(), parts.join(separator));
    QTRY_VERIFY(row->implicitWidth() > 0 && row->implicitWidth() <= format->width());

    // The width of each part as a label of the status bar draws it, and of
    // the dot between two with its 6 px either side.
    const QFontMetricsF metrics(stateLabel->property("font").value<QFont>());
    const qreal dot = metrics.horizontalAdvance(QStringLiteral("·")) + 12;
    qsizetype previous = parts.size();
    QSet<qsizetype> seen;
    for (int width = 1000; width >= 96; width -= 8) {
        harness.window->setWidth(width);
        QTRY_COMPARE(status->width(), qreal(width));
        QCoreApplication::processEvents();
        const QString text = format->property("text").toString();
        const qsizetype shown = text.isEmpty() ? 0 : text.split(separator).size();
        // Whole parts, from the first.
        QCOMPARE(text, parts.mid(0, shown).join(separator));
        QVERIFY2(shown <= previous, qPrintable(QStringLiteral("%1 at %2 px").arg(text).arg(width)));
        // Within the room, and dropping the next part only because it
        // wouldn't fit.
        QVERIFY2(row->implicitWidth() <= format->width() + 0.5,
                 qPrintable(QStringLiteral("%1 px in %2 at %3").arg(row->implicitWidth()).arg(format->width()).arg(width)));
        if (shown < parts.size()) {
            const qreal next = metrics.horizontalAdvance(parts[shown]) + (shown > 0 ? dot : 0);
            QVERIFY2(row->implicitWidth() + next > format->width() - 1.0,
                     qPrintable(QStringLiteral("\"%1\" dropped with %2 px free at %3")
                                    .arg(parts[shown])
                                    .arg(format->width() - row->implicitWidth())
                                    .arg(width)));
        }
        seen.insert(shown);
        previous = shown;
    }
    // On the way down, each count from all four to none.
    QCOMPARE(seen, (QSet<qsizetype>{0, 1, 2, 3, 4}));
    // And back: all of it once there is room again.
    harness.window->setWidth(1000);
    QTRY_COMPARE(format->property("text").toString(), parts.join(separator));
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
    QQuickItem *bar = harness.item(QStringLiteral("scrobbleIndicatorBar"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QQuickItem *label = harness.item(QStringLiteral("scrobbleIndicatorLabel"));
    QVERIFY(slot && box && bar && glyph && label);
    QObject *bridge = harness.bridge;
    QVERIFY(!slot->isVisible());
    const auto progress = [slot]() { return slot->property("barProgress").toDouble(); };

    // Listening: the glyph in the secondary colour, and under it the bar in
    // the text colour, as long as the listening done; the time left in the
    // tooltip.
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    bridge->setProperty("lastFmScrobbleListenedMs", 77000);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(!label->isVisible());
    // A 16 px glyph (#373 H6: 10 px inside a ring was a smudge at 1x), and
    // a 3 px bar 2 px under it, as wide as it (#373 H8: 2 px was too faint
    // for its states to differ by shape).
    QCOMPARE(box->size(), QSizeF(16, 21));
    QCOMPARE(glyph->size(), QSizeF(16, 16));
    QCOMPARE(glyph->position(), QPointF(0, 0));
    QVERIFY(bar->isVisible());
    QCOMPARE(bar->y(), 18.0);
    QCOMPARE(bar->x(), 0.0);
    QCOMPARE(bar->height(), 3.0);
    QCOMPARE(bar->opacity(), 1.0);
    QCOMPARE(slot->property("barWidth").toInt(), 16);
    QCOMPARE(progress(), std::floor(77.0 / 115.0 * 16) / 16);
    QCOMPARE(bar->width(), std::floor(77.0 / 115.0 * 16));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(sameColor(bar->property("color").value<QColor>(), harness.token("text")));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 0:38 to scrobble"));
    QCOMPARE(accessibleText(slot, QAccessible::Name), QStringLiteral("Last.fm: 0:38 to scrobble"));
    QCOMPARE(accessibleText(slot, QAccessible::Description), QStringLiteral("Press to pause scrobbling."));
    // Until the track's length is known, there is no scrobble point: the
    // glyph alone.
    bridge->setProperty("lastFmScrobbleThresholdMs", 0);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: listening"));
    QCOMPARE(progress(), 0.0);
    QVERIFY(!bar->isVisible());
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);

    // Once playback has stopped after a play that ended before its
    // scrobble point: the slot says so, the glyph is quieter, there is no
    // bar, and the tooltip says how far the play got.
    bridge->setProperty("lastFmScrobbleState", 6);
    bridge->setProperty("lastFmScrobbleIneligibility", 3);
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Not scrobbled"));
    QVERIFY(label->isVisible());
    QVERIFY(!bar->isVisible());
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm.svg")));
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

    // Being sent and scrobbled differ by shape (#373 H8): while it is sent,
    // the full 3 px bar in the text colour at half strength; once accepted,
    // no bar, and the glyph in the secondary colour. The glyph is the same
    // in both, at full strength.
    bridge->setProperty("lastFmScrobbleState", 2);
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(bar->isVisible());
    QCOMPARE(progress(), 1.0);
    QCOMPARE(bar->width(), 16.0);
    QCOMPARE(bar->height(), 3.0);
    QCOMPARE(bar->opacity(), 0.5);
    QVERIFY(sameColor(bar->property("color").value<QColor>(), harness.token("text")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(glyph->opacity(), 1.0);
    QVERIFY(!label->isVisible());
    bridge->setProperty("lastFmScrobbleState", 3);
    QTRY_VERIFY(!bar->isVisible());
    QVERIFY(slot->isVisible());
    QCOMPARE(progress(), 0.0);
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(glyph->opacity(), 1.0);
    QVERIFY(!label->isVisible());
    // The glyph keeps its place.
    QCOMPARE(box->size(), QSizeF(16, 21));
    QCOMPARE(glyph->position(), QPointF(0, 0));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: scrobbled"));
    bridge->setProperty("lastFmScrobbleState", 2);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: sending the scrobble"));

    // Queued: the words and the 14 px offline glyph where the Last.fm glyph
    // is, without the bar.
    bridge->setProperty("lastFmScrobbleState", 5);
    bridge->setProperty("lastFmPendingScrobbleCount", 3);
    QTRY_VERIFY(slot->isVisible());
    QCOMPARE(label->property("text").toString(), QStringLiteral("3 scrobbles waiting"));
    QVERIFY(label->isVisible());
    QVERIFY(!bar->isVisible());
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("status-offline.svg")));
    QCOMPARE(glyph->size(), QSizeF(14, 14));
    QCOMPARE(glyph->position(), QPointF(1, 1));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(toolTipText(slot).startsWith(QStringLiteral("Last.fm can't be reached.")));
    bridge->setProperty("lastFmPendingScrobbleCount", 1);
    QCOMPARE(label->property("text").toString(), QStringLiteral("1 scrobble waiting"));

    // Failed: the words, and the glyph and the full bar in the negative
    // colour, the slot's only colour, at least 3:1 on the chrome.
    bridge->setProperty("lastFmPendingScrobbleCount", 0);
    bridge->setProperty("lastFmScrobbleState", 4);
    bridge->setProperty("lastFmScrobbleMessage", QStringLiteral("Invalid session key"));
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Scrobble failed"));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm.svg")));
    QCOMPARE(glyph->size(), QSizeF(16, 16));
    QVERIFY(bar->isVisible());
    QCOMPARE(progress(), 1.0);
    QCOMPARE(bar->opacity(), 1.0);
    const QColor negative = harness.token("negative");
    QVERIFY(sameColor(bar->property("color").value<QColor>(), negative));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), negative));
    QVERIFY2(contrastRatio(negative, harness.token("chrome")) >= 3.0,
             qPrintable(describe(QStringLiteral("failed glyph"), negative, harness.token("chrome"))));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm didn't accept the scrobble: Invalid session key"));

    // Paused by the user: the glyph struck through, quieter, without the
    // bar, in the same place; a click resumes.
    bridge->setProperty("lastFmScrobbleState", 1);
    bridge->setProperty("lastFmScrobblingPaused", true);
    QTRY_COMPARE(toolTipText(slot), QStringLiteral("Last.fm: scrobbling is paused"));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm-paused.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textTertiary")));
    QVERIFY(!bar->isVisible());
    QCOMPARE(box->size(), QSizeF(16, 21));
    QCOMPARE(glyph->position(), QPointF(0, 0));
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(slot));
    QTRY_VERIFY(!bridge->property("lastFmScrobblingPaused").toBool());

    // Signed out or turned off: empty.
    bridge->setProperty("lastFmAuthState", 0);
    QTRY_VERIFY(!slot->isVisible());
}

// The time left comes from the scrobbler's listened time alone: a seek
// either way and a pause leave it, and only a new play, whose count starts
// from zero, starts it over. The bar and the tooltip move by whole seconds
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
    const double atThirty = slot->property("barProgress").toDouble();
    QVERIFY(atThirty > 0.0);

    // Seeks forward and back, and a pause and a resume: the position and
    // the playback state move, the count doesn't.
    for (const double position : {90.0, 10.0}) {
        harness.root->setProperty("positionSeconds", position);
        QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:25 to scrobble"));
        QCOMPARE(slot->property("barProgress").toDouble(), atThirty);
    }
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:25 to scrobble"));
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:25 to scrobble"));
    QCOMPARE(slot->property("barProgress").toDouble(), atThirty);

    // Snapshots within a second of listening change nothing.
    QSignalSpy progressMoved(slot, SIGNAL(barProgressChanged()));
    QSignalSpy textChanged(slot, SIGNAL(stateTextChanged()));
    for (int listened = 30040; listened < 31000; listened += 40) {
        bridge->setProperty("lastFmScrobbleListenedMs", listened);
    }
    QCOMPARE(progressMoved.count(), 0);
    QCOMPARE(textChanged.count(), 0);
    bridge->setProperty("lastFmScrobbleListenedMs", 31000);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:24 to scrobble"));
    // The bar moves by whole device pixels: with the second, if it crosses
    // one.
    const double barPixels = std::round(slot->property("barWidth").toDouble()
                                        * std::max(1.0, harness.window->devicePixelRatio()));
    const bool crossesAPixel = std::floor(31.0 / 115.0 * barPixels) != std::floor(30.0 / 115.0 * barPixels);
    QCOMPARE(progressMoved.count(), crossesAPixel ? 1 : 0);
    QCOMPARE(textChanged.count(), 1);

    // A new play (a new track, a replay, Repeat One) counts from zero.
    bridge->setProperty("lastFmScrobbleListenedMs", 0);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 1:55 to scrobble"));
    QCOMPARE(slot->property("barProgress").toDouble(), 0.0);
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
    QVERIFY2(differing >= 30, qPrintable(QString::number(differing)));
    // The stroke crosses the box's middle, from corner to corner.
    const int side = int(sceneRect(box).width());
    QVERIFY(pausedInk > 0);
    QVERIFY(paused[(side / 2) * side + side / 2]);
    QVERIFY(paused[5 * side + 5]);
    QVERIFY(paused[(side - 6) * side + side - 6]);
}

void TransportStatusTest::lastFmSlotBarFillsWithoutATrough_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The bar under the glyph, as drawn at 1x (#373 H6): as many whole pixels
// of the glyph's 16 as the listening done, from the left, and the chrome
// everywhere else: no trough, so at 0 % there is the glyph alone, and
// nothing between the glyph and the bar. Each step reads apart from the
// last, the bar 3:1 on the chrome; being sent fills it at half strength,
// scrobbled leaves it out, failed fills it in the negative colour.
void TransportStatusTest::lastFmSlotBarFillsWithoutATrough() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QCOMPARE(harness.window->effectiveDevicePixelRatio(), 1.0);
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *box = harness.item(QStringLiteral("scrobbleIndicatorBox"));
    QVERIFY(slot && box);
    QObject *bridge = harness.bridge;
    const QColor chrome = harness.token("chrome");
    bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    bridge->setProperty("lastFmScrobbleListenedMs", 0);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());

    // The 2 px gap under the glyph and the bar's 3 px rows across the
    // glyph's width: the pixels drawn in `colour`, which must start at the
    // left and run on, and those drawn in neither it nor the chrome.
    struct Drawn {
        int lit = 0;
        int stray = 0;
        bool fromTheLeft = true;
    };
    qreal scale = 1;
    const auto drawn = [&](const QColor &colour) {
        const QImage image = grabbed(harness.window, &scale);
        const QPointF topLeft = sceneRect(box).topLeft();
        Drawn result;
        for (int y = 16; y < 21; ++y) {
            bool ended = false;
            for (int x = 0; x < 16; ++x) {
                const QColor pixel = pixelAt(image, topLeft + QPointF(x + 0.5, y + 0.5), scale);
                const bool isChrome = channelDistance(pixel, chrome) <= 2;
                const bool isLit = y >= 18 && channelDistance(pixel, colour) <= 2;
                if (isLit) {
                    result.lit += 1;
                    result.fromTheLeft = result.fromTheLeft && !ended;
                } else {
                    ended = true;
                    result.stray += isChrome ? 0 : 1;
                }
            }
        }
        return result;
    };
    const QColor text = harness.token("text");
    QVERIFY2(contrastRatio(text, chrome) >= 3.0, qPrintable(describe(QStringLiteral("bar"), text, chrome)));
    int previous = -1;
    for (const double done : {0.0, 0.25, 0.5, 0.9}) {
        bridge->setProperty("lastFmScrobbleListenedMs", int(done * 120000));
        const int columns = int(std::floor(done * 16));
        QTRY_COMPARE(slot->property("barProgress").toDouble(), columns / 16.0);
        Drawn now;
        QTRY_VERIFY2((now = drawn(text), now.lit == 3 * columns),
                     qPrintable(QStringLiteral("%1 done drew %2 px of the bar").arg(done).arg(now.lit)));
        QVERIFY2(now.stray == 0, qPrintable(QStringLiteral("%1 done: %2 px of trough or ink").arg(done).arg(now.stray)));
        QVERIFY(now.fromTheLeft);
        // Apart from the last by a quarter of the bar or more.
        QVERIFY2(columns - previous >= 4 || previous < 0, qPrintable(QString::number(columns - previous)));
        previous = columns;
    }

    // Being sent: full, the text colour at half strength. Scrobbled: no bar
    // at all. Failed: full, in the negative colour.
    const QColor sending = drawnOver(withAlpha(text, 0.5), chrome);
    for (const auto &[state, colour, lit] :
         {std::tuple{2, sending, 48}, std::tuple{3, text, 0}, std::tuple{4, harness.token("negative"), 48}}) {
        bridge->setProperty("lastFmScrobbleState", state);
        if (lit > 0) {
            QVERIFY2(contrastRatio(colour, chrome) >= 1.5,
                     qPrintable(describe(QStringLiteral("bar"), colour, chrome)));
        }
        Drawn now;
        QTRY_VERIFY2((now = drawn(colour), now.lit == lit && now.stray == 0),
                     qPrintable(QStringLiteral("%1: %2 px, %3 stray").arg(state).arg(now.lit).arg(now.stray)));
    }
}

void TransportStatusTest::lastFmSlotSendingAndScrobbledLookApart_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// Listening, being sent and scrobbled look apart as drawn, by shape and
// not by shade (#373 H5, H6: they drew alike; H8: they still differed
// mainly in shade). Listening: a bar in the text colour, never full length
// (a play at its scrobble point is being sent). Being sent: the full bar at
// half strength, which can't be taken for a listening bar at any length,
// as half its length could. Scrobbled: no bar, the glyph in the secondary
// colour. The glyph is drawn alike in all three.
void TransportStatusTest::lastFmSlotSendingAndScrobbledLookApart() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *box = harness.item(QStringLiteral("scrobbleIndicatorBox"));
    QQuickItem *bar = harness.item(QStringLiteral("scrobbleIndicatorBar"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QVERIFY(slot && box && bar && glyph);
    const QColor chrome = harness.token("chrome");
    const QColor text = harness.token("text");
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 120000);

    qreal scale = 1;
    // The bar's row through its middle, from the left of the glyph, and
    // the glyph's strongest pixel.
    struct Drawn {
        QList<QColor> bar;
        QColor glyph;
    };
    const auto drawn = [&]() {
        const QImage image = grabbed(harness.window, &scale);
        Drawn result;
        const QPointF boxTopLeft = sceneRect(box).topLeft();
        for (int x = 0; x < 16; ++x) {
            result.bar.append(pixelAt(image, boxTopLeft + QPointF(x + 0.5, 19.5), scale));
        }
        result.glyph = chrome;
        for (int y = 0; y < int(glyph->height()); ++y) {
            for (int x = 0; x < int(glyph->width()); ++x) {
                const QColor pixel = pixelAt(image, glyph->mapToScene(QPointF(x + 0.5, y + 0.5)), scale);
                if (channelDistance(pixel, chrome) > channelDistance(result.glyph, chrome)) {
                    result.glyph = pixel;
                }
            }
        }
        return result;
    };
    const auto count = [](const QList<QColor> &row, const QColor &colour) {
        return int(std::count_if(row.cbegin(), row.cend(),
                                 [&colour](const QColor &pixel) { return channelDistance(pixel, colour) <= 2; }));
    };

    // Listening, 15 s short of the scrobble point: the bar in the text
    // colour, short of full, by whole pixels at 1x and 2x.
    harness.bridge->setProperty("lastFmScrobbleListenedMs", 105000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible() && bar->isVisible());
    Drawn listening;
    QTRY_VERIFY((listening = drawn(), count(listening.bar, text) == 14));
    QCOMPARE(count(listening.bar, chrome), 2);

    // Being sent: the full bar, every pixel of it half the text colour on
    // the chrome, and none of it the listening bar's colour.
    harness.bridge->setProperty("lastFmScrobbleListenedMs", 120000);
    harness.bridge->setProperty("lastFmScrobbleState", 2);
    const QColor half = drawnOver(withAlpha(text, 0.5), chrome);
    Drawn sending;
    QTRY_VERIFY((sending = drawn(), count(sending.bar, half) == 16));
    QVERIFY2(channelDistance(half, text) >= 48 && channelDistance(half, chrome) >= 48,
             qPrintable(half.name() + QStringLiteral(" between ") + text.name() + QStringLiteral(" and ")
                        + chrome.name()));
    QCOMPARE(count(sending.bar, text), 0);

    // Scrobbled: no bar, only the chrome under the glyph.
    harness.bridge->setProperty("lastFmScrobbleState", 3);
    Drawn scrobbled;
    QTRY_VERIFY((scrobbled = drawn(), count(scrobbled.bar, chrome) == 16));
    QVERIFY(!bar->isVisible());

    // The glyph: the secondary colour at full strength in all three.
    // Offscreen, without the desktop's platform theme, Kirigami draws a mask
    // file uncoloured, so the drawn glyph shows its strength, not its colour.
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(glyph->opacity(), 1.0);
    QVERIFY2(channelDistance(listening.glyph, sending.glyph) <= 2
                 && channelDistance(sending.glyph, scrobbled.glyph) <= 2,
             qPrintable(listening.glyph.name() + QStringLiteral(" / ") + sending.glyph.name()
                        + QStringLiteral(" / ") + scrobbled.glyph.name()));
}

void TransportStatusTest::lastFmSlotGlyphReadsAtOneX_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The Last.fm glyph is drawn for 1x at 16 px on whole pixels (#373 H6: a
// 10 px glyph in a ring was a smudge; H8: H6's bold one read "QS"): its ink
// within rows 3 to 12 and columns 1 to 14, every pixel ink or clear, in
// Breeze's stroke weight (2 px verticals, 1 px horizontals, about 1.5 px),
// and a two-storey a that reads as one: its bowl's counter open, the bowl
// open to the left under the arch, the stem whole, and apart from the s.
void TransportStatusTest::lastFmSlotGlyphReadsAtOneX() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QCOMPARE(harness.window->effectiveDevicePixelRatio(), 1.0);
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QVERIFY(slot && glyph);
    const QColor chrome = harness.token("chrome");
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    qreal scale = 1;
    // Once the status bar has laid the slot out.
    grabbed(harness.window, &scale);
    QCOMPARE(glyph->size(), QSizeF(16, 16));
    // On whole pixels.
    const QPointF topLeft = sceneRect(glyph).topLeft();
    QCOMPARE(topLeft, QPointF(std::floor(topLeft.x()), std::floor(topLeft.y())));

    // Ink, not its colour: offscreen, without the desktop's platform theme,
    // Kirigami draws a mask file uncoloured. The ink of each pixel, 0 to 1
    // of the strongest.
    const QImage image = grabbed(harness.window, &scale);
    QList<int> distance(16 * 16);
    int strongest = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            distance[y * 16 + x] = channelDistance(pixelAt(image, topLeft + QPointF(x + 0.5, y + 0.5), scale), chrome);
            strongest = std::max(strongest, distance[y * 16 + x]);
        }
    }
    QVERIFY2(strongest > 24, qPrintable(QString::number(strongest)));
    const auto ink = [&](int x, int y) { return double(distance[y * 16 + x]) / strongest; };
    int inked = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const bool inside = y >= 3 && y <= 12 && x >= 1 && x <= 14;
            inked += ink(x, y) > 0.1 ? 1 : 0;
            QVERIFY2(inside || ink(x, y) <= 0.1, qPrintable(QStringLiteral("ink at %1,%2").arg(x).arg(y)));
        }
    }
    QVERIFY2(inked >= 50, qPrintable(QString::number(inked)));
    // On the pixel grid: no antialiased edge, every pixel ink or clear.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            QVERIFY2(ink(x, y) >= 0.9 || ink(x, y) <= 0.1,
                     qPrintable(QStringLiteral("%1 at %2,%3").arg(ink(x, y)).arg(x).arg(y)));
        }
    }
    // The stroke weight: how thick the stroke is at each pixel of ink, its
    // shortest run of ink across, down or along either diagonal. Never more
    // than 2 px, and about 1.5 on average, the weight of the status bar's
    // other glyphs: H6's 2 px strokes everywhere, 1.8 on this measure, read
    // blocky beside Breeze's line icons.
    const auto isInk = [&](int x, int y) { return x >= 0 && x < 16 && y >= 0 && y < 16 && ink(x, y) >= 0.9; };
    double weight = 0;
    int weighed = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (!isInk(x, y)) {
                continue;
            }
            int thickness = 16;
            for (const QPoint step : {QPoint(1, 0), QPoint(0, 1), QPoint(1, 1), QPoint(1, -1)}) {
                int run = 1;
                for (const int sign : {1, -1}) {
                    for (int k = 1; isInk(x + sign * k * step.x(), y + sign * k * step.y()); ++k) {
                        ++run;
                    }
                }
                thickness = std::min(thickness, run);
            }
            QVERIFY2(thickness <= 2, qPrintable(QStringLiteral("%1 px at %2,%3").arg(thickness).arg(x).arg(y)));
            weight += thickness;
            ++weighed;
        }
    }
    weight /= weighed;
    QVERIFY2(weight >= 1.2 && weight <= 1.6, qPrintable(QString::number(weight)));
    const auto clear = [&](int left, int top, int right, int bottom) {
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                if (ink(x, y) > 0.1) {
                    return false;
                }
            }
        }
        return true;
    };
    const auto solid = [&](int left, int top, int right, int bottom) {
        for (int y = top; y <= bottom; ++y) {
            for (int x = left; x <= right; ++x) {
                if (ink(x, y) < 0.9) {
                    return false;
                }
            }
        }
        return true;
    };
    // The a: an arch over the stem, the stem whole in columns 6 and 7 from
    // the arch to the baseline, a bowl with a 3 px counter, and under the
    // arch the bowl open to the left, so that it is no o, 0 or Q.
    QVERIFY(solid(2, 3, 5, 3));
    QVERIFY(solid(6, 4, 7, 12));
    QVERIFY(solid(2, 7, 5, 7) && solid(1, 8, 2, 10));
    QVERIFY(clear(3, 8, 5, 10));
    QVERIFY(clear(1, 4, 5, 6));
    // A clear column between the a and the s.
    QVERIFY(clear(8, 3, 8, 12));
    // The s: three bars with its two counters open between them.
    QVERIFY(solid(10, 3, 13, 3) && solid(10, 7, 13, 7) && solid(10, 12, 13, 12));
    QVERIFY(clear(11, 5, 14, 6) && clear(9, 8, 12, 10));
}

void TransportStatusTest::lastFmBarReadsApartFromTheScanBar_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The Last.fm bar is the glyph's, not a second scan or a download (#373
// H6), with a scan running beside it: as wide as the glyph and under it,
// less than a quarter of the scan bar's length and thinner than it, with
// square ends where the scan bar's are round, no trough where the scan
// bar has one, and none of the scan bar's colours in any state.
void TransportStatusTest::lastFmBarReadsApartFromTheScanBar() {
    QFETCH(bool, dark);
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(dark ? kBreezeDark : kBreezeLight, 1600, &errorText), qPrintable(errorText));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *bar = harness.item(QStringLiteral("scrobbleIndicatorBar"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QQuickItem *scanBar = harness.item(QStringLiteral("statusBarScanBar"));
    QQuickItem *scanFill = harness.item(QStringLiteral("statusBarScanBarFill"));
    QVERIFY(slot && bar && glyph && scanBar && scanFill);
    QObject *bridge = harness.bridge;
    bridge->setProperty("libraryScanProcessed", 412);
    bridge->setProperty("libraryScanTotal", 1204);
    bridge->setProperty("libraryScanInProgress", true);
    bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    bridge->setProperty("lastFmScrobbleListenedMs", 60000);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible() && bar->isVisible() && scanBar->isVisible());
    qreal scale = 1;
    grabbed(harness.window, &scale);

    // Its place and size: under the glyph, 2 px below it, at most its width.
    const QRectF glyphRect = sceneRect(glyph);
    const QRectF barRect = sceneRect(bar);
    QCOMPARE(barRect.left(), glyphRect.left());
    QCOMPARE(barRect.top() - glyphRect.bottom(), 2.0);
    QCOMPARE(slot->property("barWidth").toReal(), glyphRect.width());
    QVERIFY(barRect.width() <= glyphRect.width());
    QVERIFY2(glyphRect.width() * 4 < scanBar->width(), qPrintable(QString::number(scanBar->width())));
    // 3 px to the scan bar's 4 (#373 H8: 2 px was too faint to carry its
    // states by shape).
    QCOMPARE(bar->height(), 3.0);
    QVERIFY2(bar->height() < scanBar->height(), qPrintable(QString::number(scanBar->height())));
    QCOMPARE(bar->property("radius").toReal(), 0.0);
    QVERIFY(scanBar->property("radius").toReal() > 0.0);
    // The scan bar's trough is drawn; where the Last.fm bar is still to
    // come, the chrome.
    const QColor chrome = harness.token("chrome");
    const QImage image = grabbed(harness.window, &scale);
    const QPointF scanTrough(sceneRect(scanBar).right() - 2, sceneRect(scanBar).center().y());
    QVERIFY(!sameColor(pixelAt(image, scanTrough, scale), chrome));
    const QPointF barToCome(glyphRect.right() - 1.5, barRect.center().y());
    QVERIFY2(sameColor(pixelAt(image, barToCome, scale), chrome), qPrintable(pixelAt(image, barToCome, scale).name()));

    // Its colours as drawn, listening, sending and failed, are none of the
    // scan bar's; once scrobbled there is no bar.
    const QColor scanFillColor = scanFill->property("color").value<QColor>();
    const QColor scanTroughColor = drawnOver(scanBar->property("color").value<QColor>(), chrome);
    for (const int state : {1, 2, 3, 4}) {
        bridge->setProperty("lastFmScrobbleState", state);
        if (state == 3) {
            QTRY_VERIFY(!bar->isVisible());
            continue;
        }
        QTRY_VERIFY(bar->isVisible());
        const QColor colour = drawnOver(withAlpha(bar->property("color").value<QColor>(), bar->opacity()), chrome);
        QVERIFY2(channelDistance(colour, scanFillColor) >= 24 && channelDistance(colour, scanTroughColor) >= 24,
                 qPrintable(QStringLiteral("state %1: %2 by %3 and %4").arg(state).arg(colour.name(),
                                                                                       scanFillColor.name(),
                                                                                       scanTroughColor.name())));
    }
}

// "Bridge disconnected" was the developer's word for it (#373 H5): the
// status bar says the playback engine isn't running, and its tooltip what
// to do.
void TransportStatusTest::statusBarNamesAStoppedEngineInPlainWords() {
    Harness harness;
    QString errorText;
    QVERIFY2(harness.load(kBreezeDark, 1600, &errorText), qPrintable(errorText));
    QQuickItem *label = harness.item(QStringLiteral("statusBarDisconnectedLabel"));
    QQuickItem *state = harness.item(QStringLiteral("statusBarPlaybackState"));
    QVERIFY(label && state);
    QVERIFY(!label->isVisible());
    harness.bridge->setProperty("connected", false);
    QTRY_VERIFY(label->isVisible());
    QVERIFY(!state->isVisible());
    QCOMPARE(label->property("text").toString(), QStringLiteral("Playback engine not running"));
    const QString tip = QStringLiteral(
        "Ferrous can't play music until its playback engine runs. Restart Ferrous to try again.");
    QCOMPARE(toolTipText(label), tip);
    QCOMPARE(accessibleText(label, QAccessible::Description), tip);
    QTest::mouseMove(harness.window, sceneCentre(label));
    QTRY_VERIFY(toolTipShown(label));
    // A failure the backend reported says so instead, in the same words
    // (#373 H6: "Backend stopped").
    QQuickItem *stopped = harness.item(QStringLiteral("statusBarBackendStoppedLabel"));
    QVERIFY(stopped != nullptr);
    QVERIFY(!stopped->isVisible());
    harness.bridge->setProperty("backendFailure", QStringLiteral("stopped for testing"));
    QTRY_VERIFY(!label->isVisible());
    QTRY_VERIFY(stopped->isVisible());
    QCOMPARE(stopped->property("text").toString(), QStringLiteral("Playback engine stopped"));
    QVERIFY(sameColor(stopped->property("color").value<QColor>(), harness.token("negative")));
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
    const QList<int> glyphs{38, 48, 38, 38};
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
