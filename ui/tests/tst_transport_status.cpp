// SPDX-License-Identifier: GPL-3.0-or-later

// The transport and the status bar of #373: the flat transport buttons and
// the one Play/Pause button, the seek waveform's colours, contrast and
// states as drawn, the time readout, the volume slider's hover and focus,
// and the status bar's playback, repeat, format, scan, Last.fm and issue
// states.

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QFile>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <Kirigami/Platform/Units>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <tuple>
#include <utility>

#include "InputModality.h"
#include "Localization.h"
#include "StandardKeys.h"
#include "TestColor.h"
#include "TestItems.h"
#include "TestMainWindow.h"
#include "TestMessages.h"
#include "TestMotion.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"
#include "WaveformItem.h"

namespace {

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
    // The Header colour set's background, which the transport takes, as
    // Kirigami's platform theme gives it.
    QString header;
};

const Scheme kBreezeDark{QStringLiteral("#202326"), QStringLiteral("#141618"), QStringLiteral("#fcfcfc"),
                         QStringLiteral("#a1a9b1"), QStringLiteral("#3daee9"), QStringLiteral("#da4453"),
                         QStringLiteral("#292c30"), QStringLiteral("#292c30")};
// A dark scheme with a darker accent, whose waveform needs more than 50%
// to keep 3:1.
const Scheme kPurpleDark{QStringLiteral("#2a2e32"), QStringLiteral("#1b1e20"), QStringLiteral("#eff0f1"),
                         QStringLiteral("#a1a9b1"), QStringLiteral("#9b59b6"), QStringLiteral("#da4453"),
                         QStringLiteral("#31363b"), QStringLiteral("#31363b")};
const Scheme kBreezeLight{QStringLiteral("#eff0f1"), QStringLiteral("#ffffff"), QStringLiteral("#232629"),
                          QStringLiteral("#707d8a"), QStringLiteral("#3daee9"), QStringLiteral("#da4453"),
                          QStringLiteral("#fcfcfc"), QStringLiteral("#dee0e2")};

const Scheme &breeze(bool dark) {
    return dark ? kBreezeDark : kBreezeLight;
}

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
    // How often Space reached the Play/Pause shortcut.
    property int playPauseCount: 0
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
        property bool waveformEndsEarly: false
        property string currentTrackPath: ""
        property int queueLength: 0
        property string playingQueueLengthText: ""
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
    // Space plays and pauses, as in the main window: a focused control
    // that takes Space must keep it from this shortcut.
    Action {
        id: playPauseAction
        text: "&Pause"
        shortcut: "Space"
        onTriggered: window.playPauseCount += 1
    }
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
                // The Header colour set's background, as the platform
                // theme gives it to the transport's colour set.
                Kirigami.Theme.backgroundColor: "%9"
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
        .arg(scheme.header)
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

    // Why load() failed.
    QString error;

    bool load(const Scheme &scheme = kBreezeDark, int width = 1600) {
        QQmlComponent component(&engine);
        component.setData(harnessQml(scheme, width),
                          QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/TransportStatusHarness.qml")));
        root.reset(component.create());
        if (!root) {
            error = formatErrors(component.errors());
            return false;
        }
        window = qobject_cast<QQuickWindow *>(root.get());
        bridge = root->property("bridge").value<QObject *>();
        tokens = root->property("tokens").value<QObject *>();
        errors = root->property("errors").value<QObject *>();
        if (window == nullptr || bridge == nullptr || tokens == nullptr || errors == nullptr
            || !QTest::qWaitForWindowExposed(window)) {
            error = QStringLiteral("the harness did not show");
            return false;
        }
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window)) {
            error = QStringLiteral("the harness did not become active");
            return false;
        }
        auto *start = root->property("startButton").value<QQuickItem *>();
        start->forceActiveFocus(Qt::TabFocusReason);
        return start->hasActiveFocus();
    }

    // An issue for the status bar, of `category` and `severity`, as the
    // error controller is told one; it logs each as a warning, which the
    // test drops.
    bool reportIssue(int category, int severity, const QString &message) const {
        const bool reported = QMetaObject::invokeMethod(errors, "report", Q_ARG(QVariant, category),
                                                        Q_ARG(QVariant, severity), Q_ARG(QVariant, QString()),
                                                        Q_ARG(QVariant, message), Q_ARG(QVariant, QString()));
        takeCapturedMessages();
        return reported;
    }

    QQuickItem *item(const QString &name) const {
        return root->findChild<QQuickItem *>(name);
    }

    QColor token(const char *name) const {
        return tokens->property(name).value<QColor>();
    }

    // The transport's chrome: its Header colour set's background.
    QColor transportChrome() const {
        return item(QStringLiteral("transportBar"))->property("chromeColor").value<QColor>();
    }
};

// The engine's Kirigami.Units, whose durations Plasma's animation speed
// sets: with animations off, short is 0 and long is clamped to 1 ms.
Kirigami::Platform::Units *unitsOf(QQuickItem *item) {
    return qobject_cast<Kirigami::Platform::Units *>(
        evaluateOn(item, QStringLiteral("Kirigami.Units")).value<QObject *>());
}

// Rows of Breeze Dark and Breeze Light, in a column "dark".
void addThemeRows() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// Tooltips with no delay, for a test of when one shows rather than how
// soon: the delay is Kirigami's.
void showToolTipsAtOnce(QQuickItem *item) {
    unitsOf(item)->setToolTipDelay(0);
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

QColor withAlpha(const QColor &color, double alpha) {
    QColor result = color;
    result.setAlphaF(static_cast<float>(alpha));
    return result;
}

QColor pixelAt(const QImage &image, const QPointF &scenePoint, qreal scale) {
    return image.pixelColor(static_cast<int>(std::floor(scenePoint.x() * scale)),
                            static_cast<int>(std::floor(scenePoint.y() * scale)));
}

// Full-scale peaks: the waveform fills its ±12 px everywhere.
QByteArray fullScalePeaks() {
    return QByteArray(800, static_cast<char>(255));
}

// The window drawn, with the scale of its image to the scene: the grab
// polishes and draws the window as it is now.
QImage grabbed(QQuickWindow *window, qreal *scale) {
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

// What the harness's seek bar sent, in order.
QVariantList seeksOf(const Harness &harness) {
    return harness.root->property("seeks").value<QJSValue>().toVariant().toList();
}

// Steps `animations` a frame at a time until `done`, for up to 2 s of
// animation time: for checks of where a fade or a tint ends, which then
// waits for no real time. Whether it came to `done`.
template <typename Done>
bool stepUntil(SteppedAnimationDriver &animations, Done done) {
    for (int elapsed = 0; !done(); elapsed += 16) {
        if (elapsed >= 2000) {
            return false;
        }
        (void)animations.step(16);
    }
    return true;
}

// The glyph a transport button draws.
QQuickItem *glyphOf(QQuickItem *button) {
    QList<QQuickItem *> pending{button};
    while (!pending.isEmpty()) {
        QQuickItem *item = pending.takeFirst();
        if (item == nullptr) {
            continue;
        }
        if (item->objectName() == QStringLiteral("transportButtonGlyph")) {
            return item;
        }
        pending.append(item->childItems());
    }
    return nullptr;
}

// A glyph's ink as drawn on `chrome`: the pixels at least half as far from
// it as the glyph's strongest, so that how a mask is coloured doesn't count
// (offscreen, without the desktop's platform theme, Kirigami draws a mask
// file uncoloured, which is dark on the dark chrome); their count and the
// rows they span, in scene pixels. None where nothing is drawn.
struct Ink {
    int pixels = 0;
    int height = 0;
};

Ink inkOf(QQuickWindow *window, QQuickItem *glyph, const QColor &chrome) {
    qreal scale = 1;
    const QImage image = grabbed(window, &scale);
    const int width = int(glyph->width());
    QList<int> distances;
    for (int y = 0; y < int(glyph->height()); ++y) {
        for (int x = 0; x < width; ++x) {
            distances.append(channelDistance(pixelAt(image, glyph->mapToScene(QPointF(x + 0.5, y + 0.5)), scale), chrome));
        }
    }
    Ink ink;
    const int strongest = distances.isEmpty() ? 0 : *std::max_element(distances.cbegin(), distances.cend());
    if (strongest < 16) {
        return ink;
    }
    int top = -1;
    int bottom = -1;
    for (int i = 0; i < distances.size(); ++i) {
        if (distances[i] * 2 >= strongest) {
            ink.pixels += 1;
            top = top < 0 ? i / width : top;
            bottom = i / width;
        }
    }
    ink.height = bottom - top + 1;
    return ink;
}

} // namespace

class TransportStatusTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void transportLaysOutFlatButtonsSeekTimeAndVolume();
    void volumeGrooveGrowsWithTheThumb_data();
    void volumeGrooveGrowsWithTheThumb();
    void muteButtonIsClearOfTheVolumeGroove_data();
    void muteButtonIsClearOfTheVolumeGroove();
    void previousGlyphStartsOnTheContentEdge_data();
    void previousGlyphStartsOnTheContentEdge();
    void playPauseIsOneButtonThatShowsTheState();
    void playPauseGlyphsCrossFadeWithoutABlink();
    void transportToolTipsNameTheShortcut();
    void seekWaveformMeetsItsContrastAsDrawn_data();
    void seekWaveformMeetsItsContrastAsDrawn();
    void seekViewBandMarksTheViewBehindTheWaveform_data();
    void seekViewBandMarksTheViewBehindTheWaveform();
    void seekViewBandStaysFindableAtDeepZoom();
    void seekViewBandIsDecorationOnly();
    void seekHoverDrawsALineAndANativeToolTip();
    void seekReleaseHandsTheTimeBoxToTheHover_data();
    void seekReleaseHandsTheTimeBoxToTheHover();
    void seekReleaseOffTheBarHidesTheTimeBox();
    void seekHoverLineLeavesThePlayheadWhole_data();
    void seekHoverLineLeavesThePlayheadWhole();
    void seekHoverLineWaitsForThePointerToMoveAfterASeek_data();
    void seekHoverLineWaitsForThePointerToMoveAfterASeek();
    void seekFocusDrawsOneRingAroundTheBar_data();
    void seekFocusDrawsOneRingAroundTheBar();
    void volumeGlyphHasTheTransportsWeight_data();
    void volumeGlyphHasTheTransportsWeight();
    void volumeGlyphsAreSmoothOutlines_data();
    void volumeGlyphsAreSmoothOutlines();
    void transportGlyphsDrawOnOneScale_data();
    void transportGlyphsDrawOnOneScale();
    void transportButtonsDrawTheAppsFocusRing_data();
    void transportButtonsDrawTheAppsFocusRing();
    void transportPressedReadsApartFromHover_data();
    void transportPressedReadsApartFromHover();
    void seekToolTipFadesInAtTheShortDuration();
    void seekWithoutATrackIsALineAndNoTime();
    void seekStoppedWithATrackKeepsItsLengthAndWaveform_data();
    void seekStoppedWithATrackKeepsItsLengthAndWaveform();
    void seekStoppedShowsNoOtherTracksWaveform();
    void transportStaysTheTrackThroughATrackChange();
    void seekEmptyLineRunsPastAnEarlyEndingWaveform_data();
    void seekEmptyLineRunsPastAnEarlyEndingWaveform();
    void seekMarksWhereTheFileEnds_data();
    void seekMarksWhereTheFileEnds();
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
    void statusBarModeGlyphsAreDrawnAtTheirSize();
    void statusBarShowsRepeatAndShuffleInWords();
    void statusBarShowsTheFormatGroupWithoutThePosition();
    void statusBarFormatShowsWhatFitsWhole();
    void statusBarFormatKeepsItsPlace_data();
    void statusBarFormatKeepsItsPlace();
    void statusBarShowsTheScanWithItsBar();
    void statusBarScanBarGrowsWithTheFont();
    void statusBarScanBarFillsOnlyOnceTheRescanEnds();
    void statusBarScanWithoutATotalIsStill();
    void statusBarScanKeepsItsPlaceWhileItCounts_data();
    void statusBarScanKeepsItsPlaceWhileItCounts();
    void lastFmSlotShowsEachState();
    void lastFmSlotCountsTheTimeLeftFromTheListenedTime();
    void lastFmSlotPausedLooksStruckThrough_data();
    void lastFmSlotPausedLooksStruckThrough();
    void lastFmPausedKeepsBothLettersAndTheStrike_data();
    void lastFmPausedKeepsBothLettersAndTheStrike();
    void lastFmSlotBarFillsItsTrough_data();
    void lastFmSlotBarFillsItsTrough();
    void lastFmSlotToolTipShowsOnKeyboardFocus();
    void lastFmKeyboardMenuOpensAboveTheBar_data();
    void lastFmKeyboardMenuOpensAboveTheBar();
    void lastFmSlotSendingAndScrobbledLookApart_data();
    void lastFmSlotSendingAndScrobbledLookApart();
    void lastFmGlyphIsCentredOnItsBlock_data();
    void lastFmGlyphIsCentredOnItsBlock();
    void lastFmSlotGlyphReadsAtOneX_data();
    void lastFmSlotGlyphReadsAtOneX();
    void lastFmBarReadsApartFromTheScanBar_data();
    void lastFmBarReadsApartFromTheScanBar();
    void lastFmListeningNeverFillsTheBar_data();
    void lastFmListeningNeverFillsTheBar();
    void lastFmGlyphsAreOneFamily();
    void warningGlyphIsTheSizeOfItsNeighbours();
    void statusBarNamesAStoppedEngineInPlainWords();
    void issuesButtonShowsNewSeenAndOpen_data();
    void issuesButtonShowsNewSeenAndOpen();
    void statusBarHasNoDividers();
    void transportAndStatusBarGrowWithALargeFont();
};

void TransportStatusTest::initTestCase() {
    startCapturingMessages();
    QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                     QStringLiteral("icons/breeze/index.theme"))
                  .isEmpty(),
             "The Breeze icon theme is missing: install breeze-icon-theme (docs/INSTALL.md)");
}

void TransportStatusTest::cleanup() {
    const QStringList list = takeCapturedMessages();
    QVERIFY2(list.isEmpty(), qPrintable(list.join(QLatin1Char('\n'))));
}

void TransportStatusTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

// The sizes of the design at 1×: a 48 px band; 32 px buttons 4 px apart
// from 12 px, the content's inset; the seek bar 16 px after Next; then on
// one rhythm, between what is drawn: the time 16 px after the seek bar, as
// wide as "0:00 / 0:00", the volume glyph 24 px after it, and the 96 px
// slider's groove 8 px after the glyph, 12 px from the end. The volume glyph
// is the size of Previous and Next.
void TransportStatusTest::transportLaysOutFlatButtonsSeekTimeAndVolume() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QVERIFY(transport != nullptr);
    QCOMPARE(transport->height(), 48.0);
    const qreal top = sceneRect(transport).top();

    const QStringList names{QStringLiteral("previous"), QStringLiteral("playPause"), QStringLiteral("stop"),
                            QStringLiteral("next")};
    // Play/Pause a step above its neighbours, in the same 32 px button, and
    // the others one size (22, 16 and 14 read as three weights).
    const QList<int> glyphs{16, 20, 16, 16};
    for (int i = 0; i < names.size(); ++i) {
        QQuickItem *button = harness.item(QStringLiteral("transportButton_") + names[i]);
        QVERIFY2(button != nullptr, qPrintable(names[i]));
        const QRectF rect = sceneRect(button);
        QCOMPARE(rect.size(), QSizeF(32, 32));
        // Previous's 16 px glyph, 8 px into its button, on the 12 px
        // content edge.
        QCOMPARE(rect.left(), 4.0 + i * 36.0);
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
    QCOMPARE(seekRect.left(), sceneRect(harness.item(QStringLiteral("transportButton_next"))).right() + 16.0);
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
    // 6 px from the button's frame, clear of its ring.
    QCOMPARE(sceneRect(groove).left() - muteGlyphRect.right(), 14.0);
    QCOMPARE(volumeRect.width(), 96.0);
    // Stopped, "–:–– / –:––" leaves the box as wide, and the volume where
    // it is; it fills the box as a time does, in the secondary colour and
    // tabular figures, rather than leaving a hole before the volume ("–:––"
    // alone ended some 45 px short of the box's end).
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(time->property("text").toString(), QStringLiteral("–:–– / –:––"));
    QVERIFY(position->isVisible() && length->isVisible());
    QCOMPARE(sceneRect(time), timeRect);
    QCOMPARE(sceneRect(muteGlyph), muteGlyphRect);
    for (QQuickItem *part : {position, length}) {
        QVERIFY(sameColor(part->property("color").value<QColor>(), harness.token("textSecondary")));
        QVERIFY(part->property("font").value<QFont>().featureTags().contains(QFont::Tag("tnum")));
    }
    const qreal stoppedRight = sceneRect(length).left() + length->implicitWidth();
    QVERIFY2(stoppedRight <= timeRect.right() + 0.5 && stoppedRight >= timeRect.right() - 0.15 * timeRect.width(),
             qPrintable(QStringLiteral("ends at %1, the box at %2").arg(stoppedRight).arg(timeRect.right())));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QCOMPARE(sceneRect(transport).right() - volumeRect.right(), 12.0);
}

void TransportStatusTest::playPauseIsOneButtonThatShowsTheState() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *button = harness.item(QStringLiteral("transportButton_playPause"));
    QQuickItem *play = harness.item(QStringLiteral("transportPlayPauseGlyph_play"));
    QQuickItem *pause = harness.item(QStringLiteral("transportPlayPauseGlyph_pause"));
    QVERIFY(button && play && pause);
    const auto actionText = [button]() {
        auto *action = button->property("action").value<QObject *>();
        return action != nullptr ? action->property("text").toString() : QString();
    };
    SteppedAnimationDriver animations;
    const auto shows = [&](QQuickItem *glyph, QQuickItem *other) {
        return stepUntil(animations, [&] { return glyph->opacity() == 1.0 && !other->isVisible(); });
    };

    // Playing: Pause.
    QCOMPARE(actionText(), QStringLiteral("Pause"));
    QCOMPARE(accessibleText(button, QAccessible::Name), QStringLiteral("Pause"));
    QVERIFY(shows(pause, play));

    // Paused and stopped: Play. The glyphs cross-fade.
    for (const QString &state : {QStringLiteral("Paused"), QStringLiteral("Stopped")}) {
        harness.bridge->setProperty("playbackState", state);
        QCOMPARE(actionText(), QStringLiteral("Play"));
        QCOMPARE(accessibleText(button, QAccessible::Name), QStringLiteral("Play"));
        QVERIFY(shows(play, pause));
    }
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QCOMPARE(actionText(), QStringLiteral("Pause"));
    QVERIFY(shows(pause, play));

    // A click triggers what it shows.
    auto *pauseAction = button->property("action").value<QObject *>();
    QSignalSpy paused(pauseAction, SIGNAL(triggered(QObject*)));
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(button));
    QTRY_COMPARE(paused.count(), 1);
}

