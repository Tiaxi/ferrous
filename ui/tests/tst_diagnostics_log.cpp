// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTemporaryDir>
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
    void pathsAreQuotedWithTheirQuotesEscaped();
    void writerKeepsTheOrderAndWritesWhatWaitsWhenDestroyed();
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
    QTest::newRow("a dotted sibling of a root")
        << QStringLiteral("[playback] info: Track started (user): \"/srv/fixture-music.old/Artist/Private Song.flac\".")
        << QStringLiteral("[playback] info: Track started (user): \"<path>.flac\".");
    QTest::newRow("a sibling of a root with a space")
        << QStringLiteral("[library] info: Scanning \"/home/fixture-user/Music (backup)/Private Song.flac\".")
        << QStringLiteral("[library] info: Scanning \"<path>.flac\".");
    QTest::newRow("an unquoted sibling of a root")
        << QStringLiteral("could not open /srv/fixture-music (backup)/Private Song.flac: denied")
        << QStringLiteral("could not open <path>");
    QTest::newRow("escaped quotes in a path")
        << QStringLiteral("[playback] info: Track started (user): "
                          "\"/srv/fixture-music/Album \\\"Deluxe\\\" Edition/01 Private Song.flac\".")
        << QStringLiteral("[playback] info: Track started (user): \"<library 1>/….flac\".");
    QTest::newRow("escaped backslashes in a path")
        << QStringLiteral("[tags] info: Renamed \"/srv/fixture-music/AC\\\\DC\\\\\" to \"/srv/fixture-music/b.flac\".")
        << QStringLiteral("[tags] info: Renamed \"<library 1>/…");
    QTest::newRow("a quoted root with an escaped quote")
        << QStringLiteral("Read \"/srv/fixture-music\\\"x/Private Song.flac\".")
        << QStringLiteral("Read \"<path>.flac\".");
    QTest::newRow("Rust's debug strings")
        << QStringLiteral("[ferrous] [system] debug: Command: Queue(Append([\"/srv/fixture-music/A \\\"B\\\"/1.flac\", "
                          "\"/media/stick/C\\\\D.mp3\"]))")
        << QStringLiteral("[ferrous] [system] debug: Command: Queue(Append([\"<library 1>/…");
    QTest::newRow("a Rust debug string alone")
        << QStringLiteral("[ferrous] [system] debug: Command: Library(AddTrack(\"/media/stick/C\\\\D \\\"E\\\".mp3\"))")
        << QStringLiteral("[ferrous] [system] debug: Command: Library(AddTrack(\"<path>.mp3\"))");
    QTest::newRow("an older line with unescaped quotes")
        << QStringLiteral("[playback] info: Track started (user): "
                          "\"/srv/fixture-music/Album \"Deluxe\" Edition/01 Private Song.flac\".")
        << QStringLiteral("[playback] info: Track started (user): \"<library 1>/…");
    QTest::newRow("an older line with an unescaped quote before a space")
        << QStringLiteral("[playback] info: Track started (user): \"/srv/fixture-music/A\" B/01 Private Song.flac\".")
        << QStringLiteral("[playback] info: Track started (user): \"<library 1>/…");
    QTest::newRow("a quoted file name with escaped quotes")
        << QStringLiteral("[tags] warning: \"01 \\\"Private\\\" Song.flac\" is read-only")
        << QStringLiteral("[tags] warning: \"<file>.flac\" is read-only");
    // A line with other quotes after a path's cannot tell an older path
    // with quotes in it from two paths, so all of it after the path goes.
    QTest::newRow("two quoted paths")
        << QStringLiteral("[tags] info: Renamed \"/srv/fixture-music/A/1.flac\" to \"/srv/fixture-music/A/01 B.flac\".")
        << QStringLiteral("[tags] info: Renamed \"<library 1>/…");
    QTest::newRow("an older line with quotes, the first before a space")
        << QStringLiteral("[playback] info: Track started (user): "
                          "\"/srv/fixture-music/01 \" Private Album \" Song.flac\".")
        << QStringLiteral("[playback] info: Track started (user): \"<library 1>/…");
    QTest::newRow("a path outside the library, then quotes")
        << QStringLiteral("Read \"/media/stick/01 \" Private Album \" Song.flac\".")
        << QStringLiteral("Read \"<path>");
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
    // Playback errors quote a file or path in curly quotes, which nothing
    // escapes in a name.
    QTest::newRow("a path in curly quotes")
        << QStringLiteral("[playback] error: Couldn't resume \u201c/home/user/Music/Example Artist/"
                          "\u00c4\u00e4ni \u2013 Song.flac\u201d where it was left: the file is missing.")
        << QStringLiteral("[playback] error: Couldn't resume \u201c<path>.flac\u201d where it was left: "
                          "the file is missing.");
    QTest::newRow("a library path in curly quotes")
        << QStringLiteral("[playback] error: Can't release \u201c/srv/fixture-music/Example Artist/Song.flac\u201d: "
                          "reading it has not finished.")
        << QStringLiteral("[playback] error: Can't release \u201c<library 1>/\u2026.flac\u201d: "
                          "reading it has not finished.");
    QTest::newRow("paths in curly quotes")
        << QStringLiteral("[playback] error: Couldn't release \u201c/home/user/Music/A/a.flac\u201d, "
                          "\u201c/home/user/Music/A/b.flac\u201d in time.")
        << QStringLiteral("[playback] error: Couldn't release \u201c<path>.flac\u201d in time.");
    QTest::newRow("a file name in curly quotes")
        << QStringLiteral("[playback] error: Couldn't play \u201c01 S\u00e4vel \u201cLive\u201d Song.flac\u201d "
                          "(Resource not found); skipped it.")
        << QStringLiteral("[playback] error: Couldn't play \u201c<file>.flac\u201d (Resource not found); "
                          "skipped it.");
    QTest::newRow("a file name in curly quotes, then other curly quotes")
        << QStringLiteral("Couldn't play \u201cSong.flac\u201d (in \u201cMusic\u201d)")
        << QStringLiteral("Couldn't play \u201c<file>.flac\u201d (in \u201cMusic\u201d)");
    QTest::newRow("an ac3 file name in curly quotes")
        << QStringLiteral("Couldn't play \u201cPrivate Song.ac3\u201d (Resource not found); skipped it.")
        << QStringLiteral("Couldn't play \u201c<file>.ac3\u201d (Resource not found); skipped it.");
    QTest::newRow("a dts file name in curly quotes")
        << QStringLiteral("Couldn't play \u201cPrivate Song.DTS\u201d (Resource not found); skipped it.")
        << QStringLiteral("Couldn't play \u201c<file>.DTS\u201d (Resource not found); skipped it.");
    QTest::newRow("words in curly quotes stay")
        << QStringLiteral("Scanning \u201cMusic\u201d: 3 added.")
        << QStringLiteral("Scanning \u201cMusic\u201d: 3 added.");
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
        QStringLiteral("[2026-11-10T12:00:04.000+02:00] [playback] error: Couldn't play "
                       "\u201c05 Fixture Song.flac\u201d (Resource not found); skipped it."),
        QStringLiteral("[2026-11-10T12:00:05.000+02:00] [playback] error: Couldn't resume "
                       "\u201c/home/fixture-user/Music/Fixture Artist/06 Fixture Song.flac\u201d where it was left: "
                       "the file is missing."),
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

