# Runtime Capture Architecture

This document describes how the JVM instrumentation captures callsite information at runtime.

See [03-source-ownership-map.md](03-source-ownership-map.md) for the files involved.  
See [02-phase-history.md](02-phase-history.md) for why specific design decisions were made.

---

## Overview

The system intercepts dispatch at two points in the JVM's method invocation pipeline:

```
Java bytecode execution
        │
        │  invokehandle bytecode
        ▼
TemplateTable::invokehandle         ← WARM PATH (fires every dispatch)
        │
        │  first execution of a callsite
        ▼
LinkResolver::resolve_handle_call   ← COLD PATH (fires once per CP cache entry)
        │
        │  invokeinterface bytecode (first resolution)
        ▼
LinkResolver::runtime_resolve_interface_method  ← COLD PATH
        │
        │  class loading
        ▼
KlassFactory (klassFactory.cpp)     ← hidden class identity hook
        │
        │  JVM shutdown
        ▼
soroush_graph_export_runtime_targets()  ← JSONL export
```

---

## Cold-Path Capture: `resolve_handle_call`

### When it fires
`resolve_handle_call` is called from `LinkResolver::resolve_invokehandle`, which is invoked during the first execution of each `invokehandle` bytecode. HotSpot maps one constant-pool-cache (CPC) entry per CP index. Once the CPC entry is resolved, subsequent executions of the same bytecode (same CP index) do NOT call `resolve_handle_call`. They go directly through the already-resolved entry.

This means:
- If two bytecodes share the same CP index (sibling BCIs), only the first fires `resolve_handle_call`.
- The warm-path hook (see below) was added specifically to handle this.

### What it captures

`resolve_handle_call` receives:
- The `CallInfo& result` containing the resolved `MethodHandle` appendix and the resolved target `Method*`.
- The current Java thread.

The MH appendix is the live MethodHandle object attached to the CPC entry. It is walked by `sg_walk_mh` to decompose the adapter chain.

### Dispatch case handling

The function immediately classifies the call site into one of three cases:

```
resolve_handle_call fires
        │
        ├── top frame = DirectMethodHandleAccessor → Case B (reflection)
        │         Walk vframeStream past accessor
        │         Source: user frame above accessor
        │         Receiver: accessor.this.target → sg_walk_mh(depth=6)
        │
        ├── top frame = java/lang/invoke/* LF/BMH → Case A2 (LF dispatch)
        │         Walk vframeStream to user frame
        │         Source: recovered user frame
        │         Receiver: LF local[0] (the MH being dispatched)
        │
        └── top frame = user frame → Case A (normal)
                  Source: top frame
                  Receiver: thread->last_Java_sp() + arg_slots
```

**Case A (normal user MH call):**
```java
MethodHandle mh = lookup.findVirtual(MyClass.class, "foo", mt);
mh.invoke(obj);  // invokehandle at this BCI
```
The user frame is at the top of the interpreter stack. Source = top frame. Receiver = top of stack minus argument slots.

**Case A2 (LambdaForm dispatch chain):**
```java
// Internal: LambdaMetafactory creates a BMH; when the MH chain dispatches,
// the top frame is a java/lang/invoke/LambdaForm$DMH or BMH internal.
```
The top frame is JDK-internal (`java/lang/invoke/`). Walk `vframeStream` until a non-invoke frame is found. That frame is the user frame. Read the MH from `LF local[0]` (the first argument to the LambdaForm body, which is the MH being invoked).

**Case B (JDK 21 reflection via DirectMethodHandleAccessor):**
```java
Method m = MyClass.class.getMethod("foo");
m.invoke(obj);  // routes through DirectMethodHandleAccessor.invokeImpl
```
The accessor is a JDK-internal class. Walk vframeStream past it to the user frame. The appendix MH is inside the accessor's `target` field (a BMH adapter wrapping the actual method). Use `sg_walk_mh(target, depth=6)` to traverse the BMH chain.

### MH Walk: `sg_walk_mh`