// The Play/Pause glyphs cross-fade: the one shown goes out as the other
// comes in, over shortDuration, so the button never drops below 0.6 of its
// ink (a fade out, then in, left it below half for 50 ms). At rest one glyph
// is whole and the other gone; a change back mid-fade reverses from where it
// is; with animations off the swap is instant. Checked at every frame, on
// stepped animation time.
void TransportStatusTest::playPauseGlyphsCrossFadeWithoutABlink() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *play = harness.item(QStringLiteral("transportPlayPauseGlyph_play"));
    QQuickItem *pause = harness.item(QStringLiteral("transportPlayPauseGlyph_pause"));
    QVERIFY(play && pause);
    auto *swap = play->parentItem()->property("swap").value<QObject *>();
    auto *units = unitsOf(play);
    QVERIFY(swap != nullptr && units != nullptr);
    QCOMPARE(swap->objectName(), QStringLiteral("transportPlayPauseSwap"));
    // Long enough for many frames of the fade.
    units->setShortDuration(600);
    SteppedAnimationDriver animations;
    QVERIFY(stepUntil(animations, [&] { return pause->opacity() == 1.0 && !play->isVisible(); }));

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
    // Steps frame by frame until `done`, checking the ink at each frame.
    const auto fadeUntil = [&](const auto &done) {
        return stepUntil(animations, [&] {
            ++frames;
            framesWithBoth += drawn(play) > 0.0 && drawn(pause) > 0.0 ? 1 : 0;
            if (failure.isEmpty() && combined() < 0.6) {
                failure = QStringLiteral("combined ink %1 at frame %2: play %3, pause %4")
                              .arg(combined()).arg(frames).arg(drawn(play)).arg(drawn(pause));
            }
            return done();
        });
    };
    const auto settled = [&]() { return !swap->property("running").toBool(); };

    // Playing to paused: Pause goes out as Play comes in, at once.
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QVERIFY(swap->property("running").toBool());
    QVERIFY(fadeUntil([&] { return drawn(play) > 0.0 && drawn(pause) > 0.0; }));
    QVERIFY(fadeUntil(settled));
    QCOMPARE(play->opacity(), 1.0);
    QCOMPARE(pause->opacity(), 0.0);
    QVERIFY(!pause->isVisible());
    QVERIFY2(framesWithBoth > 0, "the glyphs never cross-faded");

    // Playing again, and back to paused mid-fade: the fade reverses from
    // where it is and settles on Play alone.
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QVERIFY(fadeUntil([&] { return play->opacity() < 0.9 && pause->opacity() > 0.1; }));
    const qreal turnedAt = pause->opacity();
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QVERIFY(swap->property("running").toBool());
    // No jump: Pause goes back from where it was.
    QVERIFY(std::abs(pause->opacity() - turnedAt) < 0.1);
    QVERIFY(fadeUntil(settled));
    QCOMPARE(play->opacity(), 1.0);
    QCOMPARE(pause->opacity(), 0.0);
    QVERIFY(!pause->isVisible());

    // To Pause and all the way.
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QVERIFY(fadeUntil(settled));
    QCOMPARE(pause->opacity(), 1.0);
    QCOMPARE(play->opacity(), 0.0);
    QVERIFY(!play->isVisible());
    QVERIFY2(failure.isEmpty(), qPrintable(failure));

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
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *next = harness.item(QStringLiteral("transportButton_next"));
    QQuickItem *stop = harness.item(QStringLiteral("transportButton_stop"));
    QQuickItem *playPause = harness.item(QStringLiteral("transportButton_playPause"));
    QVERIFY(next && stop && playPause);
    QCOMPARE(toolTipText(next), QStringLiteral("Next · Ctrl+Right"));
    QCOMPARE(toolTipText(stop), QStringLiteral("Stop · Ctrl+Shift+Space"));
    // Play and Pause have no shortcut of their own: Space toggles them
    // (Previous named its shortcut and Pause none).
    QCOMPARE(toolTipText(playPause), QStringLiteral("Pause · Space"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QTRY_COMPARE(toolTipText(playPause), QStringLiteral("Play · Space"));
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    // Keyboard focus shows it, as hover does.
    showToolTipsAtOnce(next);
    next->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(toolTipShown(next));
}

void TransportStatusTest::seekWaveformMeetsItsContrastAsDrawn_data() {
    QTest::addColumn<int>("scheme");
    QTest::addColumn<double>("opacity");
    QTest::addColumn<double>("measured");
    // On the Header colours (the tones before were 2.99:1 and 2.72:1 on
    // them).
    QTest::newRow("breeze dark") << 0 << 0.5 << 3.21;
    QTest::newRow("breeze light") << 1 << 0.65 << 3.26;
    // Not measured by the design: the opacity rises to keep 3.2:1.
    QTest::newRow("dark, purple accent") << 2 << 0.0 << 0.0;
}

// The played part is `wave` mixed 12% toward the chrome (dark) or 30%
// toward the text (light); the unplayed part is it at 50% or 62%, raised
// to be 3.2:1 on the chrome, a margin over 3:1. The chrome is the
// transport's Header colour. The playhead is a 2 px amber line across the
// 28 px bar, past the ±12 px waveform, with a casing on each side that
// stands apart from the played part. Measured in the drawn window.
void TransportStatusTest::seekWaveformMeetsItsContrastAsDrawn() {
    QFETCH(int, scheme);
    QFETCH(double, opacity);
    QFETCH(double, measured);
    const Scheme &colors = scheme == 0 ? kBreezeDark : (scheme == 1 ? kBreezeLight : kPurpleDark);
    const bool dark = scheme != 1;
    Harness harness;
    QVERIFY2(harness.load(colors), qPrintable(harness.error));
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

    const QColor chrome = harness.transportChrome();
    const QColor wave = harness.token("wave");
    const QColor text = harness.token("text");
    // The transport's chrome is the scheme's Header colour, not the
    // window's.
    QVERIFY(sameColor(chrome, QColor(colors.header)));
    const QColor expectedPlayed = dark ? mixed(wave, chrome, 0.12) : mixed(wave, text, 0.3);
    QVERIFY2(channelDistance(transport->property("seekPlayedColor").value<QColor>(), expectedPlayed) <= 1,
             qPrintable(transport->property("seekPlayedColor").value<QColor>().name()));
    const qreal unplayedOpacity = transport->property("seekUnplayedOpacity").toReal();
    if (opacity > 0) {
        QVERIFY2(std::abs(unplayedOpacity - opacity) < 0.001, qPrintable(QString::number(unplayedOpacity)));
    } else {
        QVERIFY2(unplayedOpacity > 0.5 && unplayedOpacity < 0.75, qPrintable(QString::number(unplayedOpacity)));
    }
    QVERIFY(sameColor(line->property("color").value<QColor>(), harness.token("now")));
    QVERIFY(sameColor(casing->property("color").value<QColor>(),
                      dark ? QColor::fromRgbF(0, 0, 0, 0.6f) : QColor::fromRgbF(1, 1, 1, 0.85f)));
    QCOMPARE(line->height(), 28.0);
    QCOMPARE(line->width(), 2.0);

    qreal scale = 1;
    const QImage image = grabbed(harness.window, &scale);
    QVERIFY(!image.isNull());
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
    const QColor expectedUnplayed = mixed(chrome, expectedPlayed, unplayedOpacity);
    QVERIFY2(channelDistance(drawnUnplayed, expectedUnplayed) <= 2,
             qPrintable(drawnUnplayed.name() + QStringLiteral(" vs ") + expectedUnplayed.name()));
    QVERIFY2(channelDistance(drawnLine, harness.token("now")) <= 1, qPrintable(drawnLine.name()));
    // Full-scale peaks reach the waveform's top and bottom rows, played
    // and unplayed, and the bar is the chrome just outside them.
    for (const qreal y : {wave_.top() + 0.5, wave_.bottom() - 0.5}) {
        const QColor playedEdge = pixelAt(image, QPointF(wave_.left() + wave_.width() * 0.25, y), scale);
        const QColor unplayedEdge = pixelAt(image, QPointF(wave_.left() + wave_.width() * 0.75, y), scale);
        QVERIFY2(channelDistance(playedEdge, expectedPlayed) <= 2, qPrintable(playedEdge.name()));
        QVERIFY2(channelDistance(unplayedEdge, mixed(chrome, expectedPlayed, unplayedOpacity)) <= 2,
                 qPrintable(unplayedEdge.name()));
    }
    for (const qreal y : {wave_.top() - 0.5, wave_.bottom() + 0.5}) {
        const QColor outside = pixelAt(image, QPointF(wave_.left() + wave_.width() * 0.25, y), scale);
        QVERIFY2(channelDistance(outside, chrome) <= 1, qPrintable(outside.name()));
    }

    // The unplayed part is at least 3:1 on the chrome (3.21:1 dark, 3.26:1
    // light on the Header colours), and the played part more, a clear step
    // apart, so the two tones stay two.
    const double unplayedContrast = contrastRatio(drawnUnplayed, drawnChrome);
    QVERIFY2(unplayedContrast >= 3.15, qPrintable(describeContrast(QStringLiteral("unplayed"), drawnUnplayed, drawnChrome)));
    QVERIFY2(measured <= 0 || contrastRatio(drawnPlayed, drawnUnplayed) >= 2.2,
             qPrintable(describeContrast(QStringLiteral("played on unplayed"), drawnPlayed, drawnUnplayed)));
    QVERIFY2(measured <= 0 || std::abs(unplayedContrast - measured) < 0.1,
             qPrintable(describeContrast(QStringLiteral("unplayed"), drawnUnplayed, drawnChrome)));
    QVERIFY2(contrastRatio(drawnPlayed, drawnChrome) >= 4.5,
             qPrintable(describeContrast(QStringLiteral("played"), drawnPlayed, drawnChrome)));
    // The casing separates the line from the played part by luminance.
    for (const QColor &casingColor : {drawnCasingLeft, drawnCasingRight}) {
        QVERIFY2(channelDistance(casingColor, drawnOver(casing->property("color").value<QColor>(), expectedPlayed)) <= 2
                     || channelDistance(casingColor,
                                        drawnOver(casing->property("color").value<QColor>(), expectedUnplayed)) <= 2,
                 qPrintable(casingColor.name()));
        QVERIFY2(contrastRatio(casingColor, drawnLine) >= 3.0,
                 qPrintable(describeContrast(QStringLiteral("line on its casing"), drawnLine, casingColor)));
    }
    QVERIFY2(contrastRatio(drawnCasingLeft, drawnPlayed) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("casing on the played part"), drawnCasingLeft, drawnPlayed)));
    QVERIFY2(contrastRatio(drawnLine, drawnChrome) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("playhead"), drawnLine, drawnChrome)));
}

void TransportStatusTest::seekViewBandMarksTheViewBehindTheWaveform_data() {
    addThemeRows();
}

// While the visualization is zoomed, a band behind the seek bar's
// waveform marks the part of the track its view shows: in the view colour,
// on the light theme a step toward the chrome (H16), across the bar's
// height, on whole device pixels. The unplayed part is at
// the unplayed opacity on the band as on the chrome, and the played part
// under the view colour at 15 %, so the band's edge shows in the
// waveform's ink on either side of the playhead: under the bright played
// part alone, the band read as starting at the playhead (H14). On the
// light theme's pale band it is 10 %: at 15 % the played part there
// paled away from its colour off the band (H15).
void TransportStatusTest::seekViewBandMarksTheViewBehindTheWaveform() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    // Half-scale peaks: the bars reach ±6 px of the waveform's ±12.
    harness.bridge->setProperty("waveformPeaksPacked", QByteArray(800, static_cast<char>(128)));
    harness.bridge->setProperty("waveformComplete", true);
    harness.bridge->setProperty("waveformCoverageSeconds", 229.0);
    harness.root->setProperty("positionSeconds", 100.0);
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QQuickItem *band = harness.item(QStringLiteral("seekViewBand"));
    QQuickItem *bandUnplayed = harness.item(QStringLiteral("seekViewBandUnplayed"));
    QQuickItem *bandPlayed = harness.item(QStringLiteral("seekViewBandPlayed"));
    QQuickItem *line = harness.item(QStringLiteral("seekPlayheadLine"));
    QVERIFY(transport && seek && waveform && band && bandUnplayed && bandPlayed && line);
    QVERIFY(!band->isVisible());
    QVERIFY(!bandUnplayed->isVisible());
    QVERIFY(!bandPlayed->isVisible());

    // A view from 80 s to 120 s of the 229 s track.
    transport->setProperty("viewStartSeconds", 80.0);
    transport->setProperty("viewEndSeconds", 120.0);
    QVERIFY(band->isVisible());
    const qreal ratio = harness.window->effectiveDevicePixelRatio();
    const QRectF bar = sceneRect(seek);
    const double barPixels = std::floor(bar.width() * ratio + 1e-6);
    const auto barX = [&](double seconds) {
        return bar.left() + std::round(seconds / 229.0 * barPixels) / ratio;
    };
    const QRectF bandRect = sceneRect(band);
    QCOMPARE(bandRect.left(), barX(80.0));
    QCOMPARE(bandRect.right(), barX(120.0));
    QCOMPARE(bandRect.top(), bar.top());
    QCOMPARE(bandRect.height(), bar.height());
    // Behind the waveform.
    QVERIFY(band->z() <= waveform->z());
    QVERIFY(band->parentItem() == waveform->parentItem());
    QVERIFY(band->parentItem()->childItems().indexOf(band)
            < band->parentItem()->childItems().indexOf(waveform));

    const QColor chrome = harness.transportChrome();
    QVERIFY(sameColor(harness.token("view"), QColor(breeze(dark).view)));
    // The band: the view colour on the dark theme, and on the light one a
    // quarter of the way from it to the chrome, about #f6f7f8 on Breeze's
    // #dee0e2 (the white view colour read louder there, H16).
    const QColor view = transport->property("seekViewBandColor").value<QColor>();
    QVERIFY2(channelDistance(view, dark ? QColor(breeze(dark).view) : QColor(0xf6, 0xf7, 0xf8)) <= 1,
             qPrintable(view.name()));
    const QColor played = transport->property("seekPlayedColor").value<QColor>();
    const qreal unplayedOpacity = transport->property("seekUnplayedOpacity").toReal();
    qreal scale = 1;
    const QImage image = grabbed(harness.window, &scale);
    QVERIFY(!image.isNull());
    const QRectF wave = sceneRect(waveform);
    const qreal middle = wave.center().y();
    const QRectF lineRect = sceneRect(line);
    QVERIFY(lineRect.left() > bandRect.left() + 10 && lineRect.right() < bandRect.right() - 10);
    const qreal playedX = (bandRect.left() + lineRect.left()) / 2;
    const qreal unplayedX = (lineRect.right() + bandRect.right()) / 2;
    // The band, where no bars are: at the bar's edges and above the bars,
    // on either side of the playhead.
    for (const qreal x : {playedX, unplayedX}) {
        for (const qreal y : {bar.top() + 0.5, wave.top() + 0.5, wave.top() + 4.5, bar.bottom() - 0.5}) {
            const QColor drawn = pixelAt(image, QPointF(x, y), scale);
            QVERIFY2(channelDistance(drawn, view) <= 1,
                     qPrintable(QStringLiteral("%1 at %2,%3").arg(drawn.name()).arg(x).arg(y)));
        }
    }
    // The bars on it: the unplayed part as on the chrome but on the band,
    // the played part under the view colour at 15 %, 10 % in light.
    const QColor drawnPlayed = pixelAt(image, QPointF(playedX, middle), scale);
    const QColor drawnUnplayed = pixelAt(image, QPointF(unplayedX, middle), scale);
    const QColor expectedPlayed = mixed(played, view, dark ? 0.15 : 0.10);
    QVERIFY2(channelDistance(drawnPlayed, expectedPlayed) <= 2,
             qPrintable(drawnPlayed.name() + QStringLiteral(" vs ") + expectedPlayed.name()));
    // Played and outside the band, the bars keep their colour.
    QVERIFY(bandRect.left() > bar.left() + 20);
    const QColor playedOutside = pixelAt(image, QPointF(bandRect.left() - 10, middle), scale);
    QVERIFY2(channelDistance(playedOutside, played) <= 2, qPrintable(playedOutside.name()));
    // The played part's cover runs from the band's left edge to the
    // playhead's line, under the playhead.
    QCOMPARE(sceneRect(bandPlayed).left(), bandRect.left());
    QQuickItem *unplayedPart = harness.item(QStringLiteral("seekUnplayed"));
    QVERIFY(unplayedPart != nullptr);
    QCOMPARE(sceneRect(bandPlayed).right(), sceneRect(unplayedPart).left());
    QCOMPARE(sceneRect(bandPlayed).top(), wave.top());
    QCOMPARE(sceneRect(bandPlayed).height(), wave.height());
    const QColor expectedUnplayed = mixed(view, played, unplayedOpacity);
    QVERIFY2(channelDistance(drawnUnplayed, expectedUnplayed) <= 2,
             qPrintable(drawnUnplayed.name() + QStringLiteral(" vs ") + expectedUnplayed.name()));
    // Outside the band, the bar is as without it.
    for (const qreal x : {bandRect.left() - 3, bandRect.right() + 3}) {
        const QColor outside = pixelAt(image, QPointF(x, wave.top() + 0.5), scale);
        QVERIFY2(channelDistance(outside, chrome) <= 1, qPrintable(outside.name()));
    }
    const QColor unplayedOutside = pixelAt(image, QPointF(bandRect.right() + 20, middle), scale);
    QVERIFY2(channelDistance(unplayedOutside, mixed(chrome, played, unplayedOpacity)) <= 2,
             qPrintable(unplayedOutside.name()));
    // The playhead is drawn over it as ever.
    QVERIFY2(channelDistance(pixelAt(image, QPointF(lineRect.left() + 0.5, wave.top() + 0.5), scale),
                             harness.token("now")) <= 1,
             "playhead");

    // Faint: a quiet step from the chrome, well under the waveform's, and
    // the waveform keeps at least its contrast on the chrome.
    const double bandContrast = contrastRatio(view, chrome);
    QVERIFY2(bandContrast >= 1.2 && bandContrast <= (dark ? 1.6 : 1.25),
             qPrintable(describeContrast(QStringLiteral("band"), view, chrome)));
    const QColor unplayedOnChrome = mixed(chrome, played, unplayedOpacity);
    QVERIFY2(contrastRatio(drawnUnplayed, view) >= contrastRatio(unplayedOnChrome, chrome),
             qPrintable(describeContrast(QStringLiteral("unplayed on the band"), drawnUnplayed, view)));
    QVERIFY2(contrastRatio(drawnPlayed, view) >= 4.5,
             qPrintable(describeContrast(QStringLiteral("played on the band"), drawnPlayed, view)));

    // A view all before the playhead: the cover is the band's, edge to
    // edge, with nothing unplayed on it.
    transport->setProperty("viewStartSeconds", 40.0);
    transport->setProperty("viewEndSeconds", 60.0);
    QVERIFY(bandPlayed->isVisible());
    QVERIFY(!bandUnplayed->isVisible());
    QCOMPARE(sceneRect(bandPlayed).left(), sceneRect(band).left());
    QCOMPARE(sceneRect(bandPlayed).right(), sceneRect(band).right());
    // And all after it: none.
    transport->setProperty("viewStartSeconds", 140.0);
    transport->setProperty("viewEndSeconds", 160.0);
    QVERIFY(!bandPlayed->isVisible());
    QVERIFY(bandUnplayed->isVisible());

    // A view past the track's end stops at the bar's end.
    transport->setProperty("viewStartSeconds", 200.0);
    transport->setProperty("viewEndSeconds", 260.0);
    QCOMPARE(sceneRect(band).right(), bar.left() + barPixels / ratio);

    // None without a range, a length or a track.
    transport->setProperty("viewStartSeconds", std::numeric_limits<double>::quiet_NaN());
    QVERIFY(!band->isVisible());
    transport->setProperty("viewStartSeconds", 80.0);
    transport->setProperty("viewEndSeconds", 120.0);
    QVERIFY(band->isVisible());
    harness.bridge->setProperty("durationSeconds", std::numeric_limits<double>::quiet_NaN());
    QVERIFY(!band->isVisible());
    QVERIFY(!bandUnplayed->isVisible());
    QVERIFY(!bandPlayed->isVisible());
    harness.bridge->setProperty("durationSeconds", 0.0);
    QVERIFY(!band->isVisible());
    harness.bridge->setProperty("durationSeconds", 229.0);
    QVERIFY(band->isVisible());
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QVERIFY(!band->isVisible());
}

// However deep the zoom, the band is 6 px wide or more: centred on the
// view's range, on whole device pixels, and within the bar at either end.
// The unplayed part's cover has the same edges.
void TransportStatusTest::seekViewBandStaysFindableAtDeepZoom() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    harness.bridge->setProperty("waveformPeaksPacked", QByteArray(800, static_cast<char>(128)));
    harness.bridge->setProperty("waveformComplete", true);
    harness.bridge->setProperty("waveformCoverageSeconds", 229.0);
    harness.root->setProperty("positionSeconds", 0.0);
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *band = harness.item(QStringLiteral("seekViewBand"));
    QQuickItem *bandUnplayed = harness.item(QStringLiteral("seekViewBandUnplayed"));
    QVERIFY(transport && seek && band && bandUnplayed);
    const qreal ratio = harness.window->effectiveDevicePixelRatio();
    const QRectF bar = sceneRect(seek);
    const double barPixels = std::floor(bar.width() * ratio + 1e-6);
    const double least = std::round(6.0 * ratio);
    const auto showView = [&](double start, double end) {
        transport->setProperty("viewStartSeconds", start);
        transport->setProperty("viewEndSeconds", end);
    };
    // The band's left edge and width, in device pixels from the bar's left.
    const auto drawn = [&]() {
        const QRectF rect = sceneRect(band);
        return QPointF((rect.left() - bar.left()) * ratio, rect.width() * ratio);
    };

    // Within the bar, centred on the range to within half a device pixel.
    for (const double start : {20.0, 50.0, 50.07, 50.13, 100.0, 180.0}) {
        for (const double span : {0.001, 0.2, 0.7}) {
            showView(start, start + span);
            QVERIFY(band->isVisible());
            const QPointF shown = drawn();
            QCOMPARE(shown.y(), least);
            QVERIFY(std::abs(shown.x() - std::round(shown.x())) < 1e-6);
            const double centre = (start + span / 2) / 229.0 * barPixels;
            QVERIFY2(std::abs(shown.x() + least / 2 - centre) <= 0.5 + 1e-6,
                     qPrintable(QStringLiteral("%1 at %2, not about %3").arg(least).arg(shown.x()).arg(centre)));
        }
    }
    // Whole at either end, also for a view partly past it.
    showView(0.0, 0.05);
    QCOMPARE(drawn(), QPointF(0.0, least));
    showView(-1.0, 0.05);
    QCOMPARE(drawn(), QPointF(0.0, least));
    showView(228.95, 229.0);
    QCOMPARE(drawn(), QPointF(barPixels - least, least));
    showView(228.99, 229.5);
    QCOMPARE(drawn(), QPointF(barPixels - least, least));
    // The unplayed part's cover is the band's, to both of its edges.
    QCOMPARE(sceneRect(bandUnplayed).right(), sceneRect(band).right());
    QCOMPARE(sceneRect(bandUnplayed).left(), sceneRect(band).left());

    // One that is wide enough keeps its own edges.
    showView(50.0, 60.0);
    QCOMPARE(drawn().x(), std::round(50.0 / 229.0 * barPixels));
    QCOMPARE(drawn().x() + drawn().y(), std::round(60.0 / 229.0 * barPixels));
}

// The band is decoration: it changes neither the seek bar's value nor what
// assistive technology reads of it, and is no item of its own there.
void TransportStatusTest::seekViewBandIsDecorationOnly() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *band = harness.item(QStringLiteral("seekViewBand"));
    QVERIFY(transport && seek && band);
    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(seek);
    QVERIFY(accessible != nullptr && accessible->valueInterface() != nullptr);
    const auto state = [&]() {
        return QVariantList{seek->property("value"), seek->property("from"), seek->property("to"),
                            accessible->valueInterface()->currentValue(),
                            accessible->text(QAccessible::Name), accessible->text(QAccessible::Description),
                            accessible->text(QAccessible::Value), accessible->childCount()};
    };
    const QVariantList before = state();
    transport->setProperty("viewStartSeconds", 60.0);
    transport->setProperty("viewEndSeconds", 90.0);
    QVERIFY(band->isVisible());
    QCOMPARE(state(), before);
    QVERIFY(harness.root->property("seeks").value<QJSValue>().toVariant().toList().isEmpty());
    // Neither part of the band is an accessible object.
    for (const char *name : {"seekViewBand", "seekViewBandUnplayed", "seekViewBandPlayed"}) {
        QAccessibleInterface *part = QAccessible::queryAccessibleInterface(harness.item(QString::fromLatin1(name)));
        QVERIFY2(part == nullptr || part->role() == QAccessible::NoRole || part->state().invisible,
                 name);
    }
}

