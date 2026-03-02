#include "LibraryTreeModel.h"

#include <algorithm>

#include <QVariantMap>

LibraryTreeModel::LibraryTreeModel(QObject *parent)
    : QAbstractListModel(parent) {}

int LibraryTreeModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant LibraryTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const int row = index.row();
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const FlatRow &item = m_rows[static_cast<size_t>(row)];
    switch (role) {
    case RowTypeRole:
        switch (item.type) {
        case RowType::Artist:
            return QStringLiteral("artist");
        case RowType::Album:
            return QStringLiteral("album");
        case RowType::Track:
            return QStringLiteral("track");
        }
        return {};
    case ArtistRole:
        return item.artist;
    case NameRole:
        return item.name;
    case TitleRole:
        return item.title;
    case CountRole:
        return item.count;
    case ExpandedRole:
        return item.expanded;
    case SourceIndexRole:
        return item.sourceIndex;
    case KeyRole:
        return item.key;
    case TrackNumberRole:
        return item.trackNumber;
    case TrackPathRole:
        return item.trackPath;
    case CoverPathRole:
        return item.coverPath;
    case SelectionKeyRole:
        return item.selectionKey;
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryTreeModel::roleNames() const {
    return {
        {RowTypeRole, "rowType"},
        {ArtistRole, "artist"},
        {NameRole, "name"},
        {TitleRole, "title"},
        {CountRole, "count"},
        {ExpandedRole, "expanded"},
        {SourceIndexRole, "sourceIndex"},
        {KeyRole, "key"},
        {TrackNumberRole, "trackNumber"},
        {TrackPathRole, "trackPath"},
        {CoverPathRole, "coverPath"},
        {SelectionKeyRole, "selectionKey"},
    };
}

int LibraryTreeModel::count() const {
    return static_cast<int>(m_rows.size());
}

void LibraryTreeModel::setLibraryTree(const QVariantList &tree) {
    QVector<ArtistNode> parsed;
    parsed.reserve(tree.size());
    for (const QVariant &artistVar : tree) {
        const QVariantMap artistMap = artistVar.toMap();
        ArtistNode artistNode;
        artistNode.artist = artistMap.value(QStringLiteral("artist")).toString();
        const QVariantList albums = artistMap.value(QStringLiteral("albums")).toList();
        artistNode.albums.reserve(albums.size());
        for (const QVariant &albumVar : albums) {
            const QVariantMap albumMap = albumVar.toMap();
            AlbumNode albumNode;
            albumNode.artist = artistNode.artist;
            albumNode.name = albumMap.value(QStringLiteral("name")).toString();
            albumNode.count = albumMap.value(QStringLiteral("count")).toInt();
            const QVariant sourceIndexVar = albumMap.value(QStringLiteral("sourceIndex"));
            albumNode.sourceIndex = sourceIndexVar.isValid() ? sourceIndexVar.toInt() : -1;
            albumNode.coverPath = albumMap.value(QStringLiteral("coverPath")).toString();
            albumNode.key =
                QStringLiteral("%1|%2|%3").arg(albumNode.artist).arg(albumNode.sourceIndex).arg(albumNode.name);

            const QVariantList tracks = albumMap.value(QStringLiteral("tracks")).toList();
            albumNode.tracks.reserve(tracks.size());
            for (const QVariant &trackVar : tracks) {
                const QVariantMap trackMap = trackVar.toMap();
                TrackNode trackNode;
                trackNode.title = trackMap.value(QStringLiteral("title")).toString();
                trackNode.path = trackMap.value(QStringLiteral("path")).toString();
                if (!trackNode.title.isEmpty() || !trackNode.path.isEmpty()) {
                    albumNode.tracks.push_back(std::move(trackNode));
                }
            }
            artistNode.albums.push_back(std::move(albumNode));
        }
        parsed.push_back(std::move(artistNode));
    }
    m_tree = std::move(parsed);
    rebuildRows();
}

void LibraryTreeModel::setSearchText(const QString &text) {
    const QString next = toLower(text.trimmed());
    if (m_searchLower == next) {
        return;
    }
    m_searchLower = next;
    rebuildRows();
}

void LibraryTreeModel::toggleArtist(const QString &artist) {
    if (!m_searchLower.isEmpty()) {
        return;
    }

    int artistRowIndex = -1;
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        const FlatRow &row = m_rows[static_cast<size_t>(i)];
        if (row.type == RowType::Artist && row.artist == artist) {
            artistRowIndex = i;
            break;
        }
    }
    if (artistRowIndex < 0) {
        return;
    }

    FlatRow &artistRow = m_rows[static_cast<size_t>(artistRowIndex)];
    const bool nextExpanded = !artistRow.expanded;
    m_expandedArtists.insert(artistRow.artist, nextExpanded);
    artistRow.expanded = nextExpanded;
    emit dataChanged(index(artistRowIndex), index(artistRowIndex), {ExpandedRole});

    if (nextExpanded) {
        const ArtistNode *artistNode = findArtistNode(artistRow.artist);
        if (!artistNode) {
            return;
        }

        QVector<FlatRow> rowsToInsert;
        for (const AlbumNode &album : artistNode->albums) {
            FlatRow albumRow = makeAlbumRow(artistNode->artist, album);
            const bool albumExpanded = albumRow.expanded;
            rowsToInsert.push_back(std::move(albumRow));
            if (albumExpanded) {
                int trackNumber = 1;
                for (const TrackNode &track : album.tracks) {
                    rowsToInsert.push_back(makeTrackRow(album, trackNumber++, track));
                }
            }
        }
        if (rowsToInsert.isEmpty()) {
            return;
        }

        const int insertAt = artistRowIndex + 1;
        const int last = insertAt + rowsToInsert.size() - 1;
        beginInsertRows(QModelIndex(), insertAt, last);
        for (int i = 0; i < rowsToInsert.size(); ++i) {
            m_rows.insert(insertAt + i, std::move(rowsToInsert[i]));
        }
        endInsertRows();
        emit countChanged();
        return;
    }

    const int removeFirst = artistRowIndex + 1;
    int removeLast = removeFirst - 1;
    for (int i = removeFirst; i < static_cast<int>(m_rows.size()); ++i) {
        if (m_rows[static_cast<size_t>(i)].type == RowType::Artist) {
            break;
        }
        removeLast = i;
    }
    if (removeLast < removeFirst) {
        return;
    }
    beginRemoveRows(QModelIndex(), removeFirst, removeLast);
    for (int i = removeLast; i >= removeFirst; --i) {
        m_rows.removeAt(i);
    }
    endRemoveRows();
    emit countChanged();
}

