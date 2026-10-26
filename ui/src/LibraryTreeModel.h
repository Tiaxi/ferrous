// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "BinaryBridgeCodec.h"

// The library tree's visible rows, and which of them are selected.
//
// The selection is a set of selection keys, so it follows its rows through
// tree updates; a row that leaves the tree, or is collapsed away, leaves the
// selection. Rows report it in the `selected` role, and a key-to-row index
// answers lookups, so neither costs more with a larger selection.
class LibraryTreeModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool parsing READ parsing NOTIFY parsingChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    // Whether a selected row has tracks to play.
    Q_PROPERTY(bool selectionActionable READ selectionActionable NOTIFY selectionChanged)
    // Whether revealKey is waiting for the backend to load the rows.
    Q_PROPERTY(bool revealPending READ revealPending NOTIFY revealPendingChanged)

public:
    enum Roles {
        RowTypeRole = Qt::UserRole + 1,
        ArtistRole,
        NameRole,
        TitleRole,
        CountRole,
        ExpandedRole,
        SourceIndexRole,
        KeyRole,
        TrackNumberRole,
        TrackPathRole,
        CoverPathRole,
        CoverStampRole,
        SelectionKeyRole,
        DepthRole,
        OpenPathRole,
        PlayPathsRole,
        SelectedRole,
    };

    explicit LibraryTreeModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    bool parsing() const;
    int selectionCount() const;
    bool selectionActionable() const;
    bool revealPending() const;

    Q_INVOKABLE void setLibraryTreeFromBinary(const QByteArray &treeBytes);
    Q_INVOKABLE void setExpandedKeys(const QStringList &keys);
    Q_INVOKABLE void setSearchText(const QString &text);
    Q_INVOKABLE void collapseAll();
    Q_INVOKABLE void toggleKey(const QString &key);
    Q_INVOKABLE void toggleArtist(const QString &artist);
    Q_INVOKABLE void toggleAlbum(const QString &albumKey);
    Q_INVOKABLE bool hasSelectionKey(const QString &selectionKey) const;
    Q_INVOKABLE int indexForSelectionKey(const QString &selectionKey) const;
    Q_INVOKABLE int sourceIndexForRow(int row) const;
    Q_INVOKABLE QString selectionKeyForRow(int row) const;
    Q_INVOKABLE int findArtistRowByPrefix(const QString &prefix, int startRow) const;
    Q_INVOKABLE QVariantMap rowDataForRow(int row) const;

    // Select only `row`, which becomes the anchor of range selections.
    Q_INVOKABLE void selectRow(int row);
    // Select the rows from the anchor to `row`; `row` is the anchor when
    // there is none.
    Q_INVOKABLE void selectRange(int row);
    // Add `row` to the selection or take it out; it becomes the anchor.
    Q_INVOKABLE void toggleRowSelected(int row);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isRowSelected(int row) const;
    Q_INVOKABLE bool isKeySelected(const QString &selectionKey) const;
    Q_INVOKABLE int anchorRow() const;
    // The first selected row, or -1.
    Q_INVOKABLE int firstSelectedRow() const;
    // The selected keys and rows, in tree order.
    Q_INVOKABLE QStringList selectedKeys() const;
    Q_INVOKABLE QVariantList selectedRowData() const;
    // What to queue for the selected rows that have tracks, in tree order,
    // for BridgeClient::playLibraryItems and the like.
    Q_INVOKABLE QVariant queueItemsForSelection() const;
    // What an action on the row with `selectionKey` queues: the selection
    // when the row is one of several selected rows, otherwise the row.
    Q_INVOKABLE QVariant queueItemsForKey(const QString &selectionKey) const;

    // Expand `expandKeys` and return the row of `selectionKey`. Rows the
    // backend has not loaded yet are -1 for now: once a tree update brings
    // them, or shows the key is not in the tree, revealFinished reports
    // the row, or -1. A later call or cancelReveal drops the wait.
    Q_INVOKABLE int revealKey(const QStringList &expandKeys, const QString &selectionKey);
    Q_INVOKABLE void cancelReveal();

