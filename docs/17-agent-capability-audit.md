# Agent Capability Audit — V4 Benchmark Planning

**Purpose**: Determine how much of the ManyCore JVM platform's captured information
is actually reachable by Agent B in the current benchmark setup, identify the gaps,
and determine whether V4 should expose bytecode artifacts before adding more scenarios.

**Investigation method**: Source-verified. Every claim below was confirmed by reading:
- `src/hotspot/share/classfile/soroushProvenanceGraph.cpp` — JVM capture + export
- `src/hotspot/share/classfile/klassFactory.cpp` — bytecode dump logic
- `src/hotspot/share/classfile/soroushClassfileRewriter.cpp` — rewriter phases
- `tools/manycore-ui/app.py` — full API surface (8 causality endpoints + others)
- `tools/manycore-ui/indexer.py` — what the indexer parses and exposes
- `tools/manycore-ui/runner.py` — which SOROUSH_* env vars are always set
- `tools/benchmark/harness_v2.py` — exactly which tools Agent B receives

---

## Main audit table

| Feature | Captured by JVM | Exported to JSONL | In graph builder | Available in manycore-ui | Available through API | Usable by benchmark harness | Usable by Agent B today | Notes |
|---------|:-:|:-:|:-:|:-:|:-:|:-:|:-:|-------|
| **callsite_target records** | ✓ | ✓ | ✓ (NT_CALLSITE, ET_CALLSITE_TARGET) | ✓ (class/method/callsite views) | ✓ (reflection, polymorphic, proxies, search) | ✓ (pre-captured in CAUSALITY string) | ✓ | Core record type; 4 filtered views available |
| **runtime_target records** | ✓ (Phase 2B) | ✓ | ✓ (NT_RUNTIME_TARGET, ET_CALLSITE_RT_ATTRIBUTED) | ✓ (indexed in `runtime_target_ref`) | ✗ (no dedicated endpoint) | ✗ | ✗ | MH linkage events with source attribution. Graph uses them but no causality API endpoint exposes them directly |
| **runtime target attribution** (`source_capture=exact/missing`) | ✓ | ✓ | ✓ (ET_CALLSITE_RT_ATTRIBUTED edge) | ✓ | ✗ | ✗ | ✗ | Fields present in JSONL, graph uses them; 0 orphans on all workloads. No agent-queryable endpoint |
| **reflection attribution** (Method.invoke → target) | ✓ (Phase 2E, warm-path) | ✓ (`category=reflection_method_invoke`) | ✓ | ✓ | ✓ `/causality/reflection` | ✓ (in CAUSALITY string) | ✓ | Best-used capability in current benchmark |
| **polymorphic dispatch information** (multiple targets per BCI) | ✓ (Phase 2C/2D warm-path) | ✓ (multiple records per BCI) | ✓ (`static_label=blocked_multi_target`, Pass 1.5) | ✓ | ✓ `/causality/polymorphic` | ✓ (in CAUSALITY string) | ✓ | Used in V2/V3 benchmarks |
| **Constructor.newInstance attribution** | ✓ (`reflection_constructor_newInstance` category) | ✓ | ✓ | ✓ | ✓ (included in `/causality/reflection` filter) | ✓ | ✓ | Reflection filter includes both method+constructor |
| **callsite_adapter_graph records** (MH chain) | ✓ (`sg_walk_mh`, `sg_walk_generic_bmh`) | ✓ | ✓ (NT_ADAPTER_GRAPH, NT_ADAPTER_NODE, ET_HAS_ADAPTER_GRAPH, ET_HAS_ADAPTER_NODE) | ✓ (indexed, record_types set) | ✗ (no causality endpoint) | ✗ | ✗ | **GAP**: Adapter semantics (string_concat, guard_with_test, type_conversion, primary_target) captured and indexed but no API surfaces them |
| **callsite_target_set records** (GWT/tryFinally/catchException) | ✓ (`soroush_graph_target_set_callsite`) | ✓ (Phase 3.5) | ✓ (NT_TARGET_SET, ET_HAS_TARGET_SET, ET_TARGET_SET_MEMBER) | ✓ (indexed) | ✗ (no causality endpoint; only counted in summary) | ✗ | ✗ | **GAP**: Multi-target MH combinators fully captured; no agent-reachable query |
| **loader metadata** (classloader IDs in all records) | ✓ | ✓ (`source_loader_id`, `target_loader_id`) | ✓ (node identity includes loader) | ✓ | ✓ (present in all callsite records in API responses) | ✓ (in CAUSALITY string fields) | ✓ (passively — field present but agent rarely interprets it) | Gap #17 fix: loader-aware dedup; loader ID present everywhere |
| **source_bci** | ✓ (all warm/cold path hooks) | ✓ | ✓ (part of callsite identity) | ✓ | ✓ (in all causality responses) | ✓ | ✓ | Agent B uses BCI to pinpoint callsite |
| **source_opcode** | ✓ (`invokevirtual`, `invokeinterface`, `invokehandle`, `reflection_method_invoke`, etc.) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | Agents see opcode in response; used to distinguish reflection from poly dispatch |
| **static_label** | ✗ (not a JVM field) | ✗ | ✓ (assigned by graph builder pass 3) | ✓ | ✓ (present in chain/explain/summary responses) | ✗ (not in CAUSALITY string) | ✗ | Computed post-hoc; not in benchmark pre-capture |
| **blocked_multi_target label** | ✗ | ✗ | ✓ (Pass 1.5 assigns SR_MULTI_TARGET) | ✓ | ✓ (in polymorphic endpoint) | ✓ (SR_MULTI_TARGET in CAUSALITY string) | ✓ | Used by Agent B in V2/V3 |
| **generated class artifacts** (ByteBuddy, CGLIB subclasses) | ✓ (when `SOROUSH_CAPTURE_FINAL_BYTECODE=1`) | ✓ (`bytecode_artifact` records, `kind=final`) | ✓ (NT_BYTECODE_ART, ET_HAS_BYTECODE_ART) | ✓ (`artifact_by_class` index, download endpoint) | ✓ `/api/runs/<id>/artifact` + `/api/runs/<id>/bytecode` (javap) | ✗ | ✗ | **GAP**: .class files ARE written during live runs (runner.py sets `SOROUSH_CAPTURE_FINAL_BYTECODE=1` always); artifact API exists; benchmark harness does not expose it |
| **hidden class artifacts** (lambda hidden classes, `defineHiddenClass`) | ✓ — CRC always; bytes when `SOROUSH_CAPTURE_FINAL_BYTECODE=1` | ✓ (`hidden_class_identity` + `bytecode_artifact` records) | ✓ (NT_HIDDEN_CLASS, ET_HAS_HIDDEN_IDENTITY) | ✓ (`hidden_id_map` index) | ✓ `/causality/hidden` exists | ✓ (partially — CAUSALITY string mentions reflection constructor) | ✗ (benchmark harness omits `causality_hidden` from Agent B tools) | **GAP**: `/causality/hidden` API exists and is implemented; benchmark harness tool list simply does not include it |
| **lambda artifacts** (invokedynamic → LMF hidden class) | ✓ — `lmf_impl_class/method` captured cold-path; CRC via `hidden_class_identity` | ✓ | ✓ (NT_CALLSITE, ET_LAMBDA_BODY edge) | ✓ | ✓ (`/causality/search` returns `lmf_impl_*` fields; `/causality/hidden` returns CRC) | ✗ | ✗ | **GAP**: Lambda body exact method is in JSONL (`lmf_impl_method`); retrievable via search endpoint not available in benchmark |
| **ByteBuddy-generated classes** | ✓ (captured as normal or hidden class depending on load mechanism) | ✓ | ✓ | ✓ | ✓ (shows in `/causality/proxies` by class name pattern; bytecode artifact available) | ✗ | ✗ | Area 6 breadth-validated; class name and artifact captured; benchmark does not expose |
| **proxy artifacts** (JDK Proxy $Proxy0, Spring CGLIB) | ✓ | ✓ | ✓ | ✓ | ✓ `/causality/proxies` | ✓ (in CAUSALITY string) | ✓ | Best-covered proxy scenario; bytecode artifact additionally available but not exposed to agent |
| **class CRCs** | ✓ (`soroush_crc32()` in klassFactory.cpp, runs for all classes) | ✓ (`crc` field in `bytecode_artifact`; `artifact_crc` in `hidden_class_identity`) | ✓ (node identity includes CRC) | ✓ | ✓ (present in artifact and hidden endpoints) | ✗ | ✗ | CRC is stable cross-run identity for generated classes. Currently unused by agent |
| **captured bytecode (.class files on disk)** | ✓ (`capture_final_class_bytes()` in klassFactory.cpp, always enabled by runner.py) | ✓ (`artifact_path` field in `bytecode_artifact` records, conditional on file existence check) | ✓ (artifact nodes have path attribute) | ✓ (indexer stores artifact records; `/download/artifacts` endpoint serves zip) | ✓ `/api/runs/<id>/bytecode?artifact_path=<path>` calls `javap -c -p -verbose` | ✗ | ✗ | **VERIFIED**: `runner.py` sets `SOROUSH_CAPTURE_FINAL_BYTECODE=1` unconditionally. Files are written to `SOROUSH_BYTECODE_DUMP_DIR` (default `/tmp/soroush_jvm_dump`). The API endpoint to serve them and run javap already exists. Benchmark harness never exposes this. |
| **javap / decompiled output** | N/A (post-processing) | N/A | N/A | ✓ (`/api/runs/<id>/bytecode` runs `javap -c -p -verbose` on any artifact file) | ✓ (live subprocess call, returns stdout + stderr) | ✗ | ✗ | **GAP**: Full javap decompilation of any captured class is available through the API. Not exposed to any agent. This is uniquely valuable for generated/lambda classes. |
| **artifact storage / indexes** | N/A | N/A | ✓ (artifact nodes, ET_HAS_BYTECODE_ART edges) | ✓ (`artifact_by_class` dict, `find_best_artifact()`, `/download/artifacts` zip) | ✓ `/api/runs/<id>/artifact?class=<cls>` | ✗ | ✗ | Fully built, not exposed to agent |
| **graph path reconstruction** (callsite → target chain) | N/A | N/A | ✓ (`query_chain()`) | N/A | ✓ `/causality/chain?class=&method=&bci=` | ✗ | ✗ | **GAP**: Chain endpoint fully implemented; not in benchmark tool list |
| **callsite → target → artifact chains** | N/A | N/A | ✓ (ET_CALLSITE_TARGET + ET_HAS_BYTECODE_ART edges) | N/A | ✗ (no single endpoint traverses class→artifact for a given chain) | ✗ | ✗ | **GAP**: Graph structure supports this traversal; no API endpoint packages it as a single query |
| **classfile rewriter** (entry/exit tracing, method tokens) | ✓ (4501-line `soroushClassfileRewriter.cpp`; Phase 1: bytecode marking, Phase 2: entry NOPs, Phase 3: constructor trace, Phase 5: exit trace via `soroushTraceExit(I)V`) | ✓ (method token registry; `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1` set by runner.py) | ✗ (graph builder does not import rewriter output) | ✗ | ✗ | ✗ | ✗ | **MAJOR GAP**: 4500 lines of instrumentation for method-level entry/exit tracing exist and Phase 5 is always enabled by runner.py, but the output is never indexed, never graphed, and never exposed to any agent |

