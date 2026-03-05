#include "GlobalSearchResultsModel.h"

#include <algorithm>
#include <limits>
#include <vector>

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
    const SearchDisplayRow &row = m_rows[static_cast<qsizetype>(index.row())];
    if (role == Qt::DisplayRole) {
        return row.label;
    }
    switch (role) {
    case KindRole:
        return row.kind;
    case RowTypeRole:
        return row.rowType;
    case SectionTitleRole:
        return row.sectionTitle;
    case ScoreRole:
        if (row.kind != QLatin1String(kItemKind)) {
            return {};
        }
        return row.score;
    case LabelRole:
        return row.label;
    case ArtistRole:
        return row.artist;
    case AlbumRole:
        return row.album;
    case GenreRole:
        return row.genre;
    case CoverPathRole:
        return row.coverPath;
    case CoverUrlRole:
        return row.coverUrl;
    case ArtistKeyRole:
        return row.artistKey;
    case AlbumKeyRole:
        return row.albumKey;
    case SectionKeyRole:
        return row.sectionKey;
    case TrackKeyRole:
        return row.trackKey;
    case TrackPathRole:
        return row.trackPath;
    case YearRole:
        if (row.year == std::numeric_limits<int>::min()) {
            return {};
        }
        return row.year;
    case TrackNumberRole:
        if (row.trackNumber <= 0) {
            return {};
        }
        return row.trackNumber;
    case CountRole:
        if (row.kind != QLatin1String(kItemKind)) {
            return {};
        }
        return row.count;
    case LengthSecondsRole:
        if (row.kind != QLatin1String(kItemKind)) {
            return {};
        }
        return row.lengthSeconds;
    case LengthTextRole:
        return row.lengthText;
    default:
        return {};
    }
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

void GlobalSearchResultsModel::replaceRows(QVector<SearchDisplayRow> rows) {
    if (m_rows.isEmpty() && rows.isEmpty()) {
        return;
    }
    if (rows.isEmpty()) {
        beginRemoveRows(
            QModelIndex{},
            0,
            static_cast<int>(m_rows.size()) - 1);
        m_rows.clear();
        endRemoveRows();
        return;
    }
    if (m_rows.isEmpty()) {
        beginInsertRows(
            QModelIndex{},
            0,
            static_cast<int>(rows.size()) - 1);
        m_rows = std::move(rows);
        endInsertRows();
        return;
    }
    if (m_rows.size() == rows.size()) {
        bool anyViewDiff = false;
        for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
            const qsizetype idx = static_cast<qsizetype>(i);
            if (!m_rows[idx].equivalentForView(rows[idx])) {
                anyViewDiff = true;
                break;
            }
        }
        if (!anyViewDiff) {
            for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
                const qsizetype idx = static_cast<qsizetype>(i);
                m_rows[idx].score = rows[idx].score;
            }
            return;
        }
    }

    if (m_rows == rows) {
        return;
    }

    const int oldSize = static_cast<int>(m_rows.size());
    const int newSize = static_cast<int>(rows.size());
    const int diff = std::abs(oldSize - newSize);
    const int overlap = std::min(oldSize, newSize);

    // For large non-empty cardinality shifts, overlap updates can trigger costly
    // delegate churn in QML. A direct rebuild has proven smoother in practice.
    const bool largeCardinalityShift =
        (oldSize > 0 && newSize > 0)
        && (diff >= 64)
        && (std::min(oldSize, newSize) <= 64);
    if (largeCardinalityShift) {
        beginRemoveRows(QModelIndex{}, 0, oldSize - 1);
        m_rows.clear();
        endRemoveRows();

        beginInsertRows(QModelIndex{}, 0, newSize - 1);
        m_rows = std::move(rows);
        endInsertRows();
        return;
    }

    if (newSize < oldSize) {
        beginRemoveRows(QModelIndex{}, newSize, oldSize - 1);
        m_rows.resize(newSize);
        endRemoveRows();
    }

    std::vector<QPair<int, int>> changedRuns;
    int runStart = -1;
    for (int i = 0; i < overlap; ++i) {
        const qsizetype idx = static_cast<qsizetype>(i);
        if (m_rows[idx].equivalentForView(rows[idx])) {
            m_rows[idx].score = rows[idx].score;
            if (runStart >= 0) {
                changedRuns.emplace_back(runStart, i - 1);
                runStart = -1;
            }
            continue;
        }
        m_rows[idx] = rows[idx];
        if (runStart < 0) {
            runStart = i;
        }
    }
    if (runStart >= 0) {
        changedRuns.emplace_back(runStart, overlap - 1);
    }
    for (const auto &run : changedRuns) {
        emit dataChanged(index(run.first, 0), index(run.second, 0));
    }

    if (newSize > oldSize) {
        beginInsertRows(QModelIndex{}, oldSize, newSize - 1);
        m_rows.reserve(newSize);
        for (int i = oldSize; i < newSize; ++i) {
            m_rows.push_back(rows[static_cast<qsizetype>(i)]);
        }
        endInsertRows();
    }
}

QVariantMap GlobalSearchResultsModel::rowDataAt(int index) const {
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const SearchDisplayRow &row = m_rows[static_cast<qsizetype>(index)];
    QVariantMap out;
    if (!row.kind.isEmpty()) {
        out.insert(QStringLiteral("kind"), row.kind);
    }
    if (!row.rowType.isEmpty()) {
        out.insert(QStringLiteral("rowType"), row.rowType);
    }
    if (!row.sectionTitle.isEmpty()) {
        out.insert(QStringLiteral("sectionTitle"), row.sectionTitle);
    }
    if (row.kind != QLatin1String(kItemKind)) {
        return out;
    }

    out.insert(QStringLiteral("score"), row.score);
    out.insert(QStringLiteral("label"), row.label);
    out.insert(QStringLiteral("artist"), row.artist);
    out.insert(QStringLiteral("album"), row.album);
    out.insert(QStringLiteral("genre"), row.genre);
    out.insert(QStringLiteral("count"), row.count);
    out.insert(QStringLiteral("coverPath"), row.coverPath);
    out.insert(QStringLiteral("coverUrl"), row.coverUrl);
    out.insert(QStringLiteral("artistKey"), row.artistKey);
    out.insert(QStringLiteral("albumKey"), row.albumKey);
    out.insert(QStringLiteral("sectionKey"), row.sectionKey);
    out.insert(QStringLiteral("trackKey"), row.trackKey);
    out.insert(QStringLiteral("trackPath"), row.trackPath);
    if (row.year != std::numeric_limits<int>::min()) {
        out.insert(QStringLiteral("year"), row.year);
    }
    if (row.trackNumber > 0) {
        out.insert(QStringLiteral("trackNumber"), row.trackNumber);
    }
    out.insert(QStringLiteral("lengthSeconds"), row.lengthSeconds);
    out.insert(QStringLiteral("lengthText"), row.lengthText);
    return out;
}

bool GlobalSearchResultsModel::isSelectableIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return false;
    }
    const SearchDisplayRow &row = m_rows[static_cast<qsizetype>(index)];
    return row.kind == QLatin1String(kItemKind);
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
