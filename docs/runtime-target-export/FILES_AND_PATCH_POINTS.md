# Files and Patch Points

This document provides exact source file paths, function names, and line ranges
for every major component of the runtime target export system.

**Important:** There are two copies of `linkResolver.cpp`:
- **Export copy** (edit here): `src/hotspot/share/interpreter/linkResolver.cpp`
- **Build copy** (sync here after edits): `src/hotspot/share/classfile/linkResolver.cpp`

After every edit to the export copy, sync to the build copy:
```bash
cp jdk21u-export/src/hotspot/share/interpreter/linkResolver.cpp \
   jdk21u/src/hotspot/share/classfile/linkResolver.cpp
```

After every `make hotspot`, copy the dylib:
```bash
cp build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
   build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib
```

---

## linkResolver.cpp — core MH capture

**Export path:** `src/hotspot/share/interpreter/linkResolver.cpp`

### Helper functions

| Function | Lines | Description |
|----------|-------|-------------|
| `sg_u2at` | 2161–2163 | Read big-endian u2 from bytecode |
| `sg_analyze_mh_receiver` | 2228–2613 | Symbolic backward receiver analysis |
| `sg_emit_sibling_bcis` | 2634–2710 | Sibling CP-cache BCI scan and diagnostic emission |
| `sg_extract_dmh_target` | 2877–2890 | Extract SgMhTarget from a DirectMethodHandle |
| `sg_unwrap_delegating` | 2895–2901 | Peel DelegatingMethodHandle wrapper |
| `sg_walk_generic_bmh` | 3085–3215 | Walk a generic BMH species to build SgAdapterNode array |
| `sg_recover_mh_recv_from_java_sp` | 3255–3275 | Read MH oop from live Java expression stack |
| `sg_walk_mh` | 3279–3376 | Classify MH oop: DIRECT/GWT/GWC/ADAPTER_GRAPH/UNKNOWN |
| `sg_oop_valid` | 2801–2803 | Minimal oop sanity check |

### Data structures

| Struct | Lines | Description |
|--------|-------|-------------|
| `SgMhTarget` | 2740–2746 | Resolved concrete Java method target |
| `SgAdapterNode` | 2749–2760 | One node in a BMH adapter graph |
| `SgAdapterEdge` | 2763–2767 | Edge between adapter nodes |
| `SgMhWalkResult` | 2769–2799 | Result of sg_walk_mh: shape + targets/nodes |

### Three-case framework in `resolve_handle_call`

| Element | Lines | Description |
|---------|-------|-------------|
| `is_lf_dispatch_case` declaration | 3506 | Flag distinguishing Case A from A2 |
| Case B — reflection accessor | 3520–3558 | Top frame is `jdk/internal/reflect/` accessor |
| Case A2 — LF dispatch frame | 3560–3645 | Top frame is `java/lang/invoke/` LF; walk vframeStream |
| Case A — user frame | 3648–3673 | Top frame IS the user frame; read directly |
| Step 1 symbolic analysis call | ~3700 | Call to `sg_analyze_mh_receiver` |
| Step 2 runtime fallback (Case A) | 3731–3746 | `sg_recover_mh_recv_from_java_sp` |
| Step 2a local[0] fallback (Case A2) | 3747–3760 | `top.interpreter_frame_local_at(0)` |
| `soroush_graph_target_set_callsite` call | 3785 | GWT/GWC record emission |
| `soroush_graph_adapter_graph_callsite` call | 3820 | ADAPTER_GRAPH record emission |
| `soroush_graph_generic_callsite` call | 3932 | DIRECT/diagnostic record emission |

---

## soroushProvenanceGraph.hpp — types and declarations

**Path:** `src/hotspot/share/classfile/soroushProvenanceGraph.hpp`

