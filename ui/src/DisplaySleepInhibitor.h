// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>

#include <memory>

class DisplaySleepInhibitorBackend {
public:
    virtual ~DisplaySleepInhibitorBackend() = default;

    virtual void setInhibited(bool inhibited) = 0;
};

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

private:
    std::unique_ptr<DisplaySleepInhibitorBackend> m_backend;
    bool m_inhibited{false};
};
