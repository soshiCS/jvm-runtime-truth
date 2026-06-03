# Phase 2 Design Review: Runtime Causality Graph

**Date**: 2026-05-29 (review) / 2026-05-30 (Phase 2a + 2b implementation)  
**Status**: Phase 2a COMPLETE. Phase 2b COMPLETE. Phase 2c–d: not started.  
**Scope**: Determine whether a Runtime Causality Graph MVP can be built from the existing Phase 1 JSONL export, which gaps exist, and what the Phase 2 roadmap should be.

---

## Phase 2a Implementation Status (2026-05-30)

**COMPLETE.** `tools/rt-ui/graph_builder.py` was implemented and validated.

| Item | Status |
|---|---|
| `graph_builder.py` offline builder | ✓ Implemented |
| 14 synthetic fixture tests | ✓ All passing |
| 12-case validation | ✓ 10/10 validation checks pass |
| Spring Boot validation | ✓ 10/10 validation checks pass |
| Heuristic edges | ✓ 0 (hard rule enforced) |
| Gap report | ✓ Produced on every run |
| Query API | ✓ chain / orphans / blocked / staticizable |

**Spring Boot results (2026-05-30):**
- 27,826 nodes, 20,790 edges, 2,906 gap records
- 3,280 CALLSITE_TARGET edges + 869 LAMBDA_BODY edges
- 630/630 adapter graphs connected
- 1/1 target sets connected
- 1,381/1,381 hidden class identities → bytecode artifacts (100%)
- 2,905 runtime_target orphans (Phase 2b will fix)
- 1,024 `staticizable_candidate_direct` callsites
- 6 `staticizable_candidate_adapter_modeled` callsites
- 3,290 `observed_only_not_proven` callsites

**Runtime Truth results (2026-05-30):**
- 6,204 nodes, 4,974 edges, 916 gap records
- 38 CALLSITE_TARGET edges + 54 LAMBDA_BODY edges
- 96/96 adapter graphs connected
- 3/3 target sets connected
- 529/529 hidden identities linked (100%)
- 915 runtime_target orphans

**Schema discovery (invokedynamic records):**
`callsite_target[invokedynamic]` records have NO `target_class`/`target_method` fields. The implementation is in `lmf_impl_class`/`lmf_impl_method`. Graph produces `LAMBDA_BODY` edges (not `CALLSITE_TARGET`) for invokedynamic callsites. This is correct behavior, not a gap.

**Run graph builder:**
```bash
# Tests
python3 tools/rt-ui/tests/test_graph_builder.py

# Runtime Truth
python3 tools/rt-ui/graph_builder.py /tmp/rt_val.jsonl --report --validate

# Spring Boot
python3 tools/rt-ui/graph_builder.py /tmp/spring_out.jsonl --report --validate
```

See [05-validation-guide.md Part 4](05-validation-guide.md) for full validation commands and expected output.  
See [03-source-ownership-map.md → Offline Causality Graph Builder](03-source-ownership-map.md) for the full API reference.

---

## Executive Summary

The Phase 1 JSONL export is sufficient to build a Runtime Causality Graph MVP covering all user-code dispatch chains. No JVM modifications are required. The graph is necessarily a static structure — ordered by callsite resolution, not by invocation frequency or wall-clock time — but it is complete for all user-code call sites exercised in the test workloads.

The single most important finding of this review is that the Phase 1 schema has one significant structural gap: **2,905 `runtime_target` records have no source BCI**. These represent JVM-internal method resolution events (all with `caller_context="MemberName.resolve"`) that cannot be connected to any specific user callsite without additional instrumentation. Every other record type provides full causality edges.

A second finding: the documentation in `06-known-limitations.md` overstates Limitation #1. The `reflection_method_invoke` and `reflection_constructor_newInstance` categories DO exist in the export and DO provide fully attributed callsite records (source_class, source_method, source_bci). These were not present in earlier test runs but appeared in the Spring Boot validation output and are confirmed by the source in `linkResolver.cpp` lines 3966–3967. The documentation has been corrected.

**Recommendation**: Proceed with Phase 2a (offline graph builder from Phase 1 data) before any JVM changes. The MVP will surface the runtime_target gap concretely and justify Phase 2b prioritization.

