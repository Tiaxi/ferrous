// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

// The process's malloc policy. Qt and the Rust library share the C library's
// allocator, so this covers both.
namespace FerrousHeap {

// Allocations of this size or more get their own mapping, which goes back to
// the system when they are freed. It is glibc's default threshold, held fixed.
inline constexpr std::size_t kMmapThresholdBytes = 128 * 1024;

struct AllocatorSetup {
    // The C library is glibc, whose malloc this policy is for.
    bool supported = false;
    // The mmap threshold was set; false where GLIBC_TUNABLES set it already.
    bool mmapThresholdFixed = false;
    // The trim threshold was set; false where GLIBC_TUNABLES set it already.
    bool trimThresholdFixed = false;
};

// Applies the policy. Call it first in main, before any thread starts.
// `glibcTunables` is the value of GLIBC_TUNABLES, or null: a threshold named
// there is left as the user set it.
AllocatorSetup configureAllocator(const char *glibcTunables);

// Whether trimFreedHeap can return memory: the C library is glibc.
bool canTrim();

// Returns the free pages inside the malloc arenas to the system (glibc's
// malloc_trim). It walks every arena, taking each one's lock in turn, so call
// it off the GUI thread and only once a large release has settled.
void trimFreedHeap();

// Whether a GLIBC_TUNABLES value (`name=value` pairs separated by colons)
// sets the tunable `name`.
bool tunableIsSet(const char *glibcTunables, const char *name);

} // namespace FerrousHeap