| Declaration | Lines | Description |
|-------------|-------|-------------|
| `SoroushGraphNodeType` enum | 27–44 | 14 node types (SG_NODE_CLASS through SG_NODE_LAMBDAFORM_EXEC) |
| `SoroushGraphEdgeType` enum | 47–66 | 16 edge types |
| `soroush_method_token_register` | 105–107 | Register a method token at rewrite time |
| `soroush_method_token_lookup` | 112–114 | Look up a token at runtime |
| `soroush_graph_generated_class` | 119–122 | Record a runtime-generated class |
| `soroush_graph_generic_callsite` | 173–181 | Record a MH callsite (single target or diagnostic) |
| `soroush_graph_bytecode` | 196–198 | Record a bytecode artifact |
| `SgMhTargetEntry` struct | 285–294 | Target entry for callsite_target_set export |
| `soroush_graph_target_set_callsite` | 306–313 | Record a GWT/GWC callsite |
| `SgAdapterNodeEntry` struct | 318–333 | Node entry for callsite_adapter_graph export |
| `SgAdapterEdgeEntry` struct | 336–340 | Edge entry for callsite_adapter_graph export |
| `soroush_graph_adapter_graph_callsite` | 356–365 | Record a BMH adapter graph callsite |
| `soroush_graph_export_runtime_targets` | 371 | Export all records to JSONL at shutdown |

---

## soroushProvenanceGraph.cpp — implementation

**Path:** `src/hotspot/share/classfile/soroushProvenanceGraph.cpp`

### Token registry

| Function | Lines | Description |
|----------|-------|-------------|
| `soroush_method_token_register` | 251–300 | Append-only, immortal per-method token table |
| `soroush_method_token_lookup` | 302–321 | Look up token → (class, method, desc, loader, hidden, crc) |

### Callsite record tables

| Symbol | Lines | Description |
|--------|-------|-------------|
| `g_gen_buckets` | ~153 | 512-bucket hash table for generic callsites |
| `g_ts_buckets` | ~191 | 512-bucket hash table for target-set callsites |
| `g_ag_buckets` | ~247 | 256-bucket hash table for adapter-graph callsites |
| `soroush_graph_generic_callsite` | 419–525 | Insert/upgrade generic callsite record |
| `soroush_graph_target_set_callsite` | 527–605 | Insert target-set record |
| `soroush_graph_adapter_graph_callsite` | 606–708 | Insert adapter-graph record |

### Generated class and bytecode recording

| Function | Lines | Description |
|----------|-------|-------------|
| `soroush_graph_generated_class` | 1017–1053 | Record a generated class with provenance fields |

### JSONL export phases

| Phase | Lines | Output records |
|-------|-------|----------------|
| Phase 1 — method identity | 1570–1608 | `method_identity` |
| Phase 2 — indy callsite_target | 1610–1682 | `callsite_target` (category=invokedynamic) |
| Phase 3 — generic callsite_target | 1684–1878 | `callsite_target` or `diagnostic` |
| Phase 3.5 — callsite_target_set | 1878–1957 | `callsite_target_set` |
| Phase 3.6 — callsite_adapter_graph | 1957–2081 | `callsite_adapter_graph` |
| Phase 4 — remaining records | 2081–2382 | `runtime_target`, `generated_class`, `bytecode_artifact` |
| Summary | ~2390 | `export_summary` |

---

## klassFactory.cpp — class load hooks

**Path:** `src/hotspot/share/classfile/klassFactory.cpp`

| Hook | Lines | Description |
|------|-------|-------------|
| `recover_runtime_generated_class` | 509–605 | Capture runtime-generated class bytes + provenance |
| `soroush_graph_generated_class` call | ~600 | Feed recovery data into the graph |
| Bytecode artifact capture block | ~935 | Capture original + final bytecode |
| Disk dump to `/tmp/soroush_jvm_dump/` | ~246, 282, 315, 524 | Write raw class bytes to disk |

---

## systemDictionary.cpp — invokedynamic hook

**Path:** `src/hotspot/share/classfile/systemDictionary.cpp`

