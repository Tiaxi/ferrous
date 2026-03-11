#include "TagEditorController.h"

#include "BridgeClient.h"
#include "FerrousBridgeFfi.h"

#include <algorithm>
#include <QDir>
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

QString buildActionDetails(const QStringList &successLines, const QStringList &failureLines) {
    QStringList sections;
    if (!successLines.isEmpty()) {
        QStringList lines;
        lines.reserve(successLines.size() + 1);
        lines.push_back(QStringLiteral("Succeeded:"));
        for (const QString &line : successLines) {
            lines.push_back(QStringLiteral("  ") + line);
        }
        sections.push_back(lines.join(QLatin1Char('\n')));
    }
    if (!failureLines.isEmpty()) {
        QStringList lines;
        lines.reserve(failureLines.size() + 1);
        lines.push_back(QStringLiteral("Failed:"));
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
        rowArray.push_back(rowToJson(row));
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

    const QByteArray payload = buildSelectionBlob(selections);
    std::size_t len = 0;
    std::uint8_t *raw = ferrous_ffi_tag_editor_load(
        reinterpret_cast<const std::uint8_t *>(payload.constData()),
        static_cast<std::size_t>(payload.size()),
        &len);
    QByteArray response;
    if (raw != nullptr && len > 0) {
        response = QByteArray(reinterpret_cast<const char *>(raw), static_cast<int>(len));
        ferrous_ffi_tag_editor_free_buffer(raw, len);
    }

    QVector<TagEditorRowState> rows;
    QStringList resolvedPaths;
    QString errorText;
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
    } else {
        errorText = QStringLiteral("failed to parse tag editor load response");
    }

    m_tableModel.setRows(std::move(rows));
    m_loadedPaths = resolvedPaths;
    resetSelection();
    const bool wasOpen = m_open;
    m_open = !m_loadedPaths.isEmpty();
    if (wasOpen != m_open) {
        emit openChanged();
    }
    setStatusText(errorText);

    m_loading = false;
    emit loadingChanged();
    emit bulkSummaryChanged();
    return m_open;
}

void TagEditorController::close() {
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

    m_saving = true;
    emit savingChanged();

    const QByteArray payload = buildSaveBlob(m_tableModel.rows());
    std::size_t len = 0;
    std::uint8_t *raw = ferrous_ffi_tag_editor_save(
        reinterpret_cast<const std::uint8_t *>(payload.constData()),
        static_cast<std::size_t>(payload.size()),
        &len);
    QByteArray response;
    if (raw != nullptr && len > 0) {
        response = QByteArray(reinterpret_cast<const char *>(raw), static_cast<int>(len));
        ferrous_ffi_tag_editor_free_buffer(raw, len);
    }

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
        status = QStringLiteral("failed to parse tag editor save response");
    }

    m_tableModel.applySaveResults(errorsByPath, successPaths);
    if (!successPaths.isEmpty() && m_bridge != nullptr) {
        QStringList paths = successPaths.values();
        paths.sort();
        m_bridge->refreshEditedPaths(paths);
    }
    if (status.isEmpty()) {
        if (errorsByPath.isEmpty()) {
            status = QStringLiteral("Saved %1 file(s).").arg(successPaths.size());
        } else {
            status = QStringLiteral("Saved %1 file(s); %2 failed.")
                         .arg(successPaths.size())
                         .arg(errorsByPath.size());
        }
    }
    successLines.sort();
    failureLines.sort();
    setStatusText(status, buildActionDetails(successLines, failureLines));

    m_saving = false;
    emit savingChanged();
    emit bulkSummaryChanged();
    return errorsByPath.isEmpty();
}

