// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"
#include "TestColor.h"

#include <QFontInfo>
#include <QFontMetricsF>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQmlIncubationController>
#include <QTextLayout>
#include <QTextOption>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

// The album art viewer: its zoom and details, and the iTunes artwork
// preview.
class AlbumArtSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void albumArtZoomFollowsTheWheelDelta();
    void albumArtZoomKeepsTheImagePointUnderThePointer_data();
    void albumArtZoomKeepsTheImagePointUnderThePointer();
    void albumArtSmallerThanTheRequestIsShownScaledUp();
    void albumArtViewerShowsTheArtAndItsControls_data();
    void albumArtViewerShowsTheArtAndItsControls();
    void albumArtViewerShowsThePreloadedCoverAtTwiceTheScale();
    void itunesPreviewAppliesOrGoesBackToTheResults();
    void itunesPreviewButtonsTakeSpaceFromThePlaybackShortcut();
    void itunesPreviewButtonsStayWholeInTheDetails_data();
    void itunesPreviewButtonsStayWholeInTheDetails();
    void itunesPreviewCannotApplyACandidateThatIsNotReady();
    void itunesPreviewShowsTheSearchedAlbumsArtAfterPlaybackMovesOn();
    void itunesPreviewApplyShowsTheAppliedCoverAfterPlaybackMovesOn();
    void itunesAppliedCoverGivesWayWhenTheSameTrackPlaysAgain();
    void itunesArtworkResultsAreAFlatList();
    void itunesArtworkDialogShowsAPlaceholderWhileSearchingAndWithNothingFound();
    void itunesArtworkHoldsStillBetweenItsStates_data();
    void itunesArtworkHoldsStillBetweenItsStates();
    void itunesArtworkActionsFollowTheTextFont();
    void itunesReplaceShowsWhatWasChosen_data();
    void itunesReplaceShowsWhatWasChosen();
    void itunesReplaceStatesTheOutcomeThatApplies_data();
    void itunesReplaceStatesTheOutcomeThatApplies();
    void itunesArtworkCountLineHoldsAtLargeFonts();
    void itunesArtworkDialogOpensWithItsRowsWhole();
    void itunesArtworkListIsAsTallAsItsRows();
    void itunesArtworkScalesAsAWholeWithTheFont_data();
    void itunesArtworkScalesAsAWholeWithTheFont();
    void albumArtDetailsLieOverTheArt();
    void albumArtDetailsLabelsStayWholeWithLargerFonts();
    void albumArtFolderPathTakesTheLinesItNeedsAtLargeFonts();
    void albumArtKeepsAGutterBesideTheDetails_data();
    void albumArtKeepsAGutterBesideTheDetails();
    void albumArtDetailsAreOnTheTypeScale_data();
    void albumArtDetailsAreOnTheTypeScale();
    void albumArtPreviewFooterIsOnTheTypeScale_data();
    void albumArtPreviewFooterIsOnTheTypeScale();
    void albumArtViewerShowsWhenItsImageCannotBeRead();
    void albumArtDetailsAreOnThePlotStripInEitherTheme_data();
    void albumArtDetailsAreOnThePlotStripInEitherTheme();
    void albumArtStripSetsTheZoomOffAsItsPlateDoes_data();
    void albumArtStripSetsTheZoomOffAsItsPlateDoes();
    void albumArtPanShowsItsHand_data();
    void albumArtPanShowsItsHand();
    void albumArtCopyPathIsOnTheFoldersLineUnderClose_data();
    void albumArtCopyPathIsOnTheFoldersLineUnderClose();
};

namespace {

// What createSurfaceWindow shows: the image at `source` in a window of
// `size`, which the viewer decodes for unless `decodeSize` is given.
struct SurfaceOptions {
    QSize size;
    QString source;
    QSize decodeSize;
    bool infoVisible = false;
};

// A window of the album art viewer's view alone, which fills it: its
// `surface`, its palette as `tokens`, and how often the view asked to close
// as `closeCount`.
QObject *createSurfaceWindow(QQmlEngine &engine, const SurfaceOptions &options, QString *errorText) {
    const QSize decode = options.decodeSize.isValid() ? options.decodeSize : options.size;
    engine.rootContext()->setContextProperty(
        QStringLiteral("testSurface"),
        QVariantMap{{QStringLiteral("width"), options.size.width()},
                    {QStringLiteral("height"), options.size.height()},
                    {QStringLiteral("source"), options.source},
                    {QStringLiteral("decodeWidth"), decode.width()},
                    {QStringLiteral("decodeHeight"), decode.height()},
                    {QStringLiteral("infoVisible"), options.infoVisible}});
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    return createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: testSurface.width
    height: testSurface.height
    visible: true
    property alias surface: surface
    readonly property var tokens: surface.uiPalette
    property int closeCount: 0

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testSurface.source
        infoVisible: testSurface.infoVisible
        initialViewToken: 0
        viewerDecodeWidth: testSurface.decodeWidth
        viewerDecodeHeight: testSurface.decodeHeight
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() { surfaceWindow.closeCount += 1 }
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, errorText);
}

} // namespace

void AlbumArtSmokeTest::albumArtZoomFollowsTheWheelDelta() {
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createSurfaceWindow(engine, {QSize(400, 400), QString(), QSize(), false}, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QObject *surface = qvariant_cast<QObject *>(root->property("surface"));
    QVERIFY(surface != nullptr);
    const QPointF center(200, 200);
    const auto zoom = [surface]() { return surface->property("zoom").toDouble(); };
    const double perNotch = surface->property("zoomPerNotch").toDouble();
    QVERIFY(perNotch > 1.0);

    // An eighth of a notch zooms by an eighth of a notch's factor.
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, 15)));
    QVERIFY(std::abs(zoom() - std::pow(perNotch, 0.125)) < 1e-9);
    double expected = zoom();
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, 360)));
    expected *= std::pow(perNotch, 3.0);
    QVERIFY(std::abs(zoom() - expected) < 1e-9);

    // A pixel-only event counts by its pixels.
    QVERIFY(sendWheel(window, center, QPoint(0, 60), QPoint()));
    expected *= std::pow(perNotch, 0.5);
    QVERIFY(std::abs(zoom() - expected) < 1e-9);

    // Events with no vertical delta leave the zoom alone.
    sendWheel(window, center, QPoint(), QPoint(120, 0));
    QVERIFY(std::abs(zoom() - expected) < 1e-9);

    // Zooming out is as fine-grained, down to the fitted size.
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, -15)));
    expected /= std::pow(perNotch, 0.125);
    QVERIFY(std::abs(zoom() - expected) < 1e-9);
    QVERIFY(sendWheel(window, center, QPoint(), QPoint(0, -1200)));
    QCOMPARE(zoom(), 1.0);
}

void AlbumArtSmokeTest::albumArtZoomKeepsTheImagePointUnderThePointer_data() {
    QTest::addColumn<int>("viewportWidth");
    // The square cover fills a square viewer, so zooming at any point on it
    // keeps that point under the pointer.
    QTest::newRow("cover fills the viewer") << 400;
    // In a wider viewer the cover leaves bands at its sides, and the pan
    // limits stop it short of keeping points near its sides in place.
    QTest::newRow("cover narrower than the viewer") << 600;
}

void AlbumArtSmokeTest::albumArtZoomKeepsTheImagePointUnderThePointer() {
    QFETCH(int, viewportWidth);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(400, 400, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("covers"), new CoverImageProvider);
    QString errorText;
    QScopedPointer<QObject> root(createSurfaceWindow(
        engine, {QSize(viewportWidth, 400), CoverImageProvider::urlForPath(coverPath), QSize(400, 400), false},
        &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *surface = qvariant_cast<QQuickItem *>(root->property("surface"));
    QVERIFY(surface != nullptr);
    QList<QQuickItem *> items;
    collectQuickItems(surface, &items);
    QQuickItem *image = nullptr;
    for (QQuickItem *each : items) {
        if (each->inherits("QQuickImage")) {
            image = each;
            break;
        }
    }
    QVERIFY(image != nullptr);
    // Image.Ready is 1.
    QTRY_COMPARE_WITH_TIMEOUT(image->property("status").toInt(), 1, 5000);
    // The image fills the item that is panned and zoomed, inside the viewport.
    QQuickItem *transform = image->parentItem();
    QVERIFY(transform != nullptr);
    QQuickItem *viewport = transform->parentItem();
    QVERIFY(viewport != nullptr);
    QCOMPARE(viewport->size(), QSizeF(viewportWidth, 400));
    QCOMPARE(transform->size(), QSizeF(400, 400));

    const auto zoom = [surface]() { return surface->property("zoom").toDouble(); };
    const auto pan = [surface]() {
        return QPointF(surface->property("panX").toDouble(), surface->property("panY").toDouble());
    };
    const QPointF viewportCenter(viewport->width() / 2, viewport->height() / 2);
    const QPointF imageCenter(image->width() / 2, image->height() / 2);
    const auto verifyFitted = [&]() -> QString {
        const QPointF shownCenter = image->mapToItem(viewport, imageCenter);
        if (zoom() != 1.0 || pan() != QPointF() || shownCenter != viewportCenter) {
            return QStringLiteral("not fitted: zoom %1, pan %2,%3, centre at %4,%5")
                .arg(zoom())
                .arg(pan().x())
                .arg(pan().y())
                .arg(shownCenter.x())
                .arg(shownCenter.y());
        }
        return {};
    };

    int keptAxes = 0;
    int clampedAxes = 0;
    // One axis of a zoom at `pointer`: the image point that was under the
    // pointer now shows at `shown`, with the pan at `panNow` (`panBefore`
    // before the zoom) and limited to +-`limit`.
    const auto verifyAxis = [&](char axis, double pointer, double shown, double panNow,
                                double panBefore, double limit) -> QString {
        const double drift = shown - pointer;
        // Panning moves the image one to one, so this pan would bring the
        // point back under the pointer.
        const double needed = panNow - drift;
        if (std::abs(needed) <= limit + 0.5) {
            ++keptAxes;
            if (std::abs(drift) > 0.5) {
                return QStringLiteral("%1 drifted %2 px from the pointer at zoom %3")
                    .arg(axis)
                    .arg(drift)
                    .arg(zoom());
            }
            return {};
        }
        // The pan limits stop the point short of the pointer: the image
        // still moves as far toward it as they allow.
        ++clampedAxes;
        if (std::abs(std::abs(panNow) - limit) > 1e-6 || panNow * needed < 0) {
            return QStringLiteral("%1 pan %2 is not at the limit %3 toward the pointer (needs %4)")
                .arg(axis)
                .arg(panNow)
                .arg(limit)
                .arg(needed);
        }
        const double driftUnpanned = drift - (panNow - std::clamp(panBefore, -limit, limit));
        if (std::abs(drift) > std::abs(driftUnpanned) + 1e-6) {
            return QStringLiteral("%1 moved away from the pointer: drift %2, %3 without panning")
                .arg(axis)
                .arg(drift)
                .arg(driftUnpanned);
        }
        return {};
    };
    const auto verifyZoom = [&](QPointF pointer, QPointF imagePoint, QPointF panBefore) -> QString {
        const QPointF shown = image->mapToItem(viewport, imagePoint);
        const QPointF panNow = pan();
        const double limitX = std::max(0.0, (transform->width() * zoom() - viewport->width()) / 2);
        const double limitY = std::max(0.0, (transform->height() * zoom() - viewport->height()) / 2);
        QString failure = verifyAxis('x', pointer.x(), shown.x(), panNow.x(), panBefore.x(), limitX);
        if (failure.isEmpty()) {
            failure = verifyAxis('y', pointer.y(), shown.y(), panNow.y(), panBefore.y(), limitY);
        }
        if (!failure.isEmpty()) {
            failure += QStringLiteral(" (pointer %1,%2)").arg(pointer.x()).arg(pointer.y());
        }
        return failure;
    };

    // Near each corner of the cover, and at its centre.
    const QRectF coverRect = transform->mapRectToItem(viewport, QRectF(QPointF(), transform->size()));
    const QRectF inset = coverRect.adjusted(10, 10, -10, -10);
    const QList<QPointF> pointers{
        inset.topLeft(), inset.topRight(), inset.bottomLeft(), inset.bottomRight(), inset.center()};
    QVERIFY2(verifyFitted().isEmpty(), qPrintable(verifyFitted()));

    for (const QPointF &pointer : pointers) {
        const QPointF scenePointer = viewport->mapToItem(window->contentItem(), pointer);
        // Zoom in a notch at a time, then back out to the fitted view.
        for (const int delta : {120, -120}) {
            for (int step = 0; step < 12 && (delta > 0 || zoom() > 1.0); ++step) {
                const QPointF imagePoint = image->mapFromItem(viewport, pointer);
                const QPointF panBefore = pan();
                const double zoomBefore = zoom();
                QVERIFY(sendWheel(window, scenePointer, QPoint(), QPoint(0, delta)));
                QVERIFY(delta > 0 ? zoom() > zoomBefore : zoom() < zoomBefore);
                const QString failure = verifyZoom(pointer, imagePoint, panBefore);
                QVERIFY2(failure.isEmpty(), qPrintable(failure));
            }
        }
        QVERIFY2(verifyFitted().isEmpty(), qPrintable(verifyFitted()));
    }

    for (const QPointF &pointer : pointers) {
        const QPoint scenePointer = viewport->mapToItem(window->contentItem(), pointer).toPoint();
        // A double-click zooms to twice the fitted size at the pointer.
        const QPointF imagePoint = image->mapFromItem(viewport, pointer);
        const QPointF panBefore = pan();
        QTest::mouseMove(window, scenePointer);
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, scenePointer);
        QCOMPARE(zoom(), 2.0);
        const QString failure = verifyZoom(pointer, imagePoint, panBefore);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // Another fits the cover again.
        QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, scenePointer);
        QVERIFY2(verifyFitted().isEmpty(), qPrintable(verifyFitted()));
    }
    QCOMPARE(root->property("closeCount").toInt(), 0);

    QVERIFY(keptAxes > 0);
    if (viewportWidth == 400) {
        QCOMPARE(clampedAxes, 0);
    } else {
        QVERIFY(clampedAxes > 0);
    }
}

void AlbumArtSmokeTest::albumArtSmallerThanTheRequestIsShownScaledUp() {
    // The cover provider decodes a cover smaller than the requested size at
    // its own size; the album art viewer and the tile still show it fitted
    // to their area, as they did when it was enlarged on decoding.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(300, 200, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("covers"), new CoverImageProvider);
    engine.rootContext()->setContextProperty(
        QStringLiteral("testCoverUrl"), CoverImageProvider::urlForPath(coverPath));
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: 1200
    height: 600
    visible: true
    property alias surface: surface
    property alias tile: tile

    Action { id: replaceFromItunesAction }
    QtObject {
        id: bridge
        property string currentTrackCoverPath: testCoverUrl
    }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testCoverUrl
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 1200
        viewerDecodeHeight: 600
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }

    Components.AlbumArtTile {
        id: tile
        width: 320
        height: 320
        uiBridge: bridge
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        openAlbumArtViewer: function() {}
    }
}
)QML"), baseUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));

    const auto imageIn = [&root](const char *name) -> QQuickItem * {
        auto *item = qvariant_cast<QQuickItem *>(root->property(name));
        QList<QQuickItem *> items;
        if (item != nullptr) {
            collectQuickItems(item, &items);
        }
        for (QQuickItem *each : items) {
            if (each->inherits("QQuickImage")) {
                return each;
            }
        }
        return nullptr;
    };
    struct Expected {
        const char *view;
        QSizeF painted;
        QPointF paintedCenter;
    };
    // The viewer fits the cover to its 1200x600 area, centred; the tile to
    // its 320x320 square.
    const QList<Expected> views{
        {"surface", QSizeF(900, 600), QPointF(600, 300)},
        {"tile", QSizeF(320, 320.0 * 2 / 3), QPointF(160, 160)},
    };
    for (const Expected &expected : views) {
        QQuickItem *image = imageIn(expected.view);
        QVERIFY2(image != nullptr, expected.view);
        // Image.Ready is 1.
        QTRY_COMPARE_WITH_TIMEOUT(image->property("status").toInt(), 1, 5000);
        QCOMPARE(QSize(image->property("implicitWidth").toInt(),
                       image->property("implicitHeight").toInt()),
                 QSize(300, 200));
        const QSizeF painted(image->property("paintedWidth").toReal(),
                             image->property("paintedHeight").toReal());
        QVERIFY2(std::abs(painted.width() - expected.painted.width()) < 0.5
                     && std::abs(painted.height() - expected.painted.height()) < 0.5,
                 qPrintable(QStringLiteral("%1 painted %2x%3")
                                .arg(QLatin1String(expected.view))
                                .arg(painted.width())
                                .arg(painted.height())));
        const QPointF center = image->mapToScene(
            QPointF(image->width() / 2, image->height() / 2));
        QCOMPARE(center, expected.paintedCenter);
    }
}

namespace {

// The time of every animation of the GUI thread, while it lives: a frame of
// it at each pass of the event loop, rather than real time, so that fades
// and slides end within a few passes. For tests that check where those end,
// never along them. As SteppedAnimationDriver, it needs the basic render
// loop, whose windows install no driver of their own.
class FastAnimationDriver final : public QAnimationDriver {
public:
    FastAnimationDriver() {
        m_frames.setInterval(0);
        QObject::connect(&m_frames, &QTimer::timeout, &m_frames, [this]() {
            m_elapsed += 16;
            advance();
        });
        install();
    }
    ~FastAnimationDriver() override { uninstall(); }
    FastAnimationDriver(const FastAnimationDriver &) = delete;
    FastAnimationDriver &operator=(const FastAnimationDriver &) = delete;

    qint64 elapsed() const override { return m_elapsed; }

protected:
    void start() override {
        QAnimationDriver::start();
        m_frames.start();
    }
    void stop() override {
        m_frames.stop();
        QAnimationDriver::stop();
    }

private:
    QTimer m_frames;
    qint64 m_elapsed = 0;
};


// How much wider than at its 280 px the album art viewer's details column
// is in a view `viewWidth` wide: over the whole screen a seventh; in the
// window a quarter of the view, between 280 and 320 px.
qreal infoColumnWidthScale(bool fullscreen, qreal viewWidth) {
    return fullscreen ? 1.2 : qBound(280.0, std::floor(viewWidth / 4), 320.0) / 280.0;
}

// The details column's width, less its seam, in a view `viewWidth` wide at
// the default font.
qreal infoColumnWidth(bool fullscreen, qreal viewWidth) {
    return std::round(280 * infoColumnWidthScale(fullscreen, viewWidth)) - 1;
}

// The details' column's 40 characters, less its seam: of the body font in
// a window, as much wider as the column in a wide one; over the whole
// screen, of the details' text, 1.2 times the body font.
qreal infoColumnFortyCharacters(const QFont &bodyFont, bool fullscreen, qreal viewWidth) {
    if (fullscreen) {
        QFont details = bodyFont;
        details.setPointSizeF(bodyFont.pointSizeF() * 1.2);
        return std::ceil(QFontMetricsF(details).averageCharWidth() * 40) - 1;
    }
    return std::ceil(QFontMetricsF(bodyFont).averageCharWidth() * 40 * infoColumnWidthScale(false, viewWidth)) - 1;
}

// Where `image` draws its picture, in its window.
QRectF paintedRectOf(QQuickItem *image) {
    const QSizeF painted(image->property("paintedWidth").toReal(), image->property("paintedHeight").toReal());
    return QRectF(image->mapToScene(QPointF((image->width() - painted.width()) / 2,
                                            (image->height() - painted.height()) / 2)),
                  painted);
}

// The first image under `root`.
QQuickItem *firstImageIn(QQuickItem *root) {
    QList<QQuickItem *> items;
    collectQuickItems(root, &items);
    const auto image = std::find_if(items.cbegin(), items.cend(),
                                    [](QQuickItem *each) { return each->inherits("QQuickImage"); });
    return image != items.cend() ? *image : nullptr;
}

// The cover provider, recording the size of every image it is asked for.
class RecordingCoverImageProvider final : public CoverImageProvider {
public:
    QQuickImageResponse *requestImageResponse(
        const QString &id, const QSize &requestedSize) override {
        {
            QMutexLocker locker(&m_mutex);
            m_requests.append(requestedSize);
        }
        return CoverImageProvider::requestImageResponse(id, requestedSize);
    }

    QList<QSize> requests() const {
        QMutexLocker locker(&m_mutex);
        return m_requests;
    }

private:
    mutable QMutex m_mutex;
    QList<QSize> m_requests;
};

// Main.qml, active, with a track playing, and what the album art tests use
// of it: with the cover provider recording what it is asked for.
struct AlbumArtWindow {
    AlbumArtWindow() : provider(new RecordingCoverImageProvider) {
        engine.addImageProvider(QStringLiteral("covers"), provider);
        window = loadMainWindow(engine, libraryModel);
        if (window == nullptr) {
            return;
        }
        bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        palette = qvariant_cast<QObject *>(window->property("uiPalette"));
        viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
        controlsAutoHide = window->findChild<QObject *>(QStringLiteral("albumArtFullscreenControlsAutoHide"));
        if (bridge != nullptr) {
            setPlayingTrack(bridge);
        }
        // The viewer's controls stay shown while the checks look at them.
        // Its auto-hide's timer runs on the animations' time, a frame at
        // each pass of the event loop here, so its 5 s ran out while a
        // viewer opened, and the pointer, off the window, wakes none.
        if (controlsAutoHide != nullptr) {
            controlsAutoHide->setProperty("hideDelay", 600000);
        }
    }
    bool loaded() const {
        return window != nullptr && bridge != nullptr && palette != nullptr && viewer != nullptr && surface != nullptr
            && controlsAutoHide != nullptr;
    }
    QObject *plot() const { return qvariant_cast<QObject *>(palette->property("mediaPlot")); }
    // Opens the viewer on `coverUrl`, the playing track's art, in the main
    // window or over the whole screen (`viewerMode` 0 or 1): the window it
    // shows in once the art has loaded, and the popup and the details have
    // faded in or out, or null.
    QQuickWindow *openViewer(const QString &coverUrl, int viewerMode) {
        bridge->setProperty("currentTrackCoverPath", coverUrl);
        bridge->setProperty("viewerFullscreenMode", viewerMode);
        QMetaObject::invokeMethod(viewer, "openAlbumArtViewer");
        if (!QTest::qWaitFor([&]() { return viewer->property("albumArtViewerOpen").toBool(); })) {
            return nullptr;
        }
        QQuickWindow *shownIn =
            viewerMode == 1 ? viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow")) : window;
        if (shownIn == nullptr || !QTest::qWaitForWindowActive(shownIn)
            || !QTest::qWaitFor([&]() {
                   const qreal reveal = viewer->property("albumArtInfoVisible").toBool() ? 1.0 : 0.0;
                   return surface->window() == shownIn && surface->isVisible()
                       && surface->property("imageStatus").toInt() == 1
                       && surface->property("infoReveal").toReal() == reveal && popupsSettled();
               })) {
            return nullptr;
        }
        return shownIn;
    }
    // Closes the viewer with Escape in `shownIn`; whether it closed.
    bool closeViewer(QQuickWindow *shownIn) const {
        QTest::keyClick(shownIn, Qt::Key_Escape);
        return QTest::qWaitFor([&]() { return !viewer->property("albumArtViewerOpen").toBool(); });
    }

    // The fades and slides, each to its end at once: the checks that use
    // this window check where they end.
    FastAnimationDriver animations;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    RecordingCoverImageProvider *provider;
    QQuickWindow *window = nullptr;
    QObject *bridge = nullptr;
    QObject *palette = nullptr;
    QObject *viewer = nullptr;
    QQuickItem *surface = nullptr;
    QObject *controlsAutoHide = nullptr;
};

// The hidden preload decodes the current cover as the album art viewer
// asks for it, so that opening the viewer decodes nothing more: the cover
// is decoded once at the viewer's size.
void checkPreloadedCover(AlbumArtWindow &ui, int viewerMode, double scale) {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 300, 200);
    QVERIFY(!coverPath.isEmpty());
    auto *preload = ui.window->findChild<QQuickItem *>(QStringLiteral("albumArtViewerPreloadImage"));
    QQuickItem *viewerImage = firstImageIn(ui.surface);
    QVERIFY(preload != nullptr && viewerImage != nullptr);
    ui.bridge->setProperty("viewerFullscreenMode", viewerMode);

    // Image.Ready is 1. The implicit size is in logical pixels.
    const auto readyWithTheCover = [scale](QQuickItem *image) {
        return image->property("status").toInt() == 1
            && QSizeF(image->property("implicitWidth").toReal(),
                      image->property("implicitHeight").toReal()) * scale == QSizeF(300, 200);
    };
    ui.bridge->setProperty("currentTrackCoverPath", CoverImageProvider::urlForPath(coverPath));
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(preload), 5000);

    QVERIFY(QMetaObject::invokeMethod(ui.viewer, "openAlbumArtViewerForCurrentArt"));
    QTRY_VERIFY(ui.viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(viewerImage), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(preload), 5000);
    QCOMPARE(viewerImage->property("sourceSize"), preload->property("sourceSize"));
    QCOMPARE(viewerImage->window()->effectiveDevicePixelRatio(), scale);

    // Give a second request time to reach the provider before counting.
    QTest::qWait(100);
    const QSize viewerRequest =
        viewerImage->property("sourceSize").toSize() * viewerImage->window()->effectiveDevicePixelRatio();
    const QList<QSize> requests = ui.provider->requests();
    QVERIFY2(requests.count(viewerRequest) == 1,
             qPrintable(QStringLiteral("%1 requests at %2x%3")
                            .arg(requests.count(viewerRequest))
                            .arg(viewerRequest.width())
                            .arg(viewerRequest.height())));

    QVERIFY(QMetaObject::invokeMethod(ui.viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!ui.viewer->property("albumArtViewerOpen").toBool());
}

// WCAG's contrast ratio of `color`, with its alpha, drawn over the opaque
// `background`, against that background.
qreal contrastOn(QRgb color, QRgb background) {
    return contrastRatio(drawnOver(QColor::fromRgba(color), QColor(background)), QColor(background));
}