---

## Section A — What is the richest information the JVM captures that Agent B cannot currently access?

**Rank 1: Actual .class bytecode artifacts + javap output**

The JVM writes every loaded class's bytecode to disk (`SOROUSH_CAPTURE_FINAL_BYTECODE=1`, always set by runner.py). For a generated class (ByteBuddy subclass, CGLIB proxy, lambda hidden class), this bytecode exists nowhere in the source repository — it only exists as runtime output of the generator. The `bytecode_artifact` JSONL record contains `artifact_path`, and the UI has `/api/runs/<id>/bytecode?artifact_path=<path>` which runs `javap -c -p -verbose` and returns the full disassembly.

**An agent with access to this can:**
- Read the actual bytecode of any generated class
- See what methods a CGLIB proxy interceptor actually executes
- Identify the bug in a ByteBuddy-generated class that has no source file
- Verify that a lambda hidden class compiles to the expected comparison logic

Agent B currently has none of this.

**Rank 2: callsite_adapter_graph records (MH chain decomposition)**

Every MethodHandle call site is decomposed into a classified adapter chain with semantic labels:
`primary_target`, `secondary_component` (type_conversion, string_concat, guard_with_test),
`bound_data`, `test_predicate`, `true_target`, `false_target`. These labels explain WHY a callsite
dispatches to a particular target through an adapter chain. No agent-facing API surfaces this data.

