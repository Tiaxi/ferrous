// SPDX-License-Identifier: GPL-3.0-or-later

// The design tokens of UiPalette and the shared components that the main
// window, the visualization and the dialogs are built from (#373): the row
// background, the region header, the plot plate and the plot tool button.

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QStandardPaths>
#include <QQuickWindow>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

#include "TestColor.h"
#include "TestMainWindow.h"
#include "TestMessages.h"
#include "TestMotion.h"
#include "TestPlatform.h"
#include "TestSessionIsolation.h"

namespace {

// The colours of a Breeze scheme that UiPalette reads, from Breeze's
// .colors files: the window and view backgrounds, the text, the inactive
// text, the selection and the negative text.
struct Scheme {
    QString window;
    QString view;
    QString text;
    QString inactiveText;
    QString highlight;
    QString negative;
};

const Scheme kBreezeDark{QStringLiteral("#202326"), QStringLiteral("#141618"), QStringLiteral("#fcfcfc"),
                         QStringLiteral("#a1a9b1"), QStringLiteral("#3daee9"), QStringLiteral("#da4453")};
const Scheme kBreezeLight{QStringLiteral("#eff0f1"), QStringLiteral("#ffffff"), QStringLiteral("#232629"),
                          QStringLiteral("#707d8a"), QStringLiteral("#3daee9"), QStringLiteral("#da4453")};

// A window in `scheme`, with a UiPalette `tokens` and `items` in a themed
// item that fills it.
QByteArray themedWindowQml(const Scheme &scheme, const QString &items) {
    return QStringLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components

ApplicationWindow {
    id: window
    width: 480
    height: 320
    visible: true
    palette.window: "%1"
    palette.base: "%2"
    palette.text: "%3"
    palette.windowText: "%3"
    property alias tokens: tokens

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

        %7
    }
}
)QML")
        .arg(scheme.window, scheme.view, scheme.text, scheme.inactiveText, scheme.highlight, scheme.negative,
             items)
        .toUtf8();
}

QString formatErrors(const QList<QQmlError> &errors) {
    QStringList lines;
    for (const QQmlError &error : errors) {
        lines << error.toString();
    }
    return lines.join(QLatin1Char('\n'));
}

std::unique_ptr<QObject> createWindow(QQmlEngine &engine, const QByteArray &qml, QString *errorText) {
    QQmlComponent component(&engine);
    component.setData(qml, QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR "/qml/FoundationsHarness.qml")));
    std::unique_ptr<QObject> object(component.create());
    if (!object) {
        *errorText = formatErrors(component.errors());
    }
    return object;
}

QColor colorOf(const QObject *object, const char *name) {
    return object->property(name).value<QColor>();
}

QColor fromHex(const QString &hex) {
    return QColor::fromString(hex);
}

// An opaque colour in CIELAB, under D65.
std::array<double, 3> labOf(const QColor &color) {
    const auto linear = [](double channel) {
        return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
    };
    const double r = linear(color.redF());
    const double g = linear(color.greenF());
    const double b = linear(color.blueF());
    const double x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047;
    const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    const double z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
    const auto f = [](double t) {
        return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0;
    };
    return std::array<double, 3>{116.0 * f(y) - 16.0, 500.0 * (f(x) - f(y)), 200.0 * (f(y) - f(z))};
}

// The CIE76 distance of two opaque colours in CIELAB.
double labDistance(const QColor &a, const QColor &b) {
    const std::array<double, 3> la = labOf(a);
    const std::array<double, 3> lb = labOf(b);
    return std::hypot(la[0] - lb[0], la[1] - lb[1], la[2] - lb[2]);
}

// The CIELAB chroma of an opaque colour: 0 for a neutral grey.
double labChroma(const QColor &color) {
    const std::array<double, 3> lab = labOf(color);
    return std::hypot(lab[1], lab[2]);
}

QQuickItem *itemNamed(QObject *root, const QString &name) {
    return root->findChild<QQuickItem *>(name);
}

// The named child of `item`'s subtree.
QQuickItem *childNamed(QQuickItem *item, const QString &name) {
    return item->findChild<QQuickItem *>(name);
}

// The plot tool button tests' icon, found by name as the app's Breeze icons
// are, and drawn like them: an SVG whose shape takes the text colour of
// the colour scheme, which KDE's icon loader replaces with the colour asked
// for. It is a 10 px square in the middle of 16 px, in hicolor, which every
// icon theme falls back to, under a data directory of the test's own; the
// middle of the square is the glyph colour whatever the icon theme.
bool installIconFixture(const QString &dataDirectory) {
    const QDir icons(dataDirectory);
    if (!icons.mkpath(QStringLiteral("icons/hicolor/16x16/actions"))) {
        return false;
    }
    QFile icon(icons.filePath(QStringLiteral("icons/hicolor/16x16/actions/ferrous-fixture-square.svg")));
    if (!icon.open(QIODevice::WriteOnly)) {
        return false;
    }
    icon.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\">\n"
               "<style type=\"text/css\" id=\"current-color-scheme\">"
               ".ColorScheme-Text { color: #232629; }</style>\n"
               "<rect x=\"3\" y=\"3\" width=\"10\" height=\"10\" class=\"ColorScheme-Text\""
               " style=\"fill:currentColor\"/>\n"
               "</svg>\n");
    icon.close();
    const QByteArray dataDirs = qgetenv("XDG_DATA_DIRS");
    qputenv("XDG_DATA_DIRS", QFile::encodeName(dataDirectory) + ':'
                                 + (dataDirs.isEmpty() ? QByteArray("/usr/local/share:/usr/share") : dataDirs));
    return true;
}

// The device pixels per logical pixel of a window grab. A grab under a
// scale factor (QT_SCALE_FACTOR) is larger than the window but can report a
// device pixel ratio of 1, so the scale comes from the sizes.
qreal grabScale(const QImage &image, const QQuickWindow *window) {
    return window->width() > 0 ? static_cast<qreal>(image.width()) / window->width() : 1.0;
}

QColor pixelAt(const QImage &image, const QPointF &point) {
    return image.pixelColor(static_cast<int>(std::floor(point.x())), static_cast<int>(std::floor(point.y())));
}

} // namespace

class UiFoundationsTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void drawsOffscreenUnlessAPlatformIsChosen();
    void tokensFollowTheBreezeColours_data();
    void tokensFollowTheBreezeColours();
    void tokensMeetTheirContrast_data();
    void tokensMeetTheirContrast();
    void plotColorsFollowThePlot_data();
    void plotColorsFollowThePlot();
    void listRowHoverStandsOutAndApart_data();
    void listRowHoverStandsOutAndApart();
    void selectionOutweighsRowHover_data();
    void selectionOutweighsRowHover();
    void hoverAndPressedTintsOnAnotherBackground_data();
    void hoverAndPressedTintsOnAnotherBackground();
    void selectedQueueRowAnswersHover_data();
    void selectedQueueRowAnswersHover();
    void unreadableMarkReadsOnEveryRowBackground_data();
    void unreadableMarkReadsOnEveryRowBackground();
    void rowBackgroundShowsEachState_data();
    void rowBackgroundShowsEachState();
    void rowBackgroundDrawsTheRingAndTintAsOneRectangle();
    void regionHeaderLaysOutNameTotalsAndHairline();
    void regionHeaderGivesUpTheTotalsBeforeTheName_data();
    void regionHeaderGivesUpTheTotalsBeforeTheName();
    void plotPlateLabelHasAnEdgeOutside();
    void smallControlsScaleWithALargeFont_data();
    void smallControlsScaleWithALargeFont();
    void plotSegmentHoverStaysUnderTheChosenPlate_data();
    void plotSegmentHoverStaysUnderTheChosenPlate();
    void viewerToolButtonHoverReadsOnThePlot_data();
    void viewerToolButtonHoverReadsOnThePlot();
    void viewerToolButtonShowsAKeyPressAtOnce_data();
    void viewerToolButtonShowsAKeyPressAtOnce();
    void viewerWaveformLaneSeamReadsOnWhite_data();
    void viewerWaveformLaneSeamReadsOnWhite();
    void viewerToolButtonCheckedToggleAnswersHover_data();
    void viewerToolButtonCheckedToggleAnswersHover();
    void plotToolButtonDrawsCheckedAndFocusApart_data();
    void plotToolButtonDrawsCheckedAndFocusApart();
    void plotToolButtonIsAnAccessibleCheckableButton();
    void activeSelectionIsUnmistakableInDark_data();
    void activeSelectionIsUnmistakableInDark();
};

void UiFoundationsTest::initTestCase() {
    startCapturingMessages();
    QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                     QStringLiteral("icons/breeze/index.theme"))
                  .isEmpty(),
             "The Breeze icon theme is missing: install breeze-icon-theme (docs/INSTALL.md)");
}

void UiFoundationsTest::cleanup() {
    const QStringList list = takeCapturedMessages();
    QVERIFY2(list.isEmpty(), qPrintable(list.join(QLatin1Char('\n'))));
}

void UiFoundationsTest::drawsOffscreenUnlessAPlatformIsChosen() {
    verifyOffscreenPlatformByDefault();
}

void UiFoundationsTest::tokensFollowTheBreezeColours_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The tokens are mixed from the theme, and Breeze produces the colours of
// the design within rounding.
void UiFoundationsTest::tokensFollowTheBreezeColours() {
    QFETCH(bool, dark);
    const Scheme &scheme = dark ? kBreezeDark : kBreezeLight;
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(scheme, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);
    QCOMPARE(tokens->property("themeIsDark").toBool(), dark);

    struct Expected {
        const char *token;
        QString darkHex;
        QString lightHex;
    };
    const QList<Expected> expected{
        {"chrome", QStringLiteral("#202326"), QStringLiteral("#eff0f1")},
        {"view", QStringLiteral("#141618"), QStringLiteral("#ffffff")},
        {"text", QStringLiteral("#fcfcfc"), QStringLiteral("#232629")},
        {"textSecondary", QStringLiteral("#aeb7bd"), QStringLiteral("#4f5a63")},
        // Lightened or darkened from the mix to be 4.6:1 on a hovered row.
        {"textTertiary", QStringLiteral("#91989f"), QStringLiteral("#59626d")},
        // Light: darkened from #5a5c5e to be 4.5:1 on a selected queue row
        // under the pointer.
        {"textColumn", QStringLiteral("#c2c2c3"), QStringLiteral("#555759")},
        // The theme's placeholder colour, darkened on light to be 4.6:1 on
        // the view, which Breeze Light's 4.2:1 was not.
        {"placeholderText", QStringLiteral("#a1a9b1"), QStringLiteral("#697582")},
        {"seam", QStringLiteral("#383b40"), QStringLiteral("#d6d9dc")},
        {"hairline", QStringLiteral("#26292c"), QStringLiteral("#ebedef")},
        {"chromeLine", QStringLiteral("#383b40"), QStringLiteral("#c9cdd1")},
        // Light: 38 %, 1.4:1 on the view, past a hovered row.
        {"selection", QStringLiteral("#21475b"), QStringLiteral("#b5e0f7")},
        {"focusRing", QStringLiteral("#6cc5f2"), QStringLiteral("#1a74a8")},
        {"link", QStringLiteral("#3daee9"), QStringLiteral("#1b73a1")},
        {"negative", QStringLiteral("#ed5a6a"), QStringLiteral("#c4293b")},
        {"wave", QStringLiteral("#b9e1f5"), QStringLiteral("#2c566c")},
        // Light: darkened from #955500 to be 4.5:1 on a hovered list row,
        // on the selection (#864c00 on the 1.44:1 hover), and on a selected
        // queue row under the pointer.
        {"now", QStringLiteral("#f2b035"), QStringLiteral("#814900")},
    };
    for (const Expected &entry : expected) {
        const QColor actual = colorOf(tokens, entry.token);
        const QColor wanted = fromHex(dark ? entry.darkHex : entry.lightHex);
        QVERIFY2(actual.alphaF() == 1.0f, entry.token);
        QVERIFY2(channelDistance(actual, wanted) <= 2,
                 qPrintable(QStringLiteral("%1 is %2, not %3")
                                .arg(QLatin1String(entry.token), actual.name(), wanted.name())));
    }

    // The tokens given as a mix rather than as a colour.
    const QColor text = fromHex(scheme.text);
    const QColor view = fromHex(scheme.view);
    const QColor accent = fromHex(scheme.highlight);
    const QColor hover = colorOf(tokens, "hover");
    QCOMPARE(channelDistance(hover, text), 0);
    // The hover tint: the least of the text, in whole percent, that is
    // 1.3:1 on the view.
    QVERIFY2(std::abs(hover.alphaF() - (dark ? 0.10 : 0.14)) < 0.002, qPrintable(QString::number(hover.alphaF())));
    QVERIFY(contrastRatio(drawnOver(hover, view), view) >= 1.3);
    QColor lighterHover = hover;
    lighterHover.setAlphaF(hover.alphaF() - 0.01f);
    QVERIFY(contrastRatio(drawnOver(lighterHover, view), view) < 1.3);
    QVERIFY(sameColor(colorOf(tokens, "hoveredRow"), drawnOver(colorOf(tokens, "rowHover"), view)));
    // The inactive selection: the view mixed with the accent, at 60 % of
    // that mix's saturation and the same luminance.
    {
        const QColor mix = mixed(view, accent, dark ? 0.24 : 0.33);
        const QColor inactive = colorOf(tokens, "selectionInactive");
        QVERIFY2(std::abs(relativeLuminance(inactive) - relativeLuminance(mix)) <= 0.002,
                 qPrintable(inactive.name() + QStringLiteral(" vs ") + mix.name()));
        QVERIFY2(std::abs(inactive.hslSaturationF() - 0.6 * mix.hslSaturationF()) <= 0.02,
                 qPrintable(QStringLiteral("%1 %2 vs %3 %4").arg(inactive.name()).arg(inactive.hslSaturationF())
                                .arg(mix.name()).arg(mix.hslSaturationF())));
        QVERIFY(std::abs(inactive.hslHueF() - mix.hslHueF()) <= 0.01);
        QVERIFY2(channelDistance(inactive, QColor(dark ? QStringLiteral("#283a43") : QStringLiteral("#cde2ed"))) <= 2,
                 qPrintable(inactive.name()));
    }
    const QColor dim = colorOf(tokens, "dim");
    QCOMPARE(channelDistance(dim, QColor(Qt::black)), 0);
    QVERIFY(std::abs(dim.alphaF() - (dark ? 0.65 : 0.25)) < 0.002);

    // Tertiary text steps up to the selected row's own on a selected row.
    QVariant stepped;
    QVERIFY(QMetaObject::invokeMethod(tokens, "textTertiaryOn", Q_RETURN_ARG(QVariant, stepped),
                                      Q_ARG(QVariant, QVariant(true)), Q_ARG(QVariant, QVariant(false))));
    QCOMPARE(stepped.value<QColor>(), colorOf(tokens, "textTertiarySelected"));
    QVariant plain;
    QVERIFY(QMetaObject::invokeMethod(tokens, "textTertiaryOn", Q_RETURN_ARG(QVariant, plain),
                                      Q_ARG(QVariant, QVariant(false)), Q_ARG(QVariant, QVariant(false))));
    QCOMPARE(plain.value<QColor>(), colorOf(tokens, "textTertiary"));

    // The old names keep their colours until their users move.
    QVERIFY2(sameColor(colorOf(tokens, "uiTextColor"), text), qPrintable(colorOf(tokens, "uiTextColor").name(QColor::HexArgb) + QStringLiteral(" vs ") + text.name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(tokens, "uiSurfaceRaisedColor"), view), qPrintable(colorOf(tokens, "uiSurfaceRaisedColor").name(QColor::HexArgb) + QStringLiteral(" vs ") + view.name(QColor::HexArgb)));
}