// A media surface is dark in both themes: in a light theme, a dark cover
// sat between white bars. The art is on black, in the main window and over
// the whole screen, and the viewer's controls, the corner plate or the
// title strip, and the details are the spectrogram's: the same set in both
// viewers.
void checkBlackAroundTheArtInALightTheme(AlbumArtWindow &ui, int viewerMode) {
    QObject *plot = ui.plot();
    QVERIFY(plot != nullptr);
    // The tests' theme is a light one, where the view colour was white.
    QVERIFY(!ui.palette->property("themeIsDark").toBool());
    QCOMPARE(colorOf(plot, "plotBg"), qRgb(0, 0, 0));
    // The spectrogram's set, on black rather than on the plot's floor.
    QObject *spectrogramPlot = qvariant_cast<QObject *>(ui.palette->property("spectrogramPlot"));
    QVERIFY(spectrogramPlot != nullptr);
    QCOMPARE(colorOf(plot, "plotStrip"), colorOf(spectrogramPlot, "plotStrip"));
    QCOMPARE(colorOf(plot, "plotText"), colorOf(spectrogramPlot, "plotText"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QQuickWindow *shownIn = ui.openViewer(QUrl::fromLocalFile(savedCover(dir, 300, 200)).toString(), viewerMode);
    QVERIFY(shownIn != nullptr);
    QCOMPARE(ui.surface->property("fullscreen").toBool(), viewerMode == 1);
    if (viewerMode == 1) {
        QCOMPARE(shownIn->color().rgba(), qRgb(0, 0, 0));
    } else {
        // The black is the art surface's floor, the popup's only one: it
        // draws no background of its own.
        QObject *popup = ui.window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
        QVERIFY(popup != nullptr);
        auto *background = qvariant_cast<QQuickItem *>(popup->property("background"));
        QVERIFY(background != nullptr);
        QVERIFY(!background->flags().testFlag(QQuickItem::ItemHasContents));
        QQuickItem *floor = itemNamed(ui.surface, QStringLiteral("albumArtFloor"));
        QVERIFY(floor != nullptr && floor->isVisible());
        QCOMPARE(colorOf(floor, "color"), qRgb(0, 0, 0));
    }

    // The controls are in the plot's colours: the corner plate's buttons
    // or the title strip.
    const QString prefix = viewerMode == 1 ? QStringLiteral("albumArtFullscreen") : QStringLiteral("albumArtPopup");
    QQuickItem *close = visibleItemNamed(shownIn, prefix + QStringLiteral("CloseButton"));
    QQuickItem *info = visibleItemNamed(shownIn, prefix + QStringLiteral("InfoButton"));
    QVERIFY(close != nullptr && info != nullptr);
    QVERIFY(qvariant_cast<QObject *>(close->property("plotColors")) == plot);
    QVERIFY(qvariant_cast<QObject *>(info->property("plotColors")) == plot);
    if (viewerMode == 1) {
        QQuickItem *strip = visibleItemNamed(shownIn, QStringLiteral("albumArtFullscreenStrip"));
        QVERIFY(strip != nullptr);
        QVERIFY(qvariant_cast<QObject *>(strip->property("plotColors")) == plot);
    } else {
        QQuickItem *controls = visibleItemNamed(shownIn, QStringLiteral("albumArtPopupControls"));
        QVERIFY(controls != nullptr);
        QQuickItem *fill = itemNamed(controls, QStringLiteral("plotPlateFill"));
        QVERIFY(fill != nullptr);
        QCOMPARE(colorOf(fill, "color"), colorOf(plot, "plotPlate"));
    }
    // Their focus ring is 3:1 on what they sit on.
    const QRgb controlsBackground = viewerMode == 1
        ? colorOf(plot, "plotStrip")
        : drawnOver(QColor::fromRgba(colorOf(plot, "plotPlate")), QColor(colorOf(plot, "plotBg"))).rgb();
    QVERIFY(contrastOn(colorOf(plot, "plotFocusRing"), controlsBackground) >= 3.0);
    QVERIFY(contrastOn(colorOf(plot, "plotText"), controlsBackground) >= 4.5);

    // Black around the art, wherever it does not reach.
    QQuickItem *image = itemNamed(ui.surface, QStringLiteral("albumArtImage"));
    QQuickItem *viewport = itemNamed(ui.surface, QStringLiteral("albumArtViewport"));
    QVERIFY(image != nullptr && viewport != nullptr);
    const QRectF viewRect = sceneRectOf(viewport);
    QTRY_VERIFY(paintedRectOf(image).width() > 0);
    const QRectF art = paintedRectOf(image);
    QPointF around;
    if (art.left() - viewRect.left() > 8) {
        around = QPointF((viewRect.left() + art.left()) / 2, viewRect.center().y());
    } else {
        QVERIFY(viewRect.bottom() - art.bottom() > 8);
        around = QPointF(viewRect.center().x(), (art.bottom() + viewRect.bottom()) / 2);
    }
    const QImage shown = shownIn->grabWindow().convertToFormat(QImage::Format_RGB32);
    const qreal dpr = shown.devicePixelRatio();
    QCOMPARE(shown.pixel(int(around.x() * dpr), int(around.y() * dpr)), qRgb(0, 0, 0));
    QCOMPARE(shown.pixel(int(art.center().x() * dpr), int(art.center().y() * dpr)), QColor(Qt::darkCyan).rgb());

    // The details: on the plot's strip colour, in its text colours.
    QTest::keyClick(shownIn, Qt::Key_I);
    QTRY_VERIFY(ui.viewer->property("albumArtInfoVisible").toBool());
    QQuickItem *backing = itemNamed(ui.surface, QStringLiteral("albumArtInfoBacking"));
    QQuickItem *title = itemNamed(ui.surface, QStringLiteral("albumArtInfoTitle"));
    QVERIFY(backing != nullptr && title != nullptr);
    QTRY_VERIFY(backing->isVisible());
    QCOMPARE(colorOf(backing, "color"), colorOf(plot, "plotStrip"));
    QCOMPARE(colorOf(title, "color"), colorOf(plot, "plotText"));
    QTest::keyClick(shownIn, Qt::Key_I);
    QTRY_VERIFY(!ui.viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(ui.closeViewer(shownIn));
}

// While the details show, the art fits what they leave of the view and is
// centred there, left of their seam (it kept its place under them, centred
// in the whole view), windowed and over the whole screen; it keeps its
// zoom. The pointer on the details works them rather than the art beneath.
void checkArtFitsLeftOfTheDetails(AlbumArtWindow &ui, int viewerMode) {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Wider than the view, so that it fits the space's width.
    QQuickWindow *shownIn = ui.openViewer(QUrl::fromLocalFile(savedCover(dir, 1600, 800)).toString(), viewerMode);
    QVERIFY(shownIn != nullptr);
    QVERIFY(!ui.viewer->property("albumArtInfoVisible").toBool());
    QQuickItem *surface = ui.surface;
    QQuickItem *viewport = itemNamed(surface, QStringLiteral("albumArtViewport"));
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *seam = itemNamed(surface, QStringLiteral("albumArtInfoSeam"));
    QObject *contextMenu = surface->findChild<QObject *>(QStringLiteral("albumArtViewerContextMenu"));
    QVERIFY(viewport != nullptr && image != nullptr && column != nullptr && seam != nullptr
            && contextMenu != nullptr);
    QTRY_VERIFY(viewport->width() > 400 && viewport->height() > 200);
    struct View {
        QRectF art;
        double zoom;
        double panX;
        double panY;
    };
    const auto view = [&]() {
        return View{paintedRectOf(image), surface->property("zoom").toDouble(),
                    surface->property("panX").toDouble(), surface->property("panY").toDouble()};
    };
    const auto sameView = [](const View &a, const View &b) {
        return a.art == b.art && a.zoom == b.zoom && a.panX == b.panX && a.panY == b.panY;
    };
    const auto describe = [](const View &v) {
        return QStringLiteral("art %1 zoom %2 pan %3,%4").arg(describeRect(v.art)).arg(v.zoom).arg(v.panX).arg(v.panY);
    };
    const QRectF viewRect = sceneRectOf(viewport);
    QCOMPARE(paintedRectOf(image).center().x(), viewRect.center().x());

    // I shows the details over the view's right edge: the art fits left of
    // their seam, centred there.
    QTest::keyClick(shownIn, Qt::Key_I);
    QTRY_VERIFY(ui.viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(column->isVisible() && seam->isVisible());
    // They slide in, and rest at the view's right edge.
    QTRY_COMPARE(sceneRectOf(column).right(), viewRect.right());
    QCOMPARE(sceneRectOf(viewport), viewRect);
    const QRectF columnRect = sceneRectOf(column);
    const QRectF seamRect = sceneRectOf(seam);
    QCOMPARE(columnRect.top(), viewRect.top());
    QCOMPARE(columnRect.bottom(), viewRect.bottom());
    QCOMPARE(seamRect.right(), columnRect.left());
    // 280 px with its seam, 320 px in a window 1280 px wide or more, a
    // quarter of one between, and 336 px over the whole screen, for its
    // text 1.2 times larger.
    QCOMPARE(columnRect.width(), infoColumnWidth(viewerMode == 1, viewRect.width()));
    if (viewerMode == 1) {
        QCOMPARE(columnRect.width(), 335.0);
    } else if (viewRect.width() >= 1280) {
        QCOMPARE(columnRect.width(), 319.0);
    } else if (viewRect.width() <= 1120) {
        QCOMPARE(columnRect.width(), 279.0);
    }
    const QRectF left(viewRect.left(), viewRect.top(), seamRect.left() - viewRect.left(), viewRect.height());
    QTRY_VERIFY2(qAbs(paintedRectOf(image).center().x() - left.center().x()) < 0.5,
                 qPrintable(describe(view()) + QStringLiteral(" / %1").arg(left.center().x())));
    QVERIFY2(qAbs(paintedRectOf(image).center().y() - left.center().y()) < 0.5, qPrintable(describe(view())));
    // The wide art fits the space's width less a 16 px gutter on each side
    // (it met the seam), and runs under neither the seam nor the details.
    const qreal margin = surface->property("artMargin").toReal();
    QCOMPARE(margin, 16.0);
    QVERIFY2(qAbs(paintedRectOf(image).width() - (left.width() - 2 * margin)) < 0.5, qPrintable(describe(view())));
    QVERIFY(paintedRectOf(image).left() >= left.left() + margin - 0.5);
    QVERIFY(paintedRectOf(image).right() <= seamRect.left() - margin + 0.5);
    QCOMPARE(surface->property("zoom").toDouble(), 1.0);
    // The column draws on its own backing, and the art beside it shows.
    const QImage shown = shownIn->grabWindow().convertToFormat(QImage::Format_RGB32);
    const qreal dpr = shown.devicePixelRatio();
    const auto pixelAt = [&shown, dpr](QPointF point) {
        return shown.pixel(int(point.x() * dpr), int(point.y() * dpr));
    };
    // Low in the art, below the details' text.
    const QRectF artShown = paintedRectOf(image).intersected(viewRect);
    QVERIFY2(artShown.bottom() - 8 > columnRect.top() + columnRect.height() / 2,
             qPrintable(QStringLiteral("%1").arg(artShown.bottom())));
    const qreal sampleY = artShown.bottom() - 8;
    QCOMPARE(pixelAt(QPointF(columnRect.center().x(), sampleY)), QColor::fromRgba(colorOf(ui.plot(), "plotStrip")).rgb());
    QCOMPARE(pixelAt(QPointF(seamRect.left() - margin - 8, sampleY)), QColor(Qt::darkCyan).rgb());

    // Zoomed and panned by the pointer on the art.
    const QPointF artPoint = left.center() - QPointF(left.width() / 4, 0);
    QVERIFY(sendWheel(shownIn, artPoint, QPoint(), QPoint(0, 600)));
    const double zoom = surface->property("zoom").toDouble();
    QVERIFY(zoom > 1.0);
    QTest::mousePress(shownIn, Qt::LeftButton, Qt::NoModifier, artPoint.toPoint());
    QTest::mouseMove(shownIn, (artPoint + QPointF(40, 30)).toPoint());
    QTest::mouseMove(shownIn, (artPoint + QPointF(80, 60)).toPoint());
    QTest::mouseRelease(shownIn, Qt::LeftButton, Qt::NoModifier, (artPoint + QPointF(80, 60)).toPoint());
    QVERIFY(surface->property("panX").toDouble() != 0.0 || surface->property("panY").toDouble() != 0.0);
    // The details at rest before the view is taken: under load the reveal's
    // last sub-pixel step could land after it.
    QTRY_COMPARE(surface->property("infoReveal").toDouble(), 1.0);
    const View before = view();

    // The wheel, a drag, a double-click and the context button on the
    // details neither zoom nor pan the art, nor close the viewer.
    const QPoint onColumn = QPointF(columnRect.center().x(), sampleY - 32).toPoint();
    QTest::mouseMove(shownIn, onColumn);
    sendWheel(shownIn, onColumn, QPoint(), QPoint(0, 360));
    sendWheel(shownIn, onColumn, QPoint(), QPoint(0, -720));
    QTest::mousePress(shownIn, Qt::LeftButton, Qt::NoModifier, onColumn);
    QTest::mouseMove(shownIn, onColumn + QPoint(-60, -40));
    QTest::mouseMove(shownIn, onColumn + QPoint(-120, -80));
    QTest::mouseRelease(shownIn, Qt::LeftButton, Qt::NoModifier, onColumn + QPoint(-120, -80));
    QTest::mouseDClick(shownIn, Qt::LeftButton, Qt::NoModifier, onColumn);
    // The seam is the details' too.
    const QPoint onSeam = seamRect.center().toPoint();
    QTest::mouseDClick(shownIn, Qt::LeftButton, Qt::NoModifier, onSeam);
    sendWheel(shownIn, onSeam, QPoint(), QPoint(0, 360));
    QTest::mouseClick(shownIn, Qt::RightButton, Qt::NoModifier, onColumn);
    QTest::qWait(50);
    QVERIFY2(sameView(view(), before), qPrintable(describe(view()) + QStringLiteral(" / ") + describe(before)));
    QVERIFY(!contextMenu->property("visible").toBool());
    QVERIFY(ui.viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(ui.viewer->property("albumArtInfoVisible").toBool());

    // Closing them again keeps the zoom, in the whole view again.
    QTest::keyClick(shownIn, Qt::Key_I);
    QTRY_VERIFY(!ui.viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(!column->isVisible());
    QCOMPARE(surface->property("zoom").toDouble(), zoom);
    QCOMPARE(sceneRectOf(viewport), viewRect);
    // Fitted again, it is centred in the whole view.
    QVERIFY(QMetaObject::invokeMethod(surface, "resetView"));
    QTRY_VERIFY2(qAbs(paintedRectOf(image).center().x() - viewRect.center().x()) < 0.5, qPrintable(describe(view())));
    QVERIFY(ui.closeViewer(shownIn));
}

// The windowed viewer fades in with one black floor under the art. Its
// popup fades its children one by one, not as one layer: with a second
// floor of its own, at half opacity the window showed through only a
// quarter, so the window went dark before the art came in, a blink of
// 33 ms as it opened. At half the popup's opacity, the window beside the art
// shows at half.
void checkWindowedViewerFadesInOverOneFloor(AlbumArtWindow &ui) {
    QObject *popup = ui.window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
    QVERIFY(popup != nullptr);
    // A point beside the square art, clear of the corner's controls.
    const QPoint beside(4, ui.window->height() / 2);
    QTRY_VERIFY(popupsSettled());
    const QColor before = ui.window->grabWindow().pixelColor(beside);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(ui.openViewer(QUrl::fromLocalFile(savedCover(dir, 400, 400)).toString(), 0) == ui.window);
    QQuickItem *image = itemNamed(ui.surface, QStringLiteral("albumArtImage"));
    QVERIFY(image != nullptr);
    QVERIFY2(sceneRectOf(image).left() > beside.x() + 2, "the point is beside the art");
    QCOMPARE(ui.window->grabWindow().pixelColor(beside).rgb(), qRgb(0, 0, 0));

    // Half way through its fade, as the style's transition sets it.
    QVERIFY(popup->setProperty("opacity", 0.5));
    const QColor half = ui.window->grabWindow().pixelColor(beside);
    for (const auto &[got, was] : {std::pair{half.red(), before.red()}, std::pair{half.green(), before.green()},
                                   std::pair{half.blue(), before.blue()}}) {
        QVERIFY2(std::abs(got - was / 2.0) <= 3,
                 qPrintable(QStringLiteral("%1 at half of %2, not %3").arg(half.name(), before.name())
                                .arg(QColor(before.red() / 2, before.green() / 2, before.blue() / 2).name())));
    }
    QVERIFY(popup->setProperty("opacity", 1.0));
    QVERIFY(ui.closeViewer(ui.window));
}

// The windowed viewer's corner plate: zoomed, the readout and Reset are set
// off from the viewer's buttons by a 1 px divider in the plot plate's edge
// colour, with the buttons' 4 px either side and 6 px of plate above and
// below it, as on the visualization viewer's. Not zoomed, none.
void checkWindowedViewerCornerSetsTheZoomOff(AlbumArtWindow &ui) {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(ui.openViewer(QUrl::fromLocalFile(savedCover(dir, 400, 400)).toString(), 0) == ui.window);
    QQuickItem *controls = visibleItemNamed(ui.window, QStringLiteral("albumArtPopupControls"));
    QVERIFY(controls != nullptr);
    QQuickItem *divider = itemNamed(controls, QStringLiteral("viewerControlsDivider"));
    QQuickItem *reset = itemNamed(controls, QStringLiteral("viewerResetButton"));
    QQuickItem *info = itemNamed(controls, QStringLiteral("albumArtPopupInfoButton"));
    QQuickItem *close = itemNamed(controls, QStringLiteral("albumArtPopupCloseButton"));
    QVERIFY(divider != nullptr && reset != nullptr && info != nullptr && close != nullptr);
    QVERIFY(!divider->isVisible());

    ui.surface->setProperty("zoom", 2.5);
    QTRY_VERIFY(divider->isVisible() && reset->isVisible());
    QCOMPARE(divider->width(), 1.0);
    QCOMPARE(sceneRectOf(divider).left() - sceneRectOf(reset).right(), 4.0);
    QCOMPARE(sceneRectOf(info).left() - sceneRectOf(divider).right(), 4.0);
    QCOMPARE(sceneRectOf(divider).top() - sceneRectOf(controls).top(), 6.0);
    QCOMPARE(sceneRectOf(controls).bottom() - sceneRectOf(divider).bottom(), 6.0);
    QCOMPARE(colorOf(divider, "color"), colorOf(ui.plot(), "plotPlateEdge"));
    // The plate holds it all, Close ending 2 px in from its edge.
    QTRY_COMPARE(sceneRectOf(controls).right() - sceneRectOf(close).right(), 2.0);

    ui.surface->setProperty("zoom", 1.0);
    QTRY_VERIFY(!divider->isVisible());
    QVERIFY(ui.closeViewer(ui.window));
}

// Over the whole screen, Image Details is tinted only while the details
// show, and ringed only while it has the keyboard; zoomed, the strip has
// the readout and Reset, which fits the art again.
void checkInfoButtonTintedOnlyWhileChecked(AlbumArtWindow &ui) {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QQuickWindow *fullscreen = ui.openViewer(QUrl::fromLocalFile(savedCover(dir, 400, 400)).toString(), 1);
    QVERIFY(fullscreen != nullptr);
    QQuickItem *info = visibleItemNamed(fullscreen, QStringLiteral("albumArtFullscreenInfoButton"));
    QVERIFY(info != nullptr);
    QQuickItem *tint = itemNamed(info, QStringLiteral("viewerToolButtonChecked"));
    QQuickItem *ring = itemNamed(info, QStringLiteral("viewerToolButtonFocusRing"));
    QQuickItem *edge = itemNamed(info, QStringLiteral("viewerToolButtonCheckedEdge"));
    QQuickItem *column = itemNamed(ui.surface, QStringLiteral("albumArtInfoColumn"));
    QVERIFY(tint != nullptr && ring != nullptr && edge != nullptr && column != nullptr);
    QObject *plot = ui.plot();

    // Off: neither.
    QVERIFY(!ui.viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(!tint->isVisible() && !ring->isVisible());
    QVERIFY(!column->isVisible());

    // A click shows the details: the tint, and no ring.
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(info));
    QTRY_VERIFY(ui.viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(info->property("checked").toBool());
    QVERIFY(tint->isVisible());
    // The mode switch's chosen segment: the plot's text at 18 %, inside a
    // 1 px edge of segmentEdge, rather than an accent tint.
    QColor neutral = QColor::fromRgba(colorOf(plot, "plotText"));
    neutral.setAlphaF(0.18);
    QVERIFY(std::abs(qAlpha(colorOf(tint, "color")) - neutral.alpha()) <= 1);
    QCOMPARE(colorOf(tint, "color") & 0xffffff, neutral.rgba() & 0xffffff);
    QVERIFY(edge->isVisible());
    QCOMPARE(QQmlProperty(edge, QStringLiteral("border.width")).read().toReal(), 1.0);
    QCOMPARE(QQmlProperty(edge, QStringLiteral("border.color")).read().value<QColor>().rgba(),
             colorOf(plot, "segmentEdge"));
    QVERIFY(!ring->isVisible());
    QTRY_VERIFY(column->isVisible());

    // I hides them again, and the button follows.
    QTest::keyClick(fullscreen, Qt::Key_I);
    QTRY_VERIFY(!ui.viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(!info->property("checked").toBool());
    QVERIFY(!tint->isVisible());

    // The keyboard reaches it: the ring for focus, and Space toggles it,
    // with both the tint and the ring.
    QVERIFY(tabTo(fullscreen, info));
    QVERIFY(ring->isVisible());
    QVERIFY(!tint->isVisible());
    QTest::keyClick(fullscreen, Qt::Key_Space);
    QTRY_VERIFY(ui.viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(tint->isVisible() && ring->isVisible());

    // Zoomed, the strip has the readout and Reset, which fits the art again.
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("albumArtFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QQuickItem *readout = itemNamed(strip, QStringLiteral("viewerZoomReadout"));
    QQuickItem *reset = itemNamed(strip, QStringLiteral("viewerResetButton"));
    QVERIFY(readout != nullptr && reset != nullptr);
    QVERIFY(!readout->isVisible() && !reset->isVisible());
    ui.surface->setProperty("zoom", 2.5);
    // In place at once, without waiting for the window to draw.
    QVERIFY(readout->isVisible() && reset->isVisible());
    QVERIFY(clearOfTheOtherButtons(reset) && clearOfTheOtherButtons(readout)
            && sceneRectOf(readout).right() <= sceneRectOf(reset).left());
    QCOMPARE(readout->property("text").toString(), QStringLiteral("Zoom 250%"));
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(reset));
    QTRY_COMPARE(ui.surface->property("zoom").toDouble(), 1.0);
    QTRY_VERIFY(!readout->isVisible());
    QTest::keyClick(fullscreen, Qt::Key_I);
    QTRY_VERIFY(!ui.viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(ui.closeViewer(fullscreen));
}

} // namespace

void AlbumArtSmokeTest::albumArtViewerShowsTheArtAndItsControls_data() {
    QTest::addColumn<int>("viewerMode");
    QTest::newRow("popup") << 0;
    QTest::newRow("whole screen") << 1;
}

// The album art viewer in the main window and over the whole screen: the
// checks above in turn, each with a cover of its own, in one window.
void AlbumArtSmokeTest::albumArtViewerShowsTheArtAndItsControls() {
    QFETCH(int, viewerMode);
    AlbumArtWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    using Check = std::function<void()>;
    QList<Check> checks{
        [&]() { checkPreloadedCover(ui, viewerMode, 1.0); },
        [&]() { checkBlackAroundTheArtInALightTheme(ui, viewerMode); },
        [&]() { checkArtFitsLeftOfTheDetails(ui, viewerMode); },
    };
    if (viewerMode == 0) {
        checks << [&]() { checkWindowedViewerFadesInOverOneFloor(ui); }
               << [&]() { checkWindowedViewerCornerSetsTheZoomOff(ui); };
    } else {
        checks << [&]() { checkInfoButtonTintedOnlyWhileChecked(ui); };
    }
    for (const Check &check : std::as_const(checks)) {
        check();
        if (QTest::currentTestFailed()) {
            return;
        }
    }
}

// As albumArtViewerShowsTheArtAndItsControls's check of the preload, in a
// window on a screen at twice the scale.
void AlbumArtSmokeTest::albumArtViewerShowsThePreloadedCoverAtTwiceTheScale() {
    AlbumArtWindow ui;
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QScreen *screen = hiDpiTestScreen(2.0);
    if (screen == nullptr) {
        QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
    }
    QVERIFY(moveWindowToScreen(*ui.window, screen));
    if (std::abs(ui.window->effectiveDevicePixelRatio() - 2.0) > 1e-9) {
        QSKIP("The window is at another scale");
    }
    checkPreloadedCover(ui, 0, 2.0);
}

namespace {

// A 1000 × 600 window of the album art viewer's view alone, showing the
// image at `coverPath` with its details.
QObject *createDetailsSurfaceWindow(QQmlEngine &engine, const QString &coverPath, QString *errorText) {
    return createSurfaceWindow(engine, {QSize(1000, 600), QUrl::fromLocalFile(coverPath).toString(), QSize(), true},
                               errorText);
}

// createDetailsSurfaceWindow's window, with a 300 px square cover, `size`
// as it shows if one is given.
struct DetailsSurface {
    explicit DetailsSurface(QSize size = {}) {
        QString errorText;
        root.reset(createDetailsSurfaceWindow(engine, savedCover(dir, 300, 300), &errorText));
        window = qobject_cast<QQuickWindow *>(root.get());
        if (window == nullptr) {
            qWarning() << errorText;
            return;
        }
        if (size.isValid()) {
            window->resize(size);
        }
        surface = qvariant_cast<QQuickItem *>(root->property("surface"));
        palette = qvariant_cast<QObject *>(root->property("tokens"));
    }
    // Whether it loaded and shows.
    bool shown() const {
        return window != nullptr && surface != nullptr && palette != nullptr && QTest::qWaitForWindowExposed(window);
    }
    QQuickItem *item(const char *name) const { return itemNamed(surface, QString::fromLatin1(name)); }

    QTemporaryDir dir;
    QQmlApplicationEngine engine;
    QScopedPointer<QObject> root;
    QQuickWindow *window = nullptr;
    QQuickItem *surface = nullptr;
    QObject *palette = nullptr;
};

} // namespace

void AlbumArtSmokeTest::albumArtDetailsLieOverTheArt() {
    const DetailsSurface ui;
    QVERIFY(ui.shown());
    QQuickWindow *window = ui.window;
    QQuickItem *surface = ui.surface;
    QObject *palette = ui.palette;
    const QString folder = QStringLiteral("/music/The Quiet Meridian/Small Hours (Remastered "
                                          "Deluxe Edition With Bonus Tracks)/Disc One");
    const QVariantMap details{
        {QStringLiteral("path"), folder + QStringLiteral("/cover.jpg")},
        {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
        {QStringLiteral("width"), 1400},
        {QStringLiteral("height"), 1400},
        {QStringLiteral("fileSizeText"), QStringLiteral("412 KiB")},
        {QStringLiteral("format"), QStringLiteral("JPEG")},
        {QStringLiteral("embeddedCover"), false},
    };
    surface->setProperty("fileInfo", details);

    // The art on black, beside a 280 px column on the plot's strip colour
    // behind a 1 px seam over the view's right edge.
    QQuickItem *floor = itemNamed(surface, QStringLiteral("albumArtFloor"));
    QQuickItem *viewport = itemNamed(surface, QStringLiteral("albumArtViewport"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *seam = itemNamed(surface, QStringLiteral("albumArtInfoSeam"));
    QQuickItem *backing = itemNamed(surface, QStringLiteral("albumArtInfoBacking"));
    QObject *plot = qvariant_cast<QObject *>(palette->property("mediaPlot"));
    QVERIFY(floor != nullptr && viewport != nullptr && column != nullptr && seam != nullptr
            && backing != nullptr && plot != nullptr);
    QCOMPARE(colorOf(floor, "color"), qRgb(0, 0, 0));
    // The view is the whole window; the column at its right edge, as tall.
    const QRectF viewRect(QPointF(0, 0), window->size());
    QCOMPARE(sceneRectOf(viewport), viewRect);
    const auto columnAtTheEdge = [&](bool fullscreen) {
        const qreal width = infoColumnWidth(fullscreen, viewRect.width());
        return QRectF(viewRect.right() - width, viewRect.top(), width, viewRect.height());
    };
    QTRY_COMPARE(sceneRectOf(column), columnAtTheEdge(false));
    QCOMPARE(sceneRectOf(seam), QRectF(sceneRectOf(column).left() - 1, viewRect.top(), 1, viewRect.height()));
    QCOMPARE(qAlpha(colorOf(seam, "color")), 255);
    QVERIFY(colorOf(seam, "color") != colorOf(plot, "plotStrip") && colorOf(seam, "color") != qRgb(0, 0, 0));
    QCOMPARE(sceneRectOf(backing), sceneRectOf(column));
    QCOMPARE(colorOf(backing, "color"), colorOf(plot, "plotStrip"));
    QCOMPARE(qAlpha(colorOf(backing, "color")), 255);
    // The square art fits the view's height, centred in what the details
    // leave of the view, left of their seam (it was centred in the whole
    // view), however the decode request is shaped.
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QVERIFY(image != nullptr);
    const auto paintedArt = [image]() { return paintedRectOf(image); };
    QTRY_COMPARE(paintedArt().size(), QSizeF(viewRect.height(), viewRect.height()));
    QCOMPARE(paintedArt().center(), QPointF(sceneRectOf(seam).left() / 2, viewRect.center().y()));
    const QRectF art = paintedArt();
    const QImage shown = window->grabWindow().convertToFormat(QImage::Format_RGB32);
    const auto pixelAt = [&shown](qreal x, qreal y) { return shown.pixel(int(x), int(y)); };
    QCOMPARE(pixelAt(art.right() - 20, art.bottom() - 10), QColor(Qt::darkCyan).rgb());
    QCOMPARE(pixelAt(sceneRectOf(column).center().x(), art.bottom() - 10),
             QColor::fromRgba(colorOf(plot, "plotStrip")).rgb());
    // Black around the art, on both sides of it.
    QCOMPARE(pixelAt(art.left() / 2, art.center().y()), qRgb(0, 0, 0));
    QCOMPARE(pixelAt((art.right() + sceneRectOf(seam).left()) / 2, art.center().y()), qRgb(0, 0, 0));

    // The file's name in semibold, then the rows: label, then value.
    QQuickItem *title = itemNamed(column, QStringLiteral("albumArtInfoTitle"));
    QVERIFY(title != nullptr);
    QCOMPARE(title->property("text").toString(), QStringLiteral("cover.jpg"));
    QCOMPARE(qvariant_cast<QFont>(title->property("font")).weight(), QFont::DemiBold);
    QStringList values;
    for (QQuickItem *value : visibleItemsNamed(column, QStringLiteral("albumArtInfoValue"))) {
        values << value->property("text").toString();
    }
    const QString size = QLocale().toString(1400.0, 'f', 0);
    QCOMPARE(values, (QStringList{QStringLiteral("%1 × %1 px").arg(size), QStringLiteral("412 KiB"),
                                  QStringLiteral("JPEG"), QStringLiteral("Folder image")}));
    // The values at the body size, as the file's name, and the labels at
    // the small size, on the app's type scale (albumArtDetailsAreOnTheTypeScale).
    const qreal bodySize = qvariant_cast<QFont>(title->property("font")).pointSizeF();
    for (const QString &name : {QStringLiteral("albumArtInfoValue"), QStringLiteral("albumArtFolderPath")}) {
        for (QQuickItem *text : visibleItemsNamed(column, name)) {
            QCOMPARE(qvariant_cast<QFont>(text->property("font")).pointSizeF(), bodySize);
        }
    }
    for (const QString &name : {QStringLiteral("albumArtInfoLabel"), QStringLiteral("albumArtFolderLabel")}) {
        for (QQuickItem *text : visibleItemsNamed(column, name)) {
            QCOMPARE(qvariant_cast<QFont>(text->property("font")).pointSizeF(),
                     palette->property("smallPointSize").toReal());
        }
    }
    QCOMPARE(visibleTextItems(column, QStringLiteral("Dimensions")).size(), 1);
    QCOMPARE(visibleTextItems(column, QStringLiteral("Source")).size(), 1);

    // The folder: its label on a line of its own, and under it the path on
    // one line across the column's whole width, elided in the middle, with
    // Copy Path at the label's line's end (in the grid the path had the
    // values' part of the row and broke onto a second line). The whole path
    // is in its tooltip.
    QQuickItem *folderLabel = itemNamed(column, QStringLiteral("albumArtFolderLabel"));
    QQuickItem *path = itemNamed(column, QStringLiteral("albumArtFolderPath"));
    QQuickItem *copy = itemNamed(column, QStringLiteral("albumArtCopyPathButton"));
    QQuickItem *folderRow = itemNamed(column, QStringLiteral("albumArtFolderRow"));
    QVERIFY(folderLabel != nullptr && path != nullptr && copy != nullptr && folderRow != nullptr);
    // Each item's own box: a text's bounding rect is only its painted text.
    const auto boxOf = [](QQuickItem *item) {
        return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    };
    QVERIFY(folderLabel->isVisible() && path->isVisible() && copy->isVisible());
    QCOMPARE(folderLabel->property("text").toString(), QStringLiteral("Folder"));
    const QList<QQuickItem *> rows = visibleItemsNamed(column, QStringLiteral("albumArtInfoRow"));
    QCOMPARE(rows.size(), 4);
    QQuickItem *sourceLabel = itemNamed(rows.last(), QStringLiteral("albumArtInfoLabel"));
    QQuickItem *sourceValue = itemNamed(rows.last(), QStringLiteral("albumArtInfoValue"));
    QVERIFY(sourceLabel != nullptr && sourceValue != nullptr);
    // The row spans the details' width, as the others.
    QTRY_COMPARE(boxOf(folderRow).left(), boxOf(rows.last()).left());
    QCOMPARE(boxOf(folderRow).right(), boxOf(rows.last()).right());
    // The label starts where the other labels do, on a line of its own,
    // over the path.
    QCOMPARE(boxOf(folderLabel).left(), boxOf(sourceLabel).left());
    QVERIFY2(boxOf(folderLabel).bottom() <= boxOf(path).top() + 0.01, "the folder's label is not on a line of its own");
    // Copy Path, beside it, is clear of its text.
    QVERIFY(boxOf(folderLabel).left() + folderLabel->property("contentWidth").toReal() < boxOf(copy).left());
    QCOMPARE(folderLabel->property("lineCount").toInt(), 1);
    // The labels' column: 80 px at the default font (it was 76 px, the
    // width of "Dimensions").
    QCOMPARE(surface->property("infoLabelColumnMinimum").toReal(), 80.0);
    QVERIFY2(sourceLabel->width() >= 80.0, qPrintable(QString::number(sourceLabel->width())));
    // The row follows the others at their rhythm.
    const qreal rhythm = surface->property("infoRowHeight").toReal();
    QCOMPARE(boxOf(folderRow).top() - boxOf(rows.last()).top(), rhythm);
    // The path from the column's start, not the values', to the row's end,
    // under Copy Path; a long one wraps after its slashes, on
    // the lines it needs up to eight, three during a preview, and what does
    // not fit is elided (on one line, elided in its middle, a typical one lost
    // its folders; on three, a large font cut it short).
    QCOMPARE(boxOf(path).left(), boxOf(folderRow).left());
    QVERIFY(boxOf(path).left() < boxOf(sourceValue).left());
    QCOMPARE(boxOf(copy).right(), boxOf(folderRow).right());
    QCOMPARE(boxOf(path).right(), boxOf(folderRow).right());
    QCOMPARE(path->property("path").toString(), folder);
    // Its lines break after a slash, where a zero-width space follows each;
    // in a name that fits on a line, the spaces are no-break spaces and a
    // word joiner follows a hyphen; a name wider than the line breaks
    // between its words, rather than anywhere.
    const auto breaksAtSlashesOnly = [path](const QString &text) {
        const QFontMetricsF metrics(qvariant_cast<QFont>(path->property("font")));
        QStringList names = text.split(QLatin1Char('/'));
        for (QString &name : names) {
            if (metrics.horizontalAdvance(name + QLatin1Char('/')) <= path->width()) {
                name.replace(QLatin1Char(' '), QChar(0x00a0)).replace(QLatin1Char('-'), QStringLiteral("-\u2060"));
            }
        }
        return names.join(QStringLiteral("/\u200b"));
    };
    QCOMPARE(path->property("text").toString(), breaksAtSlashesOnly(folder));
    QVERIFY(path->property("text").toString().contains(
        QStringLiteral("/\u200bThe\u00a0Quiet\u00a0Meridian/\u200bSmall Hours (Remastered Deluxe")));
    QCOMPARE(path->property("wrapMode").toInt(), int(QTextOption::WrapAtWordBoundaryOrAnywhere));
    QCOMPARE(path->property("elide").toInt(), int(Qt::ElideRight));
    QCOMPARE(path->property("maximumLineCount").toInt(), 8);
    QTRY_COMPARE(path->property("lineCount").toInt(), 3);
    QVERIFY(!path->property("elided").toBool());
    // Copy Path's glyph is centred on the folder's label's line, the button
    // clear of the label's text and of the path;
    // over the whole screen, the button and its glyph are a step larger, as
    // the details' text is: 24 and 16 px in a window, 29 and 19 px there.
    QQuickItem *copyGlyph = itemNamed(copy, QStringLiteral("viewerToolButtonIcon"));
    QVERIFY(copyGlyph != nullptr);
    const auto checkCopyOnTheLabelsLine = [&](qreal buttonSize, qreal glyphSize, const QString &where) {
        QTRY_VERIFY2(boxOf(copy).width() == buttonSize && boxOf(copy).height() == buttonSize,
                     qPrintable(QStringLiteral("%1: %2 x %3").arg(where).arg(boxOf(copy).width()).arg(boxOf(copy).height())));
        QCOMPARE(boxOf(copyGlyph).size(), QSizeF(glyphSize, glyphSize));
        QTRY_COMPARE(path->property("lineCount").toInt(), 3);
        const qreal labelsLine = boxOf(folderLabel).center().y();
        QTRY_VERIFY2(qAbs(boxOf(copyGlyph).center().y() - labelsLine) <= 0.51,
                     qPrintable(QStringLiteral("%1: the glyph's centre at %2, the label's at %3")
                                    .arg(where).arg(boxOf(copyGlyph).center().y()).arg(labelsLine)));
        QVERIFY2(boxOf(folderLabel).left() + folderLabel->property("contentWidth").toReal() < boxOf(copy).left(),
                 qPrintable(where + QStringLiteral(": over the folder's label")));
        QTRY_COMPARE(boxOf(copy).right(), boxOf(folderRow).right());
        QTRY_VERIFY2(boxOf(copy).bottom() <= boxOf(path).top() + 0.01, qPrintable(where + QStringLiteral(": over the path")));
        QTRY_COMPARE(boxOf(path).right(), boxOf(folderRow).right());
    };
    checkCopyOnTheLabelsLine(24, 16, QStringLiteral("in a window"));
    surface->setProperty("fullscreen", true);
    checkCopyOnTheLabelsLine(std::round(24 * 1.2), std::round(16 * 1.2), QStringLiteral("over the whole screen"));
    surface->setProperty("fullscreen", false);
    QTRY_COMPARE(boxOf(copy).width(), 24.0);
    // Once the details are laid out again at their width in a window.
    const qreal padding = surface->property("infoPadding").toReal();
    QTRY_COMPARE(boxOf(folderRow).width(), column->width() - 2 * padding);
    const QFontMetricsF pathMetrics(qvariant_cast<QFont>(path->property("font")));
    QVERIFY(pathMetrics.horizontalAdvance(folder) > 2 * path->width());
    QVERIFY(path->height() >= 3 * pathMetrics.lineSpacing() - 1);
    // Its accessible name is the path itself.
    QCOMPARE(QAccessible::queryAccessibleInterface(path)->text(QAccessible::Name), folder);
    // Where the tooltip shows, not after how long: at once.
    QVERIFY(evaluateOn(path, QStringLiteral("ToolTip.delay = 0")).isValid());
    QTest::mouseMove(window, centerInWindow(path));
    QTRY_COMPARE(visibleTextItems(window->contentItem(), folder).size(), 1);

    // A path that fits is whole, on its one line.
    surface->setProperty("fileInfo", QVariantMap{{QStringLiteral("path"), QStringLiteral("/music/cover.jpg")},
                                                 {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
                                                 {QStringLiteral("width"), 1400},
                                                 {QStringLiteral("height"), 1400}});
    QTRY_COMPARE(path->property("path").toString(), QStringLiteral("/music"));
    QTRY_VERIFY(!path->property("elided").toBool());
    QCOMPARE(path->property("lineCount").toInt(), 1);
    // A typical path too long for a line is whole on two, broken after a
    // slash rather than in a folder's name.
    const QString typical = QStringLiteral("/music/The Quiet Meridian/2005 - Small Hours");
    surface->setProperty("fileInfo", QVariantMap{{QStringLiteral("path"), typical + QStringLiteral("/cover.jpg")},
                                                 {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
                                                 {QStringLiteral("width"), 1400},
                                                 {QStringLiteral("height"), 1400}});
    QTRY_COMPARE(path->property("path").toString(), typical);
    QVERIFY(pathMetrics.horizontalAdvance(typical) > path->width());
    QTRY_COMPARE(path->property("lineCount").toInt(), 2);
    QVERIFY(!path->property("elided").toBool());
    QVERIFY(pathMetrics.horizontalAdvance(QStringLiteral("/music/The Quiet Meridian/")) <= path->width());
    QVERIFY(pathMetrics.horizontalAdvance(QStringLiteral("2005 - Small Hours")) <= path->width());

    // Over the whole screen, a path of three lines breaks between its
    // folders, not inside one's name ("…/2005 - Small" broke before
    // "Hours"). The lines are laid out as Qt's text lays out the label's,
    // from a text and the label's font and width.
    const auto linesOf = [path](const QString &text) {
        QTextLayout layout(text, qvariant_cast<QFont>(path->property("font")));
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(option);
        QStringList lines;
        layout.beginLayout();
        for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
            line.setLineWidth(path->width());
            lines << text.mid(line.textStart(), line.textLength())
                         .remove(QChar(0x200b))
                         .remove(QChar(0x2060))
                         .replace(QChar(0x00a0), QLatin1Char(' '));
        }
        layout.endLayout();
        return lines;
    };
    surface->setProperty("fullscreen", true);
    const QString fixtureFolder =
        QStringLiteral("/mnt/ferrous/council/inventory/fixtures/library/The Quiet Meridian/2005 - Small Hours Deluxe");
    surface->setProperty("fileInfo", QVariantMap{{QStringLiteral("path"), fixtureFolder + QStringLiteral("/cover.jpg")},
                                                 {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
                                                 {QStringLiteral("width"), 1400},
                                                 {QStringLiteral("height"), 1400}});
    QTRY_COMPARE(path->property("path").toString(), fixtureFolder);
    // The column at its width over the whole screen, at the view's edge,
    // and the details laid out in it.
    QTRY_COMPARE(boxOf(copy).width(), std::round(24 * 1.2));
    QTRY_COMPARE(sceneRectOf(column), columnAtTheEdge(true));
    QTRY_VERIFY(boxOf(folderRow).right() < sceneRectOf(column).right() && boxOf(folderRow).width() > 280);
    QTRY_COMPARE(path->property("text").toString(), breaksAtSlashesOnly(fixtureFolder));
    const QStringList lines = linesOf(path->property("text").toString());
    QTRY_COMPARE(path->property("lineCount").toInt(), lines.size());
    QVERIFY(!path->property("elided").toBool());
    QVERIFY2(lines.size() == 3 && lines.last().endsWith(QStringLiteral("2005 - Small Hours Deluxe")),
             qPrintable(lines.join(QLatin1Char('|'))));
    for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
        QVERIFY2(lines.at(i).endsWith(QLatin1Char('/')), qPrintable(lines.join(QLatin1Char('|'))));
    }
    // With its spaces and hyphens as they are, a line would break inside a
    // name.
    const QStringList withSpaces = linesOf(QString(fixtureFolder).replace(QLatin1Char('/'), QStringLiteral("/\u200b")));
    QVERIFY2(std::any_of(withSpaces.cbegin(), withSpaces.cend() - 1,
                         [](const QString &line) { return !line.endsWith(QLatin1Char('/')); }),
             qPrintable(withSpaces.join(QLatin1Char('|'))));
    // The copy, the tooltip and the accessible name keep the path as it is.
    QCOMPARE(QAccessible::queryAccessibleInterface(path)->text(QAccessible::Name), fixtureFolder);
    QTest::mouseMove(window, centerInWindow(path));
    QTRY_COMPARE(visibleTextItems(window->contentItem(), fixtureFolder).size(), 1);
    QGuiApplication::clipboard()->clear();
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(copy));
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), fixtureFolder);
    surface->setProperty("fullscreen", false);
    QTRY_COMPARE(boxOf(copy).width(), 24.0);
    // The details laid out again in the column, at its width in a window.
    QTRY_COMPARE(sceneRectOf(column), columnAtTheEdge(false));
    QTRY_VERIFY(sceneRectOf(column).contains(boxOf(copy)));
    surface->setProperty("fileInfo", details);
    QTRY_COMPARE(path->property("path").toString(), folder);

    // Copy Path puts the folder on the clipboard. The path's tooltip, shown
    // above it at once here, reaches over the button: off the path, it goes.
    QTest::mouseMove(window, QPoint(8, window->height() - 8));
    QTRY_VERIFY(visibleTextItems(window->contentItem(), folder).isEmpty());
    QTRY_COMPARE(boxOf(folderRow).width(), column->width() - 2 * padding);
    QGuiApplication::clipboard()->clear();
    QCOMPARE(copy->property("text").toString(), QStringLiteral("Copy Path"));
    QCOMPARE(sceneRectOf(copy).size(), QSizeF(24, 24));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centerInWindow(copy));
    QTRY_COMPARE(QGuiApplication::clipboard()->text(), folder);

    // Art the track embeds has no folder of its own.
    surface->setProperty("sourceKind", QStringLiteral("embedded"));
    QTRY_VERIFY(!path->isVisible());
    QVERIFY(!folderLabel->isVisible());
    QCOMPARE(title->property("text").toString(), QStringLiteral("Embedded Image"));
    QCOMPARE(visibleTextItems(column, QStringLiteral("Embedded in the track")).size(), 1);

    // Without the details, the art is centred in the whole view, where they
    // were too.
    surface->setProperty("infoVisible", false);
    QTRY_VERIFY(!column->isVisible());
    QVERIFY(!seam->isVisible());
    QCOMPARE(sceneRectOf(viewport), viewRect);
    QCOMPARE(paintedArt(), QRectF(QPointF(viewRect.center().x() - art.width() / 2, 0), art.size()));
    QTRY_COMPARE(window->grabWindow().convertToFormat(QImage::Format_RGB32).pixel(
                     int(columnAtTheEdge(false).left() + 40), int(art.bottom() - 10)),
                 QColor(Qt::darkCyan).rgb());
}

void AlbumArtSmokeTest::albumArtDetailsLabelsStayWholeWithLargerFonts() {
    // The rows' labels share a column as wide as the widest of them in the
    // current font, so none is cut short: at 14 pt, "Dimensions" is wider
    // than the 76 px the column once had. Past two fifths of the row, a
    // label wraps between its words, never inside one: at 24 pt, the
    // details' column was a fixed 279 px, and "Dimensions" broke into
    // "Dime" and "nsions". The column is about 40 characters wide, up to
    // two fifths of the view, so it grows with the font. Every row's value
    // starts at the same x, and all stay within the column's margins.
    for (const qreal pointSize : {0.0, 14.0, 24.0}) {
        const QByteArray at = QByteArray("point size ") + QByteArray::number(pointSize);
        const ScopedApplicationFont font(pointSize);
        const DetailsSurface ui;
        QVERIFY2(ui.shown(), at.constData());
        QQuickWindow *window = ui.window;
        QQuickItem *surface = ui.surface;
        QObject *palette = ui.palette;
        surface->setProperty("fileInfo", QVariantMap{
            {QStringLiteral("path"), QStringLiteral("/music/Small Hours/cover.jpg")},
            {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
            {QStringLiteral("width"), 1400},
            {QStringLiteral("height"), 1400},
            {QStringLiteral("fileSizeText"), QStringLiteral("412 KiB")},
            {QStringLiteral("format"), QStringLiteral("JPEG")},
            {QStringLiteral("embeddedCover"), false},
        });
        QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
        QVERIFY2(column != nullptr, at.constData());
        // About 40 characters, 279 px and its seam at the default font, up
        // to two fifths of the view.
        const qreal fortyCharacters = std::ceil(
            QFontMetricsF(qvariant_cast<QFont>(palette->property("baseFont"))).averageCharWidth() * 40) - 1;
        const qreal columnWidth = qMax(279.0, qMin(std::floor(window->width() * 0.4), fortyCharacters));
        QTRY_COMPARE(column->width(), columnWidth);
        if (pointSize == 0.0) {
            QCOMPARE(column->width(), 279.0);
        } else if (pointSize >= 24.0) {
            QVERIFY2(column->width() > 279.0, at.constData());
        }
        QTRY_COMPARE(visibleItemsNamed(column, QStringLiteral("albumArtInfoRow")).size(), 4);
        const QList<QQuickItem *> rows = visibleItemsNamed(column, QStringLiteral("albumArtInfoRow"));
        // The column's padding: 16 px with the usual font, more with a
        // larger one.
        const qreal padding = surface->property("infoPadding").toReal();
        QCOMPARE(padding, pointSize > 10 ? std::round(16 * pointSize / 10) : 16.0);
        // Once laid out, each row is the column's width inside its margins.
        QTRY_VERIFY2(std::all_of(rows.cbegin(), rows.cend(), [column, padding](QQuickItem *row) {
                         return qFuzzyCompare(row->width(), column->width() - 2 * padding);
                     }),
                     at.constData());
        QList<QQuickItem *> labels;
        QList<QQuickItem *> values;
        qreal widest = 0;
        qreal widestWord = 0;
        const auto wordsOf = [](QQuickItem *label) {
            return label->property("text").toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        };
        const auto widestWordOf = [&wordsOf](QQuickItem *label) {
            const QFontMetricsF metrics(qvariant_cast<QFont>(label->property("font")));
            qreal wordWidth = 0;
            for (const QString &word : wordsOf(label)) {
                wordWidth = qMax(wordWidth, metrics.horizontalAdvance(word));
            }
            return wordWidth;
        };
        for (QQuickItem *row : rows) {
            QQuickItem *label = itemNamed(row, QStringLiteral("albumArtInfoLabel"));
            QQuickItem *value = itemNamed(row, QStringLiteral("albumArtInfoValue"));
            QVERIFY2(label != nullptr && value != nullptr, at.constData());
            // The values at the body size, the application's font's, and
            // the labels at the small size.
            QCOMPARE(qvariant_cast<QFont>(label->property("font")).pointSizeF(),
                     palette->property("smallPointSize").toReal());
            QCOMPARE(qvariant_cast<QFont>(value->property("font")).pointSizeF(),
                     palette->property("basePointSize").toReal());
            if (pointSize > 0) {
                QCOMPARE(palette->property("basePointSize").toReal(), pointSize);
            }
            labels << label;
            values << value;
            widest = qMax(widest, label->implicitWidth());
            widestWord = qMax(widestWord, widestWordOf(label));
        }
        // The column of the labels: the widest label, up to two fifths of
        // the row inside the column's margins, and never narrower than the
        // widest word.
        // 80 px or more at the default font, more with a larger.
        const qreal least = surface->property("infoLabelColumnMinimum").toReal();
        QCOMPARE(least, pointSize > 10 ? std::round(80 * pointSize / 10) : 80.0);
        const qreal cap = std::floor((column->width() - 2 * padding) * 0.4);
        const qreal labelWidth = qMax(std::ceil(widestWord), qMin(qMax(std::ceil(widest), least), cap));
        // Each item's own box: a text's bounding rect is only its painted text.
        const auto boxOf = [](QQuickItem *item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        const QRectF inside = boxOf(column).adjusted(padding - 0.5, 0, -(padding - 0.5), 0);
        const auto describe = [&](QQuickItem *label, QQuickItem *value) {
            return QStringLiteral("%1, %2: label %3 (implicit %4, %5 lines), value %6, column %7..%8")
                .arg(QString::fromLatin1(at), label->property("text").toString())
                .arg(QStringLiteral("%1..%2").arg(boxOf(label).left()).arg(boxOf(label).right()))
                .arg(label->implicitWidth())
                .arg(label->property("lineCount").toInt())
                .arg(QStringLiteral("%1..%2").arg(boxOf(value).left()).arg(boxOf(value).right()))
                .arg(inside.left())
                .arg(inside.right());
        };
        for (int i = 0; i < labels.size(); ++i) {
            QQuickItem *label = labels.at(i);
            QQuickItem *value = values.at(i);
            const QString where = describe(label, value);
            // Whole: never elided, and either as wide as its text or wrapped.
            QCOMPARE(label->property("elide").toInt(), int(Qt::ElideNone));
            QVERIFY2(!label->property("truncated").toBool(), qPrintable(where));
            QVERIFY2(qFuzzyCompare(label->width(), labelWidth), qPrintable(where));
            const bool fits = label->implicitWidth() <= label->width() + 0.01;
            QVERIFY2(fits || label->property("lineCount").toInt() > 1, qPrintable(where));
            QVERIFY2(fits == (label->property("lineCount").toInt() == 1), qPrintable(where));
            // Wrapped only between words: each word fits on a line, and
            // there are no more lines than words.
            QCOMPARE(label->property("wrapMode").toInt(), int(QTextOption::WordWrap));
            QVERIFY2(label->width() + 0.01 >= widestWordOf(label), qPrintable(where));
            QVERIFY2(label->property("lineCount").toInt() <= wordsOf(label).size(), qPrintable(where));
            // The values all start at the same x, after the labels' column.
            QCOMPARE(boxOf(label).left(), boxOf(labels.first()).left());
            QCOMPARE(boxOf(value).left(), boxOf(values.first()).left());
            QVERIFY2(boxOf(label).right() + 12 <= boxOf(value).left() + 0.01, qPrintable(where));
            // A label stands on its value's first line, however many lines
            // either takes: the two share a baseline.
            const qreal labelBaseline = boxOf(label).top() + label->baselineOffset();
            const qreal valueBaseline = boxOf(value).top() + value->baselineOffset();
            QVERIFY2(qAbs(labelBaseline - valueBaseline) < 0.5,
                     qPrintable(QStringLiteral("%1; baselines %2 and %3, value %4 lines")
                                    .arg(where).arg(labelBaseline).arg(valueBaseline)
                                    .arg(value->property("lineCount").toInt())));
            // Within the column's margins, and with room for the value: the
            // labels pass their two fifths only for a word that would not
            // fit, and the values keep at least two fifths.
            QVERIFY2(inside.contains(boxOf(label)) && inside.contains(boxOf(value)),
                     qPrintable(where));
            QVERIFY2(value->width() >= cap, qPrintable(where));
        }
        if (pointSize == 0.0) {
            // The usual font fits every label on its line.
            QVERIFY2(labelWidth < cap, at.constData());
        }
        if (pointSize >= 24.0) {
            // Every one-word label on one line, "Dimensions" whole.
            for (QQuickItem *label : std::as_const(labels)) {
                if (wordsOf(label).size() == 1) {
                    QVERIFY2(label->property("lineCount").toInt() == 1,
                             qPrintable(describe(label, values.at(labels.indexOf(label)))));
                }
            }
        }
    }
}

void AlbumArtSmokeTest::albumArtKeepsAGutterBesideTheDetails_data() {
    QTest::addColumn<QSize>("size");
    QTest::newRow("1000x720") << QSize(1000, 720);
    QTest::newRow("1600x1000") << QSize(1600, 1000);
}

// While the details show, the art keeps 16 px clear of their seam and of
// the view's left edge, centred in what is left (in a window as wide as it
// was tall, the art met the seam); the gutter eases with the details'
// reveal. Without them, the art fits the whole view.
void AlbumArtSmokeTest::albumArtKeepsAGutterBesideTheDetails() {
    QFETCH(QSize, size);
    const DetailsSurface ui(size);
    QVERIFY(ui.shown());
    QQuickWindow *window = ui.window;
    QQuickItem *surface = ui.surface;
    QTRY_COMPARE(window->size(), size);
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QQuickItem *seam = itemNamed(surface, QStringLiteral("albumArtInfoSeam"));
    QVERIFY(image != nullptr && seam != nullptr);
    const auto paintedArt = [image]() {
        const QSizeF painted(image->property("paintedWidth").toReal(), image->property("paintedHeight").toReal());
        const QPointF topLeft = image->mapToScene(
            QPointF((image->width() - painted.width()) / 2, (image->height() - painted.height()) / 2));
        return QRectF(topLeft, painted);
    };
    // With the details: the art fits the view left of the seam, less 16 px
    // on each side, and centred there.
    QTRY_VERIFY(seam->isVisible() && sceneRectOf(seam).left() > 0);
    const qreal seamLeft = sceneRectOf(seam).left();
    const qreal room = std::min(seamLeft - 32, qreal(size.height()));
    const auto describe = [&]() {
        const QRectF art = paintedArt();
        return QStringLiteral("art %1..%2, seam at %3").arg(art.left()).arg(art.right()).arg(seamLeft);
    };
    QTRY_VERIFY2(qAbs(paintedArt().width() - room) <= 1.0, qPrintable(describe()));
    QVERIFY2(paintedArt().left() >= 16 - 0.5, qPrintable(describe()));
    QVERIFY2(paintedArt().right() <= seamLeft - 16 + 0.5, qPrintable(describe()));
    QVERIFY2(qAbs(paintedArt().center().x() - seamLeft / 2) <= 1.0, qPrintable(describe()));
    QCOMPARE(surface->property("artMargin").toReal(), 16.0);
    // During a preview, 24 px.
    surface->setProperty("previewActive", true);
    QTRY_COMPARE(surface->property("artMargin").toReal(), 24.0);
    QTRY_VERIFY2(paintedArt().right() <= seamLeft - 24 + 0.5 && paintedArt().left() >= 24 - 0.5, qPrintable(describe()));
    surface->setProperty("previewActive", false);

    // Hiding the details eases the gutter with them, to none: the art fits
    // the whole view.
    QVERIFY(surface->property("infoRevealDuration").toInt() > 0);
    // Each frame of the reveal, with the gutter at that frame.
    QList<std::pair<qreal, qreal>> steps;
    const QMetaObject::Connection stepped = connect(window, &QQuickWindow::afterAnimating, surface, [&]() {
        steps << std::pair{surface->property("infoReveal").toReal(), surface->property("artMargin").toReal()};
    });
    surface->setProperty("infoVisible", false);
    QTRY_COMPARE(surface->property("infoReveal").toReal(), 0.0);
    disconnect(stepped);
    QVERIFY(std::any_of(steps.cbegin(), steps.cend(), [](const auto &step) { return step.first > 0 && step.first < 1; }));
    for (const auto &[reveal, margin] : steps) {
        QCOMPARE(margin, 16 * reveal);
    }
    QCOMPARE(surface->property("artMargin").toReal(), 0.0);
    const qreal whole = std::min(qreal(size.width()), qreal(size.height()));
    QTRY_VERIFY2(qAbs(paintedArt().width() - whole) <= 1.0, qPrintable(describe()));
    QVERIFY2(qAbs(paintedArt().center().x() - size.width() / 2.0) <= 1.0, qPrintable(describe()));
}

// At 24 pt in a window, a typical folder path is whole on the lines it
// needs, broken after its slashes, rather than cut short on three over an
// empty column ("…/2005 - Smal…").
void AlbumArtSmokeTest::albumArtFolderPathTakesTheLinesItNeedsAtLargeFonts() {
    const ScopedApplicationFont font(24);
    const DetailsSurface ui;
    QVERIFY(ui.shown());
    QQuickItem *surface = ui.surface;
    const QString folder =
        QStringLiteral("/mnt/ferrous/council/inventory/fixtures/library/The Quiet Meridian/2005 - Small Hours");
    surface->setProperty("fileInfo", QVariantMap{{QStringLiteral("path"), folder + QStringLiteral("/cover.jpg")},
                                                 {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
                                                 {QStringLiteral("width"), 1400},
                                                 {QStringLiteral("height"), 1400}});
    QQuickItem *path = itemNamed(surface, QStringLiteral("albumArtFolderPath"));
    QVERIFY(path != nullptr);
    QTRY_COMPARE(path->property("path").toString(), folder);
    // Laid out in the details' column, inside its padding, beside Copy Path.
    QQuickItem *folderRow = itemNamed(surface, QStringLiteral("albumArtFolderRow"));
    QVERIFY(folderRow != nullptr);
    QTRY_VERIFY(path->isVisible() && folderRow->width() > 0);
    QTRY_VERIFY2(path->width() > folderRow->width() / 2,
                 qPrintable(QStringLiteral("%1 of %2").arg(path->width()).arg(folderRow->width())));
    QCOMPARE(path->property("maximumLineCount").toInt(), 8);
    QTRY_VERIFY2(path->property("lineCount").toInt() > 3, qPrintable(path->property("lineCount").toString()));
    QVERIFY(!path->property("elided").toBool());
    QVERIFY(path->property("text").toString().endsWith(QStringLiteral("Hours")));
    // The details' column holds the path's lines whole.
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QVERIFY(column != nullptr);
    const QRectF pathBox = path->mapRectToScene(QRectF(0, 0, path->width(), path->height()));
    QVERIFY(path->height() >= path->property("contentHeight").toReal() - 0.5);
    QVERIFY2(pathBox.bottom() <= column->mapRectToScene(QRectF(0, 0, column->width(), column->height())).bottom(),
             qPrintable(QString::number(pathBox.bottom())));

    // During a preview, under the candidate's label and Apply, three lines,
    // the rest elided; after it, the lines it needs again.
    const int lines = path->property("lineCount").toInt();
    surface->setProperty("previewActive", true);
    QCOMPARE(path->property("maximumLineCount").toInt(), 3);
    QTRY_COMPARE(path->property("lineCount").toInt(), 3);
    QVERIFY(path->property("elided").toBool());
    surface->setProperty("previewActive", false);
    QTRY_COMPARE(path->property("lineCount").toInt(), lines);
    QVERIFY(!path->property("elided").toBool());
}

void AlbumArtSmokeTest::albumArtDetailsAreOnTheTypeScale_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<bool>("fullscreen");
    QTest::newRow("10 pt") << 10.0 << false;
    QTest::newRow("10 pt, whole screen") << 10.0 << true;
    QTest::newRow("24 pt") << 24.0 << false;
}

// The details are on the app's type scale (everything was at the body size,
// crammed): with the default 10 pt font, 13 px values and 12 px labels, the
// rows 26 px apart, a list row's height, and 16 px in from the column's
// edges; all from the font, so a larger font scales them, and a step larger
// over the whole screen. During a preview, Back to Results and Apply sit in
// a footer at the column's bottom, Apply the style's default button.
void AlbumArtSmokeTest::albumArtDetailsAreOnTheTypeScale() {
    QFETCH(qreal, pointSize);
    QFETCH(bool, fullscreen);
    const ScopedApplicationFont font(pointSize);
    const DetailsSurface ui;
    QVERIFY(ui.shown());
    QQuickWindow *window = ui.window;
    QQuickItem *surface = ui.surface;
    QObject *palette = ui.palette;
    // Wide enough at 24 pt for every value on one line.
    window->setWidth(pointSize > 10 ? 1800 : 1000);
    QCOMPARE(palette->property("basePointSize").toReal(), pointSize);
    surface->setProperty("fullscreen", fullscreen);
    surface->setProperty("fileInfo", QVariantMap{
        {QStringLiteral("path"), QStringLiteral("/music/Small Hours/cover.jpg")},
        {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
        {QStringLiteral("width"), 1400},
        {QStringLiteral("height"), 1400},
        {QStringLiteral("fileSizeText"), QStringLiteral("412 KiB")},
        {QStringLiteral("format"), QStringLiteral("JPEG")},
        {QStringLiteral("embeddedCover"), false},
    });
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QVERIFY(column != nullptr);
    QTRY_COMPARE(visibleItemsNamed(column, QStringLiteral("albumArtInfoRow")).size(), 4);
    const qreal scale = fullscreen ? 1.2 : 1.0;

    // Values at the body size, labels at the small size: 13 px and 12 px
    // at 10 pt.
    QList<QQuickItem *> values = visibleItemsNamed(column, QStringLiteral("albumArtInfoValue"))
        + visibleItemsNamed(column, QStringLiteral("albumArtFolderPath"));
    QList<QQuickItem *> labels = visibleItemsNamed(column, QStringLiteral("albumArtInfoLabel"))
        + visibleItemsNamed(column, QStringLiteral("albumArtFolderLabel"));
    QCOMPARE(values.size(), 5);
    QCOMPARE(labels.size(), 5);
    const qreal body = palette->property("basePointSize").toReal() * scale;
    const qreal small = palette->property("smallPointSize").toReal() * scale;
    QVERIFY(small < body);
    for (QQuickItem *value : std::as_const(values)) {
        QCOMPARE(qvariant_cast<QFont>(value->property("font")).pointSizeF(), body);
    }
    for (QQuickItem *label : std::as_const(labels)) {
        QCOMPARE(qvariant_cast<QFont>(label->property("font")).pointSizeF(), small);
    }
    if (pointSize == 10.0 && !fullscreen) {
        QCOMPARE(QFontInfo(qvariant_cast<QFont>(values.first()->property("font"))).pixelSize(), 13);
        QCOMPARE(QFontInfo(qvariant_cast<QFont>(labels.first()->property("font"))).pixelSize(), 12);
    }

    // The rows a list row apart: a line of the values' text and a small
    // spacing above and below it, 26 px at 10 pt.
    const qreal lineHeight = QFontMetricsF(qvariant_cast<QFont>(values.first()->property("font"))).height();
    const qreal rhythm = std::round(lineHeight) + 2 * std::round(4 * scale);
    QCOMPARE(surface->property("infoRowHeight").toReal(), rhythm);
    if (!fullscreen) {
        QCOMPARE(rhythm, palette->property("rowHeight").toReal());
    }
    if (pointSize == 10.0 && !fullscreen && std::round(lineHeight) == 18.0) {
        QCOMPARE(rhythm, 26.0);
    }
    const QList<QQuickItem *> rows = visibleItemsNamed(column, QStringLiteral("albumArtInfoRow"));
    // Once laid out.
    QTRY_COMPARE(sceneRectOf(rows.last()).top() - sceneRectOf(rows.first()).top(), 3 * rhythm);
    for (int i = 1; i < rows.size(); ++i) {
        QCOMPARE(sceneRectOf(rows.at(i)).top() - sceneRectOf(rows.at(i - 1)).top(), rhythm);
        // A line each: the line's height rounded up.
        QCOMPARE(rows.at(i - 1)->height(), std::ceil(lineHeight));
    }

    // 16 px in from the column's edges at 10 pt, scaled with the font.
    const qreal padding = pointSize > 10 ? std::round(16 * pointSize / 10) : 16.0;
    QCOMPARE(surface->property("infoPadding").toReal(), padding);
    QQuickItem *title = itemNamed(column, QStringLiteral("albumArtInfoTitle"));
    QVERIFY(title != nullptr);
    QCOMPARE(sceneRectOf(title).left() - sceneRectOf(column).left(), padding);
    for (QQuickItem *row : rows) {
        QCOMPARE(sceneRectOf(row).left() - sceneRectOf(column).left(), padding);
        QCOMPARE(sceneRectOf(column).right() - sceneRectOf(row).right(), padding);
    }

    // A preview: the footer at the column's bottom under a seam, its buttons
    // the padding in from its edges, Apply last and the default.
    surface->setProperty("previewActive", true);
    surface->setProperty("previewLabel", QStringLiteral("iTunes Artwork"));
    surface->setProperty("previewHasCandidate", true);
    surface->setProperty("previewCanApply", true);
    QQuickItem *footer = visibleItemNamed(window, QStringLiteral("albumArtPreviewFooter"));
    QQuickItem *seam = visibleItemNamed(window, QStringLiteral("albumArtPreviewFooterSeam"));
    QQuickItem *back = visibleItemNamed(window, QStringLiteral("albumArtPreviewBackButton"));
    QQuickItem *apply = visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton"));
    QVERIFY(footer != nullptr && seam != nullptr && back != nullptr && apply != nullptr);
    // Which image shows is the heading; its file's name, a label under it,
    // at the small size in the secondary colour.
    QQuickItem *previewLabel = visibleItemNamed(window, QStringLiteral("albumArtPreviewLabel"));
    QVERIFY(previewLabel != nullptr);
    const auto headingFont = qvariant_cast<QFont>(previewLabel->property("font"));
    QCOMPARE(headingFont.pointSizeF(), palette->property("basePointSize").toReal() * 1.2);
    QCOMPARE(headingFont.weight(), QFont::DemiBold);
    QCOMPARE(qvariant_cast<QFont>(title->property("font")).pointSizeF(), small);
    QCOMPARE(qvariant_cast<QFont>(title->property("font")).weight(), QFont::Normal);
    QObject *plot = qvariant_cast<QObject *>(palette->property("mediaPlot"));
    QVERIFY(plot != nullptr);
    QCOMPARE(colorOf(previewLabel, "color"), colorOf(plot, "plotText"));
    QCOMPARE(colorOf(title, "color"), colorOf(plot, "plotTextSecondary"));
    QCOMPARE(sceneRectOf(previewLabel).left(), sceneRectOf(title).left());
    const QRectF columnRect = sceneRectOf(column);
    QTRY_COMPARE(sceneRectOf(footer).bottom(), columnRect.bottom());
    QCOMPARE(sceneRectOf(seam), QRectF(columnRect.left(), sceneRectOf(footer).top(), columnRect.width(), 1));
    QCOMPARE(sceneRectOf(apply).bottom(), columnRect.bottom() - padding);
    QCOMPARE(columnRect.right() - sceneRectOf(apply).right(), padding);
    QVERIFY(sceneRectOf(back).right() <= sceneRectOf(apply).left() || sceneRectOf(back).bottom() <= sceneRectOf(apply).top());
    const auto isDefault = [](QQuickItem *button) {
        return QQmlExpression(qmlContext(button), button, QStringLiteral("Accessible.defaultButton")).evaluate().toBool();
    };
    QVERIFY(isDefault(apply));
    QVERIFY(!isDefault(back));
    // The style's own button, not one drawn by the details.
    QVERIFY(apply->inherits("QQuickButton"));
    QVERIFY(itemNamed(apply, QStringLiteral("albumArtDetailsButtonLabel")) == nullptr);
    QVERIFY(!apply->property("highlighted").toBool() && !apply->property("flat").toBool());
    // The details stop above the footer.
    QQuickItem *details = itemNamed(column, QStringLiteral("albumArtInfoBody"));
    QVERIFY(details != nullptr && details->clip());
    QCOMPARE(sceneRectOf(details).bottom(), sceneRectOf(footer).top());
}

void AlbumArtSmokeTest::albumArtPreviewFooterIsOnTheTypeScale_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::addColumn<bool>("fullscreen");
    QTest::newRow("10 pt") << 10.0 << false;
    QTest::newRow("10 pt, whole screen") << 10.0 << true;
    QTest::newRow("24 pt") << 24.0 << false;
    QTest::newRow("24 pt, whole screen") << 24.0 << true;
}

// The preview's footer is drawn on the details' type scale, 13 px in the
// window and 16 px over the whole screen at the default font, and follows
// a larger one: as drawn, for the style paints the caption itself, in the
// application's font unless its background is given the button's (the
// buttons declared the details' 16 px and showed 13).
void AlbumArtSmokeTest::albumArtPreviewFooterIsOnTheTypeScale() {
    QFETCH(qreal, pointSize);
    QFETCH(bool, fullscreen);
    const ScopedApplicationFont font(pointSize);
    const DetailsSurface ui(QSize(1800, 800));
    QVERIFY(ui.shown());
    QQuickWindow *window = ui.window;
    QQuickItem *surface = ui.surface;
    surface->setProperty("fullscreen", fullscreen);
    surface->setProperty("previewActive", true);
    surface->setProperty("previewHasCandidate", true);
    surface->setProperty("previewCanApply", true);
    QQuickItem *back = visibleItemNamed(window, QStringLiteral("albumArtPreviewBackButton"));
    QQuickItem *apply = visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton"));
    QVERIFY(back != nullptr && apply != nullptr);
    const qreal points = ui.palette->property("basePointSize").toReal() * (fullscreen ? 1.2 : 1.0);
    for (QQuickItem *button : {back, apply}) {
        const QFont declared = button->property("font").value<QFont>();
        QCOMPARE(declared.pointSizeF(), points);
        if (pointSize == 10.0) {
            QCOMPARE(QFontInfo(declared).pixelSize(), fullscreen ? 16 : 13);
        }
        // The caption alone: the glyph aside.
        QQmlExpression noGlyph(qmlContext(button), button, QStringLiteral("icon.name = \"\""));
        noGlyph.evaluate();
        QVERIFY2(!noGlyph.hasError(), qPrintable(noGlyph.error().toString()));
        const QString text = button->property("text").toString();
        const double wanted = QFontMetricsF(declared).tightBoundingRect(text).width();
        const auto captionWidth = [&]() {
            const QImage image = window->grabWindow().convertToFormat(QImage::Format_RGB32);
            const double dpr = static_cast<double>(image.width()) / window->width();
            const QRect inside = QRectF(sceneRectOf(button).topLeft() * dpr, sceneRectOf(button).size() * dpr)
                                     .toAlignedRect().adjusted(5, 5, -5, -5);
            if (!image.rect().contains(inside) || inside.isEmpty()) {
                return 0.0;
            }
            const QColor fill = image.pixelColor(inside.left() + 1, inside.center().y());
            int first = inside.right() + 1;
            int last = inside.left() - 1;
            for (int y = inside.top(); y <= inside.bottom(); ++y) {
                for (int x = inside.left(); x <= inside.right(); ++x) {
                    const QColor drawn = image.pixelColor(x, y);
                    if (std::abs(drawn.red() - fill.red()) + std::abs(drawn.green() - fill.green())
                            + std::abs(drawn.blue() - fill.blue()) > 150) {
                        first = std::min(first, x);
                        last = std::max(last, x);
                    }
                }
            }
            return std::max(0, last + 1 - first) / dpr;
        };
        QTRY_VERIFY2(std::abs(captionWidth() - wanted) <= 0.07 * wanted,
                     qPrintable(QStringLiteral("\"%1\" drawn %2 px wide, %3 in its font")
                                    .arg(text).arg(captionWidth()).arg(wanted)));
        // Room for all of it.
        QVERIFY(button->width() > wanted);
        // The style's bevel still, with no caption of its own under this one.
        auto *bevel = qvariant_cast<QQuickItem *>(button->property("background"));
        QVERIFY(bevel != nullptr && bevel->property("elementType").toString() == QLatin1String("button"));
        QCOMPARE(bevel->property("text").toString(), QString());
        QCOMPARE(bevel->property("activeControl").toString(), button == apply ? QStringLiteral("default") : QString());
        // Alt and the caption's letter still presses it, as the style's.
        auto *content = qvariant_cast<QQuickItem *>(button->property("contentItem"));
        QVERIFY(content != nullptr);
        QQmlExpression mnemonic(qmlContext(content), content, QStringLiteral("Kirigami.MnemonicData.sequence.toString()"));
        const QKeySequence keys(mnemonic.evaluate().toString());
        QVERIFY2(!keys.isEmpty(), qPrintable(text));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
        QQuickItem *caption = itemNamed(button, QStringLiteral("captionButtonCaption"));
        QQuickItem *underline = itemNamed(button, QStringLiteral("captionButtonMnemonic"));
        QVERIFY(caption != nullptr && underline != nullptr);
        QCOMPARE(caption->property("text").toString(), text);
        QVERIFY(!caption->property("truncated").toBool());
        // Underlined while Alt is held, and only then.
        QVERIFY(!underline->isVisible());
        QTest::keyPress(window, Qt::Key_Alt, Qt::AltModifier);
        QTRY_VERIFY2(underline->isVisible(), qPrintable(text));
        QTest::keyRelease(window, Qt::Key_Alt);
        QTRY_VERIFY(!underline->isVisible());
        if (button == back) {
            QSignalSpy clicked(button, SIGNAL(clicked()));
            QTest::keySequence(window, keys);
            QTRY_COMPARE(clicked.size(), 1);
        }
        QVERIFY(button->height() >= QFontMetricsF(declared).height());
    }
}

void AlbumArtSmokeTest::albumArtViewerShowsWhenItsImageCannotBeRead() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot open: .*missing\\.png")));
    QQmlApplicationEngine engine;
    const QString missingUrl = QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.png"))).toString();
    QString errorText;
    QScopedPointer<QObject> root(createSurfaceWindow(engine, {QSize(600, 400), missingUrl, QSize(), false}, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *surface = qvariant_cast<QQuickItem *>(root->property("surface"));
    QVERIFY(surface != nullptr);
    QQuickItem *errorState = itemNamed(surface, QStringLiteral("albumArtErrorState"));
    QVERIFY(errorState != nullptr);

    // The view says so in its own place, centred in it, rather than in a
    // banner.
    QTRY_VERIFY(errorState->isVisible());
    QCOMPARE(itemNamed(errorState, QStringLiteral("albumArtErrorText"))->property("text").toString(),
             QStringLiteral("Couldn't load this image."));
    const QPointF errorCenter = sceneRectOf(errorState).center();
    QVERIFY(std::abs(errorCenter.x() - 300) <= 1 && std::abs(errorCenter.y() - 200) <= 1);

    // An image it can read replaces the message.
    surface->setProperty("viewerSource", QUrl::fromLocalFile(coverPath).toString());
    QTRY_COMPARE(surface->property("imageStatus").toInt(), 1);
    QVERIFY(!errorState->isVisible());
    // Only the image's own warning, which the test expects.
    const QString log = takeCapturedMessagesText();
    QVERIFY2(log.contains(QStringLiteral("Cannot open")) && !log.contains(QLatin1Char('\n')),
             qPrintable(log));
}

namespace {

// Generated candidates for the iTunes artwork fake: ready rows, each with
// its own image, as the search and the download leave them.
QVariantList itunesArtworkFixture(const QTemporaryDir &dir, int count) {
    QVariantList rows;
    for (int i = 0; i < count; ++i) {
        const QString path = dir.filePath(QStringLiteral("candidate-%1.png").arg(i));
        QImage image(48, 48, QImage::Format_RGB32);
        image.fill(QColor::fromHsv((i * 37) % 360, 160, 200));
        if (!image.save(path)) {
            return {};
        }
        const QString url = QUrl::fromLocalFile(path).toString();
        rows.push_back(QVariantMap{
            {QStringLiteral("albumTitle"), QStringLiteral("Small Hours %1").arg(i)},
            {QStringLiteral("artistName"), QStringLiteral("The Quiet Meridian")},
            {QStringLiteral("previewSource"), url},
            {QStringLiteral("normalizedUrl"), url},
            {QStringLiteral("normalizedPath"), path},
            {QStringLiteral("assetReady"), true},
        });
    }
    return rows;
}

// A local fake of the iTunes artwork search and downloads, for the Replace
// from iTunes dialog of Main.qml: it holds the suggestions a test sets and
// records the ones prepared and applied. The playing track, its art and the
// image details come from `real`, the window's bridge.
QObject *injectItunesArtworkFake(QQmlEngine &engine, QObject *dialog, QObject *real) {
    QQmlComponent component(&engine);
    component.setData(QByteArrayLiteral(R"QML(
import QtQuick 2.15
QtObject {
    id: fake
    required property var real
    property var itunesArtworkResults: []
    property var itunesArtworkTarget: ({})
    property bool itunesArtworkLoading: false
    property string itunesArtworkStatusText: ""
    property bool itunesArtworkFailed: false
    property string itunesArtworkErrorText: ""
    property var preparedItunesArtwork: []
    property var appliedItunesArtwork: []
    readonly property string currentTrackPath: fake.real.currentTrackPath
    readonly property string currentTrackCoverPath: fake.real.currentTrackCoverPath
    // The backend's word on whether the cover has a cover's name, and the
    // cover's details, when a test sets them; else unknown, and the real
    // bridge's.
    property var currentTrackCoverNamed: undefined
    property var coverDetails: null
    signal itunesArtworkChanged()
    signal snapshotChanged()
    signal imageFileDetailsChanged(string path)
    function itunesArtworkResultAt(index) { return fake.itunesArtworkResults[index] || ({}) }
    function prepareItunesArtworkSuggestion(index) {
        fake.preparedItunesArtwork = fake.preparedItunesArtwork.concat([index])
    }
    function applyItunesArtworkSuggestion(index) {
        fake.appliedItunesArtwork = fake.appliedItunesArtwork.concat([index])
    }
    function clearItunesArtworkSuggestions() {
        fake.itunesArtworkResults = []
        fake.itunesArtworkTarget = ({})
    }
    function requestImageFileDetails(path) { fake.real.requestImageFileDetails(path) }
    function cachedImageFileDetails(path) {
        return fake.coverDetails !== null ? fake.coverDetails : fake.real.cachedImageFileDetails(path)
    }
    property Connections forward: Connections {
        target: fake.real
        function onSnapshotChanged() { fake.snapshotChanged() }
        function onImageFileDetailsChanged(path) { fake.imageFileDetailsChanged(path) }
    }
}
)QML"),
                      QUrl());
    QObject *fake = component.createWithInitialProperties(
        QVariantMap{{QStringLiteral("real"), QVariant::fromValue(real)}});
    if (fake == nullptr) {
        qWarning() << component.errorString();
        return nullptr;
    }
    fake->setParent(dialog);
    dialog->setProperty("uiBridge", QVariant::fromValue(fake));
    return fake;
}

// Replaces the fake's suggestion at `index` with `changes` applied, as a
// download that starts, ends or fails does.
void updateItunesArtworkRow(QObject *fake, int index, const QVariantMap &changes) {
    QVariantList rows = fake->property("itunesArtworkResults").toList();
    QVariantMap row = rows.value(index).toMap();
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        row.insert(it.key(), it.value());
    }
    rows[index] = row;
    fake->setProperty("itunesArtworkResults", rows);
    QMetaObject::invokeMethod(fake, "itunesArtworkChanged");
}

// Opens Replace from iTunes with `rows` for `target`, as a search finds them.
bool openItunesArtworkDialog(QQuickWindow *window, QObject *dialog, QObject *fake,
                             const QVariantList &rows, const QVariantMap &target) {
    fake->setProperty("itunesArtworkResults", rows);
    fake->setProperty("itunesArtworkTarget", target);
    QQmlEngine *engine = qmlEngine(dialog);
    // With its rows whole: the list builds those the hidden dialog had no
    // room for in the background (#627).
    return engine != nullptr
        && QMetaObject::invokeMethod(window, "openItunesArtworkDialog")
        && QTest::qWaitFor([dialog]() { return dialog->property("opened").toBool(); })
        && QTest::qWaitForWindowExposed(qobject_cast<QQuickWindow *>(dialog))
        && finishIncubation(*engine);
}

// Drops what the offscreen platform says when a window's least size
// changes, as a dialog's does with a larger font: it does not support it.
void dropSizeHintMessages() {
    dropCapturedMessages(offscreenSizeHintsWarning());
}

// Whether `item` is `ancestor` or inside it.
bool isWithin(const QQuickItem *item, const QQuickItem *ancestor) {
    for (; item != nullptr; item = item->parentItem()) {
        if (item == ancestor) {
            return true;
        }
    }
    return false;
}

// Clicks the preview's button `objectName` in `host` once it is enabled and
// sits whole inside the image's details, at the same place on two looks in
// a row, with the details slid in whole: where the user sees it, not where
// the viewer opening or the details sliding in might still move it from.
// No popup is still fading in or out, such as the dialog that opened the
// preview, which takes the click until it has gone.
bool clickPreviewButton(QQuickWindow *host, const QString &objectName) {
    QRectF last;
    const bool settled = QTest::qWaitFor([&]() {
        QQuickItem *column = visibleItemNamed(host, QStringLiteral("albumArtInfoColumn"));
        QQuickItem *button = visibleItemNamed(host, objectName);
        if (column == nullptr || button == nullptr || !button->isEnabled() || !isWithin(button, column)
            || column->parentItem()->property("infoReveal").toReal() < 1.0) {
            last = QRectF();
            return false;
        }
        const QRectF rect = sceneRectOf(button);
        const bool same = !last.isNull() && rect == last;
        last = rect;
        return same && sceneRectOf(column).contains(rect) && popupsSettled();
    });
    return settled && clickVisibleButton(host, objectName);
}

// During a preview in `host`: which image shows heads the image's details,
// its file's name under it in the secondary colour, and its
// buttons in a footer at their bottom, under a seam, Back to Results before
// Apply on one row, Apply the style's default button; the art
// keeps 24 px clear of the details' seam, centred in the space left of it;
// and the viewer's controls named by `prefix` have none of the preview's,
// nor Image Details.
void checkPreviewControlsInTheDetails(QQuickWindow *host, const QString &prefix,
                                      const QByteArray &at) {
    QQuickItem *column = visibleItemNamed(host, QStringLiteral("albumArtInfoColumn"));
    QVERIFY2(column != nullptr, at.constData());
    QQuickItem *label = visibleItemNamed(host, QStringLiteral("albumArtPreviewLabel"));
    QQuickItem *back = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"));
    QQuickItem *apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"));
    QVERIFY2(label != nullptr && back != nullptr && apply != nullptr, at.constData());
    const auto rectOf = [](QQuickItem *item) {
        return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    };
    // The viewer can still be taking its size, as its window shows, and
    // the details sliding in.
    const auto settled = [&]() {
        const QRectF rect = rectOf(column);
        return column->parentItem()->property("infoReveal").toReal() >= 1.0 && rect.contains(rectOf(label))
            && rect.contains(rectOf(back)) && rect.contains(rectOf(apply));
    };
    QTRY_VERIFY2(settled(), at.constData());
    const QRectF columnRect = rectOf(column);
    for (QQuickItem *item : {label, back, apply}) {
        QVERIFY2(isWithin(item, column), at.constData());
        QVERIFY2(columnRect.contains(rectOf(item)), at.constData());
    }
    // The label first, over the file's name; the buttons in a footer at
    // the column's bottom (they followed the details, crammed under them),
    // under a seam the column's width, the padding clear of the column's
    // edges, Back to Results then Apply, side by side or, in a view too
    // narrow for that, stacked. The details end above the footer.
    QQuickItem *title = visibleItemNamed(host, QStringLiteral("albumArtInfoTitle"));
    QVERIFY2(title != nullptr, at.constData());
    QVERIFY2(rectOf(label).bottom() <= rectOf(title).top(), at.constData());
    const qreal padding = column->parentItem()->property("infoPadding").toReal();
    QVERIFY2(padding >= 16.0, at.constData());
    QQuickItem *footer = visibleItemNamed(host, QStringLiteral("albumArtPreviewFooter"));
    QQuickItem *footerSeam = visibleItemNamed(host, QStringLiteral("albumArtPreviewFooterSeam"));
    QVERIFY2(footer != nullptr && footerSeam != nullptr, at.constData());
    QCOMPARE(rectOf(footer).bottom(), columnRect.bottom());
    QCOMPARE(rectOf(footerSeam), QRectF(columnRect.left(), rectOf(footer).top(), columnRect.width(), 1));
    QTRY_VERIFY2(qAbs(rectOf(apply).bottom() - (columnRect.bottom() - padding)) <= 0.5,
                 qPrintable(QStringLiteral("%1: the column ends at %2, the buttons at %3")
                                .arg(QString::fromUtf8(at)).arg(columnRect.bottom()).arg(rectOf(apply).bottom())));
    QCOMPARE(rectOf(back).top() - rectOf(footerSeam).bottom(), padding);
    qreal detailsBottom = rectOf(title).bottom();
    for (const QString &name : {QStringLiteral("albumArtInfoRow"), QStringLiteral("albumArtCompareButton")}) {
        for (QQuickItem *item : visibleItemsNamed(column, name)) {
            detailsBottom = qMax(detailsBottom, rectOf(item).bottom());
        }
    }
    QVERIFY2(detailsBottom <= rectOf(footerSeam).top(), at.constData());
    QQuickItem *buttons = visibleItemNamed(host, QStringLiteral("albumArtPreviewButtons"));
    QVERIFY2(buttons != nullptr, at.constData());
    if (buttons->property("oneRow").toBool()) {
        QCOMPARE(rectOf(back).top(), rectOf(apply).top());
        QVERIFY2(rectOf(back).right() <= rectOf(apply).left(), at.constData());
    } else {
        QVERIFY2(back->implicitWidth() + buttons->property("spacing").toReal() + apply->implicitWidth()
                     + 2 * padding > std::floor(column->parentItem()->width() * 0.4),
                 at.constData());
        QVERIFY2(rectOf(back).bottom() <= rectOf(apply).top(), at.constData());
    }
    QVERIFY2(columnRect.right() - rectOf(apply).right() <= 24.0, at.constData());
    // About 40 characters of the body font wide, 280 px with its seam at
    // the default font, up to half the view, or wider for the buttons; over
    // the whole screen, whose details' text is 1.2 times the body size, as
    // much wider, 336 px. Its contents 16 px in from both edges.
    const bool fullscreen = column->parentItem()->property("fullscreen").toBool();
    const qreal infoScale = fullscreen ? 1.2 : 1.0;
    const qreal widthScale = infoColumnWidthScale(fullscreen, column->parentItem()->width());
    QQuickItem *value = visibleItemsNamed(column, QStringLiteral("albumArtInfoValue")).value(0);
    QVERIFY2(value != nullptr, at.constData());
    QFont font = value->property("font").value<QFont>();
    font.setPointSizeF(font.pointSizeF() / infoScale);
    const qreal fortyCharacters = infoColumnFortyCharacters(font, fullscreen, column->parentItem()->width());
    const qreal halfTheView = std::floor(column->parentItem()->width() / 2);
    QVERIFY2(column->width() >= qMax(std::round(280 * widthScale) - 1, qMin(halfTheView, fortyCharacters)) - 0.5,
             qPrintable(QStringLiteral("%1: %2 px").arg(QString::fromUtf8(at)).arg(column->width())));
    QCOMPARE(rectOf(label).left() - columnRect.left(), padding);
    QCOMPARE(columnRect.right() - rectOf(apply).right(), padding);
    // The heading, which image shows, a step larger than the body size:
    // 12 pt to its 10; over the whole screen, the details' 16 px, as the
    // title strip's track title over it (at 18 px it outranked the title).
    // The file's name is secondary under it (it headed both images' details,
    // often "cover.jpg" for either).
    const qreal bodyPointSize = value->property("font").value<QFont>().pointSizeF() / infoScale;
    const auto headingFont = label->property("font").value<QFont>();
    QVERIFY2(qAbs(headingFont.pointSizeF() - bodyPointSize * 1.2) < 0.001, at.constData());
    QCOMPARE(headingFont.weight(), QFont::DemiBold);
    QVERIFY2(title->property("font").value<QFont>().pointSizeF() < bodyPointSize * infoScale, at.constData());
    QCOMPARE(title->property("font").value<QFont>().weight(), QFont::Normal);
    QVERIFY2(colorOf(label, "color") != colorOf(title, "color"), at.constData());
    // Apply is the style's default button, as a dialog's (it was a solid
    // accent fill, unlike any other default button); Back to Results is a
    // plain one. Both are the style's own, drawn in Breeze's complementary
    // colours, dark in either theme as the details are.
    const auto evaluate = [](QQuickItem *button, const QString &expression) {
        return QQmlExpression(qmlContext(button), button, expression).evaluate();
    };
    QVERIFY2(evaluate(apply, QStringLiteral("Accessible.defaultButton")).toBool(), at.constData());
    QVERIFY2(!evaluate(back, QStringLiteral("Accessible.defaultButton")).toBool(), at.constData());
    for (QQuickItem *button : {back, apply}) {
        QVERIFY2(itemNamed(button, QStringLiteral("albumArtDetailsButtonLabel")) == nullptr, at.constData());
        QVERIFY2(!button->property("flat").toBool() && !button->property("highlighted").toBool(), at.constData());
        QCOMPARE(evaluate(button, QStringLiteral("Kirigami.Theme.colorSet === Kirigami.Theme.Complementary")).toBool(),
                 true);
        QCOMPARE(evaluate(button, QStringLiteral("Kirigami.Theme.inherit")).toBool(), false);
    }
    // The art fits the space left of the seam, 24 px clear of it, centred
    // there.
    QQuickItem *image = visibleItemNamed(host, QStringLiteral("albumArtImage"));
    QQuickItem *seam = visibleItemNamed(host, QStringLiteral("albumArtInfoSeam"));
    QVERIFY2(image != nullptr && seam != nullptr, at.constData());
    // One seam colour, as drawn, where the column's seam meets the footer's
    // and, over the whole screen, the strip's rule (it was #242424 between
    // their #333333).
    QList<std::pair<QString, QPointF>> rules{
        {QStringLiteral("the footer's seam"), rectOf(footerSeam).center()}};
    QQuickItem *strip = visibleItemNamed(host, QStringLiteral("albumArtFullscreenStrip"));
    if (fullscreen && strip != nullptr && qFuzzyCompare(strip->opacity(), 1.0)) {
        QQuickItem *rule = visibleItemNamed(host, QStringLiteral("viewerStripHairline"));
        QVERIFY2(rule != nullptr, at.constData());
        rules.append({QStringLiteral("the strip's rule"), QPointF(rectOf(column).center().x(), rectOf(rule).center().y())});
    }
    const auto unlike = [&]() -> QString {
        const QImage drawn = host->grabWindow().convertToFormat(QImage::Format_RGB32);
        const double dpr = static_cast<double>(drawn.width()) / host->width();
        const auto pixel = [&](const QPointF &point) {
            return drawn.pixelColor(int(point.x() * dpr), int(point.y() * dpr));
        };
        const QColor seamColor = pixel(rectOf(seam).center());
        for (const auto &[what, point] : std::as_const(rules)) {
            const QColor other = pixel(point);
            if (std::abs(seamColor.red() - other.red()) > 1 || std::abs(seamColor.green() - other.green()) > 1
                || std::abs(seamColor.blue() - other.blue()) > 1) {
                return QStringLiteral("the column's seam %1, %2 %3").arg(seamColor.name(), what, other.name());
            }
        }
        return {};
    };
    QTRY_VERIFY2(unlike().isEmpty(), qPrintable(QString::fromUtf8(at) + QStringLiteral(": ") + unlike()));
    const auto paintedArt = [image]() {
        const QSizeF painted(image->property("paintedWidth").toReal(),
                             image->property("paintedHeight").toReal());
        return QRectF(image->mapToScene(QPointF((image->width() - painted.width()) / 2,
                                                (image->height() - painted.height()) / 2)),
                      painted);
    };
    QTRY_VERIFY2(paintedArt().width() > 0
                     && rectOf(seam).left() - paintedArt().right() >= 24.0 - 0.5
                     && qAbs(paintedArt().center().x() - rectOf(seam).left() / 2) <= 1.0,
                 qPrintable(QStringLiteral("%1: art %2..%3, seam at %4")
                                .arg(QString::fromUtf8(at)).arg(paintedArt().left())
                                .arg(paintedArt().right()).arg(rectOf(seam).left())));
    // Nothing of the preview on the strip or the corner plate.
    for (const QString &name : {QStringLiteral("PreviewLabel"), QStringLiteral("BackButton"),
                                QStringLiteral("ApplyButton"), QStringLiteral("InfoButton")}) {
        QVERIFY2(visibleItemNamed(host, prefix + name) == nullptr,
                 qPrintable(QStringLiteral("%1: %2").arg(QString::fromUtf8(at), prefix + name)));
    }
}

// The Replace Album Art question: a window of its own, above the window
// that asks it.
QQuickWindow *replaceQuestion(QQuickWindow *window) {
    return qobject_cast<QQuickWindow *>(window->findChild<QObject *>(QStringLiteral("replaceCoverConfirmDialog")));
}

// The question's button `name`, while the question shows above `host`.
QQuickItem *replaceQuestionButton(QQuickWindow *window, QQuickWindow *host, const QString &name) {
    QQuickWindow *question = replaceQuestion(window);
    return question != nullptr && question->isVisible() && question->transientParent() == host
            && QTest::qWaitForWindowExposed(question)
        ? visibleItemNamed(question, name)
        : nullptr;
}

// Applies candidate `index` from its preview, past the question, and waits
// until the dialog and the question have gone, faded out, and the viewer
// shows `appliedUrl`.
void applyItunesCandidateFromPreview(QQuickWindow *window, QObject *dialog, QObject *fake,
                                     QObject *viewer, int index, const QString &appliedUrl,
                                     const QByteArray &at) {
    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, index)));
    QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
    QVERIFY2(clickPreviewButton(window, QStringLiteral("albumArtPreviewApplyButton")), at.constData());
    QTRY_VERIFY2(replaceQuestionButton(window, window, QStringLiteral("confirmDialogConfirmButton")) != nullptr,
                 at.constData());
    // Asked from the preview, the question has its heading: it names the
    // album, above the details, never a blank line (opened alone, with no
    // message, it showed one).
    {
        QQuickItem *message = visibleItemNamed(replaceQuestion(window), QStringLiteral("confirmDialogMessage"));
        QQuickItem *details =
            visibleItemNamed(replaceQuestion(window), QStringLiteral("replaceCoverFolderCaseOutcome"));
        QVERIFY2(message != nullptr && details != nullptr, at.constData());
        const QString text = message->property("text").toString();
        QVERIFY2(text.startsWith(QStringLiteral("Replace the ")) && text.contains(QStringLiteral("album art")) && message->height() > 0,
                 qPrintable(text));
        QVERIFY(sceneRectOf(message).bottom() <= sceneRectOf(details).top());
        // The question leads, semibold, the details regular.
        QCOMPARE(message->property("font").value<QFont>().weight(), QFont::DemiBold);
        QCOMPARE(details->property("font").value<QFont>().weight(), QFont::Normal);
    }
    QVERIFY(clickVisibleButton(replaceQuestion(window), QStringLiteral("confirmDialogConfirmButton")));
    QTRY_COMPARE(fake->property("appliedItunesArtwork").toList(), (QVariantList{index}));
    QTRY_VERIFY2(!dialog->property("visible").toBool(), at.constData());
    QVERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(!viewer->property("albumArtPreviewActive").toBool());
    QCOMPARE(viewer->property("albumArtViewerSource").toString(), appliedUrl);
    QTRY_VERIFY2(visibleItemNamed(window, QStringLiteral("albumArtPreviewBackButton")) == nullptr,
                 at.constData());
    QTRY_VERIFY2(popupsSettled(), at.constData());
}

// Main.qml, active, with the iTunes artwork fake as its Replace from iTunes
// dialog's bridge, and a track playing whose album has the art at
// `coverUrl`.
struct ItunesWindow {
    explicit ItunesWindow(const QString &coverUrl) {
        window = loadMainWindow(engine, libraryModel);
        if (window == nullptr) {
            return;
        }
        bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
        surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
        if (bridge != nullptr && dialog != nullptr) {
            fake = injectItunesArtworkFake(engine, dialog, bridge);
            setPlayingTrack(bridge);
            bridge->setProperty("currentTrackCoverPath", coverUrl);
        }
    }
    bool loaded() const {
        return window != nullptr && bridge != nullptr && viewer != nullptr && dialog != nullptr && surface != nullptr
            && fake != nullptr;
    }
    // The window the viewer shows in: the main window, or its own over the
    // whole screen.
    QQuickWindow *viewerHost() {
        return bridge->property("viewerFullscreenMode").toInt() == 1
            ? viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"))
            : window;
    }
    QVariantList applied() const { return fake->property("appliedItunesArtwork").toList(); }

    // The fades and slides, each to its end at once: the tests that use
    // this window check where they end.
    FastAnimationDriver animations;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    QObject *bridge = nullptr;
    QObject *viewer = nullptr;
    QObject *dialog = nullptr;
    QQuickItem *surface = nullptr;
    QObject *fake = nullptr;
};

// Starts a new play, as the bridge reports it: the play serial advances, also
// when the path stays.
void startPlay(QObject *bridge) {
    bridge->setProperty("currentPlaySerial", bridge->property("currentPlaySerial").toDouble() + 1);
}

// The name of the item with the keyboard focus in `window`.
QString focusedName(QQuickWindow *window) {
    QQuickItem *focused = window->activeFocusItem();
    return focused != nullptr ? focused->objectName() : QString();
}

// A point on the art the album art viewer's `surface` shows, clear of the
// details and the controls over it: in the left quarter of the image as
// drawn, halfway down. Null until the image has loaded.
QPoint pointOnTheArt(QQuickItem *surface) {
    if (!QTest::qWaitFor([surface]() { return surface->property("imageStatus").toInt() == 1; }, 5000)) {
        return {};
    }
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QQuickItem *viewport = itemNamed(surface, QStringLiteral("albumArtViewport"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    if (image == nullptr || viewport == nullptr || column == nullptr) {
        return {};
    }
    const QSizeF painted(image->property("paintedWidth").toReal(), image->property("paintedHeight").toReal());
    const QPointF origin((image->width() - painted.width()) / 2, (image->height() - painted.height()) / 2);
    const QRectF art = QRectF(image->mapToScene(origin),
                              image->mapToScene(origin + QPointF(painted.width(), painted.height())))
                           .intersected(sceneRectOf(viewport));
    const QPointF point(art.left() + art.width() / 4, art.center().y());
    if (art.width() < 8 || (column->isVisible() && point.x() >= sceneRectOf(column).left())) {
        return {};
    }
    return point.toPoint();
}

} // namespace

namespace {

// Replace from iTunes on its own, with a stand-in bridge whose state a test
// sets: the playing track's art, with its details, and the suggestions.
QObject *createItunesArtworkDialogWindow(QQmlApplicationEngine &engine, const QString &coverUrl,
                                         QString *errorText) {
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QObject *root = createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import "components" as Components
import "dialogs" as Dialogs
ApplicationWindow {
    id: window
    width: 1000
    height: 760
    visible: true
    property alias dialog: dialog
    property alias bridge: bridge
    property string coverUrl: ""
    Components.UiPalette { id: palette; windowRoot: window }
    QtObject {
        id: bridge
        property string currentTrackPath: "/fixture/Small Hours/01 Lantern Signal.flac"
        property string currentTrackCoverPath: window.coverUrl
        property var itunesArtworkTarget: ({})
        property var itunesArtworkResults: []
        property bool itunesArtworkLoading: false
        property string itunesArtworkStatusText: ""
        property bool itunesArtworkFailed: false
        property string itunesArtworkErrorText: ""
        signal itunesArtworkChanged()
        signal imageFileDetailsChanged(string path)
        signal snapshotChanged()
        function itunesArtworkResultAt(index) { return itunesArtworkResults[index] || ({}) }
        function applyItunesArtworkSuggestion(index) {}
        function prepareItunesArtworkSuggestion(index) {}
        function requestImageFileDetails(path) {}
        function cachedImageFileDetails(path) {
            return { width: 1200, height: 1200, format: "JPEG", fileType: "JPEG image",
                     fileSizeBytes: 39244, fileSizeText: "38 KiB (39,244 bytes)",
                     mimeType: "image/jpeg" }
        }
        function clearItunesArtworkSuggestions() {}
    }
    Dialogs.ItunesArtworkDialog {
        id: dialog
        uiBridge: bridge
        uiPalette: palette
        windowRoot: window
        openAlbumArtViewerForSuggestion: function(row) { return false }
        openAlbumArtViewerForCurrentArt: function() { return false }
        endAlbumArtPreview: function(applied) {}
    }
}
)QML"), baseUrl, errorText);
    if (root != nullptr) {
        root->setProperty("coverUrl", coverUrl);
    }
    return root;
}

// The visible items under `root` that draw a filled or outlined
// rectangle, a card's background, leaving out the buttons, which have
// their style's.
QList<QQuickItem *> rectanglesIn(QQuickItem *root) {
    QList<QQuickItem *> found;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible() || item->objectName().endsWith(QStringLiteral("Button"))) {
            return;
        }
        if (item != root && item->property("radius").isValid() && item->property("border").isValid()) {
            found.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    visit(root);
    return found;
}

// Every visible text under `root`, but the buttons' labels.
QStringList visibleTextsIn(QQuickItem *root) {
    QStringList texts;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible() || item->objectName().endsWith(QStringLiteral("Button"))) {
            return;
        }
        if (item->property("lineCount").isValid()) {
            texts << item->property("text").toString();
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    visit(root);
    return texts;
}

} // namespace

void AlbumArtSmokeTest::itunesArtworkResultsAreAFlatList() {
    // The results are a flat list on the view colour: no card around a row
    // or the current art, a 64 px thumbnail, a hairline between rows, the
    // title and one line of details in plain words, and flat Preview and
    // Apply at the row's end. The current art is on the same grid.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    QVariantList rows = itunesArtworkFixture(dir, 3);
    QCOMPARE(rows.size(), 3);
    QVariantMap ready = rows[0].toMap();
    ready.insert(QStringLiteral("width"), 1200);
    ready.insert(QStringLiteral("height"), 1200);
    ready.insert(QStringLiteral("format"), QStringLiteral("JPEG"));
    ready.insert(QStringLiteral("fileType"), QStringLiteral("JPEG image"));
    ready.insert(QStringLiteral("fileSizeBytes"), 412 * 1024);
    ready.insert(QStringLiteral("fileSizeText"), QStringLiteral("412 KiB (421,888 bytes)"));
    ready.insert(QStringLiteral("mimeType"), QStringLiteral("image/jpeg"));
    rows[0] = ready;
    QVariantMap loading = rows[1].toMap();
    loading.insert(QStringLiteral("assetLoading"), true);
    loading.insert(QStringLiteral("detailStatusText"), QStringLiteral("Loading full-size image..."));
    rows[1] = loading;
    QVariantMap failed = rows[2].toMap();
    failed.insert(QStringLiteral("assetError"), QStringLiteral("The artwork could not be downloaded."));
    failed.insert(QStringLiteral("detailStatusText"),
                  QStringLiteral("The full-size image couldn\u2019t be loaded."));
    rows[2] = failed;

    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createItunesArtworkDialogWindow(engine, coverUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *dialog = qvariant_cast<QObject *>(root->property("dialog"));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QVERIFY(dialog != nullptr && bridge != nullptr);
    bridge->setProperty("itunesArtworkResults", rows);
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("Found 3 suggestions."));
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(qobject_cast<QQuickWindow *>(dialog)));
    QTRY_VERIFY(popupsSettled());
    // The content inside the window's insets.
    auto *content = itemNamed(qobject_cast<QQuickWindow *>(dialog)->contentItem(),
                              QStringLiteral("itunesArtworkContent"));
    QVERIFY(content != nullptr);

    QList<QQuickItem *> rowItems;
    QTRY_COMPARE((rowItems = visibleItemsNamed(content, QStringLiteral("itunesArtworkResultRow"))).size(), 3);
    // A row is in the list before it is whole (#627).
    QVERIFY(finishIncubation(engine));
    // Laid out in its window, three rows need no scroll bar.
    QQuickItem *resultsView = itemNamed(content, QStringLiteral("itunesArtworkResultsView"));
    QVERIFY(resultsView != nullptr);
    QTRY_VERIFY(resultsView->property("contentHeight").toReal() <= resultsView->height() + 0.5);
    std::sort(rowItems.begin(), rowItems.end(), [](QQuickItem *a, QQuickItem *b) {
        return sceneRectOf(a).top() < sceneRectOf(b).top();
    });
    QQuickItem *frame = itemNamed(content, QStringLiteral("itunesArtworkResultsFrame"));
    QQuickItem *currentRow = itemNamed(content, QStringLiteral("itunesArtworkCurrentRow"));
    QVERIFY(frame != nullptr && currentRow != nullptr);
    // On the view colour, in a square frame: no card.
    QCOMPARE(frame->property("radius").toReal(), 0.0);
    QCOMPARE(frame->property("color").value<QColor>(),
             qvariant_cast<QObject *>(dialog->property("uiPalette"))->property("view").value<QColor>());
    // Neither a row nor the current art has a background of its own: the
    // only rectangles are the hairlines between rows, 1 px high.
    QVERIFY(!currentRow->property("color").isValid());
    QVERIFY(rectanglesIn(currentRow).isEmpty());
    int hairlines = 0;
    for (QQuickItem *row : rowItems) {
        QVERIFY(!row->property("color").isValid());
        for (QQuickItem *rect : rectanglesIn(row)) {
            QCOMPARE(rect->objectName(), QStringLiteral("itunesArtworkResultHairline"));
            QCOMPARE(rect->height(), 1.0);
            // From the text's left edge to the row's end (not from the
            // frame's edge, under the thumbnail).
            QQuickItem *rowTitle = itemNamed(row, QStringLiteral("itunesArtworkResultTitle"));
            QQuickItem *rowThumbnail = itemNamed(row, QStringLiteral("itunesArtworkResultThumbnail"));
            QVERIFY(rowTitle != nullptr && rowThumbnail != nullptr);
            QCOMPARE(sceneRectOf(rect).left(), rowTitle->mapToScene(QPointF(0, 0)).x());
            QCOMPARE(sceneRectOf(rect).left(), sceneRectOf(rowThumbnail).right() + 12);
            QCOMPARE(sceneRectOf(rect).right(), sceneRectOf(row).right());
            QCOMPARE(sceneRectOf(rect).bottom(), sceneRectOf(row).bottom());
            ++hairlines;
        }
    }
    QCOMPARE(hairlines, 2);

    // Each row: a 64 px thumbnail, the title, and exactly one line of
    // details, rows as tall as the thumbnail with 8 px above and below
    // (itunesArtworkScalesAsAWholeWithTheFont has the sizes). The
    // size as the art viewer gives it, "1,200 × 1,200 px", with the locale's
    // thousands separator (not "1200 × 1200").
    const QString itunesSize = QLocale().toString(1200.0, 'f', 0);
    const QStringList expectedDetails{
        QStringLiteral("The Quiet Meridian · %1 × %1 px · JPEG · 412 KiB").arg(itunesSize),
        QStringLiteral("The Quiet Meridian · Loading full-size image…"),
        QStringLiteral("The artwork could not be downloaded."),
    };
    const qreal thumbnailSize = dialog->property("thumbnailSize").toReal();
    const qreal rowHeight = dialog->property("rowHeight").toReal();
    QCOMPARE(rowHeight, thumbnailSize + 2 * 8);
    for (int i = 0; i < rowItems.size(); ++i) {
        QQuickItem *row = rowItems[i];
        const QByteArray at = QByteArray("row ") + QByteArray::number(i);
        QCOMPARE(row->height(), rowHeight);
        QQuickItem *thumbnail = itemNamed(row, QStringLiteral("itunesArtworkResultThumbnail"));
        QVERIFY2(thumbnail != nullptr, at.constData());
        QCOMPARE(sceneRectOf(thumbnail).size(), QSizeF(thumbnailSize, thumbnailSize));
        const QList<QQuickItem *> titles = visibleItemsNamed(row, QStringLiteral("itunesArtworkResultTitle"));
        const QList<QQuickItem *> details = visibleItemsNamed(row, QStringLiteral("itunesArtworkResultDetails"));
        QCOMPARE(titles.size(), 1);
        QCOMPARE(details.size(), 1);
        QCOMPARE(titles[0]->property("text").toString(), QStringLiteral("Small Hours %1").arg(i));
        QCOMPARE(details[0]->property("text").toString(), expectedDetails[i]);
        // Two lines of text, and nothing else but the buttons'.
        QCOMPARE(visibleTextsIn(row).filter(QRegularExpression(QStringLiteral("\\S"))).size(), 2);
        // Flat Preview, then Apply at the row's end, side by side.
        QQuickItem *preview = itemNamed(row, QStringLiteral("itunesArtworkPreviewButton"));
        QQuickItem *apply = itemNamed(row, QStringLiteral("itunesArtworkApplyButton"));
        QVERIFY2(preview != nullptr && apply != nullptr, at.constData());
        QVERIFY2(preview->property("flat").toBool() && apply->property("flat").toBool(), at.constData());
        QCOMPARE(sceneRectOf(preview).center().y(), sceneRectOf(apply).center().y());
        QVERIFY2(sceneRectOf(preview).right() <= sceneRectOf(apply).left(), at.constData());
        QVERIFY2(sceneRectOf(row).right() - sceneRectOf(apply).right() <= 16.5, at.constData());
        QCOMPARE(apply->property("text").toString(), QStringLiteral("Apply"));
        QCOMPARE(preview->property("text").toString(), QStringLiteral("Preview"));
        QCOMPARE(apply->isEnabled(), i != 1);
    }
    // A failed download says so in the negative colour.
    QQuickItem *failedDetails = visibleItemsNamed(rowItems[2], QStringLiteral("itunesArtworkResultDetails")).first();
    QQuickItem *readyDetails = visibleItemsNamed(rowItems[0], QStringLiteral("itunesArtworkResultDetails")).first();
    QVERIFY(failedDetails->property("color").value<QColor>() != readyDetails->property("color").value<QColor>());

    // No developer's MIME line anywhere.
    for (const QString &text : visibleTextsIn(content)) {
        QVERIFY2(!text.contains(QStringLiteral("MIME")) && !text.contains(QStringLiteral("image/")),
                 qPrintable(text));
    }

    // The current art: on the same grid, its thumbnail over the results'
    // and its Preview over theirs, with one line of plain details.
    QQuickItem *currentThumbnail = itemNamed(currentRow, QStringLiteral("itunesArtworkCurrentThumbnail"));
    QQuickItem *firstThumbnail = itemNamed(rowItems[0], QStringLiteral("itunesArtworkResultThumbnail"));
    QVERIFY(currentThumbnail != nullptr && firstThumbnail != nullptr);
    QCOMPARE(sceneRectOf(currentThumbnail).size(), QSizeF(thumbnailSize, thumbnailSize));
    QCOMPARE(sceneRectOf(currentThumbnail).left(), sceneRectOf(firstThumbnail).left());
    QQuickItem *currentPreview = itemNamed(currentRow, QStringLiteral("itunesArtworkCurrentPreviewButton"));
    QQuickItem *firstPreview = itemNamed(rowItems[0], QStringLiteral("itunesArtworkPreviewButton"));
    QVERIFY(currentPreview != nullptr && firstPreview != nullptr);
    QVERIFY(currentPreview->property("flat").toBool());
    QCOMPARE(sceneRectOf(currentPreview).left(), sceneRectOf(firstPreview).left());
    QQuickItem *currentTitle = itemNamed(currentRow, QStringLiteral("itunesArtworkCurrentTitle"));
    QQuickItem *firstTitle = itemNamed(rowItems[0], QStringLiteral("itunesArtworkResultTitle"));
    QVERIFY(currentTitle != nullptr && firstTitle != nullptr);
    QCOMPARE(sceneRectOf(currentTitle).left(), sceneRectOf(firstTitle).left());
    QCOMPARE(visibleItemsNamed(currentRow, QStringLiteral("itunesArtworkCurrentDetails")).size(), 1);
    QTRY_COMPARE(itemNamed(currentRow, QStringLiteral("itunesArtworkCurrentDetails"))->property("text").toString(),
                 QStringLiteral("%1 × %1 px · JPEG · 38 KiB").arg(itunesSize));
    QCOMPARE(currentRow->height(), rowHeight);
    // The dialog's first line, with no space above it.
    QCOMPARE(sceneRectOf(currentRow).top(), sceneRectOf(content).top());
    // What the search found: between the current art and the results'
    // frame, 8 px clear of each, its text on the thumbnails' inset.
    QQuickItem *heading = itemNamed(content, QStringLiteral("itunesArtworkResultsHeading"));
    QVERIFY(heading != nullptr && heading->isVisible());
    QCOMPARE(heading->property("text").toString(), QStringLiteral("Found 3 suggestions."));
    QCOMPARE(sceneRectOf(heading).top() - sceneRectOf(currentRow).bottom(), 8.0);
    QCOMPARE(sceneRectOf(frame).top() - sceneRectOf(heading).bottom(), 8.0);
    QCOMPARE(sceneRectOf(heading).left(), sceneRectOf(firstThumbnail).left());
    // Nothing to search for or wait on.
    QVERIFY(visibleItemsNamed(content, QStringLiteral("itunesArtworkPlaceholder")).isEmpty());
}

void AlbumArtSmokeTest::itunesArtworkDialogShowsAPlaceholderWhileSearchingAndWithNothingFound() {
    // While it searches, when it found nothing, and when the search failed,
    // a placeholder message where the results would be, a little above
    // their middle, with no frame or well around it, and a running busy
    // indicator above it while searching, under the current art, which stays
    // the dialog's first line. A search that ends without results says how
    // in a semibold heading, the window's whole answer, with a line under
    // it: "No artwork found" under the search's glyph, and a failure, with
    // the system's words, under a warning glyph, so it does not read as
    // finding nothing. The glyph is 48 px in the spinner's place (32 px
    // and a lone line were under-scaled for the window's answer).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createItunesArtworkDialogWindow(engine, coverUrl, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr && QTest::qWaitForWindowExposed(window));
    QObject *dialog = qvariant_cast<QObject *>(root->property("dialog"));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QVERIFY(dialog != nullptr && bridge != nullptr);
    bridge->setProperty("itunesArtworkLoading", true);
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("Searching iTunes…"));
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(qobject_cast<QQuickWindow *>(dialog)));
    // The content inside the window's insets.
    auto *content = itemNamed(qobject_cast<QQuickWindow *>(dialog)->contentItem(),
                              QStringLiteral("itunesArtworkContent"));
    QVERIFY(content != nullptr);
    QQuickItem *frame = itemNamed(content, QStringLiteral("itunesArtworkResultsFrame"));
    QQuickItem *placeholder = itemNamed(content, QStringLiteral("itunesArtworkPlaceholder"));
    QQuickItem *message = itemNamed(content, QStringLiteral("itunesArtworkPlaceholderMessage"));
    QQuickItem *busy = itemNamed(content, QStringLiteral("itunesArtworkBusyIndicator"));
    QQuickItem *results = itemNamed(content, QStringLiteral("itunesArtworkResultsView"));
    QQuickItem *currentRow = itemNamed(content, QStringLiteral("itunesArtworkCurrentRow"));
    QQuickItem *heading = itemNamed(content, QStringLiteral("itunesArtworkPlaceholderHeading"));
    QVERIFY(frame != nullptr && placeholder != nullptr && message != nullptr && busy != nullptr
            && results != nullptr && currentRow != nullptr && heading != nullptr);
    QObject *palette = qvariant_cast<QObject *>(dialog->property("uiPalette"));
    QVERIFY(palette != nullptr);

    enum class State { Searching, NothingFound, Failed };
    const auto checkState = [&](State state, const QString &text) {
        const bool searching = state == State::Searching;
        const bool failed = state == State::Failed;
        const QByteArray at = searching ? "searching" : (failed ? "failed" : "nothing found");
        QTRY_VERIFY2(placeholder->isVisible(), at.constData());
        QVERIFY2(!results->isVisible(), at.constData());
        QCOMPARE(message->property("text").toString(), text);
        QCOMPARE(busy->isVisible(), searching);
        QCOMPARE(busy->property("running").toBool(), searching);
        if (searching) {
            // The style's spinner shows, at full opacity.
            auto *spinner = qvariant_cast<QQuickItem *>(busy->property("contentItem"));
            QVERIFY2(spinner != nullptr && spinner->isVisible(), at.constData());
            QTRY_COMPARE(spinner->opacity(), 1.0);
            QCOMPARE(busy->width(), 48.0);
            QCOMPARE(busy->height(), 48.0);
        }
        // No frame or well: the dialog's own colour behind the message.
        QCOMPARE(frame->property("color").value<QColor>().alpha(), 0);
        QCOMPARE(frame->property("border").value<QObject *>()->property("width").toReal(), 0.0);
        for (QQuickItem *rect : rectanglesIn(frame)) {
            QVERIFY2(rect->property("color").value<QColor>().alpha() == 0
                         && rect->property("border").value<QObject *>()->property("width").toReal() == 0.0,
                     qPrintable(rect->objectName()));
        }
        // A quiet placeholder: a centred line in the base font, regular,
        // in the secondary colour whether it searches, found nothing or
        // failed, when it holds the system's words (nothing found was in
        // the text's, brighter than searching).
        QCOMPARE(message->property("color").value<QColor>().rgba(),
                 palette->property("textSecondary").value<QColor>().rgba());
        QCOMPARE(heading->isVisible(), !searching);
        if (!searching) {
            QCOMPARE(heading->property("text").toString(), failed ? QStringLiteral("Couldn\u2019t search iTunes")
                                                                  : QStringLiteral("No artwork found"));
            QCOMPARE(heading->property("font").value<QFont>().weight(), QFont::DemiBold);
            QCOMPARE(heading->property("color").value<QColor>().rgba(),
                     palette->property("text").value<QColor>().rgba());
            QCOMPARE(heading->property("horizontalAlignment").toInt(), int(Qt::AlignHCenter));
            // Laid out once it shows.
            QTRY_VERIFY2(sceneRectOf(heading).bottom() <= sceneRectOf(message).top(), at.constData());
        }
        // A failed search has a warning glyph where the spinner is while
        // searching: 32 px, a mask in the negative colour as every failure's
        // glyph is, above the heading.
        QQuickItem *warning = itemNamed(content, QStringLiteral("itunesArtworkFailureIcon"));
        QVERIFY2(warning != nullptr, at.constData());
        QCOMPARE(warning->isVisible(), failed);
        if (failed) {
            // The app's own warning mark, whose "!" survives the mask
            // (dialog-warning drew a plain triangle).
            QVERIFY2(warning->property("source").toString().endsWith(QStringLiteral("assets/status-warning.svg")),
                     qPrintable(warning->property("source").toString()));
            QVERIFY(warning->property("isMask").toBool());
            QCOMPARE(warning->property("color").value<QColor>().rgba(),
                     palette->property("negative").value<QColor>().rgba());
            QTRY_COMPARE(sceneRectOf(warning).size(), QSizeF(48, 48));
            QCOMPARE(sceneRectOf(warning).top(), sceneRectOf(placeholder).top());
            QCOMPARE(sceneRectOf(warning).center().x(), sceneRectOf(placeholder).center().x());
            QVERIFY2(sceneRectOf(warning).bottom() < sceneRectOf(heading).top(), at.constData());
        }
        if (searching) {
            QCOMPARE(sceneRectOf(busy).top(), sceneRectOf(placeholder).top());
        }
        // Finding nothing has the search's glyph there, as quiet as the
        // line.
        QQuickItem *found = itemNamed(content, QStringLiteral("itunesArtworkNothingFoundIcon"));
        QVERIFY2(found != nullptr, at.constData());
        QCOMPARE(found->isVisible(), !searching && !failed);
        if (!searching && !failed) {
            QCOMPARE(found->property("source").toString(), QStringLiteral("edit-find"));
            QVERIFY(found->property("isMask").toBool());
            QCOMPARE(found->property("color").value<QColor>().rgba(),
                     palette->property("textSecondary").value<QColor>().rgba());
            QTRY_COMPARE(sceneRectOf(found).size(), QSizeF(48, 48));
            QCOMPARE(sceneRectOf(found).top(), sceneRectOf(placeholder).top());
            QCOMPARE(sceneRectOf(found).center().x(), sceneRectOf(placeholder).center().x());
            QVERIFY2(sceneRectOf(found).bottom() < sceneRectOf(heading).top(), at.constData());
        }
        // The only glyph: the busy indicator's while searching, the
        // warning's when the search failed, the search's with nothing found.
        int glyphs = 0;
        const std::function<void(QQuickItem *)> countGlyphs = [&](QQuickItem *item) {
            if (!item->isVisible()) {
                return;
            }
            if (QString::fromLatin1(item->metaObject()->className()).contains(QStringLiteral("Icon"))) {
                ++glyphs;
            }
            for (QQuickItem *child : item->childItems()) {
                countGlyphs(child);
            }
        };
        countGlyphs(placeholder);
        QCOMPARE(glyphs, 1);
        QCOMPARE(message->property("horizontalAlignment").toInt(), int(Qt::AlignHCenter));
        const QFont font = message->property("font").value<QFont>();
        QCOMPARE(font.weight(), QFont::Normal);
        QCOMPARE(font.pointSizeF(), QGuiApplication::font().pointSizeF());
        QCOMPARE(message->property("opacity").toReal(), 1.0);
        // Where the results would be: the glyph's slot and one line 3 grid
        // units above their middle, in every state.
        QVERIFY2(sceneRectOf(frame).contains(sceneRectOf(placeholder)), at.constData());
        const qreal slot = busy->parentItem()->height();
        const qreal oneLine = slot + placeholder->property("spacing").toReal()
            + QFontMetricsF(message->property("font").value<QFont>()).height();
        const qreal raise = qMin(3 * dialog->property("contentPadding").toReal(),
                                 (sceneRectOf(frame).height() - oneLine) / 2);
        QTRY_VERIFY2(qAbs(sceneRectOf(placeholder).top() + oneLine / 2 - (sceneRectOf(frame).center().y() - raise)) <= 1.5,
                 qPrintable(QStringLiteral("%1: at %2, the frame's middle %3")
                                .arg(QString::fromLatin1(at)).arg(sceneRectOf(placeholder).top())
                                .arg(sceneRectOf(frame).center().y())));
        QVERIFY2(raise > 0, at.constData());
        if (searching) {
            QVERIFY2(sceneRectOf(busy).bottom() <= sceneRectOf(message).top(), at.constData());
        }
        // The current art first, with no space above it, then the frame.
        QCOMPARE(sceneRectOf(currentRow).top(), sceneRectOf(content).top());
        QVERIFY2(sceneRectOf(frame).top() - sceneRectOf(currentRow).bottom() <= 8.0, at.constData());
        // No other glyph or text stands in for the state.
        QStringList texts = visibleTextsIn(content);
        texts.removeAll(QString());
        QCOMPARE(texts.count(text), 1);
    };
    // The current art's Preview: in the results' column in every state, so
    // nothing moves as the search ends (with no rows it went to the
    // content's right inset, 109 px).
    QQuickItem *currentPreview = itemNamed(currentRow, QStringLiteral("itunesArtworkCurrentPreviewButton"));
    QQuickItem *close = itemNamed(qobject_cast<QQuickWindow *>(dialog)->contentItem(),
                                  QStringLiteral("itunesArtworkCloseButton"));
    QVERIFY(currentPreview != nullptr && close != nullptr && close->isVisible());

    checkState(State::Searching, QStringLiteral("Searching iTunes…"));
    const qreal columnX = sceneRectOf(currentPreview).left();
    QVERIFY2(sceneRectOf(close).right() - sceneRectOf(currentPreview).right() > 50,
             qPrintable(describeRect(sceneRectOf(currentPreview))));

    bridge->setProperty("itunesArtworkLoading", false);
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("iTunes has no suggestions for this album."));
    checkState(State::NothingFound, QStringLiteral("iTunes has no suggestions for this album."));
    QCOMPARE(sceneRectOf(currentPreview).left(), columnX);

    // As BridgeClient tells a failed search (BridgeClientTest::
    // aFailedArtworkSearchSaysSoApartFromFindingNothing).
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("iTunes search failed: Connection refused"));
    bridge->setProperty("itunesArtworkErrorText", QStringLiteral("Connection refused"));
    bridge->setProperty("itunesArtworkFailed", true);
    checkState(State::Failed, QStringLiteral("Connection refused"));
    QCOMPARE(sceneRectOf(currentPreview).left(), columnX);

    // Searching again, in the column still; the rows then arrive under it.
    bridge->setProperty("itunesArtworkFailed", false);
    bridge->setProperty("itunesArtworkErrorText", QString());
    bridge->setProperty("itunesArtworkLoading", true);
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("Searching iTunes…"));
    QTRY_COMPARE(sceneRectOf(currentPreview).left(), columnX);
    bridge->setProperty("itunesArtworkResults", itunesArtworkFixture(dir, 2));
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("Found 2 suggestions."));
    bridge->setProperty("itunesArtworkLoading", false);
    QTRY_VERIFY(results->isVisible());
    QCOMPARE(sceneRectOf(currentPreview).left(), columnX);
    QQuickItem *firstPreview = nullptr;
    QTRY_VERIFY((firstPreview = itemNamed(results, QStringLiteral("itunesArtworkPreviewButton"))) != nullptr);
    QTRY_COMPARE(sceneRectOf(firstPreview).left(), columnX);
}

void AlbumArtSmokeTest::itunesArtworkHoldsStillBetweenItsStates_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// Replace from iTunes holds still as a search ends: the current art's
// Preview at one x while searching, with nothing found, when it failed and
// over two results or seven, which scroll (it moved 109 px), and the first
// line of the placeholder on one baseline, under a slot for the state's
// glyph (nothing found was 22 px above searching).
void AlbumArtSmokeTest::itunesArtworkHoldsStillBetweenItsStates() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(
        createItunesArtworkDialogWindow(engine, QUrl::fromLocalFile(savedCover(dir)).toString(), &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    QVERIFY(QTest::qWaitForWindowExposed(qobject_cast<QQuickWindow *>(root.get())));
    QObject *dialog = qvariant_cast<QObject *>(root->property("dialog"));
    QObject *bridge = qvariant_cast<QObject *>(root->property("bridge"));
    QVERIFY(dialog != nullptr && bridge != nullptr);
    bridge->setProperty("itunesArtworkLoading", true);
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("Searching iTunes…"));
    QVERIFY(QMetaObject::invokeMethod(dialog, "open"));
    auto *dialogWindow = qobject_cast<QQuickWindow *>(dialog);
    QVERIFY(QTest::qWaitForWindowExposed(dialogWindow));
    QQuickItem *preview = itemNamed(dialogWindow->contentItem(), QStringLiteral("itunesArtworkCurrentPreviewButton"));
    QQuickItem *message = itemNamed(dialogWindow->contentItem(), QStringLiteral("itunesArtworkPlaceholderMessage"));
    QQuickItem *heading = itemNamed(dialogWindow->contentItem(), QStringLiteral("itunesArtworkPlaceholderHeading"));
    QQuickItem *results = itemNamed(dialogWindow->contentItem(), QStringLiteral("itunesArtworkResultsView"));
    QVERIFY(preview != nullptr && message != nullptr && heading != nullptr && results != nullptr);
    const auto baselineOf = [](QQuickItem *label) { return sceneRectOf(label).top() + label->baselineOffset(); };
    QTRY_VERIFY(message->isVisible() && message->width() > 0 && popupsSettled());
    QVERIFY(finishIncubation(engine));
    QVERIFY(dialogWindow->grabWindow().width() > 0);
    const qreal previewX = sceneRectOf(preview).left();
    const qreal baseline = baselineOf(message);
    // The glyph's slot: 48 px at 10 pt, scaled with a larger font, in every
    // state.
    QQuickItem *slot = itemNamed(dialogWindow->contentItem(), QStringLiteral("itunesArtworkGlyphSlot"));
    QVERIFY(slot != nullptr);
    const QSizeF slotSize(std::round(48 * std::max(1.0, pointSize / 10)), std::round(48 * std::max(1.0, pointSize / 10)));
    QCOMPARE(slot->size(), slotSize);
    const qreal slotY = sceneRectOf(slot).top();
    // The dialog's lead in each of its states, as the other dialogs' first
    // line is (it had the weight of every result's title).
    QQuickItem *lead = itemNamed(dialogWindow->contentItem(), QStringLiteral("itunesArtworkCurrentTitle"));
    QVERIFY(lead != nullptr);
    const auto weightOf = [](QQuickItem *label) { return label->property("font").value<QFont>().weight(); };
    QCOMPARE(weightOf(lead), QFont::DemiBold);

    // Finding nothing's heading, and a failure's, take searching's line.
    bridge->setProperty("itunesArtworkLoading", false);
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("iTunes has no suggestions for this album."));
    QTRY_VERIFY(heading->isVisible());
    QVERIFY(dialogWindow->grabWindow().width() > 0);
    QCOMPARE(sceneRectOf(preview).left(), previewX);
    QTRY_VERIFY2(qAbs(baselineOf(heading) - baseline) <= 1.0,
                 qPrintable(QStringLiteral("heading %1, searching %2").arg(baselineOf(heading)).arg(baseline)));
    QCOMPARE(slot->size(), slotSize);
    QCOMPARE(sceneRectOf(slot).top(), slotY);
    QCOMPARE(weightOf(lead), QFont::DemiBold);

    bridge->setProperty("itunesArtworkErrorText", QStringLiteral("Connection refused"));
    bridge->setProperty("itunesArtworkFailed", true);
    QTRY_VERIFY(heading->isVisible());
    QVERIFY(dialogWindow->grabWindow().width() > 0);
    QCOMPARE(sceneRectOf(preview).left(), previewX);
    QTRY_VERIFY2(qAbs(baselineOf(heading) - baseline) <= 1.0,
                 qPrintable(QStringLiteral("heading %1, searching %2").arg(baselineOf(heading)).arg(baseline)));
    QCOMPARE(slot->size(), slotSize);
    QCOMPARE(sceneRectOf(slot).top(), slotY);

    QCOMPARE(weightOf(lead), QFont::DemiBold);
    bridge->setProperty("itunesArtworkFailed", false);
    bridge->setProperty("itunesArtworkErrorText", QString());
    for (const int count : {2, 7}) {
        bridge->setProperty("itunesArtworkResults", itunesArtworkFixture(dir, count));
        QTRY_COMPARE(results->property("count").toInt(), count);
        QVERIFY(finishIncubation(engine));
        QTRY_VERIFY(!visibleItemsNamed(results, QStringLiteral("itunesArtworkResultRow")).isEmpty());
        QVERIFY(dialogWindow->grabWindow().width() > 0);
        if (count == 7) {
            QTRY_VERIFY(results->property("contentHeight").toReal() > results->height() + 0.5);
        }
        QCOMPARE(sceneRectOf(preview).left(), previewX);
        for (QQuickItem *row : visibleItemsNamed(results, QStringLiteral("itunesArtworkResultRow"))) {
            QQuickItem *rowPreview = itemNamed(row, QStringLiteral("itunesArtworkPreviewButton"));
            QQuickItem *apply = itemNamed(row, QStringLiteral("itunesArtworkApplyButton"));
            QVERIFY(rowPreview != nullptr && apply != nullptr);
            QTRY_COMPARE(sceneRectOf(rowPreview).left(), previewX);
            QCOMPARE(weightOf(lead), QFont::DemiBold);
            // What is offered is regular.
            QQuickItem *offered = itemNamed(row, QStringLiteral("itunesArtworkResultTitle"));
            QVERIFY(offered != nullptr);
            QCOMPARE(weightOf(offered), QFont::Normal);
            // Clear of the scroll bar's 8 px lane.
            QVERIFY(sceneRectOf(apply).right() <= sceneRectOf(results).right() - 8);
        }
    }
}

namespace {

// Replace from iTunes, open with `count` suggestions that are ready.
struct OpenItunesArtworkDialog {
    QScopedPointer<QObject> root;
    QObject *dialog = nullptr;
    QQuickWindow *dialogWindow = nullptr;
    QQuickItem *content = nullptr;
};

bool openItunesArtworkDialog(QQmlApplicationEngine &engine, const QTemporaryDir &dir, int count,
                             const QFont *windowFont, OpenItunesArtworkDialog &out) {
    QString errorText;
    out.root.reset(createItunesArtworkDialogWindow(engine, QUrl::fromLocalFile(savedCover(dir)).toString(),
                                                   &errorText));
    if (out.root == nullptr) {
        qWarning() << errorText;
        return false;
    }
    auto *window = qobject_cast<QQuickWindow *>(out.root.get());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return false;
    }
    out.dialog = qvariant_cast<QObject *>(out.root->property("dialog"));
    QObject *bridge = qvariant_cast<QObject *>(out.root->property("bridge"));
    if (out.dialog == nullptr || bridge == nullptr) {
        return false;
    }
    if (windowFont != nullptr) {
        out.dialog->setProperty("font", *windowFont);
    }
    bridge->setProperty("itunesArtworkResults", itunesArtworkFixture(dir, count));
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("Found %1 suggestions.").arg(count));
    if (!QMetaObject::invokeMethod(out.dialog, "open")) {
        return false;
    }
    out.dialogWindow = qobject_cast<QQuickWindow *>(out.dialog);
    if (out.dialogWindow == nullptr || !QTest::qWaitForWindowExposed(out.dialogWindow)) {
        return false;
    }
    out.content = itemNamed(out.dialogWindow->contentItem(), QStringLiteral("itunesArtworkContent"));
    // A row is in the list before it is whole, and the list builds the rows
    // the hidden dialog had no room for in the background.
    return out.content != nullptr
        && QTest::qWaitFor([&]() {
               return visibleItemsNamed(out.content, QStringLiteral("itunesArtworkResultRow")).size()
                   >= qMin(count, 5);
           })
        && finishIncubation(engine);
}

