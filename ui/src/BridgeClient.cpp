#include "BridgeClient.h"

#include "DiagnosticsLog.h"
#include "FerrousBridgeFfi.h"
#include "SpectrogramSeekTrace.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageIOHandler>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QPointer>
#include <QNetworkRequest>
#include <QProcess>
#include <QSet>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
#define FERROUS_PROFILE_LOG_DIAGNOSTIC(category, message) logDiagnostic(category, message)
#else
#define FERROUS_PROFILE_LOG_DIAGNOSTIC(category, message) \
    do {                                                  \
    } while (false)
#endif

namespace {
constexpr quint8 kAnalysisFrameMagic = 0xA1;
constexpr quint8 kAnalysisFlagWaveform = 0x01;
constexpr quint8 kAnalysisFlagReset = 0x02;
constexpr quint8 kAnalysisFlagSpectrogram = 0x04;
constexpr quint8 kAnalysisFlagWaveformComplete = 0x08;
constexpr quint32 kMaxAnalysisFrameBytes = 8 * 1024 * 1024;
constexpr int kMaxDiagnosticsLines = 2000;
constexpr int kItunesArtworkSearchRequestLimit = 50;
constexpr int kItunesArtworkResultDisplayLimit = 40;

bool shouldEmitUiProfileLog(qint64 nowMs, qint64 *lastMs, qint64 minIntervalMs = 250) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (lastMs == nullptr) {
        return true;
    }
    if (*lastMs != 0 && (nowMs - *lastMs) < minIntervalMs) {
        return false;
    }
    *lastMs = nowMs;
    return true;
#else
    (void)nowMs;
    (void)lastMs;
    (void)minIntervalMs;
    return false;
#endif
}

bool isNewerSeq(quint32 seq, quint32 last) {
    return static_cast<qint32>(seq - last) > 0;
}

QString normalizeLocalPathArg(const QString &path) {
    QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    if (trimmed.startsWith(QStringLiteral("QUrl(\"")) && trimmed.endsWith(QStringLiteral("\")"))) {
        trimmed = trimmed.mid(6, trimmed.size() - 8);
    }

    const QUrl asUrl(trimmed);
    if (asUrl.isValid() && asUrl.isLocalFile()) {
        const QString localPath = asUrl.toLocalFile().trimmed();
        if (!localPath.isEmpty()) {
            return localPath;
        }
    }

    if (trimmed.startsWith(QStringLiteral("file://"))) {
        const QString localPath = QUrl(trimmed).toLocalFile().trimmed();
        if (!localPath.isEmpty()) {
            return localPath;
        }
    }

    return trimmed;
}

QString normalizeRootNameArg(const QString &name) {
    return name.trimmed();
}

QString rootSearchLabel(const QString &path, const QString &name) {
    const QString trimmedName = normalizeRootNameArg(name);
    if (!trimmedName.isEmpty()) {
        return trimmedName;
    }
    const QFileInfo info(path);
    const QString base = info.fileName().trimmed();
    return base.isEmpty() ? path : base;
}

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
QString playbackLogPathField(const QString &path) {
    const QString trimmed = path.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("<none>") : trimmed;
}
#endif

QString formatBinaryFileSize(qint64 sizeBytes) {
    if (sizeBytes < 0) {
        return {};
    }

    static constexpr const char *kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double scaled = static_cast<double>(sizeBytes);
    int unitIndex = 0;
    while (scaled >= 1024.0 && unitIndex < 4) {
        scaled /= 1024.0;
        ++unitIndex;
    }

    const QLocale locale = QLocale::system();
    if (unitIndex == 0) {
        return QStringLiteral("%1 B").arg(locale.toString(sizeBytes));
    }

    const int precision = scaled >= 10.0 ? 0 : 1;
    return QStringLiteral("%1 %2 (%3 bytes)")
        .arg(locale.toString(scaled, 'f', precision),
             QString::fromLatin1(kUnits[unitIndex]),
             locale.toString(sizeBytes));
}

QString canonicalizeSearchQuery(const QString &query) {
    // Keep UI and backend semantics aligned: backend splits on whitespace.
    // Canonicalization avoids duplicate sends from trailing/multiple spaces.
    return query.simplified();
}

QString searchCoverUrlFast(const QString &path) {
    if (path.isEmpty()) {
        return {};
    }
    if (path.startsWith(QStringLiteral("qrc:/")) || path.startsWith(QStringLiteral(":/"))) {
        return path;
    }
    if (path.startsWith(QStringLiteral("file://"))) {
        return path;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    }

    const QString canonicalPath = info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath()
        : info.canonicalFilePath();
    QUrl coverUrl = QUrl::fromLocalFile(canonicalPath);
    coverUrl.setFragment(
        QStringLiteral("v=%1").arg(info.lastModified().toMSecsSinceEpoch()));
    return coverUrl.toString(QUrl::FullyEncoded);
}

QString cacheOnlyLocalFileUrl(const QString &path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    if (trimmed.startsWith(QStringLiteral("qrc:/")) || trimmed.startsWith(QStringLiteral(":/"))) {
        return trimmed;
    }
    if (trimmed.startsWith(QStringLiteral("file://"))) {
        return trimmed;
    }

    const QUrl url(trimmed);
    if (url.isValid() && !url.scheme().isEmpty() && !url.isLocalFile()) {
        return trimmed;
    }

    const QString localPath = normalizeLocalPathArg(trimmed);
    if (localPath.isEmpty()) {
        return {};
    }
    return QUrl::fromLocalFile(localPath).toString(QUrl::FullyEncoded);
}

QString normalizedItunesMatchKey(const QString &value) {
    return value.simplified().toCaseFolded();
}

int itunesMatchRankGroup(
    const QString &candidateAlbum,
    const QString &candidateArtist,
    const QString &wantedAlbum,
    const QString &wantedArtist)
{
    const QString album = normalizedItunesMatchKey(candidateAlbum);
    const QString artist = normalizedItunesMatchKey(candidateArtist);
    const QString wantedAlbumKey = normalizedItunesMatchKey(wantedAlbum);
    const QString wantedArtistKey = normalizedItunesMatchKey(wantedArtist);

    const bool albumExact = !album.isEmpty() && album == wantedAlbumKey;
    const bool artistExact = !artist.isEmpty() && artist == wantedArtistKey;
    const bool albumPartial = !wantedAlbumKey.isEmpty() && album.contains(wantedAlbumKey);
    const bool artistPartial = !wantedArtistKey.isEmpty() && artist.contains(wantedArtistKey);

    if (albumExact && artistExact) {
        return 0;
    }
    if (albumExact) {
        return 1;
    }
    if (artistExact) {
        return 2;
    }
    if (albumPartial || artistPartial) {
        return 3;
    }
    return 4;
}

QString replaceUrlTerminalExtension(const QString &urlString, const QString &extension) {
    const QString trimmedExtension = extension.trimmed().toLower();
    if (trimmedExtension.isEmpty()) {
        return {};
    }

    QUrl url(urlString);
    QString path = url.path();
    const int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    if (lastSlash < 0) {
        return {};
    }
    const int lastDot = path.lastIndexOf(QLatin1Char('.'));
    if (lastDot <= lastSlash) {
        return {};
    }

    path = path.left(lastDot + 1) + trimmedExtension;
    url.setPath(path);
    return url.toString(QUrl::FullyEncoded);
}

QString sourceArtworkExtension(const QUrl &url) {
    const QString path = url.path();
    const int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    if (lastSlash <= 0) {
        return {};
    }
    const int previousSlash = path.lastIndexOf(QLatin1Char('/'), lastSlash - 1);
    const QString sourceSegment = path.mid(previousSlash + 1, lastSlash - previousSlash - 1);
    const QString suffix = QFileInfo(sourceSegment).suffix().trimmed().toLower();
    if (suffix == QStringLiteral("jpeg")) {
        return QStringLiteral("jpg");
    }
    if (suffix == QStringLiteral("tiff")) {
        return QStringLiteral("tif");
    }
    return suffix;
}

QStringList deriveItunesArtworkUrls(const QString &artworkUrl100) {
    QString highRes = artworkUrl100.trimmed();
    if (highRes.isEmpty()) {
        return {};
    }
    highRes.replace(QStringLiteral("100x100bb"), QStringLiteral("100000x100000-999"));

    QStringList urls;
    auto appendUnique = [&urls](const QString &candidate) {
        const QString trimmed = candidate.trimmed();
        if (!trimmed.isEmpty() && !urls.contains(trimmed)) {
            urls.push_back(trimmed);
        }
    };

    const QUrl highResUrl(highRes);
    const QString path = highResUrl.path();
    const QString sourceExt = sourceArtworkExtension(highResUrl);
    const QString terminalExt = QFileInfo(path).suffix().trimmed().toLower();
    const QString thumbMarker = QStringLiteral("/image/thumb/");
    const int thumbPos = path.indexOf(thumbMarker);

    if (thumbPos >= 0) {
        const QString afterThumb = path.mid(thumbPos + thumbMarker.size());
        const int lastSlash = afterThumb.lastIndexOf(QLatin1Char('/'));
        if (lastSlash > 0) {
            appendUnique(QStringLiteral("https://a5.mzstatic.com/us/r1000/0/") + afterThumb.left(lastSlash));
        }
    }

    appendUnique(highRes);

    if (!sourceExt.isEmpty() && sourceExt != terminalExt) {
        appendUnique(replaceUrlTerminalExtension(highRes, sourceExt));
    }

    const QString highResFallback =
        QStringLiteral("https://is5-ssl.mzstatic.com") + path;
    if (!sourceExt.isEmpty() && sourceExt != terminalExt) {
        appendUnique(replaceUrlTerminalExtension(highResFallback, sourceExt));
    }
    appendUnique(highResFallback);

    return urls;
}

QString deriveItunesPreviewUrl(const QString &artworkUrl100) {
    QString previewUrl = artworkUrl100.trimmed();
    if (previewUrl.isEmpty()) {
        return {};
    }

    previewUrl.replace(QStringLiteral("100x100bb"), QStringLiteral("600x600bb"));
    return previewUrl;
}

QString imageFormatExtension(const QByteArray &format) {
    const QByteArray lower = format.trimmed().toLower();
    if (lower == "jpeg") {
        return QStringLiteral("jpg");
    }
    if (lower == "tiff") {
        return QStringLiteral("tif");
    }
    return QString::fromLatin1(lower);
}

int readEnvMillis(const char *key, int fallback) {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(key, &ok);
    if (!ok) {
        return fallback;
    }
    return std::clamp(value, 8, 1000);
}

QVariantMap readImageFileDetails(const QString &path) {
    QVariantMap out;

    const QString localPath = normalizeLocalPathArg(path);
    if (localPath.isEmpty()) {
        return out;
    }

    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile()) {
        return out;
    }

    const QString resolvedPath = info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath()
        : info.canonicalFilePath();
    out.insert(QStringLiteral("path"), resolvedPath);
    out.insert(QStringLiteral("fileName"), info.fileName());

    const qint64 sizeBytes = info.size();
    if (sizeBytes >= 0) {
        out.insert(QStringLiteral("fileSizeBytes"), sizeBytes);
        out.insert(QStringLiteral("fileSizeText"), formatBinaryFileSize(sizeBytes));
    }

    const QString suffix = info.suffix().trimmed().toUpper();
    if (!suffix.isEmpty()) {
        out.insert(QStringLiteral("extension"), suffix);
    }

    const QMimeDatabase mimeDb;
    const auto mimeType = mimeDb.mimeTypeForFile(info, QMimeDatabase::MatchDefault);
    const QString mimeName = mimeType.name().trimmed();
    if (!mimeName.isEmpty() && mimeName != QStringLiteral("application/octet-stream")) {
        out.insert(QStringLiteral("mimeType"), mimeName);
    }

    QString fileType = mimeType.comment().trimmed();
    QImageReader reader(resolvedPath);
    const QSize imageSize = reader.size();
    if (imageSize.isValid()) {
        out.insert(QStringLiteral("width"), imageSize.width());
        out.insert(QStringLiteral("height"), imageSize.height());
        out.insert(
            QStringLiteral("resolutionText"),
            QStringLiteral("%1 x %2").arg(imageSize.width()).arg(imageSize.height()));
    }

    const QString format = QString::fromLatin1(reader.format()).trimmed().toUpper();
    if (!format.isEmpty()) {
        out.insert(QStringLiteral("format"), format);
    }

    if (fileType.isEmpty()) {
        if (!format.isEmpty()) {
            fileType = QStringLiteral("%1 image").arg(format);
        } else if (!suffix.isEmpty()) {
            fileType = QStringLiteral("%1 image").arg(suffix);
        }
    }
    if (!fileType.isEmpty()) {
        out.insert(QStringLiteral("fileType"), fileType);
    }

    return out;
}

} // namespace

BridgeClient::ItunesArtworkAssetJobResult BridgeClient::processItunesArtworkAssetPayload(
    const QByteArray &payload,
    const QString &tempDirPath,
    quint64 generation,
    int candidateIndex,
    int assetUrlIndex)
{
    BridgeClient::ItunesArtworkAssetJobResult result;
    result.generation = generation;
    result.candidateIndex = candidateIndex;
    result.assetUrlIndex = assetUrlIndex;
    result.usedFallback = assetUrlIndex > 0;

    if (payload.isEmpty()) {
        result.errorMessage = QStringLiteral("Failed to load high-resolution artwork.");
        return result;
    }
    if (tempDirPath.trimmed().isEmpty()) {
        result.errorMessage = QStringLiteral("Failed to prepare temporary artwork cache.");
        return result;
    }

    QBuffer buffer;
    buffer.setData(payload);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QByteArray format = reader.format().trimmed().toLower();
    const QString sourceExtension = imageFormatExtension(format);
    const auto sourceTransform = reader.transformation();
    if (sourceExtension != QStringLiteral("png")
        && sourceExtension != QStringLiteral("jpg")
        && sourceExtension != QStringLiteral("tif")) {
        result.errorMessage = QStringLiteral("Unsupported artwork format returned by iTunes.");
        return result;
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        result.errorMessage = QStringLiteral("Failed to decode downloaded artwork.");
        return result;
    }

    const QString baseName = QStringLiteral("candidate-%1-%2")
        .arg(generation)
        .arg(candidateIndex, 3, 10, QChar('0'));
    const QString originalPath = QDir(tempDirPath).filePath(
        baseName + QStringLiteral("-orig.") + sourceExtension);
    QFile originalFile(originalPath);
    if (!originalFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || originalFile.write(payload) != payload.size()) {
        result.errorMessage = QStringLiteral("Failed to cache downloaded artwork.");
        return result;
    }
    originalFile.close();

    const int side = std::min(image.width(), image.height());
    const int x = std::max(0, (image.width() - side) / 2);
    const int y = std::max(0, (image.height() - side) / 2);
    const bool needsCrop = image.width() != image.height();
    const QImage normalized = !needsCrop
        ? image
        : image.copy(x, y, side, side);
    const QString normalizedExtension = sourceExtension == QStringLiteral("png")
        ? QStringLiteral("png")
        : QStringLiteral("jpg");
    const bool needsTransformBake = sourceTransform != QImageIOHandler::TransformationNone;
    const bool canReuseOriginalAsset =
        !needsCrop
        && !needsTransformBake
        && sourceExtension == normalizedExtension;

    QString normalizedPath = originalPath;
    if (!canReuseOriginalAsset) {
        normalizedPath = QDir(tempDirPath).filePath(
            baseName + QStringLiteral("-normalized.") + normalizedExtension);
        const bool saved = normalizedExtension == QStringLiteral("jpg")
            ? normalized.save(normalizedPath, "JPG", 95)
            : normalized.save(normalizedPath, "PNG");
        if (!saved) {
            result.errorMessage = QStringLiteral("Failed to normalize downloaded artwork.");
            return result;
        }
    }

    result.success = true;
    result.normalizedPath = normalizedPath;
    result.downloadPath = originalPath;
    result.imageDetails = readImageFileDetails(normalizedPath);
    return result;
}

namespace {

QString findAlbumCoverPath(const QStringList &trackPaths) {
    static const QSet<QString> kImageExts{
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("png"),
        QStringLiteral("webp"),
        QStringLiteral("bmp"),
    };
    static const QStringList kPreferredBases{
        QStringLiteral("cover"),
        QStringLiteral("folder"),
        QStringLiteral("front"),
        QStringLiteral("album"),
        QStringLiteral("artwork"),
    };

    QString bestPath;
    int bestScore = std::numeric_limits<int>::max();
    QSet<QString> scannedDirs;
    for (const QString &trackPath : trackPaths) {
        const QFileInfo trackInfo(trackPath);
        if (!trackInfo.exists()) {
            continue;
        }
        const QDir dir = trackInfo.dir();
        const QString dirPath = dir.absolutePath();
        if (scannedDirs.contains(dirPath)) {
            continue;
        }
        scannedDirs.insert(dirPath);

        const QFileInfoList files = dir.entryInfoList(
            QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden,
            QDir::Name);
        for (const QFileInfo &info : files) {
            const QString ext = info.suffix().toLower();
            if (!kImageExts.contains(ext)) {
                continue;
            }
            const QString base = info.completeBaseName().toLower();
            int score = 4;
            for (int i = 0; i < kPreferredBases.size(); ++i) {
                const QString &preferred = kPreferredBases[i];
                if (base == preferred) {
                    score = i;
                    break;
                }
                if (base.startsWith(preferred)) {
                    score = i + 1;
                }
            }
            if (bestPath.isEmpty() || score < bestScore
                || (score == bestScore && info.absoluteFilePath() < bestPath)) {
                bestPath = info.absoluteFilePath();
                bestScore = score;
            }
        }
    }
    return bestPath;
}

QString findTrackCoverUrl(const QString &trackPath) {
    if (trackPath.trimmed().isEmpty()) {
        return {};
    }
    const QString coverPath = findAlbumCoverPath({trackPath});
    if (coverPath.isEmpty()) {
        return {};
    }
    return QUrl::fromLocalFile(coverPath).toString();
}

QString trackDirectoryPath(const QString &trackPath) {
    if (trackPath.trimmed().isEmpty()) {
        return {};
    }
    const QFileInfo info(trackPath);
    return info.absoluteDir().absolutePath();
}

QString playbackStateText(int state, const QString &fallback) {
    switch (state) {
    case 0:
        return QStringLiteral("Stopped");
    case 1:
        return QStringLiteral("Playing");
    case 2:
        return QStringLiteral("Paused");
    default:
        return fallback;
    }
}

QString channelLayoutText(int channels) {
    switch (channels) {
    case 1:
        return QStringLiteral("mono");
    case 2:
        return QStringLiteral("stereo");
    case 3:
        return QStringLiteral("3.0");
    case 4:
        return QStringLiteral("4.0");
    case 5:
        return QStringLiteral("5.0");
    case 6:
        return QStringLiteral("5.1");
    case 7:
        return QStringLiteral("6.1");
    case 8:
        return QStringLiteral("7.1");
    default:
        return channels > 0 ? QStringLiteral("%1 ch").arg(channels) : QString{};
    }
}

QString channelLayoutIconKey(int channels) {
    switch (channels) {
    case 1:
        return QStringLiteral("mono");
    case 2:
        return QStringLiteral("stereo");
    case 4:
        return QStringLiteral("4.0");
    case 5:
        return QStringLiteral("5.0");
    case 6:
        return QStringLiteral("5.1");
    case 7:
        return QStringLiteral("6.1");
    case 8:
        return QStringLiteral("7.1");
    default:
        return QString{};
    }
}

QString spectrogramChannelLabelText(quint8 code, int fallbackIndex) {
    switch (code) {
    case 0:
        return QStringLiteral("M");
    case 1:
        return QStringLiteral("L");
    case 2:
        return QStringLiteral("R");
    case 3:
        return QStringLiteral("C");
    case 4:
        return QStringLiteral("LFE");
    case 5:
        return QStringLiteral("SL");
    case 6:
        return QStringLiteral("SR");
    case 7:
        return QStringLiteral("RL");
    case 8:
        return QStringLiteral("RR");
    case 9:
        return QStringLiteral("RC");
    default:
        return QStringLiteral("Ch%1").arg(std::max(1, fallbackIndex + 1));
    }
}

QString formatLabelFromPath(const QString &path) {
    const QString ext = QFileInfo(path).suffix().trimmed().toLower();
    if (ext == QStringLiteral("m4a")
        || ext == QStringLiteral("m4b")
        || ext == QStringLiteral("m4p")
        || ext == QStringLiteral("m4r")
        || ext == QStringLiteral("mp4")) {
        return QStringLiteral("AAC");
    }
    if (ext == QStringLiteral("aif")
        || ext == QStringLiteral("aiff")
        || ext == QStringLiteral("aifc")
        || ext == QStringLiteral("afc")) {
        return QStringLiteral("AIFF");
    }
    if (ext == QStringLiteral("ogg")) {
        return QStringLiteral("Vorbis");
    }
    if (ext == QStringLiteral("wv")) {
        return QStringLiteral("WavPack");
    }
    return ext.isEmpty() ? QString{} : ext.toUpper();
}

} // namespace

QueueRowsModel::QueueRowsModel(QObject *parent)
    : QAbstractListModel(parent) {}

int QueueRowsModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size();
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

