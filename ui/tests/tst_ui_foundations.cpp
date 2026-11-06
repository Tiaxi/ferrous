// SPDX-License-Identifier: GPL-3.0-or-later

// The design tokens of UiPalette and the shared components that the main
// window, the visualization and the dialogs are built from (#373): the row
// background, the region header, the plot plate and the plot tool button.

#include <QAccessible>
#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QQuickWindow>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>
#include <memory>

#include "TestPlatform.h"

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

// The largest difference of the colours' 8-bit channels.
int channelDistance(const QColor &a, const QColor &b) {
    const auto channel = [](double value) { return static_cast<int>(std::lround(value * 255.0)); };
    return std::max({std::abs(channel(a.redF()) - channel(b.redF())),
                     std::abs(channel(a.greenF()) - channel(b.greenF())),
                     std::abs(channel(a.blueF()) - channel(b.blueF()))});
}

// Whether the colours are the same to 8 bits a channel, alpha included,
// however each was made (QColor's == also compares how it stores them).
bool sameColor(const QColor &a, const QColor &b) {
    return channelDistance(a, b) == 0 && std::lround(a.alphaF() * 255.0) == std::lround(b.alphaF() * 255.0);
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
    void rowBackgroundShowsEachState_data();
    void rowBackgroundShowsEachState();
    void rowBackgroundDrawsTheRingAndTintAsOneRectangle();
    void regionHeaderLaysOutNameTotalsAndHairline();
    void plotPlateLabelHasAnEdgeOutside();
    void plotToolButtonDrawsCheckedAndFocusApart_data();
    void plotToolButtonDrawsCheckedAndFocusApart();
    void plotToolButtonIsAnAccessibleCheckableButton();
};

void UiFoundationsTest::initTestCase() {
    previousHandler() = qInstallMessageHandler(recordWarnings);
    QVERIFY2(!QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                     QStringLiteral("icons/breeze/index.theme"))
                  .isEmpty(),
             "The Breeze icon theme is missing: install breeze-icon-theme (docs/INSTALL.md)");
}

void UiFoundationsTest::cleanup() {
    const QStringList list = takeWarnings();
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
        {"textTertiary", QStringLiteral("#8f959a"), QStringLiteral("#646d77")},
        {"textColumn", QStringLiteral("#c2c2c3"), QStringLiteral("#5a5c5e")},
        {"seam", QStringLiteral("#383b40"), QStringLiteral("#d6d9dc")},
        {"hairline", QStringLiteral("#26292c"), QStringLiteral("#ebedef")},
        {"chromeLine", QStringLiteral("#383b40"), QStringLiteral("#c9cdd1")},
        {"selection", QStringLiteral("#1f3e4e"), QStringLiteral("#d4edfa")},
        {"focusRing", QStringLiteral("#6cc5f2"), QStringLiteral("#1a74a8")},
        {"link", QStringLiteral("#3daee9"), QStringLiteral("#1a74a8")},
        {"negative", QStringLiteral("#ed5a6a"), QStringLiteral("#c4293b")},
        {"wave", QStringLiteral("#b9e1f5"), QStringLiteral("#2c566c")},
        {"now", QStringLiteral("#f2b035"), QStringLiteral("#955500")},
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
    QVERIFY(std::abs(hover.alphaF() - (dark ? 0.065 : 0.08)) < 0.002);
    QVERIFY(channelDistance(colorOf(tokens, "selectionInactive"), mixed(view, accent, dark ? 0.20 : 0.16)) <= 1);
    const QColor dim = colorOf(tokens, "dim");
    QCOMPARE(channelDistance(dim, QColor(Qt::black)), 0);
    QVERIFY(std::abs(dim.alphaF() - (dark ? 0.5 : 0.25)) < 0.002);

    // Tertiary text steps up to secondary on a selected row.
    QVariant stepped;
    QVERIFY(QMetaObject::invokeMethod(tokens, "textTertiaryOn", Q_RETURN_ARG(QVariant, stepped),
                                      Q_ARG(QVariant, QVariant(true))));
    QCOMPARE(stepped.value<QColor>(), colorOf(tokens, "textSecondary"));
    QVariant plain;
    QVERIFY(QMetaObject::invokeMethod(tokens, "textTertiaryOn", Q_RETURN_ARG(QVariant, plain),
                                      Q_ARG(QVariant, QVariant(false))));
    QCOMPARE(plain.value<QColor>(), colorOf(tokens, "textTertiary"));

    // The old names keep their colours until their users move.
    QVERIFY2(sameColor(colorOf(tokens, "uiTextColor"), text), qPrintable(colorOf(tokens, "uiTextColor").name(QColor::HexArgb) + QStringLiteral(" vs ") + text.name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(tokens, "uiWaveformColor"), colorOf(tokens, "wave")), qPrintable(colorOf(tokens, "uiWaveformColor").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "wave").name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(tokens, "uiSurfaceRaisedColor"), view), qPrintable(colorOf(tokens, "uiSurfaceRaisedColor").name(QColor::HexArgb) + QStringLiteral(" vs ") + view.name(QColor::HexArgb)));
}

