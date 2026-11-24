// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

#include <QBuffer>
#include <QDirIterator>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QQmlNetworkAccessManagerFactory>
#include <QTextDocument>

// Text from outside the app, such as tags, file names and answers from the
// network, shows as it is: never as markup, which could also load an image.
class PlainTextSmokeTest : public QmlSmokeTest {
    Q_OBJECT

public:
    using QmlSmokeTest::QmlSmokeTest;

private slots:
    void theMainWindowShowsMarkupLiterally();
    void theViewersShowMarkupLiterally();
    void theDialogsShowMarkupLiterally();
    void theTagEditorShowsMarkupLiterally();
    void searchShowsMarkupLiterally();
    void textFromBeforeTheWindowShowsLiterally();
    void ordinaryTextLooksAsWithAutoText();
    void plainToolTipsReadAsTheStyles_data();
    void plainToolTipsReadAsTheStyles();
    void everyTextItemStatesASafeFormat_data();
    void everyTextItemStatesASafeFormat();
    void theFormatCheckFindsAnUnsafeItem_data();
    void theFormatCheckFindsAnUnsafeItem();
};

namespace {

// Records what a QML engine asks the network for, and answers each request
// with an image of its own, so that nothing leaves the process. The engine's
// image reader asks from a thread of its own.
class RequestRecorder final : public QQmlNetworkAccessManagerFactory {
public:
    RequestRecorder() {
        QImage image(1, 1, QImage::Format_RGB32);
        image.fill(Qt::black);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        m_answer = QUrl(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64()));
    }

    QNetworkAccessManager *create(QObject *parent) override { return new Manager(*this, parent); }

    QStringList requests() {
        QMutexLocker locker(&m_mutex);
        return m_requests;
    }

private:
    class Manager final : public QNetworkAccessManager {
    public:
        Manager(RequestRecorder &recorder, QObject *parent)
            : QNetworkAccessManager(parent), m_recorder(recorder) {}

    protected:
        QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override {
            {
                QMutexLocker locker(&m_recorder.m_mutex);
                m_recorder.m_requests.push_back(request.url().toString());
            }
            return QNetworkAccessManager::createRequest(GetOperation, QNetworkRequest(m_recorder.m_answer));
        }

    private:
        RequestRecorder &m_recorder;
    };

    QMutex m_mutex;
    QStringList m_requests;
    QUrl m_answer;
};

// A value with markup and an image, marked with its `id`. Each test and
// value has an image of its own, which Qt's image cache can't answer.
QString hostile(const QString &id) {
    return QStringLiteral("<img src=\"http://example.invalid/%1/%2.png\"> <b>%2</b> & more")
        .arg(QString::fromLatin1(QTest::currentTestFunction()), id);
}

// A hostile file or folder name, which can have no slash.
QString hostileName(const QString &id) {
    return QStringLiteral("<b>%1<i>").arg(id);
}

// What a text holds without the invisible characters that only shape its
// lines (BreakableText.js).
QString withoutBreaks(QString text) {
    static const QRegularExpression invisible(QStringLiteral("[\\x{200B}\\x{2060}]"));
    return text.remove(invisible);
}

QStringList idsIn(const QString &text) {
    static const QRegularExpression marker(QStringLiteral("<b>([a-z0-9]+)<"));
    QStringList ids;
    auto matches = marker.globalMatch(withoutBreaks(text));
    while (matches.hasNext()) {
        ids << matches.next().captured(1);
    }
    return ids;
}

struct Shown {
    // The values that some text shows as they are.
    QSet<QString> literally;
    // The texts that show one as markup.
    QStringList asMarkup;
};

void inspect(QQuickItem *root, Shown &shown) {
    if (root == nullptr) {
        return;
    }
    QList<QQuickItem *> items;
    collectQuickItems(root, &items);
    for (QQuickItem *item : std::as_const(items)) {
        const bool editor = item->inherits("QQuickTextEdit");
        if (!editor && !item->inherits("QQuickText")) {
            continue;
        }
        const QString text = item->property("text").toString();
        const int format = item->property("textFormat").toInt();
        if (format == Qt::RichText && editor) {
            // Markup of the app's own, in which a value is escaped: the
            // document then reads as the value.
            QString read;
            QMetaObject::invokeMethod(item, "getText", Q_RETURN_ARG(QString, read),
                                      Q_ARG(int, 0), Q_ARG(int, item->property("length").toInt()));
            for (const QString &id : idsIn(read)) {
                shown.literally.insert(id);
            }
            if (text.contains(QStringLiteral("<img"))) {
                shown.asMarkup << QStringLiteral("rich text %1 holds an image").arg(item->objectName());
            }
            continue;
        }
        if (item->objectName() == QLatin1String("globalSearchResultContext")) {
            // The same, in a search result's styled context: without the
            // tags of its matches, and with the entities read, it is the
            // value.
            static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
            QString read = text;
            read.remove(tag);
            read.replace(QLatin1String("&lt;"), QLatin1String("<"))
                .replace(QLatin1String("&gt;"), QLatin1String(">"))
                .replace(QLatin1String("&amp;"), QLatin1String("&"));
            for (const QString &id : idsIn(read)) {
                shown.literally.insert(id);
            }
            // A value that went in unescaped has its own tags.
            static const QRegularExpression foreign(QStringLiteral("<(?!/?font\\b)"));
            if (text.contains(foreign)) {
                shown.asMarkup << QStringLiteral("styled text %1 holds markup of a value").arg(item->objectName());
            }
            continue;
        }
        // An editor that took a value for markup gives it back as HTML.
        if (editor && format != Qt::PlainText && text.contains(QStringLiteral("example.invalid"))) {
            shown.asMarkup << QStringLiteral("editor %1 took a value for markup").arg(item->objectName());
            continue;
        }
        const QStringList ids = idsIn(text);
        if (ids.isEmpty()) {
            continue;
        }
        if (format == Qt::PlainText) {
            for (const QString &id : ids) {
                shown.literally.insert(id);
            }
        } else if (editor || format != Qt::AutoText || Qt::mightBeRichText(text)) {
            const QQmlContext *context = qmlContext(item);
            shown.asMarkup << QStringLiteral("%1 in %2 (%3)")
                                  .arg(ids.join(QLatin1Char(',')),
                                       context ? context->baseUrl().fileName() : QString(),
                                       item->objectName().isEmpty() ? QString::fromLatin1(item->metaObject()->className())
                                                                    : item->objectName());
        }
    }
}

