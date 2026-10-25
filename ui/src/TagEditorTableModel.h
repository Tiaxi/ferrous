// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

struct TagEditorRowState {
    QString path;
    QString fileName;
    QString directory;
    QString formatKind;
    QString title;
    QString artist;
    QString album;
    QString albumArtist;
    QString genre;
    QString year;
    QString trackNo;
    QString discNo;
    QString totalTracks;
    QString totalDiscs;
    QString comment;
    QString originalTitle;
    QString originalArtist;
    QString originalAlbum;
    QString originalAlbumArtist;
    QString originalGenre;
    QString originalYear;
    QString originalTrackNo;
    QString originalDiscNo;
    QString originalTotalTracks;
    QString originalTotalDiscs;
    QString originalComment;
    bool dirty{false};
    bool selected{false};
    QString errorText;
    // The values after the first of fields that hold several, by field name.
    // Saving a changed field replaces them all with its one value.
    QHash<QString, QStringList> extraValues;
};

class TagEditorTableModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        PathRole = Qt::UserRole + 1,
        FileNameRole,
        DirectoryRole,
        FormatKindRole,
        TitleRole,
        ArtistRole,
        AlbumRole,
        AlbumArtistRole,
        GenreRole,
        YearRole,
        TrackNoRole,
        DiscNoRole,
        TotalTracksRole,
        TotalDiscsRole,
        CommentRole,
        DirtyRole,
        ErrorRole,
        SelectedRole,
        ExtraValuesRole,
    };

    explicit TagEditorTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRows(QVector<TagEditorRowState> rows);
    void clear();
    const TagEditorRowState *rowAt(int row) const;
    TagEditorRowState *rowAt(int row);
    bool setFieldValue(int row, const QString &field, const QString &value);
    QString fieldValue(int row, const QString &field) const;
    QStringList loadedPaths() const;
    bool hasDirtyRows() const;
    // Marks exactly `rows` selected, notifying only the rows that change.
    void setSelection(const QSet<int> &rows);
    // The extra values of `field` that a save would keep, which are those of
    // an unchanged field.
    static QStringList keptExtraValues(const TagEditorRowState &row, const QString &field);
    QVector<TagEditorRowState> rows() const;
    // Request names of the fields that differ from their loaded values.
    static QStringList changedFields(const TagEditorRowState &row);
    void clearErrors();
    void applySaveResults(const QHash<QString, QString> &errorsByPath, const QSet<QString> &successPaths);
    // Moves renamed rows to their new paths and marks the rows that failed,
    // in place, so the view keeps its rows and position.
    void applyRenameResults(const QHash<QString, QString> &newPathsByPath,
                            const QHash<QString, QString> &errorsByPath);

signals:
    void rowsChanged();
    void dirtyStateChanged();

private:
    QVariant fieldData(const TagEditorRowState &row, int role) const;
    static QString normalizedFieldName(const QString &field);
    static QString *fieldRef(TagEditorRowState &row, const QString &field);
    static const QString *fieldRef(const TagEditorRowState &row, const QString &field);
    bool refreshDirtyState(TagEditorRowState &row);
    void emitRowChanged(int row);
    void countDirtyRows();

    QVector<TagEditorRowState> m_rows;
    int m_dirtyRowCount{0};
};
