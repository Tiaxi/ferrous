// SPDX-License-Identifier: GPL-3.0-or-later

#include "LibraryTreeModel.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <vector>

#include <QFileInfo>
#include <QHash>
#include <QSet>
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

    quint64 remaining() const {
        return static_cast<quint64>(m_bytes.size() - m_offset);
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

    bool readUtf8(QString *out) {
        if (!out) {
            return false;
        }
        quint32 len = 0;
        if (!readU32(&len)) {
            return false;
        }
        if (static_cast<quint64>(len) > static_cast<quint64>(m_bytes.size() - m_offset)) {
            return false;
        }
        *out = QString::fromUtf8(m_bytes.constData() + m_offset, static_cast<qsizetype>(len));
        m_offset += static_cast<qsizetype>(len);
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
        quint32 trackNumber = 0;
        quint32 childCount = 0;
        quint32 playPathCount = 0;

        if (!reader.readU8(&row.rowType)
            || !reader.readU16(&depth)
            || !reader.readI32(&sourceIndex)
            || !reader.readU32(&trackNumber)
            || !reader.readU32(&childCount)
            || !reader.readUtf8(&row.title)
            || !reader.readUtf8(&row.key)
            || !reader.readUtf8(&row.artist)
            || !reader.readUtf8(&row.path)
            || !reader.readUtf8(&row.coverPath)
            || !reader.readUtf8(&row.trackPath)
            || !reader.readU32(&playPathCount)) {
            return false;
        }

        row.depth = static_cast<int>(depth);
        row.sourceIndex = static_cast<int>(sourceIndex);
        row.trackNumber = static_cast<int>(std::min<quint32>(trackNumber, INT_MAX));
        row.childCount = static_cast<int>(std::min<quint32>(childCount, INT_MAX));
        // Each path takes at least its 4-byte length, which bounds the count.
        if (playPathCount > reader.remaining() / 4) {
            return false;
        }
        row.playPaths.reserve(static_cast<qsizetype>(playPathCount));
        for (quint32 p = 0; p < playPathCount; ++p) {
            QString path;
            if (!reader.readUtf8(&path)) {
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
    return rowTotal();
}

int LibraryTreeModel::rowTotal() const {
    if (m_stagedNext != nullptr) {
        return m_stagedPrefix + static_cast<int>(m_rows.size()) - m_stagedOldStart;
    }
    return static_cast<int>(m_rows.size());
}

const LibraryTreeModel::FlatRow &LibraryTreeModel::rowAt(int row) const {
    if (m_stagedNext != nullptr) {
        if (row < m_stagedPrefix) {
            return (*m_stagedNext)[row];
        }
        return m_rows[m_stagedOldStart + row - m_stagedPrefix];
    }
    return m_rows[row];
}

QVariant LibraryTreeModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const int row = index.row();
    if (row < 0 || row >= rowTotal()) {
        return {};
    }

    const FlatRow &item = rowAt(row);
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
    return rowTotal();
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

void LibraryTreeModel::setExpandedKeys(const QStringList &keys) {
    QHash<QString, bool> expandedByKey;
    expandedByKey.reserve(keys.size());
    for (const QString &key : keys) {
        const QString normalized = key.trimmed();
        if (!normalized.isEmpty()) {
            expandedByKey.insert(normalized, true);
        }
    }
    if (m_expandedByKey == expandedByKey) {
        return;
    }
    m_expandedByKey = std::move(expandedByKey);
    if (!m_tree.isEmpty()) {
        rebuildRows();
    }
}

void LibraryTreeModel::setSearchText(const QString &text) {
    const QString next = toLower(text.trimmed());
    if (next == m_searchLower) {
        return;
    }
    m_searchLower = next;
    rebuildRows();
}

void LibraryTreeModel::collapseAll() {
    QStringList expandedKeys;
    expandedKeys.reserve(m_expandedByKey.size());
    for (auto it = m_expandedByKey.begin(); it != m_expandedByKey.end(); ++it) {
        if (it.value()) {
            expandedKeys.push_back(it.key());
            it.value() = false;
        }
    }

    bool changed = !expandedKeys.isEmpty();
    for (const TreeNode &node : m_tree) {
        if (node.rowType == QStringLiteral("root") && isExpanded(node, false)) {
            m_expandedByKey.insert(node.key, false);
            changed = true;
        }
    }

    if (!changed) {
        return;
    }
    for (const QString &key : expandedKeys) {
        emit nodeExpansionRequested(key, false);
    }
    rebuildRows();
}

void LibraryTreeModel::toggleKey(const QString &key) {
    if (key.isEmpty() || !m_searchLower.isEmpty()) {
        return;
    }

    for (int i = 0; i < rowTotal(); ++i) {
        const FlatRow &row = rowAt(i);
        if (row.key == key && row.hasChildren) {
            const bool nextExpanded = !row.expanded;
            m_expandedByKey.insert(key, nextExpanded);
            if (row.rowType != QStringLiteral("root")) {
                emit nodeExpansionRequested(key, nextExpanded);
            }
            rebuildRows();
            return;
        }
    }
}

void LibraryTreeModel::toggleArtist(const QString &artist) {
    for (int i = 0; i < rowTotal(); ++i) {
        const FlatRow &row = rowAt(i);
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
    return indexForSelectionKey(selectionKey) >= 0;
}

int LibraryTreeModel::indexForSelectionKey(const QString &selectionKey) const {
    if (selectionKey.isEmpty()) {
        return -1;
    }
    for (int i = 0; i < rowTotal(); ++i) {
        if (rowAt(i).selectionKey == selectionKey) {
            return i;
        }
    }
    return -1;
}

int LibraryTreeModel::sourceIndexForRow(int row) const {
    if (row < 0 || row >= rowTotal()) {
        return -1;
    }
    return rowAt(row).sourceIndex;
}

QString LibraryTreeModel::selectionKeyForRow(int row) const {
    if (row < 0 || row >= rowTotal()) {
        return {};
    }
    return rowAt(row).selectionKey;
}

int LibraryTreeModel::findArtistRowByPrefix(const QString &prefix, int startRow) const {
    const QString loweredPrefix = toLower(prefix.trimmed());
    const int rowCount = rowTotal();
    if (loweredPrefix.isEmpty() || rowCount == 0) {
        return -1;
    }

    const int normalizedStart = std::clamp(startRow, 0, rowCount);
    for (int offset = 0; offset < rowCount; ++offset) {
        const int row = (normalizedStart + offset) % rowCount;
        const FlatRow &item = rowAt(row);
        if (item.rowType != QStringLiteral("artist")) {
            continue;
        }
        if (toLower(item.artist).startsWith(loweredPrefix)) {
            return row;
        }
    }
    return -1;
}

QVariantMap LibraryTreeModel::rowDataForRow(int row) const {
    QVariantMap out;
    if (row < 0 || row >= rowTotal()) {
        return out;
    }

    const FlatRow &item = rowAt(row);
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
    out.insert(QStringLiteral("expanded"), item.expanded);
    out.insert(QStringLiteral("hasChildren"), item.hasChildren);
    return out;
}

QString LibraryTreeModel::toLower(const QString &text) {
    return text.toLower();
}

bool LibraryTreeModel::rowsEqual(const FlatRow &a, const FlatRow &b) {
    return a.rowType == b.rowType
        && a.key == b.key
        && a.selectionKey == b.selectionKey
        && a.artist == b.artist
        && a.name == b.name
        && a.title == b.title
        && a.count == b.count
        && a.expanded == b.expanded
        && a.sourceIndex == b.sourceIndex
        && a.trackNumber == b.trackNumber
        && a.trackPath == b.trackPath
        && a.openPath == b.openPath
        && a.coverPath == b.coverPath
        && a.playPaths == b.playPaths
        && a.depth == b.depth
        && a.hasChildren == b.hasChildren;
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
    const bool hasChildren = !node.children.isEmpty()
        || (node.rowType != QStringLiteral("track") && node.count > 0);
    if (!hasChildren) {
        return false;
    }
    if (autoExpand) {
        return true;
    }
    const bool defaultExpanded = node.rowType == QStringLiteral("root");
    return m_expandedByKey.value(node.key, defaultExpanded);
}

void LibraryTreeModel::appendFlatRows(
    const QVector<TreeNode> &nodes,
    QVector<FlatRow> *rows,
    int depth,
    bool autoExpand) const {
    if (!rows) {
        return;
    }
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
        row.hasChildren = !node.children.isEmpty()
            || (node.rowType != QStringLiteral("track") && node.count > 0);
        row.expanded = isExpanded(node, autoExpand);
        rows->push_back(std::move(row));

        if (!node.children.isEmpty() && rows->back().expanded) {
            appendFlatRows(node.children, rows, depth + 1, autoExpand);
        }
    }
}

QVector<LibraryTreeModel::FlatRow> LibraryTreeModel::buildFlatRows() const {
    QVector<FlatRow> rows;
    const bool autoExpand = !m_searchLower.isEmpty();
    rows.reserve(m_rows.size());
    appendFlatRows(m_tree, &rows, 0, autoExpand);
    return rows;
}

namespace {

// The positions in `sequence` of a longest strictly increasing subsequence.
std::vector<bool> longestIncreasingRun(const std::vector<int> &sequence) {
    const int size = static_cast<int>(sequence.size());
    std::vector<int> tails;
    std::vector<int> tailPositions;
    std::vector<int> previous(static_cast<size_t>(size), -1);
    for (int i = 0; i < size; ++i) {
        const int value = sequence[static_cast<size_t>(i)];
        const auto at = std::lower_bound(tails.begin(), tails.end(), value);
        const auto length = static_cast<size_t>(at - tails.begin());
        if (length > 0) {
            previous[static_cast<size_t>(i)] = tailPositions[length - 1];
        }
        if (at == tails.end()) {
            tails.push_back(value);
            tailPositions.push_back(i);
        } else {
            *at = value;
            tailPositions[length] = i;
        }
    }
    std::vector<bool> inRun(static_cast<size_t>(size), false);
    for (int i = tailPositions.empty() ? -1 : tailPositions.back(); i >= 0;
         i = previous[static_cast<size_t>(i)]) {
        inRun[static_cast<size_t>(i)] = true;
    }
    return inRun;
}

} // namespace

// Applies `nextRows` as row insertions, removals and data changes, matched by
// selection key, so that views keep their items, current index and position.
// A row that moved is removed and inserted again. Only when no row can be
// matched, because the keys are not unique or no key is kept, is the model
// reset.
bool LibraryTreeModel::applyRowsIncremental(const QVector<FlatRow> &nextRows) {
    const int oldCount = static_cast<int>(m_rows.size());
    const int newCount = static_cast<int>(nextRows.size());

    QHash<QString, int> oldPositions;
    oldPositions.reserve(oldCount);
    for (int i = 0; i < oldCount; ++i) {
        const QString &key = m_rows[i].selectionKey;
        if (key.isEmpty() || oldPositions.contains(key)) {
            return false;
        }
        oldPositions.insert(key, i);
    }

    // For each new row, its old position, if it had one.
    std::vector<int> matchedOld(static_cast<size_t>(newCount), -1);
    std::vector<int> sequence;
    sequence.reserve(static_cast<size_t>(newCount));
    QSet<QString> newKeys;
    newKeys.reserve(newCount);
    for (int i = 0; i < newCount; ++i) {
        const QString &key = nextRows[i].selectionKey;
        if (key.isEmpty() || newKeys.contains(key)) {
            return false;
        }
        newKeys.insert(key);
        const auto found = oldPositions.constFind(key);
        if (found != oldPositions.constEnd()) {
            matchedOld[static_cast<size_t>(i)] = found.value();
            sequence.push_back(found.value());
        }
    }
    if (sequence.empty() && oldCount > 0 && newCount > 0) {
        // Nothing is kept: a reset says the same with one signal.
        return false;
    }

    // Rows kept in place are a longest run of matched rows in the same
    // order; the other matched rows moved.
    const std::vector<bool> inRun = longestIncreasingRun(sequence);
    std::vector<bool> oldKept(static_cast<size_t>(oldCount), false);
    std::vector<bool> newKept(static_cast<size_t>(newCount), false);
    for (int i = 0, matched = 0; i < newCount; ++i) {
        const int old = matchedOld[static_cast<size_t>(i)];
        if (old < 0) {
            continue;
        }
        if (inRun[static_cast<size_t>(matched)]) {
            oldKept[static_cast<size_t>(old)] = true;
            newKept[static_cast<size_t>(i)] = true;
        }
        ++matched;
    }

    // Views see the new rows before m_stagedPrefix and the old ones from
    // m_stagedOldStart on, so each change costs only its own rows.
    m_stagedNext = &nextRows;
    m_stagedPrefix = 0;
    m_stagedOldStart = 0;
    std::vector<int> changedRows;
    while (m_stagedOldStart < oldCount || m_stagedPrefix < newCount) {
        if (m_stagedOldStart < oldCount && !oldKept[static_cast<size_t>(m_stagedOldStart)]) {
            int end = m_stagedOldStart;
            while (end < oldCount && !oldKept[static_cast<size_t>(end)]) {
                ++end;
            }
            beginRemoveRows(QModelIndex{}, m_stagedPrefix, m_stagedPrefix + end - m_stagedOldStart - 1);
            m_stagedOldStart = end;
            endRemoveRows();
            continue;
        }
        if (m_stagedPrefix < newCount && !newKept[static_cast<size_t>(m_stagedPrefix)]) {
            int end = m_stagedPrefix;
            while (end < newCount && !newKept[static_cast<size_t>(end)]) {
                ++end;
            }
            beginInsertRows(QModelIndex{}, m_stagedPrefix, end - 1);
            m_stagedPrefix = end;
            endInsertRows();
            continue;
        }
        // Both rows are kept, and the run keeps them in the same order.
        if (!rowsEqual(m_rows[m_stagedOldStart], nextRows[m_stagedPrefix])) {
            changedRows.push_back(m_stagedPrefix);
        }
        ++m_stagedPrefix;
        ++m_stagedOldStart;
    }
    m_stagedNext = nullptr;
    m_rows = nextRows;

    for (size_t i = 0; i < changedRows.size();) {
        size_t end = i + 1;
        while (end < changedRows.size() && changedRows[end] == changedRows[end - 1] + 1) {
            ++end;
        }
        emit dataChanged(index(changedRows[i]), index(changedRows[end - 1]));
        i = end;
    }
    return true;
}

void LibraryTreeModel::applyRows(QVector<FlatRow> nextRows) {
    if (applyRowsIncremental(nextRows)) {
        return;
    }
    beginResetModel();
    m_rows = std::move(nextRows);
    endResetModel();
}

void LibraryTreeModel::rebuildRows() {
    const int oldCount = count();
    applyRows(buildFlatRows());
    if (oldCount != count()) {
        emit countChanged();
    }
    emit treeApplied();
}
