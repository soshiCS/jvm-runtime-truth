# Agent Navigator

**You are reading this because you are starting work on the Runtime Truth project.**
**Read this document first. Then use it to decide which deeper documents to open.**

---

## Read This First (90 seconds)

This project is a custom fork of OpenJDK 21 that instruments the HotSpot JVM interpreter to capture a complete runtime callsite attribution map — which method is actually dispatched to at every dynamic call site in a running Java program. The long-term goal is staticization: eliminating dynamic dispatch for workloads where all targets are provably fixed.

**What exists today (Phase 1, complete 2026-05-29):**
- 12 synthetic test cases covering every major JVM dispatch mechanism all pass with zero user-code errors.
- A real Spring Boot 4.0.6 application runs and exits cleanly under the custom JVM, producing 26 user-code callsite records, all exact.
- The instrumentation is stable. No JVM crashes, no silent omissions for user code.

**Phase 2a (complete):**
- `tools/manycore-ui/graph_builder.py` — offline Runtime Causality Graph MVP. 22 tests pass (14 Phase 2a + 5 Phase 2b + 3 poly model). ManyCore + Spring Boot validation: 11/11 checks each.

**Phase 2b (COMPLETE 2026-05-30):**
- `runtime_target` source attribution: vframeStream walk in `soroush_trace_membername_resolution()` (`methodHandles.cpp`). ManyCore: 915→0 orphans. Spring Boot: 2905→0 orphans. `source_capture=exact/missing` fields now in `runtime_target` records. `graph_builder.py` connects attributed records via `ET_CALLSITE_RT_ATTRIBUTED`. 19/19 tests pass.

**Polymorphic callsite model (COMPLETE 2026-05-30, prerequisite for Phase 2C):**
- New `soroush_graph_poly_callsite()` in `soroushProvenanceGraph.cpp/.hpp`. Uses dedup key `(src_class, src_method, src_desc, src_bci, target_class, target_method, target_desc)` — one entry per unique (callsite, target) pair. Stored in `g_poly_buckets`; exported as Phase 3.1 in the JSONL pipeline.
- `graph_builder.py` Pass 1.5: upgrades callsite nodes with >1 distinct `CALLSITE_TARGET` destinations to `static_label=blocked_multi_target`. 22/22 tests pass.

**Phase 2C invokevirtual capture (COMPLETE 2026-05-30):**
- **Cold-path hook** in `runtime_resolve_virtual_method` in `linkResolver.cpp` (after line 1674). Records the first-resolved target per CP cache entry. Calls `soroush_graph_poly_callsite("invokevirtual", ...)`.
- **Warm-path hook** in `TemplateTable::invokevirtual_helper` (`templateTable_aarch64.cpp`), non-final vtable path. Fires on **every interpreted non-final invokevirtual dispatch**. After `__ lookup_virtual_method(r0, index, method)`, saves `lr`, calls `InterpreterRuntime::soroush_trace_iv_dispatch(thread, rmethod)`, restores `lr`. `rmethod` is automatically preserved by `call_VM_leaf_base`. JRT_ENTRY in `interpreterRuntime.cpp`; walks vframeStream, guards `op == _invokevirtual` to skip invokeinterface forced-virtual cases.
- **True polymorphic capture**: a single `invokevirtual` BCI now produces one record per distinct receiver type actually seen at runtime. Case14 BCI 54 → Dog.sound, Cat.sound, Bird.sound (all same `source_bci=54`).
- 14/14 ManyCore cases pass. Case13: `Circle.describe()` mono (BCI 9). Case14: `Animal.sound()` poly (BCI 54 → 3 targets, `static_label=blocked_multi_target`). `heuristic_edges_created=0`.
- Spring Boot: `com/example/springboot/Application.lambda$validationRunner$3 bci=21 → HelloController.index` captured via warm-path invokevirtual hook.