void LibraryTreeModel::toggleAlbum(const QString &albumKey) {
    if (!m_searchLower.isEmpty()) {
        return;
    }

    int albumRowIndex = -1;
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        const FlatRow &row = m_rows[static_cast<size_t>(i)];
        if (row.type == RowType::Album && row.key == albumKey) {
            albumRowIndex = i;
            break;
        }
    }
    if (albumRowIndex < 0) {
        return;
    }

    FlatRow &albumRow = m_rows[static_cast<size_t>(albumRowIndex)];
    const bool nextExpanded = !albumRow.expanded;
    m_expandedAlbums.insert(albumRow.key, nextExpanded);
    albumRow.expanded = nextExpanded;
    emit dataChanged(index(albumRowIndex), index(albumRowIndex), {ExpandedRole});

    if (nextExpanded) {
        const AlbumNode *albumNode = findAlbumNode(albumKey);
        if (!albumNode || albumNode->tracks.isEmpty()) {
            return;
        }

        QVector<FlatRow> rowsToInsert;
        rowsToInsert.reserve(albumNode->tracks.size());
        int trackNumber = 1;
        for (const TrackNode &track : albumNode->tracks) {
            rowsToInsert.push_back(makeTrackRow(*albumNode, trackNumber++, track));
        }

        const int insertAt = albumRowIndex + 1;
        const int last = insertAt + rowsToInsert.size() - 1;
        beginInsertRows(QModelIndex(), insertAt, last);
        for (int i = 0; i < rowsToInsert.size(); ++i) {
            m_rows.insert(insertAt + i, std::move(rowsToInsert[i]));
        }
        endInsertRows();
        emit countChanged();
        return;
    }

    const int removeFirst = albumRowIndex + 1;
    int removeLast = removeFirst - 1;
    for (int i = removeFirst; i < static_cast<int>(m_rows.size()); ++i) {
        if (m_rows[static_cast<size_t>(i)].type != RowType::Track) {
            break;
        }
        removeLast = i;
    }
    if (removeLast < removeFirst) {
        return;
    }
    beginRemoveRows(QModelIndex(), removeFirst, removeLast);
    for (int i = removeLast; i >= removeFirst; --i) {
        m_rows.removeAt(i);
    }
    endRemoveRows();
    emit countChanged();
}