void UiFoundationsTest::tokensMeetTheirContrast_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<double>("secondaryRatio");
    QTest::addColumn<bool>("secondaryOnChrome");
    QTest::addColumn<double>("tertiaryRatio");
    // The ratios the design lists, on the backgrounds it measured them on.
    QTest::newRow("breeze dark") << true << 7.75 << true << 6.2;
    QTest::newRow("breeze light") << false << 7.1 << false << 6.2;
}

// Text is at least 4.5:1 and graphics 3:1 wherever the design puts them.
void UiFoundationsTest::tokensMeetTheirContrast() {
    QFETCH(bool, dark);
    QFETCH(double, secondaryRatio);
    QFETCH(bool, secondaryOnChrome);
    QFETCH(double, tertiaryRatio);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);

    const QColor view = colorOf(tokens, "view");
    const QColor chrome = colorOf(tokens, "chrome");
    const QColor selection = colorOf(tokens, "selection");
    const QColor selectionInactive = colorOf(tokens, "selectionInactive");
    const QColor hovered = drawnOver(colorOf(tokens, "hover"), view);
    const QColor secondary = colorOf(tokens, "textSecondary");
    const QColor tertiary = colorOf(tokens, "textTertiary");

    // The ratios the design lists.
    const QColor secondaryBackground = secondaryOnChrome ? chrome : view;
    QVERIFY2(std::abs(contrastRatio(secondary, secondaryBackground) - secondaryRatio) <= 0.15,
             qPrintable(describeContrast(QStringLiteral("secondary"), secondary, secondaryBackground)));
    QVERIFY2(std::abs(contrastRatio(tertiary, view) - tertiaryRatio) <= 0.15,
             qPrintable(describeContrast(QStringLiteral("tertiary"), tertiary, view)));

    struct Pair {
        QString what;
        QColor foreground;
        QColor background;
        double minimum;
    };
    const QList<Pair> pairs{
        {QStringLiteral("text on view"), colorOf(tokens, "text"), view, 4.5},
        {QStringLiteral("text on chrome"), colorOf(tokens, "text"), chrome, 4.5},
        {QStringLiteral("secondary on view"), secondary, view, 4.5},
        {QStringLiteral("secondary on chrome"), secondary, chrome, 4.5},
        {QStringLiteral("secondary on a hovered row"), secondary, hovered, 4.5},
        {QStringLiteral("secondary on the selection"), secondary, selection, 4.5},
        {QStringLiteral("secondary on the inactive selection"), secondary, selectionInactive, 4.5},
        {QStringLiteral("tertiary on chrome"), tertiary, chrome, 4.5},
        // A margin over 4.5:1 on the view and a hovered row, so rendering
        // cannot tip the row numbers under it.
        {QStringLiteral("tertiary on view"), tertiary, view, 4.6},
        {QStringLiteral("tertiary on a hovered row"), tertiary, hovered, 4.6},
        {QStringLiteral("column text on view"), colorOf(tokens, "textColumn"), view, 4.5},
        // A field's placeholder carries what saving does ("Multiple
        // values"), so it reads as text, with the margin tertiary has.
        {QStringLiteral("placeholder on view"), colorOf(tokens, "placeholderText"), view, 4.6},
        {QStringLiteral("column text on the selection"), colorOf(tokens, "textColumn"), selection, 4.5},
        {QStringLiteral("text on the selection"), colorOf(tokens, "text"), selection, 4.5},
        {QStringLiteral("playing title on view"), colorOf(tokens, "now"), view, 4.5},
        {QStringLiteral("playing title on a hovered row"), colorOf(tokens, "now"), hovered, 4.5},
        {QStringLiteral("playing title on the selection"), colorOf(tokens, "now"), selection, 4.5},
        {QStringLiteral("playing title on the inactive selection"), colorOf(tokens, "now"), selectionInactive, 4.5},
        {QStringLiteral("link on view"), colorOf(tokens, "link"), view, 4.5},
        // The dialogs' links: About, About KDE and Recent Issues.
        {QStringLiteral("link on chrome"), colorOf(tokens, "link"), chrome, 4.5},
        {QStringLiteral("negative on view"), colorOf(tokens, "negative"), view, 4.5},
        {QStringLiteral("negative on chrome"), colorOf(tokens, "negative"), chrome, 4.5},
        {QStringLiteral("focus ring on the selection"), colorOf(tokens, "focusRing"), selection, 3.0},
        {QStringLiteral("focus ring on view"), colorOf(tokens, "focusRing"), view, 3.0},
    };
    for (const Pair &pair : pairs) {
        QVERIFY2(contrastRatio(pair.foreground, pair.background) >= pair.minimum,
                 qPrintable(describeContrast(pair.what, pair.foreground, pair.background)));
    }

    // The paused amber name of a collapsed tree row that holds the playing
    // track is at least 70% and 4.5:1 on every background a row can have,
    // as the queue's amber title is.
    const double pausedOpacity = tokens->property("nowPausedOpacity").toDouble();
    QVERIFY2(pausedOpacity >= 0.7 && pausedOpacity <= 1.0, qPrintable(QString::number(pausedOpacity)));
    for (const QColor &background : {view, hovered, selection, selectionInactive}) {
        QColor name = colorOf(tokens, "now");
        name.setAlphaF(static_cast<float>(pausedOpacity));
        const QColor drawn = drawnOver(name, background);
        QVERIFY2(contrastRatio(drawn, background) >= 4.5,
                 qPrintable(describeContrast(QStringLiteral("the tree's paused amber name"), drawn, background)));
    }

    // A hovered row stands out from the view at 1.3:1, yet is nearer to it
    // than a selected row is, in lightness and hue together.
    QVERIFY2(contrastRatio(hovered, view) >= 1.3, qPrintable(describeContrast(QStringLiteral("hover tint"), hovered, view)));
    QVERIFY2(labDistance(hovered, view) < 0.9 * labDistance(selection, view),
             qPrintable(QStringLiteral("hover %1 is %2 from the view, selection %3 is %4")
                            .arg(hovered.name())
                            .arg(labDistance(hovered, view))
                            .arg(selection.name())
                            .arg(labDistance(selection, view))));
}

