// SPDX-License-Identifier: GPL-3.0-or-later

#include "GlobalSearchResultsModel.h"
#include <utility>

#include <algorithm>
#include <limits>
#include <vector>

namespace {
constexpr auto kItemKind = "item";
}

GlobalSearchResultsModel::GlobalSearchResultsModel(QObject *parent)
    : QAbstractListModel(parent) {
    m_batchTimer.setSingleShot(true);
    m_batchTimer.setInterval(0);
    connect(&m_batchTimer, &QTimer::timeout, this, &GlobalSearchResultsModel::insertNextBatch);
}

int GlobalSearchResultsModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_rows.size());
}

QVariant GlobalSearchResultsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const SearchDisplayRow &row = m_rows[static_cast<qsizetype>(index.row())];
    if (role == Qt::DisplayRole) {
        return row.label;
    }
    switch (role) {
    case MatchDetailRole:
        return row.matchDetail;
    case KindRole:
        return row.kind;
    case RowTypeRole:
        return row.rowType;
    case SectionTitleRole:
        return row.sectionTitle;
    case ScoreRole:
        if (row.kind != QLatin1String(kItemKind)) {
            return {};
        }
        return row.score;
    case LabelRole:
        return row.label;
    case ArtistRole:
        return row.artist;
    case AlbumRole:
        return row.album;
    case RootLabelRole:
        return row.rootLabel;
    case GenreRole:
        return row.genre;
    case CoverPathRole:
        return row.coverPath;
    case CoverUrlRole:
        return row.coverUrl;
    case ArtistKeyRole:
        return row.artistKey;
    case AlbumKeyRole:
        return row.albumKey;
    case SectionKeyRole:
        return row.sectionKey;
    case TrackKeyRole:
        return row.trackKey;
    case TrackPathRole:
        return row.trackPath;
    case YearRole:
        if (row.year == std::numeric_limits<int>::min()) {
            return {};
        }
        return row.year;
    case TrackNumberRole:
        if (row.trackNumber <= 0) {
            return {};
        }
        return row.trackNumber;
    case CountRole:
        if (row.kind != QLatin1String(kItemKind)) {
            return {};
        }
        return row.count;
    case LengthSecondsRole:
        if (row.kind != QLatin1String(kItemKind)) {
            return {};
        }
        return row.lengthSeconds;
    case LengthTextRole:
        return row.lengthText;
    case DelegateTypeRole:
        if (row.kind == QLatin1String("section"))
            return QStringLiteral("section");
        if (row.kind == QLatin1String("columns"))
            return QStringLiteral("columns-") + row.rowType;
        return row.rowType;
    default:
        return {};
    }
}

QHash<int, QByteArray> GlobalSearchResultsModel::roleNames() const {
    return {
        {MatchDetailRole, "matchDetail"},
        {KindRole, "kind"},
        {RowTypeRole, "rowType"},
        {SectionTitleRole, "sectionTitle"},
        {ScoreRole, "score"},
        {LabelRole, "label"},
        {ArtistRole, "artist"},
        {AlbumRole, "album"},
        {RootLabelRole, "rootLabel"},
        {GenreRole, "genre"},
        {CoverPathRole, "coverPath"},
        {CoverUrlRole, "coverUrl"},
        {ArtistKeyRole, "artistKey"},
        {AlbumKeyRole, "albumKey"},
        {SectionKeyRole, "sectionKey"},
        {TrackKeyRole, "trackKey"},
        {TrackPathRole, "trackPath"},
        {YearRole, "year"},
        {TrackNumberRole, "trackNumber"},
        {CountRole, "count"},
        {LengthSecondsRole, "lengthSeconds"},
        {LengthTextRole, "lengthText"},
        {DelegateTypeRole, "delegateType"},
    };
}

