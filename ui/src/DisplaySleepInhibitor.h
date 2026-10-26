// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

#include <functional>
#include <memory>
#include <utility>

#if defined(Q_OS_LINUX)
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QPointer>

#include <functional>
#include <optional>

class QDBusPendingCallWatcher;
#endif

class DisplaySleepInhibitorBackend {
public:
    using FailureHandler = std::function<void(const QString &reason)>;

    virtual ~DisplaySleepInhibitorBackend() = default;

    virtual void setInhibited(bool inhibited) = 0;

    // Called when the display could not be kept awake while it was asked to.
    void setFailureHandler(FailureHandler handler) { m_onFailure = std::move(handler); }

protected:
    void reportFailure(const QString &reason) const {
        if (m_onFailure) {
            m_onFailure(reason);
        }
    }

private:
    FailureHandler m_onFailure;
};

#if defined(Q_OS_LINUX)

// Inhibits idle actions (screen saver, locking, display power-off) through
// org.freedesktop.ScreenSaver. It deliberately avoids
// org.freedesktop.PowerManagement.Inhibit, which also blocks suspend on KDE.
class ScreenSaverInhibitorBackend final : public QObject, public DisplaySleepInhibitorBackend {
public:
    using DbusCall = std::function<QDBusPendingCall(const QDBusMessage &message)>;

    // Sends calls on the session bus unless a call function is given.
    explicit ScreenSaverInhibitorBackend(DbusCall call = {});
    ~ScreenSaverInhibitorBackend() override;

    void setInhibited(bool inhibited) override;

private:
    void finishAcquire(QDBusPendingCallWatcher *watcher);
    void releaseCookie(quint32 cookie);

    DbusCall m_call;
    std::optional<quint32> m_cookie;
    QPointer<QDBusPendingCallWatcher> m_acquireWatcher;
    bool m_inhibited{false};
};

#endif

class DisplaySleepInhibitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool inhibited READ inhibited WRITE setInhibited NOTIFY inhibitedChanged)

public:
    explicit DisplaySleepInhibitor(QObject *parent = nullptr);
    explicit DisplaySleepInhibitor(
        std::unique_ptr<DisplaySleepInhibitorBackend> backend,
        QObject *parent = nullptr);
    ~DisplaySleepInhibitor() override;

    bool inhibited() const;

public slots:
    void setInhibited(bool inhibited);

signals:
    void inhibitedChanged();
    // The display could not be kept awake although it was asked to be.
    void inhibitionFailed(const QString &message);

private:
    std::unique_ptr<DisplaySleepInhibitorBackend> m_backend;
    bool m_inhibited{false};
};