```
sg_walk_mh(oop mh_oop, int depth)
        │
        ├── DirectMethodHandle → extract member, emit primary_target node
        │
        ├── BoundMethodHandle$Species_* → sg_walk_generic_bmh
        │         │
        │         ├── extract lform (LambdaForm) + vmentry (target MH)
        │         ├── extract bound args (Species fields L0, L1, ...)
        │         ├── classify each bound arg (user class / internal JDK / bound data)
        │         └── recurse: sg_walk_mh(vmentry, depth-1)
        │
        ├── delegating MH types (AsType, FilterArgs, etc.) → extract delegate, recurse
        │
        └── fallback → emit with classification=unknown
```

Walk depth is controlled by `sg_mh_walk_depth()`, default 10. The depth limit prevents infinite loops on pathological MH chains.

`SgMhWalkResult` accumulates:
- `target_class`, `target_method`, `target_descriptor` — the final concrete method
- `nodes[]` — all adapter nodes encountered
- `exact` — whether the target is exact (single concrete method) or polymorphic

---

## Warm-Path Capture: `TemplateTable::invokehandle`

### When it fires
Every execution of an `invokehandle` bytecode, regardless of whether the CPC entry was already resolved.

### How it works

The AArch64 `TemplateTable::invokehandle` assembly stub calls `prepare_invoke`, which:
1. Resolves the CPC entry (first time) or reads it from cache (subsequent times).
2. Loads the appendix MethodHandle into `r2`.
3. Loads the resolved target `Method*` into `rmethod`.
4. Loads the invoke return-entry address into `lr`.

After the null-check of `r2`, the hook:
1. Reads `g_sg_enabled` via `lea + ldrb`.
2. If zero, jumps to `L_sg_skip` (3 instructions overhead — negligible).
3. Saves `lr` to native stack (`stp lr, zr, [sp, -16]!`).
4. Calls `InterpreterRuntime::sg_trace_mh_dispatch(r2)` via `call_VM`.
5. Restores `lr` from native stack (`ldp lr, zr, [sp], 16`).

`sg_trace_mh_dispatch` (JRT_ENTRY) delegates to `sg_trace_mh_impl`:
1. Guard: `Universe::is_fully_initialized()` — skip during JVM bootstrap.
2. Guard: `soroush_graph_enabled()` — skip if export off.
3. Guard: `recv_oop != nullptr`.
4. Walk `vframeStream` from top to first non-JDK frame.
5. Verify bytecode at that frame's BCI is `invokehandle` (0xba).
6. Read source class, method, descriptor, BCI, CP index, loader.
7. Call `sg_walk_mh(recv_oop, depth)` to decompose the live MH receiver.
8. Call `soroush_graph_generic_callsite(...)` to record the callsite target.

### Why `lr` must be saved

```
prepare_invoke:
  ldr lr, [table + offset]   ← lr = invoke return-entry address (table lookup)

call_VM:
  blr rscratch1              ← call_VM_leaf_base issues a BLR instruction
                               BLR = Branch with Link Register
                               This overwrites lr with PC+4 (return addr within call_VM)

After call_VM returns:
  lr = stale (points inside call_VM stub, not the invoke return-entry)

jump_from_interpreted:
  br target_method_entry      ← branches to compiled entry of dispatched method

Compiled method returns:
  ret                         ← returns via lr
                               lr is wrong → jumps to garbage → NPE or crash
```

The `stp lr, zr, [sp, -16]!` / `ldp lr, zr, [sp], 16` pair saves `lr` on the native stack across the `call_VM`.

Note: `rmethod` (r12) is automatically saved/restored by `call_VM_leaf_base` via:
```cpp
stp(rscratch1, rmethod, Address(pre(sp, -2 * wordSize)));
// ... call ...
ldp(rscratch1, rmethod, Address(post(sp,  2 * wordSize)));
```
Do not add explicit `rmethod` save/restore; it would produce a double-save with unbalanced pops.

---

## invokeinterface Capture (Phase 2D)

Phase 2D uses two complementary hooks (same architecture as invokevirtual Phase 2C): a cold-path hook (once per CP cache entry) and a warm-path hook (every interpreted itable dispatch).

### Cold-path hook

**When it fires**: The first time each `invokeinterface` CP cache entry is resolved. `runtime_resolve_interface_method` is called during the first execution of an unresolved `invokeinterface` bytecode. After the CP cache entry is filled, subsequent dispatches skip this path.

