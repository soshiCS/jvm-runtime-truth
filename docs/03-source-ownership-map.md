# Source Ownership Map

For each capability, this document lists the primary and secondary files, the key functions, and what to modify when extending it.

See [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) for how these pieces connect.

---

## Core Export Infrastructure

### Primary file
`src/hotspot/share/classfile/soroushProvenanceGraph.cpp`  
`src/hotspot/share/classfile/soroushProvenanceGraph.hpp`

### Key functions

| Function | Purpose |
|---|---|
| `soroush_graph_enabled()` | Master switch — checks `SOROUSH_PROVENANCE_GRAPH=1`. Lazy-initializes `g_sg_enabled`. |
| `soroush_graph_enabled_addr()` | Returns `&g_sg_enabled` for direct assembly reads (warm-path hook). |
| `volatile int g_sg_enabled` | File-scope flag. 0 = disabled, 1 = enabled. Read by assembly without a function call. |
| `soroush_graph_generic_callsite()` | Monomorphic callsite emission. Used by invokehandle warm-path hook. Deduplicates by (src_class, method, descriptor, bci) — first-in-wins, one record per BCI. |
| `soroush_graph_poly_callsite()` | Polymorphic callsite emission. Used by invokevirtual cold/warm-path hooks (Phase 2C) and invokeinterface cold/warm-path hooks (Phase 2D). Deduplicates by **(src_class, src_loader_id, method, descriptor, bci, target_class, target_loader_id, target_method, target_desc)** — one record per unique (callsite, loader-qualified target) pair; multiple records per BCI when different targets or different loaders observed. Stored in `g_poly_buckets`, exported as Phase 3.1 in `soroush_graph_export_runtime_targets()`. Gap #17 fix (2026-05-30): both `src_loader_id` and `target_loader_id` are included in both hash and equality check. |
| `soroush_graph_indy_callsite()` | `invokedynamic` callsite emission (cold-path, from linkResolver). |
| `soroush_graph_target_set_callsite()` | GWT multi-target emission. |
| `soroush_graph_adapter_graph_callsite()` | Adapter graph emission. |
| `soroush_graph_generated_class()` | Bytecode artifact + generated class linkage. |
| `soroush_graph_bytecode()` | Records a class's bytecode (CRC, size, dump path). |
| `soroush_graph_hidden_identity()` | Records `runtime_name → artifact_crc` for hidden classes. |
| `soroush_graph_linkage()` | Records `runtime_target` (MH linkage, reflection constructor). Phase 2B: now encodes vframeStream source attribution in the node label (`scp=exact/missing`, `sc=`, `sm=`, `sd=`, `sb=`, `sl=`, `smr=`). |
| `soroush_graph_export_runtime_targets()` | The export pipeline. Walks all side tables and writes JSONL. |
| `soroush_graph_dump_summary()` | Emits `export_summary` record. |

### What to modify when extending
- Adding a new record type: add a side table struct, a `soroush_graph_*_callsite()` function, and a matching export block in `soroush_graph_export_runtime_targets()`.
- Changing dedup key: modify the `key` computation in `soroush_graph_generic_callsite()` or the relevant side table's `match()` function.
- Adding a new JSONL field: add `fprintf(f, ...)` to the export block for the relevant record type.

---

## Cold-Path Callsite Capture (invokehandle, invokeinterface)

### Primary file
`src/hotspot/share/interpreter/linkResolver.cpp`

### Key functions

