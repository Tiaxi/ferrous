// SPDX-License-Identifier: GPL-3.0-or-later

#include "QueueSearchResultsModel.h"

#include "CoverImageProvider.h"
#include "GlobalSearchResultsModel.h"
#include "QueueRows.h"

#include <algorithm>
#include <utility>

QueueSearchResultsModel::QueueSearchResultsModel(QueueRowsModel *queue, QObject *parent)
    : QAbstractListModel(parent), m_queue(queue) {
    if (m_queue == nullptr) {
        return;
    }
    connect(m_queue, &QAbstractItemModel::rowsInserted, this,
            &QueueSearchResultsModel::queueRowsInserted);
    connect(m_queue, &QAbstractItemModel::rowsRemoved, this,
            &QueueSearchResultsModel::queueRowsRemoved);
    connect(m_queue, &QAbstractItemModel::rowsMoved, this,
            &QueueSearchResultsModel::queueRowsMoved);
    connect(m_queue, &QAbstractItemModel::dataChanged, this,
            &QueueSearchResultsModel::queueDataChanged);
    // A whole new queue: the rows found were of the old one.
    connect(m_queue, &QAbstractItemModel::modelReset, this, &QueueSearchResultsModel::clear);
}

int QueueSearchResultsModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant QueueSearchResultsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size() || m_queue == nullptr) {
        return {};
    }
    const int queueRow = m_rows.at(index.row());
    const QueueRowData *row = m_queue->rowAt(queueRow);
    if (row == nullptr) {
        return {};
    }
    switch (role) {
    case TitleRole:
        return row->title;
    case ArtistRole:
        return row->artist;
    case AlbumRole:
        return row->album;
    case GenreRole:
        return row->genre;
    case CoverUrlRole:
        // Through the image provider, which reads the file off the GUI
        // thread, as the search results' covers.
        return row->coverPath.isEmpty() ? QString()
                                        : CoverImageProvider::urlForPath(row->coverPath, row->coverStamp);
    case LengthTextRole:
        return row->lengthText;
    case PathRole:
        return row->path;
    case PlayNextRole:
        return m_queue->playNextPlace(queueRow);
    case QueueRowRole:
        return queueRow;
    default:
        return {};
    }
}

QHash<int, QByteArray> QueueSearchResultsModel::roleNames() const {
    return {
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {AlbumRole, "album"},
        {GenreRole, "genre"},
        {CoverUrlRole, "coverUrl"},
        {LengthTextRole, "lengthText"},
        {PathRole, "path"},
        {PlayNextRole, "playNext"},
        {QueueRowRole, "queueRow"},
    };
}

void QueueSearchResultsModel::setRows(QVector<int> rows) {
    const int queueLength = m_queue != nullptr ? m_queue->rowCount() : 0;
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [queueLength](int row) { return row < 0 || row >= queueLength; }),
               rows.end());
    const bool countChanges = rows.size() != m_rows.size();
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
    if (countChanges) {
        emit countChanged();
    }
}

void QueueSearchResultsModel::clear() {
    if (m_rows.isEmpty()) {
        return;
    }
    setRows({});
}

int QueueSearchResultsModel::queueRowAt(int index) const {
    return index >= 0 && index < m_rows.size() ? m_rows.at(index) : -1;
}

int QueueSearchResultsModel::indexOfQueueRow(int queueRow) const {
    const auto found = std::lower_bound(m_rows.cbegin(), m_rows.cend(), queueRow);
    return found != m_rows.cend() && *found == queueRow ? static_cast<int>(found - m_rows.cbegin())
                                                        : -1;
}

QVariantMap QueueSearchResultsModel::rowDataAt(int index) const {
    if (index < 0 || index >= m_rows.size()) {
        return {};
    }
    QVariantMap out;
    const QModelIndex at = this->index(index, 0);
    const auto names = roleNames();
    for (auto it = names.cbegin(); it != names.cend(); ++it) {
        out.insert(QString::fromUtf8(it.value()), data(at, it.key()));
    }
    return out;
}

QVariantList QueueSearchResultsModel::highlightSegments(const QString &text, const QString &query) const {
    return GlobalSearchResultsModel::segmentsOf(text, query);
}