---

## 1. Record Type Analysis: Causality Value

### 1.1 `callsite_target` — PRIMARY GRAPH EDGE SOURCE

**Spring Boot count**: 4,322 records  
**Causality role**: Directed call edges from source callsites to resolved target methods.

Each record encodes: source_class, source_method, source_bci, source_opcode, source_descriptor, source_loader_id, and the corresponding target fields. This is a complete, unambiguous edge in the causality graph.

Six categories, each with its own dispatch semantics:

| Category | Count | Source attribution | Unique fields | Causality completeness |
|---|---|---|---|---|
| `invokedynamic` | 1,042 | Full | `lmf_impl_class`, `lmf_impl_method`, `lmf_impl_descriptor`, `indy_name`, `bootstrap_method`, `trace_id` | Complete + lambda body shortcut |
| `invokeinterface` | 3,260 | Full | — | Complete |
| `methodhandle_invoke` | 5 | Full | — | Complete |
| `methodhandle_invokeExact` | 5 | Full | — | Complete |
| `reflection_method_invoke` | 7 | Full (source_opcode=invokevirtual) | — | Complete |
| `reflection_constructor_newInstance` | 3 | Full (source_opcode=invokevirtual) | — | Complete |

**`lmf_impl_class` / `lmf_impl_method` shortcut**: `invokedynamic` records embed the lambda body implementation class and method directly. For graph construction this eliminates the need to resolve through `hidden_class_identity` for the method-level link. The CRC-level link (to bytecode_artifact) still requires hidden_class_identity.

**`trace_id`**: Present on all 1,042 `invokedynamic` records, range 1–1,042, sequential integers. This is an INTRA-TYPE sequential identifier. It is NOT shared with `callsite_adapter_graph` or any other record type. Joining a `callsite_target` to its `callsite_adapter_graph` requires the composite key `(src_class, src_method, src_descriptor, src_bci)`.

**`reflection_method_invoke` / `reflection_constructor_newInstance`**: These categories are produced in `linkResolver.cpp` lines 3966–3967 from the Case B branch (when a `DirectMethodHandleAccessor` or `DirectConstructorHandleAccessor` frame is at the top of the stack). The vframeStream walk recovers the user frame above the accessor, so the `source_class`/`source_method`/`source_bci` fields reflect the actual user callsite. In the Spring Boot run, these records originate in `SimpleInstantiationStrategy.lambda$instantiate$0` (Spring framework reflection calls).

**Verdict**: EXCELLENT. Six categories, all with complete source→target edges. The backbone of the causality graph.

---

### 1.2 `callsite_adapter_graph` — ADAPTER CHAIN MATERIALIZATION

**Spring Boot count**: 630 records  
**Causality role**: Decomposes the MethodHandle adapter chain for one callsite into its constituent adapter nodes.

**Join key**: `(src_class, src_method, src_descriptor, src_bci)` — all four fields required. The `callsite_target` record for the same BCI provides the `primary_target`; the adapter graph provides the path through BoundMethodHandle, LambdaForm, and intermediate adapter layers.

**Node types in the graph field**:
- `lambda_form_entry` — LambdaForm dispatcher entry point
- `adapter_invoke` — intermediate adapter step
- `bound_method_handle` — captured value node (a BoundMethodHandle$Species_*)
- `primary_target` — the final concrete target (same as target_class in callsite_target)
- `adapter_unknown_shape` — emitted when `sg_walk_mh` encounters an unrecognized adapter class

**For causality graph construction**: The `primary_target` node is redundant with the target in `callsite_target`. The intermediate nodes reveal the dispatch mechanism. For the MVP, adapter graphs can be represented as edge annotations on CALLS edges rather than as separate graph nodes.

**`staticizable` and `reconstructable` fields**: Present on both `callsite_target` and `callsite_adapter_graph`. These are AOT feasibility assessments — not causality data. Include them as graph edge attributes but do not confuse them with causality.

**Verdict**: GOOD for adapter path analysis. Valuable for staticization feasibility. Not required for basic causality graph construction (basic graph needs only callsite_target).

---

### 1.3 `runtime_target` — CAUSALITY ORPHANS (CRITICAL GAP)

