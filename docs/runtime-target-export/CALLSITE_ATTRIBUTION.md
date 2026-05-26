# Callsite Attribution

## Overview

When `resolve_handle_call` fires for a MethodHandle callsite, the system must
answer two questions before it can emit any record:

1. **Which user callsite is this?** — class, method, BCI, opcode
2. **What is the runtime MH receiver?** — the actual MethodHandle oop

These are answered differently depending on which frame is on top of the
interpreter stack. The three cases are:

```
Case A  — top frame is the user frame (holder ∉ java/lang/invoke/)
Case A2 — top frame is a java/lang/invoke/ LF dispatch frame
Case B  — top frame is a jdk/internal/reflect/ accessor frame
```

---

## The three-case framework

### Case A — user frame on top (is_lf_dispatch_case = false)

**When:** The user calls `mh.invokeExact(...)` or `mh.invoke(...)` directly.
The top interpreter frame is the user method. This is the common case.

**Source attribution:** Read directly from the top frame.

```
top frame: UserClass.main  bci=35  opcode=invokehandle
```

**BCI offset:** `interpreter_frame_bci()` returns the BCI of the
*next* instruction (post-advance), which is `actual_bci + 6` because `invokehandle`
is 6 bytes internally. The symbolic backward scanner (`sg_analyze_mh_receiver`)
corrects for this in its analysis.

**MH receiver:** Two steps:
1. **Step 1 (symbolic):** `sg_analyze_mh_receiver()` walks the bytecode backward
   from `actual_bci` to find which local variable or expression stack slot holds
   the MH at invoke time (`SG_RECV_LOCAL` → local slot N).
2. **Step 2 (runtime fallback):** If symbolic analysis fails, read the actual
   MH oop from the expression stack via `sg_recover_mh_recv_from_java_sp(thread, arg_slots)`.

See [RUNTIME_RECEIVER_RECOVERY.md](RUNTIME_RECEIVER_RECOVERY.md) for deep detail
on the recovery mechanism.

**Code location:** `linkResolver.cpp:3648–3673` (Case A assignment block).

---

### Case A2 — LambdaForm dispatch frame on top (is_lf_dispatch_case = true)

**When:** The MH being invoked is itself an adapter (BMH), so the invocation
dispatches through a `java/lang/invoke/` LambdaForm before reaching the user
site. The top interpreter frame is an LF internal frame, not the user frame.

**Example:** `MethodHandles.insertArguments(mhAdd, 0, 100).invokeExact(5)`
— the top frame is `BoundMethodHandle$Species_LI.invoke` or similar.

**Source attribution (vframeStream walk):** Walk the vframeStream past ALL
`java/lang/invoke/` frames (skipping compiled/deopt/stub frames which raw frame
walking cannot handle) until the first frame whose class holder does NOT have
`java/lang/invoke/` as a prefix. That is the user frame.

```cpp
vframeStream vfst(JavaThread::cast(THREAD), true);
while (!vfst.at_end()) {
  InstanceKlass* h = vfst.method()->method_holder();
  if (strncmp(h->name()->as_C_string(), "java/lang/invoke/", 17) != 0) {
    // found user frame
    break;
  }
  vfst.next();
}
```

**Code location:** `linkResolver.cpp:3560–3645` (Case A2 block, sets
`is_lf_dispatch_case = true` at line 3506).

**MH receiver (Step 2a):** The LF dispatch frame's `local[0]` is always the
MethodHandle being dispatched — the `this` of the `invoke`/`invokeExact` call.
Read it with `top.interpreter_frame_local_at(0)`:

```cpp
intptr_t* local0 = top.interpreter_frame_local_at(0);
oop candidate = *(oop*)local0;
// Verify: must be a MethodHandle instance
if (sg_oop_valid(candidate) &&
    java_lang_invoke_MethodHandle::is_instance(candidate)) {
    mh_recv = candidate;
}
```

**Code location:** `linkResolver.cpp:3747–3760` (Step 2a block).

---

### Case B — reflection accessor frame (is_reflection_accessor = true)

**When:** The callsite is `Method.invoke(...)` or `Constructor.newInstance(...)`.
The top interpreter frame is the reflection accessor:
- `jdk/internal/reflect/DirectMethodHandleAccessor`
- `jdk/internal/reflect/DirectConstructorHandleAccessor`

**Source attribution:** Walk vframeStream past the accessor frame to find the user
frame calling `method.invoke()`.

**Target extraction:** The accessor object holds a `target` field (the internal
MethodHandle wrapping the actual method). Key complication (JDK21): the target is
**not** a raw `DirectMethodHandle`. `MethodHandleAccessorFactory.makeSpecializedTarget()`
always wraps the core DMH in `dropArguments` + `asType` adapter chains (a BMH).
Only the trivial 0-param or already-adapted case returns the DMH directly.

