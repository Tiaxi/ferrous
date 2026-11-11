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
#include <QScopeGuard>
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

#include "TestPlatform.h"
#include "TestSessionIsolation.h"

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

// The CIE76 distance of two opaque colours in CIELAB, under D65.
double labDistance(const QColor &a, const QColor &b) {
    const auto lab = [](const QColor &color) {
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
    };
    const std::array<double, 3> la = lab(a);
    const std::array<double, 3> lb = lab(b);
    return std::hypot(la[0] - lb[0], la[1] - lb[1], la[2] - lb[2]);
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
    void regionHeaderGivesUpTheTotalsBeforeTheName_data();
    void regionHeaderGivesUpTheTotalsBeforeTheName();
    void plotPlateLabelHasAnEdgeOutside();
    void smallControlsScaleWithALargeFont_data();
    void smallControlsScaleWithALargeFont();
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
        // Lightened or darkened from the mix to be 4.6:1 on a hovered row.
        {"textTertiary", QStringLiteral("#91989f"), QStringLiteral("#59626d")},
        {"textColumn", QStringLiteral("#c2c2c3"), QStringLiteral("#5a5c5e")},
        {"seam", QStringLiteral("#383b40"), QStringLiteral("#d6d9dc")},
        {"hairline", QStringLiteral("#26292c"), QStringLiteral("#ebedef")},
        {"chromeLine", QStringLiteral("#383b40"), QStringLiteral("#c9cdd1")},
        {"selection", QStringLiteral("#1f3e4e"), QStringLiteral("#d4edfa")},
        {"focusRing", QStringLiteral("#6cc5f2"), QStringLiteral("#1a74a8")},
        {"link", QStringLiteral("#3daee9"), QStringLiteral("#1b73a1")},
        {"negative", QStringLiteral("#ed5a6a"), QStringLiteral("#c4293b")},
        {"wave", QStringLiteral("#b9e1f5"), QStringLiteral("#2c566c")},
        // Light: darkened from #955500 to be 4.5:1 on a hovered row.
        {"now", QStringLiteral("#f2b035"), QStringLiteral("#905200")},
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
    QVERIFY(sameColor(colorOf(tokens, "hoveredRow"), drawnOver(hover, view)));
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
    const auto linkOn = [tokens](const QColor &background) {
        QVariant link;
        QMetaObject::invokeMethod(tokens, "linkOn", Q_RETURN_ARG(QVariant, link), Q_ARG(QVariant, background));
        return link.value<QColor>();
    };
    // Where the link already reads, it stays the link.
    QVERIFY(sameColor(linkOn(view), colorOf(tokens, "link")));

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
        // A margin over 4.5:1 on the view and a hovered row, so rendering
        // cannot tip the row numbers under it.
        {QStringLiteral("tertiary on view"), tertiary, view, 4.6},
        {QStringLiteral("tertiary on a hovered row"), tertiary, hovered, 4.6},
        {QStringLiteral("column text on view"), colorOf(tokens, "textColumn"), view, 4.5},
        {QStringLiteral("column text on the selection"), colorOf(tokens, "textColumn"), selection, 4.5},
        {QStringLiteral("text on the selection"), colorOf(tokens, "text"), selection, 4.5},
        {QStringLiteral("playing title on view"), colorOf(tokens, "now"), view, 4.5},
        {QStringLiteral("playing title on a hovered row"), colorOf(tokens, "now"), hovered, 4.5},
        {QStringLiteral("playing title on the selection"), colorOf(tokens, "now"), selection, 4.5},
        {QStringLiteral("playing title on the inactive selection"), colorOf(tokens, "now"), selectionInactive, 4.5},
        {QStringLiteral("link on view"), colorOf(tokens, "link"), view, 4.5},
        // The dialogs' links: About, About KDE and Recent Issues.
        {QStringLiteral("link on chrome"), colorOf(tokens, "link"), chrome, 4.5},
        {QStringLiteral("link on a hovered button on chrome"), linkOn(drawnOver(colorOf(tokens, "hover"), chrome)),
         drawnOver(colorOf(tokens, "hover"), chrome), 4.5},
        {QStringLiteral("negative on view"), colorOf(tokens, "negative"), view, 4.5},
        {QStringLiteral("negative on chrome"), colorOf(tokens, "negative"), chrome, 4.5},
        {QStringLiteral("focus ring on the selection"), colorOf(tokens, "focusRing"), selection, 3.0},
        {QStringLiteral("focus ring on view"), colorOf(tokens, "focusRing"), view, 3.0},
    };
    for (const Pair &pair : pairs) {
        QVERIFY2(contrastRatio(pair.foreground, pair.background) >= pair.minimum,
                 qPrintable(describe(pair.what, pair.foreground, pair.background)));
    }

    // The quiet amber mark of a collapsed tree row is at least 60% and
    // 3:1 on every background a row can have.
    const double markOpacity = tokens->property("nowMarkOpacity").toDouble();
    QVERIFY2(markOpacity >= 0.6 && markOpacity <= 1.0, qPrintable(QString::number(markOpacity)));
    for (const QColor &background : {view, hovered, selection, selectionInactive}) {
        QColor mark = colorOf(tokens, "now");
        mark.setAlphaF(static_cast<float>(markOpacity));
        const QColor drawn = drawnOver(mark, background);
        QVERIFY2(contrastRatio(drawn, background) >= 3.0,
                 qPrintable(describe(QStringLiteral("the tree's amber mark"), drawn, background)));
    }

    // A hovered row stands out from the view at 1.3:1, yet is nearer to it
    // than a selected row is, in lightness and hue together.
    QVERIFY2(contrastRatio(hovered, view) >= 1.3, qPrintable(describe(QStringLiteral("hover tint"), hovered, view)));
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
    QVERIFY2(sameColor(colorOf(spectrogram, "plotBg"), fromHex(QStringLiteral("#000000"))), qPrintable(colorOf(spectrogram, "plotBg").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#000000")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotOutline"), fromHex(QStringLiteral("#000000"))), qPrintable(colorOf(spectrogram, "plotOutline").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#000000")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotLaneSeam"), fromHex(QStringLiteral("#1a1a1a"))), qPrintable(colorOf(spectrogram, "plotLaneSeam").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#1a1a1a")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotText"), fromHex(QStringLiteral("#ececec"))), qPrintable(colorOf(spectrogram, "plotText").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#ececec")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "plotLink"), fromHex(QStringLiteral("#7cc8f0"))), qPrintable(colorOf(spectrogram, "plotLink").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#7cc8f0")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "meterTrough"), fromHex(QStringLiteral("#2b1f43"))), qPrintable(colorOf(spectrogram, "meterTrough").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#2b1f43")).name(QColor::HexArgb)));
    QVERIFY2(sameColor(colorOf(spectrogram, "meterFill"), fromHex(QStringLiteral("#8f87a3"))), qPrintable(colorOf(spectrogram, "meterFill").name(QColor::HexArgb) + QStringLiteral(" vs ") + fromHex(QStringLiteral("#8f87a3")).name(QColor::HexArgb)));
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
    // On the colour map's black floor, what is drawn is neutral, not tinted,
    // except the level meter, which is in the colour map's violet (#373 H5).
    for (const char *token : {"plotPlate", "plotStrip", "plotText", "plotTextSecondary", "plotLaneSeam",
                              "plotTick"}) {
        const QColor color = colorOf(spectrogram, token);
        QVERIFY2(color.red() == color.green() && color.green() == color.blue(),
                 qPrintable(QLatin1String(token) + QStringLiteral(" ") + color.name(QColor::HexArgb)));
    }
    // The lane seam and the meter's track stand out from the floor, and the
    // meter's gaps from its track. Its fill is quieter than the plot's text.
    QVERIFY(contrastRatio(colorOf(spectrogram, "plotLaneSeam"), colorOf(spectrogram, "plotBg")) >= 1.15);
    QVERIFY(contrastRatio(colorOf(spectrogram, "meterTrough"), colorOf(spectrogram, "plotBg")) >= 1.3);
    QVERIFY(contrastRatio(colorOf(spectrogram, "meterTrough"), colorOf(spectrogram, "meterSeam")) >= 1.3);
    QVERIFY(contrastRatio(colorOf(spectrogram, "meterFill"), colorOf(spectrogram, "plotBg"))
            < contrastRatio(colorOf(spectrogram, "plotText"), colorOf(spectrogram, "plotBg")));
    // A viewer's title strip is opaque (#373 H5: the playhead showed through
    // it): the plate on the floor, and the view on the waveform.
    QCOMPARE(colorOf(spectrogram, "plotStrip").alphaF(), 1.0f);
    QVERIFY(channelDistance(colorOf(spectrogram, "plotStrip"),
                            drawnOver(colorOf(spectrogram, "plotPlate"), colorOf(spectrogram, "plotBg")))
            <= 1);

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
    // A checked flat button, as on a viewer's title strip: the selection on
    // the view, and the dark theme's selection on the spectrogram's floor.
    QVERIFY(sameColor(colorOf(waveform, "checkedTint"), colorOf(tokens, "selection")));
    QVERIFY(channelDistance(colorOf(spectrogram, "checkedTint"),
                            mixed(fromHex(QStringLiteral("#000000")), colorOf(tokens, "accent"), 0.26))
            <= 1);
    // A viewer's title strip is a plate over the plot: on the spectrogram,
    // its secondary text reads over the colour map's brightest white too.
    {
        const QColor stripOverWhite = drawnOver(colorOf(spectrogram, "plotPlate"), QColor(Qt::white));
        const QColor secondary = drawnOver(colorOf(spectrogram, "plotTextSecondary"), stripOverWhite);
        QVERIFY2(contrastRatio(secondary, stripOverWhite) >= 4.5,
                 qPrintable(describe(QStringLiteral("strip text over white"), secondary, stripOverWhite)));
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
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    if (pointSize > 0) {
        QFont large = applicationFont;
        large.setPointSizeF(pointSize);
        QGuiApplication::setFont(large);
    }
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
    QTest::addColumn<int>("iconSize");
    QTest::addColumn<int>("buttonSize");
    QTest::newRow("10 pt") << 10.0 << 16 << 24;
    // 2.4 times the default: 16 px is 38 px, and the button 4 px more
    // around it.
    QTest::newRow("24 pt") << 24.0 << 38 << 46;
}

// The plot's and the viewers' small tool buttons and their glyphs are 24
// and 16 px at the default 10 pt font, and grow with a larger one as the
// transport's do; a lane label's plate holds its text's line, 20 px tall
// at the default font, with 6 px either side of the text.
void UiFoundationsTest::smallControlsScaleWithALargeFont() {
    QFETCH(qreal, pointSize);
    QFETCH(int, iconSize);
    QFETCH(int, buttonSize);
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont font = applicationFont;
    font.setPointSizeF(pointSize);
    QGuiApplication::setFont(font);
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
        QVERIFY2(label->height() >= text->implicitHeight() + 6,
                 qPrintable(QStringLiteral("%1 for %2").arg(label->height()).arg(text->implicitHeight())));
    }
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
    UiFoundationsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui_foundations.moc"
