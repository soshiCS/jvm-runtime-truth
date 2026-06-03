# Production-Readiness Audit

**Date:** 2026-05-25
**Scope:** Runtime target export / rt staticization branch
**Standard:** Every executed dynamic callsite must produce either an exact record
or an explicit diagnostic. Zero silent omissions.

---

## Executive Summary

The branch meets its correctness standard for the cases it exercises in the
current demo suite. However, **three classes of production gap exist** that can
produce misleading success: a true silent omission path on OOM, unchecked I/O
that produces a partial-but-complete-looking JSONL file, and a class-name filter
that silently drops runtime-generated classes that don't match known patterns.
All other findings are either bounded/acceptable or configurable. The system is
not dishonest under normal conditions, but it cannot tell you when it is failing
under adverse conditions.

---

## Category 1 — Hardcoded Limits

### 1.1 Graph node / edge capacity
**File:** `soroushProvenanceGraph.cpp:17–18`
```cpp
static const long SG_MAX_NODES = 1000000;
static const long SG_MAX_EDGES = 2000000;
```
**When exceeded:** Node capacity prints one line to stderr (`[JVM GRAPH NOTE] node capacity reached`) and silently drops all subsequent nodes. Edge capacity silently drops edges with no log at all. Neither appears in the JSONL export.

**Impact:** A large application run could hit these caps. Dropped class/method nodes mean those classes have no generated-class or callsite records in the export. **There is no indicator in the JSONL file of how many records were dropped.**

**Verdict:** C — bounded and intentional, but unquantifiable in the JSONL output.

---

### 1.2 InvokeDynamic site table
**File:** `soroushProvenanceGraph.cpp:122`
```cpp
static const uint32_t SG_INDY_SITE_MAX = 1u << 20; // 1M indy sites
```
**When exceeded:** Logs once (`[JVM CALLSITE] indy site table full`) then every subsequent indy bootstrap is silently untracked. No JSONL diagnostic.

**Verdict:** C — one-time log, then completely invisible.

---

### 1.3 Method token registry
**File:** `soroushProvenanceGraph.cpp:84`
```cpp
static const uint32_t SG_TOKEN_MAX = 8u * 1024u * 1024u; // 8M methods
```
**When exceeded:** Logs once (`[JVM TRACE METHOD] token registry full`) then returns token `0`. Callers receiving `0` may emit `identity-unresolved` log lines, but no JSONL diagnostic is produced. Subsequent overflows are completely silent.

**Verdict:** C — one-time log, then invisible.

---

### 1.4 Adapter graph per-callsite cap
**Files:**
- `linkResolver.cpp:2792` — `SgAdapterNode graph_nodes[16]`
- `linkResolver.cpp:2985` — `if (r->n_graph_nodes >= 16)` → stops adding nodes
- `soroushProvenanceGraph.cpp:201–202` — `SG_AG_MAX_NODES 16`, `SG_AG_MAX_EDGES 16`
- `linkResolver.cpp:3795` — `int ne = (walk.n_graph_nodes < 16) ? walk.n_graph_nodes : 16`

**Behavior:** The walker stops collecting nodes at 16. The call site clamps to 16 before calling the storage function. Nodes beyond 16 are silently dropped from the graph; they never appear in the JSONL as either nodes or diagnostics. The `all_exact` flag remains whatever it was at the 16th node — it does NOT reflect the unread nodes.

**In practice:** No observed adapter with >16 nodes in the current demo suite. `filterArguments` with 15 filters would hit this. Real-world Spring framework usage is unlikely to exceed 16, but there is no safety net.

**Verdict:** B — acceptable for current workloads, but a production adapter with >16 slots silently produces an incomplete record with no indication of truncation.

---

### 1.5 Target-set callsite max 4 targets
**File:** `soroushProvenanceGraph.cpp:185` — `SgTsTarget targets[4]`

**Behavior:** `guardWithTest` has exactly 3 targets; `catchException` has exactly 2. The limit of 4 is never reached by any standard combinator. Silent truncation would only occur with a hypothetical custom combinator exposing 5+ targets. Not a current concern.

**Verdict:** A — safe for all known combinators.

---

