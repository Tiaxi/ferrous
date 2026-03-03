#include "LibraryTreeModel.h"

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
    if (role == RowTypeRole) {
        return item.rowType;
    }
    if (role == ArtistRole) {
        return item.artist;
    }
    if (role == NameRole) {
        return item.name;
    }
    if (role == TitleRole) {
        return item.title;
    }
    if (role == CountRole) {
        return item.count;
    }
    if (role == ExpandedRole) {
        return item.expanded;
    }
    if (role == SourceIndexRole) {
        return item.sourceIndex;
    }
    if (role == KeyRole) {
        return item.key;
    }
    if (role == TrackNumberRole) {
        return item.trackNumber;
    }
    if (role == TrackPathRole) {
        return item.trackPath;
    }
    if (role == CoverPathRole) {
        return item.coverPath;
    }
    if (role == SelectionKeyRole) {
        return item.selectionKey;
    }
    if (role == DepthRole) {
        return item.depth;
    }
    if (role == OpenPathRole) {
        return item.openPath;
    }
    if (role == PlayPathsRole) {
        return item.playPaths;
    }
    return {};
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
        {DepthRole, "depth"},
        {OpenPathRole, "openPath"},
        {PlayPathsRole, "playPaths"},
    };
}

int LibraryTreeModel::count() const {
    return static_cast<int>(m_rows.size());
}

void LibraryTreeModel::setLibraryTree(const QVariantList &tree) {
    m_tree = parseTreeNodes(tree);
    rebuildRows();
}

void LibraryTreeModel::setSearchText(const QString &text) {
    const QString next = toLower(text.trimmed());
    if (next == m_searchLower) {
        return;
    }
    m_searchLower = next;
    rebuildRows();
}

void LibraryTreeModel::toggleKey(const QString &key) {
    if (key.isEmpty() || !m_searchLower.isEmpty()) {
        return;
    }

    for (const FlatRow &row : m_rows) {
        if (row.key == key && row.hasChildren) {
            m_expandedByKey.insert(key, !row.expanded);
            rebuildRows();
            return;
        }
    }
}

void LibraryTreeModel::toggleArtist(const QString &artist) {
    for (const FlatRow &row : m_rows) {
        if (row.rowType == QStringLiteral("artist") && row.artist == artist) {
            toggleKey(row.key);
            return;
        }
    }
}

void LibraryTreeModel::toggleAlbum(const QString &albumKey) {
    toggleKey(albumKey);
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
    out.insert(QStringLiteral("rowType"), item.rowType);
    out.insert(QStringLiteral("selectionKey"), item.selectionKey);
    out.insert(QStringLiteral("sourceIndex"), item.sourceIndex);
    out.insert(QStringLiteral("artist"), item.artist);
    out.insert(QStringLiteral("name"), item.name);
    out.insert(QStringLiteral("title"), item.title);
    out.insert(QStringLiteral("trackPath"), item.trackPath);
    out.insert(QStringLiteral("openPath"), item.openPath);
    out.insert(QStringLiteral("playPaths"), item.playPaths);
    out.insert(QStringLiteral("key"), item.key);
    out.insert(QStringLiteral("depth"), item.depth);
    out.insert(QStringLiteral("count"), item.count);
    return out;
}

QString LibraryTreeModel::toLower(const QString &text) {
    return text.toLower();
}

QStringList LibraryTreeModel::toStringList(const QVariantList &values) {
    QStringList out;
    out.reserve(values.size());
    for (const QVariant &value : values) {
        const QString text = value.toString();
        if (!text.isEmpty()) {
            out.push_back(text);
        }
    }
    return out;
}

QVector<LibraryTreeModel::TreeNode> LibraryTreeModel::parseTreeNodes(const QVariantList &rows) {
    return parseNodes(rows);
}

