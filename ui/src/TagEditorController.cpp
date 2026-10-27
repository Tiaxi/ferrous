// SPDX-License-Identifier: GPL-3.0-or-later

#include "TagEditorController.h"

#include "BridgeClient.h"
#include "FerrousBridgeFfi.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <KLocalizedString>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QtGlobal>

namespace {

QString readString(const QJsonObject &object, const char *key) {
    return object.value(QLatin1String(key)).toString();
}

QString displayNameForPath(const QString &path) {
    return QFileInfo(path).fileName();
}

// "<done>; <failed>.", as in "Saved 2 files; 1 failed."
QString partialResultText(const QString &done, qsizetype failed) {
    return i18nc("@info:status files done; files that failed", "%1; %2.",
                 done, i18ncp("@info:status", "%1 failed", "%1 failed", failed));
}

QString buildActionDetails(const QStringList &successLines, const QStringList &failureLines) {
    QStringList sections;
    if (!successLines.isEmpty()) {
        QStringList lines;
        lines.reserve(successLines.size() + 1);
        lines.push_back(i18nc("@title list of files", "Succeeded:"));
        for (const QString &line : successLines) {
            lines.push_back(QStringLiteral("  ") + line);
        }
        sections.push_back(lines.join(QLatin1Char('\n')));
    }
    if (!failureLines.isEmpty()) {
        QStringList lines;
        lines.reserve(failureLines.size() + 1);
        lines.push_back(i18nc("@title list of files", "Failed:"));
        for (const QString &line : failureLines) {
            lines.push_back(QStringLiteral("  ") + line);
        }
        sections.push_back(lines.join(QLatin1Char('\n')));
    }
    return sections.join(QStringLiteral("\n\n"));
}

TagEditorRowState rowFromJson(const QJsonObject &object) {
    TagEditorRowState row;
    row.path = readString(object, "path");
    row.fileName = readString(object, "fileName");
    row.directory = readString(object, "directory");
    row.formatKind = readString(object, "formatKind");
    row.title = readString(object, "title");
    row.artist = readString(object, "artist");
    row.album = readString(object, "album");
    row.albumArtist = readString(object, "albumArtist");
    row.genre = readString(object, "genre");
    row.year = readString(object, "year");
    row.trackNo = readString(object, "trackNo");
    row.discNo = readString(object, "discNo");
    row.totalTracks = readString(object, "totalTracks");
    row.totalDiscs = readString(object, "totalDiscs");
    row.comment = readString(object, "comment");
    row.originalTitle = row.title;
    row.originalArtist = row.artist;
    row.originalAlbum = row.album;
    row.originalAlbumArtist = row.albumArtist;
    row.originalGenre = row.genre;
    row.originalYear = row.year;
    row.originalTrackNo = row.trackNo;
    row.originalDiscNo = row.discNo;
    row.originalTotalTracks = row.totalTracks;
    row.originalTotalDiscs = row.totalDiscs;
    row.originalComment = row.comment;
    const QJsonObject extraValues = object.value(QStringLiteral("extraValues")).toObject();
    for (auto it = extraValues.constBegin(); it != extraValues.constEnd(); ++it) {
        QStringList values;
        for (const QJsonValue &value : it.value().toArray()) {
            values.push_back(value.toString());
        }
        if (!values.isEmpty()) {
            row.extraValues.insert(it.key(), values);
        }
    }
    return row;
}

QJsonObject rowToJson(const TagEditorRowState &row) {
    QJsonObject object;
    object.insert(QStringLiteral("path"), row.path);
    object.insert(QStringLiteral("fileName"), row.fileName);
    object.insert(QStringLiteral("directory"), row.directory);
    object.insert(QStringLiteral("formatKind"), row.formatKind);
    object.insert(QStringLiteral("title"), row.title);
    object.insert(QStringLiteral("artist"), row.artist);
    object.insert(QStringLiteral("album"), row.album);
    object.insert(QStringLiteral("albumArtist"), row.albumArtist);
    object.insert(QStringLiteral("genre"), row.genre);
    object.insert(QStringLiteral("year"), row.year);
    object.insert(QStringLiteral("trackNo"), row.trackNo);
    object.insert(QStringLiteral("discNo"), row.discNo);
    object.insert(QStringLiteral("totalTracks"), row.totalTracks);
    object.insert(QStringLiteral("totalDiscs"), row.totalDiscs);
    object.insert(QStringLiteral("comment"), row.comment);
    return object;
}

QByteArray buildPathBlob(const QStringList &paths) {
    QJsonArray entries;
    for (const QString &path : paths) {
        QJsonObject object;
        object.insert(QStringLiteral("path"), path);
        entries.push_back(object);
    }
    return QJsonDocument(entries).toJson(QJsonDocument::Compact);
}

QByteArray buildSelectionBlob(const QVariantList &selections) {
    return QJsonDocument::fromVariant(QVariant(selections)).toJson(QJsonDocument::Compact);
}

QByteArray buildSaveBlob(const QVector<TagEditorRowState> &rows) {
    QJsonArray rowArray;
    for (const TagEditorRowState &row : rows) {
        if (!row.dirty) {
            continue;
        }
        // The backend writes only these fields and keeps everything else in the file.
        QJsonObject object = rowToJson(row);
        object.insert(
            QStringLiteral("changedFields"),
            QJsonArray::fromStringList(TagEditorTableModel::changedFields(row)));
        rowArray.push_back(object);
    }
    QJsonObject root;
    root.insert(QStringLiteral("rows"), rowArray);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QString nytTitleCaseWord(const QString &word) {
    QString out = word.toLower();
    bool capitalizeNext = true;
    for (int i = 0; i < out.size(); ++i) {
        if (capitalizeNext && out[i].isLetterOrNumber()) {
            out[i] = out[i].toUpper();
            capitalizeNext = false;
        } else if (out[i] == QLatin1Char('-')) {
            capitalizeNext = true;
        }
    }
    return out;
}

bool isNytSmallWord(const QString &word) {
    static const QSet<QString> smallWords{
        QStringLiteral("a"),   QStringLiteral("an"),  QStringLiteral("and"),
        QStringLiteral("as"),  QStringLiteral("at"),  QStringLiteral("but"),
        QStringLiteral("by"),  QStringLiteral("en"),  QStringLiteral("for"),
        QStringLiteral("if"),  QStringLiteral("in"),  QStringLiteral("of"),
        QStringLiteral("on"),  QStringLiteral("or"),  QStringLiteral("the"),
        QStringLiteral("to"),  QStringLiteral("v"),   QStringLiteral("vs"),
        QStringLiteral("via"),
    };
    return smallWords.contains(word);
}

struct TitleCaseToken {
    QString text;
    bool isWord{false};
};

bool followsStrongBreak(const QVector<TitleCaseToken> &tokens, int tokenIndex) {
    for (int i = tokenIndex - 1; i >= 0; --i) {
        if (tokens[i].isWord) {
            break;
        }
        if (tokens[i].text.contains(QLatin1Char(':'))
            || tokens[i].text.contains(QLatin1Char('?'))
            || tokens[i].text.contains(QLatin1Char('!')))
        {
            return true;
        }
    }
    return false;
}

QString nytHyphenTitleCase(const QString &word, bool firstWord, bool lastWord, bool afterBreak) {
    const QStringList parts = word.split(QLatin1Char('-'));
    if (parts.isEmpty()) {
        return word;
    }

    QStringList outParts;
    outParts.reserve(parts.size());
    for (int i = 0; i < parts.size(); ++i) {
        const QString lowered = parts[i].toLower();
        if (i == 0) {
            if (!firstWord && !lastWord && !afterBreak && isNytSmallWord(lowered)) {
                outParts.push_back(lowered);
            } else {
                outParts.push_back(nytTitleCaseWord(parts[i]));
            }
            continue;
        }

        const QString previousLowered = parts[i - 1].toLower();
        const bool doubledVowelPrefix = previousLowered.size() >= 2
            && previousLowered.size() <= 3
            && !previousLowered.isEmpty()
            && !lowered.isEmpty()
            && previousLowered.back() == lowered.front()
            && QStringLiteral("aeiou").contains(previousLowered.back());
        if (i == 1 && doubledVowelPrefix) {
            outParts.push_back(lowered);
            continue;
        }
        outParts.push_back(nytTitleCaseWord(parts[i]));
    }
    return outParts.join(QLatin1Char('-'));
}

QString englishTitleCaseText(const QString &text) {
    QVector<TitleCaseToken> tokens;
    QString current;
    bool inWord = false;
    const auto flushCurrent = [&tokens, &current, &inWord]() {
        if (current.isEmpty()) {
            return;
        }
        tokens.push_back(TitleCaseToken{current, inWord});
        current.clear();
    };

    for (const QChar ch : text) {
        const bool wordChar = ch.isLetterOrNumber() || ch == QLatin1Char('\'');
        if (current.isEmpty()) {
            current.append(ch);
            inWord = wordChar;
            continue;
        }
        if (wordChar == inWord) {
            current.append(ch);
            continue;
        }
        flushCurrent();
        current.append(ch);
        inWord = wordChar;
    }
    flushCurrent();

    QVector<int> wordIndices;
    for (int i = 0; i < tokens.size(); ++i) {
        if (tokens[i].isWord) {
            wordIndices.push_back(i);
        }
    }
    if (wordIndices.isEmpty()) {
        return text;
    }

    for (int wordPosition = 0; wordPosition < wordIndices.size(); ++wordPosition) {
        const int tokenIndex = wordIndices[wordPosition];
        const bool firstWord = wordPosition == 0;
        const bool lastWord = wordPosition == wordIndices.size() - 1;
        const bool afterBreak = followsStrongBreak(tokens, tokenIndex);
        const QString lowered = tokens[tokenIndex].text.toLower();
        if (tokens[tokenIndex].text.contains(QLatin1Char('-'))) {
            tokens[tokenIndex].text = nytHyphenTitleCase(
                tokens[tokenIndex].text,
                firstWord,
                lastWord,
                afterBreak);
        } else if (!firstWord && !lastWord && !afterBreak && isNytSmallWord(lowered)) {
            tokens[tokenIndex].text = lowered;
        } else {
            tokens[tokenIndex].text = nytTitleCaseWord(tokens[tokenIndex].text);
        }
    }

    QString out;
    for (const TitleCaseToken &token : tokens) {
        out.append(token.text);
    }
    return out;
}

QString finnishCapitalizeText(const QString &text) {
    const QString lowered = text.toLower();
    if (lowered.isEmpty()) {
        return lowered;
    }
    QString out = lowered;
    out[0] = out[0].toUpper();
    return out;
}

QString genreCapitalizeText(const QString &text) {
    return finnishCapitalizeText(text);
}

QString paddedNumber(int value, int width) {
    return QString::number(value).rightJustified(width, QLatin1Char('0'));
}

struct GroupInfo {
    int startIndex{0};
    int rowCount{0};
};

QVector<GroupInfo> buildGroups(
    const QVector<int> &rows,
    const TagEditorTableModel &model,
    bool resetOnFolder,
    bool resetOnDiscChange)
{
    QVector<GroupInfo> groups;
    if (rows.isEmpty()) {
        return groups;
    }

    GroupInfo current;
    current.startIndex = rows[0];
    current.rowCount = 1;
    QString previousDirectory = model.fieldValue(rows[0], QStringLiteral("directory"));
    QString previousDisc = model.fieldValue(rows[0], QStringLiteral("discNo"));

    for (int i = 1; i < rows.size(); ++i) {
        const int rowIndex = rows[i];
        const QString directory = model.fieldValue(rowIndex, QStringLiteral("directory"));
        const QString disc = model.fieldValue(rowIndex, QStringLiteral("discNo"));
        const bool folderChanged = resetOnFolder && directory != previousDirectory;
        const bool discChanged = resetOnDiscChange
            && !disc.isEmpty()
            && !previousDisc.isEmpty()
            && disc != previousDisc;
        if (folderChanged || discChanged) {
            groups.push_back(current);
            current = GroupInfo{rowIndex, 1};
        } else {
            current.rowCount += 1;
        }
        previousDirectory = directory;
        previousDisc = disc;
    }
    groups.push_back(current);
    return groups;
}

} // namespace

TagEditorController::TagEditorController(BridgeClient *bridge, QObject *parent)
    : QObject(parent)
    , m_bridge(bridge)
    , m_tableModel(this) {
    connect(&m_tableModel, &TagEditorTableModel::dirtyStateChanged, this, &TagEditorController::dirtyChanged);
    connect(&m_tableModel, &TagEditorTableModel::rowsChanged, this, &TagEditorController::bulkSummaryChanged);
    m_workerPool = new QThreadPool();
}

TagEditorController::~TagEditorController() {
    // Pending results are dropped with the watchers; only the workers remain.
    ++m_operationGeneration;
    if (m_workerPool->waitForDone(kExitWaitMs)) {
        delete m_workerPool;
        return;
    }
    // Deleting the pool would wait for the stalled operation, so it is left
    // to end with the process. Tag saves and renames replace each file in one
    // rename, so the file keeps its old tags or gets its new ones.
    std::fprintf(stderr,
                 "[ferrous] a tag editor file operation was still running at exit; "
                 "its changes may not have been saved\n");
}

QByteArray TagEditorController::executeOperation(Operation operation, const QByteArray &payload,
                                                 const RenameGate &renameGate) {
    const auto *data = reinterpret_cast<const std::uint8_t *>(payload.constData());
    const auto size = static_cast<std::size_t>(payload.size());
    std::size_t len = 0;
    std::uint8_t *raw = nullptr;
    switch (operation) {
    case Operation::Load: raw = ferrous_ffi_tag_editor_load(data, size, &len); break;
    case Operation::Save: raw = ferrous_ffi_tag_editor_save(data, size, &len); break;
    case Operation::Rename:
        // Through the gate, the bridge learns of the renames in order with imports.
        raw = renameGate ? ferrous_ffi_rename_gate_rename(renameGate.get(), data, size, &len)
                         : ferrous_ffi_tag_editor_rename(data, size, &len);
        break;
    case Operation::PreviewRename:
        raw = ferrous_ffi_tag_editor_preview_rename(data, size, &len);
        break;
    }
    QByteArray response;
    if (raw != nullptr) {
        response = QByteArray(reinterpret_cast<const char *>(raw), static_cast<qsizetype>(len));
        ferrous_ffi_tag_editor_free_buffer(raw, len);
    }
    return response;
}

void TagEditorController::runOperation(Operation operation, const QByteArray &payload,
                                       std::function<void(const QByteArray &)> apply) {
    const quint64 generation = ++m_operationGeneration;
    RenameGate renameGate;
    if (operation == Operation::Rename && m_bridge != nullptr) renameGate = m_bridge->renameGate();
    auto *watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this,
            [this, watcher, generation, apply = std::move(apply)]() {
        watcher->deleteLater();
        if (generation == m_operationGeneration) apply(watcher->result());
    });
    // The worker owns its input, function, and gate reference; it never accesses a
    // controller or bridge.
    watcher->setFuture(
        QtConcurrent::run(m_workerPool, m_executeOperation, operation, payload, renameGate));
}

