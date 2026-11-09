// SPDX-License-Identifier: GPL-3.0-or-later

#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "HeapAllocator.h"
#include "HeapTrimmer.h"
#include "TestSessionIsolation.h"

// Sanitizers that replace malloc intercept mallopt (it returns 0) and serve
// allocations mallinfo2 cannot see, so the allocator policy cannot be checked
// under them. Each check is nested: an undefined function-like macro is a
// preprocessor error even after a false `defined` test in the same #if.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FERROUS_SANITIZED_ALLOCATOR 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer) \
    || __has_feature(thread_sanitizer)
#define FERROUS_SANITIZED_ALLOCATOR 1
#endif
#endif

#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 33)
#define FERROUS_HAVE_MALLINFO2 1
#endif
#endif

#if defined(FERROUS_HAVE_MALLINFO2) && !defined(FERROUS_SANITIZED_ALLOCATOR)
#define FERROUS_PROBE_ALLOCATOR 1
#endif

namespace {

#if defined(FERROUS_PROBE_ALLOCATOR)
// Set in the environment of a child copy of this binary that runs
// runAllocatorProbe instead of the tests: "configured" applies the policy
// first, "unconfigured" leaves glibc's defaults.
constexpr char kProbeModeVariable[] = "FERROUS_HEAP_ALLOCATOR_PROBE";

// The probe's exit codes.
enum ProbeResult : int {
    ProbeLargeBlocksMapped = 0,
    ProbeLargeBlockFromArena = 10,
    ProbeLargeBlockKept = 11,
    ProbeSmallBlockMapped = 12,
};

// Keeps the compiler from pairing up and dropping a malloc and its free.
void *volatile g_sink = nullptr;

std::size_t mmappedBytes() {
    return mallinfo2().hblkhd;
}

// Bytes in mmapped blocks while a block of `size` bytes is allocated, over
// those before it.
std::size_t mmappedGrowthFor(std::size_t size) {
    const std::size_t before = mmappedBytes();
    void *block = std::malloc(size);
    std::memset(block, 1, size);
    g_sink = block;
    const std::size_t during = mmappedBytes();
    std::free(block);
    g_sink = nullptr;
    return during > before ? during - before : 0;
}

// Runs in a fresh process, before anything else, with no malloc tunables:
// whether a large block is mapped after a larger mapped block was freed.
int runAllocatorProbe(bool configure) {
    if (configure) {
        FerrousHeap::configureAllocator(nullptr);
    }
    // Large enough that the arena's top, which a fresh process keeps small,
    // cannot serve it: a block that fits there is never mapped.
    constexpr std::size_t large = 1024 * 1024;
    // A free of a mapped block raises glibc's adaptive threshold to its size.
    constexpr std::size_t larger = 16 * 1024 * 1024;
    if (mmappedGrowthFor(larger) < larger || mmappedGrowthFor(large) < large) {
        return ProbeLargeBlockFromArena;
    }
    const std::size_t before = mmappedBytes();
    g_sink = std::malloc(large);
    std::free(g_sink);
    g_sink = nullptr;
    if (mmappedBytes() != before) {
        return ProbeLargeBlockKept;
    }
    if (mmappedGrowthFor(FerrousHeap::kMmapThresholdBytes / 2) != 0) {
        return ProbeSmallBlockMapped;
    }
    return ProbeLargeBlocksMapped;
}

// Runs the probe in a child copy of this binary without malloc tunables, so
// neither this process's heap nor its environment decides the outcome.
int runProbeChild(const char *mode, QByteArray *output) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QStringList keys = environment.keys();
    for (const QString &key : keys) {
        if (key == QLatin1String("GLIBC_TUNABLES") || key.startsWith(QLatin1String("MALLOC_"))) {
            environment.remove(key);
        }
    }
    environment.insert(QString::fromLatin1(kProbeModeVariable), QString::fromLatin1(mode));
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(), QStringList{});
    if (!child.waitForFinished(30000) || child.exitStatus() != QProcess::NormalExit) {
        child.kill();
        child.waitForFinished();
        *output = child.readAll();
        return -1;
    }
    *output = child.readAll();
    return child.exitCode();
}
#endif

constexpr int kTestSettleMs = 20;

} // namespace

class HeapAllocatorTest : public QObject {
    Q_OBJECT

private slots:
    void readsTunableNames();
    void reportsWhetherTheLibraryIsGlibc();
    void largeBlocksStayMappedAfterALargerFree();
    void leavesThresholdsTheUserTuned();
    void trimsOnceABurstOfRequestsSettles();
    void trimsOffTheGuiThread();
    void trimsWhenTheQueueLengthChanges();
    void trimsWhenTheWindowStopsBeingShown();
    void trimsAgainForARequestDuringATrim();
    void waitsForARunningTrimWhenDestroyed();
    void doesNothingWithoutATrimFunction();
    void trimsWithTheSystemAllocatorOnlyOnGlibc();
};