QVector<LibraryTreeModel::TreeNode> LibraryTreeModel::parseLegacyArtistTree(const QVariantList &rows) {
    QVector<TreeNode> artists;
    artists.reserve(rows.size());

    for (int artistIndex = 0; artistIndex < rows.size(); ++artistIndex) {
        const QVariantMap artistMap = rows[artistIndex].toMap();
        const QString artistName = artistMap.value(QStringLiteral("artist")).toString();
        const QVariantList albums = artistMap.value(QStringLiteral("albums")).toList();

        TreeNode artist;
        artist.rowType = QStringLiteral("artist");
        artist.artist = artistName;
        artist.title = artistName;
        artist.name = artistName;
        artist.key = QStringLiteral("artist|%1").arg(artistName);
        artist.selectionKey = artist.key;
        artist.count = albums.size();

        for (int albumIndex = 0; albumIndex < albums.size(); ++albumIndex) {
            const QVariantMap albumMap = albums[albumIndex].toMap();
            TreeNode album;
            album.rowType = QStringLiteral("album");
            album.artist = artistName;
            album.name = albumMap.value(QStringLiteral("name")).toString();
            album.title = album.name;
            album.count = albumMap.value(QStringLiteral("count")).toInt(0);
            if (albumMap.contains(QStringLiteral("sourceIndex"))) {
                album.sourceIndex = albumMap.value(QStringLiteral("sourceIndex")).toInt();
            } else {
                album.sourceIndex = -1;
            }
            album.coverPath = albumMap.value(QStringLiteral("coverPath")).toString();
            album.key = QStringLiteral("album|%1|%2").arg(artistName).arg(albumIndex);
            album.selectionKey = album.key;

            const QVariantList tracks = albumMap.value(QStringLiteral("tracks")).toList();
            QStringList albumPaths;
            int trackNo = 0;
            for (const QVariant &trackVar : tracks) {
                const QVariantMap trackMap = trackVar.toMap();
                const QString path = trackMap.value(QStringLiteral("path")).toString();
                TreeNode track;
                track.rowType = QStringLiteral("track");
                track.trackPath = path;
                track.openPath = path;
                track.trackNumber = ++trackNo;
                track.title = trackMap.value(QStringLiteral("title")).toString();
                track.name = track.title;
                track.key = path.isEmpty()
                    ? QStringLiteral("track|%1|%2").arg(album.key).arg(trackNo)
                    : QStringLiteral("track|%1").arg(path);
                track.selectionKey = track.key;
                if (!path.isEmpty()) {
                    track.playPaths.push_back(path);
                    albumPaths.push_back(path);
                }
                album.children.push_back(std::move(track));
            }
            album.playPaths = albumPaths;
            artist.playPaths.append(albumPaths);
            artist.children.push_back(std::move(album));
        }

        artists.push_back(std::move(artist));
    }

    return artists;
}

