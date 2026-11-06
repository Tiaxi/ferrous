// SPDX-License-Identifier: GPL-3.0-or-later

#include "QueueRows.h"

#include <QChar>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

QString formatQueueDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return QStringLiteral("--:--");
    }
    const int total = static_cast<int>(seconds + 0.5);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(secs, 2, 10, QChar('0'));
}

QueueRowData queueRowFromTrack(const BinaryBridgeCodec::DecodedQueueTrack &track) {
    QueueRowData row;
    row.title = track.title.trimmed().isEmpty() ? track.path : track.title;
    row.artist = track.artist;
    row.album = track.album;
    row.coverPath = track.coverPath;
    row.coverStamp = track.coverStamp;
    row.genre = track.genre;
    row.lengthText = track.lengthSeconds >= 0.0f
        ? formatQueueDuration(static_cast<double>(track.lengthSeconds))
        : QStringLiteral("--:--");
    row.path = track.path;
    row.trackNumber = track.trackNumber;
    row.year = track.year;
    return row;
}

QVector<QueueRowData> queueRowsFromTracks(
    const QVector<BinaryBridgeCodec::DecodedQueueTrack> &tracks) {
    QVector<QueueRowData> rows;
    rows.reserve(tracks.size());
    for (const auto &track : tracks) {
        rows.push_back(queueRowFromTrack(track));
    }
    return rows;
}

PreparedQueueRows prepareQueueRows(BinaryBridgeCodec::DecodedQueue &queue) {
    PreparedQueueRows prepared;
    prepared.prepared = true;
    if (!queue.present) {
        return prepared;
    }
    prepared.rows = queueRowsFromTracks(queue.tracks);
    queue.tracks = {};
    prepared.opRows.resize(queue.ops.size());
    for (qsizetype i = 0; i < queue.ops.size(); ++i) {
        BinaryBridgeCodec::DecodedQueueOp &op = queue.ops[i];
        prepared.opRows[i] = queueRowsFromTracks(op.tracks);
        op.tracks = {};
    }
    return prepared;
}

QueueRowsModel::QueueRowsModel(QObject *parent)
    : QAbstractListModel(parent) {}

int QueueRowsModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant QueueRowsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }

    const QueueRowData &row = m_rows.at(index.row());
    switch (role) {
    case TitleRole:
        return row.title;
    case ArtistRole:
        return row.artist;
    case AlbumRole:
        return row.album;
    case CoverPathRole:
        return row.coverPath;
    case GenreRole:
        return row.genre;
    case LengthTextRole:
        return row.lengthText;
    case PathRole:
        return row.path;
    case TrackNumberRole:
        return row.trackNumber > 0 ? QVariant(row.trackNumber) : QVariant{};
    case YearRole:
        return row.year != std::numeric_limits<int>::min() ? QVariant(row.year) : QVariant{};
    default:
        return {};
    }
}

QHash<int, QByteArray> QueueRowsModel::roleNames() const {
    return {
        {TitleRole, "title"},
        {ArtistRole, "artist"},
        {AlbumRole, "album"},
        {CoverPathRole, "coverPath"},
        {GenreRole, "genre"},
        {LengthTextRole, "lengthText"},
        {PathRole, "path"},
        {TrackNumberRole, "trackNumber"},
        {YearRole, "year"},
    };
}

void QueueRowsModel::setRowReleaser(RowReleaser releaser) {
    m_releaser = std::move(releaser);
}

void QueueRowsModel::release(QVector<QueueRowData> &&rows) {
    if (m_releaser && rows.size() >= kReleaseThreshold) {
        m_releaser(std::move(rows));
        return;
    }
    rows = QVector<QueueRowData>();
}

void QueueRowsModel::countPath(const QString &path, int step) {
    auto it = m_pathCounts.find(path);
    if (it == m_pathCounts.end()) {
        if (step > 0) {
            m_pathCounts.insert(path, step);
        }
        return;
    }
    *it += step;
    if (*it <= 0) {
        m_pathCounts.erase(it);
    }
}

void QueueRowsModel::countPaths(const QVector<QueueRowData> &rows, qsizetype from, qsizetype count,
                                int step) {
    for (qsizetype i = from; i < from + count; ++i) {
        countPath(rows.at(i).path, step);
    }
}

