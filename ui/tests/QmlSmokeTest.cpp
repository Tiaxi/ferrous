// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

namespace {

QtMessageHandler &previousMessageHandler() {
    static QtMessageHandler handler = nullptr;
    return handler;
}

void captureQtMessage(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        QMutexLocker locker(&capturedMessageMutex());
        capturedMessages().push_back(message);
    }
    if (previousMessageHandler()) {
        previousMessageHandler()(type, context, message);
    } else {
        qt_message_output(type, context, message);
    }
}

} // namespace

void QmlSmokeTest::initTestCase() {
    previousMessageHandler() = qInstallMessageHandler(captureQtMessage);
    // Every area runs alone, so none may rely on another's registrations.
    registerFerrousQmlTypes();
}

void QmlSmokeTest::init() {
    clearCapturedMessages();
    // Each test starts its bridges with empty user directories, so none
    // restores the session, settings or library an earlier test saved.
    m_userDirectories.emplace(m_userDirectoriesParent);
    QVERIFY(m_userDirectories->isValid());
}

void QmlSmokeTest::cleanup() {
    // The test's bridges are destroyed by now.
    m_userDirectories.reset();
    const QString warnings = takeCapturedMessagesText();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings));
}

void QmlSmokeTest::cleanupTestCase() {
    qInstallMessageHandler(previousMessageHandler());
    previousMessageHandler() = nullptr;
}

void presentWithoutDrawing(QWindow &window) {
    WindowPresence *presence = WindowPresence::of(&window);
    presence->m_isVisible = [](const QWindow &) { return true; };
    presence->m_isExposed = [](const QWindow &) { return true; };
    presence->update();
}

void setTestExposed(QWindow &window, bool exposed) {
    for (WindowPresence *presence : window.findChildren<WindowPresence *>()) {
        if (presence->window() == &window) {
            presence->m_isExposed = [exposed](const QWindow &) { return exposed; };
        }
    }
    QExposeEvent event(exposed ? QRegion(QRect(QPoint(), window.size())) : QRegion());
    QCoreApplication::sendEvent(&window, &event);
}

void endTestUnexposedGrace(QWindow &window) {
    for (WindowPresence *presence : window.findChildren<WindowPresence *>()) {
        if (presence->window() == &window && presence->m_unexposedGrace.isActive()) {
            presence->m_unexposedGrace.stop();
            presence->unexposedGraceEnded();
        }
    }
}

int drawnCanvasFragments(QSGNode *node) {
    QSGNode *tilesRoot = node != nullptr ? node->childAtIndex(1) : nullptr;
    int fragments = 0;
    for (int i = 0; tilesRoot != nullptr && i < tilesRoot->childCount(); ++i) {
        const auto *fragment = static_cast<QSGSimpleTextureNode *>(tilesRoot->childAtIndex(i));
        if (!fragment->rect().isEmpty()) {
            ++fragments;
        }
    }
    return fragments;
}

void prepareHighRatePlayback(SpectrogramItem &item, int width, int height, int bins, bool rolling) {
    item.setWidth(width);
    item.setHeight(height);
    item.setDisplayMode(rolling ? 0 : 1);
    item.setPlaying(true);
    constexpr int columns = 12'000;
    constexpr int hop = 64;
    item.feedPrecomputedChunk(
        QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, 200'000, 384'000, hop, false, true, 1, true, 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = item.referenceHopLocked() / hop;
    }
    setTestPosition(item, 0.5);
}

QScreen *hiDpiTestScreen(double scale) {
    const QString name =
        QStringLiteral("ferrous-hidpi-%1").arg(static_cast<int>(std::lround(scale * 100.0)));
    const QList<QScreen *> screens = QGuiApplication::screens();
    const auto found = std::find_if(screens.begin(), screens.end(),
        [&name](const QScreen *screen) { return screen->name() == name; });
    return found != screens.end() ? *found : nullptr;
}

bool moveWindowToScreen(QQuickWindow &window, QScreen *screen) {
    window.setScreen(screen);
    window.setPosition(screen->geometry().topLeft() + QPoint(16, 16));
    return QTest::qWaitFor([&window, screen]() {
        return window.screen() == screen
            && std::abs(window.effectiveDevicePixelRatio() - screen->devicePixelRatio()) < 1e-9;
    }, 5000);
}

