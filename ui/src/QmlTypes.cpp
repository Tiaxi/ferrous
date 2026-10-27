// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlTypes.h"

#include <QString>
#include <qqml.h>

#include "LevelMeterItem.h"
#include "Localization.h"
#include "PlaybackClock.h"
#include "SpectrogramItem.h"
#include "WaveformEditorItem.h"
#include "WaveformItem.h"

void registerFerrousQmlTypes() {
    FerrousLocalization::registerQmlTypes();
    qmlRegisterType<SpectrogramItem>("FerrousUi", 1, 0, "SpectrogramItem");
    qmlRegisterType<WaveformItem>("FerrousUi", 1, 0, "WaveformItem");
    qmlRegisterType<WaveformEditorItem>("FerrousUi", 1, 0, "WaveformEditorItem");
    qmlRegisterType<LevelMeterItem>("FerrousUi", 1, 0, "LevelMeterItem");
    qmlRegisterUncreatableType<PlaybackClock>(
        "FerrousUi", 1, 0, "PlaybackClock", QStringLiteral("The bridge owns the playback clock"));
}