int fontPixelSize(QQuickItem *item) {
    return QFontInfo(qvariant_cast<QFont>(item->property("font"))).pixelSize();
}

// Builds what the engine builds in the background by its smallest step at
// each turn of the event loop, as on a machine too busy to give it its
// share of a frame: whatever a test waits for meanwhile is seen half made.
class SlowIncubationController final : public QObject, public QQmlIncubationController {
public:
    SlowIncubationController() {
        m_step.setInterval(0);
        connect(&m_step, &QTimer::timeout, this, [this]() {
            std::atomic<bool> more{false};
            incubateWhile(&more);
            if (incubatingObjectCount() == 0) {
                m_step.stop();
            }
        });
    }

protected:
    void incubatingObjectCountChanged(int count) override {
        if (count > 0 && !m_step.isActive()) {
            m_step.start();
        }
    }

private:
    QTimer m_step;
};

} // namespace

// The line over the results that says how many there are is one line in
// the dialog at 24 pt, as wide as it opens on a 1600 px screen ("it."
// wrapped alone onto a second line).
void AlbumArtSmokeTest::itunesReplaceStatesTheOutcomeThatApplies_data() {
    QTest::addColumn<qreal>("pointSize");
    // The cover's file name, whether the backend says it has a cover's name
    // (invalid: not said), and whether its details say it is embedded art
    // (invalid: no details yet).
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QVariant>("named");
    QTest::addColumn<QVariant>("embedded");
    // Whether the suggestions are for a track playback has moved on from,
    // whose word on the name the search kept.
    QTest::addColumn<bool>("movedOn");
    // What the question says, or empty for both cases.
    QTest::addColumn<QString>("outcome");
    QTest::addColumn<bool>("warns");
    const QString embed = QStringLiteral("Ferrous writes the new art into this album’s tracks in this folder.");
    const QString overwrite = QStringLiteral("Ferrous overwrites cover.png, this folder’s cover image.");
    const QString add = QStringLiteral("Ferrous adds a cover file beside back.png, which stays.");
    for (const qreal pointSize : {10.0, 24.0}) {
        const QByteArray at = QByteArray::number(pointSize) + " pt";
        QTest::newRow(("embedded art, " + at).constData())
            << pointSize << QStringLiteral("0123456789abcdef0123456789abcdef.png") << QVariant(false)
            << QVariant(true) << false << embed << true;
        QTest::newRow(("folder cover, " + at).constData())
            << pointSize << QStringLiteral("cover.png") << QVariant(true) << QVariant(false) << false << overwrite
            << true;
        QTest::newRow(("other folder image, " + at).constData())
            << pointSize << QStringLiteral("back.png") << QVariant(false) << QVariant(false) << false << add << false;
        QTest::newRow(("not known yet, " + at).constData())
            << pointSize << QStringLiteral("cover.png") << QVariant() << QVariant(false) << false << QString()
            << true;
    }
    // A cover's name settles it before its details come.
    QTest::newRow("folder cover, no details")
        << 10.0 << QStringLiteral("cover.png") << QVariant(true) << QVariant() << false << overwrite << true;
    // Not a cover's name, without details: embedded art or another image.
    QTest::newRow("other name, no details")
        << 10.0 << QStringLiteral("back.png") << QVariant(false) << QVariant() << false << QString() << true;
    QTest::newRow("folder cover, moved on")
        << 10.0 << QStringLiteral("cover.png") << QVariant(true) << QVariant(false) << true << overwrite << true;
}