void TransportStatusTest::seekHoverDrawsALineAndANativeToolTip() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *hoverLine = harness.item(QStringLiteral("seekHoverLine"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(seek && hoverLine && tip);
    QVERIFY(tip->inherits("QQuickToolTip"));
    QVERIFY(!hoverLine->isVisible());
    QVERIFY(!tip->property("visible").toBool());

    // Over the bar, at 62% of its width. The time waits a moment, so a
    // pointer crossing the bar shows none, and its line comes with it.
    const QRectF bar = sceneRect(seek);
    const QPointF at(bar.left() + std::round(bar.width() * 0.62), bar.center().y());
    QElapsedTimer waited;
    QTest::mouseMove(harness.window, at.toPoint());
    waited.start();
    QVERIFY(tip->property("shown").toBool());
    QVERIFY(!tip->property("visible").toBool());
    QVERIFY(!hoverLine->isVisible());
    QTRY_VERIFY(tip->property("visible").toBool());
    QVERIFY2(waited.elapsed() >= 70, qPrintable(QString::number(waited.elapsed())));
    QTRY_VERIFY(hoverLine->isVisible());
    QCOMPARE(sceneRect(hoverLine).left(), std::floor(at.x()));
    QCOMPARE(hoverLine->width(), 1.0);
    QCOMPARE(hoverLine->height(), 28.0);
    QVERIFY(sameColor(hoverLine->property("color").value<QColor>(), withAlpha(harness.token("text"), 0.7)));
    const double seconds = seek->property("hoverSecond").toInt();
    const QString expected = QStringLiteral("%1:%2")
                                 .arg(static_cast<int>(seconds) / 60)
                                 .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'));
    QCOMPARE(tip->property("text").toString(), expected);
    QVERIFY2(!expected.startsWith(QLatin1Char('0')), qPrintable(expected));
    // Above the bar, its bottom edge on the bar's top, centred on the line:
    // it covers none of the waveform it reads, and nothing under the bar
    // such as the queue's header.
    auto *tipBox = tip->property("background").value<QQuickItem *>();
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QVERIFY(tipBox != nullptr && waveform != nullptr);
    QTRY_VERIFY(tip->property("opened").toBool());
    const auto expectAbove = [&](const QRectF &tipRect) {
        QVERIFY2(std::abs(tipRect.bottom() - bar.top()) <= 0.5, qPrintable(describeRect(tipRect)));
        QVERIFY2(!tipRect.intersects(sceneRect(waveform)), qPrintable(describeRect(tipRect)));
        QVERIFY2(!tipRect.intersects(bar), qPrintable(describeRect(tipRect)));
    };
    QRectF tipRect = sceneRect(tipBox);
    expectAbove(tipRect);
    QVERIFY2(std::abs(tipRect.center().x() - at.x()) <= 1.0, qPrintable(describeRect(tipRect)));
    QVERIFY(tipRect.left() < at.x() && tipRect.right() > at.x());

    // Beside the playhead, still centred on the line, now that it covers
    // neither the playhead nor the waveform: nothing to dodge.
    QQuickItem *playhead = harness.item(QStringLiteral("seekPlayhead"));
    QVERIFY(playhead != nullptr);
    const QRectF head = sceneRect(playhead);
    const QPointF nearHead(std::round(head.right() + 4), bar.center().y());
    QTest::mouseMove(harness.window, nearHead.toPoint());
    QTRY_COMPARE(sceneRect(hoverLine).left(), std::floor(nearHead.x()));
    tipRect = sceneRect(tipBox);
    expectAbove(tipRect);
    QVERIFY2(std::abs(tipRect.center().x() - nearHead.x()) <= 1.0, qPrintable(describeRect(tipRect)));
    QVERIFY(!tipRect.intersects(head));

    // At either end of the bar, within it.
    const QPointF atStart(bar.left() + 3, bar.center().y());
    QTest::mouseMove(harness.window, atStart.toPoint());
    QTRY_COMPARE(sceneRect(hoverLine).left(), atStart.x());
    tipRect = sceneRect(tipBox);
    expectAbove(tipRect);
    QCOMPARE(tipRect.left(), bar.left());
    const QPointF atEnd(bar.right() - 2, bar.center().y());
    QTest::mouseMove(harness.window, atEnd.toPoint());
    QTRY_COMPARE(sceneRect(hoverLine).left(), atEnd.x());
    tipRect = sceneRect(tipBox);
    expectAbove(tipRect);
    QCOMPARE(tipRect.right(), bar.right());
    // Nothing else moves it: a little way in from the start, it is centred
    // on its line again (a menu bar's titles kept it right of them, up to
    // 350 px from its line).
    const QPointF nearStart(bar.left() + tipRect.width() / 2 + 20, bar.center().y());
    QTest::mouseMove(harness.window, nearStart.toPoint());
    QTRY_COMPARE(sceneRect(hoverLine).left(), std::floor(nearStart.x()));
    tipRect = sceneRect(tipBox);
    expectAbove(tipRect);
    QVERIFY2(std::abs(tipRect.center().x() - nearStart.x()) <= 1.0, qPrintable(describeRect(tipRect)));

    // Away from the bar, both go.
    QTest::mouseMove(harness.window, QPoint(static_cast<int>(at.x()), static_cast<int>(bar.bottom() + 60)));
    QTRY_VERIFY(!hoverLine->isVisible());
    QTRY_VERIFY(!tip->property("visible").toBool());
    QTRY_VERIFY(!tipBox->isVisible());

    // A press on the bar shows the playhead's time at once, with no wait
    // and no hover line, centred on the playhead above the bar.
    QTest::mousePress(harness.window, Qt::LeftButton, Qt::NoModifier, at.toPoint());
    QVERIFY(seek->property("adjusting").toBool());
    QVERIFY(tip->property("visible").toBool());
    QTRY_VERIFY(tip->property("opened").toBool());
    QVERIFY(!hoverLine->isVisible());
    QQuickItem *playheadLine = harness.item(QStringLiteral("seekPlayheadLine"));
    QVERIFY(playheadLine != nullptr);
    tipRect = sceneRect(tipBox);
    expectAbove(tipRect);
    QVERIFY2(std::abs(tipRect.center().x() - sceneRect(playheadLine).center().x()) <= 1.0,
             qPrintable(describeRect(tipRect)));
    QTest::mouseRelease(harness.window, Qt::LeftButton, Qt::NoModifier, at.toPoint());
}

void TransportStatusTest::seekReleaseHandsTheTimeBoxToTheHover_data() {
    QTest::addColumn<bool>("hoveredFirst");
    QTest::addColumn<bool>("frameBeforeRelease");
    QTest::newRow("hovered, then dragged") << true << true;
    QTest::newRow("pressed as it arrived") << false << true;
    QTest::newRow("released within a frame") << true << false;
}

// A drag's release over the bar hands the time box to the hover as one box:
// it is never hidden between them, stays whole in every frame drawn, and is
// at the released point with its time from the release on (it went out for a
// frame and faded in again at each release).
void TransportStatusTest::seekReleaseHandsTheTimeBoxToTheHover() {
    QFETCH(bool, hoveredFirst);
    QFETCH(bool, frameBeforeRelease);
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(seek && tip);
    auto *tipBox = tip->property("background").value<QQuickItem *>();
    QVERIFY(tipBox != nullptr);
    const QRectF bar = sceneRect(seek);
    const int y = qRound(bar.center().y());
    const auto at = [&bar, y](double fraction) { return QPoint(qRound(bar.left() + fraction * bar.width()), y); };
    if (hoveredFirst) {
        QTest::mouseMove(harness.window, at(0.3));
    }
    QTest::mousePress(harness.window, Qt::LeftButton, Qt::NoModifier, at(0.3));
    QTest::mouseMove(harness.window, at(0.38));
    QTest::mouseMove(harness.window, at(0.46));
    QTRY_VERIFY(tip->property("opened").toBool() && tip->property("opacity").toReal() >= 1.0);
    if (frameBeforeRelease) {
        QVERIFY(!harness.window->grabWindow().isNull());
    }

    QSignalSpy shownChanges(tip, SIGNAL(shownChanged()));
    QSignalSpy visibleChanges(tip, SIGNAL(visibleChanged()));
    qreal faintest = 1.0;
    const FrameSampler sampler(harness.window, [&]() {
        faintest = std::min(faintest, tip->property("visible").toBool() ? tip->property("opacity").toReal() : 0.0);
    });
    QTest::mouseRelease(harness.window, Qt::LeftButton, Qt::NoModifier, at(0.46));
    QCOMPARE(seeksOf(harness).size(), 1);
    QCOMPARE(shownChanges.count(), 0);
    QCOMPARE(visibleChanges.count(), 0);
    QCOMPARE(tip->property("opacity").toReal(), 1.0);
    // At the released point at once, with its time, before any frame.
    QVERIFY2(std::abs(sceneRect(tipBox).center().x() - at(0.46).x()) <= 2.0,
             qPrintable(describeRect(sceneRect(tipBox))));
    const int second = int(std::floor(seeksOf(harness).first().toDouble()));
    QCOMPARE(tip->property("text").toString(),
             QStringLiteral("%1:%2").arg(second / 60).arg(second % 60, 2, 10, QLatin1Char('0')));
    // Through the frames after it, in which the hover catches up.
    for (int frame = 0; frame < 4; ++frame) {
        QVERIFY(!harness.window->grabWindow().isNull());
        QTest::qWait(20);
    }
    QCOMPARE(shownChanges.count(), 0);
    QCOMPARE(visibleChanges.count(), 0);
    QCOMPARE(faintest, 1.0);
    QVERIFY(std::abs(sceneRect(tipBox).center().x() - at(0.46).x()) <= 2.0);
    // The hover's from there: it follows the pointer, and leaves with it.
    QTest::mouseMove(harness.window, at(0.6));
    QTRY_VERIFY(std::abs(sceneRect(tipBox).center().x() - at(0.6).x()) <= 2.0);
    QCOMPARE(shownChanges.count(), 0);
    QTest::mouseMove(harness.window, QPoint(at(0.6).x(), qRound(bar.bottom() + 60)));
    QTRY_VERIFY(!tip->property("visible").toBool());
}

// A drag released off the bar shows no hover's time: the box leaves where
// the drag had it, rather than from the point the hover last had.
void TransportStatusTest::seekReleaseOffTheBarHidesTheTimeBox() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(seek && tip);
    auto *tipBox = tip->property("background").value<QQuickItem *>();
    QVERIFY(tipBox != nullptr);
    const QRectF bar = sceneRect(seek);
    const QPoint start(qRound(bar.left() + 0.3 * bar.width()), qRound(bar.center().y()));
    const QPoint end(qRound(bar.left() + 0.5 * bar.width()), qRound(bar.bottom() + 60));
    QTest::mouseMove(harness.window, start);
    QTest::mousePress(harness.window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(harness.window, QPoint(end.x() - 20, start.y()));
    QTest::mouseMove(harness.window, end);
    QTRY_VERIFY(tip->property("opened").toBool());
    const qreal draggedTo = sceneRect(tipBox).center().x();
    QTest::mouseRelease(harness.window, Qt::LeftButton, Qt::NoModifier, end);
    QVERIFY(!tip->property("shown").toBool());
    QVERIFY(std::abs(sceneRect(tipBox).center().x() - draggedTo) <= 1.0);
    QTRY_VERIFY(!tip->property("visible").toBool());
}

void TransportStatusTest::seekHoverLineLeavesThePlayheadWhole_data() {
    addThemeRows();
}

// The hover's line never paints over the playhead or stands beside it as a
// second one: it is under it, and shows only with a gap as wide as the
// playhead's line between them. The time stays.
void TransportStatusTest::seekHoverLineLeavesThePlayheadWhole() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    harness.root->setProperty("positionSeconds", 100.0);
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *hoverLine = harness.item(QStringLiteral("seekHoverLine"));
    QQuickItem *playhead = harness.item(QStringLiteral("seekPlayhead"));
    QQuickItem *line = harness.item(QStringLiteral("seekPlayheadLine"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(seek && hoverLine && playhead && line && tip);
    const QList<QQuickItem *> layers = playhead->parentItem()->childItems();
    QVERIFY(layers.indexOf(hoverLine) < layers.indexOf(playhead));

    const QRectF bar = sceneRect(seek);
    const int y = qRound(bar.center().y());
    QTest::mouseMove(harness.window, QPoint(qRound(sceneRect(playhead).right() + 30), y));
    QTRY_VERIFY(hoverLine->isVisible());
    QTRY_VERIFY(tip->property("opened").toBool());

    const QRectF head = sceneRect(playhead);
    const qreal clearance = sceneRect(line).width();
    const qreal ratio = harness.window->effectiveDevicePixelRatio();
    for (int x = qFloor(head.left()) - 6; x <= qCeil(head.right()) + 6; ++x) {
        QTest::mouseMove(harness.window, QPoint(x, y));
        // On the device pixel under the pointer.
        const qreal at = bar.left() + std::floor((x - bar.left()) * ratio + 1e-6) / ratio;
        QTRY_VERIFY(std::abs(sceneRect(hoverLine).left() - at) < 1e-6);
        const bool clear = at + hoverLine->width() <= head.left() - clearance + 1e-6
            || at >= head.right() + clearance - 1e-6;
        QVERIFY2(hoverLine->isVisible() == clear, qPrintable(QStringLiteral("at %1").arg(x)));
        QVERIFY(tip->property("visible").toBool());
    }

    // As drawn with the pointer on it: amber, with the bar beside it.
    QTest::mouseMove(harness.window, sceneRect(line).center().toPoint());
    QTRY_VERIFY(!hoverLine->isVisible());
    qreal scale = 1;
    const QImage image = grabbed(harness.window, &scale);
    QVERIFY(!image.isNull());
    QVERIFY(channelDistance(pixelAt(image, QPointF(sceneRect(line).left() + 0.5, y), scale),
                            harness.token("now")) <= 1);

    // It comes back as playback takes the playhead on from under it, and
    // goes as the playhead reaches it, as after a click there.
    harness.root->setProperty("positionSeconds", 110.0);
    QTRY_VERIFY(hoverLine->isVisible());
    harness.root->setProperty("positionSeconds", 100.0);
    QTRY_VERIFY(!hoverLine->isVisible());
}

void TransportStatusTest::seekHoverLineWaitsForThePointerToMoveAfterASeek_data() {
    QTest::addColumn<bool>("dragged");
    QTest::newRow("click") << false;
    QTest::newRow("drag") << true;
}

// A click leaves the pointer on the playhead, and as playback took that on
// the hover's line came in beside it, a second playhead, with no pointer
// moved. It waits for the pointer to move a pixel; the time stays.
void TransportStatusTest::seekHoverLineWaitsForThePointerToMoveAfterASeek() {
    QFETCH(bool, dragged);
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *hoverLine = harness.item(QStringLiteral("seekHoverLine"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("seekTimeBox"));
    QVERIFY(seek && hoverLine && tip);
    const QRectF bar = sceneRect(seek);
    const QPoint from(qRound(bar.left() + 0.3 * bar.width()), qRound(bar.center().y()));
    const QPoint to = dragged ? from + QPoint(qRound(0.16 * bar.width()), 0) : from;

    QTest::mouseMove(harness.window, from);
    QTest::mousePress(harness.window, Qt::LeftButton, Qt::NoModifier, from);
    if (dragged) {
        QTest::mouseMove(harness.window, (from + to) / 2);
        QTest::mouseMove(harness.window, to);
    }
    QTRY_VERIFY(tip->property("opened").toBool());
    QTest::mouseRelease(harness.window, Qt::LeftButton, Qt::NoModifier, to);
    QCOMPARE(seeksOf(harness).size(), 1);
    const double landed = seeksOf(harness).first().toDouble();

    bool lineShown = false;
    bool boxHidden = false;
    const FrameSampler sampler(harness.window, [&]() {
        lineShown = lineShown || hoverLine->isVisible();
        boxHidden = boxHidden || !tip->property("visible").toBool();
    });
    // Playback takes the playhead on from under the pointer, which rests.
    for (int second = 0; second <= 8; ++second) {
        harness.root->setProperty("positionSeconds", landed + second);
        QVERIFY(!harness.window->grabWindow().isNull());
        QTest::qWait(20);
        QVERIFY2(!hoverLine->isVisible(), qPrintable(QStringLiteral("%1 s after").arg(second)));
    }
    QVERIFY(!lineShown);
    QVERIFY(!boxHidden);

    // A pixel's move brings it, at the pointer.
    QTest::mouseMove(harness.window, to - QPoint(1, 0));
    QTRY_VERIFY(hoverLine->isVisible());
    QVERIFY(std::abs(sceneRect(hoverLine).left() - (to.x() - 1)) <= 1.0);
    // And back where it was, it is an ordinary hover's.
    QTest::mouseMove(harness.window, to);
    QTRY_VERIFY(std::abs(sceneRect(hoverLine).left() - to.x()) <= 1.0);
    QVERIFY(hoverLine->isVisible());
}

void TransportStatusTest::seekFocusDrawsOneRingAroundTheBar_data() {
    addThemeRows();
}

void TransportStatusTest::seekFocusDrawsOneRingAroundTheBar() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
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
    QVERIFY2(contrastRatio(ringColor, harness.transportChrome()) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("focus ring"), ringColor, harness.transportChrome())));
    // Left and Right seek by 5 s.
    QTest::keyClick(harness.window, Qt::Key_Right);
    const QVariantList seeks = seeksOf(harness);
    QCOMPARE(seeks.size(), 1);
    QCOMPARE(seeks.first().toDouble(), 82.0);
}

void TransportStatusTest::volumeGlyphHasTheTransportsWeight_data() {
    addThemeRows();
}

// The volume glyph is drawn at the weight of Next beside it (Breeze's
// speaker, with 1 px waves, read lighter than Play and Next), in both of
// its looks: at least 80 % of Next's ink, where Breeze's speaker had 63 %,
// and in Next's box.
void TransportStatusTest::volumeGlyphHasTheTransportsWeight() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QQuickItem *next = harness.item(QStringLiteral("transportButton_next"));
    QQuickItem *muteGlyph = harness.item(QStringLiteral("muteButtonGlyph"));
    QQuickItem *nextGlyph = glyphOf(next);
    QVERIFY(nextGlyph && muteGlyph);
    QCOMPARE(muteGlyph->size(), nextGlyph->size());
    const auto ink = [&](QQuickItem *glyph) { return inkOf(harness.window, glyph, harness.transportChrome()).pixels; };
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

void TransportStatusTest::volumeGlyphsAreSmoothOutlines_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<double>("ink");
    // The ink of each, in pixels of its 16 px drawing, as heavy as Next.
    QTest::newRow("volume") << QStringLiteral("volume.svg") << 96.0;
    QTest::newRow("muted") << QStringLiteral("volume-muted.svg") << 80.0;
}

// The speaker and the muted speaker are outlines: at 16 px their straight
// strokes are whole pixels and as heavy as before, and at any larger size,
// a 24 pt font's 38 px or 2x, their diagonals and curves are smooth (a grid
// of squares was a staircase of 2 to 4 px steps beside the other glyphs).
void TransportStatusTest::volumeGlyphsAreSmoothOutlines() {
    QFETCH(QString, name);
    QFETCH(double, ink);
    const auto coverage = [&name](int size) {
        QImageReader reader(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/assets/") + name);
        reader.setScaledSize(QSize(size, size));
        return reader.read().convertToFormat(QImage::Format_ARGB32);
    };
    const QImage one = coverage(16);
    QCOMPARE(one.size(), QSize(16, 16));
    const auto covered = [&one](int x, int y) { return qAlpha(one.pixel(x, y)) / 255.0; };
    double total = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            total += covered(x, y);
        }
    }
    QVERIFY2(std::abs(total - ink) <= 0.1 * ink, qPrintable(QStringLiteral("%1 px of ink").arg(total)));
    // Whole pixels: the cone's back and the middle of each wave, with a
    // clear column between the parts.
    for (int y = 3; y <= 12; ++y) {
        QVERIFY2(covered(7, y) >= 0.95 && covered(8, y) <= 0.05, qPrintable(QStringLiteral("back at %1").arg(y)));
    }
    if (name == QStringLiteral("volume.svg")) {
        for (const int y : {7, 8}) {
            for (const int x : {10, 11, 14, 15}) {
                QVERIFY2(covered(x, y) >= 0.85, qPrintable(QStringLiteral("wave at %1,%2").arg(x).arg(y)));
            }
            QVERIFY2(covered(9, y) <= 0.1 && covered(12, y) <= 0.1, qPrintable(QStringLiteral("gap at %1").arg(y)));
        }
    }

    for (const int size : {32, 38, 64}) {
        const QImage large = coverage(size);
        QCOMPARE(large.size(), QSize(size, size));
        const double scale = size / 16.0;
        // Along a diagonal, the cone's and the cross's, each row of pixels
        // holds its own ink; a staircase repeats a row for each step.
        const auto rowInk = [&](int y, int left, int right) {
            double sum = 0;
            for (int x = left; x < right; ++x) {
                sum += qAlpha(large.pixel(x, y)) / 255.0;
            }
            return sum;
        };
        for (int y = int(std::ceil(2.4 * scale)); y + 1 < int(std::floor(4.6 * scale)); ++y) {
            QVERIFY2(rowInk(y + 1, 0, size / 2) - rowInk(y, 0, size / 2) >= 0.5,
                     qPrintable(QStringLiteral("the cone steps at row %1 of %2 px").arg(y).arg(size)));
        }
        // The wave's arc, or the cross's stroke, from its upper end toward
        // the middle: its ink's middle moves in every row.
        const bool waves = name == QStringLiteral("volume.svg");
        const int left = int(std::ceil((waves ? 10.9 : 13.3) * scale));
        const auto middle = [&](int y) {
            double sum = 0;
            double moment = 0;
            for (int x = left; x < size; ++x) {
                const double alpha = qAlpha(large.pixel(x, y)) / 255.0;
                sum += alpha;
                moment += alpha * x;
            }
            return sum > 0 ? moment / sum : 0.0;
        };
        const double from = (waves ? 3.4 : 6.2) * scale;
        const double to = (waves ? 4.8 : 7.2) * scale;
        for (int y = int(std::ceil(from)); y + 1 < int(std::floor(to)); ++y) {
            QVERIFY2(std::abs(middle(y + 1) - middle(y)) >= 0.1,
                     qPrintable(QStringLiteral("the stroke steps at row %1 of %2 px").arg(y).arg(size)));
        }
    }
}

void TransportStatusTest::transportGlyphsDrawOnOneScale_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("small");
    QTest::addColumn<int>("large");
    QTest::newRow("10 pt") << 10.0 << 16 << 20;
    QTest::newRow("24 pt") << 24.0 << 38 << 48;
}

