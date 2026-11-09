// SPDX-License-Identifier: GPL-3.0-or-later

#include "QmlSmokeTest.h"

// Offscreen screens at 2x, 1.5x and 1.25x beside the default one, so that a
// HiDPI test can move a window to another scale as on a desktop. The first
// screen is the offscreen platform's default one, which the other tests use.
void addHiDpiTestScreens(const QTemporaryDir &storage) {
    if (qgetenv("QT_QPA_PLATFORM") != "offscreen"
        || qEnvironmentVariableIsSet("QT_SCREEN_SCALE_FACTORS")
        || qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        return;
    }
    QFile config(storage.filePath(QStringLiteral("screens.json")));
    if (!config.open(QIODevice::WriteOnly)) return;
    const auto screen = [](const QString &name, int x) {
        return QJsonObject{
            {QStringLiteral("name"), name},
            {QStringLiteral("x"), x},
            {QStringLiteral("y"), 0},
            {QStringLiteral("width"), x == 0 ? 800 : 8000},
            {QStringLiteral("height"), x == 0 ? 800 : 6000},
            {QStringLiteral("logicalDpi"), 96},
            {QStringLiteral("logicalBaseDpi"), 96},
            {QStringLiteral("dpr"), 1},
        };
    };
    const QJsonArray screens{
        screen(QString(), 0),
        screen(QStringLiteral("ferrous-hidpi-200"), 100'000),
        screen(QStringLiteral("ferrous-hidpi-150"), 200'000),
        screen(QStringLiteral("ferrous-hidpi-125"), 300'000),
    };
    config.write(QJsonDocument(QJsonObject{{QStringLiteral("screens"), screens}}).toJson());
    config.close();
    qputenv("QT_QPA_PLATFORM", "offscreen:configfile=" + QFile::encodeName(config.fileName()));
    qputenv("QT_SCREEN_SCALE_FACTORS",
        "ferrous-hidpi-200=2;ferrous-hidpi-150=1.5;ferrous-hidpi-125=1.25");
}

// Each test class's maker, in the class's own tst_qml_smoke_<name>.cpp.
std::unique_ptr<QmlSmokeTest> makeSpectrogramSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeSpectrogramPlaybackSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeSpectrogramCanvasSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeWaveformSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeWaveformSceneGraphSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeWaveformCoverageSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeLibraryQueueSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeTransportInputSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeAlbumArtSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeWindowSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makePreferencesSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeErrorsSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeTagEditorSmokeTest(const QString &userDirectoriesParent);
std::unique_ptr<QmlSmokeTest> makeScrobbleSmokeTest(const QString &userDirectoriesParent);

namespace {

struct SmokeTestArea {
    const char *name;
    std::unique_ptr<QmlSmokeTest> (*make)(const QString &userDirectoriesParent);
};

// Every test class, by the area name that the CTest entries use. An area
// may have several classes, which run in turn in its process.
const std::array<SmokeTestArea, 14> kSmokeTestAreas{{
    {"spectrogram", &makeSpectrogramSmokeTest},
    {"spectrogram", &makeSpectrogramPlaybackSmokeTest},
    {"spectrogram", &makeSpectrogramCanvasSmokeTest},
    {"waveform", &makeWaveformSmokeTest},
    {"waveform", &makeWaveformSceneGraphSmokeTest},
    {"waveform_coverage", &makeWaveformCoverageSmokeTest},
    {"library_queue", &makeLibraryQueueSmokeTest},
    {"transport_input", &makeTransportInputSmokeTest},
    {"transport_input", &makeAlbumArtSmokeTest},
    {"window", &makeWindowSmokeTest},
    {"preferences", &makePreferencesSmokeTest},
    {"errors", &makeErrorsSmokeTest},
    {"tag_editor", &makeTagEditorSmokeTest},
    {"scrobble", &makeScrobbleSmokeTest},
}};

// QTest's options that take a value.
const QStringList kQTestValueOptions{
    QStringLiteral("-o"), QStringLiteral("-maxwarnings"), QStringLiteral("-eventdelay"),
    QStringLiteral("-keydelay"), QStringLiteral("-mousedelay"), QStringLiteral("-repeat"),
    QStringLiteral("-iterations"), QStringLiteral("-median"), QStringLiteral("-minimumvalue"),
    QStringLiteral("-minimumtotal"), QStringLiteral("-perfcounter"), QStringLiteral("-seed"),
};

// Whether `test` has the test function `name`, which may have a data tag.
bool hasTestFunction(const QObject &test, const QString &name) {
    const QByteArray signature = name.section(QLatin1Char(':'), 0, 0).toLatin1() + "()";
    const int index = test.metaObject()->indexOfMethod(signature.constData());
    return index >= 0 && test.metaObject()->method(index).access() == QMetaMethod::Private;
}

// Runs the tests of one area, `-area <name>`, or of every area in turn.
// Test functions named without an area run in the test class that has them,
// so `ferrous_qml_smoke_tests waveformSceneGraphMatchesRaster` works as with
// one test class. `-check-areas <names>` checks that CTest runs every area.
int runSmokeTests(const QString &userDirectoriesParent, const QStringList &arguments) {
    QString onlyArea;
    QStringList checkedAreas;
    bool checkAreas = false;
    QStringList options;
    QStringList functions;
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == QStringLiteral("-area") && i + 1 < arguments.size()) {
            onlyArea = arguments.at(++i);
        } else if (argument == QStringLiteral("-check-areas")) {
            checkAreas = true;
            checkedAreas = arguments.mid(i + 1);
            break;
        } else if (kQTestValueOptions.contains(argument) && i + 1 < arguments.size()) {
            options << argument << arguments.at(++i);
        } else if (argument.startsWith(QLatin1Char('-'))) {
            options << argument;
        } else {
            functions << argument;
        }
    }

    QStringList areaNames;
    for (const SmokeTestArea &area : kSmokeTestAreas) {
        const QString name = QString::fromLatin1(area.name);
        if (!areaNames.contains(name)) {
            areaNames << name;
        }
    }
    if (checkAreas) {
        QStringList expected = areaNames;
        expected.sort();
        checkedAreas.sort();
        if (checkedAreas != expected) {
            qCritical("CTest runs the areas %s, but the smoke tests have %s",
                qPrintable(checkedAreas.join(QLatin1Char(' '))), qPrintable(expected.join(QLatin1Char(' '))));
            return 1;
        }
        return 0;
    }
    if (!onlyArea.isEmpty() && !areaNames.contains(onlyArea)) {
        qCritical("No smoke test area %s; the areas are %s", qPrintable(onlyArea),
            qPrintable(areaNames.join(QLatin1Char(' '))));
        return 1;
    }

    // QTest exits after listing one class's functions: list every class's.
    if (options.contains(QStringLiteral("-functions"))) {
        const QStringList fixtures{QStringLiteral("initTestCase"), QStringLiteral("init"),
            QStringLiteral("cleanup"), QStringLiteral("cleanupTestCase")};
        for (const SmokeTestArea &area : kSmokeTestAreas) {
            if (!onlyArea.isEmpty() && onlyArea != QLatin1String(area.name)) {
                continue;
            }
            const std::unique_ptr<QmlSmokeTest> test = area.make(userDirectoriesParent);
            const QMetaObject *meta = test->metaObject();
            for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
                const QMetaMethod method = meta->method(i);
                const QString name = QString::fromLatin1(method.name());
                if (method.methodType() == QMetaMethod::Slot && method.access() == QMetaMethod::Private
                    && method.parameterCount() == 0 && !fixtures.contains(name)
                    && !name.endsWith(QStringLiteral("_data"))) {
                    std::printf("%s()\n", qPrintable(name));
                }
            }
        }
        return 0;
    }

    int failures = 0;
    QStringList unknownFunctions = functions;
    for (const SmokeTestArea &area : kSmokeTestAreas) {
        if (!onlyArea.isEmpty() && onlyArea != QLatin1String(area.name)) {
            continue;
        }
        const std::unique_ptr<QmlSmokeTest> test = area.make(userDirectoriesParent);
        QStringList areaFunctions;
        for (const QString &function : std::as_const(functions)) {
            if (hasTestFunction(*test, function)) {
                areaFunctions << function;
                unknownFunctions.removeAll(function);
            }
        }
        if (!functions.isEmpty() && areaFunctions.isEmpty()) {
            continue;
        }
        failures += std::max(QTest::qExec(test.get(), QStringList{arguments.first()} + options + areaFunctions), 0);
    }
    if (!unknownFunctions.isEmpty()) {
        qCritical("Unknown test functions: %s", qPrintable(unknownFunctions.join(QLatin1Char(' '))));
        return 1;
    }
    return std::min(failures, 127);
}

} // namespace

