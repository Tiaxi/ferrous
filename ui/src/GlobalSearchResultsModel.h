// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include <limits>

class GlobalSearchResultsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString resultFilter READ resultFilter WRITE setResultFilter NOTIFY searchRowsChanged)

public:
    struct SearchDisplayRow {
        QString kind;
        QString rowType;
        QString sectionTitle;
        float score{0.0f};
        QString label;
        QString artist;
        QString album;
        QString rootLabel;
        QString genre;
        QString coverPath;
        QString coverUrl;
        QString artistKey;
        QString albumKey;
        QString sectionKey;
        QString trackKey;
        QString trackPath;
        int year{std::numeric_limits<int>::min()};
        int trackNumber{0};
        int count{0};
        float lengthSeconds{-1.0f};
        QString lengthText;

        bool operator==(const SearchDisplayRow &other) const {
            return kind == other.kind
                && rowType == other.rowType
                && sectionTitle == other.sectionTitle
                && score == other.score
                && label == other.label
                && artist == other.artist
                && album == other.album
                && rootLabel == other.rootLabel
                && genre == other.genre
                && coverPath == other.coverPath
                && coverUrl == other.coverUrl
                && artistKey == other.artistKey
                && albumKey == other.albumKey
                && sectionKey == other.sectionKey
                && trackKey == other.trackKey
                && trackPath == other.trackPath
                && year == other.year
                && trackNumber == other.trackNumber
                && count == other.count
                && lengthSeconds == other.lengthSeconds
                && lengthText == other.lengthText;
        }

        bool operator!=(const SearchDisplayRow &other) const {
            return !(*this == other);
        }

        bool equivalentForView(const SearchDisplayRow &other) const {
            return kind == other.kind
                && rowType == other.rowType
                && sectionTitle == other.sectionTitle
                && label == other.label
                && artist == other.artist
                && album == other.album
                && rootLabel == other.rootLabel
                && genre == other.genre
                && coverPath == other.coverPath
                && coverUrl == other.coverUrl
                && artistKey == other.artistKey
                && albumKey == other.albumKey
                && sectionKey == other.sectionKey
                && trackKey == other.trackKey
                && trackPath == other.trackPath
                && year == other.year
                && trackNumber == other.trackNumber
                && count == other.count
                && lengthSeconds == other.lengthSeconds
                && lengthText == other.lengthText;
        }
    };

    enum Role {
        KindRole = Qt::UserRole + 1,
        RowTypeRole,
        SectionTitleRole,
        ScoreRole,
        LabelRole,
        ArtistRole,
        AlbumRole,
        RootLabelRole,
        GenreRole,
        CoverPathRole,
        CoverUrlRole,
        ArtistKeyRole,
        AlbumKeyRole,
        SectionKeyRole,
        TrackKeyRole,
        TrackPathRole,
        YearRole,
        TrackNumberRole,
        CountRole,
        LengthSecondsRole,
        LengthTextRole,
        DelegateTypeRole,
    };
    Q_ENUM(Role)

    explicit GlobalSearchResultsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void presentSearchRows(QVector<SearchDisplayRow> rows);
    QString resultFilter() const { return m_resultFilter; }
    void setResultFilter(const QString &filter);
    Q_INVOKABLE QString highlightText(const QString &text, const QString &query) const;
    void replaceRows(QVector<SearchDisplayRow> rows);
    void replaceRowsBatched(QVector<SearchDisplayRow> rows, int batchSize = 10);
    void cancelBatchedInsertion();

    Q_INVOKABLE QVariantMap rowDataAt(int index) const;
    Q_INVOKABLE bool isSelectableIndex(int index) const;
    Q_INVOKABLE int nextSelectableIndex(int startIndex, int step, bool wrap) const;

signals:
    void searchRowsChanged();

private:
    void rebuildSearchRows();
    QString m_resultFilter{QStringLiteral("all")};
    QVector<SearchDisplayRow> m_sourceSearchRows;
    QVector<SearchDisplayRow> m_rows;

    QVector<SearchDisplayRow> m_pendingBatchRows;
    int m_batchInsertOffset{0};
    int m_batchSize{10};
    QTimer m_batchTimer;

    void insertNextBatch();
};
