# Known Limitations

This document separates **implemented reality** from **deferred work**. Every
limitation listed here is intentional, documented, and accounted for in the
exact-or-diagnostic invariant (unresolvable sites produce explicit diagnostics,
never silent omissions).

---

## Runtime mode: -Xint required

**Status: intentional constraint, not fixable without significant work**

The entire callsite attribution system requires the template interpreter. In
compiled (JIT) code:
- `resolve_handle_call` does not fire (compiled code inlines or specializes the call)
- Frame locals are not accessible in the same format
- The expression stack layout at invocation time differs

**Consequence:** Any class running in compiled mode will NOT have its MH callsites
captured. Classes touched by JIT before the first interpreted execution of a
callsite are silently not captured.

**Mitigation in use:** All validation and production runs use `-Xint`. For
Manycore staticization use cases, the slice is run interpreter-only.

**Deferred:** Compiled-frame recovery would require JIT-specific frame inspection
(deoptimization-based or via `vframeStream` with JVMTI). This is a separate large
engineering task.

---

## asType secondary unboxing node — exact=false

**Status: intentional, C++ accessor unavailable**

When `mh.asType(newType)` boxes or unboxes a primitive, the adapter chain contains
a secondary `BoundMethodHandle$Species_L` holding the boxing/unboxing adapter.
This secondary node is itself a BMH, and its inner contents are not readable from
C++ without unsafe generated-class field accessors.

**Output:** The `callsite_adapter_graph` record has `all_exact=false`. The primary
target (the user's method) has `exact=true`. The secondary node has `exact=false`
with `exact_false_reason="slot_is_bmh"`.

**Impact:** The user's target method is correctly identified. The boxing helper
identity is not extracted — only its species class name is reported.

---

## tryFinally cleanup slots — all exact=false

**Status: intentional, same root cause as asType**

`MethodHandles.tryFinally(target, cleanup)` creates a `TRY_FINALLY` BMH where
both the target and cleanup slots can themselves be BMH adapters (not raw DMHs).
The current single-pass walk cannot extract their inner contents.

**Output:** `callsite_adapter_graph` with `adapter_kind="try_finally"`, `all_exact=false`,
all 4 nodes have `exact=false`.

---

## asCollector secondary component — exact=false

**Status: intentional**

`asCollector` binds an internal array-collector trampoline in its secondary slot.
This trampoline is a JVM-internal BMH with no corresponding Java DMH.

---

## 4 JDK-internal diagnostics

**Status: acceptable, zero user impact**

The following 4 callsites inside the JDK itself produce `recv_from_method_result_or_field`
diagnostics because their MH receivers are loaded from method-return values:

| Class | Method | BCI | Reason |
|-------|--------|-----|--------|
| `jdk/internal/util/ByteArray` | `getUnsignedShort` | 5 | MH from array element access |
| `jdk/internal/util/ByteArray` | `getInt` | 5 | Same |
| `java/lang/module/ModuleDescriptor$Builder` | `provides` | 67 | MH from field of returned object |
| `jdk/internal/reflect/MethodHandleAccessorFactory` | `makeSpecializedTarget` | 86 | MH from method result during factory initialization |

These are JDK-internal classes that are not user code and are not target
candidates for staticization. The diagnostics are expected and documented.

---

## Virtual and interface dispatch deferred

**Status: deferred, separate feature**

`invokevirtual` and `invokeinterface` on user-defined interfaces are not captured.
The callsite attribution system only captures `invokehandle` (MethodHandle dispatch)
and `invokedynamic`. Regular polymorphic dispatch (virtual method calls) is not
within the scope of this branch.

---

## MH receiver from field — recv_from_method_result_or_field

**Status: known limitation of symbolic analysis**

When the symbolic backward analysis (`sg_analyze_mh_receiver`) traces the MH
receiver backward and finds it came from a method return value (rather than a
local variable or field load), it cannot recover the actual MH at analysis time.

This applies to patterns like:
```java
getMH().invokeExact(args...);
cache.getOrCompute(key).invokeExact(args...);
```

**What happens:** The Step 2 runtime fallback (`sg_recover_mh_recv_from_java_sp`)
fires for Case A. If that recovers the oop, the site gets an exact record. If the
expression stack state at the time of resolution doesn't yield a valid MH oop
(e.g., the MH was already consumed or the stack layout differs), a diagnostic is
emitted.

For Case A2 (LF dispatch top frame), Step 2a reads LF local[0] and usually succeeds
unless the LF frame is itself calling another MH dynamically from a method result.

---

## First-call timing for reflection accessors

**Status: known edge case**

`MethodHandleAccessorFactory.makeSpecializedTarget` runs lazily on the first
reflection invocation. If `resolve_handle_call` fires during the factory's own
initialization (before the accessor's `target` field is populated), Case B will
find `target_mh == null` and emit a diagnostic. The prefer-exact upgrade mechanism
handles this: when the second invocation fires (with the target field populated),
the diagnostic is upgraded to an exact `callsite_target` record.

---

## BoundMethodHandle inner-layer extraction

**Status: intentional depth limit**

`sg_walk_mh` has a depth limit of 6. Deeply nested adapter chains (e.g.,
`asType(filterArguments(asType(mh1,...),asType(mh2,...)),...)`) are walked up to
6 levels deep. Beyond that, inner nodes are classified `exact=false` with
`exact_false_reason="depth_limit"`.

In practice, no user-constructed adapter chain has been observed to exceed 4
levels. The depth limit is a safety bound, not an expected constraint.

---

## `invokedynamic` target mutations

**Status: intentional**

`MutableCallSite` and `VolatileCallSite` allow the bootstrap method to install a
target that changes after bootstrap. Only the initial target (at bootstrap time)
is captured. Dynamic mutations are not tracked.

---

## CGLIB / ByteBuddy class name detection

**Status: heuristic, may misclassify**

The `generated_by` field for ByteBuddy and CGLIB classes is determined by
heuristic pattern matching on class names. A non-standard naming convention
will result in `generated_by="unknown"` even if the bytes are still captured.

---

## In-memory graph cap

**Status: known constraint**

The provenance graph caps at 1 million nodes and 2 million edges. Broad-prefix
Spring Boot runs (all `org/springframework` classes) approach but don't exceed
this in practice. For the callsite tables specifically, the caps are:
- `g_gen_buckets` — 512 hash buckets, chained
- `g_ts_buckets` — 512 hash buckets, chained
- `g_ag_buckets` — 256 hash buckets, chained

Records dropped beyond the cap are silently omitted (fail-safe). The export
summary counts only records successfully written, not dropped ones.

---

## Not in scope for this branch

The following are implemented in the broader fork but are NOT part of the
runtime target export branch:

- Full async / cross-thread causality (SCHEDULES, CONTINUES_ON edges)
- MethodHandle / LambdaForm execution tracing (LambdaFormExecution nodes)
- Bytecode rewriter ENTER/EXIT for arbitrary class prefixes (that's a separate
  observability feature, not staticization support)
- Graph database persistence
- Failure reconstruction UX