**Rank 3: classfile rewriter output (method entry/exit tracing)**

A 4,500-line classfile transformer instruments method entries and exits with unique integer method tokens
(`System.soroushTraceEnter(I)`, `soroushTraceExit(I)`). Phase 5 (`SOROUSH_REWRITER_PHASE5_NORMAL_EXIT`)
is always active via runner.py. This produces a call sequence trace — not just "which dispatch sites fired"
but "which methods ran to completion in what order." This is entirely unused by any agent or graph builder.

---

## Section B — What information would provide the biggest debugging advantage if exposed?

**1. Bytecode artifacts (javap access) — for generated/hidden-class bugs**

For bugs inside generated code (lambda, ByteBuddy, CGLIB), the agent currently sees:
- Class name: `com/example/ProductService$$Lambda$1/0x0000000123456789`
- That's all. No source, no line number, no code.

With javap access:
- Agent reads the actual disassembled bytecode of the generated class
- Can identify wrong comparator sign, wrong offset, missing null check directly from the bytecode
- This is not a hypothetical — the class file already exists on disk; the endpoint to serve it already exists

**2. causality_hidden in benchmark harness — for lambda debugging**

`/causality/hidden` returns: runtime_name → artifact_crc → stable identity across runs.
Paired with `/causality/search`, it resolves: `Lambda$1/0x...` → `lambda$sortByPrice$0` in `ProductService.java`.
This endpoint is already implemented. The benchmark harness tool list simply does not include it.
Adding `causality_hidden` to Agent B's tool set requires one line in `harness_v2.py`.