bool QueueRowsModel::setRows(QVector<QueueRowData> rows) {
    if (m_rows == rows) {
        return false;
    }
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
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

BridgeClient::BridgeClient(QObject *parent)
    : QObject(parent) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    m_profileUiEnabled = qEnvironmentVariableIsSet("FERROUS_PROFILE_UI")
        || qEnvironmentVariableIsSet("FERROUS_PROFILE");
#endif
    m_fileBrowserName = detectFileBrowserNameHeuristic();
    m_diagnosticsLogPath = resolveDiagnosticsLogPath();
    reloadDiagnosticsFromDisk();
    logDiagnostic(QStringLiteral("ui"), QStringLiteral("BridgeClient started"));

    m_snapshotNotifyTimer.setSingleShot(true);
    m_snapshotNotifyTimer.setInterval(readEnvMillis("FERROUS_UI_SNAPSHOT_NOTIFY_MS", 33));
    connect(&m_snapshotNotifyTimer, &QTimer::timeout, this, [this]() {
        if (m_snapshotChangedPending) {
            m_snapshotChangedPending = false;
            emit snapshotChanged();
        }
    });

    m_globalSearchDebounceTimer.setSingleShot(true);
    m_globalSearchDebounceMs = readEnvMillis("FERROUS_UI_SEARCH_DEBOUNCE_MS", 90);
    m_globalSearchShortDebounceMs = readEnvMillis(
        "FERROUS_UI_SEARCH_DEBOUNCE_SHORT_MS",
        std::max(220, m_globalSearchDebounceMs + 130));
    {
        bool ok = false;
        const int value =
            qEnvironmentVariableIntValue("FERROUS_UI_SEARCH_DEBOUNCE_SHORT_CHARS", &ok);
        m_globalSearchShortDebounceChars = ok ? std::clamp(value, 1, 8) : 1;
    }
    {
        const QByteArray raw = qgetenv("FERROUS_UI_SEARCH_LEGACY_LISTS");
        const QByteArray normalized = raw.trimmed().toLower();
        m_publishLegacySearchLists = !normalized.isEmpty()
            && normalized != QByteArrayLiteral("0")
            && normalized != QByteArrayLiteral("false")
            && normalized != QByteArrayLiteral("no");
    }
    m_globalSearchDebounceTimer.setInterval(m_globalSearchDebounceMs);
    connect(&m_globalSearchDebounceTimer, &QTimer::timeout, this, &BridgeClient::flushGlobalSearchQuery);

    m_searchApplyDispatchMs = readEnvMillis("FERROUS_UI_SEARCH_APPLY_DISPATCH_MS", 12);
    m_searchApplyDispatchTimer.setSingleShot(true);
    m_searchApplyDispatchTimer.setInterval(m_searchApplyDispatchMs);
    connect(
        &m_searchApplyDispatchTimer,
        &QTimer::timeout,
        this,
        &BridgeClient::dispatchPendingSearchApplyFrame);

    {
        bool ok = false;
        const int configuredBudgetMs =
            qEnvironmentVariableIntValue("FERROUS_UI_BRIDGE_POLL_BUDGET_MS", &ok);
        m_bridgePollBudgetMs = ok ? std::clamp(configuredBudgetMs, 1, 100) : 5;
    }
    m_bridgePollTimer.setSingleShot(true);
    m_bridgePollTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_bridgePollTimer, &QTimer::timeout, this, &BridgeClient::pollInProcessBridge);

    if (m_profileUiEnabled) {
        const int uiStallWatchdogIntervalMs =
            std::max(4, readEnvMillis("FERROUS_UI_STALL_WATCHDOG_MS", 8));
        const int uiStallThresholdMs =
            std::max(12, readEnvMillis("FERROUS_UI_STALL_THRESHOLD_MS", 20));
        m_uiStallWatchdogTimer.setTimerType(Qt::PreciseTimer);
        m_uiStallWatchdogTimer.setInterval(uiStallWatchdogIntervalMs);
        m_uiStallWatchdogElapsed.start();
        m_uiStallWatchdogLastTickMs = m_uiStallWatchdogElapsed.elapsed();
        connect(&m_uiStallWatchdogTimer, &QTimer::timeout, this, [this, uiStallThresholdMs, uiStallWatchdogIntervalMs]() {
            if (!m_uiStallWatchdogElapsed.isValid()) {
                m_uiStallWatchdogElapsed.start();
                m_uiStallWatchdogLastTickMs = m_uiStallWatchdogElapsed.elapsed();
                return;
            }

            const qint64 elapsedMs = m_uiStallWatchdogElapsed.elapsed();
            const qint64 gapMs = elapsedMs - m_uiStallWatchdogLastTickMs;
            m_uiStallWatchdogLastTickMs = elapsedMs;

            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (gapMs >= uiStallThresholdMs
                && shouldEmitUiProfileLog(nowMs, &m_lastUiStallProfileLogMs, 25)) {
                FERROUS_PROFILE_LOG_DIAGNOSTIC(
                    QStringLiteral("ui-prof"),
                    QStringLiteral(
                        "event_loop_stall ms=%1 timer_ms=%2 connected=%3 snapshot_pending=%4")
                        .arg(gapMs)
                        .arg(uiStallWatchdogIntervalMs)
                        .arg(m_connected ? 1 : 0)
                        .arg(m_snapshotChangedPending ? 1 : 0));
            }
        });
        m_uiStallWatchdogTimer.start();
    }

    startSearchApplyWorker();
    startCoverLookupWorker();
    startFileBrowserNameDetection();
    startInProcessBridge();
}

BridgeClient::~BridgeClient() {
    m_bridgePollTimer.stop();
    if (m_bridgeWakeNotifier != nullptr) {
        m_bridgeWakeNotifier->setEnabled(false);
        delete m_bridgeWakeNotifier;
        m_bridgeWakeNotifier = nullptr;
    }
    m_bridgeWakeFd = -1;
    m_uiStallWatchdogTimer.stop();
    m_globalSearchDebounceTimer.stop();
    m_searchApplyDispatchTimer.stop();
    cancelItunesArtworkRequests();
    resetItunesArtworkTempDir();
    shutdownBridgeGracefully();
    stopCoverLookupWorker();
    stopSearchApplyWorker();
    if (m_ffiBridge != nullptr) {
        ferrous_ffi_bridge_destroy(m_ffiBridge);
        m_ffiBridge = nullptr;
    }
}

void BridgeClient::startFileBrowserNameDetection() {
    const QPointer<BridgeClient> self(this);
    std::thread([self]() {
        const QString detected = BridgeClient::detectFileBrowserName();
        QCoreApplication *app = QCoreApplication::instance();
        if (app == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            app,
            [self, detected]() {
                if (!self) {
                    return;
                }
                self->applyDetectedFileBrowserName(detected);
            },
            Qt::QueuedConnection);
    }).detach();
}

void BridgeClient::applyDetectedFileBrowserName(const QString &name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_fileBrowserName == trimmed) {
        return;
    }
    m_fileBrowserName = trimmed;
    scheduleSnapshotChanged();
}

bool BridgeClient::startInProcessBridge() {
    if (m_bridgeWakeNotifier != nullptr) {
        m_bridgeWakeNotifier->setEnabled(false);
        delete m_bridgeWakeNotifier;
        m_bridgeWakeNotifier = nullptr;
    }
    m_bridgeWakeFd = -1;

    m_ffiBridge = ferrous_ffi_bridge_create();
    if (m_ffiBridge == nullptr) {
        logDiagnostic(QStringLiteral("bridge"), QStringLiteral("failed to create in-process bridge"));
        emit bridgeError(QStringLiteral("failed to create in-process Rust bridge"));
        return false;
    }

    m_bridgeWakeFd = ferrous_ffi_bridge_wakeup_fd(m_ffiBridge);
    if (m_bridgeWakeFd >= 0) {
        m_bridgeWakeNotifier = new QSocketNotifier(
            QSocketDescriptor(m_bridgeWakeFd),
            QSocketNotifier::Read,
            this);
        connect(
            m_bridgeWakeNotifier,
            &QSocketNotifier::activated,
            this,
            [this](QSocketDescriptor, QSocketNotifier::Type) {
                if (m_ffiBridge == nullptr || m_bridgeWakeNotifier == nullptr) {
                    return;
                }
                m_bridgeWakeNotifier->setEnabled(false);
                ferrous_ffi_bridge_ack_wakeup(m_ffiBridge);
                pollInProcessBridge();
            });
        m_bridgeWakeNotifier->setEnabled(true);
    } else {
        logDiagnostic(
            QStringLiteral("bridge"),
            QStringLiteral("wake fd unavailable; using continuation timer only"));
        scheduleBridgePoll(0);
    }
    if (!m_connected) {
        m_connected = true;
        emit connectedChanged();
    }
    logDiagnostic(QStringLiteral("bridge"), QStringLiteral("in-process bridge created"));
    requestSnapshot();
    return true;
}

void BridgeClient::startSearchApplyWorker() {
    std::lock_guard<std::mutex> lock(m_searchApplyMutex);
    if (m_searchApplyThread.joinable()) {
        return;
    }
    m_searchApplyStop = false;
    m_searchApplyThread = std::thread([this]() {
        searchApplyWorkerLoop();
    });
}

void BridgeClient::stopSearchApplyWorker() {
    {
        std::lock_guard<std::mutex> lock(m_searchApplyMutex);
        m_searchApplyStop = true;
        m_searchPendingInputFrame.reset();
    }
    {
        std::lock_guard<std::mutex> lock(m_searchOutputMutex);
        m_searchPendingOutputFrame.reset();
        m_searchOutputCoalescedDrops = 0;
    }
    m_searchApplyCv.notify_all();
    if (m_searchApplyThread.joinable()) {
        m_searchApplyThread.join();
    }
}

void BridgeClient::startCoverLookupWorker() {
    std::lock_guard<std::mutex> lock(m_coverLookupMutex);
    if (m_coverLookupThread.joinable()) {
        return;
    }
    m_coverLookupStop = false;
    m_coverLookupThread = std::thread([this]() {
        coverLookupWorkerLoop();
    });
}

void BridgeClient::stopCoverLookupWorker() {
    {
        std::lock_guard<std::mutex> lock(m_coverLookupMutex);
        m_coverLookupStop = true;
        m_coverLookupPendingPath.reset();
        m_coverLookupInFlightPath.clear();
    }
    m_coverLookupCv.notify_all();
    if (m_coverLookupThread.joinable()) {
        m_coverLookupThread.join();
    }
}

void BridgeClient::requestTrackCoverLookup(const QString &trackPath) {
    const QString normalizedPath = trackPath.trimmed();
    if (normalizedPath.isEmpty()) {
        return;
    }
    if (m_trackCoverByPath.contains(normalizedPath)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_coverLookupMutex);
        if (m_coverLookupPendingPath.has_value() && *m_coverLookupPendingPath == normalizedPath) {
            return;
        }
        if (m_coverLookupInFlightPath == normalizedPath) {
            return;
        }
        m_coverLookupPendingPath = normalizedPath;
    }
    m_coverLookupCv.notify_one();
}

void BridgeClient::coverLookupWorkerLoop() {
    for (;;) {
        QString trackPath;
        {
            std::unique_lock<std::mutex> lock(m_coverLookupMutex);
            m_coverLookupCv.wait(lock, [this]() {
                return m_coverLookupStop || m_coverLookupPendingPath.has_value();
            });
            if (m_coverLookupStop) {
                return;
            }
            if (!m_coverLookupPendingPath.has_value()) {
                continue;
            }
            trackPath = std::move(*m_coverLookupPendingPath);
            m_coverLookupPendingPath.reset();
            m_coverLookupInFlightPath = trackPath;
        }

        const QString coverUrl = findTrackCoverUrl(trackPath);

        {
            std::lock_guard<std::mutex> lock(m_coverLookupMutex);
            if (m_coverLookupInFlightPath == trackPath) {
                m_coverLookupInFlightPath.clear();
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, trackPath, coverUrl]() {
                applyTrackCoverLookupResult(trackPath, coverUrl);
            },
            Qt::QueuedConnection);
    }
}

void BridgeClient::applyTrackCoverLookupResult(const QString &trackPath, const QString &coverUrl) {
    if (trackPath.trimmed().isEmpty()) {
        return;
    }
    cacheTrackCoverForPath(trackPath, coverUrl);
    if (m_currentTrackPath == trackPath && m_currentTrackCoverPath != coverUrl) {
        m_currentTrackCoverPath = coverUrl;
        scheduleSnapshotChanged();
    }
}

void BridgeClient::cacheTrackCoverForPath(const QString &trackPath, const QString &coverUrl) {
    const QString normalizedPath = trackPath.trimmed();
    if (normalizedPath.isEmpty()) {
        return;
    }
    m_trackCoverByPath.insert(normalizedPath, coverUrl);
    const QString dirPath = trackDirectoryPath(normalizedPath);
    if (!dirPath.isEmpty()) {
        m_trackCoverByDirectory.insert(dirPath, coverUrl);
    }
    if (m_trackCoverByPath.size() > 4096) {
        m_trackCoverByPath.clear();
        m_trackCoverByPath.insert(normalizedPath, coverUrl);
    }
    if (!dirPath.isEmpty() && m_trackCoverByDirectory.size() > 2048) {
        m_trackCoverByDirectory.clear();
        m_trackCoverByDirectory.insert(dirPath, coverUrl);
    }
}

void BridgeClient::rebuildQueuePathFirstIndex() {
    m_queuePathFirstIndex.clear();
    m_queuePathFirstIndex.reserve(m_queuePaths.size());
    for (int index = 0; index < m_queuePaths.size(); ++index) {
        const QString &path = m_queuePaths.at(index);
        if (!path.isEmpty() && !m_queuePathFirstIndex.contains(path)) {
            m_queuePathFirstIndex.insert(path, index);
        }
    }
}

int BridgeClient::queuePathFirstIndex(const QString &path) const {
    if (path.isEmpty()) {
        return -1;
    }
    return m_queuePathFirstIndex.value(path, -1);
}

QString BridgeClient::coverUrlForPath(const QString &path) const {
    const QString localPath = normalizeLocalPathArg(path);
    QString baseUrl;
    if (!localPath.isEmpty()) {
        const auto urlIt = m_coverUrlCacheByPath.constFind(localPath);
        if (urlIt != m_coverUrlCacheByPath.constEnd()) {
            baseUrl = urlIt.value();
        } else {
            baseUrl = searchCoverUrlFast(localPath);
            m_coverUrlCacheByPath.insert(localPath, baseUrl);
            if (m_coverUrlCacheByPath.size() > 4096) {
                m_coverUrlCacheByPath.clear();
                m_coverUrlCacheByPath.insert(localPath, baseUrl);
            }
        }
    } else {
        baseUrl = searchCoverUrlFast(path);
    }
    if (localPath.isEmpty()) {
        return baseUrl;
    }
    if (m_coverRefreshNonceByPath.isEmpty()) {
        return baseUrl;
    }

    QString canonicalPath;
    const auto canonicalIt = m_coverCanonicalPathCacheByPath.constFind(localPath);
    if (canonicalIt != m_coverCanonicalPathCacheByPath.constEnd()) {
        canonicalPath = canonicalIt.value();
    } else {
        const QFileInfo info(localPath);
        canonicalPath = info.canonicalFilePath().isEmpty()
            ? info.absoluteFilePath()
            : info.canonicalFilePath();
        if (!canonicalPath.isEmpty()) {
            m_coverCanonicalPathCacheByPath.insert(localPath, canonicalPath);
            if (m_coverCanonicalPathCacheByPath.size() > 4096) {
                m_coverCanonicalPathCacheByPath.clear();
                m_coverCanonicalPathCacheByPath.insert(localPath, canonicalPath);
            }
        }
    }
    if (canonicalPath.isEmpty()) {
        return baseUrl;
    }

    const auto nonceIt = m_coverRefreshNonceByPath.constFind(canonicalPath);
    if (nonceIt == m_coverRefreshNonceByPath.constEnd()) {
        return baseUrl;
    }

    QUrl url(baseUrl);
    if (!url.isValid()) {
        url = QUrl::fromLocalFile(canonicalPath);
    }
    QString fragment = url.fragment(QUrl::FullyDecoded);
    const int refreshPos = fragment.indexOf(QStringLiteral("&r="));
    if (refreshPos >= 0) {
        fragment = fragment.left(refreshPos);
    } else if (fragment.startsWith(QStringLiteral("r="))) {
        fragment.clear();
    }
    if (!fragment.isEmpty()) {
        fragment += QStringLiteral("&");
    }
    fragment += QStringLiteral("r=%1").arg(nonceIt.value());
    url.setFragment(fragment);
    return url.toString(QUrl::FullyEncoded);
}

void BridgeClient::bumpCoverRefreshNonce(const QString &path) {
    const QString localPath = normalizeLocalPathArg(path);
    if (localPath.isEmpty()) {
        return;
    }

    m_coverUrlCacheByPath.remove(localPath);
    m_coverCanonicalPathCacheByPath.remove(localPath);

    const QFileInfo info(localPath);
    const QString canonicalPath = info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath()
        : info.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        return;
    }

    m_coverRefreshNonceByPath.insert(canonicalPath, m_nextCoverRefreshNonce++);
    if (m_coverRefreshNonceByPath.size() > 4096) {
        m_coverRefreshNonceByPath.clear();
        m_coverRefreshNonceByPath.insert(canonicalPath, m_nextCoverRefreshNonce++);
    }
}

void BridgeClient::cancelItunesArtworkRequests() {
    const auto replies = m_itunesArtworkReplies.values();
    m_itunesArtworkReplies.clear();
    for (QNetworkReply *reply : replies) {
        if (!reply) {
            continue;
        }
        reply->abort();
        reply->deleteLater();
    }
}

void BridgeClient::startItunesArtworkAssetDownload(
    int candidateIndex,
    int assetUrlIndex)
{
    if (candidateIndex < 0 || candidateIndex >= m_itunesArtworkCandidates.size()) {
        return;
    }
    const QStringList assetUrls = m_itunesArtworkCandidates[candidateIndex].assetUrls;
    const QString trimmedUrl = (assetUrlIndex >= 0 && assetUrlIndex < assetUrls.size())
        ? assetUrls[assetUrlIndex].trimmed()
        : QString();
    QVariantMap row = itunesArtworkResultAt(candidateIndex);
    if (row.isEmpty()) {
        return;
    }
    if (trimmedUrl.isEmpty()) {
        row.insert(QStringLiteral("assetLoading"), false);
        row.insert(QStringLiteral("assetReady"), false);
        row.insert(
            QStringLiteral("assetError"),
            QStringLiteral("No high-resolution artwork URL was available."));
        row.insert(
            QStringLiteral("detailStatusText"),
            QStringLiteral("High-resolution artwork could not be loaded."));
        updateItunesArtworkResult(candidateIndex, row);
        return;
    }
    if (!ensureItunesArtworkTempDir()) {
        row.insert(QStringLiteral("assetLoading"), false);
        row.insert(QStringLiteral("assetReady"), false);
        row.insert(
            QStringLiteral("assetError"),
            QStringLiteral("Failed to prepare temporary artwork cache."));
        row.insert(
            QStringLiteral("detailStatusText"),
            QStringLiteral("High-resolution artwork could not be loaded."));
        updateItunesArtworkResult(candidateIndex, row);
        return;
    }

    const quint64 generation = m_itunesArtworkGeneration;
    const QString tempDirPath = m_itunesArtworkTempDir ? m_itunesArtworkTempDir->path() : QString();
    QNetworkRequest request{QUrl(trimmedUrl)};
    request.setTransferTimeout(30000);
    auto *reply = m_itunesArtworkNetwork.get(request);
    m_itunesArtworkReplies.insert(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, candidateIndex, assetUrlIndex, tempDirPath]() {
        m_itunesArtworkReplies.remove(reply);
        const bool stale = generation != m_itunesArtworkGeneration;
        const auto error = reply->error();
        const QString errorText = reply->errorString();
        const QByteArray payload = reply->readAll();
        reply->deleteLater();
        if (stale) {
            return;
        }

        QVariantMap row = itunesArtworkResultAt(candidateIndex);
        if (row.isEmpty()) {
            return;
        }

        auto failCandidate = [this, candidateIndex, assetUrlIndex, row](const QString &message) mutable {
            if (candidateIndex >= 0 && candidateIndex < m_itunesArtworkCandidates.size()) {
                const QStringList assetUrls = m_itunesArtworkCandidates[candidateIndex].assetUrls;
                if (assetUrlIndex + 1 < assetUrls.size()) {
                    startItunesArtworkAssetDownload(candidateIndex, assetUrlIndex + 1);
                    return;
                }
            }
            row.insert(QStringLiteral("assetLoading"), false);
            row.insert(QStringLiteral("assetReady"), false);
            row.insert(QStringLiteral("assetError"), message);
            row.insert(
                QStringLiteral("detailStatusText"),
                QStringLiteral("High-resolution artwork could not be loaded."));
            updateItunesArtworkResult(candidateIndex, row);
        };

        if (error != QNetworkReply::NoError || payload.isEmpty()) {
            failCandidate(
                errorText.trimmed().isEmpty()
                    ? QStringLiteral("Failed to load high-resolution artwork.")
                    : QStringLiteral("Failed to load high-resolution artwork: %1").arg(errorText));
            return;
        }

        const QPointer<BridgeClient> self(this);
        std::thread([self, payload, tempDirPath, generation, candidateIndex, assetUrlIndex]() {
            BridgeClient::ItunesArtworkAssetJobResult result = BridgeClient::processItunesArtworkAssetPayload(
                payload,
                tempDirPath,
                generation,
                candidateIndex,
                assetUrlIndex);
            QCoreApplication *app = QCoreApplication::instance();
            if (app == nullptr) {
                return;
            }
            QMetaObject::invokeMethod(
                app,
                [self, result = std::move(result)]() mutable {
                    if (!self) {
                        return;
                    }
                    self->applyItunesArtworkAssetJobResult(std::move(result));
                },
                Qt::QueuedConnection);
        }).detach();
    });
}