QObject *TagEditorController::tableModel() const {
    return const_cast<TagEditorTableModel *>(&m_tableModel);
}

bool TagEditorController::isOpen() const {
    return m_open;
}

bool TagEditorController::loading() const {
    return m_loading;
}

bool TagEditorController::saving() const {
    return m_saving;
}

bool TagEditorController::dirty() const {
    return m_tableModel.hasDirtyRows();
}

QString TagEditorController::statusText() const {
    return m_statusText;
}

QString TagEditorController::statusDetails() const {
    return m_statusDetails;
}

bool TagEditorController::statusIsError() const {
    return m_statusIsError;
}

int TagEditorController::selectedCount() const {
    return static_cast<int>(m_selectedRows.size());
}

QStringList TagEditorController::operationFiles() const {
    return m_operationFiles;
}

bool TagEditorController::openForPaths(const QStringList &paths) {
    if (m_loading || m_saving) {
        return false;
    }

    QStringList sanitized;
    sanitized.reserve(paths.size());
    for (const QString &path : paths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty() && !sanitized.contains(trimmed)) {
            sanitized.push_back(trimmed);
        }
    }
    if (sanitized.isEmpty()) {
        return false;
    }

    const QByteArray payload = buildPathBlob(sanitized);
    return openSelection(QJsonDocument::fromJson(payload).array().toVariantList());
}