### 1.6 Hash bucket counts
**File:** `soroushProvenanceGraph.cpp:152,190,200`
```cpp
#define SG_GEN_BUCKETS 512u
#define SG_TS_BUCKETS  512u
#define SG_AG_BUCKETS  256u
```
**Behavior:** Hash chains are unbounded in length. No overflow. High collision rates degrade insertion performance (O(n) chain walk) but do not drop data. At 512 buckets for up to ~10K callsites, average chain length stays well under 20.

**Verdict:** A — safe.

---

### 1.7 Loader-divergence name table
**File:** `soroushProvenanceGraph.cpp:62`
```cpp
static const long SG_NAME_TABLE = 1 << 16; // 65536 slots
```
This is a direct-mapped table for tracking which classes were seen under a second loader. Hash collisions overwrite silently (the code comment at line 940 explicitly acknowledges this). These are observational `NOTE` diagnostics only, not callsite records. Missing a NOTE does not mean a callsite record is missing.

**Verdict:** A — observational only, documented as best-effort.

---

## Category 2 — Silent Omission Risks

### 2.1 ⚠️ TRUE SILENT OMISSION: adapter graph callsite OOM → no record, no diagnostic

**File:** `linkResolver.cpp:3820–3826`
```cpp
soroush_graph_adapter_graph_callsite(
    cat, ..., node_entries, ne, ...);   // return value DISCARDED
multi_target_emitted = true;            // set unconditionally
```

**The problem:** `soroush_graph_adapter_graph_callsite` returns `false` on OOM
(`malloc` at line 645 fails). The return value is not checked. `multi_target_emitted`
is set to `true` unconditionally. The fallback path at line 3914 (`if (!multi_target_emitted)`)
therefore does NOT fire. The callsite has no record in the JSONL and no diagnostic
record. **This is a true silent omission — neither exact nor diagnostic.**

The same pattern applies to `soroush_graph_target_set_callsite` (line 3790):
```cpp
soroush_graph_target_set_callsite(...);   // return value DISCARDED
multi_target_emitted = true;              // set unconditionally
```

**Frequency in practice:** Only fires if `malloc` fails for a ~300-byte struct.
Unlikely under normal conditions. Under memory pressure (large app, `-Xint` has
higher memory overhead) this becomes possible.

**Verdict: E — must fix.** The return value of both calls must be checked. If the
storage call fails, `multi_target_emitted` must NOT be set, allowing the fallback
to emit a diagnostic.

---

### 2.2 ⚠️ Runtime-generated class filter: non-matching names silently not recorded

**File:** `klassFactory.cpp:234–241`
```cpp
static bool should_dump_soroush_class(bool is_hidden, const char* class_name) {
    if (is_hidden) return true;
    if (class_name == nullptr) return false;
    return strstr(class_name, "$$Lambda") != nullptr ||
           strstr(class_name, "$Proxy")   != nullptr ||
           strstr(class_name, "CGLIB")    != nullptr ||
           strstr(class_name, "ByteBuddy") != nullptr;
}
```

If `recover_runtime_generated_class` is only called when `should_dump_soroush_class`
returns `true`, then any runtime-defined class that does not match these patterns
and is not a hidden class is silently not captured: no `generated_class` record,
no disk dump, no diagnostic. This covers:
- Custom dynamic class generation via `Unsafe.defineClass` with non-standard names
- Framework-generated classes with naming conventions not listed above
- JVM agent-generated classes

**How common is this in production?** Spring Boot uses CGLIB/ByteBuddy for proxies
(covered). But custom annotation processors or code generation frameworks (Quarkus,
Micronaut, MapStruct) generate classes at build time rather than runtime, so they
load from JAR and are captured as `bytecode_artifact`. Truly runtime-generated
classes that miss the filter are less common but not zero.

**Verdict:** D/E — needs explicit visibility. At minimum, the filter logic and its
implications should be documented at the capture site. Ideally: emit a diagnostic
for classes that are not from a known classloader source but weren't dumped.

---

### 2.3 malloc OOM in all graph functions returns false silently

**File:** `soroushProvenanceGraph.cpp:476, 562, 645` and ~20 other locations

All three callsite record storage functions return `false` on OOM. As documented
in 2.1, the callers in `linkResolver.cpp` do not check these return values.
Beyond the silent-omission issue in 2.1, any `strdup` failure inside the storage
functions partially initializes the stored record with null fields. These null
fields reach the JSONL writer, which calls `sg_json_str(f, nullptr)` → writes
`null`. This produces valid JSON with `null` values in place of class/method names
— a record that looks structurally valid but is semantically empty.