**3. causality_chain in benchmark harness — for proxy chain bugs**

`/causality/chain?class=&method=&bci=` returns the full dispatch chain from a callsite through any
proxy layers to the final concrete target. Currently available in manycore-ui but omitted from the
benchmark tool set. A proxy-chain bug is insoluble in 2 turns without `causality_chain`;
with it, Agent B can traverse proxy → interceptor → real implementation in one call.

---

## Section C — Smallest set of new API endpoints that would unlock the high-value information

**No new JVM changes are needed for any of these.** All data already exists.

| Priority | Action | What it unlocks | Effort |
|----------|--------|----------------|--------|
| 1 | Add `causality_hidden` to benchmark harness tool list (`harness_v2.py` line ~88) | Lambda hidden class → source method resolution in benchmark | ~2 lines |
| 2 | Add `causality_chain` to benchmark harness tool list | Full proxy chain traversal in benchmark | ~4 lines |
| 3 | Add `causality_bytecode` tool to benchmark harness — calls `/api/runs/<id>/bytecode?artifact_path=<path>` | Actual bytecode of any generated class, readable by agent | ~15 lines in harness; endpoint already in app.py |
| 4 | Add `causality_artifact` tool to benchmark harness — calls `/api/runs/<id>/artifact?class=<cls>` | Resolves class name → artifact_path for generated classes | ~10 lines; endpoint already in app.py |
| 5 | New API endpoint: `/api/runs/<id>/causality/adapters` | Exposes callsite_adapter_graph data (MH chain semantics) | ~40 lines in app.py; data already in index |

The total cost of items 1–4 is under 30 lines of Python in the benchmark harness and zero JVM changes.

---

## Section D — For a generated-class benchmark (ByteBuddy, lambda, plugin, hidden class), what exact queries would Agent B need?

**Scenario**: A bug in a lambda comparator / ByteBuddy-generated class / hidden class.
The bug's class has no `.java` source file. The runtime class name changes every JVM run.

**Required query sequence:**

```
Step 1: causality_summary
  → understand how many dynamic dispatch events exist

Step 2: causality_search?q=<class-containing-the-callsite>
  → find the invokedynamic or reflection callsite; get lmf_impl_class/method for lambdas

Step 3: causality_hidden
  → map runtime_name (+0x...) → artifact_crc (stable)
  → identify which hidden class corresponds to the callsite

Step 4: causality_artifact?class=<base-class-name>
  (NEW — calls /api/runs/<id>/artifact?class=<name>)
  → resolve class name → artifact_path on disk

Step 5: causality_bytecode?artifact_path=<path>
  (NEW — calls /api/runs/<id>/bytecode?artifact_path=<path>)
  → read javap output of the generated class
  → identify the bug in bytecode (wrong sign, wrong constant, wrong branch)

Step 6: submit_diagnosis
  → file=ProductService.java line=<lambda definition line>
  → patch: fix the lambda body in the source file (the class will be regenerated correctly)
```

**For a ByteBuddy or plugin subclass scenario**, Step 2 would use `causality_proxies` or `causality_search`
to find the callsite targeting the ByteBuddy class, then Steps 4–5 to read its bytecode.

**Without Steps 3–5**, Agent B cannot solve this class of bug. There is no grep-able source.
The current benchmark has no generated-class bug scenario because Agent B lacks the tools to solve it.

---

## Section E — Are we currently benchmarking only a subset of the platform?

**Yes — a small subset.**

Current benchmark (V1, V2, V3) exercises:

| Capability exercised | Fraction of platform |
|---------------------|---------------------|
| `callsite_target` records via 4 filtered views (reflection, polymorphic, proxies, summary) | 4 of 8 causality API endpoints |
| Static source code search (search_files, read_file, grep) | Not unique to ManyCore |
| `hidden_class_identity` API | Implemented, **not exposed to benchmark agent** |
| `callsite_adapter_graph` data | Captured, indexed, **not exposed** |
| `callsite_target_set` data | Captured, indexed, **not exposed** |
| Bytecode artifacts (javap) | Dumped to disk, API exists, **not exposed** |
| Classfile rewriter output | Runs during captures, **never indexed or queried** |
| `causality_chain` / `causality_explain` | Implemented, **not in benchmark tool list** |