void UiFoundationsTest::tokensMeetTheirContrast_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<double>("secondaryRatio");
    QTest::addColumn<bool>("secondaryOnChrome");
    QTest::addColumn<double>("tertiaryRatio");
    // The ratios the design lists, on the backgrounds it measured them on.
    QTest::newRow("breeze dark") << true << 7.75 << true << 6.0;
    QTest::newRow("breeze light") << false << 7.1 << false << 5.3;
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
             qPrintable(describe(QStringLiteral("secondary"), secondary, secondaryBackground)));
    QVERIFY2(std::abs(contrastRatio(tertiary, view) - tertiaryRatio) <= 0.15,
             qPrintable(describe(QStringLiteral("tertiary"), tertiary, view)));

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
        {QStringLiteral("tertiary on a hovered row"), tertiary, hovered, 4.5},
        {QStringLiteral("column text on view"), colorOf(tokens, "textColumn"), view, 4.5},
        {QStringLiteral("column text on the selection"), colorOf(tokens, "textColumn"), selection, 4.5},
        {QStringLiteral("text on the selection"), colorOf(tokens, "text"), selection, 4.5},
        {QStringLiteral("playing title on view"), colorOf(tokens, "now"), view, 4.5},
        {QStringLiteral("playing title on the selection"), colorOf(tokens, "now"), selection, 4.5},
        {QStringLiteral("playing title on the inactive selection"), colorOf(tokens, "now"), selectionInactive, 4.5},
        {QStringLiteral("link on view"), colorOf(tokens, "link"), view, 4.5},
        {QStringLiteral("negative on view"), colorOf(tokens, "negative"), view, 4.5},
        {QStringLiteral("negative on chrome"), colorOf(tokens, "negative"), chrome, 4.5},
        {QStringLiteral("focus ring on the selection"), colorOf(tokens, "focusRing"), selection, 3.0},
        {QStringLiteral("focus ring on view"), colorOf(tokens, "focusRing"), view, 3.0},
    };
    for (const Pair &pair : pairs) {
        QVERIFY2(contrastRatio(pair.foreground, pair.background) >= pair.minimum,
                 qPrintable(describe(pair.what, pair.foreground, pair.background)));
    }
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
    QVERIFY2(sameColor(colorOf(spectrogram, "plotBg"), fromHex(QStringLiteral("#14062e"))), qPrintable(colorOf(spectrogram, "plotBg").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#14062e")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotOutline"), fromHex(QStringLiteral("#14062e"))), qPrintable(colorOf(spectrogram, "plotOutline").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#14062e")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotLaneSeam"), fromHex(QStringLiteral("#0a0318"))), qPrintable(colorOf(spectrogram, "plotLaneSeam").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#0a0318")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotText"), fromHex(QStringLiteral("#ece8f4"))), qPrintable(colorOf(spectrogram, "plotText").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#ece8f4")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotLink"), fromHex(QStringLiteral("#7cc8f0"))), qPrintable(colorOf(spectrogram, "plotLink").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#7cc8f0")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "meterTrough"), fromHex(QStringLiteral("#2b1f43"))), qPrintable(colorOf(spectrogram, "meterTrough").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#2b1f43")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "meterFill"), fromHex(QStringLiteral("#a39bb3"))), qPrintable(colorOf(spectrogram, "meterFill").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#a39bb3")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "meterPeak"), fromHex(QStringLiteral("#f4f4f6"))), qPrintable(colorOf(spectrogram, "meterPeak").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#f4f4f6")).name(QColor::HexArgb)));
    // A checked button: the accent, its glyph the floor; focus: the dark
    // theme's focus ring.
    QVERIFY(sameColor(colorOf(spectrogram, "checkedFill"), colorOf(tokens, "accent")));
    QVERIFY(sameColor(colorOf(spectrogram, "checkedGlyph"), fromHex(QStringLiteral("#14062e"))));
    QVERIFY(channelDistance(colorOf(spectrogram, "plotFocusRing"), fromHex(QStringLiteral("#6cc5f2"))) <= 2);
    QVERIFY(std::abs(colorOf(spectrogram, "plotPlate").alphaF() - 0.88) < 0.002);
    QCOMPARE(channelDistance(colorOf(spectrogram, "plotPlate"), fromHex(QStringLiteral("#14062e"))), 0);
    QVERIFY(std::abs(colorOf(spectrogram, "plotPlateEdge").alphaF() - 0.14) < 0.002);
    QVERIFY(std::abs(colorOf(spectrogram, "plotTextSecondary").alphaF() - 0.72) < 0.002);
    QVERIFY(std::abs(colorOf(spectrogram, "plotTick").alphaF() - 0.75) < 0.002);
    QVERIFY(std::abs(colorOf(spectrogram, "meterClipOff").alphaF() - 0.2) < 0.002);

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
    QVERIFY(channelDistance(colorOf(waveform, "meterFill"),
                            fromHex(dark ? QStringLiteral("#7d8790") : QStringLiteral("#5b6670")))
            <= 3);
    // A checked button: the accent in dark, the focus ring's darker blue in
    // light; its glyph the view in dark, white in light.
    QVERIFY(channelDistance(colorOf(waveform, "checkedFill"),
                            fromHex(dark ? QStringLiteral("#3daee9") : QStringLiteral("#1a74a8")))
            <= 2);
    QVERIFY(sameColor(colorOf(waveform, "checkedGlyph"), dark ? view : QColor(Qt::white)));
    QVERIFY(sameColor(colorOf(waveform, "plotFocusRing"), colorOf(tokens, "focusRing")));

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
            {QStringLiteral("a link on the plot"), colorOf(plot, "plotLink"), floor, 4.5},
            {QStringLiteral("the checked fill on a plate"), checked, plate, 3.0},
            {QStringLiteral("the checked glyph or letter on the fill"), colorOf(plot, "checkedGlyph"), checked, 4.5},
            {QStringLiteral("the focus ring on a plate"), colorOf(plot, "plotFocusRing"), plate, 3.0},
            {QStringLiteral("the meter's fill"), colorOf(plot, "meterFill"), trough, 3.0},
            {QStringLiteral("the meter's peak"), drawnOver(colorOf(plot, "meterPeak"), floor), trough, 3.0},
        };
        for (const auto &[what, foreground, background, minimum] : pairs) {
            QVERIFY2(contrastRatio(foreground, background) >= minimum,
                     qPrintable(describe(name + QStringLiteral(", ") + what, foreground, background)));
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

void UiFoundationsTest::rowBackgroundShowsEachState_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("hovered");
    QTest::addColumn<bool>("selected");
    QTest::addColumn<bool>("active");
    QTest::addColumn<bool>("focused");
    QTest::addColumn<QString>("tint");
    for (const bool dark : {true, false}) {
        const QString theme = dark ? QStringLiteral("dark, ") : QStringLiteral("light, ");
        QTest::newRow(qPrintable(theme + QStringLiteral("rest"))) << dark << false << false << true << false << "";
        QTest::newRow(qPrintable(theme + QStringLiteral("hovered"))) << dark << true << false << true << false << "";
        QTest::newRow(qPrintable(theme + QStringLiteral("selected")))
            << dark << false << true << true << false << "selection";
        QTest::newRow(qPrintable(theme + QStringLiteral("selected without focus")))
            << dark << false << true << false << false << "selectionInactive";
        QTest::newRow(qPrintable(theme + QStringLiteral("selected and focused")))
            << dark << true << true << true << true << "selection";
        QTest::newRow(qPrintable(theme + QStringLiteral("focused, not selected")))
            << dark << false << false << true << true << "";
    }
}

void UiFoundationsTest::rowBackgroundShowsEachState() {
    QFETCH(bool, dark);
    QFETCH(bool, hovered);
    QFETCH(bool, selected);
    QFETCH(bool, active);
    QFETCH(bool, focused);
    QFETCH(QString, tint);
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

    // The row leaves the 8 px scrollbar lane free.
    QCOMPARE(background->x(), 0.0);
    QCOMPARE(background->width(), row->width() - 8.0);
    QCOMPARE(background->height(), row->height());

    background->setProperty("hovered", hovered);
    background->setProperty("selected", selected);
    background->setProperty("active", active);
    background->setProperty("focused", focused);

    QTRY_COMPARE(hoverTint->opacity(), hovered ? 1.0 : 0.0);
    QCOMPARE(hoverTint->isVisible(), hovered);
    QVERIFY2(sameColor(colorOf(hoverTint, "color"), colorOf(tokens, "hover")), qPrintable(colorOf(hoverTint, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(tokens, "hover").name(QColor::HexArgb)));
    QCOMPARE(tintRect->isVisible(), selected || focused);
    if (tint.isEmpty()) {
        QCOMPARE(colorOf(tintRect, "color").alphaF(), 0.0f);
    } else {
        QVERIFY(sameColor(colorOf(tintRect, "color"), colorOf(tokens, qPrintable(tint))));
    }
    QObject *border = tintRect->property("border").value<QObject *>();
    QVERIFY(border != nullptr);
    QCOMPARE(border->property("width").toReal(), focused ? 1.0 : 0.0);
    QCOMPARE(channelDistance(colorOf(border, "color"), colorOf(tokens, "focusRing")), 0);
    // The ring and the tint are one rectangle.
    QCOMPARE(tintRect->size(), background->size());
}

// The focused, selected row draws its 1 px ring on the edge of its tint,
// and nothing in the scrollbar lane.
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
    expectPixel(QPointF(topLeft.x() + width + 4 * scale, middle), view, "the scrollbar lane");
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    QVERIFY(totalsFont.featureTags().contains(QFont::Tag("tnum")));
    QCOMPARE(totalsFont.featureValue(QFont::Tag("tnum")), 1u);
#endif
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

    // Narrow, the totals elide rather than run into the right end.
    header->setProperty("totals", QStringLiteral("12 artists · 28 albums · 176 tracks"));
    header->setWidth(180);
    QTRY_VERIFY(totals->property("truncated").toBool());
    QVERIFY(totals->mapToItem(header, QPointF(totals->width(), 0)).x() <= trailing->mapToItem(header, QPointF(0, 0)).x());
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
                     qPrintable(describe(name + QStringLiteral(": the fill on the plate"), drawnInside, drawnGap)));
        }
        QVERIFY2(contrastRatio(letterColor, drawnInside) >= 4.5,
                 qPrintable(describe(name + QStringLiteral(": the letter"), letterColor, drawnInside)));

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
                 qPrintable(describe(name + QStringLiteral(": the drawn letter"), letterPixel, drawnInside)));

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
                 qPrintable(describe(name + QStringLiteral(": the drawn icon"), iconPixel, iconBehind)));
        if (state.focused) {
            const QColor drawnRing = pixel(25.5);
            QVERIFY2(contrastRatio(drawnRing, drawnGap) >= 3.0,
                     qPrintable(describe(name + QStringLiteral(": the ring on the plate"), drawnRing, drawnGap)));
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

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // The app's controls style, which it needs to start (docs/INSTALL.md).
    // With it, Kirigami draws theme icons in the colour asked for, as the
    // plot tool buttons' glyphs need.
    QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
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
    // The same platform whatever the desktop the tests run in: without a
    // desktop, Qt loads no desktop's platform theme.
    qunsetenv("XDG_CURRENT_DESKTOP");
    qunsetenv("QT_QPA_PLATFORMTHEME");
    QApplication app(argc, argv);
    // Breeze, as a KDE session's platform theme sets it: KDE's icon loader
    // recolours an icon by name when the theme follows the colour scheme,
    // as Breeze does. Without a theme, it looks for the Breeze that an app
    // can embed, which this one does not.
    QIcon::setThemeName(QStringLiteral("breeze"));
    UiFoundationsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_foundations.moc"
