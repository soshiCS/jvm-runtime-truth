# Phase History

This document records every completed milestone in chronological order.
For architecture details, see [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md).
For the current build/test commands, see [07-build-workflow-guide.md](07-build-workflow-guide.md).

---

## Milestone 1: Reflection Recovery (Case B)

### Goal
Capture `callsite_target` records for call sites that go through `java.lang.reflect.Method.invoke` and `java.lang.reflect.Constructor.newInstance`.

### Problem
JDK 21 routes all `Method.invoke` calls through `DirectMethodHandleAccessor.invokeImpl`, which itself executes an `invokehandle` opcode on an internally-constructed MethodHandle. The cold-path hook in `resolve_handle_call` fires with the accessor frame at the top of the stack, not the user frame. Without special handling, the source class/method/BCI is attributed to the accessor, not the user's `m.invoke(...)` callsite.

Additionally, the accessor's target field always holds a `BoundMethodHandle` (BMH) adapter wrapping the actual target. Walking only the top MH finds the BMH, not the underlying concrete method.

### Solution

**Case B detection** in `resolve_handle_call`:
- When `top_frame.method().holder()` matches `jdk/internal/reflect/DirectMethodHandleAccessor` or `DirectConstructorHandleAccessor`, set `is_reflection_accessor = true`.
- Walk `vframeStream` past the accessor to find the user frame, use it as the callsite source.
- Read the accessor's `this` local (local 0), then walk its `target` field via `sg_walk_mh(target_mh, 6)` to traverse any BMH adapter chain and reach the concrete method.

**`sg_walk_mh` depth**: `MethodHandleAccessorFactory.makeSpecializedTarget` always wraps the core DMH in BMH adapters. Depth 6 is sufficient for the accessor chain in JDK 21.

### Files Modified
- `src/hotspot/share/interpreter/linkResolver.cpp` — Case B branch in `resolve_handle_call`, `sg_walk_mh` implementation

### Validation
Case 10 (`Case10_Reflection.java`) passes with exact records for static, instance, constructor, private, and `strlen` (string length via MH).

### Lessons Learned
- `DirectMethodHandleAccessor` is new in JDK 21; earlier JDK versions used `NativeMethodAccessorImpl`. The Case B detection must use the JDK 21 class names.
- `sg_walk_mh` must handle both the case where the top-level MH is already a `DirectMethodHandle` (no BMH wrapping) and where it is wrapped in one or more `BoundMethodHandle$Species_*` adapters.

---

## Milestone 2: Hidden Class Identity Recovery

### Goal
Associate the runtime address-qualified names of hidden classes (`App$$Lambda+0x0000007000133400`) with stable, content-addressable identifiers (CRC32 of class bytes).

### Problem
Lambda stubs and `LambdaForm$*` hidden classes are named with a runtime heap address suffix (`+0x...`). This address changes on every JVM run. The provenance graph's `bytecode_artifact` record for a hidden class needs a stable identity to allow the indexer to match it to `callsite_target` records, and to support cross-run comparison.

The naive fix — calling `soroush_graph_hidden_identity()` inside `soroush_graph_bytecode()` — fails because `soroush_graph_bytecode()` is called before `ClassFileParser::create_instance_klass()` completes. At that point, the `+0x` suffix has not yet been appended to the class name by `ClassFileParser::mangle_hidden_class_name()`. The internal name is still the base (e.g., `App$$Lambda`, not `App$$Lambda+0x...`).

### Solution

**Two-phase emission in `klassFactory.cpp`**:
1. Before parsing: compute `hidden_artifact_crc = soroush_crc32(actual_stream->buffer(), actual_stream->length())`.
2. After `parser.create_instance_klass()` returns (when the mangled name with `+0x` is now set), call:
   ```cpp
   soroush_graph_hidden_identity(result->name()->as_C_string(), hidden_artifact_crc, loader_id)
   ```

**`SgHiddenId` side table** in `soroushProvenanceGraph.cpp`: stores `(runtime_name, artifact_crc, loader_id)` tuples. At export time, emits a `hidden_class_identity` JSONL record for each.

**Indexer alias**: In `indexer.py`, for hidden `bytecode_artifact` records, also inserts `artifacts[(base_crc, loader)]` alias so `find_best_artifact()` can resolve a CRC-suffixed lookup.