**Spring Boot count**: 2,905 records  
**Causality role**: Target methods discovered through JVM internal linkage resolution.

**Full field set**: `record`, `evidence`, `dispatch_kind`, `caller_context`, `target_class`, `target_method`, `target_descriptor`, `target_loader_id`, `target_hidden`

**The gap**: There is NO `source_class`, `source_method`, or `source_bci` field. The `caller_context` field for all 2,905 Spring Boot records is the string `"MemberName.resolve"`. This tells you the linkage mechanism but not which specific callsite triggered it.

**Structural consequence**: All 2,905 runtime_target records are causality orphans. They appear as target nodes with no incoming edges in the graph. You know WHAT was resolved, but not WHERE in user code the resolution was triggered.

**Post-hoc correlation opportunity**: For cases where a `runtime_target` target_class/target_method matches a `reflection_method_invoke` record's target fields, tentative 1:1 correlation may be possible. However this is heuristic, not exact — multiple user callsites could resolve the same target.

**Verdict**: POOR as causality edges. The target information is correct but the source is missing. Addressing this gap is the highest-priority JVM change for Phase 2b.

---

### 1.4 `callsite_target_set` — MULTI-TARGET EDGES

**Spring Boot count**: 1 record  
**Causality role**: Guard-With-Test (GWT) multi-target dispatch. One callsite resolves to {test predicate, true-branch target, false-branch target}.

For graph construction: treat as two CALLS edges (source → true_target, source → false_target) with a GWT annotation on both.

**Verdict**: CORRECT semantics. Low volume in current workloads. Design the graph schema to support multi-target edges from the start.

---

### 1.5 `hidden_class_identity` + `bytecode_artifact` — BRIDGING LAYER

**Spring Boot counts**: 1,381 + 6,384 records respectively

**Role in causality**: These two record types form the name-resolution bridge between hidden class nodes (whose names contain a run-specific `+0x<addr>` suffix) and their stable CRC-based identity.

**Chain**:
```
invokedynamic callsite
  → lmf_impl_class = "Application$$Lambda+0x00007f8c01234a00"  (runtime name)
  → hidden_class_identity: runtime_name → artifact_crc=0x12345678
  → bytecode_artifact: (Application$$Lambda, loader_id) → dump_path
```

The first step (invokedynamic → lmf_impl_class) is already embedded in the callsite_target record. The bridge resolves the hidden class's runtime name to its stable CRC for cross-run identity.

**`export_summary.generated_class_count` is always 0**: The `generated_class` record type is defined in the schema but never emitted by the current implementation. The `bytecode_artifact` record type covers the same purpose (bytecode snapshots). This is a schema/implementation discrepancy — do not design the Phase 2 graph around `generated_class` records.

**Note on `export_summary.callsite_target_count`**: The export_summary field `callsite_target_count: 4,953` counts three record types together: `callsite_target` (4,322) + `callsite_adapter_graph` (630) + `callsite_target_set` (1) = 4,953. These are separate record types in the JSONL but grouped in the summary counter.

**Verdict**: ESSENTIAL for stable identity across JVM runs. Required for any cross-run graph comparison.

---

### 1.6 `diagnostic` — UNRESOLVED EDGE PLACEHOLDERS

**Spring Boot count**: 39 records (0 user-code, 12 Spring framework, 27 JDK-internal)

In the causality graph, diagnostics represent callsites where a target was NOT resolved. Represent them as nodes with an "unresolved" flag and a `reason` attribute. They are evidence of graph incompleteness, not graph errors.

**Phase 1 quality guarantee**: Zero user-code diagnostics. All 39 are framework- or JDK-internal, where the JDK class filter suppresses resolution by design.

**Verdict**: Not causality edges, but necessary for graph completeness auditing.

---

## 2. Causality Chains Reconstructable from Phase 1 Data

### Chain 1: Direct Call (invokedynamic / invokeinterface)

```
Application.commandLineRunner (BCI 1)
  ──[callsite_target: invokedynamic]──▶ Application$$Lambda+0x....run
     └── lmf_impl_class = Application$$Lambda, lmf_impl_method = run
```

**Status**: FULLY RECONSTRUCTABLE. Source BCI → target method → lambda body.