| Hook | Lines | Description |
|------|-------|-------------|
| `soroush_trace_indy_enabled` | 134–141 | Env-var gate for indy tracing |
| trace_id assignment | ~2556 | Assign indy_trace_id at bootstrap time |
| Graph call for indy callsite | ~2626, 2695 | `soroush_graph_indy_callsite` |

---

## methodHandles.cpp — MH/reflection linkage hook

**Path:** `src/hotspot/share/prims/methodHandles.cpp`

| Hook | Lines | Description |
|------|-------|-------------|
| `soroush_trace_membername_resolution` | 95–121 | Record `LINKAGE_GUARANTEED` target at MemberName.resolve |
| Call from Method path | 260 | java.lang.reflect.Method |
| Call from Constructor path | 271 | java.lang.reflect.Constructor |
| Call from MemberName.resolve | 849, 873 | General MH resolution path |

---

## java.cpp — shutdown export hook

**Path:** `src/hotspot/share/runtime/java.cpp`

| Hook | Lines | Description |
|------|-------|-------------|
| `soroush_graph_export_runtime_targets` declaration | ~396 | Declaration for before_exit call |
| Export call in `before_exit` | ~470 | Triggers JSONL write at VM shutdown |
| Full shutdown block | ~460–472 | Reads `SOROUSH_EXPORT_RUNTIME_TARGETS` env var |

---

## soroushClassfileRewriter.cpp — bytecode rewriter

**Path:** `src/hotspot/share/classfile/soroushClassfileRewriter.cpp`

(Relevant to bytecode artifact export; less central to callsite attribution)

| Function | Lines | Description |
|----------|-------|-------------|
| `soroush_build_pc_map` | 482–591 | Build instruction layout with new PCs |
| `soroush_converge_layout` | 906–1065 | Iterative fixed-point branch-widening convergence |
| `soroush_emit_rewritten_code` | 1066–1250+ | Emit the rewritten bytecode |
| `soroush_transform_code_attribute_entry_code` | 3102–3250+ | Per-method transform |
| `insert_entry_exit_trace` (Phase 5) | 4491–4493 | Main entry point |
| `soroush_method_token_register` call | ~4314 | Register each method's token at rewrite time |

---

## JDK Java-side changes

These are modifications to Java source files in the JDK class library:

| File | Change |
|------|--------|
| `src/java.base/share/classes/java/lang/System.java` | `soroushTraceEnter(int)`, `soroushTraceExit(int)` native method declarations; `soroushAsyncEnabled()`, `soroushAsyncHandoff()` for async tracking |
| `src/java.base/share/native/libjava/System.c` | Native registration for all `soroush*` natives |
| `make/data/hotspot-symbols/symbols-unix` | Export symbols for `JVM_SoroushTraceEnter`, `JVM_SoroushTraceExit`, etc. |
| `src/java.base/share/classes/java/util/concurrent/ThreadPoolExecutor.java` | `soroushAsyncHandoff` calls at submit/run |
| `src/java.base/share/classes/java/util/concurrent/ForkJoinPool.java` | `soroushAsyncHandoff` at poolSubmit |
| `src/java.base/share/classes/java/util/concurrent/ForkJoinTask.java` | `soroushAsyncHandoff` at doExec |

---

## New files (not in upstream jdk21u)

| File | Description |
|------|-------------|
| `src/hotspot/share/classfile/soroushProvenanceGraph.cpp` | In-memory provenance graph + all callsite record tables + JSONL export |
| `src/hotspot/share/classfile/soroushProvenanceGraph.hpp` | Public API declarations for the graph |
| `src/hotspot/share/classfile/soroushClassfileRewriter.cpp` | Verifier-safe Phase 5 bytecode rewriter |
| `src/hotspot/share/classfile/soroushClassfileRewriter.hpp` | Rewriter public API |
| `SOROUSH_JVM_SPEC.md` | Broader project spec (this branch is §4D + §4E) |
| `docs/runtime-target-export/` | This documentation tree |
