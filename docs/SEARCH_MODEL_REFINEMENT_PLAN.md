# Plan: Eliminate Remaining Search Apply Hitches — Model & Storage Refinements

## Summary
Phases 1–4 of the original hitch plan are complete and working well. The worker thread pipeline (decode → materialize → queued signal) runs in 0–3 ms consistently. However, **sporadic UI-thread spikes** of 46–82 ms still occur in `modelApplyMs`, caused by QML delegate churn from full model resets and GC pressure from `QVariantMap`-heavy storage.

This plan addresses the two remaining root causes with targeted, low-risk changes.

## Evidence from Profiling Logs (2026-03-05)
With ~24k tracks indexed and the full worker pipeline active:

| Seq | Query | Rows | `modelApplyMs` | `queueDelayMs` | Notes |
|-----|-------|------|----------------|-----------------|-------|
| 3 | `"por"` | 171 | 0 | 3 | Fast — no spike |
| **4** | `"porcu"` | 169 | **82** | **86** | **Spike — same row count as seq 3** |
| 5 | `"porcup"` | 169 | 0 | 10 | Fast again |
| 6 | `"porcupine"` | 169 | 0 | 4 | Fast |
| **9** | `".3 in"` | 15 | **46** | **49** | **Spike — only 15 rows** |
| 10 | `".3 in a"` | 15 | 1 | 2 | Fast — same row count as seq 9 |

Key observations:
- Spikes are **not proportional to row count** (15 rows → 46 ms; 169 rows → 0 ms).
- `queueDelayMs` correlates with `modelApplyMs` — the UI thread was blocked when the worker posted its result, so the queued signal waited.
- All other timing stages (`ffiPopMs`, `decodeMs`, `materializeMs`, `workerMs`) are consistently 0–3 ms. The problem is isolated to `modelApplyMs` on the UI thread.

## Root Cause Analysis

### 1. Full model reset triggers delegate destruction storm
`GlobalSearchResultsModel::replaceRows()` calls `beginResetModel()`/`endResetModel()` whenever old and new row counts differ. A model reset destroys **all** QML delegates and recreates them from scratch. Even though the data is ready instantly, QML must:
- Destroy old delegate objects and their bindings
- Run the QML garbage collector on the freed objects
- Instantiate new delegates and evaluate all bindings
- Lay out the new items

This is O(visible_delegates) in the best case but can be much worse when GC decides to do a full sweep.

### 2. `QVariantMap` storage amplifies GC pressure
Each row is stored as a `QVariantMap` with ~18 key-value pairs. With 170 rows, that's ~3,000 individually heap-allocated `QString` keys + `QVariant` values. When the old rows are replaced, all of these become garbage simultaneously, increasing the chance of a GC pause coinciding with the model apply.

## Goals and Success Criteria
### User-visible goals
- Eliminate the remaining sporadic multi-frame hitches during search result updates.

### Performance targets (baseline library ~24k tracks)
- `modelApplyMs`: p95 < 5 ms, max < 15 ms (currently: p50 ≈ 0 ms, spikes to 82 ms).
- No regressions to search correctness, keyboard navigation, or context menu behavior.

## Non-Goals
- Changing the search worker pipeline (already optimal).
- Changing search ranking, result caps, or debounce behavior.
- Modifying the QML delegate visual design.

## Implementation Plan

### Phase A: Replace `QVariantMap` Storage with Typed Struct
#### Changes
In `GlobalSearchResultsModel`:
- Define a `SearchDisplayRow` struct with typed fields (`QString label`, `QString artist`, `int rowType`, etc.) instead of `QVariantMap`.
- Change `m_rows` from `QVector<QVariantMap>` to `QVector<SearchDisplayRow>`.
- `data()` constructs `QVariant` on-the-fly from the struct field matching the requested role, using `switch` on the role enum (same pattern as `roleKeyForRole()`, but returning the value directly).
- `replaceRows()` takes `QVector<SearchDisplayRow>` instead of `QVector<QVariantMap>`.
- `rowDataAt()` builds a `QVariantMap` on demand (only used for context menu / action lookups, not per-frame).

In `searchApplyWorkerLoop()` and `processSearchResultsFrame()`:
- Build `SearchDisplayRow` structs directly instead of `QVariantMap` items.
- Section/columns rows are also `SearchDisplayRow` with a `kind` field.