**Hook location**: `LinkResolver::runtime_resolve_interface_method` (in `linkResolver.cpp`).

**Logic**:
```
runtime_resolve_interface_method fires
        │
        ├── recv is null → skip
        ├── selected_method is null → skip
        ├── soroush_graph_enabled() = false → skip
        │
        └── walk vframeStream from top:
                  for each frame:
                    if holder starts with java/, jdk/, sun/, com/sun/ → skip
                    else → this is the user frame; stop
                  
                  verify user frame's bytecode at BCI = 0xb9 (invokeinterface)
                  read source_class, source_method, source_descriptor, source_bci
                  read cp_index, loader_id
                  call soroush_graph_poly_callsite("invokeinterface", ...)
```

### Warm-path hook

**When it fires**: Every interpreted normal itable dispatch (path 3 of `TemplateTable::invokeinterface`), including all executions at the same BCI with different receiver types. This is what enables true polymorphic capture when the cold path fires only once.

**Hook location**: `TemplateTable::invokeinterface` (`templateTable_aarch64.cpp`), normal itable path. Inserted after `__ profile_arguments_type(r3, rmethod, r13, true)`, before `__ jump_from_interpreted(rmethod, r3)`. **Not** inserted in the forced-virtual path (Object methods) or the vfinal/private path.

**Assembly pattern** (identical to the invokevirtual warm-path hook):
```cpp
{
  Label L_sg_ii_skip;
  __ lea(rscratch1, ExternalAddress((address)soroush_graph_enabled_addr()));
  __ ldrb(rscratch1, Address(rscratch1));
  __ cbz(rscratch1, L_sg_ii_skip);
  __ stp(lr, zr, __ pre(sp, -2 * wordSize));   // save lr (set by prepare_invoke)
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address, InterpreterRuntime::soroush_trace_ii_dispatch),
             rmethod);                           // arg1 = concrete Method* (from itable)
  __ ldp(lr, zr, __ post(sp, 2 * wordSize));   // restore lr
  __ bind(L_sg_ii_skip);
}
```

**C++ side**: `InterpreterRuntime::soroush_trace_ii_dispatch` (JRT_ENTRY in `interpreterRuntime.cpp`; declared in `interpreterRuntime.hpp`). Walks vframeStream, guards `op == _invokeinterface`. Emits via `soroush_graph_poly_callsite("invokeinterface", ...)`.

**Register state at hook point**: `rmethod` = concrete Method* after itable lookup (from `lookup_interface_method`). `rlocals = r24` (callee-saved, safe). `lr` set by `prepare_invoke` to the invoke return-entry address — must be saved/restored around `call_VM`. `rmethod` is automatically preserved by `call_VM_leaf_base` (saved in `stp`/`ldp` across call).

**Result**: Case15 BCI 54 → three `callsite_target` records (Dog.sound, Cat.sound, Bird.sound), all `source_opcode=invokeinterface`, `evidence=OBSERVED_ONLY`. Graph node: `static_label=blocked_multi_target` (SR_MULTI_TARGET). `heuristic_edges_created=0`.

---

## invokevirtual Capture (Phase 2C)

Phase 2C uses two complementary hooks: a cold-path hook (once per CP cache entry) and a warm-path hook (every interpreted dispatch).

### Cold-path hook

**When it fires**: The first time each `invokevirtual` CP cache entry is resolved. `runtime_resolve_virtual_method` is called during the first execution of an unresolved `invokevirtual` bytecode. After the CP cache entry is filled, subsequent dispatches skip this path.

**Hook location**: `LinkResolver::runtime_resolve_virtual_method` (in `linkResolver.cpp`), after the existing `soroush_trace_runtime_dispatch(...)` call.

### Warm-path hook

**When it fires**: Every interpreted non-final vtable dispatch, including all subsequent executions at the same BCI with different receiver types. This is what enables true polymorphic capture.

**Hook location**: `TemplateTable::invokevirtual_helper` (`templateTable_aarch64.cpp`), non-final path. Inserted after `__ lookup_virtual_method(r0, index, method)` and `__ profile_arguments_type(...)`, before `__ jump_from_interpreted(method, r3)`.