// The transport's glyphs on one scale (Play/Pause 22, Previous and Next 16
// and Stop 14 read as three weights): Previous, Stop, Next and the volume
// 16 px, their ink 12 px tall, and Play/Pause a step above at 20 px, its ink
// 14 px tall; Stop's and Pause's ink at most 1.4 times Next's. They grow
// with the font on whole pixels, to at least 0.9 of its pixel size at 24 pt,
// and are drawn at that size (Kirigami rounded a 24 pt font's 38 px glyphs
// down to 32 px theme icons, and would draw a 20 px one at 16).
void TransportStatusTest::transportGlyphsDrawOnOneScale() {
    QFETCH(qreal, pointSize);
    QFETCH(int, small);
    QFETCH(int, large);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QCOMPARE(harness.tokens->property("basePointSize").toReal(), pointSize);
    const auto glyphOfButton = [&harness](const char *name) {
        return glyphOf(harness.item(QStringLiteral("transportButton_") + QLatin1String(name)));
    };
    struct Glyph {
        const char *name;
        QQuickItem *item;
        int side;
        int inkHeight;
    };
    // Playing: Pause shows.
    QQuickItem *pause = harness.item(QStringLiteral("transportPlayPauseGlyph_pause"));
    SteppedAnimationDriver animations;
    QVERIFY(pause != nullptr && stepUntil(animations, [&] { return pause->opacity() == 1.0; }));
    const QList<Glyph> glyphs{
        {"previous", glyphOfButton("previous"), small, small * 12 / 16},
        {"pause", pause, large, large * 14 / 20},
        {"stop", glyphOfButton("stop"), small, small * 12 / 16},
        {"next", glyphOfButton("next"), small, small * 12 / 16},
        {"volume", harness.item(QStringLiteral("muteButtonGlyph")), small, small * 12 / 16},
    };
    const int fontPixels = QFontInfo(QGuiApplication::font()).pixelSize();
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

    // As drawn.
    const QColor chrome = harness.transportChrome();
    QHash<QString, Ink> inks;
    for (const Glyph &glyph : glyphs) {
        Ink ink;
        QTRY_VERIFY((ink = inkOf(harness.window, glyph.item, chrome)).pixels > 0);
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
    QVERIFY(stepUntil(animations, [&] { return play->opacity() == 1.0 && !pause->isVisible(); }));
    Ink playInk;
    QTRY_VERIFY((playInk = inkOf(harness.window, play, chrome)).pixels > 0);
    // Give or take the triangle's thin tips, at most a pixel at each end.
    QVERIFY2(std::abs(playInk.height - inks.value(QStringLiteral("pause")).height) <= 2,
             qPrintable(QStringLiteral("Play %1 px, Pause %2 px").arg(playInk.height)
                            .arg(inks.value(QStringLiteral("pause")).height)));
}

void TransportStatusTest::transportButtonsDrawTheAppsFocusRing_data() {
    addThemeRows();
}

// The transport's buttons draw their own background: the hover tint, and
// for keyboard focus the seek bar's and the volume's 2 px ring, 3 px out,
// which meets 3:1 on the chrome in both themes. The style's frame, 2.18:1
// on the light chrome, is not drawn: the background is the app's.
void TransportStatusTest::transportButtonsDrawTheAppsFocusRing() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    const QColor chrome = harness.transportChrome();
    SteppedAnimationDriver animations;
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
        QVERIFY2(stepUntil(animations, [&] { return hover->isVisible() && hover->opacity() == 1.0; }), qPrintable(name));
        QVERIFY(sameColor(hover->property("color").value<QColor>(),
                          harness.item(QStringLiteral("transportBar"))->property("hoverTint").value<QColor>()));
        QVERIFY(!ring->isVisible());
        QTest::mouseMove(harness.window, QPoint(10, harness.window->height() - 10));
        QVERIFY2(stepUntil(animations, [&] { return !hover->isVisible(); }), qPrintable(name));

        // Keyboard focus: one 2 px ring, 3 px out from the button.
        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY2(ring->isVisible(), qPrintable(name));
        QCOMPARE(evaluateOn(ring, QStringLiteral("border.width")).toInt(), 2);
        QCOMPARE(sceneRect(ring), sceneRect(button).adjusted(-3, -3, 3, 3));
        const QColor ringColor = evaluateOn(ring, QStringLiteral("border.color")).value<QColor>();
        QVERIFY(sameColor(ringColor, harness.token("focusRing")));
        QVERIFY2(contrastRatio(ringColor, chrome) >= 3.0,
                 qPrintable(describeContrast(name, ringColor, chrome)));
        harness.item(QStringLiteral("seekSlider"))->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY2(!ring->isVisible(), qPrintable(name));
    }
    // The mute glyph, which the style's background drew before, is drawn,
    // at Next's size.
    QQuickItem *muteGlyph = harness.item(QStringLiteral("muteButtonGlyph"));
    QVERIFY(muteGlyph != nullptr && muteGlyph->isVisible());
    QVERIFY(sameColor(muteGlyph->property("color").value<QColor>(), harness.token("text")));
    QCOMPARE(muteGlyph->width(), glyphOf(harness.item(QStringLiteral("transportButton_next")))->width());
}

void TransportStatusTest::transportPressedReadsApartFromHover_data() {
    addThemeRows();
}

// A pressed transport button reads apart from a hovered one (the press,
// text 10%, was the hover's tint in the dark and lighter than it in the
// light): the pressed tint a clear step past the hover's, as drawn on the
// transport's Header chrome, each as strong on it as the window's on the
// view: 1.3:1 and 1.65:1, 9% / 15% and 16% / 26% text in Breeze. The status
// bar's buttons, on the window colour, take the window's tints.
void TransportStatusTest::transportPressedReadsApartFromHover() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    const QColor chrome = harness.transportChrome();
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QVERIFY(transport != nullptr);
    const QColor hover = transport->property("hoverTint").value<QColor>();
    const QColor pressed = transport->property("pressedTint").value<QColor>();
    QCOMPARE(std::lround(pressed.alphaF() * 100), dark ? 16L : 26L);
    QCOMPARE(std::lround(hover.alphaF() * 100), dark ? 9L : 15L);
    QVERIFY(sameColor(QColor::fromRgbF(pressed.redF(), pressed.greenF(), pressed.blueF()), harness.token("text")));
    QVERIFY(sameColor(QColor::fromRgbF(hover.redF(), hover.greenF(), hover.blueF()), harness.token("text")));
    const QColor hoverDrawn = drawnOver(hover, chrome);
    const QColor pressedDrawn = drawnOver(pressed, chrome);
    QVERIFY2(contrastRatio(hoverDrawn, chrome) >= 1.3,
             qPrintable(describeContrast(QStringLiteral("hover on the chrome"), hoverDrawn, chrome)));
    QVERIFY2(contrastRatio(pressedDrawn, chrome) >= 1.65,
             qPrintable(describeContrast(QStringLiteral("pressed on the chrome"), pressedDrawn, chrome)));
    QVERIFY2(contrastRatio(pressedDrawn, hoverDrawn) >= 1.2,
             qPrintable(describeContrast(QStringLiteral("pressed on hover"), pressedDrawn, hoverDrawn)));

    QQuickItem *button = harness.item(QStringLiteral("transportButton_previous"));
    QVERIFY(button != nullptr);
    button->setProperty("hoverEnabled", true);
    auto *background = button->property("background").value<QQuickItem *>();
    QVERIFY(background != nullptr);
    auto *tint = background->findChild<QQuickItem *>(QStringLiteral("transportButtonHover"));
    QVERIFY(tint != nullptr);
    SteppedAnimationDriver animations;
    // From elsewhere: a move to where the pointer last was is no move.
    QTest::mouseMove(harness.window, QPoint(10, harness.window->height() - 10));
    QTest::mouseMove(harness.window, sceneCentre(button));
    QVERIFY(stepUntil(animations, [&] { return tint->isVisible() && tint->opacity() == 1.0; }));
    QVERIFY(sameColor(tint->property("color").value<QColor>(), hover));
    qreal scale = 1;
    const QPointF corner = sceneRect(button).topLeft() + QPointF(4.5, 4.5);
    QVERIFY(channelDistance(pixelAt(grabbed(harness.window, &scale), corner, scale), hoverDrawn) <= 2);
    QTest::mousePress(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(button));
    QVERIFY(button->property("down").toBool());
    QVERIFY(sameColor(tint->property("color").value<QColor>(), pressed));
    QVERIFY(channelDistance(pixelAt(grabbed(harness.window, &scale), corner, scale), pressedDrawn) <= 2);
    QTest::mouseRelease(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(button));

    // The status bar's issues button and Last.fm slot take the window's.
    const QColor windowPressed = harness.token("pressed");
    QCOMPARE(std::lround(windowPressed.alphaF() * 100), dark ? 17L : 25L);
    QVERIFY(harness.reportIssue(1, 0, QStringLiteral("first")));
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    for (const auto &[name, backgroundName] :
         {std::pair{QStringLiteral("statusBarErrorsButton"), QStringLiteral("statusBarErrorsBackground")},
          std::pair{QStringLiteral("scrobbleIndicator"), QStringLiteral("scrobbleIndicatorBackground")}}) {
        QQuickItem *statusButton = harness.item(name);
        QQuickItem *statusBackground = harness.item(backgroundName);
        QVERIFY2(statusButton && statusBackground, qPrintable(name));
        QVERIFY2(stepUntil(animations, [&] { return statusButton->isVisible(); }), qPrintable(name));
        // No press of the issues button, which would open them.
        statusButton->setProperty("down", true);
        QVERIFY2(stepUntil(animations,
                           [&] { return sameColor(statusBackground->property("color").value<QColor>(), windowPressed); }),
                 qPrintable(name));
        // An invalid value resets it, to follow the press again.
        statusButton->setProperty("down", QVariant());
    }
}

// The seek bar's tooltip comes in over the window's short duration, as its
// other hovers do, not the style's long one (200 ms against 100 ms
// elsewhere); it goes as the style's tooltips do.
void TransportStatusTest::seekToolTipFadesInAtTheShortDuration() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
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
    QVERIFY2(harness.load(), qPrintable(harness.error));
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
    QCOMPARE(time->property("text").toString(), QStringLiteral("–:–– / –:––"));
    QVERIFY(sameColor(position->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(!seek->isEnabled());
}

void TransportStatusTest::seekStoppedWithATrackKeepsItsLengthAndWaveform_data() {
    addThemeRows();
}

// Stopped with a track loaded is not the empty transport (it was drawn as
// one): the track's length stays, in the secondary colour, and its waveform
// in the unplayed tone, with no playhead and no amber. It does not seek, as
// before.
void TransportStatusTest::seekStoppedWithATrackKeepsItsLengthAndWaveform() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QObject *bridge = harness.bridge;
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/track.flac"));
    bridge->setProperty("queueLength", 1);
    bridge->setProperty("waveformPeaksPacked", fullScalePeaks());
    bridge->setProperty("waveformCoverageSeconds", 229.0);
    bridge->setProperty("waveformComplete", true);
    QQuickItem *time = harness.item(QStringLiteral("transportTime"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QVERIFY(time && seek && waveform && transport);
    QTRY_VERIFY(waveform->isVisible());
    const QRectF timeRect = sceneRect(time);

    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QCOMPARE(time->property("text").toString(), QStringLiteral("–:–– / 3:49"));
    for (const char *part : {"transportTimePosition", "transportTimeLength"}) {
        QVERIFY(sameColor(harness.item(QString::fromLatin1(part))->property("color").value<QColor>(),
                          harness.token("textSecondary")));
    }
    QCOMPARE(sceneRect(time), timeRect);
    QVERIFY(waveform->isVisible());
    QVERIFY(!harness.item(QStringLiteral("seekPlayhead"))->isVisible());
    QVERIFY(!harness.item(QStringLiteral("seekEmptyLine"))->isVisible());
    QVERIFY(!seek->isEnabled());

    // As drawn: the unplayed tone from the bar's first pixel to its last,
    // and nowhere the amber.
    const QColor chrome = harness.transportChrome();
    const QColor unplayed = mixed(chrome, transport->property("seekPlayedColor").value<QColor>(),
                                  transport->property("seekUnplayedOpacity").toDouble());
    const QRectF bar = sceneRect(seek);
    qreal scale = 1;
    const auto allUnplayed = [&]() {
        const QImage image = grabbed(harness.window, &scale);
        for (qreal x = bar.left() + 0.5; x < std::floor(bar.right()); x += 1.0) {
            const QColor pixel = pixelAt(image, QPointF(x, std::floor(bar.center().y()) + 0.5), scale);
            if (channelDistance(pixel, unplayed) > 3) {
                return false;
            }
        }
        return true;
    };
    QTRY_VERIFY(allUnplayed());

    // Stopped with no length of its own yet, as after a start: the queue's
    // length for it, and dashes when that has none either.
    bridge->setProperty("durationSeconds", 0.0);
    bridge->setProperty("durationText", QStringLiteral("00:00"));
    bridge->setProperty("playingQueueLengthText", QStringLiteral("4:05"));
    QCOMPARE(time->property("text").toString(), QStringLiteral("–:–– / 4:05"));
    bridge->setProperty("playingQueueLengthText", QString());
    QCOMPARE(time->property("text").toString(), QStringLiteral("–:–– / –:––"));

    // No track, or an empty queue: the hairline and dashes.
    bridge->setProperty("durationSeconds", 229.0);
    bridge->setProperty("durationText", QStringLiteral("03:49"));
    bridge->setProperty("playingQueueLengthText", QStringLiteral("3:49"));
    for (const bool noPath : {false, true}) {
        bridge->setProperty("queueLength", noPath ? 1 : 0);
        bridge->setProperty("currentTrackPath", noPath ? QString() : QStringLiteral("/music/track.flac"));
        QCOMPARE(time->property("text").toString(), QStringLiteral("–:–– / –:––"));
        QVERIFY(!waveform->isVisible());
        QVERIFY(harness.item(QStringLiteral("seekEmptyLine"))->isVisible());
        QVERIFY(!harness.item(QStringLiteral("seekUnplayed"))->isVisible());
    }
}

// A track made current while stopped has no waveform held: the bridge keeps
// the peaks of the track before until the new one's come, with no coverage.
void TransportStatusTest::seekStoppedShowsNoOtherTracksWaveform() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QObject *bridge = harness.bridge;
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/first.flac"));
    bridge->setProperty("queueLength", 2);
    bridge->setProperty("waveformPeaksPacked", fullScalePeaks());
    bridge->setProperty("waveformCoverageSeconds", 137.0);
    bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QQuickItem *emptyLine = harness.item(QStringLiteral("seekEmptyLine"));
    QVERIFY(waveform && emptyLine);
    // What was decoded of a track stopped early is held.
    QVERIFY(waveform->isVisible());
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/second.flac"));
    bridge->setProperty("waveformCoverageSeconds", 0.0);
    bridge->setProperty("durationSeconds", 0.0);
    QVERIFY(!waveform->isVisible());
    QVERIFY(emptyLine->isVisible());
}

// Gapless and Repeat One never stop, so they never take the stopped look:
// the playhead and the position stay, also for the moment without a length
// and peaks.
void TransportStatusTest::transportStaysTheTrackThroughATrackChange() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QObject *bridge = harness.bridge;
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/first.flac"));
    bridge->setProperty("queueLength", 2);
    QQuickItem *time = harness.item(QStringLiteral("transportTime"));
    QQuickItem *playhead = harness.item(QStringLiteral("seekPlayhead"));
    QVERIFY(time && playhead);
    // The next track, then the same again as Repeat One plays it.
    for (int change = 0; change < 2; ++change) {
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/second.flac"));
        bridge->setProperty("durationSeconds", 0.0);
        bridge->setProperty("waveformCoverageSeconds", 0.0);
        bridge->setProperty("playingQueueLengthText", QStringLiteral("4:05"));
        harness.root->setProperty("positionText", QStringLiteral("00:00"));
        QVERIFY(playhead->isVisible());
        QCOMPARE(time->property("text").toString(), QStringLiteral("0:00 / –:––"));
        QVERIFY(sameColor(harness.item(QStringLiteral("transportTimePosition"))->property("color").value<QColor>(),
                          harness.token("text")));
        bridge->setProperty("durationSeconds", 245.0);
        bridge->setProperty("durationText", QStringLiteral("04:05"));
        QCOMPARE(time->property("text").toString(), QStringLiteral("0:00 / 4:05"));
    }
}

void TransportStatusTest::seekEmptyLineRunsPastAnEarlyEndingWaveform_data() {
    addThemeRows();
}

// A waveform that ends before the track does, as a file that ends early or
// one not yet analysed to the end, leaves the rest of the bar its empty
// line, 1 px through the middle as without a track, to the bar's last
// pixel (nothing was drawn past the waveform's end). So does a track playing
// before its first peaks.
void TransportStatusTest::seekEmptyLineRunsPastAnEarlyEndingWaveform() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *emptyLine = harness.item(QStringLiteral("seekEmptyLine"));
    QVERIFY(waveform && seek && emptyLine);
    QCOMPARE(waveform->property("emptyLineColor").value<QColor>(),
             harness.item(QStringLiteral("transportBar"))->property("seekEmptyLineColor").value<QColor>());
    harness.root->setProperty("positionSeconds", 0.0);
    const QColor chrome = harness.transportChrome();
    const QRectF bar = sceneRect(seek);
    const QRectF wave = sceneRect(waveform);
    QCOMPARE(wave.left(), bar.left());
    QCOMPARE(wave.right(), bar.right());
    // The line's row, where the line drawn without a track is.
    const qreal lineY = std::round(bar.center().y() - 0.5) + 0.5;

    // The pixels of the line's row and the rows either side of it, from
    // `from` to the bar's end: each in the row must be `colour` (the first
    // found when it is invalid), and the rows either side the chrome.
    qreal scale = 1;
    const auto lineRunsTo = [&](qreal from, QColor *colour, QString *why) {
        const QImage image = grabbed(harness.window, &scale);
        for (qreal x = from + 0.5; x < bar.right(); x += 1.0) {
            const QColor pixel = pixelAt(image, QPointF(x, lineY), scale);
            if (!colour->isValid()) {
                *colour = pixel;
            }
            if (channelDistance(pixel, *colour) > 2 || channelDistance(pixel, chrome) < 8) {
                *why = QStringLiteral("%1 at %2").arg(pixel.name()).arg(x);
                return false;
            }
            for (const qreal y : {lineY - 1, lineY + 1}) {
                const QColor beside = pixelAt(image, QPointF(x, y), scale);
                if (channelDistance(beside, chrome) > 2) {
                    *why = QStringLiteral("%1 beside the line at %2,%3").arg(beside.name()).arg(x).arg(y);
                    return false;
                }
            }
        }
        return true;
    };

    // Quiet peaks analysed to 60 % of the track: the waveform's thinnest
    // bars to there, and the empty line from there to the end.
    harness.bridge->setProperty("waveformPeaksPacked", QByteArray(600, '\0'));
    harness.bridge->setProperty("waveformComplete", false);
    harness.bridge->setProperty("waveformCoverageSeconds", 229.0 * 0.6);
    QTRY_VERIFY(!emptyLine->isVisible());
    const qreal waveEnd = wave.left() + std::floor(0.6 * wave.width());
    QColor line;
    QString why;
    QTRY_VERIFY2(lineRunsTo(waveEnd, &line, &why), qPrintable(why));
    // Before its end, the waveform's 2 px bars, not the line.
    const QImage image = grabbed(harness.window, &scale);
    const QPointF inWave(waveEnd - 20.5, lineY - 1);
    QVERIFY2(channelDistance(pixelAt(image, inWave, scale), chrome) > 8, qPrintable(pixelAt(image, inWave, scale).name()));

    // Playing before the first peaks: the line across the whole bar, past
    // the playhead at its start.
    harness.bridge->setProperty("waveformPeaksPacked", QByteArray());
    harness.bridge->setProperty("waveformCoverageSeconds", 0.0);
    QColor wholeLine;
    QTRY_VERIFY2(lineRunsTo(bar.left() + 4, &wholeLine, &why), qPrintable(why));
    QVERIFY2(channelDistance(wholeLine, line) <= 2, qPrintable(wholeLine.name() + QLatin1Char(' ') + line.name()));
}

void TransportStatusTest::seekMarksWhereTheFileEnds_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("breeze dark") << true << 10.0;
    QTest::newRow("breeze light") << false << 10.0;
    QTest::newRow("breeze dark, 24 pt") << true << 24.0;
}

// A file known to end early has the status bar's warning sign on its
// waveform at the cut, under the playhead, and the seek bar's description
// says where it ends (the waveform only stopped, with nothing to tell why).
void TransportStatusTest::seekMarksWhereTheFileEnds() {
    QFETCH(bool, dark);
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QObject *bridge = harness.bridge;
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *mark = harness.item(QStringLiteral("seekEndsEarlyMark"));
    QVERIFY(seek && mark);
    bridge->setProperty("waveformPeaksPacked", fullScalePeaks());
    bridge->setProperty("waveformCoverageSeconds", 137.0);
    // A waveform still being analysed is no cut.
    QVERIFY(!mark->isVisible());
    bridge->setProperty("waveformEndsEarly", true);
    QTRY_VERIFY(mark->isVisible());
    const QRectF bar = sceneRect(seek);
    const QRectF markRect = sceneRect(mark);
    QVERIFY2(std::abs(markRect.center().x() - (bar.left() + 137.0 / 229.0 * std::floor(bar.width()))) <= 1.0,
             qPrintable(describeRect(markRect)));
    QVERIFY2(within(markRect, bar), qPrintable(describeRect(markRect)));
    QCOMPARE(markRect.width(), std::round(16.0 * pointSize / 10.0));
    // The gap is the waveform's height, with no slivers of it by the sign.
    const QRectF wave = sceneRect(harness.item(QStringLiteral("seekWaveform")));
    QCOMPARE(markRect.top(), wave.top());
    QCOMPARE(markRect.height(), wave.height());
    QCOMPARE(markRect.topLeft(), QPointF(std::round(markRect.left()), std::round(markRect.top())));
    QQuickItem *glyph = harness.item(QStringLiteral("seekEndsEarlyGlyph"));
    QVERIFY(glyph != nullptr);
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("status-warning.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("negative")));
    QVERIFY2(contrastRatio(harness.token("negative"), harness.transportChrome()) >= 3.0, "the mark on the chrome");
    // Under the playhead, over the waveform and its unplayed cover.
    QQuickItem *playhead = harness.item(QStringLiteral("seekPlayhead"));
    const QList<QQuickItem *> layers = playhead->parentItem()->childItems();
    QVERIFY(layers.indexOf(harness.item(QStringLiteral("seekUnplayed"))) < layers.indexOf(mark));
    QVERIFY(layers.indexOf(mark) < layers.indexOf(playhead));
    QCOMPARE(accessibleText(seek, QAccessible::Description), QStringLiteral("1:17 of 3:49. The file ends at 2:17."));

    // At the bar's very end it stays on the bar.
    bridge->setProperty("waveformCoverageSeconds", 228.9);
    QVERIFY(within(sceneRect(mark), bar));
    // Paused keeps it. The next track's, without a cut, has neither.
    bridge->setProperty("playbackState", QStringLiteral("Paused"));
    QVERIFY(mark->isVisible());
    bridge->setProperty("waveformEndsEarly", false);
    QVERIFY(!mark->isVisible());
    QTRY_COMPARE(accessibleText(seek, QAccessible::Description), QStringLiteral("1:17 of 3:49"));
    // Without a length or peaks the cut has no place on the bar.
    bridge->setProperty("waveformEndsEarly", true);
    bridge->setProperty("durationSeconds", 0.0);
    QVERIFY(!mark->isVisible());
    bridge->setProperty("durationSeconds", 229.0);
    bridge->setProperty("waveformPeaksPacked", QByteArray());
    QVERIFY(!mark->isVisible());
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
    QVERIFY2(harness.load(), qPrintable(harness.error));
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
    const QVariantList seeks = seeksOf(harness);
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
    QVERIFY2(harness.load(), qPrintable(harness.error));
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
    QVERIFY2(harness.load(), qPrintable(harness.error));
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
    QCOMPARE(time->property("text").toString(), QStringLiteral("–:–– / –:––"));

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
    QVERIFY2(harness.load(), qPrintable(harness.error));
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
    addThemeRows();
}

// The volume slider in the seek bar's language: a 3 px groove, filled up to
// the thumb in the seek bar's played colour and the rest in its unplayed
// one, opaque, and a solid 12 px thumb in the played colour, centred on the
// groove. Hover brightens the thumb to the text colour and shows
// "Volume 80%"; keyboard focus draws a 2 px ring around the thumb.
void TransportStatusTest::volumeDrawsGrooveHoverAndFocus() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *groove = harness.item(QStringLiteral("volumeGroove"));
    QQuickItem *fill = harness.item(QStringLiteral("volumeGrooveFill"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QQuickItem *ring = harness.item(QStringLiteral("volumeFocusRing"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("volumeToolTip"));
    QVERIFY(transport && volume && groove && fill && thumb && ring && tip);
    const QColor chrome = harness.transportChrome();
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
    QVERIFY2(contrastRatio(unplayed, chrome) >= 3.0, qPrintable(describeContrast(QStringLiteral("unfilled"), unplayed, chrome)));
    QVERIFY2(contrastRatio(played, chrome) > contrastRatio(unplayed, chrome),
             qPrintable(describeContrast(QStringLiteral("filled"), played, chrome)));
    qreal scale = 1;
    const QImage image = grabbed(harness.window, &scale);
    const QRectF thumbRect = sceneRect(thumb);
    QVERIFY(channelDistance(pixelAt(image, thumbRect.center(), scale), played) <= 2);
    QVERIFY(channelDistance(pixelAt(image, QPointF(sceneRect(groove).right() - 2, sceneRect(groove).center().y()), scale),
                            unplayed) <= 2);
    QVERIFY(channelDistance(pixelAt(image, QPointF(sceneRect(groove).left() + 2, sceneRect(groove).center().y()), scale),
                            played) <= 2);

    // Hover brightens the thumb and shows "Volume 80%".
    showToolTipsAtOnce(volume);
    SteppedAnimationDriver animations;
    QTest::mouseMove(harness.window, sceneCentre(volume));
    QVERIFY(volume->property("hovered").toBool());
    QVERIFY(stepUntil(animations, [&] { return sameColor(thumb->property("color").value<QColor>(), harness.token("text")); }));
    QCOMPARE(tip->property("text").toString(), QStringLiteral("Volume 80%"));
    QVERIFY(stepUntil(animations, [&] { return tip->property("visible").toBool(); }));
    QVERIFY(!ring->isVisible());

    // Away, the thumb is at rest again; keyboard focus draws a 2 px ring
    // around it and shows the tooltip too.
    QTest::mouseMove(harness.window, QPoint(10, harness.window->height() - 10));
    QVERIFY(!volume->property("hovered").toBool());
    QVERIFY(stepUntil(animations, [&] { return sameColor(thumb->property("color").value<QColor>(), restFill); }));
    volume->forceActiveFocus(Qt::TabFocusReason);
    QVERIFY(ring->isVisible());
    QCOMPARE(evaluateOn(ring, QStringLiteral("border.width")).toInt(), 2);
    QCOMPARE(sceneRect(ring), sceneRect(thumb).adjusted(-3, -3, 3, 3));
    QVERIFY(sameColor(evaluateOn(ring, QStringLiteral("border.color")).value<QColor>(), harness.token("focusRing")));
    QVERIFY2(contrastRatio(harness.token("focusRing"), chrome) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("volume focus"), harness.token("focusRing"), chrome)));
    QVERIFY(stepUntil(animations, [&] { return tip->property("visible").toBool(); }));
    // A key's step is the slider's tenth.
    QTest::keyClick(harness.window, Qt::Key_Left);
    QTRY_COMPARE(tip->property("text").toString(), QStringLiteral("Volume 70%"));
    QCOMPARE(harness.bridge->property("volume").toDouble(), 0.7);
}

void TransportStatusTest::volumeGrooveGrowsWithTheThumb_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("windowWidth");
    QTest::addColumn<qreal>("expectedWidth");
    QTest::addColumn<qreal>("thumbSize");
    QTest::newRow("10 pt, wide") << 10.0 << 1600 << 96.0 << 12.0;
    // A narrow window shortens the volume.
    QTest::newRow("10 pt, narrow") << 10.0 << 1000 << 72.0 << 12.0;
    QTest::newRow("12 pt, wide") << 12.0 << 1600 << 115.0 << 14.0;
    QTest::newRow("24 pt, wide") << 24.0 << 1600 << 144.0 << 29.0;
    QTest::newRow("24 pt, narrow") << 24.0 << 1000 << 108.0 << 29.0;
}