void BridgeClient::applyItunesArtworkAssetJobResult(ItunesArtworkAssetJobResult result) {
    if (result.generation != m_itunesArtworkGeneration) {
        return;
    }
    QVariantMap row = itunesArtworkResultAt(result.candidateIndex);
    if (row.isEmpty()) {
        return;
    }

    if (!result.success) {
        if (result.candidateIndex >= 0 && result.candidateIndex < m_itunesArtworkCandidates.size()) {
            const QStringList assetUrls = m_itunesArtworkCandidates[result.candidateIndex].assetUrls;
            if (result.assetUrlIndex + 1 < assetUrls.size()) {
                startItunesArtworkAssetDownload(result.candidateIndex, result.assetUrlIndex + 1);
                return;
            }
        }
        row.insert(QStringLiteral("assetLoading"), false);
        row.insert(QStringLiteral("assetReady"), false);
        row.insert(QStringLiteral("assetError"), result.errorMessage);
        row.insert(
            QStringLiteral("detailStatusText"),
            QStringLiteral("High-resolution artwork could not be loaded."));
        updateItunesArtworkResult(result.candidateIndex, row);
        return;
    }

    cacheImageFileDetails(result.normalizedPath, result.imageDetails);
    row.insert(QStringLiteral("previewSource"), cacheOnlyLocalFileUrl(result.normalizedPath));
    row.insert(QStringLiteral("normalizedPath"), result.normalizedPath);
    row.insert(QStringLiteral("normalizedUrl"), cacheOnlyLocalFileUrl(result.normalizedPath));
    row.insert(QStringLiteral("downloadPath"), result.downloadPath);
    row.insert(QStringLiteral("usedFallback"), result.usedFallback);
    row.insert(QStringLiteral("assetReady"), true);
    row.insert(QStringLiteral("assetLoading"), false);
    row.insert(QStringLiteral("assetError"), QString());
    row.insert(
        QStringLiteral("detailStatusText"),
        result.usedFallback
            ? QStringLiteral("Loaded the high-resolution fallback artwork.")
            : QString());
    for (auto it = result.imageDetails.constBegin(); it != result.imageDetails.constEnd(); ++it) {
        row.insert(it.key(), it.value());
    }
    updateItunesArtworkResult(result.candidateIndex, row);
}

void BridgeClient::updateItunesArtworkResult(int index, const QVariantMap &row) {
    if (index < 0 || index >= m_itunesArtworkResults.size()) {
        return;
    }
    m_itunesArtworkResults[index] = row;
    emit itunesArtworkChanged();
}

void BridgeClient::resetItunesArtworkTempDir() {
    m_itunesArtworkTempDir.reset();
}

bool BridgeClient::ensureItunesArtworkTempDir() {
    if (m_itunesArtworkTempDir && m_itunesArtworkTempDir->isValid()) {
        return true;
    }

    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (baseDir.trimmed().isEmpty()) {
        baseDir = QDir::tempPath();
    } else {
        baseDir = QDir(baseDir).filePath(QStringLiteral("ferrous"));
    }
    QDir().mkpath(baseDir);

    auto dir = std::make_unique<QTemporaryDir>(
        QDir(baseDir).filePath(QStringLiteral("itunes-artwork-XXXXXX")));
    if (!dir->isValid()) {
        return false;
    }
    dir->setAutoRemove(true);
    m_itunesArtworkTempDir = std::move(dir);
    return true;
}

void BridgeClient::enqueueSearchFrame(quint32 seq, QByteArray payload, qint64 ffiPopMs) {
    {
        std::lock_guard<std::mutex> lock(m_searchApplyMutex);
        if (m_searchPendingInputFrame.has_value()) {
            m_searchInputCoalescedDrops++;
        }
        m_searchPendingInputFrame = SearchWorkerInputFrame{
            seq,
            std::move(payload),
            QDateTime::currentMSecsSinceEpoch(),
            ffiPopMs,
        };
    }
    m_searchApplyCv.notify_one();
}

void BridgeClient::queuePreparedSearchResultsFrame(SearchWorkerOutputFrame frame) {
    {
        std::lock_guard<std::mutex> lock(m_searchOutputMutex);
        if (m_searchPendingOutputFrame.has_value()) {
            m_searchOutputCoalescedDrops++;
        }
        m_searchPendingOutputFrame = std::move(frame);
    }
    QMetaObject::invokeMethod(
        this,
        &BridgeClient::scheduleSearchApplyDispatch,
        Qt::QueuedConnection);
}

void BridgeClient::scheduleSearchApplyDispatch() {
    const int nextDelayMs = searchApplyDispatchDelayMs();
    if (m_searchApplyDispatchTimer.isActive()) {
        const int remaining = m_searchApplyDispatchTimer.remainingTime();
        // If typing is still active, prefer a later dispatch window for coalescing.
        if (nextDelayMs <= remaining) {
            return;
        }
    }
    m_searchApplyDispatchTimer.start(nextDelayMs);
}

void BridgeClient::dispatchPendingSearchApplyFrame() {
    // While the query debounce is active, user input is still in flight.
    // Delay apply to allow superseded frames to coalesce away.
    if (m_globalSearchDebounceTimer.isActive()) {
        scheduleSearchApplyDispatch();
        return;
    }

    SearchWorkerOutputFrame frame;
    {
        std::lock_guard<std::mutex> lock(m_searchOutputMutex);
        if (!m_searchPendingOutputFrame.has_value()) {
            return;
        }
        frame = std::move(*m_searchPendingOutputFrame);
        m_searchPendingOutputFrame.reset();
        frame.coalescedOutputDrops = m_searchOutputCoalescedDrops;
        m_searchOutputCoalescedDrops = 0;
    }

    const bool changed = applyPreparedSearchResultsFrame(std::move(frame));
    if (changed) {
        emit globalSearchResultsChanged();
    }

    {
        std::lock_guard<std::mutex> lock(m_searchOutputMutex);
        if (m_searchPendingOutputFrame.has_value() && !m_searchApplyDispatchTimer.isActive()) {
            m_searchApplyDispatchTimer.start();
        }
    }
}

int BridgeClient::searchApplyDispatchDelayMs() const {
    int delayMs = m_searchApplyDispatchMs;
    if (m_globalSearchDebounceTimer.isActive()) {
        const int remaining = m_globalSearchDebounceTimer.remainingTime();
        if (remaining > 0) {
            delayMs = std::max(delayMs, remaining + 6);
        }
    }
    return std::clamp(delayMs, m_searchApplyDispatchMs, 220);
}

void BridgeClient::scheduleBridgePoll(int delayMs) {
    if (m_ffiBridge == nullptr) {
        return;
    }
    const int clampedDelay = std::max(0, delayMs);
    if (m_bridgeWakeNotifier != nullptr) {
        m_bridgeWakeNotifier->setEnabled(false);
    }
    if (m_bridgePollTimer.isActive()) {
        const int remaining = m_bridgePollTimer.remainingTime();
        if (remaining >= 0 && remaining <= clampedDelay) {
            return;
        }
    }
    m_bridgePollTimer.start(clampedDelay);
}

BridgeClient::BridgePollRunResult BridgeClient::drainBridgeQueues(qint64 budgetMs) {
    BridgePollRunResult result;
    const qint64 clampedBudgetMs = std::max<qint64>(1, budgetMs);
    QElapsedTimer budgetTimer;
    budgetTimer.start();

    const auto budgetAvailable = [&]() {
        return budgetTimer.elapsed() < clampedBudgetMs;
    };
    const auto markBudgetExhaustedIfNeeded = [&]() {
        if (!budgetAvailable()) {
            result.budgetExhausted = true;
            return true;
        }
        return false;
    };

    constexpr int kMaxAnalysisFramesPerPass = 8;
    while (result.processedAnalysisFrames < kMaxAnalysisFramesPerPass) {
        if (markBudgetExhaustedIfNeeded()) {
            break;
        }
        std::size_t len = 0;
        std::uint8_t *framePtr = ferrous_ffi_bridge_pop_analysis_frame(m_ffiBridge, &len);
        if (framePtr == nullptr || len == 0) {
            break;
        }
        result.processedAnalysisFrames++;
        const QByteArray chunk(
            reinterpret_cast<const char *>(framePtr),
            static_cast<qsizetype>(len));
        result.processedAnalysisBytes += chunk.size();
        ferrous_ffi_bridge_free_analysis_frame(framePtr, len);
        processAnalysisBytes(chunk);
    }
    result.analysisCapSaturated = result.processedAnalysisFrames >= kMaxAnalysisFramesPerPass;

    constexpr int kMaxTreeFramesPerPass = 4;
    while (!result.budgetExhausted && result.processedTreeFrames < kMaxTreeFramesPerPass) {
        if (markBudgetExhaustedIfNeeded()) {
            break;
        }
        std::size_t len = 0;
        std::uint32_t version = 0;
        std::uint8_t *treePtr = ferrous_ffi_bridge_pop_library_tree(m_ffiBridge, &len, &version);
        if (treePtr == nullptr || len == 0) {
            break;
        }
        result.processedTreeFrames++;
        const QByteArray treeBytes(
            reinterpret_cast<const char *>(treePtr),
            static_cast<qsizetype>(len));
        ferrous_ffi_bridge_free_library_tree(treePtr, len);
        const int versionInt = version > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : static_cast<int>(version);
        applyLibraryTreeFrame(versionInt, treeBytes);
    }
    result.treeCapSaturated = result.processedTreeFrames >= kMaxTreeFramesPerPass;

    constexpr int kMaxSearchFramesPerPass = 4;
    while (!result.budgetExhausted && result.processedSearchFrames < kMaxSearchFramesPerPass) {
        if (markBudgetExhaustedIfNeeded()) {
            break;
        }
        QElapsedTimer popTimer;
        popTimer.start();
        std::size_t len = 0;
        std::uint32_t seq = 0;
        std::uint8_t *searchPtr = ferrous_ffi_bridge_pop_search_results(
            m_ffiBridge,
            &len,
            &seq);
        if (searchPtr == nullptr || len == 0) {
            break;
        }
        result.processedSearchFrames++;
        const QByteArray payload(
            reinterpret_cast<const char *>(searchPtr),
            static_cast<qsizetype>(len));
        ferrous_ffi_bridge_free_search_results(searchPtr, len);
        enqueueSearchFrame(seq, payload, popTimer.elapsed());
    }
    result.searchCapSaturated = result.processedSearchFrames >= kMaxSearchFramesPerPass;

    constexpr int kMaxEventsPerPass = 3;
    while (!result.budgetExhausted && result.processedEvents < kMaxEventsPerPass) {
        if (markBudgetExhaustedIfNeeded()) {
            break;
        }
        std::size_t len = 0;
        std::uint8_t *packetPtr = ferrous_ffi_bridge_pop_binary_event(m_ffiBridge, &len);
        if (packetPtr == nullptr || len == 0) {
            break;
        }
        result.processedEvents++;
        const QByteArray packet(
            reinterpret_cast<const char *>(packetPtr),
            static_cast<qsizetype>(len));
        ferrous_ffi_bridge_free_binary_event(packetPtr, len);

        BinaryBridgeCodec::DecodedSnapshot decoded;
        QString decodeError;
        if (!BinaryBridgeCodec::decodeSnapshotPacket(packet, &decoded, &decodeError)) {
            logDiagnostic(
                QStringLiteral("bridge"),
                QStringLiteral("snapshot decode error: %1").arg(decodeError));
            emit bridgeError(QStringLiteral("invalid bridge packet: %1").arg(decodeError));
            continue;
        }
        processBinarySnapshot(decoded);
    }
    result.eventCapSaturated = result.processedEvents >= kMaxEventsPerPass;

    return result;
}

void BridgeClient::searchApplyWorkerLoop() {
    for (;;) {
        SearchWorkerInputFrame input;
        quint64 coalescedInputDrops = 0;
        {
            std::unique_lock<std::mutex> lock(m_searchApplyMutex);
            m_searchApplyCv.wait(lock, [this]() {
                return m_searchApplyStop || m_searchPendingInputFrame.has_value();
            });
            if (m_searchApplyStop) {
                return;
            }
            if (!m_searchPendingInputFrame.has_value()) {
                continue;
            }
            input = std::move(*m_searchPendingInputFrame);
            m_searchPendingInputFrame.reset();
            coalescedInputDrops = m_searchInputCoalescedDrops;
            m_searchInputCoalescedDrops = 0;
        }

        QElapsedTimer workerTimer;
        workerTimer.start();

        BinaryBridgeCodec::DecodedSearchResults decoded;
        QString decodeError;
        QElapsedTimer decodeTimer;
        decodeTimer.start();
        const bool decodedOk =
            BinaryBridgeCodec::decodeSearchResultsFrame(input.payload, &decoded, &decodeError);
        const qint64 decodeMs = decodeTimer.elapsed();
        if (decodedOk && input.seq != 0) {
            decoded.seq = input.seq;
        }

        SearchWorkerOutputFrame out;
        out.seq = decoded.seq;
        out.decodeError = decodedOk ? QString{} : decodeError;
        out.ffiPoppedAtMs = input.ffiPoppedAtMs;
        out.ffiPopMs = input.ffiPopMs;
        out.decodeMs = decodeMs;
        out.coalescedInputDrops = coalescedInputDrops;

        if (decodedOk) {
            QElapsedTimer materializeTimer;
            materializeTimer.start();
            QVector<GlobalSearchResultsModel::SearchDisplayRow> artistRows;
            QVector<GlobalSearchResultsModel::SearchDisplayRow> albumRows;
            QVector<GlobalSearchResultsModel::SearchDisplayRow> trackRows;
            artistRows.reserve(decoded.rows.size());
            albumRows.reserve(decoded.rows.size());
            trackRows.reserve(decoded.rows.size());
            if (m_publishLegacySearchLists) {
                out.artistRows.reserve(decoded.rows.size());
                out.albumRows.reserve(decoded.rows.size());
                out.trackRows.reserve(decoded.rows.size());
            }
            for (const auto &row : decoded.rows) {
                QString rowTypeLabel;
                switch (row.rowType) {
                case BinaryBridgeCodec::SearchRowArtist:
                    rowTypeLabel = QStringLiteral("artist");
                    break;
                case BinaryBridgeCodec::SearchRowAlbum:
                    rowTypeLabel = QStringLiteral("album");
                    break;
                case BinaryBridgeCodec::SearchRowTrack:
                    rowTypeLabel = QStringLiteral("track");
                    break;
                default:
                    break;
                }
                if (rowTypeLabel.isEmpty()) {
                    continue;
                }

                GlobalSearchResultsModel::SearchDisplayRow item;
                item.kind = QStringLiteral("item");
                item.rowType = rowTypeLabel;
                item.score = row.score;
                item.label = row.label;
                item.artist = row.artist;
                item.album = row.album;
                item.rootLabel = row.rootLabel;
                item.genre = row.genre;
                item.coverPath = row.coverPath;
                item.coverUrl = searchCoverUrlFast(row.coverPath);
                item.artistKey = row.artistKey;
                item.albumKey = row.albumKey;
                item.sectionKey = row.sectionKey;
                item.trackKey = row.trackKey;
                item.trackPath = row.trackPath;
                item.year = row.year;
                item.trackNumber = row.trackNumber;
                item.count = row.count;
                item.lengthSeconds = row.lengthSeconds;
                item.lengthText = row.lengthSeconds >= 0.0f
                    ? BridgeClient::formatDurationCompact(static_cast<double>(row.lengthSeconds))
                    : QStringLiteral("--:--");

                QVariantMap legacyItem;
                if (m_publishLegacySearchLists) {
                    legacyItem.insert(QStringLiteral("rowType"), row.rowType);
                    legacyItem.insert(QStringLiteral("score"), row.score);
                    legacyItem.insert(QStringLiteral("label"), row.label);
                    legacyItem.insert(QStringLiteral("artist"), row.artist);
                    legacyItem.insert(QStringLiteral("album"), row.album);
                    legacyItem.insert(QStringLiteral("rootLabel"), row.rootLabel);
                    legacyItem.insert(QStringLiteral("genre"), row.genre);
                    legacyItem.insert(QStringLiteral("count"), row.count);
                    legacyItem.insert(QStringLiteral("coverPath"), row.coverPath);
                    legacyItem.insert(QStringLiteral("coverUrl"), item.coverUrl);
                    legacyItem.insert(QStringLiteral("artistKey"), row.artistKey);
                    legacyItem.insert(QStringLiteral("albumKey"), row.albumKey);
                    legacyItem.insert(QStringLiteral("sectionKey"), row.sectionKey);
                    legacyItem.insert(QStringLiteral("trackKey"), row.trackKey);
                    legacyItem.insert(QStringLiteral("trackPath"), row.trackPath);
                    if (row.year != std::numeric_limits<int>::min()) {
                        legacyItem.insert(QStringLiteral("year"), row.year);
                    } else {
                        legacyItem.insert(QStringLiteral("year"), QVariant{});
                    }
                    if (row.trackNumber > 0) {
                        legacyItem.insert(QStringLiteral("trackNumber"), row.trackNumber);
                    } else {
                        legacyItem.insert(QStringLiteral("trackNumber"), QVariant{});
                    }
                    legacyItem.insert(QStringLiteral("lengthSeconds"), row.lengthSeconds);
                    legacyItem.insert(QStringLiteral("lengthText"), item.lengthText);
                }
                switch (row.rowType) {
                case BinaryBridgeCodec::SearchRowArtist:
                    artistRows.push_back(std::move(item));
                    if (m_publishLegacySearchLists) {
                        out.artistRows.push_back(std::move(legacyItem));
                    }
                    break;
                case BinaryBridgeCodec::SearchRowAlbum:
                    albumRows.push_back(std::move(item));
                    if (m_publishLegacySearchLists) {
                        out.albumRows.push_back(std::move(legacyItem));
                    }
                    break;
                case BinaryBridgeCodec::SearchRowTrack:
                    trackRows.push_back(std::move(item));
                    if (m_publishLegacySearchLists) {
                        out.trackRows.push_back(std::move(legacyItem));
                    }
                    break;
                default:
                    break;
                }
            }
            const auto appendSection = [&out](
                                           const QString &title,
                                           const QString &rowType,
                                           const QVector<GlobalSearchResultsModel::SearchDisplayRow> &sourceRows) {
                if (sourceRows.isEmpty()) {
                    return;
                }
                GlobalSearchResultsModel::SearchDisplayRow sectionRow;
                sectionRow.kind = QStringLiteral("section");
                sectionRow.sectionTitle = title;
                sectionRow.rowType = rowType;
                out.displayRows.push_back(std::move(sectionRow));

                GlobalSearchResultsModel::SearchDisplayRow columnsRow;
                columnsRow.kind = QStringLiteral("columns");
                columnsRow.rowType = rowType;
                out.displayRows.push_back(std::move(columnsRow));

                out.displayRows.reserve(out.displayRows.size() + sourceRows.size());
                for (const auto &source : sourceRows) {
                    out.displayRows.push_back(source);
                }
            };
            out.artistCount = artistRows.size();
            out.albumCount = albumRows.size();
            out.trackCount = trackRows.size();
            out.displayRows.reserve(
                artistRows.size() + albumRows.size() + trackRows.size() + 6);
            appendSection(QStringLiteral("Artists"), QStringLiteral("artist"), artistRows);
            appendSection(QStringLiteral("Albums"), QStringLiteral("album"), albumRows);
            appendSection(QStringLiteral("Tracks"), QStringLiteral("track"), trackRows);
            out.materializeMs = materializeTimer.elapsed();
        }

        out.workerTotalMs = workerTimer.elapsed();
        queuePreparedSearchResultsFrame(std::move(out));
    }
}

void BridgeClient::pollInProcessBridge() {
    if (m_ffiBridge == nullptr) {
        return;
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    QElapsedTimer pollTimer;
    pollTimer.start();
#endif

    m_pollPlaybackChanged = false;
    m_pollSnapshotChanged = false;
    const BridgePollRunResult run = drainBridgeQueues(m_bridgePollBudgetMs);

    if (m_pollPlaybackChanged) {
        schedulePlaybackChanged();
    }
    if (m_pollSnapshotChanged) {
        scheduleSnapshotChanged();
    }

    if (m_ffiBridge != nullptr) {
        if (run.shouldContinueImmediately()) {
            scheduleBridgePoll(0);
        } else if (m_bridgeWakeNotifier != nullptr) {
            m_bridgeWakeNotifier->setEnabled(true);
        } else if (m_connected) {
            scheduleBridgePoll(16);
        }
    }

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        const double pollMs = static_cast<double>(pollTimer.nsecsElapsed()) / 1'000'000.0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool saturated = run.shouldContinueImmediately();
        if ((pollMs >= 6.0 || saturated)
            && shouldEmitUiProfileLog(nowMs, &m_lastBridgePollProfileLogMs, 200)) {
            FERROUS_PROFILE_LOG_DIAGNOSTIC(
                QStringLiteral("ui-prof"),
                QStringLiteral(
                    "bridge_poll ms=%1 analysis_frames=%2 analysis_kb=%3 tree_frames=%4 search_frames=%5 events=%6 budget_exhausted=%7 saturated=%8 wake_notifier=%9 timer_active=%10")
                    .arg(pollMs, 0, 'f', 2)
                    .arg(run.processedAnalysisFrames)
                    .arg(static_cast<double>(run.processedAnalysisBytes) / 1024.0, 0, 'f', 1)
                    .arg(run.processedTreeFrames)
                    .arg(run.processedSearchFrames)
                    .arg(run.processedEvents)
                    .arg(run.budgetExhausted ? 1 : 0)
                    .arg(saturated ? 1 : 0)
                    .arg(m_bridgeWakeNotifier != nullptr ? 1 : 0)
                    .arg(m_bridgePollTimer.isActive() ? 1 : 0));
        }
    }
#endif
}