bool showOnScreen(QQuickWindow &window, double scale) {
    QScreen *screen = hiDpiTestScreen(scale);
    if (screen == nullptr) {
        return false;
    }
    window.resize(240, 160);
    if (!moveWindowToScreen(window, screen)) {
        return false;
    }
    window.show();
    return QTest::qWaitForWindowExposed(&window, 5000)
        && std::abs(window.effectiveDevicePixelRatio() - scale) < 1e-9;
}

OverlayOnScreen overlayOnScreen(QSGNode *node, int layer) {
    OverlayOnScreen shown;
    QSGNode *root = node->childAtIndex(layer);
    QSGNode *lines = root->childAtIndex(0);
    QSGNode *images = root->childAtIndex(1);
    for (int i = 0; i < lines->childCount(); ++i) {
        const auto *line = static_cast<QSGSimpleRectNode *>(lines->childAtIndex(i));
        if (!line->rect().isEmpty()) {
            shown.lines.push_back(line->rect());
        }
    }
    for (int i = 0; i < images->childCount(); ++i) {
        const auto *image = static_cast<QSGSimpleTextureNode *>(images->childAtIndex(i));
        if (!image->rect().isEmpty() && image->texture() != nullptr) {
            shown.images.push_back({image->rect(), image->texture()->textureSize()});
        }
    }
    return shown;
}

bool sendPendingBackendZoom(SpectrogramItem &item) {
    return QTest::qWaitFor(
        [&item]() { return !item.m_zoomDebounceTimer->isActive(); }, 1000);
}

void rebuildCanvasNow(
    SpectrogramItem &item, int width, int height,
    qint64 displayLeft, qint64 displayRight, bool rollingMode) {
    const auto job = item.makeRasterJobLocked(
        width, height, displayLeft, displayRight, rollingMode);
    const auto result = job ? SpectrogramItem::rasterize(*job) : nullptr;
    if (!result || !item.installRasterResultLocked(*result)) {
        item.invalidateCanvas();
        return;
    }
    item.m_precomputedCanvasDirty = false;
}

QImage canvasInViewOrder(const SpectrogramItem &item) {
    const QImage &canvas = item.m_canvas;
    const int filled = std::min(item.m_canvasFilledCols, canvas.width());
    const int start = (item.m_canvasWriteX - filled + canvas.width()) % canvas.width();
    QImage unwrapped(filled, canvas.height(), canvas.format());
    for (int x = 0; x < filled; ++x) {
        for (int y = 0; y < canvas.height(); ++y) {
            unwrapped.setPixel(x, y, canvas.pixel((start + x) % canvas.width(), y));
        }
    }
    return unwrapped;
}

bool spectrogramRasterPending(SpectrogramItem &item) {
    QMutexLocker lock(&item.m_stateMutex);
    return item.m_rasterPending;
}

QSGNode *paintSettled(SpectrogramItem &item, QSGNode *node) {
    node = item.updatePaintNode(node, nullptr);
    for (int round = 0; round < 8 && spectrogramRasterPending(item); ++round) {
        if (!QTest::qWaitFor([&item]() { return !spectrogramRasterPending(item); }, 5000)) {
            break;
        }
        {
            // Its window may have painted it meanwhile, taking what changed
            // of its canvas for the window's own node: this node needs it too.
            QMutexLocker lock(&item.m_stateMutex);
            item.markCanvasDirtyLocked();
        }
        node = item.updatePaintNode(node, nullptr);
    }
    return node;
}

void appendUtf8(QByteArray &out, const QString &value) {
    const QByteArray utf8 = value.toUtf8();
    appendLe<quint32>(out, static_cast<quint32>(utf8.size()));
    out.append(utf8);
}

