// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

namespace DiagnosticsLog {

QString defaultLogPath();
bool appendLine(const QString &logPath, const QString &line);
bool appendLines(const QString &logPath, const QStringList &lines);

// What withoutPaths needs to know to tell paths apart.
struct PathRedaction {
    // The library roots, numbered in this order as <library 1>, <library 2>.
    QStringList libraryRoots;
    // The user's home folder, which is shown as ~.
    QString homePath;
    // Folders whose paths are kept as they are, such as Ferrous's own
    // config, data and cache folders: they tell nothing about the library.
    QStringList keptFolders;
};

// `text`, lines of the log, with paths and file names replaced by
// placeholders for sharing in a bug report: a path in a library root by
// "<library N>/…", keeping its extension, any other path by "<path>", and a
// quoted file name by "<file>". A path ends at the quote or bracket that
// opened before it, as the log's lines quote paths, or else at the end of
// the line, so that a path with spaces is never cut short.
QString withoutPaths(const QString &text, const PathRedaction &redaction);

} // namespace DiagnosticsLog
