// SPDX-License-Identifier: GPL-3.0-or-later

#include "DisplaySleepInhibitor.h"

#if defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusPendingCallWatcher>
#include <QDebug>
#include <QString>
#include <QVariant>

#include <utility>
#endif

namespace {

#if defined(Q_OS_LINUX)

constexpr auto kScreenSaverService = "org.freedesktop.ScreenSaver";
constexpr auto kScreenSaverPath = "/org/freedesktop/ScreenSaver";
constexpr auto kScreenSaverInterface = "org.freedesktop.ScreenSaver";

QDBusMessage screenSaverCall(const QString &method) {
    return QDBusMessage::createMethodCall(
        QString::fromUtf8(kScreenSaverService),
        QString::fromUtf8(kScreenSaverPath),
        QString::fromUtf8(kScreenSaverInterface),
        method);
}

std::unique_ptr<DisplaySleepInhibitorBackend> makeDefaultBackend() {
    return std::make_unique<ScreenSaverInhibitorBackend>();
}

#else

class NoopDisplaySleepInhibitorBackend final : public DisplaySleepInhibitorBackend {
public:
    void setInhibited(bool inhibited) override {
        Q_UNUSED(inhibited)
    }
};

std::unique_ptr<DisplaySleepInhibitorBackend> makeDefaultBackend() {
    return std::make_unique<NoopDisplaySleepInhibitorBackend>();
}

#endif

} // namespace

#if defined(Q_OS_LINUX)

ScreenSaverInhibitorBackend::ScreenSaverInhibitorBackend(DbusCall call)
    : m_call(call ? std::move(call) : [](const QDBusMessage &message) {
          return QDBusConnection::sessionBus().asyncCall(message);
      }) {}

ScreenSaverInhibitorBackend::~ScreenSaverInhibitorBackend() {
    if (m_cookie.has_value()) {
        releaseCookie(*m_cookie);
    }
}

void ScreenSaverInhibitorBackend::setInhibited(bool inhibited) {
    if (m_inhibited == inhibited) {
        return;
    }
    m_inhibited = inhibited;
    if (!m_inhibited) {
        if (m_cookie.has_value()) {
            releaseCookie(*m_cookie);
            m_cookie.reset();
        }
        // A pending request is released when its reply arrives.
        return;
    }
    if (m_cookie.has_value() || m_acquireWatcher != nullptr) {
        return;
    }

    QDBusMessage message = screenSaverCall(QStringLiteral("Inhibit"));
    message << QStringLiteral("Ferrous") << QStringLiteral("Fullscreen visualization is playing");
    auto *watcher = new QDBusPendingCallWatcher(m_call(message), this);
    m_acquireWatcher = watcher;
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *finished) {
        finishAcquire(finished);
    });
}

void ScreenSaverInhibitorBackend::finishAcquire(QDBusPendingCallWatcher *watcher) {
    m_acquireWatcher.clear();
    const QDBusMessage reply = watcher->reply();
    watcher->deleteLater();
    const QVariant cookie = reply.arguments().value(0);
    if (reply.type() != QDBusMessage::ReplyMessage || cookie.metaType() != QMetaType::fromType<quint32>()) {
        if (m_inhibited) {
            const QString reason = reply.type() == QDBusMessage::ErrorMessage
                ? reply.errorMessage()
                : QStringLiteral("unexpected reply");
            qWarning() << "Unable to inhibit display sleep:" << reason;
            reportFailure(reason);
        }
        return;
    }
    if (m_inhibited) {
        m_cookie = cookie.value<quint32>();
    } else {
        releaseCookie(cookie.value<quint32>());
    }
}

void ScreenSaverInhibitorBackend::releaseCookie(quint32 cookie) {
    QDBusMessage message = screenSaverCall(QStringLiteral("UnInhibit"));
    message << cookie;
    m_call(message);
}

#endif

DisplaySleepInhibitor::DisplaySleepInhibitor(QObject *parent)
    : DisplaySleepInhibitor(makeDefaultBackend(), parent) {}

DisplaySleepInhibitor::DisplaySleepInhibitor(
    std::unique_ptr<DisplaySleepInhibitorBackend> backend,
    QObject *parent)
    : QObject(parent),
      m_backend(std::move(backend)) {
    m_backend->setFailureHandler([this](const QString &reason) {
        // Only a request still standing failed; a released one no longer matters.
        if (m_inhibited) {
            emit inhibitionFailed(
                QStringLiteral("Couldn't keep the screen awake during fullscreen playback: %1")
                    .arg(reason));
        }
    });
}

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
