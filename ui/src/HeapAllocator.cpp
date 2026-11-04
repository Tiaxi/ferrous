// SPDX-License-Identifier: GPL-3.0-or-later

#include "HeapAllocator.h"

#include <cstring>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace FerrousHeap {

bool tunableIsSet(const char *glibcTunables, const char *name) {
    if (glibcTunables == nullptr || name == nullptr || *name == '\0') {
        return false;
    }
    const std::size_t nameLength = std::strlen(name);
    const char *entry = glibcTunables;
    for (;;) {
        if (std::strncmp(entry, name, nameLength) == 0 && entry[nameLength] == '=') {
            return true;
        }
        const char *next = std::strchr(entry, ':');
        if (next == nullptr) {
            return false;
        }
        entry = next + 1;
    }
}

AllocatorSetup configureAllocator(const char *glibcTunables) {
    AllocatorSetup setup;
#if defined(__GLIBC__)
    // glibc raises its mmap threshold to the size of each mmapped block that
    // is freed, up to 32 MiB, and its trim threshold with it. After the first
    // large free, canvases, PCM tiles, FFI frames and queue snapshots all came
    // from the arenas, and what they freed stayed there: a long session held
    // hundreds of MiB of free heap. Fixing both thresholds at glibc's defaults
    // turns that adjustment off, so blocks of 128 KiB or more are mapped on
    // their own and unmapped when freed, and free memory at the top of an
    // arena goes back once it exceeds 128 KiB.
    setup.supported = true;
    const auto threshold = static_cast<int>(kMmapThresholdBytes);
    if (!tunableIsSet(glibcTunables, "glibc.malloc.mmap_threshold")) {
        setup.mmapThresholdFixed = mallopt(M_MMAP_THRESHOLD, threshold) == 1;
    }
    if (!tunableIsSet(glibcTunables, "glibc.malloc.trim_threshold")) {
        setup.trimThresholdFixed = mallopt(M_TRIM_THRESHOLD, threshold) == 1;
    }
#else
    // Other C libraries have neither the adaptive threshold nor mallopt.
    static_cast<void>(glibcTunables);
#endif
    return setup;
}

bool canTrim() {
#if defined(__GLIBC__)
    return true;
#else
    return false;
#endif
}

void trimFreedHeap() {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

} // namespace FerrousHeap
