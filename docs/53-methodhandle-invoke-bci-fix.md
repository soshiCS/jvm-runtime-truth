# Gap A Fix: MethodHandle Adapter-Chain Invoke BCI Callsite Targets

**Date:** 2026-06-02  
**Predecessor doc:** `docs/52-runtime-dispatch-completeness-audit.md`  
**Status:** RESOLVED — all adapter-chained `methodhandle_invoke` BCIs now emit `callsite_target` records

---

## 1. Problem

`docs/52` identified **Gap A**: for MethodHandle invocations where the MH walk produces an `ADAPTER_GRAPH` shape (i.e., the MH has been adapted via `asType`, `insertArguments`, `dropArguments`, `bindTo`, `permuteArguments`, `filterArguments`, `filterReturnValue`, or `foldArguments`), the JVM emitted a `callsite_adapter_graph` record but **no** `callsite_target` record at the actual `invokevirtual` BCI.

This meant that a staticizer or UI tool had to traverse the adapter graph to find the final dispatch target, even when the invoke BCI and the target were both known. The UI could not answer "what does this BCI call?" with a direct lookup.

**Affected cases:**
- Case05_TypeAdapters: 5 of 5 adapter-chain invoke BCIs (43, 83, 127, 153, 177) lacked `callsite_target`
- Case06_ArgumentAdapters: all 5 adapter-chain invoke BCIs (66, 72, 99, 134, 227) lacked `callsite_target`
- Case07_FilterFoldCollect: all 3 composite invoke BCIs (77, 140, 222) lacked `callsite_target`

---

## 2. Root Cause

The fix location is `resolve_handle_call` in `src/hotspot/share/classfile/linkResolver.cpp`.

When the MH walk produces shape `DIRECT`, the existing code sets `tgt_ok = true` and falls through to the generic callsite path, which calls `soroush_graph_generic_callsite` to emit a `callsite_target` record. When the shape is `GWT`/`GWC`, `soroush_graph_target_set_callsite` emits a `callsite_target_set` record. Both paths set `multi_target_emitted = true`, which suppresses a second generic callsite emit.

For `ADAPTER_GRAPH` shape, `soroush_graph_adapter_graph_callsite` emits the adapter graph record and sets `multi_target_emitted = true`. The existing code had a special case for `reflection_method_invoke` to also emit a `callsite_target` from the first exact adapter graph node, so that reflection targets were visible in the reflection view without graph traversal. **No equivalent case existed for `methodhandle_invoke` or `methodhandle_invokeExact`.**

**Note on wrong file:** The JDK source tree has both `src/hotspot/share/classfile/linkResolver.cpp` and `src/hotspot/share/interpreter/linkResolver.cpp`. The build compiles only the `classfile/` version (confirmed by inspecting the build cmdline file). The fix was applied to the `classfile/` version. The `interpreter/` version also received the parallel change for consistency, but it is not compiled.

---

## 3. Fix

In the `ADAPTER_GRAPH` success branch of `resolve_handle_call`, immediately after the existing `reflection_method_invoke` special case, added:

```cpp
// For MH adapter-chained invocations, also emit a callsite_target so
// the invoke BCI has a direct source→target link without adapter graph
// traversal. Dedup is keyed on (src_class, src_method, src_desc,
// src_bci) — first exact node wins.
if (cat != nullptr &&
    (strcmp(cat, "methodhandle_invoke") == 0 ||
     strcmp(cat, "methodhandle_invokeExact") == 0)) {
  for (int ni = 0; ni < ne; ni++) {
    if (node_entries[ni].exact && node_entries[ni].klass != nullptr
        && node_entries[ni].method != nullptr) {
      soroush_graph_generic_callsite(cat,
          src_class, src_loader, src_method, src_desc,
          src_bci, src_opcode, src_cp,
          node_entries[ni].klass, node_entries[ni].loader_id,
          node_entries[ni].method, node_entries[ni].descriptor,
          src_ok, true, nullptr);
      break;
    }
  }
}
```

**Design notes:**
- The `soroush_graph_generic_callsite` dedup key is `(src_class, src_method, src_desc, src_bci)` — target is NOT in the key. Only one record per BCI is stored (first-in-wins). This is correct: the adapter graph already contains the full chain; the `callsite_target` record provides a direct primary-target link.
- The first node with `exact=true && klass != null && method != null` is the primary semantic target (role = `primary_target` or `adapted_target`). The break after the first match is intentional.
- For composite combinators (filterArguments with multiple filter functions), only the outermost target is stored here. All constituent methods are fully described in the `callsite_adapter_graph` record.
- The `evidence` field on the emitted record is `OBSERVED_ONLY` (set by the export function), consistent with all other runtime-observed targets.

