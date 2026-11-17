// LD_PRELOAD screenshot and driving probe for the UI capture harness (tools/ui-capture).
// Public Qt API only. Commands come one per line from the FIFO
// $SHOTPROBE_DIR/cmd as "<id> <cmd> <args>"; replies go to $SHOTPROBE_DIR/reply.log
// as "R <id> <text>".
#include <QNetworkProxy>
#include <QCoreApplication>
#include <QFile>
#include <QFontMetricsF>
#include <QFontInfo>
#include <QAccessible>
#include <QUrl>
#include <QMetaProperty>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlContext>
#include <QtQml/qqml.h>
#include <QFont>
#include <QColor>
#include <QJSValue>
#include <QQmlProperty>
#include <QEasingCurve>
#include <QMetaEnum>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSocketNotifier>
#include <QWheelEvent>
#include <QWindow>
#include <QDir>

// whether a text names this machine's home folder, read without the invisible break hints a path carries (the
// app breaks long paths with U+200B): the pack paints such texts out of the shots and fails while one shows
static bool lcNamesHome(QString t) {
    for (const QChar c : {QChar(0x200B), QChar(0x200C), QChar(0x200D), QChar(0x2060), QChar(0xFEFF), QChar(0x00AD)})
        t.remove(c);
    // the real home folder (the harness passes it: the app runs with an isolated HOME of its own)
    const QString home = qEnvironmentVariableIsSet("SHOT_PRIVATE_HOME") ? qEnvironmentVariable("SHOT_PRIVATE_HOME")
                                                                         : QDir::homePath();
    return t.contains(home);
}
#include <QImageReader>
#include <QElapsedTimer>
#include <QMutex>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTimer>
#include <atomic>
#include <QtConcurrent/QtConcurrentRun>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {

QString g_dir;


// Frame-strip recorder: reads back every frame each hooked window renders,
// on the render thread right after the scene graph has drawn it (public API:
// QQuickWindow::afterRendering + beginExternalCommands, QOpenGLFunctions).
// Threads: stripstart/stripstop run on the GUI thread, readbackFrame on each window's render thread. Everything
// both read is guarded by g_stripMutex; g_recording is also atomic so the GUI thread and a render thread's fast
// path can test it without the lock (it changes only with the lock held). A readback takes the settings and the
// recording's generation under the lock, reads the frame without it, and keeps the frame only if the same
// recording is still running: a frame of a stopped recording (a callback already past its check when stripstop
// disconnected it) is dropped, never added to the next recording's buffer.
struct StripFrame {
    qint64 ns;
    int win;
    QImage img;
};
struct StripSettings {
    QRect crop;          // logical, of the target window (main, or win=); empty = whole window
    int every = 1;       // every=N: keep every Nth rendered frame (per window)
    double scale = 1.0;  // scale=F: downscale kept frames (memory budget for large windows)
};
QElapsedTimer g_clock;
QMutex g_stripMutex;
// guarded by g_stripMutex:
std::vector<StripFrame> g_frames;
std::vector<QString> g_marks;
StripSettings g_strip;
quint64 g_stripGeneration = 0;  // bumped at every start and stop
QHash<int, int> g_frameCount;
// written only with g_stripMutex held; read with or without it
std::atomic<bool> g_recording{false};
// GUI thread only (stripstart, hookWindows):
QString g_winFilter;  // win=NAME: record only windows whose title/objectName contains NAME

