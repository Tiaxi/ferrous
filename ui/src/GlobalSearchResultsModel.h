#pragma once

#include <QAbstractListModel>
#include <QVariantMap>
#include <QVector>

#include <limits>

class GlobalSearchResultsModel : public QAbstractListModel {
    Q_OBJECT

public:
    struct SearchDisplayRow {
        QString kind;
        QString rowType;
        QString sectionTitle;
        float score{0.0f};
        QString label;
        QString artist;
        QString album;
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
    };

    enum Role {
        KindRole = Qt::UserRole + 1,
        RowTypeRole,
        SectionTitleRole,
        ScoreRole,
        LabelRole,
        ArtistRole,
        AlbumRole,
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
    };
    Q_ENUM(Role)

    explicit GlobalSearchResultsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void replaceRows(QVector<SearchDisplayRow> rows);

    Q_INVOKABLE QVariantMap rowDataAt(int index) const;
    Q_INVOKABLE bool isSelectableIndex(int index) const;
    Q_INVOKABLE int nextSelectableIndex(int startIndex, int step, bool wrap) const;

private:
    QVector<SearchDisplayRow> m_rows;
};