**Assembly pattern** (identical to the invokehandle warm-path hook):
```asm
lea  rscratch1, [soroush_graph_enabled_addr]
ldrb rscratch1, [rscratch1]
cbz  rscratch1, skip
stp  lr, xzr, [sp, #-16]!      // save lr (prepare_invoke sets lr = return entry)
call InterpreterRuntime::soroush_trace_iv_dispatch(thread, rmethod)
ldp  lr, xzr, [sp], #16        // restore lr
skip:
```

`rmethod` (= concrete Method* from vtable lookup) is passed as `c_rarg1`. `call_VM_leaf_base` automatically saves/restores `rmethod` (r12) via `stp`/`ldp`.

**C++ side**: `InterpreterRuntime::soroush_trace_iv_dispatch` (JRT_ENTRY in `interpreterRuntime.cpp`). Walks vframeStream, guards `op == _invokevirtual` to skip invokeinterface forced-virtual cases (those call `invokevirtual_helper` too). Emits via `soroush_graph_poly_callsite("invokevirtual", ...)`.

### Emission function
`soroush_graph_poly_callsite("invokevirtual", ...)` — dedup key `(src_class, src_loader_id, src_method, src_desc, src_bci, target_class, target_loader_id, target_method, target_desc)`. Multiple records per BCI when different receiver types or different classloaders observed. Stored in `g_poly_buckets`. Exported at Phase 3.1. Evidence field: `OBSERVED_ONLY`. (Gap #17 fix, 2026-05-30: both loader IDs included in hash and equality check.)

### Polymorphic result (Case14 example)
```
Case14_InvokevirtualPoly.run() BCI 54: invokevirtual Animal.sound()
  → callsite_target: Dog.sound   evidence=OBSERVED_ONLY
  → callsite_target: Cat.sound   evidence=OBSERVED_ONLY
  → callsite_target: Bird.sound  evidence=OBSERVED_ONLY

graph node: callsite::...::run::()V::54  static_label=blocked_multi_target (SR_MULTI_TARGET)
heuristic_edges_created=0
```

---

## Reflection Attribution (Phase 2E)

Phase 2E adds warm-path attribution for `Method.invoke` calls, resolving Gap #15: the missing edge between the call site that calls `Method.invoke` and the actual reflected target method.

### Why cold-path alone is insufficient

`MemberName.resolve` (the cold-path hook in `methodHandles.cpp`) fires once per reflected method — the first time any caller resolves a given `MemberName`. If startup code resolves `HelloController.index` first, subsequent HTTP-path `Method.invoke` calls find the `MemberName` already resolved and fire no hook. The missing edge is absent under both mixed-mode and `-Xint`.

### Warm-path hook: decoding the Method receiver

**When it fires**: Every time `soroush_trace_iv_dispatch` is called and `concrete_method == java/lang/reflect/Method.invoke`. This happens on every interpreted non-final `invokevirtual` dispatch to `Method.invoke`.

**Receiver oop**: `recv_oop` (= r2, the `java.lang.reflect.Method` object). Passed from the template table as arg2 to `call_VM`. This register holds the receiver of the `invokevirtual` call, which is the Method object being invoked.

**Decoding**:
```cpp
oop clazz_mirror = java_lang_reflect_Method::clazz(recv_oop);   // declaring class
int mslot        = java_lang_reflect_Method::slot(recv_oop);     // method idnum
Klass* dk        = java_lang_Class::as_Klass(clazz_mirror);
InstanceKlass* dik = InstanceKlass::cast(dk);
Method* rmet     = dik->method_with_idnum(mslot);                // actual Method*
```

**Emission**: `soroush_graph_poly_callsite("reflection_method_invoke", src..., tgt_class, tgt_loader, rmet->name, rmet->signature)`.

**Dedup**: Same `g_poly_buckets` used for invokevirtual/invokeinterface. Dedup key includes both loader IDs. Multiple reflected targets at the same BCI produce SR_MULTI_TARGET.

**Source files**:
- `src/hotspot/share/interpreter/interpreterRuntime.cpp` — Phase 2E block in `soroush_trace_iv_dispatch`
- `src/hotspot/share/interpreter/interpreterRuntime.hpp` — `recv_oop` parameter (3rd arg)
- `src/hotspot/cpu/aarch64/templateTable_aarch64.cpp` — `recv` passed as arg2 in `call_VM`

### Validation (Spring Boot HTTP, 2026-05-30)

| Run | `doInvoke bci=55 → HelloController.index` |
|-----|:---:|
| startup-only (no HTTP) | 0 — correct, no HTTP dispatch |
| mixed+HTTP (5 requests) | **1** — Gap #15 resolved |
| -Xint+HTTP (5 requests) | **1** — consistent, no JIT dependency |

---

## Receiver Discovery

For a live `invokehandle` dispatch (warm path), the receiver MH oop is passed directly from `r2` (set by `prepare_invoke`).

For the cold path, receiver discovery is more complex because the MH is the appendix object attached to the CPC entry, not a live stack value.

**Step 1: Read from static analysis of the frame.**
The receiver analyzer reads local variable slots of the user frame, looking for a valid OOP at the expected position. This can fail if:
- The receiver flowed from a method return value or field load (not a local slot)
- The stack frame layout is unusual (adapter construction frames, SP underflow)

**Step 2: Runtime stack fallback (Case A).**
Read `thread->last_Java_sp() + arg_slots * wordSize`. This gives the top-of-stack value at the time `resolve_handle_call` was entered — the MH argument. Works for simple user frames. Does NOT work for Case A2 (LF dispatch top frame; `last_Java_sp` reflects the LF frame, not the user frame below it).

**Step 2a: Case A2 fallback.**
Read `top.interpreter_frame_local_at(0)` — the first local of the innermost `java/lang/invoke/` LambdaForm frame. In LF dispatch, local[0] is the MethodHandle being invoked. This is the live value.

If all fallbacks fail, a `diagnostic` record is emitted with the appropriate reason code.

---

## Adapter Graph Generation

After `sg_walk_mh` returns, the `SgMhWalkResult` contains:
- `nodes[]`: a list of `SgAdapterNode` entries
- Each node has: `role`, `classification`, `class_name`, `method_name`, `descriptor`, `exact`, `exact_false_reason`

Node roles:
| Role | Meaning |
|---|---|
| `primary_target` | The final concrete dispatch target |
| `secondary_component` | An intermediate adapter (type conversion, argument binding, etc.) |
| `bound_data` | A non-MH bound argument (int, String, etc.) |
| `test_predicate` | The guard predicate in a GWT |
| `true_target` / `false_target` | The two branches of a GWT |

Node classifications:
| Classification | Meaning |
|---|---|
| `user_class` | User code (not JDK-internal) |
| `internal_jdk` | JDK runtime class (`java/lang/invoke/`, etc.) |
| `bound_data` | Non-class bound value |
| `unknown` | Classification failed |

`sg_compute_node_semantic` assigns semantic labels (`string_concat`, `guard_with_test`, `type_conversion`, etc.) based on adapter class name and LambdaForm kind.

---

## Export Pipeline

### Trigger
The export function `soroush_graph_export_runtime_targets()` is registered as a JVM shutdown hook during `soroush_graph_enabled()` initialization. It fires when the JVM exits normally (code 0 or any non-abort exit).

### Output path
Set via `SOROUSH_EXPORT_RUNTIME_TARGETS` env var. If unset, no file is written. If the file cannot be opened, a stderr warning is printed and the JVM continues to exit normally.

### Side tables

| Table | Contents | Dedup key |
|---|---|---|
| `g_indy_sites` | invokedynamic callsite_target records | trace_id (1-based, assigned at bytecode rewrite) |
| `g_gen_buckets` | invokehandle callsite_target and diagnostic records | (src_class, src_method, src_descriptor, src_bci) — first-in-wins |
| `g_poly_buckets` | invokevirtual + invokeinterface callsite_target records (Phase 2C/2D) | (src_class, src_method, src_descriptor, src_bci, target_class, target_method, target_desc) — one per (callsite, target) pair |
| `g_ts_buckets` | callsite_target_set records (GWT / GWC MH sites) | (src_class, src_method, src_descriptor, src_bci) — first-in-wins |
| `g_ag_buckets` | callsite_adapter_graph records | (src_class, src_method, src_descriptor, src_bci) — first-in-wins |
| `g_sg_nodes` | runtime_target records (MH linkage, reflection) | target identity + source frame fields |
| `g_hidden_ids` | hidden_class_identity records | (runtime_name) — linear scan |
| Class metadata | bytecode_artifact records | (class_name, loader_id) from loaded Klass list |

Each table has its own mutex. Export reads all tables at shutdown, lock-free after a snapshot.

### Record emission order (export phases)

| Phase | Source | Record type |
|---|---|---|
| 1 | `g_indy_sites` | `callsite_target` (invokedynamic) |
| 2 | `g_sg_nodes` | `execution`, `runtime_target` |
| 3 | `g_gen_buckets` (exact=true) | `callsite_target` |
| 3 | `g_gen_buckets` (exact=false, not dedup-suppressed) | `diagnostic` |
| 3.1 | `g_poly_buckets` | `callsite_target` (one per unique (BCI, target) pair) |
| 3.5 | `g_ts_buckets` | `callsite_target_set` |
| 4 | `g_ag_buckets` | `callsite_adapter_graph` |
| 5 | `g_hidden_ids` | `hidden_class_identity` |
| 6 | Loaded Klass registry | `bytecode_artifact` |
| Final | — | `export_summary` (counts, `complete: true`) |

**Part C diagnostic suppression** (within Phase 3): before emitting diagnostics from `g_gen_buckets`, the exporter builds a dedup set of all source BCIs that have exact records in `g_gen_buckets`, `g_ag_buckets`, `g_ts_buckets`, and `g_poly_buckets`. Any diagnostic whose BCI appears in that set is suppressed.

---

## Example: Full End-to-End Trace

```java
// User code:
MethodHandle mh = MethodHandles.lookup().findVirtual(
    HelloController.class, "index", MethodType.methodType(String.class));
String result = (String) mh.invokeWithArguments(controller);
//                         ^ invokehandle bytecode at this BCI
```

**Execution flow:**

1. JVM executes `invokehandle` at BCI X in `Application.lambda$validationRunner$3`.
2. **Warm-path hook fires** (every dispatch):
   - `r2` = live `MethodHandle` receiver (the `mh` object)
   - `g_sg_enabled` check: 1, proceed
   - Save `lr`, call `sg_trace_mh_dispatch(r2)`
   - Restore `lr`
   - In `sg_trace_mh_impl`:
     - Walk vframeStream → user frame = `lambda$validationRunner$3` at BCI X
     - Call `sg_walk_mh(mh_oop, 10)` → result: target = `HelloController.index`
     - Call `soroush_graph_generic_callsite("methodhandle_invokeExact", "com/example/springboot/Application", "lambda$validationRunner$3", ..., BCI_X, "HelloController", "index", ...)`
     - Side table insert: dedup key = (class, method, desc, bci) — first call, insert succeeds
3. **Cold-path also fires** (first dispatch only):
   - `resolve_handle_call` is entered
   - Case A: top frame = user frame (same)
   - `sg_walk_mh(appendix_mh, 10)` → same target
   - `soroush_graph_generic_callsite(...)` → dedup key already present, no duplicate record
4. **At JVM shutdown:**
   - Export pipeline writes `callsite_target` record to JSONL:
     ```json
     {"record":"callsite_target","category":"methodhandle_invokeExact",
      "source_class":"com/example/springboot/Application",
      "source_method":"lambda$validationRunner$3","source_bci":111,
      "source_capture":"exact","target_class":"com/example/springboot/HelloController",
      "target_method":"index","target_descriptor":"()Ljava/lang/String;"}
     ```

**Note on dedup ordering**: If the warm-path hook and cold-path both try to insert the same (class, method, bci) key, the first-in-wins rule applies. Because the warm-path hook fires at actual execution time (before `call_VM` returns and `resolve_handle_call` continues), the warm-path record is typically inserted first. This is the desired behavior: warm-path records are exact, cold-path may be less reliable for edge cases.
