#include "TagEditorTableModel.h"

#include <utility>

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
    };
}

void TagEditorTableModel::setRows(QVector<TagEditorRowState> rows) {
    beginResetModel();
    m_rows = std::move(rows);
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
    emitRowChanged(row);
    emit rowsChanged();
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
    for (const TagEditorRowState &row : m_rows) {
        if (row.dirty) {
            return true;
        }
    }
    return false;
}

QVector<TagEditorRowState> TagEditorTableModel::rows() const {
    return m_rows;
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
            item.originalTitle = item.title;
            item.originalArtist = item.artist;
            item.originalAlbum = item.album;
            item.originalAlbumArtist = item.albumArtist;
            item.originalGenre = item.genre;
            item.originalYear = item.year;
            item.originalTrackNo = item.trackNo;
            item.originalDiscNo = item.discNo;
            item.originalTotalTracks = item.totalTracks;
            item.originalTotalDiscs = item.totalDiscs;
            item.originalComment = item.comment;
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
    const bool nextDirty = row.title != row.originalTitle
        || row.artist != row.originalArtist
        || row.album != row.originalAlbum
        || row.albumArtist != row.originalAlbumArtist
        || row.genre != row.originalGenre
        || row.year != row.originalYear
        || row.trackNo != row.originalTrackNo
        || row.discNo != row.originalDiscNo
        || row.totalTracks != row.originalTotalTracks
        || row.totalDiscs != row.originalTotalDiscs
        || row.comment != row.originalComment;
    const bool changed = row.dirty != nextDirty;
    row.dirty = nextDirty;
    return changed;
}

void TagEditorTableModel::emitRowChanged(int row) {
    const QModelIndex index = createIndex(row, 0);
    emit dataChanged(index, index);
}