QString BridgeClient::playbackState() const {
    return m_playbackState;
}

QString BridgeClient::positionText() const {
    return m_positionText;
}

QString BridgeClient::durationText() const {
    return m_durationText;
}

double BridgeClient::positionSeconds() const {
    return m_positionSeconds;
}

double BridgeClient::durationSeconds() const {
    return m_durationSeconds;
}

double BridgeClient::volume() const {
    return m_volume;
}

int BridgeClient::queueLength() const {
    return m_queueLength;
}

int BridgeClient::queueVersion() const {
    return m_queueVersion;
}

QString BridgeClient::queueDurationText() const {
    return m_queueDurationText;
}

QObject *BridgeClient::queueRows() const {
    return const_cast<QueueRowsModel *>(&m_queueRowsModel);
}

int BridgeClient::selectedQueueIndex() const {
    return m_selectedQueueIndex;
}

int BridgeClient::playingQueueIndex() const {
    return m_playingQueueIndex;
}

QString BridgeClient::currentTrackPath() const {
    return m_currentTrackPath;
}

QString BridgeClient::currentTrackCoverPath() const {
    return m_currentTrackCoverPath;
}

QString BridgeClient::currentTrackTitle() const {
    return m_currentTrackTitle;
}

QString BridgeClient::currentTrackArtist() const {
    return m_currentTrackArtist;
}

QString BridgeClient::currentTrackAlbum() const {
    return m_currentTrackAlbum;
}

QString BridgeClient::currentTrackGenre() const {
    return m_currentTrackGenre;
}

QVariant BridgeClient::currentTrackYear() const {
    return m_currentTrackYear;
}

QString BridgeClient::currentTrackFormatLabel() const {
    return m_currentTrackFormatLabel;
}

QString BridgeClient::currentTrackChannelLayoutText() const {
    return channelLayoutText(m_currentTrackChannels);
}

QString BridgeClient::currentTrackChannelLayoutIconKey() const {
    return channelLayoutIconKey(m_currentTrackChannels);
}

int BridgeClient::currentTrackSampleRateHz() const {
    return m_currentTrackSampleRateHz;
}

int BridgeClient::currentTrackBitDepth() const {
    return m_currentTrackBitDepth;
}

int BridgeClient::currentTrackCurrentBitrateKbps() const {
    return m_currentTrackCurrentBitrateKbps;
}

QByteArray BridgeClient::waveformPeaksPacked() const {
    return m_waveformPeaksPacked;
}

double BridgeClient::waveformCoverageSeconds() const {
    return m_waveformCoverageSeconds;
}

bool BridgeClient::waveformComplete() const {
    return m_waveformComplete;
}

bool BridgeClient::spectrogramReset() const {
    return m_spectrogramReset;
}

int BridgeClient::sampleRateHz() const {
    return m_sampleRateHz;
}

int BridgeClient::fftSize() const {
    return m_fftSize;
}

int BridgeClient::spectrogramViewMode() const {
    return m_spectrogramViewMode;
}

int BridgeClient::viewerFullscreenMode() const {
    return m_viewerFullscreenMode;
}

double BridgeClient::dbRange() const {
    return m_dbRange;
}

bool BridgeClient::logScale() const {
    return m_logScale;
}

int BridgeClient::repeatMode() const {
    return m_repeatMode;
}

bool BridgeClient::shuffleEnabled() const {
    return m_shuffleEnabled;
}

bool BridgeClient::showFps() const {
    return m_showFps;
}

bool BridgeClient::systemMediaControlsEnabled() const {
    return m_systemMediaControlsEnabled;
}

bool BridgeClient::lastFmScrobblingEnabled() const {
    return m_lastFmScrobblingEnabled;
}

bool BridgeClient::lastFmBuildConfigured() const {
    return m_lastFmBuildConfigured;
}

QString BridgeClient::lastFmUsername() const {
    return m_lastFmUsername;
}

int BridgeClient::lastFmAuthState() const {
    return m_lastFmAuthState;
}

int BridgeClient::lastFmPendingScrobbleCount() const {
    return m_lastFmPendingScrobbleCount;
}

QString BridgeClient::lastFmStatusText() const {
    return m_lastFmStatusText;
}

QStringList BridgeClient::libraryAlbums() const {
    return m_libraryAlbums;
}

QByteArray BridgeClient::libraryTreeBinary() const {
    return m_libraryTreeBinary;
}

int BridgeClient::libraryVersion() const {
    return m_libraryVersion;
}

bool BridgeClient::libraryScanInProgress() const {
    return m_libraryScanInProgress;
}

int BridgeClient::libraryRootCount() const {
    return m_libraryRootCount;
}

int BridgeClient::libraryTrackCount() const {
    return m_libraryTrackCount;
}

int BridgeClient::libraryArtistCount() const {
    return m_libraryArtistCount;
}

int BridgeClient::libraryAlbumCount() const {
    return m_libraryAlbumCount;
}

QStringList BridgeClient::libraryRoots() const {
    return m_libraryRoots;
}

QVariantList BridgeClient::libraryRootEntries() const {
    return m_libraryRootEntries;
}

int BridgeClient::librarySortMode() const {
    return m_librarySortMode;
}

QString BridgeClient::fileBrowserName() const {
    return m_fileBrowserName;
}

int BridgeClient::libraryScanRootsCompleted() const {
    return m_libraryScanRootsCompleted;
}

int BridgeClient::libraryScanRootsTotal() const {
    return m_libraryScanRootsTotal;
}

int BridgeClient::libraryScanDiscovered() const {
    return m_libraryScanDiscovered;
}

int BridgeClient::libraryScanProcessed() const {
    return m_libraryScanProcessed;
}

double BridgeClient::libraryScanFilesPerSecond() const {
    return m_libraryScanFilesPerSecond;
}

double BridgeClient::libraryScanEtaSeconds() const {
    return m_libraryScanEtaSeconds;
}

QVariantList BridgeClient::globalSearchArtistResults() const {
    return m_globalSearchArtistResults;
}

QVariantList BridgeClient::globalSearchAlbumResults() const {
    return m_globalSearchAlbumResults;
}

QVariantList BridgeClient::globalSearchTrackResults() const {
    return m_globalSearchTrackResults;
}

int BridgeClient::globalSearchArtistCount() const {
    return m_globalSearchArtistCount;
}

int BridgeClient::globalSearchAlbumCount() const {
    return m_globalSearchAlbumCount;
}

int BridgeClient::globalSearchTrackCount() const {
    return m_globalSearchTrackCount;
}

quint32 BridgeClient::globalSearchSeq() const {
    return m_globalSearchSeq;
}

QObject *BridgeClient::globalSearchModel() const {
    return const_cast<GlobalSearchResultsModel *>(&m_globalSearchModel);
}

QVariantList BridgeClient::itunesArtworkResults() const {
    return m_itunesArtworkResults;
}

bool BridgeClient::itunesArtworkLoading() const {
    return m_itunesArtworkLoading;
}

QString BridgeClient::itunesArtworkStatusText() const {
    return m_itunesArtworkStatusText;
}

QString BridgeClient::diagnosticsText() const {
    return m_diagnosticsText;
}

QString BridgeClient::diagnosticsLogPath() const {
    return m_diagnosticsLogPath;
}

bool BridgeClient::connected() const {
    return m_connected;
}

void BridgeClient::play() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("ui-prof"),
            QStringLiteral(
                "playback_command action=play state=%1 current_path=%2 playing_index=%3 selected_index=%4")
                .arg(m_playbackState)
                .arg(playbackLogPathField(m_currentTrackPath))
                .arg(m_playingQueueIndex)
                .arg(m_selectedQueueIndex));
    }
#endif
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdPlay));
}

void BridgeClient::pause() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("ui-prof"),
            QStringLiteral(
                "playback_command action=pause state=%1 current_path=%2 playing_index=%3 selected_index=%4")
                .arg(m_playbackState)
                .arg(playbackLogPathField(m_currentTrackPath))
                .arg(m_playingQueueIndex)
                .arg(m_selectedQueueIndex));
    }
#endif
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdPause));
}

void BridgeClient::stop() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("ui-prof"),
            QStringLiteral(
                "playback_command action=stop state=%1 current_path=%2 playing_index=%3 selected_index=%4")
                .arg(m_playbackState)
                .arg(playbackLogPathField(m_currentTrackPath))
                .arg(m_playingQueueIndex)
                .arg(m_selectedQueueIndex));
    }
#endif
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdStop));
}

void BridgeClient::next() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("ui-prof"),
            QStringLiteral(
                "playback_command action=next state=%1 current_path=%2 playing_index=%3 selected_index=%4")
                .arg(m_playbackState)
                .arg(playbackLogPathField(m_currentTrackPath))
                .arg(m_playingQueueIndex)
                .arg(m_selectedQueueIndex));
    }
#endif
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdNext));
}

void BridgeClient::previous() {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("ui-prof"),
            QStringLiteral(
                "playback_command action=previous state=%1 current_path=%2 playing_index=%3 selected_index=%4")
                .arg(m_playbackState)
                .arg(playbackLogPathField(m_currentTrackPath))
                .arg(m_playingQueueIndex)
                .arg(m_selectedQueueIndex));
    }
#endif
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdPrevious));
}

void BridgeClient::seek(double seconds) {
    const double target = std::max(0.0, seconds);
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    SpectrogramSeekTrace::noteSeekIssued(target);
    if (m_profileUiEnabled) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("ui-prof"),
            QStringLiteral(
                "playback_command action=seek target_s=%1 state=%2 current_path=%3 playing_index=%4 selected_index=%5")
                .arg(target, 0, 'f', 3)
                .arg(m_playbackState)
                .arg(playbackLogPathField(m_currentTrackPath))
                .arg(m_playingQueueIndex)
                .arg(m_selectedQueueIndex));
    }
#endif
    m_pendingSeek = true;
    m_pendingSeekTargetSeconds = target;
    m_pendingSeekUntilMs = QDateTime::currentMSecsSinceEpoch() + 900;
    bool changed = false;
    if (!qFuzzyCompare(m_positionSeconds + 1.0, target + 1.0)) {
        m_positionSeconds = target;
        changed = true;
    }
    const QString targetText = formatSeconds(target);
    if (m_positionText != targetText) {
        m_positionText = targetText;
        changed = true;
    }
    if (changed) {
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandF64(BinaryBridgeCodec::CmdSeek, target));
}

void BridgeClient::setVolume(double value) {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandF64(
        BinaryBridgeCodec::CmdSetVolume,
        std::clamp(value, 0.0, 1.0)));
}

void BridgeClient::setFftSize(int value) {
    const int clamped = std::clamp(value, 512, 8192);
    if (m_fftSize != clamped) {
        m_fftSize = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU32(
        BinaryBridgeCodec::CmdSetFftSize,
        static_cast<quint32>(clamped)));
}

void BridgeClient::setSpectrogramViewMode(int value) {
    const int clamped = std::clamp(value, 0, 1);
    if (m_spectrogramViewMode != clamped) {
        m_spectrogramViewMode = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetSpectrogramViewMode,
        static_cast<quint8>(clamped)));
}

void BridgeClient::setViewerFullscreenMode(int value) {
    const int clamped = std::clamp(value, 0, 1);
    if (m_viewerFullscreenMode != clamped) {
        m_viewerFullscreenMode = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetViewerFullscreenMode,
        static_cast<quint8>(clamped)));
}

void BridgeClient::setDbRange(double value) {
    const double clamped = std::clamp(value, 50.0, 120.0);
    if (!qFuzzyCompare(m_dbRange + 1.0, clamped + 1.0)) {
        m_dbRange = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandF32(
        BinaryBridgeCodec::CmdSetDbRange,
        static_cast<float>(clamped)));
}

void BridgeClient::setLogScale(bool value) {
    if (m_logScale != value) {
        m_logScale = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetLogScale,
        static_cast<quint8>(value ? 1 : 0)));
}

void BridgeClient::setRepeatMode(int mode) {
    const int clamped = std::clamp(mode, 0, 2);
    if (m_repeatMode != clamped) {
        m_repeatMode = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetRepeatMode,
        static_cast<quint8>(clamped)));
}

void BridgeClient::setShuffleEnabled(bool value) {
    if (m_shuffleEnabled != value) {
        m_shuffleEnabled = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetShuffle,
        static_cast<quint8>(value ? 1 : 0)));
}

void BridgeClient::setShowFps(bool value) {
    if (m_showFps != value) {
        m_showFps = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetShowFps,
        static_cast<quint8>(value ? 1 : 0)));
}

void BridgeClient::setSystemMediaControlsEnabled(bool value) {
    if (m_systemMediaControlsEnabled != value) {
        m_systemMediaControlsEnabled = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetSystemMediaControls,
        static_cast<quint8>(value ? 1 : 0)));
}

void BridgeClient::setLastFmScrobblingEnabled(bool value) {
    if (m_lastFmScrobblingEnabled != value) {
        m_lastFmScrobblingEnabled = value;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU8(
        BinaryBridgeCodec::CmdSetLastFmScrobblingEnabled,
        static_cast<quint8>(value ? 1 : 0)));
}

void BridgeClient::beginLastFmAuth() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(
        BinaryBridgeCodec::CmdBeginLastFmAuth));
}

void BridgeClient::completeLastFmAuth() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(
        BinaryBridgeCodec::CmdCompleteLastFmAuth));
}

void BridgeClient::disconnectLastFm() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(
        BinaryBridgeCodec::CmdDisconnectLastFm));
}

void BridgeClient::playAt(int index) {
    if (index < 0) {
        return;
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("ui-prof"),
            QStringLiteral(
                "playback_command action=play_at index=%1 state=%2 current_path=%3 playing_index=%4 selected_index=%5")
                .arg(index)
                .arg(m_playbackState)
                .arg(playbackLogPathField(m_currentTrackPath))
                .arg(m_playingQueueIndex)
                .arg(m_selectedQueueIndex));
    }
#endif
    m_pendingQueueSelection = index;
    m_pendingQueueSelectionUntilMs = QDateTime::currentMSecsSinceEpoch() + 700;
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU32(
        BinaryBridgeCodec::CmdPlayAt,
        static_cast<quint32>(index)));
}

void BridgeClient::selectQueueIndex(int index) {
    if (index < 0) {
        return;
    }
    if (m_selectedQueueIndex == index) {
        return;
    }
    m_selectedQueueIndex = index;
    scheduleSnapshotChanged();
    m_pendingQueueSelection = index;
    m_pendingQueueSelectionUntilMs = QDateTime::currentMSecsSinceEpoch() + 700;
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandI32(
        BinaryBridgeCodec::CmdSelectQueue,
        static_cast<qint32>(index)));
}

void BridgeClient::removeAt(int index) {
    if (index < 0) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandU32(
        BinaryBridgeCodec::CmdRemoveAt,
        static_cast<quint32>(index)));
}

void BridgeClient::moveQueue(int from, int to) {
    if (from < 0 || to < 0) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandMoveQueue(
        static_cast<quint32>(from),
        static_cast<quint32>(to)));
}

void BridgeClient::clearQueue() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdClearQueue));
}

void BridgeClient::replaceAlbumAt(int index) {
    if (index < 0 || index >= m_libraryAlbumTrackPaths.size()) {
        return;
    }
    replaceWithPaths(m_libraryAlbumTrackPaths[index]);
}

void BridgeClient::appendAlbumAt(int index) {
    if (index < 0 || index >= m_libraryAlbumTrackPaths.size()) {
        return;
    }
    appendPaths(m_libraryAlbumTrackPaths[index]);
}

void BridgeClient::playTrack(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("ui-prof"),
            QStringLiteral(
                "playback_command action=play_track target_path=%1 state=%2 current_path=%3 playing_index=%4 selected_index=%5")
                .arg(playbackLogPathField(normalized))
                .arg(m_playbackState)
                .arg(playbackLogPathField(m_currentTrackPath))
                .arg(m_playingQueueIndex)
                .arg(m_selectedQueueIndex));
    }
#endif
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(
        BinaryBridgeCodec::CmdPlayTrack,
        normalized));
}

void BridgeClient::appendTrack(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(
        BinaryBridgeCodec::CmdAddTrack,
        normalized));
}

void BridgeClient::replaceAlbumByKey(const QString &artist, const QString &album) {
    if (artist.trimmed().isEmpty() || album.trimmed().isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringPair(
        BinaryBridgeCodec::CmdReplaceAlbumByKey,
        artist,
        album));
}

void BridgeClient::appendAlbumByKey(const QString &artist, const QString &album) {
    if (artist.trimmed().isEmpty() || album.trimmed().isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringPair(
        BinaryBridgeCodec::CmdAppendAlbumByKey,
        artist,
        album));
}

void BridgeClient::replaceArtistByName(const QString &artist) {
    if (artist.trimmed().isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(
        BinaryBridgeCodec::CmdReplaceArtistByKey,
        artist));
}

void BridgeClient::appendArtistByName(const QString &artist) {
    if (artist.trimmed().isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(
        BinaryBridgeCodec::CmdAppendArtistByKey,
        artist));
}

void BridgeClient::replaceAllLibraryTracks() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(
        BinaryBridgeCodec::CmdReplaceAllTracks));
}

void BridgeClient::appendAllLibraryTracks() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(
        BinaryBridgeCodec::CmdAppendAllTracks));
}

void BridgeClient::replaceWithPaths(const QStringList &paths) {
    QStringList sanitized;
    sanitized.reserve(paths.size());
    for (const QString &path : paths) {
        const QString normalized = normalizeLocalPathArg(path);
        if (!normalized.isEmpty()) {
            sanitized.push_back(normalized);
        }
    }
    if (sanitized.isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringList(
        BinaryBridgeCodec::CmdReplaceAlbum,
        sanitized));
}

void BridgeClient::appendPaths(const QStringList &paths) {
    QStringList sanitized;
    sanitized.reserve(paths.size());
    for (const QString &path : paths) {
        const QString normalized = normalizeLocalPathArg(path);
        if (!normalized.isEmpty()) {
            sanitized.push_back(normalized);
        }
    }
    if (sanitized.isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringList(
        BinaryBridgeCodec::CmdAppendAlbum,
        sanitized));
}

QString BridgeClient::libraryAlbumCoverAt(int index) const {
    if (index < 0 || index >= m_libraryAlbumCoverPaths.size()) {
        return {};
    }
    const QString path = m_libraryAlbumCoverPaths[index];
    if (path.isEmpty()) {
        return {};
    }
    return QUrl::fromLocalFile(path).toString();
}

QString BridgeClient::libraryThumbnailSource(const QString &path) const {
    const QString normalizedPath = normalizeLocalPathArg(path);
    if (normalizedPath.isEmpty()) {
        return {};
    }

    if (const auto it = m_libraryThumbnailSourceCache.constFind(normalizedPath);
        it != m_libraryThumbnailSourceCache.constEnd()) {
        return it.value();
    }

    const QString result = cacheOnlyLocalFileUrl(normalizedPath);
    m_libraryThumbnailSourceCache.insert(normalizedPath, result);
    if (m_libraryThumbnailSourceCache.size() > 4096) {
        m_libraryThumbnailSourceCache.clear();
        m_libraryThumbnailSourceCache.insert(normalizedPath, result);
    }
    return result;
}

QString BridgeClient::queuePathAt(int index) const {
    if (index < 0 || index >= m_queuePaths.size()) {
        return {};
    }
    return m_queuePaths[index];
}

QVariant BridgeClient::queueTrackNumberAt(int index) const {
    return m_queueRowsModel.trackNumberAt(index);
}

void BridgeClient::addLibraryRoot(const QString &path, const QString &name) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    m_pendingAddRootPath = normalized;
    m_pendingAddRootIssuedMs = QDateTime::currentMSecsSinceEpoch();
    sendLibraryRootCommand(
        BinaryBridgeCodec::CmdAddRoot,
        normalized,
        normalizeRootNameArg(name));
}

void BridgeClient::setLibraryRootName(const QString &path, const QString &name) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    sendLibraryRootCommand(
        BinaryBridgeCodec::CmdRenameRoot,
        normalized,
        normalizeRootNameArg(name));
}

void BridgeClient::removeLibraryRoot(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    sendLibraryRootCommand(BinaryBridgeCodec::CmdRemoveRoot, normalized);
}

void BridgeClient::rescanLibraryRoot(const QString &path) {
    const QString normalized = normalizeLocalPathArg(path);
    if (normalized.isEmpty()) {
        return;
    }
    sendLibraryRootCommand(BinaryBridgeCodec::CmdRescanRoot, normalized);
}

void BridgeClient::rescanAllLibraryRoots() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdRescanAll));
}

void BridgeClient::setLibraryNodeExpanded(const QString &key, bool expanded) {
    const QString normalized = key.trimmed();
    if (normalized.isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringBool(
        BinaryBridgeCodec::CmdSetNodeExpanded,
        normalized,
        expanded));
}

void BridgeClient::setLibrarySortMode(int mode) {
    const int clamped = std::clamp(mode, 0, 1);
    if (m_librarySortMode != clamped) {
        m_librarySortMode = clamped;
        scheduleSnapshotChanged();
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandI32(
        BinaryBridgeCodec::CmdSetLibrarySortMode,
        static_cast<qint32>(clamped)));
}

