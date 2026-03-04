#include "LibraryTreeModel.h"

#include <functional>

#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>
#include <QVariantMap>
#include <QtEndian>

namespace {

constexpr quint8 kRowTypeRoot = 0;
constexpr quint8 kRowTypeArtist = 1;
constexpr quint8 kRowTypeAlbum = 2;
constexpr quint8 kRowTypeSection = 3;
constexpr quint8 kRowTypeTrack = 4;

struct ParsedBinaryRow {
    quint8 rowType{0};
    int depth{0};
    int sourceIndex{-1};
    int trackNumber{0};
    int childCount{0};
    QString title;
    QString key;
    QString artist;
    QString path;
    QString coverPath;
    QString trackPath;
    QStringList playPaths;
};

class Reader {
public:
    explicit Reader(const QByteArray &bytes)
        : m_bytes(bytes)
        , m_offset(0) {}

    bool atEnd() const {
        return m_offset == m_bytes.size();
    }

    bool readU8(quint8 *out) {
        if (!out || m_offset + 1 > m_bytes.size()) {
            return false;
        }
        *out = static_cast<quint8>(m_bytes[m_offset]);
        m_offset += 1;
        return true;
    }

    bool readU16(quint16 *out) {
        if (!out || m_offset + 2 > m_bytes.size()) {
            return false;
        }
        *out = qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(m_bytes.constData() + m_offset));
        m_offset += 2;
        return true;
    }

    bool readI32(qint32 *out) {
        if (!out || m_offset + 4 > m_bytes.size()) {
            return false;
        }
        *out = qFromLittleEndian<qint32>(reinterpret_cast<const uchar *>(m_bytes.constData() + m_offset));
        m_offset += 4;
        return true;
    }

    bool readU32(quint32 *out) {
        if (!out || m_offset + 4 > m_bytes.size()) {
            return false;
        }
        *out = qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(m_bytes.constData() + m_offset));
        m_offset += 4;
        return true;
    }

    bool readUtf8U16(QString *out) {
        if (!out) {
            return false;
        }
        quint16 len = 0;
        if (!readU16(&len)) {
            return false;
        }
        if (m_offset + len > m_bytes.size()) {
            return false;
        }
        *out = QString::fromUtf8(m_bytes.constData() + m_offset, len);
        m_offset += len;
        return true;
    }

private:
    const QByteArray &m_bytes;
    qsizetype m_offset;
};

QString rowTypeName(quint8 rowType) {
    switch (rowType) {
    case kRowTypeRoot:
        return QStringLiteral("root");
    case kRowTypeArtist:
        return QStringLiteral("artist");
    case kRowTypeAlbum:
        return QStringLiteral("album");
    case kRowTypeSection:
        return QStringLiteral("section");
    case kRowTypeTrack:
        return QStringLiteral("track");
    default:
        return QStringLiteral("unknown");
    }
}

QString fallbackNodeName(const ParsedBinaryRow &row) {
    if (row.rowType == kRowTypeAlbum && !row.path.isEmpty()) {
        const QString fileName = QFileInfo(row.path).fileName();
        if (!fileName.isEmpty()) {
            return fileName;
        }
    }
    return row.title;
}

bool parseRows(const QByteArray &treeBytes, QVector<ParsedBinaryRow> *rowsOut) {
    if (!rowsOut) {
        return false;
    }
    rowsOut->clear();
    if (treeBytes.isEmpty()) {
        return true;
    }

    Reader reader(treeBytes);
    quint32 rowCount = 0;
    if (!reader.readU32(&rowCount)) {
        return false;
    }

    rowsOut->reserve(static_cast<int>(rowCount));
    for (quint32 i = 0; i < rowCount; ++i) {
        ParsedBinaryRow row;
        quint16 depth = 0;
        qint32 sourceIndex = -1;
        quint16 trackNumber = 0;
        quint16 childCount = 0;
        quint16 playPathCount = 0;

        if (!reader.readU8(&row.rowType)
            || !reader.readU16(&depth)
            || !reader.readI32(&sourceIndex)
            || !reader.readU16(&trackNumber)
            || !reader.readU16(&childCount)
            || !reader.readUtf8U16(&row.title)
            || !reader.readUtf8U16(&row.key)
            || !reader.readUtf8U16(&row.artist)
            || !reader.readUtf8U16(&row.path)
            || !reader.readUtf8U16(&row.coverPath)
            || !reader.readUtf8U16(&row.trackPath)
            || !reader.readU16(&playPathCount)) {
            return false;
        }

        row.depth = static_cast<int>(depth);
        row.sourceIndex = static_cast<int>(sourceIndex);
        row.trackNumber = static_cast<int>(trackNumber);
        row.childCount = static_cast<int>(childCount);
        row.playPaths.reserve(playPathCount);
        for (quint16 p = 0; p < playPathCount; ++p) {
            QString path;
            if (!reader.readUtf8U16(&path)) {
                return false;
            }
            row.playPaths.push_back(path);
        }

        rowsOut->push_back(std::move(row));
    }

    return reader.atEnd();
}

} // namespace

