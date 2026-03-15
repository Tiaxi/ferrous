#pragma once

#include <QtGlobal>

class SpectrogramSeekTrace {
public:
    static bool runtimeEnabled();
    static void noteSeekIssued(double targetSeconds);
    static quint64 currentGeneration();
    static qint64 startedAtMs();
    static double targetSeconds();
    static bool isActive(qint64 nowMs);
};