| Function | Where called | Purpose |
|---|---|---|
| `LinkResolver::resolve_handle_call()` | `LinkResolver::resolve_invokehandle()` | Cold-path hook: fires once per CP cache entry. Reads appendix MH, walks with `sg_walk_mh`, emits `callsite_target` and/or `callsite_adapter_graph`. |
| `LinkResolver::runtime_resolve_interface_method()` | `LinkResolver::resolve_interface_call()` | **Phase 2D** invokeinterface cold-path hook: fires once per CP cache entry (first receiver type). Walks vframeStream to user frame, emits `callsite_target` via `soroush_graph_poly_callsite()`. |
| `InterpreterRuntime::soroush_trace_ii_dispatch()` | `TemplateTable::invokeinterface` (warm-path, normal itable path) | **Phase 2D** invokeinterface warm-path hook: fires on every normal itable dispatch. Receives concrete `Method*` (`rmethod`) after `lookup_interface_method`. Walks vframeStream, guards `op == _invokeinterface`. Emits `callsite_target` via `soroush_graph_poly_callsite()`. JRT_ENTRY in `interpreterRuntime.cpp`; declared in `interpreterRuntime.hpp`. |
| `LinkResolver::runtime_resolve_virtual_method()` | `LinkResolver::resolve_virtual_call()` | **Phase 2C** invokevirtual cold-path hook: fires once per CP cache entry (first receiver type). Walks vframeStream to user frame, emits `callsite_target` via `soroush_graph_poly_callsite()`. |
| `InterpreterRuntime::soroush_trace_iv_dispatch()` | `TemplateTable::invokevirtual_helper` (warm-path) | **Phase 2C** invokevirtual warm-path hook: fires on every non-final interpreted vtable dispatch. Receives concrete `Method*` (`rmethod`) after `lookup_virtual_method`. Walks vframeStream, guards `op == _invokevirtual` to skip invokeinterface forced-virtual cases. Emits `callsite_target` via `soroush_graph_poly_callsite()`. JRT_ENTRY in `interpreterRuntime.cpp`; declared in `interpreterRuntime.hpp`. |
| `sg_walk_mh()` | `resolve_handle_call`, `sg_trace_mh_impl` | Recursively decomposes a MethodHandle into `SgMhWalkResult`. Handles DirectMethodHandle, BoundMethodHandle, delegating MH types. |
| `sg_walk_generic_bmh()` | `sg_walk_mh` | Handles `BoundMethodHandle$Species_*` inner recursion. |
| `sg_trace_mh_impl()` | `InterpreterRuntime::sg_trace_mh_dispatch` | Warm-path MH target capture: walks live interpreter frame, reads receiver MH oop from the stack, calls `sg_walk_mh`. |
| `sg_u2at()` | Multiple | Reads a native-endian u2 from `bcp+1`. Always use this — never manual big-endian reads. |
| `soroush_trace_runtime_dispatch()` | `runtime_resolve_virtual_method`, `runtime_resolve_interface_method` | Stderr-only trace, fires for all resolved dispatches. Not related to JSONL export. |

### Dispatch case taxonomy

| Case | Condition | Source frame | Receiver recovery |
|---|---|---|---|
| **A** | Top interpreter frame is user code | Top frame IS the user frame | `thread->last_Java_sp() + arg_slots` (runtime stack) |
| **A2** | Top interpreter frame is `java/lang/invoke/*` LF/BMH internal | User frame recovered via vframeStream | `top.interpreter_frame_local_at(0)` (LF local[0] = the MH being invoked) |
| **B** | Top frame is `DirectMethodHandleAccessor` or `DirectConstructorHandleAccessor` | Walk vframeStream past accessor to user frame | Accessor's `target` field → `sg_walk_mh` depth 6 |

### What to modify when extending
- Adding a new dispatch category: add a branch in `resolve_handle_call` or add a new function called from the appropriate `LinkResolver::resolve_*` method.
- Supporting a new MH adapter type in `sg_walk_mh`: add a branch for the new `InstanceKlass` name, extract the relevant fields, recurse.
- Supporting a new reflection accessor type (JDK upgrade): update the Case B holder name check.

---

## Warm-Path invokehandle Hook

### Primary files
`src/hotspot/cpu/aarch64/templateTable_aarch64.cpp` (hook stub)  
`src/hotspot/share/interpreter/interpreterRuntime.cpp` / `.hpp` (JRT_ENTRY trampoline)  
`src/hotspot/share/interpreter/linkResolver.cpp` — `sg_trace_mh_impl` (implementation)

### Key code

**Hook location** in `templateTable_aarch64.cpp`, function `TemplateTable::invokehandle`:
```
After prepare_invoke, after null_check(r2).
r2 = live receiver MH oop.
rmethod = resolved Method*.
```

