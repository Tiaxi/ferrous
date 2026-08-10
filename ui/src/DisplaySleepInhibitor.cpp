// SPDX-License-Identifier: GPL-3.0-or-later

#include "DisplaySleepInhibitor.h"

#include <QtGlobal>

#if defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QPointer>
#include <QString>

#include <optional>
#include <utility>
#include <vector>
#endif

namespace {

#if defined(Q_OS_LINUX)

class FreedesktopDisplaySleepInhibitorBackend final
    : public QObject,
      public DisplaySleepInhibitorBackend {
public:
    FreedesktopDisplaySleepInhibitorBackend() {
        m_endpoints.push_back({
            QStringLiteral("org.freedesktop.ScreenSaver"),
            QStringLiteral("/org/freedesktop/ScreenSaver"),
            QStringLiteral("org.freedesktop.ScreenSaver"),
            std::nullopt,
            nullptr,
        });
        m_endpoints.push_back({
            QStringLiteral("org.freedesktop.PowerManagement"),
            QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
            QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
            std::nullopt,
            nullptr,
        });
    }

    ~FreedesktopDisplaySleepInhibitorBackend() override {
        releaseAllCookies();
    }

    void setInhibited(bool inhibited) override {
        if (m_inhibited == inhibited) {
            return;
        }

        m_inhibited = inhibited;
        if (m_inhibited) {
            m_failureReported = false;
            for (std::size_t index = 0; index < m_endpoints.size(); ++index) {
                acquire(index);
            }
            return;
        }

        releaseAllCookies();
    }

private:
    struct Endpoint {
        QString service;
        QString path;
        QString interfaceName;
        std::optional<quint32> cookie;
        QPointer<QDBusPendingCallWatcher> acquireWatcher;
    };

    void acquire(std::size_t index) {
        Endpoint &endpoint = m_endpoints[index];
        if (endpoint.cookie.has_value() || endpoint.acquireWatcher != nullptr) {
            return;
        }

        QDBusMessage message = QDBusMessage::createMethodCall(
            endpoint.service,
            endpoint.path,
            endpoint.interfaceName,
            QStringLiteral("Inhibit"));
        message << QStringLiteral("Ferrous")
                << QStringLiteral("Fullscreen audio visualization is active");

        auto *watcher = new QDBusPendingCallWatcher(
            QDBusConnection::sessionBus().asyncCall(message),
            this);
        endpoint.acquireWatcher = watcher;
        connect(
            watcher,
            &QDBusPendingCallWatcher::finished,
            this,
            [this, index](QDBusPendingCallWatcher *finishedWatcher) {
                finishAcquire(index, finishedWatcher);
            });
    }

    void finishAcquire(std::size_t index, QDBusPendingCallWatcher *watcher) {
        Endpoint &endpoint = m_endpoints[index];
        endpoint.acquireWatcher.clear();

        const QDBusPendingReply<quint32> reply = *watcher;
        watcher->deleteLater();
        if (!reply.isError()) {
            const quint32 cookie = reply.value();
            if (m_inhibited) {
                endpoint.cookie = cookie;
            } else {
                releaseCookie(endpoint, cookie);
            }
        }

        reportFailureIfFinished();
    }

    void reportFailureIfFinished() {
        if (!m_inhibited || m_failureReported) {
            return;
        }

        for (const Endpoint &endpoint : std::as_const(m_endpoints)) {
            if (endpoint.cookie.has_value() || endpoint.acquireWatcher != nullptr) {
                return;
            }
        }

        m_failureReported = true;
        qWarning() << "Unable to inhibit display sleep: no supported desktop inhibition service responded";
    }

    static void releaseCookie(const Endpoint &endpoint, quint32 cookie) {
        QDBusMessage message = QDBusMessage::createMethodCall(
            endpoint.service,
            endpoint.path,
            endpoint.interfaceName,
            QStringLiteral("UnInhibit"));
        message << cookie;
        QDBusConnection::sessionBus().asyncCall(message);
    }

    void releaseAllCookies() {
        for (Endpoint &endpoint : m_endpoints) {
            if (!endpoint.cookie.has_value()) {
                continue;
            }
            releaseCookie(endpoint, *endpoint.cookie);
            endpoint.cookie.reset();
        }
    }

    std::vector<Endpoint> m_endpoints;
    bool m_inhibited{false};
    bool m_failureReported{false};
};

#else

class FreedesktopDisplaySleepInhibitorBackend final : public DisplaySleepInhibitorBackend {
public:
    void setInhibited(bool inhibited) override {
        Q_UNUSED(inhibited)
    }
};

#endif

} // namespace

DisplaySleepInhibitor::DisplaySleepInhibitor(QObject *parent)
    : DisplaySleepInhibitor(
          std::make_unique<FreedesktopDisplaySleepInhibitorBackend>(),
          parent) {}

DisplaySleepInhibitor::DisplaySleepInhibitor(
    std::unique_ptr<DisplaySleepInhibitorBackend> backend,
    QObject *parent)
    : QObject(parent),
      m_backend(std::move(backend)) {}

DisplaySleepInhibitor::~DisplaySleepInhibitor() = default;

bool DisplaySleepInhibitor::inhibited() const {
    return m_inhibited;
}

void DisplaySleepInhibitor::setInhibited(bool inhibited) {
    if (m_inhibited == inhibited) {
        return;
    }

    m_inhibited = inhibited;
    m_backend->setInhibited(m_inhibited);
    emit inhibitedChanged();
}
