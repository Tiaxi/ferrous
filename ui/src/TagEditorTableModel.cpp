// SPDX-License-Identifier: GPL-3.0-or-later

#include "TagEditorTableModel.h"

#include <QFileInfo>
#include <QDir>
#include <QVariantMap>

#include <algorithm>
#include <iterator>
#include <utility>

namespace {

struct EditableField {
    // Matches the field names of the Rust save request.
    const char *name;
    QString TagEditorRowState::*value;
    QString TagEditorRowState::*original;
};

constexpr EditableField kEditableFields[] = {
    {"title", &TagEditorRowState::title, &TagEditorRowState::originalTitle},
    {"artist", &TagEditorRowState::artist, &TagEditorRowState::originalArtist},
    {"album", &TagEditorRowState::album, &TagEditorRowState::originalAlbum},
    {"albumArtist", &TagEditorRowState::albumArtist, &TagEditorRowState::originalAlbumArtist},
    {"genre", &TagEditorRowState::genre, &TagEditorRowState::originalGenre},
    {"year", &TagEditorRowState::year, &TagEditorRowState::originalYear},
    {"trackNo", &TagEditorRowState::trackNo, &TagEditorRowState::originalTrackNo},
    {"discNo", &TagEditorRowState::discNo, &TagEditorRowState::originalDiscNo},
    {"totalTracks", &TagEditorRowState::totalTracks, &TagEditorRowState::originalTotalTracks},
    {"totalDiscs", &TagEditorRowState::totalDiscs, &TagEditorRowState::originalTotalDiscs},
    {"comment", &TagEditorRowState::comment, &TagEditorRowState::originalComment},
};

bool fieldChanged(const TagEditorRowState &row, const EditableField &field) {
    return row.*field.value != row.*field.original;
}

const EditableField *editableField(const QString &name) {
    for (const EditableField &field : kEditableFields) {
        if (name == QLatin1String(field.name)) {
            return &field;
        }
    }
    return nullptr;
}

} // namespace

TagEditorTableModel::TagEditorTableModel(QObject *parent)
    : QAbstractListModel(parent) {}

int TagEditorTableModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant TagEditorTableModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return {};
    }
    return fieldData(m_rows[static_cast<size_t>(row)], role);
}

QHash<int, QByteArray> TagEditorTableModel::roleNames() const {
    return {
        {PathRole, "path"},
        {FileNameRole, "fileName"},
        {DirectoryRole, "directory"},
        {FormatKindRole, "formatKind"},
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {AlbumRole, "album"},
        {AlbumArtistRole, "albumArtist"},
        {GenreRole, "genre"},
        {YearRole, "year"},
        {TrackNoRole, "trackNo"},
        {DiscNoRole, "discNo"},
        {TotalTracksRole, "totalTracks"},
        {TotalDiscsRole, "totalDiscs"},
        {CommentRole, "comment"},
        {DirtyRole, "dirty"},
        {ErrorRole, "errorText"},
        {SelectedRole, "selected"},
        {ExtraValuesRole, "extraValues"},
    };
}

void TagEditorTableModel::setRows(QVector<TagEditorRowState> rows) {
    beginResetModel();
    m_rows = std::move(rows);
    countDirtyRows();
    endResetModel();
    emit rowsChanged();
    emit dirtyStateChanged();
}

void TagEditorTableModel::clear() {
    if (m_rows.isEmpty()) {
        return;
    }
    beginResetModel();
    m_rows.clear();
    m_dirtyRowCount = 0;
    endResetModel();
    emit rowsChanged();
    emit dirtyStateChanged();
}

const TagEditorRowState *TagEditorTableModel::rowAt(int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return nullptr;
    }
    return &m_rows[static_cast<size_t>(row)];
}

TagEditorRowState *TagEditorTableModel::rowAt(int row) {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return nullptr;
    }
    return &m_rows[static_cast<size_t>(row)];
}

bool TagEditorTableModel::setFieldValue(int row, const QString &field, const QString &value) {
    TagEditorRowState *item = rowAt(row);
    if (!item) {
        return false;
    }
    QString *target = fieldRef(*item, field);
    if (!target || *target == value) {
        return false;
    }

    const bool wasDirty = hasDirtyRows();
    *target = value;
    item->errorText.clear();
    refreshDirtyState(*item);
    // The caller announces a batch of edits once it is done, so an edit of
    // thousands of rows does not recompute summaries after every row.
    emitRowChanged(row);
    if (wasDirty != hasDirtyRows()) {
        emit dirtyStateChanged();
    }
    return true;
}