---

### Chain 2: Lambda Body → Bytecode

```
Application$$Lambda+0x00007f8c01234a00 (runtime name)
  ──[hidden_class_identity]──▶ artifact_crc = 0x12345678
  ──[bytecode_artifact]──────▶ /tmp/.../Application$$Lambda.class
```

**Status**: FULLY RECONSTRUCTABLE. Requires hidden_class_identity join, then bytecode_artifact CRC match.

---

### Chain 3: Proxy Dispatch Chain

```
Application.validationRunner lambda (invokes Greeter interface)
  ──[callsite_target: invokeinterface]──▶ $Proxy63.greet
Application$$Lambda+0x....invoke (calls implementation)
  ──[callsite_target: invokeinterface]──▶ GreeterImpl.greet
```

**Status**: FULLY RECONSTRUCTABLE by chaining callsite_target records across methods. Requires graph traversal (follow target_class.target_method to find its own outgoing callsite_target records).

---

### Chain 4: Reflection Dispatch

```
SimpleInstantiationStrategy.lambda$instantiate$0 (BCI 7)
  ──[callsite_target: reflection_method_invoke]──▶ HealthEndpointGroupsBeanPostProcessor.healthEndpointGroupsBeanPostProcessor
```

**Status**: FULLY RECONSTRUCTABLE. The `reflection_method_invoke` category provides complete source and target attribution.

---

### Chain 5: MethodHandle Adapter Chain

```
source_class.source_method (BCI N)
  ──[callsite_target: methodhandle_invokeExact]──▶ target_class.target_method
  ──[callsite_adapter_graph]──────────────────▶ [LF entry] → [BMH$Species_*] → [primary_target]
```

Join: `(src_class, src_method, src_descriptor, src_bci)` — NOT `trace_id`.

**Status**: FULLY RECONSTRUCTABLE. The composite-key join works reliably; the adapter graph enriches but is not required for basic causality.

---

### Chain 6: Constructor Resolution (PARTIALLY RECONSTRUCTABLE)

**User-code reflection path**:
```
UserClass.someMethod (BCI X)
  ──[callsite_target: reflection_constructor_newInstance]──▶ SomeBean.<init>
```
**Status**: FULLY RECONSTRUCTABLE when the reflection is in user code.

**Framework DI path**:
```
??? (source unknown)
  ──[runtime_target: caller_context="MemberName.resolve"]──▶ SomeBean.<init>
```
**Status**: TARGET KNOWN, SOURCE UNKNOWN. 2,905 records of this type. The gap.

---

## 3. Information Missing for Full Causality

### Gap 1: `runtime_target` Source Attribution (HIGH IMPACT — 2,905 orphan nodes)

**What is missing**: `source_class`, `source_method`, `source_bci` for all `runtime_target` records.

**Why it matters**: In a Spring Boot application, the majority of bean construction and method resolution happens through the DI framework's reflection calls. Without source attribution, the path "user code → Spring DI → bean method" has a broken link at the DI layer. The causality graph looks like: "Application entry point → [disconnected island of 2,905 resolved targets]."

**Post-hoc workaround**: Where a `runtime_target`'s `target_class`/`target_method` exactly matches a `reflection_method_invoke` record's target, tentative correlation is possible. This works when the same target appears in both records and the cardinality is 1:1. For beans resolved multiple times or by multiple callers, this heuristic fails.

**Phase 2b fix**: In `methodHandles.cpp`, replace the four `soroush_graph_linkage()` call sites inside `soroush_trace_membername_resolution()` with `soroush_graph_generic_callsite()` + a vframeStream walk (Option B). The walk skips `java/lang/invoke/`, `java/lang/reflect/`, `jdk/internal/reflect/`, and `sun/reflect/` frames to find the first user frame. **`linkResolver.cpp` is NOT the fix location.** See [10-phase2b-runtime-target-attribution-design.md](10-phase2b-runtime-target-attribution-design.md) for the complete investigation, option analysis, and implementation design.

---

### Gap 2: No Invocation Frequency Counts (MEDIUM IMPACT)

**What is missing**: How many times each dispatch edge was traversed.