// A larger font widens the volume's groove with its thumb and the glyphs,
// up to half again: at 24 pt, 144 px and 108 px in a narrow window (the
// 29 px thumb crowded a 96 px groove). The default font keeps 96 px and
// 72 px. The groove ends on the content edge, and its background is as wide
// as the slider.
void TransportStatusTest::volumeGrooveGrowsWithTheThumb() {
    QFETCH(qreal, pointSize);
    QFETCH(int, windowWidth);
    QFETCH(qreal, expectedWidth);
    QFETCH(qreal, thumbSize);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(kBreezeDark, windowWidth), qPrintable(harness.error));
    QCOMPARE(harness.tokens->property("basePointSize").toReal(), pointSize);
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *groove = harness.item(QStringLiteral("volumeGroove"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QVERIFY(transport && volume && groove && thumb);
    QCOMPARE(volume->width(), expectedWidth);
    QCOMPARE(sceneRect(groove).width(), expectedWidth);
    QCOMPARE(thumb->width(), thumbSize);
    QCOMPARE(sceneRect(transport).right() - sceneRect(groove).right(), 12.0);
    auto *background = volume->property("background").value<QQuickItem *>();
    QVERIFY(background != nullptr);
    QCOMPARE(background->width(), expectedWidth);
}

void TransportStatusTest::muteButtonIsClearOfTheVolumeGroove_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// The mute button's hover fill and its focus ring are clear of the volume's
// groove, also of the thumb's ring at no volume (the fill butted against the
// groove, which was drawn through the ring).
void TransportStatusTest::muteButtonIsClearOfTheVolumeGroove() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *mute = harness.item(QStringLiteral("muteButton"));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *groove = harness.item(QStringLiteral("volumeGroove"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QVERIFY(mute && volume && groove && thumb);
    QQuickItem *ring = itemNamed(mute, QStringLiteral("transportButtonFocusRing"));
    QQuickItem *fill = itemNamed(mute, QStringLiteral("transportButtonHover"));
    QVERIFY(ring && fill);
    mute->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(ring->isVisible());
    QVERIFY2(sceneRect(groove).left() - sceneRect(ring).right() >= 3.0,
             qPrintable(describeRect(sceneRect(ring)) + QLatin1Char(' ') + describeRect(sceneRect(groove))));
    QVERIFY(sceneRect(groove).left() - sceneRect(fill).right() >= 6.0);
    harness.bridge->setProperty("volume", 0.0);
    volume->forceActiveFocus(Qt::TabFocusReason);
    QQuickItem *thumbRing = harness.item(QStringLiteral("volumeFocusRing"));
    QTRY_VERIFY(thumbRing->isVisible());
    QVERIFY(sceneRect(thumbRing).left() >= sceneRect(fill).right());
}

void TransportStatusTest::previousGlyphStartsOnTheContentEdge_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<bool>("dark");
    // The glyph's place and the ring's follow the font alone; the themes
    // take turns.
    QTest::newRow("10 pt, dark") << 10.0 << true;
    QTest::newRow("14 pt, light") << 14.0 << false;
    QTest::newRow("24 pt, dark") << 24.0 << true;
}

// Previous's glyph starts on the 12 px content edge, where the library's
// rows and the status bar's text do, as the transport's right end is
// measured to the volume's groove (its ink was at 20 px, with the button's
// clear space added to the inset). Its hover tint and focus ring stay inside
// the window: at a large font, whose clear space would put the ring past the
// window's edge, the glyph starts a pixel or two further in.
void TransportStatusTest::previousGlyphStartsOnTheContentEdge() {
    QFETCH(qreal, pointSize);
    QFETCH(bool, dark);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QCOMPARE(harness.tokens->property("basePointSize").toReal(), pointSize);
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QQuickItem *button = harness.item(QStringLiteral("transportButton_previous"));
    QVERIFY(transport && button);
    QCOMPARE(sceneRect(transport).left(), 0.0);
    QQuickItem *glyph = glyphOf(button);
    QVERIFY(glyph != nullptr);
    const qreal clearSpace = transport->property("smallGlyphInset").toReal();
    // 12 px, unless the ring, 3 px outside the button, would leave the
    // window.
    const qreal expectedLeft = std::max(12.0, 3.0 + clearSpace);
    QCOMPARE(sceneRect(glyph).left(), expectedLeft);
    if (pointSize == 10.0) {
        QCOMPARE(expectedLeft, 12.0);
    }

    // As drawn, along the glyph's middle row: the first column at least
    // half as far from the chrome as the glyph's strongest, so that how a
    // mask is coloured offscreen, or a disabled glyph's colour, doesn't
    // count. The glyph's bar is at its left.
    const QColor chrome = harness.transportChrome();
    const auto inkColumn = [&]() {
        qreal scale = 1;
        const QImage image = grabbed(harness.window, &scale);
        const int middle = int(sceneRect(glyph).center().y());
        QList<int> distances;
        for (int x = 0; x < int(sceneRect(glyph).right()) + 2; ++x) {
            distances.append(channelDistance(pixelAt(image, QPointF(x + 0.5, middle + 0.5), scale), chrome));
        }
        const int strongest = *std::max_element(distances.cbegin(), distances.cend());
        for (int x = 0; strongest >= 16 && x < distances.size(); ++x) {
            if (distances[x] * 2 >= strongest) {
                return x;
            }
        }
        return -1;
    };
    int ink = -1;
    QTRY_VERIFY((ink = inkColumn()) >= 0);
    QCOMPARE(qreal(ink), expectedLeft);

    // Hovered and focused, the tint and the ring inside the window.
    auto *background = button->property("background").value<QQuickItem *>();
    QVERIFY(background != nullptr);
    QQuickItem *hover = background->findChildren<QQuickItem *>(QStringLiteral("transportButtonHover")).value(0);
    QQuickItem *ring = background->findChildren<QQuickItem *>(QStringLiteral("transportButtonFocusRing")).value(0);
    QVERIFY(hover && ring);
    button->setProperty("hoverEnabled", true);
    SteppedAnimationDriver animations;
    // From elsewhere, so that the move is one, after the last row's.
    QTest::mouseMove(harness.window, QPoint(harness.window->width() / 2, harness.window->height() - 10));
    QTest::mouseMove(harness.window, sceneCentre(button));
    QVERIFY(stepUntil(animations, [&] { return hover->isVisible() && hover->opacity() == 1.0; }));
    QVERIFY2(sceneRect(hover).left() >= 0.0, qPrintable(describeRect(sceneRect(hover))));
    button->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(ring->isVisible());
    QVERIFY2(sceneRect(ring).left() >= 0.0 && sceneRect(ring).top() >= sceneRect(transport).top(),
             qPrintable(describeRect(sceneRect(ring))));
    // The ring's left side drawn, in full, at the window's edge or in it.
    qreal scale = 1;
    const QImage image = grabbed(harness.window, &scale);
    const QColor ringColor = harness.token("focusRing");
    const QPointF ringLeft(sceneRect(ring).left() + 1.0, sceneRect(ring).center().y());
    QVERIFY2(channelDistance(pixelAt(image, ringLeft, scale), ringColor) <= 8,
             qPrintable(pixelAt(image, ringLeft, scale).name()));
}

void TransportStatusTest::volumeMuteKeepsTheThumb_data() {
    addThemeRows();
}

// A mute leaves the thumb at the volume it put aside (it threw the thumb
// to 0), dims the filled groove and the thumb at rest to the unfilled
// colour together, over the same fade (the thumb stayed bright), and shows
// the muted glyph; the tooltip says so. A hover still shows the thumb in
// the text colour. Unmuting brings both back.
void TransportStatusTest::volumeMuteKeepsTheThumb() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
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
    QCOMPARE(volume->property("value").toDouble(), 80.0);
    const qreal thumbX = thumb->x();
    const qreal fillWidth = fill->width();
    QVERIFY(muteGlyph->property("source").toString().endsWith(QStringLiteral("/volume.svg")));

    const QColor unfilled = groove->property("color").value<QColor>();
    QVERIFY(sameColor(thumb->property("color").value<QColor>(), played));
    QVERIFY(sameColor(fill->property("color").value<QColor>(), played));
    const auto colorOf = [](QQuickItem *item) { return item->property("color").value<QColor>(); };
    SteppedAnimationDriver animations;
    // The fill and the thumb fade as one: the same colour at every frame
    // of the fade, which takes the short duration.
    const auto fadeTogether = [&](const QColor &to) {
        bool between = false;
        int frame = 0;
        QString apart;
        const bool ended = stepUntil(animations, [&] {
            if (apart.isEmpty() && !sameColor(colorOf(fill), colorOf(thumb))) {
                apart = QStringLiteral("fill %1, thumb %2 at frame %3")
                            .arg(colorOf(fill).name(), colorOf(thumb).name())
                            .arg(frame);
            }
            ++frame;
            between = between || (!sameColor(colorOf(fill), played) && !sameColor(colorOf(fill), unfilled));
            return sameColor(colorOf(fill), to) && sameColor(colorOf(thumb), to);
        });
        QVERIFY2(apart.isEmpty(), qPrintable(apart));
        QVERIFY(ended);
        QVERIFY2(between, "no fade");
    };

    // Away from the slider, so the thumb is at rest.
    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(mute));
    QTRY_VERIFY(harness.root->property("muted").toBool());
    QCOMPARE(harness.bridge->property("volume").toDouble(), 0.0);
    QVERIFY(muteGlyph->property("source").toString().endsWith(QStringLiteral("/volume-muted.svg")));
    // The thumb and the fill stay; both dim to the groove's colour.
    QCOMPARE(volume->property("value").toDouble(), 80.0);
    QCOMPARE(thumb->x(), thumbX);
    QCOMPARE(fill->width(), fillWidth);
    fadeTogether(unfilled);
    QCOMPARE(tip->property("text").toString(), QStringLiteral("Volume 80%, muted"));
    QCOMPARE(accessibleText(volume, QAccessible::Description), QStringLiteral("80%"));

    // A hover shows the thumb in the text colour while muted too, and the
    // fill stays dim.
    QTest::mouseMove(harness.window, sceneCentre(thumb));
    QVERIFY(stepUntil(animations, [&] { return sameColor(colorOf(thumb), harness.token("text")); }));
    QVERIFY(sameColor(colorOf(fill), unfilled));
    QTest::mouseMove(harness.window, sceneCentre(mute));
    QVERIFY(stepUntil(animations, [&] { return sameColor(colorOf(thumb), unfilled); }));

    QTest::mouseClick(harness.window, Qt::LeftButton, Qt::NoModifier, sceneCentre(mute));
    QTRY_VERIFY(!harness.root->property("muted").toBool());
    QCOMPARE(harness.bridge->property("volume").toDouble(), 0.8);
    QCOMPARE(thumb->x(), thumbX);
    fadeTogether(played);
    QCOMPARE(tip->property("text").toString(), QStringLiteral("Volume 80%"));
}

// The volume's tooltip shows the value it is dragged to, even when the drag
// starts before the hover's delay has shown it ("Volume 100%" with the thumb
// at 40%: the shared tooltip kept the text it had when the delay began).
void TransportStatusTest::volumeToolTipFollowsTheDrag() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *volume = harness.item(QStringLiteral("volumeSlider"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QObject *tip = harness.root->findChild<QObject *>(QStringLiteral("volumeToolTip"));
    QVERIFY(volume && thumb && tip);
    Kirigami::Platform::Units *units = unitsOf(volume);
    QVERIFY(units != nullptr);
    // A delay long enough that the drag starts and ends within it.
    units->setToolTipDelay(400);
    harness.bridge->setProperty("volume", 1.0);
    QTRY_COMPARE(volume->property("value").toDouble(), 100.0);

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
    QTRY_VERIFY(std::abs(volume->property("value").toDouble() - 40.0) < 2.0);
    QTRY_VERIFY(tip->property("opened").toBool());
    const QString expected = QStringLiteral("Volume %1%").arg(std::lround(volume->property("value").toDouble()));
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
    QVERIFY2(harness.load(), qPrintable(harness.error));
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
    const qreal playingGlyphWidth = glyph->width();
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
    QCOMPARE(glyph->width(), playingGlyphWidth);
    QCOMPARE(glyph->property("source").toString(), QStringLiteral("media-playback-stop"));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QTRY_COMPARE(sceneRect(label).left(), labelLeft);
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("textSecondary")));
}

// Breeze draws Repeat and Shuffle for 16 px: they are drawn at that, on
// whole pixels (at 14 px their lines were soft and half as bright as the
// words beside them).
void TransportStatusTest::statusBarModeGlyphsAreDrawnAtTheirSize() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *repeat = harness.item(QStringLiteral("statusBarRepeatGlyph"));
    QQuickItem *shuffle = harness.item(QStringLiteral("statusBarShuffleGlyph"));
    QVERIFY(shuffle != nullptr);
    QCOMPARE(shuffle->size(), QSizeF(16, 16));
    QVERIFY(repeat != nullptr);
    QCOMPARE(repeat->size(), QSizeF(16, 16));
    harness.bridge->setProperty("shuffleEnabled", true);
    for (const int mode : {1, 2}) {
        harness.bridge->setProperty("repeatMode", mode);
        qreal scale = 1;
        grabbed(harness.window, &scale);
        for (QQuickItem *glyph : {repeat, shuffle}) {
            const QPointF at = sceneRect(glyph).topLeft();
            QVERIFY2(std::abs(at.x() - std::round(at.x())) < 0.01 && std::abs(at.y() - std::round(at.y())) < 0.01,
                     qPrintable(QStringLiteral("%1 at %2,%3").arg(glyph->objectName()).arg(at.x()).arg(at.y())));
        }
    }
}

void TransportStatusTest::statusBarShowsRepeatAndShuffleInWords() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *group = harness.item(QStringLiteral("statusBarRepeatShuffle"));
    QQuickItem *repeat = harness.item(QStringLiteral("statusBarRepeatLabel"));
    QQuickItem *shuffle = harness.item(QStringLiteral("statusBarShuffle"));
    QQuickItem *shuffleGlyph = harness.item(QStringLiteral("statusBarShuffleGlyph"));
    QQuickItem *shuffleLabel = harness.item(QStringLiteral("statusBarShuffleLabel"));
    QQuickItem *repeatGlyph = nullptr;
    QQuickItem *format = harness.item(QStringLiteral("statusBarFormat"));
    QQuickItem *state = harness.item(QStringLiteral("statusBarPlaybackState"));
    QVERIFY(group && repeat && shuffle && shuffleGlyph && shuffleLabel && state && format);
    // Off: nothing.
    QVERIFY(!group->isVisible());
    harness.bridge->setProperty("repeatMode", 1);
    QTRY_VERIFY(repeat->isVisible());
    QCOMPARE(repeat->property("text").toString(), QStringLiteral("Repeat One Track"));
    QVERIFY(!shuffle->isVisible());
    // 16 px after the format, or after the playback state without one.
    QTRY_COMPARE(sceneRect(group).left(), sceneRect(format).right() + 16.0);
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QTRY_COMPARE(sceneRect(group).left(), sceneRect(state).right() + 16.0);
    harness.bridge->setProperty("playbackState", QStringLiteral("Playing"));
    QTRY_COMPARE(sceneRect(group).left(), sceneRect(format).right() + 16.0);
    harness.bridge->setProperty("repeatMode", 2);
    QCOMPARE(repeat->property("text").toString(), QStringLiteral("Repeat Whole Queue"));
    // Shuffle as repeat is (it was a glyph alone, named only in its
    // tooltip): its glyph and the words of its menu item, "Shuffle", in the
    // secondary colour, 16 px after repeat.
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
    QTRY_COMPARE(sceneRect(group).left(), sceneRect(format).right() + 16.0);
    QTRY_COMPARE(sceneRect(shuffle).left(), sceneRect(group).left());
}

void TransportStatusTest::statusBarShowsTheFormatGroupWithoutThePosition() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *format = harness.item(QStringLiteral("statusBarFormat"));
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QVERIFY(format && status);
    QCOMPARE(format->property("text").toString(), QStringLiteral("FLAC · 16-bit / 44.1 kHz · Stereo · 557 kbps"));
    // The position is the transport's alone.
    const QList<QQuickItem *> labels = status->findChildren<QQuickItem *>();
    for (QQuickItem *item : labels) {
        const QString text = item->property("text").toString();
        QVERIFY2(!text.contains(QStringLiteral("1:17")) && !text.contains(QStringLiteral("3:49")), qPrintable(text));
    }
    harness.bridge->setProperty("playbackState", QStringLiteral("Stopped"));
    QCOMPARE(format->property("text").toString(), QString());
}

// The format group shows what fits whole, not what the window's width
// allows (at 1000 px it showed "FLAC" alone, with most of the bar empty).
// With the room, all of it; with less, it drops parts from the end, each
// only once it no longer fits, and never cuts one.
void TransportStatusTest::statusBarFormatShowsWhatFitsWhole() {
    Harness harness;
    QVERIFY2(harness.load(kBreezeDark, 1000), qPrintable(harness.error));
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
    // the separator between two: the headers' middle dot between two
    // spaces, as wide as that text and no wider (a dot with 6 px either side
    // of it stood wider than the headers').
    const QFontMetricsF metrics(stateLabel->property("font").value<QFont>());
    const qreal dot = metrics.horizontalAdvance(separator);
    QList<QQuickItem *> separators;
    const std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
        if (item->objectName() == QStringLiteral("statusBarFormatSeparator") && item->isVisible()) {
            separators.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            collect(child);
        }
    };
    collect(row);
    QCOMPARE(separators.size(), parts.size() - 1);
    for (QQuickItem *item : std::as_const(separators)) {
        QCOMPARE(item->property("text").toString(), separator);
        QCOMPARE(item->property("leftPadding").toReal(), 0.0);
        QCOMPARE(item->property("rightPadding").toReal(), 0.0);
        QVERIFY2(std::abs(item->width() - dot) <= 1.0,
                 qPrintable(QStringLiteral("%1 px for %2").arg(item->width()).arg(dot)));
        QVERIFY(sameColor(item->property("color").value<QColor>(), harness.token("textTertiary")));
    }
    qreal partsWidth = 0;
    for (const QString &part : parts) {
        partsWidth += metrics.horizontalAdvance(part);
    }
    QVERIFY2(std::abs(row->implicitWidth() - (partsWidth + (parts.size() - 1) * dot)) <= parts.size(),
             qPrintable(QStringLiteral("%1 px for %2").arg(row->implicitWidth()).arg(partsWidth + 3 * dot)));
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