**Verdict:** D — the record is emitted with `null` fields rather than a diagnostic.
Under OOM, the JSONL will contain callsite_target records with `"target_class":null`
which looks like a legitimate record but carries no information.

---

### 2.4 Export lock-free walk vs concurrent insertions

**File:** `soroushProvenanceGraph.cpp:1752–1757`
```cpp
// Snapshot count under lock; walk buckets lock-free at shutdown.
pthread_mutex_lock(&g_gen_lock);
uint32_t gen_snap = g_gen_count;
pthread_mutex_unlock(&g_gen_lock);

// ... then walks g_gen_buckets[0..SG_GEN_BUCKETS-1] without holding the lock
```

The export takes a count snapshot under lock, releases, then walks the buckets
lock-free. Any `soroush_graph_generic_callsite` call that fires AFTER the lock
release and writes to a bucket that the walker has already passed will be excluded
from the export. Any write to a bucket the walker hasn't reached yet will be
included.

**Java thread quiescence at before_exit:** HotSpot's `before_exit` does NOT
guarantee Java threads are stopped before the custom export call. The export fires
early in `before_exit` (line ~470), before `JavaThreads::destroy_java_vm_thread`
or equivalent quiescence is established. In practice with `-Xint`, if the main
thread has exited and no other threads are running MH code, the race doesn't fire.
But a multi-threaded application with live executor threads at shutdown time could
trigger it.

**Verdict:** C — real race condition, bounded impact (late insertions may be
missed), not a crash risk. Acceptable for single-threaded slices; not safe for
multi-threaded production workloads.

---

### 2.5 Graph node capacity drop: no JSONL visibility

As noted in 1.1: when `g_node_count >= SG_MAX_NODES`, node additions silently
return 0. All subsequent class, method, and callsite graph nodes use node-id 0,
which means their LINKS_TO / GENERATES / EXECUTES edges go to node 0 (the null
node). The export emits records for callsites that were stored BEFORE the cap was
hit, but any callsite that requires a new class node after the cap produces a
record with a null or zero `loader_id` on the target.

**Verdict:** C — data degrades silently after the cap.

---

## Category 3 — Filtering and Noise Suppression

### 3.1 java/lang/invoke/ frame skipping in Case A2

**File:** `linkResolver.cpp:3560–3645`
```cpp
} else if (strncmp(holder, "java/lang/invoke/", 17) == 0) {
    is_lf_dispatch_case = true;
    // walk vframeStream past all java/lang/invoke/ frames
```

Classes in `java/lang/invoke/` are unconditionally treated as LF dispatch
infrastructure and skipped during source frame recovery. This is correct for all
standard JDK MH classes. It would be wrong if a user class were named with this
prefix (impossible — the package is protected by the JDK), or if a third-party
framework placed its dispatch logic in this prefix (also impossible by Java
security model).

**Verdict:** A — safe, package is sealed.

---

### 3.2 jdk/internal/reflect/ for Case B detection

**File:** `linkResolver.cpp:3520`

The accessor frame detection uses exact class name matching for
`DirectMethodHandleAccessor` and `DirectConstructorHandleAccessor`. Other class
names in `jdk/internal/reflect/` that call `invokehandle` would be treated as
Case A (user frame), not Case B. This is correct — only those two accessor classes
need Case B treatment.

**Verdict:** A — safe.

---

### 3.3 Phase 5 rewriter prefix filter

Classes whose names don't match `SOROUSH_REWRITER_PHASE5_PREFIX` are not
instrumented. They produce no `method_identity` records and no ENTER/EXIT
execution trace. This is a deliberate scope control, not a correctness gap —
non-instrumented classes still produce MH callsite records via `resolve_handle_call`.

**Verdict:** A — intentional, well-documented.

---

### 3.4 Dedup first-in-wins for target_set and adapter_graph

`soroush_graph_target_set_callsite` and `soroush_graph_adapter_graph_callsite`
are first-in-wins with no upgrade path. If the first resolution of a GWT/GWC
callsite fires from a JVM-internal warmup context (where the MH receiver has wrong
shape), the stored record reflects that incorrect first resolution and is never
corrected. There is no prefer-exact upgrade for these two record types, unlike
`soroush_graph_generic_callsite`.

