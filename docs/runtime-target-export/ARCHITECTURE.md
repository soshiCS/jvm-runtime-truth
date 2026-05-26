# Architecture

## The four-layer model

```
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 1 — Hook sites                                               │
│  Where HotSpot fires at the right moment                            │
│                                                                     │
│  resolve_handle_call()     linkResolver.cpp      MH callsite        │
│  resolve_invokedynamic()   systemDictionary.cpp  indy bootstrap     │
│  MemberName.resolve()      methodHandles.cpp     reflection linkage │
│  KlassFactory::create()    klassFactory.cpp      class load         │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ raw HotSpot oops, frames, CP cache
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 2 — Capture functions                                        │
│  Transform raw HotSpot data into structured records                 │
│                                                                     │
│  sg_analyze_mh_receiver()      symbolic backward analysis           │
│  sg_recover_mh_recv_from_*()   live stack / local[0] recovery       │
│  sg_walk_mh()                  MH oop → shape classification        │
│  sg_walk_generic_bmh()         BMH → adapter node array             │
│  sg_extract_dmh_target()       DMH → SgMhTarget                     │
│  sg_emit_sibling_bcis()        scan for shared CP-cache siblings    │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ SgMhTarget, SgAdapterNode, SgMhWalkResult
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 3 — In-memory record tables                                  │
│  soroushProvenanceGraph.cpp                                         │
│                                                                     │
│  soroush_graph_generic_callsite()     → g_gen_buckets[] (dedup)     │
│  soroush_graph_target_set_callsite()  → g_ts_buckets[] (dedup)      │
│  soroush_graph_adapter_graph_callsite() → g_ag_buckets[] (dedup)    │
│  soroush_graph_generated_class()      → generated-class list        │
│  soroush_graph_bytecode_artifact()    → bytecode-artifact list      │
│  soroush_method_token_register()      → token registry              │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ at VM shutdown (before_exit)
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Layer 4 — JSONL export                                             │
│  soroush_graph_export_runtime_targets()  soroushProvenanceGraph.cpp │
│                                                                     │
│  Phase 1: method_identity records                                   │
│  Phase 2: callsite_target (invokedynamic)                           │
│  Phase 3: callsite_target / diagnostic (MH generic)                 │
│  Phase 3.5: callsite_target_set (GWT / GWC)                         │
│  Phase 3.6: callsite_adapter_graph                                  │
│  Phase 4: runtime_target, generated_class, bytecode_artifact        │
│  Final:   export_summary                                            │
└─────────────────────────────────────────────────────────────────────┘
```

---

## The central hook: `resolve_handle_call`

`resolve_handle_call` in `classfile/linkResolver.cpp` is the JVM's CP-cache
resolution function for `invokehandle` bytecodes (the internal rewrite of
`invokevirtual MethodHandle.invoke*`). It fires **once per CP-cache entry**, the
first time a given callsite is executed. This is the main hook for all MH
callsite attribution.

**Why here:** At this exact moment the JVM has the live interpreter stack, the
full frame chain, and the MH receiver oop — all stable and readable. After
resolution the CP-cache entry is populated and `resolve_handle_call` never fires
for this site again.

**What the system must determine at this hook:**

1. **Source attribution** — which user class, method, and BCI is invoking the MH?
   (The top frame may be a `java/lang/invoke/` LambdaForm internal, not the user.)

2. **MH receiver identity** — what is the actual MethodHandle oop being invoked?
   (The MH receiver is NOT available directly from the CP-cache entry or the
   appendix; it must be recovered from the interpreter stack or frame locals.)

3. **MH shape** — is the receiver a DirectMethodHandle (single exact target),
   a GuardWithTest/CatchException BMH (target set), or a generic BMH adapter chain?

These three questions are answered by the three-case framework documented in
[CALLSITE_ATTRIBUTION.md](CALLSITE_ATTRIBUTION.md).

---

## Key data structures

All defined in `src/hotspot/share/interpreter/linkResolver.cpp`:

### `SgMhTarget` (lines 2740–2746)

A resolved concrete Java method target extracted from a DMH:

```cpp
struct SgMhTarget {
  const char* klass;       // internal class name
  const char* method;      // method name
  const char* descriptor;  // JVM method descriptor
  uint64_t    loader_id;   // ClassLoaderData* as integer
  bool        valid;        // false → extraction failed
};
```

### `SgAdapterNode` (lines 2749–2760)

One node in a BMH adapter graph. Each node corresponds to one `argL` slot in a
BoundMethodHandle species:

```cpp
struct SgAdapterNode {
  int         id;
  const char* role;               // "primary_target", "secondary_component", etc.
  const char* from_desc;          // outer adapter type (type_conversion nodes)
  const char* to_desc;            // inner component type
  const char* node_adapter_class; // BMH class name if slot is itself a BMH
  bool        has_target;         // true → slot holds a DMH → SgMhTarget valid
  SgMhTarget  target;             // valid when has_target == true
  bool        exact;              // true → has_target && target.valid
  const char* exact_false_reason; // why exact is false
  const char* node_classification; // "user_target", "helper_boxing", etc.
};
```

### `SgMhWalkResult` (lines 2769–2799)

The result of `sg_walk_mh()` — classifies the MH receiver oop:

```cpp
struct SgMhWalkResult {
  enum Shape {
    DIRECT,        // DMH — single exact target
    GWT,           // guardWithTest BMH
    GWC,           // guardWithCatch (catchException) BMH
    ADAPTER_GRAPH, // generic BMH adapter — call sg_walk_generic_bmh
    BMH_UNKNOWN,   // BMH with unrecognized lf_kind
    MH_UNKNOWN     // not a DMH or known BMH
  };
  Shape      shape;
  SgMhTarget targets[4];    // valid for DIRECT (targets[0]) and GWT/GWC
  int        n_targets;
  SgAdapterNode graph_nodes[8]; // valid for ADAPTER_GRAPH
  int        n_graph_nodes;
  const char* adapter_class;
  const char* lf_kind;
  const char* exception_class; // GWC only
};
```

---

## Integration points (where hooks live)

| Hook | File | Line | Signal |
|------|------|------|--------|
| MH callsite — first CP-cache resolution | `classfile/linkResolver.cpp` | ~3445 | `resolve_handle_call` |
| invokedynamic — bootstrap | `classfile/systemDictionary.cpp` | ~2556 | bootstrap site |
| Reflection linkage — MemberName.resolve | `prims/methodHandles.cpp` | ~260,271,849,873 | `soroush_trace_membername_resolution` |
| Class load — generated class recovery | `classfile/klassFactory.cpp` | ~600 | `recover_runtime_generated_class` |
| Class load — bytecode artifact capture | `classfile/klassFactory.cpp` | ~935 | bytecode capture block |
| VM shutdown — JSONL export | `runtime/java.cpp` | ~470 | `before_exit` |

---

## Dedup model

All three callsite record tables (g_gen_buckets, g_ts_buckets, g_ag_buckets) dedup
by `(source_class, source_method, source_descriptor, source_bci)`. The `category`
field (invokeExact vs invoke) is excluded from the dedup key — a single BCI can
only have one callsite record regardless of which invoke variant fires first.

**Prefer-exact upgrade:** If a diagnostic record is stored for a given BCI and a
later resolution of the same CP-cache entry produces an exact result, the table
entry is upgraded in place to the exact record. This handles the case where the
first-ever resolution misses the MH receiver (e.g., JVM internal initialization)
but a later resolution from user code has the live receiver.

---

## Exact-or-diagnostic invariant

Every MH callsite in the running program produces exactly one of:
- `callsite_target` — exact single target
- `callsite_target_set` — exact multi-target (GWT/GWC)
- `callsite_adapter_graph` — structural adapter extraction (may have `exact=false` nodes)
- `diagnostic` — explicit explanation of why the site cannot be represented exactly

There are no silent omissions. The sibling BCI scan (`sg_emit_sibling_bcis`,
`linkResolver.cpp:2634`) additionally scans for invoke bytecodes that share a CP
cache entry with the primary site and emits diagnostics for those too.

---

## Env-var gate summary

| Env var | Effect |
|---------|--------|
| `SOROUSH_PROVENANCE_GRAPH=1` | Enable all graph and callsite record accumulation |
| `SOROUSH_EXPORT_RUNTIME_TARGETS=<path>` | Write JSONL to path at shutdown |
| `SOROUSH_RUNTIME_RECOVERY=1` | Enable generated-class recovery + sidecar dump |
| `SOROUSH_CAPTURE_FINAL_BYTECODE=1` | Enable post-rewrite bytecode artifact capture |
| `SOROUSH_TRACE_INDY=1` | Enrich indy bootstrap records (BSM, LMF fields) |
| `SOROUSH_RUNTIME_RECOVERY=1` | Enable reflection accessor tracing |
| `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1` | Enable ENTER/EXIT bytecode instrumentation |
| `SOROUSH_REWRITER_PHASE5_PREFIX=<prefix>` | Class prefix for Phase 5 rewriter (slash form) |

All flags are read at JVM startup. Setting `SOROUSH_PROVENANCE_GRAPH=0` or leaving
it unset disables the entire capture system with zero overhead.
