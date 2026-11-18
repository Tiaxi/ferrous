// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QPoint>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QString>
#include <QVariant>

#include <functional>

#if defined(QT_QMLTEST_LIB)
#include <QtQuickTest/quicktest.h>
#endif

// Finding the items of a window by their objectName, and where they are.
// The item tree, unlike the object tree, holds what repeaters, views and
// loaders make, and under the window's content item its popups too.

// Every item named `name` at or under `root`, parents first; none without
// a root.
inline QList<QQuickItem *> itemsNamed(QQuickItem *root, const QString &name) {
    QList<QQuickItem *> found;
    const std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (item->objectName() == name) {
            found.push_back(item);
        }
        for (QQuickItem *child : item->childItems()) {
            visit(child);
        }
    };
    if (root != nullptr) {
        visit(root);
    }
    return found;
}

// The first of itemsNamed(), or null.
inline QQuickItem *itemNamed(QQuickItem *root, const QString &name) {
    const QList<QQuickItem *> found = itemsNamed(root, name);
    return found.isEmpty() ? nullptr : found.first();
}

// The first item named `name` in the window, or null.
inline QQuickItem *named(QQuickWindow *window, const QString &name) {
    return itemNamed(window->contentItem(), name);
}

inline QQuickItem *named(QQuickWindow *window, const char *name) {
    return named(window, QString::fromLatin1(name));
}

// The item's rectangle in its window's coordinates.
inline QRectF sceneRect(QQuickItem *item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

// The item's centre in its window's coordinates, to the pixel, as for a
// click.
inline QPoint sceneCentre(QQuickItem *item) {
    return sceneRect(item).center().toPoint();
}

// Whether `inner` lies within `outer`, give or take half a pixel.
inline bool within(const QRectF &inner, const QRectF &outer) {
    return inner.left() >= outer.left() - 0.5 && inner.right() <= outer.right() + 0.5
        && inner.top() >= outer.top() - 0.5 && inner.bottom() <= outer.bottom() + 0.5;
}

// A rectangle as "x,y widthxheight", for a failed check.
inline QString describeRect(const QRectF &rect) {
    return QStringLiteral("%1,%2 %3x%4").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height());
}

// Evaluates `expression` in `item`'s own QML scope, as for its attached
// properties. An invalid value for an error or undefined.
inline QVariant evaluateOn(QQuickItem *item, const QString &expression) {
    QQmlExpression evaluated(qmlContext(item), item, expression);
    bool undefined = false;
    const QVariant value = evaluated.evaluate(&undefined);
    return evaluated.hasError() || undefined ? QVariant() : value;
}

#if defined(QT_QMLTEST_LIB)
// Whether `item` or an item it sits in still waits for its polish, where
// layouts and positioners place their children. A Row culls a child that is
// hidden and takes it back only there: until then, a child shown again says
// it is visible, but Qt neither draws it nor gives it presses.
inline bool layoutPending(const QQuickItem *item) {
    for (; item != nullptr; item = item->parentItem()) {
        if (QQuickTest::qIsPolishScheduled(item)) {
            return true;
        }
    }
    return false;
}
#endif