**Hook structure** (abbreviated):
```cpp
{
  Label L_sg_skip;
  __ lea(rscratch1, ExternalAddress((address)soroush_graph_enabled_addr()));
  __ ldrb(rscratch1, Address(rscratch1));
  __ cbz(rscratch1, L_sg_skip);
  __ stp(lr, zr, __ pre(sp, -2 * wordSize));   // MUST save lr
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address, InterpreterRuntime::sg_trace_mh_dispatch),
             r2);
  __ ldp(lr, zr, __ post(sp, 2 * wordSize));   // MUST restore lr
  __ bind(L_sg_skip);
}
```

**Critical constraints**:
- `lr` MUST be saved before `call_VM` and restored after. See [02-phase-history.md](02-phase-history.md) Milestone 6 for the full explanation.
- `rmethod` does NOT need explicit save/restore; `call_VM_leaf_base` saves it automatically.
- `__ pre()` / `__ post()` not bare `pre()` / `post()`.

### What to modify when extending
- Supporting arm64 JIT-compiled frames in the warm path: modify `sg_trace_mh_impl` to handle compiled frames (currently only interpreter frames are walked).
- ~~Adding warm-path capture for `invokevirtual`~~: DONE (Phase 2C). Hook is in `TemplateTable::invokevirtual_helper` non-final path; passes `rmethod` to `InterpreterRuntime::soroush_trace_iv_dispatch`.
- ~~Adding warm-path capture for `invokeinterface`~~: DONE (Phase 2D). Hook is in `TemplateTable::invokeinterface` normal itable path (after `profile_arguments_type`, before `jump_from_interpreted`); passes `rmethod` to `InterpreterRuntime::soroush_trace_ii_dispatch`. Cold-path (`runtime_resolve_interface_method`) also migrated to `soroush_graph_poly_callsite`.
- Adding warm-path capture for `invokespecial`: same pattern but `invokespecial` doesn't use vtable dispatch — the target is fully resolved at link time so warm-path adds no information.

---

## Hidden Class Identity

### Primary files
`src/hotspot/share/classfile/klassFactory.cpp` — hook site  
`src/hotspot/share/classfile/soroushProvenanceGraph.cpp` — `SgHiddenId`, `soroush_graph_hidden_identity`, export

### Key code sites in `klassFactory.cpp`
```cpp
// Before parse:
uint32_t hidden_artifact_crc = soroush_crc32(actual_stream->buffer(), actual_stream->length());

// After create_instance_klass() returns:
if (cl_info.is_hidden() && soroush_graph_enabled() && hidden_artifact_crc != 0) {
  const char* runtime_name = result->name()->as_C_string();
  soroush_graph_hidden_identity(runtime_name, hidden_artifact_crc, loader_id);
}
```

### What to modify when extending
- Supporting a new hidden class type: no change needed; the hook fires for all hidden classes.
- Changing the CRC algorithm: modify `soroush_crc32()` in `soroushProvenanceGraph.cpp`.

---

## Bytecode Rewriter

### Primary files
`src/hotspot/share/classfile/soroushClassfileRewriter.cpp`  
`src/hotspot/share/classfile/soroushClassfileRewriter.hpp`

The rewriter transforms bytecode of user-specified classes (controlled by `SOROUSH_REWRITER_PREFIX`) before class loading. It instruments method entries and exit points to support tracing and provenance tracking beyond what the interpreter hooks capture.

The rewriter is complementary to the interpreter hooks, not a replacement. Phase 1 validation was performed with the interpreter hooks alone (the Spring Boot and Runtime Truth runs use the interpreter hooks exclusively; the rewriter prefix is optional).

---

## Diagnostics

### Primary file
`src/hotspot/share/interpreter/linkResolver.cpp` — all diagnostic emission

### Key patterns

| Reason code | Meaning |
|---|---|
| `recv_from_method_result_or_field` | Receiver oop flows from a method return value or field load — static analyzer cannot track it |
| `recv_slot_oob` | Stack pointer minus arg_slots points outside the visible frame (SP underflow during adapter construction) |
| `recv_local_oop_invalid` | A local variable slot exists but its OOP is not valid (stale, uninitialized, or null) |
| `backward_goto_at_N_target_M` | Bytecode has a backward branch before the invoke — analyzer cannot determine receiver after loop |
| `unsupported_fast_multiop_0xde` | Bytecode rewriter has substituted fast opcode 0xde (fast_multiop); analyzer doesn't model it |
| `adapter_unknown_shape` | The adapter class (`DirectMethodHandle$StaticAccessor`, etc.) is not modeled by `sg_walk_mh` |
| `source_compiled_frame_unavailable` | The caller frame is JIT-compiled; no interpreter frame info available |
| `sibling_bci_unanalyzable` | (Superseded) Sibling BCI whose receiver cannot be determined at cold-path time |

