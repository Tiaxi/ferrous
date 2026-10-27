// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <cstddef>
#include <optional>

// Points $XDG_CONFIG_HOME, $XDG_DATA_HOME and $XDG_CACHE_HOME at empty
// directories of their own while it lives, then back at what they were, and
// removes the directories. A bridge started meanwhile keeps its settings,
// session, library and caches there, out of every other test's reach.
//
// Make it before the test's first bridge and destroy it after the last one
// is destroyed: changing the environment while the bridge's threads read it
// is not safe. Its directories are made in `parent`, a directory of the
// whole test process: what outlives a test there, such as the GStreamer
// registry written into the first test's cache directory, goes when the
// process removes `parent`. Test binaries write no GPU shader cache; see
// disableGpuShaderCaches() in TestPlatform.h.
class TestUserDirectories {
public:
    explicit TestUserDirectories(const QString &parent)
        : m_root(QDir(parent).filePath(QStringLiteral("test-XXXXXX"))) {
        if (!m_root.isValid()) {
            return;
        }
        for (std::size_t i = 0; i < kVariables.size(); ++i) {
            if (qEnvironmentVariableIsSet(kVariables[i])) {
                m_previous[i] = qgetenv(kVariables[i]);
            }
            qputenv(kVariables[i], QFile::encodeName(m_root.filePath(QString::fromLatin1(kNames[i]))));
        }
    }

    ~TestUserDirectories() {
        if (!m_root.isValid()) {
            return;
        }
        for (std::size_t i = 0; i < kVariables.size(); ++i) {
            if (m_previous[i].has_value()) {
                qputenv(kVariables[i], *m_previous[i]);
            } else {
                qunsetenv(kVariables[i]);
            }
        }
    }

    TestUserDirectories(const TestUserDirectories &) = delete;
    TestUserDirectories &operator=(const TestUserDirectories &) = delete;

    bool isValid() const { return m_root.isValid(); }

private:
    static constexpr std::array<const char *, 3> kVariables{
        "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"};
    static constexpr std::array<const char *, 3> kNames{"config", "data", "cache"};

    QTemporaryDir m_root;
    std::array<std::optional<QByteArray>, 3> m_previous;
};