bool TagEditorController::openSelection(const QVariantList &selections) {
    if (m_loading || m_saving || selections.isEmpty()) {
        return false;
    }

    m_loading = true;
    emit loadingChanged();

    m_open = true;
    m_tableModel.clear();
    m_loadedPaths.clear();
    setStatusText(i18nc("@info:status", "Loading tags..."));
    emit openChanged();
    runLoad(buildSelectionBlob(selections));
    return true;
}

void TagEditorController::runLoad(const QByteArray &payload) {
    const quint64 generation = ++m_operationGeneration;
    auto *watcher = new QFutureWatcher<LoadResult>(this);
    connect(watcher, &QFutureWatcher<LoadResult>::finished, this, [this, watcher, generation]() {
        watcher->deleteLater();
        if (generation == m_operationGeneration) applyLoadResult(watcher->result());
    });
    // Like runOperation, the worker owns its input and function.
    watcher->setFuture(QtConcurrent::run(m_workerPool, [execute = m_executeOperation, payload]() {
        return parseLoadResponse(execute(Operation::Load, payload, RenameGate()));
    }));
}

TagEditorController::LoadResult TagEditorController::parseLoadResponse(const QByteArray &response) {
    LoadResult result;
    QVector<TagEditorRowState> &rows = result.rows;
    QStringList &resolvedPaths = result.resolvedPaths;
    QString &errorText = result.errorText;
    QStringList &failureLines = result.failureLines;
    const QJsonDocument document = QJsonDocument::fromJson(response);
    if (document.isObject()) {
        const QJsonObject root = document.object();
        errorText = root.value(QStringLiteral("error")).toString();
        const QJsonArray resolved = root.value(QStringLiteral("resolvedPaths")).toArray();
        if (!resolved.isEmpty()) {
            resolvedPaths.clear();
            for (const QJsonValue &value : resolved) {
                resolvedPaths.push_back(value.toString());
            }
        }
        const QJsonArray rowArray = root.value(QStringLiteral("rows")).toArray();
        rows.reserve(rowArray.size());
        for (const QJsonValue &value : rowArray) {
            if (value.isObject()) {
                rows.push_back(rowFromJson(value.toObject()));
            }
        }
        const QJsonArray failures = root.value(QStringLiteral("failures")).toArray();
        for (const QJsonValue &value : failures) {
            const QJsonObject failure = value.toObject();
            failureLines.push_back(QStringLiteral("%1: %2").arg(
                displayNameForPath(readString(failure, "path")),
                readString(failure, "error")));
        }
    } else {
        errorText = i18nc("@info:status", "failed to parse tag editor load response");
    }
    if (errorText.isEmpty() && !failureLines.isEmpty()) {
        errorText = partialResultText(
            i18ncp("@info:status", "Loaded %1 file", "Loaded %1 files", rows.size()),
            failureLines.size());
    }
    failureLines.sort();
    return result;
}