// Replace Album Art states the one outcome that applies, as the backend
// applies the art (artwork::apply_artwork_to_track): it embeds the art when
// the shown art is embedded, overwrites a folder image with a cover's name,
// and adds a cover file beside another folder image. One paragraph in the
// secondary ink under the question, the only semibold line, and the loss
// line, also secondary, only when a file is overwritten. When the dialog
// can't tell, both cases as regular conditions, 12 px apart at 10 pt, with
// the loss line (two semibold conditions outranked the question and made the
// reader work out the case; a loss line in the text's colour outranked the
// outcome above it).
void AlbumArtSmokeTest::itunesReplaceStatesTheOutcomeThatApplies() {
    QFETCH(qreal, pointSize);
    QFETCH(QString, fileName);
    QFETCH(QVariant, named);
    QFETCH(QVariant, embedded);
    QFETCH(bool, movedOn);
    QFETCH(QString, outcome);
    QFETCH(bool, warns);
    const ScopedApplicationFont font(pointSize);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(fileName);
    QVERIFY(QFile::rename(savedCover(dir), coverPath) || coverPath == dir.filePath(QStringLiteral("cover.png")));
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    const QVariantList rows = itunesArtworkFixture(dir, 2);
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QVariantMap target{
        {QStringLiteral("trackPath"), movedOn ? QStringLiteral("/music/Elsewhere/01 Lantern Rows.flac")
                                              : QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("artist"), QStringLiteral("The Quiet Meridian")},
        {QStringLiteral("coverPath"), coverUrl},
    };
    if (movedOn) {
        // The playing track's own word is about another cover.
        target.insert(QStringLiteral("coverNamed"), named);
        ui.fake->setProperty("currentTrackCoverNamed", QVariant(!named.toBool()));
    } else if (named.isValid()) {
        ui.fake->setProperty("currentTrackCoverNamed", named);
    }
    if (embedded.isValid()) {
        ui.fake->setProperty("coverDetails", QVariantMap{
            {QStringLiteral("path"), coverPath},
            {QStringLiteral("fileName"), fileName},
            {QStringLiteral("embeddedCover"), embedded},
            {QStringLiteral("width"), 120},
            {QStringLiteral("height"), 120},
        });
    }
    QVERIFY(openItunesArtworkDialog(ui.window, ui.dialog, ui.fake, rows, target));
    QQuickWindow *question = replaceQuestion(ui.window);
    QVERIFY(question != nullptr);
    QObject *palette = qvariant_cast<QObject *>(ui.dialog->property("uiPalette"));
    QVERIFY(palette != nullptr);
    QVERIFY(QMetaObject::invokeMethod(ui.dialog, "requestSuggestionApply", Q_ARG(QVariant, 0)));
    QTRY_VERIFY(question->property("opened").toBool());
    QVERIFY(QTest::qWaitForWindowExposed(question));

    const QRgb ink = palette->property("text").value<QColor>().rgba();
    const QRgb secondary = palette->property("textSecondary").value<QColor>().rgba();
    QQuickItem *message = visibleItemNamed(question, QStringLiteral("confirmDialogMessage"));
    QQuickItem *chosen = visibleItemNamed(question, QStringLiteral("replaceCoverChosen"));
    QQuickItem *paragraph = visibleItemNamed(question, QStringLiteral("replaceCoverOutcome"));
    QQuickItem *cases = visibleItemNamed(question, QStringLiteral("replaceCoverConsequence"));
    QQuickItem *warning = visibleItemNamed(question, QStringLiteral("replaceCoverWarning"));
    QQuickItem *cancel = visibleItemNamed(question, QStringLiteral("confirmDialogCancelButton"));
    QVERIFY(message != nullptr && chosen != nullptr && cancel != nullptr);
    QCOMPARE(message->property("font").value<QFont>().weight(), QFont::DemiBold);
    QCOMPARE(warning != nullptr, warns);
    QList<QQuickItem *> lines;
    if (!outcome.isEmpty()) {
        QVERIFY(paragraph != nullptr);
        QVERIFY(cases == nullptr);
        QCOMPARE(paragraph->property("text").toString(), outcome);
        QCOMPARE(paragraph->property("color").value<QColor>().rgba(), secondary);
        lines.append(paragraph);
    } else {
        QVERIFY(paragraph == nullptr);
        QVERIFY(cases != nullptr);
        const std::array<std::pair<QString, QString>, 2> conditions{{
            {QStringLiteral("If the folder has a JPEG or PNG image"),
             QStringLiteral("Ferrous overwrites its cover file, such as cover.jpg, or adds one beside its images.")},
            {QStringLiteral("If it has none"),
             QStringLiteral("Ferrous writes the new art into this album’s tracks in this folder.")},
        }};
        const std::array<QString, 2> names{QStringLiteral("replaceCoverFolderCase"),
                                           QStringLiteral("replaceCoverEmbedCase")};
        QQuickItem *previous = nullptr;
        for (std::size_t i = 0; i < conditions.size(); ++i) {
            QQuickItem *lead = visibleItemNamed(question, names[i] + QStringLiteral("Lead"));
            QQuickItem *result = visibleItemNamed(question, names[i] + QStringLiteral("Outcome"));
            QVERIFY2(lead != nullptr && result != nullptr, qPrintable(names[i]));
            QCOMPARE(lead->property("text").toString(), conditions[i].first);
            QCOMPARE(result->property("text").toString(), conditions[i].second);
            QCOMPARE(lead->property("color").value<QColor>().rgba(), ink);
            QCOMPARE(result->property("color").value<QColor>().rgba(), secondary);
            QTRY_VERIFY(sceneRectOf(result).top() >= sceneRectOf(lead).bottom());
            if (previous != nullptr) {
                // 12 px between the cases at 10 pt, more with a larger font.
                const qreal gap = sceneRectOf(lead).top() - sceneRectOf(previous).bottom();
                QCOMPARE(gap, std::round(12 * std::max(1.0, pointSize / 10)));
            }
            previous = result;
            lines << lead << result;
        }
    }
    if (warning != nullptr) {
        QCOMPARE(warning->property("text").toString(), QStringLiteral("Ferrous keeps no copy of what it overwrites."));
        // Secondary, as the other confirmations' loss lines.
        QCOMPARE(warning->property("color").value<QColor>().rgba(), secondary);
        QTRY_VERIFY(sceneRectOf(warning).top() > sceneRectOf(lines.last()).bottom());
        lines.append(warning);
    }
    // The question is the only semibold line; all of it is whole, plain
    // text, on one left edge, under what was chosen and over the buttons.
    for (QQuickItem *line : lines) {
        QCOMPARE(line->property("font").value<QFont>().weight(), QFont::Normal);
        QCOMPARE(line->property("textFormat").toInt(), int(Qt::PlainText));
        QTRY_VERIFY2(!line->property("truncated").toBool() && sceneRectOf(line).right() <= question->width(),
                     qPrintable(line->objectName()));
        QCOMPARE(sceneRectOf(line).left(), sceneRectOf(message).left());
    }
    QTRY_VERIFY(sceneRectOf(lines.first()).top() > sceneRectOf(chosen).bottom());
    QTRY_VERIFY(sceneRectOf(lines.last()).bottom() <= sceneRectOf(cancel).top());
    QVERIFY(sceneRectOf(cancel).bottom() <= question->height());
    // It fits a 1000 px screen whole, with room for the window's frame.
    QVERIFY2(question->property("fittedHeight").toInt() <= 1000 - 2 * 18 - 28,
             qPrintable(QString::number(question->property("fittedHeight").toInt())));
    // Display only: nothing is written until Replace.
    QVERIFY(ui.applied().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(question, "reject"));
    QTRY_VERIFY(!question->isVisible());
}

