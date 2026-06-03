# Phase 2B Design Review: `runtime_target` Source Attribution

**Date**: 2026-05-30  
**Status**: COMPLETE. Design and implementation both done.  
**Scope**: Root-cause investigation of runtime_target orphan records; design review of attribution solutions; Phase 2B implementation.

## Phase 2B Implementation Summary (2026-05-30)

**Result**: Runtime Truth 915 → 0 orphans (100%). Spring Boot 2,905 → 0 orphans (100%).

**Approach chosen**: Extended `runtime_target` records (Option A) — source attribution added to the `runtime_target` JSONL schema, keeping the record type for backward compatibility. The `soroush_graph_linkage()` function signature was extended with source frame parameters; the vframeStream walk was added to `soroush_trace_membername_resolution()` in `methodHandles.cpp`.

**New JSONL fields on `runtime_target`**:
- `source_capture`: `"exact"` (frame found) or `"missing"` (no user frame)
- When `exact`: `source_class`, `source_method`, `source_descriptor`, `source_bci` (int), `source_loader_id`
- When `missing`: `source_missing_reason` (e.g., `"no_user_frame_on_stack"`)

**Files changed**:
- `src/hotspot/share/prims/methodHandles.cpp` — vframeStream walk in `soroush_trace_membername_resolution()`; added `#include "runtime/vframe.inline.hpp"`
- `src/hotspot/share/classfile/soroushProvenanceGraph.hpp` — extended `soroush_graph_linkage()` signature
- `src/hotspot/share/classfile/soroushProvenanceGraph.cpp` — extended `soroush_graph_linkage()` label encoding; extended Phase 4 export to emit source fields
- `tools/rt-ui/graph_builder.py` — new `ET_CALLSITE_RT_ATTRIBUTED` edge type; `runtime_target` handler now creates callsite nodes for attributed records; `connected_runtime_targets` computed from actual data
- `tools/rt-ui/tests/test_graph_builder.py` — 5 new Phase 2B tests (tests 15–19)

**Validation**: 11/11 checks pass on Runtime Truth and Spring Boot. 19/19 unit tests pass. `heuristic_edges_created = 0` preserved.

---

## 1. Problem Statement

Phase 2A produced a causality graph with two categories of unconnected nodes:

| Export | `runtime_target` orphans | `connected_runtime_targets` |
|---|---|---|
| Runtime Truth (12-case suite) | **915** | 0 |
| Spring Boot | **2,905** | 0 |

Every `runtime_target` record in both exports contains target identity (`target_class`, `target_method`, `target_descriptor`, `target_loader_id`) but no source identity (`source_class`, `source_method`, `source_bci`). Without a source, the graph builder cannot create a `CALLSITE_TARGET` edge. The records become orphan nodes.

---

## 2. `runtime_target` Architecture

### 2.1 How runtime_target records are produced