void UiFoundationsTest::plotColorsFollowThePlot_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The spectrogram's plot set is the same in both themes; the waveform's is
// the view's. Everything on a plot stands out from it.
void UiFoundationsTest::plotColorsFollowThePlot() {
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);
    QObject *spectrogram = tokens->property("spectrogramPlot").value<QObject *>();
    QObject *waveform = tokens->property("waveformPlot").value<QObject *>();
    QVERIFY(spectrogram != nullptr && waveform != nullptr);

    QVariant chosen;
    QVERIFY(QMetaObject::invokeMethod(tokens, "plotColors", Q_RETURN_ARG(QVariant, chosen), Q_ARG(QVariant, 0)));
    QCOMPARE(chosen.value<QObject *>(), spectrogram);
    QVERIFY(QMetaObject::invokeMethod(tokens, "plotColors", Q_RETURN_ARG(QVariant, chosen), Q_ARG(QVariant, 1)));
    QCOMPARE(chosen.value<QObject *>(), waveform);

    // The spectrogram's fixed set.
    QVERIFY2(sameColor(colorOf(spectrogram, "plotBg"), fromHex(QStringLiteral("#14062e"))), qPrintable(colorOf(spectrogram, "plotBg").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#000000")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotOutline"), fromHex(QStringLiteral("#000000"))), qPrintable(colorOf(spectrogram, "plotOutline").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#000000")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotLaneSeam"), fromHex(QStringLiteral("#2e2e2e"))), qPrintable(colorOf(spectrogram, "plotLaneSeam").name(QColor::HexArgb) + QStringLiteral(" vs #2e2e2e")));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotText"), fromHex(QStringLiteral("#ececec"))), qPrintable(colorOf(spectrogram, "plotText").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#ececec")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotLink"), fromHex(QStringLiteral("#7cc8f0"))), qPrintable(colorOf(spectrogram, "plotLink").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#7cc8f0")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "meterTrough"), fromHex(QStringLiteral("#30244a"))), qPrintable(colorOf(spectrogram, "meterTrough").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#2b1f43")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "meterFill"), fromHex(QStringLiteral("#c149a0"))), qPrintable(colorOf(spectrogram, "meterFill").name(QColor::HexArgb) + QStringLiteral(" vs #c149a0")));
    QVERIFY2(sameColor(colorOf(spectrogram, "meterPeak"), fromHex(QStringLiteral("#f4f4f6"))), qPrintable(colorOf(spectrogram, "meterPeak").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#f4f4f6")).name(QColor::HexArgb)));
    // A checked button: the accent, its glyph the floor; focus: the dark
    // theme's focus ring.
    QVERIFY(sameColor(colorOf(spectrogram, "checkedFill"), colorOf(tokens, "accent")));
    QVERIFY(sameColor(colorOf(spectrogram, "checkedGlyph"), fromHex(QStringLiteral("#000000"))));
    QVERIFY(channelDistance(colorOf(spectrogram, "plotFocusRing"), fromHex(QStringLiteral("#6cc5f2"))) <= 2);
    QVERIFY(std::abs(colorOf(spectrogram, "plotPlate").alphaF() - 0.88) < 0.002);
    // The plate is a neutral dark lifted from the floor.
    QCOMPARE(channelDistance(colorOf(spectrogram, "plotPlate"), fromHex(QStringLiteral("#141414"))), 0);
    QVERIFY(std::abs(colorOf(spectrogram, "plotPlateEdge").alphaF() - 0.14) < 0.002);
    QVERIFY(std::abs(colorOf(spectrogram, "plotTextSecondary").alphaF() - 0.72) < 0.002);
    QVERIFY(std::abs(colorOf(spectrogram, "plotTick").alphaF() - 0.75) < 0.002);
    QVERIFY(std::abs(colorOf(spectrogram, "meterClipOff").alphaF() - 0.2) < 0.002);
    QVERIFY(sameColor(colorOf(spectrogram, "meterSeam"), fromHex(QStringLiteral("#0a0318"))));
    // On the plot's floor, what is drawn is neutral, not tinted, except the
    // level meter, which is in the colour map's violet.
    for (const char *token : {"plotPlate", "plotStrip", "plotText", "plotTextSecondary", "plotLaneSeam",
                              "plotTick"}) {
        const QColor color = colorOf(spectrogram, token);
        QVERIFY2(color.red() == color.green() && color.green() == color.blue(),
                 qPrintable(QLatin1String(token) + QStringLiteral(" ") + color.name(QColor::HexArgb)));
    }
    // The lane seam and the meter's track stand out from the floor, and the
    // meter's gaps from its track. Its fill is quieter than the plot's text.
    // The seam reads on the floor (at 1.18:1 it did not).
    const double seamContrast = contrastRatio(colorOf(spectrogram, "plotLaneSeam"), colorOf(spectrogram, "plotBg"));
    QVERIFY2(seamContrast >= 1.33, qPrintable(QString::number(seamContrast)));
    // At rest, never played (the lines as drawn in
    // TransportInputSmokeTest::paneAtRestIsAStereoInstrument): the seam
    // between lanes is a cut, the plot's deep violet under the floor, the
    // meter's seam, in the floor's hue rather than the playing seam's grey,
    // and well apart from the rest lines over the floor.
    {
        const QColor floor = colorOf(spectrogram, "plotBg");
        const QColor restSeam = colorOf(tokens, "plotRestLaneSeam");
        QCOMPARE(restSeam.alphaF(), 1.0f);
        QVERIFY(sameColor(restSeam, fromHex(QStringLiteral("#0a0318"))));
        QVERIFY(sameColor(restSeam, colorOf(spectrogram, "meterSeam")));
        QVERIFY2(relativeLuminance(restSeam) < relativeLuminance(floor), qPrintable(restSeam.name()));
        QVERIFY2(restSeam.blue() > restSeam.green() + 10 && restSeam.red() > restSeam.green(), qPrintable(restSeam.name()));
    }
    QVERIFY(contrastRatio(colorOf(spectrogram, "meterTrough"), colorOf(spectrogram, "plotBg")) >= 1.3);
    QVERIFY(contrastRatio(colorOf(spectrogram, "meterTrough"), colorOf(spectrogram, "meterSeam")) >= 1.3);
    QVERIFY(contrastRatio(colorOf(spectrogram, "meterFill"), colorOf(spectrogram, "plotBg"))
            < contrastRatio(colorOf(spectrogram, "plotText"), colorOf(spectrogram, "plotBg")));
    // The meter's rungs, the app icon's harmonic magenta, read on their
    // trough, at 3:1 (the colour map's #c20055 was 2.32:1), and are not the
    // clip lamp's red: some 30 degrees of hue apart.
    {
        const double rungs = contrastRatio(colorOf(spectrogram, "meterFill"), colorOf(spectrogram, "meterTrough"));
        QVERIFY2(rungs >= 3.0, qPrintable(QString::number(rungs)));
        const QColor clip = colorOf(tokens, "negative");
        const int hueApart = std::abs(colorOf(spectrogram, "meterFill").hslHue() - clip.hslHue());
        QVERIFY2(std::min(hueApart, 360 - hueApart) >= 30,
                 qPrintable(QStringLiteral("%1 against the clip lamp's %2")
                                .arg(colorOf(spectrogram, "meterFill").name(), clip.name())));
    }
    // A viewer's title strip is opaque (the playhead showed through it): the
    // plate on black, neutral as the plate is, and the view on the waveform.
    QCOMPARE(colorOf(spectrogram, "plotStrip").alphaF(), 1.0f);
    QVERIFY(channelDistance(colorOf(spectrogram, "plotStrip"),
                            drawnOver(colorOf(spectrogram, "plotPlate"), QColor(Qt::black)))
            <= 1);
    // The album art viewer's set is the spectrogram's on a black floor
    // (the plot's floor is indigo; a cover stays on black).
    QObject *media = tokens->property("mediaPlot").value<QObject *>();
    QVERIFY(media != nullptr && media != spectrogram);
    QVERIFY(sameColor(colorOf(media, "plotBg"), fromHex(QStringLiteral("#000000"))));
    for (const char *token : {"plotPlate", "plotPlateEdge", "plotStrip", "plotText", "plotTextSecondary",
                              "plotLink", "plotFocusRing", "checkedFill", "checkedGlyph"}) {
        QVERIFY2(sameColor(colorOf(media, token), colorOf(spectrogram, token)), token);
    }

    // The waveform's set is the view's.
    const QColor view = colorOf(tokens, "view");
    QVERIFY2(sameColor(colorOf(waveform, "plotBg"), view), qPrintable(colorOf(waveform, "plotBg").name(QColor::HexArgb) + QStringLiteral(" vs ") + view.name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(waveform, "plotOutline"), view), qPrintable(colorOf(waveform, "plotOutline").name(QColor::HexArgb) + QStringLiteral(" vs ") + view.name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(waveform, "plotText"), colorOf(tokens, "text")), qPrintable(colorOf(waveform, "plotText").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "text").name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(waveform, "plotTextSecondary"), colorOf(tokens, "textSecondary")), qPrintable(colorOf(waveform, "plotTextSecondary").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "textSecondary").name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(waveform, "plotPlateEdge"), colorOf(tokens, "seam")), qPrintable(colorOf(waveform, "plotPlateEdge").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "seam").name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(waveform, "plotLaneSeam"), colorOf(tokens, "hairline")), qPrintable(colorOf(waveform, "plotLaneSeam").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "hairline").name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(waveform, "plotLink"), colorOf(tokens, "link")), qPrintable(colorOf(waveform, "plotLink").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "link").name(QColor::HexArgb)));
    QVERIFY(std::abs(colorOf(waveform, "plotPlate").alphaF() - 0.94) < 0.002);
    QVERIFY(sameColor(colorOf(waveform, "plotStrip"), view));
    QVERIFY(sameColor(colorOf(waveform, "meterSeam"), view));
    // The waveform's meter in the waveform's colour: 35 % toward the view,
    // or by 5 % steps less, the most that is 3:1 on the trough; the trough
    // a step off the view that frames the bar, #2b2d2f in dark and #e8e9ea
    // in light (the text at 6 %, 1.2:1, did not show); the marks the text
    // at 30 %; a seam across its top in the seam colour. The spectrogram's
    // meter has no such seam.
    {
        const QColor wave = colorOf(tokens, "paneWave");
        QVERIFY(sameColor(colorOf(waveform, "meterTrough"),
                          fromHex(dark ? QStringLiteral("#2b2d2f") : QStringLiteral("#e8e9ea"))));
        double amount = 0.35;
        while (amount > 0.01 && contrastRatio(mixed(wave, view, amount), colorOf(waveform, "meterTrough")) < 3.0) {
            amount -= 0.05;
        }
        QVERIFY2(amount >= 0.2, qPrintable(QString::number(amount)));
        QVERIFY2(channelDistance(colorOf(waveform, "meterFill"), mixed(wave, view, amount)) <= 1,
                 qPrintable(colorOf(waveform, "meterFill").name()));
        const QColor tick = colorOf(waveform, "meterTick");
        QVERIFY(std::abs(tick.alphaF() - 0.3) < 0.003 && sameColor(QColor(tick.rgb()), QColor(colorOf(tokens, "text").rgb())));
        QVERIFY(std::abs(colorOf(spectrogram, "meterTick").alphaF() - 0.4) < 0.003);
        // Its rungs 3:1 on their trough, as drawn on the view.
        const QColor trough = drawnOver(colorOf(waveform, "meterTrough"), view);
        QVERIFY2(contrastRatio(colorOf(waveform, "meterFill"), trough) >= 3.0,
                 qPrintable(describeContrast(QStringLiteral("rungs on the trough"), colorOf(waveform, "meterFill"), trough)));
        // The pane's waveform: the seek bar's in light; in dark a quieter
        // tone of the same blue, #87bad4, some 8.6:1 on the view rather
        // than the seek bar's 13:1.
        if (dark) {
            QVERIFY2(channelDistance(wave, fromHex(QStringLiteral("#87bad4"))) <= 3, qPrintable(wave.name()));
            QVERIFY(!sameColor(wave, colorOf(tokens, "wave")));
            const double onView = contrastRatio(wave, view);
            QVERIFY2(onView >= 8.0 && onView <= 9.2, qPrintable(QString::number(onView)));
            QVERIFY(onView < contrastRatio(colorOf(tokens, "wave"), view));
        } else {
            QVERIFY(sameColor(wave, colorOf(tokens, "wave")));
        }
        // The trough reads against the view it frames.
        QVERIFY2(contrastRatio(colorOf(waveform, "meterTrough"), view) >= 1.15,
                 qPrintable(QString::number(contrastRatio(colorOf(waveform, "meterTrough"), view))));
        QVERIFY(sameColor(colorOf(waveform, "meterTopSeam"), colorOf(tokens, "seam")));
        QCOMPARE(colorOf(spectrogram, "meterTopSeam").alpha(), 0);
        QCOMPARE(colorOf(media, "meterTopSeam").alpha(), 0);
    }
    // A checked button: the accent in dark, the focus ring's darker blue in
    // light; its glyph the view in dark, white in light.
    QVERIFY(channelDistance(colorOf(waveform, "checkedFill"),
                            fromHex(dark ? QStringLiteral("#3daee9") : QStringLiteral("#1a74a8")))
            <= 2);
    QVERIFY(sameColor(colorOf(waveform, "checkedGlyph"), dark ? view : QColor(Qt::white)));
    QVERIFY(sameColor(colorOf(waveform, "plotFocusRing"), colorOf(tokens, "focusRing")));
    // A chosen choice on a plate, as the mode switch's segment and a
    // viewer's info toggle: the plot's text at 18 % or more inside a 1 px
    // edge that is 3:1 or more on the plate, over the floor, and on an 18 %
    // fill: the dark theme's tertiary text on the spectrogram's dark plot in
    // both themes, and the theme's on the waveform's. The mode switch's
    // fuller fill takes the edge further where it needs it (PlotToolButton's
    // segmentEdgeColor).
    QVERIFY(sameColor(colorOf(spectrogram, "segmentEdge"), fromHex(QStringLiteral("#8f959a"))));
    QVERIFY(sameColor(colorOf(media, "segmentEdge"), fromHex(QStringLiteral("#8f959a"))));
    QVERIFY(sameColor(colorOf(waveform, "segmentEdge"), colorOf(tokens, "textTertiary")));
    for (QObject *plot : {spectrogram, waveform, media}) {
        const QColor plate = drawnOver(colorOf(plot, "plotPlate"), colorOf(plot, "plotBg"));
        QColor text = colorOf(plot, "plotText");
        text.setAlphaF(0.18);
        const QColor fill = drawnOver(text, plate);
        const QColor edge = colorOf(plot, "segmentEdge");
        QVERIFY2(contrastRatio(edge, plate) >= 3.0, qPrintable(describeContrast(QStringLiteral("edge on plate"), edge, plate)));
        QVERIFY2(contrastRatio(edge, fill) >= 3.0, qPrintable(describeContrast(QStringLiteral("edge on fill"), edge, fill)));
        QVERIFY2(contrastRatio(edge, colorOf(plot, "plotBg")) >= 3.0,
                 qPrintable(describeContrast(QStringLiteral("edge on the plot"), edge, colorOf(plot, "plotBg"))));
        QVERIFY(!plot->property("checkedTint").isValid());
    }
    // A viewer's title strip is a plate over the plot: on the spectrogram,
    // its secondary text reads over the colour map's brightest white too.
    {
        const QColor stripOverWhite = drawnOver(colorOf(spectrogram, "plotPlate"), QColor(Qt::white));
        const QColor secondary = drawnOver(colorOf(spectrogram, "plotTextSecondary"), stripOverWhite);
        QVERIFY2(contrastRatio(secondary, stripOverWhite) >= 4.5,
                 qPrintable(describeContrast(QStringLiteral("strip text over white"), secondary, stripOverWhite)));
    }

    for (QObject *plot : {spectrogram, waveform}) {
        const QString name = plot == spectrogram ? QStringLiteral("spectrogram") : QStringLiteral("waveform");
        const QColor floor = colorOf(plot, "plotBg");
        const QColor plate = drawnOver(colorOf(plot, "plotPlate"), floor);
        const QColor trough = drawnOver(colorOf(plot, "meterTrough"), floor);
        const QColor checked = colorOf(plot, "checkedFill");
        QCOMPARE(checked.alphaF(), 1.0f);
        QCOMPARE(colorOf(plot, "plotFocusRing").alphaF(), 1.0f);
        const QList<std::tuple<QString, QColor, QColor, double>> pairs{
            {QStringLiteral("text on the plot"), colorOf(plot, "plotText"), floor, 4.5},
            {QStringLiteral("text on a plate"), colorOf(plot, "plotText"), plate, 4.5},
            {QStringLiteral("secondary text on the plot"), drawnOver(colorOf(plot, "plotTextSecondary"), floor),
             floor, 4.5},
            {QStringLiteral("secondary text on a plate"), drawnOver(colorOf(plot, "plotTextSecondary"), plate),
             plate, 4.5},
            {QStringLiteral("a link on the plot"), colorOf(plot, "plotLink"), floor, 4.5},
            {QStringLiteral("the checked fill on a plate"), checked, plate, 3.0},
            {QStringLiteral("the checked glyph or letter on the fill"), colorOf(plot, "checkedGlyph"), checked, 4.5},
            {QStringLiteral("the focus ring on a plate"), colorOf(plot, "plotFocusRing"), plate, 3.0},
            {QStringLiteral("the meter's fill"), colorOf(plot, "meterFill"), trough, 3.0},
            {QStringLiteral("the meter's peak"), drawnOver(colorOf(plot, "meterPeak"), floor), trough, 3.0},
        };
        for (const auto &[what, foreground, background, minimum] : pairs) {
            QVERIFY2(contrastRatio(foreground, background) >= minimum,
                     qPrintable(describeContrast(name + QStringLiteral(", ") + what, foreground, background)));
        }
    }
}

namespace {

const QString kRowItems = QStringLiteral(R"QML(
        Rectangle {
            anchors.fill: parent
            color: tokens.view
        }

        Item {
            objectName: "row"
            x: 20
            y: 40
            width: 300
            height: tokens.rowHeight

            Components.RowBackground {
                objectName: "rowBackground"
                uiPalette: tokens
            }
        }
)QML");

} // namespace

void UiFoundationsTest::listRowHoverStandsOutAndApart_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The library's and the queue's hover tint is about 1.45:1 on a dark view
// (at 1.3:1 a hovered row was faint across a wide list) and 1.31:1 on a
// light one, where more outweighed the selection: the least of the text in
// whole percent that reaches it, 13% / 14% in Breeze. It stays apart from
// the selection and the inactive selection, and every text a row has, the
// playing row's amber title among them, stays readable on it; tertiary text
// steps up to 4.6:1 there, and only there, where it falls short.
void UiFoundationsTest::listRowHoverStandsOutAndApart() {
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);

    const QColor view = colorOf(tokens, "view");
    const QColor text = colorOf(tokens, "text");
    const QColor rowHover = colorOf(tokens, "rowHover");
    QCOMPARE(channelDistance(rowHover, text), 0);
    QVERIFY2(std::abs(rowHover.alphaF() - (dark ? 0.13 : 0.14)) < 0.002, qPrintable(QString::number(rowHover.alphaF())));
    const QColor hovered = colorOf(tokens, "hoveredRow");
    QVERIFY(sameColor(hovered, drawnOver(rowHover, view)));
    const double target = dark ? 1.44 : 1.31;
    QVERIFY2(contrastRatio(hovered, view) >= target && contrastRatio(hovered, view) < target + 0.06,
             qPrintable(describeContrast(QStringLiteral("row hover"), hovered, view)));
    QColor lighter = rowHover;
    lighter.setAlphaF(rowHover.alphaF() - 0.01f);
    QVERIFY(contrastRatio(drawnOver(lighter, view), view) < target);
    // A step past a button's hover tint on dark; as faint as it on light,
    // never fainter.
    if (dark) {
        QVERIFY(rowHover.alphaF() > colorOf(tokens, "hover").alphaF());
    } else {
        QVERIFY(rowHover.alphaF() >= colorOf(tokens, "hover").alphaF());
    }

    // Apart from the selection, active or not: neutral where they are blue.
    // The inactive one is a quieter blue, nearer the grey, yet further from
    // the view and a clear step apart.
    for (const char *selection : {"selection", "selectionInactive"}) {
        const QColor selected = colorOf(tokens, selection);
        const double apart = qstrcmp(selection, "selection") == 0 ? 10.0 : 7.5;
        QVERIFY2(labDistance(hovered, selected) >= apart,
                 qPrintable(QStringLiteral("hover %1 is %2 from %3 %4")
                                .arg(hovered.name())
                                .arg(labDistance(hovered, selected))
                                .arg(QLatin1String(selection), selected.name())));
    }

    // A row's texts on it; its tertiary figures with the margin that
    // subpixel antialiasing takes from small figures.
    const QColor tertiaryHovered = colorOf(tokens, "textTertiaryHovered");
    const double figureContrast = tokens->property("rowFigureContrast").toDouble();
    QCOMPARE(figureContrast, 5.4);
    const QList<std::pair<const char *, double>> texts{{"text", 4.5},
                                                       {"textColumn", 4.5},
                                                       {"textSecondary", 4.5},
                                                       {"now", 4.5},
                                                       {"textTertiaryHovered", figureContrast}};
    for (const auto &[token, minimum] : texts) {
        QVERIFY2(contrastRatio(colorOf(tokens, token), hovered) >= minimum,
                 qPrintable(describeContrast(QLatin1String(token), colorOf(tokens, token), hovered)));
    }
    // The tertiary colour keeps its step below the secondary elsewhere:
    // only a hovered row's is stronger, where the plain one falls short of
    // the margin on it, as in both Breeze themes; it stays under the text.
    QCOMPARE(sameColor(tertiaryHovered, colorOf(tokens, "textTertiary")),
             contrastRatio(colorOf(tokens, "textTertiary"), hovered) >= figureContrast);
    QVERIFY(!sameColor(tertiaryHovered, colorOf(tokens, "textTertiary")));
    QVERIFY(contrastRatio(tertiaryHovered, hovered) < contrastRatio(text, hovered) - 4);
    QVERIFY2(std::abs(contrastRatio(colorOf(tokens, "textTertiary"), view) - 6.2) <= 0.15,
             qPrintable(describeContrast(QStringLiteral("tertiary"), colorOf(tokens, "textTertiary"), view)));
    const auto tertiaryOn = [tokens](bool selected, bool rowHovered) {
        QVariant color;
        QMetaObject::invokeMethod(tokens, "textTertiaryOn", Q_RETURN_ARG(QVariant, color),
                                  Q_ARG(QVariant, QVariant(selected)), Q_ARG(QVariant, QVariant(rowHovered)));
        return color.value<QColor>();
    };
    QVERIFY(sameColor(tertiaryOn(false, false), colorOf(tokens, "textTertiary")));
    QVERIFY(sameColor(tertiaryOn(false, true), tertiaryHovered));
    QVERIFY(sameColor(tertiaryOn(true, true), colorOf(tokens, "textTertiarySelected")));
}