void BridgeClient::setGlobalSearchQuery(const QString &query) {
    const QString nextQuery = canonicalizeSearchQuery(query);
    const int trimmedChars = nextQuery.size();
    const int nextDebounceMs =
        trimmedChars <= m_globalSearchShortDebounceChars
        ? m_globalSearchShortDebounceMs
        : m_globalSearchDebounceMs;
    if (m_globalSearchDebounceTimer.interval() != nextDebounceMs) {
        m_globalSearchDebounceTimer.setInterval(nextDebounceMs);
    }
    if (!m_globalSearchDebounceTimer.isActive()
        && m_pendingGlobalSearchQuery == nextQuery
        && m_lastGlobalSearchQuerySent == nextQuery) {
        return;
    }
    if (m_pendingGlobalSearchQuery == nextQuery && m_globalSearchDebounceTimer.isActive()) {
        return;
    }
    m_pendingGlobalSearchQuery = nextQuery;

    if (nextQuery.trimmed().isEmpty()) {
        bool changed = false;
        if (m_publishLegacySearchLists) {
            if (!m_globalSearchArtistResults.isEmpty()) {
                m_globalSearchArtistResults.clear();
                changed = true;
            }
            if (!m_globalSearchAlbumResults.isEmpty()) {
                m_globalSearchAlbumResults.clear();
                changed = true;
            }
            if (!m_globalSearchTrackResults.isEmpty()) {
                m_globalSearchTrackResults.clear();
                changed = true;
            }
        }
        if (m_globalSearchArtistCount != 0 || m_globalSearchAlbumCount != 0
            || m_globalSearchTrackCount != 0) {
            m_globalSearchArtistCount = 0;
            m_globalSearchAlbumCount = 0;
            m_globalSearchTrackCount = 0;
            changed = true;
        }
        if (m_globalSearchModel.rowCount() > 0) {
            m_globalSearchModel.replaceRows({});
            changed = true;
        }
        if (changed) {
            emit globalSearchResultsChanged();
        }
        m_globalSearchSentAtMs.clear();
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("search"),
            QStringLiteral("clear query"));
        m_globalSearchDebounceTimer.stop();
        flushGlobalSearchQuery();
        return;
    }

    m_globalSearchDebounceTimer.start();
}

void BridgeClient::searchCurrentTrackArtworkSuggestions() {
    const QString album = m_currentTrackAlbum.trimmed();
    const QString artist = m_currentTrackArtist.trimmed();

    cancelItunesArtworkRequests();
    m_itunesArtworkGeneration++;
    m_itunesArtworkCandidates.clear();
    m_itunesArtworkResults.clear();

    if (album.isEmpty() || artist.isEmpty()) {
        m_itunesArtworkLoading = false;
        m_itunesArtworkStatusText = QStringLiteral("Album and artist metadata are required.");
        emit itunesArtworkChanged();
        return;
    }

    m_itunesArtworkLoading = true;
    m_itunesArtworkStatusText = QStringLiteral("Searching iTunes...");
    emit itunesArtworkChanged();

    const quint64 generation = m_itunesArtworkGeneration;
    struct SearchAggregation {
        QVector<ItunesArtworkCandidate> candidates;
        QSet<QString> dedupKeys;
        QSet<qint64> artistLookupIds;
        int nextApiOrder{0};
        int pendingRequests{0};
        int successfulRequests{0};
        QString firstError;
    };

    QStringList searchTerms;
    QSet<QString> seenSearchTerms;
    const QStringList rawTerms{
        artist + QStringLiteral(" ") + album,
        album,
        artist,
    };
    for (const QString &rawTerm : rawTerms) {
        const QString term = rawTerm.simplified();
        const QString termKey = normalizedItunesMatchKey(term);
        if (term.isEmpty() || seenSearchTerms.contains(termKey)) {
            continue;
        }
        seenSearchTerms.insert(termKey);
        searchTerms.push_back(term);
    }

    auto aggregation = std::make_shared<SearchAggregation>();
    aggregation->pendingRequests = searchTerms.size();

    auto albumPreviewStrings = [](const QJsonArray &results) {
        QStringList rawPreview;
        const int rawLimit = std::min(15, static_cast<int>(results.size()));
        rawPreview.reserve(rawLimit);
        for (int i = 0; i < rawLimit; ++i) {
            const QJsonObject previewObj = results[i].toObject();
            rawPreview.push_back(
                QStringLiteral("[%1] %2 | %3")
                    .arg(i)
                    .arg(previewObj.value(QStringLiteral("collectionName")).toString().trimmed(),
                         previewObj.value(QStringLiteral("artistName")).toString().trimmed()));
        }
        return rawPreview;
    };

    auto addAlbumResults = [aggregation, album, artist](const QJsonArray &results) {
        for (const QJsonValue &value : results) {
            const QJsonObject obj = value.toObject();
            const QString artworkUrl100 = obj.value(QStringLiteral("artworkUrl100")).toString();
            if (artworkUrl100.trimmed().isEmpty()) {
                continue;
            }
            const QStringList assetUrls = deriveItunesArtworkUrls(artworkUrl100);
            if (assetUrls.isEmpty()) {
                continue;
            }

            const qint64 collectionId = static_cast<qint64>(
                obj.value(QStringLiteral("collectionId")).toDouble(-1));
            QString dedupKey;
            if (collectionId > 0) {
                dedupKey = QStringLiteral("id:%1").arg(collectionId);
            } else {
                dedupKey = obj.value(QStringLiteral("collectionViewUrl")).toString().trimmed();
                if (dedupKey.isEmpty()) {
                    dedupKey = QStringLiteral("%1|%2|%3")
                        .arg(obj.value(QStringLiteral("collectionName")).toString().trimmed(),
                             obj.value(QStringLiteral("artistName")).toString().trimmed(),
                             artworkUrl100.trimmed());
                }
            }
            if (aggregation->dedupKeys.contains(dedupKey)) {
                continue;
            }
            aggregation->dedupKeys.insert(dedupKey);

            ItunesArtworkCandidate candidate;
            candidate.albumTitle = obj.value(QStringLiteral("collectionName")).toString().trimmed();
            candidate.artistName = obj.value(QStringLiteral("artistName")).toString().trimmed();
            candidate.collectionUrl = obj.value(QStringLiteral("collectionViewUrl")).toString().trimmed();
            candidate.previewUrl = deriveItunesPreviewUrl(artworkUrl100);
            candidate.assetUrls = assetUrls;
            candidate.rankGroup = itunesMatchRankGroup(
                candidate.albumTitle,
                candidate.artistName,
                album,
                artist);
            candidate.apiOrder = aggregation->nextApiOrder++;
            aggregation->candidates.push_back(std::move(candidate));
        }
    };

    auto finalizeSearch = [this, aggregation]() {
        std::stable_sort(
            aggregation->candidates.begin(),
            aggregation->candidates.end(),
            [](const ItunesArtworkCandidate &lhs, const ItunesArtworkCandidate &rhs) {
                if (lhs.rankGroup != rhs.rankGroup) {
                    return lhs.rankGroup < rhs.rankGroup;
                }
                return lhs.apiOrder < rhs.apiOrder;
        });
        if (aggregation->candidates.size() > kItunesArtworkResultDisplayLimit) {
            aggregation->candidates.resize(kItunesArtworkResultDisplayLimit);
        }

        m_itunesArtworkCandidates = aggregation->candidates;
        m_itunesArtworkResults.clear();
        if (aggregation->candidates.isEmpty()) {
            m_itunesArtworkLoading = false;
            if (aggregation->successfulRequests <= 0 && !aggregation->firstError.isEmpty()) {
                m_itunesArtworkStatusText = aggregation->firstError;
            } else {
                m_itunesArtworkStatusText = QStringLiteral("No iTunes artwork suggestions found.");
            }
            emit itunesArtworkChanged();
            return;
        }

        QVariantList rows;
        rows.reserve(aggregation->candidates.size());
        for (int i = 0; i < aggregation->candidates.size(); ++i) {
            const auto &candidate = aggregation->candidates[i];
            QVariantMap row;
            row.insert(QStringLiteral("albumTitle"), candidate.albumTitle);
            row.insert(QStringLiteral("artistName"), candidate.artistName);
            row.insert(QStringLiteral("collectionUrl"), candidate.collectionUrl);
            row.insert(QStringLiteral("previewSource"), candidate.previewUrl);
            row.insert(QStringLiteral("normalizedPath"), QString());
            row.insert(QStringLiteral("normalizedUrl"), QString());
            row.insert(QStringLiteral("downloadPath"), QString());
            row.insert(QStringLiteral("sortIndex"), i);
            row.insert(QStringLiteral("usedFallback"), false);
            row.insert(QStringLiteral("assetReady"), false);
            row.insert(QStringLiteral("assetLoading"), false);
            row.insert(QStringLiteral("assetError"), QString());
            row.insert(
                QStringLiteral("detailStatusText"),
                QStringLiteral("High-resolution file info loads on preview or apply."));
            rows.push_back(row);
        }

        m_itunesArtworkResults = rows;
        m_itunesArtworkLoading = false;
        m_itunesArtworkStatusText =
            QStringLiteral("Found %1 suggestion(s). High-resolution artwork loads on preview/apply.")
                .arg(rows.size());
        emit itunesArtworkChanged();
    };

    auto dispatchArtistLookup = [this, generation, album, artist, aggregation, finalizeSearch, albumPreviewStrings, addAlbumResults](qint64 artistId) {
        if (artistId <= 0 || aggregation->artistLookupIds.contains(artistId)) {
            return;
        }
        aggregation->artistLookupIds.insert(artistId);
        aggregation->pendingRequests += 1;

        QUrl url(QStringLiteral("https://itunes.apple.com/lookup"));
        QUrlQuery query(url);
        query.addQueryItem(QStringLiteral("id"), QString::number(artistId));
        query.addQueryItem(QStringLiteral("country"), QStringLiteral("fi"));
        query.addQueryItem(QStringLiteral("entity"), QStringLiteral("album"));
        query.addQueryItem(
            QStringLiteral("limit"),
            QString::number(kItunesArtworkSearchRequestLimit));
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setTransferTimeout(30000);
        auto *reply = m_itunesArtworkNetwork.get(request);
        m_itunesArtworkReplies.insert(reply);

        connect(reply, &QNetworkReply::finished, this, [this, reply, generation, aggregation, finalizeSearch, artistId, albumPreviewStrings, addAlbumResults]() {
            m_itunesArtworkReplies.remove(reply);
            const bool stale = generation != m_itunesArtworkGeneration;
            const auto error = reply->error();
            const QString errorText = reply->errorString();
            const QByteArray payload = reply->readAll();
            reply->deleteLater();
            if (stale) {
                return;
            }

            aggregation->pendingRequests = std::max(0, aggregation->pendingRequests - 1);
            if (error != QNetworkReply::NoError) {
            } else {
                aggregation->successfulRequests += 1;
                const auto doc = QJsonDocument::fromJson(payload);
                const auto results = doc.object().value(QStringLiteral("results")).toArray();
                addAlbumResults(results);
            }

            if (aggregation->pendingRequests == 0) {
                finalizeSearch();
            }
        });
    };

    for (const QString &searchTerm : searchTerms) {
        QUrl url(QStringLiteral("https://itunes.apple.com/search"));
        QUrlQuery query(url);
        query.addQueryItem(QStringLiteral("term"), searchTerm);
        query.addQueryItem(QStringLiteral("country"), QStringLiteral("fi"));
        query.addQueryItem(QStringLiteral("entity"), QStringLiteral("album"));
        query.addQueryItem(
            QStringLiteral("limit"),
            QString::number(kItunesArtworkSearchRequestLimit));
        url.setQuery(query);

        QNetworkRequest request(url);
        request.setTransferTimeout(30000);
        auto *reply = m_itunesArtworkNetwork.get(request);
        m_itunesArtworkReplies.insert(reply);

        connect(reply, &QNetworkReply::finished, this, [this, reply, generation, album, artist, aggregation, finalizeSearch, searchTerm, albumPreviewStrings, addAlbumResults, dispatchArtistLookup]() {
            m_itunesArtworkReplies.remove(reply);
            const bool stale = generation != m_itunesArtworkGeneration;
            const auto error = reply->error();
            const QString errorText = reply->errorString();
            const QByteArray payload = reply->readAll();
            reply->deleteLater();
            if (stale) {
                return;
            }

            aggregation->pendingRequests = std::max(0, aggregation->pendingRequests - 1);
            if (error != QNetworkReply::NoError) {
                if (aggregation->firstError.isEmpty()) {
                    aggregation->firstError =
                        QStringLiteral("iTunes search failed: %1").arg(errorText);
                }
            } else {
                aggregation->successfulRequests += 1;
                const auto doc = QJsonDocument::fromJson(payload);
                const auto results = doc.object().value(QStringLiteral("results")).toArray();
                addAlbumResults(results);

                if (normalizedItunesMatchKey(searchTerm) == normalizedItunesMatchKey(artist)) {
                    for (const QJsonValue &value : results) {
                        const QJsonObject obj = value.toObject();
                        const QString resultArtist = obj.value(QStringLiteral("artistName")).toString().trimmed();
                        if (normalizedItunesMatchKey(resultArtist) != normalizedItunesMatchKey(artist)) {
                            continue;
                        }
                        const qint64 artistId = static_cast<qint64>(
                            obj.value(QStringLiteral("artistId")).toDouble(-1));
                        if (artistId > 0) {
                            dispatchArtistLookup(artistId);
                        }
                    }
                }
            }

            if (aggregation->pendingRequests == 0) {
                finalizeSearch();
            }
        });
    }
}

void BridgeClient::clearItunesArtworkSuggestions() {
    cancelItunesArtworkRequests();
    m_itunesArtworkGeneration++;
    m_itunesArtworkCandidates.clear();
    m_itunesArtworkResults.clear();
    m_itunesArtworkLoading = false;
    m_itunesArtworkStatusText.clear();
    emit itunesArtworkChanged();
}

QVariantMap BridgeClient::itunesArtworkResultAt(int index) const {
    if (index < 0 || index >= m_itunesArtworkResults.size()) {
        return {};
    }
    return m_itunesArtworkResults[index].toMap();
}

void BridgeClient::requestImageFileDetails(const QString &path) {
    const QString normalizedPath = normalizeLocalPathArg(path);
    if (normalizedPath.isEmpty()) {
        return;
    }
    if (m_imageFileDetailsCache.contains(normalizedPath)
        || m_pendingImageFileDetailsPaths.contains(normalizedPath)) {
        return;
    }

    m_pendingImageFileDetailsPaths.insert(normalizedPath);
    const QPointer<BridgeClient> self(this);
    std::thread([self, normalizedPath]() {
        QVariantMap details = readImageFileDetails(normalizedPath);
        QCoreApplication *app = QCoreApplication::instance();
        if (app == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            app,
            [self, normalizedPath, details = std::move(details)]() mutable {
                if (!self) {
                    return;
                }
                self->applyImageFileDetailsResult(normalizedPath, std::move(details));
            },
            Qt::QueuedConnection);
    }).detach();
}

QVariantMap BridgeClient::cachedImageFileDetails(const QString &path) const {
    const QString normalizedPath = normalizeLocalPathArg(path);
    if (normalizedPath.isEmpty()) {
        return {};
    }
    const auto it = m_imageFileDetailsCache.constFind(normalizedPath);
    if (it == m_imageFileDetailsCache.constEnd()) {
        return {};
    }
    return it.value();
}

void BridgeClient::applyImageFileDetailsResult(const QString &requestedPath, QVariantMap details) {
    m_pendingImageFileDetailsPaths.remove(requestedPath);
    cacheImageFileDetails(requestedPath, details);
    emit imageFileDetailsChanged(requestedPath);

    const QString resolvedPath = details.value(QStringLiteral("path")).toString().trimmed();
    if (!resolvedPath.isEmpty() && resolvedPath != requestedPath) {
        emit imageFileDetailsChanged(resolvedPath);
    }
}

void BridgeClient::cacheImageFileDetails(const QString &requestedPath, const QVariantMap &details) {
    const QString normalizedRequestPath = normalizeLocalPathArg(requestedPath);
    if (!normalizedRequestPath.isEmpty()) {
        m_imageFileDetailsCache.insert(normalizedRequestPath, details);
    }

    const QString resolvedPath = details.value(QStringLiteral("path")).toString().trimmed();
    if (!resolvedPath.isEmpty()) {
        m_imageFileDetailsCache.insert(resolvedPath, details);
    }

    if (m_imageFileDetailsCache.size() > 4096) {
        QHash<QString, QVariantMap> retained;
        if (!normalizedRequestPath.isEmpty()) {
            retained.insert(normalizedRequestPath, details);
        }
        if (!resolvedPath.isEmpty()) {
            retained.insert(resolvedPath, details);
        }
        m_imageFileDetailsCache = std::move(retained);
    }
}

void BridgeClient::prepareItunesArtworkSuggestion(int index) {
    if (index < 0 || index >= m_itunesArtworkCandidates.size()) {
        return;
    }

    QVariantMap row = itunesArtworkResultAt(index);
    if (row.isEmpty()) {
        return;
    }
    if (row.value(QStringLiteral("assetReady")).toBool()
        || row.value(QStringLiteral("assetLoading")).toBool()) {
        return;
    }

    row.insert(QStringLiteral("assetLoading"), true);
    row.insert(QStringLiteral("assetError"), QString());
    row.insert(QStringLiteral("detailStatusText"), QStringLiteral("Loading high-resolution artwork..."));
    updateItunesArtworkResult(index, row);
    startItunesArtworkAssetDownload(index);
}

void BridgeClient::applyItunesArtworkSuggestion(int index) {
    if (index < 0 || index >= m_itunesArtworkResults.size()) {
        return;
    }
    const QVariantMap row = m_itunesArtworkResults[index].toMap();
    const QString normalizedPath = normalizeLocalPathArg(
        row.value(QStringLiteral("normalizedPath")).toString());
    const QString trackPath = normalizeLocalPathArg(m_currentTrackPath);
    if (normalizedPath.isEmpty() || trackPath.isEmpty()) {
        if (trackPath.isEmpty()) {
            return;
        }
        prepareItunesArtworkSuggestion(index);
        return;
    }

    m_trackCoverByPath.remove(trackPath);
    const QString dirPath = trackDirectoryPath(trackPath);
    if (!dirPath.isEmpty()) {
        m_trackCoverByDirectory.remove(dirPath);
    }
    m_pendingAppliedArtworkTrackPath = trackPath;

    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringPair(
        BinaryBridgeCodec::CmdApplyAlbumArt,
        trackPath,
        normalizedPath));
}

void BridgeClient::openInFileBrowser(const QString &path) {
    if (path.trimmed().isEmpty()) {
        return;
    }
    const bool ok = openUrlInFileBrowser(path, false);
    if (!ok) {
        emit bridgeError(QStringLiteral("failed to open in %1: %2")
                             .arg(m_fileBrowserName, path));
    }
}

void BridgeClient::openContainingFolder(const QString &path) {
    if (path.trimmed().isEmpty()) {
        return;
    }
    const bool ok = openUrlInFileBrowser(path, true);
    if (!ok) {
        emit bridgeError(QStringLiteral("failed to open containing folder in %1: %2")
                             .arg(m_fileBrowserName, path));
    }
}

void BridgeClient::refreshEditedPaths(const QStringList &paths) {
    QStringList sanitized;
    sanitized.reserve(paths.size());
    for (const QString &path : paths) {
        const QString normalized = normalizeLocalPathArg(path);
        if (!normalized.isEmpty()) {
            sanitized.push_back(normalized);
        }
    }
    if (sanitized.isEmpty()) {
        return;
    }
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringList(
        BinaryBridgeCodec::CmdRefreshEditedPaths,
        sanitized));
}

QByteArray BridgeClient::renameEditedFiles(const QByteArray &payload) {
    if (m_ffiBridge == nullptr || payload.isEmpty()) {
        return {};
    }
    std::size_t len = 0;
    std::uint8_t *raw = ferrous_ffi_bridge_rename_edited_files(
        m_ffiBridge,
        reinterpret_cast<const std::uint8_t *>(payload.constData()),
        static_cast<std::size_t>(payload.size()),
        &len);
    QByteArray response;
    if (raw != nullptr && len > 0) {
        response = QByteArray(reinterpret_cast<const char *>(raw), static_cast<int>(len));
        ferrous_ffi_tag_editor_free_buffer(raw, len);
    }
    return response;
}

QVariantMap BridgeClient::imageFileDetails(const QString &path) const {
    return readImageFileDetails(path);
}

void BridgeClient::scanRoot(const QString &path) {
    addLibraryRoot(path);
}

void BridgeClient::scanDefaultMusicRoot() {
    const QString home = QDir::homePath();
    const QString music = QDir(home).filePath(QStringLiteral("Music"));
    scanRoot(music);
}

QVariantMap BridgeClient::takeSpectrogramRowsDeltaPacked(int maxRowsPerChannel) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    QElapsedTimer deltaTimer;
    deltaTimer.start();