void readbackFrame(QQuickWindow *w, int winIndex, bool isMain) {
    if (!g_recording.load()) {
        return;
    }
    const qint64 ns = g_clock.nsecsElapsed();
    StripSettings strip;
    quint64 generation = 0;
    {
        QMutexLocker lock(&g_stripMutex);
        if (!g_recording.load()) {
            return;
        }
        if (g_frameCount[winIndex]++ % qMax(1, g_strip.every) != 0) {
            return;
        }
        strip = g_strip;
        generation = g_stripGeneration;
    }
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    if (ctx == nullptr) {
        return;
    }
    w->beginExternalCommands();
    QOpenGLFunctions *f = ctx->functions();
    const qreal dpr = w->effectiveDevicePixelRatio();
    const int fw = qRound(w->width() * dpr);
    const int fh = qRound(w->height() * dpr);
    QRect r(0, 0, fw, fh);
    if (isMain && !strip.crop.isEmpty()) {
        r = QRect(qRound(strip.crop.x() * dpr), qRound(strip.crop.y() * dpr), qRound(strip.crop.width() * dpr),
                  qRound(strip.crop.height() * dpr)).intersected(r);
    }
    QImage img(r.width(), r.height(), QImage::Format_RGBA8888);
    f->glBindFramebuffer(GL_FRAMEBUFFER, ctx->defaultFramebufferObject());
    f->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    f->glReadPixels(r.x(), fh - r.y() - r.height(), r.width(), r.height(), GL_RGBA, GL_UNSIGNED_BYTE, img.bits());
    w->endExternalCommands();
    img = img.flipped(Qt::Vertical);
    if (strip.scale > 0 && strip.scale < 1.0) {
        img = img.scaled(qRound(img.width() * strip.scale), qRound(img.height() * strip.scale), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    }
    img.setDevicePixelRatio(1);
    QMutexLocker lock(&g_stripMutex);
    if (!g_recording.load() || g_stripGeneration != generation) {
        return;  // that recording stopped (and maybe another started) while this frame was read
    }
    g_frames.push_back({ns, winIndex, std::move(img)});
}


// ---- layout checker: objective layout defects of the live item tree (public API only) ----
QString lcTypeName(const QObject *o) {
    QString c = o->metaObject()->className();
    int i = c.indexOf("_QMLTYPE_");
    if (i < 0) i = c.indexOf("_QML_");
    if (i >= 0) c = c.left(i);
    if (c.startsWith("QQuick")) c = c.mid(6);
    return c;
}
QString lcId(const QObject *o) {
    if (QQmlContext *ctx = qmlContext(o)) {
        return ctx->nameForObject(const_cast<QObject *>(o));
    }
    return QString();
}
QString lcFile(const QObject *o) {
    if (QQmlContext *ctx = qmlContext(o)) {
        return ctx->baseUrl().toString();  // qrc:/... for the app's own QML, a file path for the style's
    }
    return QString();
}
QString lcName(const QQuickItem *it) {
    if (!it->objectName().isEmpty()) return it->objectName();
    const QString id = lcId(it);
    return id.isEmpty() ? lcTypeName(it) : id;
}

// The accessibility transcript's state list (public QAccessible API), and the update events an assistive
// technology would be sent while `a11yevents` records (the previous handler is chained and put back).
QString a11yStates(const QAccessible::State &st) {
    QStringList ss;
    if (st.focused) ss << "focused";
    if (st.focusable) ss << "focusable";
    // checked is reported whether or not the item says it is checkable (a radio button's checked state can
    // change while its interface does not claim checkable: the transcript shows both)
    if (st.checkable) ss << "checkable";
    if (st.checked) ss << "checked";
    else if (st.checkable) ss << "unchecked";
    if (st.selected) ss << "selected";
    if (st.expandable) ss << (st.expanded ? "expanded" : "collapsed");
    if (st.pressed) ss << "pressed";
    if (st.disabled) ss << "disabled";
    if (st.readOnly) ss << "read-only";
    if (st.hasPopup) ss << "has popup";
    return ss.join(", ");
}
QString a11yChanged(const QAccessible::State &st) {  // the states an event says changed (not their values)
    QStringList ss;
    if (st.focused) ss << "focused";
    if (st.checked) ss << "checked";
    if (st.selected) ss << "selected";
    if (st.expanded) ss << "expanded";
    if (st.pressed) ss << "pressed";
    if (st.disabled) ss << "disabled";
    return ss.join(", ");
}
QJsonArray g_a11yEvents;
QElapsedTimer g_a11yClock;
QAccessible::UpdateHandler g_a11yPrev = nullptr;
bool g_a11yRecording = false;
void a11yRecord(QAccessibleEvent *ev) {
    if (g_a11yRecording && ev) {
        QJsonObject j;
        j["t"] = double(g_a11yClock.elapsed());
        j["event"] = QString::fromLatin1(QMetaEnum::fromType<QAccessible::Event>().valueToKey(ev->type()));
        if (auto *qi = qobject_cast<QQuickItem *>(ev->object())) {
            j["item"] = lcName(qi);
        } else if (ev->object()) {
            j["item"] = ev->object()->objectName();
        }
        if (QAccessibleInterface *ai = ev->accessibleInterface()) {
            j["role"] = QString::fromLatin1(QMetaEnum::fromType<QAccessible::Role>().valueToKey(ai->role()));
            j["name"] = ai->text(QAccessible::Name);
            j["states"] = a11yStates(ai->state());
            j["value"] = ai->text(QAccessible::Value);
        }
        if (ev->type() == QAccessible::StateChanged) {
            j["changed"] = a11yChanged(static_cast<QAccessibleStateChangeEvent *>(ev)->changedStates());
        }
        g_a11yEvents.append(j);
    }
    if (g_a11yPrev) {
        g_a11yPrev(ev);
    }
}
QString lcPath(const QQuickItem *it) {
    QStringList parts;
    for (const QQuickItem *p = it->parentItem(); p != nullptr && parts.size() < 4; p = p->parentItem()) {
        QString n = p->objectName();
        if (n.isEmpty()) n = lcId(p);
        if (!n.isEmpty()) parts.prepend(n);
    }
    return parts.join('/');
}
QJsonArray lcRect(const QRectF &r) {
    return QJsonArray{qRound(r.x()), qRound(r.y()), qRound(r.width()), qRound(r.height())};
}
struct LcState {
    double opacity = 1.0;
    QRectF clip;          // scene rect of the nearest clipping ancestor (empty: none)
    QString clipOwner;
    QRectF viewport;      // scene rect of the Flickable whose content this is (empty: none)
    QRectF popup;         // scene rect of the popup this item is in (empty: none)
    QString popupName;
    int oobEdges = 0;     // edges the parent already overflows (reported once, at the outermost item)
    bool inOverlay = false;  // in the window's overlay (popups)
    bool underDim = false;   // under a modal popup's dim (drawn dimmed on purpose)
};
// The overlay's stacking: popup items and the dims of modal popups are its children, bottom to top.
int lcLastDim(QQuickItem *overlay) {
    int last = -1;
    const auto kids = overlay->childItems();
    for (int i = 0; i < kids.size(); ++i) {
        QQuickItem *k = kids[i];
        if (k->isVisible() && k->opacity() > 0.01 && !k->inherits("QQuickPopupItem") && k->width() > 1 && k->height() > 1) {
            last = i;
        }
    }
    return last;
}
QString lcColumn(const QQuickItem *it) {
    // A table cell's column: its modelData's "key" (e.g. the tag editor's Repeater of Labels per column).
    const QVariant md = it->property("modelData");
    if (!md.isValid()) return QString();
    if (md.canConvert<QJSValue>()) {
        const QJSValue v = md.value<QJSValue>();
        if (v.isObject() && v.hasProperty("key")) return v.property("key").toString();
    }
    const QVariantMap m = md.toMap();
    return m.value("key").toString();
}
void lcFinding(QJsonArray &out, const QString &kind, const QQuickItem *it, const QRectF &r, QJsonObject extra) {
    extra["kind"] = kind;
    extra["name"] = lcName(it);
    extra["type"] = lcTypeName(it);
    extra["path"] = lcPath(it);
    extra["file"] = lcFile(it);
    extra["rect"] = lcRect(r);
    const QString col = lcColumn(it);
    if (!col.isEmpty()) extra["column"] = col;
    out.append(extra);
}
int lcEdges(const QRectF &r, const QRectF &b, QJsonObject &by) {
    int e = 0;
    const double l = b.left() - r.left(), t = b.top() - r.top(), rr = r.right() - b.right(), bb = r.bottom() - b.bottom();
    if (l > 1) { e |= 1; by["left"] = qRound(l); }
    if (t > 1) { e |= 2; by["top"] = qRound(t); }
    if (rr > 1) { e |= 4; by["right"] = qRound(rr); }
    if (bb > 1) { e |= 8; by["bottom"] = qRound(bb); }
    return e;
}
// The buttons with a caption seen in the current window walk (their font's pixel size against the dialog's
// labels): the desktop style draws a button's caption through QStyle, with no QQuickText to read.
static QJsonArray *g_actions = nullptr;
void lcWalk(QQuickItem *it, const QRectF &win, LcState st, QJsonArray &finds, QJsonArray &texts) {
    if (!it->isVisible()) return;
    st.opacity *= it->opacity();
    if (st.opacity < 0.01) return;
    const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
    if (g_actions && it->inherits("QQuickAbstractButton") && st.opacity > 0.3 && win.contains(r.center())) {
        const QString cap = it->property("text").toString();
        const int display = it->property("display").toInt();  // AbstractButton.IconOnly == 0
        if (!cap.trimmed().isEmpty() && display != 0) {
            const QFont f = it->property("font").value<QFont>();
            QJsonObject a;
            a["name"] = lcName(it); a["type"] = lcTypeName(it); a["path"] = lcPath(it); a["file"] = lcFile(it);
            a["rect"] = lcRect(r); a["text"] = cap.left(60);
            if (lcNamesHome(cap)) a["home"] = true;  // (past the 60 recorded characters too)
            a["pt"] = f.pointSizeF() > 0 ? f.pointSizeF() : f.pixelSize() * 0.75;
            a["px"] = QFontInfo(f).pixelSize();
            // what its caption's ink would measure in that font (the mnemonic's & aside): the layout check compares
            // it with the ink drawn in the shot, since a style can paint the caption in another font (the
            // desktop style's ToolButton paints in the application's tool button font)
            QString plain = cap;
            plain.replace(QStringLiteral("&&"), QStringLiteral("\x01")).remove(QChar('&')).replace(QChar(1), QChar('&'));
            const QRectF tb = QFontMetricsF(f).tightBoundingRect(plain);
            a["tight"] = QJsonArray{tb.width(), tb.height()};
            const QVariant icv = it->property("icon");
            bool hasIcon = false;
            if (icv.isValid()) {  // QQuickIcon is a gadget: its name and source through the meta type
                const QMetaObject *mo = QMetaType(icv.metaType()).metaObject();
                if (mo) {
                    const int ni = mo->indexOfProperty("name"), si = mo->indexOfProperty("source");
                    const QString nm = ni >= 0 ? mo->property(ni).readOnGadget(icv.constData()).toString() : QString();
                    const QUrl src = si >= 0 ? mo->property(si).readOnGadget(icv.constData()).toUrl() : QUrl();
                    hasIcon = !nm.isEmpty() || !src.isEmpty();
                    const int wi = mo->indexOfProperty("width");
                    a["icon_w"] = wi >= 0 ? mo->property(wi).readOnGadget(icv.constData()).toDouble() : 0.0;
                }
            }
            a["icon"] = hasIcon && display != 1;  // AbstractButton.TextOnly == 1
            a["spacing"] = it->property("spacing").toDouble();
            a["enabled"] = it->isEnabled();
            a["overlay"] = st.inOverlay;
            a["dimmed"] = st.underDim;
            g_actions->append(a);
        }
    }
    const bool sized = it->width() > 0.5 && it->height() > 0.5;
    const bool isPopupItem = it->inherits("QQuickPopupItem");
    // Content scrolled (partly) out of its Flickable's viewport is normal scrolling: not checked.
    const bool scrolledOut = !st.viewport.isEmpty() && sized && !st.viewport.adjusted(-1, -1, 1, 1).contains(r);
    // For text, the bounds checks use what is painted: the content (contentWidth/Height), placed by the
    // padding and alignment inside the item's box, not the box (a wide label box with short text is fine).
    QRectF pr = r;
    if (it->inherits("QQuickText") && sized) {
        const double cw = it->property("contentWidth").toDouble(), ch = it->property("contentHeight").toDouble();
        const double lp = it->property("leftPadding").toDouble(), rp = it->property("rightPadding").toDouble();
        const double tp = it->property("topPadding").toDouble(), bp = it->property("bottomPadding").toDouble();
        const int ha = it->property("effectiveHorizontalAlignment").toInt();
        const int va = it->property("verticalAlignment").toInt();
        const double aw = it->width() - lp - rp, ah = it->height() - tp - bp;
        double x = lp, y = tp;
        if (cw < aw) x += (ha & Qt::AlignRight) ? aw - cw : (ha & Qt::AlignHCenter) ? (aw - cw) / 2 : 0;
        if (ch < ah) y += (va & Qt::AlignBottom) ? ah - ch : (va & Qt::AlignVCenter) ? (ah - ch) / 2 : 0;
        pr = it->mapRectToScene(QRectF(x, y, cw, ch));
    }
    int edges = 0;
    if (sized && !scrolledOut && !isPopupItem) {
        const bool isText = it->inherits("QQuickText");
        QJsonObject by;
        int e = lcEdges(pr, win, by);
        if (e & ~st.oobEdges) {
            QJsonObject x; x["by"] = by; x["bound"] = "window";
            lcFinding(finds, isText ? "text-clipped" : "out-of-bounds", it, r, x);
        }
        edges |= e;
        if (!st.clip.isEmpty()) {
            QJsonObject by2;
            int e2 = lcEdges(pr, st.clip, by2);
            if (e2 & ~st.oobEdges) {
                QJsonObject x; x["by"] = by2; x["bound"] = "clip:" + st.clipOwner;
                lcFinding(finds, isText ? "text-clipped" : "out-of-bounds", it, r, x);
            }
            edges |= e2;
        }
        if (!st.popup.isEmpty()) {
            QJsonObject by3;
            int e3 = lcEdges(pr, st.popup, by3);
            if (e3 & ~st.oobEdges) {
                QJsonObject x; x["by"] = by3; x["bound"] = "popup:" + st.popupName;
                lcFinding(finds, isText ? "text-clipped" : "out-of-bounds", it, r, x);
            }
            edges |= e3;
        }
        if (isText) {
            const QString text = it->property("text").toString();
            const double lp = it->property("leftPadding").toDouble(), rp = it->property("rightPadding").toDouble();
            const double tp = it->property("topPadding").toDouble(), bp = it->property("bottomPadding").toDouble();
            const double cw = it->property("contentWidth").toDouble(), ch = it->property("contentHeight").toDouble();
            const double aw = it->width() - lp - rp, ah = it->height() - tp - bp;
            if (!text.trimmed().isEmpty()) {
                if (it->property("truncated").toBool()) {
                    QJsonObject x; x["text"] = text.left(200); x["shown_width"] = qRound(it->width());
                    // what is left of it: the elided string in the item's font, mode and width, and the share of
                    // its characters (the ellipsis aside) shown
                    const QFont ef = it->property("font").value<QFont>();
                    const int mode = it->property("elide").toInt();
                    if (!text.contains('\n') && mode >= 0 && mode <= 2) {
                        const QString shown = QFontMetricsF(ef).elidedText(text, Qt::TextElideMode(mode), aw);
                        x["shown_text"] = shown.left(200);
                        const int kept = shown.size() - (shown.contains(QChar(0x2026)) ? 1 : 0);
                        x["share"] = text.size() ? double(kept) / text.size() : 1.0;
                    }
                    lcFinding(finds, "elided", it, r, x);
                } else if (cw > aw + 1.5 || ch > ah + 1.5) {
                    QJsonObject x; x["text"] = text.left(200);
                    if (cw > aw + 1.5) x["over_x"] = qRound(cw - aw);
                    if (ch > ah + 1.5) x["over_y"] = qRound(ch - ah);
                    x["content"] = QJsonArray{qRound(cw), qRound(ch)};
                    x["wrap"] = it->property("wrapMode").toInt();
                    lcFinding(finds, "text-overflow", it, r, x);
                }
                if (it->isEnabled() && win.contains(r.center())) {
                    const QColor c = it->property("color").value<QColor>();
                    const QFont f = it->property("font").value<QFont>();
                    QJsonObject t;
                    t["name"] = lcName(it); t["type"] = lcTypeName(it); t["path"] = lcPath(it); t["file"] = lcFile(it);
                    t["rect"] = lcRect(r); t["text"] = text.left(60);
                    if (lcNamesHome(text)) t["home"] = true;  // (past the 60 recorded characters too)
                    if (!lcColumn(it).isEmpty()) t["column"] = lcColumn(it);
                    t["color"] = c.name(QColor::HexArgb); t["opacity"] = st.opacity;
                    t["pt"] = f.pointSizeF() > 0 ? f.pointSizeF() : f.pixelSize() * 0.75;
                    t["px"] = QFontInfo(f).pixelSize();
                    t["bold"] = f.weight() >= QFont::Bold;
                    t["content"] = QJsonArray{qRound(cw), qRound(ch)};
                    t["overlay"] = st.inOverlay;
                    t["dimmed"] = st.underDim;
                    texts.append(t);
                }
            }
        }
        // Overlapping siblings in a layout or positioner.
        if (it->inherits("QQuickLayout") || it->inherits("QQuickBasePositioner")) {
            QList<QQuickItem *> kids;
            for (QQuickItem *c : it->childItems()) {
                if (c->isVisible() && c->opacity() > 0.01 && c->width() > 0.5 && c->height() > 0.5 && c->z() == 0) kids.append(c);
            }
            for (int i = 0; i < kids.size(); ++i) {
                const QRectF a = kids[i]->mapRectToScene(QRectF(0, 0, kids[i]->width(), kids[i]->height()));
                for (int j = i + 1; j < kids.size(); ++j) {
                    const QRectF b = kids[j]->mapRectToScene(QRectF(0, 0, kids[j]->width(), kids[j]->height()));
                    const QRectF o = a.intersected(b);
                    if (o.width() > 1 && o.height() > 1) {
                        QJsonObject x; x["with"] = lcName(kids[j]); x["with_type"] = lcTypeName(kids[j]);
                        x["overlap"] = lcRect(o); x["layout"] = lcName(it);
                        lcFinding(finds, "overlap", kids[i], a, x);
                    }
                }
            }
        }
    }
    LcState child = st;
    child.oobEdges = st.oobEdges | edges;
    if (it->inherits("QQuickOverlay")) {
        child.inOverlay = true;
        const int lastDim = lcLastDim(it);
        const auto kids = it->childItems();
        for (int i = 0; i < kids.size(); ++i) {
            LcState cs = child;
            cs.underDim = i < lastDim;  // a popup with a dim above it is dimmed
            lcWalk(kids[i], win, cs, finds, texts);
        }
        return;
    }
    if (isPopupItem) {
        child.popup = r;
        child.popupName = lcName(it);
        child.oobEdges = 0;
    }
    if (it->clip() && sized) {
        child.clip = st.clip.isEmpty() ? r : st.clip.intersected(r);
        child.clipOwner = lcName(it);
    }
    if (it->inherits("QQuickFlickable")) {
        QQuickItem *content = it->property("contentItem").value<QQuickItem *>();
        for (QQuickItem *c : it->childItems()) {
            LcState cs = child;
            if (c == content) {
                cs.viewport = r;
                cs.clip = QRectF();  // the viewport, not a clip finding, governs scrolled content
                cs.oobEdges = 0;
            }
            lcWalk(c, win, cs, finds, texts);
        }
        return;
    }
    for (QQuickItem *c : it->childItems()) lcWalk(c, win, child, finds, texts);
}

// Non-sibling overlaps inside a dialog (a dialog window, or a popup in the main window): visible text and
// buttons whose rects overlap while neither contains the other, e.g. a footer drawn over a link row.
void lcCollect(QQuickItem *it, double op, const QRectF &viewport, QList<QPair<QQuickItem *, QRectF>> &out,
               QHash<QQuickItem *, QQuickItem *> &layer, QQuickItem *lay) {
    if (!it->isVisible()) return;
    op *= it->opacity();
    if (op < 0.05) return;
    QRectF vp = viewport;
    const QRectF r = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
    const bool sized = it->width() > 1 && it->height() > 1;
    if (!vp.isEmpty() && sized && !vp.adjusted(-1, -1, 1, 1).contains(r)) return;  // scrolled (partly) out
    if (it->inherits("QQuickPopupItem")) lay = it;  // a popup is its own layer, drawn over the content on purpose
    layer.insert(it, lay);
    const bool button = it->inherits("QQuickAbstractButton");
    if (sized && (button || (it->inherits("QQuickText") && !it->property("text").toString().trimmed().isEmpty()))) {
        QRectF pr = r;
        if (!button) {  // painted extent of the text
            const double cw = it->property("contentWidth").toDouble(), ch = it->property("contentHeight").toDouble();
            const double lp = it->property("leftPadding").toDouble(), tp = it->property("topPadding").toDouble();
            const double aw = it->width() - lp - it->property("rightPadding").toDouble();
            const double ah = it->height() - tp - it->property("bottomPadding").toDouble();
            const int ha = it->property("effectiveHorizontalAlignment").toInt();
            const int va = it->property("verticalAlignment").toInt();
            double x = lp, y = tp;
            if (cw < aw) x += (ha & Qt::AlignRight) ? aw - cw : (ha & Qt::AlignHCenter) ? (aw - cw) / 2 : 0;
            if (ch < ah) y += (va & Qt::AlignBottom) ? ah - ch : (va & Qt::AlignVCenter) ? (ah - ch) / 2 : 0;
            pr = it->mapRectToScene(QRectF(x, y, qMin(cw, aw > 0 ? aw : cw), qMin(ch, ah > 0 ? ah : ch)));
        }
        out.append({it, pr});
        if (button) return;  // a button's own text is part of it
    }
    if (it->inherits("QQuickFlickable")) {
        QQuickItem *content = it->property("contentItem").value<QQuickItem *>();
        for (QQuickItem *c : it->childItems()) lcCollect(c, op, c == content ? r : vp, out, layer, lay);
        return;
    }
    for (QQuickItem *c : it->childItems()) lcCollect(c, op, vp, out, layer, lay);
}
void lcNonSiblingOverlaps(QQuickItem *root, QJsonArray &finds) {
    QList<QPair<QQuickItem *, QRectF>> items;
    QHash<QQuickItem *, QQuickItem *> layer;
    lcCollect(root, 1.0, QRectF(), items, layer, nullptr);
    for (int i = 0; i < items.size(); ++i) {
        for (int j = i + 1; j < items.size(); ++j) {
            QQuickItem *a = items[i].first, *b = items[j].first;
            if (a->isAncestorOf(b) || b->isAncestorOf(a)) continue;
            if (layer.value(a) != layer.value(b)) continue;  // content vs a popup over it: by design
            const QRectF o = items[i].second.intersected(items[j].second);
            if (o.width() > 2 && o.height() > 2) {
                QJsonObject x; x["with"] = lcName(b); x["with_type"] = lcTypeName(b);
                x["overlap"] = lcRect(o); x["layout"] = "not siblings: " + lcName(root);
                lcFinding(finds, "overlap", a, items[i].second, x);
            }
        }
    }
}

class Probe : public QObject {
public:
    Probe() {
        const QString fifo = g_dir + "/cmd";
        m_fd = ::open(fifo.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
        m_reply.setFileName(g_dir + "/reply.log");
        if (!m_reply.open(QIODevice::WriteOnly | QIODevice::Append)) qWarning("shotprobe: cannot open %s", qPrintable(m_reply.fileName()));
        if (m_fd >= 0) {
            m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
            connect(m_notifier, &QSocketNotifier::activated, this, [this] { readCommands(); });
        }
        g_clock.start();
        connect(&m_hookTimer, &QTimer::timeout, this, [this] { hookWindows(); });
        reply("0", "started");
    }

private:
    QList<QQuickWindow *> windows() {
        QList<QQuickWindow *> out;
        for (QWindow *w : QGuiApplication::topLevelWindows()) {
            if (auto *q = qobject_cast<QQuickWindow *>(w)) {
                out.append(q);
            }
        }
        return out;
    }
    QQuickWindow *mainWindow() {
        for (QQuickWindow *w : windows()) {
            if (QQmlEngine::contextForObject(w) != nullptr && w->isVisible()) {
                return w;
            }
        }
        for (QQuickWindow *w : windows()) {
            if (QQmlEngine::contextForObject(w) != nullptr) {
                return w;
            }
        }
        return nullptr;
    }
    QQuickWindow *targetWindow(const QString &which) {
        // "main" or a substring of the window title / objectName / class.
        if (which.isEmpty() || which == "main") {
            return mainWindow();
        }
        for (QQuickWindow *w : windows()) {
            if (w->isVisible() && (w->title().contains(which) || w->objectName().contains(which) ||
                                   QString(w->metaObject()->className()).contains(which))) {
                return w;
            }
        }
        return nullptr;
    }
    void reply(const QString &id, const QString &text) {
        m_reply.write(("R " + id + " " + text + "\n").toUtf8());
        m_reply.flush();
    }
    void readCommands() {
        char buf[16384];
        ssize_t n;
        while ((n = ::read(m_fd, buf, sizeof buf)) > 0) {
            m_pending.append(buf, n);
        }
        int nl;
        while ((nl = m_pending.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_pending.left(nl)).trimmed();
            m_pending.remove(0, nl + 1);
            if (!line.isEmpty()) {
                handle(line);
            }
        }
    }
    void mouse(QQuickWindow *w, QEvent::Type t, QPointF p, Qt::MouseButton b, Qt::MouseButtons s,
               Qt::KeyboardModifiers mods = Qt::NoModifier) {
        QMouseEvent e(t, p, p, w->mapToGlobal(p), b, s, mods);
        QCoreApplication::sendEvent(w, &e);
    }
    static QString rest(const QString &line, int words) {
        QString s = line;
        for (int i = 0; i < words; ++i) {
            const int sp = s.indexOf(' ');
            s = sp < 0 ? QString() : s.mid(sp + 1);
        }
        return s;
    }
    void handle(const QString &line) {
        const QStringList a = line.split(' ', Qt::SkipEmptyParts);
        const QString id = a.value(0);
        const QString cmd = a.value(1);
        if (cmd == "windows") {
            QString out;
            const QList<QQuickWindow *> ws = windows();
            for (QQuickWindow *w : ws) {
                // parent: the index (in this list) of the window it is transient for, -1 for none
                const int parent = int(ws.indexOf(qobject_cast<QQuickWindow *>(w->transientParent())));
                out += QString("[%1 title=%2 name=%3 vis=%4 %5x%6 dpr=%7 pos=%8,%9 flags=%10 active=%11 parent=%12] ")
                           .arg(w->metaObject()->className(), w->title(), w->objectName())
                           .arg(w->isVisible()).arg(w->width()).arg(w->height()).arg(w->devicePixelRatio())
                           .arg(w->x()).arg(w->y()).arg(int(w->flags()), 0, 16).arg(w->isActive()).arg(parent);
            }
            reply(id, out);
            return;
        }
        if (cmd == "quit") {
            reply(id, "quit");
            QCoreApplication::quit();
            return;
        }
        if (g_recording.load()) {
            QMutexLocker lock(&g_stripMutex);
            g_marks.push_back(QString::number(g_clock.nsecsElapsed() / 1e6, 'f', 3) + "\t" + line);
        }
        if (cmd == "now") {
            reply(id, QString::number(g_clock.nsecsElapsed() / 1e6, 'f', 3));
            return;
        }
        if (cmd == "stripstart") {
            // stripstart [x y w h] [win=NAME] [every=N] [scale=F]: [x y w h] is a crop of the main window
            // (logical px).
            QStringList pos;
            StripSettings strip;
            g_winFilter.clear();
            for (int i = 2; i < a.size(); ++i) {
                const QString t = a.value(i);
                if (t.startsWith("win=")) {
                    g_winFilter = t.mid(4);
                } else if (t.startsWith("every=")) {
                    strip.every = qMax(1, t.mid(6).toInt());
                } else if (t.startsWith("scale=")) {
                    strip.scale = t.mid(6).toDouble();
                } else {
                    pos.append(t);
                }
            }
            strip.crop = pos.size() >= 4 ? QRect(pos[0].toInt(), pos[1].toInt(), pos[2].toInt(), pos[3].toInt()) : QRect();
            {
                QMutexLocker lock(&g_stripMutex);
                g_recording.store(false);  // (a start without a stop: the frames read so far are not this one's)
                ++g_stripGeneration;
                g_frames.clear();
                g_marks.clear();
                g_frameCount.clear();
                g_strip = strip;
            }
            m_hooked.clear();
            m_winInfo.clear();
            hookWindows();
            {
                QMutexLocker lock(&g_stripMutex);
                ++g_stripGeneration;
                g_recording.store(true);
            }
            {
                QQuickWindow *tw = mainWindow();
                if (!g_winFilter.isEmpty()) {
                    for (QQuickWindow *w : windows()) {
                        if (w->title().contains(g_winFilter) || w->objectName().contains(g_winFilter)) tw = w;
                    }
                }
                m_traceCrop = strip.crop.isEmpty() ? QRectF() : QRectF(strip.crop);
                m_traceWin = tw;
                traceStart(tw);
                m_connections.append(QObject::connect(tw, &QQuickWindow::afterAnimating, this,
                                                      [this, tw] { if (g_recording.load()) traceSample(tw); }));
            }
            m_hookTimer.start(5);
            if (QQuickWindow *mw = mainWindow()) {
                mw->update();
            }
            reply(id, "recording " + QString::number(g_clock.nsecsElapsed() / 1e6, 'f', 3));
            return;
        }
        if (cmd == "stripstop") {
            // stripstop <dir>: writes <dir>/f<NNNN>_w<win>.png and <dir>/frames.tsv.
            std::vector<StripFrame> frames;
            std::vector<QString> marks;
            {
                // stopped and taken in one step: a readback still running keeps its frame only while the
                // generation it read is current, so nothing reaches g_frames after this
                QMutexLocker lock(&g_stripMutex);
                g_recording.store(false);
                ++g_stripGeneration;
                frames.swap(g_frames);
                marks.swap(g_marks);
            }
            m_hookTimer.stop();
            for (const auto &c : m_connections) {
                QObject::disconnect(c);
            }
            m_connections.clear();
            const QString dir = rest(line, 2);
            QDir().mkpath(dir);
            if (m_traceWin) traceWrite(dir);
            QFile tsv(dir + "/frames.tsv");
            if (!tsv.open(QIODevice::WriteOnly | QIODevice::Truncate)) qWarning("shotprobe: cannot write %s", qPrintable(tsv.fileName()));
            QList<QFuture<void>> jobs;
            for (size_t i = 0; i < frames.size(); ++i) {
                const QString file = QString("f%1_w%2.png").arg(i, 4, 10, QChar('0')).arg(frames[i].win);
                tsv.write(QString("frame\t%1\t%2\t%3\t%4\n").arg(i).arg(frames[i].ns / 1e6, 0, 'f', 3)
                              .arg(frames[i].win).arg(file).toUtf8());
                QImage img = std::move(frames[i].img);
                const QString path = dir + "/" + file;
                jobs.append(QtConcurrent::run([img, path] { img.convertToFormat(QImage::Format_RGB888).save(path, "PNG", 80); }));
            }
            for (auto &j : jobs) {
                j.waitForFinished();
            }
            for (const QString &m : marks) {
                tsv.write(("mark\t" + m + "\n").toUtf8());
            }
            for (const QString &wi : m_winInfo) {
                tsv.write(("window\t" + wi + "\n").toUtf8());
            }
            tsv.close();
            reply(id, QString("saved %1 frames").arg(frames.size()));
            return;
        }
        // Commands on a window: "<id> <cmd>@<window> ..." picks one, default main.
        const int at = cmd.indexOf('@');
        const QString name = at < 0 ? cmd : cmd.left(at);
        QQuickWindow *w = targetWindow(at < 0 ? QString() : cmd.mid(at + 1));
        if (w == nullptr) {
            reply(id, "ERROR no window");
            return;
        }
        if (name == "eval") {
            QQmlContext *ctx = QQmlEngine::contextForObject(w);
            if (ctx == nullptr) {
                ctx = QQmlEngine::contextForObject(mainWindow());
            }
            QQmlExpression expr(ctx, w, rest(line, 2));
            const QVariant v = expr.evaluate();
            reply(id, expr.hasError() ? "ERROR " + expr.error().toString()
                                      : QString::fromUtf8(QJsonDocument::fromVariant(QVariantList{v}).toJson(QJsonDocument::Compact)));
        } else if (name == "grab") {
            // grab <png> [<layout.json>]: the layout walk runs in this same command, right after the frame
            // is read back, with no event processing in between: walk and image describe the same frame.
            const QImage img = w->grabWindow();
            const bool walked = a.size() >= 4 ? writeLayout(a.value(3)) : true;
            const bool ok = img.save(a.value(2), "PNG", 80);
            reply(id, QString("%1 %2x%3%4").arg(ok ? "ok" : "ERROR save").arg(img.width()).arg(img.height())
                          .arg(walked ? "" : " (layout not written)"));
        } else if (name == "click" || name == "rclick" || name == "dblclick" || name == "press" || name == "release") {
            const QPointF p(a.value(2).toDouble(), a.value(3).toDouble());
            const Qt::MouseButton b = name == "rclick" ? Qt::RightButton : Qt::LeftButton;
            mouse(w, QEvent::MouseMove, p, Qt::NoButton, name == "release" ? Qt::MouseButtons(b) : Qt::MouseButtons(Qt::NoButton));
            if (name != "release") {
                mouse(w, QEvent::MouseButtonPress, p, b, b);
            }
            if (name != "press") {
                mouse(w, QEvent::MouseButtonRelease, p, b, Qt::NoButton);
            }
            if (name == "dblclick") {
                mouse(w, QEvent::MouseButtonPress, p, b, b);
                mouse(w, QEvent::MouseButtonDblClick, p, b, b);
                mouse(w, QEvent::MouseButtonRelease, p, b, Qt::NoButton);
            }
            reply(id, "ok");
        } else if (name == "move") {
            const QPointF p(a.value(2).toDouble(), a.value(3).toDouble());
            mouse(w, QEvent::MouseMove, p, Qt::NoButton, Qt::NoButton);
            QEnterEvent e(p, p, w->mapToGlobal(p));
            reply(id, "ok");
        } else if (name == "layout") {
            // layout <out.json>: layout findings and visible text of every visible window.
            reply(id, writeLayout(rest(line, 2)) ? "ok" : "ERROR write");
        } else if (name == "opacity") {
            // opacity: a fingerprint (sum and count) of every visible item's effective opacity in the window;
            // equal twice in a row = nothing is fading.
            double sum = 0; int n = 0;
            QList<QQuickItem *> stack{w->contentItem()};
            while (!stack.isEmpty()) {
                QQuickItem *it = stack.takeLast();
                if (!it->isVisible()) continue;
                for (QQuickItem *c : it->childItems()) stack.append(c);
                const double o = effOpacity(it);
                if (o > 0.0001 && o < 0.9999) { sum += o * (1 + (n % 7)); ++n; }
            }
            reply(id, QString("%1 %2").arg(sum, 0, 'f', 4).arg(n));
        } else if (name == "update") {
            // update: schedules a frame (a strip then shows a held state at a later time).
            w->update();
            reply(id, "ok");
        } else if (name == "popups") {
            // popups: every QQuickPopup (menus, dialogs, tooltips) with its state and scene rect, as JSON.
            QJsonArray out;
            // Popups are QObject children of whatever declared them, which may live in another window's
            // tree (an item reparented into a fullscreen viewer): search every window, keep those shown in w.
            QSet<QObject *> seen;
            QList<QObject *> all;
            QHash<QObject *, QQuickItem *> shownItem;  // a popup's item in an overlay: its rect where it is drawn
            for (QQuickWindow *ow : windows()) {
                all += ow->contentItem()->findChildren<QObject *>();
                all += ow->findChildren<QObject *>();
                // An attached ToolTip is one shared popup that no window item owns: find it through its popup
                // item, which is in the window's overlay (the visual tree), and that item's owner.
                QList<QQuickItem *> st{ow->contentItem()};
                while (!st.isEmpty()) {
                    QQuickItem *it = st.takeLast();
                    st += it->childItems();
                    if (it->inherits("QQuickPopupItem") && it->parent() && it->parent()->inherits("QQuickPopup")) {
                        all += it->parent();
                        if (it->window() == w) shownItem.insert(it->parent(), it);
                    }
                }
            }
            for (QObject *o : all) {
                if (seen.contains(o) || !o->inherits("QQuickPopup")) {
                    continue;
                }
                seen.insert(o);
                const bool vis = o->property("visible").toBool();
                if (!vis) {
                    continue;
                }
                QQuickItem *parent = o->property("parent").value<QQuickItem *>();
                if (parent != nullptr && parent->window() != w) {
                    continue;
                }
                QPointF pos(o->property("x").toReal(), o->property("y").toReal());
                if (parent != nullptr) {
                    pos = parent->mapToScene(pos);
                }
                QRectF rr(pos.x(), pos.y(), o->property("width").toReal(), o->property("height").toReal());
                if (QQuickItem *pi = shownItem.value(o))  // where its item is drawn (a shared ToolTip moves)
                    rr = pi->mapRectToScene(QRectF(0, 0, pi->width(), pi->height()));
                QJsonObject j;
                j["cls"] = QString(o->metaObject()->className());
                j["name"] = o->objectName();
                j["opened"] = o->property("opened").toBool();
                j["opacity"] = o->property("opacity").toReal();
                j["rect"] = QJsonArray{qRound(rr.x()), qRound(rr.y()), qRound(rr.width()), qRound(rr.height())};
                j["tooltip"] = o->inherits("QQuickToolTip");
                out.append(j);
            }
            reply(id, QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        } else if (name == "a11y") {
            // a11y: what the accessibility interface says of the item with keyboard focus in the main window:
            // its role, name, value, description and states, as an assistive technology would be told. Public API
            // (QAccessible). Accessibility is switched on at the first call, as a screen reader's start would.
            if (!QAccessible::isActive()) {
                QAccessible::setActive(true);
                QCoreApplication::processEvents();
            }
            // (a11y@WINDOW reads that window's focus item: a dialog's focus stops)
            QQuickItem *it = w ? w->activeFocusItem() : nullptr;
            QJsonObject j;
            if (w) {
                j["window"] = w->title().isEmpty() ? w->objectName() : w->title();
            }
            if (it) {
                j["item"] = lcName(it);
                QQuickItem *on = it;
                QAccessibleInterface *ai = QAccessible::queryAccessibleInterface(on);
                // the focus item may be an inner part (a delegate's focus scope): the nearest ancestor with a role
                while (on && (!ai || ai->role() == QAccessible::NoRole || ai->role() == QAccessible::Client)) {
                    on = on->parentItem();
                    ai = on ? QAccessible::queryAccessibleInterface(on) : nullptr;
                    if (on == w->contentItem()) { break; }
                }
                if (ai && on) {
                    j["on"] = on == it ? QStringLiteral("") : lcName(on);
                    j["role"] = QString::fromLatin1(QMetaEnum::fromType<QAccessible::Role>().valueToKey(ai->role()));
                    j["name"] = ai->text(QAccessible::Name);
                    j["description"] = ai->text(QAccessible::Description);
                    QString val = ai->text(QAccessible::Value);
                    if (QAccessibleValueInterface *vi = ai->valueInterface())
                        val = QStringLiteral("%1 (%2 to %3)").arg(vi->currentValue().toString(), vi->minimumValue().toString(),
                                                                 vi->maximumValue().toString());
                    j["value"] = val;
                    j["states"] = a11yStates(ai->state());
                }
            }
            reply(id, QString::fromUtf8(QJsonDocument(j).toJson(QJsonDocument::Compact)));
        } else if (name == "a11yevents") {
            // a11yevents start | stop: records the accessibility update events (focus, state changes such as
            // checked, name and value changes) the app sends while running; stop replies with them as JSON.
            if (!QAccessible::isActive()) {
                QAccessible::setActive(true);
                QCoreApplication::processEvents();
            }
            if (a.value(2) == "start") {
                g_a11yEvents = QJsonArray();
                g_a11yClock.start();
                if (!g_a11yRecording) {
                    g_a11yPrev = QAccessible::installUpdateHandler(a11yRecord);
                }
                g_a11yRecording = true;
                reply(id, "ok");
            } else {
                if (g_a11yRecording) {
                    QAccessible::installUpdateHandler(g_a11yPrev);
                    g_a11yPrev = nullptr;
                    g_a11yRecording = false;
                }
                reply(id, QString::fromUtf8(QJsonDocument(g_a11yEvents).toJson(QJsonDocument::Compact)));
            }
        } else if (name == "svg") {
            // svg <in> <px> <out>: renders an SVG with Qt's image plugin (as icon themes do).
            QImageReader r(a.value(2));
            r.setScaledSize(QSize(a.value(3).toInt(), a.value(3).toInt()));
            const QImage img = r.read();
            reply(id, img.isNull() ? "ERROR " + r.errorString() : (img.save(a.value(4)) ? "ok" : "ERROR save"));
        } else if (name == "dmove") {
            const QPointF p(a.value(2).toDouble(), a.value(3).toDouble());
            mouse(w, QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton);
            reply(id, "ok");
        } else if (name == "mwheel") {
            // mwheel x y delta modifiers
            const QPointF p(a.value(2).toDouble(), a.value(3).toDouble());
            QWheelEvent e(p, w->mapToGlobal(p), QPoint(), QPoint(0, a.value(4, "-120").toInt()), Qt::NoButton,
                          Qt::KeyboardModifiers(a.value(5, "0").toInt()), Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(w, &e);
            reply(id, "ok");
        } else if (name == "wheel") {
            const QPointF p(a.value(2).toDouble(), a.value(3).toDouble());
            QWheelEvent e(p, w->mapToGlobal(p), QPoint(), QPoint(0, a.value(4, "-120").toInt()), Qt::NoButton,
                          Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(w, &e);
            reply(id, "ok");
        } else if (name == "key") {
            // key <Qt::Key decimal> <modifier mask decimal> [text]
            const int key = a.value(2).toInt();
            const auto mods = Qt::KeyboardModifiers(a.value(3, "0").toInt());
            // the command line splits on spaces, so a typed space comes as "%20"
            const QString text = a.value(4) == QLatin1String("%20") ? QStringLiteral(" ") : a.value(4);
            QKeyEvent p(QEvent::KeyPress, key, mods, text);
            QCoreApplication::sendEvent(w, &p);
            QKeyEvent r(QEvent::KeyRelease, key, mods, text);
            QCoreApplication::sendEvent(w, &r);
            reply(id, "ok");
        } else if (name == "netproxy") {
            // netproxy <host> <port>: Qt's application proxy (QNetworkAccessManager's default), so a request
            // the app makes goes there; with 127.0.0.1 9 every request is refused at once and nothing leaves the
            // machine (the app's own test does the same for its iTunes manager). Replies with the proxy now set.
            QNetworkProxy::setApplicationProxy(
                QNetworkProxy(QNetworkProxy::HttpProxy, a.value(2), quint16(a.value(3).toUInt())));
            const QNetworkProxy now = QNetworkProxy::applicationProxy();
            reply(id, QString("%1 %2 %3").arg(int(now.type())).arg(now.hostName()).arg(now.port()));
        } else if (name == "resize") {
            w->resize(a.value(2).toInt(), a.value(3).toInt());
            reply(id, "ok");
        } else if (name == "find") {
            // find <objectName>: scene rect of the first visible item so named.
            QList<QQuickItem *> stack{w->contentItem()};
            QString out = "none";
            while (!stack.isEmpty()) {
                QQuickItem *item = stack.takeFirst();
                if (item->objectName() == a.value(2) && item->isVisible()) {
                    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
                    out = QString("%1 %2 %3 %4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
                    break;
                }
                stack.append(item->childItems());
            }
            reply(id, out);
        } else if (name == "dump") {
            QString out;
            dump(w->contentItem(), 0, out);
            QFile f(rest(line, 2));
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(out.toUtf8());
            }
            reply(id, "ok");
        } else {
            reply(id, "ERROR unknown " + line);
        }
    }
    void dump(QQuickItem *item, int depth, QString &out) {
        QString idName;
        if (QQmlContext *ctx = qmlContext(item)) {
            idName = ctx->nameForObject(item);
        }
        const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        out += QString("%1%2 id=%3 name=%4 vis=%5 rect=%6,%7,%8x%9\n")
                   .arg(QString(depth, ' '), item->metaObject()->className(), idName, item->objectName())
                   .arg(item->isVisible()).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
        for (QQuickItem *c : item->childItems()) {
            dump(c, depth + 1, out);
        }
    }

    bool writeLayout(const QString &path) {
        QJsonArray wins;
        for (QQuickWindow *ww : windows()) {
            if (!ww->isVisible()) continue;
            QJsonArray finds, texts;
            LcState st;
            for (QQuickItem *k : ww->contentItem()->childItems()) {
                if (k->inherits("QQuickOverlay") && lcLastDim(k) >= 0) st.underDim = true;
            }
            QJsonArray actions;
            g_actions = &actions;
            lcWalk(ww->contentItem(), QRectF(0, 0, ww->width(), ww->height()), st, finds, texts);
            g_actions = nullptr;
            if (ww != mainWindow()) {
                lcNonSiblingOverlaps(ww->contentItem(), finds);  // a dialog window
            } else {
                for (QQuickItem *k : ww->contentItem()->childItems()) {
                    if (!k->inherits("QQuickOverlay")) continue;
                    for (QQuickItem *pi : k->childItems()) {
                        if (pi->inherits("QQuickPopupItem") && pi->isVisible() && pi->width() > 1) lcNonSiblingOverlaps(pi, finds);
                    }
                }
            }
            QJsonObject wo;
            wo["cls"] = QString(ww->metaObject()->className());
            wo["title"] = ww->title();
            wo["name"] = ww->objectName();
            wo["size"] = QJsonArray{ww->width(), ww->height()};
            wo["main"] = ww == mainWindow();
            wo["findings"] = finds;
            wo["texts"] = texts;
            wo["actions"] = actions;
            bool dimmed = false;
            QList<QObject *> objs = ww->contentItem()->findChildren<QObject *>();
            for (QQuickWindow *ow : windows()) objs += ow->findChildren<QObject *>();
            for (QObject *o : objs) {
                if (o->inherits("QQuickPopup") && o->property("visible").toBool() && o->property("dim").toBool()) {
                    QQuickItem *par = o->property("parent").value<QQuickItem *>();
                    if (par == nullptr || par->window() == ww) { dimmed = true; break; }
                }
            }
            wo["dimmed"] = dimmed;
            wins.append(wo);
        }
        QFile f(path);
        return f.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               f.write(QJsonDocument(QJsonObject{{"windows", wins}}).toJson(QJsonDocument::Compact)) > 0;
    }

    void hookWindows() {
        QQuickWindow *mw = mainWindow();
        for (QQuickWindow *w : windows()) {
            if (m_hooked.contains(w)) {
                continue;
            }
            const int index = int(m_hooked.size());
            m_hooked.append(w);
            const bool matches = !g_winFilter.isEmpty() &&
                (w->title().contains(g_winFilter) || w->objectName().contains(g_winFilter));
            if (!g_winFilter.isEmpty() && !matches) {
                continue;  // win=: only that window is recorded (it may appear later)
            }
            const bool isMain = g_winFilter.isEmpty() ? w == mw : true;
            m_winInfo.append(QString("%1\t%2\t%3\t%4,%5,%6x%7\t%8").arg(index).arg(w->metaObject()->className())
                                 .arg(w->title()).arg(w->x()).arg(w->y()).arg(w->width()).arg(w->height())
                                 .arg(QString::number(g_clock.nsecsElapsed() / 1e6, 'f', 3)));
            m_connections.append(QObject::connect(
                w, &QQuickWindow::afterRendering, w, [w, index, isMain] { readbackFrame(w, index, isMain); },
                Qt::DirectConnection));
            m_connections.append(QObject::connect(w, &QWindow::visibleChanged, this, [this, w, index](bool v) {
                if (g_recording.load()) {
                    QMutexLocker lock(&g_stripMutex);
                    g_marks.push_back(QString::number(g_clock.nsecsElapsed() / 1e6, 'f', 3) +
                                      QString("\twindow %1 visible=%2 pos=%3,%4 size=%5x%6")
                                          .arg(index).arg(v).arg(w->x()).arg(w->y()).arg(w->width()).arg(w->height()));
                }
            }));
        }
    }

    // ---- motion trace during a strip: per-frame effective opacity of changing items, and the QML
    // Behaviors / standalone Animations that ran (public API: properties, QQmlProperty) ----
    // (`fulls`, the item's own scene rect before its clipping ancestors cut it: a cover revealed by a growing
    // clip keeps its size; written as "~x,y,w,h" on the points where it is clipped)
    struct OpTrack { QString key; double last = -1; bool text = false; QString layer; QRectF rect; QRectF full; QList<QPair<double, double>> pts; QList<QRectF> rects; QList<QString> layers; QList<QRectF> fulls; };
    struct AnimTrack {
        QPointer<QObject> obj, anim; QString kind, target, prop; QQmlProperty qp;
        QVariant last; QVariant from, to; double start = -1, end = -1; bool wasRunning = false;
        QList<QPair<double, double>> runs;
        struct Run { double start, end; QString from, to; };
        QList<Run> done;  // earlier runs of a Behavior
        QStringList runFrom, runTo;  // a standalone animation's runs: its target property's value at start and end
        QList<double> runFirst;      // the first value read while running (did it jump to its declared `from`?)
        double lastNum = 0; bool lastIsNum = false;
    };
    QHash<QQuickItem *, OpTrack> m_op;
    QHash<QString, int> m_keyCount;
    QList<AnimTrack> m_anims;
    QRectF m_traceCrop;
    double m_t0 = 0;

    static QString valueText(const QVariant &v) {  // numbers short (an opacity, not 0.30000000000000004)
        return v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Float ? QString::number(v.toDouble(), 'g', 4) : v.toString();
    }
    static double effOpacity(QQuickItem *it) {
        double o = 1.0;
        for (QQuickItem *p = it; p != nullptr; p = p->parentItem()) {
            if (!p->isVisible()) return 0.0;
            o *= p->opacity();
        }
        return o;
    }
    static QString easingName(const QVariant &v) {
        const QEasingCurve c = v.value<QEasingCurve>();
        const QMetaEnum me = QMetaEnum::fromType<QEasingCurve::Type>();
        return QString::fromLatin1(me.valueToKey(c.type()));
    }
    void traceStart(QQuickWindow *tw) {
        m_op.clear(); m_keyCount.clear(); m_anims.clear();
        m_t0 = g_clock.nsecsElapsed() / 1e6;
        QList<QObject *> objs;
        for (QQuickWindow *w : windows()) objs += w->findChildren<QObject *>() + w->contentItem()->findChildren<QObject *>();
        QSet<QObject *> seen;
        for (QObject *o : objs) {
            if (seen.contains(o)) continue;
            seen.insert(o);
            if (o->inherits("QQuickBehavior")) {
                AnimTrack a;
                a.obj = o; a.kind = "Behavior";
                a.anim = o->property("animation").value<QObject *>();
                const QVariant tp = o->property("targetProperty");
                if (tp.canConvert<QQmlProperty>()) a.qp = tp.value<QQmlProperty>();
                if (!a.qp.isValid()) continue;
                a.prop = a.qp.name();
                QQuickItem *ti = qobject_cast<QQuickItem *>(a.qp.object());
                a.target = ti ? (lcPath(ti).isEmpty() ? lcName(ti) : lcPath(ti) + "/" + lcName(ti)) : lcTypeName(a.qp.object());
                a.last = a.qp.read();
                m_anims.append(a);
            } else if (o->inherits("QQuickAbstractAnimation")) {
                QObject *par = o->parent();
                if (par && (par->inherits("QQuickBehavior") || par->inherits("QQuickTransition") ||
                            par->inherits("QQuickAnimationGroup"))) continue;  // reported through their owner
                AnimTrack a;
                a.obj = o; a.anim = o; a.kind = lcTypeName(o);
                QObject *t = o->property("target").value<QObject *>();
                QQuickItem *ti = qobject_cast<QQuickItem *>(t);
                a.target = ti ? (lcPath(ti).isEmpty() ? lcName(ti) : lcPath(ti) + "/" + lcName(ti))
                              : (o->parent() ? lcTypeName(o->parent()) : QString());
                a.prop = o->property("property").toString();
                if (a.prop.isEmpty()) a.prop = o->property("properties").toString();
                // the values it really ran between: an animation without `from` starts at the current value
                if (t && !a.prop.isEmpty() && !a.prop.contains(',')) {
                    a.qp = QQmlProperty(t, a.prop);
                    if (a.qp.isValid()) a.last = a.qp.read();
                }
                a.wasRunning = o->property("running").toBool();
                if (a.wasRunning) a.start = 0;
                m_anims.append(a);
            }
        }
        traceSample(tw);  // the initial state (time 0)
    }
    void traceSample(QQuickWindow *tw) {
        const double t = g_clock.nsecsElapsed() / 1e6 - m_t0;
        // Opacity of every item in the traced region, recorded when it changes.
        // Each item's rect is cut to its clipping ancestors (a list's rows behind a footer are not on screen).
        QList<QPair<QQuickItem *, QRectF>> stack{{tw->contentItem(), QRectF()}};
        QSet<QQuickItem *> seen;
        while (!stack.isEmpty()) {
            const auto [it, clipTo] = stack.takeLast();
            const QRectF full = it->mapRectToScene(QRectF(0, 0, it->width(), it->height()));
            const QRectF r = clipTo.isNull() ? full : full.intersected(clipTo);
            const QRectF childClip = it->clip() ? r : clipTo;
            if (!(it->clip() && r.isEmpty()))
                for (QQuickItem *c : it->childItems()) stack.append({c, childClip});
            if (it->width() < 1 || it->height() < 1 || r.width() < 1 || r.height() < 1) continue;
            if (!m_traceCrop.isEmpty() && !m_traceCrop.intersects(r)) continue;
            seen.insert(it);
            const double o = effOpacity(it);
            // its layer: the popup it is in (drawn over the content on purpose), or the window's content; it can
            // change (the viewer's plot is reparented into its popup)
            QString layer;
            for (QQuickItem *p = it; p != nullptr; p = p->parentItem())
                if (p->inherits("QQuickPopupItem")) { layer = "popup@" + QString::number(quintptr(p), 16); break; }
            auto f = m_op.find(it);
            if (f == m_op.end()) {
                OpTrack tr;
                QString base = lcPath(it).isEmpty() ? lcName(it) : lcPath(it) + "/" + lcName(it);
                const int n = m_keyCount[base]++;
                tr.key = n ? base + "#" + QString::number(n + 1) : base;
                tr.text = it->inherits("QQuickText");
                tr.layer = layer;
                tr.layers.append(layer);
                tr.rect = r;
                tr.full = full;
                tr.last = o;
                tr.pts.append({t, o});
                tr.rects.append(r);
                tr.fulls.append(full);
                m_op.insert(it, tr);
            } else if (qAbs(f->last - o) > 0.002 || qAbs(f->rect.x() - r.x()) > 1 || qAbs(f->rect.y() - r.y()) > 1
                       || qAbs(f->rect.width() - r.width()) > 1 || qAbs(f->rect.height() - r.height()) > 1 || f->layer != layer
                       || qAbs(f->full.width() - full.width()) > 1 || qAbs(f->full.height() - full.height()) > 1
                       || qAbs(f->full.x() - full.x()) > 1 || qAbs(f->full.y() - full.y()) > 1) {
                // opacity, place or layer changed (list delegates move and are reused): a point with all three
                f->last = o;
                f->pts.append({t, o});
                f->rects.append(r);
                f->layers.append(layer);
                f->fulls.append(full);
                f->rect = r;
                f->full = full;
                f->layer = layer;
            }
        }
        // An item that left the tree (a destroyed or pooled delegate), shrank to nothing or left the region is no
        // longer on screen there: record it as 0 rather than keeping its last opacity. Only the keys are compared;
        // a gone item's pointer is never dereferenced.
        for (auto f = m_op.begin(); f != m_op.end(); ++f) {
            if (f->last > 0.002 && !seen.contains(f.key())) {
                f->last = 0;
                f->pts.append({t, 0});
                f->rects.append(f->rect);
                f->layers.append(f->layer);
                f->fulls.append(f->full);
            }
        }
        for (AnimTrack &a : m_anims) {
            if (!a.obj) continue;
            if (a.kind == "Behavior") {
                const QVariant v = a.qp.read();
                if (v != a.last) {
                    if (a.start < 0 || t - a.end > 60) {  // a new run of this Behavior
                        if (a.start >= 0) a.done.append({a.start, a.end, a.from.toString(), a.to.toString()});
                        a.start = t; a.from = a.last;
                    }
                    a.end = t; a.to = v; a.last = v;
                }
            } else {
                const bool r = a.obj->property("running").toBool();
                const QVariant cur = a.qp.isValid() ? a.qp.read() : QVariant();
                if (r && !a.wasRunning) {
                    a.runs.append({t, -1});
                    // the value before the run, unless the run jumped to its declared `from` (closer to it)
                    QString fr = a.last.isValid() ? valueText(a.last) : QString();
                    bool okF = false;
                    const double declared = a.anim->property("from").toDouble(&okF);
                    bool okC = false;
                    const double now = cur.toDouble(&okC);
                    if (okF && okC && a.lastIsNum && qAbs(now - declared) < qAbs(now - a.lastNum))
                        fr = valueText(a.anim->property("from"));
                    a.runFrom.append(fr);
                    a.runTo.append(QString());
                }
                if (!r && a.wasRunning && !a.runs.isEmpty()) {
                    a.runs.last().second = t;
                    QString to = cur.isValid() ? valueText(cur) : QString();
                    // A run that lasted its duration reached its declared `to`; a handler on finished may have
                    // reset the property before this sample (modeFade 0 to 1, then 0): both are reported.
                    bool okD = false;
                    const double dur = a.anim->property("duration").toDouble(&okD);
                    const QVariant decl = a.anim->property("to");
                    if (okD && dur > 0 && t - a.runs.last().first >= 0.9 * dur && decl.isValid() && !to.isEmpty()
                        && valueText(decl) != to)
                        to = valueText(decl) + " (then " + to + ")";
                    a.runTo.last() = to;
                }
                if (!r && a.wasRunning && a.runs.isEmpty()) {
                    a.runs.append({0, t});
                    a.runFrom.append(QString());
                    a.runTo.append(cur.isValid() ? valueText(cur) : QString());
                }
                a.wasRunning = r;
                a.last = cur;
                a.lastNum = cur.toDouble(&a.lastIsNum);
            }
        }
    }
    void traceWrite(const QString &dir) {
        QFile f(dir + "/opacity.tsv");
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(QString("#t0\t%1\n").arg(m_t0, 0, 'f', 3).toUtf8());  // trace times are relative to this
            for (auto it = m_op.cbegin(); it != m_op.cend(); ++it) {
                // what changed, and every shown text (its rect scopes the text measurements: text vs images)
                if (it->pts.size() < 2 && !(it->text && it->last > 0.3)) continue;
                const QRectF &r = it->rects.first();  // the first place; "@x,y,w,h" on a point where it moved
                QString pts;
                for (int k = 0; k < it->pts.size(); ++k) {
                    const auto &p = it->pts[k];
                    pts += QString::number(p.first, 'f', 2) + ":" + QString::number(p.second, 'f', 3);
                    const QRectF &q = it->rects[k];
                    if (k > 0 && q != it->rects[k - 1])
                        pts += QString("@%1,%2,%3,%4").arg(qRound(q.x())).arg(qRound(q.y())).arg(qRound(q.width())).arg(qRound(q.height()));
                    const QRectF &fq = it->fulls[k];
                    if (fq.toRect() != q.toRect())  // clipped here: "~" and the item's own rect
                        pts += QString("~%1,%2,%3,%4").arg(qRound(fq.x())).arg(qRound(fq.y())).arg(qRound(fq.width())).arg(qRound(fq.height()));
                    if (k > 0 && it->layers[k] != it->layers[k - 1])  // "^layer": it moved to another layer
                        pts += "^" + (it->layers[k].isEmpty() ? QStringLiteral("-") : it->layers[k]);
                    pts += " ";
                }
                f.write(QString("%1\t%2\t%3,%4,%5,%6\t%7\t%8\n").arg(it->key).arg(it->text ? 1 : 0)
                            .arg(qRound(r.x())).arg(qRound(r.y())).arg(qRound(r.width())).arg(qRound(r.height()))
                            .arg(pts.trimmed(), it->layers.first().isEmpty() ? QStringLiteral("-") : it->layers.first()).toUtf8());
            }
        }
        QFile g(dir + "/animations.tsv");
        if (g.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            g.write(QString("#t0\t%1\n").arg(m_t0, 0, 'f', 3).toUtf8());
            for (const AnimTrack &a : m_anims) {
                if (!a.anim) continue;
                const QString dur = a.anim->property("duration").toString();
                const QString ease = easingName(a.anim->property("easing"));
                const QString cls = lcTypeName(a.anim);
                if (a.kind == "Behavior" && a.start >= 0) {
                    QList<AnimTrack::Run> runs = a.done;
                    runs.append({a.start, a.end, a.from.toString(), a.to.toString()});
                    for (const auto &r : runs) {
                        g.write(QString("Behavior\t%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\n").arg(a.target, a.prop, cls)
                                    .arg(r.from, r.to, dur, ease)
                                    .arg(r.start, 0, 'f', 1).arg(r.end, 0, 'f', 1).toUtf8());
                    }
                } else {
                    for (int i = 0; i < a.runs.size(); ++i) {
                        const auto &r = a.runs[i];
                        // the target property's value when the run started and ended; the declared from/to otherwise
                        const QString fr = i < a.runFrom.size() && !a.runFrom[i].isEmpty() ? a.runFrom[i] : a.anim->property("from").toString();
                        const QString to = i < a.runTo.size() && !a.runTo[i].isEmpty() ? a.runTo[i] : a.anim->property("to").toString();
                        g.write(QString("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\n").arg(a.kind, a.target, a.prop, cls)
                                    .arg(fr, to, dur, ease)
                                    .arg(r.first, 0, 'f', 1).arg(r.second, 0, 'f', 1).toUtf8());
                    }
                }
            }
        }
    }

    QPointer<QQuickWindow> m_traceWin;
    QList<QPointer<QQuickWindow>> m_hooked;
    QStringList m_winInfo;
    QList<QMetaObject::Connection> m_connections;
    QTimer m_hookTimer;
    int m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
    QByteArray m_pending;
    QFile m_reply;
};

void startProbe() {
    g_dir = qEnvironmentVariable("SHOTPROBE_DIR");
    if (g_dir.isEmpty() || qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr) {
        return;
    }
    new Probe;
}

}  // namespace

Q_COREAPP_STARTUP_FUNCTION(startProbe)