void TagEditorController::applyLoadResult(LoadResult result) {
    // A preview of the rows replaced here is out of date.
    m_renamePreview = {};
    const QString &errorText = result.errorText;
    m_tableModel.setRows(std::move(result.rows));
    m_loadedPaths = std::move(result.resolvedPaths);
    resetSelection();
    const bool wasOpen = m_open;
    m_open = !m_loadedPaths.isEmpty();
    if (wasOpen != m_open) {
        emit openChanged();
    }
    setStatusText(errorText, buildActionDetails({}, result.failureLines), !errorText.isEmpty());
    if (!errorText.isEmpty()) {
        reportFailures(errorText, result.failureLines);
    }

    m_loading = false;
    emit loadingChanged();
    emit bulkSummaryChanged();
}

void TagEditorController::close() {
    if (m_saving) return;
    ++m_operationGeneration;
    m_renamePreview = {};
    if (m_loading) { m_loading = false; emit loadingChanged(); }
    const bool wasOpen = m_open;
    m_open = false;
    m_loadedPaths.clear();
    m_tableModel.clear();
    resetSelection();
    setStatusText(QString());
    if (wasOpen) {
        emit openChanged();
    }
}

void TagEditorController::reload() {
    if (m_loadedPaths.isEmpty()) {
        return;
    }
    openForPaths(m_loadedPaths);
}

bool TagEditorController::save() {
    if (m_saving || m_loading || m_loadedPaths.isEmpty() || !m_tableModel.hasDirtyRows()) {
        return false;
    }

    m_operationFiles.clear();
    for (const TagEditorRowState &row : m_tableModel.rows()) {
        if (row.dirty) {
            m_operationFiles.push_back(row.fileName);
        }
    }
    m_saving = true;
    emit savingChanged();

    runOperation(Operation::Save, buildSaveBlob(m_tableModel.rows()), [this](const QByteArray &response) {
        applySaveResult(response);
    });
    return true;
}