#endif
    QVariantMap out;
    QVariantList channels;
    channels.reserve(m_spectrogramChannels.size());
    const int clampedMaxRows = maxRowsPerChannel > 0 ? maxRowsPerChannel : std::numeric_limits<int>::max();
    int rowsToTake = std::numeric_limits<int>::max();
    for (const auto &channel : m_spectrogramChannels) {
        if (channel.packedRowsCount <= 0 || channel.packedBins <= 0 || channel.packedRows.isEmpty()) {
            continue;
        }
        rowsToTake = std::min(rowsToTake, channel.packedRowsCount);
    }
    if (rowsToTake == std::numeric_limits<int>::max()) {
        rowsToTake = 0;
    } else {
        rowsToTake = std::min(rowsToTake, clampedMaxRows);
    }

    int totalRows = 0;
    qsizetype totalBytes = 0;
    bool hasRemainingRows = false;
    for (auto &channel : m_spectrogramChannels) {
        if (channel.packedRowsCount <= 0 || channel.packedBins <= 0 || channel.packedRows.isEmpty()) {
            channel.packedRows.clear();
            channel.packedRowsCount = 0;
            continue;
        }
        const int takeRows = std::min(channel.packedRowsCount, rowsToTake);
        if (takeRows <= 0) {
            hasRemainingRows = hasRemainingRows || channel.packedRowsCount > 0;
            continue;
        }
        const qsizetype takeBytes = static_cast<qsizetype>(takeRows)
            * static_cast<qsizetype>(channel.packedBins);
        QVariantMap channelMap;
        channelMap.insert(QStringLiteral("label"), channel.label);
        channelMap.insert(QStringLiteral("rows"), takeRows);
        channelMap.insert(QStringLiteral("bins"), channel.packedBins);
        channelMap.insert(
            QStringLiteral("data"),
            channel.packedRows.left(static_cast<int>(takeBytes)));
        channels.push_back(channelMap);
        totalRows += takeRows;
        totalBytes += takeBytes;
        if (takeRows >= channel.packedRowsCount) {
            channel.packedRows.clear();
            channel.packedRowsCount = 0;
        } else {
            channel.packedRows.remove(0, takeBytes);
            channel.packedRowsCount -= takeRows;
            hasRemainingRows = true;
        }
    }
    out.insert(QStringLiteral("channels"), channels);
    const bool reset = m_spectrogramReset && !channels.isEmpty();
    const bool seedHistory = m_spectrogramSeedBurstRowsRemaining > 0 && !channels.isEmpty();
    out.insert(QStringLiteral("reset"), reset);
    out.insert(QStringLiteral("seedHistory"), seedHistory);
    if (seedHistory) {
        m_spectrogramSeedBurstRowsRemaining = std::max(0, m_spectrogramSeedBurstRowsRemaining - rowsToTake);
    }
    if (reset) {
        m_spectrogramReset = false;
    }
    if (!hasRemainingRows) {
        m_spectrogramChannels.clear();
    } else {
        scheduleAnalysisChanged();
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        const double deltaMs = static_cast<double>(deltaTimer.nsecsElapsed()) / 1'000'000.0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (SpectrogramSeekTrace::isActive(nowMs)
            && shouldEmitUiProfileLog(nowMs, &m_lastSpectrogramDeltaProfileLogMs, 120)) {
            FERROUS_PROFILE_LOG_DIAGNOSTIC(
                QStringLiteral("ui-prof"),
                QStringLiteral(
                    "seek_spectrogram_delta gen=%1 rows=%2 channels=%3 reset=%4 seed=%5 remaining=%6 kb=%7")
                    .arg(SpectrogramSeekTrace::currentGeneration())
                    .arg(totalRows)
                    .arg(channels.size())
                    .arg(reset ? 1 : 0)
                    .arg(seedHistory ? 1 : 0)
                    .arg(hasRemainingRows ? 1 : 0)
                    .arg(static_cast<double>(totalBytes) / 1024.0, 0, 'f', 1));
        }
        if ((deltaMs >= 2.0 || totalRows >= 48)
            && shouldEmitUiProfileLog(nowMs, &m_lastSpectrogramDeltaProfileLogMs, 200)) {
            FERROUS_PROFILE_LOG_DIAGNOSTIC(
                QStringLiteral("ui-prof"),
                QStringLiteral("spectrogram_delta ms=%1 channels=%2 rows=%3 kb=%4")
                    .arg(deltaMs, 0, 'f', 2)
                    .arg(channels.size())
                    .arg(totalRows)
                    .arg(static_cast<double>(totalBytes) / 1024.0, 0, 'f', 1));
        }
    }
#endif
    return out;
}

void BridgeClient::clearSpectrogramDeltaState() {
    m_spectrogramChannels.clear();
    m_spectrogramReset = false;
    m_spectrogramSeedBurstRowsRemaining = 0;
}

void BridgeClient::requestSnapshot() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdRequestSnapshot));
}

void BridgeClient::shutdown() {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdShutdown));
}

void BridgeClient::clearDiagnostics() {
    m_diagnosticsLines.clear();
    m_diagnosticsText.clear();
    if (!m_diagnosticsLogPath.isEmpty()) {
        QFile::remove(m_diagnosticsLogPath);
    }
    emit diagnosticsChanged();
    logDiagnostic(QStringLiteral("ui"), QStringLiteral("diagnostics cleared"));
}

void BridgeClient::reloadDiagnosticsFromDisk() {
    QStringList lines;
    if (!m_diagnosticsLogPath.isEmpty()) {
        QFile file(m_diagnosticsLogPath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            while (!in.atEnd()) {
                const QString line = in.readLine();
                if (!line.isNull()) {
                    lines.push_back(line);
                }
            }
        }
    }
    if (lines.size() > kMaxDiagnosticsLines) {
        lines = lines.mid(lines.size() - kMaxDiagnosticsLines);
    }
    m_diagnosticsLines = std::move(lines);
    rebuildDiagnosticsText();
    emit diagnosticsChanged();
}

void BridgeClient::logDiagnostic(const QString &category, const QString &message) {
    const QString ts = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    const QString cat = category.trimmed().isEmpty() ? QStringLiteral("app") : category.trimmed();
    QString msg = message;
    msg.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    msg.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    const QString line = QStringLiteral("[%1] [%2] %3").arg(ts, cat, msg);

    appendDiagnosticLine(line);

    if (m_diagnosticsLogPath.isEmpty()) {
        return;
    }
    const bool written = DiagnosticsLog::appendLine(m_diagnosticsLogPath, line);
    (void)written;
}

void BridgeClient::appendDiagnosticLine(const QString &line) {
    if (line.isEmpty()) {
        return;
    }
    m_diagnosticsLines.push_back(line);
    if (m_diagnosticsLines.size() > kMaxDiagnosticsLines) {
        const int removeCount = m_diagnosticsLines.size() - kMaxDiagnosticsLines;
        m_diagnosticsLines.erase(
            m_diagnosticsLines.begin(),
            m_diagnosticsLines.begin() + removeCount);
    }
}

void BridgeClient::rebuildDiagnosticsText() {
    m_diagnosticsText = m_diagnosticsLines.join(QLatin1Char('\n'));
}

QString BridgeClient::resolveDiagnosticsLogPath() {
    return DiagnosticsLog::defaultLogPath();
}

bool BridgeClient::processSearchResultsFrame(const BinaryBridgeCodec::DecodedSearchResults &frame) {
    SearchWorkerOutputFrame out;
    out.seq = frame.seq;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> artistRows;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> albumRows;
    QVector<GlobalSearchResultsModel::SearchDisplayRow> trackRows;
    artistRows.reserve(frame.rows.size());
    albumRows.reserve(frame.rows.size());
    trackRows.reserve(frame.rows.size());
    if (m_publishLegacySearchLists) {
        out.artistRows.reserve(frame.rows.size());
        out.albumRows.reserve(frame.rows.size());
        out.trackRows.reserve(frame.rows.size());
    }
    for (const auto &row : frame.rows) {
        QString rowTypeLabel;
        switch (row.rowType) {
        case BinaryBridgeCodec::SearchRowArtist:
            rowTypeLabel = QStringLiteral("artist");
            break;
        case BinaryBridgeCodec::SearchRowAlbum:
            rowTypeLabel = QStringLiteral("album");
            break;
        case BinaryBridgeCodec::SearchRowTrack:
            rowTypeLabel = QStringLiteral("track");
            break;
        default:
            break;
        }
        if (rowTypeLabel.isEmpty()) {
            continue;
        }

        GlobalSearchResultsModel::SearchDisplayRow item;
        item.kind = QStringLiteral("item");
        item.rowType = rowTypeLabel;
        item.score = row.score;
        item.label = row.label;
        item.artist = row.artist;
        item.album = row.album;
        item.rootLabel = row.rootLabel;
        item.genre = row.genre;
        item.coverPath = row.coverPath;
        item.coverUrl = searchCoverUrlFast(row.coverPath);
        item.artistKey = row.artistKey;
        item.albumKey = row.albumKey;
        item.sectionKey = row.sectionKey;
        item.trackKey = row.trackKey;
        item.trackPath = row.trackPath;
        item.year = row.year;
        item.trackNumber = row.trackNumber;
        item.count = row.count;
        item.lengthSeconds = row.lengthSeconds;
        item.lengthText = row.lengthSeconds >= 0.0f
            ? formatDurationCompact(static_cast<double>(row.lengthSeconds))
            : QStringLiteral("--:--");

        QVariantMap legacyItem;
        if (m_publishLegacySearchLists) {
            legacyItem.insert(QStringLiteral("rowType"), row.rowType);
            legacyItem.insert(QStringLiteral("score"), row.score);
            legacyItem.insert(QStringLiteral("label"), row.label);
            legacyItem.insert(QStringLiteral("artist"), row.artist);
            legacyItem.insert(QStringLiteral("album"), row.album);
            legacyItem.insert(QStringLiteral("rootLabel"), row.rootLabel);
            legacyItem.insert(QStringLiteral("genre"), row.genre);
            legacyItem.insert(QStringLiteral("count"), row.count);
            legacyItem.insert(QStringLiteral("coverPath"), row.coverPath);
            legacyItem.insert(QStringLiteral("coverUrl"), item.coverUrl);
            legacyItem.insert(QStringLiteral("artistKey"), row.artistKey);
            legacyItem.insert(QStringLiteral("albumKey"), row.albumKey);
            legacyItem.insert(QStringLiteral("sectionKey"), row.sectionKey);
            legacyItem.insert(QStringLiteral("trackKey"), row.trackKey);
            legacyItem.insert(QStringLiteral("trackPath"), row.trackPath);
            if (row.year != std::numeric_limits<int>::min()) {
                legacyItem.insert(QStringLiteral("year"), row.year);
            } else {
                legacyItem.insert(QStringLiteral("year"), QVariant{});
            }
            if (row.trackNumber > 0) {
                legacyItem.insert(QStringLiteral("trackNumber"), row.trackNumber);
            } else {
                legacyItem.insert(QStringLiteral("trackNumber"), QVariant{});
            }
            legacyItem.insert(QStringLiteral("lengthSeconds"), row.lengthSeconds);
            legacyItem.insert(QStringLiteral("lengthText"), item.lengthText);
        }
        switch (row.rowType) {
        case BinaryBridgeCodec::SearchRowArtist:
            artistRows.push_back(std::move(item));
            if (m_publishLegacySearchLists) {
                out.artistRows.push_back(std::move(legacyItem));
            }
            break;
        case BinaryBridgeCodec::SearchRowAlbum:
            albumRows.push_back(std::move(item));
            if (m_publishLegacySearchLists) {
                out.albumRows.push_back(std::move(legacyItem));
            }
            break;
        case BinaryBridgeCodec::SearchRowTrack:
            trackRows.push_back(std::move(item));
            if (m_publishLegacySearchLists) {
                out.trackRows.push_back(std::move(legacyItem));
            }
            break;
        default:
            break;
        }
    }
    const auto appendSection = [&out](
                                   const QString &title,
                                   const QString &rowType,
                                   const QVector<GlobalSearchResultsModel::SearchDisplayRow> &sourceRows) {
        if (sourceRows.isEmpty()) {
            return;
        }
        GlobalSearchResultsModel::SearchDisplayRow sectionRow;
        sectionRow.kind = QStringLiteral("section");
        sectionRow.sectionTitle = title;
        sectionRow.rowType = rowType;
        out.displayRows.push_back(std::move(sectionRow));

        GlobalSearchResultsModel::SearchDisplayRow columnsRow;
        columnsRow.kind = QStringLiteral("columns");
        columnsRow.rowType = rowType;
        out.displayRows.push_back(std::move(columnsRow));

        out.displayRows.reserve(out.displayRows.size() + sourceRows.size());
        for (const auto &source : sourceRows) {
            out.displayRows.push_back(source);
        }
    };
    out.artistCount = artistRows.size();
    out.albumCount = albumRows.size();
    out.trackCount = trackRows.size();
    out.displayRows.reserve(
        artistRows.size() + albumRows.size() + trackRows.size() + 6);
    appendSection(QStringLiteral("Artists"), QStringLiteral("artist"), artistRows);
    appendSection(QStringLiteral("Albums"), QStringLiteral("album"), albumRows);
    appendSection(QStringLiteral("Tracks"), QStringLiteral("track"), trackRows);
    return applyPreparedSearchResultsFrame(std::move(out));
}

bool BridgeClient::applyPreparedSearchResultsFrame(SearchWorkerOutputFrame frame) {
    m_searchFramesReceived++;
    if (!frame.decodeError.isEmpty()) {
        m_searchFramesDecodeErrors++;
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("search"),
            QStringLiteral("decode error seq=%1 error=%2")
                .arg(frame.seq)
                .arg(frame.decodeError));
        emit bridgeError(QStringLiteral("invalid search frame: %1").arg(frame.decodeError));
        return false;
    }
    if (m_latestGlobalSearchSeqSent != 0
        && frame.seq != m_latestGlobalSearchSeqSent
        && !isNewerSeq(frame.seq, m_latestGlobalSearchSeqSent)) {
        m_searchFramesDroppedStale++;
        m_globalSearchSentAtMs.remove(frame.seq);
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("search"),
            QStringLiteral("drop stale frame seq=%1 latestSent=%2 dropped=%3")
                .arg(frame.seq)
                .arg(m_latestGlobalSearchSeqSent)
                .arg(m_searchFramesDroppedStale));
        return false;
    }
    if (m_globalSearchSeq != 0
        && frame.seq != m_globalSearchSeq
        && !isNewerSeq(frame.seq, m_globalSearchSeq)) {
        m_searchFramesDroppedStale++;
        m_globalSearchSentAtMs.remove(frame.seq);
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("search"),
            QStringLiteral("drop non-new frame seq=%1 current=%2 dropped=%3")
                .arg(frame.seq)
                .arg(m_globalSearchSeq)
                .arg(m_searchFramesDroppedStale));
        return false;
    }
    if (frame.seq != 0 && frame.seq == m_globalSearchSeq) {
        return false;
    }

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    QElapsedTimer modelApplyTimer;
    modelApplyTimer.start();
#endif
    const int artistCount = frame.artistCount;
    const int albumCount = frame.albumCount;
    const int trackCount = frame.trackCount;
    m_globalSearchSeq = frame.seq;
    m_globalSearchArtistCount = artistCount;
    m_globalSearchAlbumCount = albumCount;
    m_globalSearchTrackCount = trackCount;
    if (m_publishLegacySearchLists) {
        m_globalSearchArtistResults = std::move(frame.artistRows);
        m_globalSearchAlbumResults = std::move(frame.albumRows);
        m_globalSearchTrackResults = std::move(frame.trackRows);
    } else {
        if (!m_globalSearchArtistResults.isEmpty()) {
            m_globalSearchArtistResults.clear();
        }
        if (!m_globalSearchAlbumResults.isEmpty()) {
            m_globalSearchAlbumResults.clear();
        }
        if (!m_globalSearchTrackResults.isEmpty()) {
            m_globalSearchTrackResults.clear();
        }
    }
    m_globalSearchModel.replaceRows(std::move(frame.displayRows));
    m_searchFramesApplied++;

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    const qint64 modelApplyMs = modelApplyTimer.elapsed();
    const qint64 sentAtMs = m_globalSearchSentAtMs.take(frame.seq);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 latencyMs = sentAtMs > 0 ? (nowMs - sentAtMs) : -1;
    const qint64 queueDelayMs = frame.ffiPoppedAtMs > 0 ? (nowMs - frame.ffiPoppedAtMs) : -1;
    FERROUS_PROFILE_LOG_DIAGNOSTIC(
        QStringLiteral("search"),
        QStringLiteral("apply frame seq=%1 artists=%2 albums=%3 tracks=%4 latencyMs=%5 ffiPopMs=%6 decodeMs=%7 materializeMs=%8 modelApplyMs=%9 queueDelayMs=%10 workerMs=%11 coalesced=%12 coalescedUi=%13 recv=%14 applied=%15 dropped=%16 decodeErr=%17")
            .arg(frame.seq)
            .arg(artistCount)
            .arg(albumCount)
            .arg(trackCount)
            .arg(latencyMs)
            .arg(frame.ffiPopMs)
            .arg(frame.decodeMs)
            .arg(frame.materializeMs)
            .arg(modelApplyMs)
            .arg(queueDelayMs)
            .arg(frame.workerTotalMs)
            .arg(frame.coalescedInputDrops)
            .arg(frame.coalescedOutputDrops)
            .arg(m_searchFramesReceived)
            .arg(m_searchFramesApplied)
            .arg(m_searchFramesDroppedStale)
            .arg(m_searchFramesDecodeErrors));
#else
    m_globalSearchSentAtMs.take(frame.seq);
#endif
    return true;
}

void BridgeClient::flushGlobalSearchQuery() {
    if (m_ffiBridge == nullptr) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("search"),
            QStringLiteral("skip send: bridge unavailable"));
        return;
    }
    if (m_pendingGlobalSearchQuery == m_lastGlobalSearchQuerySent) {
        FERROUS_PROFILE_LOG_DIAGNOSTIC(
            QStringLiteral("search"),
            QStringLiteral("skip duplicate query"));
        return;
    }
    const quint32 seq = m_nextGlobalSearchSeq++;
    m_latestGlobalSearchSeqSent = seq;
    m_globalSearchSentAtMs.insert(seq, QDateTime::currentMSecsSinceEpoch());
    if (m_globalSearchSentAtMs.size() > 256) {
        m_globalSearchSentAtMs.clear();
    }
    m_lastGlobalSearchQuerySent = m_pendingGlobalSearchQuery;
    const QString trimmedQuery = m_pendingGlobalSearchQuery.trimmed();
    QString preview = trimmedQuery;
    if (preview.size() > 64) {
        preview = preview.left(64) + QStringLiteral("...");
    }
    FERROUS_PROFILE_LOG_DIAGNOSTIC(
        QStringLiteral("search"),
        QStringLiteral("send query seq=%1 chars=%2 text=\"%3\"")
            .arg(seq)
            .arg(trimmedQuery.size())
            .arg(preview));
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandSearchQuery(
        BinaryBridgeCodec::CmdSetSearchQuery,
        seq,
        m_pendingGlobalSearchQuery));
}