**Phase 2D invokeinterface capture (COMPLETE 2026-05-30):**
- **Cold-path hook** in `runtime_resolve_interface_method` (`linkResolver.cpp`) migrated from `soroush_graph_generic_callsite` to `soroush_graph_poly_callsite("invokeinterface", ...)`.
- **Warm-path hook** in `TemplateTable::invokeinterface` normal itable path (`templateTable_aarch64.cpp`), after `profile_arguments_type`, before `jump_from_interpreted`. Fires on **every interpreted normal itable dispatch**. Saves `lr`, calls `InterpreterRuntime::soroush_trace_ii_dispatch(thread, rmethod)`, restores `lr`.
- **`soroush_trace_ii_dispatch`** JRT_ENTRY in `interpreterRuntime.cpp`; declared in `interpreterRuntime.hpp`. Walks vframeStream, guards `op == _invokeinterface`. Emits via `soroush_graph_poly_callsite("invokeinterface", ...)`.
- **True polymorphic capture**: a single `invokeinterface` BCI produces one record per distinct concrete type observed. Case15 BCI 54 → Dog.sound, Cat.sound, Bird.sound (all same `source_bci=54`, `source_opcode=invokeinterface`). Graph node: `static_label=blocked_multi_target` (SR_MULTI_TARGET). `heuristic_edges_created=0`.
- 15/15 ManyCore cases pass. Spring Boot: 11,956 invokeinterface records captured, 0 crashes.