void TagEditorController::applySaveResult(const QByteArray &response) {
    QHash<QString, QString> errorsByPath;
    QSet<QString> successPaths;
    QString status;
    QStringList successLines;
    QStringList failureLines;

    const QJsonDocument document = QJsonDocument::fromJson(response);
    if (document.isObject()) {
        const QJsonObject root = document.object();
        status = root.value(QStringLiteral("error")).toString();
        const QJsonArray successArray = root.value(QStringLiteral("successfulPaths")).toArray();
        for (const QJsonValue &value : successArray) {
            successPaths.insert(value.toString());
        }
        const QJsonArray results = root.value(QStringLiteral("results")).toArray();
        for (const QJsonValue &value : results) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject item = value.toObject();
            const QString path = item.value(QStringLiteral("path")).toString();
            if (path.isEmpty()) {
                continue;
            }
            if (item.value(QStringLiteral("ok")).toBool()) {
                successPaths.insert(path);
                successLines.push_back(displayNameForPath(path));
            } else {
                const QString error = item.value(QStringLiteral("error")).toString();
                errorsByPath.insert(path, error);
                const QString targetPath = item.value(QStringLiteral("newPath")).toString();
                const QString oldName = displayNameForPath(path);
                const QString targetName = targetPath.isEmpty() ? QString() : displayNameForPath(targetPath);
                failureLines.push_back(
                    targetName.isEmpty()
                        ? QStringLiteral("%1: %2").arg(oldName, error)
                        : QStringLiteral("%1 -> %2: %3").arg(oldName, targetName, error));
            }
        }
    } else {
        status = i18nc("@info:status", "failed to parse tag editor save response");
    }

    const bool success = document.isObject() && status.isEmpty()
        && !successPaths.isEmpty() && errorsByPath.isEmpty();
    m_tableModel.applySaveResults(errorsByPath, successPaths);
    if (!successPaths.isEmpty() && m_bridge != nullptr) {
        QStringList paths = successPaths.values();
        paths.sort();
        m_bridge->refreshEditedPaths(paths);
    }
    if (status.isEmpty()) {
        if (errorsByPath.isEmpty()) {
            status = i18ncp("@info:status", "Saved %1 file.", "Saved %1 files.", successPaths.size());
        } else {
            status = partialResultText(
                i18ncp("@info:status", "Saved %1 file", "Saved %1 files", successPaths.size()),
                errorsByPath.size());
        }
    }
    successLines.sort();
    failureLines.sort();
    setStatusText(status, buildActionDetails(successLines, failureLines), !success);
    if (!success) {
        reportFailures(status, failureLines);
    }

    m_operationFiles.clear();
    m_saving = false;
    emit savingChanged();
    emit bulkSummaryChanged();
    emit saveFinished(success);
}