void BridgeClient::processAnalysisBytes(const QByteArray &chunk) {
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    QElapsedTimer analysisTimer;
    analysisTimer.start();
#endif
    if (chunk.isEmpty()) {
        return;
    }
    m_analysisBuffer += chunk;

    bool changed = false;
    int parsedFrames = 0;
    int totalRows = 0;
    int totalChannels = 0;
    int maxBins = 0;
    qsizetype readOffset = m_analysisBufferReadOffset;
    const qsizetype totalSize = m_analysisBuffer.size();
    const auto *base = reinterpret_cast<const uchar *>(m_analysisBuffer.constData());

    while ((totalSize - readOffset) >= static_cast<qsizetype>(sizeof(quint32))) {
        const auto *lenPtr = base + readOffset;
        const quint32 frameBytes = qFromLittleEndian<quint32>(lenPtr);
        if (frameBytes == 0 || frameBytes > kMaxAnalysisFrameBytes) {
            emit bridgeError(QStringLiteral("invalid analysis frame size: %1").arg(frameBytes));
            m_analysisBuffer.clear();
            m_analysisBufferReadOffset = 0;
            break;
        }
        const qsizetype totalBytes = static_cast<qsizetype>(sizeof(quint32) + frameBytes);
        if ((totalSize - readOffset) < totalBytes) {
            break;
        }
        const auto *data = base + readOffset + sizeof(quint32);
        readOffset += totalBytes;

        if (frameBytes < 21) {
            continue;
        }
        if (data[0] != kAnalysisFrameMagic) {
            continue;
        }
        const quint32 sampleRate = qFromLittleEndian<quint32>(data + 1);
        const quint8 flags = data[5];
        const quint16 waveformLen = qFromLittleEndian<quint16>(data + 6);
        const quint32 waveformCoverageMillis = qFromLittleEndian<quint32>(data + 8);
        const quint16 rowCount = qFromLittleEndian<quint16>(data + 12);
        const quint16 binCount = qFromLittleEndian<quint16>(data + 14);
        const quint32 frameSeq = qFromLittleEndian<quint32>(data + 16);
        const quint8 channelCount = data[20];
        const qsizetype labelBytes =
            (((flags & kAnalysisFlagSpectrogram) != 0 || (flags & kAnalysisFlagReset) != 0)
                ? static_cast<qsizetype>(channelCount)
                : 0);
        const qsizetype expected = 21 + static_cast<qsizetype>(waveformLen) + labelBytes
            + static_cast<qsizetype>(rowCount) * static_cast<qsizetype>(channelCount)
                * static_cast<qsizetype>(binCount);
        if (static_cast<qsizetype>(frameBytes) < expected) {
            continue;
        }
        parsedFrames++;
        totalRows += static_cast<int>(rowCount);
        totalChannels = std::max(totalChannels, static_cast<int>(channelCount));
        maxBins = std::max(maxBins, static_cast<int>(binCount));
        if (m_hasAnalysisFrameSeq && !isNewerSeq(frameSeq, m_lastAnalysisFrameSeq)) {
            m_analysisDroppedFrames++;
            continue;
        }
        m_hasAnalysisFrameSeq = true;
        m_lastAnalysisFrameSeq = frameSeq;

        const uchar *cursor = data + 21;

        if (sampleRate > 0 && m_sampleRateHz != static_cast<int>(sampleRate)) {
            m_sampleRateHz = static_cast<int>(sampleRate);
            changed = true;
        }

        const bool spectrogramReset = (flags & kAnalysisFlagReset) != 0;
        const double waveformCoverageSeconds =
            static_cast<double>(waveformCoverageMillis) / 1000.0;
        const bool waveformComplete = (flags & kAnalysisFlagWaveformComplete) != 0;
        if (m_spectrogramReset != spectrogramReset) {
            m_spectrogramReset = spectrogramReset;
            changed = true;
        }
        if (std::abs(m_waveformCoverageSeconds - waveformCoverageSeconds) > 0.0001) {
            m_waveformCoverageSeconds = waveformCoverageSeconds;
            changed = true;
        }
        if (m_waveformComplete != waveformComplete) {
            m_waveformComplete = waveformComplete;
            changed = true;
        }
        if (spectrogramReset) {
            if (!m_spectrogramChannels.isEmpty()) {
                m_spectrogramChannels.clear();
                changed = true;
            }
            m_spectrogramSeedBurstRowsRemaining = 0;
        }

        if ((flags & kAnalysisFlagWaveform) != 0) {
            QByteArray peaks(reinterpret_cast<const char *>(cursor), waveformLen);
            cursor += waveformLen;
            if (m_waveformPeaksPacked != peaks) {
                m_waveformPeaksPacked = peaks;
                changed = true;
            }
        } else {
            cursor += waveformLen;
        }

        QVector<QString> channelLabels;
        channelLabels.reserve(channelCount);
        for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
            channelLabels.push_back(
                spectrogramChannelLabelText(cursor[channelIndex], channelIndex));
        }
        cursor += labelBytes;

        if ((flags & kAnalysisFlagSpectrogram) != 0
            && rowCount > 0
            && binCount > 0
            && channelCount > 0) {
            bool rebuildChannels = m_spectrogramChannels.size() != channelCount;
            if (!rebuildChannels) {
                for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
                    const auto &channel = m_spectrogramChannels[channelIndex];
                    if (channel.label != channelLabels[channelIndex]
                        || (channel.packedBins > 0
                            && channel.packedBins != static_cast<int>(binCount))) {
                        rebuildChannels = true;
                        break;
                    }
                }
            }
            if (rebuildChannels) {
                m_spectrogramChannels.clear();
                m_spectrogramChannels.reserve(channelCount);
                for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
                    SpectrogramChannelDelta channel;
                    channel.label = channelLabels[channelIndex];
                    channel.packedBins = static_cast<int>(binCount);
                    m_spectrogramChannels.push_back(channel);
                }
                changed = true;
            }

            constexpr int kMaxPendingSpectrogramRows = 512;
            const qsizetype rowBytes = static_cast<qsizetype>(binCount);
            for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
                for (int channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
                    auto &channel = m_spectrogramChannels[channelIndex];
                    channel.packedRows.append(reinterpret_cast<const char *>(cursor), rowBytes);
                    channel.packedRowsCount += 1;
                    if (channel.packedRowsCount > kMaxPendingSpectrogramRows && channel.packedBins > 0) {
                        const int dropRows = channel.packedRowsCount - kMaxPendingSpectrogramRows;
                        const qsizetype dropBytes = static_cast<qsizetype>(dropRows)
                            * static_cast<qsizetype>(channel.packedBins);
                        channel.packedRows.remove(0, dropBytes);
                        channel.packedRowsCount = kMaxPendingSpectrogramRows;
                    }
                    cursor += rowBytes;
                }
            }
            if (!m_spectrogramChannels.isEmpty()) {
                const bool hasPendingRows = std::any_of(
                    m_spectrogramChannels.cbegin(),
                    m_spectrogramChannels.cend(),
                    [](const SpectrogramChannelDelta &channel) {
                        return channel.packedRowsCount > 0;
                    });
                if (hasPendingRows) {
                    if (spectrogramReset) {
                        m_spectrogramSeedBurstRowsRemaining = std::max(
                            m_spectrogramSeedBurstRowsRemaining,
                            static_cast<int>(rowCount));
                    }
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        scheduleAnalysisChanged();
    }

    if (m_analysisBuffer.isEmpty()) {
        m_analysisBufferReadOffset = 0;
        return;
    }
    if (readOffset >= m_analysisBuffer.size()) {
        m_analysisBuffer.clear();
        m_analysisBufferReadOffset = 0;
        return;
    }

    if (readOffset > (64 * 1024) || readOffset > (m_analysisBuffer.size() / 2)) {
        m_analysisBuffer.remove(0, readOffset);
        m_analysisBufferReadOffset = 0;
    } else {
        m_analysisBufferReadOffset = readOffset;
    }

#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        int pendingRows = 0;
        for (const auto &channel : m_spectrogramChannels) {
            pendingRows = std::max(pendingRows, channel.packedRowsCount);
        }
        const double analysisMs = static_cast<double>(analysisTimer.nsecsElapsed()) / 1'000'000.0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if ((analysisMs >= 3.0 || pendingRows >= 96 || parsedFrames > 1)
            && shouldEmitUiProfileLog(nowMs, &m_lastAnalysisProfileLogMs, 200)) {
            FERROUS_PROFILE_LOG_DIAGNOSTIC(
                QStringLiteral("ui-prof"),
                QStringLiteral(
                    "analysis_decode ms=%1 chunk_kb=%2 parsed_frames=%3 rows=%4 channels=%5 bins=%6 pending_rows=%7 changed=%8 dropped=%9")
                    .arg(analysisMs, 0, 'f', 2)
                    .arg(static_cast<double>(chunk.size()) / 1024.0, 0, 'f', 1)
                    .arg(parsedFrames)
                    .arg(totalRows)
                    .arg(totalChannels)
                    .arg(maxBins)
                    .arg(pendingRows)
                    .arg(changed ? 1 : 0)
                    .arg(static_cast<qulonglong>(m_analysisDroppedFrames)));
        }
    }
#endif
}

void BridgeClient::scheduleSnapshotChanged() {
    m_snapshotChangedPending = true;
    if (!m_snapshotNotifyTimer.isActive()) {
        m_snapshotNotifyTimer.start();
    }
}

void BridgeClient::schedulePlaybackChanged() {
    if (m_playbackChangedPending) {
        return;
    }
    m_playbackChangedPending = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            if (!m_playbackChangedPending) {
                return;
            }
            m_playbackChangedPending = false;
            emit playbackChanged();
        },
        Qt::QueuedConnection);
}

void BridgeClient::scheduleAnalysisChanged() {
    if (m_analysisChangedPending) {
        return;
    }
    m_analysisChangedPending = true;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            if (!m_analysisChangedPending) {
                return;
            }
            m_analysisChangedPending = false;
            emit analysisChanged();
        },
        Qt::QueuedConnection);
}

void BridgeClient::shutdownBridgeGracefully() {
    if (m_ffiBridge == nullptr) {
        return;
    }

    sendBinaryCommand(BinaryBridgeCodec::encodeCommandNoPayload(BinaryBridgeCodec::CmdShutdown));
}

QString BridgeClient::detectFileBrowserNameHeuristic() {
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    if (desktop.contains(QStringLiteral("kde"))) {
        return QStringLiteral("Dolphin");
    }
    if (desktop.contains(QStringLiteral("gnome"))) {
        return QStringLiteral("Files");
    }
    if (desktop.contains(QStringLiteral("xfce"))) {
        return QStringLiteral("Thunar");
    }
    if (desktop.contains(QStringLiteral("cinnamon"))) {
        return QStringLiteral("Nemo");
    }
    if (desktop.contains(QStringLiteral("lxqt")) || desktop.contains(QStringLiteral("lxde"))) {
        return QStringLiteral("PCManFM");
    }
    return QStringLiteral("File Manager");
}

QString BridgeClient::detectFileBrowserName() {
    auto fromDesktopId = [](const QString &desktopId) -> QString {
        const QString lowered = desktopId.trimmed().toLower();
        if (lowered.contains(QStringLiteral("dolphin"))) {
            return QStringLiteral("Dolphin");
        }
        if (lowered.contains(QStringLiteral("nautilus"))
            || lowered.contains(QStringLiteral("org.gnome.files"))) {
            return QStringLiteral("Files");
        }
        if (lowered.contains(QStringLiteral("thunar"))) {
            return QStringLiteral("Thunar");
        }
        if (lowered.contains(QStringLiteral("nemo"))) {
            return QStringLiteral("Nemo");
        }
        if (lowered.contains(QStringLiteral("pcmanfm"))) {
            return QStringLiteral("PCManFM");
        }
        if (!lowered.isEmpty()) {
            QString base = lowered;
            if (base.endsWith(QStringLiteral(".desktop"))) {
                base.chop(8);
            }
            const int slash = base.lastIndexOf('/');
            if (slash >= 0 && slash + 1 < base.size()) {
                base = base.mid(slash + 1);
            }
            if (base.startsWith(QStringLiteral("org.kde."))) {
                base = base.mid(QStringLiteral("org.kde.").size());
            } else if (base.startsWith(QStringLiteral("org.gnome."))) {
                base = base.mid(QStringLiteral("org.gnome.").size());
            }
            if (!base.isEmpty()) {
                base[0] = base[0].toUpper();
                return base;
            }
        }
        return QString{};
    };

    QProcess proc;
    proc.start(
        QStringLiteral("xdg-mime"),
        {QStringLiteral("query"), QStringLiteral("default"), QStringLiteral("inode/directory")});
    if (proc.waitForFinished(250)) {
        const QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        const QString detected = fromDesktopId(output);
        if (!detected.isEmpty()) {
            return detected;
        }
    }
    return detectFileBrowserNameHeuristic();
}

bool BridgeClient::openUrlInFileBrowser(const QString &path, bool containingFolder) const {
    if (path.trimmed().isEmpty()) {
        return false;
    }

    QString localPath = path.trimmed();
    const QUrl maybeUrl(localPath);
    if (maybeUrl.isValid() && maybeUrl.isLocalFile()) {
        localPath = maybeUrl.toLocalFile();
    }

    QFileInfo info(localPath);
    QString targetPath;
    if (containingFolder) {
        targetPath = info.absolutePath();
    } else if (info.isFile()) {
        targetPath = info.absolutePath();
    } else {
        targetPath = info.absoluteFilePath();
    }

    if (targetPath.isEmpty()) {
        return false;
    }
    return QDesktopServices::openUrl(QUrl::fromLocalFile(targetPath));
}

void BridgeClient::sendBinaryCommand(const QByteArray &payload) {
    if (payload.isEmpty()) {
        logDiagnostic(QStringLiteral("bridge"), QStringLiteral("drop empty command payload"));
        emit bridgeError(QStringLiteral("failed to encode binary bridge command"));
        return;
    }
    if (m_ffiBridge == nullptr) {
        logDiagnostic(QStringLiteral("bridge"), QStringLiteral("drop command: bridge not initialized"));
        emit bridgeError(QStringLiteral("bridge is not initialized"));
        return;
    }
    const auto *ptr = reinterpret_cast<const std::uint8_t *>(payload.constData());
    if (!ferrous_ffi_bridge_send_binary(m_ffiBridge, ptr, static_cast<std::size_t>(payload.size()))) {
        logDiagnostic(
            QStringLiteral("bridge"),
            QStringLiteral("failed to send command bytes=%1").arg(payload.size()));
        emit bridgeError(QStringLiteral("failed to send command to in-process bridge"));
        return;
    }
}

void BridgeClient::sendLibraryRootCommand(quint16 cmdId, const QString &path) {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandString(cmdId, path));
}

void BridgeClient::sendLibraryRootCommand(quint16 cmdId, const QString &path, const QString &name) {
    sendBinaryCommand(BinaryBridgeCodec::encodeCommandStringPair(cmdId, path, name));
}

void BridgeClient::applyLibraryTreeFrame(int version, const QByteArray &treeBytes) {
    m_libraryTreeBinary = treeBytes;
    m_libraryVersion = version;

    m_libraryAlbums.clear();
    m_libraryAlbumArtists.clear();
    m_libraryAlbumNames.clear();
    m_libraryAlbumCoverPaths.clear();
    m_libraryAlbumTrackPaths.clear();
    m_trackCoverByPath.clear();
    m_trackCoverByDirectory.clear();
    if (!m_currentTrackPath.isEmpty()) {
        requestTrackCoverLookup(m_currentTrackPath);
    }

    emit libraryTreeFrameReceived(version, treeBytes);
}