void HeapAllocatorTest::readsTunableNames() {
    using FerrousHeap::tunableIsSet;
    const char *name = "glibc.malloc.mmap_threshold";
    QVERIFY(!tunableIsSet(nullptr, name));
    QVERIFY(!tunableIsSet("", name));
    QVERIFY(!tunableIsSet("glibc.malloc.mmap_threshold=1", ""));
    QVERIFY(tunableIsSet("glibc.malloc.mmap_threshold=131072", name));
    QVERIFY(tunableIsSet("glibc.malloc.arena_max=4:glibc.malloc.mmap_threshold=1", name));
    QVERIFY(tunableIsSet("glibc.malloc.mmap_threshold=1:glibc.malloc.arena_max=4", name));
    // Another tunable whose name starts with this one, a name without a
    // value, and the name as another tunable's value do not count.
    QVERIFY(!tunableIsSet("glibc.malloc.mmap_threshold_max=1", name));
    QVERIFY(!tunableIsSet("glibc.malloc.mmap_threshold", name));
    QVERIFY(!tunableIsSet("glibc.malloc.arena_max=glibc.malloc.mmap_threshold=1", name));
    QVERIFY(!tunableIsSet("glibc.malloc.trim_threshold=131072", name));
}

void HeapAllocatorTest::reportsWhetherTheLibraryIsGlibc() {
    const FerrousHeap::AllocatorSetup setup = FerrousHeap::configureAllocator(nullptr);
#if defined(__GLIBC__)
    QVERIFY(setup.supported);
#if defined(FERROUS_SANITIZED_ALLOCATOR)
    QSKIP("the sanitizer's allocator intercepts mallopt");
#endif
    QVERIFY(setup.mmapThresholdFixed);
    QVERIFY(setup.trimThresholdFixed);
#else
    // Elsewhere the setup does nothing, and says so.
    QVERIFY(!setup.supported);
    QVERIFY(!setup.mmapThresholdFixed);
    QVERIFY(!setup.trimThresholdFixed);
#endif
}

void HeapAllocatorTest::largeBlocksStayMappedAfterALargerFree() {
#if defined(FERROUS_SANITIZED_ALLOCATOR)
    QSKIP("the sanitizer's allocator replaces glibc's, which mallinfo2 reports on");
#elif defined(FERROUS_PROBE_ALLOCATOR)
    QByteArray output;
    // With the policy, large blocks are mapped and go back when freed, and
    // small ones still come from the arenas.
    int result = runProbeChild("configured", &output);
    QVERIFY2(result == ProbeLargeBlocksMapped,
        qPrintable(QStringLiteral("probe exit %1: %2").arg(result).arg(QString::fromLocal8Bit(output))));
    // Without it, glibc's adaptive threshold serves the large block from an
    // arena, so the probe tells the two apart.
    result = runProbeChild("unconfigured", &output);
    QVERIFY2(result == ProbeLargeBlockFromArena,
        qPrintable(QStringLiteral("probe exit %1: %2").arg(result).arg(QString::fromLocal8Bit(output))));
#else
    QSKIP("needs glibc 2.33 or later for mallinfo2");
#endif
}

void HeapAllocatorTest::leavesThresholdsTheUserTuned() {
    const FerrousHeap::AllocatorSetup setup = FerrousHeap::configureAllocator(
        "glibc.malloc.arena_max=2:glibc.malloc.mmap_threshold=65536");
#if defined(__GLIBC__)
    QVERIFY(setup.supported);
    QVERIFY(!setup.mmapThresholdFixed);
#if defined(FERROUS_SANITIZED_ALLOCATOR)
    QSKIP("the sanitizer's allocator intercepts mallopt");
#endif
    QVERIFY(setup.trimThresholdFixed);
#else
    QVERIFY(!setup.supported);
#endif
}

void HeapAllocatorTest::trimsOnceABurstOfRequestsSettles() {
    std::atomic<int> trims{0};
    HeapTrimmer trimmer([&trims] { ++trims; }, kTestSettleMs);
    QSignalSpy trimmed(&trimmer, &HeapTrimmer::trimmed);
    trimmer.requestTrim();
    trimmer.requestTrim();
    QVERIFY(trimmer.pending());
    trimmer.requestTrim();
    QVERIFY(trimmed.wait());
    QCOMPARE(trims.load(), 1);
    QVERIFY(!trimmer.pending());
    // Nothing more follows without another request.
    QTest::qWait(kTestSettleMs * 5);
    QCOMPARE(trimmed.count(), 1);
}