// Every text of the application's windows, of their popups, and of the
// shared tooltip as it shows each item's own.
Shown inspectWindows(QQuickWindow *main) {
    Shown shown;
    QList<QQuickItem *> roots;
    const auto windows = QGuiApplication::allWindows();
    for (QWindow *window : windows) {
        if (auto *quickWindow = qobject_cast<QQuickWindow *>(window)) {
            roots << quickWindow->contentItem();
        }
    }
    const auto objects = main->findChildren<QObject *>();
    for (QObject *object : objects) {
        if (object->inherits("QQuickPopup")) {
            if (auto *content = qvariant_cast<QQuickItem *>(object->property("contentItem"))) {
                roots << content;
            }
        }
    }
    QList<QPointer<QQuickItem>> items;
    for (QQuickItem *root : std::as_const(roots)) {
        inspect(root, shown);
        QList<QQuickItem *> found;
        collectQuickItems(root, &found);
        for (QQuickItem *item : std::as_const(found)) {
            items << item;
        }
    }
    // A popup of a list's row, such as an unreadable row's reason: the
    // row's own child, not the window's, as delegates are the view's.
    for (const QPointer<QQuickItem> &item : std::as_const(items)) {
        if (item.isNull()) {
            continue;
        }
        const auto children = item->findChildren<QObject *>(Qt::FindDirectChildrenOnly);
        for (QObject *child : children) {
            const auto popups = child->inherits("QQuickPopup")
                ? QList<QObject *>{child}
                : child->findChildren<QObject *>(Qt::FindDirectChildrenOnly);
            for (QObject *popup : popups) {
                if (popup->inherits("QQuickPopup") && !objects.contains(popup)) {
                    inspect(qvariant_cast<QQuickItem *>(popup->property("contentItem")), shown);
                }
            }
        }
    }
    for (const QPointer<QQuickItem> &item : std::as_const(items)) {
        if (item.isNull() || qmlContext(item) == nullptr
            || idsIn(evaluateOn(item, QStringLiteral("ToolTip.text")).toString()).isEmpty()) {
            continue;
        }
        evaluateOn(item, QStringLiteral("ToolTip.show(ToolTip.text)"));
        auto *tip = qvariant_cast<QObject *>(evaluateOn(item, QStringLiteral("ToolTip.toolTip")));
        if (tip != nullptr) {
            inspect(qvariant_cast<QQuickItem *>(tip->property("contentItem")), shown);
        }
        evaluateOn(item, QStringLiteral("ToolTip.hide()"));
    }
    return shown;
}

// A text that may load an image does so, which shows that the recorder
// sees what a text loads. The image reader has been given any image of an
// earlier text by then.
bool requestsAreSeen(QQmlEngine &engine, RequestRecorder &recorder) {
    static int serial = 0;
    const QString url = QStringLiteral("http://example.invalid/control/%1.png").arg(++serial);
    const QByteArray source = QStringLiteral("import QtQuick\nText { text: '<img src=\"%1\">control' }").arg(url).toUtf8();
    QString error;
    const std::unique_ptr<QObject> control(createQmlObjectFromSource(engine, source, QUrl(), &error));
    if (!control || !QTest::qWaitFor([&] { return recorder.requests().contains(url); })) {
        return false;
    }
    QTest::qWait(50);
    return true;
}

// What is wrong with the windows' texts, with `expected` the values that
// must show: nothing, where all is well.
QString problems(QQuickWindow *main, QQmlEngine &engine, RequestRecorder &recorder, const QStringList &expected) {
    const Shown shown = inspectWindows(main);
    QStringList found;
    if (!requestsAreSeen(engine, recorder)) {
        found << QStringLiteral("the test can't see requests");
    }
    for (const QString &text : shown.asMarkup) {
        found << QStringLiteral("markup: ") + text;
    }
    for (const QString &id : expected) {
        if (!shown.literally.contains(id)) {
            found << QStringLiteral("not shown literally: ") + id;
        }
    }
    const QStringList requests = recorder.requests();
    for (const QString &url : requests) {
        if (!url.contains(QStringLiteral("/control/"))) {
            found << QStringLiteral("requested: ") + url;
        }
    }
    found.removeDuplicates();
    return found.join(QLatin1Char('\n'));
}

// Main.qml over its fallback bridge, with the engine's requests recorded.
struct RecordedWindow {
    bool load() {
        engine.setNetworkAccessManagerFactory(&recorder);
        window = loadMainWindowShown(engine, libraryModel);
        if (window != nullptr) {
            bridge = qvariant_cast<QObject *>(window->property("uiBridge"));
        }
        return bridge != nullptr;
    }
    QObject *child(const char *name) const { return window->findChild<QObject *>(QString::fromLatin1(name)); }
    // The message is the system's text too, so that its line is checked.
    void report(int category, const QString &subject, const QString &message) const {
        QMetaObject::invokeMethod(bridge, "bridgeError", Q_ARG(QString, message), Q_ARG(int, category),
                                  Q_ARG(int, 1), Q_ARG(QString, subject), Q_ARG(QString, message));
    }
    QString check(const QStringList &expected) {
        QTest::qWait(100);
        return problems(window, engine, recorder, expected);
    }