bool BridgeClient::processBinarySnapshot(const BinaryBridgeCodec::DecodedSnapshot &snapshot) {
    if (snapshot.hasStopped) {
        if (m_connected) {
            m_connected = false;
            emit connectedChanged();
        }
        return false;
    }

    if (!snapshot.errorMessage.trimmed().isEmpty()) {
        emit bridgeError(snapshot.errorMessage);
        return false;
    }

    if (!snapshot.playback.present
        && !snapshot.queue.present
        && !snapshot.library.present
        && !snapshot.metadata.present
        && !snapshot.settings.present
        && !snapshot.lastfm.present) {
        return false;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool profileSnapshot = m_profileUiEnabled;
    QElapsedTimer snapshotProfileTimer;
    qint64 snapshotProfileMarkNs = 0;
    double snapshotPlaybackMs = 0.0;
    double snapshotQueueMs = 0.0;
    double snapshotTrackMs = 0.0;
    double snapshotSettingsMs = 0.0;
    double snapshotLastFmMs = 0.0;
    double snapshotLibraryMs = 0.0;
    if (profileSnapshot) {
        snapshotProfileTimer.start();
    }
    const auto finishSnapshotSection = [&](double &slot) {
        if (!profileSnapshot) {
            return;
        }
        const qint64 nowNs = snapshotProfileTimer.nsecsElapsed();
        slot += static_cast<double>(nowNs - snapshotProfileMarkNs) / 1000000.0;
        snapshotProfileMarkNs = nowNs;
    };

    const QString nextState = playbackStateText(snapshot.playback.state, m_playbackState);
    const bool isStopped = nextState == QStringLiteral("Stopped");
    const double pos = snapshot.playback.present ? snapshot.playback.positionSeconds : m_positionSeconds;
    const double dur = snapshot.playback.present ? snapshot.playback.durationSeconds : m_durationSeconds;
    const int repeatMode = std::clamp(snapshot.playback.present ? snapshot.playback.repeatMode : m_repeatMode, 0, 2);
    const bool shuffleEnabled = snapshot.playback.present ? snapshot.playback.shuffleEnabled : m_shuffleEnabled;
    const QString playbackCurrentPath = snapshot.playback.present ? snapshot.playback.currentPath : m_currentTrackPath;
    const QString currentPath = isStopped && playbackCurrentPath.trimmed().isEmpty()
        ? m_currentTrackPath
        : playbackCurrentPath;
    int playing = snapshot.playback.present ? snapshot.playback.currentQueueIndex : m_playingQueueIndex;

    const int qlen = snapshot.queue.present ? snapshot.queue.len : m_queueLength;
    const int selected = snapshot.queue.present ? snapshot.queue.selectedIndex : m_selectedQueueIndex;

    const QString metadataSourcePath = snapshot.metadata.present ? snapshot.metadata.sourcePath : QString{};
    const QString metadataCoverPath = snapshot.metadata.present ? snapshot.metadata.coverPath : QString{};
    const QString metadataGenre = snapshot.metadata.present ? snapshot.metadata.genre : QString{};
    const int metadataYear = snapshot.metadata.present
        ? snapshot.metadata.year
        : std::numeric_limits<int>::min();
    const QString metadataFormatLabel =
        snapshot.metadata.present ? snapshot.metadata.formatLabel : QString{};
    const int metadataChannels = snapshot.metadata.present ? snapshot.metadata.channels : 0;
    const int metadataSampleRateHz = snapshot.metadata.present ? snapshot.metadata.sampleRateHz : 0;
    const int metadataBitDepth = snapshot.metadata.present ? snapshot.metadata.bitDepth : 0;
    const int metadataCurrentBitrateKbps = snapshot.metadata.present
        ? (snapshot.metadata.currentBitrateKbps > 0
            ? snapshot.metadata.currentBitrateKbps
            : snapshot.metadata.bitrateKbps)
        : 0;
    QString metadataCoverUrl;
    if (!metadataCoverPath.trimmed().isEmpty() && metadataSourcePath == currentPath) {
        if (!m_pendingAppliedArtworkTrackPath.isEmpty()
            && m_pendingAppliedArtworkTrackPath == currentPath) {
            bumpCoverRefreshNonce(metadataCoverPath);
            m_pendingAppliedArtworkTrackPath.clear();
        }
        metadataCoverUrl = coverUrlForPath(metadataCoverPath);
    }

    bool changed = false;
    bool playbackSignalChanged = false;
    bool snapshotSignalChanged = false;
    const bool hadTrackContextPath = !m_currentTrackPath.isEmpty();
    const QString previousPlaybackState = m_playbackState;
    const QString previousTrackPath = m_currentTrackPath;
    const int previousPlayingIndex = m_playingQueueIndex;

    if (snapshot.queue.present && !m_loggedStartupQueuePresent) {
        logDiagnostic(
            QStringLiteral("session"),
            QStringLiteral("startup queue snapshot present len=%1 selected=%2 unknownDur=%3")
                .arg(snapshot.queue.len)
                .arg(snapshot.queue.selectedIndex)
                .arg(snapshot.queue.unknownDurationCount));
        m_loggedStartupQueuePresent = true;
    } else if (!snapshot.queue.present
        && !m_loggedStartupQueueMissing
        && m_queueLength == 0
        && (snapshot.library.present || snapshot.playback.present))
    {
        logDiagnostic(
            QStringLiteral("session"),
            QStringLiteral("startup snapshot omitted queue libraryTracks=%1 playbackPresent=%2")
                .arg(snapshot.library.present ? snapshot.library.trackCount : -1)
                .arg(snapshot.playback.present ? 1 : 0));
        m_loggedStartupQueueMissing = true;
    }

    if (m_playbackState != nextState) {
        m_playbackState = nextState;
        changed = true;
        playbackSignalChanged = true;
    }

    bool applyIncomingPosition = true;
    if (m_pendingSeek) {
        if (nowMs >= m_pendingSeekUntilMs) {
            m_pendingSeek = false;
        } else if (std::abs(pos - m_pendingSeekTargetSeconds) <= 0.8) {
            m_pendingSeek = false;
        } else {
            applyIncomingPosition = false;
        }
    }
    if (applyIncomingPosition) {
        const QString posText = formatSeconds(pos);
        if (m_positionText != posText) {
            m_positionText = posText;
            changed = true;
            playbackSignalChanged = true;
        }
        if (std::abs(m_positionSeconds - pos) >= 0.03) {
            m_positionSeconds = pos;
            changed = true;
            playbackSignalChanged = true;
        }
    }

    const QString durText = formatSeconds(dur);
    if (m_durationText != durText) {
        m_durationText = durText;
        changed = true;
        playbackSignalChanged = true;
    }
    if (!qFuzzyCompare(m_durationSeconds + 1.0, dur + 1.0)) {
        m_durationSeconds = dur;
        changed = true;
        playbackSignalChanged = true;
    }

    if (m_repeatMode != repeatMode) {
        m_repeatMode = repeatMode;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_shuffleEnabled != shuffleEnabled) {
        m_shuffleEnabled = shuffleEnabled;
        changed = true;
        snapshotSignalChanged = true;
    }

    const double settingsVolume = snapshot.settings.present
        ? static_cast<double>(snapshot.settings.volume)
        : m_volume;
    if (std::abs(m_volume - settingsVolume) > 0.0005) {
        m_volume = settingsVolume;
        changed = true;
        snapshotSignalChanged = true;
    }

    if (m_queueLength != qlen) {
        m_queueLength = qlen;
        changed = true;
        snapshotSignalChanged = true;
        if (m_pendingQueueSelection >= qlen) {
            m_pendingQueueSelection = -1;
            m_pendingQueueSelectionUntilMs = 0;
        }
    }
    finishSnapshotSection(snapshotPlaybackMs);

    if (snapshot.queue.present) {
        QString nextQueueDurationText = formatSeconds(snapshot.queue.totalDurationSeconds);
        const int queueUnknownDurationCount = std::max(0, snapshot.queue.unknownDurationCount);
        if (queueUnknownDurationCount > 0) {
            nextQueueDurationText = QStringLiteral("%1+?").arg(nextQueueDurationText);
        }
        if (m_queueDurationText != nextQueueDurationText) {
            m_queueDurationText = nextQueueDurationText;
            changed = true;
            snapshotSignalChanged = true;
        }

        QVector<QueueRowData> rows;
        QStringList paths;
        rows.reserve(snapshot.queue.tracks.size());
        paths.reserve(snapshot.queue.tracks.size());
        for (const auto &track : snapshot.queue.tracks) {
            QueueRowData row;
            row.title = track.title.trimmed().isEmpty() ? track.path : track.title;
            row.artist = track.artist;
            row.album = track.album;
            row.coverPath = track.coverPath;
            row.genre = track.genre;
            row.lengthText = track.lengthSeconds >= 0.0f
                ? formatDurationCompact(static_cast<double>(track.lengthSeconds))
                : QStringLiteral("--:--");
            row.path = track.path;
            row.trackNumber = track.trackNumber;
            row.year = track.year;

            paths.push_back(track.path);
            rows.push_back(row);
        }
        if (m_queueRowsModel.setRows(std::move(rows))) {
            changed = true;
            snapshotSignalChanged = true;
        }
        if (m_queuePaths != paths) {
            m_queuePaths = paths;
            rebuildQueuePathFirstIndex();
            m_queueVersion = m_queueVersion < std::numeric_limits<int>::max()
                ? (m_queueVersion + 1)
                : 1;
            changed = true;
            snapshotSignalChanged = true;
        } else if (m_queuePathFirstIndex.isEmpty() && !m_queuePaths.isEmpty()) {
            rebuildQueuePathFirstIndex();
        }
    }

    if (m_pendingQueueSelection >= 0) {
        if (selected == m_pendingQueueSelection || nowMs >= m_pendingQueueSelectionUntilMs) {
            m_pendingQueueSelection = -1;
            m_pendingQueueSelectionUntilMs = 0;
            if (m_selectedQueueIndex != selected) {
                m_selectedQueueIndex = selected;
                changed = true;
                snapshotSignalChanged = true;
            }
        }
    } else if (m_selectedQueueIndex != selected) {
        m_selectedQueueIndex = selected;
        changed = true;
        snapshotSignalChanged = true;
    }
    finishSnapshotSection(snapshotQueueMs);

    const bool currentPathChanged = m_currentTrackPath != currentPath;
    if (currentPathChanged) {
        m_currentTrackPath = currentPath;
        changed = true;
        snapshotSignalChanged = true;
    }

    if (playing < 0 && !currentPath.isEmpty() && !m_queuePaths.isEmpty()) {
        playing = queuePathFirstIndex(currentPath);
    }
    if (m_playingQueueIndex != playing) {
        m_playingQueueIndex = playing;
        changed = true;
        snapshotSignalChanged = true;
    }
#if defined(FERROUS_ENABLE_PROFILE_LOGS) && FERROUS_ENABLE_PROFILE_LOGS
    if (m_profileUiEnabled) {
        if (previousPlaybackState != m_playbackState) {
            FERROUS_PROFILE_LOG_DIAGNOSTIC(
                QStringLiteral("ui-prof"),
                QStringLiteral(
                    "playback_state_change from=%1 to=%2 current_path=%3 previous_path=%4 playing_index=%5 selected_index=%6 queue_length=%7")
                    .arg(previousPlaybackState)
                    .arg(m_playbackState)
                    .arg(playbackLogPathField(m_currentTrackPath))
                    .arg(playbackLogPathField(previousTrackPath))
                    .arg(m_playingQueueIndex)
                    .arg(m_selectedQueueIndex)
                    .arg(m_queueLength));
        }
        if (currentPathChanged) {
            FERROUS_PROFILE_LOG_DIAGNOSTIC(
                QStringLiteral("ui-prof"),
                QStringLiteral(
                    "playback_track_change from=%1 to=%2 state=%3 previous_playing_index=%4 playing_index=%5 selected_index=%6 queue_length=%7")
                    .arg(playbackLogPathField(previousTrackPath))
                    .arg(playbackLogPathField(m_currentTrackPath))
                    .arg(m_playbackState)
                    .arg(previousPlayingIndex)
                    .arg(m_playingQueueIndex)
                    .arg(m_selectedQueueIndex)
                    .arg(m_queueLength));
        }
    }
#endif

    QString nextTrackTitle = m_currentTrackTitle;
    QString nextTrackArtist = m_currentTrackArtist;
    QString nextTrackAlbum = m_currentTrackAlbum;
    QString nextTrackGenre = m_currentTrackGenre;
    QVariant nextTrackYear = m_currentTrackYear;
    const bool metadataMatchesCurrentPath =
        !currentPath.isEmpty() && snapshot.metadata.present && metadataSourcePath == currentPath;
    QString nextTrackFormatLabel = currentPath.isEmpty()
        ? QString{}
        : (metadataMatchesCurrentPath
            ? formatLabelFromPath(currentPath)
            : m_currentTrackFormatLabel);
    int nextTrackChannels = currentPath.isEmpty() ? 0 : m_currentTrackChannels;
    int nextTrackSampleRateHz = currentPath.isEmpty() ? 0 : m_currentTrackSampleRateHz;
    int nextTrackBitDepth = currentPath.isEmpty() ? 0 : m_currentTrackBitDepth;
    int nextTrackCurrentBitrateKbps = currentPath.isEmpty() ? 0 : m_currentTrackCurrentBitrateKbps;
    QString queueTrackCover;
    const bool stoppedTrackAdvanced = isStopped && hadTrackContextPath && currentPathChanged;
    if (stoppedTrackAdvanced
        && (!m_spectrogramChannels.isEmpty() || m_spectrogramReset)) {
        clearSpectrogramDeltaState();
    }
    if (!metadataMatchesCurrentPath && !currentPath.isEmpty() && nextTrackFormatLabel.isEmpty()) {
        nextTrackFormatLabel = formatLabelFromPath(currentPath);
    }
    if (!currentPath.isEmpty()) {
        int detailIndex = playing;
        if (detailIndex < 0 && !m_queuePaths.isEmpty()) {
            detailIndex = queuePathFirstIndex(currentPath);
        }

        if (snapshot.queue.present
            && detailIndex >= 0
            && detailIndex < snapshot.queue.tracks.size()) {
            const auto &track = snapshot.queue.tracks[detailIndex];
            nextTrackTitle = track.title;
            nextTrackArtist = track.artist;
            nextTrackAlbum = track.album;
            nextTrackGenre = track.genre;
            queueTrackCover = coverUrlForPath(track.coverPath);
            if (track.year != std::numeric_limits<int>::min()) {
                nextTrackYear = track.year;
            }
        } else if (const QueueRowData *row = m_queueRowsModel.rowAt(detailIndex)) {
            nextTrackTitle = row->title;
            nextTrackArtist = row->artist;
            nextTrackAlbum = row->album;
            nextTrackGenre = row->genre;
            queueTrackCover = coverUrlForPath(row->coverPath);
            if (row->year != std::numeric_limits<int>::min()) {
                nextTrackYear = row->year;
            }
        }

        if (snapshot.metadata.present && metadataSourcePath == currentPath) {
            if (!snapshot.metadata.title.trimmed().isEmpty()) {
                nextTrackTitle = snapshot.metadata.title;
            }
            if (!snapshot.metadata.artist.trimmed().isEmpty()) {
                nextTrackArtist = snapshot.metadata.artist;
            }
            if (!snapshot.metadata.album.trimmed().isEmpty()) {
                nextTrackAlbum = snapshot.metadata.album;
            }
            if (!metadataGenre.trimmed().isEmpty()) {
                nextTrackGenre = metadataGenre;
            }
            if (metadataYear != std::numeric_limits<int>::min()) {
                nextTrackYear = metadataYear;
            }
        }

        if (nextTrackTitle.trimmed().isEmpty()) {
            const QFileInfo info(currentPath);
            const QString fallbackTitle = info.fileName();
            nextTrackTitle = fallbackTitle.isEmpty() ? currentPath : fallbackTitle;
        }
    }
    if (metadataMatchesCurrentPath) {
        if (!metadataFormatLabel.trimmed().isEmpty()) {
            nextTrackFormatLabel = metadataFormatLabel;
        }
        nextTrackChannels = metadataChannels;
        nextTrackSampleRateHz = metadataSampleRateHz;
        nextTrackBitDepth = metadataBitDepth;
        nextTrackCurrentBitrateKbps = metadataCurrentBitrateKbps;
    }
    if (m_currentTrackTitle != nextTrackTitle) {
        m_currentTrackTitle = nextTrackTitle;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackArtist != nextTrackArtist) {
        m_currentTrackArtist = nextTrackArtist;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackAlbum != nextTrackAlbum) {
        m_currentTrackAlbum = nextTrackAlbum;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackGenre != nextTrackGenre) {
        m_currentTrackGenre = nextTrackGenre;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackYear != nextTrackYear) {
        m_currentTrackYear = nextTrackYear;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackFormatLabel != nextTrackFormatLabel) {
        m_currentTrackFormatLabel = nextTrackFormatLabel;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackChannels != nextTrackChannels) {
        m_currentTrackChannels = nextTrackChannels;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackSampleRateHz != nextTrackSampleRateHz) {
        m_currentTrackSampleRateHz = nextTrackSampleRateHz;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackBitDepth != nextTrackBitDepth) {
        m_currentTrackBitDepth = nextTrackBitDepth;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_currentTrackCurrentBitrateKbps != nextTrackCurrentBitrateKbps) {
        m_currentTrackCurrentBitrateKbps = nextTrackCurrentBitrateKbps;
        changed = true;
        snapshotSignalChanged = true;
    }

    QString currentCover = metadataCoverUrl;
    if (currentCover.isEmpty() && !queueTrackCover.isEmpty()) {
        currentCover = queueTrackCover;
    }
    if (isStopped && !stoppedTrackAdvanced && currentCover.isEmpty()) {
        currentCover = m_currentTrackCoverPath;
    } else if (currentCover.isEmpty() && !currentPath.isEmpty()) {
        const auto cached = m_trackCoverByPath.constFind(currentPath);
        if (cached != m_trackCoverByPath.constEnd()) {
            currentCover = cached.value();
        } else {
            const QString dirPath = trackDirectoryPath(currentPath);
            const auto dirCached = m_trackCoverByDirectory.constFind(dirPath);
            if (!dirPath.isEmpty() && dirCached != m_trackCoverByDirectory.constEnd()) {
                currentCover = dirCached.value();
                cacheTrackCoverForPath(currentPath, currentCover);
            } else {
                requestTrackCoverLookup(currentPath);
            }
        }
    }
    if (!currentPath.isEmpty() && !currentCover.isEmpty()) {
        cacheTrackCoverForPath(currentPath, currentCover);
    }
    if (m_currentTrackCoverPath != currentCover) {
        m_currentTrackCoverPath = currentCover;
        changed = true;
        snapshotSignalChanged = true;
    }
    finishSnapshotSection(snapshotTrackMs);

    const double dbRange = snapshot.settings.present
        ? static_cast<double>(snapshot.settings.dbRange)
        : m_dbRange;
    const int fftSize = snapshot.settings.present ? snapshot.settings.fftSize : m_fftSize;
    if (m_fftSize != fftSize) {
        m_fftSize = fftSize;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int spectrogramViewMode = std::clamp(
        snapshot.settings.present ? snapshot.settings.spectrogramViewMode : m_spectrogramViewMode,
        0,
        1);
    if (m_spectrogramViewMode != spectrogramViewMode) {
        m_spectrogramViewMode = spectrogramViewMode;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int viewerFullscreenMode = std::clamp(
        snapshot.settings.present
            ? snapshot.settings.viewerFullscreenMode
            : m_viewerFullscreenMode,
        0,
        1);
    if (m_viewerFullscreenMode != viewerFullscreenMode) {
        m_viewerFullscreenMode = viewerFullscreenMode;
        changed = true;
        snapshotSignalChanged = true;
    }

    if (!qFuzzyCompare(m_dbRange + 1.0, dbRange + 1.0)) {
        m_dbRange = dbRange;
        changed = true;
        snapshotSignalChanged = true;
    }

    const bool logScale = snapshot.settings.present ? snapshot.settings.logScale : m_logScale;
    if (m_logScale != logScale) {
        m_logScale = logScale;
        changed = true;
        snapshotSignalChanged = true;
    }

    const bool showFps = snapshot.settings.present ? snapshot.settings.showFps : m_showFps;
    if (m_showFps != showFps) {
        m_showFps = showFps;
        changed = true;
        snapshotSignalChanged = true;
    }

    const bool systemMediaControlsEnabled = snapshot.settings.present
        ? snapshot.settings.systemMediaControlsEnabled
        : m_systemMediaControlsEnabled;
    if (m_systemMediaControlsEnabled != systemMediaControlsEnabled) {
        m_systemMediaControlsEnabled = systemMediaControlsEnabled;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int settingsSortMode = std::clamp(
        snapshot.settings.present ? snapshot.settings.librarySortMode : m_librarySortMode,
        0,
        1);
    if (m_librarySortMode != settingsSortMode) {
        m_librarySortMode = settingsSortMode;
        changed = true;
        snapshotSignalChanged = true;
    }
    finishSnapshotSection(snapshotSettingsMs);

    const bool lastFmEnabled = snapshot.lastfm.present ? snapshot.lastfm.enabled : m_lastFmScrobblingEnabled;
    if (m_lastFmScrobblingEnabled != lastFmEnabled) {
        m_lastFmScrobblingEnabled = lastFmEnabled;
        changed = true;
        snapshotSignalChanged = true;
    }

    const bool lastFmBuildConfigured = snapshot.lastfm.present
        ? snapshot.lastfm.buildConfigured
        : m_lastFmBuildConfigured;
    if (m_lastFmBuildConfigured != lastFmBuildConfigured) {
        m_lastFmBuildConfigured = lastFmBuildConfigured;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int lastFmAuthState = snapshot.lastfm.present ? snapshot.lastfm.authState : m_lastFmAuthState;
    if (m_lastFmAuthState != lastFmAuthState) {
        m_lastFmAuthState = lastFmAuthState;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int lastFmPendingScrobbleCount = snapshot.lastfm.present
        ? snapshot.lastfm.pendingScrobbleCount
        : m_lastFmPendingScrobbleCount;
    if (m_lastFmPendingScrobbleCount != lastFmPendingScrobbleCount) {
        m_lastFmPendingScrobbleCount = lastFmPendingScrobbleCount;
        changed = true;
        snapshotSignalChanged = true;
    }

    const QString lastFmUsername = snapshot.lastfm.present ? snapshot.lastfm.username : m_lastFmUsername;
    if (m_lastFmUsername != lastFmUsername) {
        m_lastFmUsername = lastFmUsername;
        changed = true;
        snapshotSignalChanged = true;
    }

    const QString lastFmStatusText = snapshot.lastfm.present ? snapshot.lastfm.statusText : m_lastFmStatusText;
    if (m_lastFmStatusText != lastFmStatusText) {
        m_lastFmStatusText = lastFmStatusText;
        changed = true;
        snapshotSignalChanged = true;
    }

    const QString lastFmAuthUrl = snapshot.lastfm.present ? snapshot.lastfm.authUrl : m_lastFmAuthUrl;
    if (m_lastFmAuthUrl != lastFmAuthUrl) {
        m_lastFmAuthUrl = lastFmAuthUrl;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (!m_lastFmAuthUrl.trimmed().isEmpty() && m_lastOpenedExternalUrl != m_lastFmAuthUrl) {
        const QUrl authUrl(m_lastFmAuthUrl);
        if (authUrl.isValid()) {
            QDesktopServices::openUrl(authUrl);
            m_lastOpenedExternalUrl = m_lastFmAuthUrl;
        }
    }
    if (m_lastFmAuthUrl.trimmed().isEmpty()) {
        m_lastOpenedExternalUrl.clear();
    }
    finishSnapshotSection(snapshotLastFmMs);

    const bool scanInProgress = snapshot.library.present ? snapshot.library.scanInProgress : m_libraryScanInProgress;
    if (m_libraryScanInProgress != scanInProgress) {
        m_libraryScanInProgress = scanInProgress;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int roots = snapshot.library.present ? snapshot.library.rootCount : m_libraryRootCount;
    if (m_libraryRootCount != roots) {
        m_libraryRootCount = roots;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int tracks = snapshot.library.present ? snapshot.library.trackCount : m_libraryTrackCount;
    if (m_libraryTrackCount != tracks) {
        m_libraryTrackCount = tracks;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int artists = snapshot.library.present ? snapshot.library.artistCount : m_libraryArtistCount;
    if (m_libraryArtistCount != artists) {
        m_libraryArtistCount = artists;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int albums = snapshot.library.present ? snapshot.library.albumCount : m_libraryAlbumCount;
    if (m_libraryAlbumCount != albums) {
        m_libraryAlbumCount = albums;
        changed = true;
        snapshotSignalChanged = true;
    }

    const QStringList rootPaths = snapshot.library.present ? snapshot.library.rootPaths : m_libraryRoots;
    if (m_libraryRoots != rootPaths) {
        m_libraryRoots = rootPaths;
        changed = true;
        snapshotSignalChanged = true;
    }

    QVariantList rootEntries = m_libraryRootEntries;
    if (snapshot.library.present) {
        rootEntries.clear();
        rootEntries.reserve(snapshot.library.rootEntries.size());
        for (const auto &root : snapshot.library.rootEntries) {
            const QString trimmedName = normalizeRootNameArg(root.name);
            QVariantMap entry;
            entry.insert(QStringLiteral("path"), root.path);
            entry.insert(QStringLiteral("name"), trimmedName);
            entry.insert(
                QStringLiteral("displayName"),
                trimmedName.isEmpty() ? root.path : trimmedName);
            entry.insert(QStringLiteral("searchLabel"), rootSearchLabel(root.path, trimmedName));
            rootEntries.push_back(entry);
        }
    }
    if (m_libraryRootEntries != rootEntries) {
        m_libraryRootEntries = rootEntries;
        changed = true;
        snapshotSignalChanged = true;
    }

    const QString libraryLastError = snapshot.library.present ? snapshot.library.lastError : m_libraryLastError;
    if (m_libraryLastError != libraryLastError) {
        m_libraryLastError = libraryLastError;
        if (!m_libraryLastError.trimmed().isEmpty()) {
            emit bridgeError(QStringLiteral("library: %1").arg(m_libraryLastError));
        }
        snapshotSignalChanged = true;
    }

    if (!m_pendingAddRootPath.isEmpty()) {
        const bool fresh = m_pendingAddRootIssuedMs > 0 && (nowMs - m_pendingAddRootIssuedMs) <= 10000;
        const bool rootAppeared = rootPaths.contains(m_pendingAddRootPath);
        if (!fresh || rootAppeared || m_libraryScanInProgress) {
            m_pendingAddRootPath.clear();
            m_pendingAddRootIssuedMs = 0;
        }
    }

    const int librarySortMode = std::clamp(
        snapshot.library.present ? snapshot.library.sortMode : m_librarySortMode,
        0,
        1);
    if (m_librarySortMode != librarySortMode) {
        m_librarySortMode = librarySortMode;
        changed = true;
        snapshotSignalChanged = true;
    }

    const int rootsCompleted = snapshot.library.present ? std::max(0, snapshot.library.rootsCompleted) : m_libraryScanRootsCompleted;
    const int rootsTotal = snapshot.library.present ? std::max(0, snapshot.library.rootsTotal) : m_libraryScanRootsTotal;
    const int discovered = snapshot.library.present ? std::max(0, snapshot.library.filesDiscovered) : m_libraryScanDiscovered;
    const int processed = snapshot.library.present ? std::max(0, snapshot.library.filesProcessed) : m_libraryScanProcessed;
    const double filesPerSecond = snapshot.library.present ? snapshot.library.filesPerSecond : m_libraryScanFilesPerSecond;
    const double etaSeconds = snapshot.library.present ? snapshot.library.etaSeconds : m_libraryScanEtaSeconds;

    if (m_libraryScanRootsCompleted != rootsCompleted) {
        m_libraryScanRootsCompleted = rootsCompleted;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_libraryScanRootsTotal != rootsTotal) {
        m_libraryScanRootsTotal = rootsTotal;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_libraryScanDiscovered != discovered) {
        m_libraryScanDiscovered = discovered;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (m_libraryScanProcessed != processed) {
        m_libraryScanProcessed = processed;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (!qFuzzyCompare(m_libraryScanFilesPerSecond + 1.0, filesPerSecond + 1.0)) {
        m_libraryScanFilesPerSecond = filesPerSecond;
        changed = true;
        snapshotSignalChanged = true;
    }
    if (!qFuzzyCompare(m_libraryScanEtaSeconds + 2.0, etaSeconds + 2.0)) {
        m_libraryScanEtaSeconds = etaSeconds;
        changed = true;
        snapshotSignalChanged = true;
    }

    finishSnapshotSection(snapshotLibraryMs);
    if (playbackSignalChanged) {
        m_pollPlaybackChanged = true;
    }
    if (snapshotSignalChanged) {
        m_pollSnapshotChanged = true;
    }
    if (profileSnapshot) {
        const double snapshotTotalMs =
            static_cast<double>(snapshotProfileTimer.nsecsElapsed()) / 1000000.0;
        const double snapshotMaxSectionMs =
            std::max({snapshotPlaybackMs,
                      snapshotQueueMs,
                      snapshotTrackMs,
                      snapshotSettingsMs,
                      snapshotLastFmMs,
                      snapshotLibraryMs});
        if ((snapshotTotalMs >= 4.0 || snapshotMaxSectionMs >= 2.0)
            && shouldEmitUiProfileLog(nowMs, &m_lastSnapshotApplyProfileLogMs, 50)) {
            FERROUS_PROFILE_LOG_DIAGNOSTIC(
                QStringLiteral("ui-prof"),
                QStringLiteral(
                    "snapshot_apply ms=%1 playback_ms=%2 queue_ms=%3 track_ms=%4 "
                    "settings_ms=%5 lastfm_ms=%6 library_ms=%7 queue_tracks=%8 "
                    "root_entries=%9 changed=%10")
                    .arg(snapshotTotalMs, 0, 'f', 2)
                    .arg(snapshotPlaybackMs, 0, 'f', 2)
                    .arg(snapshotQueueMs, 0, 'f', 2)
                    .arg(snapshotTrackMs, 0, 'f', 2)
                    .arg(snapshotSettingsMs, 0, 'f', 2)
                    .arg(snapshotLastFmMs, 0, 'f', 2)
                    .arg(snapshotLibraryMs, 0, 'f', 2)
                    .arg(snapshot.queue.present ? snapshot.queue.tracks.size() : m_queuePaths.size())
                    .arg(snapshot.library.present ? snapshot.library.rootEntries.size() : m_libraryRootEntries.size())
                    .arg(changed ? 1 : 0));
        }
    }

    return changed;
}

QString BridgeClient::formatSeconds(double seconds) {
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
        .arg(minutes, 2, 10, QChar('0'))
        .arg(secs, 2, 10, QChar('0'));
}

QString BridgeClient::formatDurationCompact(double seconds) {
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