void GlobalSearchResultsModel::replaceRows(QVector<SearchDisplayRow> rows) {
    if (!m_pendingBatchRows.isEmpty()) {
        cancelBatchedInsertion();
    }
    if (m_rows.isEmpty() && rows.isEmpty()) {
        return;
    }
    if (rows.isEmpty()) {
        beginRemoveRows(
            QModelIndex{},
            0,
            static_cast<int>(m_rows.size()) - 1);
        m_rows.clear();
        endRemoveRows();
        return;
    }
    if (m_rows.isEmpty()) {
        beginInsertRows(
            QModelIndex{},
            0,
            static_cast<int>(rows.size()) - 1);
        m_rows = std::move(rows);
        endInsertRows();
        return;
    }
    if (m_rows.size() == rows.size()) {
        bool anyViewDiff = false;
        for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
            const qsizetype idx = static_cast<qsizetype>(i);
            if (!m_rows[idx].equivalentForView(rows[idx])) {
                anyViewDiff = true;
                break;
            }
        }
        if (!anyViewDiff) {
            for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
                const qsizetype idx = static_cast<qsizetype>(i);
                m_rows[idx].score = rows[idx].score;
            }
            return;
        }
    }

    if (m_rows == rows) {
        return;
    }

    const int oldSize = static_cast<int>(m_rows.size());
    const int newSize = static_cast<int>(rows.size());
    const int diff = std::abs(oldSize - newSize);
    const int overlap = std::min(oldSize, newSize);

    // For large non-empty cardinality shifts, overlap updates can trigger costly
    // delegate churn in QML. A direct rebuild has proven smoother in practice.
    const bool largeCardinalityShift =
        (oldSize > 0 && newSize > 0)
        && (diff >= 64)
        && (std::min(oldSize, newSize) <= 64);

    if (largeCardinalityShift) {
        beginRemoveRows(QModelIndex{}, 0, oldSize - 1);
        m_rows.clear();
        endRemoveRows();

        beginInsertRows(QModelIndex{}, 0, newSize - 1);
        m_rows = std::move(rows);
        endInsertRows();
        return;
    }

    // DelegateChooser selects the delegate component on rowsInserted, not on
    // dataChanged.  If an overlapping row's effective delegate type changes,
    // an incremental dataChanged would leave the old delegate in place.
    //
    // The effective delegate type is:
    //   section  → "section"  (same for all sections regardless of rowType)
    //   columns  → "columns-" + rowType
    //   item     → rowType
    //
    // We find the first row where the delegate type diverges and do a partial
    // rebuild from that point: dataChanged for rows before it (cheap — keeps
    // existing delegates), remove+insert for rows from that point onwards
    // (forces DelegateChooser to re-evaluate delegate selection).
    int firstDelegateTypeMismatch = overlap;
    for (int i = 0; i < overlap; ++i) {
        const qsizetype idx = static_cast<qsizetype>(i);
        const auto &oldRow = m_rows[idx];
        const auto &newRow = rows[idx];
        // Section delegates are type-agnostic — only kind matters, not rowType.
        if (oldRow.kind == newRow.kind
            && (oldRow.kind == QLatin1String("section")
                || oldRow.rowType == newRow.rowType)) {
            continue;
        }
        firstDelegateTypeMismatch = i;
        break;
    }

    if (firstDelegateTypeMismatch < overlap) {
        // Update rows before the mismatch with dataChanged (keeps delegates).
        std::vector<QPair<int, int>> earlyChangedRuns;
        int earlyRunStart = -1;
        for (int i = 0; i < firstDelegateTypeMismatch; ++i) {
            const qsizetype idx = static_cast<qsizetype>(i);
            if (m_rows[idx].equivalentForView(rows[idx])) {
                m_rows[idx].score = rows[idx].score;
                if (earlyRunStart >= 0) {
                    earlyChangedRuns.emplace_back(earlyRunStart, i - 1);
                    earlyRunStart = -1;
                }
            } else {
                m_rows[idx] = rows[idx];
                if (earlyRunStart < 0) {
                    earlyRunStart = i;
                }
            }
        }
        if (earlyRunStart >= 0) {
            earlyChangedRuns.emplace_back(earlyRunStart, firstDelegateTypeMismatch - 1);
        }
        for (const auto &run : earlyChangedRuns) {
            emit dataChanged(index(run.first, 0), index(run.second, 0));
        }

        // Remove everything from the mismatch point onwards.
        if (oldSize > firstDelegateTypeMismatch) {
            beginRemoveRows(QModelIndex{}, firstDelegateTypeMismatch, oldSize - 1);
            m_rows.resize(firstDelegateTypeMismatch);
            endRemoveRows();
        }

        // Insert new rows from the mismatch point onwards.
        if (newSize > firstDelegateTypeMismatch) {
            beginInsertRows(QModelIndex{}, firstDelegateTypeMismatch, newSize - 1);
            m_rows.reserve(newSize);
            for (int i = firstDelegateTypeMismatch; i < newSize; ++i) {
                m_rows.push_back(std::move(rows[static_cast<qsizetype>(i)]));
            }
            endInsertRows();
        }
        return;
    }

    if (newSize < oldSize) {
        beginRemoveRows(QModelIndex{}, newSize, oldSize - 1);
        m_rows.resize(newSize);
        endRemoveRows();
    }

    std::vector<QPair<int, int>> changedRuns;
    int runStart = -1;
    for (int i = 0; i < overlap; ++i) {
        const qsizetype idx = static_cast<qsizetype>(i);
        if (m_rows[idx].equivalentForView(rows[idx])) {
            m_rows[idx].score = rows[idx].score;
            if (runStart >= 0) {
                changedRuns.emplace_back(runStart, i - 1);
                runStart = -1;
            }
            continue;
        }
        m_rows[idx] = rows[idx];
        if (runStart < 0) {
            runStart = i;
        }
    }
    if (runStart >= 0) {
        changedRuns.emplace_back(runStart, overlap - 1);
    }
    for (const auto &run : changedRuns) {
        emit dataChanged(index(run.first, 0), index(run.second, 0));
    }

    if (newSize > oldSize) {
        beginInsertRows(QModelIndex{}, oldSize, newSize - 1);
        m_rows.reserve(newSize);
        for (int i = oldSize; i < newSize; ++i) {
            m_rows.push_back(rows[static_cast<qsizetype>(i)]);
        }
        endInsertRows();
    }
}

