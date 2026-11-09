// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include "DiagnosticsLog.h"
#include "TestSessionIsolation.h"

namespace {

// The roots and folders of a fixture library; none is a real one.
DiagnosticsLog::PathRedaction fixtureRedaction() {
    DiagnosticsLog::PathRedaction redaction;
    redaction.libraryRoots = {QStringLiteral("/srv/fixture-music/"), QStringLiteral("/home/fixture-user/Music")};
    redaction.homePath = QStringLiteral("/home/fixture-user");
    redaction.keptFolders = {QStringLiteral("/home/fixture-user/.config/ferrous"),
                             QStringLiteral("/home/fixture-user/.local/share/ferrous")};
    return redaction;
}

} // namespace

class DiagnosticsLogTest : public QObject {
    Q_OBJECT

private slots:
    void pathsBecomePlaceholders_data();
    void pathsBecomePlaceholders();
    void noLibraryPathOrFileNameIsLeft();
};

void DiagnosticsLogTest::pathsBecomePlaceholders_data() {
    QTest::addColumn<QString>("line");
    QTest::addColumn<QString>("expected");
    const QString prefix = QStringLiteral("[2026-11-10T12:00:00.000+02:00] [stderr] [ferrous] ");
    QTest::newRow("track start")
        << prefix + QStringLiteral("[playback] info: Track started (user): "
                                   "\"/srv/fixture-music/Fixture Artist/Fixture Album/01 Song.flac\".")
        << prefix + QStringLiteral("[playback] info: Track started (user): \"<library 1>/….flac\".");
    QTest::newRow("a root alone") << QStringLiteral("[library] info: Scanned \"/srv/fixture-music\" in 1.2 s: 3 added.")
                                  << QStringLiteral("[library] info: Scanned \"<library 1>\" in 1.2 s: 3 added.");
    QTest::newRow("a root under home")
        << QStringLiteral("[library] info: Scanning \"/home/fixture-user/Music\".")
        << QStringLiteral("[library] info: Scanning \"<library 2>\".");
    QTest::newRow("a longer name is another folder")
        << QStringLiteral("Read \"/srv/fixture-music-old/x.flac\".") << QStringLiteral("Read \"<path>.flac\".");
    QTest::newRow("two quoted paths")
        << QStringLiteral("[tags] info: Renamed \"/srv/fixture-music/A/1.flac\" to \"/srv/fixture-music/A/01 B.flac\".")
        << QStringLiteral("[tags] info: Renamed \"<library 1>/….flac\" to \"<library 1>/….flac\".");
    QTest::newRow("brackets in a path")
        << QStringLiteral("[playback] warning: Could not play it. (/srv/fixture-music/Live (2001)/Track (Edit).mp3)")
        << QStringLiteral("[playback] warning: Could not play it. (<library 1>/….mp3)");
    QTest::newRow("an apostrophe in a path")
        << QStringLiteral("[ferrous] [library] warning: failed to scan '/srv/fixture-music/Guns N' Roses': denied")
        << QStringLiteral("[ferrous] [library] warning: failed to scan '<library 1>/…': denied");
    QTest::newRow("unquoted, to the end of the line")
        << QStringLiteral("[stderr] could not open /srv/fixture-music/A B/c.flac: No such file")
        << QStringLiteral("[stderr] could not open <library 1>/…");
    QTest::newRow("a path outside the library")
        << QStringLiteral("[mpris] info: OpenUri \"/media/stick/Loose Song.opus\".")
        << QStringLiteral("[mpris] info: OpenUri \"<path>.opus\".");
    QTest::newRow("a file URI")
        << QStringLiteral("uri=file:///srv/fixture-music/A%20B/01%20Song.flac failed")
        << QStringLiteral("uri=<path>.flac failed");
    QTest::newRow("Ferrous's own folders are kept, with home as ~")
        << QStringLiteral("[system] info: Backend started: config \"/home/fixture-user/.config/ferrous\", "
                          "data \"/home/fixture-user/.local/share/ferrous\".")
        << QStringLiteral("[system] info: Backend started: config \"~/.config/ferrous\", "
                          "data \"~/.local/share/ferrous\".");
    QTest::newRow("other folders of home are not")
        << QStringLiteral("cover \"/home/fixture-user/Pictures/fixture.png\"")
        << QStringLiteral("cover \"<path>.png\"");
    QTest::newRow("system paths and URLs are kept")
        << QStringLiteral("plugin /usr/lib64/gstreamer-1.0/libgstflac.so, https://ws.audioscrobbler.com/2.0/ and 3/4")
        << QStringLiteral("plugin /usr/lib64/gstreamer-1.0/libgstflac.so, https://ws.audioscrobbler.com/2.0/ and 3/4");
    QTest::newRow("a quoted file name")
        << QStringLiteral("[tags] warning: \"01 Fixture Song.flac\" is read-only")
        << QStringLiteral("[tags] warning: \"<file>.flac\" is read-only");
    QTest::newRow("words in quotes stay")
        << QStringLiteral("[lastfm] info: Sending now playing: \"Artist\" - \"Title\".")
        << QStringLiteral("[lastfm] info: Sending now playing: \"Artist\" - \"Title\".");
}

void DiagnosticsLogTest::pathsBecomePlaceholders() {
    QFETCH(QString, line);
    QFETCH(QString, expected);
    QCOMPARE(DiagnosticsLog::withoutPaths(line, fixtureRedaction()), expected);
}

void DiagnosticsLogTest::noLibraryPathOrFileNameIsLeft() {
    const QStringList lines{
        QStringLiteral("[2026-11-10T12:00:00.000+02:00] [stderr] [ferrous] [playback] info: Track started "
                       "(gapless): \"/srv/fixture-music/Fixture Artist/Fixture Album/02 Fixture Song.flac\"."),
        QStringLiteral("[2026-11-10T12:00:01.000+02:00] [playback] error: Could not play the file. "
                       "(/home/fixture-user/Music/Fixture Artist/Fixture Album/03 Fixture Song.mp3)"),
        QStringLiteral("[2026-11-10T12:00:02.000+02:00] [stderr] [ferrous] [tags] info: Saved the tags of 2 files "
                       "(album): \"/srv/fixture-music/Fixture Artist/a.flac\", "
                       "\"/srv/fixture-music/Fixture Artist/b.flac\"."),
        QStringLiteral("[2026-11-10T12:00:03.000+02:00] [stderr] [analysis] spectrogram of "
                       "/srv/fixture-music/Fixture Artist/Fixture Album/04 Fixture Song.wv failed: bad frame"),
    };
    const QString text = lines.join(QLatin1Char('\n'));
    const QString copied = DiagnosticsLog::withoutPaths(text, fixtureRedaction());
    QCOMPARE(copied.split(QLatin1Char('\n')).size(), lines.size());
    for (const QString &leak : {QStringLiteral("fixture-music"), QStringLiteral("fixture-user"),
                                QStringLiteral("Fixture Artist"), QStringLiteral("Fixture Album"),
                                QStringLiteral("Fixture Song"), QStringLiteral("a.flac")}) {
        QVERIFY2(!copied.contains(leak), qPrintable(leak + QStringLiteral(" in:\n") + copied));
    }
    QVERIFY(copied.contains(QStringLiteral("[tags] info: Saved the tags of 2 files (album):")));
}

int main(int argc, char **argv) {
    // No desktop session's platform theme; TestSessionIsolation.h.
    isolateFromDesktopSession();
    QCoreApplication app(argc, argv);
    DiagnosticsLogTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_diagnostics_log.moc"