### Files Modified
- `src/hotspot/share/classfile/klassFactory.cpp` — CRC capture before parse, `soroush_graph_hidden_identity` call after `create_instance_klass`
- `src/hotspot/share/classfile/soroushProvenanceGraph.cpp` — `SgHiddenId` struct, `soroush_graph_hidden_identity()`, export emission
- `src/hotspot/share/classfile/soroushProvenanceGraph.hpp` — `soroush_graph_hidden_identity()` declaration
- `tools/manycore-ui/indexer.py` — base-CRC alias insertion

### Validation
Case 12 (`Case12_HiddenClass.java`) passes. Spring Boot run produces 1,381 `hidden_class_identity` records, including 5 `Application$$Lambda+0x...` entries with unique CRCs.

### Lessons Learned
- The timing constraint is: identity can only be emitted AFTER `create_instance_klass()` sets the mangled name. Calling it before is the silent failure mode — name has no `+0x`, identity record has wrong key.
- A hidden class that is never loaded (dead code path) produces no `hidden_class_identity` record, which is correct.

---

## Milestone 3: invokeinterface Capture / Dynamic Proxy Dispatch

### Goal
Capture `callsite_target` records for `invokeinterface` bytecodes, specifically for JDK dynamic proxy dispatch (`Proxy.newProxyInstance`) and CGLIB interceptor calls.

### Problem
The existing instrumentation only hooked `resolve_handle_call` (invokehandle path). `invokeinterface` had only a `stderr`-only trace via `soroush_trace_runtime_dispatch`. This meant:
- JDK proxy: `$Proxy0.add(7, 4)` dispatch to the `InvocationHandler` lambda was unrecorded.
- CGLIB interceptor: `Application$$SpringCGLIB$$0.setBeanFactory` dispatch to `BeanFactoryAwareMethodInterceptor.intercept` was unrecorded.
- Case 11 (`Case11_DynamicProxy.java`): BCIs 31/41/48/89 completely absent.

### Solution

**Hook in `LinkResolver::runtime_resolve_interface_method`** (after existing `soroush_trace_runtime_dispatch` call):

```cpp
if (soroush_graph_enabled() && !recv.is_null() && !selected_method.is_null()) {
  // Walk vframeStream to first non-JDK frame
  // Verify top user bytecode is invokeinterface (0xb9)
  // Read source BCI, cp_index, loader
  // Call soroush_graph_generic_callsite("invokeinterface", ...)
}
```

The hook fires once per call site due to the inline cache: after first resolution, HotSpot patches the call site and `runtime_resolve_interface_method` is never called again for that site. This means exactly one `callsite_target` record per `invokeinterface` call site, which is the desired behavior.

**JDK-internal frame filtering**: The vframeStream walk skips frames whose holder starts with `java/`, `jdk/`, `sun/`, or `com/sun/`. This prevents JDK-internal `invokeinterface` from being attributed to user code and vice versa.

### Files Modified
- `src/hotspot/share/interpreter/linkResolver.cpp` — `runtime_resolve_interface_method`, invokeinterface hook

### Validation
Case 11 passes with records for all 4 user-code BCIs.  
Spring Boot: 9 user-code `invokeinterface` callsite records including proxy and CGLIB paths.

### Lessons Learned
- The inline-cache mechanism is a feature here: it ensures exactly-once firing per call site, naturally deduplicating without requiring the side-table dedup. But it also means if `runtime_resolve_interface_method` is NOT called (e.g., if the call site is megamorphic), no record is emitted. For Phase 1 workloads this is acceptable; megamorphic dispatch is a Phase 2 concern.
- The CGLIB `setBeanFactory` callsite in `Application$$SpringCGLIB$$0` was captured via this hook, confirming that generated class call sites (not just user call sites) are covered.

---

## Milestone 4: Adapter Graph Decomposition

### Goal
For `invokehandle` and `invokedynamic` sites, decompose the MethodHandle adapter chain into a structured graph of nodes, each describing the role and classification of one adapter component.

### Problem
A MethodHandle invocation such as `mh.invoke(x, y)` may traverse a chain of adapters: type conversion, argument binding, guard-with-test, filter, fold, etc. Recording only the final target misses the structural information needed to understand whether the call site is staticizable.

### Solution