The export produces `runtime_target` records in Phase 4 of `soroush_graph_export_runtime_targets()` ([soroushProvenanceGraph.cpp:2323](../src/hotspot/share/classfile/soroushProvenanceGraph.cpp#L2323)) by walking the in-memory graph edges at VM shutdown. Three edge types produce `runtime_target` records:

| Edge type | Source node type | `dispatch_kind` in record | `caller_context` |
|---|---|---|---|
| `LINKS_TO` | `SG_NODE_METHODHANDLE_LINKAGE` | `methodhandle_linkage` | string from node label |
| `LINKS_TO` | `SG_NODE_REFLECTION_INVOKE` | `reflection` | string from node label |
| `RESOLVES_TO` | `SG_NODE_MH_ADAPTER` | `direct_methodhandle` | MH kind string |
| `EXECUTES` | `SG_NODE_EXECUTION` | `execution_trace` | (none) |

In practice, only the first two types appear in Phase 1 exports:
- `methodhandle_linkage` + `reflection` account for 100% of all orphans
- `direct_methodhandle` and `execution_trace` produce zero orphan records

### 2.2 What the caller_context string encodes

The `caller_context` field is extracted from the `label` field of the source node, which is set when the node was originally created. The label contains a `src=<string>` field. That string comes from the `source` argument passed to `soroush_graph_linkage()`, which is set to one of three literal strings:

| `caller_context` value | Origin |
|---|---|
| `MemberName.resolve` | Lines 849, 873 of `methodHandles.cpp` — MH/constructor resolution |
| `java.lang.reflect.Method` | Line 260 of `methodHandles.cpp` — `reflect.Method` → MH conversion |
| `java.lang.reflect.Constructor` | Line 271 of `methodHandles.cpp` — `reflect.Constructor` → MH conversion |

These strings are NOT file/class/method names. They are the hardcoded context strings passed as the first argument to `soroush_trace_membername_resolution()`.

---

## 3. Emission Site Inventory

### 3.1 All emission sites

There are **four call sites** to `soroush_trace_membername_resolution()`, all in a single file:

**File**: `src/hotspot/share/prims/methodHandles.cpp`

```
Line 260:  soroush_trace_membername_resolution("java.lang.reflect.Method", m, THREAD);
Line 271:  soroush_trace_membername_resolution("java.lang.reflect.Constructor", m, THREAD);
Line 849:  soroush_trace_membername_resolution("MemberName.resolve", result.resolved_method(), THREAD);
Line 873:  soroush_trace_membername_resolution("MemberName.resolve", result.resolved_method(), THREAD);
```

All four call `soroush_graph_linkage(node_type, source, holder, mname, sig, loader_id)`, which creates a graph node with no source frame data.

### 3.2 Line 260 — `reflect.Method → MemberName`

**Containing function**: `MethodHandles::init_MemberName(Handle mname, Handle target, TRAPS)`  
**Call path**: User code → `MethodHandles.lookup().unreflect(method)` → JNI →  `MethodHandles::init_MemberName()` → emission  
**What is resolved**: A `java.lang.reflect.Method` is being wrapped into a `MemberName` for use as a MethodHandle target. The target `Method*` is fully known.  
**Records produced**: 5 (Runtime Truth), 3 (Spring Boot) — rare; happens only when user code explicitly calls `unreflect()` on a reflect Method.

### 3.3 Line 271 — `reflect.Constructor → MemberName`

**Containing function**: `MethodHandles::init_MemberName(Handle mname, Handle target, TRAPS)`  
**Call path**: User code → `MethodHandles.lookup().unreflectConstructor(ctor)` → JNI → `MethodHandles::init_MemberName()` → emission  
**What is resolved**: A `java.lang.reflect.Constructor` is being wrapped into a `MemberName`. The target `Method*` is fully known.  
**Records produced**: 7 (Runtime Truth), 249 (Spring Boot) — moderately common in Spring Boot (DI constructor resolution).

### 3.4 Lines 849, 873 — `MemberName.resolve`

**Containing function**: `MethodHandles::resolve_MemberName(Handle mname, Klass* caller, int lookup_mode, bool speculative_resolve, TRAPS)`  
**Call paths**:
- Line 849 (`IS_METHOD` case): User code → `java.lang.invoke.MemberName.resolve()` → JNI → `MethodHandles::resolve_MemberName()` → IS_METHOD branch → emission
- Line 873 (`IS_CONSTRUCTOR` case): Same path → IS_CONSTRUCTOR branch → emission

**What is resolved**: A `MemberName` object (encoding a class + method name + descriptor) is being resolved to a concrete JVM `Method*`. This is the core MH construction linkage operation triggered by `MethodHandles.lookup().findVirtual()`, `findStatic()`, `findConstructor()`, etc.  
**Records produced**: 903 (Runtime Truth), 2,653 (Spring Boot) — the dominant category.

### 3.5 Why linkResolver.cpp is NOT the correct file

Previous documentation (including `06-known-limitations.md` Limitation #13 and `00-agent-handoff.md` Task 2) incorrectly stated the fix location as `linkResolver.cpp`. This is wrong.

`linkResolver.cpp` does NOT call `soroush_graph_linkage()`. It calls:
- `soroush_graph_generic_callsite()` — for MH warm-path invocations (lines 1827, 2829, 4312, 4440, 4739)
- `soroush_graph_mh_chain()` — for MH structure walk (lines 241, 286)
- `soroush_graph_indy_callsite()` — for invokedynamic linkage (via `systemDictionary.cpp`)

The `runtime_target` orphan problem is entirely in `methodHandles.cpp`.

---

## 4. Root Cause

The root cause is a **missing source frame capture** in `soroush_trace_membername_resolution()`.

The function receives:
- `source` — a hardcoded string constant (not a frame reference)
- `method` — the resolved target `Method*` (complete)
- `THREAD` — the calling `JavaThread*` (the full Java call stack is available)

But `soroush_graph_linkage()`, which is called from within this function, stores only target identity. It has no parameters for source frame data. The `SgNode` it creates is keyed by target method identity alone.

At export time, the LINKS_TO edge that `soroush_graph_linkage()` creates is walked and produces a `runtime_target` record. At that point, only the target method identity is available — the source frame that existed at call time has long since been discarded.

**Stated differently**: the graph node for MH linkage was designed to record "what was linked" (target), not "who requested it" (source). No mechanism was added to the node, edge, or side tables to carry the calling context.

---

## 5. Source Frame Availability

At each emission site, `THREAD` is a live `JavaThread*`. The Java call stack is fully available via `vframeStream`.

**Evidence that vframeStream works at these sites:**
- `vframeStream` is already used in `linkResolver.cpp` at lines 1786, 3970, 4014 — in `sg_trace_mh_impl` and `resolve_handle_call`, both of which fire from a similar "call from Java into native" context
- The Case B pattern at lines 3969–4001 of `linkResolver.cpp` proves the exact same walk works for reflection accessor frames, and that pattern was developed and validated in Phase 1

**Frame state at the four sites:**
- Line 260/271: Called from `JVM_ResolveMHConstant` or similar JNI path. Java thread has an interpreter or compiled frame. `THREAD->has_last_Java_frame()` is true.
- Lines 849/873: Called from deep inside `MethodHandles::resolve_MemberName()`. The Java thread is in a normal "Java → JNI → C++ native" transition. `THREAD->has_last_Java_frame()` is true. Interpreter frames are immediately available. For compiled frames, BCI comes from debug info.

**Packages to skip when walking** (same as Case B in linkResolver.cpp lines 3974–3977):
```
java/lang/invoke/
java/lang/reflect/
jdk/internal/reflect/
sun/reflect/
```

The first frame NOT in these packages is the effective caller of the MH resolution operation.

---

## 6. Option Comparison

### Option A: Extend `soroush_graph_linkage()` with source frame fields

Add `src_class`, `src_method`, `src_desc`, `src_bci`, `src_loader_id` parameters to `soroush_graph_linkage()`. Store them in a new side table keyed by node ID. At export time, emit them in the `runtime_target` record as source fields.

**Assessment:**
- Changes `soroushProvenanceGraph.hpp` API (non-trivial ABI impact)
- Requires new fields in `SgNode` or a new parallel side table  
- Changes the export's LINKS_TO handler (line 2371 of `soroushProvenanceGraph.cpp`)
- Records remain `runtime_target` type with added source fields
- `graph_builder.py` must be updated to consume the new fields and create `CALLSITE_TARGET` edges instead of `ORPHAN_RT` edges
- Preserves the existing runtime_target record type (backward-compatible output change)

**Complexity**: High (two-layer change: JVM + graph_builder)  
**Correctness**: High  
**Maintenance cost**: High (extended API, new structs)

### Option B: Route through `soroush_graph_generic_callsite()` with vframeStream walk

Replace the four `soroush_graph_linkage()` calls in `methodHandles.cpp` with `soroush_graph_generic_callsite()` calls. Add a vframeStream walk immediately before the call to extract the source frame. The call produces a record in `g_gen_buckets`, which exports as a `callsite_target` record.

**Assessment:**
- No changes to `soroushProvenanceGraph.hpp` API
- No changes to `SgNode`, `SgEdge`, the graph intern tables, or the export's graph walk
- Uses existing infrastructure: `soroush_graph_generic_callsite()` already handles dedup, source+target storage, and exact/diagnostic branching
- Records change from `runtime_target` to `callsite_target` — the graph builder already handles `callsite_target` records with full attribution, producing `CALLSITE_TARGET` edges
- Categories: `"membername_method_resolve"`, `"membername_constructor_resolve"`, `"membername_linkage"` (new category strings, or can reuse `"methodhandle_invoke"`)
- When source frame is not available: `source_exact=false` triggers a `diagnostic` record (existing fallback, no new code)
- The `soroush_graph_linkage()` calls are removed; the `SG_NODE_METHODHANDLE_LINKAGE` and `SG_NODE_REFLECTION_INVOKE` nodes for these sites are no longer created (they were only used for runtime_target emission, which is replaced)

**Complexity**: Low (changes to one function in one file, ~25 lines total)  
**Correctness**: High — same mechanism already proven for Case B  
**Maintenance cost**: Low — no new structs, no new API surface  
**Completeness**: Same as Option A — produces `diagnostic` when source unavailable

### Option C: Emit a correlation record at emission time; join offline

Add a new record type (`callsite_linkage_corr`) emitted at the four sites with both source frame and target identity. The graph builder joins at analysis time.

**Assessment:**
- Adds a new JSONL record type (changes to export schema, graph_builder, docs)
- Adds storage overhead (new side table in the JVM)
- More complex than Option B for no benefit — Option B already uses the existing generic callsite table which IS the offline join mechanism
- Not recommended

### Option D: Post-hoc heuristic correlation in graph_builder.py

Match `runtime_target.target_class/method` against `callsite_target` targets; create speculative edges.

**Assessment:**
- Already documented as unacceptable — violates the hard rule `heuristic_edges_created = 0`
- Fails when same target is resolved from multiple callers
- Not recommended

**Recommendation: Option B.**

---

## 7. Option B: Detailed Design

### 7.1 Changes to `methodHandles.cpp`

Modify `soroush_trace_membername_resolution()` as follows:

```cpp
static void soroush_trace_membername_resolution(const char* source,
                                                 Method* method, TRAPS) {
  if (method == nullptr) return;
  bool trace = soroush_trace_reflection_enabled();
  bool graph = soroush_graph_enabled();
  if (!trace && !graph) return;

  ResourceMark rm(THREAD);
  const char* holder = method->method_holder()->name()->as_C_string();
  const char* mname  = method->name()->as_C_string();
  const char* sig    = method->signature()->as_C_string();
  if (trace) {
    fprintf(stderr, "[JVM REFLECT] membername_source=%s\n", source);
    fprintf(stderr, "[JVM REFLECT] membername_target=%s.%s%s\n", holder, mname, sig);
  }
  if (!graph) return;

  uint64_t tgt_loader = (uint64_t)(uintptr_t)
      method->method_holder()->class_loader_data();

  // Determine category from source string.
  const char* cat;
  if (source != nullptr && strncmp(source, "java.lang.reflect.Method", 24) == 0)
    cat = "membername_method_resolve";
  else if (source != nullptr && strncmp(source, "java.lang.reflect.Constructor", 29) == 0)
    cat = "membername_constructor_resolve";
  else
    cat = "membername_linkage";

  // Walk the Java stack to find the first non-MH-internal user frame.
  // Skip: java/lang/invoke/, java/lang/reflect/, jdk/internal/reflect/, sun/reflect/
  // — same skip-list used by the Case B walk in linkResolver.cpp lines 3970-4001.
  const char* src_class  = nullptr;
  const char* src_method = nullptr;
  const char* src_desc   = nullptr;
  int         src_bci    = -1;
  int         src_opcode = 0;
  int         src_cp     = -1;
  uint64_t    src_loader = 0;
  bool        src_ok     = false;

  if (THREAD->has_last_Java_frame()) {
    for (vframeStream vfst(THREAD); !vfst.at_end(); vfst.next()) {
      Method* m = vfst.method();
      if (m == nullptr) continue;
      const char* h = m->method_holder()->name()->as_C_string();
      if (strncmp(h, "java/lang/invoke/",    17) == 0 ||
          strncmp(h, "java/lang/reflect/",   18) == 0 ||
          strncmp(h, "jdk/internal/reflect/",21) == 0 ||
          strncmp(h, "sun/reflect/",         12) == 0)
        continue;
      int bci = vfst.bci();
      if (bci < 0) break;
      src_class  = h;
      src_loader = (uint64_t)(uintptr_t)m->method_holder()->class_loader_data();
      src_method = m->name()->as_C_string();
      src_desc   = m->signature()->as_C_string();
      src_bci    = bci;
      if (bci < m->code_size()) {
        address bcp = m->bcp_from(bci);
        src_opcode = (int)(uint8_t)Bytecodes::java_code_at(m, bcp);
      }
      src_ok = true;
      break;
    }
  }

  // Route through the generic callsite table — same infrastructure used by
  // reflection_method_invoke and methodhandle_invoke records.
  // source_exact = src_ok (frame found); target_exact = true (Method* is exact).
  // When !src_ok: soroush_graph_generic_callsite emits a diagnostic record.
  soroush_graph_generic_callsite(
      cat,
      src_class, src_loader,
      src_method, src_desc,
      src_bci, src_opcode, src_cp,
      holder, tgt_loader,
      mname, sig,
      /*source_exact=*/ src_ok, /*target_exact=*/ true,
      /*diagnostic_reason=*/ src_ok ? nullptr : "no_user_frame_on_stack");
}
```

The four existing `soroush_graph_linkage()` calls are **removed** (replaced by the `soroush_graph_generic_callsite()` call above).

### 7.2 Required includes in `methodHandles.cpp`

The following headers are already available in the file but `vframeStream` may need an additional include:
```cpp
#include "runtime/vframe.inline.hpp"  // if not already included
```

Verify against the existing includes in `linkResolver.cpp` where `vframeStream` is used.

### 7.3 Changes to `soroushProvenanceGraph.cpp` / `.hpp`

**None.** `soroush_graph_generic_callsite()` already exists and already handles all required cases.

### 7.4 Changes to `graph_builder.py`

The `callsite_target` records produced by this path will have:
- `category` in `{"membername_method_resolve", "membername_constructor_resolve", "membername_linkage"}`
- `evidence = "OBSERVED_ONLY"` (from the generic callsite export)
- Full `source_class`, `source_method`, `source_descriptor`, `source_bci` fields

**graph_builder.py already handles this correctly.** The Phase 1 `callsite_target` processing in `build_graph()` (lines 318–369) is category-agnostic. It processes any record with `record=callsite_target` and `source_class` populated. No changes are needed.

The staticization label for these records will be:
- `observed_only_not_proven` — because `evidence=OBSERVED_ONLY` and `staticizable=False` (MH linkage does not guarantee staticizability from source alone)

This is correct: MH linkage events observed at one execution may have different targets at a different load or configuration.

### 7.5 Changes to existing `runtime_target` infrastructure

The `SG_NODE_METHODHANDLE_LINKAGE` and `SG_NODE_REFLECTION_INVOKE` nodes for these four sites will no longer be created (since `soroush_graph_linkage()` is no longer called). The LINKS_TO edges for these sites will no longer be created. As a result:
- `runtime_target` records with `dispatch_kind=methodhandle_linkage` or `dispatch_kind=reflection` will no longer be emitted for these sites
- `rt_count` in the export summary drops to 0 for LINKS_TO-derived records
- `callsite_target_count` increases by ~915 (Runtime Truth) / ~2,905 (Spring Boot)

The `direct_methodhandle` path from `RESOLVES_TO` edges is unaffected (it has zero records in practice).

---

## 8. Attribution Feasibility Analysis

### 8.1 When source attribution succeeds

A user frame is present when the MH linkage event is triggered synchronously from Java code that is currently executing in the interpreter. This covers:

- `MethodHandles.lookup().findVirtual(...)` called from user code
- `MethodHandles.lookup().findConstructor(...)` called from user code
- Spring DI constructor resolution triggered from `ApplicationContext.refresh()` — a Spring framework frame IS found (at `org/springframework/beans/factory/...`), which is the correct attribution
- Any `unreflect(method)` or `unreflectConstructor(ctor)` called from user or framework code

For compiled frames: the vframeStream transparently handles compiled/deoptimized frames. BCI comes from inline debug info. This may be approximate by ±1 BCI (same limitation as Case B in Phase 1).

### 8.2 When source attribution fails (produces `diagnostic`)

A user frame is NOT present when:
1. **JVM boot initialization**: Class loading and MH linkage during JVM startup before `main()` is on the stack. These happen on the main thread before any Java frames are established.
2. **Background daemon threads**: Threads that perform MH resolution without any user code in their call chain (e.g., JIT compiler threads, GC helpers, finalizer threads).
3. **Compiled frame with no debug info**: Very rare; only if `-g:none` or aggressive inlining strips debug info.

These cases produce `diagnostic` records (via the `!src_ok` path in `soroush_graph_generic_callsite()`).

### 8.3 Expected counts

Based on Phase 1 data:

**Runtime Truth (915 orphans):**

| Category | Count | Attributable? | Reason |
|---|---|---|---|
| `methodhandle_linkage` (MemberName.resolve) | 903 | ~880 (~97%) | Most MH resolutions are user-triggered |
| `reflection` (reflect.Constructor) | 7 | ~7 (100%) | Direct user code in test cases |
| `reflection` (reflect.Method) | 5 | ~5 (100%) | Direct user code |
| **Estimated remaining orphans** | **~23** | — | JVM init-time resolutions |

**Spring Boot (2,905 orphans):**

| Category | Count | Attributable? | Reason |
|---|---|---|---|
| `methodhandle_linkage` (MemberName.resolve) | 2,653 | ~2,400 (~90%) | Most are Spring DI init (Spring frames found) |
| `reflection` (reflect.Constructor) | 249 | ~240 (~96%) | Spring DI constructor injection |
| `reflection` (reflect.Method) | 3 | ~3 (100%) | Direct user or framework calls |
| **Estimated remaining orphans** | **~265** | — | JVM/Spring init before main(), background threads |

Note: "attributable" includes Spring framework frames (`org/springframework/...`). The attribution is correct: these MH resolutions ARE triggered from Spring code, which itself IS triggered from user code via `SpringApplication.run()`. If user-only frames are required, the remaining unattributed count is higher (~800 for Spring Boot).

### 8.4 Fundamentally unattributable records

Some `runtime_target` records cannot be attributed regardless of implementation approach:

1. **Static initializer MH linkage**: A `<clinit>` method that calls `MethodHandles.lookup().findVirtual()` at class load time. The stack shows only JVM-internal class initialization frames. No user callsite exists.

2. **Bootstrap linkage during `main()` setup**: Some MH linkage happens during JDK internal bootstrap before the user's `main()` frame is on the stack. This is a small count (< 10 in Runtime Truth).

3. **Finalizer thread**: Any MH usage in a finalizer runs on the finalizer background thread with no user callsite.

These will correctly produce `diagnostic` records rather than `callsite_target` records, which is the right behavior per the hard rule "never emit callsite_target with guessed data."

---

## 9. Risk Analysis

### Risk 1: vframeStream walk cost

**Description**: Walking the Java stack at MH linkage time adds overhead. MH linkage is not a hot path (it happens once per unique MH target, not on every invocation), but if a large workload resolves thousands of MH targets, the walk adds cost.

**Mitigation**: The walk is guarded by `soroush_graph_enabled()` (which checks `g_sg_enabled`, an exported byte read directly in assembly for hot paths). When the provenance system is disabled, the entire function is a no-op. The cost is zero in production.

When enabled, the vframeStream walk is O(stack depth) which is typically 10–30 frames. This is negligible compared to the cost of MH linkage itself.

**Verdict**: Acceptable.

### Risk 2: ResourceMark scope interaction

**Description**: `vframeStream::method()` returns a `Method*` whose `name()->as_C_string()` uses ResourceMark allocation. The `ResourceMark rm(THREAD)` at the top of the function covers this. All string pointers passed to `soroush_graph_generic_callsite()` are `sg_strdup`'d immediately inside the callee, so ResourceMark expiry after the call returns is safe.

**Mitigation**: Same as existing Case B pattern in `linkResolver.cpp` lines 3974–3998, which is already in production use.

**Verdict**: No risk.

### Risk 3: `has_last_Java_frame()` false on unexpected threads

**Description**: Some threads that trigger MH linkage may not have a last Java frame (e.g., JIT compiler threads, VM operation threads).

**Mitigation**: The `if (THREAD->has_last_Java_frame())` guard before the walk handles this correctly. When the guard fails, `src_ok = false`, and a `diagnostic` record is emitted. The VM is not aborted or affected.

**Verdict**: Handled.

### Risk 4: Dedup interaction with existing callsite_target records

**Description**: `soroush_graph_generic_callsite()` deduplicates by `(src_class, src_method, src_desc, src_bci)`. If a single BCI both fires a `callsite_target` warm-path hook (from `sg_trace_mh_impl`) AND reaches `soroush_trace_membername_resolution()`, first-in-wins applies. The warm-path hook fires LATER (at MH invocation time), while `soroush_trace_membername_resolution` fires at MH CONSTRUCTION time.

These are structurally different events (construction vs invocation) and typically at different BCIs. If they DO collide at the same BCI (unusual), the construction-time record wins (it was inserted first). This is safe — both records have full attribution.

**Verdict**: Acceptable.

### Risk 5: Category string proliferation

**Description**: Three new category strings (`membername_method_resolve`, `membername_constructor_resolve`, `membername_linkage`) are added. Future tooling must know about these.

**Mitigation**: The categories are documented in this file and in `03-source-ownership-map.md`. The graph builder is category-agnostic for `callsite_target` records. No string matching on category is required for graph construction.

**Verdict**: Low risk. Categories are documentation, not logic.

---

## 10. Proposed Phase 2B Scope

**Minimal Phase 2B (recommended):**
Implement Option B as described in Section 7. Modify `soroush_trace_membername_resolution()` in `methodHandles.cpp`. No other JVM files need changes.

**Estimated lines of JVM code changed**: ~25 lines modified (the function body), zero new files, zero new headers.

**Estimated impact**:
- Runtime Truth: 915 orphans → ~23 orphans (~97% reduction)
- Spring Boot: 2,905 orphans → ~265 orphans (~91% reduction)

**Validation steps after implementation:**

1. Rebuild hotspot (no Java changes needed):
   ```bash
   make hotspot CONF=macosx-aarch64-server-fastdebug
   ```

2. Run Runtime Truth suite and regenerate JSONL:
   ```bash
   SOROUSH_EXPORT_RUNTIME_TARGETS=1 SOROUSH_PROVENANCE_GRAPH=1 \
     java -cp /tmp/cases-build/classes testcases.TestCasesMain \
     > /tmp/rt_phase2b.jsonl 2>/dev/null
   ```

3. Run graph builder and check orphan count:
   ```bash
   python3 tools/rt-ui/graph_builder.py /tmp/rt_phase2b.jsonl --report --validate
   # Expected: runtime_target orphans < 30 (was 915)
   # Expected: CALLSITE_TARGET edges increase by ~880
   ```

4. Confirm new category records exist:
   ```bash
   python3 -c "
   import json
   cats = {}
   for line in open('/tmp/rt_phase2b.jsonl'):
       r = json.loads(line.strip())
       if r.get('record') == 'callsite_target':
           c = r.get('category','')
           if 'membername' in c:
               cats[c] = cats.get(c,0) + 1
   print(cats)
   "
   ```

5. Run Spring Boot and repeat.

6. Run all 14 unit tests (no changes expected):
   ```bash
   python3 tools/rt-ui/tests/test_graph_builder.py
   ```

**Blocked Phase 2B items** (require investigation before implementation):

- Addressing the remaining ~23/265 orphans from JVM-init MH linkage — these require a different hook point (class initialization machinery, possibly `ClassLoader::loadClass`) and are out of scope for this Phase.
- Attributing `direct_methodhandle` runtime_target records from `RESOLVES_TO` edges — none appear in current Phase 1 data, so there is no evidence-based design need.

---

## 11. Documentation Changes Required

The following documents contain incorrect information that this investigation corrected:

| Document | Incorrect claim | Correction |
|---|---|---|
| `06-known-limitations.md` Limitation #13 | "Add a vframeStream walk at the `runtime_target` emission sites in `linkResolver.cpp`" | Correct file is `methodHandles.cpp`; `linkResolver.cpp` has no `soroush_graph_linkage` calls |
| `00-agent-handoff.md` Task 2 | "Add a vframeStream walk at the `runtime_target` emission sites in `linkResolver.cpp`" | Same correction |
| `08-phase2-causality-graph-design-review.md` Phase 2b section | "In `linkResolver.cpp`..." | Same correction |

These are corrected in the respective documents. See update notes in each file.

---

## 12. Cross-References

- [src/hotspot/share/prims/methodHandles.cpp](../src/hotspot/share/prims/methodHandles.cpp) — emission sites (lines 260, 271, 849, 873)
- [src/hotspot/share/classfile/soroushProvenanceGraph.cpp](../src/hotspot/share/classfile/soroushProvenanceGraph.cpp) — `soroush_graph_linkage()` implementation; runtime_target export (Phase 4, line 2323)
- [src/hotspot/share/interpreter/linkResolver.cpp](../src/hotspot/share/interpreter/linkResolver.cpp) — existing vframeStream usage patterns (lines 3970, 4014)
- [tools/rt-ui/graph_builder.py](../tools/rt-ui/graph_builder.py) — no changes required
- [docs/06-known-limitations.md](06-known-limitations.md) — Limitation #13 (updated)
- [docs/08-phase2-causality-graph-design-review.md](08-phase2-causality-graph-design-review.md) — Phase 2b section (updated)