void TransportStatusTest::statusBarFormatKeepsItsPlace_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// The format keeps one x as Repeat and Shuffle toggle, after the playback
// state, and the modes follow it at an x a changing bitrate doesn't move
// (the format jumped some 210 px with the modes). Where there is too little
// room, the format gives its parts up and the modes stay whole.
void TransportStatusTest::statusBarFormatKeepsItsPlace() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(kBreezeDark, 2400), qPrintable(harness.error));
    QQuickItem *format = harness.item(QStringLiteral("statusBarFormat"));
    QQuickItem *row = harness.item(QStringLiteral("statusBarFormatRow"));
    QQuickItem *modes = harness.item(QStringLiteral("statusBarRepeatShuffle"));
    QQuickItem *state = harness.item(QStringLiteral("statusBarPlaybackState"));
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QVERIFY(format && row && modes && state && status);
    QObject *bridge = harness.bridge;
    qreal scale = 1;
    const auto laidOut = [&]() { grabbed(harness.window, &scale); };
    laidOut();
    const qreal formatLeft = sceneRect(row).left();
    QCOMPARE(formatLeft, sceneRect(state).right() + 16.0);
    for (const auto &[repeat, shuffle] :
         {std::pair{1, false}, std::pair{2, true}, std::pair{0, true}, std::pair{1, true}, std::pair{0, false}}) {
        bridge->setProperty("repeatMode", repeat);
        bridge->setProperty("shuffleEnabled", shuffle);
        laidOut();
        QCOMPARE(sceneRect(row).left(), formatLeft);
        if (modes->isVisible()) {
            QVERIFY2(sceneRect(modes).left() >= sceneRect(row).right() + 15.5,
                     qPrintable(describeRect(sceneRect(modes))));
        }
    }
    bridge->setProperty("repeatMode", 2);
    laidOut();
    const qreal modesLeft = sceneRect(modes).left();
    // It follows the format, and is not at the bar's other end.
    QVERIFY(modesLeft - sceneRect(row).right() < 16.0 + 2 * pointSize * 2);
    for (const int kbps : {64, 320, 999, 1411, 9216, 987, 1000}) {
        bridge->setProperty("currentTrackCurrentBitrateKbps", kbps);
        laidOut();
        QCOMPARE(sceneRect(modes).left(), modesLeft);
        QCOMPARE(sceneRect(row).left(), formatLeft);
    }

    // With less room than both take, the modes stay whole and in the bar.
    const int narrow = int(std::ceil(sceneRect(state).right() + 16 + modes->width() + 40));
    harness.window->setWidth(narrow);
    QTRY_COMPARE(status->width(), qreal(narrow));
    laidOut();
    QVERIFY2(sceneRect(modes).right() <= sceneRect(status).right() + 0.5, qPrintable(describeRect(sceneRect(modes))));
    QVERIFY(row->implicitWidth() <= format->width() + 0.5);
    QVERIFY(format->property("text").toString() != QStringLiteral("FLAC · 16-bit / 44.1 kHz · Stereo · 1000 kbps"));

    // Where only some of the format fits, the modes follow what is shown,
    // with no hole where the parts that were dropped would be.
    QSet<qsizetype> shown;
    for (int width = 2400; width >= narrow; width -= 23) {
        harness.window->setWidth(width);
        QTRY_COMPARE(status->width(), qreal(width));
        laidOut();
        QVERIFY2(sceneRect(modes).right() <= sceneRect(status).right() + 0.5, qPrintable(QString::number(width)));
        const QString text = format->property("text").toString();
        if (text.isEmpty() || text.contains(QStringLiteral("kbps"))) {
            continue;
        }
        shown.insert(text.size());
        QVERIFY2(sceneRect(modes).left() - sceneRect(row).right() >= 12.0
                     && sceneRect(modes).left() - sceneRect(row).right() <= 20.0,
                 qPrintable(QStringLiteral("%1 px after \"%2\" at %3")
                                .arg(sceneRect(modes).left() - sceneRect(row).right()).arg(text).arg(width)));
    }
    QVERIFY(!shown.isEmpty());

    // With room for the modes alone, they take the format's place, with no
    // gap kept for it to push the bar's right end out of the window.
    const int tight = int(std::ceil(formatLeft + modes->width())) + 8;
    harness.window->setWidth(tight);
    QTRY_COMPARE(status->width(), qreal(tight));
    laidOut();
    QCOMPARE(format->property("text").toString(), QString());
    QCOMPARE(sceneRect(modes).left(), formatLeft);
    auto *content = status->property("contentItem").value<QQuickItem *>();
    QVERIFY(content != nullptr);
    QVERIFY2(content->implicitWidth() > 0 && sceneRect(modes).right() <= sceneRect(content).right() + 0.5,
             qPrintable(describeRect(sceneRect(modes))));
    QVERIFY2(modes->parentItem()->width() >= modes->width()
                 && modes->parentItem()->width() < modes->width() + 16,
             qPrintable(QString::number(modes->parentItem()->width())));
}

void TransportStatusTest::statusBarShowsTheScanWithItsBar() {
    for (const int width : {1600, 1000}) {
        Harness harness;
        QVERIFY2(harness.load(kBreezeDark, width), qPrintable(harness.error));
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
        // Words that say what the progress is, and its percent of the files
        // to scan, not of those found so far (the bar alone read as a twin
        // of the Last.fm slot's); the files in the tooltip.
        QCOMPARE(count->property("text").toString(), QStringLiteral("34%"));
        QCOMPARE(accessibleText(scan, QAccessible::Name), QStringLiteral("Scanning “Music”: 412 of 1,204 files"));
        if (width == 1000) {
            QCOMPARE(label->property("text").toString(), QStringLiteral("Scanning…"));
            QVERIFY(!bar->isVisible());
            continue;
        }
        QCOMPARE(label->property("text").toString(), QStringLiteral("Scanning “Music”…"));
        QVERIFY(bar->isVisible());
        QCOMPARE(bar->size(), QSizeF(72, 4));
        // The percent beside the bar, and the bar after it.
        QTRY_VERIFY2(sceneRect(label).right() <= sceneRect(count).left(),
                     qPrintable(describeRect(sceneRect(label)) + QStringLiteral(" / ") + describeRect(sceneRect(count))));
        QVERIFY2(sceneRect(count).right() <= sceneRect(bar).left(),
                 qPrintable(describeRect(sceneRect(count)) + QStringLiteral(" / ") + describeRect(sceneRect(bar))));
        // The bar moves with the batches, by whole pixels.
        QCOMPARE(fill->width(), std::floor(bar->width() * 412 / 1204));
        harness.bridge->setProperty("libraryScanProcessed", 602);
        QCOMPARE(fill->width(), bar->width() / 2);
        QCOMPARE(count->property("text").toString(), QStringLiteral("50%"));
        // Two folders: no name.
        harness.bridge->setProperty(
            "libraryRootEntries",
            QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Music")}},
                         QVariantMap{{QStringLiteral("name"), QStringLiteral("More")}}});
        QCOMPARE(label->property("text").toString(), QStringLiteral("Scanning library…"));
        harness.bridge->setProperty("libraryScanInProgress", false);
        QTRY_VERIFY(!scan->isVisible());
    }
}

// The scan's bar grows with a larger font, as the Last.fm slot's does (at
// 24 pt it stayed 72 by 4 px beside 32 px text), and stays within the bar.
void TransportStatusTest::statusBarScanBarGrowsWithTheFont() {
    const ScopedApplicationFont font(24);
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QQuickItem *bar = harness.item(QStringLiteral("statusBarScanBar"));
    QQuickItem *fill = harness.item(QStringLiteral("statusBarScanBarFill"));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QVERIFY(status && bar && fill && slot);
    harness.bridge->setProperty("libraryScanProcessed", 50);
    harness.bridge->setProperty("libraryScanTotal", 100);
    harness.bridge->setProperty("libraryScanInProgress", true);
    QTRY_VERIFY(bar->isVisible());
    QCOMPARE(bar->width(), 173.0);
    const int lastFm = slot->property("barHeight").toInt();
    QVERIFY(lastFm > 3);
    QVERIFY2(bar->height() > lastFm && bar->height() <= 2 * lastFm, qPrintable(QString::number(bar->height())));
    QCOMPARE(bar->property("radius").toReal(), bar->height() / 2);
    QCOMPARE(fill->property("radius").toReal(), bar->height() / 2);
    QCOMPARE(fill->width(), std::floor(bar->width() / 2));
    QTRY_VERIFY(within(sceneRect(bar), sceneRect(status)));
}

// A rescan, whose files are processed as soon as they are found: the bar
// runs from empty to full against the files to scan, and is full only once
// they all are.
void TransportStatusTest::statusBarScanBarFillsOnlyOnceTheRescanEnds() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *count = harness.item(QStringLiteral("statusBarScanCount"));
    QQuickItem *bar = harness.item(QStringLiteral("statusBarScanBar"));
    QQuickItem *fill = harness.item(QStringLiteral("statusBarScanBarFill"));
    QVERIFY(count && bar && fill);
    harness.bridge->setProperty("libraryScanTotal", 1000);
    harness.bridge->setProperty("libraryScanProcessed", 0);
    harness.bridge->setProperty("libraryScanDiscovered", 0);
    harness.bridge->setProperty("libraryScanInProgress", true);
    QTRY_VERIFY(fill->isVisible());
    QCOMPARE(fill->width(), 0.0);
    QCOMPARE(count->property("text").toString(), QStringLiteral("0%"));
    const qreal full = bar->width();
    QVERIFY(full > 0);

    double previous = 0.0;
    for (int processed = 1; processed < 1000; ++processed) {
        harness.bridge->setProperty("libraryScanProcessed", processed);
        harness.bridge->setProperty("libraryScanDiscovered", processed);
        const double width = fill->width();
        QVERIFY2(width >= previous, qPrintable(QString::number(processed)));
        QVERIFY2(width < full, qPrintable(QStringLiteral("full at %1 of 1000").arg(processed)));
        QVERIFY2(count->property("text").toString() != QStringLiteral("100%"),
                 qPrintable(QStringLiteral("100% at %1 of 1000").arg(processed)));
        previous = width;
    }
    QCOMPARE(count->property("text").toString(), QStringLiteral("99%"));
    QCOMPARE(fill->width(), full - 1);
    harness.bridge->setProperty("libraryScanProcessed", 1000);
    QCOMPARE(fill->width(), full);
    QCOMPARE(count->property("text").toString(), QStringLiteral("100%"));
}

// The scan's words and its bar keep their places while it counts: the
// percent keeps the room of "100%", in tabular figures, so the status bar's
// groups do not move as it goes from 0% to 9% to 10% to 100%, at 10 and at
// 24 pt, wide and narrow.
void TransportStatusTest::statusBarScanKeepsItsPlaceWhileItCounts_data() {
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("pointSize");
    QTest::newRow("1600, 10 pt") << 1600 << 10;
    QTest::newRow("1000, 10 pt") << 1000 << 10;
    QTest::newRow("1600, 24 pt") << 1600 << 24;
    QTest::newRow("1000, 24 pt") << 1000 << 24;
}

void TransportStatusTest::statusBarScanKeepsItsPlaceWhileItCounts() {
    QFETCH(int, width);
    QFETCH(int, pointSize);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(kBreezeDark, width), qPrintable(harness.error));
    QCOMPARE(harness.tokens->property("basePointSize").toReal(), qreal(pointSize));
    QQuickItem *scan = harness.item(QStringLiteral("statusBarScan"));
    QQuickItem *label = harness.item(QStringLiteral("statusBarScanLabel"));
    QQuickItem *count = harness.item(QStringLiteral("statusBarScanCount"));
    QVERIFY(scan && label && count);
    harness.bridge->setProperty("libraryRootEntries",
                                QVariantList{QVariantMap{{QStringLiteral("name"), QStringLiteral("Music")}}});
    harness.bridge->setProperty("libraryScanTotal", 1000);
    harness.bridge->setProperty("libraryScanProcessed", 0);
    harness.bridge->setProperty("libraryScanInProgress", true);
    QTRY_VERIFY(scan->isVisible());
    QTRY_VERIFY(scan->width() > 0);
    const QRectF scanRect = sceneRect(scan);
    const QRectF labelRect = sceneRect(label);
    const QRectF countRect = sceneRect(count);
    // The percent is in tabular figures.
    QVERIFY(count->property("font").value<QFont>().featureValue(QFont::Tag("tnum")) == 1);
    for (const int processed : {5, 90, 100, 555, 999, 1000}) {
        harness.bridge->setProperty("libraryScanProcessed", processed);
        QTRY_COMPARE(count->property("text").toString(), QStringLiteral("%1%").arg(processed / 10));
        // The percent is whole, never cut.
        QVERIFY2(count->property("implicitWidth").toReal() <= count->width() + 0.01,
                 qPrintable(QString::number(count->property("implicitWidth").toReal())));
        QCOMPARE(sceneRect(scan), scanRect);
        QCOMPARE(sceneRect(label), labelRect);
        QCOMPARE(sceneRect(count), countRect);
    }
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
        QVERIFY2(harness.load(), qPrintable(harness.error));
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
        FrameSampler counting(harness.window, [&frames]() { ++frames; });
        harness.window->update();
        QTRY_VERIFY(frames > 0);
        const QList<QRectF> before = geometry();
        QTest::qWait(300);
        counting.stop();
        QCOMPARE(geometry(), before);

        // The walk has ended: the files it found are the total, and the bar
        // fills against it.
        harness.bridge->setProperty("libraryScanTotal", 1500);
        QVERIFY(bar->isVisible());
        QVERIFY(fill->isVisible());
        QCOMPARE(count->property("text").toString(), QStringLiteral("80%"));
        QCOMPARE(fill->width(), std::floor(bar->width() * 1204 / 1500));
    }
}

void TransportStatusTest::lastFmSlotShowsEachState() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *box = harness.item(QStringLiteral("scrobbleIndicatorBox"));
    QQuickItem *bar = harness.item(QStringLiteral("scrobbleIndicatorBar"));
    QQuickItem *trough = harness.item(QStringLiteral("scrobbleIndicatorTrough"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QQuickItem *label = harness.item(QStringLiteral("scrobbleIndicatorLabel"));
    QVERIFY(slot && box && bar && trough && glyph && label);
    QObject *bridge = harness.bridge;
    QVERIFY(!slot->isVisible());
    const auto progress = [slot]() { return slot->property("barProgress").toDouble(); };

    // Listening: the glyph in the secondary colour, and under it the bar in
    // the same colour, the listening done of its trough, which is opaque and
    // 3:1 on the status bar (the text at 20% was 1.9:1); the time left in
    // the tooltip.
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    bridge->setProperty("lastFmScrobbleListenedMs", 77000);
    bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(!label->isVisible());
    // A 16 px glyph (10 px inside a ring was a smudge at 1x), and a 16 by
    // 3 px trough 2 px under its ink (the bar under the glyph's cell pushed
    // the glyph 2.5 px above the middle; at 2 px its proportions did not
    // read at 1x), which the bar fills from the left by whole pixels.
    QCOMPARE(box->size(), QSizeF(16, 16));
    QCOMPARE(glyph->size(), QSizeF(16, 16));
    QCOMPARE(glyph->position(), QPointF(0, 0));
    QVERIFY(trough->isVisible());
    QCOMPARE(trough->position(), QPointF(0, 15));
    QCOMPARE(trough->size(), QSizeF(16, 3));
    const QColor troughColor = trough->property("color").value<QColor>();
    QCOMPARE(troughColor.alphaF(), 1.0F);
    QVERIFY2(contrastRatio(troughColor, harness.token("chrome")) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("trough"), troughColor, harness.token("chrome"))));
    QVERIFY(bar->isVisible());
    QCOMPARE(bar->position(), QPointF(0, 0));
    QCOMPARE(bar->height(), 3.0);
    QCOMPARE(bar->opacity(), 1.0);
    QCOMPARE(slot->property("barWidth").toInt(), 16);
    QCOMPARE(progress(), std::floor(77.0 / 115.0 * 16) / 16);
    QCOMPARE(bar->width(), std::floor(77.0 / 115.0 * 16));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(sameColor(bar->property("color").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: 0:38 to scrobble"));
    QCOMPARE(accessibleText(slot, QAccessible::Name), QStringLiteral("Last.fm: 0:38 to scrobble"));
    QCOMPARE(accessibleText(slot, QAccessible::Description), QStringLiteral("Press to pause scrobbling."));
    // Until the track's length is known, there is no scrobble point: the
    // trough and the 3 px mark that listening always has.
    bridge->setProperty("lastFmScrobbleThresholdMs", 0);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: listening"));
    QCOMPARE(progress(), 0.0);
    QVERIFY(trough->isVisible());
    QVERIFY(bar->isVisible());
    QCOMPARE(bar->width(), 3.0);
    bridge->setProperty("lastFmScrobbleThresholdMs", 115000);

    // Once playback has stopped after a play that ended before its
    // scrobble point: the slot says so, the glyph is quieter, there is no
    // bar, and the tooltip says how far the play got.
    bridge->setProperty("lastFmScrobbleState", 6);
    bridge->setProperty("lastFmScrobbleIneligibility", 3);
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Not scrobbled"));
    QVERIFY(label->isVisible());
    QVERIFY(!bar->isVisible() && !trough->isVisible());
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

    // Being sent and scrobbled differ by shape: while it is sent, the bar
    // fills its trough; once accepted, neither, and the glyph in the
    // secondary colour. The glyph is the same in both, at full strength.
    bridge->setProperty("lastFmScrobbleState", 2);
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(bar->isVisible());
    QCOMPARE(progress(), 1.0);
    QCOMPARE(bar->width(), 16.0);
    QCOMPARE(bar->height(), 3.0);
    QCOMPARE(bar->opacity(), 1.0);
    QVERIFY(sameColor(bar->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(glyph->opacity(), 1.0);
    QVERIFY(!label->isVisible());
    bridge->setProperty("lastFmScrobbleState", 3);
    QTRY_VERIFY(!bar->isVisible() && !trough->isVisible());
    QVERIFY(slot->isVisible());
    QCOMPARE(progress(), 0.0);
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QCOMPARE(glyph->opacity(), 1.0);
    QVERIFY(!label->isVisible());
    // The glyph keeps its place.
    QCOMPARE(box->size(), QSizeF(16, 16));
    QCOMPARE(glyph->position(), QPointF(0, 0));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: scrobbled"));
    bridge->setProperty("lastFmScrobbleState", 2);
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm: sending the scrobble"));

    // Queued: the words and the offline glyph, of the Last.fm glyph's
    // family and size, where it is (it was a 14 px one), without the bar.
    bridge->setProperty("lastFmScrobbleState", 5);
    bridge->setProperty("lastFmPendingScrobbleCount", 3);
    QTRY_VERIFY(slot->isVisible());
    QCOMPARE(label->property("text").toString(), QStringLiteral("3 scrobbles waiting"));
    QVERIFY(label->isVisible());
    QVERIFY(!bar->isVisible() && !trough->isVisible());
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("status-offline.svg")));
    QCOMPARE(glyph->size(), QSizeF(16, 16));
    QCOMPARE(glyph->position(), QPointF(0, 0));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(toolTipText(slot).startsWith(QStringLiteral("Last.fm can't be reached.")));
    bridge->setProperty("lastFmPendingScrobbleCount", 1);
    QCOMPARE(label->property("text").toString(), QStringLiteral("1 scrobble waiting"));

    // Failed: the status bar's one treatment of an error: the words in the
    // text colour and the 16 px glyph in the negative colour, the slot's
    // only colour, at least 3:1 on the chrome; no bar.
    bridge->setProperty("lastFmPendingScrobbleCount", 0);
    bridge->setProperty("lastFmScrobbleState", 4);
    bridge->setProperty("lastFmScrobbleMessage", QStringLiteral("Invalid session key"));
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("Scrobble failed"));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm.svg")));
    QCOMPARE(glyph->size(), QSizeF(16, 16));
    QVERIFY(!bar->isVisible() && !trough->isVisible());
    QCOMPARE(progress(), 0.0);
    const QColor negative = harness.token("negative");
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), negative));
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("text")));
    QCOMPARE(label->property("font").value<QFont>().weight(), QFont::Normal);
    QVERIFY2(contrastRatio(negative, harness.token("chrome")) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("failed glyph"), negative, harness.token("chrome"))));
    QCOMPARE(toolTipText(slot), QStringLiteral("Last.fm didn't accept the scrobble: Invalid session key"));

    // Paused by the user: the glyph struck through, quieter, without the
    // bar, in the same place; a click resumes.
    bridge->setProperty("lastFmScrobbleState", 1);
    bridge->setProperty("lastFmScrobblingPaused", true);
    QTRY_COMPARE(toolTipText(slot), QStringLiteral("Last.fm: scrobbling is paused"));
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("/lastfm-paused.svg")));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textTertiary")));
    QVERIFY(!bar->isVisible());
    QCOMPARE(box->size(), QSizeF(16, 16));
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
    QVERIFY2(harness.load(), qPrintable(harness.error));
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
    // The bar moves by whole device pixels of its trough: with the
    // second, if it crosses one.
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
    addThemeRows();
}

// Paused differs from listening in its shape, not only its colour: what is
// drawn, ink against the chrome, differs in many pixels.
void TransportStatusTest::lastFmSlotPausedLooksStruckThrough() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
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

void TransportStatusTest::lastFmPausedKeepsBothLettersAndTheStrike_data() {
    addThemeRows();
}