**Why it matters**: A lambda invoked 100,000 times in a hot loop is a far higher staticization priority than one invoked once at startup. Without counts, the causality graph treats all edges equally.

**Current partial signal**: `trace_id` on `invokedynamic` records is a sequential integer (order of first resolution), not an invocation count.

**Phase 2c fix**: Add a `g_freq_table` counter map in `soroushProvenanceGraph.cpp`, keyed by `(source_class, source_method, source_bci)`. Increment on each warm-path dispatch. Export as `callsite_frequency` records at shutdown.

---

### Gap 3: No Temporal Ordering (LOW IMPACT FOR MVP)

**What is missing**: Wall-clock timestamps or a global invocation sequence number.

**Available proxy**: `trace_id` gives ordering within `invokedynamic` records (order of first resolution, not invocation). No ordering signal across record types.

**Why it matters for causality**: "A called B before C called D" is not recoverable. Causal ordering requires timestamps or a happens-before relation.

**Phase 2 fix**: Add a global monotonic counter, emitted as a `seq` field on each record. Low implementation cost, high analytical value.

---

### Gap 4: `generated_class` Records Not Emitted (LOW IMPACT)

**What is missing**: Records for classes rewritten by `soroushClassfileRewriter`. The schema defines a `generated_class` record type; it is never emitted.

**Actual behavior**: `bytecode_artifact` records capture both original and rewritten bytecode with the same schema. The gap is a label, not the data.

**Phase 2 fix**: Either emit `generated_class` records from the rewriter, or document that `bytecode_artifact` serves this role and remove `generated_class` from the schema.

---

### Gap 5: JDK-Internal Dispatch Chains Filtered (BY DESIGN — NOT A GAP)

The invokeinterface hook intentionally skips frames whose holder starts with `java/`, `jdk/`, `sun/`, or `com/sun/`. This is correct for user-code staticization. Relaxing this filter for JDK bootstrap analysis is a separate Phase 2 decision, not a bug.

---

## 4. MVP Feasibility Verdict

**VERDICT: BUILD THE MVP NOW. No JVM modifications required.**

The Phase 1 JSONL export is sufficient for a Runtime Causality Graph covering all user-code dispatch chains. The MVP is a post-processing tool, not a JVM change.

**What the MVP can answer**:
- "What methods are reachable from Application.main?"
- "Which lambda bodies are ever invoked, and where are they defined?"
- "What are the complete adapter chains for each invokeinterface site?"
- "Which callsites involve proxy dispatch (JDK proxy, CGLIB)?"
- "Which callsites involve reflection-based dispatch?"
- "Is every dynamic callsite attributed to an exact target?"
- "Which callsites are `staticizable=true`?"

**What the MVP cannot answer** (Phase 2b/c required):
- "How many times was each edge traversed?"
- "Where in user code did Spring's constructor resolutions originate?"
- "Which dispatch chain was executed first?"

**MVP implementation estimate**: The existing `indexer.py` builds lookup tables from the JSONL. The graph builder is an evolution of indexer.py that emits a directed graph (NetworkX DiGraph, GraphML, or similar) instead of lookup tables. The bulk of the indexing logic (artifact registry, hidden class resolution, adapter graph lookup) already exists. The incremental work is the graph output layer and edge-type modeling.

---

## 5. Proposed New Record Types for Phase 2 Completeness

These are design proposals only. None are implemented.

### 5.1 `runtime_target_attributed` (replaces `runtime_target`)

Add a vframeStream walk to the sites that currently emit bare `runtime_target`. New fields added to the existing schema:

```json
{
  "record": "runtime_target_attributed",
  "source_class": "...",
  "source_method": "...",
  "source_bci": ...,
  "source_opcode": "...",
  "caller_context": "MemberName.resolve",
  "target_class": "...",
  "target_method": "...",
  "target_descriptor": "...",
  "target_loader_id": "..."
}
```

**Impact**: Converts 2,905 orphan nodes into connected graph edges. This is the single highest-impact schema addition for Phase 2.

**Implementation note**: The vframeStream walk must skip `java/lang/invoke/` frames (same logic as Case A2) before finding the user frame. If no user frame exists (pure JDK-internal call), fall back to the current bare `runtime_target` behavior.

---