bool QueueRowsModel::containsPath(const QString &path) const {
    return m_pathCounts.contains(path);
}

bool QueueRowsModel::setRows(QVector<QueueRowData> rows) {
    if (m_rows == rows) {
        release(std::move(rows));
        return false;
    }
    beginResetModel();
    std::swap(m_rows, rows);
    m_pathCounts.clear();
    m_pathCounts.reserve(m_rows.size());
    countPaths(m_rows, 0, m_rows.size(), 1);
    endResetModel();
    // The rows the model had.
    release(std::move(rows));
    return true;
}

bool QueueRowsModel::insertRowsAt(int at, QVector<QueueRowData> rows) {
    if (at < 0 || at > m_rows.size()) {
        return false;
    }
    if (rows.isEmpty()) {
        return true;
    }
    beginInsertRows(QModelIndex(), at, at + static_cast<int>(rows.size()) - 1);
    countPaths(rows, 0, rows.size(), 1);
    if (m_rows.isEmpty()) {
        std::swap(m_rows, rows);
    } else {
        m_rows.insert(at, rows.size(), QueueRowData{});
        std::move(rows.begin(), rows.end(), m_rows.begin() + at);
    }
    endInsertRows();
    return true;
}

bool QueueRowsModel::removeRowsAt(int start, int count) {
    if (start < 0 || count < 0 || count > m_rows.size() - start) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    beginRemoveRows(QModelIndex(), start, start + count - 1);
    countPaths(m_rows, start, count, -1);
    QVector<QueueRowData> removed;
    if (count == m_rows.size()) {
        std::swap(removed, m_rows);
    } else if (m_releaser && count >= kReleaseThreshold) {
        // Moved out, the strings are freed with `removed`; what is left of
        // the rows is empty and cheap to drop.
        removed.reserve(count);
        std::move(m_rows.begin() + start, m_rows.begin() + start + count,
                  std::back_inserter(removed));
        m_rows.remove(start, count);
    } else {
        m_rows.remove(start, count);
    }
    endRemoveRows();
    release(std::move(removed));
    return true;
}

bool QueueRowsModel::moveRow(int from, int to) {
    if (from < 0 || to < 0 || from >= m_rows.size() || to >= m_rows.size()) {
        return false;
    }
    if (from == to) {
        return true;
    }
    // Qt names the row the moved one goes before, counted before the move.
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), to > from ? to + 1 : to)) {
        return false;
    }
    m_rows.move(from, to);
    endMoveRows();
    return true;
}

bool QueueRowsModel::moveRows(int from, int count, int to) {
    const int size = static_cast<int>(m_rows.size());
    if (from < 0 || count <= 0 || to < 0 || count > size - from || count > size - to) {
        return false;
    }
    if (from == to) {
        return true;
    }
    // Qt names the row the moved ones go before, counted before the move.
    if (!beginMoveRows(QModelIndex(), from, from + count - 1, QModelIndex(),
                       to > from ? to + count : to)) {
        return false;
    }
    QVector<QueueRowData> moved(std::make_move_iterator(m_rows.begin() + from),
                                std::make_move_iterator(m_rows.begin() + from + count));
    m_rows.remove(from, count);
    m_rows.insert(to, count, QueueRowData{});
    std::move(moved.begin(), moved.end(), m_rows.begin() + to);
    endMoveRows();
    return true;
}

bool QueueRowsModel::updateRow(int index, QueueRowData row) {
    if (index < 0 || index >= m_rows.size()) {
        return false;
    }
    if (m_rows.at(index) == row) {
        return true;
    }
    if (m_rows.at(index).path != row.path) {
        countPath(m_rows.at(index).path, -1);
        countPath(row.path, 1);
    }
    m_rows[index] = std::move(row);
    const QModelIndex changed = this->index(index, 0);
    emit dataChanged(changed, changed);
    return true;
}

const QueueRowData *QueueRowsModel::rowAt(int index) const {
    if (index < 0 || index >= m_rows.size()) {
        return nullptr;
    }
    return &m_rows.at(index);
}

QVariant QueueRowsModel::trackNumberAt(int index) const {
    const QueueRowData *row = rowAt(index);
    if (row == nullptr || row->trackNumber <= 0) {
        return {};
    }
    return row->trackNumber;
}