// Paused at 1x, with its strike (#470): both letters read under the strike,
// which is a layer of its own in the text colour over the tertiary letters,
// in a clear casing (in one colour, the strike and its clear space left
// neither letter whole).
void TransportStatusTest::lastFmPausedKeepsBothLettersAndTheStrike() {
    QFETCH(bool, dark);
    const auto coverage = [](const QString &name) {
        QImageReader reader(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/assets/") + name);
        reader.setScaledSize(QSize(16, 16));
        const QImage image = reader.read().convertToFormat(QImage::Format_ARGB32);
        QList<double> covered(16 * 16, 0.0);
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                covered[y * 16 + x] = qAlpha(image.pixel(x, y)) / 255.0;
            }
        }
        return covered;
    };
    const QList<double> whole = coverage(QStringLiteral("lastfm.svg"));
    const QList<double> paused = coverage(QStringLiteral("lastfm-paused.svg"));
    // Each letter keeps its ink but for the strike's casing: nearly all of
    // it, and the pixels two from the strike's middle half or more.
    for (const auto &[left, right] : {std::pair{1, 8}, std::pair{9, 15}}) {
        double ink = 0;
        double kept = 0;
        for (int y = 0; y < 16; ++y) {
            for (int x = left; x < right; ++x) {
                const double before = whole[y * 16 + x];
                const double after = paused[y * 16 + x];
                if (std::abs(x - y) >= 2) {
                    ink += before;
                    kept += std::min(before, after);
                }
                if (std::abs(x - y) == 2 && before >= 0.9) {
                    QVERIFY2(after >= 0.5, qPrintable(QStringLiteral("%1,%2 keeps %3").arg(x).arg(y).arg(after)));
                }
                if (x == y) {
                    QVERIFY2(after <= 0.1, qPrintable(QStringLiteral("no casing at %1").arg(x)));
                }
            }
        }
        QVERIFY2(kept >= 0.9 * ink, qPrintable(QStringLiteral("%1 of %2 from column %3").arg(kept).arg(ink).arg(left)));
    }

    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QQuickItem *strike = harness.item(QStringLiteral("scrobbleIndicatorStrike"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QVERIFY(strike && glyph);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QVERIFY(!strike->isVisible());
    harness.bridge->setProperty("lastFmScrobblingPaused", true);
    QTRY_VERIFY(strike->isVisible());
    QCOMPARE(sceneRect(strike), sceneRect(glyph));
    const QColor chrome = harness.token("chrome");
    const QColor strikeColor = strike->property("color").value<QColor>();
    const QColor letters = glyph->property("color").value<QColor>();
    QVERIFY(sameColor(strikeColor, harness.token("text")));
    QVERIFY(sameColor(letters, harness.token("textTertiary")));
    QVERIFY2(contrastRatio(strikeColor, chrome) >= 1.5 * contrastRatio(letters, chrome),
             qPrintable(describeContrast(QStringLiteral("strike"), strikeColor, chrome)));
}

void TransportStatusTest::lastFmSlotBarFillsItsTrough_data() {
    addThemeRows();
}

// The bar under the glyph, as drawn at 1x: a trough 16 by 3 px, the glyph's
// width, 2 px under its ink, the text at 35% in the chrome, or nearer the
// text where that is under 3:1 on the chrome (the text at 20% was 1.9:1
// dark and 1.5:1 light), and in it from the left the listening done, in the
// secondary text colour: 3:1 or more on the chrome and well apart from its
// trough, so that it reads as a share of a whole (a bar with no trough,
// that started 6 px long and was brighter than its glyph, did not read as
// progress). Nothing between the glyph and the trough. At 0% a 3 px mark in
// the trough, so that listening that has just started never looks like
// scrobbled (the bare glyph, or the trough alone, did). Being sent fills
// it; scrobbled and failed have neither bar nor trough.
void TransportStatusTest::lastFmSlotBarFillsItsTrough() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
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

    const QColor fill = harness.token("textSecondary");
    const QColor trough = slot->property("troughColor").value<QColor>();
    QCOMPARE(trough.alphaF(), 1.0F);
    QVERIFY2(contrastRatio(fill, chrome) >= 3.0, qPrintable(describeContrast(QStringLiteral("bar"), fill, chrome)));
    QVERIFY2(contrastRatio(trough, chrome) >= 3.0, qPrintable(describeContrast(QStringLiteral("trough"), trough, chrome)));
    const QColor thirtyFive = mixed(chrome, harness.token("text"), 0.35);
    if (contrastRatio(thirtyFive, chrome) >= 3.1) {
        QVERIFY2(channelDistance(trough, thirtyFive) <= 1, qPrintable(trough.name() + QStringLiteral(" vs ") + thirtyFive.name()));
    } else {
        // Only as much nearer the text as 3:1 takes.
        QVERIFY2(contrastRatio(trough, chrome) < 3.3, qPrintable(describeContrast(QStringLiteral("trough"), trough, chrome)));
    }
    // The bar stands out from the trough as clearly as the trough from the
    // chrome, and is further from the chrome.
    QVERIFY2(contrastRatio(fill, trough) >= 1.9, qPrintable(describeContrast(QStringLiteral("bar on trough"), fill, trough)));
    QVERIFY(contrastRatio(fill, chrome) > contrastRatio(trough, chrome) * 1.9);

    // The 2 px gap under the glyph's ink, the trough's 3 px rows across the
    // glyph's width and the row under them: the pixels drawn as the bar,
    // which must start at the left and run on, those drawn as the trough,
    // after them, and those drawn as neither where one of them belongs, or
    // as anything but the chrome in the gap and under the trough.
    struct Drawn {
        int bar = 0;
        int trough = 0;
        int stray = 0;
        bool fromTheLeft = true;
    };
    qreal scale = 1;
    const auto drawn = [&](bool shown) {
        const QImage image = grabbed(harness.window, &scale);
        const QPointF topLeft = sceneRect(box).topLeft();
        Drawn result;
        for (int y = 13; y < 19; ++y) {
            bool ended = false;
            for (int x = 0; x < 16; ++x) {
                const QColor pixel = pixelAt(image, topLeft + QPointF(x + 0.5, y + 0.5), scale);
                if (y < 15 || y > 17 || !shown) {
                    result.stray += channelDistance(pixel, chrome) <= 2 ? 0 : 1;
                } else if (channelDistance(pixel, fill) <= 2) {
                    result.bar += 1;
                    result.fromTheLeft = result.fromTheLeft && !ended;
                } else {
                    ended = true;
                    result.trough += channelDistance(pixel, trough) <= 2 ? 1 : 0;
                    result.stray += channelDistance(pixel, trough) <= 2 ? 0 : 1;
                }
            }
        }
        return result;
    };
    for (const auto &[done, columns] :
         {std::pair{0.0, 0}, std::pair{0.1, 1}, std::pair{0.25, 4}, std::pair{0.5, 8}, std::pair{0.9, 14}}) {
        bridge->setProperty("lastFmScrobbleListenedMs", int(done * 120000));
        QTRY_COMPARE(slot->property("barProgress").toDouble(), columns / 16.0);
        // Never less than the 3 px mark, nor within one of the end.
        const int drawnColumns = std::clamp(columns, 3, 13);
        Drawn now;
        QTRY_VERIFY2((now = drawn(true), now.bar == 3 * drawnColumns),
                     qPrintable(QStringLiteral("%1 done drew %2 px of the bar").arg(done).arg(now.bar)));
        QVERIFY2(now.trough == 3 * (16 - drawnColumns),
                 qPrintable(QStringLiteral("%1 done drew %2 px of the trough").arg(done).arg(now.trough)));
        QVERIFY2(now.stray == 0, qPrintable(QStringLiteral("%1 done: %2 px of something else").arg(done).arg(now.stray)));
        QVERIFY(now.fromTheLeft);
    }

    // Being sent: full. Scrobbled and failed: the chrome alone.
    bridge->setProperty("lastFmScrobbleState", 2);
    Drawn sending;
    QTRY_VERIFY2((sending = drawn(true), sending.bar == 48 && sending.trough == 0 && sending.stray == 0),
                 qPrintable(QStringLiteral("%1 px, %2 stray").arg(sending.bar).arg(sending.stray)));
    for (const int state : {3, 4}) {
        bridge->setProperty("lastFmScrobbleState", state);
        Drawn now;
        QTRY_VERIFY2((now = drawn(false), now.stray == 0),
                     qPrintable(QStringLiteral("%1: %2 px drawn").arg(state).arg(now.stray)));
    }
}

void TransportStatusTest::lastFmSlotSendingAndScrobbledLookApart_data() {
    addThemeRows();
}

// Listening, being sent and scrobbled look apart as drawn, by shape and not
// by shade (they drew alike, and then differed mainly in shade). Listening:
// the bar short of its trough's end (a play at its scrobble point is being
// sent). Being sent: the bar over the whole trough. Scrobbled: neither, the
// glyph in the secondary colour. The glyph is drawn alike in all three.
void TransportStatusTest::lastFmSlotSendingAndScrobbledLookApart() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *box = harness.item(QStringLiteral("scrobbleIndicatorBox"));
    QQuickItem *bar = harness.item(QStringLiteral("scrobbleIndicatorBar"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QVERIFY(slot && box && bar && glyph);
    const QColor chrome = harness.token("chrome");
    const QColor fill = harness.token("textSecondary");
    const QColor trough = slot->property("troughColor").value<QColor>();
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
            result.bar.append(pixelAt(image, boxTopLeft + QPointF(x + 0.5, 15.5), scale));
        }
        result.glyph = chrome;
        // The glyph's rows above the bar, which hangs in its cell.
        for (int y = 0; y < slot->property("barY").toInt(); ++y) {
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

    // Listening, 15 s short of the scrobble point: the bar short of full,
    // by whole pixels at 1x and 2x, and the trough after it.
    harness.bridge->setProperty("lastFmScrobbleListenedMs", 105000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible() && bar->isVisible());
    Drawn listening;
    QTRY_VERIFY((listening = drawn(), count(listening.bar, fill) == 13));
    QCOMPARE(count(listening.bar, trough), 3);
    QVERIFY2(channelDistance(fill, trough) >= 48 && channelDistance(trough, chrome) >= 24,
             qPrintable(fill.name() + QStringLiteral(" on ") + trough.name() + QStringLiteral(" on ")
                        + chrome.name()));

    // Being sent: the bar over the whole trough.
    harness.bridge->setProperty("lastFmScrobbleListenedMs", 120000);
    harness.bridge->setProperty("lastFmScrobbleState", 2);
    Drawn sending;
    QTRY_VERIFY((sending = drawn(), count(sending.bar, fill) == 16));
    QCOMPARE(count(sending.bar, trough), 0);

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

void TransportStatusTest::lastFmGlyphIsCentredOnItsBlock_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("breeze dark") << true << 10.0;
    QTest::newRow("breeze light") << false << 10.0;
    QTest::newRow("breeze dark, 24 pt") << true << 24.0;
    QTest::newRow("breeze light, 24 pt") << false << 24.0;
}

// The Last.fm glyph's ink, as drawn, is centred on its block, the slot,
// both ways and on whole pixels, and the slot on the status bar under its
// seam (the glyph sat 2.5 px above the middle, over the bar's place). The
// trough under it is 3 px thick at the default font, a little more at a
// larger one, and as wide as the glyph.
void TransportStatusTest::lastFmGlyphIsCentredOnItsBlock() {
    QFETCH(bool, dark);
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QCOMPARE(harness.window->effectiveDevicePixelRatio(), 1.0);
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QQuickItem *bar = harness.item(QStringLiteral("scrobbleIndicatorTrough"));
    QVERIFY(status && slot && glyph && bar);
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    harness.bridge->setProperty("lastFmScrobbleListenedMs", 60000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible() && bar->isVisible());
    const QRectF statusBar = sceneRect(status).adjusted(0, 1, 0, 0);
    QTRY_VERIFY(std::abs(sceneRect(slot).center().y() - statusBar.center().y()) <= 0.5);
    qreal scale = 1;
    const QImage image = grabbed(harness.window, &scale);
    const QColor chrome = harness.token("chrome");
    const QRectF slotRect = sceneRect(slot);
    const QRectF glyphRect = sceneRect(glyph);
    QCOMPARE(glyphRect.topLeft(), QPointF(std::round(glyphRect.left()), std::round(glyphRect.top())));
    QCOMPARE(slotRect.topLeft(), QPointF(std::round(slotRect.left()), std::round(slotRect.top())));

    // The glyph's ink: the pixels of its cell above the bar at least half
    // as far from the chrome as its strongest. Offscreen, Kirigami draws a
    // mask file uncoloured, so this is its ink, not its colour.
    const int side = int(glyphRect.width());
    const int barY = int(sceneRect(bar).top() - glyphRect.top());
    QList<int> distance(side * barY);
    int strongest = 0;
    for (int y = 0; y < barY; ++y) {
        for (int x = 0; x < side; ++x) {
            distance[y * side + x] =
                channelDistance(pixelAt(image, glyphRect.topLeft() + QPointF(x + 0.5, y + 0.5), scale), chrome);
            strongest = std::max(strongest, distance[y * side + x]);
        }
    }
    QVERIFY2(strongest > 24, qPrintable(QString::number(strongest)));
    QRect ink;
    for (int y = 0; y < barY; ++y) {
        for (int x = 0; x < side; ++x) {
            if (distance[y * side + x] * 2 >= strongest) {
                ink = ink.united(QRect(x, y, 1, 1));
            }
        }
    }
    const QRectF inkRect = QRectF(ink).translated(glyphRect.topLeft());
    const QString where = describeRect(inkRect) + QStringLiteral(" in ") + describeRect(slotRect)
        + QStringLiteral(" in ") + describeRect(statusBar);
    QVERIFY2(std::abs(inkRect.center().y() - slotRect.center().y()) <= 0.5, qPrintable(where));
    QVERIFY2(std::abs(inkRect.center().x() - slotRect.center().x()) <= 0.5, qPrintable(where));
    QVERIFY2(std::abs(inkRect.center().y() - statusBar.center().y()) <= 0.5, qPrintable(where));
    if (pointSize == 10.0) {
        // At the default font, exactly: 6 px above and below 10 px of ink.
        QCOMPARE(inkRect.top() - slotRect.top(), 6.0);
        QCOMPARE(slotRect.bottom() - inkRect.bottom(), 6.0);
    }

    // The trough and its bar as drawn: their rows and columns under the
    // ink, a gap of chrome between.
    const QRectF barArea(glyphRect.left(), inkRect.bottom(), glyphRect.width(), slotRect.bottom() - inkRect.bottom());
    QRect drawnBar;
    for (int y = 0; y < int(barArea.height()); ++y) {
        for (int x = 0; x < int(barArea.width()); ++x) {
            if (channelDistance(pixelAt(image, barArea.topLeft() + QPointF(x + 0.5, y + 0.5), scale), chrome) > 8) {
                drawnBar = drawnBar.united(QRect(x, y, 1, 1));
            }
        }
    }
    QVERIFY2(!drawnBar.isEmpty(), qPrintable(where));
    const QRectF barRect = QRectF(drawnBar).translated(barArea.topLeft());
    const int thickness = slot->property("barHeight").toInt();
    if (pointSize == 10.0) {
        QCOMPARE(thickness, 3);
    }
    QVERIFY(thickness >= 3);
    QCOMPARE(barRect.height(), qreal(thickness));
    QVERIFY2(barRect.top() - inkRect.bottom() >= 2.0, qPrintable(describeRect(barRect) + QLatin1Char(' ') + where));
    QVERIFY2(barRect.bottom() <= slotRect.bottom(), qPrintable(describeRect(barRect) + QLatin1Char(' ') + where));
    // As wide as the glyph, on whole pixels.
    QCOMPARE(barRect.left(), glyphRect.left());
    QCOMPARE(barRect.width(), glyphRect.width());
    // Half the listening done: the bar over half of it.
    QQuickItem *fill = harness.item(QStringLiteral("scrobbleIndicatorBar"));
    QVERIFY(fill != nullptr);
    QCOMPARE(fill->width(), glyphRect.width() / 2);
    QCOMPARE(sceneRect(fill).topLeft(), sceneRect(bar).topLeft());
}

void TransportStatusTest::lastFmSlotGlyphReadsAtOneX_data() {
    addThemeRows();
}

// The Last.fm glyph is drawn for 1x at 16 px on whole pixels (a 10 px glyph
// in a ring was a smudge, and a bold one read "QS"): its ink within rows 3
// to 12 and columns 1 to 14, its straight strokes whole pixels and only its
// corners antialiased (with every pixel ink or clear it was a staircase, of
// another hand than the offline glyph), in Breeze's stroke weight (2 px
// verticals, 1 px horizontals, about 1.5 px), and a two-storey a that reads
// as one: its bowl's counter open, the bowl open to the left under the
// arch, the stem whole, and apart from the s.
void TransportStatusTest::lastFmSlotGlyphReadsAtOneX() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QCOMPARE(harness.window->effectiveDevicePixelRatio(), 1.0);
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QVERIFY(slot && glyph);
    const QColor chrome = harness.token("chrome");
    // Scrobbled: the glyph alone, without the bar that hangs in its cell.
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    harness.bridge->setProperty("lastFmScrobbleState", 3);
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
    // Hinted to the pixel grid: the strokes are whole pixels, and the only
    // pixels in between are the corners' arcs, a few of them, each beside
    // whole ink.
    int whole = 0;
    int between = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if (ink(x, y) >= 0.9) {
                ++whole;
            } else if (ink(x, y) > 0.1) {
                ++between;
                bool besideInk = false;
                for (const QPoint step : {QPoint(1, 0), QPoint(-1, 0), QPoint(0, 1), QPoint(0, -1)}) {
                    const QPoint at = QPoint(x, y) + step;
                    besideInk = besideInk || (at.x() >= 0 && at.x() < 16 && at.y() >= 0 && at.y() < 16
                                              && ink(at.x(), at.y()) >= 0.9);
                }
                QVERIFY2(besideInk, qPrintable(QStringLiteral("%1 at %2,%3").arg(ink(x, y)).arg(x).arg(y)));
            }
        }
    }
    QVERIFY2(whole >= 60 && between >= 8 && between * 3 <= whole,
             qPrintable(QStringLiteral("%1 whole, %2 between").arg(whole).arg(between)));
    // The stroke weight: how thick the stroke is at each pixel of ink, its
    // shortest run of ink across, down or along either diagonal. Never more
    // than 2 px, and about 1.5 on average, the weight of the status bar's
    // other glyphs: 2 px strokes everywhere, 1.8 on this measure, read
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
    addThemeRows();
}

// The Last.fm bar is the glyph's, not a second scan or a download, with a
// scan running beside it: as wide as the glyph and under it, less than a
// quarter of the scan bar's length and half its height, with square ends
// where the scan bar's are round. It has a trough, as the scan bar has
// (without one it did not read as progress).
void TransportStatusTest::lastFmBarReadsApartFromTheScanBar() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *bar = harness.item(QStringLiteral("scrobbleIndicatorBar"));
    QQuickItem *trough = harness.item(QStringLiteral("scrobbleIndicatorTrough"));
    QQuickItem *glyph = harness.item(QStringLiteral("scrobbleIndicatorGlyph"));
    QQuickItem *scanBar = harness.item(QStringLiteral("statusBarScanBar"));
    QQuickItem *scanFill = harness.item(QStringLiteral("statusBarScanBarFill"));
    QVERIFY(slot && bar && trough && glyph && scanBar && scanFill);
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

    // Its place and size: under the glyph's ink (rows 3 to 12 of the 16 px
    // glyph), 2 px below it, in a trough of the glyph's width.
    const QRectF glyphRect = sceneRect(glyph);
    const QRectF barRect = sceneRect(bar);
    const QRectF troughRect = sceneRect(trough);
    QCOMPARE(troughRect.left(), glyphRect.left());
    QCOMPARE(troughRect.width(), glyphRect.width());
    QCOMPARE(troughRect.top() - (glyphRect.top() + 13), 2.0);
    QCOMPARE(barRect.topLeft(), troughRect.topLeft());
    QCOMPARE(slot->property("barWidth").toReal(), glyphRect.width());
    QCOMPARE(barRect.width(), glyphRect.width() / 2);
    QVERIFY2(glyphRect.width() * 4 < scanBar->width(), qPrintable(QString::number(scanBar->width())));
    // 3 px to the scan bar's 4 (at 2 px its proportions did not read at
    // 1x), with square ends to the scan bar's round ones, and the scan bar
    // has its percent beside it.
    QCOMPARE(bar->height(), 3.0);
    QCOMPARE(trough->height(), 3.0);
    QQuickItem *scanCount = harness.item(QStringLiteral("statusBarScanCount"));
    QVERIFY(scanCount != nullptr && scanCount->isVisible());
    QCOMPARE(scanCount->property("text").toString(), QStringLiteral("34%"));
    QVERIFY2(bar->height() < scanBar->height(), qPrintable(QString::number(scanBar->height())));
    QCOMPARE(bar->property("radius").toReal(), 0.0);
    QCOMPARE(trough->property("radius").toReal(), 0.0);
    QVERIFY(scanBar->property("radius").toReal() > 0.0);
    // Both troughs are drawn: where the Last.fm bar is still to come, its
    // trough, not the chrome.
    const QColor chrome = harness.token("chrome");
    const QImage image = grabbed(harness.window, &scale);
    const QPointF scanTrough(sceneRect(scanBar).right() - 2, sceneRect(scanBar).center().y());
    QVERIFY(!sameColor(pixelAt(image, scanTrough, scale), chrome));
    const QPointF barToCome(glyphRect.right() - 1.5, troughRect.center().y());
    const QColor troughColor = drawnOver(trough->property("color").value<QColor>(), chrome);
    QVERIFY2(channelDistance(pixelAt(image, barToCome, scale), troughColor) <= 2,
             qPrintable(pixelAt(image, barToCome, scale).name() + QStringLiteral(" for ") + troughColor.name()));

    // Once scrobbled, and when the scrobble failed, there is no bar.
    for (const int state : {1, 2, 3, 4}) {
        bridge->setProperty("lastFmScrobbleState", state);
        if (state >= 3) {
            QTRY_VERIFY(!bar->isVisible() && !trough->isVisible());
            continue;
        }
        QTRY_VERIFY(bar->isVisible() && trough->isVisible());
    }
}

void TransportStatusTest::lastFmListeningNeverFillsTheBar_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// Only a decided scrobble fills the bar: listening stops a mark short of
// its end, however close the scrobble point (a second short looked the same
// as being sent). The bar fills with the decision, as before (#540).
void TransportStatusTest::lastFmListeningNeverFillsTheBar() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *bar = harness.item(QStringLiteral("scrobbleIndicatorBar"));
    QQuickItem *trough = harness.item(QStringLiteral("scrobbleIndicatorTrough"));
    QVERIFY(slot && bar && trough);
    const int mark = slot->property("minimumBarLength").toInt();
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    for (const int listened : {114000, 119000, 119999, 120000, 130000}) {
        harness.bridge->setProperty("lastFmScrobbleListenedMs", listened);
        QVERIFY2(bar->width() <= trough->width() - mark, qPrintable(QString::number(bar->width())));
    }
    harness.bridge->setProperty("lastFmScrobbleState", 2);
    QCOMPARE(bar->width(), trough->width());
}

// The warning triangle is at its neighbours' optical size: 12 px across, as
// dialog-error's square and the info glyph's circle, and about 11 px high,
// centred across its box, its base's lower edge on a whole pixel at 16 px.
// Filling its box, about 14 by 12 px, it was the largest glyph in the row
// beside a 12 px info glyph and a 10 px close.
void TransportStatusTest::warningGlyphIsTheSizeOfItsNeighbours() {
    // The pixels of `name` at `size` with at least half their area covered.
    const auto inkOf = [](const QString &name, int size) {
        QImageReader reader(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/assets/") + name);
        reader.setScaledSize(QSize(size, size));
        const QImage image = reader.read().convertToFormat(QImage::Format_ARGB32);
        QRect ink;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(image.pixel(x, y)) >= 128) {
                    ink = ink.united(QRect(x, y, 1, 1));
                }
            }
        }
        return ink;
    };
    // At 64 px, a 16 px drawing's quarter pixels.
    const QRect warning = inkOf(QStringLiteral("status-warning.svg"), 64);
    const QRect error = inkOf(QStringLiteral("status-error.svg"), 64);
    const QString sizes = QStringLiteral("warning %1x%2 at %3,%4, error %5x%6")
                              .arg(warning.width()).arg(warning.height()).arg(warning.x()).arg(warning.y())
                              .arg(error.width()).arg(error.height());
    QVERIFY2(std::abs(warning.width() - error.width()) <= 2, qPrintable(sizes));
    QVERIFY2(std::abs(warning.height() - 42) <= 2, qPrintable(sizes));
    QVERIFY2(std::abs(warning.left() - (64 - 1 - warning.right())) <= 1, qPrintable(sizes));
    QVERIFY2(std::abs((warning.top() + warning.bottom() + 1) / 2.0 - 32) <= 2, qPrintable(sizes));

    // At 16 px, the base is a whole row of pixels, with none under it.
    QImageReader reader(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/assets/status-warning.svg"));
    reader.setScaledSize(QSize(16, 16));
    const QImage one = reader.read().convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(one.size(), QSize(16, 16));
    QVERIFY2(qAlpha(one.pixel(8, 12)) >= 230, qPrintable(QString::number(qAlpha(one.pixel(8, 12)))));
    QVERIFY2(qAlpha(one.pixel(8, 13)) <= 25, qPrintable(QString::number(qAlpha(one.pixel(8, 13)))));
}

