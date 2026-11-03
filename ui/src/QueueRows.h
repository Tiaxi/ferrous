// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QString>
#include <QVector>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "BinaryBridgeCodec.h"

struct QueueRowData {
    QString title;
    QString artist;
    QString album;
    QString coverPath;
    QString genre;
    QString lengthText;
    QString path;
    int trackNumber{0};
    int year{std::numeric_limits<int>::min()};
    quint64 coverStamp{0};

    bool operator==(const QueueRowData &other) const = default;
};

// A length as the queue and the search results show it: m:ss, or h:mm:ss
// from an hour on, and --:-- when it is not known.
QString formatQueueDuration(double seconds);
QueueRowData queueRowFromTrack(const BinaryBridgeCodec::DecodedQueueTrack &track);
QVector<QueueRowData> queueRowsFromTracks(
    const QVector<BinaryBridgeCodec::DecodedQueueTrack> &tracks);

// The rows of a queue section, made ready for the model: for a whole queue
// its rows, for a delta the rows of each operation, by the operation's
// index. Rows that are not prepared are made from the section when it is
// applied.
struct PreparedQueueRows {
    bool prepared{false};
    QVector<QueueRowData> rows;
    QVector<QVector<QueueRowData>> opRows;
};

// Make the rows of `queue` and drop its decoded tracks, which the rows
// replace, so that both are made and freed where this runs.
PreparedQueueRows prepareQueueRows(BinaryBridgeCodec::DecodedQueue &queue);

class QueueRowsModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        TitleRole = Qt::UserRole + 1,
        ArtistRole,
        AlbumRole,
        CoverPathRole,
        GenreRole,
        LengthTextRole,
        PathRole,
        TrackNumberRole,
        YearRole,
    };

    // Takes rows the model no longer has, to free them elsewhere.
    using RowReleaser = std::function<void(QVector<QueueRowData> &&)>;
    // From how many rows on the releaser frees them; fewer are freed here.
    static constexpr qsizetype kReleaseThreshold = 512;

    explicit QueueRowsModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Replace every row, with a model reset.
    bool setRows(QVector<QueueRowData> rows);
    // The row edits of a queue delta, each with its row signals. Each
    // returns false, changing nothing, if it does not fit the rows.
    bool insertRowsAt(int at, QVector<QueueRowData> rows);
    bool removeRowsAt(int start, int count);
    // Take the row at `from` out and put it back at `to`.
    bool moveRow(int from, int to);
    // Take the `count` rows from `from` on out and put them back, in their
    // order, from `to` on.
    bool moveRows(int from, int count, int to);
    bool updateRow(int index, QueueRowData row);
    const QueueRowData *rowAt(int index) const;
    QVariant trackNumberAt(int index) const;

    // Hand rows the model drops, from kReleaseThreshold on, to `releaser`
    // instead of freeing them on the model's thread. None frees them here.
    void setRowReleaser(RowReleaser releaser);

private:
    void release(QVector<QueueRowData> &&rows);

    QVector<QueueRowData> m_rows;
    RowReleaser m_releaser;
};

// Decodes large snapshot packets and frees dropped queue rows on a thread of
// its own, so that the GUI thread does neither for a queue of thousands of
// tracks. The decoded snapshots come out in the order their packets went in.
class QueueSnapshotWorker {
public:
    struct Result {
        // The generation the packet was submitted in.
        quint64 generation{0};
        bool ok{false};
        QString error;
        BinaryBridgeCodec::DecodedSnapshot snapshot;
        PreparedQueueRows rows;
    };

    // Snapshot packets from this size on are worth decoding here: about a
    // thousand queue rows, a millisecond or so of decoding.
    static constexpr qsizetype kMinPacketBytes = 128 * 1024;

    // `ready` runs on the worker's thread after a result is ready; it may
    // only post to another thread.
    explicit QueueSnapshotWorker(std::function<void()> ready);
    ~QueueSnapshotWorker();
    QueueSnapshotWorker(const QueueSnapshotWorker &) = delete;
    QueueSnapshotWorker &operator=(const QueueSnapshotWorker &) = delete;

    // Stops the thread, dropping the packets not decoded yet, freeing the
    // rows handed to it, and calling `ready` no more. Later rows are freed
    // where they are released and later packets dropped.
    void stop();
    void submit(QByteArray packet);
    // Free `rows` on the worker's thread.
    void release(QVector<QueueRowData> rows);
    // The results ready, oldest first, of packets submitted since the last
    // invalidate().
    std::vector<Result> takeReady();
    // Packets submitted since the last invalidate() whose results were not
    // taken yet.
    int pendingCount() const;
    // Forget every packet submitted so far: results not taken yet, and those
    // still being decoded, are dropped.
    void invalidate();
    quint64 generation() const;

    // Tests: called on the worker's thread after it freed `count` rows.
    void setReleaseObserver(std::function<void(qsizetype count)> observer);
    std::thread::id threadId() const;

private:
    struct Task {
        bool decode{false};
        quint64 generation{0};
        QByteArray packet;
        QVector<QueueRowData> rows;
    };

    void run();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Task> m_tasks;
    std::deque<Result> m_ready;
    quint64 m_generation{1};
    int m_pending{0};
    bool m_stopped{false};
    std::function<void()> m_readyCallback;
    std::function<void(qsizetype)> m_releaseObserver;
    std::thread m_thread;
};