**`sg_walk_mh` / `sg_walk_generic_bmh`** in `linkResolver.cpp`:
- Recursively traverses a `BoundMethodHandle$Species_*` graph.
- Each node gets a `role` (primary_target, secondary_component, etc.), `classification` (user_class, internal_jdk, bound_data, etc.), and exactness flag.
- Collects into `SgMhWalkResult` which feeds `soroush_graph_adapter_graph_callsite()`.

**`soroush_graph_adapter_graph_callsite()`** in `soroushProvenanceGraph.cpp`:
- Stores adapter graph in `g_ag_buckets` side table, keyed by (source_class, method, bci).
- At export time, emits `callsite_adapter_graph` records with the full `nodes[]` array.

**`sg_node_infer_semantic_type_conv` / `sg_compute_node_semantic`**: semantic labeling of adapter nodes (e.g., `string_concat`, `guard_with_test`, `type_conversion`).

**`soroush_graph_target_set_callsite()`**: for GWT adapters, captures the test predicate, true branch, and false branch as a `callsite_target_set` record.

### Files Modified
- `src/hotspot/share/interpreter/linkResolver.cpp` — `sg_walk_mh`, `sg_walk_generic_bmh`, `sg_classify_dual_target_semantics`, `sg_node_infer_semantic_type_conv`, `sg_compute_node_semantic`
- `src/hotspot/share/classfile/soroushProvenanceGraph.cpp` — `soroush_graph_adapter_graph_callsite`, `soroush_graph_target_set_callsite`, export for both record types

### Validation
Cases 05–09 pass (type adapters, argument adapters, filter/fold, spread/collector, guard/catch/finally).  
Spring Boot: 630 `callsite_adapter_graph` records, 13 from user-code sources with 31 total adapter nodes.

### Lessons Learned
- `BoundMethodHandle$Species_*` classes are named by the bound field type sequence (e.g., `Species_LL` = two reference fields). The walk must handle all species variants.
- `SgMhWalkResult` carries both the final target and intermediate nodes. Both are needed: the target for `callsite_target`, the nodes for `callsite_adapter_graph`.

---

## Milestone 5: Case04 Sibling-BCI Silent Omission Fix

### Goal
Capture callsite records for BCIs that share a constant pool cache entry with an earlier BCI.

### Problem
In `Case04_MHReceiverOrigins.run()`, BCIs 51, 67, 95, and 121 all invoke `MethodHandle` objects obtained via different means, but two pairs share the same CP constant pool index:
- BCIs 31 and 51, 95, 121 share CP index `#51`
- BCIs 42 and 67 share CP index `#57`

HotSpot maps ONE CP index → ONE CP cache entry. `resolve_handle_call` fires only when a CP cache entry is resolved for the first time. BCIs 51, 67, 95, 121 execute after their respective CP cache entries are already resolved (by BCIs 31 and 42). They never trigger `resolve_handle_call`, so they were silently absent from the export.

An initial fix attempted to use `sg_emit_sibling_bcis` — scanning the bytecode for other BCIs that share the same CP cache index and emitting diagnostic records for them. This failed because the CP cache index was being read with the wrong byte order.

**Root cause of wrong byte order**: HotSpot's bytecode rewriter stores CP cache indices with `Bytes::put_native_u2` (native/little-endian on AArch64). The scan code was reading them big-endian (manual `byte[bci+1]<<8 | byte[bci+2]` or `sg_u2at(bcp+1)` which read at offset +2 instead of +1). The guard `src_cp >= 0` always blocked `sg_emit_sibling_bcis` because `src_cp` was always -1.

### Solution (superseded by Milestone 6)

The immediate fix corrected the byte order in four places in `linkResolver.cpp`:
- `sg_emit_sibling_bcis` scan: `sg_u2at(scan_bcp + 1)` → `sg_u2at(scan_bcp)`
- Case A2 user frame BCI: `sg_u2at(bcp + 1)` → `sg_u2at(bcp)`
- Case A2 LF-fallback BCI: `sg_u2at(bcp + 1)` → `sg_u2at(bcp)`
- Case A direct frame BCI: manual big-endian → `sg_u2at(bcp)`

where `sg_u2at(bcp) = Bytes::get_native_u2(bcp + 1)` — reads 2 bytes at opcode+1 in native byte order.

This produced diagnostic records for BCIs 51, 67, 95, 121 instead of silence. The diagnostics were accurate (the receiver cannot be determined at sibling time) but not exact.