int main(int argc, char **argv) {
    useOffscreenPlatformByDefault(argc, argv);
    disableGpuShaderCaches();
    // Bridge construction starts library workers; keep scans and tag operations
    // independent of saved application data and concurrent test processes.
    // Each test also gets user directories of its own; see init().
    QTemporaryDir testStorage;
    if (!testStorage.isValid()) return 1;
    qputenv("XDG_DATA_HOME", QFile::encodeName(testStorage.filePath("data")));
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(testStorage.filePath("config")));
    qputenv("XDG_CACHE_HOME", QFile::encodeName(testStorage.filePath("cache")));
    keepGStreamerRegistryIn(testStorage.path());
    qputenv("QT_NO_XDG_DESKTOP_PORTAL", "1");
    qputenv("KDE_KIRIGAMI_TABLET_MODE", "0");
    // Tests with a real bridge play into a sink that needs no audio device.
    qputenv("FERROUS_GST_OUTPUT_SINK", "fakesink");
    addHiDpiTestScreens(testStorage);
    // The bridges' file manager is the one guessed from $XDG_CURRENT_DESKTOP,
    // not the one that this desktop's xdg-mime names.
    BridgeClient::setFileBrowserNameDetector(nullptr);

    QApplication app(argc, argv);
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments << QString::fromLocal8Bit(argv[i]);
    }
    return runSmokeTests(testStorage.path(), arguments);
}