bool LibraryTreeModel::hasSelectionKey(const QString &selectionKey) const {
    if (selectionKey.isEmpty()) {
        return true;
    }
    for (const FlatRow &row : m_rows) {
        if (row.selectionKey == selectionKey) {
            return true;
        }
    }
    return false;
}

int LibraryTreeModel::indexForSelectionKey(const QString &selectionKey) const {
    if (selectionKey.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        if (m_rows[static_cast<size_t>(i)].selectionKey == selectionKey) {
            return i;
        }
    }
    return -1;
}

int LibraryTreeModel::sourceIndexForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return -1;
    }
    return m_rows[static_cast<size_t>(row)].sourceIndex;
}

QString LibraryTreeModel::selectionKeyForRow(int row) const {
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return {};
    }
    return m_rows[static_cast<size_t>(row)].selectionKey;
}

QVariantMap LibraryTreeModel::rowDataForRow(int row) const {
    QVariantMap out;
    if (row < 0 || row >= static_cast<int>(m_rows.size())) {
        return out;
    }
    const FlatRow &item = m_rows[static_cast<size_t>(row)];
    out.insert(QStringLiteral("selectionKey"), item.selectionKey);
    out.insert(QStringLiteral("sourceIndex"), item.sourceIndex);
    out.insert(QStringLiteral("artist"), item.artist);
    out.insert(QStringLiteral("name"), item.name);
    out.insert(QStringLiteral("trackPath"), item.trackPath);
    switch (item.type) {
    case RowType::Artist:
        out.insert(QStringLiteral("rowType"), QStringLiteral("artist"));
        break;
    case RowType::Album:
        out.insert(QStringLiteral("rowType"), QStringLiteral("album"));
        break;
    case RowType::Track:
        out.insert(QStringLiteral("rowType"), QStringLiteral("track"));
        break;
    }
    return out;
}

bool LibraryTreeModel::isArtistExpanded(const QString &artist, bool autoExpand) const {
    if (autoExpand) {
        return true;
    }
    const auto it = m_expandedArtists.constFind(artist);
    if (it == m_expandedArtists.constEnd()) {
        return true;
    }
    return it.value();
}

bool LibraryTreeModel::isAlbumExpanded(const QString &albumKey, bool autoExpand) const {
    if (autoExpand) {
        return true;
    }
    const auto it = m_expandedAlbums.constFind(albumKey);
    if (it == m_expandedAlbums.constEnd()) {
        return false;
    }
    return it.value();
}

const LibraryTreeModel::ArtistNode *LibraryTreeModel::findArtistNode(const QString &artist) const {
    for (const ArtistNode &artistNode : m_tree) {
        if (artistNode.artist == artist) {
            return &artistNode;
        }
    }
    return nullptr;
}

const LibraryTreeModel::AlbumNode *LibraryTreeModel::findAlbumNode(const QString &albumKey) const {
    for (const ArtistNode &artistNode : m_tree) {
        for (const AlbumNode &albumNode : artistNode.albums) {
            if (albumNode.key == albumKey) {
                return &albumNode;
            }
        }
    }
    return nullptr;
}

LibraryTreeModel::FlatRow LibraryTreeModel::makeAlbumRow(const QString &artistName, const AlbumNode &album) const {
    FlatRow row;
    row.type = RowType::Album;
    row.artist = artistName;
    row.name = album.name;
    row.count = album.count;
    row.expanded = isAlbumExpanded(album.key, false);
    row.sourceIndex = album.sourceIndex;
    row.key = album.key;
    row.coverPath = album.coverPath;
    row.selectionKey = selectionKeyForAlbum(album.sourceIndex);
    return row;
}

