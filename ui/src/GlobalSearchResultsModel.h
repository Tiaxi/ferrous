// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

#include <limits>

class GlobalSearchResultsModel : public QAbstractListModel {
    Q_OBJECT

public:
    struct SearchDisplayRow {
        QString matchDetail;
        QString kind;
        QString rowType;
        QString sectionTitle;
        float score{0.0f};
        QString label;
        QString artist;
        QString album;
        QString rootLabel;
        bool showRoot{false};
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
        // A "more" or "truncated" row: how many of its section's results
        // show; `count` is how many there are.
        int shownCount{0};

        bool operator==(const SearchDisplayRow &other) const {
            return matchDetail == other.matchDetail && kind == other.kind
                && rowType == other.rowType
                && sectionTitle == other.sectionTitle
                && score == other.score
                && label == other.label
                && artist == other.artist
                && album == other.album
                && rootLabel == other.rootLabel
                && showRoot == other.showRoot
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
                && lengthText == other.lengthText
                && shownCount == other.shownCount;
        }

        bool operator!=(const SearchDisplayRow &other) const {
            return !(*this == other);
        }

        bool equivalentForView(const SearchDisplayRow &other) const {
            return matchDetail == other.matchDetail && kind == other.kind
                && rowType == other.rowType
                && sectionTitle == other.sectionTitle
                && label == other.label
                && artist == other.artist
                && album == other.album
                && rootLabel == other.rootLabel
                && showRoot == other.showRoot
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
                && lengthText == other.lengthText
                && shownCount == other.shownCount;
        }
    };

    enum Role {
        MatchDetailRole = Qt::UserRole + 1,
        KindRole,
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
        ShowRootRole,
        ShownCountRole,
    };
    Q_ENUM(Role)

    explicit GlobalSearchResultsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // The most results of a type a search shows; see
    // BridgeClient::expandGlobalSearch.
    static constexpr int kMaxShownPerType = 2000;

    // Shows the result rows of a search in sections, one per result type,
    // in the order of their best match. `totals` are how many artists,
    // albums and tracks the search found; a section that shows fewer ends
    // in a "more" row, or a "truncated" note when `canShowMore` is false.
    void presentSearchRows(QVector<SearchDisplayRow> rows, const QVector<int> &totals = {},
                           bool canShowMore = false);
    Q_INVOKABLE QString highlightText(const QString &text, const QString &query) const;
    // `text` cut where the query's matches start and end: a list of
    // {text, match} maps, in order, that join to `text`.
    Q_INVOKABLE QVariantList highlightSegments(const QString &text, const QString &query) const;
    void replaceRows(QVector<SearchDisplayRow> rows);
    void replaceRowsBatched(QVector<SearchDisplayRow> rows, int batchSize = 10);
    void cancelBatchedInsertion();

    Q_INVOKABLE QVariantMap rowDataAt(int index) const;
    Q_INVOKABLE bool isSelectableIndex(int index) const;
    Q_INVOKABLE int nextSelectableIndex(int startIndex, int step, bool wrap) const;
    // The row of the `ordinal`th (from 0) result of `rowType`, or -1.
    Q_INVOKABLE int indexOfResult(const QString &rowType, int ordinal) const;

signals:
    void searchRowsChanged();

private:
    void rebuildSearchRows();
    // Which characters of `text` the query matches.
    QVector<bool> matchedCharacters(const QString &text, const QString &query) const;
    QVector<SearchDisplayRow> m_sourceSearchRows;
    QVector<int> m_sourceTotals;
    bool m_canShowMore{false};
    QVector<SearchDisplayRow> m_rows;

    QVector<SearchDisplayRow> m_pendingBatchRows;
    int m_batchInsertOffset{0};
    int m_batchSize{10};
    QTimer m_batchTimer;

    void insertNextBatch();
};