**Impact:** Only `soroush_graph_generic_callsite` has the prefer-exact upgrade.
If a GWT callsite's first resolution fires before the actual runtime MH is
installed (unusual, but possible in framework initialization), the stored targets
could be wrong with no diagnostic and no correction.

**Verdict:** C — correctness risk for early-resolution GWT/GWC callsites. The
generic callsite has the right behavior (upgrade); the other two don't.

---

### 3.5 Sibling BCI scan only fires after a successful primary site

`sg_emit_sibling_bcis` is called from `resolve_handle_call` after the primary
record is stored. If the primary site is never executed (the code path that reaches
that BCI never runs in the slice), neither the primary site nor any siblings are
captured. This is correct behavior — unexecuted sites should not be in the export.

However: if the primary BCI was already resolved (CP-cache pre-populated by JVM
initialization or by a different `invoke` variant), `resolve_handle_call` doesn't
fire, the sibling scan doesn't run, and any other BCIs that share the same CP-cache
entry in the same method are silently not captured.

**Verdict:** B — bounded by the single-run, first-resolution model. The same
limitation affects the primary site. Acceptable for the stated use case.

---

## Category 4 — Exactness Discipline

### 4.1 `sg_extract_dmh_target` — strict and correct

**File:** `linkResolver.cpp:2877–2890`

Every exact=true path goes through `sg_extract_dmh_target`. The function:
1. Validates the MH oop with `sg_oop_valid`
2. Checks `DirectMethodHandle::is_instance`
3. Reads `member` field (MemberName oop) with null check + `is_instance` check
4. Reads `vmtarget` with null check
5. Reads all fields from the validated `Method*`

All four checks must pass before `valid = true` is set. There is no path where
`exact = true` is set without going through a validated `Method*`.

**Verdict:** A — provably correct.

---

### 4.2 Loader identity — always from ClassLoaderData*

Every `loader_id` field in every callsite record is set as:
```cpp
(uint64_t)(uintptr_t)method->method_holder()->class_loader_data()
```
No inference, no name-based lookup, no fallback to zero. The bootstrap classloader
has a non-null `ClassLoaderData*`.

**Verdict:** A — provably correct.

---

### 4.3 Step 2 stack recovery — correct object, but not verified as THE callsite MH

**File:** `linkResolver.cpp:3255–3275`

After `sg_recover_mh_recv_from_java_sp` returns a candidate:
1. `sg_oop_valid` → verifies it's a live object with valid header
2. `is_instance(candidate)` → verifies it's a MethodHandle

These checks guarantee the object is a real, live MethodHandle. They do NOT
guarantee it is the specific MH being dispatched at this callsite (vs. some other
MH that happens to be at `sp + arg_slots`). The correctness depends on:
- `arg_slots` being computed correctly from the CP cache method type ✓
- The Java expression stack being in the expected layout at the time of the
  `invokehandle` slow path call ✓ (documented JVM invariant under -Xint)
- No other MethodHandle sitting at exactly `sp + arg_slots` ✓ (the expression
  stack layout for `invokevirtual/invokehandle` is deterministic: receiver is at
  exactly the position above the arguments)

**Assessment:** The correctness argument is sound under `-Xint`. The stack layout
is deterministic and specified by the JVM spec. Under compiled mode (not `-Xint`),
this would be wrong.

**Verdict:** A under `-Xint`. D if ever run without `-Xint`.

---

### 4.4 Case B reflection: "first has_target node" assumption

**File:** `linkResolver.cpp:3894–3903`
```cpp
for (int ni = 0; ni < walk.n_graph_nodes && !tgt_ok; ni++) {
    const SgAdapterNode& gn = walk.graph_nodes[ni];
    if (gn.has_target && gn.target.valid) {
        tgt_class  = gn.target.klass;
        ...
    }
}
```

For reflection case B, the code takes the FIRST node in the adapter graph that
has `has_target=true` and assumes it is the reflected method. This assumes
`sg_walk_generic_bmh` traverses the BMH species fields in an order where the
innermost DMH (the actual reflected method) is the first valid target node.

**Actual BMH structure for JDK21 reflection:**
```
accessor.target = BoundMethodHandle$Species_L:
  argL0 = DirectMethodHandle (the actual reflected method)
  [no further L slots typically]
```

