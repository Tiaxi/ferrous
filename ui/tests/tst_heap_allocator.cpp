// SPDX-License-Identifier: GPL-3.0-or-later

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

namespace {

// Keeps the compiler from pairing up and dropping a malloc and its free.
void *volatile g_sink = nullptr;

#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 33)
#define FERROUS_HAVE_MALLINFO2 1

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
#if defined(FERROUS_HAVE_MALLINFO2)
    FerrousHeap::configureAllocator(nullptr);
    constexpr std::size_t large = 256 * 1024;
    QVERIFY(mmappedGrowthFor(large) >= large);
    // A free of a mapped block raises glibc's adaptive threshold to its size;
    // with the threshold fixed, the next large block is mapped all the same
    // and goes back to the system when it is freed.
    QVERIFY(mmappedGrowthFor(8 * 1024 * 1024) >= 8 * 1024 * 1024);
    QVERIFY(mmappedGrowthFor(large) >= large);
    const std::size_t before = mmappedBytes();
    g_sink = std::malloc(large);
    std::free(g_sink);
    g_sink = nullptr;
    QCOMPARE(mmappedBytes(), before);
    // Smaller blocks still come from the arenas.
    QCOMPARE(mmappedGrowthFor(FerrousHeap::kMmapThresholdBytes / 2), std::size_t{0});
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

QTEST_GUILESS_MAIN(HeapAllocatorTest)

#include "tst_heap_allocator.moc"