**This fix was superseded by Milestone 6 (warm-path hook)**, which produces exact records for all sibling BCIs by capturing targets at actual execution time.

### Files Modified
- `src/hotspot/share/interpreter/linkResolver.cpp` — four `sg_u2at` byte-order fixes, `sg_emit_sibling_bcis`

### Lessons Learned
- `Bytes::get_native_u2(addr)` reads a 16-bit value at `addr` in native byte order, which is little-endian on AArch64. `addr` here is `bcp+1` (the byte after the opcode). So `sg_u2at(bcp)` is the right call, NOT `sg_u2at(bcp+1)`.
- The guard `src_cp >= 0` was a silent failure mode. If `sg_u2at` returns -1 due to a wrong address, no scan runs, no error is logged, and the siblings are silently absent.

---

## Milestone 6: Case04 Warm-Path invokehandle Hook

### Goal
Capture exact `callsite_target` records for EVERY `invokehandle` execution, not just the first (cold-path) resolution.

### Problem
`sg_emit_sibling_bcis` (Milestone 5) could only produce diagnostic records for sibling BCIs because the CP cache entry is already resolved when they execute — there is no way to read the appendix MH from the cache at that point in a way that distinguishes the sibling's actual receiver. The only way to get exact targets for all BCIs is to intercept at actual execution time.

### Solution

**Warm-path hook in `TemplateTable::invokehandle`** (`templateTable_aarch64.cpp`):

After `prepare_invoke` (which leaves `r2 = live receiver MH oop` and `rmethod = resolved Method*`), and after the null-check of `r2`, insert:

```cpp
{
  Label L_sg_skip;
  __ lea(rscratch1, ExternalAddress((address)soroush_graph_enabled_addr()));
  __ ldrb(rscratch1, Address(rscratch1));
  __ cbz(rscratch1, L_sg_skip);
  __ stp(lr, zr, __ pre(sp, -2 * wordSize));   // save lr (clobbered by call_VM)
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address, InterpreterRuntime::sg_trace_mh_dispatch),
             r2);
  __ ldp(lr, zr, __ post(sp, 2 * wordSize));   // restore lr
  __ bind(L_sg_skip);
}
```

`sg_trace_mh_dispatch` is a `JRT_ENTRY` trampoline in `interpreterRuntime.cpp` that delegates to `sg_trace_mh_impl` in `linkResolver.cpp`. The implementation reads the live interpreter frame stack, walks `vframeStream` to find the user frame, reads the MH receiver oop from `r2`, calls `sg_walk_mh`, and emits a `callsite_target` record via `soroush_graph_generic_callsite`.