// The Last.fm slot's three glyphs are one family (the "as" was a grid of
// squares, its strike a staircase of them, and the offline glyph thin
// antialiased strokes at 14 px: three hands in one slot). Each is an
// outline drawn for 16 px: its straight strokes whole pixels there, its
// curves and its strike antialiased, and sharp at 2x. Paused and offline
// are struck through with the same stroke, from corner to corner, with
// clear space on either side of it.
void TransportStatusTest::lastFmGlyphsAreOneFamily() {
    // How much of each pixel a glyph's file covers at `size`, 0 to 1.
    const auto coverage = [](const QString &name, int size) {
        QImageReader reader(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/assets/") + name);
        reader.setScaledSize(QSize(size, size));
        const QImage image = reader.read().convertToFormat(QImage::Format_ARGB32);
        QList<double> covered;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                covered.push_back(qAlpha(image.pixel(x, y)) / 255.0);
            }
        }
        return covered;
    };
    // Paused is two files, one glyph: its letters and the strike over them.
    const QStringList names{QStringLiteral("lastfm.svg"), QStringLiteral("lastfm-paused.svg"),
                            QStringLiteral("status-offline.svg")};
    const auto glyphCoverage = [&coverage](const QString &name, int size) {
        QList<double> covered = coverage(name, size);
        if (name == QStringLiteral("lastfm-paused.svg")) {
            const QList<double> strike = coverage(QStringLiteral("lastfm-strike.svg"), size);
            for (qsizetype i = 0; i < std::min(covered.size(), strike.size()); ++i) {
                covered[i] = std::max(covered[i], strike[i]);
            }
        }
        return covered;
    };
    QHash<QString, QList<double>> atOne;
    for (const QString &name : names) {
        const QList<double> one = glyphCoverage(name, 16);
        QCOMPARE(one.size(), 16 * 16);
        atOne.insert(name, one);
        int whole = 0;
        int between = 0;
        QRect ink;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const double covered = one[y * 16 + x];
                whole += covered >= 0.9 ? 1 : 0;
                between += covered > 0.1 && covered < 0.9 ? 1 : 0;
                if (covered > 0.1) {
                    ink = ink.united(QRect(x, y, 1, 1));
                }
            }
        }
        // Whole pixels of ink at 1x, and antialiased ones: none is a grid
        // of squares, and none a blur (the offline glyph's arcs have the
        // fewest whole pixels).
        QVERIFY2(whole >= 20 && between >= 8,
                 qPrintable(QStringLiteral("%1: %2 whole, %3 between").arg(name).arg(whole).arg(between)));
        // All drawn for the same 16 px cell, within a pixel of its edges,
        // and 14 px wide.
        QVERIFY2(ink.left() == 1 && ink.right() == 14 && ink.top() >= 1 && ink.bottom() <= 14,
                 qPrintable(QStringLiteral("%1: ink %2..%3 x %4..%5").arg(name).arg(ink.left()).arg(ink.right())
                                .arg(ink.top()).arg(ink.bottom())));
        // Sharp at 2x: the outline's edges still on whole pixels, so most
        // of its ink is whole.
        const QList<double> two = glyphCoverage(name, 32);
        int wholeAtTwo = 0;
        int betweenAtTwo = 0;
        for (const double covered : two) {
            wholeAtTwo += covered >= 0.9 ? 1 : 0;
            betweenAtTwo += covered > 0.1 && covered < 0.9 ? 1 : 0;
        }
        QVERIFY2(wholeAtTwo >= 3 * whole && betweenAtTwo < wholeAtTwo,
                 qPrintable(QStringLiteral("%1 at 2x: %2 whole, %3 between").arg(name).arg(wholeAtTwo).arg(betweenAtTwo)));
    }
    // The "as" keeps its rows, 3 to 12, and has no strike.
    const QList<double> as = atOne.value(names[0]);
    for (int i = 0; i < 16; ++i) {
        for (const int row : {0, 1, 2, 13, 14, 15}) {
            QVERIFY2(as[row * 16 + i] <= 0.1, qPrintable(QStringLiteral("%1,%2").arg(i).arg(row)));
        }
    }
    // The strike of the other two: a whole pixel in each row along the
    // diagonal, half a pixel on either side of it, the same in both; then
    // clear space in offline's, while paused's is apart by its colour, and
    // clear of the letters only under it and in a narrow casing.
    atOne.insert(names[1], coverage(QStringLiteral("lastfm-strike.svg"), 16));
    for (const QString &name : {names[1], names[2]}) {
        const QList<double> struck = atOne.value(name);
        for (int i = 2; i <= 13; ++i) {
            const QString at = QStringLiteral("%1 at %2").arg(name).arg(i);
            QVERIFY2(struck[i * 16 + i] >= 0.9, qPrintable(at));
            QVERIFY2(std::abs(struck[i * 16 + i - 1] - 0.5) <= 0.15 && std::abs(struck[i * 16 + i + 1] - 0.5) <= 0.15,
                     qPrintable(at));
            if (i >= 3 && i <= 12) {
                QVERIFY2(struck[i * 16 + i - 2] <= 0.35 && struck[i * 16 + i + 2] <= 0.35, qPrintable(at));
            }
        }
    }
    for (int i = 1; i <= 14; ++i) {
        for (const int offset : {-1, 0, 1}) {
            if (i + offset < 0 || i + offset > 15) {
                continue;
            }
            QVERIFY2(std::abs(atOne.value(names[1])[i * 16 + i + offset] - atOne.value(names[2])[i * 16 + i + offset])
                         <= 0.05,
                     qPrintable(QStringLiteral("strikes differ at %1%2").arg(i).arg(offset)));
        }
    }
}

// "Bridge disconnected" was the developer's word for it: the status bar
// says the playback engine isn't running, and its tooltip what to do. It is
// an error, in the status bar's one treatment of them (a stopped engine was
// red semibold words, one not running plain words with no glyph): the words
// in the text colour, at the status bar's weight, after a 16 px warning
// glyph in the negative colour.
void TransportStatusTest::statusBarNamesAStoppedEngineInPlainWords() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *label = harness.item(QStringLiteral("statusBarDisconnectedLabel"));
    QQuickItem *state = harness.item(QStringLiteral("statusBarPlaybackState"));
    QQuickItem *glyph = harness.item(QStringLiteral("statusBarEngineErrorGlyph"));
    QQuickItem *stateLabel = harness.item(QStringLiteral("statusBarPlaybackStateLabel"));
    QVERIFY(label && state && glyph && stateLabel);
    QVERIFY(!label->isVisible());
    QVERIFY(!glyph->isVisible());
    const qreal stateLeft = sceneRect(state).left();
    harness.bridge->setProperty("connected", false);
    QTRY_VERIFY(label->isVisible());
    QVERIFY(!state->isVisible());
    const auto treatedAsAnError = [&](QQuickItem *words) {
        return glyph->isVisible() && glyph->size() == QSizeF(16, 16)
            && glyph->property("source").toString().endsWith(QStringLiteral("status-warning.svg"))
            && sameColor(glyph->property("color").value<QColor>(), harness.token("negative"))
            && sameColor(words->property("color").value<QColor>(), harness.token("text"))
            && words->property("font").value<QFont>().weight() == QFont::Normal
            && words->property("font").value<QFont>() == stateLabel->property("font").value<QFont>()
            // The glyph where the playback state's glyph was, the words 6 px after it.
            && sceneRect(glyph).left() == stateLeft
            && sceneRect(words).left() - sceneRect(glyph).right() == 6.0
            && std::abs(sceneRect(glyph).center().y() - sceneRect(words).center().y()) <= 0.5;
    };
    QTRY_VERIFY(treatedAsAnError(label));
    QCOMPARE(label->property("text").toString(), QStringLiteral("Playback engine not running"));
    const QString tip = QStringLiteral(
        "Ferrous can't play music until its playback engine runs. Restart Ferrous to try again.");
    QCOMPARE(toolTipText(label), tip);
    QCOMPARE(accessibleText(label, QAccessible::Description), tip);
    showToolTipsAtOnce(label);
    QTest::mouseMove(harness.window, sceneCentre(label));
    QTRY_VERIFY(toolTipShown(label));
    // A failure the backend reported says so instead, in the same words
    // (it said "Backend stopped").
    QQuickItem *stopped = harness.item(QStringLiteral("statusBarBackendStoppedLabel"));
    QVERIFY(stopped != nullptr);
    QVERIFY(!stopped->isVisible());
    harness.bridge->setProperty("backendFailure", QStringLiteral("stopped for testing"));
    QTRY_VERIFY(!label->isVisible());
    QTRY_VERIFY(stopped->isVisible());
    QCOMPARE(stopped->property("text").toString(), QStringLiteral("Playback engine stopped"));
    QTRY_VERIFY(treatedAsAnError(stopped));
    // Its tooltip says why, in the backend's words.
    QCOMPARE(toolTipText(stopped), QStringLiteral("stopped for testing"));
    QCOMPARE(accessibleText(stopped, QAccessible::Description), QStringLiteral("stopped for testing"));
    showToolTipsAtOnce(stopped);
    QTest::mouseMove(harness.window, sceneCentre(stopped));
    QTRY_VERIFY(toolTipShown(stopped));
    QVERIFY2(contrastRatio(harness.token("negative"), harness.token("chrome")) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("glyph"), harness.token("negative"), harness.token("chrome"))));
}

void TransportStatusTest::lastFmSlotToolTipShowsOnKeyboardFocus() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QQuickItem *background = harness.item(QStringLiteral("scrobbleIndicatorBackground"));
    QVERIFY(slot && background);
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    QVERIFY(slot->activeFocusOnTab());
    QVERIFY(!toolTipShown(slot));
    showToolTipsAtOnce(slot);
    slot->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(toolTipShown(slot));
    QCOMPARE(evaluateOn(background, QStringLiteral("border.width")).toInt(), 2);
    QVERIFY(sameColor(evaluateOn(background, QStringLiteral("border.color")).value<QColor>(),
                      harness.token("focusRing")));
    // Space pauses scrobbling, not playback.
    QTest::keyClick(harness.window, Qt::Key_Space);
    QTRY_VERIFY(harness.bridge->property("lastFmScrobblingPaused").toBool());
    QCOMPARE(harness.root->property("playPauseCount").toInt(), 0);
}

void TransportStatusTest::lastFmKeyboardMenuOpensAboveTheBar_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<int>("key");
    QTest::newRow("Shift+F10") << 10.0 << int(Qt::Key_F10);
    QTest::newRow("Menu") << 10.0 << int(Qt::Key_Menu);
    QTest::newRow("Shift+F10, 24 pt") << 24.0 << int(Qt::Key_F10);
}

// The Last.fm menu opened from the keyboard is in the window, above the
// status bar and on the indicator's right edge (it opened under the bar,
// out of the window). The pointer's stays a window of its own at the
// pointer.
void TransportStatusTest::lastFmKeyboardMenuOpensAboveTheBar() {
    QFETCH(qreal, pointSize);
    QFETCH(int, key);
    const ScopedApplicationFont font(pointSize);
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    // The status bar at the window's bottom, as in the app, with room over it.
    harness.window->setHeight(700);
    QTRY_COMPARE(harness.window->contentItem()->height(), 700.0);
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QQuickItem *slot = harness.item(QStringLiteral("scrobbleIndicator"));
    QObject *menu = harness.root->findChild<QObject *>(QStringLiteral("scrobbleIndicatorMenu"));
    QVERIFY(status && slot && menu);
    status->setParentItem(harness.window->contentItem());
    status->setPosition(QPointF(0, 700 - status->height()));
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 120000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QTRY_VERIFY(slot->isVisible());
    const auto openFromKeys = [&]() {
        slot->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(harness.window, Qt::Key(key),
                        key == Qt::Key_F10 ? Qt::ShiftModifier : Qt::NoModifier);
    };
    auto *panel = menu->property("background").value<QQuickItem *>();
    QVERIFY(panel != nullptr);
    for (int opening = 0; opening < 2; ++opening) {
        openFromKeys();
        QTRY_VERIFY(menu->property("opened").toBool());
        QCOMPARE(panel->window(), harness.window);
        QRectF at;
        QTRY_VERIFY2((at = sceneRect(panel), std::abs(at.right() - sceneRect(slot).right()) <= 1.0),
                     qPrintable(describeRect(at)));
        QVERIFY2(at.bottom() <= sceneRect(status).top() && at.bottom() >= sceneRect(status).top() - 6,
                 qPrintable(describeRect(at)));
        QVERIFY2(at.top() >= 0 && at.left() >= 0 && at.right() <= harness.window->width(),
                 qPrintable(describeRect(at)));
        // The keys work in it, and Escape closes it.
        QTest::keyClick(harness.window, Qt::Key_Down);
        QTRY_VERIFY(menu->property("currentIndex").toInt() >= 0);
        QTest::keyClick(harness.window, Qt::Key_Escape);
        QTRY_VERIFY(!menu->property("visible").toBool());
    }
    // The pointer's, after that: a window of its own, at the pointer.
    const QPoint pressed = sceneCentre(slot);
    QTest::mouseClick(harness.window, Qt::RightButton, Qt::NoModifier, pressed);
    QTRY_VERIFY(menu->property("opened").toBool());
    QCOMPARE(menu->property("popupType").toInt(), 1); // Popup.Window
    auto *anchor = menu->property("pointAnchor").value<QQuickItem *>();
    QVERIFY(anchor != nullptr);
    QCOMPARE(menu->property("parent").value<QQuickItem *>(), anchor);
    QVERIFY(std::abs(anchor->mapToScene(QPointF()).x() - pressed.x()) <= 1.0
            && std::abs(anchor->mapToScene(QPointF()).y() - pressed.y()) <= 1.0);
    QVERIFY(QMetaObject::invokeMethod(menu, "close"));
    QTRY_VERIFY(!menu->property("visible").toBool());
}

void TransportStatusTest::issuesButtonShowsNewSeenAndOpen_data() {
    addThemeRows();
}

void TransportStatusTest::issuesButtonShowsNewSeenAndOpen() {
    QFETCH(bool, dark);
    Harness harness;
    QVERIFY2(harness.load(breeze(dark)), qPrintable(harness.error));
    QQuickItem *button = harness.item(QStringLiteral("statusBarErrorsButton"));
    QQuickItem *glyph = harness.item(QStringLiteral("statusBarErrorsGlyph"));
    QQuickItem *label = harness.item(QStringLiteral("statusBarErrorsLabel"));
    QQuickItem *background = harness.item(QStringLiteral("statusBarErrorsBackground"));
    QQuickItem *status = harness.item(QStringLiteral("statusBar"));
    QVERIFY(button && glyph && label && background && status);
    const QColor chrome = harness.token("chrome");
    QVERIFY(!button->isVisible());

    // One old issue, seen; then two new ones.
    QVERIFY(harness.reportIssue(1, 0, QStringLiteral("old")));
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "markSeen"));
    QVERIFY(harness.reportIssue(1, 0, QStringLiteral("first")));
    QVERIFY(harness.reportIssue(2, 1, QStringLiteral("second")));
    QTRY_VERIFY(button->isVisible());
    QCOMPARE(label->property("text").toString(), QStringLiteral("2 new issues"));
    // The status bar's one treatment of an error: a 16 px glyph in the
    // negative colour and the words in the text colour.
    QCOMPARE(glyph->size(), QSizeF(16, 16));
    // An outlined triangle: Breeze's filled one loses its "!" as a mask.
    QVERIFY(glyph->property("source").toString().endsWith(QStringLiteral("status-warning.svg")));
    QCOMPARE(button->height(), 22.0);
    const QColor negative = harness.token("negative");
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), negative));
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("text")));
    QVERIFY2(contrastRatio(negative, chrome) >= 3.0, qPrintable(describeContrast(QStringLiteral("new glyph"), negative, chrome)));
    // 6 px from the status bar's end.
    QTRY_COMPARE(sceneRect(status).right() - sceneRect(button).right(), 6.0);

    // Keyboard focus shows the tooltip, as hover does.
    showToolTipsAtOnce(button);
    button->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(toolTipShown(button));
    QCOMPARE(toolTipText(button), QStringLiteral("Show Recent Issues"));
    QCOMPARE(evaluateOn(background, QStringLiteral("border.width")).toInt(), 2);

    // Open: the same on a text-10% tint, on which the text keeps 4.5:1.
    // Space presses the focused button; it does not play or pause (#587).
    QSignalSpy requested(harness.errors, SIGNAL(historyRequested()));
    QTest::keyClick(harness.window, Qt::Key_Space);
    QTRY_COMPARE(requested.count(), 1);
    QCOMPARE(harness.root->property("playPauseCount").toInt(), 0);
    harness.root->property("statusBar").value<QObject *>()->setProperty("issuesOpen", true);
    SteppedAnimationDriver animations;
    QVERIFY(stepUntil(animations, [&] {
        return sameColor(background->property("color").value<QColor>(), withAlpha(harness.token("text"), 0.10));
    }));
    QCOMPARE(label->property("text").toString(), QStringLiteral("2 new issues"));
    const QColor onTint = drawnOver(withAlpha(harness.token("text"), 0.10), chrome);
    QVERIFY2(contrastRatio(harness.token("text"), onTint) >= 4.5,
             qPrintable(describeContrast(QStringLiteral("issues on the tint"), harness.token("text"), onTint)));
    QVERIFY2(contrastRatio(negative, onTint) >= 3.0,
             qPrintable(describeContrast(QStringLiteral("glyph on the tint"), negative, onTint)));

    // Seen: "3 issues" in the secondary colour.
    QVERIFY(QMetaObject::invokeMethod(harness.errors, "markSeen"));
    harness.root->property("statusBar").value<QObject *>()->setProperty("issuesOpen", false);
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("3 issues"));
    QCOMPARE(glyph->size(), QSizeF(16, 16));
    QVERIFY(sameColor(glyph->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY(sameColor(label->property("color").value<QColor>(), harness.token("textSecondary")));
    QVERIFY2(contrastRatio(harness.token("textSecondary"), chrome) >= 4.5,
             qPrintable(describeContrast(QStringLiteral("seen issues"), harness.token("textSecondary"), chrome)));

    // A repeat is new again.
    QVERIFY(harness.reportIssue(1, 0, QStringLiteral("old")));
    QTRY_COMPARE(label->property("text").toString(), QStringLiteral("1 new issue"));
}

// Groups are set apart by space alone: no "|" labels, no 1 px rules other
// than the seam along the top.
void TransportStatusTest::statusBarHasNoDividers() {
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    harness.bridge->setProperty("repeatMode", 1);
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    harness.bridge->setProperty("lastFmScrobbleState", 1);
    QVERIFY(harness.reportIssue(1, 0, QStringLiteral("issue")));
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
    const ScopedApplicationFont font(24);
    Harness harness;
    QVERIFY2(harness.load(), qPrintable(harness.error));
    QCOMPARE(harness.tokens->property("basePointSize").toReal(), 24.0);

    // What is drawn at a size at the default 10 pt font, grown with the
    // font, on whole pixels.
    const auto grown = [](qreal size) { return std::round(size * 24.0 / 10.0); };

    // The transport: the glyphs grown from the default font's, each inside
    // its button, the buttons side by side inside the band.
    QQuickItem *transport = harness.item(QStringLiteral("transportBar"));
    QVERIFY(transport != nullptr);
    const QRectF band = sceneRect(transport);
    const QStringList names{QStringLiteral("previous"), QStringLiteral("playPause"), QStringLiteral("stop"),
                            QStringLiteral("next")};
    const QList<int> glyphs{int(grown(16)), int(grown(20)), int(grown(16)), int(grown(16))};
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
    // The seek waveform and the volume's thumb and groove grow as the glyphs
    // do (they stayed 24 and 12 px beside 48 px glyphs), the seek bar 2 px
    // past the waveform on either side, and both inside the band.
    QQuickItem *waveform = harness.item(QStringLiteral("seekWaveform"));
    QQuickItem *seek = harness.item(QStringLiteral("seekSlider"));
    QQuickItem *thumb = harness.item(QStringLiteral("volumeThumb"));
    QQuickItem *groove = harness.item(QStringLiteral("volumeGroove"));
    QVERIFY(waveform && seek && thumb && groove);
    QCOMPARE(waveform->height(), grown(24));
    QCOMPARE(seek->height(), waveform->height() + 4);
    QCOMPARE(sceneRect(waveform).center().y(), sceneRect(seek).center().y());
    QCOMPARE(thumb->size(), QSizeF(grown(12), grown(12)));
    QCOMPARE(groove->height(), grown(3));
    QVERIFY(std::abs(sceneRect(thumb).center().y() - sceneRect(groove).center().y()) <= 0.5);
    for (QQuickItem *item : {waveform, thumb}) {
        QVERIFY2(within(sceneRect(item), band), qPrintable(item->objectName() + QLatin1Char(' ') + describeRect(sceneRect(item))));
    }
    QVERIFY(sceneRect(harness.item(QStringLiteral("seekSlider"))).left() >= previousRight);
    QVERIFY(sceneRect(harness.item(QStringLiteral("muteButton"))).left()
            >= sceneRect(harness.item(QStringLiteral("transportTime"))).right());

    // The status bar: its text and glyphs within the bar under the seam,
    // and centred in it.
    harness.bridge->setProperty("lastFmScrobbleThresholdMs", 115000);
    harness.bridge->setProperty("lastFmScrobbleState", 6);
    harness.bridge->setProperty("lastFmScrobbleIneligibility", 3);
    QVERIFY(harness.reportIssue(1, 0, QStringLiteral("first")));
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
    // 11 px and the issues' 16 px.
    QCOMPARE(harness.item(QStringLiteral("statusBarPlaybackGlyph"))->size(), QSizeF(grown(11), grown(11)));
    QCOMPARE(harness.item(QStringLiteral("statusBarErrorsGlyph"))->size(), QSizeF(grown(16), grown(16)));
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
    // The basic render loop, whatever the environment asks for: the threaded
    // one installs an animation driver of its own for the GUI thread, and Qt
    // then takes no SteppedAnimationDriver.
    qputenv("QSG_RENDER_LOOP", "basic");
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
    returnFocusFromClosedPopups();
    QAccessible::setActive(true);
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    InputModality::registerQmlTypes();
    StandardKeys::registerQmlTypes();
    TransportStatusTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_transport_status.moc"
