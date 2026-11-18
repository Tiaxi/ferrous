// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCoreApplication>
#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QtTest/QtTest>

#include <functional>
#include <initializer_list>

#include "../src/LibraryTreeModel.h"

// Loading the main window from the QML sources, with its fallback bridge,
// the application font a test sets for its run, and a test's checks of one
// window run in turn.

// The application font at `pointSize` until the end of the scope, then the
// font it was; 0 or less keeps the font as it is.
class ScopedApplicationFont {
public:
    explicit ScopedApplicationFont(qreal pointSize)
        : m_font(QGuiApplication::font()) {
        if (pointSize > 0) {
            QFont font = m_font;
            font.setPointSizeF(pointSize);
            QGuiApplication::setFont(font);
        }
    }
    ~ScopedApplicationFont() { QGuiApplication::setFont(m_font); }
    ScopedApplicationFont(const ScopedApplicationFont &) = delete;
    ScopedApplicationFont &operator=(const ScopedApplicationFont &) = delete;

private:
    QFont m_font;
};

struct MainWindowOptions {
    // The window's size; 0 by 0 keeps the size Main.qml gives it.
    int width = 0;
    int height = 0;
    // Waits for the window to be active, as the keyboard needs.
    bool activate = true;
    // The context properties the app sets.
    QString appVersion = QStringLiteral("test");
    QVariant appComponents;
};

// Main.qml, shown, at the options' size, and active unless they say not.
// Null when it failed to load or to show.
inline QQuickWindow *loadMainWindow(QQmlApplicationEngine &engine, LibraryTreeModel &libraryModel,
                                    const MainWindowOptions &options = {}) {
    QQmlContext *context = engine.rootContext();
    context->setContextProperty(QStringLiteral("libraryModel"), &libraryModel);
    context->setContextProperty(QStringLiteral("appVersion"), options.appVersion);
    if (options.appComponents.isValid()) {
        context->setContextProperty(QStringLiteral("appComponents"), options.appComponents);
    }
    engine.load(QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return nullptr;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (window == nullptr || !QTest::qWaitForWindowExposed(window)) {
        return nullptr;
    }
    const bool resize = options.width > 0 && options.height > 0;
    if (resize) {
        window->resize(options.width, options.height);
    }
    if (options.activate) {
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window)) {
            return nullptr;
        }
    }
    if (resize) {
        if (!QTest::qWaitFor(
                [&]() { return window->width() == options.width && window->height() == options.height; })) {
            return nullptr;
        }
        // The window reads its new size at once, but tells its bindings
        // with the window system's geometry event, which comes after.
        QCoreApplication::processEvents();
    }
    return window;
}

// The main window of one test: Main.qml's engine and the library model it
// shows, which outlives the engine.
struct MainWindowFixture {
    explicit MainWindowFixture(const MainWindowOptions &options = {})
        : window(loadMainWindow(engine, libraryModel, options)) {}

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
};

// Runs the checks in turn, up to the first that fails: for a test that
// loads its window once for several checks.
inline void runInTurn(std::initializer_list<std::function<void()>> checks) {
    for (const auto &check : checks) {
        check();
        if (QTest::currentTestFailed()) {
            return;
        }
    }
}