QVector<LibraryTreeModel::TreeNode> LibraryTreeModel::parseNodes(const QVariantList &rows) {
    QVector<TreeNode> nodes;
    nodes.reserve(rows.size());

    bool hasRowType = false;
    for (const QVariant &rowValue : rows) {
        if (!rowValue.toMap().value(QStringLiteral("rowType")).toString().isEmpty()) {
            hasRowType = true;
            break;
        }
    }
    if (!hasRowType) {
        return parseLegacyArtistTree(rows);
    }

    for (int index = 0; index < rows.size(); ++index) {
        const QVariantMap row = rows[index].toMap();
        TreeNode node;
        node.rowType = row.value(QStringLiteral("rowType")).toString();
        node.key = row.value(QStringLiteral("key")).toString();
        node.selectionKey = node.key;
        node.artist = row.value(QStringLiteral("artist")).toString();
        node.name = row.value(QStringLiteral("name")).toString();
        node.title = row.value(QStringLiteral("title")).toString();
        node.count = row.value(QStringLiteral("count")).toInt(0);
        if (row.contains(QStringLiteral("sourceIndex"))) {
            node.sourceIndex = row.value(QStringLiteral("sourceIndex")).toInt();
        } else {
            node.sourceIndex = -1;
        }
        node.trackNumber = row.value(QStringLiteral("trackNumber")).toInt(0);
        node.trackPath = row.value(QStringLiteral("trackPath")).toString();
        node.openPath = row.value(QStringLiteral("path")).toString();
        node.coverPath = row.value(QStringLiteral("coverPath")).toString();
        node.playPaths = toStringList(row.value(QStringLiteral("playPaths")).toList());

        if (node.rowType == QStringLiteral("track") && node.trackPath.isEmpty()) {
            node.trackPath = node.openPath;
        }
        if (node.playPaths.isEmpty() && !node.trackPath.isEmpty()) {
            node.playPaths.push_back(node.trackPath);
        }
        if (node.name.isEmpty()) {
            node.name = node.title;
        }
        if (node.title.isEmpty()) {
            node.title = node.name;
        }
        if (node.key.isEmpty()) {
            const QString basis = !node.trackPath.isEmpty() ? node.trackPath : node.openPath;
            node.key = basis.isEmpty()
                ? QStringLiteral("%1|%2").arg(node.rowType).arg(index)
                : QStringLiteral("%1|%2").arg(node.rowType, basis);
            node.selectionKey = node.key;
        }

        node.children = parseNodes(row.value(QStringLiteral("children")).toList());
        nodes.push_back(std::move(node));
    }

    return nodes;
}

bool LibraryTreeModel::nodeMatchesSearch(const TreeNode &node, const QString &searchLower) {
    if (searchLower.isEmpty()) {
        return true;
    }

    const QString title = toLower(node.title);
    const QString name = toLower(node.name);
    const QString artist = toLower(node.artist);
    const QString trackPath = toLower(node.trackPath);
    const QString openPath = toLower(node.openPath);
    if (title.contains(searchLower) || name.contains(searchLower) || artist.contains(searchLower)
        || trackPath.contains(searchLower) || openPath.contains(searchLower))
    {
        return true;
    }

    for (const TreeNode &child : node.children) {
        if (nodeMatchesSearch(child, searchLower)) {
            return true;
        }
    }

    return false;
}

bool LibraryTreeModel::isExpanded(const TreeNode &node, bool autoExpand) const {
    if (node.children.isEmpty()) {
        return false;
    }
    if (autoExpand) {
        return true;
    }
    return m_expandedByKey.value(node.key, false);
}

void LibraryTreeModel::appendFlatRows(const QVector<TreeNode> &nodes, int depth, bool autoExpand) {
    for (const TreeNode &node : nodes) {
        if (!nodeMatchesSearch(node, m_searchLower)) {
            continue;
        }

        FlatRow row;
        row.rowType = node.rowType;
        row.key = node.key;
        row.selectionKey = node.selectionKey;
        row.artist = node.artist;
        row.name = node.name;
        row.title = node.title;
        row.count = node.count;
        row.sourceIndex = node.sourceIndex;
        row.trackNumber = node.trackNumber;
        row.trackPath = node.trackPath;
        row.openPath = node.openPath;
        row.coverPath = node.coverPath;
        row.playPaths = node.playPaths;
        row.depth = depth;
        row.hasChildren = !node.children.isEmpty();
        row.expanded = isExpanded(node, autoExpand);
        m_rows.push_back(std::move(row));

        if (!node.children.isEmpty() && m_rows.back().expanded) {
            appendFlatRows(node.children, depth + 1, autoExpand);
        }
    }
}

void LibraryTreeModel::rebuildRows() {
    const int oldCount = count();
    beginResetModel();
    m_rows.clear();
    const bool autoExpand = !m_searchLower.isEmpty();
    appendFlatRows(m_tree, 0, autoExpand);
    endResetModel();
    if (oldCount != count()) {
        emit countChanged();
    }
}