void GlobalSearchResultsModel::replaceRowsBatched(QVector<SearchDisplayRow> rows, int batchSize) {
    cancelBatchedInsertion();
    // Only batch on cold start (empty model) with enough rows to justify it.
    // When already populated, replaceRows uses efficient diff-based updates.
    if (!m_rows.isEmpty() || rows.size() <= batchSize) {
        replaceRows(std::move(rows));
        return;
    }
    m_pendingBatchRows = std::move(rows);
    m_batchInsertOffset = 0;
    m_batchSize = batchSize;
    insertNextBatch();
}

void GlobalSearchResultsModel::insertNextBatch() {
    if (m_batchInsertOffset >= m_pendingBatchRows.size()) {
        m_pendingBatchRows.clear();
        m_pendingBatchRows.squeeze();
        return;
    }
    const int start = m_batchInsertOffset;
    const int end = std::min(start + m_batchSize,
                             static_cast<int>(m_pendingBatchRows.size()));
    beginInsertRows(QModelIndex{}, start, end - 1);
    m_rows.reserve(m_pendingBatchRows.size());
    for (int i = start; i < end; ++i) {
        m_rows.push_back(std::move(m_pendingBatchRows[static_cast<qsizetype>(i)]));
    }
    endInsertRows();
    m_batchInsertOffset = end;
    if (m_batchInsertOffset < m_pendingBatchRows.size()) {
        m_batchTimer.start();
    } else {
        m_pendingBatchRows.clear();
        m_pendingBatchRows.squeeze();
    }
}