void UiFoundationsTest::selectionOutweighsRowHover_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// A selected row of the library or the queue reads as more selected than
// the row under the pointer, in both themes: further from the view, in
// contrast and in CIELAB, and blue where the hover is neutral (light's
// 1.44:1 hover outweighed its 1.2:1 selection). The selection of a list
// without focus sits between them in contrast: hover < inactive < active
// (the dark inactive one, 1.40:1, and the light, 1.27:1, were under the
// hover). Every text a selected row draws stays 4.5:1 on it, active or not.
void UiFoundationsTest::selectionOutweighsRowHover() {
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);

    const QColor view = colorOf(tokens, "view");
    const QColor hovered = colorOf(tokens, "hoveredRow");
    const QColor selection = colorOf(tokens, "selection");
    const QColor selectionInactive = colorOf(tokens, "selectionInactive");

    // Weight: the selection is the stronger step off the view.
    QVERIFY2(contrastRatio(selection, view) >= contrastRatio(hovered, view) + 0.05,
             qPrintable(describeContrast(QStringLiteral("selection"), selection, view)
                        + QStringLiteral(" vs ") + describeContrast(QStringLiteral("hover"), hovered, view)));
    for (const QColor &selected : {selection, selectionInactive}) {
        QVERIFY2(labDistance(selected, view) >= 1.15 * labDistance(hovered, view),
                 qPrintable(QStringLiteral("%1 is %2 from the view, hover %3 is %4")
                                .arg(selected.name())
                                .arg(labDistance(selected, view))
                                .arg(hovered.name())
                                .arg(labDistance(hovered, view))));
        // Hue: blue, where the hover is a neutral grey; the inactive one
        // a quieter blue.
        QVERIFY2(labChroma(selected) >= (selected == selection ? 10.0 : 7.0) && labChroma(hovered) < 3.0,
                 qPrintable(QStringLiteral("chroma: %1 %2, hover %3 %4")
                                .arg(selected.name())
                                .arg(labChroma(selected))
                                .arg(hovered.name())
                                .arg(labChroma(hovered))));
    }
    QVERIFY(labDistance(selection, view) > labDistance(selectionInactive, view));
    // The inactive selection reads apart from the active one, not as a
    // second active selection: a visible difference in colour (at the
    // accent's full saturation it was 1.5 ΔE on dark).
    QVERIFY2(labDistance(selection, selectionInactive) >= 5.0,
             qPrintable(QStringLiteral("%1 vs %2: %3").arg(selection.name(), selectionInactive.name())
                            .arg(labDistance(selection, selectionInactive))));
    QVERIFY(labChroma(selectionInactive) < labChroma(selection));
    // In contrast on the view: hover < inactive < active, each a visible
    // step.
    QVERIFY2(contrastRatio(selectionInactive, view) >= contrastRatio(hovered, view) + 0.02
                 && contrastRatio(selection, view) >= contrastRatio(selectionInactive, view) + 0.02,
             qPrintable(describeContrast(QStringLiteral("hover"), hovered, view) + QStringLiteral(", ")
                        + describeContrast(QStringLiteral("inactive"), selectionInactive, view) + QStringLiteral(", ")
                        + describeContrast(QStringLiteral("active"), selection, view)));

    // The texts of a selected row: the title, the secondary and column
    // text, the playing amber, and the selected row's number, which is
    // tertiary stepped up.
    QVariant number;
    QVERIFY(QMetaObject::invokeMethod(tokens, "textTertiaryOn", Q_RETURN_ARG(QVariant, number),
                                      Q_ARG(QVariant, QVariant(true)), Q_ARG(QVariant, QVariant(true))));
    const QList<std::pair<QString, QColor>> texts{
        {QStringLiteral("text"), colorOf(tokens, "text")},
        {QStringLiteral("secondary"), colorOf(tokens, "textSecondary")},
        {QStringLiteral("column"), colorOf(tokens, "textColumn")},
        {QStringLiteral("now"), colorOf(tokens, "now")},
        {QStringLiteral("selected number"), number.value<QColor>()},
    };
    for (const QColor &selected : {selection, selectionInactive}) {
        for (const auto &[what, color] : texts) {
            QVERIFY2(contrastRatio(color, selected) >= 4.5, qPrintable(describeContrast(what, color, selected)));
        }
        // The number with the margin that small figures lose to subpixel
        // antialiasing, and under the text.
        const double figureContrast = tokens->property("rowFigureContrast").toDouble();
        QVERIFY2(contrastRatio(number.value<QColor>(), selected) >= figureContrast,
                 qPrintable(describeContrast(QStringLiteral("selected number"), number.value<QColor>(), selected)));
        // A step under the text, by their own contrast: on dark's stronger
        // selection both are nearer it.
        QVERIFY(contrastRatio(colorOf(tokens, "text"), selected) >= 1.5 * contrastRatio(number.value<QColor>(), selected));
    }
}

void UiFoundationsTest::hoverAndPressedTintsOnAnotherBackground_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<QString>("background");
    QTest::addColumn<int>("hoverPercent");
    QTest::addColumn<int>("pressedPercent");
    // Breeze's Header colours, the transport's.
    QTest::newRow("breeze dark header") << true << QStringLiteral("#292c30") << 9 << 16;
    QTest::newRow("breeze light header") << false << QStringLiteral("#dee0e2") << 15 << 26;
}

// The hover and pressed tints on another background than the view are as
// strong on it as `hover` and `pressed` are on the view: 1.3:1 and 1.65:1.
void UiFoundationsTest::hoverAndPressedTintsOnAnotherBackground() {
    QFETCH(bool, dark);
    QFETCH(QString, background);
    QFETCH(int, hoverPercent);
    QFETCH(int, pressedPercent);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);
    const QColor under = fromHex(background);
    const auto tint = [tokens, &under](const char *function) {
        QVariant color;
        QMetaObject::invokeMethod(tokens, function, Q_RETURN_ARG(QVariant, color), Q_ARG(QVariant, QVariant(under)));
        return color.value<QColor>();
    };
    const QColor hover = tint("hoverOn");
    const QColor pressed = tint("pressedOn");
    QCOMPARE(channelDistance(hover, colorOf(tokens, "text")), 0);
    QCOMPARE(std::lround(hover.alphaF() * 100), long(hoverPercent));
    QCOMPARE(std::lround(pressed.alphaF() * 100), long(pressedPercent));
    QVERIFY(contrastRatio(drawnOver(hover, under), under) >= 1.3);
    QVERIFY(contrastRatio(drawnOver(pressed, under), under) >= 1.65);
}

void UiFoundationsTest::selectedQueueRowAnswersHover_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// A selected queue row under the pointer steps its selection tint toward
// the text colour, with the hover tint's own fade, active or not; the
// library's does not (hover changed nothing on a selected row). The step is
// the most text, in whole percent up to 4 % on dark and 6 % on light, that
// the text keeps 4.5:1 on; the row's other texts and its mark are made
// readable on it. It is visible yet under the hover's
// from the view, and every text a selected row draws, the column text and
// the playing amber made readable on it, stays 4.5:1 on it.
void UiFoundationsTest::selectedQueueRowAnswersHover() {
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, kRowItems), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QQuickItem *background = itemNamed(root.get(), QStringLiteral("rowBackground"));
    QVERIFY(tokens != nullptr && background != nullptr);
    QQuickItem *hoverTint = childNamed(background, QStringLiteral("rowBackgroundHover"));
    QQuickItem *tintRect = childNamed(background, QStringLiteral("rowBackgroundTint"));
    QVERIFY(hoverTint != nullptr && tintRect != nullptr);

    const QColor view = colorOf(tokens, "view");
    const QColor text = colorOf(tokens, "text");
    const QColor hovered = colorOf(tokens, "hoveredRow");
    const QList<std::pair<const char *, const char *>> selections{{"selection", "selectionHovered"},
                                                                  {"selectionInactive", "selectionInactiveHovered"}};
    QVariant number;
    QVERIFY(QMetaObject::invokeMethod(tokens, "textTertiaryOn", Q_RETURN_ARG(QVariant, number),
                                      Q_ARG(QVariant, QVariant(true)), Q_ARG(QVariant, QVariant(true))));
    const QList<std::pair<QString, QColor>> texts{
        {QStringLiteral("text"), text},
        {QStringLiteral("secondary"), colorOf(tokens, "textSecondarySelected")},
        {QStringLiteral("column"), colorOf(tokens, "textColumn")},
        {QStringLiteral("now"), colorOf(tokens, "now")},
        {QStringLiteral("selected number"), number.value<QColor>()},
    };
    const double share = tokens->property("_selectionHoverShare").toDouble();
    QVERIFY2(std::abs(share - (dark ? 0.04 : 0.06)) < 1e-9, qPrintable(QString::number(share)));
    for (const auto &[selectionToken, hoveredToken] : selections) {
        const QColor selected = colorOf(tokens, selectionToken);
        const QColor selectedHovered = colorOf(tokens, hoveredToken);
        QVERIFY2(channelDistance(selectedHovered, mixed(selected, text, share)) <= 1,
                 qPrintable(QLatin1String(hoveredToken) + QStringLiteral(" is ") + selectedHovered.name()));
        // A visible step, toward the text, yet less than the hover's.
        QVERIFY2(labDistance(selectedHovered, selected) >= 2.0
                     && labDistance(selectedHovered, selected) < labDistance(hovered, view),
                 qPrintable(QStringLiteral("%1 is %2 from %3; the hover %4 from the view")
                                .arg(selectedHovered.name())
                                .arg(labDistance(selectedHovered, selected))
                                .arg(selected.name())
                                .arg(labDistance(hovered, view))));
        QVERIFY(contrastRatio(selectedHovered, text) < contrastRatio(selected, text));
        for (const auto &[what, color] : texts) {
            QVERIFY2(contrastRatio(color, selectedHovered) >= 4.5, qPrintable(describeContrast(what, color, selectedHovered)));
        }
        const double figureContrast = tokens->property("rowFigureContrast").toDouble();
        for (const QColor &under : {selected, selectedHovered}) {
            QVERIFY2(contrastRatio(number.value<QColor>(), under) >= figureContrast,
                     qPrintable(describeContrast(QStringLiteral("selected number"), number.value<QColor>(), under)));
        }
    }

    // The hover's fade in animation time, to its end.
    SteppedAnimationDriver animations;
    const auto settle = [&animations] { (void)animations.step(1000); };

    // The library's rows: a selected row keeps its tint under the pointer.
    background->setProperty("selected", true);
    background->setProperty("hovered", true);
    settle();
    QCOMPARE(hoverTint->opacity(), 1.0);
    QVERIFY(sameColor(colorOf(tintRect, "color"), colorOf(tokens, "selection")));

    // The queue's: it steps to the hovered selection with the hover's fade,
    // and back.
    background->setProperty("hovered", false);
    settle();
    QCOMPARE(hoverTint->opacity(), 0.0);
    background->setProperty("selectedHover", true);
    QVERIFY(sameColor(colorOf(tintRect, "color"), colorOf(tokens, "selection")));
    background->setProperty("hovered", true);
    settle();
    QCOMPARE(hoverTint->opacity(), 1.0);
    QCOMPARE(background->property("selectionHoverLevel").toReal(), 1.0);
    QVERIFY(sameColor(colorOf(tintRect, "color"), colorOf(tokens, "selectionHovered")));
    background->setProperty("active", false);
    QVERIFY(sameColor(colorOf(tintRect, "color"), colorOf(tokens, "selectionInactiveHovered")));
    background->setProperty("hovered", false);
    settle();
    QCOMPARE(hoverTint->opacity(), 0.0);
    QVERIFY(sameColor(colorOf(tintRect, "color"), colorOf(tokens, "selectionInactive")));
    // Selection still changes at once, hovered or not.
    background->setProperty("active", true);
    background->setProperty("hovered", true);
    settle();
    QCOMPARE(hoverTint->opacity(), 1.0);
    background->setProperty("selected", false);
    QCOMPARE(colorOf(tintRect, "color").alphaF(), 0.0f);
    background->setProperty("selected", true);
    QVERIFY(sameColor(colorOf(tintRect, "color"), colorOf(tokens, "selectionHovered")));
}

void UiFoundationsTest::unreadableMarkReadsOnEveryRowBackground_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// An unreadable row's warning sign, in the negative colour as the status
// bar's, reads at 3:1 or more, a graphic's minimum, on every background a
// row of the library or the queue can have.
void UiFoundationsTest::unreadableMarkReadsOnEveryRowBackground() {
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);
    const QColor negative = colorOf(tokens, "negative");
    const QColor negativeSelected = colorOf(tokens, "negativeSelected");
    for (const char *background :
         {"view", "hoveredRow", "selection", "selectionInactive", "selectionHovered", "selectionInactiveHovered"}) {
        const QColor under = colorOf(tokens, background);
        const QColor mark = QByteArray(background).startsWith("selection") ? negativeSelected : negative;
        QVERIFY2(contrastRatio(mark, under) >= 3.0,
                 qPrintable(describeContrast(QLatin1String(background), mark, under)));
    }
}