QByteArray encodeRows(const QVector<BinaryTreeRow> &rows) {
    QByteArray out;
    appendLe<quint32>(out, static_cast<quint32>(rows.size()));
    for (const BinaryTreeRow &row : rows) {
        out.append(static_cast<char>(row.rowType));
        appendLe<quint16>(out, row.depth);
        appendLe<qint32>(out, row.sourceIndex);
        appendLe<quint32>(out, row.trackNumber);
        appendLe<quint32>(out, row.childCount);
        appendLe<qint32>(out, row.year);
        appendUtf8(out, row.title);
        appendUtf8(out, row.key);
        appendUtf8(out, row.artist);
        appendUtf8(out, row.path);
        appendUtf8(out, row.coverPath);
        appendLe<quint64>(out, row.coverStamp);
        appendUtf8(out, row.trackPath);
        appendLe<quint32>(out, static_cast<quint32>(row.playPaths.size()));
        for (const QString &playPath : row.playPaths) {
            appendUtf8(out, playPath);
        }
    }
    return out;
}

QByteArray sampleArtistAlbumTreeBinary(quint64 albumCoverStamp) {
    const QString trackPath = QStringLiteral("/music/artist/album/track01.flac");
    QVector<BinaryTreeRow> rows;
    rows.push_back(BinaryTreeRow{
        1,
        0,
        -1,
        0,
        1,
        QStringLiteral("Artist A (1)"),
        QStringLiteral("artist|Artist A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A"),
        {},
        {},
        {},
    });
    rows.push_back(BinaryTreeRow{
        2,
        1,
        0,
        0,
        1,
        QStringLiteral("Album A"),
        QStringLiteral("album|Artist A|Album A"),
        QStringLiteral("Artist A"),
        QStringLiteral("/music/Artist A/Album A"),
        QStringLiteral("/music/Artist A/Album A/cover.jpg"),
        {},
        {},
        albumCoverStamp,
    });
    rows.push_back(BinaryTreeRow{
        4,
        2,
        -1,
        1,
        0,
        QStringLiteral("01 - Track 01"),
        QStringLiteral("track|/music/artist/album/track01.flac"),
        QStringLiteral("Artist A"),
        trackPath,
        {},
        trackPath,
        QStringList{trackPath},
    });
    return encodeRows(rows);
}

QString formatQmlErrors(const QList<QQmlError> &errors) {
    QStringList lines;
    lines.reserve(errors.size());
    for (const QQmlError &error : errors) {
        lines.push_back(error.toString());
    }
    return lines.join('\n');
}

QMutex &capturedMessageMutex() {
    static QMutex mutex;
    return mutex;
}

QStringList &capturedMessages() {
    static QStringList messages;
    return messages;
}

void clearCapturedMessages() {
    QMutexLocker locker(&capturedMessageMutex());
    capturedMessages().clear();
}

QString takeCapturedMessagesText() {
    QMutexLocker locker(&capturedMessageMutex());
    const QString text = capturedMessages().join('\n');
    capturedMessages().clear();
    return text;
}

QObject *findObjectByStringProperty(QObject *root, const char *propertyName, const QString &expectedValue) {
    if (!root) {
        return nullptr;
    }
    if (root->property(propertyName).toString() == expectedValue) {
        return root;
    }
    const QObjectList children = root->children();
    for (QObject *child : children) {
        if (QObject *match = findObjectByStringProperty(child, propertyName, expectedValue)) {
            return match;
        }
    }
    return nullptr;
}

QQuickItem *findQuickItemByObjectName(QQuickItem *root, const QString &objectName) {
    if (root->objectName() == objectName) {
        return root;
    }
    for (QQuickItem *child : root->childItems()) {
        if (QQuickItem *match = findQuickItemByObjectName(child, objectName)) {
            return match;
        }
    }
    return nullptr;
}

QObject *createQmlObjectFromSource(
    QQmlEngine &engine,
    const QByteArray &qmlSource,
    const QUrl &baseUrl,
    QString *errorText) {
    QQmlComponent component(&engine);
    component.setData(qmlSource, baseUrl);
    if (!component.errors().isEmpty()) {
        if (errorText) {
            *errorText = formatQmlErrors(component.errors());
        }
        return nullptr;
    }

    QObject *object = component.create();
    if (!object && errorText) {
        *errorText = formatQmlErrors(component.errors());
    }
    return object;
}

QQuickWindow *loadMainWindowShown(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel) {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    return window;
}

