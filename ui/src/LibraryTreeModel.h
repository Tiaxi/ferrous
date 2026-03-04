#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

class LibraryTreeModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool parsing READ parsing NOTIFY parsingChanged)

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
        SelectionKeyRole,
        DepthRole,
        OpenPathRole,
        PlayPathsRole,
    };

    explicit LibraryTreeModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;
    bool parsing() const;

    Q_INVOKABLE void setLibraryTreeFromBinary(const QByteArray &treeBytes);
    Q_INVOKABLE void setSearchText(const QString &text);
    Q_INVOKABLE void toggleKey(const QString &key);
    Q_INVOKABLE void toggleArtist(const QString &artist);
    Q_INVOKABLE void toggleAlbum(const QString &albumKey);
    Q_INVOKABLE bool hasSelectionKey(const QString &selectionKey) const;
    Q_INVOKABLE int indexForSelectionKey(const QString &selectionKey) const;
    Q_INVOKABLE int sourceIndexForRow(int row) const;
    Q_INVOKABLE QString selectionKeyForRow(int row) const;
    Q_INVOKABLE QVariantMap rowDataForRow(int row) const;

signals:
    void countChanged();
    void parsingChanged();
    void treeApplied();
    void nodeExpansionRequested(const QString &key, bool expanded);

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
        QStringList playPaths;
        int depth{0};
        bool hasChildren{false};
    };

    static QString toLower(const QString &text);
    static QVector<TreeNode> parseTreeNodesFromBinary(const QByteArray &treeBytes);
    static bool rowsEqual(const FlatRow &a, const FlatRow &b);
    static bool nodeMatchesSearch(const TreeNode &node, const QString &searchLower);
    static bool hasDuplicateSelectionKeys(const QVector<FlatRow> &rows);
    bool isExpanded(const TreeNode &node, bool autoExpand) const;
    void appendFlatRows(
        const QVector<TreeNode> &nodes,
        QVector<FlatRow> *rows,
        int depth,
        bool autoExpand) const;
    QVector<FlatRow> buildFlatRows() const;
    bool hasDetectedMove(const QVector<FlatRow> &nextRows) const;
    bool shouldUseResetForRows(const QVector<FlatRow> &nextRows) const;
    bool applyRowsIncremental(const QVector<FlatRow> &nextRows);
    void applyRowsReset(const QVector<FlatRow> &nextRows);
    void rebuildRows();

    QVector<TreeNode> m_tree;
    QVector<FlatRow> m_rows;
    QHash<QString, bool> m_expandedByKey;
    QString m_searchLower;
    QFutureWatcher<QVector<TreeNode>> m_parseWatcher;
    int m_parseGeneration{0};
    bool m_parseInFlight{false};
    bool m_hasQueuedTree{false};
    QByteArray m_queuedTree;
};