QString TagEditorTableModel::fieldValue(int row, const QString &field) const {
    const TagEditorRowState *item = rowAt(row);
    if (!item) {
        return {};
    }
    if (normalizedFieldName(field) == QStringLiteral("directory")) {
        return item->directory;
    }
    const QString *target = fieldRef(*item, field);
    return target ? *target : QString();
}

QStringList TagEditorTableModel::loadedPaths() const {
    QStringList out;
    out.reserve(m_rows.size());
    for (const TagEditorRowState &row : m_rows) {
        out.push_back(row.path);
    }
    return out;
}

bool TagEditorTableModel::hasDirtyRows() const {
    return m_dirtyRowCount > 0;
}

void TagEditorTableModel::countDirtyRows() {
    m_dirtyRowCount = static_cast<int>(std::count_if(
        m_rows.cbegin(), m_rows.cend(), [](const TagEditorRowState &row) { return row.dirty; }));
}

void TagEditorTableModel::setSelection(const QSet<int> &rows) {
    // Notify runs of changed rows, so selecting thousands of rows is a few
    // signals rather than one per row.
    int runStart = -1;
    const auto flushRun = [this, &runStart](int end) {
        if (runStart >= 0) {
            emit dataChanged(createIndex(runStart, 0), createIndex(end - 1, 0), {SelectedRole});
            runStart = -1;
        }
    };
    for (int row = 0; row < static_cast<int>(m_rows.size()); ++row) {
        TagEditorRowState &item = m_rows[static_cast<size_t>(row)];
        const bool selected = rows.contains(row);
        if (item.selected == selected) {
            flushRun(row);
            continue;
        }
        item.selected = selected;
        if (runStart < 0) {
            runStart = row;
        }
    }
    flushRun(static_cast<int>(m_rows.size()));
}

QStringList TagEditorTableModel::keptExtraValues(const TagEditorRowState &row, const QString &field) {
    const EditableField *editable = editableField(field);
    if (editable == nullptr || fieldChanged(row, *editable)) {
        return {};
    }
    return row.extraValues.value(field);
}

QVector<TagEditorRowState> TagEditorTableModel::rows() const {
    return m_rows;
}

QStringList TagEditorTableModel::changedFields(const TagEditorRowState &row) {
    QStringList out;
    for (const EditableField &field : kEditableFields) {
        if (fieldChanged(row, field)) {
            out.push_back(QLatin1String(field.name));
        }
    }
    return out;
}

void TagEditorTableModel::clearErrors() {
    for (int row = 0; row < static_cast<int>(m_rows.size()); ++row) {
        TagEditorRowState &item = m_rows[static_cast<size_t>(row)];
        if (item.errorText.isEmpty()) {
            continue;
        }
        item.errorText.clear();
        emitRowChanged(row);
    }
    emit rowsChanged();
}

void TagEditorTableModel::applySaveResults(
    const QHash<QString, QString> &errorsByPath,
    const QSet<QString> &successPaths)
{
    const bool wasDirty = hasDirtyRows();
    for (int row = 0; row < static_cast<int>(m_rows.size()); ++row) {
        TagEditorRowState &item = m_rows[static_cast<size_t>(row)];
        const bool success = successPaths.contains(item.path);
        const QString error = errorsByPath.value(item.path);
        if (success) {
            for (const EditableField &field : kEditableFields) {
                if (fieldChanged(item, field)) {
                    // The save wrote this field's one value over all of them.
                    item.extraValues.remove(QLatin1String(field.name));
                }
                item.*field.original = item.*field.value;
            }
            item.errorText.clear();
        } else if (!error.isEmpty()) {
            item.errorText = error;
        }
        const bool dirtyChanged = refreshDirtyState(item);
        if (success || !error.isEmpty() || dirtyChanged) {
            emitRowChanged(row);
        }
    }
    emit rowsChanged();
    if (wasDirty != hasDirtyRows()) {
        emit dirtyStateChanged();
    }
}