### 5.2 `callsite_frequency` (new counter record)

A batch of counter records emitted at JVM shutdown, one per observed callsite:

```json
{
  "record": "callsite_frequency",
  "source_class": "...",
  "source_method": "...",
  "source_descriptor": "...",
  "source_bci": ...,
  "invocation_count": ...
}
```

**Impact**: Enables frequency-weighted causality analysis. Hot paths vs cold paths become distinguishable without requiring a profiler.

**Implementation note**: The counter table must be thread-safe. An array of `jlong` counters indexed by the same dedup key as the main side tables is sufficient. The warm-path hook already fires on every invokehandle dispatch and is the right place to increment. For invokeinterface and invokedynamic, the cold-path fire is not per-invocation; a separate warm-path hook would be required to count those accurately.

---

## 6. Phase 2 Roadmap

### Phase 2a: Offline Causality Graph MVP

**Goal**: Build and query the runtime causality graph from Phase 1 JSONL data.  
**JVM changes**: None.  
**Deliverable**: `tools/rt-ui/graph_builder.py`

Steps:
1. Define node types: `Method`, `Callsite`, `HiddenClass`, `BytecodeArtifact`
2. Define edge types: `CALLS` (callsite_target), `ADAPTS_VIA` (callsite_adapter_graph), `LAMBDA_BODY` (invokedynamic.lmf_impl_*), `RESOLVES_TO` (hidden_class_identity → bytecode_artifact)
3. Load all records from JSONL → populate nodes and edges
4. Add `runtime_target` records as orphan target nodes (no incoming CALLS edge; flag as `source_unattributed=true`)
5. Produce GraphML or JSON output
6. Validate against 12-case workload (expected graph shapes are well-defined per case)
7. Validate against Spring Boot run (confirm 26 user-code CALLS edges, 0 unresolved user-code nodes)

---

### Phase 2b: `runtime_target` Source Attribution — **COMPLETE (2026-05-30)**

**Goal**: Convert runtime_target orphan nodes into connected graph edges.  
**JVM changes**: `methodHandles.cpp` only (NOT `linkResolver.cpp` — see [10-phase2b-runtime-target-attribution-design.md](10-phase2b-runtime-target-attribution-design.md)).  
**Deliverable**: `runtime_target` records now carry source attribution fields + updated `graph_builder.py` + 5 new unit tests

**Implementation** (Option A from doc 10 — keep `runtime_target` record type, add source fields):
- `soroush_trace_membername_resolution()` walks `vframeStream` at every call site to find the first non-filtered frame
- `soroush_graph_linkage()` signature extended with source frame parameters; source encoded into node label as `scp=exact/missing` fields
- Phase 4 export parses source fields from label, emits `source_class`/`source_method`/`source_descriptor`/`source_bci`/`source_loader_id`/`source_capture`/`source_missing_reason`
- `graph_builder.py`: `source_capture=exact` records produce `NT_CALLSITE` + `ET_CALLSITE_RT_ATTRIBUTED` edge; `missing` records remain `NT_RUNTIME_TARGET` orphans

**Validation results (2026-05-30)**:
- Runtime Truth: 915 orphans → **0** (100% reduction), connected_runtime_targets = 915
- Spring Boot: 2,905 orphans → **0** (100% reduction), connected_runtime_targets = 2,912
- 11/11 validation checks pass on both workloads
- 19/19 unit tests pass
- `heuristic_edges_created = 0` preserved

---

### Phase 2c: Invocation Frequency Counters

**Goal**: Add per-callsite invocation counts to all CALLS edges.  
**JVM changes**: `soroushProvenanceGraph.cpp` (counter table), `interpreterRuntime.cpp` (warm-path increment), `templateTable_aarch64.cpp` (hook integration).  
**Deliverable**: `callsite_frequency` record type + JMH overhead benchmark

Steps:
1. Add `g_freq_table` (atomic counter map) to `soroushProvenanceGraph.cpp`
2. Increment in `sg_trace_mh_dispatch` (warm-path) per invocation
3. Emit `callsite_frequency` records at shutdown
4. Benchmark: warm-path hook overhead at JMH scale with `SOROUSH_PROVENANCE_GRAPH=0` (confirm < 1ns per dispatch when disabled)

