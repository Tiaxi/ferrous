#pragma once

#include <QAbstractListModel>
#include <QVariantMap>
#include <QVector>

class GlobalSearchResultsModel : public QAbstractListModel {
    Q_OBJECT

public:
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

    void replaceRows(QVector<QVariantMap> rows);

    Q_INVOKABLE QVariantMap rowDataAt(int index) const;
    Q_INVOKABLE bool isSelectableIndex(int index) const;
    Q_INVOKABLE int nextSelectableIndex(int startIndex, int step, bool wrap) const;

private:
    static QString roleKeyForRole(int role);
    QVector<QVariantMap> m_rows;
};