void UiFoundationsTest::rowBackgroundShowsEachState_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// Each state of a row in turn: its hover, its tint and its focus ring.
void UiFoundationsTest::rowBackgroundShowsEachState() {
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, kRowItems), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QQuickItem *row = itemNamed(root.get(), QStringLiteral("row"));
    QQuickItem *background = itemNamed(root.get(), QStringLiteral("rowBackground"));
    QVERIFY(tokens != nullptr && row != nullptr && background != nullptr);
    QQuickItem *hoverTint = childNamed(background, QStringLiteral("rowBackgroundHover"));
    QQuickItem *tintRect = childNamed(background, QStringLiteral("rowBackgroundTint"));
    QVERIFY(hoverTint != nullptr && tintRect != nullptr);
    QObject *border = tintRect->property("border").value<QObject *>();
    QVERIFY(border != nullptr);

    // The row's tints run to its right edge, under an overlay scrollbar; a
    // list whose scrollbar takes a lane leaves it free.
    QCOMPARE(background->x(), 0.0);
    QCOMPARE(background->width(), row->width());
    QCOMPARE(background->height(), row->height());
    background->setProperty("scrollbarLane", 8);
    QCOMPARE(background->width(), row->width() - 8.0);
    background->setProperty("scrollbarLane", 0);

    struct State {
        const char *name;
        bool hovered;
        bool selected;
        bool active;
        bool focused;
        // The tint's token, or none.
        const char *tint;
    };
    const QList<State> states{
        {"rest", false, false, true, false, nullptr},
        {"hovered", true, false, true, false, nullptr},
        {"selected", false, true, true, false, "selection"},
        {"selected without focus", false, true, false, false, "selectionInactive"},
        {"selected and focused", true, true, true, true, "selection"},
        {"focused, not selected", false, false, true, true, nullptr},
    };
    // The hover's fade in animation time, to its end.
    SteppedAnimationDriver animations;
    for (const State &state : states) {
        const QByteArray name(state.name);
        background->setProperty("hovered", state.hovered);
        background->setProperty("selected", state.selected);
        background->setProperty("active", state.active);
        background->setProperty("focused", state.focused);
        (void)animations.step(1000);

        QVERIFY2(hoverTint->opacity() == (state.hovered ? 1.0 : 0.0), name.constData());
        QVERIFY2(hoverTint->isVisible() == state.hovered, name.constData());
        QVERIFY2(sameColor(colorOf(hoverTint, "color"), colorOf(tokens, "hover")),
                 qPrintable(colorOf(hoverTint, "color").name(QColor::HexArgb) + QStringLiteral(" vs ")
                            + colorOf(tokens, "hover").name(QColor::HexArgb)));
        QVERIFY2(tintRect->isVisible() == (state.selected || state.focused), name.constData());
        if (state.tint == nullptr) {
            QVERIFY2(colorOf(tintRect, "color").alphaF() == 0.0f, name.constData());
        } else {
            QVERIFY2(sameColor(colorOf(tintRect, "color"), colorOf(tokens, state.tint)), name.constData());
        }
        QVERIFY2(border->property("width").toReal() == (state.focused ? 1.0 : 0.0), name.constData());
        QVERIFY2(channelDistance(colorOf(border, "color"), colorOf(tokens, "focusRing")) == 0, name.constData());
        // The ring and the tint are one rectangle.
        QVERIFY2(tintRect->size() == background->size(), name.constData());
    }
}

// The focused, selected row draws its 1 px ring on the edge of its tint,
// and nothing beyond the row.
void UiFoundationsTest::rowBackgroundDrawsTheRingAndTintAsOneRectangle() {
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(kBreezeDark, kRowItems), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QQuickItem *row = itemNamed(root.get(), QStringLiteral("row"));
    QQuickItem *background = itemNamed(root.get(), QStringLiteral("rowBackground"));
    QVERIFY(tokens != nullptr && row != nullptr && background != nullptr);
    background->setProperty("selected", true);
    background->setProperty("focused", true);

    const QImage image = window->grabWindow();
    QVERIFY(!image.isNull());
    const qreal scale = grabScale(image, window);
    const QPointF topLeft = background->mapToScene(QPointF(0, 0)) * scale;
    const qreal width = background->width() * scale;
    const qreal height = background->height() * scale;
    const qreal middle = topLeft.y() + height / 2;
    const QColor ring = colorOf(tokens, "focusRing");
    const QColor selection = colorOf(tokens, "selection");
    const QColor view = colorOf(tokens, "view");
    const auto expectPixel = [&](const QPointF &point, const QColor &wanted, const char *what) {
        const QColor actual = pixelAt(image, point);
        QVERIFY2(channelDistance(actual, wanted) <= 2,
                 qPrintable(QStringLiteral("%1 at %2,%3 is %4, not %5")
                                .arg(QLatin1String(what))
                                .arg(point.x())
                                .arg(point.y())
                                .arg(actual.name(), wanted.name())));
    };
    expectPixel(QPointF(topLeft.x() + 0.5, middle), ring, "the ring's left edge");
    expectPixel(QPointF(topLeft.x() + width - 0.5, middle), ring, "the ring's right edge");
    expectPixel(QPointF(topLeft.x() + width / 2, topLeft.y() + 0.5), ring, "the ring's top edge");
    expectPixel(QPointF(topLeft.x() + 1.5 * scale, middle), selection, "the tint inside the ring");
    expectPixel(QPointF(topLeft.x() + width + 4 * scale, middle), view, "beyond the row");
}

void UiFoundationsTest::regionHeaderLaysOutNameTotalsAndHairline() {
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(kBreezeLight, QStringLiteral(R"QML(
        Components.RegionHeader {
            objectName: "header"
            width: 400
            uiPalette: tokens
            title: "Library"
            totals: "12 artists · 28 albums · 176 tracks"

            Label {
                objectName: "trailingLabel"
                text: "Length"
            }
        }
)QML")),
                                                       &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QQuickItem *header = itemNamed(root.get(), QStringLiteral("header"));
    QVERIFY(tokens != nullptr && header != nullptr);
    QQuickItem *title = childNamed(header, QStringLiteral("regionHeaderTitle"));
    QQuickItem *totals = childNamed(header, QStringLiteral("regionHeaderTotals"));
    QQuickItem *hairline = childNamed(header, QStringLiteral("regionHeaderHairline"));
    QQuickItem *trailing = childNamed(header, QStringLiteral("trailingLabel"));
    QVERIFY(title != nullptr && totals != nullptr && hairline != nullptr && trailing != nullptr);

    // A row tall, with a hairline under it.
    QCOMPARE(header->height(), tokens->property("rowHeight").toReal());
    QCOMPARE(hairline->height(), 1.0);
    QCOMPARE(hairline->width(), header->width());
    QCOMPARE(hairline->y(), header->height() - 1.0);
    QVERIFY2(sameColor(colorOf(hairline, "color"), colorOf(tokens, "hairline")), qPrintable(colorOf(hairline, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "hairline").name(QColor::HexArgb)));

    // The name, small and semibold in the text colour, 12 px in; then 8 px;
    // then the totals, small and tabular in the tertiary colour.
    const QPointF titleOrigin = title->mapToItem(header, QPointF(0, 0));
    const QPointF totalsOrigin = totals->mapToItem(header, QPointF(0, 0));
    QCOMPARE(titleOrigin.x(), 12.0);
    // The layout puts items on whole pixels.
    QVERIFY(std::abs(totalsOrigin.x() - (titleOrigin.x() + title->width() + 8.0)) <= 1.0);
    // The whole name, on whole pixels: never a fraction short, which
    // would elide it.
    QCOMPARE(title->width(), std::ceil(title->implicitWidth()));
    QVERIFY(!title->property("truncated").toBool());
    const QFont titleFont = title->property("font").value<QFont>();
    const QFont totalsFont = totals->property("font").value<QFont>();
    const qreal smallPointSize = tokens->property("smallPointSize").toReal();
    QCOMPARE(titleFont.pointSizeF(), smallPointSize);
    QCOMPARE(titleFont.weight(), QFont::DemiBold);
    QCOMPARE(totalsFont.pointSizeF(), smallPointSize);
    QCOMPARE(totalsFont.weight(), QFont::Normal);
    QVERIFY(totalsFont.featureTags().contains(QFont::Tag("tnum")));
    QCOMPARE(totalsFont.featureValue(QFont::Tag("tnum")), 1u);
    QVERIFY2(sameColor(colorOf(title, "color"), colorOf(tokens, "text")), qPrintable(colorOf(title, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "text").name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(totals, "color"), colorOf(tokens, "textTertiary")), qPrintable(colorOf(totals, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "textTertiary").name(QColor::HexArgb)));
    // Both centred in the row above the hairline.
    QVERIFY(std::abs(titleOrigin.y() + title->height() / 2 - (header->height() - 1.0) / 2) <= 0.5);

    // Items declared inside it go at its right end.
    const QPointF trailingOrigin = trailing->mapToItem(header, QPointF(0, 0));
    QCOMPARE(trailingOrigin.x() + trailing->width(), header->width());

    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(header);
    QVERIFY(accessible != nullptr);
    QCOMPARE(accessible->role(), QAccessible::Heading);
    QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Library, 12 artists · 28 albums · 176 tracks"));

    // Without totals, only the name.
    header->setProperty("totals", QString());
    QVERIFY(!totals->isVisible());
    QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Library"));

    // Narrow, the totals drop whole totals rather than run into the right
    // end or end in a part of one; the header still names them all.
    header->setProperty("totals", QStringLiteral("12 artists · 28 albums · 176 tracks"));
    header->setWidth(180);
    QTRY_VERIFY(totals->property("text").toString() != QStringLiteral("12 artists · 28 albums · 176 tracks"));
    QVERIFY(totals->isVisible());
    QVERIFY2(QStringList({QStringLiteral("12 artists"), QStringLiteral("12 artists · 28 albums")})
                 .contains(totals->property("text").toString()),
             qPrintable(totals->property("text").toString()));
    QVERIFY(!totals->property("truncated").toBool());
    QVERIFY(totals->mapToItem(header, QPointF(totals->width(), 0)).x() <= trailing->mapToItem(header, QPointF(0, 0)).x());
    QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Library, 12 artists · 28 albums · 176 tracks"));
}

void UiFoundationsTest::regionHeaderGivesUpTheTotalsBeforeTheName_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("default font") << 0.0;
    QTest::newRow("24 pt") << 24.0;
}

// However narrow the header, its name stays whole while there is room for
// it: the totals drop whole totals first, down to their first total, and
// then go. They never run into the name or the items at the right end.
void UiFoundationsTest::regionHeaderGivesUpTheTotalsBeforeTheName() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(kBreezeDark, QStringLiteral(R"QML(
        Components.RegionHeader {
            objectName: "header"
            width: 900
            uiPalette: tokens
            title: "Library"
            totals: "13 artists · 29 albums · 180 tracks"

            Label {
                objectName: "trailingLabel"
                text: "Length"
            }
        }
)QML")),
                                                       &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *header = itemNamed(root.get(), QStringLiteral("header"));
    QVERIFY(header != nullptr);
    QQuickItem *title = childNamed(header, QStringLiteral("regionHeaderTitle"));
    QQuickItem *totals = childNamed(header, QStringLiteral("regionHeaderTotals"));
    QQuickItem *trailing = childNamed(header, QStringLiteral("trailingLabel"));
    QVERIFY(title != nullptr && totals != nullptr && trailing != nullptr);
    const qreal titleWidth = std::ceil(title->implicitWidth());
    const qreal leftInset = header->property("leftInset").toReal();
    const qreal fixed = leftInset + titleWidth + header->property("rightInset").toReal() + trailing->implicitWidth();
    const QFont totalsFont = totals->property("font").value<QFont>();
    const qreal firstTotal = QFontMetricsF(totalsFont).horizontalAdvance(QStringLiteral("13 artists"));

    const QString allTotals = QStringLiteral("13 artists · 29 albums · 180 tracks");
    const QStringList wholeTotals{QStringLiteral("13 artists"), QStringLiteral("13 artists · 29 albums"), allTotals};
    bool sawShortened = false;
    bool sawDropped = false;
    for (qreal width = std::ceil(fixed + 8 + totals->implicitWidth() + 40); width >= fixed; width -= 3) {
        header->setWidth(width);
        const QByteArray at = QByteArray("width ") + QByteArray::number(width);
        // Laid out again; the items declared inside stay at the right end.
        QTRY_COMPARE(trailing->mapToItem(header, QPointF(trailing->width(), 0)).x(), width);
        // Shown while there is room for the first total, give or take the
        // rounding of a pixel.
        const qreal room = width - fixed - 8;
        if (room >= firstTotal + 1) {
            QTRY_VERIFY2(totals->isVisible(), at.constData());
        } else if (room < firstTotal - 1) {
            QTRY_VERIFY2(!totals->isVisible(), at.constData());
        }
        // The whole name, from the inset.
        QTRY_COMPARE(title->width(), titleWidth);
        QVERIFY2(!title->property("truncated").toBool(), at.constData());
        QCOMPARE(title->mapToItem(header, QPointF(0, 0)).x(), leftInset);
        const qreal trailingLeft = trailing->mapToItem(header, QPointF(0, 0)).x();
        if (totals->isVisible()) {
            const qreal totalsLeft = totals->mapToItem(header, QPointF(0, 0)).x();
            QVERIFY2(totalsLeft >= leftInset + titleWidth + 8 - 1, at.constData());
            QVERIFY2(totalsLeft + totals->width() <= trailingLeft + 0.5, at.constData());
            // At least its first total shows, and only whole totals: never
            // a part of one, as "29 a…".
            QVERIFY2(totals->width() >= std::floor(firstTotal) - 1, at.constData());
            const QString shown = totals->property("text").toString();
            QVERIFY2(wholeTotals.contains(shown), qPrintable(QString::fromLatin1(at) + QStringLiteral(": ") + shown));
            QVERIFY2(!totals->property("truncated").toBool(), qPrintable(QString::fromLatin1(at) + QStringLiteral(": ") + shown));
            sawShortened = sawShortened || shown != allTotals;
        } else {
            sawDropped = true;
        }
    }
    QVERIFY(sawShortened);
    QVERIFY(sawDropped);
}