void AlbumArtSmokeTest::itunesArtworkCountLineHoldsAtLargeFonts() {
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont large(QStringLiteral("Noto Sans"));
    large.setPointSizeF(24);
    QGuiApplication::setFont(large);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QQmlApplicationEngine engine;
    OpenItunesArtworkDialog opened;
    QVERIFY(openItunesArtworkDialog(engine, dir, 5, nullptr, opened));
    QObject *bridge = qvariant_cast<QObject *>(opened.root->property("bridge"));
    QVERIFY(bridge != nullptr);
    // BridgeClient's words for five (itunesArtworkFoundTextIsPlainWords).
    bridge->setProperty("itunesArtworkStatusText",
                        QStringLiteral("Found 5 suggestions. Full-size images download when you preview or apply one."));
    // The width it opens at with a 24 pt font: its most, 1280 px, and as
    // much as its inset has grown on each side.
    const qreal gridUnit = evaluateOn(opened.content, QStringLiteral("Kirigami.Units.gridUnit")).toReal();
    QVERIFY(gridUnit > 0);
    const int widest = qRound(1280 + 2 * (opened.dialog->property("contentPadding").toReal() - gridUnit));
    QVERIFY2(widest > 1280, qPrintable(QString::number(widest)));
    QCOMPARE(opened.dialog->property("defaultWidth").toReal(), qreal(widest));
    opened.dialogWindow->resize(widest, opened.dialogWindow->height());
    QTRY_COMPARE(opened.dialogWindow->width(), widest);
    QQuickItem *heading = itemNamed(opened.content, QStringLiteral("itunesArtworkResultsHeading"));
    QVERIFY(heading != nullptr);
    QTRY_VERIFY(heading->isVisible());
    QTRY_COMPARE(heading->property("lineCount").toInt(), 1);
}