    // Before the engine, which asks it from its own threads until it goes.
    RequestRecorder recorder;
    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    QObject *bridge = nullptr;
};

QString writeCover(const QTemporaryDir &folder) {
    const QString path = folder.filePath(QStringLiteral("cover.png"));
    QImage cover(16, 16, QImage::Format_RGB32);
    cover.fill(Qt::darkGray);
    return cover.save(path) ? QUrl::fromLocalFile(path).toString() : QString();
}

// A QML source with its strings, regular expressions and comments blanked.
QString codeOf(const QString &source) {
    QString code = source;
    QChar last;
    const qsizetype size = source.size();
    const auto blankTo = [&](qsizetype from, qsizetype to) {
        for (qsizetype i = from; i < std::min(to, size); ++i) {
            if (code.at(i) != QLatin1Char('\n')) {
                code[i] = QLatin1Char(' ');
            }
        }
    };
    for (qsizetype i = 0; i < size; ++i) {
        const QChar c = source.at(i);
        const QStringView rest = QStringView(source).mid(i);
        if (rest.startsWith(QLatin1String("//"))) {
            qsizetype end = source.indexOf(QLatin1Char('\n'), i);
            end = end < 0 ? size : end;
            blankTo(i, end);
            i = end;
        } else if (rest.startsWith(QLatin1String("/*"))) {
            qsizetype end = source.indexOf(QLatin1String("*/"), i + 2);
            end = end < 0 ? size : end + 2;
            blankTo(i, end);
            i = end - 1;
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`')
                   || (c == QLatin1Char('/') && QStringLiteral("(,=:[!&|?").contains(last))) {
            qsizetype end = i + 1;
            bool inBrackets = false;
            while (end < size) {
                const QChar next = source.at(end);
                if (next == QLatin1Char('\\')) {
                    end += 2;
                    continue;
                }
                if (next == c && !inBrackets) {
                    break;
                }
                if (c == QLatin1Char('/')) {
                    inBrackets = next == QLatin1Char('[') || (inBrackets && next != QLatin1Char(']'));
                }
                ++end;
            }
            blankTo(i + 1, end);
            i = end;
            last = QLatin1Char('"');
        } else if (!c.isSpace()) {
            last = c;
        }
    }
    return code;
}

struct TextItemScan {
    // The formats other than PlainText that the source sets.
    QStringList otherFormats;
    // What has no safe format of its own.
    QStringList problems;
};

// The text items of a QML source, by what each states. The types here draw
// markup of any text that looks like it, unless told otherwise.
TextItemScan scanTextItems(const QString &source, const QString &file) {
    static const QSet<QString> textTypes{
        QStringLiteral("Text"), QStringLiteral("Label"), QStringLiteral("TextEdit"),
        QStringLiteral("TextArea"), QStringLiteral("Heading"), QStringLiteral("SelectableLabel")};
    // These decide the format inside, where their user can't.
    static const QHash<QString, QString> wrappers{
        {QStringLiteral("ToolTip"), QStringLiteral("components/PlainToolTip.qml")},
        {QStringLiteral("InlineMessage"), QStringLiteral("components/PlainInlineMessage.qml")},
        {QStringLiteral("MessageDialog"), QString()},
        {QStringLiteral("PlaceholderMessage"), QString()},
        {QStringLiteral("PromptDialog"), QString()}};
    // These hold their text back, so their users set a message instead.
    static const QSet<QString> plainComponents{
        QStringLiteral("PlainToolTip"), QStringLiteral("PlainInlineMessage")};
    static const QRegularExpression token(
        QStringLiteral("[{}]|(?<![\\w.])textFormat\\s*:\\s*([\\w.]+)"
                       "|(?<![\\w.])contentItem\\s*:|(?<![\\w.])text\\s*:"));
    static const QRegularExpression type(
        QStringLiteral("(?:^|(?<=[\\s:;{\\[,]))(?:[A-Za-z_]\\w*\\.)*([A-Z]\\w*)(?:\\s+on\\s+[\\w.]+)?\\s*\\z"));

    struct Object {
        QString type;
        int line{0};
        bool hasFormat{false};
        bool hasContent{false};
        bool hasText{false};
    };
    TextItemScan scan;
    const QString code = codeOf(source);
    QList<Object> open;
    auto tokens = token.globalMatch(code);
    while (tokens.hasNext()) {
        const QRegularExpressionMatch match = tokens.next();
        const qsizetype at = match.capturedStart();
        if (match.captured() == QLatin1String("{")) {
            const qsizetype from = std::max<qsizetype>(0, at - 160);
            open.append({type.match(code.mid(from, at - from)).captured(1),
                         int(code.left(at).count(QLatin1Char('\n'))) + 1});
        } else if (open.isEmpty()) {
            scan.problems << file + QStringLiteral(": unbalanced braces");
            break;
        } else if (match.captured() == QLatin1String("}")) {
            const Object object = open.takeLast();
            const QString where = QStringLiteral("%1:%2: %3").arg(file).arg(object.line).arg(object.type);
            if (textTypes.contains(object.type) && !object.hasFormat) {
                scan.problems << where + QStringLiteral(" states no textFormat");
            }
            if (wrappers.contains(object.type) && file != wrappers.value(object.type)
                && !(object.type == QLatin1String("ToolTip") && object.hasContent)) {
                scan.problems << where + QStringLiteral(" renders markup; use a plain component instead");
            }
            if (plainComponents.contains(object.type) && object.hasText) {
                scan.problems << where + QStringLiteral(" takes a message, not a text");
            }
        } else if (match.capturedStart(1) >= 0) {
            open.last().hasFormat = true;
            const QString format = match.captured(1);
            if (format != QLatin1String("Text.PlainText") && format != QLatin1String("TextEdit.PlainText")) {
                scan.otherFormats << format;
            }
        } else if (match.captured().startsWith(QLatin1String("text"))) {
            open.last().hasText = true;
        } else {
            open.last().hasContent = true;
        }
    }
    if (!open.isEmpty()) {
        scan.problems << file + QStringLiteral(": unbalanced braces");
    }
    return scan;
}

} // namespace

void PlainTextSmokeTest::theMainWindowShowsMarkupLiterally() {
    RecordedWindow ui;
    QVERIFY(ui.load());
    ui.window->resize(1400, 900);
    const QString path = QStringLiteral("/fixture/track.flac");
    ui.bridge->setProperty("queueRows", QVariantList{QVariantMap{
        {QStringLiteral("title"), hostile(QStringLiteral("queuetitle"))},
        {QStringLiteral("artist"), hostile(QStringLiteral("queueartist"))},
        {QStringLiteral("album"), hostile(QStringLiteral("queuealbum"))},
        {QStringLiteral("lengthText"), hostile(QStringLiteral("queuelength"))},
        {QStringLiteral("path"), path}}});
    ui.bridge->setProperty("queueLength", 1);

    BinaryTreeRow artist;
    artist.rowType = 1;
    artist.childCount = 1;
    artist.title = hostile(QStringLiteral("libraryartist"));
    artist.key = QStringLiteral("artist|Artist");
    artist.artist = artist.title;
    artist.path = QStringLiteral("/fixture/Artist");
    BinaryTreeRow album;
    album.rowType = 2;
    album.depth = 1;
    album.sourceIndex = 0;
    album.childCount = 1;
    album.title = hostile(QStringLiteral("libraryalbum"));
    album.key = QStringLiteral("album|Artist|Album");
    album.artist = artist.title;
    album.path = QStringLiteral("/fixture/Artist/Album");
    ui.libraryModel.setExpandedKeys({artist.key});
    ui.libraryModel.setLibraryTreeFromBinary(encodeRows({artist, album}));

    ui.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    ui.bridge->setProperty("currentTrackPath", path);
    ui.bridge->setProperty("currentTrackTitle", hostile(QStringLiteral("nowtitle")));
    ui.bridge->setProperty("currentTrackArtist", hostile(QStringLiteral("nowartist")));
    ui.bridge->setProperty("currentTrackAlbum", hostile(QStringLiteral("nowalbum")));
    ui.bridge->setProperty("currentTrackGenre", hostile(QStringLiteral("nowgenre")));
    ui.bridge->setProperty("currentTrackFormatLabel", hostile(QStringLiteral("nowformat")));
    ui.bridge->setProperty("libraryScanInProgress", true);
    ui.bridge->setProperty("libraryRootEntries", QVariantList{QVariantMap{
        {QStringLiteral("name"), hostile(QStringLiteral("scanfolder"))},
        {QStringLiteral("path"), QStringLiteral("/fixture")}}});
    ui.bridge->setProperty("lastFmBuildConfigured", true);
    ui.bridge->setProperty("lastFmUsername", QStringLiteral("listener"));
    ui.bridge->setProperty("lastFmScrobblingEnabled", true);
    ui.bridge->setProperty("lastFmAuthState", 2);
    ui.bridge->setProperty("lastFmScrobbleState", 4);
    ui.bridge->setProperty("lastFmScrobbleMessage", hostile(QStringLiteral("scrobbleerror")));
    ui.report(1, path, hostile(QStringLiteral("unreadable")));
    // Last.fm's reason, while the engine runs: the Last.fm slot, whose
    // scrobbler runs in the engine, shows only then.
    ui.bridge->setProperty("connected", true);
    QTRY_COMPARE(ui.libraryModel.rowCount(), 2);
    const QString scrobble = ui.check({QStringLiteral("scrobbleerror")});
    QVERIFY2(scrobble.isEmpty(), qPrintable(scrobble));

    // The rest once the engine has stopped, and says why.
    ui.bridge->setProperty("backendFailure", hostile(QStringLiteral("backendfailure")));
    const QString wrong = ui.check({
        QStringLiteral("queuetitle"), QStringLiteral("queueartist"), QStringLiteral("queuealbum"),
        QStringLiteral("queuelength"), QStringLiteral("libraryartist"), QStringLiteral("libraryalbum"),
        QStringLiteral("nowtitle"), QStringLiteral("nowartist"), QStringLiteral("nowalbum"),
        QStringLiteral("nowformat"), QStringLiteral("scanfolder"), QStringLiteral("unreadable"),
        QStringLiteral("backendfailure")});
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
}

void PlainTextSmokeTest::theViewersShowMarkupLiterally() {
    QTemporaryDir folder;
    const QString cover = writeCover(folder);
    QVERIFY(!cover.isEmpty());
    RecordedWindow ui;
    QVERIFY(ui.load());
    QObject *viewer = ui.child("viewerController");
    QVERIFY(viewer != nullptr);
    ui.bridge->setProperty("viewerFullscreenMode", 1);
    ui.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    ui.bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/track.flac"));
    ui.bridge->setProperty("currentTrackCoverPath", cover);
    ui.bridge->setProperty("currentTrackTitle", hostile(QStringLiteral("striptitle")));
    ui.bridge->setProperty("currentTrackArtist", hostile(QStringLiteral("stripartist")));
    ui.bridge->setProperty("currentTrackAlbum", hostile(QStringLiteral("stripalbum")));

    QVERIFY(QMetaObject::invokeMethod(viewer, "openAlbumArtViewer"));
    viewer->setProperty("albumArtViewerFileInfo", QVariantMap{
        {QStringLiteral("fileName"), hostileName(QStringLiteral("artfile"))},
        {QStringLiteral("format"), hostile(QStringLiteral("artformat"))}});
    viewer->setProperty("albumArtInfoVisible", true);
    const QString wrong = ui.check({
        QStringLiteral("striptitle"), QStringLiteral("stripartist"), QStringLiteral("stripalbum"),
        QStringLiteral("artfile"), QStringLiteral("artformat")});
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
}

void PlainTextSmokeTest::theDialogsShowMarkupLiterally() {
    RecordedWindow ui;
    QVERIFY(ui.load());
    QObject *preferences = ui.child("preferencesDialog");
    QObject *recentIssues = ui.child("recentErrorsDialog");
    QObject *itunes = ui.child("itunesArtworkDialog");
    QObject *confirm = ui.child("replaceCoverConfirmDialog");
    QVERIFY(preferences != nullptr && recentIssues != nullptr && itunes != nullptr && confirm != nullptr);

    ui.bridge->setProperty("libraryRootEntries", QVariantList{QVariantMap{
        {QStringLiteral("name"), hostile(QStringLiteral("rootname"))},
        {QStringLiteral("path"), QStringLiteral("/fixture/") + hostileName(QStringLiteral("rootpath"))},
        {QStringLiteral("scanProblem"), hostile(QStringLiteral("scanproblem"))}}});
    ui.bridge->setProperty("lastFmBuildConfigured", true);
    ui.bridge->setProperty("lastFmUsername", hostile(QStringLiteral("lastfmuser")));
    ui.bridge->setProperty("lastFmStatusText", hostile(QStringLiteral("lastfmstatus")));
    QVERIFY(QMetaObject::invokeMethod(preferences, "openLastFmPage"));

    ui.report(4, QStringLiteral("/fixture/") + hostileName(QStringLiteral("issuesubject")),
              hostile(QStringLiteral("issuemessage")));
    QVERIFY(QMetaObject::invokeMethod(recentIssues, "open"));

    ui.bridge->setProperty("itunesArtworkStatusText", hostile(QStringLiteral("itunesstatus")));
    ui.bridge->setProperty("itunesArtworkResults", QVariantList{QVariantMap{
        {QStringLiteral("albumTitle"), hostile(QStringLiteral("itunesalbum"))},
        {QStringLiteral("artistName"), hostile(QStringLiteral("itunesartist"))},
        {QStringLiteral("assetError"), hostile(QStringLiteral("itunesasset"))},
        {QStringLiteral("detailStatusText"), hostile(QStringLiteral("itunesdetail"))}}});
    QVERIFY(QMetaObject::invokeMethod(itunes, "open"));

    confirm->setProperty("message", hostile(QStringLiteral("confirmmessage")));
    confirm->setProperty("details", hostile(QStringLiteral("confirmdetails")));
    confirm->setProperty("suggestion", QVariantMap{
        {QStringLiteral("albumTitle"), hostile(QStringLiteral("chosentitle"))},
        {QStringLiteral("artistName"), hostile(QStringLiteral("chosenartist"))}});
    confirm->setProperty("path", QStringLiteral("/fixture/") + hostileName(QStringLiteral("confirmpath")));

    const QString wrong = ui.check({
        QStringLiteral("rootname"), QStringLiteral("rootpath"), QStringLiteral("scanproblem"),
        QStringLiteral("lastfmuser"), QStringLiteral("lastfmstatus"), QStringLiteral("issuemessage"),
        QStringLiteral("itunesasset"),
        QStringLiteral("itunesstatus"), QStringLiteral("itunesalbum"), QStringLiteral("confirmmessage"),
        QStringLiteral("confirmdetails"), QStringLiteral("confirmpath"),
        QStringLiteral("chosentitle"), QStringLiteral("chosenartist")});
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
}

void PlainTextSmokeTest::theTagEditorShowsMarkupLiterally() {
    RequestRecorder recorder;
    TagEditorController editor{nullptr};
    const QString path = QStringLiteral("/fixture/track.flac");
    const QByteArray loaded = QJsonDocument(QJsonObject{
        {QStringLiteral("resolvedPaths"), QJsonArray{path}},
        {QStringLiteral("rows"), QJsonArray{QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("fileName"), hostileName(QStringLiteral("tagfile"))},
            {QStringLiteral("directory"), QStringLiteral("/fixture")},
            {QStringLiteral("formatKind"), QStringLiteral("FLAC")},
            {QStringLiteral("title"), hostile(QStringLiteral("tagtitle"))},
            {QStringLiteral("artist"), hostile(QStringLiteral("tagartist"))},
            {QStringLiteral("album"), hostile(QStringLiteral("tagalbum"))}}}},
    }).toJson(QJsonDocument::Compact);
    const QByteArray failed = QJsonDocument(QJsonObject{
        {QStringLiteral("results"), QJsonArray{QJsonObject{
            {QStringLiteral("path"), path},
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), hostile(QStringLiteral("tagerror"))}}}},
        {QStringLiteral("successfulPaths"), QJsonArray{}},
    }).toJson(QJsonDocument::Compact);
    editor.m_executeOperation = [loaded, failed](TagEditorController::Operation operation, const QByteArray &,
                                                 const TagEditorController::RenameGate &) {
        return operation == TagEditorController::Operation::Save ? failed : loaded;
    };

    LibraryTreeModel libraryModel;
    QQmlApplicationEngine engine;
    engine.setNetworkAccessManagerFactory(&recorder);
    engine.rootContext()->setContextProperty(QStringLiteral("tagEditor"), &editor);
    QQuickWindow *window = loadMainWindowShown(engine, libraryModel);
    QVERIFY(window != nullptr);
    auto *dialog = window->findChild<QObject *>(QStringLiteral("tagEditorDialog"));
    auto *details = window->findChild<QObject *>(QStringLiteral("tagEditorStatusDetailsDialog"));
    auto *rename = window->findChild<QObject *>(QStringLiteral("renamePreviewDialog"));
    QVERIFY(dialog != nullptr && details != nullptr && rename != nullptr);
    QVERIFY(editor.openForPaths({path}));
    QTRY_VERIFY(!editor.loading() && dialog->property("opened").toBool());
    editor.setCell(0, QStringLiteral("genre"), QStringLiteral("Ambient"));
    QSignalSpy saved(&editor, &TagEditorController::saveFinished);
    QVERIFY(editor.save());
    QTRY_COMPARE(saved.size(), 1);
    QVERIFY(QMetaObject::invokeMethod(details, "open"));

    rename->setProperty("rows", QVariantList{
        QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("unchanged"), false},
                    {QStringLiteral("oldName"), hostileName(QStringLiteral("oldname"))},
                    {QStringLiteral("newName"), hostileName(QStringLiteral("newname"))}},
        QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("unchanged"), false},
                    {QStringLiteral("oldName"), hostileName(QStringLiteral("refusedname"))},
                    {QStringLiteral("error"), hostile(QStringLiteral("renameerror"))}}});
    QVERIFY(QMetaObject::invokeMethod(rename, "open"));
    QTest::qWait(100);

    const QString wrong = problems(window, engine, recorder, {
        QStringLiteral("tagfile"), QStringLiteral("tagtitle"), QStringLiteral("tagartist"),
        QStringLiteral("tagalbum"), QStringLiteral("tagerror"), QStringLiteral("oldname"),
        QStringLiteral("newname"), QStringLiteral("refusedname"), QStringLiteral("renameerror")});
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
}

void PlainTextSmokeTest::searchShowsMarkupLiterally() {
    GlobalSearchResultsModel model;
    GlobalSearchResultsModel empty;
    RecordedWindow ui;
    QVERIFY(ui.load());
    QObject *dialog = ui.child("globalSearchDialog");
    QObject *controller = ui.child("globalSearchController");
    QVERIFY(dialog != nullptr && controller != nullptr);
    GlobalSearchResultsModel::SearchDisplayRow row;
    row.kind = QStringLiteral("item");
    row.rowType = QStringLiteral("track");
    row.label = hostile(QStringLiteral("searchtitle"));
    row.artist = hostile(QStringLiteral("searchartist"));
    row.album = hostile(QStringLiteral("searchalbum"));
    row.matchDetail = hostile(QStringLiteral("searchdetail"));
    row.lengthText = hostile(QStringLiteral("searchlength"));
    row.trackPath = QStringLiteral("/fixture/track.flac");
    model.replaceRows({row});
    ui.bridge->setProperty("globalSearchModel", QVariant::fromValue<QObject *>(&model));
    controller->setProperty("recentQueries", QStringList{hostile(QStringLiteral("recentquery"))});
    QVERIFY(QMetaObject::invokeMethod(controller, "openDialog"));
    QTRY_VERIFY(dialog->property("opened").toBool());

    auto *content = qvariant_cast<QQuickItem *>(dialog->property("contentItem"));
    QVERIFY(content != nullptr);
    QQuickItem *field = findQuickItemByObjectName(content, QStringLiteral("globalSearchQueryField"));
    QVERIFY(field != nullptr);
    field->setProperty("text", QStringLiteral("more"));
    QTest::qWait(100);
    QList<QQuickItem *> items;
    collectQuickItems(content, &items);
    for (QQuickItem *item : std::as_const(items)) {
        if (item->metaObject()->indexOfMethod("toolTipText()") >= 0 && item->property("isResult").toBool()) {
            dialog->setProperty("hoveredResultRow", QVariant::fromValue(item));
            // The tooltip takes a row's text as the pointer rests on it.
            QVariant tip;
            QVERIFY(QMetaObject::invokeMethod(item, "toolTipText", Q_RETURN_ARG(QVariant, tip)));
            dialog->setProperty("toolTipText", tip);
        }
    }
    QString wrong = ui.check({QStringLiteral("searchtitle"), QStringLiteral("searchartist"),
                              QStringLiteral("searchalbum"), QStringLiteral("searchdetail"),
                              QStringLiteral("searchlength")});
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));

    field->setProperty("text", QString());
    wrong = ui.check({QStringLiteral("recentquery")});
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));

    dialog->setProperty("hoveredResultRow", QVariant::fromValue<QQuickItem *>(nullptr));
    ui.bridge->setProperty("globalSearchModel", QVariant::fromValue<QObject *>(&empty));
    field->setProperty("text", hostile(QStringLiteral("typedquery")));
    wrong = ui.check({QStringLiteral("typedquery")});
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
}

// The components that set the style's format after the style made its
// label must never lay out a value that they are made with as markup.
void PlainTextSmokeTest::textFromBeforeTheWindowShowsLiterally() {
    RequestRecorder recorder;
    QQmlEngine engine;
    engine.setNetworkAccessManagerFactory(&recorder);
    engine.rootContext()->setContextProperty(QStringLiteral("failure"), hostile(QStringLiteral("earlyfailure")));
    engine.rootContext()->setContextProperty(QStringLiteral("tip"), hostile(QStringLiteral("earlytip")));
    QString error;
    const std::unique_ptr<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "components" as Components
ApplicationWindow {
    width: 400
    height: 200
    visible: true
    Components.PlainInlineMessage {
        objectName: "earlyMessage"
        width: parent.width
        visible: true
        message: failure
    }
    Components.PlainToolTip {
        id: earlyTip
        visible: true
        message: tip
    }
}
)QML"), QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/PlainTextHarness.qml")),
        &error));
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY2(window != nullptr, qPrintable(error));
    QVERIFY(QTest::qWaitForWindowExposed(window));
    const QString wrong = problems(window, engine, recorder,
                                   {QStringLiteral("earlyfailure"), QStringLiteral("earlytip")});
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
}

// Text that is not markup draws the same pixels as it did as AutoText.
void PlainTextSmokeTest::ordinaryTextLooksAsWithAutoText() {
    RecordedWindow ui;
    QVERIFY(ui.load());
    ui.window->resize(900, 600);
    ui.bridge->setProperty("queueRows", QVariantList{
        QVariantMap{{QStringLiteral("title"), QStringLiteral("Salt & Stone's \"Harbour\" < 3")},
                    {QStringLiteral("artist"), QString::fromUtf8("Björk Ægir 東京")},
                    {QStringLiteral("album"), QStringLiteral("A long album name that is cut short where its column ends")},
                    {QStringLiteral("path"), QStringLiteral("/fixture/one.flac")}},
        QVariantMap{{QStringLiteral("title"), QString::fromUtf8("שלום עולם")},
                    {QStringLiteral("artist"), QString::fromUtf8("مرحبا & أهلا")},
                    {QStringLiteral("album"), QStringLiteral("Two\nlines")},
                    {QStringLiteral("path"), QStringLiteral("/fixture/two.flac")}}});
    ui.bridge->setProperty("queueLength", 2);
    // The semibold title, beside the queue's regular rows.
    ui.bridge->setProperty("playbackState", QStringLiteral("Paused"));
    ui.bridge->setProperty("currentTrackPath", QStringLiteral("/fixture/one.flac"));
    ui.bridge->setProperty("currentTrackTitle", QStringLiteral("Salt & Stone's \"Harbour\" < 3"));
    QTest::qWait(100);

    const auto settled = [&ui] {
        QImage last = ui.window->grabWindow();
        for (int tries = 0; tries < 40; ++tries) {
            QTest::qWait(50);
            const QImage next = ui.window->grabWindow();
            if (next == last) {
                break;
            }
            last = next;
        }
        return last;
    };
    const QImage plain = settled();
    QVERIFY(!plain.isNull());
    QList<QQuickItem *> items;
    collectQuickItems(ui.window->contentItem(), &items);
    int changed = 0;
    for (QQuickItem *item : std::as_const(items)) {
        if (item->inherits("QQuickText") && item->property("textFormat").toInt() == Qt::PlainText) {
            QVERIFY(item->setProperty("textFormat", int(Qt::AutoText)));
            ++changed;
        }
    }
    QVERIFY(changed > 0);
    QCOMPARE(settled(), plain);
}

void PlainTextSmokeTest::plainToolTipsReadAsTheStyles_data() {
    QTest::addColumn<QString>("text");
    QTest::newRow("ampersand") << QStringLiteral("Salt & Stone");
    QTest::newRow("ampersand in a word") << QStringLiteral("R&B Classics of 1994");
    QTest::newRow("ampersand in the last word but one") << QStringLiteral("Rock&Roll Party");
    QTest::newRow("quotes") << QStringLiteral("It's the \"Harbour\" < 3 > 2");
    QTest::newRow("lines") << QStringLiteral("Title\nArtist\nAlbum");
    QTest::newRow("long") << QStringLiteral("/fixture/a folder with a long name/and one in it/with a longer name still.flac");
    QTest::newRow("right to left") << QString::fromUtf8("שלום עולם & مرحبا");
    QTest::newRow("CJK") << QString::fromUtf8("東京の夜");
}

// Both ways to a plain tooltip give what the style's tooltip gives for text
// that is not markup.
void PlainTextSmokeTest::plainToolTipsReadAsTheStyles() {
    QFETCH(QString, text);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("sample"), text);
    QString error;
    const std::unique_ptr<QObject> root(createQmlObjectFromSource(engine, QByteArrayLiteral(R"QML(
import QtQuick
import QtQuick.Controls
import "components" as Components
import "logic/PlainText.js" as PlainText
ApplicationWindow {
    id: window
    width: 600
    height: 400
    visible: true
    property var tips: [styles, plain, shared.ToolTip.toolTip]
    ToolTip { id: styles; visible: true; text: sample }
    Components.PlainToolTip { id: plain; visible: true; message: sample }
    Item {
        id: shared
        Component.onCompleted: {
            PlainText.apply(ToolTip.toolTip.contentItem)
            ToolTip.show(sample)
        }
    }
}
)QML"), QUrl::fromLocalFile(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/ToolTipHarness.qml")),
        &error));
    QVERIFY2(root != nullptr, qPrintable(error));
    QTest::qWait(50);
    const QVariantList tips = root->property("tips").value<QJSValue>().toVariant().toList();
    QCOMPARE(tips.size(), 3);
    QList<QVariantList> read;
    for (const QVariant &tip : tips) {
        auto *content = qvariant_cast<QQuickItem *>(qvariant_cast<QObject *>(tip)->property("contentItem"));
        QVERIFY(content != nullptr);
        QList<QQuickItem *> items;
        collectQuickItems(content, &items);
        for (QQuickItem *item : std::as_const(items)) {
            if (item->inherits("QQuickText")) {
                read << QVariantList{item->property("text"), item->property("contentWidth"),
                                     item->property("contentHeight"), item->property("lineCount"),
                                     content->implicitWidth(), content->implicitHeight()};
            }
        }
    }
    QCOMPARE(read.size(), 3);
    QCOMPARE(read[1][0].toString(), text);
    QCOMPARE(read[1], read[0]);
    QCOMPARE(read[2], read[0]);
}

void PlainTextSmokeTest::everyTextItemStatesASafeFormat_data() {
    QTest::addColumn<QString>("file");
    const QDir qml(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml"));
    QDirIterator files(qml.path(), {QStringLiteral("*.qml")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString file = qml.relativeFilePath(files.next());
        QTest::newRow(qPrintable(file)) << file;
    }
}

void PlainTextSmokeTest::everyTextItemStatesASafeFormat() {
    // The markup the app means. Nothing from outside goes into it unescaped.
    static const QHash<QString, QStringList> intendedMarkup{
        // The menus' own titles, with the mnemonic underlined while Alt is held.
        {QStringLiteral("components/AppMenuBarItem.qml"), {QStringLiteral("Text.AutoText")}},
        // The menus' items, the same; Kirigami's mnemonic label escapes the words
        // (tst_ui_menus' menuItemGlyphsGrowWithTheFont).
        {QStringLiteral("components/AppMenuItem.qml"), {QStringLiteral("Text.StyledText")}},
        // The forms' own labels, underlined in the same way.
        {QStringLiteral("preferences/PreferencesForm.qml"), {QStringLiteral("Text.AutoText")}},
        // A search result's context, whose matches are in the text's colour; escaped() escapes every part.
        {QStringLiteral("dialogs/GlobalSearchDialog.qml"), {QStringLiteral("Text.StyledText")}},
        // KDE's description, a message of the app's own with no arguments.
        {QStringLiteral("dialogs/AboutKdeDialog.qml"), {QStringLiteral("Text.StyledText")}},
        // The details and their sizer, whose values escaped() escapes.
        {QStringLiteral("dialogs/TagEditorStatusDetailsDialog.qml"),
         {QStringLiteral("TextEdit.RichText"), QStringLiteral("TextEdit.RichText")}}};

    QFETCH(QString, file);
    QFile source(QStringLiteral(FERROUS_UI_SOURCE_DIR) + QStringLiteral("/qml/") + file);
    QVERIFY(source.open(QIODevice::ReadOnly));
    const TextItemScan scan = scanTextItems(QString::fromUtf8(source.readAll()), file);
    QVERIFY2(scan.problems.isEmpty(), qPrintable(scan.problems.join(QLatin1Char('\n'))));
    QCOMPARE(scan.otherFormats, intendedMarkup.value(file));
}

void PlainTextSmokeTest::theFormatCheckFindsAnUnsafeItem_data() {
    QTest::addColumn<QString>("source");
    QTest::addColumn<bool>("safe");
    QTest::newRow("label") << QStringLiteral("Label {\n text: name\n}") << false;
    QTest::newRow("qualified") << QStringLiteral("Item { Controls.Label { text: name } }") << false;
    QTest::newRow("delegate") << QStringLiteral("ListView { delegate: Text { text: name } }") << false;
    QTest::newRow("inline component") << QStringLiteral("Item { component Cell: Text { text: name } }") << false;
    QTest::newRow("property") << QStringLiteral("Item { property Item cell: Text { text: name } }") << false;
    QTest::newRow("format of a child") << QStringLiteral("Label { Text { textFormat: Text.PlainText } }") << false;
    QTest::newRow("format in a comment") << QStringLiteral("Text {\n // textFormat: Text.PlainText\n}") << false;
    QTest::newRow("tooltip") << QStringLiteral("ToolTip { text: name }") << false;
    QTest::newRow("inline message") << QStringLiteral("Kirigami.InlineMessage { text: name }") << false;
    QTest::newRow("plain tooltip given a text") << QStringLiteral("Components.PlainToolTip { text: name }") << false;
    QTest::newRow("plain message given a text") << QStringLiteral("PlainInlineMessage {\n text: name\n}") << false;
    QTest::newRow("plain tooltip given a message") << QStringLiteral("Components.PlainToolTip { message: name }") << true;
    QTest::newRow("plain") << QStringLiteral("Label { text: \"{\" + name; textFormat: Text.PlainText }") << true;
    QTest::newRow("after a regular expression")
        << QStringLiteral("Item { property var a: /[{/]/; Text { textFormat: Text.PlainText } }") << true;
    QTest::newRow("tooltip of its own") << QStringLiteral("ToolTip { contentItem: Item {} }") << true;
}

void PlainTextSmokeTest::theFormatCheckFindsAnUnsafeItem() {
    QFETCH(QString, source);
    QFETCH(bool, safe);
    QCOMPARE(scanTextItems(source, QStringLiteral("Sample.qml")).problems.isEmpty(), safe);
}

std::unique_ptr<QmlSmokeTest> makePlainTextSmokeTest(const QString &userDirectoriesParent) {
    return std::make_unique<PlainTextSmokeTest>(userDirectoriesParent);
}

#include "tst_qml_smoke_plain_text.moc"
