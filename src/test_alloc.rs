// SPDX-License-Identifier: GPL-3.0-or-later

//! Counts the allocations each test thread makes, and the bytes they ask
//! for, so a test can check that code on a hot path does not allocate per
//! item or build a large buffer, and the bytes the thread holds, so a test
//! can check how much a structure takes.

use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;

struct CountingAllocator;

thread_local! {
    static ALLOCATIONS: Cell<usize> = const { Cell::new(0) };
    static ALLOCATED_BYTES: Cell<usize> = const { Cell::new(0) };
    static LIVE_ALLOCATIONS: Cell<isize> = const { Cell::new(0) };
    static LIVE_BYTES: Cell<isize> = const { Cell::new(0) };
}

fn count(bytes: usize) {
    // The counters have no destructor, but a thread that is being torn down
    // may no longer reach them; its allocations then go uncounted.
    let _ = ALLOCATIONS.try_with(|allocations| allocations.set(allocations.get() + 1));
    let _ = ALLOCATED_BYTES.try_with(|total| total.set(total.get().saturating_add(bytes)));
}

/// Adds `allocations` and `bytes` to what the current thread holds.
fn hold(allocations: isize, bytes: isize) {
    let _ = LIVE_ALLOCATIONS.try_with(|live| live.set(live.get().wrapping_add(allocations)));
    let _ = LIVE_BYTES.try_with(|live| live.set(live.get().wrapping_add(bytes)));
}

/// `bytes` as a change in what a thread holds.
fn signed(bytes: usize) -> isize {
    isize::try_from(bytes).unwrap_or(isize::MAX)
}

// SAFETY: Every call is forwarded to the system allocator with the caller's
// arguments, so its guarantees hold; counting touches only a thread-local
// cell that never allocates.
unsafe impl GlobalAlloc for CountingAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        count(layout.size());
        // SAFETY: The caller upholds `GlobalAlloc::alloc`'s contract.
        let ptr = unsafe { System.alloc(layout) };
        if !ptr.is_null() {
            hold(1, signed(layout.size()));
        }
        ptr
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        count(layout.size());
        // SAFETY: The caller upholds `GlobalAlloc::alloc_zeroed`'s contract.
        let ptr = unsafe { System.alloc_zeroed(layout) };
        if !ptr.is_null() {
            hold(1, signed(layout.size()));
        }
        ptr
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        count(new_size);
        // SAFETY: The caller upholds `GlobalAlloc::realloc`'s contract, and
        // `ptr` came from this allocator, which is the system allocator.
        let new = unsafe { System.realloc(ptr, layout, new_size) };
        if !new.is_null() {
            hold(0, signed(new_size).wrapping_sub(signed(layout.size())));
        }
        new
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // SAFETY: `ptr` came from this allocator, which is the system
        // allocator, with `layout`.
        unsafe { System.dealloc(ptr, layout) };
        hold(-1, signed(layout.size()).wrapping_neg());
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

/// How many allocations the current thread holds, less those it freed of
/// other threads': a difference over code that keeps to its thread is what
/// that code holds.
pub(crate) fn live_allocations() -> isize {
    LIVE_ALLOCATIONS.with(Cell::get)
}

/// How many bytes the current thread's allocations hold, counted as
/// [`live_allocations`] is.
pub(crate) fn live_bytes() -> isize {
    LIVE_BYTES.with(Cell::get)
}