signals:
    void countChanged();
    void parsingChanged();
    void treeApplied();
    void nodeExpansionRequested(const QString &key, bool expanded);
    void selectionChanged();
    void revealPendingChanged();
    void revealFinished(const QString &selectionKey, int row);

private:
    struct TreeNode {
        QString rowType;
        QString key;
        QString selectionKey;
        QString artist;
        QString name;
        QString title;
        int count{0};
        int sourceIndex{-1};
        int trackNumber{0};
        QString trackPath;
        QString openPath;
        QString coverPath;
        quint64 coverStamp{0};
        QStringList playPaths;
        QVector<TreeNode> children;
    };

    struct FlatRow {
        QString rowType;
        QString key;
        QString selectionKey;
        QString artist;
        QString name;
        QString title;
        int count{0};
        bool expanded{false};
        int sourceIndex{-1};
        int trackNumber{0};
        QString trackPath;
        QString openPath;
        QString coverPath;
        quint64 coverStamp{0};
        QStringList playPaths;
        int depth{0};
        bool hasChildren{false};
        // The backend sent the children of a row that has them; it sends
        // those of collapsed artists and albums once they are expanded.
        bool childrenLoaded{false};
    };

    static QString toLower(const QString &text);
    static QVector<TreeNode> parseTreeNodesFromBinary(const QByteArray &treeBytes);
    static bool rowsEqual(const FlatRow &a, const FlatRow &b);
    static bool nodeMatchesSearch(const TreeNode &node, const QString &searchLower);
    bool isExpanded(const TreeNode &node, bool autoExpand) const;
    void appendFlatRows(
        const QVector<TreeNode> &nodes,
        QVector<FlatRow> *rows,
        int depth,
        bool autoExpand) const;
    QVector<FlatRow> buildFlatRows() const;
    // The rows as views see them, also while an update is applied.
    int rowTotal() const;
    const FlatRow &rowAt(int row) const;
    void applyRows(QVector<FlatRow> nextRows);
    bool applyRowsIncremental(const QVector<FlatRow> &nextRows);
    void rebuildRows();
    void rebuildRowIndex();
    static bool isActionable(const FlatRow &row);
    static void appendQueueItem(const FlatRow &row, BinaryBridgeCodec::LibraryQueueItems *out);
    QVariant queueItemsForRows(const QVector<int> &rows) const;
    QVector<int> selectedRowsSorted() const;
    // Replace the selection with `keys`, telling views which rows changed.
    void setSelection(QSet<QString> keys, const QVector<int> &changedRows);
    void notifyRowsChanged(QVector<int> rows, const QVector<int> &roles);
    // Drop selected keys that have no row now.
    void pruneSelection();
    bool revealAnswered(const QStringList &expandKeys) const;
    void settleReveal();

    QVector<TreeNode> m_tree;
    QVector<FlatRow> m_rows;
    // While an update is applied, the rows are the first m_stagedPrefix rows
    // of *m_stagedNext, followed by m_rows from m_stagedOldStart on.
    const QVector<FlatRow> *m_stagedNext{nullptr};
    int m_stagedPrefix{0};
    int m_stagedOldStart{0};
    QHash<QString, bool> m_expandedByKey;
    // The row of each selection key, for m_rows.
    QHash<QString, int> m_rowByKey;
    QSet<QString> m_selectedKeys;
    QString m_anchorKey;
    mutable int m_actionableCache{-1};
    // The reveal waiting for rows, if any.
    QString m_revealKey;
    QStringList m_revealExpandKeys;
    bool m_hasTree{false};
    QString m_searchLower;
    QFutureWatcher<QVector<TreeNode>> m_parseWatcher;
    int m_parseGeneration{0};
    bool m_parseInFlight{false};
    bool m_hasQueuedTree{false};
    QByteArray m_queuedTree;
};