---

### Phase 2d: Web Request Path Validation

**Goal**: Validate graph coverage under Spring MVC `@RequestMapping` dispatch.  
**JVM changes**: None.  
**Deliverable**: Extended validation guide + Spring Web section in 05-validation-guide.md

Steps:
1. Run Spring Boot in web mode (`--spring.main.web-application-type=servlet`)
2. Send a fixed set of HTTP requests using `curl` or `wrk`
3. Trigger graceful shutdown (`kill -SIGTERM`)
4. Verify `@RequestMapping` dispatch target chains appear in the causality graph
5. Document new record patterns specific to web dispatch (DispatcherServlet, HandlerMapping, etc.)

---

## 7. Highest-Leverage Next Step

**Build Phase 2a — the offline graph builder from Phase 1 JSONL data.**

This is the highest-leverage first move because:

1. **Zero JVM risk.** No changes to the interpreter. No chance of regression against the 12/12 validated baseline.

2. **Immediate analytical value.** The graph is queryable for reachability, lambda discovery, adapter path analysis, and staticization feasibility as soon as it builds.

3. **Validates the schema before extending it.** Building the MVP will surface any join inconsistencies, missing fields, or ID collisions that were not apparent from individual record inspection. The runtime_target gap will be visible as a concrete count of orphan nodes rather than an abstract estimate.

4. **Justifies Phase 2b scope precisely.** After the MVP exists, the count of `source_unattributed=true` nodes for user-code targets (vs. JDK-internal) will be a concrete number with specific target_class/method values. Phase 2b can then be scoped to exactly the emission sites that produce those orphans.

5. **Extends existing infrastructure.** `indexer.py` already does the artifact registry, hidden class resolution, and adapter graph lookup. The graph builder is an extension of indexer.py, not a new tool.

**Concrete entry point**:

```python
# tools/rt-ui/graph_builder.py
#
# Usage:
#   python3 graph_builder.py /tmp/spring_out.jsonl --output graph.graphml
#
# Node types: Method, Callsite, HiddenClass, BytecodeArtifact
# Edge types: CALLS, LAMBDA_BODY, ADAPTS_VIA, RESOLVES_TO
#
# Validation:
#   12-case: all case graphs match expected shapes in 05-validation-guide.md
#   Spring Boot: 26 user-code CALLS edges, 0 unresolved user-code nodes
```

---

## 8. Documentation Corrections Made in This Review

The following documentation was found to be inaccurate and has been updated:

### `docs/06-known-limitations.md` — Limitation #1

**Previous text**: "There is no record of the form `category=reflection, source_bci=<the m.invoke() BCI>`."

**Actual behavior**: `reflection_method_invoke` (7 records) and `reflection_constructor_newInstance` (3 records) categories exist in the Phase 1 Spring Boot export. Both have full source attribution (source_class, source_method, source_bci, source_opcode=invokevirtual). These are emitted from Case B in `linkResolver.cpp` (lines 3966–3967) when a `DirectMethodHandleAccessor` or `DirectConstructorHandleAccessor` frame is detected.

**Correction applied**: Limitation #1 has been rewritten to accurately describe what works (full attribution for direct reflection calls), what the remaining gap is (dispatch via MH warm path produces `methodhandle_invokeExact` not `reflection_*`), and where the real unresolved gap is (runtime_target orphans, documented as Limitation #13 — moved here with full detail).

### `docs/00-agent-handoff.md` — Recommended Next Task #2

**Previous text**: "Reflection call site attribution: `Method.invoke(...)` at the user level currently produces a `callsite_target` with the `DirectMethodHandleAccessor` as source."

**Actual behavior**: The user frame IS correctly attributed as the source — the vframeStream walk in Case A2 skips the DMH frame and finds the user frame. The category is `methodhandle_invokeExact`, not `reflection`. The recommendation has been corrected to reflect the actual remaining work.

---

*For the build workflow, see [07-build-workflow-guide.md](07-build-workflow-guide.md).*  
*For Phase 1 validation commands, see [05-validation-guide.md](05-validation-guide.md).*  
*For the current known limitations, see [06-known-limitations.md](06-known-limitations.md).*