namespace {

const QString kPlotItems = QStringLiteral(R"QML(
        Rectangle {
            id: plot
            objectName: "plot"
            property var plotColors: tokens.plotColors(plotMode)
            property int plotMode: 0
            x: 16
            y: 16
            width: 300
            height: 120
            color: plotColors.plotBg

            Components.PlotPlate {
                objectName: "lanePlate"
                x: 5
                y: 5
                plotColors: plot.plotColors
                uiPalette: tokens
                text: "L"
            }

            Components.PlotPlate {
                objectName: "switchPlate"
                x: 200
                y: 40
                plotColors: plot.plotColors
                uiPalette: tokens

                Components.PlotToolButton {
                    objectName: "firstButton"
                    plotColors: plot.plotColors
                    uiPalette: tokens
                    text: "Spectrogram"
                    icon.name: "ferrous-fixture-square"
                    checkable: true
                    checked: true
                }

                Components.PlotToolButton {
                    objectName: "secondButton"
                    plotColors: plot.plotColors
                    uiPalette: tokens
                    text: "Mute L"
                    glyphText: "M"
                    checkable: true
                }
            }
        }
)QML");

} // namespace

void UiFoundationsTest::plotPlateLabelHasAnEdgeOutside() {
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(kBreezeDark, kPlotItems), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *plot = itemNamed(root.get(), QStringLiteral("plot"));
    QQuickItem *label = itemNamed(root.get(), QStringLiteral("lanePlate"));
    QQuickItem *switchPlate = itemNamed(root.get(), QStringLiteral("switchPlate"));
    QVERIFY(plot != nullptr && label != nullptr && switchPlate != nullptr);
    QObject *plotColors = plot->property("plotColors").value<QObject *>();
    QVERIFY(plotColors != nullptr);

    // A lane label: at least 20 px square, 6 px either side of its text,
    // with a 3 px radius, its text small, semibold, in the plot's text colour.
    QCOMPARE(label->height(), 20.0);
    QQuickItem *text = childNamed(label, QStringLiteral("plotPlateLabel"));
    QVERIFY(text != nullptr);
    QCOMPARE(label->width(), std::max(20.0, text->width() + 12.0));
    QVERIFY2(sameColor(colorOf(text, "color"), colorOf(plotColors, "plotText")), qPrintable(colorOf(text, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(plotColors, "plotText").name(QColor::HexArgb)));
    QCOMPARE(text->property("font").value<QFont>().weight(), QFont::DemiBold);
    QQuickItem *fill = childNamed(label, QStringLiteral("plotPlateFill"));
    QQuickItem *edge = childNamed(label, QStringLiteral("plotPlateEdge"));
    QVERIFY(fill != nullptr && edge != nullptr);
    QVERIFY2(sameColor(colorOf(fill, "color"), colorOf(plotColors, "plotPlate")), qPrintable(colorOf(fill, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(plotColors, "plotPlate").name(QColor::HexArgb)));
    QCOMPARE(fill->property("radius").toReal(), 3.0);
    // The edge is 1 px outside the plate.
    QCOMPARE(edge->x(), -1.0);
    QCOMPARE(edge->y(), -1.0);
    QCOMPARE(edge->width(), label->width() + 2.0);
    QCOMPARE(edge->height(), label->height() + 2.0);
    QObject *edgeBorder = edge->property("border").value<QObject *>();
    QCOMPARE(edgeBorder->property("width").toReal(), 1.0);
    QVERIFY2(sameColor(colorOf(edgeBorder, "color"), colorOf(plotColors, "plotPlateEdge")), qPrintable(colorOf(edgeBorder, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(plotColors, "plotPlateEdge").name(QColor::HexArgb)));
    QCOMPARE(colorOf(edge, "color").alphaF(), 0.0f);

    // A plate of two 24 px buttons: 2 px around them and between them.
    QCOMPARE(switchPlate->width(), 2.0 + 24.0 + 2.0 + 24.0 + 2.0);
    QCOMPARE(switchPlate->height(), 28.0);
    QQuickItem *first = itemNamed(root.get(), QStringLiteral("firstButton"));
    QQuickItem *second = itemNamed(root.get(), QStringLiteral("secondButton"));
    QVERIFY(first != nullptr && second != nullptr);
    QCOMPARE(first->mapToItem(switchPlate, QPointF(0, 0)), QPointF(2, 2));
    QCOMPARE(second->mapToItem(switchPlate, QPointF(0, 0)), QPointF(28, 2));
    QCOMPARE(childNamed(switchPlate, QStringLiteral("plotPlateFill"))->property("radius").toReal(), 4.0);

    // The plate takes the plot's colours: the waveform's in waveform mode.
    QObject *tokens = root->property("tokens").value<QObject *>();
    plot->setProperty("plotMode", 1);
    QObject *waveform = tokens->property("waveformPlot").value<QObject *>();
    QVERIFY2(sameColor(colorOf(fill, "color"), colorOf(waveform, "plotPlate")), qPrintable(colorOf(fill, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(waveform, "plotPlate").name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(edgeBorder, "color"), colorOf(waveform, "plotPlateEdge")), qPrintable(colorOf(edgeBorder, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(waveform, "plotPlateEdge").name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(text, "color"), colorOf(waveform, "plotText")), qPrintable(colorOf(text, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(waveform, "plotText").name(QColor::HexArgb)));
}

void UiFoundationsTest::smallControlsScaleWithALargeFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// The plot's and the viewers' small tool buttons and their glyphs are 24
// and 16 px at the default 10 pt font, and grow with a larger one as the
// transport's do: the glyph by the font's share of 10 pt, the button 4 px
// more around it. A lane label's plate holds its text's line, 20 px tall
// at the default font, with 6 px either side of the text.
void UiFoundationsTest::smallControlsScaleWithALargeFont() {
    QFETCH(qreal, pointSize);
    const int iconSize = qRound(16 * pointSize / 10);
    const int buttonSize = iconSize + 8;
    const ScopedApplicationFont font(pointSize);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(kBreezeDark, kPlotItems + QStringLiteral(R"QML(
        Components.ViewerToolButton {
            objectName: "viewerButton"
            x: 16
            y: 160
            uiPalette: tokens
            text: "Close"
            icon.name: "ferrous-fixture-square"
        }
)QML")),
                                                       &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *plotButton = itemNamed(root.get(), QStringLiteral("firstButton"));
    QQuickItem *letterButton = itemNamed(root.get(), QStringLiteral("secondButton"));
    QQuickItem *viewerButton = itemNamed(root.get(), QStringLiteral("viewerButton"));
    QQuickItem *label = itemNamed(root.get(), QStringLiteral("lanePlate"));
    QVERIFY(plotButton && letterButton && viewerButton && label);
    QQuickItem *plotIcon = childNamed(plotButton, QStringLiteral("plotToolButtonIcon"));
    QQuickItem *viewerIcon = childNamed(viewerButton, QStringLiteral("viewerToolButtonIcon"));
    QQuickItem *text = childNamed(label, QStringLiteral("plotPlateLabel"));
    QVERIFY(plotIcon && viewerIcon && text);

    for (QQuickItem *button : {plotButton, letterButton, viewerButton}) {
        QCOMPARE(button->size(), QSizeF(buttonSize, buttonSize));
    }
    for (QQuickItem *icon : {plotIcon, viewerIcon}) {
        QCOMPARE(icon->size(), QSizeF(iconSize, iconSize));
    }
    // The M of Mute stays inside its button.
    QQuickItem *letter = childNamed(letterButton, QStringLiteral("plotToolButtonGlyph"));
    QVERIFY(letter != nullptr);
    QVERIFY(letter->implicitHeight() <= buttonSize);

    // The plates' letters, the lane's and the M, drawn as curves with grey
    // antialiasing at any font size, DemiBold (the platform's subpixel
    // antialiasing fringed them in colour).
    for (QQuickItem *item : {text, letter}) {
        QCOMPARE(item->property("renderType").toInt(), 2 /* Text.CurveRendering */);
        QCOMPARE(item->property("font").value<QFont>().weight(), QFont::DemiBold);
    }

    // The plot's text, the lane label's and the M, at least 11 px at the
    // default font and growing with a larger one, as the rest of the
    // design's small text.
    for (QQuickItem *item : {text, letter}) {
        const int pixels = QFontInfo(item->property("font").value<QFont>()).pixelSize();
        QVERIFY2(pixels >= std::max(11.0, 11.0 * pointSize / 10.0), qPrintable(QString::number(pixels)));
    }

    // The lane label's plate: its text whole inside it, 6 px either side.
    QCOMPARE(label->width(), std::max(20.0, text->width() + 12.0));
    QVERIFY2(text->implicitHeight() <= label->height(),
             qPrintable(QStringLiteral("%1 > %2").arg(text->implicitHeight()).arg(label->height())));
    if (pointSize == 10.0) {
        QCOMPARE(label->height(), 20.0);
    } else {
        // A larger font's is its capitals and the room around them, in
        // step with the font, not the text's line and more: a letter's
        // chip is no tombstone.
        const qreal capitals = QFontMetricsF(text->property("font").value<QFont>()).capHeight();
        QVERIFY2(label->height() >= capitals + 12 && label->height() <= text->implicitHeight() + 4,
                 qPrintable(QStringLiteral("%1 for capitals of %2 on a line of %3")
                                .arg(label->height()).arg(capitals).arg(text->implicitHeight())));
    }
}

void UiFoundationsTest::plotSegmentHoverStaysUnderTheChosenPlate_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("plotMode");
    QTest::newRow("spectrogram, dark") << true << 0;
    QTest::newRow("spectrogram, light") << false << 0;
    QTest::newRow("waveform, dark") << true << 1;
    QTest::newRow("waveform, light") << false << 1;
}

// A segment of a plate's choices, as the visualization's mode switch,
// under the pointer while another is the chosen one (a 12 % hover with its
// glyph whole came to the chosen plate's brightness): a wash of the plot's
// text colour at 10 % or more, against the chosen plate's fill and its edge,
// with its glyph at 85 %, whole only while pressed (8 % with the glyph at
// 60 % was 1.2:1, nearly invisible). As drawn, in both themes and on both
// plots, the hovered one shows on the plate; the chosen one's fill is 3:1 on
// the plate, so the choice reads by its fill rather than its edge alone, and
// 2:1 on the hovered one's wash (at 2:1 on the plate it read by its edge).
void UiFoundationsTest::plotSegmentHoverStaysUnderTheChosenPlate() {
    QFETCH(bool, dark);
    QFETCH(int, plotMode);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, kPlotItems), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *plot = itemNamed(root.get(), QStringLiteral("plot"));
    QVERIFY(plot != nullptr);
    plot->setProperty("plotMode", plotMode);
    QObject *plotColors = plot->property("plotColors").value<QObject *>();
    QVERIFY(plotColors != nullptr);
    QQuickItem *chosen = itemNamed(root.get(), QStringLiteral("firstButton"));
    QQuickItem *other = itemNamed(root.get(), QStringLiteral("secondButton"));
    QVERIFY(chosen != nullptr && other != nullptr);
    for (QQuickItem *button : {chosen, other}) {
        QVERIFY(button->setProperty("segment", true));
    }
    QVERIFY(chosen->property("checked").toBool());
    QVERIFY(!other->property("checked").toBool());
    QQuickItem *hover = childNamed(other, QStringLiteral("plotToolButtonHover"));
    QQuickItem *fill = childNamed(chosen, QStringLiteral("plotToolButtonChecked"));
    QQuickItem *letter = childNamed(other, QStringLiteral("plotToolButtonGlyph"));
    QVERIFY(hover != nullptr && fill != nullptr && letter != nullptr);

    // The pointer over the other one, until its wash has faded in, in
    // animation time: from off the plate, as QTest leaves a move to where it
    // already is out.
    SteppedAnimationDriver animations;
    QTest::mouseMove(window, QPoint(2, 2));
    QTest::mouseMove(window, other->mapToScene(QPointF(12, 12)).toPoint());
    QTRY_VERIFY(other->property("hovered").toBool());
    (void)animations.step(1000);
    QCOMPARE(hover->opacity(), 1.0);
    const QColor text = colorOf(plotColors, "plotText");
    const QColor wash = colorOf(hover, "color");
    const QColor plateFill = colorOf(fill, "color");
    QCOMPARE(wash.rgb(), text.rgb());
    // The wash from 10 % up, as the plate needs (14 % and 18 % were 1.14:1
    // apart in dark, 1.08:1 in light); the chosen fill some 35 % on the
    // dark plates, 44 % on the light one, where the same tint steps less.
    QVERIFY2(wash.alphaF() >= 0.095 && wash.alphaF() <= 0.135, qPrintable(QString::number(wash.alphaF())));
    QVERIFY2(std::abs(wash.alphaF() - other->property("segmentHoverOpacity").toReal()) < 0.005,
             qPrintable(QString::number(wash.alphaF())));
    QCOMPARE(plateFill.rgb(), text.rgb());
    QVERIFY2(plateFill.alphaF() >= 0.3 && plateFill.alphaF() <= 0.55, qPrintable(QString::number(plateFill.alphaF())));
    QVERIFY2(std::abs(plateFill.alphaF() - chosen->property("segmentFillOpacity").toReal()) < 0.005,
             qPrintable(QString::number(plateFill.alphaF())));
    QVERIFY2(std::abs(letter->opacity() - 0.85) < 0.005, qPrintable(QString::number(letter->opacity())));

    // As drawn: the plate around the buttons, the chosen one's plate and
    // the hovered one's wash, clear of their glyphs.
    const QImage image = window->grabWindow();
    QVERIFY(!image.isNull());
    const qreal scale = grabScale(image, window);
    const auto at = [&](QQuickItem *item, QPointF point) {
        return pixelAt(image, item->mapToScene(point) * scale);
    };
    const QColor plate = drawnOver(colorOf(plotColors, "plotPlate"), colorOf(plotColors, "plotBg"));
    const QColor drawnChosen = at(chosen, QPointF(2.5, 2.5));
    const QColor drawnHover = at(other, QPointF(2.5, 2.5));
    QVERIFY2(channelDistance(drawnChosen, drawnOver(plateFill, plate)) <= 3,
             qPrintable(describeContrast(QStringLiteral("the chosen plate"), drawnChosen, plate)));
    QVERIFY2(channelDistance(drawnHover, drawnOver(wash, plate)) <= 3,
             qPrintable(describeContrast(QStringLiteral("the hover wash"), drawnHover, plate)));
    // Toward the text colour, both: lighter on a dark plate, darker on a
    // light one; the hovered one shows, and less than the chosen one.
    const qreal chosenStep = contrastRatio(drawnChosen, plate);
    const qreal hoverStep = contrastRatio(drawnHover, plate);
    QVERIFY2(hoverStep >= 1.25 && hoverStep < chosenStep,
             qPrintable(QStringLiteral("hovered %1:1, chosen %2:1").arg(hoverStep).arg(chosenStep)));
    // The chosen fill reads on its own: 2:1 on the hovered wash, and on a
    // dark plate 3:1 on the plate, within a step of rounding; on a light one
    // some 2.5:1, where its dark glyph keeps 4.5:1 on it.
    const bool lightPlate = relativeLuminance(plate) > 0.5;
    QVERIFY2(chosenStep >= (lightPlate ? 2.4 : 2.97),
             qPrintable(describeContrast(QStringLiteral("the chosen plate on the plate"), drawnChosen, plate)));
    QVERIFY2(contrastRatio(drawnChosen, drawnHover) >= 1.98,
             qPrintable(describeContrast(QStringLiteral("the chosen plate on the hovered one"), drawnChosen, drawnHover)));
    // The chosen one's edge, as drawn, 3:1 on the plate and on its fill.
    const QColor edge = at(chosen, QPointF(chosen->width() / 2, 0.5));
    QVERIFY2(contrastRatio(edge, plate) >= 3.0 && contrastRatio(edge, drawnChosen) >= 3.0,
             qPrintable(QStringLiteral("edge %1 on plate %2 and fill %3").arg(edge.name(), plate.name(), drawnChosen.name())));

    // Pressed, its glyph is whole.
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, other->mapToScene(QPointF(12, 12)).toPoint());
    QTRY_VERIFY(other->property("down").toBool());
    QCOMPARE(letter->opacity(), 1.0);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, other->mapToScene(QPointF(12, 12)).toPoint());
}

void UiFoundationsTest::viewerToolButtonHoverReadsOnThePlot_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("plotMode");
    QTest::newRow("spectrogram, dark") << true << 0;
    QTest::newRow("spectrogram, light") << false << 0;
    QTest::newRow("waveform, dark") << true << 1;
    QTest::newRow("waveform, light") << false << 1;
}

// A viewer's tool button on a plot's title strip, as the album art
// viewer's info and close buttons: the pointer over it washes it in the
// plot's text colour, 14 % on a dark plot and 12 % on a light one, which
// read on the strip as drawn, some 1.4:1 and 1.25:1, as a plate's hovered
// segment (at 6.5 %, 1.15:1 on a 24 px target, it hardly showed). Pressed,
// 22 %, steps further.
void UiFoundationsTest::viewerToolButtonHoverReadsOnThePlot() {
    QFETCH(bool, dark);
    QFETCH(int, plotMode);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight,
                                                                               kPlotItems + QStringLiteral(R"QML(
        Rectangle {
            objectName: "strip"
            x: 16
            y: 150
            width: 120
            height: 40
            color: plot.plotColors.plotStrip

            Components.ViewerToolButton {
                objectName: "viewerButton"
                x: 8
                y: 8
                uiPalette: tokens
                plotColors: plot.plotColors
                text: "Close"
                icon.name: "ferrous-fixture-square"
            }
        }
)QML")),
                                                       &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *plot = itemNamed(root.get(), QStringLiteral("plot"));
    QQuickItem *strip = itemNamed(root.get(), QStringLiteral("strip"));
    QQuickItem *button = itemNamed(root.get(), QStringLiteral("viewerButton"));
    QVERIFY(plot != nullptr && strip != nullptr && button != nullptr);
    plot->setProperty("plotMode", plotMode);
    QObject *plotColors = plot->property("plotColors").value<QObject *>();
    QVERIFY(plotColors != nullptr);
    QQuickItem *hover = childNamed(button, QStringLiteral("viewerToolButtonHover"));
    QVERIFY(hover != nullptr);
    const QColor text = colorOf(plotColors, "plotText");
    const QColor surface = colorOf(strip, "color");
    const bool darkPlot = relativeLuminance(colorOf(plotColors, "plotBg")) < 0.2;

    // At rest, nothing.
    QVERIFY(!hover->isVisible());

    // The wash's fade in animation time, to its end.
    SteppedAnimationDriver animations;
    const auto settle = [&animations] { (void)animations.step(1000); };
    QTest::mouseMove(window, QPoint(2, 2));
    const QPoint inside = button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
    QTest::mouseMove(window, inside);
    QTRY_VERIFY(button->property("hovered").toBool());
    settle();
    QCOMPARE(hover->opacity(), 1.0);
    const QColor wash = colorOf(hover, "color");
    QCOMPARE(wash.rgb(), text.rgb());
    QVERIFY2(std::abs(wash.alphaF() - (darkPlot ? 0.14 : 0.12)) < 0.005, qPrintable(QString::number(wash.alphaF())));

    // As drawn, clear of the glyph.
    QImage image = window->grabWindow();
    QVERIFY(!image.isNull());
    const qreal scale = grabScale(image, window);
    const QColor drawnHover = pixelAt(image, button->mapToScene(QPointF(2.5, 2.5)) * scale);
    QVERIFY2(channelDistance(drawnHover, drawnOver(wash, surface)) <= 3,
             qPrintable(describeContrast(QStringLiteral("the hover wash"), drawnHover, surface)));
    const qreal hoverStep = contrastRatio(drawnHover, surface);
    QVERIFY2(hoverStep >= (darkPlot ? 1.35 : 1.25),
             qPrintable(describeContrast(QStringLiteral("the hover wash"), drawnHover, surface)));

    // Pressed, further: the pressed fill alone, over no hover.
    QQuickItem *pressedFill = childNamed(button, QStringLiteral("viewerToolButtonPressed"));
    QVERIFY(pressedFill != nullptr);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, inside);
    QTRY_VERIFY(button->property("down").toBool());
    QVERIFY(pressedFill->isVisible());
    QVERIFY(!hover->isVisible());
    const QColor pressed = colorOf(pressedFill, "color");
    QCOMPARE(pressed.rgb(), text.rgb());
    QVERIFY2(std::abs(pressed.alphaF() - 0.22) < 0.005, qPrintable(QString::number(pressed.alphaF())));
    // On the press's first frame.
    image = window->grabWindow();
    const QColor drawnPressed = pixelAt(image, button->mapToScene(QPointF(2.5, 2.5)) * scale);
    QVERIFY2(contrastRatio(drawnPressed, surface) > hoverStep,
             qPrintable(describeContrast(QStringLiteral("the pressed wash"), drawnPressed, surface)));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, inside);
}