QStringList errorHistoryMessages(QObject *errors) {
    QStringList messages;
    auto *entries = qvariant_cast<QAbstractItemModel *>(errors->property("entries"));
    if (entries == nullptr) {
        return messages;
    }
    const int messageRole = entries->roleNames().key("message");
    for (int row = 0; row < entries->rowCount(); ++row) {
        messages.push_back(entries->index(row, 0).data(messageRole).toString());
    }
    return messages;
}

QList<QQuickItem *> visibleTextItems(QQuickItem *root, const QString &text) {
    QList<QQuickItem *> found;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible()) {
            return;
        }
        if (item->property("text").toString() == text && item->property("lineCount").isValid()) {
            found.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    visit(root);
    return found;
}

QList<QQuickItem *> itemsNamed(QQuickItem *root, const QString &objectName) {
    QList<QQuickItem *> found;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (item->objectName() == objectName) {
            found.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    visit(root);
    return found;
}

QQuickItem *itemNamed(QQuickItem *root, const QString &objectName) {
    const QList<QQuickItem *> found = itemsNamed(root, objectName);
    return found.isEmpty() ? nullptr : found.first();
}

bool clearOfTheOtherButtons(QQuickItem *button) {
    QQuickItem *parentRow = button->parentItem();
    const QRectF own = sceneRectOf(button);
    if (parentRow == nullptr || !sceneRectOf(parentRow).contains(own)
        || own.left() < 0 || own.right() > button->window()->width()) {
        return false;
    }
    QQuickItem *row = button->parentItem();
    while (row != nullptr && row->parentItem() != nullptr
           && !row->objectName().contains(QStringLiteral("Controls"))
           && !row->objectName().contains(QStringLiteral("Strip"))) {
        row = row->parentItem();
    }
    if (row == nullptr) {
        return false;
    }
    const QRectF rect = sceneRectOf(button);
    const std::function<bool(QQuickItem *)> overlaps = [&](QQuickItem *item) {
        if (!item->isVisible()) {
            return false;
        }
        if (item != button && item->property("flat").isValid()
            && sceneRectOf(item).intersects(rect)) {
            return true;
        }
        for (QQuickItem *child : item->childItems()) {
            if (overlaps(child)) {
                return true;
            }
        }
        return false;
    };
    return !overlaps(row);
}

QPoint centerInWindow(QQuickItem *item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

bool finishIncubation(QQmlEngine &engine) {
    QQmlIncubationController *controller = engine.incubationController();
    if (controller == nullptr) {
        return true;
    }
    return QTest::qWaitFor([controller]() { return controller->incubatingObjectCount() == 0; });
}

QQuickWindow *loadMainWindowForQuit(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel) {
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    engine.rootContext()->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    engine.rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral("test"));
    engine.load(QUrl::fromLocalFile(
        QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    return window;
}

bool clickButton(QObject *window, const QString &objectName) {
    QObject *button = window->findChild<QObject *>(objectName);
    return button != nullptr && QMetaObject::invokeMethod(button, "clicked");
}

void callMprisQuit() {
    MprisController mpris(nullptr);
    mpris.quitApplication();
}

QQuickItem *shownItemNamed(QQuickItem *root, const QString &objectName) {
    for (QQuickItem *child : root->childItems()) {
        if (!child->isVisible()) {
            continue;
        }
        if (child->objectName() == objectName) {
            return child;
        }
        if (QQuickItem *found = shownItemNamed(child, objectName)) {
            return found;
        }
    }
    return nullptr;
}

void giveSampleDetail(WaveformEditorItem &item, int rate, int channels, double start, double end,
    bool sameSource) {
    const qint64 firstFrame = std::llround(start * rate);
    const int points = static_cast<int>(std::llround((end - start) * rate));
    if (!sameSource) item.clearDetailLocked();
    item.m_sampleRateHz = rate;
    item.m_channelCount = channels;
    auto &detail = item.m_detail;
    detail = {};
    detail.sampleRateHz = rate;
    detail.channelCount = channels;
    detail.startSeconds = static_cast<double>(firstFrame) / rate;
    detail.endSeconds = static_cast<double>(firstFrame + points) / rate;
    detail.framesPerPoint = 1;
    detail.pointCount = points;
    detail.extrema.resize(static_cast<std::size_t>(points) * channels * 2);
    for (int point = 0; point < points; ++point) {
        const double frame = static_cast<double>(firstFrame + point);
        for (int channel = 0; channel < channels; ++channel) {
            // Off the lane's center line, which is drawn over the waveform.
            const auto value = static_cast<float>(
                0.5 + 0.35 * std::sin(frame * 0.05 * (1.0 + 0.2 * channel) + channel));
            const std::size_t index = (static_cast<std::size_t>(point) * channels + channel) * 2;
            detail.extrema[index] = value;
            detail.extrema[index + 1] = value;
        }
    }
}

QString lanesWithEmptyColumns(const QImage &image, int lanes, int skipFirst, int skipLast) {
    const QImage pixels = image.convertToFormat(QImage::Format_RGB32);
    QString result;
    for (int lane = 0; lane < lanes; ++lane) {
        const int top = lane * pixels.height() / lanes + 1;
        const int bottom = (lane + 1) * pixels.height() / lanes - 1;
        // Raw pixels and flags, as an unoptimized build checks millions.
        std::vector<unsigned char> drawn(static_cast<std::size_t>(pixels.width()), 0);
        unsigned char *const flags = drawn.data();
        const int width = pixels.width();
        int left = width;
        for (int y = top; y < bottom && left > 0; ++y) {
            const auto *row = reinterpret_cast<const QRgb *>(pixels.constScanLine(y));
            for (int x = 0; x < width; ++x) {
                const QRgb pixel = row[x];
                const auto green = static_cast<int>((pixel >> 8) & 0xffU);
                const auto red = static_cast<int>((pixel >> 16) & 0xffU);
                // As isWaveformPixel.
                if (flags[x] == 0 && green >= 70 && green >= red + 40) {
                    flags[x] = 1;
                    --left;
                }
            }
        }
        int empty = 0;
        int first = -1;
        for (int x = 0; x < width; ++x) {
            if (flags[x] != 0 || (x >= skipFirst && x <= skipLast)) continue;
            if (first < 0) first = x;
            ++empty;
        }
        if (empty > 0) {
            result += QStringLiteral("lane %1: %2 of %3 columns empty from %4; ")
                .arg(lane).arg(empty).arg(pixels.width()).arg(first);
        }
    }
    return result;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void prepareWholeTrackPlayback(SpectrogramItem &item, double position) {
    item.setWidth(400);
    item.setHeight(100);
    item.setDisplayMode(1);
    constexpr int bins = 64;
    constexpr int hop = 20'480;
    constexpr int columns = 380;
    item.feedPrecomputedChunk(QByteArray(bins * columns, '\x50'), bins, 0, columns,
        0, columns, 48'000, hop, true, true, 1, true, 1);
    {
        QMutexLocker lock(&item.m_stateMutex);
        item.m_renderZoomLevel = 1024.0 / hop;
    }
    setTestPosition(item, position);
    item.setPlaying(true);
    setTestPosition(item, position);
}

bool playbackFrameScheduled(const SpectrogramItem &item) {
    return item.m_frameScheduler != nullptr && item.m_frameScheduler->dueNs(&item) >= 0;
}

void feedColumnStripes(SpectrogramItem &item) {
    QByteArray data(kStripeBins * kStripeColumns, Qt::Uninitialized);
    for (int column = 0; column < kStripeColumns; ++column) {
        std::fill_n(data.data() + static_cast<qsizetype>(column) * kStripeBins, kStripeBins,
            static_cast<char>(40 + (column * 53) % 180));
    }
    item.feedPrecomputedChunk(data, kStripeBins, 0, kStripeColumns, 0, kStripeColumns,
        48'000, 1024, true, true, 1, true, 1);
}

void prepareScrollingPlayback(SpectrogramItem &item, TestTime &time, int mode, double height) {
    time.drive(item);
    item.setWidth(400);
    item.setHeight(height);
    item.setDisplayMode(mode);
    feedColumnStripes(item);
    setTestPosition(item, 40.0);
    item.setPlaying(true);
    setTestPosition(item, 40.0);
}

void addScrollingRows() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<double>("scale");
    for (const double scale : {1.0, 1.25, 1.5, 2.0}) {
        QTest::addRow("rolling %gx", scale) << 0 << scale;
        QTest::addRow("centered %gx", scale) << 1 << scale;
    }
}

QQuickWindow *loadActiveMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel) {
    QQuickWindow *window = loadMainWindowForQuit(engine, libraryModel);
    if (window == nullptr) {
        return nullptr;
    }
    window->requestActivate();
    return QTest::qWaitForWindowActive(window) ? window : nullptr;
}

QQuickWindow *viewerWindowNamed(QQmlApplicationEngine &engine, const QString &name) {
    return qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst()->findChild<QObject *>(name));
}

bool sendWheel(QQuickWindow *window, QPointF scenePosition, QPoint pixelDelta, QPoint angleDelta) {
    QWheelEvent event(scenePosition, window->mapToGlobal(scenePosition),
                      pixelDelta, angleDelta, Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QSpontaneKeyEvent::setSpontaneous(&event);
    QCoreApplication::sendEvent(window, &event);
    return event.isAccepted();
}

QQuickItem *visibleItemNamed(QQuickWindow *window, const QString &objectName) {
    const std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
        if (item->objectName() == objectName && item->isVisible()) {
            return item;
        }
        for (QQuickItem *child : item->childItems()) {
            if (QQuickItem *found = find(child)) {
                return found;
            }
        }
        return nullptr;
    };
    return find(window->contentItem());
}

bool anyMenuVisible(QObject *root, const QString &objectName) {
    const QList<QObject *> menus = root->findChildren<QObject *>(objectName);
    return std::any_of(menus.cbegin(), menus.cend(), [](QObject *menu) {
        return menu->property("visible").toBool();
    });
}

QRectF sceneRectOf(const QQuickItem *item) {
    return item->mapRectToScene(item->boundingRect());
}

QList<QQuickItem *> visibleItemsNamed(QQuickItem *root, const QString &objectName) {
    QList<QQuickItem *> found;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (item->objectName() == objectName && item->isVisible()) {
            found.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    visit(root);
    return found;
}

QQuickItem *firstVisibleItemNamed(QQuickItem *root, const QString &objectName) {
    const QList<QQuickItem *> found = visibleItemsNamed(root, objectName);
    return found.isEmpty() ? nullptr : found.constFirst();
}

void setPlayingTrack(QObject *bridge) {
    bridge->setProperty("currentTrackTitle", QStringLiteral("Morning Tide Field"));
    bridge->setProperty("currentTrackArtist", QStringLiteral("The Quiet Meridian"));
    bridge->setProperty("currentTrackAlbum", QStringLiteral("Small Hours"));
    bridge->setProperty("currentTrackPath", QStringLiteral("/music/Morning Tide Field.flac"));
    bridge->setProperty("durationText", QStringLiteral("03:49"));
    bridge->setProperty("durationSeconds", 229.0);
    bridge->setProperty("playbackState", QStringLiteral("Playing"));
}

QString savedCover(const QTemporaryDir &dir, int width, int height) {
    const QString path = dir.filePath(QStringLiteral("cover.png"));
    QImage cover(width, height, QImage::Format_RGB32);
    cover.fill(Qt::darkCyan);
    return cover.save(path) ? path : QString();
}

QRgb colorOf(QObject *object, const char *name) {
    return object->property(name).value<QColor>().rgba();
}

bool tabTo(QQuickWindow *host, QQuickItem *stop) {
    for (int press = 0; press < 24 && host->activeFocusItem() != stop; ++press) {
        QTest::keyClick(host, Qt::Key_Tab);
    }
    return host->activeFocusItem() == stop;
}

bool clickVisibleButton(QQuickWindow *window, const QString &objectName) {
    QQuickItem *button = visibleItemNamed(window, objectName);
    if (button == nullptr || !button->isEnabled()) {
        return false;
    }
    const QPointF center = button->mapToScene(QPointF(button->width() / 2, button->height() / 2));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center.toPoint());
    return true;
}

// Compiled here, after QmlSmokeTest.h opened the items' private members to
// the tests, not in the build's mocs_compilation.cpp, where an earlier moc file
// would have included their headers without that.
#include "moc_QmlSmokeTest.cpp"