LibraryTreeModel::LibraryTreeModel(QObject *parent)
    : QAbstractListModel(parent) {
    connect(&m_parseWatcher, &QFutureWatcher<QVector<TreeNode>>::finished, this, [this]() {
        const bool wasParsing = m_parseInFlight;
        m_parseInFlight = false;
        const int finishedGeneration = m_parseWatcher.property("parseGeneration").toInt();
        if (finishedGeneration != m_parseGeneration) {
            if (m_hasQueuedTree) {
                QByteArray queued = std::move(m_queuedTree);
                m_queuedTree.clear();
                m_hasQueuedTree = false;
                setLibraryTreeFromBinary(queued);
                if (wasParsing && !m_parseInFlight) {
                    emit parsingChanged();
                }
                return;
            }
            if (wasParsing) {
                emit parsingChanged();
            }
            return;
        }
        if (m_hasQueuedTree) {
            QByteArray queued = std::move(m_queuedTree);
            m_queuedTree.clear();
            m_hasQueuedTree = false;
            setLibraryTreeFromBinary(queued);
            if (wasParsing && !m_parseInFlight) {
                emit parsingChanged();
            }
            return;
        }
        m_tree = m_parseWatcher.result();
        rebuildRows();
        if (wasParsing) {
            emit parsingChanged();
        }
    });
}

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

bool LibraryTreeModel::parsing() const {
    return m_parseInFlight;
}

void LibraryTreeModel::setLibraryTreeFromBinary(const QByteArray &treeBytes) {
    if (!m_parseInFlight && treeBytes.size() <= 16 * 1024) {
        ++m_parseGeneration;
        m_tree = parseTreeNodesFromBinary(treeBytes);
        rebuildRows();
        return;
    }

    if (m_parseInFlight) {
        m_queuedTree = treeBytes;
        m_hasQueuedTree = true;
        return;
    }

    const int generation = ++m_parseGeneration;
    auto future = QtConcurrent::run([treeBytes]() { return parseTreeNodesFromBinary(treeBytes); });
    m_parseWatcher.setProperty("parseGeneration", generation);
    m_parseInFlight = true;
    emit parsingChanged();
    m_parseWatcher.setFuture(future);
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

QVector<LibraryTreeModel::TreeNode> LibraryTreeModel::parseTreeNodesFromBinary(const QByteArray &treeBytes) {
    QVector<ParsedBinaryRow> parsedRows;
    if (!parseRows(treeBytes, &parsedRows) || parsedRows.isEmpty()) {
        return {};
    }

    QVector<QVector<int>> children;
    children.resize(parsedRows.size());
    QVector<int> topLevel;
    QVector<int> stack;

    for (int i = 0; i < parsedRows.size(); ++i) {
        int depth = std::max(0, parsedRows[i].depth);
        if (depth > stack.size()) {
            depth = stack.size();
        }
        while (stack.size() > depth) {
            stack.removeLast();
        }

        if (stack.isEmpty()) {
            topLevel.push_back(i);
        } else {
            children[stack.last()].push_back(i);
        }
        stack.push_back(i);
    }

    std::function<TreeNode(int)> buildNode = [&](int index) {
        const ParsedBinaryRow &row = parsedRows[index];

        TreeNode node;
        node.rowType = rowTypeName(row.rowType);
        node.key = row.key;
        node.selectionKey = row.key;
        node.artist = row.artist;
        node.title = row.title;
        node.count = row.childCount;
        node.sourceIndex = row.sourceIndex;
        node.trackNumber = row.trackNumber;
        node.trackPath = row.trackPath;
        node.openPath = row.path;
        node.coverPath = row.coverPath;
        node.playPaths = row.playPaths;

        if (node.rowType == QStringLiteral("track") && node.trackPath.isEmpty()) {
            node.trackPath = node.openPath;
        }
        if (node.rowType == QStringLiteral("track") && node.openPath.isEmpty()) {
            node.openPath = node.trackPath;
        }

        node.name = fallbackNodeName(row);
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

        const auto &childIndices = children[index];
        node.children.reserve(childIndices.size());
        for (int childIndex : childIndices) {
            node.children.push_back(buildNode(childIndex));
        }
        return node;
    };

    QVector<TreeNode> roots;
    roots.reserve(topLevel.size());
    for (int index : topLevel) {
        roots.push_back(buildNode(index));
    }
    return roots;
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
        || trackPath.contains(searchLower) || openPath.contains(searchLower)) {
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
    emit treeApplied();
}
