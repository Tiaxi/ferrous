#include "GlobalSearchResultsModel.h"

#include <algorithm>

namespace {
constexpr auto kItemKind = "item";
}

GlobalSearchResultsModel::GlobalSearchResultsModel(QObject *parent)
    : QAbstractListModel(parent) {}

int GlobalSearchResultsModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant GlobalSearchResultsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const QVariantMap &row = m_rows[static_cast<qsizetype>(index.row())];
    if (role == Qt::DisplayRole) {
        return row.value(QStringLiteral("label"));
    }
    const QString key = roleKeyForRole(role);
    if (key.isEmpty()) {
        return {};
    }
    return row.value(key);
}

QHash<int, QByteArray> GlobalSearchResultsModel::roleNames() const {
    return {
        {KindRole, "kind"},
        {RowTypeRole, "rowType"},
        {SectionTitleRole, "sectionTitle"},
        {ScoreRole, "score"},
        {LabelRole, "label"},
        {ArtistRole, "artist"},
        {AlbumRole, "album"},
        {GenreRole, "genre"},
        {CoverPathRole, "coverPath"},
        {CoverUrlRole, "coverUrl"},
        {ArtistKeyRole, "artistKey"},
        {AlbumKeyRole, "albumKey"},
        {SectionKeyRole, "sectionKey"},
        {TrackKeyRole, "trackKey"},
        {TrackPathRole, "trackPath"},
        {YearRole, "year"},
        {TrackNumberRole, "trackNumber"},
        {CountRole, "count"},
        {LengthSecondsRole, "lengthSeconds"},
        {LengthTextRole, "lengthText"},
    };
}

void GlobalSearchResultsModel::replaceRows(QVector<QVariantMap> rows) {
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

QVariantMap GlobalSearchResultsModel::rowDataAt(int index) const {
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return {};
    }
    return m_rows[static_cast<qsizetype>(index)];
}

bool GlobalSearchResultsModel::isSelectableIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return false;
    }
    const QVariantMap &row = m_rows[static_cast<qsizetype>(index)];
    return row.value(QStringLiteral("kind")).toString() == QString::fromUtf8(kItemKind);
}

int GlobalSearchResultsModel::nextSelectableIndex(int startIndex, int step, bool wrap) const {
    if (m_rows.isEmpty()) {
        return -1;
    }
    const int direction = step < 0 ? -1 : 1;
    int index = startIndex;
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        index += direction;
        if (index < 0 || index >= static_cast<int>(m_rows.size())) {
            if (!wrap) {
                return -1;
            }
            index = direction > 0 ? 0 : static_cast<int>(m_rows.size()) - 1;
        }
        if (isSelectableIndex(index)) {
            return index;
        }
    }
    return -1;
}

QString GlobalSearchResultsModel::roleKeyForRole(int role) {
    switch (role) {
    case KindRole:
        return QStringLiteral("kind");
    case RowTypeRole:
        return QStringLiteral("rowType");
    case SectionTitleRole:
        return QStringLiteral("sectionTitle");
    case ScoreRole:
        return QStringLiteral("score");
    case LabelRole:
        return QStringLiteral("label");
    case ArtistRole:
        return QStringLiteral("artist");
    case AlbumRole:
        return QStringLiteral("album");
    case GenreRole:
        return QStringLiteral("genre");
    case CoverPathRole:
        return QStringLiteral("coverPath");
    case CoverUrlRole:
        return QStringLiteral("coverUrl");
    case ArtistKeyRole:
        return QStringLiteral("artistKey");
    case AlbumKeyRole:
        return QStringLiteral("albumKey");
    case SectionKeyRole:
        return QStringLiteral("sectionKey");
    case TrackKeyRole:
        return QStringLiteral("trackKey");
    case TrackPathRole:
        return QStringLiteral("trackPath");
    case YearRole:
        return QStringLiteral("year");
    case TrackNumberRole:
        return QStringLiteral("trackNumber");
    case CountRole:
        return QStringLiteral("count");
    case LengthSecondsRole:
        return QStringLiteral("lengthSeconds");
    case LengthTextRole:
        return QStringLiteral("lengthText");
    default:
        return {};
    }
}