void DiagnosticsLogTest::pathsAreQuotedWithTheirQuotesEscaped() {
    QCOMPARE(DiagnosticsLog::quotedPath(QStringLiteral("/srv/a \"b\"\\c.flac")),
             QStringLiteral("\"/srv/a \\\"b\\\"\\\\c.flac\""));
}

void DiagnosticsLogTest::writerKeepsTheOrderAndWritesWhatWaitsWhenDestroyed() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString logPath = dir.filePath(QStringLiteral("diagnostics.log"));
    const auto read = [&logPath]() {
        QFile file(logPath);
        return file.open(QIODevice::ReadOnly | QIODevice::Text)
            ? QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts)
            : QStringList();
    };
    {
        DiagnosticsLog::Writer writer;
        writer.post(logPath, {QStringLiteral("one"), QStringLiteral("two")});
        writer.post(logPath, {QStringLiteral("three")});
        writer.waitUntilWritten();
        QCOMPARE(read(), (QStringList{QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")}));
        // Removing the log comes after the lines before it, and before those
        // after it.
        writer.removeLog(logPath);
        writer.post(logPath, {QStringLiteral("after the removal")});
        writer.waitUntilWritten();
        QCOMPARE(read(), QStringList{QStringLiteral("after the removal")});
        for (int n = 0; n < 100; ++n) {
            writer.post(logPath, {QStringLiteral("waiting %1").arg(n)});
        }
    }
    const QStringList lines = read();
    QCOMPARE(lines.size(), 101);
    QCOMPARE(lines.last(), QStringLiteral("waiting 99"));
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