---

## 4. Validation

### Before vs. After by case

| Case | Expected BCIs | Before | After |
|---|---|---|---|
| Case05_TypeAdapters | BCIs 43, 83, 127, 153, 177 | 0/5 adapter-chain BCIs had callsite_target | **5/5** ✓ |
| Case06_ArgumentAdapters | BCIs 66, 72, 99, 134, 227 | 0/5 | **5/5** ✓ |
| Case07_FilterFoldCollect | BCIs 77, 140, 222 | 0/3 | **3/3** ✓ |

### BCI → target mapping (run `6aedd221`)

**Case05_TypeAdapters.run():**
| BCI | Target | Source |
|---|---|---|
| 43 | Case05_TypeAdapters.doubleIt | adapter_graph (new) |
| 75 | Case05_TypeAdapters.boxedTriple | direct shape (pre-existing) |
| 83 | Case05_TypeAdapters.boxedTriple | adapter_graph (new) |
| 127 | Case05_TypeAdapters.widenOp | adapter_graph (new) |
| 153 | Case05_TypeAdapters.doubleIt | adapter_graph (new) |
| 177 | Case05_TypeAdapters.doubleIt | adapter_graph (new) |

**Case06_ArgumentAdapters.run():**
| BCI | Target | Source |
|---|---|---|
| 66 | Case06_ArgumentAdapters.add3 | adapter_graph (new) |
| 72 | Case06_ArgumentAdapters.add3 | adapter_graph (new) |
| 99 | Case06_ArgumentAdapters.add3 | adapter_graph (new) |
| 134 | String.substring | adapter_graph (new) |
| 227 | Case06_ArgumentAdapters.sub3 | adapter_graph (new) |

**Case07_FilterFoldCollect.run():**
| BCI | Target | Source |
|---|---|---|
| 77 | Integer.sum | adapter_graph (new) |
| 140 | Case07_FilterFoldCollect.join | adapter_graph (new) |
| 222 | Case07_FilterFoldCollect.triSum | adapter_graph (new) |

### Cross-check: all adapter-graph primary nodes match callsite_target

17 of 17 `methodhandle_invoke` adapter graph primary nodes have a matching `callsite_target` record at the same BCI. The one "MISMATCH" in the cross-check (`Case06@242`) is an `invokedynamic` BCI — outside Gap A scope, already covered by `callsite_target(invokedynamic)`.

### JSONL totals (run `6aedd221`, all 12 cases PASS)

| Record type | Count |
|---|---|
| callsite_target (all categories) | 146 |
| — methodhandle_invoke | 55 (was ~22 before fix) |
| callsite_adapter_graph | 96 |
| runtime_target | 965 |
| generated_class | 534 |
| hidden_class_identity | 530 |

### Demo output

```
PASS Case01 — Lambda / invokedynamic
PASS Case02 — String concat indy
PASS Case03 — Direct MethodHandle
PASS Case04 — MH receiver origins
PASS Case05 — asType adapters
PASS Case06 — Argument adapters
PASS Case07 — Filter / fold
PASS Case08 — Spread / collector
PASS Case09 — Guard / catch / finally
PASS Case10 — Reflection
PASS Case11 — Dynamic proxy
PASS Case12 — Hidden class
ManyCore cases demo complete — 12/12 passed
```

---

## 5. Impact on docs/52 Gap A

`docs/52` listed Gap A as BLOCKER:

> **Gap A: MethodHandle adapter-chain invoke BCIs missing `callsite_target`**  
> Status: BLOCKER — adapter-chain invocations (Cases 05, 06, 07) require adapter graph traversal to find the final target

Gap A is now **RESOLVED**. Every executed dynamic dispatch BCI has a direct `callsite_target` record. The UI and staticizer can now answer "source BCI → final target" with a single field lookup for all MethodHandle categories including adapter chains.

`docs/52` overall verdict changes from **READY (with one blocker to fix)** to **READY (no blockers)**.

---

## 6. Files Changed

| File | Change |
|---|---|
| `src/hotspot/share/classfile/linkResolver.cpp` | Added `methodhandle_invoke`/`invokeExact` branch after ADAPTER_GRAPH success to emit `callsite_target` |
| `src/hotspot/share/interpreter/linkResolver.cpp` | Parallel change for consistency (not compiled by fastdebug build) |