QByteArray TagEditorController::buildRenameRequest(QStringList *fileNames) const {
    QVector<int> rows;
    rows.reserve(m_selectedRows.size());
    for (int row : m_selectedRows) {
        if (row >= 0 && row < m_tableModel.rowCount()) {
            rows.push_back(row);
        }
    }
    std::sort(rows.begin(), rows.end());

    QJsonArray rowArray;
    for (int rowIndex : rows) {
        const TagEditorRowState *row = m_tableModel.rowAt(rowIndex);
        if (row == nullptr) {
            continue;
        }
        rowArray.push_back(rowToJson(*row));
        if (fileNames != nullptr) {
            fileNames->push_back(row->fileName);
        }
    }
    if (rowArray.isEmpty()) {
        return {};
    }
    QJsonObject root;
    root.insert(QStringLiteral("rows"), rowArray);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool TagEditorController::renameSelectedFiles() {
    if (m_loading || m_saving || m_bridge == nullptr || m_selectedRows.isEmpty()) {
        return false;
    }
    RenameRequest request;
    request.payload = buildRenameRequest(&request.fileNames);
    return startRename(std::move(request));
}

bool TagEditorController::renamePreviewedFiles() {
    if (m_loading || m_saving || m_bridge == nullptr) {
        return false;
    }
    RenameRequest request = std::exchange(m_renamePreview, {});
    if (request.payload.isEmpty()) {
        setStatusText(i18nc("@info:status",
                            "The rename preview is out of date, so nothing was renamed. "
                            "Use Rename Files again."),
                      QString(), true);
        return false;
    }
    return startRename(std::move(request));
}

bool TagEditorController::startRename(RenameRequest request) {
    if (request.payload.isEmpty()) {
        return false;
    }
    m_renamePreview = {};
    m_operationFiles = std::move(request.fileNames);
    m_saving = true;
    emit savingChanged();
    runOperation(Operation::Rename, request.payload, [this](const QByteArray &response) {
        applyRenameResult(response);
    });
    return true;
}

bool TagEditorController::previewRenameSelectedFiles() {
    if (m_loading || m_saving || m_selectedRows.isEmpty()) {
        return false;
    }
    RenameRequest request;
    request.payload = buildRenameRequest(&request.fileNames);
    if (request.payload.isEmpty()) {
        return false;
    }
    // Nothing is written. Another operation started meanwhile supersedes
    // the preview, whose answer is then dropped. The rows are captured now,
    // so the rename the user confirms is the one previewed, whatever the
    // selection or the tags are by then.
    m_renamePreview = {};
    m_pendingRenamePreview = request;
    runOperation(Operation::PreviewRename, request.payload, [this](const QByteArray &response) {
        applyRenamePreview(response, std::exchange(m_pendingRenamePreview, {}));
    });
    return true;
}

void TagEditorController::applyRenamePreview(const QByteArray &response,
                                             const RenameRequest &previewed) {
    m_renamePreview = {};
    const QJsonDocument document = QJsonDocument::fromJson(response);
    if (!document.isObject()) {
        emit renamePreviewReady({}, i18nc("@info", "failed to parse the file rename preview"));
        return;
    }
    const QJsonObject object = document.object();
    const QString error = object.value(QStringLiteral("error")).toString();
    QVariantList rows;
    QJsonObject planned;
    const QJsonArray results = object.value(QStringLiteral("results")).toArray();
    for (const QJsonValue &value : results) {
        const QJsonObject item = value.toObject();
        const QString path = item.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            continue;
        }
        const QString newPath = item.value(QStringLiteral("newPath")).toString();
        QVariantMap row;
        row.insert(QStringLiteral("path"), path);
        row.insert(QStringLiteral("newPath"), newPath);
        row.insert(QStringLiteral("oldName"), displayNameForPath(path));
        row.insert(QStringLiteral("newName"),
                   newPath.isEmpty() ? QString() : displayNameForPath(newPath));
        const bool ok = item.value(QStringLiteral("ok")).toBool();
        row.insert(QStringLiteral("ok"), ok);
        row.insert(QStringLiteral("unchanged"), ok && newPath == path);
        row.insert(QStringLiteral("error"), item.value(QStringLiteral("error")).toString());
        rows.push_back(row);
        if (ok && !newPath.isEmpty() && newPath != path) {
            planned.insert(path, newPath);
        }
    }
    // Confirming renames only what the preview showed going ahead, from
    // and to those paths; the backend refuses if the files changed since.
    if (error.isEmpty() && !planned.isEmpty()) {
        const QJsonArray previewedRows = QJsonDocument::fromJson(previewed.payload)
                                             .object()
                                             .value(QStringLiteral("rows"))
                                             .toArray();
        QJsonArray plannedRows;
        QStringList fileNames;
        for (qsizetype index = 0; index < previewedRows.size(); ++index) {
            const QJsonObject row = previewedRows.at(index).toObject();
            if (planned.contains(row.value(QStringLiteral("path")).toString())) {
                plannedRows.push_back(row);
                fileNames.push_back(previewed.fileNames.value(index));
            }
        }
        QJsonObject request;
        request.insert(QStringLiteral("rows"), plannedRows);
        request.insert(QStringLiteral("plannedRenames"), planned);
        m_renamePreview.payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
        m_renamePreview.fileNames = fileNames;
    }
    emit renamePreviewReady(rows, error);
}

void TagEditorController::applyRenameResult(const QByteArray &response) {
    const QJsonDocument document = QJsonDocument::fromJson(response);

    QHash<QString, QString> renamedPaths;
    QHash<QString, QString> errorsByPath;
    QString status;
    QStringList renamedLines;
    QStringList failureLines;
    if (document.isObject()) {
        const QJsonObject object = document.object();
        status = object.value(QStringLiteral("error")).toString();
        const QJsonArray results = object.value(QStringLiteral("results")).toArray();
        for (const QJsonValue &value : results) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject item = value.toObject();
            const QString path = item.value(QStringLiteral("path")).toString();
            const QString newPath = item.value(QStringLiteral("newPath")).toString();
            if (path.isEmpty()) {
                continue;
            }
            if (item.value(QStringLiteral("ok")).toBool()) {
                const QString resolvedNewPath = newPath.isEmpty() ? path : newPath;
                if (resolvedNewPath == path) {
                    continue;
                }
                renamedPaths.insert(path, resolvedNewPath);
                const QString oldName = displayNameForPath(path);
                const QString newName = displayNameForPath(resolvedNewPath);
                renamedLines.push_back(
                    oldName == newName
                        ? oldName
                        : QStringLiteral("%1 -> %2").arg(oldName, newName));
            } else {
                const QString error = item.value(QStringLiteral("error")).toString();
                errorsByPath.insert(path, error);
                failureLines.push_back(
                    QStringLiteral("%1: %2").arg(displayNameForPath(path), error));
            }
        }
    } else {
        status = i18nc("@info:status", "failed to parse file rename response");
    }

    const bool failed = !status.isEmpty() || !errorsByPath.isEmpty();
    m_tableModel.applyRenameResults(renamedPaths, errorsByPath);
    m_loadedPaths = m_tableModel.loadedPaths();

    if (status.isEmpty()) {
        if (errorsByPath.isEmpty()) {
            status = i18ncp("@info:status", "Renamed %1 file.", "Renamed %1 files.", renamedPaths.size());
        } else {
            status = partialResultText(
                i18ncp("@info:status", "Renamed %1 file", "Renamed %1 files", renamedPaths.size()),
                errorsByPath.size());
        }
    }
    renamedLines.sort();
    failureLines.sort();
    setStatusText(status, buildActionDetails(renamedLines, failureLines), failed);
    if (failed) {
        reportFailures(status, failureLines);
    }
    emit bulkSummaryChanged();
    m_operationFiles.clear();
    m_saving = false;
    emit savingChanged();
    emit renameFinished();
}

