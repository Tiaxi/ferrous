// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class BridgeClient;

class MprisController : public QObject {
    Q_OBJECT

public:
    explicit MprisController(BridgeClient *bridge, QObject *parent = nullptr);
    ~MprisController() override;

    bool enabled() const;
    void setEnabled(bool enabled);

    bool canQuit() const;
    bool fullscreen() const;
    bool canSetFullscreen() const;
    bool canRaise() const;
    bool hasTrackList() const;
    QString identity() const;
    QString desktopEntry() const;
    QStringList supportedUriSchemes() const;
    QStringList supportedMimeTypes() const;

    QString playbackStatus() const;
    QString loopStatus() const;
    double rate() const;
    double minimumRate() const;
    double maximumRate() const;
    QVariantMap metadata() const;
    qlonglong positionUs() const;
    double volume() const;
    bool shuffle() const;
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canPlay() const;
    bool canPause() const;
    bool canSeek() const;
    bool canControl() const;

    void raiseWindow();
    void quitApplication();
    void next();
    void previous();
    void pause();
    void playPause();
    void stop();
    void play();
    void setLoopStatus(const QString &loopStatus);
    void setShuffle(bool enabled);
    void setVolume(double value);
    void seek(qlonglong offsetUs);
    void setPosition(const QString &trackIdPath, qlonglong positionUs);

private:
    struct PublishedPlayerState {
        QString playbackStatus;
        QString loopStatus;
        QVariantMap metadata;
        QString currentTrackPath;
        qlonglong positionUs{0};
        double volume{1.0};
        bool shuffle{false};
        bool canGoNext{false};
        bool canGoPrevious{false};
        bool canPlay{false};
        bool canPause{false};
        bool canSeek{false};
        bool canControl{false};
    };

    bool ensureObjectRegistered();
    void updateRegistration();
    void publishRootProperties();
    void publishPlayerState();
    void emitPropertiesChanged(const QString &interfaceName, const QVariantMap &changed) const;
    void emitSeeked(qlonglong positionUs) const;
    QString currentTrackIdPath() const;
    QVariantMap buildMetadata() const;
    PublishedPlayerState currentPlayerState() const;
    bool shouldEmitSeeked(const PublishedPlayerState &nextState) const;

    BridgeClient *m_bridge{nullptr};
    bool m_enabled{true};
    bool m_objectRegistered{false};
    bool m_serviceRegistered{false};
    bool m_hasPublishedPlayerState{false};
    PublishedPlayerState m_lastPlayerState;
};