void TagEditorTableModel::applyRenameResults(
    const QHash<QString, QString> &newPathsByPath,
    const QHash<QString, QString> &errorsByPath)
{
    for (int row = 0; row < static_cast<int>(m_rows.size()); ++row) {
        TagEditorRowState &item = m_rows[static_cast<size_t>(row)];
        const auto renamed = newPathsByPath.constFind(item.path);
        if (renamed != newPathsByPath.constEnd()) {
            item.path = renamed.value();
            const QFileInfo info(item.path);
            item.fileName = info.fileName();
            item.directory = info.dir().absolutePath();
            item.errorText.clear();
        } else if (errorsByPath.contains(item.path)) {
            item.errorText = errorsByPath.value(item.path);
        } else {
            continue;
        }
        emitRowChanged(row);
    }
    emit rowsChanged();
}

QVariant TagEditorTableModel::fieldData(const TagEditorRowState &row, int role) const {
    switch (role) {
    case PathRole:
        return row.path;
    case FileNameRole:
        return row.fileName;
    case DirectoryRole:
        return row.directory;
    case FormatKindRole:
        return row.formatKind;
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case AlbumRole:
        return row.album;
    case AlbumArtistRole:
        return row.albumArtist;
    case GenreRole:
        return row.genre;
    case YearRole:
        return row.year;
    case TrackNoRole:
        return row.trackNo;
    case DiscNoRole:
        return row.discNo;
    case TotalTracksRole:
        return row.totalTracks;
    case TotalDiscsRole:
        return row.totalDiscs;
    case CommentRole:
        return row.comment;
    case DirtyRole:
        return row.dirty;
    case ErrorRole:
        return row.errorText;
    case SelectedRole:
        return row.selected;
    case ExtraValuesRole: {
        QVariantMap shown;
        for (auto it = row.extraValues.cbegin(); it != row.extraValues.cend(); ++it) {
            const QStringList kept = keptExtraValues(row, it.key());
            if (!kept.isEmpty()) {
                shown.insert(it.key(), kept);
            }
        }
        return shown;
    }
    default:
        return {};
    }
}

QString TagEditorTableModel::normalizedFieldName(const QString &field) {
    QString out = field.trimmed().toLower();
    out.remove(QLatin1Char('-'));
    out.remove(QLatin1Char('_'));
    out.remove(QLatin1Char(' '));
    return out;
}

QString *TagEditorTableModel::fieldRef(TagEditorRowState &row, const QString &field) {
    const QString name = normalizedFieldName(field);
    if (name == QStringLiteral("title")) {
        return &row.title;
    }
    if (name == QStringLiteral("artist")) {
        return &row.artist;
    }
    if (name == QStringLiteral("album")) {
        return &row.album;
    }
    if (name == QStringLiteral("albumartist")) {
        return &row.albumArtist;
    }
    if (name == QStringLiteral("genre")) {
        return &row.genre;
    }
    if (name == QStringLiteral("year")) {
        return &row.year;
    }
    if (name == QStringLiteral("trackno")) {
        return &row.trackNo;
    }
    if (name == QStringLiteral("discno")) {
        return &row.discNo;
    }
    if (name == QStringLiteral("totaltracks")) {
        return &row.totalTracks;
    }
    if (name == QStringLiteral("totaldiscs")) {
        return &row.totalDiscs;
    }
    if (name == QStringLiteral("comment")) {
        return &row.comment;
    }
    return nullptr;
}

const QString *TagEditorTableModel::fieldRef(const TagEditorRowState &row, const QString &field) {
    return fieldRef(const_cast<TagEditorRowState &>(row), field);
}

bool TagEditorTableModel::refreshDirtyState(TagEditorRowState &row) {
    const bool nextDirty = std::any_of(
        std::begin(kEditableFields),
        std::end(kEditableFields),
        [&row](const EditableField &field) { return fieldChanged(row, field); });
    const bool changed = row.dirty != nextDirty;
    if (changed) {
        m_dirtyRowCount += nextDirty ? 1 : -1;
    }
    row.dirty = nextDirty;
    return changed;
}

void TagEditorTableModel::emitRowChanged(int row) {
    const QModelIndex index = createIndex(row, 0);
    emit dataChanged(index, index);
}