// The dialog counts as open with its results only once its rows are whole,
// however slowly the engine builds them. The hidden dialog's list builds
// the rows it has no room for yet in the background, and a row is in the
// list, with its name, before its bindings and buttons are done: a test
// that ended then destroyed a row half made, and Qt still counted "2" items
// in creation as the engine went (#627).
void AlbumArtSmokeTest::itunesArtworkDialogOpensWithItsRowsWhole() {
    const ScopedApplicationFont font(24);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QQmlApplicationEngine engine;
    SlowIncubationController incubation;
    engine.setIncubationController(&incubation);
    OpenItunesArtworkDialog opened;
    QVERIFY(openItunesArtworkDialog(engine, dir, 5, nullptr, opened));
    QCOMPARE(incubation.incubatingObjectCount(), 0);
}

void AlbumArtSmokeTest::itunesArtworkActionsFollowTheTextFont() {
    // At 24 pt, a row's Preview and Apply, and the current art's Preview,
    // are drawn in the font of the titles beside them. They were
    // ToolButtons, whose caption the style draws in the application's
    // QToolButton font, the toolbar font that KDE sets apart, whatever the
    // control's own font: at 24 pt they stayed at 10 pt beside 24 pt titles.
    // A 10 pt QToolButton font stands in for KDE's toolbar font here. As
    // drawn captions cannot be read back, each action must be as wide as its
    // icon and caption in the labels' font need, which the style measures in
    // the font it draws with. Their column is built from the buttons' own
    // sizes: 8 px between them, and the text at least 16 px clear of them.
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QFont large = applicationFont;
    large.setPointSizeF(24);
    QGuiApplication::setFont(large);
    QFont toolbar = applicationFont;
    toolbar.setPointSizeF(10);
    QApplication::setFont(toolbar, "QToolButton");
    // QApplication's setFont() without a class drops the class fonts.
    const auto restoreToolbarFont = qScopeGuard([&] { QApplication::setFont(applicationFont); });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QQmlApplicationEngine engine;
    OpenItunesArtworkDialog opened;
    QVERIFY(openItunesArtworkDialog(engine, dir, 2, nullptr, opened));
    QTRY_VERIFY(popupsSettled());
    // At its opening width, which the offscreen screen is too narrow for
    // at 24 pt.
    opened.dialogWindow->setWidth(qCeil(opened.dialog->property("defaultWidth").toReal()));
    QTRY_COMPARE(opened.content->width(),
                 opened.dialogWindow->width() - 2 * opened.dialog->property("contentPadding").toReal());
    // Wide enough for its icon and its caption in the labels' font.
    const auto fitsCaption = [](QQuickItem *button, QQuickItem *label) {
        const QFontMetricsF metrics(qvariant_cast<QFont>(label->property("font")));
        return button->implicitWidth() >= metrics.horizontalAdvance(button->property("text").toString()) + 16;
    };
    QQuickItem *currentRow = itemNamed(opened.content, QStringLiteral("itunesArtworkCurrentRow"));
    QQuickItem *currentTitle = itemNamed(currentRow, QStringLiteral("itunesArtworkCurrentTitle"));
    QQuickItem *currentPreview = itemNamed(currentRow, QStringLiteral("itunesArtworkCurrentPreviewButton"));
    QVERIFY(currentTitle != nullptr && currentPreview != nullptr);
    QVERIFY2(fitsCaption(currentPreview, currentTitle), qPrintable(QString::number(currentPreview->implicitWidth())));
    const int textPixels = fontPixelSize(currentTitle);
    QVERIFY2(textPixels >= 30, qPrintable(QString::number(textPixels)));
    QVERIFY2(qAbs(fontPixelSize(currentPreview) - textPixels) <= 1,
             qPrintable(QStringLiteral("Preview %1 px, text %2 px").arg(fontPixelSize(currentPreview)).arg(textPixels)));

    // Each row's text ends at least 16 px before its actions, which are as
    // wide as their text needs and 8 px apart, at the row's end.
    const QList<QQuickItem *> rows = visibleItemsNamed(opened.content, QStringLiteral("itunesArtworkResultRow"));
    QCOMPARE(rows.size(), 2);
    for (QQuickItem *row : rows) {
        QQuickItem *title = itemNamed(row, QStringLiteral("itunesArtworkResultTitle"));
        QQuickItem *details = itemNamed(row, QStringLiteral("itunesArtworkResultDetails"));
        QQuickItem *preview = itemNamed(row, QStringLiteral("itunesArtworkPreviewButton"));
        QQuickItem *apply = itemNamed(row, QStringLiteral("itunesArtworkApplyButton"));
        QVERIFY(title != nullptr && details != nullptr && preview != nullptr && apply != nullptr);
        const QString at = QStringLiteral("title %1 px, Preview %2 px, Apply %3 px; "
                                          "title %4, details %5, Preview %6, Apply %7")
            .arg(fontPixelSize(title)).arg(fontPixelSize(preview)).arg(fontPixelSize(apply))
            .arg(describeRect(sceneRectOf(title)), describeRect(sceneRectOf(details)),
                 describeRect(sceneRectOf(preview)), describeRect(sceneRectOf(apply)));
        QVERIFY2(fitsCaption(preview, title) && fitsCaption(apply, title),
                 qPrintable(at + QStringLiteral("; implicit widths %1, %2")
                                     .arg(preview->implicitWidth()).arg(apply->implicitWidth())));
        QCOMPARE(fontPixelSize(title), textPixels);
        QVERIFY2(qAbs(fontPixelSize(preview) - textPixels) <= 1, qPrintable(at));
        QVERIFY2(qAbs(fontPixelSize(apply) - textPixels) <= 1, qPrintable(at));
        QVERIFY2(qFuzzyCompare(preview->width(), preview->implicitWidth())
                     && qFuzzyCompare(apply->width(), apply->implicitWidth()), qPrintable(at));
        QVERIFY2(!title->property("truncated").toBool(), qPrintable(at));
        QVERIFY2(sceneRectOf(title).right() + 16 <= sceneRectOf(preview).left() + 0.5, qPrintable(at));
        QVERIFY2(sceneRectOf(details).right() + 16 <= sceneRectOf(preview).left() + 0.5, qPrintable(at));
        QCOMPARE(sceneRectOf(apply).left() - sceneRectOf(preview).right(), 8.0);
        QVERIFY2(sceneRectOf(row).right() - sceneRectOf(apply).right() <= 16.5, qPrintable(at));
        // The row is as tall as the buttons need, and the current art's
        // row the same, its Preview over the results'.
        QVERIFY2(row->height() >= apply->height() + 16, qPrintable(at));
        QCOMPARE(currentRow->height(), row->height());
        QCOMPARE(sceneRectOf(currentPreview).left(), sceneRectOf(preview).left());
    }
    QVERIFY2(sceneRectOf(currentTitle).right() + 16 <= sceneRectOf(currentPreview).left() + 0.5,
             qPrintable(describeRect(sceneRectOf(currentTitle))));
}

void AlbumArtSmokeTest::itunesArtworkListIsAsTallAsItsRows() {
    // The results' frame ends at the last row, with no empty framed band
    // under a few results (it left about 110 px), and the dialog opens tall
    // enough for the current art and five results whole; more scroll.
    const ScopedApplicationFont font(10);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (const int count : {2, 5, 7}) {
        const QByteArray at = QByteArray::number(count) + " results";
        QQmlApplicationEngine engine;
        OpenItunesArtworkDialog opened;
        QVERIFY2(openItunesArtworkDialog(engine, dir, count, nullptr, opened), at.constData());
        QQuickItem *frame = itemNamed(opened.content, QStringLiteral("itunesArtworkResultsFrame"));
        QQuickItem *view = itemNamed(opened.content, QStringLiteral("itunesArtworkResultsView"));
        QVERIFY2(frame != nullptr && view != nullptr, at.constData());
        const qreal rowHeight = opened.dialog->property("rowHeight").toReal();
        QCOMPARE(rowHeight, 80.0);
        for (QQuickItem *row : visibleItemsNamed(opened.content, QStringLiteral("itunesArtworkResultRow"))) {
            QCOMPARE(row->height(), rowHeight);
        }
        const int shown = qMin(count, 5);
        const QString where = QStringLiteral("%1: frame %2, view %3, content %4, window %5")
            .arg(QString::fromLatin1(at), describeRect(sceneRectOf(frame)), describeRect(sceneRectOf(view)),
                 describeRect(sceneRectOf(opened.content))).arg(opened.dialogWindow->height());
        // The rows inside the frame's 1 px edge, and the frame no taller.
        QTRY_VERIFY2(qFuzzyCompare(view->height(), shown * rowHeight), qPrintable(where));
        QCOMPARE(frame->height(), shown * rowHeight + 2);
        // Five rows fill the dialog's height to its content's end; two
        // leave the dialog's own colour under the frame; more scroll.
        if (count == 5) {
            QVERIFY2(qAbs(sceneRectOf(frame).bottom() - sceneRectOf(opened.content).bottom()) <= 1.0,
                     qPrintable(where));
        } else if (count == 2) {
            QVERIFY2(sceneRectOf(opened.content).bottom() - sceneRectOf(frame).bottom() >= 3 * rowHeight - 1,
                     qPrintable(where));
        }
        QTRY_COMPARE(view->property("contentHeight").toReal() > view->height() + 0.5, count > 5);
    }
}

void AlbumArtSmokeTest::itunesArtworkScalesAsAWholeWithTheFont_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// The dialog scales as a whole with the font: the thumbnails, the current
// art's too, stay 64 px at 10 pt and grow to half as tall again as a
// row's title and details with a larger font; the actions' glyphs are
// Kirigami's size for labels, 16 px at 10 pt; and the dialog opens
// 920 px wide at 10 pt and no wider than 1280 px with a larger font, and
// its inset's growth on each side.
// At 24 pt, 64 px thumbnails stood under 92 px of text, 16 px glyphs
// beside 32 px captions, in a 1964 px window.
void AlbumArtSmokeTest::itunesArtworkScalesAsAWholeWithTheFont() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QQmlApplicationEngine engine;
    OpenItunesArtworkDialog opened;
    QVERIFY(openItunesArtworkDialog(engine, dir, 2, nullptr, opened));
    QTRY_VERIFY(popupsSettled());
    dropSizeHintMessages();
    const bool large = pointSize > 10;

    // Kirigami's size for labels, which the dialog's actions take.
    const qreal forLabels = opened.dialog->property("actionIconSize").toReal();
    if (large) {
        QVERIFY2(forLabels >= 32, qPrintable(QString::number(forLabels)));
    } else {
        QCOMPARE(forLabels, 16.0);
    }

    const qreal defaultWidth = opened.dialog->property("defaultWidth").toReal();
    if (large) {
        const qreal gridUnit = evaluateOn(opened.content, QStringLiteral("Kirigami.Units.gridUnit")).toReal();
        QVERIFY(gridUnit > 0);
        QCOMPARE(defaultWidth, 1280 + 2 * (opened.dialog->property("contentPadding").toReal() - gridUnit));
    } else {
        QCOMPARE(defaultWidth, 920.0);
    }

    // At its opening width, which the offscreen screen is too narrow for
    // at 24 pt.
    opened.dialogWindow->setWidth(qCeil(defaultWidth));
    QTRY_COMPARE(opened.content->width(), opened.dialogWindow->width() - 2 * opened.dialog->property("contentPadding").toReal());
    QQuickItem *currentRow = itemNamed(opened.content, QStringLiteral("itunesArtworkCurrentRow"));
    QVERIFY(currentRow != nullptr);
    QList<QQuickItem *> rows = visibleItemsNamed(opened.content, QStringLiteral("itunesArtworkResultRow"));
    QCOMPARE(rows.size(), 2);
    rows.prepend(currentRow);
    const qreal thumbnailSize = opened.dialog->property("thumbnailSize").toReal();
    for (QQuickItem *row : rows) {
        const bool current = row == currentRow;
        QQuickItem *thumbnail = itemNamed(row, current ? QStringLiteral("itunesArtworkCurrentThumbnail")
                                                       : QStringLiteral("itunesArtworkResultThumbnail"));
        QQuickItem *title = itemNamed(row, current ? QStringLiteral("itunesArtworkCurrentTitle")
                                                   : QStringLiteral("itunesArtworkResultTitle"));
        QQuickItem *details = itemNamed(row, current ? QStringLiteral("itunesArtworkCurrentDetails")
                                                     : QStringLiteral("itunesArtworkResultDetails"));
        QVERIFY(thumbnail != nullptr && title != nullptr && details != nullptr);
        const qreal textHeight = sceneRectOf(details).bottom() - sceneRectOf(title).top();
        const QString at = QStringLiteral("%1: thumbnail %2, title %3, details %4, window %5")
            .arg(row->objectName(), describeRect(sceneRectOf(thumbnail)), describeRect(sceneRectOf(title)),
                 describeRect(sceneRectOf(details)))
            .arg(opened.dialogWindow->width());
        QCOMPARE(thumbnail->width(), thumbnailSize);
        QCOMPARE(thumbnail->height(), thumbnailSize);
        if (large) {
            QVERIFY2(thumbnail->height() >= 1.5 * textHeight - 1, qPrintable(at));
        } else {
            QCOMPARE(thumbnail->height(), 64.0);
        }
        // The thumbnail and the text stay on one centre line, in the row.
        QVERIFY2(qAbs(sceneRectOf(thumbnail).center().y() - sceneRectOf(row).center().y()) <= 1.0, qPrintable(at));
        QVERIFY2(sceneRectOf(row).contains(sceneRectOf(thumbnail)), qPrintable(at));
        QVERIFY2(sceneRectOf(title).left() >= sceneRectOf(thumbnail).right() + 12 - 0.5, qPrintable(at));
        QVERIFY2(!title->property("truncated").toBool(), qPrintable(at));
    }
    if (!large) {
        QCOMPARE(opened.dialog->property("rowHeight").toReal(), 80.0);
    }

    // Every action's glyph is the size for labels, and the style draws
    // it so: the button is as wide as the glyph and its caption need.
    QList<QQuickItem *> actions{itemNamed(currentRow, QStringLiteral("itunesArtworkCurrentPreviewButton"))};
    for (QQuickItem *row : rows.mid(1)) {
        actions << itemNamed(row, QStringLiteral("itunesArtworkPreviewButton"))
                << itemNamed(row, QStringLiteral("itunesArtworkApplyButton"));
    }
    for (QQuickItem *action : actions) {
        QVERIFY(action != nullptr);
        QQmlExpression iconWidth(qmlContext(action), action, QStringLiteral("icon.width"));
        QQmlExpression iconHeight(qmlContext(action), action, QStringLiteral("icon.height"));
        QCOMPARE(iconWidth.evaluate().toReal(), forLabels);
        QCOMPARE(iconHeight.evaluate().toReal(), forLabels);
        const QFontMetricsF metrics(qvariant_cast<QFont>(action->property("font")));
        QVERIFY2(action->implicitWidth() >= metrics.horizontalAdvance(action->property("text").toString()) + forLabels,
                 qPrintable(QStringLiteral("%1: %2 px").arg(action->objectName()).arg(action->implicitWidth())));
    }

    // A result whose full-size image loads says so beside the
    // artist, whole: at 24 pt on two lines, inside the row, and on one
    // with the default font (it was elided at 24 pt).
    QObject *bridge = qvariant_cast<QObject *>(opened.root->property("bridge"));
    QVERIFY(bridge != nullptr);
    QVariantList loading = itunesArtworkFixture(dir, 2);
    QVariantMap first = loading.first().toMap();
    first.insert(QStringLiteral("assetReady"), false);
    first.insert(QStringLiteral("assetLoading"), true);
    loading.first() = first;
    bridge->setProperty("itunesArtworkResults", loading);
    QQuickItem *loadingRow = nullptr;
    QQuickItem *loadingDetails = nullptr;
    QTRY_VERIFY([&]() {
        for (QQuickItem *row : visibleItemsNamed(opened.content, QStringLiteral("itunesArtworkResultRow"))) {
            QQuickItem *details = itemNamed(row, QStringLiteral("itunesArtworkResultDetails"));
            if (details != nullptr && details->property("text").toString().contains(QStringLiteral("Loading"))) {
                loadingRow = row;
                loadingDetails = details;
                return true;
            }
        }
        return false;
    }());
    QCOMPARE(loadingDetails->property("text").toString(),
             QStringLiteral("The Quiet Meridian \u00b7 Loading full-size image\u2026"));
    const QString loadingAt = QStringLiteral("details %1 in the row %2, %3 lines")
        .arg(describeRect(sceneRectOf(loadingDetails)), describeRect(sceneRectOf(loadingRow)))
        .arg(loadingDetails->property("lineCount").toInt());
    QTRY_VERIFY2(!loadingDetails->property("truncated").toBool(), qPrintable(loadingAt));
    // On one line with the default font; at 24 pt on two where it needs
    // them ("Loading full-size image…" fits on one beside the artist).
    QVERIFY2(loadingDetails->property("lineCount").toInt() <= (large ? 2 : 1), qPrintable(loadingAt));
    QQuickItem *loadingTitle = itemNamed(loadingRow, QStringLiteral("itunesArtworkResultTitle"));
    QVERIFY(loadingTitle != nullptr);
    QVERIFY2(sceneRectOf(loadingTitle).top() >= sceneRectOf(loadingRow).top() + 8 - 0.5
                 && sceneRectOf(loadingDetails).bottom() <= sceneRectOf(loadingRow).bottom() - 8 + 0.5,
             qPrintable(loadingAt));

    // The search's glyph, the spinner or a failure's warning mark, is
    // 48 px with the default font and scaled with a larger one, as the
    // text under it (it was 32 px over 24 pt text, and 32 px was small for
    // the window's whole answer).
    const qreal fontScale = qvariant_cast<QObject *>(opened.dialog->property("uiPalette"))
        ->property("fontScale").toReal();
    const qreal stateGlyph = std::max(48.0, qreal(std::round(48 * fontScale)));
    if (!large) {
        QCOMPARE(stateGlyph, 48.0);
    }
    bridge->setProperty("itunesArtworkResults", QVariantList());
    bridge->setProperty("itunesArtworkLoading", true);
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("Searching iTunes\u2026"));
    QQuickItem *busy = itemNamed(opened.content, QStringLiteral("itunesArtworkBusyIndicator"));
    QVERIFY(busy != nullptr);
    QTRY_VERIFY(busy->isVisible());
    QTRY_COMPARE(busy->size(), QSizeF(stateGlyph, stateGlyph));
    bridge->setProperty("itunesArtworkLoading", false);
    bridge->setProperty("itunesArtworkErrorText", QStringLiteral("Connection refused"));
    bridge->setProperty("itunesArtworkFailed", true);
    QQuickItem *warning = itemNamed(opened.content, QStringLiteral("itunesArtworkFailureIcon"));
    QVERIFY(warning != nullptr);
    QTRY_VERIFY(warning->isVisible());
    QTRY_COMPARE(warning->size(), QSizeF(stateGlyph, stateGlyph));
}

void AlbumArtSmokeTest::itunesPreviewAppliesOrGoesBackToTheResults() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 12);
    QCOMPARE(fixture.size(), 12);
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickWindow *window = ui.window;
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    QObject *dialog = ui.dialog;
    QObject *fake = ui.fake;
    const QVariantMap target{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("artist"), QStringLiteral("The Quiet Meridian")},
        {QStringLiteral("coverPath"), coverUrl},
    };
    const int previewed = 7;
    const QString candidateUrl = fixture.at(previewed).toMap().value(QStringLiteral("normalizedUrl")).toString();
    const auto applied = [fake]() { return fake->property("appliedItunesArtwork").toList(); };
    const auto resultsView = [dialog]() {
        return dialog->findChild<QQuickItem *>(QStringLiteral("itunesArtworkResultsView"));
    };

    // The viewer over the whole screen first, as in
    // viewerKeysWorkFromEveryControl: under the offscreen platform, its window
    // made after the popup viewer was used does not take the focus.
    for (const int fullscreenMode : {1, 0}) {
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        for (const bool viewerOpenBefore : {false, true}) {
            const QString where = QStringLiteral("fullscreen %1, viewer open before %2")
                                      .arg(fullscreenMode).arg(viewerOpenBefore);
            const QByteArray at = where.toUtf8();
            // The window the viewer is in.
            const auto viewerHost = [&]() -> QQuickWindow * {
                return fullscreenMode == 0
                    ? window : viewerWindowNamed(ui.engine, QStringLiteral("albumArtFullscreenWindow"));
            };
            // The viewer's controls: the corner plate in the main window, or
            // the title strip over the whole screen.
            const QString prefix = fullscreenMode == 0 ? QStringLiteral("albumArtPopup")
                                                       : QStringLiteral("albumArtFullscreen");
            if (viewerOpenBefore) {
                QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
                QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
                QVERIFY2(QTest::qWaitForWindowActive(viewerHost()), at.constData());
            }
            // The dialog is over the viewer when that is open, in its window.
            QQuickWindow *dialogHost = viewerOpenBefore ? viewerHost() : window;
            QVERIFY2(openItunesArtworkDialog(window, dialog, fake, fixture, target), at.constData());
            QTRY_VERIFY2(resultsView() != nullptr && resultsView()->property("count").toInt() == 12,
                         at.constData());
            // A window of its own (#480): a dialog, modal, its title in
            // the title bar KWin draws and not in its content.
            auto *dialogWindow = qobject_cast<QQuickWindow *>(dialog);
            QCOMPARE(dialogWindow->transientParent(), dialogHost);
            QVERIFY2(dialogWindow->isTopLevel(), at.constData());
            QCOMPARE(dialogWindow->flags() & Qt::WindowType_Mask, Qt::Dialog);
            QCOMPARE(dialogWindow->modality(), Qt::ApplicationModal);
            QCOMPARE(dialogWindow->title(), QStringLiteral("Replace from iTunes"));
            QList<QQuickItem *> dialogItems;
            collectQuickItems(dialogWindow->contentItem(), &dialogItems);
            for (QQuickItem *item : std::as_const(dialogItems)) {
                QVERIFY2(!item->isVisible() || item->property("text").toString() != dialogWindow->title(),
                         at.constData());
            }
            resultsView()->setProperty("contentY", 400.0);
            QTRY_COMPARE(resultsView()->property("contentY").toReal(), 400.0);

            // Preview hides the dialog, keeping it, and shows the candidate
            // in the viewer, with its details and, at their foot, Back to
            // Results and Apply; the viewer's controls have none of them,
            // nor the Image Details button, and I keeps the details.
            const auto preview = [&]() {
                QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview",
                                                  Q_ARG(QVariant, previewed)));
                QTRY_VERIFY2(!dialog->property("visible").toBool(), at.constData());
                QVERIFY2(dialog->property("previewShown").toBool(), at.constData());
                QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
                QCOMPARE(viewer->property("albumArtViewerSource").toString(), candidateUrl);
                QVERIFY(!viewer->property("albumArtViewerShowsCurrentTrack").toBool());
                QQuickWindow *host = viewerHost();
                QVERIFY2(host != nullptr && QTest::qWaitForWindowActive(host), at.constData());
                QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton")) != nullptr,
                             at.constData());
                // The art has the keys once the dialog's window has hidden
                // and the viewer's is active, which the window system says
                // later: the hidden dialog, a window of the viewer's, makes
                // it say it is active before then.
                const QString viewFocus = fullscreenMode == 0 ? QStringLiteral("albumArtPopupViewFocus")
                                                              : QStringLiteral("albumArtFullscreenViewFocus");
                QTRY_VERIFY2(popupsSettled(), at.constData());
                QTRY_VERIFY2(host->activeFocusItem() != nullptr
                             && host->activeFocusItem()->objectName() == viewFocus,
                         qPrintable(QStringLiteral("%1: %2").arg(QString::fromUtf8(at),
                             host->activeFocusItem() != nullptr ? host->activeFocusItem()->objectName()
                                                                : QStringLiteral("none"))));
                QQuickItem *label = visibleItemNamed(host, QStringLiteral("albumArtPreviewLabel"));
                QVERIFY2(label != nullptr, at.constData());
                QCOMPARE(label->property("text").toString(), QStringLiteral("iTunes Artwork"));
                QCOMPARE(host->title(),
                         QStringLiteral("Morning Tide Field — iTunes Artwork — Ferrous Album Art"));
                QQuickItem *apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"));
                QVERIFY2(apply != nullptr, at.constData());
                QCOMPARE(apply->property("text").toString(), QStringLiteral("Apply iTunes Artwork"));
                QQuickItem *compare = visibleItemNamed(host, QStringLiteral("albumArtCompareButton"));
                QVERIFY2(compare != nullptr, at.constData());
                QCOMPARE(compare->property("text").toString(), QStringLiteral("Show Current Album Art"));
                QTRY_VERIFY2(apply->isEnabled(), at.constData());
                checkPreviewControlsInTheDetails(host, prefix, at);
                // I, the Image Details shortcut, keeps the details and with
                // them Back to Results and Apply, the preview's only buttons
                // (#578): it hid them, though its button was hidden.
                QTest::keyClick(host, Qt::Key_I);
                QCoreApplication::processEvents();
                QVERIFY2(viewer->property("albumArtInfoVisible").toBool(), at.constData());
                QVERIFY2(visibleItemNamed(host, QStringLiteral("albumArtInfoColumn")) != nullptr,
                         at.constData());
                checkPreviewControlsInTheDetails(host, prefix, at);
                QQuickItem *backAfterI = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"));
                QQuickItem *applyAfterI = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"));
                QVERIFY2(backAfterI != nullptr && backAfterI->isEnabled(), at.constData());
                QVERIFY2(applyAfterI != nullptr && applyAfterI->isEnabled(), at.constData());
                // The results stay as they were, hidden.
                QCOMPARE(resultsView()->property("count").toInt(), 12);
            };
            const auto backAtTheResults = [&]() {
                QTRY_VERIFY2(dialog->property("opened").toBool(), at.constData());
                QVERIFY(!dialog->property("previewShown").toBool());
                QCOMPARE(qobject_cast<QQuickWindow *>(dialog)->transientParent(), dialogHost);
                QCOMPARE(resultsView()->property("count").toInt(), 12);
                QTRY_COMPARE(resultsView()->property("contentY").toReal(), 400.0);
                QCOMPARE(fake->property("itunesArtworkResults").toList().size(), 12);
                // The viewer shows what it did before: nothing, or the art.
                QTRY_COMPARE(viewer->property("albumArtViewerOpen").toBool(), viewerOpenBefore);
                QVERIFY(!viewer->property("albumArtPreviewActive").toBool());
                if (viewerOpenBefore) {
                    QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
                    QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
                    QTRY_VERIFY2(viewerHost()->isVisible(), at.constData());
                    if (fullscreenMode == 0) {
                        QTRY_VERIFY2(window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"))
                                         ->property("visible").toBool(), at.constData());
                    }
                }
                QVERIFY(applied().isEmpty());
            };

            preview();
            // Comparing with the current art works in the preview, which can
            // still be applied from there.
            QQuickWindow *host = viewerHost();
            QTest::keyClick(host, Qt::Key_Left);
            QTRY_VERIFY2(viewer->property("albumArtViewerShowsCurrentTrack").toBool(), at.constData());
            QCOMPARE(visibleItemNamed(host, QStringLiteral("albumArtPreviewLabel"))->property("text").toString(),
                     QStringLiteral("Current Album Art"));
            // The primary button says what it installs, the same with the
            // current art on show ("Apply" read as applying that).
            QQuickItem *applyFromCurrent = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"));
            QVERIFY(applyFromCurrent->isEnabled());
            QCOMPARE(applyFromCurrent->property("text").toString(), QStringLiteral("Apply iTunes Artwork"));
            QCOMPARE(visibleItemNamed(host, QStringLiteral("albumArtCompareButton"))->property("text").toString(),
                     QStringLiteral("Show iTunes Artwork"));
            QTest::keyClick(host, Qt::Key_Right);
            QTRY_VERIFY2(!viewer->property("albumArtViewerShowsCurrentTrack").toBool(), at.constData());

            // Back to Results, clicked: the preview is in front of the dialog.
            QVERIFY2(clickPreviewButton(host, QStringLiteral("albumArtPreviewBackButton")), at.constData());
            backAtTheResults();

            // Escape goes back too; the popup viewer stays open for it, rather
            // than closing and opening again.
            preview();
            QSignalSpy popupClosed(window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup")),
                                   SIGNAL(closed()));
            QVERIFY(popupClosed.isValid());
            QTest::keyClick(viewerHost(), Qt::Key_Escape);
            backAtTheResults();
            if (viewerOpenBefore) {
                QCOMPARE(popupClosed.count(), 0);
            }

            // Apply asks first, as the album has art, over the viewer; Cancel
            // stays in the preview, and Replace applies, closes the dialog
            // and leaves the viewer showing the cover.
            preview();
            host = viewerHost();
            QVERIFY2(clickPreviewButton(host, QStringLiteral("albumArtPreviewApplyButton")), at.constData());
            QTRY_VERIFY2(replaceQuestionButton(window, host, QStringLiteral("confirmDialogConfirmButton")) != nullptr,
                         at.constData());
            QVERIFY(applied().isEmpty());
            // The question's buttons are clicked once it has faded in.
            QTRY_VERIFY2(popupsSettled(), at.constData());
            QVERIFY2(clickVisibleButton(replaceQuestion(window), QStringLiteral("confirmDialogCancelButton")), at.constData());
            QTRY_VERIFY2(replaceQuestionButton(window, host, QStringLiteral("confirmDialogConfirmButton")) == nullptr,
                         at.constData());
            QVERIFY(dialog->property("previewShown").toBool());
            QVERIFY(viewer->property("albumArtPreviewActive").toBool());
            QVERIFY2(clickPreviewButton(host, QStringLiteral("albumArtPreviewApplyButton")), at.constData());
            QTRY_VERIFY2(replaceQuestionButton(window, host, QStringLiteral("confirmDialogConfirmButton")) != nullptr,
                         at.constData());
            QTRY_VERIFY2(popupsSettled(), at.constData());
            QVERIFY2(clickVisibleButton(replaceQuestion(window), QStringLiteral("confirmDialogConfirmButton")), at.constData());
            QTRY_COMPARE(applied(), (QVariantList{previewed}));
            QVERIFY(!dialog->property("previewShown").toBool());
            QVERIFY(!dialog->property("visible").toBool());
            QVERIFY(fake->property("itunesArtworkResults").toList().isEmpty());
            QVERIFY(viewer->property("albumArtViewerOpen").toBool());
            QVERIFY(!viewer->property("albumArtPreviewActive").toBool());
            QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
            QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
            QTRY_VERIFY2(visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton")) == nullptr,
                         at.constData());
            // Out of the preview, the Image Details button is back.
            QTRY_VERIFY2(visibleItemNamed(host, prefix + QStringLiteral("InfoButton")) != nullptr,
                         at.constData());

            // Closing the viewer now closes it, with no dialog to go back to,
            // once the confirmation has faded out: it takes the keys until then.
            QTRY_VERIFY2(popupsSettled(), at.constData());
            QTest::keyClick(host, Qt::Key_Escape);
            QTRY_VERIFY2(!viewer->property("albumArtViewerOpen").toBool(), at.constData());
            QVERIFY(!dialog->property("visible").toBool());
            fake->setProperty("appliedItunesArtwork", QVariantList{});
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }
    }

    // An album without art applies at once, and the viewer shows the new
    // cover when the album has it.
    bridge->setProperty("viewerFullscreenMode", 0);
    bridge->setProperty("currentTrackCoverPath", QString());
    QVariantMap bareTarget = target;
    bareTarget.insert(QStringLiteral("coverPath"), QString());
    QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, bareTarget));
    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, previewed)));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(clickPreviewButton(window, QStringLiteral("albumArtPreviewApplyButton")));
    QTRY_COMPARE(applied(), (QVariantList{previewed}));
    QVERIFY(!replaceQuestion(window)->isVisible());
    QVERIFY(!dialog->property("visible").toBool());
    QVERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
    bridge->setProperty("currentTrackCoverPath", candidateUrl);
    QVERIFY(QMetaObject::invokeMethod(bridge, "snapshotChanged"));
    QTRY_COMPARE(viewer->property("albumArtViewerSource").toString(), candidateUrl);
}

