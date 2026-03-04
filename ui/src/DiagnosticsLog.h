#pragma once

#include <QString>

namespace DiagnosticsLog {

QString defaultLogPath();
bool appendLine(const QString &logPath, const QString &line);

} // namespace DiagnosticsLog

