#include "AppInstanceController.h"

#include "WindowActivation.h"

#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusReply>
#include <QString>

namespace {

constexpr auto kAppInstanceService = "org.ferrous.Application";
constexpr auto kAppInstanceObjectPath = "/org/ferrous/Application";
constexpr auto kAppInstanceInterface = "org.ferrous.Application";

QString fallbackError(const QString &message, const QString &fallback) {
    return message.trimmed().isEmpty() ? fallback : message;
}

class AppInstanceAdaptor final : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.ferrous.Application")

public:
    explicit AppInstanceAdaptor(AppInstanceController *controller)
        : QDBusAbstractAdaptor(controller)
        , m_controller(controller) {}

public slots:
    bool OpenPaths(const QStringList &paths) { return m_controller->openPaths(paths); }

private:
    AppInstanceController *m_controller;
};

class DbusAppInstanceTransport final : public AppInstanceTransport {
public:
    ~DbusAppInstanceTransport() override {
        QDBusConnection sessionBus = QDBusConnection::sessionBus();
        if (m_serviceRegistered && sessionBus.isConnected()) {
            sessionBus.unregisterService(QString::fromUtf8(kAppInstanceService));
        }
        if (m_objectRegistered && sessionBus.isConnected()) {
            sessionBus.unregisterObject(
                QString::fromUtf8(kAppInstanceObjectPath),
                QDBusConnection::UnregisterTree);
        }
    }

    bool isAvailable() const override {
        return QDBusConnection::sessionBus().isConnected();
    }

    bool startPrimary(QObject *object, QString *errorMessage) override {
        QDBusConnection sessionBus = QDBusConnection::sessionBus();
        if (!sessionBus.isConnected()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("session D-Bus is not available");
            }
            return false;
        }

        if (!sessionBus.registerObject(
                QString::fromUtf8(kAppInstanceObjectPath),
                object,
                QDBusConnection::ExportAdaptors)) {
            if (errorMessage != nullptr) {
                *errorMessage = fallbackError(
                    sessionBus.lastError().message(),
                    QStringLiteral("failed to register Ferrous application D-Bus object"));
            }
            return false;
        }
        m_objectRegistered = true;

        if (!sessionBus.registerService(QString::fromUtf8(kAppInstanceService))) {
            sessionBus.unregisterObject(
                QString::fromUtf8(kAppInstanceObjectPath),
                QDBusConnection::UnregisterTree);
            m_objectRegistered = false;
            if (errorMessage != nullptr) {
                *errorMessage = fallbackError(
                    sessionBus.lastError().message(),
                    QStringLiteral("failed to register Ferrous application D-Bus service"));
            }
            return false;
        }

        m_serviceRegistered = true;
        return true;
    }

    bool remoteServiceExists() const override {
        QDBusConnection sessionBus = QDBusConnection::sessionBus();
        if (!sessionBus.isConnected()) {
            return false;
        }
        QDBusConnectionInterface *iface = sessionBus.interface();
        if (iface == nullptr) {
            return false;
        }
        const QDBusReply<bool> reply = iface->isServiceRegistered(QString::fromUtf8(kAppInstanceService));
        return reply.isValid() && reply.value();
    }

    bool forwardToPrimary(const QStringList &paths, QString *errorMessage) override {
        QDBusConnection sessionBus = QDBusConnection::sessionBus();
        if (!sessionBus.isConnected()) {
            if (errorMessage != nullptr) {
                *errorMessage = QStringLiteral("session D-Bus is not available");
            }
            return false;
        }

        QDBusInterface iface(
            QString::fromUtf8(kAppInstanceService),
            QString::fromUtf8(kAppInstanceObjectPath),
            QString::fromUtf8(kAppInstanceInterface),
            sessionBus);
        if (!iface.isValid()) {
            if (errorMessage != nullptr) {
                *errorMessage = fallbackError(
                    iface.lastError().message(),
                    QStringLiteral("failed to connect to the running Ferrous instance"));
            }
            return false;
        }

        const QDBusReply<bool> reply = iface.call(QStringLiteral("OpenPaths"), paths);
        if (!reply.isValid()) {
            if (errorMessage != nullptr) {
                *errorMessage = fallbackError(
                    reply.error().message(),
                    QStringLiteral("failed to hand off the open request to the running Ferrous instance"));
            }
            return false;
        }
        if (!reply.value() && errorMessage != nullptr) {
            *errorMessage = QStringLiteral("the running Ferrous instance rejected the open request");
        }
        return reply.value();
    }

private:
    bool m_objectRegistered{false};
    bool m_serviceRegistered{false};
};

} // namespace

AppInstanceController::AppInstanceController(QObject *parent)
    : AppInstanceController(std::make_unique<DbusAppInstanceTransport>(), parent) {}

AppInstanceController::AppInstanceController(
    std::unique_ptr<AppInstanceTransport> transport,
    QObject *parent)
    : QObject(parent)
    , m_transport(std::move(transport)) {
    new AppInstanceAdaptor(this);
}

AppInstanceController::~AppInstanceController() = default;

AppInstanceController::StartupResult AppInstanceController::initialize(
    const QStringList &startupArgs,
    QString *errorMessage) {
    const QStringList sanitized = sanitizeOpenTargets(startupArgs);
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    if (!m_transport || !m_transport->isAvailable()) {
        if (!sanitized.isEmpty()) {
            m_pendingRequests.push_back(sanitized);
        }
        return StartupResult::ContinuePrimary;
    }

    QString transportError;
    if (m_transport->startPrimary(this, &transportError)) {
        if (!sanitized.isEmpty()) {
            m_pendingRequests.push_back(sanitized);
        }
        return StartupResult::ContinuePrimary;
    }

    if (m_transport->remoteServiceExists()) {
        if (m_transport->forwardToPrimary(sanitized, &transportError)) {
            return StartupResult::ExitAfterForward;
        }
        if (errorMessage != nullptr) {
            *errorMessage = transportError;
        }
        return StartupResult::ExitWithError;
    }

    if (!sanitized.isEmpty()) {
        m_pendingRequests.push_back(sanitized);
    }
    return StartupResult::ContinuePrimary;
}

void AppInstanceController::setOpenHandler(std::function<void(const QStringList &paths)> handler) {
    m_openHandler = std::move(handler);
    flushPendingRequests();
}

bool AppInstanceController::openPaths(const QStringList &paths) {
    const QStringList sanitized = sanitizeOpenTargets(paths);
    if (sanitized.isEmpty()) {
        activateTopLevelWindow();
        return true;
    }
    if (!m_openHandler) {
        m_pendingRequests.push_back(sanitized);
        return true;
    }
    m_openHandler(sanitized);
    activateTopLevelWindow();
    return true;
}

QStringList AppInstanceController::sanitizeOpenTargets(const QStringList &rawPaths) {
    QStringList sanitized;
    sanitized.reserve(rawPaths.size());
    for (const QString &path : rawPaths) {
        if (!path.trimmed().isEmpty()) {
            sanitized.push_back(path);
        }
    }
    return sanitized;
}

void AppInstanceController::flushPendingRequests() {
    if (!m_openHandler || m_pendingRequests.isEmpty()) {
        return;
    }
    const QList<QStringList> pending = m_pendingRequests;
    m_pendingRequests.clear();
    for (const QStringList &paths : pending) {
        m_openHandler(paths);
        activateTopLevelWindow();
    }
}

#include "AppInstanceController.moc"