void TagEditorController::setSelectedRows(const QVariantList &rows) {
    QSet<int> next;
    for (const QVariant &value : rows) {
        bool ok = false;
        const int row = value.toInt(&ok);
        if (ok && row >= 0 && row < m_tableModel.rowCount()) {
            next.insert(row);
        }
    }
    setSelection(std::move(next));
}

void TagEditorController::setSelection(QSet<int> rows) {
    if (rows == m_selectedRows) {
        return;
    }
    m_selectedRows = std::move(rows);
    m_tableModel.setSelection(m_selectedRows);
    emit selectionChanged();
    emit bulkSummaryChanged();
}

void TagEditorController::selectRange(int first, int last) {
    const int rowCount = m_tableModel.rowCount();
    if (rowCount == 0) {
        return;
    }
    const int start = qBound(0, qMin(first, last), rowCount - 1);
    const int end = qBound(0, qMax(first, last), rowCount - 1);
    QSet<int> next;
    next.reserve(end - start + 1);
    for (int row = start; row <= end; ++row) {
        next.insert(row);
    }
    setSelection(std::move(next));
}

void TagEditorController::toggleRowSelected(int row) {
    if (row < 0 || row >= m_tableModel.rowCount()) {
        return;
    }
    QSet<int> next = m_selectedRows;
    if (!next.remove(row)) {
        next.insert(row);
    }
    setSelection(std::move(next));
}

void TagEditorController::selectAllRows() {
    selectRange(0, m_tableModel.rowCount() - 1);
}

QVariantList TagEditorController::selectedRows() const {
    QVariantList out;
    for (int row : m_selectedRows) {
        out.push_back(row);
    }
    return out;
}

QString TagEditorController::bulkValue(const QString &field) const {
    const QVector<int> rows = targetRows();
    if (rows.isEmpty() || bulkValueMixed(field)) {
        return {};
    }
    return m_tableModel.fieldValue(rows[0], field);
}

bool TagEditorController::bulkValueMixed(const QString &field) const {
    const QVector<int> rows = targetRows();
    if (rows.isEmpty()) {
        return false;
    }
    const QString first = m_tableModel.fieldValue(rows[0], field);
    for (int i = 1; i < rows.size(); ++i) {
        if (m_tableModel.fieldValue(rows[i], field) != first) {
            return true;
        }
    }
    return false;
}

QString TagEditorController::extraValuesNote(const QString &field) const {
    const QVector<int> rows = targetRows();
    int rowsWithExtras = 0;
    QStringList firstExtras;
    for (int rowIndex : rows) {
        const TagEditorRowState *row = m_tableModel.rowAt(rowIndex);
        const QStringList extras = row == nullptr
            ? QStringList()
            : TagEditorTableModel::keptExtraValues(*row, field);
        if (extras.isEmpty()) {
            continue;
        }
        if (rowsWithExtras == 0) {
            firstExtras = extras;
        }
        ++rowsWithExtras;
    }
    if (rowsWithExtras == 0) {
        return {};
    }
    if (rows.size() == 1) {
        return i18nc("@info the file's other values of a tag",
                     "Also: %1. Editing replaces every value with the one above.",
                     firstExtras.join(QStringLiteral("; ")));
    }
    return i18ncp("@info",
                  "%1 of these files has more than one value. Editing replaces "
                  "them all with the one above.",
                  "%1 of these files have more than one value. Editing replaces "
                  "them all with the one above.",
                  rowsWithExtras);
}

void TagEditorController::applyBulkField(const QString &field, const QString &value) {
    if (m_loading || m_saving) return;
    for (int row : targetRows()) {
        m_tableModel.setFieldValue(row, field, value);
    }
    notifyRowMutation();
}

void TagEditorController::applyBulkFieldToRows(
    const QVariantList &rows,
    const QString &field,
    const QString &value)
{
    if (m_loading || m_saving) return;
    QSet<int> targetSet;
    for (const QVariant &entry : rows) {
        bool ok = false;
        const int row = entry.toInt(&ok);
        if (ok && row >= 0 && row < m_tableModel.rowCount()) {
            targetSet.insert(row);
        }
    }

    QVector<int> targets;
    if (targetSet.isEmpty()) {
        return;
    }
    targets.reserve(targetSet.size());
    for (int row : targetSet) {
        targets.push_back(row);
    }
    std::sort(targets.begin(), targets.end());

    for (int row : targets) {
        m_tableModel.setFieldValue(row, field, value);
    }
    notifyRowMutation();
}

void TagEditorController::setCell(int row, const QString &field, const QString &value) {
    if (m_loading || m_saving) return;
    if (m_tableModel.setFieldValue(row, field, value)) {
        notifyRowMutation();
    }
}

void TagEditorController::applyEnglishTitleCase(const QString &field) {
    if (m_loading || m_saving) return;
    for (int row : targetRows()) {
        m_tableModel.setFieldValue(
            row,
            field,
            englishTitleCaseText(m_tableModel.fieldValue(row, field)));
    }
    notifyRowMutation();
}

void TagEditorController::applyFinnishCapitalize(const QString &field) {
    if (m_loading || m_saving) return;
    for (int row : targetRows()) {
        m_tableModel.setFieldValue(
            row,
            field,
            finnishCapitalizeText(m_tableModel.fieldValue(row, field)));
    }
    notifyRowMutation();
}

