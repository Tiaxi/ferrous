// SPDX-License-Identifier: GPL-3.0-or-later

#include "MprisController.h"

#include "BridgeClient.h"
#include "WindowActivation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QFileInfo>
#include <QUrl>
#include <QTimer>
#include <QVariant>

namespace {

constexpr auto kMprisService = "org.mpris.MediaPlayer2.ferrous";
constexpr auto kMprisObjectPath = "/org/mpris/MediaPlayer2";
constexpr auto kMprisRootInterface = "org.mpris.MediaPlayer2";
constexpr auto kMprisPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr auto kDesktopEntryName = "ferrous";
constexpr auto kNoTrackPath = "/org/mpris/MediaPlayer2/TrackList/NoTrack";

bool fuzzyDifferent(double left, double right) {
    return std::abs(left - right) > 0.0005;
}

QString normalizedPlaybackState(const BridgeClient *bridge) {
    if (bridge == nullptr) {
        return QStringLiteral("Stopped");
    }
    const QString state = bridge->playbackState();
    if (state == QStringLiteral("Playing") || state == QStringLiteral("Paused")) {
        return state;
    }
    return QStringLiteral("Stopped");
}

} // namespace

class MprisRootAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool Fullscreen READ fullscreen)
    Q_PROPERTY(bool CanSetFullscreen READ canSetFullscreen)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ supportedUriSchemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ supportedMimeTypes)

public:
    explicit MprisRootAdaptor(MprisController *controller)
        : QDBusAbstractAdaptor(controller)
        , m_controller(controller) {}

    bool canQuit() const { return m_controller->canQuit(); }
    bool fullscreen() const { return m_controller->fullscreen(); }
    bool canSetFullscreen() const { return m_controller->canSetFullscreen(); }
    bool canRaise() const { return m_controller->canRaise(); }
    bool hasTrackList() const { return m_controller->hasTrackList(); }
    QString identity() const { return m_controller->identity(); }
    QString desktopEntry() const { return m_controller->desktopEntry(); }
    QStringList supportedUriSchemes() const { return m_controller->supportedUriSchemes(); }
    QStringList supportedMimeTypes() const { return m_controller->supportedMimeTypes(); }

public slots:
    void Raise() { m_controller->raiseWindow(); }
    void Quit() { m_controller->quitApplication(); }

private:
    MprisController *m_controller;
};

class MprisPlayerAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(QString LoopStatus READ loopStatus WRITE setLoopStatus)
    Q_PROPERTY(double Rate READ rate)
    Q_PROPERTY(bool Shuffle READ shuffle WRITE setShuffle)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ positionUs)
    Q_PROPERTY(double MinimumRate READ minimumRate)
    Q_PROPERTY(double MaximumRate READ maximumRate)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl)

public:
    explicit MprisPlayerAdaptor(MprisController *controller)
        : QDBusAbstractAdaptor(controller)
        , m_controller(controller) {}

    QString playbackStatus() const { return m_controller->playbackStatus(); }
    QString loopStatus() const { return m_controller->loopStatus(); }
    void setLoopStatus(const QString &value) { m_controller->setLoopStatus(value); }
    double rate() const { return m_controller->rate(); }
    bool shuffle() const { return m_controller->shuffle(); }
    void setShuffle(bool value) { m_controller->setShuffle(value); }
    QVariantMap metadata() const { return m_controller->metadata(); }
    double volume() const { return m_controller->volume(); }
    void setVolume(double value) { m_controller->setVolume(value); }
    qlonglong positionUs() const { return m_controller->positionUs(); }
    double minimumRate() const { return m_controller->minimumRate(); }
    double maximumRate() const { return m_controller->maximumRate(); }
    bool canGoNext() const { return m_controller->canGoNext(); }
    bool canGoPrevious() const { return m_controller->canGoPrevious(); }
    bool canPlay() const { return m_controller->canPlay(); }
    bool canPause() const { return m_controller->canPause(); }
    bool canSeek() const { return m_controller->canSeek(); }
    bool canControl() const { return m_controller->canControl(); }

public slots:
    void Next() { m_controller->next(); }
    void Previous() { m_controller->previous(); }
    void Pause() { m_controller->pause(); }
    void PlayPause() { m_controller->playPause(); }
    void Stop() { m_controller->stop(); }
    void Play() { m_controller->play(); }
    void Seek(qlonglong offsetUs) { m_controller->seek(offsetUs); }
    void SetPosition(const QDBusObjectPath &trackId, qlonglong positionUs) {
        m_controller->setPosition(trackId.path(), positionUs);
    }