void UiFoundationsTest::viewerToolButtonShowsAKeyPressAtOnce_data() {
    QTest::addColumn<bool>("onPlot");
    QTest::newRow("on a plot") << true;
    QTest::newRow("on the window") << false;
}

// Space on a focused viewer button that the pointer is not over draws the
// pressed fill whole on the press's first frame, as a pointer press does;
// only the hover fades (#601). The release takes it away at once too.
void UiFoundationsTest::viewerToolButtonShowsAKeyPressAtOnce() {
    QFETCH(bool, onPlot);
    const QString strip = QStringLiteral(R"QML(
        Rectangle {
            objectName: "strip"
            x: 16
            y: 150
            width: 120
            height: 40
            color: %1

            Components.ViewerToolButton {
                objectName: "viewerButton"
                x: 8
                y: 8
                uiPalette: tokens
                plotColors: %2
                text: "Close"
                icon.name: "ferrous-fixture-square"
            }
        }
)QML")
                              .arg(onPlot ? QStringLiteral("plot.plotColors.plotStrip") : QStringLiteral("tokens.chrome"),
                                   onPlot ? QStringLiteral("plot.plotColors") : QStringLiteral("null"));
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(kBreezeDark, kPlotItems + strip), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *stripItem = itemNamed(root.get(), QStringLiteral("strip"));
    QQuickItem *button = itemNamed(root.get(), QStringLiteral("viewerButton"));
    QVERIFY(stripItem != nullptr && button != nullptr);
    const QColor surface = colorOf(stripItem, "color");
    const QColor pressed = colorOf(button, "pressedColor");
    QVERIFY(pressed.alpha() > 0);

    // Focused from the keyboard, with the pointer elsewhere.
    QTest::mouseMove(window, QPoint(2, 2));
    button->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(button->property("visualFocus").toBool());
    QVERIFY(!button->property("hovered").toBool());

    // Animation time stands still: what shows is the press's first frame.
    SteppedAnimationDriver animations;
    // Inside the 2 px focus ring, clear of the glyph.
    const auto drawnFill = [&] {
        const QImage image = window->grabWindow();
        return pixelAt(image, button->mapToScene(QPointF(3.5, 3.5)) * grabScale(image, window));
    };
    QTest::keyPress(window, Qt::Key_Space);
    QVERIFY(button->property("down").toBool());
    const QColor drawnPressed = drawnFill();
    QVERIFY2(channelDistance(drawnPressed, drawnOver(pressed, surface)) <= 3,
             qPrintable(QStringLiteral("the first frame of the press is %1, not %2")
                            .arg(drawnPressed.name(), drawnOver(pressed, surface).name())));

    QTest::keyRelease(window, Qt::Key_Space);
    QVERIFY(!button->property("down").toBool());
    const QColor drawnReleased = drawnFill();
    QVERIFY2(channelDistance(drawnReleased, surface) <= 3,
             qPrintable(QStringLiteral("the first frame after the release is %1, not %2")
                            .arg(drawnReleased.name(), surface.name())));
}

void UiFoundationsTest::viewerWaveformLaneSeamReadsOnWhite_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// The line between the waveform's lanes in a viewer: in Breeze Light
// #cdd2d6, a step stronger than the pane's hairline, which on a white
// screen all but vanished between the channels; in Breeze Dark the pane's.
void UiFoundationsTest::viewerWaveformLaneSeamReadsOnWhite() {
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);
    QObject *waveform = tokens->property("waveformPlot").value<QObject *>();
    QVERIFY(waveform != nullptr);
    const QColor seam = colorOf(tokens, "viewerWaveformLaneSeam");
    if (dark) {
        QVERIFY(sameColor(seam, colorOf(waveform, "plotLaneSeam")));
    } else {
        QVERIFY2(sameColor(seam, fromHex(QStringLiteral("#cdd2d6"))), qPrintable(seam.name()));
        QVERIFY(contrastRatio(seam, colorOf(waveform, "plotBg"))
                > contrastRatio(colorOf(waveform, "plotLaneSeam"), colorOf(waveform, "plotBg")));
    }
}

void UiFoundationsTest::viewerToolButtonCheckedToggleAnswersHover_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("plotMode");
    QTest::newRow("spectrogram, dark") << true << 0;
    QTest::newRow("waveform, light") << false << 1;
}

// A checked toggle on a plot, such as the album art viewer's Image Details,
// which a click turns off, answers the pointer: its checked fill goes from
// 18 % to 26 % of the plot's text colour while hovered, fading over
// shortDuration, and back as the pointer leaves (at one fill it gave no
// sign that a click would change it). The chosen segment of a choice, such
// as the visualization's mode switch, which a click leaves chosen, stays at
// 18 %.
void UiFoundationsTest::viewerToolButtonCheckedToggleAnswersHover() {
    QFETCH(bool, dark);
    QFETCH(int, plotMode);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight,
                                                                               kPlotItems + QStringLiteral(R"QML(
        Rectangle {
            objectName: "strip"
            x: 16
            y: 150
            width: 120
            height: 40
            color: plot.plotColors.plotStrip

            Components.ViewerToolButton {
                objectName: "toggle"
                x: 8
                y: 8
                uiPalette: tokens
                plotColors: plot.plotColors
                checked: true
                text: "Image Details"
                icon.name: "ferrous-fixture-square"
            }

            Components.ViewerToolButton {
                objectName: "segment"
                x: 60
                y: 8
                uiPalette: tokens
                plotColors: plot.plotColors
                checkable: true
                autoExclusive: true
                checked: true
                text: "Spectrogram"
                icon.name: "ferrous-fixture-square"
            }
        }
)QML")),
                                                       &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *plot = itemNamed(root.get(), QStringLiteral("plot"));
    QQuickItem *toggle = itemNamed(root.get(), QStringLiteral("toggle"));
    QQuickItem *segment = itemNamed(root.get(), QStringLiteral("segment"));
    QVERIFY(plot != nullptr && toggle != nullptr && segment != nullptr);
    plot->setProperty("plotMode", plotMode);
    QObject *plotColors = plot->property("plotColors").value<QObject *>();
    QVERIFY(plotColors != nullptr);
    const QColor text = colorOf(plotColors, "plotText");
    QQuickItem *toggleFill = childNamed(toggle, QStringLiteral("viewerToolButtonChecked"));
    QQuickItem *segmentFill = childNamed(segment, QStringLiteral("viewerToolButtonChecked"));
    QVERIFY(toggleFill != nullptr && segmentFill != nullptr);
    QVERIFY(toggleFill->isVisible() && segmentFill->isVisible());
    const auto alphaOf = [](QQuickItem *fill) { return colorOf(fill, "color").alphaF(); };
    QCOMPARE(colorOf(toggleFill, "color").rgb(), text.rgb());
    QVERIFY2(std::abs(alphaOf(toggleFill) - 0.18) < 0.005, qPrintable(QString::number(alphaOf(toggleFill))));

    SteppedAnimationDriver animations;
    QTest::mouseMove(window, QPoint(2, 2));
    QTest::mouseMove(window, toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint());
    QTRY_VERIFY(toggle->property("hovered").toBool());
    // It fades: a moment in, between the two.
    (void)animations.step(20);
    const qreal halfway = alphaOf(toggleFill);
    QVERIFY2(halfway > 0.185 && halfway < 0.255, qPrintable(QString::number(halfway)));
    (void)animations.step(1000);
    QCOMPARE(colorOf(toggleFill, "color").rgb(), text.rgb());
    QVERIFY2(std::abs(alphaOf(toggleFill) - 0.26) < 0.005, qPrintable(QString::number(alphaOf(toggleFill))));
    // The hover wash stays aside: the checked fill alone shows.
    QQuickItem *toggleHover = childNamed(toggle, QStringLiteral("viewerToolButtonHover"));
    QVERIFY(toggleHover != nullptr);
    QVERIFY(!toggleHover->isVisible());

    // The chosen segment stays as it is.
    QTest::mouseMove(window, segment->mapToScene(QPointF(segment->width() / 2, segment->height() / 2)).toPoint());
    QTRY_VERIFY(segment->property("hovered").toBool());
    (void)animations.step(1000);
    QVERIFY2(std::abs(alphaOf(segmentFill) - 0.18) < 0.005, qPrintable(QString::number(alphaOf(segmentFill))));
    // And the toggle the pointer left is back at 18 %.
    QVERIFY(!toggle->property("hovered").toBool());
    QVERIFY2(std::abs(alphaOf(toggleFill) - 0.18) < 0.005, qPrintable(QString::number(alphaOf(toggleFill))));
}