**Of 8 implemented causality API endpoints, Agent B gets 4.**

**Of the JVM's unique data types (callsite_target, runtime_target, callsite_adapter_graph, callsite_target_set, bytecode_artifact, hidden_class_identity), Agent B queries only 1 family** (callsite_target through filtered views).

**The classfile rewriter (4,501 lines, always active) produces output that zero downstream consumers use.**

---

## Final verdict

> **Agent B is currently using approximately 20–25% of the platform's unique capabilities.**

**Justification:**

- 4/8 causality API endpoints exposed in benchmark harness = 50%
- 1/6 unique JSONL record types actually queried by agent (callsite_target only) = 17%
- 0/2 artifact capabilities (bytecode dump + javap) exposed = 0%
- 0/1 classfile rewriter capability used = 0%
- Weighted estimate (API surface × record types × artifact): **~20–25%**

The 20% estimate is conservative — the platform's most unique capabilities (bytecode artifacts
of generated classes, MH adapter chain semantics, classfile rewriter traces) are entirely unused.
The current benchmark measures the agent's ability to use dispatch-target lookup, which is
only one layer of what the platform captures.

---

## Prioritized roadmap

### 1. Highest-value capability not exposed: bytecode artifacts + javap access

**Why highest value**: Generated classes (lambda, ByteBuddy, CGLIB) have no source file.
The only way to diagnose a bug in such a class is to read its bytecode. The JVM already dumps
every class to disk. The javap endpoint already exists in app.py. The benchmark harness does
not expose it.

**What to build**: One new tool `causality_bytecode` in the harness + one new tool `causality_artifact`
(or merge into a single `causality_class?class=<name>` endpoint that returns CRC, artifact_path, and
javap output in one call). This unlocks an entire class of benchmark scenarios that are currently
unsolvable for Agent B.

**Validates**: The claim that ManyCore helps with generated/lambda bugs — currently unproven.

---

### 2. Second highest-value capability not exposed: `causality_hidden` + `causality_chain` in benchmark harness

**Why high value**: Lambda debugging (the Bug 3 scenario in the original design) specifically requires
mapping runtime_name → artifact_crc → source lambda method. `/causality/hidden` already returns this.
`/causality/chain` traverses proxy chains without requiring the agent to know intermediate class names.
Both are implemented, tested, and working — they are simply absent from the benchmark tool list.

**What to build**: Add `causality_hidden` and `causality_chain` to `TOOLS_B_EXTRA` in `harness_v2.py`.
Cost: 8 lines of Python. The pre-captured CAUSALITY data in bug scenarios would need a `/causality/hidden`
section added (same pattern as existing reflection/polymorphic sections).

**Validates**: Lambda-dispatch and proxy-chain debugging claims that were part of the original V1 design
but have not yet been tested with Agent B.

---

### 3. Third highest-value capability not exposed: `callsite_adapter_graph` via a new API endpoint

**Why high value**: MethodHandle adapter chains are semantically labeled: `guard_with_test`, `type_conversion`,
`string_concat`, `bound_data`. For a bug caused by a wrong adapter (wrong type conversion, wrong bound
argument, wrong GWT branch), this is the only way to see the full structure without running javap on the MH internals.

**What to build**: A new `/api/runs/<id>/causality/adapters` endpoint in `app.py` (40 lines) that returns
all `callsite_adapter_graph` records for the run, formatted as the other causality endpoints. Optionally
filtered by class fragment. Then expose as `causality_adapters` in the benchmark harness.

**Validates**: MH-adapter-chain debugging — a category that no other debugging tool can handle at all.

---

## Recommendation on V4 focus

**Build the bytecode exposure layer before adding more benchmark scenarios.**

Reasoning:
1. Without it, V4 can only test variations of "find which of N implementations has the bug." That
   is the V2/V3 pattern. The platform is proven to help there; running more scenarios adds confidence
   but not new evidence.
2. A generated-class bug (lambda wrong comparator, ByteBuddy wrong method body, hidden class wrong
   constant) requires bytecode access. Without it, both agents fail at the same step — "I cannot
   read the generated class" — and the benchmark measures nothing.
3. The data and endpoints already exist. The gap is entirely in the harness tool definitions and
   the pre-captured CAUSALITY scenario strings.
4. Once exposed, V4 can demonstrate a capability gap that is structurally impossible to close
   with grep/read alone: no amount of source reading resolves a bug in a class that has no source file.