signals:
    void Seeked(qlonglong positionUs);

private:
    MprisController *m_controller;
};

MprisController::MprisController(BridgeClient *bridge, QObject *parent)
    : QObject(parent)
    , m_bridge(bridge) {
    new MprisRootAdaptor(this);
    new MprisPlayerAdaptor(this);
    if (m_bridge != nullptr) {
        connect(m_bridge, &BridgeClient::playbackChanged, this, [this]() {
            if (m_serviceRegistered) {
                publishPlayerState();
            }
        });
        connect(m_bridge, &BridgeClient::trackIdentityChanged, this, [this]() {
            if (m_serviceRegistered) {
                publishPlayerState();
            }
        });
        connect(m_bridge, &BridgeClient::trackMetadataChanged, this, [this]() {
            if (m_serviceRegistered) {
                publishPlayerState();
            }
        });
        connect(m_bridge, &BridgeClient::snapshotChanged, this, [this]() {
            setEnabled(m_bridge->systemMediaControlsEnabled());
            if (m_serviceRegistered) {
                publishPlayerState();
            }
        });
        connect(m_bridge, &BridgeClient::connectedChanged, this, [this]() {
            if (m_serviceRegistered) {
                publishPlayerState();
            }
        });
    }

    QTimer::singleShot(0, this, [this]() {
        setEnabled(m_bridge != nullptr ? m_bridge->systemMediaControlsEnabled() : true);
    });
}

MprisController::~MprisController() {
    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    if (!sessionBus.isConnected()) {
        return;
    }
    if (m_serviceRegistered) {
        sessionBus.unregisterService(QString::fromUtf8(kMprisService));
    }
    if (m_objectRegistered) {
        sessionBus.unregisterObject(QString::fromUtf8(kMprisObjectPath), QDBusConnection::UnregisterTree);
    }
}

bool MprisController::enabled() const {
    return m_enabled;
}

void MprisController::setEnabled(bool enabled) {
    if (m_enabled == enabled && (enabled ? m_serviceRegistered : !m_serviceRegistered)) {
        return;
    }
    m_enabled = enabled;
    updateRegistration();
}

bool MprisController::canQuit() const {
    return true;
}

bool MprisController::fullscreen() const {
    return false;
}

bool MprisController::canSetFullscreen() const {
    return false;
}

bool MprisController::canRaise() const {
    return true;
}

bool MprisController::hasTrackList() const {
    return false;
}

QString MprisController::identity() const {
    return QStringLiteral("Ferrous");
}

QString MprisController::desktopEntry() const {
    return QString::fromUtf8(kDesktopEntryName);
}

QStringList MprisController::supportedUriSchemes() const {
    return {QStringLiteral("file")};
}

QStringList MprisController::supportedMimeTypes() const {
    return {};
}

QString MprisController::playbackStatus() const {
    return normalizedPlaybackState(m_bridge);
}

QString MprisController::loopStatus() const {
    if (m_bridge == nullptr) {
        return QStringLiteral("None");
    }
    switch (std::clamp(m_bridge->repeatMode(), 0, 2)) {
    case 1:
        return QStringLiteral("Track");
    case 2:
        return QStringLiteral("Playlist");
    default:
        return QStringLiteral("None");
    }
}

double MprisController::rate() const {
    return 1.0;
}

double MprisController::minimumRate() const {
    return 1.0;
}

double MprisController::maximumRate() const {
    return 1.0;
}

QVariantMap MprisController::metadata() const {
    return buildMetadata();
}

qlonglong MprisController::positionUs() const {
    if (m_bridge == nullptr) {
        return 0;
    }
    return static_cast<qlonglong>(std::llround(std::max(0.0, m_bridge->positionSeconds()) * 1000000.0));
}

double MprisController::volume() const {
    if (m_bridge == nullptr) {
        return 1.0;
    }
    return std::clamp(m_bridge->volume(), 0.0, 1.0);
}

bool MprisController::shuffle() const {
    return m_bridge != nullptr && m_bridge->shuffleEnabled();
}

bool MprisController::canGoNext() const {
    return m_bridge != nullptr && m_bridge->queueLength() > 0;
}