### What to modify when extending
- Adding a new diagnostic reason: add a `snprintf(reason, ...)` case and call `soroush_graph_generic_callsite` with `evidence=DIAGNOSTIC`.
- Reducing diagnostics for a specific pattern: fix the receiver analyzer to handle that pattern, or add a new case in `resolve_handle_call` / `sg_trace_mh_impl`.

---

## Export Pipeline

### Primary file
`src/hotspot/share/classfile/soroushProvenanceGraph.cpp` — `soroush_graph_export_runtime_targets()`

The export function is called at JVM shutdown (via a shutdown hook registered during `soroush_graph_enabled()` initialization). It:
1. Locks `g_mutex`
2. Iterates `g_ts_buckets` (callsite targets), `g_ag_buckets` (adapter graphs), `g_gen_buckets` (generated classes)
3. Iterates `g_hidden_ids` (hidden class identities)
4. Iterates loaded classes for `bytecode_artifact` records
5. Writes `export_summary` with counts and `complete: true`

The output file is set via `SOROUSH_EXPORT_RUNTIME_TARGETS` env var.

---

## Runtime Truth UI

### Primary files
`tools/rt-ui/app.py` — Flask server, run management, REST API  
`tools/rt-ui/indexer.py` — JSONL → in-memory index  
`tools/rt-ui/static/index.html` — SPA frontend  
`tools/rt-ui/static/style.css` — UI styles

### Key functions in `indexer.py`

| Function | Purpose |
|---|---|
| `load_and_index(path, prefixes)` | Parses JSONL, builds all index structures |
| `find_best_artifact(index, class_name, loader_id)` | Resolves bytecode artifact for a class name + loader |
| `validate_run(run_dir, index)` | Runs 9 integrity checks on a completed run |
| `callsite_summary(cs)` | Returns lightweight summary of one callsite entry |

### What to modify when extending
- Adding a new record type to the UI: add parsing in `load_and_index`, add display logic in `index.html`.
- Adding a new validation check: add to `validate_run()` in `indexer.py`.

---

## Offline Causality Graph Builder (Phase 2a)

### Primary files
`tools/rt-ui/graph_builder.py` — offline graph construction from JSONL  
`tools/rt-ui/tests/test_graph_builder.py` — synthetic fixture tests (22 tests)

### Purpose
Reads a `runtime_targets.jsonl` file and builds a deterministic directed graph of runtime causality. No heuristic edges are ever created — missing connections produce orphan nodes and gap report entries instead.

### Key functions

| Function | Purpose |
|---|---|
| `build_graph(jsonl_path)` | Main entry: three-pass graph construction (Pass 1: nodes from records; Pass 1.5: multi-target upgrade for polymorphic callsites; Pass 2: cross-record edges; Pass 3: static_label assignment for unlabelled callsites). Returns `(CausalityGraph, raw_summary)`. |
| `build_gap_report(graph, raw_summary)` | Produces the gap report dict with counts, orphan list, staticization labels. |
| `validate_graph(graph, report)` | Runs 11 deterministic correctness checks. Returns pass/fail per check. |
| `query_chain(graph, src_class, src_method, src_bci)` | Returns full causality chain for one callsite. |
| `list_orphan_runtime_targets(graph)` | Returns all runtime_target nodes with no source BCI. |
| `list_blocked_callsites(graph)` | Returns callsites that cannot be staticized. |
| `list_staticizable_callsites(graph)` | Returns callsites labelled as staticization candidates. |

### Node types

