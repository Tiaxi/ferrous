# Analysis: Global Search UI Hitching (Post-Model Refinement)

## Status (2026-03-05)
- Implemented: profiling/search diagnostics prints are now compile-time gated.
- Implemented: diagnostics text area no longer live-binds to `uiBridge.diagnosticsText` while closed.
- Result: repeated search keypress hitching caused by diagnostics text relayout is eliminated in normal builds.

## Summary
The recent model refinements (typed structs and surgical row operations) worked perfectly. As seen in the new logs, `modelApplyMs` has dropped from 46–82 ms down to **0–2 ms**. The UI apply path itself is now extremely fast and allocation-free.

However, a new and different source of hitching is causing the UI to freeze whenever `x` is repeatedly typed. This hitch is not related to search row computation, QML model applying, or garbage collection.

The root cause is the **Diagnostics Menu (`diagnosticsTextArea`) evaluating synchronous QML text layouts on every search log event.**

## Evidence from the Logs

Let's trace the lifecycle of a single query (e.g., `seq=3`) in the exact sequence it occurs:

```
[13:52:57.322] (UI Thread)     [search] send query seq=3 chars=3 text="xxx"
[13:52:57.385] (Worker Thread) [search-worker] seq=3 chars=3 tracks=24052 rows=1 elapsed_ms=6
[13:52:57.416] (UI Thread)     [search] apply frame seq=3 ... latencyMs=94 workerMs=0
```

Notice the huge gap between the UI thread logging "send query" (`.322`) and the worker thread finishing a 6 ms task (`.385`). 
1. The UI thread prepares the query and calls `logDiagnostic(...)` to print the "send query" line to `stderr` and internal memory.
2. The worker thread finishes taking only 6 ms, but according to the clock, it finishes **63 ms later**. This means the worker thread didn't even start processing the query for ~57 ms.
3. Why? Because the UI thread was completely blocked trying to execute the consequences of `logDiagnostic`.

### The `logDiagnostic` QML Trap
Every time `BridgeClient` adds a line to the diagnostics log, it does the following:
1. Appends the string to a `QStringList`.
2. Re-joins up to 2,000 strings into a massive single `QString` (`m_diagnosticsText`).
3. Emits `diagnosticsTextChanged`.

In `Main.qml`, there is a `TextArea` bound to this property, even if the Diagnostics dialog is closed:
```qml
TextArea {
    id: diagnosticsTextArea
    text: uiBridge.diagnosticsText || ""
    wrapMode: Text.Wrap
    // ...
}
```
When `diagnosticsTextChanged` is emitted, Qt Quick **synchronously** forces the internal text engine to recalculate the layout, word-wrapping, and geometry for all 2,000 lines of text. This happens on the main UI thread and takes **~50–60 ms**.

### The Double-Hitch Cycle
For every single search frame, `BridgeClient` logs *two* lines:
1. `send query` -> triggers ~50 ms UI freeze.
2. `apply frame` -> triggers another ~50 ms UI freeze.

When you hold down the `x` key, the OS generates key repeats every ~30 ms. But because the UI thread is frozen by the text layout engine for ~100+ ms per cycle, the key events pool up. 
1. The UI unfreezes, processes a burst of 'x' keys, updates the text, and starts the 90 ms debounce timer.
2. The 90 ms timer fires, triggering the next "send query" log.
3. The UI freezes for 50 ms.
4. The search worker processes it (fast).
5. The UI applies the model (fast) and logs "apply frame".
6. The UI freezes for 50 ms.
This results in exactly the ~300-400 ms periodic hitch pattern visible in the logs.

## The Solution

Do not alter the QML models or the search worker. The fix must address the diagnostics logging overhead:

1. **Decouple QML evaluation**
   Break the direct binding between `diagnosticsTextArea.text` and `uiBridge.diagnosticsText`. The UI should only query for the full text history when the user actually opens the Diagnostics dialog, or when clicking "Reload".
2. **Remove background logging updates**
   If the diagnostics window is not visible, do not append to `diagnosticsTextArea` and do not trigger a full string join string of all 2000 lines. 

By removing this synchronous log layout binding, the core UI (spectrogram, seekbar, search box) will stay perfectly responsive.