bool MprisController::canGoPrevious() const {
    return m_bridge != nullptr && m_bridge->queueLength() > 0;
}

bool MprisController::canPlay() const {
    return m_bridge != nullptr
        && (m_bridge->queueLength() > 0 || !m_bridge->currentTrackPath().trimmed().isEmpty());
}

bool MprisController::canPause() const {
    return normalizedPlaybackState(m_bridge) == QStringLiteral("Playing");
}

bool MprisController::canSeek() const {
    return m_bridge != nullptr
        && !m_bridge->currentTrackPath().trimmed().isEmpty()
        && m_bridge->durationSeconds() > 0.0;
}

bool MprisController::canControl() const {
    return m_bridge != nullptr;
}

void MprisController::raiseWindow() {
    activateTopLevelWindow();
}

void MprisController::quitApplication() {
    if (m_bridge != nullptr) {
        m_bridge->shutdown();
    }
    if (QCoreApplication *app = QCoreApplication::instance()) {
        app->quit();
    }
}

void MprisController::next() {
    if (m_bridge != nullptr) {
        m_bridge->next();
    }
}

void MprisController::previous() {
    if (m_bridge != nullptr) {
        m_bridge->previous();
    }
}

void MprisController::pause() {
    if (m_bridge != nullptr && normalizedPlaybackState(m_bridge) == QStringLiteral("Playing")) {
        m_bridge->pause();
    }
}

void MprisController::playPause() {
    if (m_bridge == nullptr) {
        return;
    }
    if (normalizedPlaybackState(m_bridge) == QStringLiteral("Playing")) {
        m_bridge->pause();
        return;
    }
    m_bridge->play();
}

void MprisController::stop() {
    if (m_bridge != nullptr) {
        m_bridge->stop();
    }
}

void MprisController::play() {
    if (m_bridge != nullptr) {
        m_bridge->play();
    }
}

void MprisController::setLoopStatus(const QString &loopStatusValue) {
    if (m_bridge == nullptr) {
        return;
    }
    const QString normalized = loopStatusValue.trimmed();
    if (normalized == QStringLiteral("Track")) {
        m_bridge->setRepeatMode(1);
        return;
    }
    if (normalized == QStringLiteral("Playlist")) {
        m_bridge->setRepeatMode(2);
        return;
    }
    m_bridge->setRepeatMode(0);
}

void MprisController::setShuffle(bool enabledValue) {
    if (m_bridge != nullptr) {
        m_bridge->setShuffleEnabled(enabledValue);
    }
}

void MprisController::setVolume(double value) {
    if (m_bridge != nullptr) {
        m_bridge->setVolume(std::clamp(value, 0.0, 1.0));
    }
}

void MprisController::seek(qlonglong offsetUs) {
    if (m_bridge == nullptr || !canSeek()) {
        return;
    }
    const double nextSeconds = std::clamp(
        m_bridge->positionSeconds() + (static_cast<double>(offsetUs) / 1000000.0),
        0.0,
        std::max(0.0, m_bridge->durationSeconds()));
    m_bridge->seek(nextSeconds);
}

void MprisController::setPosition(const QString &trackIdPath, qlonglong positionUs) {
    if (m_bridge == nullptr || !canSeek()) {
        return;
    }
    if (trackIdPath != currentTrackIdPath()) {
        return;
    }
    const double nextSeconds = std::clamp(
        static_cast<double>(positionUs) / 1000000.0,
        0.0,
        std::max(0.0, m_bridge->durationSeconds()));
    m_bridge->seek(nextSeconds);
}

bool MprisController::ensureObjectRegistered() {
    if (m_objectRegistered) {
        return true;
    }
    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    if (!sessionBus.isConnected()) {
        return false;
    }
    m_objectRegistered = sessionBus.registerObject(
        QString::fromUtf8(kMprisObjectPath),
        this,
        QDBusConnection::ExportAdaptors);
    return m_objectRegistered;
}

void MprisController::updateRegistration() {
    QDBusConnection sessionBus = QDBusConnection::sessionBus();
    if (!m_enabled) {
        if (m_serviceRegistered && sessionBus.isConnected()) {
            sessionBus.unregisterService(QString::fromUtf8(kMprisService));
        }
        m_serviceRegistered = false;
        m_hasPublishedPlayerState = false;
        return;
    }

    if (!ensureObjectRegistered()) {
        return;
    }
    if (!m_serviceRegistered) {
        m_serviceRegistered = sessionBus.registerService(QString::fromUtf8(kMprisService));
        if (!m_serviceRegistered) {
            return;
        }
        m_hasPublishedPlayerState = false;
        publishRootProperties();
    }
    publishPlayerState();
}

