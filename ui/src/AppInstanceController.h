// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>
#include <memory>

class AppInstanceTransport {
public:
    virtual ~AppInstanceTransport() = default;

    virtual bool isAvailable() const = 0;
    virtual bool startPrimary(QObject *object, QString *errorMessage) = 0;
    virtual bool remoteServiceExists() const = 0;
    virtual bool forwardToPrimary(
        const QStringList &paths,
        const QString &activationToken,
        QString *errorMessage) = 0;
};

class AppInstanceController : public QObject {
    Q_OBJECT

public:
    enum class StartupResult {
        ContinuePrimary,
        ExitAfterForward,
        ExitWithError,
    };

    // Key for the xdg-activation token in the platform data of the D-Bus
    // Open method, as in org.freedesktop.Application.
    static constexpr auto kActivationTokenKey = "activation-token";

    explicit AppInstanceController(QObject *parent = nullptr);
    explicit AppInstanceController(std::unique_ptr<AppInstanceTransport> transport, QObject *parent = nullptr);
    ~AppInstanceController() override;

    // activationToken is this process's XDG_ACTIVATION_TOKEN; a second
    // instance forwards it so the primary may take focus on Wayland.
    StartupResult initialize(
        const QStringList &startupArgs,
        QString *errorMessage = nullptr,
        const QString &activationToken = QString());
    void setOpenHandler(std::function<void(const QStringList &paths)> handler);
    // Replaces how the main window is activated; tests use this.
    void setActivationHandler(std::function<void(const QString &activationToken)> handler);
    bool openPaths(const QStringList &paths, const QString &activationToken = QString());
    bool open(const QStringList &paths, const QVariantMap &platformData);

    static QStringList sanitizeOpenTargets(const QStringList &rawPaths);

private:
    struct PendingOpenRequest {
        QStringList paths;
        QString activationToken;
    };

    void flushPendingRequests();

    std::unique_ptr<AppInstanceTransport> m_transport;
    std::function<void(const QStringList &paths)> m_openHandler;
    std::function<void(const QString &activationToken)> m_activationHandler;
    QList<PendingOpenRequest> m_pendingRequests;
};