**Previous broken approach:** Checked `java_lang_invoke_DirectMethodHandle::is_instance(target)`.
This only matched the trivial case; for any method with ≥1 parameter, a static method,
or anything that required type adaptation, the check failed and the system fell back
to a `reflection_target_adapter_mh_deferred` diagnostic.

**Correct approach:** Use `sg_walk_mh(target, 6)` to traverse the full adapter
chain:

```cpp
SgMhWalkResult walk = sg_walk_mh(target_mh, 6);
if (walk.shape == SgMhWalkResult::DIRECT && walk.n_targets > 0 && walk.targets[0].valid) {
    // Fast path: target IS already a DMH (trivial case)
    tgt_class  = walk.targets[0].klass;
    ...
} else if (walk.shape == SgMhWalkResult::ADAPTER_GRAPH) {
    // Adapter path: find first valid DMH target in the graph
    for (int ni = 0; ni < walk.n_graph_nodes && !tgt_ok; ni++) {
        const SgAdapterNode& gn = walk.graph_nodes[ni];
        if (gn.has_target && gn.target.valid) {
            tgt_class  = gn.target.klass;
            ...
        }
    }
}
```

**Code location:** `linkResolver.cpp:3520–3558` (Case B block).

---

## `is_lf_dispatch_case` flag — critical distinction

```cpp
// linkResolver.cpp:3506
bool is_lf_dispatch_case = false;
```

This flag is set to `true` **only** in the Case A2 branch. Its purpose is to
choose the correct Step 2 fallback:

```cpp
// Step 2: runtime fallback for Case A (user frame is top)
if (mh_recv == nullptr && !is_lf_dispatch_case) {
    oop candidate = sg_recover_mh_recv_from_java_sp(thread, arg_slots);
    ...
}

// Step 2a: Case A2 — read from LF dispatch frame local[0]
if (mh_recv == nullptr && is_lf_dispatch_case) {
    intptr_t* local0 = top.interpreter_frame_local_at(0);
    ...
}
```

**Why the flag is necessary:** Before this flag existed, the code set
`recv_frame_valid = top_frame_valid = true` for both Case A and Case A2, which
used `!recv_frame_valid` as the Step 2 guard. For Case A (where `recv_frame_valid`
was true), the guard was never satisfied, so `sg_recover_mh_recv_from_java_sp`
never fired. This meant every Case A site that failed symbolic analysis emitted a
`diagnostic` instead of recovering the actual runtime MH. `insertArguments` was
the primary victim of this bug.

---

## After MH receiver is obtained: dispatch to record type

Once `mh_recv` is non-null, call `sg_walk_mh(mh_recv, depth_limit)`:

```
shape == DIRECT          → callsite_target record
shape == GWT             → callsite_target_set record (role: test/true/false)
shape == GWC             → callsite_target_set record (role: try/handler)
shape == ADAPTER_GRAPH   → callsite_adapter_graph record via sg_walk_generic_bmh
shape == BMH_UNKNOWN     → diagnostic (unknown BMH lf_kind)
shape == MH_UNKNOWN      → diagnostic (not a known MH type)
mh_recv == nullptr       → diagnostic (receiver not recovered)
```

**Call sites:**
- `soroush_graph_generic_callsite()` called from `linkResolver.cpp:3932` (DIRECT/MH_UNKNOWN path)
- `soroush_graph_target_set_callsite()` called from `linkResolver.cpp:3785` (GWT/GWC path)
- `soroush_graph_adapter_graph_callsite()` called from `linkResolver.cpp:3820` (ADAPTER_GRAPH path)

---

## Sibling BCI scan

**Problem:** A single CP-cache entry can be referenced by multiple `invoke*`
bytecodes in the same method (e.g., if the same MH is invoked at two different
call sites that both use the same constant pool index after rewriting). Only the
first resolution fires `resolve_handle_call`; subsequent ones hit the already-
resolved CP-cache entry and are silently skipped.

**Solution:** After emitting the primary record for BCI X, `sg_emit_sibling_bcis`
scans all other BCIs in the same method for `invoke*` bytecodes that share the
same CP constant pool index. For each sibling BCI it runs `sg_analyze_mh_receiver`
and emits a `diagnostic` record explaining the relationship.

**Code location:** `linkResolver.cpp:2634–2710` (`sg_emit_sibling_bcis`).

**Reason codes emitted:**
- `sibling_bci_same_local` — sibling uses the same local variable as the primary BCI
- `sibling_bci_distinct_receiver` — different local (separate MH at this BCI)
- `sibling_bci_unanalyzable` — analysis failed for the sibling
- `sibling_bci_arg_slots_unknown` — arg_slots not available at scan time
