#pragma once

#include <QString>
#include <QStringList>

namespace DiagnosticsLog {

QString defaultLogPath();
bool appendLine(const QString &logPath, const QString &line);
bool appendLines(const QString &logPath, const QStringList &lines);

} // namespace DiagnosticsLog