| Type constant | Meaning |
|---|---|
| `NT_CALLSITE` | A BCI in a source method where a dispatch occurs |
| `NT_METHOD` | A Java method (class + name + descriptor + loader) |
| `NT_CLASS` | A Java class (name + loader) |
| `NT_ADAPTER_GRAPH` | Virtual node: one per `callsite_adapter_graph` record |
| `NT_ADAPTER_NODE` | A single node within an adapter graph |
| `NT_HIDDEN_CLASS` | A hidden class identified by runtime_name +0x… |
| `NT_BYTECODE_ART` | A bytecode snapshot (class + loader + CRC + kind) |
| `NT_RUNTIME_TARGET` | An unattributed linkage target (source_capture=missing — JVM init or daemon thread; Phase 2B remainder) |
| `NT_TARGET_SET` | A GWT multi-target dispatch set |
| `NT_DIAGNOSTIC` | An unresolved callsite (with reason code) |

### Edge types

| Type constant | Created from |
|---|---|
| `ET_CALLSITE_TARGET` | `callsite_target` (all non-invokedynamic categories) |
| `ET_LAMBDA_BODY` | `callsite_target[invokedynamic]` via `lmf_impl_class`/`lmf_impl_method` |
| `ET_HAS_ADAPTER_GRAPH` | `callsite_adapter_graph` → its source callsite |
| `ET_HAS_ADAPTER_NODE` | adapter_graph → each adapter_node in its nodes array |
| `ET_ADAPTER_NODE_TARGET` | adapter_node (primary_target role) → method |
| `ET_INTERNAL_EDGE` | intra-graph edges within adapter_graph.edges array |
| `ET_HAS_TARGET_SET` | `callsite_target_set` → target_set node |
| `ET_TARGET_SET_MEMBER` | target_set → each member method |
| `ET_HAS_BYTECODE_ART` | class → bytecode_artifact |
| `ET_HAS_HIDDEN_IDENTITY` | hidden_class → bytecode_artifact (by CRC + loader match) |
| `ET_CALLSITE_RT_ATTRIBUTED` | callsite → method via attributed runtime_target (Phase 2B: source_capture=exact) |
| `ET_RUNTIME_LINKAGE` | runtime_target → method (target known, but source unattributed) |
| `ET_ORPHAN_RT` | documents the orphan fact (runtime_target with source_capture=missing) |
| `ET_DIAGNOSTIC_AT` | diagnostic self-annotation |

### Staticization readiness labels

| Label | Meaning |
|---|---|
| `staticizable_candidate_direct` | Single exact target, evidence=LINKAGE_GUARANTEED |
| `staticizable_candidate_adapter_modeled` | Adapter graph present, no unknown shapes |
| `needs_hidden_class_reconstruction` | Target is hidden class — needs CRC resolution |
| `blocked_multi_target` | Site with multiple observed dispatch targets: GWT/GWC MH sites (from `callsite_target_set` records) or polymorphic invokevirtual sites (from multiple `callsite_target` records with same source BCI, assigned by Pass 1.5 in `build_graph()`) |
| `blocked_unknown_adapter` | Adapter graph has unknown_shape node |
| `observed_only_not_proven` | evidence=OBSERVED_ONLY (not guaranteed) |
| `insufficient_evidence` | Diagnostic record — target not resolved |

### Identity rules (stable node IDs)

```
callsite    callsite::{src_class}::{src_method}::{src_desc}::{src_bci}::{src_loader}
method      method::{class}::{method_name}::{descriptor}::{loader_id}
class       class::{class_name}::{loader_id}
adapter_graph  adapter_graph::{src_class}::{src_method}::{src_desc}::{src_bci}
adapter_node   adapter_node::{callsite_id}::node_{node_id}
hidden_class   hidden_class::{runtime_name}::{loader_id}
bytecode_art   bytecode_artifact::{class}::{loader_id}::{crc}::{kind}
runtime_target runtime_target::{target_class}::{target_method}::{target_desc}::{target_loader}
target_set  target_set::{src_class}::{src_method}::{src_desc}::{src_bci}
diagnostic  diagnostic::{src_class}::{src_method}::{src_desc}::{src_bci}
```

### invokedynamic schema note
`callsite_target[invokedynamic]` records have NO `target_class`/`target_method` fields. The implementation is in `lmf_impl_class`/`lmf_impl_method`. The graph creates a `LAMBDA_BODY` edge to the impl method. Some invokedynamic records (StringConcatFactory, non-LMF bootstraps) have no `lmf_impl_class` and produce a callsite node with no outgoing method edge.

### Phase 2B: runtime_target source attribution (COMPLETE)