The `dropArguments` wrapper is an outer BMH with `argL0` holding the inner DMH.
`sg_walk_generic_bmh` processes argL slots in order, so argL0 (the innermost DMH)
is node 0. This is correct for the current JDK21 implementation.

**Risk:** If a future JDK version changes the adapter wrapping order such that
`argL0` is NOT the reflected method DMH, the first `has_target=true` node would
be wrong. There is no verification against the method's expected name or signature.

**Verdict:** B — correct for JDK21, fragile against future JDK changes.

---

### 4.5 `interpreter_frame_bci()` minus-6 correction

**File:** `linkResolver.cpp` — `sg_analyze_mh_receiver` call

The symbolic analyzer is called with `invoke_bci` which has had `6` subtracted
from `interpreter_frame_bci()`. This correction assumes `invokehandle` is always
6 bytes. This is true for HotSpot's current implementation (2-byte opcode + 2-byte
CP index + 2-byte argument count). If HotSpot ever changes `invokehandle` to a
different width, this correction would silently produce wrong BCIs in two ways:
- Wrong backward-analysis start position → wrong local slot identified
- Wrong exported `source_bci` → callsite cannot be mapped to bytecode

**Verdict:** B — correct for current HotSpot, hardcoded assumption.

---

## Category 5 — Diagnostics Audit

### 5.1 ⚠️ Export I/O failures never checked

**File:** `soroushProvenanceGraph.cpp` — 131 `fprintf(f, ...)` calls, zero checked

If the export file fills a disk partition mid-write, all subsequent `fprintf`
calls fail silently. The export loop continues, the count variables still
increment, and the `export_summary` is written with the counts that WOULD have
been correct if all writes had succeeded.

**Result:** A consumer receives a JSONL file that:
- Has a valid, parseable `export_summary` at the end
- Shows e.g. `"callsite_target_count": 37`
- Actually contains only 12 callsite_target records (because writes stopped at record 12)
- Has no indication of the failure

**The `export_summary` count thus actively lies** when I/O fails. A consumer
cannot distinguish "export complete" from "export failed at record 12."

**Verdict: E — must fix.** At minimum: check `ferror(f)` before writing the
summary and emit a different summary record (`"complete": false, "error": "io_error"`)
if writes failed. Or track written count vs. intended count separately.

---

### 5.2 fopen failure — export_summary NOT written

**File:** `soroushProvenanceGraph.cpp:1560–1564`
```cpp
if (f == nullptr) {
    fprintf(stderr, "[JVM EXPORT] error: cannot open...");
    return;   // ← export_summary not written
}
```

If the file cannot be opened, the function returns immediately with no
`export_summary`. A consumer looking for `export_summary` at the end of the file
would not find it and would correctly conclude the export failed.

**This is the GOOD failure mode** — it is detectable. In contrast, mid-write
I/O failure (5.1) is the bad mode.

**Verdict:** A — fopen failure is correctly visible.

---

### 5.3 Capacity overflow log fires once, then silent

Nodes, indy sites, and token registry all log their first overflow to stderr.
Subsequent overflows are completely silent. There is no counter exported in the
JSONL summary. A consumer looking at the export cannot determine whether capacity
limits were hit.

**The export_summary does not include an overflow or data-loss indicator.**

**Verdict:** C — the summary should include `"data_loss_detected": true` or an
overflow count when any capacity limit was hit.

---

### 5.4 `soroush_graph_generic_callsite` return value not checked at call site

**File:** `linkResolver.cpp:3932`
```cpp
soroush_graph_generic_callsite(cat,
    src_class, src_loader, src_method, src_desc,
    src_bci, src_opcode, src_cp,
    ...);
// return value discarded
```

If the function returns false (OOM malloc at line 476), the callsite has no
record. For the generic callsite this is the FALLBACK path (called when no
multi-target record was stored), so OOM here means both the original record
attempt and the fallback diagnostic are lost. True silent omission.

**Verdict: E** — same root cause as 2.1. Part of the same fix.

---

## Category 6 — Validation Blind Spots

### 6.1 No test for adapter graph with >16 nodes

The current demo suite has a maximum of 3 nodes in any adapter graph. An adapter
with 17+ BMH species slots (`filterArguments` with 16 filters) would trigger
silent truncation with no test coverage. In practice this is pathological Java
code, but it's an untested path.

---

### 6.2 Multi-threaded shutdown not tested