void GlobalSearchResultsModel::cancelBatchedInsertion() {
    m_batchTimer.stop();
    m_pendingBatchRows.clear();
    m_pendingBatchRows.squeeze();
    m_batchInsertOffset = 0;
}

QVariantMap GlobalSearchResultsModel::rowDataAt(int index) const {
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const SearchDisplayRow &row = m_rows[static_cast<qsizetype>(index)];
    QVariantMap out;
    if (!row.kind.isEmpty()) {
        out.insert(QStringLiteral("matchDetail"), row.matchDetail);
        out.insert(QStringLiteral("kind"), row.kind);
    }
    if (!row.rowType.isEmpty()) {
        out.insert(QStringLiteral("rowType"), row.rowType);
    }
    if (!row.sectionTitle.isEmpty()) {
        out.insert(QStringLiteral("sectionTitle"), row.sectionTitle);
    }
    if (row.kind != QLatin1String(kItemKind)) {
        return out;
    }

    out.insert(QStringLiteral("score"), row.score);
    out.insert(QStringLiteral("label"), row.label);
    out.insert(QStringLiteral("artist"), row.artist);
    out.insert(QStringLiteral("album"), row.album);
    out.insert(QStringLiteral("rootLabel"), row.rootLabel);
    out.insert(QStringLiteral("genre"), row.genre);
    out.insert(QStringLiteral("count"), row.count);
    out.insert(QStringLiteral("coverPath"), row.coverPath);
    out.insert(QStringLiteral("coverUrl"), row.coverUrl);
    out.insert(QStringLiteral("artistKey"), row.artistKey);
    out.insert(QStringLiteral("albumKey"), row.albumKey);
    out.insert(QStringLiteral("sectionKey"), row.sectionKey);
    out.insert(QStringLiteral("trackKey"), row.trackKey);
    out.insert(QStringLiteral("trackPath"), row.trackPath);
    if (row.year != std::numeric_limits<int>::min()) {
        out.insert(QStringLiteral("year"), row.year);
    }
    if (row.trackNumber > 0) {
        out.insert(QStringLiteral("trackNumber"), row.trackNumber);
    }
    out.insert(QStringLiteral("lengthSeconds"), row.lengthSeconds);
    out.insert(QStringLiteral("lengthText"), row.lengthText);
    return out;
}

bool GlobalSearchResultsModel::isSelectableIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(m_rows.size())) {
        return false;
    }
    const SearchDisplayRow &row = m_rows[static_cast<qsizetype>(index)];
    return row.kind == QLatin1String(kItemKind);
}

int GlobalSearchResultsModel::nextSelectableIndex(int startIndex, int step, bool wrap) const {
    if (m_rows.isEmpty()) {
        return -1;
    }
    const int direction = step < 0 ? -1 : 1;
    int index = startIndex;
    for (int i = 0; i < static_cast<int>(m_rows.size()); ++i) {
        index += direction;
        if (index < 0 || index >= static_cast<int>(m_rows.size())) {
            if (!wrap) {
                return -1;
            }
            index = direction > 0 ? 0 : static_cast<int>(m_rows.size()) - 1;
        }
        if (isSelectableIndex(index)) {
            return index;
        }
    }
    return -1;
}

void GlobalSearchResultsModel::presentSearchRows(QVector<SearchDisplayRow> rows) {
    m_sourceSearchRows.clear();
    for (auto &row : rows) {
        if (row.kind == QStringLiteral("item")) m_sourceSearchRows.push_back(std::move(row));
    }
    rebuildSearchRows();
}

void GlobalSearchResultsModel::setResultFilter(const QString &filter) {
    if (filter == m_resultFilter || !QStringList{"all", "artist", "album", "track"}.contains(filter)) return;
    m_resultFilter = filter;
    rebuildSearchRows();
}

