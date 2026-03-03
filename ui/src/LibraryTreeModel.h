#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVector>

class LibraryTreeModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

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
    };

    explicit LibraryTreeModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    Q_INVOKABLE void setLibraryTree(const QVariantList &tree);
    Q_INVOKABLE void setSearchText(const QString &text);
    Q_INVOKABLE void toggleArtist(const QString &artist);
    Q_INVOKABLE void toggleAlbum(const QString &albumKey);
    Q_INVOKABLE bool hasSelectionKey(const QString &selectionKey) const;
    Q_INVOKABLE int indexForSelectionKey(const QString &selectionKey) const;
    Q_INVOKABLE int sourceIndexForRow(int row) const;
    Q_INVOKABLE QString selectionKeyForRow(int row) const;
    Q_INVOKABLE QVariantMap rowDataForRow(int row) const;

signals:
    void countChanged();

private:
    struct TrackNode {
        QString title;
        QString path;
    };

    struct AlbumNode {
        QString artist;
        QString name;
        int count{0};
        int sourceIndex{-1};
        QString coverPath;
        QString key;
        QVector<TrackNode> tracks;
    };

    struct ArtistNode {
        QString artist;
        QVector<AlbumNode> albums;
    };

    enum class RowType {
        Artist,
        Album,
        Track,
    };

    struct FlatRow {
        RowType type{RowType::Artist};
        QString artist;
        QString name;
        QString title;
        int count{0};
        bool expanded{false};
        int sourceIndex{-1};
        QString key;
        int trackNumber{0};
        QString trackPath;
        QString coverPath;
        QString selectionKey;
    };

    bool isArtistExpanded(const QString &artist, bool autoExpand) const;
    bool isAlbumExpanded(const QString &albumKey, bool autoExpand) const;
    const ArtistNode *findArtistNode(const QString &artist) const;
    const AlbumNode *findAlbumNode(const QString &albumKey) const;
    FlatRow makeAlbumRow(const QString &artistName, const AlbumNode &album) const;
    FlatRow makeTrackRow(const AlbumNode &album, int trackNumber, const TrackNode &track) const;
    void rebuildRows();
    static QString toLower(const QString &text);
    static QString selectionKeyForArtist(const QString &artist);
    static QString selectionKeyForAlbum(int sourceIndex);
    static QString selectionKeyForTrack(int sourceIndex, int trackNumber, const QString &trackPath);
    int findArtistRowIndex(const QString &artist) const;
    void clearPendingArtistInsert(const QString &artist = QString());
    void processPendingArtistInsert();
    void schedulePendingArtistInsert();

    QVector<ArtistNode> m_tree;
    QVector<FlatRow> m_rows;
    QHash<QString, bool> m_expandedArtists;
    QHash<QString, bool> m_expandedAlbums;
    QString m_searchLower;

    struct PendingArtistInsert {
        QString artist;
        QVector<FlatRow> rows;
        int inserted{0};
    };
    static constexpr int kArtistExpandInsertBatchSize = 24;
    QVector<PendingArtistInsert> m_pendingArtistInserts;
    bool m_pendingArtistInsertScheduled{false};
};