void MprisController::publishRootProperties() {
    if (!m_serviceRegistered) {
        return;
    }
    QVariantMap changed;
    changed.insert(QStringLiteral("CanQuit"), canQuit());
    changed.insert(QStringLiteral("Fullscreen"), fullscreen());
    changed.insert(QStringLiteral("CanSetFullscreen"), canSetFullscreen());
    changed.insert(QStringLiteral("CanRaise"), canRaise());
    changed.insert(QStringLiteral("HasTrackList"), hasTrackList());
    changed.insert(QStringLiteral("Identity"), identity());
    changed.insert(QStringLiteral("DesktopEntry"), desktopEntry());
    changed.insert(QStringLiteral("SupportedUriSchemes"), supportedUriSchemes());
    changed.insert(QStringLiteral("SupportedMimeTypes"), supportedMimeTypes());
    emitPropertiesChanged(QString::fromUtf8(kMprisRootInterface), changed);
}

void MprisController::publishPlayerState() {
    if (!m_serviceRegistered) {
        return;
    }

    const PublishedPlayerState nextState = currentPlayerState();
    QVariantMap changed;
    if (!m_hasPublishedPlayerState || m_lastPlayerState.playbackStatus != nextState.playbackStatus) {
        changed.insert(QStringLiteral("PlaybackStatus"), nextState.playbackStatus);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.loopStatus != nextState.loopStatus) {
        changed.insert(QStringLiteral("LoopStatus"), nextState.loopStatus);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.metadata != nextState.metadata) {
        changed.insert(QStringLiteral("Metadata"), nextState.metadata);
    }
    if (!m_hasPublishedPlayerState || fuzzyDifferent(m_lastPlayerState.volume, nextState.volume)) {
        changed.insert(QStringLiteral("Volume"), nextState.volume);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.shuffle != nextState.shuffle) {
        changed.insert(QStringLiteral("Shuffle"), nextState.shuffle);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.canGoNext != nextState.canGoNext) {
        changed.insert(QStringLiteral("CanGoNext"), nextState.canGoNext);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.canGoPrevious != nextState.canGoPrevious) {
        changed.insert(QStringLiteral("CanGoPrevious"), nextState.canGoPrevious);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.canPlay != nextState.canPlay) {
        changed.insert(QStringLiteral("CanPlay"), nextState.canPlay);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.canPause != nextState.canPause) {
        changed.insert(QStringLiteral("CanPause"), nextState.canPause);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.canSeek != nextState.canSeek) {
        changed.insert(QStringLiteral("CanSeek"), nextState.canSeek);
    }
    if (!m_hasPublishedPlayerState || m_lastPlayerState.canControl != nextState.canControl) {
        changed.insert(QStringLiteral("CanControl"), nextState.canControl);
    }
    if (!m_hasPublishedPlayerState) {
        changed.insert(QStringLiteral("Rate"), nextState.canControl ? rate() : 1.0);
        changed.insert(QStringLiteral("MinimumRate"), minimumRate());
        changed.insert(QStringLiteral("MaximumRate"), maximumRate());
    }
    if (!changed.isEmpty()) {
        emitPropertiesChanged(QString::fromUtf8(kMprisPlayerInterface), changed);
    }
    if (m_hasPublishedPlayerState && shouldEmitSeeked(nextState)) {
        emitSeeked(nextState.positionUs);
    }
    m_lastPlayerState = nextState;
    m_hasPublishedPlayerState = true;
}

void MprisController::emitPropertiesChanged(
    const QString &interfaceName,
    const QVariantMap &changed) const {
    if (changed.isEmpty()) {
        return;
    }
    QDBusMessage message = QDBusMessage::createSignal(
        QString::fromUtf8(kMprisObjectPath),
        QStringLiteral("org.freedesktop.DBus.Properties"),
        QStringLiteral("PropertiesChanged"));
    message << interfaceName << changed << QStringList{};
    QDBusConnection::sessionBus().send(message);
}

void MprisController::emitSeeked(qlonglong positionUs) const {
    QDBusMessage message = QDBusMessage::createSignal(
        QString::fromUtf8(kMprisObjectPath),
        QString::fromUtf8(kMprisPlayerInterface),
        QStringLiteral("Seeked"));
    message << positionUs;
    QDBusConnection::sessionBus().send(message);
}