Guards in `sg_trace_mh_impl`:
1. `Universe::is_fully_initialized()` — skip during JVM bootstrap before MH infrastructure is ready.
2. `soroush_graph_enabled()` — skip when export is off.
3. `recv_oop != nullptr` — skip null receivers (shouldn't happen post-null-check, but defensive).
4. `Universe::is_fully_initialized()` again on the thread pointer before frame walk.
5. JDK-internal class filter — skip frames from `java/`, `jdk/`, `sun/`.

**`sg_emit_sibling_bcis` removal**: After the warm-path hook was added, the `sg_emit_sibling_bcis` call was removed from `resolve_handle_call` Part B. Keeping it would cause diagnostic records to be emitted on the cold path before the warm-path exact records fire, and since dedup is first-wins, the diagnostics would block the exact records from ever being stored.

### The lr (Link Register) Bug

**This was the hardest bug in the project.** Without `lr` save/restore, the JVM crashes with `BootstrapMethodError: Cannot invoke "MethodHandle.invokeBasic()"`.

Root cause:
1. `prepare_invoke` loads the invoke return-entry address from a table into `lr` (AArch64 r30). This is the address the compiled callee will `ret` to after the dispatched method returns.
2. `call_VM` internally calls `call_VM_leaf_base`, which executes `blr rscratch1` (branch-and-link). This is a call instruction — it overwrites `lr` with `pc+4`.
3. After `call_VM` returns, `lr` contains an address inside the call_VM stub, not the original invoke return-entry address.
4. `jump_from_interpreted` subsequently branches to the compiled entry of the dispatched method. The compiled code eventually executes `ret`, which branches to `lr` — which now points to garbage → NPE or silent corruption.

Fix: push `lr` to the native stack before `call_VM`, pop after:
```cpp
__ stp(lr, zr, __ pre(sp, -2 * wordSize));  // lr + alignment pad
// ... call_VM ...
__ ldp(lr, zr, __ post(sp, 2 * wordSize));
```

Note: `rmethod` (r12) does NOT need explicit save/restore. `call_VM_leaf_base` already saves/restores `rscratch1` and `rmethod` via `stp/ldp`.

### The JRT_ENTRY `current` vs `thread` Bug

The `JRT_ENTRY` macro uses `current` as the thread variable internally in assertions. If the function parameter is named `thread` instead of `current`, the macro generates code that refers to an undeclared variable. The symptom is a compile-time error like `'current' was not declared`.

Fix: name the parameter `current`:
```cpp
JRT_ENTRY(void, InterpreterRuntime::sg_trace_mh_dispatch(JavaThread* current, oopDesc* recv))
```

### The `pre()`/`post()` Scope Bug

In `MacroAssembler` member functions (e.g., `call_VM_leaf_base`), `pre()` and `post()` are `MacroAssembler` member functions accessible without prefix. In `templateTable_aarch64.cpp`, the assembler is accessed via the `__` macro (`__ pre(sp, -16)`). Using bare `pre(sp, -16)` in template table code fails to compile with `'pre' was not declared in this scope`.

### Files Modified
- `src/hotspot/cpu/aarch64/templateTable_aarch64.cpp` — warm-path hook in `TemplateTable::invokehandle`, `#include "classfile/soroushProvenanceGraph.hpp"`
- `src/hotspot/share/interpreter/interpreterRuntime.cpp` — `JRT_ENTRY` for `sg_trace_mh_dispatch`
- `src/hotspot/share/interpreter/interpreterRuntime.hpp` — declarations for `sg_trace_mh_dispatch` and `sg_trace_mh_impl`
- `src/hotspot/share/interpreter/linkResolver.cpp` — `sg_trace_mh_impl` implementation, removal of `sg_emit_sibling_bcis` from Part B

### Validation
All 6 BCIs in Case04 produce `callsite_target` with `source_capture=exact`:
- BCI 31 → `Math.abs`
- BCI 42 → `Math.max`
- BCI 51 → `Math.abs` (exact, not inferred)
- BCI 67 → `Math.min` (**not** Math.abs — proves runtime dispatch, not sibling inference)
- BCI 95 → `Math.abs`
- BCI 121 → `Math.abs`

BCI 67 → `Math.min` is the critical proof: sibling inference from CP index #57 would have given `Math.max` (the primary BCI's target). The warm-path hook gives the correct `Math.min`.

### Lessons Learned
- The `lr`-clobbering pattern affects ALL `call_VM` calls that follow `prepare_invoke`. Any future warm-path hook that makes a VM call after `prepare_invoke` must save/restore `lr`.
- `call_VM_leaf_base` saves `rmethod` automatically. Do not add manual `mov` / `push` for `rmethod` around these calls; it creates imbalanced stacks.
- `sg_emit_sibling_bcis` emitting diagnostics before warm-path records arrive was the dedup conflict root cause. The lesson: warm-path records are first-class, cold-path inference should not pre-empt them.

---

## Milestone 7: Spring Boot Integration Validation

### Goal
Validate the instrumentation against a real (non-synthetic) Spring Boot 4.0 application, exercising CGLIB, JDK proxy, reflection, lambda streams, and MethodHandle paths in a realistic container startup context.

### Problem
Spring Boot starts embedded Tomcat and blocks indefinitely, making automated validation impossible. The existing test harness only covers synthetic `ManyCoreCasesMain` cases.

### Solution

**Application modification** (`gs-spring-boot/complete/src/main/java/com/example/springboot/Application.java`):
- Added `validationRunner` `CommandLineRunner` bean that explicitly exercises:
  1. Direct CGLIB bean invocation (`ctx.getBean(HelloController.class).index()`)
  2. Reflection (`HelloController.class.getMethod("index").invoke(controller)`)
  3. MethodHandle via `findVirtual` + `invokeWithArguments`
  4. JDK `Proxy.newProxyInstance` with lambda `InvocationHandler`
  5. `invokedynamic` (lambda + stream: filter, map, collect)
  6. MethodHandle via `findStatic` (`Math.abs`)
  7. Reflection on `Arrays.sort`
  8. CGLIB confirmation via `getClass().getSimpleName()`

**`--spring.main.web-application-type=none`**: Spring flag that disables Tomcat. Application runs all `CommandLineRunner` beans and exits with code 0. No code changes to the JVM or Spring internals required.

**Spring Boot version**: 4.0.6. Built with `mvn package -DskipTests`. JAR: 21.9 MB.

### Validation Results
- Exit code: 0
- User-code diagnostics: 0
- User callsite records: 26 (all `source_capture=exact`)
  - 15 invokedynamic, 9 invokeinterface, 1 methodhandle_invokeExact, 1 methodhandle_invoke
- CGLIB confirmed: `Application$$SpringCGLIB$$0` runtime class, `setBeanFactory` interceptor callsite captured
- JDK proxy end-to-end: `$Proxy63.greet` → `Application$$Lambda+0x....invoke` chain captured
- Hidden class identity: 5 `Application$$Lambda` entries with distinct CRCs
- Total records: 15,663

See [05-validation-guide.md](05-validation-guide.md) for exact reproduction commands.

### Lessons Learned
- `--spring.main.web-application-type=none` is the correct Spring mechanism for headless validation. `SpringApplication.run().close()` is less reliable and has different lifecycle semantics.
- `Method.invoke(controller)` at the user level produces a `callsite_target` with `source_class=Application` (from the warm-path hook on the underlying `invokehandle`), but there is no user-level callsite record explicitly attributing the `m.invoke(...)` line to a reflection category. The attribution travels through the accessor frame. This is a known limitation documented in [06-known-limitations.md](06-known-limitations.md).
- `CommandLineRunner.run` declares `throws Exception`. `MethodHandle.invokeWithArguments` throws `Throwable`, which is wider. Wrap with `try { } catch (Throwable t) { throw new RuntimeException(t); }`.

---

## Phase 2E — Warm-Path Reflection Attribution (2026-05-30)

### Goal
Fix Gap #15: capture the `InvocableHandlerMethod.doInvoke bci=55 → HelloController.index` edge for Spring MVC HTTP request dispatch.

### Root Cause
`MemberName.resolve` (cold-path, single-fire) fires once per reflected method. Startup validation code resolves `HelloController.index` first, attributing it to `Application.lambda$validationRunner$3`. HTTP-path `Method.invoke` calls find the `MemberName` pre-resolved — no hook fires. `-Xint` produces identical missing-edge behavior; JIT compilation was irrelevant.

### Solution
Extend `soroush_trace_iv_dispatch` to detect `Method.invoke` dispatches and decode the receiver `java.lang.reflect.Method` object:

1. **Template table** (`templateTable_aarch64.cpp`): Pass `recv` (= r2, the Method object) as arg2 to the `call_VM`.
2. **Runtime hook** (`interpreterRuntime.cpp`): When `concrete_method == java/lang/reflect/Method.invoke` and `recv_oop != nullptr`, decode via `java_lang_reflect_Method::clazz()` + `java_lang_reflect_Method::slot()` + `InstanceKlass::method_with_idnum()`. Emit `soroush_graph_poly_callsite("reflection_method_invoke", ...)`.
3. **Dedup**: Same `g_poly_buckets` as Phase 2C/2D. Multiple reflected targets at same BCI → SR_MULTI_TARGET.

### Files Changed
- `src/hotspot/share/interpreter/interpreterRuntime.cpp` — Phase 2E block in `soroush_trace_iv_dispatch`
- `src/hotspot/share/interpreter/interpreterRuntime.hpp` — `recv_oop` parameter added
- `src/hotspot/cpu/aarch64/templateTable_aarch64.cpp` — `recv` as arg2 in `call_VM`
- `tools/manycore-ui/tests/test_graph_builder.py` — 3 Phase 2E tests added (tests 24–26)
- `docs/04-runtime-capture-architecture.md` — Phase 2E section added
- `docs/06-known-limitations.md` — Gap #15 marked RESOLVED

### Validation
- Spring Boot (mixed+HTTP): `doInvoke bci=55 → HelloController.index` present (1 record). Startup-only: 0. -Xint+HTTP: 1. Consistent.
- ManyCore: 15/15 PASS. Case10 reflection records correct.
- Graph tests: 26/26 PASS (3 new Phase 2E tests).
- No debug output in production builds.
