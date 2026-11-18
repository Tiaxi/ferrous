// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QtTest/QtTest>

#include <cstdio>

// Running a test binary in shards, each in a process of its own, so that
// CTest runs them in parallel; check_test_shards.cmake checks that the
// shards together run every test once.

// The tests of one shard, with FERROUS_TEST_SHARD=<shard>/<shards>. CTest
// runs each shard in a process of its own, all in parallel
// (ui/CMakeLists.txt). A test is a row of a data-driven function, or a
// function without data: a few functions have many rows that take seconds
// each, and shards of whole functions came out unevenly. In the order of
// `-datatags`, the declaration order, a shard has every shards-th test from
// the shard-th. The rule needs no list kept up to date, and the rows of a
// function, alike in cost, go to every shard. Without the variable,
// `sharded` is false and every test runs.
struct TestShard {
    bool sharded = false;
    // The lines of `-datatags` for the shard's tests: "<class> <function>"
    // or "<class> <function> <tag>".
    QStringList listing;
    // The tests as QTest takes them on its command line: "<function>" or
    // "<function>:<tag>".
    QStringList tests;
    QString error;
};

inline TestShard testShardFromEnvironment() {
    TestShard shard;
    const QString value = qEnvironmentVariable("FERROUS_TEST_SHARD");
    if (value.isEmpty()) {
        return shard;
    }
    shard.sharded = true;
    const QStringList parts = value.split(QLatin1Char('/'));
    bool indexValid = false;
    bool countValid = false;
    const int index = parts.size() == 2 ? parts.at(0).toInt(&indexValid) : 0;
    const int count = parts.size() == 2 ? parts.at(1).toInt(&countValid) : 0;
    if (!indexValid || !countValid || index < 1 || index > count) {
        shard.error = QStringLiteral("FERROUS_TEST_SHARD is \"%1\", not <shard>/<shards> with 1 <= shard <= shards")
                          .arg(value);
        return shard;
    }
    // QTest lists the tests, rows and all, only on its standard output and
    // only to exit after: ask this binary, unsharded, in a process of its own.
    QProcess listing;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("FERROUS_TEST_SHARD"));
    listing.setProcessEnvironment(environment);
    listing.start(QCoreApplication::applicationFilePath(), {QStringLiteral("-datatags")});
    if (!listing.waitForFinished(30000) || listing.exitStatus() != QProcess::NormalExit
        || listing.exitCode() != 0) {
        shard.error = QStringLiteral("Could not list the tests with -datatags: %1 %2")
                          .arg(listing.errorString(), QString::fromLocal8Bit(listing.readAllStandardError()));
        return shard;
    }
    const QStringList lines = QString::fromLocal8Bit(listing.readAllStandardOutput()).split(QLatin1Char('\n'));
    int position = 0;
    for (const QString &line : lines) {
        const QStringList fields = line.split(QLatin1Char(' '));
        if (fields.size() < 2) {
            continue;
        }
        if (position++ % count != index - 1) {
            continue;
        }
        shard.listing << line;
        const QString tag = line.section(QLatin1Char(' '), 2);
        shard.tests << (tag.isEmpty() ? fields.at(1) : fields.at(1) + QLatin1Char(':') + tag);
    }
    if (shard.tests.isEmpty()) {
        shard.error = QStringLiteral("Shard %1 has no tests").arg(value);
    }
    return shard;
}

// QTest::qExec for the shard that FERROUS_TEST_SHARD names, or for every
// test without it.
inline int qExecShard(QObject *test, int argc, char **argv) {
    const TestShard shard = testShardFromEnvironment();
    if (!shard.error.isEmpty()) {
        // To standard error, not with qCritical(): before qExec(), Qt logs to
        // the journal where standard error is not a terminal, as under CTest.
        std::fprintf(stderr, "%s\n", qPrintable(shard.error));
        return 1;
    }
    if (!shard.sharded) {
        return QTest::qExec(test, argc, argv);
    }
    QStringList arguments;
    for (int i = 0; i < argc; ++i) {
        arguments << QString::fromLocal8Bit(argv[i]);
    }
    // QTest's -datatags lists every test: list the shard's, which
    // check_test_shards.cmake checks against it.
    if (arguments.contains(QStringLiteral("-datatags"))) {
        for (const QString &line : shard.listing) {
            std::printf("%s\n", qPrintable(line));
        }
        return 0;
    }
    return QTest::qExec(test, arguments + shard.tests);
}