void HeapAllocatorTest::trimsOffTheGuiThread() {
    std::atomic<QThread *> trimThread{nullptr};
    HeapTrimmer trimmer([&trimThread] { trimThread = QThread::currentThread(); }, kTestSettleMs);
    QSignalSpy trimmed(&trimmer, &HeapTrimmer::trimmed);
    trimmer.requestTrim();
    QVERIFY(trimmed.wait());
    QVERIFY(trimThread.load() != nullptr);
    QVERIFY(trimThread.load() != QThread::currentThread());
}

void HeapAllocatorTest::trimsWhenTheQueueLengthChanges() {
    std::atomic<int> trims{0};
    HeapTrimmer trimmer([&trims] { ++trims; }, kTestSettleMs);
    QSignalSpy trimmed(&trimmer, &HeapTrimmer::trimmed);
    // The first length is the queue the app started with.
    trimmer.setQueueLength(26000);
    QVERIFY(!trimmer.pending());
    trimmer.setQueueLength(26000);
    QVERIFY(!trimmer.pending());
    trimmer.setQueueLength(2);
    QVERIFY(trimmer.pending());
    QVERIFY(trimmed.wait());
    QCOMPARE(trims.load(), 1);
}

void HeapAllocatorTest::trimsWhenTheWindowStopsBeingShown() {
    std::atomic<int> trims{0};
    HeapTrimmer trimmer([&trims] { ++trims; }, kTestSettleMs);
    QSignalSpy trimmed(&trimmer, &HeapTrimmer::trimmed);
    trimmer.setWindowShown(false);
    trimmer.setWindowShown(true);
    QVERIFY(!trimmer.pending());
    trimmer.setWindowShown(false);
    QVERIFY(trimmer.pending());
    QVERIFY(trimmed.wait());
    trimmer.setWindowShown(false);
    QVERIFY(!trimmer.pending());
    QCOMPARE(trims.load(), 1);
}

void HeapAllocatorTest::trimsAgainForARequestDuringATrim() {
    QSemaphore entered;
    QSemaphore release;
    std::atomic<int> trims{0};
    HeapTrimmer trimmer(
        [&] {
            entered.release();
            release.acquire();
            ++trims;
        },
        kTestSettleMs);
    // A failed check must not leave the trimmer's destructor waiting.
    const auto unblock = qScopeGuard([&release] { release.release(2); });
    QSignalSpy trimmed(&trimmer, &HeapTrimmer::trimmed);
    trimmer.requestTrim();
    QTRY_COMPARE(entered.available(), 1);
    // Frees made during a trim may come after it has passed their arena.
    trimmer.requestTrim();
    QVERIFY(trimmer.pending());
    release.release(2);
    QTRY_COMPARE(trimmed.count(), 2);
    QCOMPARE(trims.load(), 2);
}

void HeapAllocatorTest::waitsForARunningTrimWhenDestroyed() {
    QSemaphore entered;
    std::atomic<bool> done{false};
    auto trimmer = std::make_unique<HeapTrimmer>(
        [&] {
            entered.release();
            QThread::msleep(50);
            done = true;
        },
        kTestSettleMs);
    trimmer->requestTrim();
    QTRY_COMPARE(entered.available(), 1);
    trimmer.reset();
    QVERIFY(done.load());
    // The trimmer's queued completion is dropped with it.
    QCoreApplication::processEvents();
}

void HeapAllocatorTest::doesNothingWithoutATrimFunction() {
    HeapTrimmer trimmer({}, kTestSettleMs);
    QVERIFY(!trimmer.enabled());
    trimmer.requestTrim();
    trimmer.setQueueLength(1);
    trimmer.setQueueLength(2);
    trimmer.setWindowShown(true);
    trimmer.setWindowShown(false);
    QVERIFY(!trimmer.pending());
}

void HeapAllocatorTest::trimsWithTheSystemAllocatorOnlyOnGlibc() {
    HeapTrimmer trimmer;
#if defined(__GLIBC__)
    QVERIFY(FerrousHeap::canTrim());
    QVERIFY(trimmer.enabled());
#else
    QVERIFY(!FerrousHeap::canTrim());
    QVERIFY(!trimmer.enabled());
#endif
    // Safe to call anywhere; it does nothing without glibc.
    FerrousHeap::trimFreedHeap();
}

int main(int argc, char *argv[]) {
#if defined(FERROUS_PROBE_ALLOCATOR)
    // The probe child: first thing, before Qt allocates.
    if (const char *mode = std::getenv(kProbeModeVariable)) {
        return runAllocatorProbe(std::strcmp(mode, "configured") == 0);
    }
#endif
    // No desktop session's platform theme; TestSessionIsolation.h.
    isolateFromDesktopSession();
    QCoreApplication app(argc, argv);
    HeapAllocatorTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "tst_heap_allocator.moc"
