// SPDX-License-Identifier: GPL-3.0-or-later

//! Counts the allocations each test thread makes, and the bytes they ask
//! for, so a test can check that code on a hot path does not allocate per
//! item or build a large buffer.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

struct CountingAllocator;

thread_local! {
    static ALLOCATIONS: Cell<usize> = const { Cell::new(0) };
    static ALLOCATED_BYTES: Cell<usize> = const { Cell::new(0) };
}

fn count(bytes: usize) {
    // The counters have no destructor, but a thread that is being torn down
    // may no longer reach them; its allocations then go uncounted.
    let _ = ALLOCATIONS.try_with(|allocations| allocations.set(allocations.get() + 1));
    let _ = ALLOCATED_BYTES.try_with(|total| total.set(total.get().saturating_add(bytes)));
}

// SAFETY: Every call is forwarded to the system allocator with the caller's
// arguments, so its guarantees hold; counting touches only a thread-local
// cell that never allocates.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        count(layout.size());
        // SAFETY: The caller upholds `GlobalAlloc::alloc`'s contract.
        unsafe { System.alloc(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        count(layout.size());
        // SAFETY: The caller upholds `GlobalAlloc::alloc_zeroed`'s contract.
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        count(new_size);
        // SAFETY: The caller upholds `GlobalAlloc::realloc`'s contract, and
        // `ptr` came from this allocator, which is the system allocator.
        unsafe { System.realloc(ptr, layout, new_size) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: `ptr` came from this allocator, which is the system
        // allocator, with `layout`.
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;

/// How many allocations the current thread has made.
pub(crate) fn allocations() -> usize {
    ALLOCATIONS.with(Cell::get)
}

/// How many bytes the current thread's allocations have asked for in total,
/// counting the full new size of each reallocation.
pub(crate) fn allocated_bytes() -> usize {
    ALLOCATED_BYTES.with(Cell::get)
}
