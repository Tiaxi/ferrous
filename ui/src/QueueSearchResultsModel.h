// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QPointer>
#include <QVariantMap>
#include <QVector>

class QueueRowsModel;

// The rows of the queue Find in Queue found, in queue order: a view of the
// queue's own rows, which shows what they show. A row found keeps its place
// in the results while the queue changes under it, and a row the queue
// loses leaves them, until the next results replace them; rows the queue
// gains are not results until a search finds them.
class QueueSearchResultsModel final : public QAbstractListModel {
    Q_OBJECT
    // The rows, notified as soon as they are inserted or removed, as
    // GlobalSearchResultsModel's.
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        AlbumRole,
        GenreRole,
        CoverUrlRole,
        LengthTextRole,
        PathRole,
        // The row's place among the rows marked to play next, from 1; 0 for
        // a row not marked.
        PlayNextRole,
        // The row's index in the queue.
        QueueRowRole,
    };
    Q_ENUM(Role)

    explicit QueueSearchResultsModel(QueueRowsModel *queue, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Show the queue's `rows`, ascending, in place of the results, with a
    // model reset. Rows the queue does not have are left out.
    void setRows(QVector<int> rows);
    void clear();
    int count() const { return static_cast<int>(m_rows.size()); }

    // The queue row of the result at `index`, or -1.
    Q_INVOKABLE int queueRowAt(int index) const;
    // The result of queue row `queueRow`, or -1.
    Q_INVOKABLE int indexOfQueueRow(int queueRow) const;
    // The roles of the result at `index` by name; empty for none.
    Q_INVOKABLE QVariantMap rowDataAt(int index) const;
    // `text` cut where the matches of `query` start and end, as global
    // search's results cut theirs: a list of {text, match} maps.
    Q_INVOKABLE QVariantList highlightSegments(const QString &text, const QString &query) const;

signals:
    void countChanged();

private:
    void queueRowsInserted(const QModelIndex &parent, int first, int last);
    void queueRowsRemoved(const QModelIndex &parent, int first, int last);
    void queueRowsMoved(const QModelIndex &parent, int start, int end, const QModelIndex &destination,
                        int row);
    void queueDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                          const QList<int> &roles);
    // The results from `first` on, whose queue rows changed, are told.
    void notifyQueueRows(int first);

    QPointer<QueueRowsModel> m_queue;
    // The queue row of each result, ascending.
    QVector<int> m_rows;
};