LibraryTreeModel::FlatRow LibraryTreeModel::makeTrackRow(
    const AlbumNode &album,
    int trackNumber,
    const TrackNode &track) const {
    FlatRow row;
    row.type = RowType::Track;
    row.sourceIndex = album.sourceIndex;
    row.trackNumber = trackNumber;
    row.title = track.title;
    row.trackPath = track.path;
    row.selectionKey = selectionKeyForTrack(album.sourceIndex, row.trackNumber, row.trackPath);
    return row;
}

void LibraryTreeModel::rebuildRows() {
    const int oldCount = count();
    beginResetModel();
    m_rows.clear();

    const bool hasSearch = !m_searchLower.isEmpty();
    const bool autoExpand = hasSearch;

    for (const ArtistNode &artistNode : m_tree) {
        const QString artistName = artistNode.artist;
        const QString artistLower = toLower(artistName);
        const bool artistMatch = !hasSearch || artistLower.contains(m_searchLower);

        struct FilteredAlbum {
            const AlbumNode *album{nullptr};
            QVector<const TrackNode *> tracks;
        };
        QVector<FilteredAlbum> filteredAlbums;
        filteredAlbums.reserve(artistNode.albums.size());

        for (const AlbumNode &albumNode : artistNode.albums) {
            const QString albumLower = toLower(albumNode.name);
            const bool albumMatch = !hasSearch || albumLower.contains(m_searchLower);
            FilteredAlbum filtered;
            filtered.album = &albumNode;
            filtered.tracks.reserve(albumNode.tracks.size());

            for (const TrackNode &trackNode : albumNode.tracks) {
                const QString titleLower = toLower(trackNode.title);
                const QString pathLower = toLower(trackNode.path);
                const bool trackMatch = titleLower.contains(m_searchLower) || pathLower.contains(m_searchLower);
                if (!hasSearch || artistMatch || albumMatch || trackMatch) {
                    filtered.tracks.push_back(&trackNode);
                }
            }

            if (!hasSearch || artistMatch || albumMatch || !filtered.tracks.isEmpty()) {
                filteredAlbums.push_back(std::move(filtered));
            }
        }

        if (filteredAlbums.isEmpty()) {
            continue;
        }

        int artistTrackCount = 0;
        for (const FilteredAlbum &filtered : filteredAlbums) {
            artistTrackCount += filtered.album->count;
        }

        FlatRow artistRow;
        artistRow.type = RowType::Artist;
        artistRow.artist = artistName;
        artistRow.count = artistTrackCount;
        artistRow.expanded = isArtistExpanded(artistName, autoExpand);
        artistRow.selectionKey = selectionKeyForArtist(artistName);
        m_rows.push_back(std::move(artistRow));

        if (!m_rows.back().expanded) {
            continue;
        }

        for (const FilteredAlbum &filtered : filteredAlbums) {
            const AlbumNode &album = *filtered.album;
            FlatRow albumRow = makeAlbumRow(artistName, album);
            albumRow.expanded = isAlbumExpanded(album.key, autoExpand);
            m_rows.push_back(std::move(albumRow));

            if (!m_rows.back().expanded) {
                continue;
            }

            int trackNo = 1;
            for (const TrackNode *track : filtered.tracks) {
                m_rows.push_back(makeTrackRow(album, trackNo++, *track));
            }
        }
    }

    endResetModel();
    if (oldCount != count()) {
        emit countChanged();
    }
}

QString LibraryTreeModel::toLower(const QString &text) {
    return text.toLower();
}

QString LibraryTreeModel::selectionKeyForArtist(const QString &artist) {
    return QStringLiteral("artist|%1").arg(artist);
}

QString LibraryTreeModel::selectionKeyForAlbum(int sourceIndex) {
    return QStringLiteral("album|%1").arg(sourceIndex);
}

QString LibraryTreeModel::selectionKeyForTrack(int sourceIndex, int trackNumber, const QString &trackPath) {
    if (!trackPath.isEmpty()) {
        return QStringLiteral("track|%1").arg(trackPath);
    }
    return QStringLiteral("track|%1|%2").arg(sourceIndex).arg(trackNumber);
}