void UiFoundationsTest::plotToolButtonDrawsCheckedAndFocusApart_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<int>("plotMode");
    QTest::newRow("spectrogram, dark") << true << 0;
    QTest::newRow("spectrogram, light") << false << 0;
    QTest::newRow("waveform, dark") << true << 1;
    QTest::newRow("waveform, light") << false << 1;
}

// Checked is a solid fill with the glyph or letter in the checked glyph
// colour; keyboard focus is a separate 2 px ring 1 px outside the button,
// with a 1 px gap in the plate's colour; checked and focused shows both.
// Each state is drawn and measured: the fill on the plate (3:1), the glyph
// or letter on the fill (4.5:1) and the ring on the plate (3:1).
void UiFoundationsTest::plotToolButtonDrawsCheckedAndFocusApart() {
    QFETCH(bool, dark);
    QFETCH(int, plotMode);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, kPlotItems), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *plot = itemNamed(root.get(), QStringLiteral("plot"));
    QVERIFY(plot != nullptr);
    plot->setProperty("plotMode", plotMode);
    QObject *plotColors = plot->property("plotColors").value<QObject *>();
    QVERIFY(plotColors != nullptr);
    QQuickItem *iconButton = itemNamed(root.get(), QStringLiteral("firstButton"));
    QQuickItem *button = itemNamed(root.get(), QStringLiteral("secondButton"));
    QVERIFY(iconButton != nullptr && button != nullptr);
    QCOMPARE(button->size(), QSizeF(24, 24));
    QQuickItem *fill = childNamed(button, QStringLiteral("plotToolButtonChecked"));
    QQuickItem *gap = childNamed(button, QStringLiteral("plotToolButtonFocusGap"));
    QQuickItem *ring = childNamed(button, QStringLiteral("plotToolButtonFocusRing"));
    QQuickItem *letter = childNamed(button, QStringLiteral("plotToolButtonGlyph"));
    QQuickItem *icon = childNamed(iconButton, QStringLiteral("plotToolButtonIcon"));
    QVERIFY(fill != nullptr && gap != nullptr && ring != nullptr && letter != nullptr && icon != nullptr);
    QVERIFY(letter->isVisible());
    QCOMPARE(letter->property("text").toString(), QStringLiteral("M"));

    // The ring: 2 px, from 1 px to 3 px outside the button; the gap: the
    // 1 px between, in the plate's colour.
    QObject *ringBorder = ring->property("border").value<QObject *>();
    QObject *gapBorder = gap->property("border").value<QObject *>();
    QCOMPARE(ringBorder->property("width").toReal(), 2.0);
    QCOMPARE(ring->mapRectToItem(button, QRectF(0, 0, ring->width(), ring->height())), QRectF(-3, -3, 30, 30));
    QCOMPARE(gapBorder->property("width").toReal(), 1.0);
    QCOMPARE(gap->mapRectToItem(button, QRectF(0, 0, gap->width(), gap->height())), QRectF(-1, -1, 26, 26));
    QCOMPARE(colorOf(ring, "color").alphaF(), 0.0f);
    QCOMPARE(colorOf(gap, "color").alphaF(), 0.0f);

    const QColor floor = colorOf(plotColors, "plotBg");
    const QColor plate = drawnOver(colorOf(plotColors, "plotPlate"), floor);
    const QColor plateEdge = drawnOver(colorOf(plotColors, "plotPlateEdge"), floor);
    const QColor checkedFill = colorOf(plotColors, "checkedFill");
    const QColor focusRing = colorOf(plotColors, "plotFocusRing");
    QVERIFY(channelDistance(colorOf(gapBorder, "color"), plate) <= 1);
    QVERIFY(sameColor(colorOf(ringBorder, "color"), focusRing));

    struct State {
        const char *name;
        bool checked;
        bool focused;
    };
    const QList<State> states{
        {"at rest", false, false},
        {"checked", true, false},
        {"focused", false, true},
        {"checked and focused", true, true},
    };
    for (const State &state : states) {
        const QString name = QString::fromLatin1(state.name);
        button->setProperty("checked", state.checked);
        iconButton->setProperty("checked", state.checked);
        if (state.focused) {
            button->forceActiveFocus(Qt::TabFocusReason);
        } else {
            plot->forceActiveFocus(Qt::OtherFocusReason);
        }
        QTRY_COMPARE(button->property("visualFocus").toBool(), state.focused);
        QCOMPARE(fill->isVisible(), state.checked);
        QCOMPARE(ring->isVisible(), state.focused);
        QCOMPARE(gap->isVisible(), state.focused);
        QVERIFY(sameColor(colorOf(fill, "color"), checkedFill));
        const QColor letterColor = colorOf(letter, "color");
        QVERIFY2(sameColor(letterColor, colorOf(plotColors, state.checked ? "checkedGlyph" : "plotText")),
                 qPrintable(name));

        const QImage image = window->grabWindow();
        QVERIFY(!image.isNull());
        const qreal scale = grabScale(image, window);
        const QPointF origin = button->mapToScene(QPointF(0, 0)) * scale;
        const auto pixel = [&](qreal x) { return pixelAt(image, origin + QPointF(x * scale, 12 * scale)); };
        const auto expect = [&](qreal x, const QColor &wanted, const QString &what) {
            const QColor actual = pixel(x);
            QVERIFY2(channelDistance(actual, wanted) <= 3,
                     qPrintable(QStringLiteral("%1, %2 at x %3 is %4, not %5")
                                    .arg(name, what)
                                    .arg(x)
                                    .arg(actual.name(), wanted.name())));
        };
        // Inside the button, clear of its letter; then, on its right: the
        // gap, the ring's two pixels (the second over the plate's edge),
        // and the plot.
        const QColor inside = state.checked ? checkedFill : plate;
        expect(2.5, inside, QStringLiteral("inside"));
        expect(24.5, plate, QStringLiteral("the gap"));
        expect(25.5, state.focused ? focusRing : plate, QStringLiteral("the ring"));
        expect(26.5, state.focused ? focusRing : plateEdge, QStringLiteral("the ring's outer pixel"));
        expect(27.5, floor, QStringLiteral("the plot"));

        // Measured on what was drawn.
        const QColor drawnInside = pixel(2.5);
        const QColor drawnGap = pixel(24.5);
        if (state.checked) {
            QVERIFY2(contrastRatio(drawnInside, drawnGap) >= 3.0,
                     qPrintable(describeContrast(name + QStringLiteral(": the fill on the plate"), drawnInside, drawnGap)));
        }
        QVERIFY2(contrastRatio(letterColor, drawnInside) >= 4.5,
                 qPrintable(describeContrast(name + QStringLiteral(": the letter"), letterColor, drawnInside)));

        // The letter is drawn: its most contrasting pixel is the letter's
        // colour, within antialiasing, and stands out from the button.
        const QRectF letterRect =
            letter->mapRectToScene(QRectF(0, 0, letter->width(), letter->height()));
        QColor letterPixel = drawnInside;
        for (int y = static_cast<int>(letterRect.top() * scale); y < static_cast<int>(letterRect.bottom() * scale); ++y) {
            for (int x = static_cast<int>(letterRect.left() * scale); x < static_cast<int>(letterRect.right() * scale);
                 ++x) {
                const QColor candidate = image.pixelColor(x, y);
                if (contrastRatio(candidate, drawnInside) > contrastRatio(letterPixel, drawnInside)) {
                    letterPixel = candidate;
                }
            }
        }
        QVERIFY2(channelDistance(letterPixel, letterColor) <= 24,
                 qPrintable(QStringLiteral("%1: the letter's strongest pixel is %2, not %3")
                                .arg(name, letterPixel.name(), letterColor.name())));
        QVERIFY2(contrastRatio(letterPixel, drawnInside) >= 4.5,
                 qPrintable(describeContrast(name + QStringLiteral(": the drawn letter"), letterPixel, drawnInside)));

        // The icon is drawn in the glyph colour: the middle of the fixture's
        // square, against the button around it.
        const QColor iconColor = colorOf(plotColors, state.checked ? "checkedGlyph" : "plotText");
        QVERIFY(sameColor(colorOf(icon, "color"), iconColor));
        const QPointF iconOrigin = iconButton->mapToScene(QPointF(0, 0)) * scale;
        const QColor iconPixel = pixelAt(image, iconOrigin + QPointF(12 * scale, 12 * scale));
        const QColor iconBehind = pixelAt(image, iconOrigin + QPointF(2.5 * scale, 12 * scale));
        QVERIFY2(channelDistance(iconPixel, iconColor) <= 3,
                 qPrintable(QStringLiteral("%1: the icon is %2, not %3").arg(name, iconPixel.name(), iconColor.name())));
        QVERIFY2(contrastRatio(iconPixel, iconBehind) >= 4.5,
                 qPrintable(describeContrast(name + QStringLiteral(": the drawn icon"), iconPixel, iconBehind)));
        if (state.focused) {
            const QColor drawnRing = pixel(25.5);
            QVERIFY2(contrastRatio(drawnRing, drawnGap) >= 3.0,
                     qPrintable(describeContrast(name + QStringLiteral(": the ring on the plate"), drawnRing, drawnGap)));
        }
    }

    // Space checks the focused button.
    button->setProperty("checked", false);
    button->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(button->property("visualFocus").toBool());
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(button->property("checked").toBool());
}

void UiFoundationsTest::plotToolButtonIsAnAccessibleCheckableButton() {
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root = createWindow(engine, themedWindowQml(kBreezeDark, kPlotItems), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QQuickItem *button = itemNamed(root.get(), QStringLiteral("secondButton"));
    QVERIFY(button != nullptr);

    QAccessibleInterface *accessible = QAccessible::queryAccessibleInterface(button);
    QVERIFY(accessible != nullptr);
    // Qt reports a checkable button as a check box: a toggle button.
    QCOMPARE(accessible->role(), QAccessible::CheckBox);
    QCOMPARE(accessible->text(QAccessible::Name), QStringLiteral("Mute L"));
    QVERIFY(!accessible->state().checked);
    QVERIFY(button->property("activeFocusOnTab").toBool());

    // A click checks it.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, button->mapToScene(QPointF(12, 12)).toPoint());
    QTRY_VERIFY(button->property("checked").toBool());
    QVERIFY(accessible->state().checked);
}

void UiFoundationsTest::activeSelectionIsUnmistakableInDark_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("breeze dark") << true;
    QTest::newRow("breeze light") << false;
}

// On dark, the selection of the list with the keyboard is unmistakable
// against another list's (they were 1.04:1 and 6 ΔE apart), and still under
// the pointer, with every text and mark of a selected row keeping its
// contrast on it. Light is as it was.
void UiFoundationsTest::activeSelectionIsUnmistakableInDark() {
    QFETCH(bool, dark);
    QQmlEngine engine;
    QString errorText;
    const std::unique_ptr<QObject> root =
        createWindow(engine, themedWindowQml(dark ? kBreezeDark : kBreezeLight, QString()), &errorText);
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QObject *tokens = root->property("tokens").value<QObject *>();
    QVERIFY(tokens != nullptr);
    const QColor active = colorOf(tokens, "selection");
    const QColor inactive = colorOf(tokens, "selectionInactive");
    const QColor inactiveHovered = colorOf(tokens, "selectionInactiveHovered");
    if (dark) {
        QVERIFY2(contrastRatio(active, inactive) >= 1.17 && labDistance(active, inactive) >= 9.0,
                 qPrintable(describeContrast(QStringLiteral("active on inactive"), active, inactive)
                            + QStringLiteral(", %1 ΔE").arg(labDistance(active, inactive))));
        // Another list's under the pointer is still the less selected.
        QVERIFY2(contrastRatio(active, colorOf(tokens, "view")) > contrastRatio(inactiveHovered, colorOf(tokens, "view"))
                     && labDistance(active, inactiveHovered) >= 7.5,
                 qPrintable(inactiveHovered.name()));
    } else {
        QVERIFY(sameColor(active, fromHex(QStringLiteral("#b5e0f7"))));
        QVERIFY(sameColor(colorOf(tokens, "textSecondarySelected"), colorOf(tokens, "textSecondary")));
        QVERIFY(sameColor(colorOf(tokens, "negativeSelected"), colorOf(tokens, "negative")));
    }
    const QList<std::pair<const char *, double>> drawn{
        {"text", 4.5}, {"textColumn", 4.5}, {"textSecondarySelected", 4.5}, {"now", 4.5},
        {"textTertiarySelected", 5.4}, {"negativeSelected", 3.0}, {"focusRing", 3.0}};
    for (const char *under : {"selection", "selectionInactive", "selectionHovered", "selectionInactiveHovered"}) {
        for (const auto &[token, minimum] : drawn) {
            QVERIFY2(contrastRatio(colorOf(tokens, token), colorOf(tokens, under)) >= minimum,
                     qPrintable(describeContrast(QLatin1String(token) + QStringLiteral(" on ") + QLatin1String(under),
                                                 colorOf(tokens, token), colorOf(tokens, under))));
        }
    }
}

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // The basic render loop, whatever the environment asks for: the threaded
    // one installs an animation driver of its own for the GUI thread, and Qt
    // then takes no SteppedAnimationDriver.
    qputenv("QSG_RENDER_LOOP", "basic");
    // The app's controls style and Breeze icons, whatever the desktop the
    // tests run in; TestSessionIsolation.h. With the style, Kirigami draws
    // theme icons in the colour asked for, as the plot tool buttons' glyphs
    // need. KDE's icon loader recolours an icon by name when the theme
    // follows the colour scheme, as Breeze does. Without a theme, it looks
    // for the Breeze that an app can embed, which this one does not.
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
    if (!installIconFixture(fixtures.filePath(QStringLiteral("data-dirs")))) {
        return 1;
    }
    QApplication app(argc, argv);
    returnFocusFromClosedPopups();
    UiFoundationsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_foundations.moc"