void QueueSearchResultsModel::notifyQueueRows(int first) {
    if (first < m_rows.size()) {
        emit dataChanged(index(first, 0), index(static_cast<int>(m_rows.size()) - 1, 0),
                         {QueueRowRole, PlayNextRole});
    }
}

void QueueSearchResultsModel::queueRowsInserted(const QModelIndex &parent, int first, int last) {
    if (parent.isValid()) {
        return;
    }
    const int inserted = last - first + 1;
    const auto from = std::lower_bound(m_rows.begin(), m_rows.end(), first);
    for (auto it = from; it != m_rows.end(); ++it) {
        *it += inserted;
    }
    notifyQueueRows(static_cast<int>(from - m_rows.begin()));
}

void QueueSearchResultsModel::queueRowsRemoved(const QModelIndex &parent, int first, int last) {
    if (parent.isValid()) {
        return;
    }
    const int removed = last - first + 1;
    const auto from = std::lower_bound(m_rows.begin(), m_rows.end(), first);
    const auto to = std::upper_bound(from, m_rows.end(), last);
    const int start = static_cast<int>(from - m_rows.begin());
    const int gone = static_cast<int>(to - from);
    if (gone > 0) {
        beginRemoveRows(QModelIndex(), start, start + gone - 1);
        m_rows.erase(from, to);
        endRemoveRows();
        emit countChanged();
    }
    for (int i = start; i < m_rows.size(); ++i) {
        m_rows[i] -= removed;
    }
    notifyQueueRows(start);
}

void QueueSearchResultsModel::queueRowsMoved(const QModelIndex &parent, int start, int end,
                                             const QModelIndex &destination, int row) {
    if (parent.isValid() || destination.isValid() || m_rows.isEmpty()) {
        return;
    }
    // Qt names the row the moved ones go before, counted before the move.
    const int count = end - start + 1;
    const int to = row > start ? row - count : row;
    const auto moved = [start, end, count, to](int queueRow) {
        if (queueRow >= start && queueRow <= end) {
            return to + (queueRow - start);
        }
        const int rest = queueRow > end ? queueRow - count : queueRow;
        return rest >= to ? rest + count : rest;
    };
    // The results of the moved rows are a run of results, which moves as
    // one, as the rows did: the other results keep their order.
    const auto first = std::lower_bound(m_rows.cbegin(), m_rows.cend(), start);
    const auto last = std::upper_bound(first, m_rows.cend(), end);
    const int runStart = static_cast<int>(first - m_rows.cbegin());
    const int runCount = static_cast<int>(last - first);
    QVector<int> rows = m_rows;
    for (int &queueRow : rows) {
        queueRow = moved(queueRow);
    }
    // Where the run goes among the other results, counted without it.
    int runTo = 0;
    for (int i = 0; i < rows.size(); ++i) {
        if ((i < runStart || i >= runStart + runCount) && rows.at(i) < to) {
            ++runTo;
        }
    }
    if (runCount == 0 || runTo == runStart) {
        m_rows = std::move(rows);
        notifyQueueRows(0);
        return;
    }
    // As a model move, so that views and the selection follow each result.
    beginMoveRows(QModelIndex(), runStart, runStart + runCount - 1, QModelIndex(),
                  runTo > runStart ? runTo + runCount : runTo);
    std::sort(rows.begin(), rows.end());
    m_rows = std::move(rows);
    endMoveRows();
    notifyQueueRows(0);
}

void QueueSearchResultsModel::queueDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight,
                                               const QList<int> &roles) {
    if (m_rows.isEmpty() || !topLeft.isValid() || !bottomRight.isValid()) {
        return;
    }
    const auto from = std::lower_bound(m_rows.cbegin(), m_rows.cend(), topLeft.row());
    const auto to = std::upper_bound(from, m_rows.cend(), bottomRight.row());
    if (from == to) {
        return;
    }
    QList<int> shown;
    for (int role : roles) {
        if (role == QueueRowsModel::PlayNextRole) {
            shown.push_back(PlayNextRole);
        }
    }
    // Any other role, or all of them, may change what a result shows.
    const bool onlyPlayNext = !roles.isEmpty() && shown.size() == roles.size();
    emit dataChanged(index(static_cast<int>(from - m_rows.cbegin()), 0),
                     index(static_cast<int>(to - m_rows.cbegin()) - 1, 0),
                     onlyPlayNext ? shown : QList<int>{});
}
