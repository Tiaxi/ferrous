// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

#include <QFontMetricsF>
#include <QQmlContext>
#include <QQmlExpression>
#include <QTextOption>
#include <cmath>

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
    void albumArtViewerShowsThePreloadedCover_data();
    void albumArtViewerShowsThePreloadedCover();
    void itunesPreviewAppliesOrGoesBackToTheResults();
    void itunesPreviewButtonsTakeSpaceFromThePlaybackShortcut();
    void itunesPreviewButtonsStackWhenTheyDoNotFitSideBySide();
    void itunesPreviewButtonsAreInTheDetailsAsSoonAsTheyShow();
    void itunesPreviewCannotApplyACandidateThatIsNotReady();
    void itunesPreviewShowsTheSearchedAlbumsArtAfterPlaybackMovesOn();
    void itunesPreviewApplyShowsTheAppliedCoverAfterPlaybackMovesOn();
    void itunesAppliedCoverGivesWayWhenTheSameTrackPlaysAgain_data();
    void itunesAppliedCoverGivesWayWhenTheSameTrackPlaysAgain();
    void itunesArtworkResultsAreAFlatList();
    void itunesArtworkDialogShowsAPlaceholderWhileSearchingAndWithNothingFound();
    void albumArtDetailsLieOverTheArt();
    void albumArtDetailsLabelsStayWholeWithLargerFonts();
    void albumArtViewerShowsWhenItsImageCannotBeRead();
    void albumArtInfoButtonIsTintedOnlyWhileChecked();
    void albumArtStaysPutWhenTheDetailsOpen_data();
    void albumArtStaysPutWhenTheDetailsOpen();
};

void AlbumArtSmokeTest::albumArtZoomFollowsTheWheelDelta() {
    QQmlApplicationEngine engine;
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: 400
    height: 400
    visible: true
    property alias surface: surface

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: ""
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 400
        viewerDecodeHeight: 400
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, &errorText));
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
    engine.rootContext()->setContextProperty(
        QStringLiteral("testCoverUrl"), CoverImageProvider::urlForPath(coverPath));
    engine.rootContext()->setContextProperty(QStringLiteral("testViewportWidth"), viewportWidth);
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: testViewportWidth
    height: 400
    visible: true
    property alias surface: surface
    property int closeCount: 0

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testCoverUrl
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 400
        viewerDecodeHeight: 400
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() { closeCount += 1 }
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, &errorText));
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

} // namespace

void AlbumArtSmokeTest::albumArtViewerShowsThePreloadedCover_data() {
    QTest::addColumn<double>("scale");
    QTest::addColumn<int>("viewerMode");
    QTest::newRow("popup") << 1.0 << 0;
    QTest::newRow("popup at 2x") << 2.0 << 0;
    QTest::newRow("whole screen") << 1.0 << 1;
}