void AlbumArtSmokeTest::itunesPreviewButtonsTakeSpaceFromThePlaybackShortcut() {
    // Space on a focused Back to Results or Apply presses it; the viewer's
    // Play/Pause shortcut does not take it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickWindow *window = ui.window;
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    QObject *dialog = ui.dialog;
    QObject *fake = ui.fake;
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    auto *surface = ui.surface;
    QVERIFY(playPause != nullptr);
    const QVariantMap target{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("artist"), QStringLiteral("The Quiet Meridian")},
        {QStringLiteral("coverPath"), coverUrl},
    };
    QSignalSpy toggled(playPause, SIGNAL(triggered(QObject*)));

    // Over the whole screen first, as in itunesPreviewAppliesOrGoesBackToTheResults.
    for (const int fullscreenMode : {1, 0}) {
        const QByteArray at = QByteArray("fullscreen ") + QByteArray::number(fullscreenMode);
        bridge->setProperty("viewerFullscreenMode", fullscreenMode);
        const auto viewerHost = [&]() -> QQuickWindow * {
            return fullscreenMode == 0
                ? window : viewerWindowNamed(ui.engine, QStringLiteral("albumArtFullscreenWindow"));
        };
        const auto preview = [&]() -> QQuickWindow * {
            if (!openItunesArtworkDialog(window, dialog, fake, fixture, target)) {
                return nullptr;
            }
            QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1));
            if (!QTest::qWaitFor([&]() { return viewer->property("albumArtPreviewActive").toBool()
                                         && !dialog->property("visible").toBool(); })) {
                return nullptr;
            }
            QQuickWindow *host = viewerHost();
            return host != nullptr && QTest::qWaitForWindowActive(host) ? host : nullptr;
        };
        const QString playbackBefore = bridge->property("playbackState").toString();

        // Back to Results.
        QQuickWindow *host = preview();
        QVERIFY2(host != nullptr, at.constData());
        QQuickItem *back = nullptr;
        QTRY_VERIFY2((back = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"))) != nullptr,
                     at.constData());
        QVERIFY2(tabTo(host, back), at.constData());
        QTest::keyClick(host, Qt::Key_Space);
        QTRY_VERIFY2(dialog->property("opened").toBool(), at.constData());
        QVERIFY2(!viewer->property("albumArtPreviewActive").toBool(), at.constData());
        QCOMPARE(toggled.count(), 0);
        QCOMPARE(bridge->property("playbackState").toString(), playbackBefore);

        // Apply: the album has art, so it asks first.
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        host = preview();
        QVERIFY2(host != nullptr, at.constData());
        QQuickItem *apply = nullptr;
        QTRY_VERIFY2((apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"))) != nullptr
                         && apply->isEnabled(), at.constData());
        QVERIFY2(tabTo(host, apply), at.constData());
        QTest::keyClick(host, Qt::Key_Space);
        QTRY_VERIFY2(replaceQuestionButton(window, host, QStringLiteral("confirmDialogConfirmButton")) != nullptr,
                     at.constData());
        QCOMPARE(toggled.count(), 0);
        QCOMPARE(bridge->property("playbackState").toString(), playbackBefore);
        QTRY_VERIFY2(popupsSettled(), at.constData());
        QVERIFY2(clickVisibleButton(replaceQuestion(window), QStringLiteral("confirmDialogCancelButton")), at.constData());
        QTRY_VERIFY2(replaceQuestionButton(window, host, QStringLiteral("confirmDialogConfirmButton")) == nullptr,
                     at.constData());

        // Away from the buttons, with the art's focus, Space still plays and
        // pauses. The main window's viewer gives the art the focus as it
        // opens.
        const QString viewFocusName = fullscreenMode == 1
            ? QStringLiteral("albumArtFullscreenViewFocus") : QStringLiteral("albumArtPopupViewFocus");
        if (fullscreenMode == 0) {
            QTest::keyClick(host, Qt::Key_Escape);
            QTRY_VERIFY2(dialog->property("opened").toBool(), at.constData());
            QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
            QTRY_VERIFY(!dialog->property("visible").toBool());
            host = preview();
            QVERIFY2(host == window, at.constData());
            QObject *popup = window->findChild<QObject *>(QStringLiteral("albumArtViewerPopup"));
            QVERIFY2(popup != nullptr && popup->property("visible").toBool(), at.constData());
            QQuickItem *content = qvariant_cast<QQuickItem *>(popup->property("contentItem"));
            QVERIFY2(content != nullptr, at.constData());
            QTRY_VERIFY2(content->hasActiveFocus(), at.constData());
            QTRY_COMPARE(focusedName(window), viewFocusName);
            QTest::keyClick(host, Qt::Key_Space);
            QTRY_COMPARE(toggled.count(), 1);
            QVERIFY2(viewer->property("albumArtPreviewActive").toBool(), at.constData());
            toggled.clear();
        }

        // A press on the art takes the focus back from a button; the art
        // still zooms with a double-click and opens its menu.
        QTRY_VERIFY2((back = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"))) != nullptr,
                     at.constData());
        QVERIFY2(tabTo(host, back), at.constData());
        const QPoint onArt = pointOnTheArt(surface);
        QVERIFY2(!onArt.isNull(), at.constData());
        QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, onArt);
        QTRY_COMPARE(focusedName(host), viewFocusName);
        QTest::keyClick(host, Qt::Key_Space);
        QTRY_COMPARE(toggled.count(), 1);
        QVERIFY2(viewer->property("albumArtPreviewActive").toBool(), at.constData());
        QVERIFY2(!dialog->property("visible").toBool(), at.constData());
        toggled.clear();
        QTest::mouseDClick(host, Qt::LeftButton, Qt::NoModifier, onArt);
        QTRY_COMPARE(surface->property("zoom").toDouble(), 2.0);
        QTest::mouseDClick(host, Qt::LeftButton, Qt::NoModifier, onArt);
        QTRY_COMPARE(surface->property("zoom").toDouble(), 1.0);
        QObject *contextMenu = surface->findChild<QObject *>(QStringLiteral("albumArtViewerContextMenu"));
        QVERIFY2(contextMenu != nullptr, at.constData());
        QTest::mouseClick(host, Qt::RightButton, Qt::NoModifier, onArt);
        QTRY_VERIFY2(contextMenu->property("opened").toBool(), at.constData());
        QVERIFY(QMetaObject::invokeMethod(contextMenu, "close"));
        QTRY_VERIFY2(!contextMenu->property("visible").toBool(), at.constData());
        QVERIFY2(viewer->property("albumArtPreviewActive").toBool(), at.constData());
        QTest::keyClick(host, Qt::Key_Escape);
        QTRY_VERIFY2(dialog->property("opened").toBool(), at.constData());
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        // Out of a preview, from Image Details too. The viewer opens with
        // the art's focus, not that of the button that had it as it closed.
        const QString infoName = fullscreenMode == 1 ? QStringLiteral("albumArtFullscreenInfoButton")
                                                     : QStringLiteral("albumArtPopupInfoButton");
        QQuickItem *info = nullptr;
        for (int opening = 0; opening < 2; ++opening) {
            QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
            QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
            host = viewerHost();
            QVERIFY2(host != nullptr && QTest::qWaitForWindowActive(host), at.constData());
            QTRY_COMPARE(focusedName(host), viewFocusName);
            QTRY_VERIFY2((info = visibleItemNamed(host, infoName)) != nullptr, at.constData());
            QVERIFY2(tabTo(host, info), at.constData());
            if (opening == 0) {
                QTest::keyClick(host, Qt::Key_Escape);
                QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
                window->requestActivate();
                QVERIFY(QTest::qWaitForWindowActive(window));
            }
        }
        const bool infoVisible = viewer->property("albumArtInfoVisible").toBool();
        const QPoint onCover = pointOnTheArt(surface);
        QVERIFY2(!onCover.isNull(), at.constData());
        QTest::mouseClick(host, Qt::LeftButton, Qt::NoModifier, onCover);
        QTRY_COMPARE(focusedName(host), viewFocusName);
        QTest::keyClick(host, Qt::Key_Space);
        QTRY_COMPARE(toggled.count(), 1);
        QCOMPARE(viewer->property("albumArtInfoVisible").toBool(), infoVisible);
        QVERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
        toggled.clear();
        QTest::keyClick(host, Qt::Key_Escape);
        QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));
    }
}

namespace {

// Back to Results and Apply stay whole inside the details column, in the
// main window's viewer and over the whole screen (`viewerMode` 0 or 1). The
// column is about 40 characters wide, so with a larger font it widens with
// them; where half the view leaves no room for that, they stack. At 24 pt,
// Back to Results is wider than the usual column: the column widens for it,
// so it is not cut short (#535).
void checkPreviewButtonsWholeInTheDetails(ItunesWindow &ui, int viewerMode, qreal pointSize,
                                          const QVariantList &fixture, const QVariantMap &target) {
    const QByteArray at = QByteArray("point size ") + QByteArray::number(pointSize)
        + QByteArray(", fullscreen ") + QByteArray::number(viewerMode);
    ui.bridge->setProperty("viewerFullscreenMode", viewerMode);
    QVERIFY2(openItunesArtworkDialog(ui.window, ui.dialog, ui.fake, fixture, target), at.constData());
    QVERIFY(QMetaObject::invokeMethod(ui.dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1)));
    QTRY_VERIFY2(ui.viewer->property("albumArtPreviewActive").toBool()
                     && !ui.dialog->property("visible").toBool(), at.constData());
    QQuickWindow *host = ui.viewerHost();
    QVERIFY2(host != nullptr && QTest::qWaitForWindowActive(host), at.constData());
    QQuickItem *column = nullptr;
    QQuickItem *back = nullptr;
    QQuickItem *apply = nullptr;
    QTRY_VERIFY2((column = visibleItemNamed(host, QStringLiteral("albumArtInfoColumn"))) != nullptr
                     && (back = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"))) != nullptr
                     && (apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"))) != nullptr,
                 at.constData());
    // The details' size: the body size, 1.2 times it over the whole screen.
    const qreal infoScale = viewerMode == 1 ? 1.2 : 1.0;
    const qreal widthScale = infoColumnWidthScale(viewerMode == 1, column->parentItem()->width());
    QFont bodyFont = qvariant_cast<QFont>(back->property("font"));
    bodyFont.setPointSizeF(bodyFont.pointSizeF() / infoScale);
    if (pointSize > 0) {
        QCOMPARE(bodyFont.pointSizeF(), pointSize);
    }
    QCoreApplication::processEvents();
    const QRectF columnRect = sceneRectOf(column);
    // The details' padding: 16 px with the usual font, more with a larger
    // one.
    const qreal padding = column->parentItem()->property("infoPadding").toReal();
    QCOMPARE(padding, pointSize > 10 ? std::round(16 * pointSize / 10) : 16.0);
    // Both whole, inside the column with its margins, apart.
    const qreal margin = padding - 0.5;
    QTRY_VERIFY2(columnRect.adjusted(margin, 0, -margin, 0).contains(sceneRectOf(back))
                     && columnRect.adjusted(margin, 0, -margin, 0).contains(sceneRectOf(apply)),
                 qPrintable(QStringLiteral("%1: column %2..%3, back %4..%5, apply %6..%7")
                                .arg(QString::fromLatin1(at))
                                .arg(columnRect.left()).arg(columnRect.right())
                                .arg(sceneRectOf(back).left()).arg(sceneRectOf(back).right())
                                .arg(sceneRectOf(apply).left()).arg(sceneRectOf(apply).right())));
    QVERIFY2(!sceneRectOf(back).intersects(sceneRectOf(apply)), at.constData());
    // Each has all the room it asks for, so the style draws its label whole,
    // not elided: Back to Results too at 24 pt.
    QVERIFY2(back->width() >= back->implicitWidth() - 0.5 && apply->width() >= apply->implicitWidth() - 0.5,
             qPrintable(QStringLiteral("%1: back %2 of %3, apply %4 of %5")
                            .arg(QString::fromLatin1(at)).arg(back->width()).arg(back->implicitWidth())
                            .arg(apply->width()).arg(apply->implicitWidth())));
    // About 40 characters of the body font wide, 279 px and its seam with
    // the usual font, 1.2 times wider over the whole screen, up to half the
    // view; or as wide as the buttons side by side, if two fifths of it
    // hold them; or as the widest button, up to three quarters for that
    // button alone (at 24 pt over this 800 px screen, its caption at the
    // details' size was cut short at half).
    const qreal fortyCharacters = infoColumnFortyCharacters(bodyFont, viewerMode == 1, column->parentItem()->width());
    const qreal leastWidth = std::round(280 * widthScale) - 1;
    const qreal widestButton = std::ceil(qMax(back->implicitWidth(), apply->implicitWidth())) + 2 * padding;
    const qreal halfTheView = std::floor(column->parentItem()->width() / 2);
    const qreal threeQuarters = std::floor(column->parentItem()->width() * 0.75);
    QQuickItem *buttons = visibleItemNamed(host, QStringLiteral("albumArtPreviewButtons"));
    QVERIFY2(buttons != nullptr, at.constData());
    const qreal twoFifths = std::floor(column->parentItem()->width() * 0.4);
    const qreal sideBySide = std::ceil(back->implicitWidth() + buttons->property("spacing").toReal()
                                       + apply->implicitWidth()) + 2 * padding;
    QCOMPARE(columnRect.width(),
             qMax(qMax(qMax(leastWidth, qMin(halfTheView, fortyCharacters)),
                       sideBySide <= twoFifths ? sideBySide : 0.0),
                  qMin(threeQuarters, widestButton)));
    // The footer's buttons, under the details, inside the column.
    QQuickItem *footer = visibleItemNamed(host, QStringLiteral("albumArtPreviewFooter"));
    QVERIFY2(footer != nullptr, at.constData());
    QVERIFY2(isWithin(back, buttons) && isWithin(apply, buttons) && isWithin(footer, column), at.constData());
    QVERIFY2(sceneRectOf(footer).bottom() <= columnRect.bottom(), at.constData());
    QVERIFY2(sceneRectOf(footer).contains(sceneRectOf(back)) && sceneRectOf(footer).contains(sceneRectOf(apply)),
             at.constData());
    const bool fitsInARow = back->implicitWidth() + buttons->property("spacing").toReal()
        + apply->implicitWidth() <= columnRect.width() - 2 * padding;
    QCOMPARE(buttons->property("oneRow").toBool(), fitsInARow);
    if (fitsInARow) {
        // One row, Back to Results then Apply, at the column's end.
        QCOMPARE(sceneRectOf(back).top(), sceneRectOf(apply).top());
        QVERIFY(sceneRectOf(back).right() <= sceneRectOf(apply).left());
        QVERIFY(columnRect.right() - padding - 0.5 <= sceneRectOf(apply).right());
    } else {
        // Stacked, Back to Results above Apply, each the column's width.
        QVERIFY2(sideBySide > twoFifths, "two fifths of the view hold them in a row");
        QVERIFY2(pointSize > 0 || viewerMode == 1, "the usual font fits them in a row in the window");
        QVERIFY(columnRect.contains(sceneRectOf(back).adjusted(0, -0.5, 0, 0)));
        QVERIFY(sceneRectOf(back).bottom() <= sceneRectOf(apply).top());
        QCOMPARE(sceneRectOf(back).width(), sceneRectOf(apply).width());
        QCOMPARE(sceneRectOf(back).width(), columnRect.width() - 2 * padding);
    }
    // In a screen-filling viewer of the 800 px test screen at 24 pt.
    if (pointSize >= 24.0 && viewerMode == 1) {
        QVERIFY2(!fitsInARow, "24 pt in an 800 px view does not fit them in a row");
    }
    QVERIFY(QMetaObject::invokeMethod(ui.viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(ui.dialog->property("opened").toBool());
    QVERIFY(QMetaObject::invokeMethod(ui.dialog, "close"));
    QTRY_VERIFY(!ui.dialog->property("visible").toBool());
    QTRY_VERIFY(!ui.viewer->property("albumArtViewerOpen").toBool());
    ui.window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(ui.window));
}

// Back to Results and Apply are in place inside the details the moment a
// preview shows them, before any frame, so that nothing can find Apply
// elsewhere: a candidate's preview after one of the current art, which had
// Back to Results alone, as when it was laid out last. In a row or
// stacked, as checkPreviewButtonsWholeInTheDetails has them.
void checkPreviewButtonsInPlaceAsSoonAsTheyShow(ItunesWindow &ui, int viewerMode, bool stacked,
                                                const QVariantList &fixture, const QVariantMap &target) {
    ui.bridge->setProperty("viewerFullscreenMode", viewerMode);
    QVERIFY(openItunesArtworkDialog(ui.window, ui.dialog, ui.fake, fixture, target));
    const auto backToTheResults = [&]() {
        QVERIFY(clickPreviewButton(ui.viewerHost(), QStringLiteral("albumArtPreviewBackButton")));
        QTRY_VERIFY(ui.dialog->property("opened").toBool());
        QTRY_VERIFY(!ui.viewer->property("albumArtViewerOpen").toBool());
    };
    // Where the preview's buttons are now, without letting the events run.
    const auto checkButtonsInPlace = [&](const QByteArray &at) {
        QQuickWindow *host = ui.viewerHost();
        QQuickItem *column = visibleItemNamed(host, QStringLiteral("albumArtInfoColumn"));
        QQuickItem *back = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"));
        QQuickItem *apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"));
        QVERIFY2(column != nullptr && back != nullptr && apply != nullptr, at.constData());
        const QRectF columnRect = sceneRectOf(column);
        // The details' padding, larger with a larger font.
        const qreal padding = column->parentItem()->property("infoPadding").toReal();
        const QRectF inside = columnRect.adjusted(padding - 0.5, 0, -(padding - 0.5), 0);
        const QRectF backRect = sceneRectOf(back);
        const QRectF applyRect = sceneRectOf(apply);
        const QString where = QStringLiteral("%1: column %2, back %3, apply %4")
            .arg(QString::fromLatin1(at), describeRect(inside), describeRect(backRect), describeRect(applyRect));
        QVERIFY2(inside.contains(backRect) && inside.contains(applyRect), qPrintable(where));
        if (stacked) {
            // Back to Results above Apply, each the column's width.
            QVERIFY2(backRect.bottom() <= applyRect.top(), qPrintable(where));
            for (const QRectF &rect : {backRect, applyRect}) {
                QVERIFY2(qAbs(rect.left() - (columnRect.left() + padding)) <= 0.5, qPrintable(where));
                QVERIFY2(qAbs(rect.width() - (columnRect.width() - 2 * padding)) <= 0.5, qPrintable(where));
            }
        } else {
            // One row, Back to Results then Apply, at the column's end.
            QVERIFY2(backRect.right() <= applyRect.left(), qPrintable(where));
            QCOMPARE(backRect.top(), applyRect.top());
            QVERIFY2(inside.right() - applyRect.right() <= 0.5, qPrintable(where));
        }
    };

    QTRY_COMPARE(ui.dialog->property("currentArtworkSource").toString(), target.value(QStringLiteral("coverPath")));
    for (int round = 1; round <= 2; ++round) {
        const QByteArray at = QByteArray("round ") + QByteArray::number(round);
        // The current art: Back to Results alone.
        QVERIFY(QMetaObject::invokeMethod(ui.dialog, "showPreview", Q_ARG(QVariant, -1)));
        QTRY_VERIFY2(ui.viewer->property("albumArtPreviewActive").toBool(), at.constData());
        QVERIFY2(visibleItemNamed(ui.viewerHost(), QStringLiteral("albumArtPreviewApplyButton")) == nullptr,
                 at.constData());
        backToTheResults();
        if (QTest::currentTestFailed()) {
            return;
        }

        // A candidate: Apply joins it, both in place at once, and stay.
        QVERIFY(QMetaObject::invokeMethod(ui.dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1)));
        QVERIFY2(ui.viewer->property("albumArtPreviewActive").toBool(), at.constData());
        checkButtonsInPlace(at);
        if (QTest::currentTestFailed()) {
            return;
        }
        QTest::qWait(50);
        checkButtonsInPlace(at + " later");
        if (QTest::currentTestFailed()) {
            return;
        }
        backToTheResults();
        if (QTest::currentTestFailed()) {
            return;
        }
    }
    QVERIFY(QMetaObject::invokeMethod(ui.dialog, "close"));
    QTRY_VERIFY(!ui.dialog->property("visible").toBool());
}

} // namespace

void AlbumArtSmokeTest::itunesPreviewButtonsStayWholeInTheDetails_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("default font") << 0.0;
    QTest::newRow("24 pt") << 24.0;
}

// The preview's buttons in the details, in the main window's viewer and
// over the whole screen: whole, and in place as soon as they show, in a row
// with the usual font, and at 24 pt over the whole 800 px test screen,
// too narrow a view for them side by side, stacked.
void AlbumArtSmokeTest::itunesPreviewButtonsStayWholeInTheDetails() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    QCOMPARE(fixture.size(), 3);
    const QVariantMap target{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("coverPath"), coverUrl},
    };
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    for (const int viewerMode : {0, 1}) {
        checkPreviewButtonsWholeInTheDetails(ui, viewerMode, pointSize, fixture, target);
        if (QTest::currentTestFailed()) {
            return;
        }
    }
    const bool large = pointSize > 0;
    checkPreviewButtonsInPlaceAsSoonAsTheyShow(ui, large ? 1 : 0, large, fixture, target);
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(finishIncubation(ui.engine));
    dropSizeHintMessages();
}

void AlbumArtSmokeTest::itunesPreviewCannotApplyACandidateThatIsNotReady() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    QVariantList fixture = itunesArtworkFixture(dir, 4);
    QCOMPARE(fixture.size(), 4);
    // Candidate 2 is still downloading, with only its thumbnail; candidate
    // 3's file is not an image.
    QVariantMap loading = fixture.at(2).toMap();
    loading.insert(QStringLiteral("normalizedUrl"), QString());
    loading.insert(QStringLiteral("normalizedPath"), QString());
    loading.insert(QStringLiteral("assetReady"), false);
    fixture[2] = loading;
    const QString brokenPath = dir.filePath(QStringLiteral("broken.png"));
    {
        QFile broken(brokenPath);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        QVERIFY(broken.write("not an image") > 0);
    }
    QVariantMap broken = fixture.at(3).toMap();
    broken.insert(QStringLiteral("normalizedUrl"), QUrl::fromLocalFile(brokenPath).toString());
    broken.insert(QStringLiteral("normalizedPath"), brokenPath);
    fixture[3] = broken;
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickWindow *window = ui.window;
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    QObject *dialog = ui.dialog;
    QObject *fake = ui.fake;
    bridge->setProperty("viewerFullscreenMode", 0);
    const QVariantMap target{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("coverPath"), QString()},
    };
    const auto applied = [fake]() { return fake->property("appliedItunesArtwork").toList(); };
    const auto applyButton = [window]() {
        return visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton"));
    };
    QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, target));

    // Preview waits for the download: the dialog stays until it is done.
    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 2)));
    QTRY_COMPARE(fake->property("preparedItunesArtwork").toList(), (QVariantList{2}));
    updateItunesArtworkRow(fake, 2, {{QStringLiteral("assetLoading"), true}});
    QVERIFY(dialog->property("visible").toBool());
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    const QString readyPath = dir.filePath(QStringLiteral("candidate-2.png"));
    updateItunesArtworkRow(fake, 2, {
        {QStringLiteral("assetLoading"), false},
        {QStringLiteral("assetReady"), true},
        {QStringLiteral("normalizedPath"), readyPath},
        {QStringLiteral("normalizedUrl"), QUrl::fromLocalFile(readyPath).toString()},
    });
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY(!dialog->property("visible").toBool());
    QTRY_VERIFY(applyButton() != nullptr && applyButton()->isEnabled());

    // Loading again, or failed, it cannot be applied from the preview.
    updateItunesArtworkRow(fake, 2, {{QStringLiteral("assetLoading"), true}});
    QTRY_VERIFY(!applyButton()->isEnabled());
    QVERIFY(!clickVisibleButton(window, QStringLiteral("albumArtPreviewApplyButton")));
    updateItunesArtworkRow(fake, 2, {
        {QStringLiteral("assetLoading"), false},
        {QStringLiteral("assetError"), QStringLiteral("The artwork could not be downloaded.")},
    });
    QVERIFY(!applyButton()->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(dialog, "applyPreviewedSuggestion"));
    QVERIFY(applied().isEmpty());
    updateItunesArtworkRow(fake, 2, {{QStringLiteral("assetError"), QString()}});
    QTRY_VERIFY(applyButton()->isEnabled());

    // A candidate whose image the viewer cannot read cannot be applied.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    // The viewer and the dialog's check of the candidate each warn once.
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Error decoding: .*broken\\.png")));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Error decoding: .*broken\\.png")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 3)));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QQuickItem *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(surface != nullptr);
    // Image.Error.
    QTRY_COMPARE(surface->property("imageStatus").toInt(), 3);
    // The two warnings for it, which the test expects. The viewer's and the
    // dialog's images decode on their own, so the second can come after the
    // viewer shows the error: wait for both.
    QStringList decodeWarnings;
    const auto bothDecodeWarnings = [&decodeWarnings]() {
        decodeWarnings += takeCapturedMessagesText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return decodeWarnings.size() >= 2;
    };
    QTRY_VERIFY2(bothDecodeWarnings(), qPrintable(decodeWarnings.join(QLatin1Char('\n'))));
    QCOMPARE(decodeWarnings.size(), 2);
    for (const QString &warning : decodeWarnings) {
        QVERIFY2(warning.contains(QStringLiteral("Error decoding")) && warning.contains(QStringLiteral("broken.png")),
                 qPrintable(warning));
    }
    QTRY_VERIFY(applyButton() != nullptr);
    QVERIFY(!applyButton()->isEnabled());
    QVERIFY(!clickVisibleButton(window, QStringLiteral("albumArtPreviewApplyButton")));
    QVERIFY(applied().isEmpty());
    // Nor while the viewer shows the current art to compare, which decodes.
    // The dialog takes the keys until it has faded out.
    QTRY_VERIFY(popupsSettled());
    QTest::keyClick(window, Qt::Key_Left);
    QTRY_VERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
    QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
    QTRY_COMPARE(surface->property("imageStatus").toInt(), 1);
    QVERIFY(!applyButton()->isEnabled());
    QVERIFY(!dialog->property("previewCandidateReady").toBool());
    QVERIFY(!clickVisibleButton(window, QStringLiteral("albumArtPreviewApplyButton")));
    QVERIFY(QMetaObject::invokeMethod(dialog, "applyPreviewedSuggestion"));
    QVERIFY(applied().isEmpty());
    QVERIFY(clickPreviewButton(window, QStringLiteral("albumArtPreviewBackButton")));
    QTRY_VERIFY(dialog->property("opened").toBool());
}

void AlbumArtSmokeTest::itunesPreviewShowsTheSearchedAlbumsArtAfterPlaybackMovesOn() {
    QTemporaryDir dir;
    QTemporaryDir otherDir;
    QVERIFY(dir.isValid() && otherDir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    const QString otherCoverUrl = QUrl::fromLocalFile(savedCover(otherDir, 40, 40)).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    QCOMPARE(fixture.size(), 3);
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickWindow *window = ui.window;
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    QObject *dialog = ui.dialog;
    QObject *fake = ui.fake;
    bridge->setProperty("viewerFullscreenMode", 0);
    // The suggestions are for the playing track's album, which has art.
    QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, QVariantMap{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("coverPath"), coverUrl},
    }));
    const auto moveOnTo = [bridge](const QString &cover) {
        bridge->setProperty("currentTrackPath", QStringLiteral("/music/Elsewhere/01 Lantern Rows.flac"));
        bridge->setProperty("currentTrackCoverPath", cover);
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
    };
    const auto backToTheResults = [&]() {
        QVERIFY(clickPreviewButton(window, QStringLiteral("albumArtPreviewBackButton")));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
    };

    for (const QString &otherCover : {QString(), otherCoverUrl}) {
        const QByteArray at = otherCover.isEmpty() ? "next album without art" : "next album with art";
        // Playback moves on to another album: the dialog keeps the searched
        // album's art, and its Preview shows that art.
        moveOnTo(otherCover);
        QTRY_COMPARE(dialog->property("currentArtworkSource").toString(), coverUrl);
        QVERIFY2(clickVisibleButton(qobject_cast<QQuickWindow *>(dialog),
                                    QStringLiteral("itunesArtworkCurrentPreviewButton")), at.constData());
        QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
        QTRY_VERIFY2(!dialog->property("visible").toBool(), at.constData());
        QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
        QTRY_VERIFY2(visibleItemNamed(window, QStringLiteral("albumArtPreviewBackButton")) != nullptr,
                     at.constData());
        // It stays on that art while playback goes on.
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
        QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
        backToTheResults();

        // A candidate compares with the searched album's art too.
        QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1)));
        QTRY_VERIFY2(viewer->property("albumArtViewerOpen").toBool(), at.constData());
        // The dialog takes the keys until it has faded out.
        QTRY_VERIFY2(popupsSettled(), at.constData());
        QTest::keyClick(window, Qt::Key_Left);
        QTRY_VERIFY2(viewer->property("albumArtViewerShowsCurrentTrack").toBool(), at.constData());
        QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
        QCOMPARE(viewer->property("albumArtViewerSource").toString(), coverUrl);
        backToTheResults();
    }

    // Without the searched album's art there is nothing to preview: the
    // dialog stays, rather than hiding with no preview to go back from.
    fake->setProperty("itunesArtworkTarget", QVariantMap{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("coverPath"), QString()},
    });
    QMetaObject::invokeMethod(bridge, "snapshotChanged");
    QTRY_COMPARE(dialog->property("currentArtworkSource").toString(), QString());
    QVERIFY(QMetaObject::invokeMethod(dialog, "showPreview", Q_ARG(QVariant, -1)));
    QVERIFY(dialog->property("opened").toBool());
    QVERIFY(!dialog->property("previewShown").toBool());
    QVERIFY(!viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(!viewer->property("albumArtPreviewActive").toBool());
}