QString MprisController::currentTrackIdPath() const {
    if (m_bridge == nullptr) {
        return QString::fromUtf8(kNoTrackPath);
    }
    const QString trackPath = m_bridge->currentTrackPath().trimmed();
    if (trackPath.isEmpty()) {
        return QString::fromUtf8(kNoTrackPath);
    }
    const QByteArray digest = QCryptographicHash::hash(
        trackPath.toUtf8(),
        QCryptographicHash::Sha1).toHex();
    return QStringLiteral("/org/mpris/MediaPlayer2/Track/%1").arg(QString::fromLatin1(digest));
}

QVariantMap MprisController::buildMetadata() const {
    QVariantMap out;
    out.insert(
        QStringLiteral("mpris:trackid"),
        QVariant::fromValue(QDBusObjectPath(currentTrackIdPath())));
    if (m_bridge == nullptr) {
        return out;
    }

    const QString trackPath = m_bridge->currentTrackPath().trimmed();
    if (!trackPath.isEmpty()) {
        out.insert(
            QStringLiteral("xesam:url"),
            QUrl::fromLocalFile(trackPath).toString(QUrl::FullyEncoded));
    }

    QString title = m_bridge->currentTrackTitle().trimmed();
    if (title.isEmpty() && !trackPath.isEmpty()) {
        const QFileInfo info(trackPath);
        title = info.fileName().trimmed();
        if (title.isEmpty()) {
            title = trackPath;
        }
    }
    if (!title.isEmpty()) {
        out.insert(QStringLiteral("xesam:title"), title);
    }
    const QString artist = m_bridge->currentTrackArtist().trimmed();
    if (!artist.isEmpty()) {
        out.insert(QStringLiteral("xesam:artist"), QStringList{artist});
        out.insert(QStringLiteral("xesam:albumArtist"), QStringList{artist});
    }
    const QString album = m_bridge->currentTrackAlbum().trimmed();
    if (!album.isEmpty()) {
        out.insert(QStringLiteral("xesam:album"), album);
    }
    const QString genre = m_bridge->currentTrackGenre().trimmed();
    if (!genre.isEmpty()) {
        out.insert(QStringLiteral("xesam:genre"), QStringList{genre});
    }
    const QVariant year = m_bridge->currentTrackYear();
    if (year.isValid() && !year.isNull()) {
        out.insert(QStringLiteral("xesam:contentCreated"), QString::number(year.toInt()));
    }
    const QString artUrl = m_bridge->currentTrackCoverPath().trimmed();
    if (!artUrl.isEmpty()) {
        out.insert(QStringLiteral("mpris:artUrl"), artUrl);
    }
    if (m_bridge->durationSeconds() > 0.0) {
        out.insert(
            QStringLiteral("mpris:length"),
            static_cast<qlonglong>(std::llround(m_bridge->durationSeconds() * 1000000.0)));
    }
    return out;
}

MprisController::PublishedPlayerState MprisController::currentPlayerState() const {
    PublishedPlayerState state;
    state.playbackStatus = playbackStatus();
    state.loopStatus = loopStatus();
    state.metadata = metadata();
    state.currentTrackPath = m_bridge != nullptr ? m_bridge->currentTrackPath().trimmed() : QString{};
    state.positionUs = positionUs();
    state.volume = volume();
    state.shuffle = shuffle();
    state.canGoNext = canGoNext();
    state.canGoPrevious = canGoPrevious();
    state.canPlay = canPlay();
    state.canPause = canPause();
    state.canSeek = canSeek();
    state.canControl = canControl();
    return state;
}

bool MprisController::shouldEmitSeeked(const PublishedPlayerState &nextState) const {
    if (!nextState.canSeek) {
        return false;
    }
    if (m_lastPlayerState.currentTrackPath != nextState.currentTrackPath) {
        return true;
    }
    const qlonglong delta = nextState.positionUs - m_lastPlayerState.positionUs;
    if (m_lastPlayerState.playbackStatus == QStringLiteral("Playing")
        && nextState.playbackStatus == QStringLiteral("Playing")
        && delta >= 0
        && delta < 2000000) {
        return false;
    }
    return std::llabs(delta) >= 500000;
}

#include "MprisController.moc"