#### Rationale
Eliminates ~3,000 small heap allocations per frame (170 rows × 18 map entries). The struct is a single contiguous allocation per row. This dramatically reduces GC pressure and makes the cost of swapping old/new data predictable.

A `QVector<SearchDisplayRow>` swap is essentially a pointer swap + destructor walk over flat structs — far cheaper than destroying a map-of-variants per row.

#### Expected impact
- Reduces per-row memory overhead by ~60–70%.
- Makes GC-triggered spikes much less likely (fewer transient heap objects).
- Slight speedup in `data()` lookups (direct field access vs. hash lookup).

### Phase B: Surgical Model Updates Instead of Full Reset
#### Changes
In `GlobalSearchResultsModel::replaceRows()`:
- When the row count changes, compute the difference and use:
  - `beginRemoveRows()`/`endRemoveRows()` to trim excess rows, then
  - `dataChanged()` to update the overlapping range, then
  - `beginInsertRows()`/`endInsertRows()` to append new rows.
- When the row count is the same, use `dataChanged()` only (already implemented for the equal-count case).
- Keep the fast paths for empty→non-empty and non-empty→empty transitions as-is (they already use targeted insert/remove).

The update order (remove → change → insert) ensures indices stay valid throughout and avoids delegate destruction for rows that still exist.

#### Rationale
Full `beginResetModel()` destroys all QML delegates, including those that could have been reused. Surgical updates let QML **reuse** existing delegate instances: it simply rebinds their properties to the new data. This avoids the delegate destruction/creation storm and the associated GC spike.

This is the **single largest remaining optimization opportunity** — it directly addresses the root cause of the 46–82 ms spikes.

#### Expected impact
- Eliminates delegate destruction storms for row-count changes.
- Reduces worst-case `modelApplyMs` to the cost of property rebinding (~1–5 ms for ~170 rows).
- Paired with Phase A, makes the entire model apply path allocation-minimal.

### Phase C: Enable QML ListView Delegate Reuse (Optional, Qt 6)
#### Changes
- In the QML `ListView` that displays `globalSearchModel`, add `reuseItems: true`.
- Ensure delegates handle the `ListView.onPooled` / `ListView.onReused` signals if they hold any external state (e.g., timers, animations).

#### Rationale
Even with surgical model updates, QML's `ListView` may still create/destroy delegates when scrolling or when the visible range shifts. `reuseItems` keeps pooled delegates alive and rebinds them, which smooths out edge cases.

#### Decision
Implement only if Phase B alone doesn't bring `modelApplyMs` consistently under 5 ms. Low risk but requires testing delegate correctness with reuse.

## Implementation Order
1. **Phase A** first — it's a clean refactor with no behavioral change and makes Phase B simpler (struct comparison is cheaper than `QVariantMap` comparison).
2. **Phase B** second — this is the high-impact change that directly fixes the spike root cause.
3. **Phase C** if needed — measure after A+B before deciding.

## Test Plan
### Functional correctness
1. Search results identical (content, order, section headers) before vs. after for representative queries.
2. Tab reveal, Enter play, Queue, context menus unchanged.
3. Empty query clears results correctly.
4. Rapid typing with coalescing still works.
5. `rowDataAt()` returns correct data for context menu actions.

### Performance verification
1. Broad query (`"a"`) on ~24k tracks: `modelApplyMs` consistently < 5 ms.
2. Rapid typing burst (`por` → `porcupine` → clear → repeat): no spikes above 15 ms.
3. Log `queueDelayMs` — should drop proportionally with `modelApplyMs` improvements.
4. Run 50+ sequential searches and verify p95/max `modelApplyMs`.

## Risks and Mitigations
- **Risk**: Surgical update logic has off-by-one errors in row indices.
  - Mitigation: Unit test `replaceRows()` with various size transitions (0→N, N→0, N→M where N<M, N>M, N==M) and verify `dataChanged`/`rowsInserted`/`rowsRemoved` signal emission.
- **Risk**: Typed struct diverges from QML expectations if new fields are added later.
  - Mitigation: `rowDataAt()` builds `QVariantMap` from struct, so QML action code sees the same shape. Add a static_assert or compile-time check that role enum count matches struct field count.
- **Risk**: `reuseItems` causes stale visual state in complex delegates.
  - Mitigation: Phase C is optional and gated on measurement. Test thoroughly if enabled.