void AlbumArtSmokeTest::itunesPreviewApplyShowsTheAppliedCoverAfterPlaybackMovesOn() {
    QTemporaryDir dir;
    QTemporaryDir otherDir;
    QTemporaryDir nextDir;
    QVERIFY(dir.isValid() && otherDir.isValid() && nextDir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    const QString otherCoverUrl = QUrl::fromLocalFile(savedCover(otherDir, 40, 40)).toString();
    const QString nextCoverUrl = QUrl::fromLocalFile(savedCover(nextDir, 32, 32)).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    QCOMPARE(fixture.size(), 3);
    const QString appliedUrl = fixture.at(1).toMap().value(QStringLiteral("normalizedUrl")).toString();
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QQuickWindow *window = ui.window;
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    QObject *dialog = ui.dialog;
    QObject *fake = ui.fake;
    bridge->setProperty("viewerFullscreenMode", 0);
    const auto playTrack = [bridge](const QString &path, const QString &cover) {
        bridge->setProperty("currentTrackPath", path);
        bridge->setProperty("currentTrackCoverPath", cover);
        startPlay(bridge);
        QMetaObject::invokeMethod(bridge, "trackIdentityChanged");
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
    };
    const auto viewerSource = [viewer]() { return viewer->property("albumArtViewerSource").toString(); };

    for (const QString &otherCover : {otherCoverUrl, QString()}) {
        const QByteArray at = otherCover.isEmpty() ? "next album without art" : "next album with art";
        // The suggestions are for album A, which has art; playback then
        // moves on to album B.
        setPlayingTrack(bridge);
        playTrack(QStringLiteral("/music/Morning Tide Field.flac"), coverUrl);
        QVERIFY2(openItunesArtworkDialog(window, dialog, fake, fixture, QVariantMap{
            {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
            {QStringLiteral("album"), QStringLiteral("Small Hours")},
            {QStringLiteral("coverPath"), coverUrl},
        }), at.constData());
        playTrack(QStringLiteral("/music/Elsewhere/01 Lantern Rows.flac"), otherCover);

        // Apply from the preview, past the question: the viewer shows the
        // cover applied to A, not B's art, nor nothing.
        applyItunesCandidateFromPreview(window, dialog, fake, viewer, 1, appliedUrl, at);
        if (QTest::currentTestFailed()) {
            return;
        }

        // It stays while B plays on, and follows playback at the next track.
        QMetaObject::invokeMethod(bridge, "snapshotChanged");
        QCOMPARE(viewerSource(), appliedUrl);
        playTrack(QStringLiteral("/music/Elsewhere/02 Harbour Steps.flac"), nextCoverUrl);
        QTRY_COMPARE(viewerSource(), nextCoverUrl);
        QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
        playTrack(QStringLiteral("/music/Elsewhere/03 Low Tide.flac"), otherCoverUrl);
        QTRY_COMPARE(viewerSource(), otherCoverUrl);

        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY2(!viewer->property("albumArtViewerOpen").toBool(), at.constData());
        fake->setProperty("appliedItunesArtwork", QVariantList{});
    }
}

// After a cover is applied to album A while B plays, B playing again by
// any route ends the applied cover, though B's path and art stay the same:
// Repeat One, B queued twice, or a stop and Play. Playing on in the same
// play keeps it.
void AlbumArtSmokeTest::itunesAppliedCoverGivesWayWhenTheSameTrackPlaysAgain() {
    QTemporaryDir dir;
    QTemporaryDir otherDir;
    QVERIFY(dir.isValid() && otherDir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    const QString otherCoverUrl = QUrl::fromLocalFile(savedCover(otherDir, 40, 40)).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    QCOMPARE(fixture.size(), 3);
    const QString appliedUrl = fixture.at(1).toMap().value(QStringLiteral("normalizedUrl")).toString();
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    QObject *bridge = ui.bridge;
    QObject *viewer = ui.viewer;
    bridge->setProperty("viewerFullscreenMode", 0);
    const auto viewerSource = [viewer]() { return viewer->property("albumArtViewerSource").toString(); };
    const auto notify = [bridge](std::initializer_list<const char *> signalNames) {
        for (const char *name : signalNames) {
            QVERIFY(QMetaObject::invokeMethod(bridge, name));
        }
    };
    const QString bPath = QStringLiteral("/music/Elsewhere/01 Lantern Rows.flac");

    for (const QString &route : {QStringLiteral("repeat one"), QStringLiteral("queued twice"),
                                 QStringLiteral("stop and play again")}) {
        const QByteArray at = route.toUtf8();
        // The suggestions are for album A; playback then moves on to B, in
        // row 2 of the queue.
        setPlayingTrack(bridge);
        bridge->setProperty("currentTrackCoverPath", coverUrl);
        bridge->setProperty("queueLength", 5);
        bridge->setProperty("playingQueueIndex", 1);
        startPlay(bridge);
        notify({"trackIdentityChanged", "snapshotChanged"});
        QVERIFY2(openItunesArtworkDialog(ui.window, ui.dialog, ui.fake, fixture, QVariantMap{
            {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
            {QStringLiteral("album"), QStringLiteral("Small Hours")},
            {QStringLiteral("coverPath"), coverUrl},
        }), at.constData());
        bridge->setProperty("currentTrackPath", bPath);
        bridge->setProperty("currentTrackCoverPath", otherCoverUrl);
        bridge->setProperty("playingQueueIndex", 2);
        startPlay(bridge);
        notify({"trackIdentityChanged", "trackMetadataChanged", "snapshotChanged"});
        applyItunesCandidateFromPreview(ui.window, ui.dialog, ui.fake, viewer, 1, appliedUrl, at);
        if (QTest::currentTestFailed()) {
            return;
        }

        // B plays on: a seek, a pause and a queue edit that keeps B
        // playing, moved up a row, keep the applied cover.
        QVERIFY(QMetaObject::invokeMethod(bridge, "seek", Q_ARG(QVariant, 90.0)));
        notify({"playbackChanged", "snapshotChanged"});
        bridge->setProperty("playbackState", QStringLiteral("Paused"));
        notify({"playbackChanged", "snapshotChanged"});
        bridge->setProperty("playbackState", QStringLiteral("Playing"));
        notify({"playbackChanged", "snapshotChanged"});
        bridge->setProperty("queueLength", 4);
        bridge->setProperty("playingQueueIndex", 1);
        notify({"trackIdentityChanged", "snapshotChanged"});
        QCoreApplication::processEvents();
        QCOMPARE(viewerSource(), appliedUrl);
        QVERIFY2(!viewer->property("albumArtViewerShowsCurrentTrack").toBool(), at.constData());

        // B plays again, with the same path and art.
        if (route == QStringLiteral("repeat one")) {
            startPlay(bridge);
            notify({"trackIdentityChanged", "playbackChanged"});
        } else if (route == QStringLiteral("queued twice")) {
            bridge->setProperty("playingQueueIndex", 2);
            startPlay(bridge);
            notify({"trackIdentityChanged", "playbackChanged"});
        } else {
            // The stop ends the play; the applied cover stays until the next.
            bridge->setProperty("playbackState", QStringLiteral("Stopped"));
            bridge->setProperty("playingQueueIndex", -1);
            notify({"trackIdentityChanged", "playbackChanged", "snapshotChanged"});
            QCoreApplication::processEvents();
            QCOMPARE(viewerSource(), appliedUrl);
            bridge->setProperty("playbackState", QStringLiteral("Playing"));
            bridge->setProperty("playingQueueIndex", 1);
            startPlay(bridge);
            notify({"trackIdentityChanged", "playbackChanged"});
        }
        QTRY_COMPARE(viewerSource(), otherCoverUrl);
        QVERIFY2(viewer->property("albumArtViewerShowsCurrentTrack").toBool(), at.constData());
        QVERIFY2(!viewer->property("albumArtShowsAppliedCover").toBool(), at.constData());
        QCOMPARE(bridge->property("currentTrackPath").toString(), bPath);

        QTest::keyClick(ui.window, Qt::Key_Escape);
        QTRY_VERIFY2(!viewer->property("albumArtViewerOpen").toBool(), at.constData());
        ui.fake->setProperty("appliedItunesArtwork", QVariantList{});
    }
}

namespace {

QRgb borderColorOf(QQuickItem *rectangle) {
    QObject *border = qvariant_cast<QObject *>(rectangle->property("border"));
    return border != nullptr ? colorOf(border, "color") : 0;
}

// A Breeze scheme's colours that UiPalette reads.
struct DetailsScheme {
    const char *window;
    const char *view;
    const char *text;
    const char *inactiveText;
    const char *highlight;
};
const DetailsScheme kDetailsBreezeDark{"#202326", "#141618", "#fcfcfc", "#a1a9b1", "#3daee9"};
const DetailsScheme kDetailsBreezeLight{"#eff0f1", "#ffffff", "#232629", "#707d8a", "#3daee9"};

// As createDetailsSurfaceWindow, in `scheme`, with Show iTunes Artwork,
// and over the whole screen if `fullscreen`.
QObject *createThemedDetailsSurfaceWindow(QQmlApplicationEngine &engine, const QString &coverPath,
                                          const DetailsScheme &scheme, bool fullscreen,
                                          QString *errorText) {
    engine.rootContext()->setContextProperty(QStringLiteral("testCoverUrl"),
                                             QUrl::fromLocalFile(coverPath).toString());
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    const QByteArray qml = QStringLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import org.kde.kirigami 2.20 as Kirigami
import "components" as Components
import "viewers" as Viewers

ApplicationWindow {
    id: surfaceWindow
    width: 1000
    height: 600
    visible: true
    palette.window: "%1"
    palette.base: "%2"
    palette.text: "%3"
    palette.windowText: "%3"
    property alias surface: surface
    property alias tokens: tokens

    Action { id: replaceFromItunesAction }

    Item {
        anchors.fill: parent
        Kirigami.Theme.inherit: false
        Kirigami.Theme.backgroundColor: "%1"
        Kirigami.Theme.textColor: "%3"
        Kirigami.Theme.disabledTextColor: "%4"
        Kirigami.Theme.highlightColor: "%5"

        Components.UiPalette {
            id: tokens
            windowRoot: surfaceWindow
        }

        Viewers.AlbumArtSurface {
            id: surface
            anchors.fill: parent
            viewerOpen: true
            viewerSource: testCoverUrl
            infoVisible: true
            fullscreen: %6
            initialViewToken: 0
            viewerDecodeWidth: 1000
            viewerDecodeHeight: 600
            replaceFromItunesAction: replaceFromItunesAction
            currentTrackItunesArtworkDisabledReason: function() { return "" }
            closeViewer: function() {}
            focusFullscreen: function() {}
            comparisonModeAvailable: true
            uiPalette: tokens
        }
    }
}
)QML")
                               .arg(QLatin1String(scheme.window), QLatin1String(scheme.view),
                                    QLatin1String(scheme.text), QLatin1String(scheme.inactiveText),
                                    QLatin1String(scheme.highlight),
                                    fullscreen ? QStringLiteral("true") : QStringLiteral("false"))
                               .toUtf8();
    return createQmlObjectFromSource(engine, qml, baseUrl, errorText);
}

} // namespace

void AlbumArtSmokeTest::albumArtDetailsAreOnThePlotStripInEitherTheme_data() {
    QTest::addColumn<bool>("dark");
    QTest::addColumn<bool>("fullscreen");
    QTest::newRow("breeze light, window") << false << false;
    QTest::newRow("breeze light, whole screen") << false << true;
    QTest::newRow("breeze dark, window") << true << false;
    QTest::newRow("breeze dark, whole screen") << true << true;
}

void AlbumArtSmokeTest::albumArtDetailsAreOnThePlotStripInEitherTheme() {
    // The details are on the plot's strip colour in both themes, with the
    // plot's text colours, 4.5:1 on it, and its focus ring, 3:1 on it, on
    // their buttons: never the light theme's colours, which would vanish
    // on the dark strip. Over the whole screen, their text is 1.2 times the
    // body size, 16 px with the default 10 pt font, the file's name 18 px,
    // in a column 320 px wide with its seam, 16 px in from its edges.
    QFETCH(bool, dark);
    QFETCH(bool, fullscreen);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 300, 300);
    QVERIFY(!coverPath.isEmpty());
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createThemedDetailsSurfaceWindow(
        engine, coverPath, dark ? kDetailsBreezeDark : kDetailsBreezeLight, fullscreen, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *surface = qvariant_cast<QQuickItem *>(root->property("surface"));
    QObject *palette = qvariant_cast<QObject *>(root->property("tokens"));
    QVERIFY(surface != nullptr && palette != nullptr);
    QCOMPARE(palette->property("themeIsDark").toBool(), dark);
    QObject *plot = qvariant_cast<QObject *>(palette->property("mediaPlot"));
    QVERIFY(plot != nullptr);
    surface->setProperty("fileInfo", QVariantMap{
        {QStringLiteral("path"), QStringLiteral("/music/Small Hours/cover.jpg")},
        {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
        {QStringLiteral("width"), 1400},
        {QStringLiteral("height"), 1400},
        {QStringLiteral("fileSizeText"), QStringLiteral("412 KiB")},
        {QStringLiteral("format"), QStringLiteral("JPEG")},
        {QStringLiteral("embeddedCover"), false},
    });

    // Black around the art, the strip colour behind the details, the same
    // in both themes.
    QQuickItem *floor = itemNamed(surface, QStringLiteral("albumArtFloor"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *backing = itemNamed(surface, QStringLiteral("albumArtInfoBacking"));
    QVERIFY(floor != nullptr && column != nullptr && backing != nullptr);
    QCOMPARE(colorOf(floor, "color"), qRgb(0, 0, 0));
    QCOMPARE(colorOf(backing, "color"), colorOf(plot, "plotStrip"));
    QCOMPARE(colorOf(plot, "plotStrip"), qRgb(0x12, 0x12, 0x12));
    const QRgb strip = colorOf(backing, "color");
    QTRY_COMPARE(column->width(), infoColumnWidth(fullscreen, surface->width()));
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QVERIFY(image != nullptr);
    QTRY_VERIFY(paintedRectOf(image).left() > 16);
    const QPoint leftOfTheArt(int(paintedRectOf(image).left() / 2), int(paintedRectOf(image).center().y()));
    QTRY_VERIFY(window->grabWindow().convertToFormat(QImage::Format_RGB32).pixel(leftOfTheArt) == qRgb(0, 0, 0));
    QCOMPARE(window->grabWindow().convertToFormat(QImage::Format_RGB32).pixel(
                 int(sceneRectOf(column).center().x()), 590),
             strip);

    // The text: the plot's, 4.5:1 on the strip.
    QQuickItem *title = itemNamed(column, QStringLiteral("albumArtInfoTitle"));
    QVERIFY(title != nullptr);
    QCOMPARE(colorOf(title, "color"), colorOf(plot, "plotText"));
    QList<QQuickItem *> texts{title};
    for (const QString &name : {QStringLiteral("albumArtInfoLabel"), QStringLiteral("albumArtInfoValue"),
                                QStringLiteral("albumArtFolderLabel"), QStringLiteral("albumArtFolderPath"),
                                QStringLiteral("albumArtDetailsButtonLabel")}) {
        const QList<QQuickItem *> found = visibleItemsNamed(column, name);
        QVERIFY2(!found.isEmpty(), qPrintable(name));
        texts << found;
    }
    for (QQuickItem *text : std::as_const(texts)) {
        const QRgb color = colorOf(text, "color");
        QVERIFY(color == colorOf(plot, "plotText") || color == colorOf(plot, "plotTextSecondary"));
        QVERIFY2(contrastOn(color, strip) >= 4.5,
                 qPrintable(QStringLiteral("%1 \"%2\": %3:1")
                                .arg(text->objectName(), text->property("text").toString())
                                .arg(contrastOn(color, strip))));
    }

    // At the body size in the window; over the whole screen, 1.2 times it,
    // 16 px at the default font, the file's name too, no larger than the
    // title strip's track title over it (at 18 px it outranked the title):
    // from the font, so a larger one scales them as well.
    const qreal body = palette->property("basePointSize").toReal();
    QQuickItem *value = visibleItemsNamed(column, QStringLiteral("albumArtInfoValue")).value(0);
    QVERIFY(value != nullptr);
    QCOMPARE(qvariant_cast<QFont>(value->property("font")).pointSizeF(), body * (fullscreen ? 1.2 : 1.0));
    QCOMPARE(qvariant_cast<QFont>(title->property("font")).pointSizeF(), body * (fullscreen ? 1.2 : 1.0));
    if (fullscreen && qFuzzyCompare(body, 10.0)) {
        QCOMPARE(QFontInfo(qvariant_cast<QFont>(value->property("font"))).pixelSize(), 16);
        QCOMPARE(QFontInfo(qvariant_cast<QFont>(title->property("font"))).pixelSize(), 16);
    }
    QCOMPARE(sceneRectOf(title).left() - sceneRectOf(column).left(), surface->property("infoPadding").toReal());

    // The buttons: Copy Path's glyph and Show iTunes Artwork in the
    // plot's colours, and the plot's focus ring for the keyboard, 3:1 on
    // the strip, outside a gap in the strip's colour.
    QQuickItem *copy = visibleItemNamed(window, QStringLiteral("albumArtCopyPathButton"));
    QQuickItem *compare = visibleItemNamed(window, QStringLiteral("albumArtCompareButton"));
    QVERIFY(copy != nullptr && compare != nullptr);
    QVERIFY(qvariant_cast<QObject *>(copy->property("plotColors")) == plot);
    QQuickItem *copyGlyph = itemNamed(copy, QStringLiteral("viewerToolButtonIcon"));
    QQuickItem *compareGlyph = itemNamed(compare, QStringLiteral("albumArtDetailsButtonIcon"));
    QVERIFY(copyGlyph != nullptr && compareGlyph != nullptr);
    QVERIFY(contrastOn(colorOf(copyGlyph, "color"), strip) >= 3.0);
    QVERIFY(contrastOn(colorOf(compareGlyph, "color"), strip) >= 4.5);
    QCOMPARE(qvariant_cast<QFont>(compare->property("font")).pointSizeF(), body * (fullscreen ? 1.2 : 1.0));
    window->requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(window));
    for (QQuickItem *button : {copy, compare}) {
        button->forceActiveFocus(Qt::TabFocusReason);
        QTRY_VERIFY(button->property("visualFocus").toBool());
        QQuickItem *ring = itemNamed(button, button == copy ? QStringLiteral("viewerToolButtonFocusRing")
                                                            : QStringLiteral("albumArtDetailsButtonFocusRing"));
        QVERIFY(ring != nullptr && ring->isVisible());
        QCOMPARE(borderColorOf(ring), colorOf(plot, "plotFocusRing"));
        QVERIFY2(contrastOn(borderColorOf(ring), strip) >= 3.0,
                 qPrintable(QStringLiteral("%1: %2:1").arg(button->objectName()).arg(contrastOn(borderColorOf(ring), strip))));
    }
    // The button asks for its whole label and glyph, so its ring goes
    // around both.
    QQuickItem *compareLabel = itemNamed(compare, QStringLiteral("albumArtDetailsButtonLabel"));
    QVERIFY(compareLabel != nullptr);
    QVERIFY(!compareLabel->property("truncated").toBool());
    QVERIFY2(sceneRectOf(compare).contains(sceneRectOf(compareLabel))
                 && sceneRectOf(compare).contains(sceneRectOf(compareGlyph)),
             qPrintable(QStringLiteral("button %1..%2, label %3..%4")
                            .arg(sceneRectOf(compare).left()).arg(sceneRectOf(compare).right())
                            .arg(sceneRectOf(compareLabel).left()).arg(sceneRectOf(compareLabel).right())));
    QVERIFY(compare->width() >= compareLabel->implicitWidth() + compareGlyph->width());
    // It starts on the column's text edge, its glyph's box as large as the
    // glyph is drawn (13 px in from the edge, it read as a stray indent),
    // and the glyph says switch, not reload.
    QVERIFY2(std::abs(sceneRectOf(compareGlyph).left() - sceneRectOf(title).left()) <= 0.5,
             qPrintable(QStringLiteral("the glyph at %1, the text at %2")
                            .arg(sceneRectOf(compareGlyph).left()).arg(sceneRectOf(title).left())));
    QCOMPARE(compareGlyph->property("paintedWidth").toReal(), compareGlyph->width());
    QCOMPARE(QQmlExpression(qmlContext(compare), compare, QStringLiteral("icon.name")).evaluate().toString(),
             QStringLiteral("swap-panels"));
    for (const QString &theme : {QStringLiteral("breeze"), QStringLiteral("breeze-dark")}) {
        const QString file = QStandardPaths::locate(
            QStandardPaths::GenericDataLocation,
            QStringLiteral("icons/%1/actions/16/swap-panels.svg").arg(theme));
        QVERIFY2(!file.isEmpty(), qPrintable(QStringLiteral("%1 draws no 16 px swap-panels").arg(theme)));
    }
    QQuickItem *gap = itemNamed(compare, QStringLiteral("albumArtDetailsButtonFocusGap"));
    QVERIFY(gap != nullptr && gap->isVisible());
    QCOMPARE(borderColorOf(gap), strip);
}

void AlbumArtSmokeTest::albumArtCopyPathIsOnTheFoldersLineUnderClose_data() {
    QTest::addColumn<int>("viewerMode");
    QTest::addColumn<int>("points");
    QTest::newRow("window, 10 pt") << 0 << 10;
    QTest::newRow("window, 24 pt") << 0 << 24;
    QTest::newRow("whole screen, 10 pt") << 1 << 10;
    QTest::newRow("whole screen, 24 pt") << 1 << 24;
}

// Copy Path is attached to what it copies: on the "Folder" label's line, on
// one axis with Close above it, and the path has the column's width under
// them, clear of the button (it floated at the path's first line's end,
// 9 px off Close's axis).
void AlbumArtSmokeTest::albumArtCopyPathIsOnTheFoldersLineUnderClose() {
    QFETCH(int, viewerMode);
    QFETCH(int, points);
    const ScopedApplicationFont font(points);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 400, 400);
    QVERIFY(!coverPath.isEmpty());
    AlbumArtWindow ui;
    QVERIFY(ui.loaded());
    QQuickWindow *host = ui.openViewer(QUrl::fromLocalFile(coverPath).toString(), viewerMode);
    QVERIFY(host != nullptr);
    ui.viewer->setProperty("albumArtInfoVisible", true);
    ui.viewer->setProperty("albumArtViewerFileInfo", QVariantMap{
        {QStringLiteral("path"), QStringLiteral("/mnt/fixtures/library/The Quiet Meridian/2005 - Small Hours/cover.jpg")},
        {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
        {QStringLiteral("embeddedCover"), false},
    });
    QTRY_COMPARE(ui.surface->property("infoReveal").toReal(), 1.0);
    QQuickItem *label = nullptr;
    QTRY_VERIFY((label = visibleItemNamed(host, QStringLiteral("albumArtFolderLabel"))) != nullptr);
    QQuickItem *copy = visibleItemNamed(host, QStringLiteral("albumArtCopyPathButton"));
    QQuickItem *path = visibleItemNamed(host, QStringLiteral("albumArtFolderPath"));
    QQuickItem *row = visibleItemNamed(host, QStringLiteral("albumArtFolderRow"));
    QQuickItem *column = visibleItemNamed(host, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *close = visibleItemNamed(host, viewerMode == 1 ? QStringLiteral("albumArtFullscreenCloseButton")
                                                               : QStringLiteral("albumArtPopupCloseButton"));
    QVERIFY(copy && path && row && column && close);
    const auto boxOf = [](QQuickItem *item) {
        return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    };
    const auto describe = [&]() {
        return QStringLiteral("Copy Path %1,%2, Close %3, the label %4")
            .arg(boxOf(copy).center().x()).arg(boxOf(copy).center().y())
            .arg(boxOf(close).center().x()).arg(boxOf(label).center().y());
    };
    QTRY_VERIFY2(qAbs(boxOf(copy).center().x() - boxOf(close).center().x()) <= 0.5, qPrintable(describe()));
    QTRY_VERIFY2(qAbs(boxOf(copy).center().y() - boxOf(label).center().y()) <= 0.51, qPrintable(describe()));
    QVERIFY2(boxOf(label).right() < boxOf(copy).left(), qPrintable(describe()));
    QVERIFY2(boxOf(column).contains(boxOf(copy)), qPrintable(describe()));
    QTRY_COMPARE(boxOf(path).width(), boxOf(row).width());
    QVERIFY2(boxOf(path).top() >= boxOf(copy).bottom() - 0.01, qPrintable(describe()));
    QVERIFY(!path->property("elided").toBool());
    QVERIFY(ui.closeViewer(host));
}

void AlbumArtSmokeTest::albumArtPanShowsItsHand_data() {
    QTest::addColumn<int>("viewerMode");
    QTest::newRow("window") << 0;
    QTest::newRow("whole screen") << 1;
}

// Zoomed art shows the open hand, a closed one while it is dragged, and
// the open one again: the area over it that takes a press for the focus
// passes the art's pointer on (its own arrow covered both hands). The
// details and the fitted art show the arrow.
void AlbumArtSmokeTest::albumArtPanShowsItsHand() {
    QFETCH(int, viewerMode);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 400, 400);
    QVERIFY(!coverPath.isEmpty());
    AlbumArtWindow ui;
    QVERIFY(ui.loaded());
    QQuickWindow *host = ui.openViewer(QUrl::fromLocalFile(coverPath).toString(), viewerMode);
    QVERIFY(host != nullptr);
    const QPoint from(host->width() / 3, host->height() / 2);
    QTest::mouseMove(host, from + QPoint(1, 1));
    QTest::mouseMove(host, from);
    QTRY_COMPARE(host->cursor().shape(), Qt::ArrowCursor);

    ui.surface->setProperty("zoom", 4.0);
    QTest::mouseMove(host, from + QPoint(2, 2));
    QTRY_COMPARE(host->cursor().shape(), Qt::OpenHandCursor);
    QTest::mousePress(host, Qt::LeftButton, Qt::NoModifier, from);
    QTest::mouseMove(host, from - QPoint(40, 0));
    QTRY_COMPARE(host->cursor().shape(), Qt::ClosedHandCursor);
    QTest::mouseRelease(host, Qt::LeftButton, Qt::NoModifier, from - QPoint(40, 0));
    QTRY_COMPARE(host->cursor().shape(), Qt::OpenHandCursor);

    // The details over the art keep the arrow.
    ui.viewer->setProperty("albumArtInfoVisible", true);
    QTRY_COMPARE(ui.surface->property("infoReveal").toReal(), 1.0);
    QQuickItem *column = itemNamed(ui.surface, QStringLiteral("albumArtInfoColumn"));
    QVERIFY(column != nullptr);
    const QPointF details = column->mapToScene(QPointF(column->width() / 2, column->height() - 20));
    QTest::mouseMove(host, details.toPoint());
    QTest::mouseMove(host, details.toPoint() + QPoint(1, 1));
    QTRY_COMPARE(host->cursor().shape(), Qt::ArrowCursor);

    // Fitted again, the arrow.
    ui.surface->setProperty("zoom", 1.0);
    QTest::mouseMove(host, from + QPoint(5, 5));
    QTRY_COMPARE(host->cursor().shape(), Qt::ArrowCursor);
    QVERIFY(ui.closeViewer(host));
}

void AlbumArtSmokeTest::albumArtStripSetsTheZoomOffAsItsPlateDoes_data() {
    QTest::addColumn<int>("viewerMode");
    QTest::newRow("window") << 0;
    QTest::newRow("whole screen") << 1;
}

// The album art viewer's toolbar is one in both presentations: zoomed, a
// divider sets the readout and Reset off from its buttons on the plate and
// in the strip alike (the strip had none).
void AlbumArtSmokeTest::albumArtStripSetsTheZoomOffAsItsPlateDoes() {
    QFETCH(int, viewerMode);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 400, 400);
    QVERIFY(!coverPath.isEmpty());
    AlbumArtWindow ui;
    QVERIFY(ui.loaded());
    QQuickWindow *host = ui.openViewer(QUrl::fromLocalFile(coverPath).toString(), viewerMode);
    QVERIFY(host != nullptr);
    QVERIFY(visibleItemNamed(host, QStringLiteral("viewerControlsDivider")) == nullptr);
    ui.surface->setProperty("zoom", 2.0);
    QQuickItem *divider = nullptr;
    QTRY_VERIFY((divider = visibleItemNamed(host, QStringLiteral("viewerControlsDivider"))) != nullptr);
    QQuickItem *reset = visibleItemNamed(host, QStringLiteral("viewerResetButton"));
    QQuickItem *info = visibleItemNamed(host, viewerMode == 1 ? QStringLiteral("albumArtFullscreenInfoButton")
                                                              : QStringLiteral("albumArtPopupInfoButton"));
    QVERIFY(reset != nullptr && info != nullptr);
    QCOMPARE(sceneRectOf(divider).left() - sceneRectOf(reset).right(), 4.0);
    QCOMPARE(sceneRectOf(info).left() - sceneRectOf(divider).right(), 4.0);
    ui.surface->setProperty("zoom", 1.0);
    QVERIFY(ui.closeViewer(host));
}

std::unique_ptr<QmlSmokeTest> makeAlbumArtSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<AlbumArtSmokeTest>(userDirectoriesParent);
}

void AlbumArtSmokeTest::itunesReplaceShowsWhatWasChosen_data() {
    QTest::addColumn<qreal>("pointSize");
    QTest::newRow("10 pt") << 10.0;
    QTest::newRow("24 pt") << 24.0;
}

// Replace Album Art shows what was chosen, from the results and from a
// preview: the suggestion's thumbnail, title and artist under the question,
// from the row as it is, then the consequence and the warning on a line of
// its own. The question named only the album, over the chosen row.
void AlbumArtSmokeTest::itunesReplaceShowsWhatWasChosen() {
    QFETCH(qreal, pointSize);
    const ScopedApplicationFont font(pointSize);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    QVariantList rows = itunesArtworkFixture(dir, 3);
    QCOMPARE(rows.size(), 3);
    QVariantMap longTitled = rows.at(1).toMap();
    longTitled.insert(QStringLiteral("albumTitle"),
                      QStringLiteral("<b>Small Hours</b> (Twentieth Anniversary Deluxe Edition, Remastered from "
                                     "the Original Tapes, with the Lost Evening Sessions)"));
    rows[1] = longTitled;
    ItunesWindow ui(coverUrl);
    QVERIFY2(ui.loaded(), "Main.qml failed to show");
    const QVariantMap target{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("artist"), QStringLiteral("The Quiet Meridian")},
        {QStringLiteral("coverPath"), coverUrl},
    };
    QVERIFY(openItunesArtworkDialog(ui.window, ui.dialog, ui.fake, rows, target));
    QQuickWindow *question = replaceQuestion(ui.window);
    QVERIFY(question != nullptr);
    QObject *palette = qvariant_cast<QObject *>(ui.dialog->property("uiPalette"));
    QVERIFY(palette != nullptr);
    const auto checkShows = [&](int index) {
        QTRY_VERIFY(question->property("opened").toBool());
        QVERIFY(QTest::qWaitForWindowExposed(question));
        const QVariantMap row = rows.at(index).toMap();
        QQuickItem *message = visibleItemNamed(question, QStringLiteral("confirmDialogMessage"));
        QQuickItem *thumbnail = visibleItemNamed(question, QStringLiteral("replaceCoverChosenThumbnail"));
        QQuickItem *title = visibleItemNamed(question, QStringLiteral("replaceCoverChosenTitle"));
        QQuickItem *artist = visibleItemNamed(question, QStringLiteral("replaceCoverChosenArtist"));
        QQuickItem *consequence = visibleItemNamed(question, QStringLiteral("replaceCoverConsequence"));
        QQuickItem *outcome = visibleItemNamed(question, QStringLiteral("replaceCoverEmbedCaseOutcome"));
        QQuickItem *warning = visibleItemNamed(question, QStringLiteral("replaceCoverWarning"));
        QVERIFY(message != nullptr && thumbnail != nullptr && title != nullptr && artist != nullptr
                && consequence != nullptr && outcome != nullptr && warning != nullptr);
        QCOMPARE(thumbnail->property("source").toString(), row.value(QStringLiteral("previewSource")).toString());
        // Drawn, not the placeholder.
        QTRY_COMPARE(thumbnail->childItems().first()->property("status").toInt(), 1);
        QTRY_COMPARE(thumbnail->width(), std::round(48 * std::max(1.0, pointSize / 10)));
        QCOMPARE(thumbnail->height(), thumbnail->width());
        QCOMPARE(title->property("text").toString(), row.value(QStringLiteral("albumTitle")).toString());
        QCOMPARE(artist->property("text").toString(), row.value(QStringLiteral("artistName")).toString());
        for (QQuickItem *line : {title, artist, outcome, warning}) {
            QCOMPARE(line->property("textFormat").toInt(), int(Qt::PlainText));
            QTRY_VERIFY2(!line->property("truncated").toBool() && sceneRectOf(line).right() <= question->width(),
                         qPrintable(line->objectName()));
        }
        if (index == 1) {
            QTRY_VERIFY(title->property("lineCount").toInt() > 1);
        }
        QTRY_VERIFY(sceneRectOf(message).bottom() < sceneRectOf(thumbnail).top());
        QVERIFY(sceneRectOf(title).left() >= sceneRectOf(thumbnail).right() + 12 - 0.5);
        QTRY_VERIFY(sceneRectOf(consequence).top()
                    > std::max(sceneRectOf(thumbnail).bottom(), sceneRectOf(artist).bottom()));
        QTRY_VERIFY(sceneRectOf(warning).top() > sceneRectOf(outcome).bottom());
        QCOMPARE(sceneRectOf(warning).left(), sceneRectOf(consequence).left());
        QCOMPARE(warning->property("color").value<QColor>().rgba(),
                 palette->property("textSecondary").value<QColor>().rgba());
        QCOMPARE(artist->property("color").value<QColor>().rgba(),
                 palette->property("textSecondary").value<QColor>().rgba());
        // The safe choice is still the default.
        auto *cancel = qvariant_cast<QQuickItem *>(question->property("initialFocusItem"));
        QVERIFY(cancel != nullptr);
        QCOMPARE(cancel->objectName(), QStringLiteral("confirmDialogCancelButton"));
        // Display only: nothing asked for, and nothing written.
        QVERIFY(ui.fake->property("preparedItunesArtwork").toList().isEmpty());
        QVERIFY(ui.applied().isEmpty());
    };

    QVERIFY(QMetaObject::invokeMethod(ui.dialog, "requestSuggestionApply", Q_ARG(QVariant, 1)));
    checkShows(1);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(QMetaObject::invokeMethod(question, "reject"));
    QTRY_VERIFY(!question->isVisible());

    // Apply iTunes Artwork, in a preview of another.
    QVERIFY(QMetaObject::invokeMethod(ui.dialog, "requestSuggestionPreview", Q_ARG(QVariant, 2)));
    QTRY_VERIFY(ui.dialog->property("previewCandidateReady").toBool());
    QVERIFY(QMetaObject::invokeMethod(ui.dialog, "applyPreviewedSuggestion"));
    checkShows(2);
    QVERIFY(!QTest::currentTestFailed());
    QVERIFY(QMetaObject::invokeMethod(question, "reject"));
    QTRY_VERIFY(!question->isVisible());
}

#include "tst_qml_smoke_album_art.moc"