All showcase demos are single-threaded. The shutdown export race (category 2.4)
has never been exercised. A multi-threaded workload with executor threads still
running MH callsites at VM exit is untested.

---

### 6.3 Disk-full export never tested

The 131 unchecked `fprintf` calls (category 5.1) have never been tested under
I/O failure conditions. The "export succeeds with partial data and misleading
summary" scenario is untested.

---

### 6.4 Non-standard generated class names never tested

The `should_dump_soroush_class` filter (category 2.2) has never been tested
with a custom `Unsafe.defineClass` invocation that uses a non-standard class name.
The current demos only test `LambdaMetafactory` and `ProxyGenerator` paths.

---

### 6.5 Reflection to static method never separately validated

All reflection showcase tests use non-static methods or constructors. Static method
reflection (`Method.invoke(null, args)`) uses a different accessor variant. While
the `sg_walk_mh` path handles it, it hasn't been explicitly tested in the showcase.

---

## Category 7 — Runtime Assumptions

| Assumption | What breaks if violated |
|------------|------------------------|
| `-Xint` (interpreter-only) | Step 2 stack recovery reads wrong memory; JIT-compiled frames produce zero callsite records; no diagnostic emitted for missed compiled-frame callsites |
| Java threads quiescent at before_exit time | Export-vs-insert race (category 2.4): late insertions may be missing from export without any indication |
| `invokehandle` is always 6 bytes | Wrong `source_bci` in all callsite records; wrong backward analysis start position; silent wrong attribution |
| `arg_slots` from CP cache is correct | Step 2 reads MH from wrong stack position; passes type check (it IS a MH) but is the wrong MH; exact=true record points to wrong target |
| JDK21 BMH structure: argL0 is always the innermost DMH | Case B reflection target extraction returns wrong method; exact=true but wrong target |
| Single-run snapshot model | Class redefinition after the first `resolve_handle_call` is not tracked; the stored record reflects the original target, not the redefined one |

**Violating `-Xint` is the most dangerous:** it causes incorrect records (wrong
MH received from wrong stack position) with no diagnostic, appearing as valid
exact records.

---

## Category 8 — "Probably Okay" Assumptions That Are Not Verified

### 8.1 `is_oop_or_null` safety on arbitrary stack memory

`sg_oop_valid` calls `oopDesc::is_oop_or_null(o)` on a pointer read from the
interpreter stack. This function checks the object header for GC mark bits and
klass pointer validity. It is safe against garbage values (returns false, no crash).
However, it does NOT guarantee the object hasn't been moved by GC between the
stack read and the header access. Under `-Xint` with the interpreter's safepoint
model, GC should not move objects during the `call_VM` for `resolve_handle_call`.
This is a JVM-internal invariant that is not explicitly verified at the call site.

---

### 8.2 `as_C_string()` lifetime

`sg_extract_dmh_target` stores:
```cpp
out->klass      = tm->method_holder()->name()->as_C_string();
out->method     = tm->name()->as_C_string();
out->descriptor = tm->signature()->as_C_string();
```

`as_C_string()` on HotSpot's `Symbol*` returns a `ResourceMark`-scoped C string.
The `SgMhTarget` struct holds raw `const char*` pointers to these resource strings.
These pointers are valid only within the current `ResourceMark` scope. The pointers
are passed immediately to `soroush_graph_generic_callsite` which calls `sg_strdup`
on them — so they are copied before the ResourceMark is released.

**Assessment:** Safe because `sg_strdup` is called before the scope exits. But
this is fragile: any future refactor that delays the `soroush_graph_*` call past
the `ResourceMark` scope would cause a use-after-free with no compiler warning.

---

### 8.3 `SgAdapterNode::klass/method/descriptor` are resource strings

Same issue as 8.2: nodes inside `SgMhWalkResult::graph_nodes` hold raw pointers
from `as_C_string()`. The call at line 3795 copies from `walk.graph_nodes[i]` to
`node_entries[i]` and immediately calls `soroush_graph_adapter_graph_callsite`
which calls `sg_strdup`. Safe in the current code, fragile to refactoring.

---

## Category 9 — Production-Readiness Verdict

### Classification

