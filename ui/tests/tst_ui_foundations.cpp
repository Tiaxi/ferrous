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
#include <QQuickWindow>
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
    QVERIFY2(sameColor(colorOf(spectrogram, "checkedGlyph"), QColor(Qt::white)), qPrintable(colorOf(spectrogram, "checkedGlyph").name(QColor::HexArgb) + QStringLiteral(" vs ") + QColor(Qt::white).name(QColor::HexArgb)));
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
    QVERIFY(channelDistance(colorOf(waveform, "checkedGlyph"),
                            fromHex(dark ? QStringLiteral("#7cc8f0") : QStringLiteral("#1a74a8")))
            <= 2);

    for (QObject *plot : {spectrogram, waveform}) {
        const QString name = plot == spectrogram ? QStringLiteral("spectrogram") : QStringLiteral("waveform");
        const QColor floor = colorOf(plot, "plotBg");
        const QColor plate = drawnOver(colorOf(plot, "plotPlate"), floor);
        const QColor trough = drawnOver(colorOf(plot, "meterTrough"), floor);
        const QColor checked = drawnOver(colorOf(plot, "checkedTint"), plate);
        QVERIFY(std::abs(colorOf(plot, "checkedTint").alphaF() - 0.45) < 0.002);
        const QList<std::tuple<QString, QColor, QColor, double>> pairs{
            {QStringLiteral("text on the plot"), colorOf(plot, "plotText"), floor, 4.5},
            {QStringLiteral("text on a plate"), colorOf(plot, "plotText"), plate, 4.5},
            {QStringLiteral("secondary text on the plot"), drawnOver(colorOf(plot, "plotTextSecondary"), floor),
             floor, 4.5},
            {QStringLiteral("a link on the plot"), colorOf(plot, "plotLink"), floor, 4.5},
            {QStringLiteral("the checked glyph"), colorOf(plot, "checkedGlyph"), checked, 3.0},
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
    const qreal scale = image.devicePixelRatio();
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
    QCOMPARE(title->width(), title->implicitWidth());
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
                    icon.name: "view-media-visualization"
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

// Checked is the tint with the checked glyph colour; keyboard focus is a
// separate 2 px accent ring; checked and focused shows both.
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
    QObject *tokens = root->property("tokens").value<QObject *>();
    QQuickItem *plot = itemNamed(root.get(), QStringLiteral("plot"));
    QVERIFY(tokens != nullptr && plot != nullptr);
    plot->setProperty("plotMode", plotMode);
    QObject *plotColors = plot->property("plotColors").value<QObject *>();
    QVERIFY(plotColors != nullptr);
    QQuickItem *checkedButton = itemNamed(root.get(), QStringLiteral("firstButton"));
    QQuickItem *plainButton = itemNamed(root.get(), QStringLiteral("secondButton"));
    QVERIFY(checkedButton != nullptr && plainButton != nullptr);
    QCOMPARE(checkedButton->size(), QSizeF(24, 24));

    struct Parts {
        QQuickItem *checked;
        QQuickItem *ring;
        QQuickItem *icon;
        QQuickItem *glyph;
    };
    const auto partsOf = [](QQuickItem *button) {
        return Parts{childNamed(button, QStringLiteral("plotToolButtonChecked")),
                     childNamed(button, QStringLiteral("plotToolButtonFocusRing")),
                     childNamed(button, QStringLiteral("plotToolButtonIcon")),
                     childNamed(button, QStringLiteral("plotToolButtonGlyph"))};
    };
    const Parts checkedParts = partsOf(checkedButton);
    const Parts plainParts = partsOf(plainButton);
    for (const Parts &parts : {checkedParts, plainParts}) {
        QVERIFY(parts.checked != nullptr && parts.ring != nullptr && parts.icon != nullptr && parts.glyph != nullptr);
    }
    const QColor accent = colorOf(tokens, "accent");

    // At rest: neither tint nor ring, the glyph in the plot's text colour.
    QVERIFY(!plainParts.checked->isVisible());
    QVERIFY(!plainParts.ring->isVisible());
    QVERIFY(plainParts.glyph->isVisible());
    QVERIFY(!plainParts.icon->isVisible());
    QCOMPARE(plainParts.glyph->property("text").toString(), QStringLiteral("M"));
    QVERIFY2(sameColor(colorOf(plainParts.glyph, "color"), colorOf(plotColors, "plotText")), qPrintable(colorOf(plainParts.glyph, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(plotColors, "plotText").name(QColor::HexArgb)));

    // Checked: the tint, and the checked glyph colour.
    QVERIFY(checkedParts.checked->isVisible());
    QVERIFY(!checkedParts.ring->isVisible());
    QVERIFY(checkedParts.icon->isVisible());
    QVERIFY2(sameColor(colorOf(checkedParts.checked, "color"), colorOf(plotColors, "checkedTint")), qPrintable(colorOf(checkedParts.checked, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + colorOf(plotColors, "checkedTint").name(QColor::HexArgb)));
    QCOMPARE(channelDistance(colorOf(checkedParts.icon, "color"), colorOf(plotColors, "checkedGlyph")), 0);

    // Keyboard focus: a 2 px accent ring, and no tint.
    plainButton->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(plainButton->property("visualFocus").toBool());
    QVERIFY(plainParts.ring->isVisible());
    QVERIFY(!plainParts.checked->isVisible());
    QObject *ringBorder = plainParts.ring->property("border").value<QObject *>();
    QCOMPARE(ringBorder->property("width").toReal(), 2.0);
    QVERIFY2(sameColor(colorOf(ringBorder, "color"), accent), qPrintable(colorOf(ringBorder, "color").name(QColor::HexArgb) + QStringLiteral(" vs ") + accent.name(QColor::HexArgb)));
    QCOMPARE(colorOf(plainParts.ring, "color").alphaF(), 0.0f);
    QCOMPARE(plainParts.ring->size(), plainButton->size());

    // Space checks it.
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(plainButton->property("checked").toBool());
    QVERIFY(plainParts.checked->isVisible());
    QCOMPARE(channelDistance(colorOf(plainParts.glyph, "color"), colorOf(plotColors, "checkedGlyph")), 0);

    // Checked and focused: both.
    checkedButton->forceActiveFocus(Qt::TabFocusReason);
    QTRY_VERIFY(checkedButton->property("visualFocus").toBool());
    QVERIFY(checkedParts.checked->isVisible());
    QVERIFY(checkedParts.ring->isVisible());

    // Drawn: the ring at the edge, the tint inside it, over the plate on
    // the plot.
    const QImage image = window->grabWindow();
    QVERIFY(!image.isNull());
    const qreal scale = image.devicePixelRatio();
    const QColor floor = colorOf(plotColors, "plotBg");
    const QColor plate = drawnOver(colorOf(plotColors, "plotPlate"), floor);
    const QColor tint = drawnOver(colorOf(plotColors, "checkedTint"), plate);
    const QPointF origin = checkedButton->mapToScene(QPointF(0, 0)) * scale;
    const auto expectPixel = [&](const QPointF &point, const QColor &wanted, const char *what) {
        const QColor actual = pixelAt(image, point);
        QVERIFY2(channelDistance(actual, wanted) <= 3,
                 qPrintable(QStringLiteral("%1 at %2,%3 is %4, not %5")
                                .arg(QLatin1String(what))
                                .arg(point.x())
                                .arg(point.y())
                                .arg(actual.name(), wanted.name())));
    };
    expectPixel(origin + QPointF(0.5, 12 * scale), accent, "the focus ring");
    expectPixel(origin + QPointF(1.5 * scale, 12 * scale), accent, "the focus ring's second pixel");
    expectPixel(origin + QPointF(2.5 * scale, 12 * scale), tint, "the checked tint inside the ring");
    // Between the buttons: the plate.
    expectPixel(origin + QPointF(25 * scale, 12 * scale), plate, "the plate between the buttons");
    // Outside the plate: its edge, then the plot.
    const QPointF plateOrigin =
        itemNamed(root.get(), QStringLiteral("switchPlate"))->mapToScene(QPointF(0, 0)) * scale;
    expectPixel(plateOrigin + QPointF(-0.5 * scale, 14 * scale), drawnOver(colorOf(plotColors, "plotPlateEdge"), floor),
                "the plate's edge");
    expectPixel(plateOrigin + QPointF(-1.5 * scale, 14 * scale), floor, "the plot");
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
    QApplication app(argc, argv);
    UiFoundationsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_foundations.moc"