void AlbumArtSmokeTest::albumArtViewerShowsThePreloadedCover() {
    // The hidden preload decodes the current cover as the album art viewer
    // asks for it, so that opening the viewer decodes nothing more: the
    // cover is decoded once at the viewer's size.
    QFETCH(double, scale);
    QFETCH(int, viewerMode);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(300, 200, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    QVERIFY(cover.save(coverPath));

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    auto *provider = new RecordingCoverImageProvider;
    engine.addImageProvider(QStringLiteral("covers"), provider);
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    if (scale != 1.0) {
        QScreen *screen = hiDpiTestScreen(scale);
        if (screen == nullptr) {
            QSKIP("Needs the offscreen platform's HiDPI test screen at that scale");
        }
        QVERIFY(moveWindowToScreen(*window, screen));
    }
    if (std::abs(window->effectiveDevicePixelRatio() - scale) > 1e-9) {
        QSKIP("The window is at another scale");
    }
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    auto *preload = window->findChild<QQuickItem *>(QStringLiteral("albumArtViewerPreloadImage"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && preload != nullptr && surface != nullptr);
    QQuickItem *viewerImage = nullptr;
    QList<QQuickItem *> surfaceItems;
    collectQuickItems(surface, &surfaceItems);
    for (QQuickItem *each : surfaceItems) {
        if (each->inherits("QQuickImage")) {
            viewerImage = each;
            break;
        }
    }
    QVERIFY(viewerImage != nullptr);
    bridge->setProperty("viewerFullscreenMode", viewerMode);

    // Image.Ready is 1. The implicit size is in logical pixels.
    const auto readyWithTheCover = [scale](QQuickItem *image) {
        return image->property("status").toInt() == 1
            && QSizeF(image->property("implicitWidth").toReal(),
                      image->property("implicitHeight").toReal()) * scale == QSizeF(300, 200);
    };
    bridge->setProperty("currentTrackCoverPath", CoverImageProvider::urlForPath(coverPath));
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(preload), 5000);

    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewerForCurrentArt"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(viewerImage), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(readyWithTheCover(preload), 5000);
    QCOMPARE(viewerImage->property("sourceSize"), preload->property("sourceSize"));
    QCOMPARE(viewerImage->window()->effectiveDevicePixelRatio(), scale);

    // Give a second request time to reach the provider before counting.
    QTest::qWait(100);
    const QSize viewerRequest =
        viewerImage->property("sourceSize").toSize() * viewerImage->window()->effectiveDevicePixelRatio();
    const QList<QSize> requests = provider->requests();
    QVERIFY2(requests.count(viewerRequest) == 1,
             qPrintable(QStringLiteral("%1 requests at %2x%3")
                            .arg(requests.count(viewerRequest))
                            .arg(viewerRequest.width())
                            .arg(viewerRequest.height())));

    QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

namespace {

// A 1000 × 600 window of the album art viewer's view alone, showing the
// image at `coverPath` with its details.
QObject *createDetailsSurfaceWindow(QQmlApplicationEngine &engine, const QString &coverPath,
                                    QString *errorText) {
    engine.rootContext()->setContextProperty(QStringLiteral("testCoverUrl"),
                                             QUrl::fromLocalFile(coverPath).toString());
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
    width: 1000
    height: 600
    visible: true
    property alias surface: surface
    readonly property var tokens: surface.uiPalette

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testCoverUrl
        infoVisible: true
        initialViewToken: 0
        viewerDecodeWidth: 1000
        viewerDecodeHeight: 600
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, errorText);
}

} // namespace

void AlbumArtSmokeTest::albumArtDetailsLieOverTheArt() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 300, 300);
    QVERIFY(!coverPath.isEmpty());
    QQmlApplicationEngine engine;
    QString errorText;
    QScopedPointer<QObject> root(createDetailsSurfaceWindow(engine, coverPath, &errorText));
    QVERIFY2(root != nullptr, qPrintable(errorText));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window));
    auto *surface = qvariant_cast<QQuickItem *>(root->property("surface"));
    QObject *palette = qvariant_cast<QObject *>(root->property("tokens"));
    QVERIFY(surface != nullptr && palette != nullptr);
    const QString folder = QStringLiteral("/music/The Quiet Meridian/Small Hours (Remastered "
                                          "Deluxe Edition With Bonus Tracks)/Disc One");
    surface->setProperty("fileInfo", QVariantMap{
        {QStringLiteral("path"), folder + QStringLiteral("/cover.jpg")},
        {QStringLiteral("fileName"), QStringLiteral("cover.jpg")},
        {QStringLiteral("width"), 1400},
        {QStringLiteral("height"), 1400},
        {QStringLiteral("fileSizeText"), QStringLiteral("412 KiB")},
        {QStringLiteral("format"), QStringLiteral("JPEG")},
        {QStringLiteral("embeddedCover"), false},
    });

    // The art on the view colour, under a 280 px column behind a 1 px seam
    // that lies over the view's right edge: the art keeps the whole view.
    QQuickItem *floor = itemNamed(surface, QStringLiteral("albumArtFloor"));
    QQuickItem *viewport = itemNamed(surface, QStringLiteral("albumArtViewport"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *seam = itemNamed(surface, QStringLiteral("albumArtInfoSeam"));
    QQuickItem *backing = itemNamed(surface, QStringLiteral("albumArtInfoBacking"));
    QVERIFY(floor != nullptr && viewport != nullptr && column != nullptr && seam != nullptr
            && backing != nullptr);
    QCOMPARE(colorOf(floor, "color"), colorOf(palette, "view"));
    QTRY_COMPARE(sceneRectOf(column), QRectF(721, 0, 279, 600));
    QCOMPARE(sceneRectOf(seam), QRectF(720, 0, 1, 600));
    QCOMPARE(colorOf(seam, "color"), colorOf(palette, "seam"));
    QCOMPARE(sceneRectOf(backing), sceneRectOf(column));
    QCOMPARE(colorOf(backing, "color"), colorOf(palette, "view"));
    QCOMPARE(qAlpha(colorOf(backing, "color")), 255);
    QCOMPARE(sceneRectOf(viewport), QRectF(0, 0, 1000, 600));
    // The square art fits the view's height, centred in the whole view,
    // however the decode request is shaped.
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QVERIFY(image != nullptr);
    const auto paintedArt = [image]() {
        const QSizeF painted(image->property("paintedWidth").toReal(),
                             image->property("paintedHeight").toReal());
        const QPointF topLeft = image->mapToScene(
            QPointF((image->width() - painted.width()) / 2, (image->height() - painted.height()) / 2));
        return QRectF(topLeft, painted);
    };
    QTRY_COMPARE(paintedArt(), QRectF(200, 0, 600, 600));
    // The details draw over the art's right edge, which runs under them.
    const QImage shown = window->grabWindow().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(shown.pixel(700, 590), QColor(Qt::darkCyan).rgb());
    QCOMPARE(shown.pixel(760, 590), QColor::fromRgba(colorOf(palette, "view")).rgb());

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
    // At the body size, as the file's name, not a footnote's: the column
    // reads at fullscreen sizes too.
    const qreal bodySize = qvariant_cast<QFont>(title->property("font")).pointSizeF();
    for (const QString &name : {QStringLiteral("albumArtInfoLabel"), QStringLiteral("albumArtInfoValue"),
                                QStringLiteral("albumArtFolderLabel"), QStringLiteral("albumArtFolderPath")}) {
        for (QQuickItem *text : visibleItemsNamed(column, name)) {
            QCOMPARE(qvariant_cast<QFont>(text->property("font")).pointSizeF(), bodySize);
        }
    }
    QCOMPARE(visibleTextItems(column, QStringLiteral("Dimensions")).size(), 1);
    QCOMPARE(visibleTextItems(column, QStringLiteral("Source")).size(), 1);

    // The folder on its own line, then its path on a line of its own,
    // elided in the middle, with the whole path in its tooltip.
    QQuickItem *folderLabel = itemNamed(column, QStringLiteral("albumArtFolderLabel"));
    QQuickItem *path = itemNamed(column, QStringLiteral("albumArtFolderPath"));
    QQuickItem *copy = itemNamed(column, QStringLiteral("albumArtCopyPathButton"));
    QVERIFY(folderLabel != nullptr && path != nullptr && copy != nullptr);
    QVERIFY(folderLabel->isVisible() && path->isVisible() && copy->isVisible());
    QCOMPARE(folderLabel->property("text").toString(), QStringLiteral("Folder"));
    QTRY_VERIFY2(sceneRectOf(path).top() >= sceneRectOf(folderLabel).bottom(),
                 qPrintable(QStringLiteral("%1 %2").arg(sceneRectOf(path).top())
                                .arg(sceneRectOf(folderLabel).bottom())));
    QCOMPARE(path->property("text").toString(), folder);
    QCOMPARE(path->property("elide").toInt(), int(Qt::ElideMiddle));
    QTRY_VERIFY(path->property("truncated").toBool());
    QVERIFY(sceneRectOf(path).left() == sceneRectOf(folderLabel).left());
    QTest::mouseMove(window, centerInWindow(path));
    QTRY_COMPARE(visibleTextItems(window->contentItem(), folder).size(), 1);

    // Copy Path puts the folder on the clipboard.
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

    // Without the details, the art is where it was, and shows whole.
    surface->setProperty("infoVisible", false);
    QTRY_VERIFY(!column->isVisible());
    QVERIFY(!seam->isVisible());
    QCOMPARE(sceneRectOf(viewport), QRectF(0, 0, 1000, 600));
    QCOMPARE(paintedArt(), QRectF(200, 0, 600, 600));
    QTRY_COMPARE(window->grabWindow().convertToFormat(QImage::Format_RGB32).pixel(760, 590),
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
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 300, 300);
    QVERIFY(!coverPath.isEmpty());

    for (const qreal pointSize : {0.0, 14.0, 24.0}) {
        const QByteArray at = QByteArray("point size ") + QByteArray::number(pointSize);
        if (pointSize > 0) {
            QFont larger = applicationFont;
            larger.setPointSizeF(pointSize);
            QGuiApplication::setFont(larger);
        }
        QQmlApplicationEngine engine;
        QString errorText;
        QScopedPointer<QObject> root(createDetailsSurfaceWindow(engine, coverPath, &errorText));
        QVERIFY2(root != nullptr, qPrintable(errorText));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        auto *surface = qvariant_cast<QQuickItem *>(root->property("surface"));
        QObject *palette = qvariant_cast<QObject *>(root->property("tokens"));
        QVERIFY(surface != nullptr && palette != nullptr);
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
        // Once laid out, each row is the column's width inside its margins.
        QTRY_VERIFY2(std::all_of(rows.cbegin(), rows.cend(), [column](QQuickItem *row) {
                         return qFuzzyCompare(row->width(), column->width() - 32);
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
            // The body size, the application's font's.
            QCOMPARE(qvariant_cast<QFont>(label->property("font")).pointSizeF(),
                     palette->property("basePointSize").toReal());
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
        // the row inside the column's 16 px margins, and never narrower
        // than the widest word.
        const qreal cap = std::floor((column->width() - 32) * 0.4);
        const qreal labelWidth = qMax(std::ceil(widestWord), qMin(std::ceil(widest), cap));
        // Each item's own box: a text's bounding rect is only its painted text.
        const auto boxOf = [](QQuickItem *item) {
            return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        };
        const QRectF inside = boxOf(column).adjusted(15.5, 0, -15.5, 0);
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
            // A label and its value start on the same line, however many
            // lines either takes.
            QVERIFY2(qAbs(boxOf(label).top() - boxOf(value).top()) < 0.5,
                     qPrintable(QStringLiteral("%1; tops %2 and %3, value %4 lines")
                                    .arg(where).arg(boxOf(label).top()).arg(boxOf(value).top())
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

void AlbumArtSmokeTest::albumArtViewerShowsWhenItsImageCannotBeRead() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Cannot open: .*missing\\.png")));
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(
        QStringLiteral("testMissingUrl"),
        QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.png"))).toString());
    const QUrl baseUrl = QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/QmlSmokeHarness.qml"));
    QString errorText;
    QScopedPointer<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window 2.15
import "components" as Components
import "viewers" as Viewers

Window {
    id: surfaceWindow
    width: 600
    height: 400
    visible: true
    property alias surface: surface

    Action { id: replaceFromItunesAction }

    Viewers.AlbumArtSurface {
        id: surface
        anchors.fill: parent
        viewerOpen: true
        viewerSource: testMissingUrl
        infoVisible: false
        initialViewToken: 0
        viewerDecodeWidth: 600
        viewerDecodeHeight: 400
        replaceFromItunesAction: replaceFromItunesAction
        currentTrackItunesArtworkDisabledReason: function() { return "" }
        closeViewer: function() {}
        focusFullscreen: function() {}
        comparisonModeAvailable: false
        uiPalette: Components.UiPalette { windowRoot: surfaceWindow }
    }
}
)QML"), baseUrl, &errorText));
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

void AlbumArtSmokeTest::albumArtInfoButtonIsTintedOnlyWhileChecked() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir, 400, 400);
    QVERIFY(!coverPath.isEmpty());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && palette != nullptr && surface != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", 1);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QQuickWindow *fullscreen = viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
    QVERIFY(fullscreen != nullptr);
    QVERIFY(QTest::qWaitForWindowActive(fullscreen));
    QQuickItem *info = visibleItemNamed(fullscreen, QStringLiteral("albumArtFullscreenInfoButton"));
    QVERIFY(info != nullptr);
    QQuickItem *tint = itemNamed(info, QStringLiteral("viewerToolButtonChecked"));
    QQuickItem *ring = itemNamed(info, QStringLiteral("viewerToolButtonFocusRing"));
    QVERIFY(tint != nullptr && ring != nullptr);
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QVERIFY(column != nullptr);

    // Off: neither.
    QVERIFY(!viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(!tint->isVisible() && !ring->isVisible());
    QVERIFY(!column->isVisible());

    // A click shows the details: the tint, and no ring.
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(info));
    QTRY_VERIFY(viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(info->property("checked").toBool());
    QVERIFY(tint->isVisible());
    QCOMPARE(colorOf(tint, "color"), colorOf(palette, "selection"));
    QVERIFY(!ring->isVisible());
    QTRY_VERIFY(column->isVisible());

    // I hides them again, and the button follows.
    QTest::keyClick(fullscreen, Qt::Key_I);
    QTRY_VERIFY(!viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(!info->property("checked").toBool());
    QVERIFY(!tint->isVisible());

    // The keyboard reaches it: the ring for focus, and Space toggles it,
    // with both the tint and the ring.
    for (int press = 0; press < 8 && !info->hasActiveFocus(); ++press) {
        QTest::keyClick(fullscreen, Qt::Key_Tab);
    }
    QVERIFY(info->hasActiveFocus());
    QVERIFY(ring->isVisible());
    QVERIFY(!tint->isVisible());
    QTest::keyClick(fullscreen, Qt::Key_Space);
    QTRY_VERIFY(viewer->property("albumArtInfoVisible").toBool());
    QVERIFY(tint->isVisible() && ring->isVisible());

    // Zoomed, the strip has the readout and Reset, which fits the art again.
    QQuickItem *strip = visibleItemNamed(fullscreen, QStringLiteral("albumArtFullscreenStrip"));
    QVERIFY(strip != nullptr);
    QQuickItem *readout = itemNamed(strip, QStringLiteral("viewerZoomReadout"));
    QQuickItem *reset = itemNamed(strip, QStringLiteral("viewerResetButton"));
    QVERIFY(readout != nullptr && reset != nullptr);
    QVERIFY(!readout->isVisible() && !reset->isVisible());
    surface->setProperty("zoom", 2.5);
    // In place at once, without waiting for the window to draw.
    QVERIFY(readout->isVisible() && reset->isVisible());
    QVERIFY(clearOfTheOtherButtons(reset) && clearOfTheOtherButtons(readout)
            && sceneRectOf(readout).right() <= sceneRectOf(reset).left());
    QTRY_VERIFY(readout->isVisible());
    QCOMPARE(readout->property("text").toString(), QStringLiteral("Zoom 250%"));
    QTRY_VERIFY(clearOfTheOtherButtons(reset) && clearOfTheOtherButtons(readout)
                && sceneRectOf(readout).right() <= sceneRectOf(reset).left());
    QTest::mouseClick(fullscreen, Qt::LeftButton, Qt::NoModifier, centerInWindow(reset));
    QTRY_COMPARE(surface->property("zoom").toDouble(), 1.0);
    QTRY_VERIFY(!readout->isVisible());

    QTest::keyClick(fullscreen, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

void AlbumArtSmokeTest::albumArtStaysPutWhenTheDetailsOpen_data() {
    QTest::addColumn<int>("viewerMode");
    QTest::newRow("popup") << 0;
    QTest::newRow("whole screen") << 1;
}

void AlbumArtSmokeTest::albumArtStaysPutWhenTheDetailsOpen() {
    // The details lie over the art's right edge: opening or closing them
    // moves, zooms and pans the art not at all, and the pointer on them
    // works the details rather than the art beneath.
    QFETCH(int, viewerMode);
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Wider than the view, so that the art runs under the details.
    const QString coverPath = savedCover(dir, 1600, 800);
    QVERIFY(!coverPath.isEmpty());
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *palette = qvariant_cast<QObject *>(window->property("uiPalette"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && palette != nullptr && surface != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", QUrl::fromLocalFile(coverPath).toString());
    bridge->setProperty("viewerFullscreenMode", viewerMode);
    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    QTRY_VERIFY(viewer->property("albumArtViewerOpen").toBool());
    QQuickWindow *shownIn = window;
    if (viewerMode == 1) {
        shownIn = viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
        QVERIFY(shownIn != nullptr);
        QVERIFY(QTest::qWaitForWindowActive(shownIn));
    }
    QTRY_VERIFY(surface->window() == shownIn && surface->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(surface->property("imageStatus").toInt(), 1, 5000);
    QVERIFY(!viewer->property("albumArtInfoVisible").toBool());
    // The popup viewer has faded in, so the window draws it as it stays.
    QTRY_VERIFY(popupsSettled());

    QQuickItem *viewport = itemNamed(surface, QStringLiteral("albumArtViewport"));
    QQuickItem *image = itemNamed(surface, QStringLiteral("albumArtImage"));
    QQuickItem *column = itemNamed(surface, QStringLiteral("albumArtInfoColumn"));
    QQuickItem *seam = itemNamed(surface, QStringLiteral("albumArtInfoSeam"));
    QObject *contextMenu = surface->findChild<QObject *>(QStringLiteral("albumArtViewerContextMenu"));
    QVERIFY(viewport != nullptr && image != nullptr && column != nullptr && seam != nullptr
            && contextMenu != nullptr);
    QTRY_VERIFY(viewport->width() > 400 && viewport->height() > 200);
    const auto paintedArt = [image]() {
        const QSizeF painted(image->property("paintedWidth").toReal(),
                             image->property("paintedHeight").toReal());
        const QPointF origin((image->width() - painted.width()) / 2,
                             (image->height() - painted.height()) / 2);
        return QRectF(image->mapToScene(origin),
                      image->mapToScene(origin + QPointF(painted.width(), painted.height())));
    };
    struct View {
        QRectF art;
        double zoom;
        double panX;
        double panY;
    };
    const auto view = [&]() {
        return View{paintedArt(), surface->property("zoom").toDouble(),
                    surface->property("panX").toDouble(), surface->property("panY").toDouble()};
    };
    const auto sameView = [](const View &a, const View &b) {
        return a.art == b.art && a.zoom == b.zoom && a.panX == b.panX && a.panY == b.panY;
    };
    const auto describe = [](const View &v) {
        return QStringLiteral("art %1,%2 %3x%4 zoom %5 pan %6,%7")
            .arg(v.art.x()).arg(v.art.y()).arg(v.art.width()).arg(v.art.height())
            .arg(v.zoom).arg(v.panX).arg(v.panY);
    };
    const QRectF viewRect = sceneRectOf(viewport);

    // Zoomed and panned by the pointer on the art.
    const QPointF artPoint = viewRect.center() - QPointF(viewRect.width() / 4, 0);
    QVERIFY(sendWheel(shownIn, artPoint, QPoint(), QPoint(0, 600)));
    QVERIFY(surface->property("zoom").toDouble() > 1.0);
    QTest::mousePress(shownIn, Qt::LeftButton, Qt::NoModifier, artPoint.toPoint());
    QTest::mouseMove(shownIn, (artPoint + QPointF(40, 30)).toPoint());
    QTest::mouseMove(shownIn, (artPoint + QPointF(80, 60)).toPoint());
    QTest::mouseRelease(shownIn, Qt::LeftButton, Qt::NoModifier, (artPoint + QPointF(80, 60)).toPoint());
    QVERIFY(surface->property("panX").toDouble() != 0.0 || surface->property("panY").toDouble() != 0.0);
    const View before = view();

    // I shows the details over the view's right edge: the art stays put.
    QTest::keyClick(shownIn, Qt::Key_I);
    QTRY_VERIFY(viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(column->isVisible() && seam->isVisible());
    QVERIFY2(sameView(view(), before), qPrintable(describe(view()) + QStringLiteral(" / ")
                                                   + describe(before)));
    QCOMPARE(sceneRectOf(viewport), viewRect);
    const QRectF columnRect = sceneRectOf(column);
    QCOMPARE(columnRect.right(), viewRect.right());
    QCOMPARE(columnRect.top(), viewRect.top());
    QCOMPARE(columnRect.bottom(), viewRect.bottom());
    QCOMPARE(columnRect.width(), 279.0);
    QVERIFY(paintedArt().right() > columnRect.right());
    // The column draws over the art, on its own backing, and the art beside
    // it still shows.
    const QImage shown = shownIn->grabWindow().convertToFormat(QImage::Format_RGB32);
    const qreal dpr = shown.devicePixelRatio();
    const auto pixelAt = [&shown, dpr](QPointF point) {
        return shown.pixel(int(point.x() * dpr), int(point.y() * dpr));
    };
    // Low in the art that shows, below the details' text.
    const QRectF artShown = paintedArt().intersected(viewRect);
    QVERIFY2(artShown.bottom() - 8 > columnRect.top() + columnRect.height() / 2,
             qPrintable(QStringLiteral("%1").arg(artShown.bottom())));
    const qreal sampleY = artShown.bottom() - 8;
    QCOMPARE(pixelAt(QPointF(columnRect.center().x(), sampleY)),
             QColor::fromRgba(colorOf(palette, "view")).rgb());
    QCOMPARE(pixelAt(QPointF(columnRect.left() - 40, sampleY)), QColor(Qt::darkCyan).rgb());

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
    const QPoint onSeam = sceneRectOf(seam).center().toPoint();
    QTest::mouseDClick(shownIn, Qt::LeftButton, Qt::NoModifier, onSeam);
    sendWheel(shownIn, onSeam, QPoint(), QPoint(0, 360));
    QTest::mouseClick(shownIn, Qt::RightButton, Qt::NoModifier, onColumn);
    QTest::qWait(50);
    QVERIFY2(sameView(view(), before), qPrintable(describe(view()) + QStringLiteral(" / ")
                                                   + describe(before)));
    QVERIFY(!contextMenu->property("visible").toBool());
    QVERIFY(viewer->property("albumArtViewerOpen").toBool());
    QVERIFY(viewer->property("albumArtInfoVisible").toBool());

    // Closing them again leaves the art as it was.
    QTest::keyClick(shownIn, Qt::Key_I);
    QTRY_VERIFY(!viewer->property("albumArtInfoVisible").toBool());
    QTRY_VERIFY(!column->isVisible());
    QVERIFY2(sameView(view(), before), qPrintable(describe(view()) + QStringLiteral(" / ")
                                                   + describe(before)));
    QCOMPARE(sceneRectOf(viewport), viewRect);

    QTest::keyClick(shownIn, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
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
    property var preparedItunesArtwork: []
    property var appliedItunesArtwork: []
    readonly property string currentTrackPath: fake.real.currentTrackPath
    readonly property string currentTrackCoverPath: fake.real.currentTrackCoverPath
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
    function cachedImageFileDetails(path) { return fake.real.cachedImageFileDetails(path) }
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
    return QMetaObject::invokeMethod(window, "openItunesArtworkDialog")
        && QTest::qWaitFor([dialog]() { return dialog->property("opened").toBool(); })
        && QTest::qWaitForWindowExposed(qobject_cast<QQuickWindow *>(dialog));
}

// Drops what the offscreen platform says when a window's least size
// changes, as a dialog's does with a larger font: it does not support it.
void dropSizeHintMessages() {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().removeAll(QStringLiteral("This plugin does not support propagateSizeHints()"));
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
// a row: where the user sees it, not where the viewer opening might still
// move it from. No popup is still fading in or out, such as the dialog that
// opened the preview, which takes the click until it has gone.
bool clickPreviewButton(QQuickWindow *host, const QString &objectName) {
    QRectF last;
    const bool settled = QTest::qWaitFor([&]() {
        QQuickItem *column = visibleItemNamed(host, QStringLiteral("albumArtInfoColumn"));
        QQuickItem *button = visibleItemNamed(host, objectName);
        if (column == nullptr || button == nullptr || !button->isEnabled() || !isWithin(button, column)) {
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

// During a preview in `host`: its label is over the image's details and its
// buttons 12 px under them, Back to Results before Apply on one row; the art
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
    // The viewer can still be taking its size, as its window shows.
    const auto settled = [&]() {
        const QRectF rect = rectOf(column);
        return rect.contains(rectOf(label)) && rect.contains(rectOf(back))
            && rect.contains(rectOf(apply));
    };
    QTRY_VERIFY2(settled(), at.constData());
    const QRectF columnRect = rectOf(column);
    for (QQuickItem *item : {label, back, apply}) {
        QVERIFY2(isWithin(item, column), at.constData());
        QVERIFY2(columnRect.contains(rectOf(item)), at.constData());
    }
    // The label first, over the file's name; the buttons directly under the
    // details, 12 px below the last of them, Back to Results then Apply,
    // ending at the column's end: not pinned to its bottom edge.
    QQuickItem *title = visibleItemNamed(host, QStringLiteral("albumArtInfoTitle"));
    QVERIFY2(title != nullptr, at.constData());
    QVERIFY2(rectOf(label).bottom() <= rectOf(title).top(), at.constData());
    qreal detailsBottom = rectOf(title).bottom();
    for (const QString &name : {QStringLiteral("albumArtInfoRow"), QStringLiteral("albumArtCompareButton")}) {
        for (QQuickItem *item : visibleItemsNamed(column, name)) {
            detailsBottom = qMax(detailsBottom, rectOf(item).bottom());
        }
    }
    QTRY_VERIFY2(qAbs(rectOf(back).top() - (detailsBottom + 12.0)) <= 0.5,
                 qPrintable(QStringLiteral("%1: details end at %2, buttons start at %3")
                                .arg(QString::fromUtf8(at)).arg(detailsBottom).arg(rectOf(back).top())));
    QVERIFY2(rectOf(back).top() < columnRect.center().y(), at.constData());
    QCOMPARE(rectOf(back).top(), rectOf(apply).top());
    QVERIFY2(rectOf(back).right() <= rectOf(apply).left(), at.constData());
    QVERIFY2(columnRect.right() - rectOf(apply).right() <= 24.0, at.constData());
    // About 40 characters wide, 280 px with its seam at the default font,
    // up to half the view, or wider for the buttons; its contents 16 px in
    // from both edges.
    const QFont font = label->property("font").value<QFont>();
    const qreal fortyCharacters = std::ceil(QFontMetricsF(font).averageCharWidth() * 40) - 1;
    const qreal halfTheView = std::floor(column->parentItem()->width() / 2);
    QVERIFY2(column->width() >= qMax(279.0, qMin(halfTheView, fortyCharacters)) - 0.5,
             qPrintable(QStringLiteral("%1: %2 px").arg(QString::fromUtf8(at)).arg(column->width())));
    QCOMPARE(rectOf(label).left() - columnRect.left(), 16.0);
    QCOMPARE(columnRect.right() - rectOf(apply).right(), 16.0);
    // The file's name a step larger than the details: 12 pt to their 10.
    QQuickItem *value = visibleItemsNamed(column, QStringLiteral("albumArtInfoValue")).value(0);
    QVERIFY2(value != nullptr, at.constData());
    QCOMPARE(title->property("font").value<QFont>().pointSizeF(),
             value->property("font").value<QFont>().pointSizeF() * 1.2);
    // Apply is the default button, which Breeze tints; Back to Results is
    // a plain one.
    const auto isDefault = [](QQuickItem *button) {
        return QQmlExpression(qmlContext(button), button, QStringLiteral("Accessible.defaultButton"))
            .evaluate().toBool();
    };
    QVERIFY2(isDefault(apply), at.constData());
    QVERIFY2(!isDefault(back), at.constData());
    // The art fits the space left of the seam, 24 px clear of it, centred
    // there.
    QQuickItem *image = visibleItemNamed(host, QStringLiteral("albumArtImage"));
    QQuickItem *seam = visibleItemNamed(host, QStringLiteral("albumArtInfoSeam"));
    QVERIFY2(image != nullptr && seam != nullptr, at.constData());
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

// The Replace Album Art question: a window of its own (#373 H5), above the
// window that asks it.
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
    loading.insert(QStringLiteral("detailStatusText"), QStringLiteral("Loading high-resolution artwork..."));
    rows[1] = loading;
    QVariantMap failed = rows[2].toMap();
    failed.insert(QStringLiteral("assetError"), QStringLiteral("The artwork could not be downloaded."));
    failed.insert(QStringLiteral("detailStatusText"),
                  QStringLiteral("High-resolution artwork could not be loaded."));
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
    // Laid out in its window, three rows need no scroll bar.
    QTRY_COMPARE(itemNamed(content, QStringLiteral("itunesArtworkResultsView"))->property("scrollbarLane").toReal(),
                 0.0);
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
            QCOMPARE(rect->width(), row->width());
            QCOMPARE(sceneRectOf(rect).bottom(), sceneRectOf(row).bottom());
            ++hairlines;
        }
    }
    QCOMPARE(hairlines, 2);

    // Each row: a 64 px thumbnail, the title, and exactly one line of
    // details, rows as tall as the thumbnail with 8 px above and below.
    const QStringList expectedDetails{
        QStringLiteral("The Quiet Meridian · 1200 × 1200 · JPEG · 412 KiB"),
        QStringLiteral("The Quiet Meridian · Loading high-resolution artwork…"),
        QStringLiteral("The artwork could not be downloaded."),
    };
    for (int i = 0; i < rowItems.size(); ++i) {
        QQuickItem *row = rowItems[i];
        const QByteArray at = QByteArray("row ") + QByteArray::number(i);
        QCOMPARE(row->height(), 80.0);
        QQuickItem *thumbnail = itemNamed(row, QStringLiteral("itunesArtworkResultThumbnail"));
        QVERIFY2(thumbnail != nullptr, at.constData());
        QCOMPARE(sceneRectOf(thumbnail).size(), QSizeF(64, 64));
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
    QCOMPARE(sceneRectOf(currentThumbnail).size(), QSizeF(64, 64));
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
                 QStringLiteral("1200 × 1200 · JPEG · 38 KiB"));
    QCOMPARE(currentRow->height(), 80.0);
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
    QCOMPARE(sceneRectOf(heading).left() - sceneRectOf(frame).left(), 17.0);
    // Nothing to search for or wait on.
    QVERIFY(visibleItemsNamed(content, QStringLiteral("itunesArtworkPlaceholder")).isEmpty());
}

void AlbumArtSmokeTest::itunesArtworkDialogShowsAPlaceholderWhileSearchingAndWithNothingFound() {
    // While it searches, and when it found nothing, the results' frame
    // holds a placeholder message, as Kirigami's looks, with a busy
    // indicator while searching,
    // under the current art, which stays the dialog's first line.
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
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("Searching iTunes..."));
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
    QVERIFY(frame != nullptr && placeholder != nullptr && message != nullptr && busy != nullptr
            && results != nullptr && currentRow != nullptr);
    QQuickItem *icon = itemNamed(content, QStringLiteral("itunesArtworkPlaceholderIcon"));
    QVERIFY(icon != nullptr);

    const auto checkState = [&](bool searching, const QString &text) {
        const QByteArray at = searching ? "searching" : "nothing found";
        QTRY_VERIFY2(placeholder->isVisible(), at.constData());
        QVERIFY2(!results->isVisible(), at.constData());
        QCOMPARE(message->property("text").toString(), text);
        QCOMPARE(busy->isVisible(), searching);
        QCOMPARE(busy->property("running").toBool(), searching);
        // A placeholder message, as Kirigami's: its icon once nothing was
        // found, and a centred heading.
        QCOMPARE(icon->isVisible(), !searching);
        QCOMPARE(message->property("horizontalAlignment").toInt(), int(Qt::AlignHCenter));
        // In the results' frame, centred there.
        QVERIFY2(sceneRectOf(frame).contains(sceneRectOf(placeholder)), at.constData());
        QVERIFY2(qAbs(sceneRectOf(placeholder).center().y() - sceneRectOf(frame).center().y()) <= 1.0,
                 at.constData());
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
    checkState(true, QStringLiteral("Searching iTunes..."));

    bridge->setProperty("itunesArtworkLoading", false);
    bridge->setProperty("itunesArtworkStatusText", QStringLiteral("No iTunes artwork suggestions found."));
    checkState(false, QStringLiteral("No iTunes artwork suggestions found."));
}

void AlbumArtSmokeTest::itunesPreviewAppliesOrGoesBackToTheResults() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 12);
    QCOMPARE(fixture.size(), 12);
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
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
                    ? window : viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
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
                QCOMPARE(label->property("text").toString(), QStringLiteral("iTunes Candidate"));
                QCOMPARE(host->title(),
                         QStringLiteral("Morning Tide Field — iTunes Candidate — Ferrous Album Art"));
                QQuickItem *apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"));
                QVERIFY2(apply != nullptr, at.constData());
                QTRY_VERIFY2(apply->isEnabled(), at.constData());
                checkPreviewControlsInTheDetails(host, prefix, at);
                QTest::keyClick(host, Qt::Key_I);
                QCoreApplication::processEvents();
                QVERIFY2(viewer->property("albumArtInfoVisible").toBool(), at.constData());
                QVERIFY2(visibleItemNamed(host, QStringLiteral("albumArtInfoColumn")) != nullptr,
                         at.constData());
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
            QVERIFY(visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"))->isEnabled());
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
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QObject *playPause = window->findChild<QObject *>(QStringLiteral("playPauseAction"));
    auto *surface = window->findChild<QQuickItem *>(QStringLiteral("albumArtSurface"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr && playPause != nullptr
            && surface != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
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
                ? window : viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
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

void AlbumArtSmokeTest::itunesPreviewButtonsStackWhenTheyDoNotFitSideBySide() {
    // Back to Results and Apply stay whole inside the details column at
    // larger fonts, in the main window's viewer and over the whole screen.
    // The column is about 40 characters wide, so with a larger font it
    // widens with them; where half the view leaves no room for that, they
    // stack. At 24 pt, Back to Results is wider than the usual column: the
    // column widens for it, so it is not cut short (#535).
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverPath = savedCover(dir);
    QVERIFY(!coverPath.isEmpty());
    const QString coverUrl = QUrl::fromLocalFile(coverPath).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);

    for (const qreal pointSize : {0.0, 14.0, 24.0}) {
        if (pointSize > 0) {
            QFont larger = applicationFont;
            larger.setPointSizeF(pointSize);
            QGuiApplication::setFont(larger);
        }
        LibraryTreeModel libraryModel;
        QQmlApplicationEngine engine;
        QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
        QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
        QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
        QVERIFY(fake != nullptr);
        setPlayingTrack(bridge);
        bridge->setProperty("currentTrackCoverPath", coverUrl);
        const QVariantMap target{
            {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
            {QStringLiteral("coverPath"), coverUrl},
        };
        for (const int fullscreenMode : {0, 1}) {
            const QByteArray at = QByteArray("point size ") + QByteArray::number(pointSize)
                + QByteArray(", fullscreen ") + QByteArray::number(fullscreenMode);
            bridge->setProperty("viewerFullscreenMode", fullscreenMode);
            QVERIFY2(openItunesArtworkDialog(window, dialog, fake, fixture, target), at.constData());
            QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1)));
            QTRY_VERIFY2(viewer->property("albumArtPreviewActive").toBool()
                             && !dialog->property("visible").toBool(), at.constData());
            QQuickWindow *host = fullscreenMode == 0
                ? window : viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow"));
            QVERIFY2(host != nullptr && QTest::qWaitForWindowActive(host), at.constData());
            QQuickItem *column = nullptr;
            QQuickItem *back = nullptr;
            QQuickItem *apply = nullptr;
            QTRY_VERIFY2((column = visibleItemNamed(host, QStringLiteral("albumArtInfoColumn"))) != nullptr
                             && (back = visibleItemNamed(host, QStringLiteral("albumArtPreviewBackButton"))) != nullptr
                             && (apply = visibleItemNamed(host, QStringLiteral("albumArtPreviewApplyButton"))) != nullptr,
                         at.constData());
            if (pointSize > 0) {
                QCOMPARE(qvariant_cast<QFont>(back->property("font")).pointSizeF(), pointSize);
            }
            QCoreApplication::processEvents();
            const QRectF columnRect = sceneRectOf(column);
            // Both whole, inside the column with its margins, apart.
            QTRY_VERIFY2(columnRect.adjusted(15.5, 0, -15.5, 0).contains(sceneRectOf(back))
                             && columnRect.adjusted(15.5, 0, -15.5, 0).contains(sceneRectOf(apply)),
                         qPrintable(QStringLiteral("%1: column %2..%3, back %4..%5, apply %6..%7")
                                        .arg(QString::fromLatin1(at))
                                        .arg(columnRect.left()).arg(columnRect.right())
                                        .arg(sceneRectOf(back).left()).arg(sceneRectOf(back).right())
                                        .arg(sceneRectOf(apply).left()).arg(sceneRectOf(apply).right())));
            QVERIFY2(!sceneRectOf(back).intersects(sceneRectOf(apply)), at.constData());
            // Each has all the room it asks for, so the style draws its
            // label whole, not elided: Back to Results too at 24 pt.
            QVERIFY2(back->width() >= back->implicitWidth() - 0.5 && apply->width() >= apply->implicitWidth() - 0.5,
                     qPrintable(QStringLiteral("%1: back %2 of %3, apply %4 of %5")
                                    .arg(QString::fromLatin1(at)).arg(back->width()).arg(back->implicitWidth())
                                    .arg(apply->width()).arg(apply->implicitWidth())));
            // About 40 characters wide, 279 px and its seam with the usual
            // font, or wider for the widest button, up to half the view.
            const qreal fortyCharacters = std::ceil(
                QFontMetricsF(qvariant_cast<QFont>(back->property("font"))).averageCharWidth() * 40) - 1;
            const qreal widestButton = std::ceil(qMax(back->implicitWidth(), apply->implicitWidth())) + 32;
            const qreal halfTheView = std::floor(column->parentItem()->width() / 2);
            QCOMPARE(columnRect.width(), qMax(279.0, qMin(halfTheView, qMax(fortyCharacters, widestButton))));
            if (pointSize == 0) {
                QCOMPARE(columnRect.width(), 279.0);
            }
            // The footer's buttons, under the details, inside the column.
            QQuickItem *footer = visibleItemNamed(host, QStringLiteral("albumArtPreviewFooter"));
            QQuickItem *buttons = visibleItemNamed(host, QStringLiteral("albumArtPreviewButtons"));
            QVERIFY2(footer != nullptr && buttons != nullptr, at.constData());
            QVERIFY2(isWithin(back, buttons) && isWithin(apply, buttons) && isWithin(footer, column),
                     at.constData());
            QVERIFY2(sceneRectOf(footer).bottom() <= columnRect.bottom(), at.constData());
            QVERIFY2(sceneRectOf(footer).contains(sceneRectOf(back))
                         && sceneRectOf(footer).contains(sceneRectOf(apply)), at.constData());
            const bool fitsInARow = back->implicitWidth() + buttons->property("spacing").toReal()
                + apply->implicitWidth() <= columnRect.width() - 32;
            QCOMPARE(buttons->property("oneRow").toBool(), fitsInARow);
            if (fitsInARow) {
                // One row, Back to Results then Apply, at the column's end.
                QCOMPARE(sceneRectOf(back).top(), sceneRectOf(apply).top());
                QVERIFY(sceneRectOf(back).right() <= sceneRectOf(apply).left());
                QVERIFY(columnRect.right() - 16.5 <= sceneRectOf(apply).right());
            } else {
                // Stacked, Back to Results above Apply, each the column's width.
                QVERIFY2(pointSize > 0, "the usual font fits them in a row");
                QVERIFY(columnRect.contains(sceneRectOf(back).adjusted(0, -0.5, 0, 0)));
                QVERIFY(sceneRectOf(back).bottom() <= sceneRectOf(apply).top());
                QCOMPARE(sceneRectOf(back).width(), sceneRectOf(apply).width());
                QCOMPARE(sceneRectOf(back).width(), columnRect.width() - 32);
            }
            // Stacked only where the column has no more room: at half the
            // view, in a screen-filling viewer of the 800 px test screen at
            // 24 pt.
            if (!fitsInARow) {
                QCOMPARE(columnRect.width(), halfTheView);
            }
            if (pointSize >= 24.0 && fullscreenMode == 1) {
                QVERIFY2(!fitsInARow, "24 pt in an 800 px view does not fit them in a row");
            }
            QVERIFY(QMetaObject::invokeMethod(viewer, "closeAlbumArtViewer"));
            QTRY_VERIFY(dialog->property("opened").toBool());
            QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
            QTRY_VERIFY(!dialog->property("visible").toBool());
            QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
            window->requestActivate();
            QVERIFY(QTest::qWaitForWindowActive(window));
        }
        QVERIFY(finishIncubation(engine));
    }
    dropSizeHintMessages();
}

void AlbumArtSmokeTest::itunesPreviewButtonsAreInTheDetailsAsSoonAsTheyShow() {
    // Back to Results and Apply are in place inside the details the moment
    // a preview shows them, before any frame, so that nothing can find
    // Apply elsewhere: a candidate's preview after one of the current art,
    // which had Back to Results alone, as when it was laid out last. With
    // the usual font they share a row; at 24 pt in a narrow view they
    // stack, as in itunesPreviewButtonsStackWhenTheyDoNotFitSideBySide.
    const QFont applicationFont = QGuiApplication::font();
    const auto restoreFont = qScopeGuard([&] { QGuiApplication::setFont(applicationFont); });
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    QCOMPARE(fixture.size(), 3);

    for (const qreal pointSize : {0.0, 24.0}) {
        const bool stacked = pointSize > 0;
        if (stacked) {
            QFont larger = applicationFont;
            larger.setPointSizeF(pointSize);
            QGuiApplication::setFont(larger);
        }
        LibraryTreeModel libraryModel;
        QQmlApplicationEngine engine;
        QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
        QVERIFY2(window != nullptr, "Main.qml failed to show");
        QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
        QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
        QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
        QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
        QVERIFY(fake != nullptr);
        setPlayingTrack(bridge);
        bridge->setProperty("currentTrackCoverPath", coverUrl);
        // At 24 pt, over the whole 800 px test screen: too narrow a view for
        // the buttons side by side.
        bridge->setProperty("viewerFullscreenMode", stacked ? 1 : 0);
        const auto host = [&]() {
            return stacked ? viewerWindowNamed(engine, QStringLiteral("albumArtFullscreenWindow")) : window;
        };
        QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, QVariantMap{
            {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
            {QStringLiteral("album"), QStringLiteral("Small Hours")},
            {QStringLiteral("coverPath"), coverUrl},
        }));
        const auto backToTheResults = [&]() {
            QVERIFY(clickPreviewButton(host(), QStringLiteral("albumArtPreviewBackButton")));
            QTRY_VERIFY(dialog->property("opened").toBool());
            QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
        };
        // Where the preview's buttons are now, without letting the events run.
        const auto checkButtonsInPlace = [&host, stacked](const QByteArray &at) {
            QQuickWindow *window = host();
            QQuickItem *column = visibleItemNamed(window, QStringLiteral("albumArtInfoColumn"));
            QQuickItem *back = visibleItemNamed(window, QStringLiteral("albumArtPreviewBackButton"));
            QQuickItem *apply = visibleItemNamed(window, QStringLiteral("albumArtPreviewApplyButton"));
            QVERIFY2(column != nullptr && back != nullptr && apply != nullptr, at.constData());
            const QRectF columnRect = sceneRectOf(column);
            const QRectF inside = columnRect.adjusted(15.5, 0, -15.5, 0);
            const QRectF backRect = sceneRectOf(back);
            const QRectF applyRect = sceneRectOf(apply);
            const auto where = [&]() {
                return QStringLiteral("%1: column %2..%3, back %4..%5 at %6, apply %7..%8 at %9")
                    .arg(QString::fromLatin1(at))
                    .arg(inside.left()).arg(inside.right())
                    .arg(backRect.left()).arg(backRect.right()).arg(backRect.top())
                    .arg(applyRect.left()).arg(applyRect.right()).arg(applyRect.top());
            };
            QVERIFY2(inside.contains(backRect) && inside.contains(applyRect), qPrintable(where()));
            if (stacked) {
                // Back to Results above Apply, each the column's width.
                QVERIFY2(backRect.bottom() <= applyRect.top(), qPrintable(where()));
                for (const QRectF &rect : {backRect, applyRect}) {
                    QVERIFY2(qAbs(rect.left() - (columnRect.left() + 16)) <= 0.5, qPrintable(where()));
                    QVERIFY2(qAbs(rect.width() - (columnRect.width() - 32)) <= 0.5, qPrintable(where()));
                }
            } else {
                // One row, Back to Results then Apply, at the column's end.
                QVERIFY2(backRect.right() <= applyRect.left(), qPrintable(where()));
                QCOMPARE(backRect.top(), applyRect.top());
                QVERIFY2(inside.right() - applyRect.right() <= 0.5, qPrintable(where()));
            }
        };

        QTRY_COMPARE(dialog->property("currentArtworkSource").toString(), coverUrl);
        for (int round = 1; round <= 2; ++round) {
            const QByteArray at = QByteArray("point size ") + QByteArray::number(pointSize)
                + QByteArray(", round ") + QByteArray::number(round);
            // The current art: Back to Results alone.
            QVERIFY(QMetaObject::invokeMethod(dialog, "showPreview", Q_ARG(QVariant, -1)));
            QTRY_VERIFY2(viewer->property("albumArtPreviewActive").toBool(), at.constData());
            QVERIFY2(visibleItemNamed(host(), QStringLiteral("albumArtPreviewApplyButton")) == nullptr,
                     at.constData());
            backToTheResults();

            // A candidate: Apply joins it, both in place at once.
            QVERIFY(QMetaObject::invokeMethod(dialog, "requestSuggestionPreview", Q_ARG(QVariant, 1)));
            QVERIFY2(viewer->property("albumArtPreviewActive").toBool(), at.constData());
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
        }
        QVERIFY(QMetaObject::invokeMethod(dialog, "close"));
        QTRY_VERIFY(!dialog->property("visible").toBool());
        QVERIFY(finishIncubation(engine));
    }
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
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
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
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
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
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
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

void AlbumArtSmokeTest::itunesAppliedCoverGivesWayWhenTheSameTrackPlaysAgain_data() {
    QTest::addColumn<QString>("route");
    QTest::newRow("repeat one") << QStringLiteral("repeat one");
    QTest::newRow("queued twice") << QStringLiteral("queued twice");
    QTest::newRow("stop and play again") << QStringLiteral("stop and play again");
}

void AlbumArtSmokeTest::itunesAppliedCoverGivesWayWhenTheSameTrackPlaysAgain() {
    // After a cover is applied to album A while B plays, B playing again
    // by any route ends the applied cover, though B's path and art stay the
    // same. Playing on in the same play keeps it.
    QFETCH(QString, route);
    QTemporaryDir dir;
    QTemporaryDir otherDir;
    QVERIFY(dir.isValid() && otherDir.isValid());
    const QString coverUrl = QUrl::fromLocalFile(savedCover(dir)).toString();
    const QString otherCoverUrl = QUrl::fromLocalFile(savedCover(otherDir, 40, 40)).toString();
    const QVariantList fixture = itunesArtworkFixture(dir, 3);
    QCOMPARE(fixture.size(), 3);
    const QString appliedUrl = fixture.at(1).toMap().value(QStringLiteral("normalizedUrl")).toString();
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = loadActiveMainWindow(engine, libraryModel);
    QVERIFY2(window != nullptr, "Main.qml failed to show");
    QObject *bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
    QObject *viewer = window->findChild<QObject *>(QStringLiteral("viewerController"));
    QObject *dialog = window->findChild<QObject *>(QStringLiteral("itunesArtworkDialog"));
    QVERIFY(bridge != nullptr && viewer != nullptr && dialog != nullptr);
    QObject *fake = injectItunesArtworkFake(engine, dialog, bridge);
    QVERIFY(fake != nullptr);
    bridge->setProperty("viewerFullscreenMode", 0);
    const auto viewerSource = [viewer]() { return viewer->property("albumArtViewerSource").toString(); };
    const auto notify = [bridge](std::initializer_list<const char *> signalNames) {
        for (const char *name : signalNames) {
            QVERIFY(QMetaObject::invokeMethod(bridge, name));
        }
    };
    const QString bPath = QStringLiteral("/music/Elsewhere/01 Lantern Rows.flac");

    // The suggestions are for album A; playback then moves on to B, in
    // row 2 of the queue.
    setPlayingTrack(bridge);
    bridge->setProperty("currentTrackCoverPath", coverUrl);
    bridge->setProperty("queueLength", 5);
    bridge->setProperty("playingQueueIndex", 1);
    startPlay(bridge);
    notify({"trackIdentityChanged", "snapshotChanged"});
    QVERIFY(openItunesArtworkDialog(window, dialog, fake, fixture, QVariantMap{
        {QStringLiteral("trackPath"), QStringLiteral("/music/Morning Tide Field.flac")},
        {QStringLiteral("album"), QStringLiteral("Small Hours")},
        {QStringLiteral("coverPath"), coverUrl},
    }));
    bridge->setProperty("currentTrackPath", bPath);
    bridge->setProperty("currentTrackCoverPath", otherCoverUrl);
    bridge->setProperty("playingQueueIndex", 2);
    startPlay(bridge);
    notify({"trackIdentityChanged", "trackMetadataChanged", "snapshotChanged"});
    applyItunesCandidateFromPreview(window, dialog, fake, viewer, 1, appliedUrl, route.toUtf8());
    if (QTest::currentTestFailed()) {
        return;
    }

    // B plays on: a seek, a pause and a queue edit that keeps B playing,
    // moved up a row, keep the applied cover.
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
    QVERIFY(!viewer->property("albumArtViewerShowsCurrentTrack").toBool());

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
    QVERIFY(viewer->property("albumArtViewerShowsCurrentTrack").toBool());
    QVERIFY(!viewer->property("albumArtShowsAppliedCover").toBool());
    QCOMPARE(bridge->property("currentTrackPath").toString(), bPath);

    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!viewer->property("albumArtViewerOpen").toBool());
}

std::unique_ptr<QmlSmokeTest> makeAlbumArtSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<AlbumArtSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_album_art.moc"