bool TagEditorController::renameSelectedFiles() {
    if (m_loading || m_saving || m_bridge == nullptr || m_selectedRows.isEmpty()) {
        return false;
    }

    QVector<int> rows;
    rows.reserve(m_selectedRows.size());
    for (int row : m_selectedRows) {
        if (row >= 0 && row < m_tableModel.rowCount()) {
            rows.push_back(row);
        }
    }
    std::sort(rows.begin(), rows.end());
    if (rows.isEmpty()) {
        return false;
    }

    QJsonArray rowArray;
    for (int rowIndex : rows) {
        const TagEditorRowState *row = m_tableModel.rowAt(rowIndex);
        if (row == nullptr) {
            continue;
        }
        rowArray.push_back(rowToJson(*row));
    }
    if (rowArray.isEmpty()) {
        return false;
    }

    QJsonObject root;
    root.insert(QStringLiteral("rows"), rowArray);
    const QByteArray response = m_bridge->renameEditedFiles(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
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
        status = QStringLiteral("failed to parse file rename response");
    }

    QVector<TagEditorRowState> updatedRows = m_tableModel.rows();
    for (TagEditorRowState &row : updatedRows) {
        const auto renamedIt = renamedPaths.constFind(row.path);
        if (renamedIt != renamedPaths.constEnd()) {
            row.path = renamedIt.value();
            const QFileInfo info(row.path);
            row.fileName = info.fileName();
            row.directory = info.dir().absolutePath();
            row.errorText.clear();
        } else if (errorsByPath.contains(row.path)) {
            row.errorText = errorsByPath.value(row.path);
        }
    }
    m_tableModel.setRows(updatedRows);
    m_loadedPaths = m_tableModel.loadedPaths();

    if (status.isEmpty()) {
        if (errorsByPath.isEmpty()) {
            status = QStringLiteral("Renamed %1 file(s).").arg(renamedPaths.size());
        } else {
            status = QStringLiteral("Renamed %1 file(s); %2 failed.")
                         .arg(renamedPaths.size())
                         .arg(errorsByPath.size());
        }
    }
    renamedLines.sort();
    failureLines.sort();
    setStatusText(status, buildActionDetails(renamedLines, failureLines));
    emit bulkSummaryChanged();
    return errorsByPath.isEmpty();
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
    if (next == m_selectedRows) {
        return;
    }
    m_selectedRows = std::move(next);
    emit selectionChanged();
    emit bulkSummaryChanged();
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
    if (rows.isEmpty()) {
        return {};
    }
    const QString first = m_tableModel.fieldValue(rows[0], field);
    for (int i = 1; i < rows.size(); ++i) {
        if (m_tableModel.fieldValue(rows[i], field) != first) {
            return QStringLiteral("<keep>");
        }
    }
    return first;
}

void TagEditorController::applyBulkField(const QString &field, const QString &value) {
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
    if (m_tableModel.setFieldValue(row, field, value)) {
        notifyRowMutation();
    }
}

void TagEditorController::applyEnglishTitleCase(const QString &field) {
    for (int row : targetRows()) {
        m_tableModel.setFieldValue(
            row,
            field,
            englishTitleCaseText(m_tableModel.fieldValue(row, field)));
    }
    notifyRowMutation();
}

void TagEditorController::applyFinnishCapitalize(const QString &field) {
    for (int row : targetRows()) {
        m_tableModel.setFieldValue(
            row,
            field,
            finnishCapitalizeText(m_tableModel.fieldValue(row, field)));
    }
    notifyRowMutation();
}

void TagEditorController::applyGenreCapitalize() {
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
        QString line = displayNameForPath(row->path) + QStringLiteral(": track ") + row->trackNo;
        if (writeDiscNumbers && !row->discNo.isEmpty()) {
            line += QStringLiteral(", disc ") + row->discNo;
        }
        successLines.push_back(line);
    }
    successLines.sort();
    setStatusText(
        QStringLiteral("Auto-numbered %1 file(s).").arg(successLines.size()),
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

void TagEditorController::setStatusText(const QString &text, const QString &details) {
    if (m_statusText == text && m_statusDetails == details) {
        return;
    }
    m_statusText = text;
    m_statusDetails = details;
    emit statusChanged();
}

void TagEditorController::resetSelection() {
    if (m_selectedRows.isEmpty()) {
        return;
    }
    m_selectedRows.clear();
    emit selectionChanged();
}

void TagEditorController::notifyRowMutation() {
    emit bulkSummaryChanged();
}