**Breadth Validation Pass (COMPLETE 2026-05-30):**
7-area validation covering real-world JVM mechanisms. All 6 runnable areas pass with 0 orphans, 0 heuristic edges. Three new gaps discovered and documented in [06-known-limitations.md](06-known-limitations.md):
- **Gap #15**: ~~HTTP-path Spring MVC — reflection attribution wrong~~ — **RESOLVED (Phase 2E, 2026-05-30)**. Warm-path hook in `soroush_trace_iv_dispatch` decodes `java.lang.reflect.Method` recv_oop → emits `reflection_method_invoke` record per call. `InvocableHandlerMethod.doInvoke bci=55 → HelloController.index` now present in HTTP runs. Consistent across mixed-mode and -Xint.
- **Gap #16**: ~~Mockito inline mock maker deadlocks under provenance capture~~ — **WORKAROUND CONFIRMED (2026-05-30)**. This is a **validation env issue, not a capture bug**. `-javaagent:byte-buddy-agent.jar` eliminates the `VirtualMachine.attach()` deadlock. With preloaded agent: mock bytecode artifact + 5 dispatch edges captured, `heuristic_edges_created=0`, `orphans=0`, `validation fail=0`. Mockito → PROVEN_COVERED.
- **Gap #17**: ~~Same-class-name dedup collision across classloaders~~ — **RESOLVED (2026-05-30)**. `soroush_graph_poly_callsite` dedup key now includes both `src_loader_id` and `target_loader_id` in hash and equality check. Validated by Area8 (two isolated loaders, same BCI, two distinct `callsite_target` records, `static_label=blocked_multi_target`, 0 heuristics). 26/26 graph builder unit tests pass.
Coverage matrix: invokevirtual ✓, invokeinterface ✓, invokedynamic ✓, MH ✓, reflection ✓ (warm-path, Phase 2E), hidden classes ✓, ByteBuddy generated classes ✓, Hibernate-style proxy chain (3-tier) ✓, custom classloaders (loader-aware dedup ✓ Gap #17 fixed), ConstantDynamic BSM ✓, VarHandle ✓. invokestatic/invokespecial: 0 records by design (statically resolved).

**Phase 2E (COMPLETE 2026-05-30):**
- Warm-path reflection attribution: `soroush_trace_iv_dispatch` receives `recv_oop` (r2), decodes `java.lang.reflect.Method` via `clazz()/slot()/method_with_idnum()`, emits `reflection_method_invoke` record.
- 3 new graph builder unit tests added (tests 24–26). 26/26 pass.
- Gap #15 closed. Only Gap #16 (Mockito safepoint starvation) remains open.

**The one rule you must not break:**
```
jdk21u-export  =  authoritative repo  →  all edits, builds, and validation happen here
jdk21u         =  historical copy     →  never modify, never build from, never copy into
```

---

## Current Status Snapshot

| Dimension | State |
|---|---|
| Phase | Phase 1 COMPLETE / Phase 2a–2E COMPLETE / Breadth Validation COMPLETE / All gaps (#15–#17) resolved or workaround confirmed |
| ManyCore test suite | 15/15 PASS, 0 user-code diagnostics |
| Spring Boot validation | HTTP reflection edge captured (`doInvoke bci=55 → HelloController.index`) |
| Breadth validation (8 areas) | Areas 1,2,3,4,5,6,7,8 ALL PROVEN_COVERED; Gap #16 workaround: `-javaagent:$BBA` |
| Graph builder unit tests | 26/26 PASS (3 Phase 2E tests added) |
| JVM stability | No crashes. `lr` save/restore bug fixed. |
| Documentation | Complete, in `jdk21u-export/docs/`; Gaps #15+#17 RESOLVED; Gap #16 workaround confirmed (validation env issue) |
| Known gaps | 17 in [06-known-limitations.md](06-known-limitations.md); all resolved or workaround documented; 0 open blocking gaps |

**Last confirmed working build:**
```
OpenJDK 64-Bit Server VM (fastdebug build 21.0.12-internal-adhoc.soroushaghajani.jdk21u-export, mixed mode)
```

---

## The Core Rule

```
╔══════════════════════════════════════════════════════════════╗
║  AUTHORITATIVE REPO: /Users/soroushaghajani/custom-jvm/      ║
║                      jdk21u-export/                          ║
║                                                              ║
║  HISTORICAL COPY:    /Users/soroushaghajani/custom-jvm/      ║
║                      jdk21u/                                 ║
║                      ← DO NOT TOUCH THIS DIRECTORY           ║
╚══════════════════════════════════════════════════════════════╝
```

Before editing any file, verify its path contains `jdk21u-export`, not `jdk21u`.

The most common violation: editing `jdk21u/src/hotspot/share/classfile/linkResolver.cpp` instead of the correct `jdk21u-export/src/hotspot/share/interpreter/linkResolver.cpp`. They are different files in different repos with different paths. See [07-build-workflow-guide.md → Key File Paths](07-build-workflow-guide.md).

---

## Where Should I Go If…

### I need the fastest project overview
→ This document + [01-project-overview.md](01-project-overview.md) (10 min)

### I need to understand how the instrumentation works end-to-end
→ [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md)

### I need to modify MethodHandle capture (invokehandle)
→ [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — Warm-Path and Cold-Path sections  
→ [03-source-ownership-map.md](03-source-ownership-map.md) — "Warm-Path invokehandle Hook" and "Cold-Path Callsite Capture" sections  
→ Key files: `templateTable_aarch64.cpp`, `linkResolver.cpp`, `interpreterRuntime.cpp/.hpp`

### I need to modify reflection capture (Method.invoke, Constructor.newInstance)
→ [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — "Case B" in the Cold-Path section  
→ [02-phase-history.md](02-phase-history.md) — "Milestone 1: Reflection Recovery"  
→ Key file: `linkResolver.cpp` — `resolve_handle_call` Case B branch, `sg_walk_mh` depth 6

### I need to modify invokeinterface capture (proxy, CGLIB, stream)
→ [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — "invokeinterface Capture" section  
→ [02-phase-history.md](02-phase-history.md) — "Milestone 3: invokeinterface Capture"  
→ Cold-path: `linkResolver.cpp` — `runtime_resolve_interface_method` (calls `soroush_graph_poly_callsite`)  
→ Warm-path: `templateTable_aarch64.cpp` — `TemplateTable::invokeinterface` normal itable path; `interpreterRuntime.cpp` — `soroush_trace_ii_dispatch`

### I need to modify hidden class handling
→ [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — "Export Pipeline" (hidden_class_identity)  
→ [02-phase-history.md](02-phase-history.md) — "Milestone 2: Hidden Class Identity Recovery"  
→ Key files: `klassFactory.cpp`, `soroushProvenanceGraph.cpp`

### I need to modify adapter graph decomposition (MH chain walking)
→ [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — "Adapter Graph Generation" section  
→ [03-source-ownership-map.md](03-source-ownership-map.md) — "Adapter Graph Decomposition"  
→ Key file: `linkResolver.cpp` — `sg_walk_mh`, `sg_walk_generic_bmh`, `sg_compute_node_semantic`

### I need to debug UI or indexer issues
→ [03-source-ownership-map.md](03-source-ownership-map.md) — "ManyCore UI" section  
→ `tools/manycore-ui/README.md` — REST API, UI layout, validation checks  
→ Key files: `tools/manycore-ui/app.py`, `tools/manycore-ui/indexer.py`

### I need to work on the demo platform or LLM benchmark
→ [11-demo-platform-design.md](11-demo-platform-design.md) — full design: benchmark spec, API spec, 4 bug specs, next tasks  
→ [12-demo-bug-suite.md](12-demo-bug-suite.md) — implementation complete; JSONL validated; A/B TTID reduction table  
→ [13-agent-benchmark-design.md](13-agent-benchmark-design.md) — A vs B benchmark design: controlled variables, scoring rubric, harness, success criteria  
→ [14-benchmark-results.md](14-benchmark-results.md) — **Results**: single-turn run, all 3 bugs; TIE on 2, +1 on Bug 3 (line precision only); no material advantage found in single-turn mode  
→ Causality API: `GET /api/runs/<run_id>/causality/{summary,search,polymorphic,reflection,proxies,hidden,chain,explain}` — already implemented in `tools/manycore-ui/app.py`  
→ Demo app: `tools/demo-buggy-app/` — 4 bugs implemented, Maven build passes, JSONL export confirmed  
→ To ingest a pre-captured run: `POST /api/runs/ingest {"label":"...","run_dir":"..."}` (requires manycore-ui restart to activate — added in runner.py + app.py)

### I need to add a new JSONL record type
→ [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — "Export Pipeline" section  
→ [03-source-ownership-map.md](03-source-ownership-map.md) — "Core Export Infrastructure" section  
→ Key file: `soroushProvenanceGraph.cpp` — add side table, emission function, export block

### I need to rebuild the JVM or fix a build problem
→ [07-build-workflow-guide.md](07-build-workflow-guide.md) — read the entire document

### I want to run a demo or test something quickly
→ [09-running-tests-and-demos.md](09-running-tests-and-demos.md) — UI quickstart, CLI one-liners, remote demo via start_demo.sh

### I need to work on the causality graph builder
→ `tools/manycore-ui/graph_builder.py` — offline graph builder (Phase 2a)  
→ `tools/manycore-ui/tests/test_graph_builder.py` — 14 fixture tests  
→ [03-source-ownership-map.md → Offline Causality Graph Builder](03-source-ownership-map.md) — node/edge types, identity rules, extension guidance  
→ [08-phase2-causality-graph-design-review.md](08-phase2-causality-graph-design-review.md) — Phase 2a status + Phase 2b–d roadmap

### I need to run the full validation suite (with per-case expected output)
→ [05-validation-guide.md](05-validation-guide.md) — all commands are copy-pasteable

### I need to understand why a diagnostic record was emitted
→ [06-known-limitations.md](06-known-limitations.md) — all reason codes documented with examples  
→ [03-source-ownership-map.md](03-source-ownership-map.md) — "Diagnostics" section for the reason code table

### I need to continue Phase 2 work
→ [08-phase2-causality-graph-design-review.md](08-phase2-causality-graph-design-review.md) — complete Phase 2 architecture review with roadmap and recommendations  
→ [10-phase2b-runtime-target-attribution-design.md](10-phase2b-runtime-target-attribution-design.md) — Phase 2b deep-dive: `runtime_target` root cause, all emission sites confirmed in `methodHandles.cpp`, Option B design  
→ [00-agent-handoff.md](00-agent-handoff.md) — "Recommended Next Tasks" section  
→ [06-known-limitations.md](06-known-limitations.md) — Phase 2 fix guidance for each limitation

### I need to understand a past design decision
→ [02-phase-history.md](02-phase-history.md) — every milestone has a "Lessons Learned" section

### I need to add a warm-path hook for a new bytecode
→ [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — "Warm-Path Capture" + "Why lr must be saved"  
→ [02-phase-history.md](02-phase-history.md) — Milestone 6 (all the lr/pre/JRT_ENTRY traps)  
→ [03-source-ownership-map.md](03-source-ownership-map.md) — "Warm-Path invokehandle Hook" — "What to modify when extending"

---

## Documentation Map

| File | One-line description |
|---|---|
| [AGENT_NAVIGATOR.md](AGENT_NAVIGATOR.md) | **This file.** Navigation map for future agents. Read first. |
| [README.md](README.md) | Entry point for humans; points here; quick-reference two-command cheat sheet |
| [00-agent-handoff.md](00-agent-handoff.md) | Project summary, status, repo layout, validation commands, known traps, Phase 2 tasks — 10-minute onboarding for new agents |
| [01-project-overview.md](01-project-overview.md) | Goals, long-term vision, staticization model, relationships between all JSONL record types |
| [02-phase-history.md](02-phase-history.md) | Every completed milestone: goal, problem, solution, files modified, validation used, lessons learned |
| [03-source-ownership-map.md](03-source-ownership-map.md) | Every capability → primary files, secondary files, key functions, how to extend |
| [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) | Cold-path capture, warm-path capture, Case A/A2/B, receiver discovery, MH walk, adapter graph, export pipeline — with diagrams and an end-to-end example |
| [05-validation-guide.md](05-validation-guide.md) | Copy-pasteable commands for every validation step including per-case checks and Spring Boot suite; known failure modes |
| [06-known-limitations.md](06-known-limitations.md) | All 17 known gaps; Gap #17 RESOLVED (loader dedup); Gaps #15 (HTTP-path) and #16 (agent starvation) remain open |
| [07-build-workflow-guide.md](07-build-workflow-guide.md) | Canonical repo/file paths, standard build sequence, env vars, common mistakes with diagnosis and fix |
| [08-phase2-causality-graph-design-review.md](08-phase2-causality-graph-design-review.md) | Phase 2 architectural review: causality graph MVP feasibility, gap analysis, new record types, roadmap |
| [10-phase2b-runtime-target-attribution-design.md](10-phase2b-runtime-target-attribution-design.md) | Phase 2b design review: `runtime_target` root-cause investigation, emission-site inventory (all in `methodHandles.cpp`), Option B implementation design |
| [09-running-tests-and-demos.md](09-running-tests-and-demos.md) | Quick-start: run 12-case suite, Spring Boot demo, ManyCore UI, and start_demo.sh remote tunnel |
| [11-demo-platform-design.md](11-demo-platform-design.md) | LLM Debugging Benchmark design: A/B agent benchmark, causality API spec (8 endpoints, implemented), 4 demo bug specs (reflection/proxy/lambda/polymorphic), implementation order, next tasks |
| [12-demo-bug-suite.md](12-demo-bug-suite.md) | Demo bug suite: 4 Spring Boot bugs (reflection/proxy/polymorphic/lambda), JSONL validation results, causality API calls for each, A/B TTID reduction table |
| [13-agent-benchmark-design.md](13-agent-benchmark-design.md) | A vs B benchmark design: controlled variables, scoring rubric (16 pts), harness, success criteria (≥3pt delta required) |
| [14-benchmark-results.md](14-benchmark-results.md) | Benchmark results: single-turn, Bugs 1–3, all TIE; no material advantage in single-turn mode; Bug 3 +1 (line precision via polymorphic dispatch data) |

---

## Feature-to-File Routing Table

| Feature / Capability | Primary source file | Secondary source file |
|---|---|---|
| Master on/off switch (`g_sg_enabled`) | `soroushProvenanceGraph.cpp` | `soroushProvenanceGraph.hpp` |
| JSONL export pipeline | `soroushProvenanceGraph.cpp` | — |
| `callsite_target` emission | `soroushProvenanceGraph.cpp` (`soroush_graph_generic_callsite`) | `linkResolver.cpp` (call sites) |
| `callsite_adapter_graph` emission | `soroushProvenanceGraph.cpp` (`soroush_graph_adapter_graph_callsite`) | `linkResolver.cpp` (`sg_walk_mh`) |
| `callsite_target_set` (GWT) emission | `soroushProvenanceGraph.cpp` (`soroush_graph_target_set_callsite`) | `linkResolver.cpp` |
| `hidden_class_identity` emission | `soroushProvenanceGraph.cpp` (`soroush_graph_hidden_identity`) | `klassFactory.cpp` (trigger) |
| `bytecode_artifact` emission | `soroushProvenanceGraph.cpp` (`soroush_graph_bytecode`) | `soroushClassfileRewriter.cpp` |
| `runtime_target` emission | `soroushProvenanceGraph.cpp` (`soroush_graph_linkage`) | `methodHandles.cpp` (`soroush_trace_membername_resolution` — Phase 2b fix target; NOT `linkResolver.cpp`) |
| `invokedynamic` cold-path capture | `linkResolver.cpp` (LambdaMetafactory linkage) | `soroushProvenanceGraph.cpp` (`soroush_graph_indy_callsite`) |
| `invokehandle` cold-path capture | `linkResolver.cpp` (`resolve_handle_call`) | — |
| `invokehandle` warm-path capture | `templateTable_aarch64.cpp` (`TemplateTable::invokehandle`) | `interpreterRuntime.cpp` (`sg_trace_mh_dispatch`) |
| `invokehandle` warm-path implementation | `linkResolver.cpp` (`sg_trace_mh_impl`) | — |
| `invokeinterface` cold-path capture | `linkResolver.cpp` (`runtime_resolve_interface_method`) | — |
| `invokeinterface` warm-path capture | `templateTable_aarch64.cpp` (`TemplateTable::invokeinterface`, normal itable path) | `interpreterRuntime.cpp` (`soroush_trace_ii_dispatch`) |
| `invokevirtual` cold-path capture | `linkResolver.cpp` (`runtime_resolve_virtual_method`) | — |
| `invokevirtual` warm-path capture | `templateTable_aarch64.cpp` (`TemplateTable::invokevirtual_helper`, non-final path) | `interpreterRuntime.cpp` (`soroush_trace_iv_dispatch`) |
| MH adapter chain walking | `linkResolver.cpp` (`sg_walk_mh`, `sg_walk_generic_bmh`) | — |
| Adapter semantic labeling | `linkResolver.cpp` (`sg_compute_node_semantic`, `sg_node_infer_semantic_type_conv`) | — |
| Case B (reflection accessor) detection | `linkResolver.cpp` (`resolve_handle_call` Case B branch) | — |
| Hidden class CRC capture | `klassFactory.cpp` (pre-parse) | `soroushProvenanceGraph.cpp` |
| Hidden class identity recording | `klassFactory.cpp` (post-`create_instance_klass`) | `soroushProvenanceGraph.cpp` |
| Bytecode rewriting | `soroushClassfileRewriter.cpp` | — |
| UI server + run management | `tools/manycore-ui/app.py` | — |
| JSONL indexing | `tools/manycore-ui/indexer.py` | — |
| UI frontend | `tools/manycore-ui/static/index.html` | `tools/manycore-ui/static/style.css` |

---

## Task-to-Validation Routing Table

| Task | Validation command / document |
|---|---|
| JVM built and libjvm deployed correctly | `java -version` → must show `jdk21u-export` in VM line |
| All 15 cases pass | `ManyCoreCasesMain` → `15/15 passed`; see [05-validation-guide.md Part 1](05-validation-guide.md) |
| Case04 warm-path hook works | BCI 67 → `Math.min` (not `Math.max`); see [05-validation-guide.md → Case 04](05-validation-guide.md) |
| Reflection (Case B) works | Case10 all 5 targets exact; see [05-validation-guide.md → Case 10](05-validation-guide.md) |
| Hidden class identity works | Case12 `HiddenClassTemplate+0x...` has non-zero CRC; see [05-validation-guide.md → Case 12](05-validation-guide.md) |
| JDK proxy capture works | Case11 `$Proxy0.add` → handler; see [05-validation-guide.md → Case 11](05-validation-guide.md) |
| Spring Boot integration | Exit 0, `=== validation complete ===`, 0 user-code diagnostics; see [05-validation-guide.md Part 3](05-validation-guide.md) |
| CGLIB proxy captured | `Application$$SpringCGLIB$$0.setBeanFactory` in JSONL; [05-validation-guide.md](05-validation-guide.md) |
| Case15 invokeinterface poly | BCI 54 → three `callsite_target` records (Dog/Cat/Bird), `static_label=blocked_multi_target`, `heuristic_edges_created=0` |
| Breadth validation (Areas 2,4,5,6,7) | See [05-validation-guide.md Part 6](05-validation-guide.md); 0 orphans, 0 heuristic edges per area |
| No regressions after a change | Run full suite: ManyCore 15 cases + Spring Boot validation |

---

## Common Traps → Which Doc Explains Them

| Trap | Doc | Section |
|---|---|---|
| Edited `jdk21u/src/...` instead of `jdk21u-export/src/...` | [07-build-workflow-guide.md](07-build-workflow-guide.md) | "My print/change never appears" |
| `linkResolver.cpp` is at `interpreter/`, not `classfile/` | [07-build-workflow-guide.md](07-build-workflow-guide.md) | "Key File Paths" |
| `make hotspot` does not update `jdk/lib/server/libjvm.dylib` | [07-build-workflow-guide.md](07-build-workflow-guide.md) | "Standard Build Sequence" |
| `lr` gets clobbered → `BootstrapMethodError` / NPE | [02-phase-history.md](02-phase-history.md) | "Milestone 6 — The lr Bug" |
| JRT_ENTRY parameter must be named `current` not `thread` | [02-phase-history.md](02-phase-history.md) | "Milestone 6 — JRT_ENTRY Bug" |
| `pre()` / `post()` must use `__ ` prefix in template table code | [02-phase-history.md](02-phase-history.md) | "Milestone 6 — pre/post Scope Bug" |
| CP cache index byte order — must use `sg_u2at(bcp)` not manual big-endian | [02-phase-history.md](02-phase-history.md) | "Milestone 5 — Root Cause" |
| Diagnostic blocks exact record (dedup first-wins) | [06-known-limitations.md](06-known-limitations.md) | "Dedup Is First-Wins" |
| Hidden class name has no `+0x` suffix (called too early) | [02-phase-history.md](02-phase-history.md) | "Milestone 2 — Lessons Learned" |
| Spring Boot hangs instead of exiting | [05-validation-guide.md](05-validation-guide.md) | "Known Failure Modes" |
| `SOROUSH_EXPORT_RUNTIME_TARGETS` set but `SOROUSH_PROVENANCE_GRAPH` not set | [07-build-workflow-guide.md](07-build-workflow-guide.md) | "Environment Variables" |
| `rmethod` saved twice — don't add explicit save around `call_VM` in warm hook | [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) | "Why lr must be saved" |

---

## Recommended Reading Paths

### Track 1 — 10-minute onboarding (just arrived, need to understand quickly)

1. This document (AGENT_NAVIGATOR.md) — you are here
2. [00-agent-handoff.md](00-agent-handoff.md) — full status, quick validation, key traps

Done. You can now run the validation suite and understand the project state.

---

### Track 2 — 30-minute onboarding (need to make a change soon)

1. This document
2. [00-agent-handoff.md](00-agent-handoff.md) — full status
3. [01-project-overview.md](01-project-overview.md) — goals and record types
4. [03-source-ownership-map.md](03-source-ownership-map.md) — find the file you need to edit
5. [07-build-workflow-guide.md](07-build-workflow-guide.md) — build and deploy correctly

Done. You understand the goal, know where the code is, and can build it.

---

### Track 3 — Deep technical onboarding (need to understand the instrumentation before changing it)

1. This document
2. [00-agent-handoff.md](00-agent-handoff.md)
3. [01-project-overview.md](01-project-overview.md)
4. [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — the full technical picture
5. [02-phase-history.md](02-phase-history.md) — why it was built this way
6. [03-source-ownership-map.md](03-source-ownership-map.md) — all key functions
7. [06-known-limitations.md](06-known-limitations.md) — what not to break

Done. You have a complete mental model of the system.

---

### Track 4 — Build/debug only (JVM not building or not producing output)

1. [07-build-workflow-guide.md](07-build-workflow-guide.md) — read entirely
2. [05-validation-guide.md](05-validation-guide.md) — run validation to confirm fix

If the build passes but output is wrong:
3. [02-phase-history.md](02-phase-history.md) — "Common Traps" in Milestone 5 and 6

---

### Track 5 — Phase 2 continuation (Phase 1 is verified, starting new work)

1. This document — confirm current status
2. [08-phase2-causality-graph-design-review.md](08-phase2-causality-graph-design-review.md) — full Phase 2 readiness review, roadmap, and highest-leverage next step
3. [00-agent-handoff.md](00-agent-handoff.md) — "Recommended Next Tasks" section
4. [06-known-limitations.md](06-known-limitations.md) — pick a limitation to fix; read its "Phase 2 fix" guidance
5. [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — understand the subsystem you're extending
6. [03-source-ownership-map.md](03-source-ownership-map.md) — find the right files
7. [05-validation-guide.md](05-validation-guide.md) — run baseline before starting, run regression after