void GlobalSearchResultsModel::rebuildSearchRows() {
    QVector<SearchDisplayRow> candidates;
    for (const auto &row : m_sourceSearchRows) {
        if (m_resultFilter == QStringLiteral("all") || row.rowType == m_resultFilter) candidates.push_back(row);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b) {
        if (a.score != b.score) return a.score < b.score;
        // Prefer an exact track when it shares a name with its parent entities.
        if (a.rowType != b.rowType) return a.rowType == "track" || (a.rowType == "album" && b.rowType == "artist");
        return QString::compare(a.label, b.label, Qt::CaseInsensitive) < 0;
    });
    QVector<SearchDisplayRow> display;
    const auto section = [&display](const QString &title) {
        SearchDisplayRow header;
        header.kind = QStringLiteral("section");
        header.sectionTitle = title;
        display.push_back(std::move(header));
    };
    int bestCount = 0;
    if (m_resultFilter == QStringLiteral("all") && !candidates.isEmpty()) {
        section(QStringLiteral("Best matches"));
        bestCount = std::min(3, static_cast<int>(candidates.size()));
        for (int i = 0; i < bestCount; ++i) display.push_back(candidates[i]);
    }
    for (const auto &type : {QStringLiteral("artist"), QStringLiteral("album"), QStringLiteral("track")}) {
        bool started = false;
        for (qsizetype i = bestCount; i < candidates.size(); ++i) {
            if (candidates[i].rowType != type) continue;
            if (!started) {
                section(type == "artist" ? QStringLiteral("Artists") : type == "album" ? QStringLiteral("Albums") : QStringLiteral("Tracks"));
                started = true;
            }
            display.push_back(candidates[i]);
        }
    }
    replaceRowsBatched(std::move(display));
    emit searchRowsChanged();
}

QString GlobalSearchResultsModel::highlightText(const QString &text, const QString &query) const {
    const auto fold = [](const QString &value) {
        QString result;
        for (auto ch : value.normalized(QString::NormalizationForm_KD).toCaseFolded()) {
            if (ch.category() != QChar::Mark_NonSpacing && ch.category() != QChar::Mark_SpacingCombining
                && ch.category() != QChar::Mark_Enclosing) result += ch;
        }
        return result;
    };
    QString normalized;
    QVector<qsizetype> positions;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const auto fragment = fold(text.mid(i, 1));
        normalized += fragment;
        for (qsizetype j = 0; j < fragment.size(); ++j) positions.push_back(i);
    }
    QVector<bool> matched(text.size(), false);
    QStringList tokens;
    QString token;
    bool quoted = false;
    for (auto ch : query) {
        if (ch == '"') quoted = !quoted;
        else if (ch.isSpace() && !quoted) {
            if (!token.isEmpty()) tokens.push_back(std::exchange(token, QString{}));
        } else token += ch;
    }
    if (!token.isEmpty()) tokens.push_back(token);
    static const QStringList fields{"title", "artist", "album", "albumartist", "album_artist", "album-artist",
        "genre", "year", "date", "composer", "conductor", "performer", "label", "publisher", "comment", "lyrics",
        "path", "filename", "root", "track", "disc"};
    for (auto rawTerm : tokens) {
        const auto colon = rawTerm.indexOf(':');
        if (colon > 0 && fields.contains(rawTerm.left(colon).toLower())) rawTerm = rawTerm.mid(colon + 1);
        const auto term = fold(rawTerm.trimmed());
        if (term.isEmpty()) continue;
        qsizetype offset = 0;
        while ((offset = normalized.indexOf(term, offset)) >= 0) {
            const auto end = offset + term.size();
            const auto originalEnd = end < positions.size() ? positions[end] : text.size();
            for (auto i = positions[offset]; i < originalEnd; ++i) matched[i] = true;
            offset = end;
        }
    }
    QString result;
    bool bold = false;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (matched[i] != bold) { bold = matched[i]; result += bold ? "<b>" : "</b>"; }
        result += text.mid(i, 1).toHtmlEscaped();
    }
    if (bold) result += "</b>";
    return result;
}