| # | Finding | Class | Priority |
|---|---------|-------|----------|
| 2.1 | OOM in callsite record storage → multi_target_emitted=true → true silent omission | **E** | Must fix |
| 5.1 | Export I/O failures unchecked → partial file with lying summary | **E** | Must fix |
| 5.4 | `soroush_graph_generic_callsite` return not checked in fallback | **E** | Must fix (same fix as 2.1) |
| 2.2 | Non-matching generated class name → silent omission, no diagnostic | **D/E** | Should fix |
| 1.1 | Graph node/edge capacity: one-time log, no JSONL visibility | **C** | Configurable, add summary field |
| 1.2 | InvokeDynamic site cap: one-time log, no JSONL visibility | **C** | Add summary field |
| 1.3 | Token registry cap: one-time log, no JSONL visibility | **C** | Add summary field |
| 2.4 | Export lock-free walk vs concurrent insertions | **C** | Acceptable for -Xint single-slice use; document |
| 3.4 | First-in-wins for GWT/GWC (no upgrade path) | **C** | Low risk for real workloads; add upgrade path |
| 1.4 | 16-node adapter graph: silent truncation | **B** | Document; acceptable for current workloads |
| 4.4 | Case B first-has_target assumption: fragile against JDK changes | **B** | Document; correct for JDK21 |
| 4.5 | invokehandle 6-byte BCI assumption: hardcoded | **B** | Document |
| 8.2/8.3 | ResourceMark-scoped string pointers in SgMhTarget/SgAdapterNode | **B** | Correct now, fragile to refactor |
| 5.2 | fopen failure returns early without summary | **A** | Correct failure mode |
| 4.1 | sg_extract_dmh_target: strict and verified | **A** | No action needed |
| 4.2 | Loader identity: always from CLD* | **A** | No action needed |
| JSON escaping (sg_json_str) | Correctly implemented | **A** | No action needed |
| exact=false nodes | Correctly documented | **A** | No action needed |

---

### Direct answers

**Can this branch currently fail silently for executed dynamic runtime paths?**

**Yes, in three ways:**

1. **OOM on callsite record storage** — if `malloc` fails while storing an adapter
   graph, target-set, or fallback generic callsite record, the callsite has no JSONL
   record and no diagnostic. This is a true silent omission. It requires memory
   pressure but is not impossible in production.

2. **Export I/O failure** — if the export filesystem runs out of space mid-export,
   the JSONL file is truncated but the `export_summary` at the end reports the
   intended counts (not the actual written counts). A consumer cannot detect the
   failure from the file alone.

3. **Non-matching generated class** — a `Unsafe.defineClass` or similar call with
   a class name that doesn't match the `should_dump_soroush_class` patterns is
   silently not captured. No `generated_class` record, no diagnostic.

**Are there places where the current demos could misleadingly appear successful while runtime truth is actually missing?**

**Yes:**

- The current demo suite never generates more than ~15 adapter graph nodes, never
  runs out of graph capacity, and never creates runtime-generated classes with
  non-standard names. It therefore never exercises any of the three silent omission
  paths above.

- The showcase script validates presence of expected records. It does NOT detect
  missing records for paths it doesn't enumerate. A callsite that produces no
  record AND no diagnostic passes the showcase script undetected.

- The export I/O is never tested under failure conditions. The showcase always runs
  on a filesystem with ample space.

- Demo success is a necessary but not sufficient condition for production correctness.
  The three gaps above are real and untested.

---

### Minimum required fixes before trusting production workloads

1. **Check return values of all three callsite record calls in `resolve_handle_call`.**
   If any returns false, do NOT set `multi_target_emitted = true`. Let the fallback
   path emit a diagnostic with reason `"record_storage_oom"`. Three lines of change.

2. **Track write failures in the export.** Use `ferror(f)` at the summary write, or
   track a `bool write_failed` flag set if any write call returns `< 0`. If
   `write_failed`, emit `"complete":false` in the summary instead of the counts.

3. **Add overflow indicators to export_summary.** Add boolean fields
   `"node_cap_hit"`, `"edge_cap_hit"`, `"token_cap_hit"`, `"indy_cap_hit"` (all
   false by default, set to true if the one-time log was fired). This makes data
   loss visible in the JSONL output without consumer knowledge of stderr logs.

4. **Document or fix the generated class name filter.** Either emit a diagnostic
   for unmatched runtime class definitions (requires defining "runtime" more
   precisely) or document explicitly which class patterns are not captured and why.
