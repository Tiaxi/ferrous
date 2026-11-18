// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <utility>

// Include after BridgeClient.h with its private members made public.

// While it lives, the backend of each client made fails to start, giving
// `reason` as the backend would; the clients made before keep theirs.
class ScopedFailingBridgeStart {
public:
    explicit ScopedFailingBridgeStart(QString reason)
        : m_previous(BridgeClient::s_bridgeCreator) {
        failureReason() = std::move(reason);
        BridgeClient::s_bridgeCreator = [](QString *why) -> FerrousFfiBridge * {
            if (why != nullptr) {
                *why = failureReason();
            }
            return nullptr;
        };
    }
    ~ScopedFailingBridgeStart() {
        BridgeClient::s_bridgeCreator = m_previous;
        failureReason().clear();
    }
    ScopedFailingBridgeStart(const ScopedFailingBridgeStart &) = delete;
    ScopedFailingBridgeStart &operator=(const ScopedFailingBridgeStart &) = delete;

private:
    static QString &failureReason() {
        static QString reason;
        return reason;
    }

    BridgeClient::BridgeCreator m_previous;
};