void TagEditorController::applyGenreCapitalize() {
    if (m_loading || m_saving) return;
    for (int row : targetRows()) {
        m_tableModel.setFieldValue(
            row,
            QStringLiteral("genre"),
            genreCapitalizeText(m_tableModel.fieldValue(row, QStringLiteral("genre"))));
    }
    notifyRowMutation();
}

void TagEditorController::autoNumber(
    int startingTrack,
    int startingDisc,
    bool writeDiscNumbers,
    bool writeTotals,
    bool resetOnFolder,
    bool resetOnDiscChange)
{
    if (m_loading || m_saving) return;
    const QVector<int> rows = targetRows();
    if (rows.isEmpty()) {
        return;
    }

    const int firstTrack = qMax(1, startingTrack);
    const int firstDisc = qMax(1, startingDisc);
    const QVector<GroupInfo> groups = buildGroups(rows, m_tableModel, resetOnFolder, resetOnDiscChange);
    const int maxTrackValue = [&groups, firstTrack]() {
        int maxValue = firstTrack;
        for (const GroupInfo &group : groups) {
            maxValue = qMax(maxValue, firstTrack + group.rowCount - 1);
        }
        return maxValue;
    }();
    const int maxDiscValue = qMax(firstDisc, firstDisc + groups.size() - 1);
    const int trackWidth = QString::number(maxTrackValue).size();
    const int discWidth = QString::number(maxDiscValue).size();
    const int totalDiscCount = groups.size();

    int currentDisc = firstDisc;
    int rowCursor = 0;
    for (const GroupInfo &group : groups) {
        int currentTrack = firstTrack;
        for (int offset = 0; offset < group.rowCount; ++offset) {
            const int rowIndex = rows[rowCursor++];
            m_tableModel.setFieldValue(rowIndex, QStringLiteral("trackNo"), paddedNumber(currentTrack, trackWidth));
            if (writeDiscNumbers) {
                m_tableModel.setFieldValue(
                    rowIndex,
                    QStringLiteral("discNo"),
                    paddedNumber(currentDisc, discWidth));
            }
            if (writeTotals) {
                m_tableModel.setFieldValue(
                    rowIndex,
                    QStringLiteral("totalTracks"),
                    paddedNumber(group.rowCount, trackWidth));
                if (writeDiscNumbers) {
                    m_tableModel.setFieldValue(
                        rowIndex,
                        QStringLiteral("totalDiscs"),
                        paddedNumber(totalDiscCount, discWidth));
                }
            }
            ++currentTrack;
        }
        ++currentDisc;
    }

    QStringList successLines;
    successLines.reserve(rows.size());
    for (int rowIndex : rows) {
        const TagEditorRowState *row = m_tableModel.rowAt(rowIndex);
        if (row == nullptr) {
            continue;
        }
        successLines.push_back(
            writeDiscNumbers && !row->discNo.isEmpty()
                ? i18nc("@item file name, track number, disc number", "%1: track %2, disc %3",
                        displayNameForPath(row->path), row->trackNo, row->discNo)
                : i18nc("@item file name, track number", "%1: track %2",
                        displayNameForPath(row->path), row->trackNo));
    }
    successLines.sort();
    setStatusText(
        i18ncp("@info:status", "Auto-numbered %1 file.", "Auto-numbered %1 files.", successLines.size()),
        buildActionDetails(successLines, {}));
    notifyRowMutation();
}

QStringList TagEditorController::loadedPaths() const {
    return m_loadedPaths;
}

QVector<int> TagEditorController::targetRows() const {
    QVector<int> rows;
    rows.reserve(m_selectedRows.size());
    if (!m_selectedRows.isEmpty()) {
        for (int row : m_selectedRows) {
            if (row >= 0 && row < m_tableModel.rowCount()) {
                rows.push_back(row);
            }
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    rows.reserve(m_tableModel.rowCount());
    for (int row = 0; row < m_tableModel.rowCount(); ++row) {
        rows.push_back(row);
    }
    return rows;
}

void TagEditorController::reportFailures(const QString &status, const QStringList &failureLines) {
    // The tag editor lists every failure; the error list shows the first.
    constexpr qsizetype kShownFailures = 10;
    QStringList lines{status};
    lines.append(failureLines.mid(0, kShownFailures));
    if (failureLines.size() > kShownFailures) {
        lines.push_back(i18ncp("@info", "…and %1 more; the tag editor lists them all.",
                               "…and %1 more; the tag editor lists them all.",
                               failureLines.size() - kShownFailures));
    }
    emit errorReported(lines.join(QLatin1Char('\n')));
}

void TagEditorController::setStatusText(const QString &text, const QString &details, bool isError) {
    if (m_statusText == text && m_statusDetails == details && m_statusIsError == isError) {
        return;
    }
    m_statusText = text;
    m_statusDetails = details;
    m_statusIsError = isError;
    emit statusChanged();
}

void TagEditorController::resetSelection() {
    if (m_selectedRows.isEmpty()) {
        return;
    }
    m_selectedRows.clear();
    m_tableModel.setSelection(m_selectedRows);
    emit selectionChanged();
}

void TagEditorController::notifyRowMutation() {
    emit bulkSummaryChanged();
}
