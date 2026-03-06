#pragma once

#include <QObject>
#include <QStringList>

#include <functional>
#include <memory>

class AppInstanceTransport {
public:
    virtual ~AppInstanceTransport() = default;

    virtual bool isAvailable() const = 0;
    virtual bool startPrimary(QObject *object, QString *errorMessage) = 0;
    virtual bool remoteServiceExists() const = 0;
    virtual bool forwardToPrimary(const QStringList &paths, QString *errorMessage) = 0;
};

class AppInstanceController : public QObject {
    Q_OBJECT

public:
    enum class StartupResult {
        ContinuePrimary,
        ExitAfterForward,
        ExitWithError,
    };

    explicit AppInstanceController(QObject *parent = nullptr);
    explicit AppInstanceController(std::unique_ptr<AppInstanceTransport> transport, QObject *parent = nullptr);
    ~AppInstanceController() override;

    StartupResult initialize(const QStringList &startupArgs, QString *errorMessage = nullptr);
    void setOpenHandler(std::function<void(const QStringList &paths)> handler);
    bool openPaths(const QStringList &paths);

    static QStringList sanitizeOpenTargets(const QStringList &rawPaths);

private:
    void flushPendingRequests();

    std::unique_ptr<AppInstanceTransport> m_transport;
    std::function<void(const QStringList &paths)> m_openHandler;
    QList<QStringList> m_pendingRequests;
};