**Runtime Truth**: 915 orphans → 0 orphans (100% reduction). **Spring Boot**: 2,905 orphans → 0 orphans (100% reduction).

Phase 2B added a vframeStream walk in `soroush_trace_membername_resolution()` (`methodHandles.cpp`). When a user or framework frame is found, `source_capture=exact` plus source fields are emitted in the `runtime_target` record. When no frame is found (JVM init, daemon threads), `source_capture=missing` with `source_missing_reason` is emitted and the record remains an orphan.

In the current workloads, every MH linkage event has a recoverable frame (even JDK-internal frames like `jdk/internal/module/` that are not in the skip list are attributed correctly). The 0-orphan result is expected for these workloads.

`runtime_target` records with `source_capture=exact` are processed in `build_graph()` pass 1 as callsite nodes with `ET_CALLSITE_RT_ATTRIBUTED` edges. Records with `source_capture=missing` (or no `source_capture` field — backward-compatible) remain `NT_RUNTIME_TARGET` orphan nodes.

### What to modify when extending
- Adding a new edge type: add a constant `ET_*`, emit it in the appropriate pass-1 or pass-2 section of `build_graph()`.
- Adding a new node type: add a constant `NT_*`, add creation logic, add it to the gap report's `nodes_by_type` (automatic via `node_counts()`).
- Changing runtime_target attribution (e.g., adding more skip-list packages): modify the vframeStream walk in `soroush_trace_membername_resolution()` in `methodHandles.cpp`.
- Adding a query: add a function that takes `CausalityGraph` and returns a list of node dicts. Expose via `--query` arg in `main()`.

---

## Test Cases

### Location
Source: `/tmp/cases-build/src/testcases/`  
Classes: `/tmp/cases-build/classes/`  
Main: `testcases.TestCasesMain`

### Files by capability

| File | Capability tested |
|---|---|
| `Case01_LambdaIndy.java` | Lambda bootstrap, `invokedynamic`, captured and free variables |
| `Case02_StringConcatIndy.java` | `StringConcatFactory.makeConcatWithConstants` |
| `Case03_DirectMethodHandle.java` | Direct `MethodHandle` (static, virtual, constructor) |
| `Case04_MHReceiverOrigins.java` | MH receiver from local, field, static, method return, array, chain — sibling BCI coverage |
| `Case05_TypeAdapters.java` | `asType()` type conversion adapters |
| `Case06_ArgumentAdapters.java` | `insertArguments`, `dropArguments`, `bindTo`, `permuteArguments` |
| `Case07_FilterFoldCollect.java` | `filterArguments`, `filterReturnValue`, `foldArguments` |
| `Case08_SpreadCollector.java` | `asSpreader`, `asCollector`, `invokeWithArguments` |
| `Case09_GuardCatchFinally.java` | `guardWithTest`, `catchException`, `tryFinally` |
| `Case10_Reflection.java` | `Method.invoke`, `Constructor.newInstance`, private access, `MethodHandle` via reflection |
| `Case11_DynamicProxy.java` | `Proxy.newProxyInstance`, `InvocationHandler`, proxy class identification |
| `Case12_HiddenClass.java` | `Lookup.defineHiddenClass`, hidden class invocation, identity |
| `Case13_InvokevirtualMono.java` | **Phase 2C**: monomorphic `invokevirtual` — one abstract class (`Shape/Circle.describe()`), cold-path + warm-path both fire, one `callsite_target` record |
| `Case14_InvokevirtualPoly.java` | **Phase 2C**: polymorphic `invokevirtual` — `Animal` with Dog/Cat/Bird subclasses. Single BCI 54 dispatches to three concrete targets via loop. Warm-path hook captures all three; graph shows `static_label=blocked_multi_target` (= SR_MULTI_TARGET). |
| `Case15_InvokeinterfacePoly.java` | **Phase 2D**: polymorphic `invokeinterface` — `Speaker` interface with Dog/Cat/Bird implementations. Single BCI 54 dispatches to three concrete targets via loop. Warm-path hook captures all three; graph shows `static_label=blocked_multi_target`. |
| `HiddenClassTemplate.java` | Template class loaded as hidden in Case12 |
| `Runtime TruthCasesMain.java` | Test runner, asserts PASS/FAIL for each of 15 cases |