QueueSnapshotWorker::QueueSnapshotWorker(std::function<void()> ready)
    : m_readyCallback(std::move(ready)),
      m_thread([this]() { run(); }) {}

QueueSnapshotWorker::~QueueSnapshotWorker() {
    stop();
}

void QueueSnapshotWorker::stop() {
    std::deque<Task> dropped;
    std::deque<Result> results;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped) {
            return;
        }
        m_stopped = true;
        ++m_generation;
        m_pending = 0;
        // Rows handed over are still freed on the worker; packets are not
        // decoded any more.
        for (auto it = m_tasks.begin(); it != m_tasks.end();) {
            if (it->decode) {
                dropped.push_back(std::move(*it));
                it = m_tasks.erase(it);
            } else {
                ++it;
            }
        }
        results.swap(m_ready);
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

quint64 QueueSnapshotWorker::submit(QByteArray packet) {
    quint64 sequence = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped) {
            return 0;
        }
        sequence = ++m_lastSequence;
        m_tasks.push_back(Task{true, m_generation, sequence, std::move(packet), {}});
        ++m_pending;
    }
    m_wake.notify_one();
    return sequence;
}

void QueueSnapshotWorker::release(QVector<QueueRowData> rows) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped) {
            // Freed here, as `rows` goes.
            return;
        }
        m_tasks.push_back(Task{false, 0, 0, {}, std::move(rows)});
    }
    m_wake.notify_one();
}

std::vector<QueueSnapshotWorker::Result> QueueSnapshotWorker::takeReady() {
    std::vector<Result> taken;
    std::lock_guard<std::mutex> lock(m_mutex);
    taken.reserve(m_ready.size());
    while (!m_ready.empty()) {
        if (m_ready.front().generation == m_generation) {
            taken.push_back(std::move(m_ready.front()));
            --m_pending;
        }
        m_ready.pop_front();
    }
    return taken;
}

int QueueSnapshotWorker::pendingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending;
}

void QueueSnapshotWorker::invalidate() {
    std::deque<Task> dropped;
    std::deque<Result> results;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_generation;
        m_pending = 0;
        for (auto it = m_tasks.begin(); it != m_tasks.end();) {
            if (it->decode) {
                dropped.push_back(std::move(*it));
                it = m_tasks.erase(it);
            } else {
                ++it;
            }
        }
        results.swap(m_ready);
    }
    // Hand what was dropped to the worker too: a dropped result can hold a
    // whole queue.
    QVector<QueueRowData> rows;
    for (Result &result : results) {
        rows.append(std::move(result.rows.rows));
        for (QVector<QueueRowData> &opRows : result.rows.opRows) {
            rows.append(std::move(opRows));
        }
    }
    if (!rows.isEmpty()) {
        release(std::move(rows));
    }
}

quint64 QueueSnapshotWorker::generation() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_generation;
}

void QueueSnapshotWorker::setReleaseObserver(std::function<void(qsizetype)> observer) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_releaseObserver = std::move(observer);
}

std::thread::id QueueSnapshotWorker::threadId() const {
    return m_thread.get_id();
}

void QueueSnapshotWorker::run() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this]() { return m_stopped || !m_tasks.empty(); });
            if (m_tasks.empty()) {
                // Stopped, with nothing left to free.
                return;
            }
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }

        if (!task.decode) {
            const qsizetype count = task.rows.size();
            task.rows = {};
            std::function<void(qsizetype)> observer;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                observer = m_releaseObserver;
            }
            if (observer) {
                observer(count);
            }
            continue;
        }

        Result result;
        result.generation = task.generation;
        result.sequence = task.sequence;
        result.ok = BinaryBridgeCodec::decodeSnapshotPacket(
            task.packet, &result.snapshot, &result.error);
        task.packet = {};
        if (result.ok) {
            result.rows = prepareQueueRows(result.snapshot.queue);
        }

        bool queued = false;
        std::function<void()> ready;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_stopped && result.generation == m_generation) {
                m_ready.push_back(std::move(result));
                queued = true;
                ready = m_readyCallback;
            }
        }
        // A stale result is freed here, with `result`.
        if (queued && ready) {
            ready();
        }
    }
}
