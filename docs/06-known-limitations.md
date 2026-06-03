# Known Limitations

This document catalogs every known gap in the instrumentation as of Phase 1 completion (2026-05-29).

For historical context on how these were found, see [02-phase-history.md](02-phase-history.md).  
For Phase 2 recommendations, see [00-agent-handoff.md](00-agent-handoff.md).

---

## 1. Reflection Callsite Attribution — Partially Resolved

**What works**: Two dedicated categories exist for reflection dispatch:
- `reflection_method_invoke` — `Method.invoke(...)` calls captured via Case B in `linkResolver.cpp` (lines 3966–3967), when a `DirectMethodHandleAccessor` frame is at the top of the call stack
- `reflection_constructor_newInstance` — `Constructor.newInstance(...)` calls captured similarly via `DirectConstructorHandleAccessor`

Both have full source attribution: `source_class`, `source_method`, `source_bci`, `source_opcode=invokevirtual`. In the Spring Boot Phase 1 run: 7 `reflection_method_invoke` records and 3 `reflection_constructor_newInstance` records, all with exact source and target fields.

**What remains incomplete**: When `Method.invoke` is dispatched through the MethodHandle warm path (i.e., the MH infrastructure invokes the accessor via `invokehandle`), the warm-path hook fires first and records the dispatch as `category=methodhandle_invokeExact`. The user frame is still correctly attributed as the source (the vframeStream walk in Case A2 skips the accessor frame), but the category label reflects the dispatch mechanism (`invokehandle`) rather than the intent (`reflection`).

**Impact on staticization**: None. The target is captured exactly in both cases. The source class/method/BCI are correct in both cases. The category distinction matters only for analysis that classifies dispatch paths by mechanism (e.g., "how many callsites in this application use reflection?").

**Phase 2 fix**: In `sg_trace_mh_impl`, detect when the recovered user frame's invoke target is a `DirectMethodHandleAccessor` or `DirectConstructorHandleAccessor`. Emit an additional record (or override the category) with `category=reflection_method_invoke` instead of `methodhandle_invokeExact`.

---

## 2. `DirectMethodHandle$StaticAccessor` Adapter Shape

**What happens**: Two diagnostics from production code:
- `org/springframework/cglib/proxy/Enhancer.wrapCachedClass` bci=43 → `adapter_unknown_shape adapter_class=java/lang/invoke/DirectMethodHandle$StaticAccessor`
- `jdk/internal/reflect/MethodHandleObjectFieldAccessorImpl.set` bci=29 → same

**Root cause**: `sg_walk_mh` handles `DirectMethodHandle` (instance/virtual), `DirectMethodHandle$Accessor` (field get/set), and `BoundMethodHandle$Species_*`. It does NOT handle `DirectMethodHandle$StaticAccessor` — the variant for static field accessors via MH. When `sg_walk_mh` encounters this class, it emits `adapter_unknown_shape`.

**Impact on staticization**: These are framework-internal call sites. Neither appears in user code in any tested application. No user-code staticization is blocked.

**Phase 2 fix**: Add a branch in `sg_walk_mh` for `DirectMethodHandle$StaticAccessor`. Extract the static field's class/name/descriptor from the `member` field (same as `DirectMethodHandle` but the lookup is a static field). Emit a `primary_target` node with the static field's owning class.

---

## 3. Receiver Flows Through Method Return Value or Field Load

**Reason code**: `recv_from_method_result_or_field`

**What happens**: The receiver analyzer reads local variable slots of the interpreter frame to find the MH receiver. If the receiver was obtained from a method call or field load, it is not in a stable local slot — it is on the operand stack, which the static analyzer does not track. The analyzer emits a diagnostic.

**Frequency**: 13 diagnostics in Spring Boot run, all in framework or JDK code.

**Examples**:
- `java/util/concurrent/atomic/AtomicBoolean.compareAndSet` — receiver is the VarHandle returned by `VarHandle.findVarHandle()`
- `org/springframework/context/support/DefaultLifecycleProcessor.lambda$startBeans$0` — receiver is a lambda captured inside another lambda

**Impact**: Zero user-code impact in Phase 1. May arise in user code if a user writes:
```java
getSomeMethodHandle().invokeExact(arg);
// The MH is from a method return — not recoverable by static analysis.
// Warm-path hook fires → sg_trace_mh_impl reads from live stack → still recovers it correctly.
```

**Note**: The warm-path hook DOES recover these cases correctly, because it reads `r2` (the live receiver in a register), not a local slot. The `recv_from_method_result_or_field` diagnostic occurs only on the cold path when `sg_trace_mh_impl` is NOT the source (i.e., for `resolve_handle_call` invocations where the warm-path hook hasn't fired yet). The invokeinterface warm-path hook (Phase 2D) does not have this limitation — it reads `rmethod` directly from the itable lookup result.

**Phase 2 fix**: Extend the receiver analyzer to track the operand stack state, not just local slots. This is a significant undertaking (requires a mini abstract interpreter).

---

## 4. Backward Branch Before invoke (Loop-Carried Receivers)

**Reason codes**: `backward_goto_at_N_target_M`, `recv_analyzer_return_before_invoke_N`

**What happens**: The receiver analyzer cannot determine the receiver when there is a backward branch (loop) or an early return before the invoke bytecode in the analyzed method. These patterns make the receiver potentially loop-variable-dependent or path-dependent.

**Examples**:
- `java/util/concurrent/ConcurrentLinkedDeque.linkFirst` — CAS retry loop with a backward branch
- `org/springframework/boot/context/properties/bind/Binder.bindDataObject` — conditional return before invoke

**Impact**: Framework and JDK code only. No user-code instances in Phase 1.

---

## 5. SP Underflow During Adapter Construction

**Reason code**: `recv_slot_oob sp=N arg_slots=M`

**What happens**: When `resolve_handle_call` fires during the construction of a MethodHandle adapter chain, the top Java frame is a `java/lang/invoke/` internal frame. In Case A2, the receiver is recovered from the internal frame's `local[0]`. In some adapter construction scenarios, the frame's SP is adjusted in ways the analyzer does not expect, causing `last_Java_sp - arg_slots * wordSize` to fall outside the frame.

**Impact**: Framework and JDK code only. The guard `src_is_mh_factory_a2` suppresses these diagnostics when they arise from adapter construction (the code in `resolve_handle_call` Part B handles this case).

---

## 6. fast_multiop Bytecode (0xde)

**Reason code**: `unsupported_fast_multiop_0xde_at_N`

**What happens**: HotSpot's bytecode rewriter replaces some bytecodes with fast variants (opcodes > 0xb9). The receiver analyzer does not model fast opcodes other than the basic fast_invoke variants. When it encounters `0xde`, it cannot continue the analysis.

**Impact**: `java/util/concurrent/ConcurrentLinkedQueue.offer` and `ConcurrentLinkedDeque.linkFirst` in the JDK. No user-code impact.

---

## 7. JIT-Compiled Frame Unavailability

**Reason code**: `source_compiled_frame_unavailable`

**What happens**: One diagnostic in the mixed-mode Spring Boot run: a call site where the caller frame is JIT-compiled. When it occurs, the frame has no interpreter-frame metadata, and the source class/method/BCI cannot be recovered.

**Note**: The UI always passes `-Xint`. The standard manual validation commands in `docs/00-agent-handoff.md` and `docs/05-validation-guide.md` do NOT pass `-Xint`. Both the 12-case test suite and the Spring Boot manual validation run in **mixed mode** (JIT active). For the current short-lived workloads, the fastdebug JIT rarely fires before exit, so only 1 compiled-frame diagnostic was observed in the Spring Boot run. Under a long-running or warmed-up workload, this count will grow.

**Impact on manual runs**: Add `-Xint` to any manual validation command to eliminate compiled-frame diagnostics and ensure fully interpreter-based capture. Without `-Xint`, captured coverage depends on whether the JIT compiled a method before the callsite was first resolved.

---

## 8. `reconstructable=false` for Lambda Captures

**What it means**: A lambda `invokedynamic` callsite has `reconstructable=false` when the lambda captures a value that is not a compile-time constant. At AOT build time, the captured value is not resolvable without runtime data.

**Examples**:
- `Application.commandLineRunner` bci=1 — captures `ctx` (ApplicationContext, a runtime bean)
- `Application.validationRunner` bci=2 — captures `this` (the Application instance) and `ctx`

**Impact on staticization**: These lambdas can be identified as `CommandLineRunner` instances but cannot be reconstructed as static constants without runtime data. Staticization is not possible for these specific callsites. The `staticizable=true` flag only means the dispatch can be made direct; `reconstructable` governs whether the lambda itself can be made a static constant.

**Phase 2 scope**: For most production lambdas that are effectively constant (stateless, or capturing only other constants), `reconstructable=false` is an overly conservative assessment. The Phase 2 reconstruction step should analyze captured fields to identify cases where reconstruction is actually possible.

---

## ~~9. invokeinterface Megamorphic Call Sites~~ — RESOLVED (Phase 2D warm-path hook)

**Status**: RESOLVED. The warm-path hook in `TemplateTable::invokeinterface` normal itable path (`templateTable_aarch64.cpp`) fires on every interpreted itable dispatch. All concrete receiver types observed at a BCI are captured over time and emitted via `soroush_graph_poly_callsite`. Both the cold-path (`runtime_resolve_interface_method`) and warm-path (`soroush_trace_ii_dispatch`) now use `soroush_graph_poly_callsite`, which deduplicates by the full (source, target) pair — one record per unique (BCI, target class) combination.

**Validation**: Case15 BCI 54 → three `callsite_target` records (Dog.sound, Cat.sound, Bird.sound), all `source_opcode=invokeinterface`. Graph node: `static_label=blocked_multi_target` (SR_MULTI_TARGET). `heuristic_edges_created=0`. Spring Boot: 11,956 invokeinterface records captured without crash.

**How it was fixed**: Same dual cold+warm architecture as invokevirtual Phase 2C. Assembly hook in `TemplateTable::invokeinterface` normal itable path (after `profile_arguments_type`, before `jump_from_interpreted`). Saves/restores `lr`, calls `InterpreterRuntime::soroush_trace_ii_dispatch(thread, rmethod)`. The JRT_ENTRY guards `op == _invokeinterface`. Cold-path hook migrated from `soroush_graph_generic_callsite` to `soroush_graph_poly_callsite`.

---

## 10. Multi-Classloader Scenarios

**What it means**: All Phase 1 tests run with a single user classloader. The dedup keys include `source_loader_id` and `target_loader_id`, and the bytecode artifact registry is keyed by `(class_name, loader_id)`. The system is architecturally correct for multi-classloader scenarios.

**What is not tested**: OSGi, multi-tenant deployments, or applications that define the same class in multiple classloaders. The indexer's `find_best_artifact` function uses loader_id matching but has not been tested under loader isolation.

---

## 11. Intentional Filtering: JDK-Internal Classes

The invokeinterface hook explicitly skips frames whose holder starts with `java/`, `jdk/`, `sun/`, or `com/sun/`. This is intentional: JDK-internal `invokeinterface` dispatches are very high volume and are not relevant to user-code staticization.

If a future use case requires capturing JDK-internal call sites (e.g., for JDK bootstrap analysis), this filter must be relaxed or made configurable.

---

## 12. Dedup Semantics Differ by Side Table

Two dedup models exist in the system:

**First-in-wins on source BCI** (`g_gen_buckets`, `g_ts_buckets`, `g_ag_buckets`):
- Dedup key: `(source_class, source_method, source_descriptor, source_bci)`. Target not included.
- Used by: `soroush_graph_generic_callsite()` — invoked by the invokeinterface cold-path hook and the invokehandle warm-path hook.
- At most one record per source BCI. If multiple targets are observed at the same BCI, only the first is stored. Subsequent targets are silently dropped.
- Upgrade path: if the first record was a diagnostic (`exact=false`) and a later exact record arrives for the same BCI, the entry is upgraded in place. Diagnostics do not permanently suppress exact records.

**First-in-wins on (source BCI, target)** (`g_poly_buckets`):
- Dedup key: `(source_class, source_method, source_descriptor, source_bci, target_class, target_method, target_descriptor)`.
- Used by: `soroush_graph_poly_callsite()` — to be invoked by the invokevirtual cold-path hook (Phase 2C).
- Multiple records per source BCI when different targets are observed. All observed targets are stored.
- The graph builder's Pass 1.5 upgrades callsite nodes with more than one distinct `CALLSITE_TARGET` destination to `static_label=blocked_multi_target`.

**Risk (for first-in-wins tables)**: Any future code that emits a diagnostic for a user-code BCI before the warm-path hook fires would cause that BCI to retain a diagnostic record. Avoid pre-emptive diagnostic emission for user call sites. This risk does not apply to `g_poly_buckets` since it never stores diagnostics.

---

## 13. `runtime_target` Records Have No Source BCI — **RESOLVED IN PHASE 2B**

**What was happening**: `runtime_target` records had `caller_context` (a hardcoded string) but no `source_class`, `source_method`, or `source_bci` fields. These records were causality orphans.

**Phase 2B fix (COMPLETE)**: `soroush_trace_membername_resolution()` in `methodHandles.cpp` now walks the Java call stack via `vframeStream` at every `soroush_graph_linkage()` call site. The walk skips `java/lang/invoke/`, `java/lang/reflect/`, `jdk/internal/reflect/`, and `sun/reflect/` frames to find the first attributable caller frame.

**New schema fields**:
- `source_capture="exact"` + `source_class`, `source_method`, `source_descriptor`, `source_bci`, `source_loader_id` — when a caller frame is found
- `source_capture="missing"` + `source_missing_reason="no_user_frame_on_stack"` — when no frame is available (JVM init, daemon threads)

**Validation results** (Phase 2B, 2026-05-30):
- Runtime Truth: 915 orphans → **0 orphans** (100% reduction), 915 connected
- Spring Boot: 2,905 orphans → **0 orphans** (100% reduction), 2,912 connected

**Remaining gap**: `source_capture=missing` records (zero in current workloads, but theoretically possible during JVM boot or daemon thread MH linkage) remain orphans. This is correct behavior — no fabricated attribution.

**Impact on causality graph**: All `runtime_target` records with `source_capture=exact` now produce `CALLSITE_RT_ATTRIBUTED` edges in `graph_builder.py`. The causality chain for user code is fully connected through MH linkage events.

See [10-phase2b-runtime-target-attribution-design.md](10-phase2b-runtime-target-attribution-design.md) for the complete design review and investigation.

---

## 14. ~~Polymorphic `invokevirtual` — Cold Path Captures Only First-Resolved Target~~ — RESOLVED (Phase 2C warm-path hook)

**Status**: RESOLVED. The warm-path hook in `TemplateTable::invokevirtual_helper` (`templateTable_aarch64.cpp`) fires on every interpreted non-final vtable dispatch. All concrete receiver types observed at a BCI are captured over time and emitted via `soroush_graph_poly_callsite`. Case14 validates: BCI 54 produces three `callsite_target` records (Dog.sound, Cat.sound, Bird.sound), graph node gets `static_label=blocked_multi_target` (SR_MULTI_TARGET), `heuristic_edges_created=0`.

**What was the problem**: The cold-path hook (`runtime_resolve_virtual_method`) fires only once per CP cache entry. Subsequent dispatches at the same BCI with different receiver types bypassed it.

**How it was fixed**: Assembly hook in `invokevirtual_helper` non-final path (after `lookup_virtual_method`, before `jump_from_interpreted`). Saves/restores `lr` (set by `prepare_invoke`), calls `InterpreterRuntime::soroush_trace_iv_dispatch(thread, rmethod)`. The JRT_ENTRY guards `op == _invokevirtual` to exclude invokeinterface forced-virtual cases.

---

## ~~15. HTTP-Path Controller Dispatch — Spring MVC~~ — RESOLVED (Phase 2E, 2026-05-30)

**Discovered**: Breadth validation pass, 2026-05-30.
**Root cause confirmed**: 2026-05-30. **RESOLVED**: 2026-05-30 (Phase 2E warm-path reflection hook).

**Status**: RESOLVED. `InvocableHandlerMethod.doInvoke bci=55 --[reflection_method_invoke]--> HelloController.index` is now present in HTTP-driven runs and absent in startup-only runs. The edge is consistent across mixed-mode and `-Xint` runs.

**What was captured before Phase 2E (intermediate chain)**: The Spring MVC HTTP dispatch chain was fully captured by invokevirtual/invokeinterface warm-path hooks, but the final attribution edge was missing:
```
DispatcherServlet.doDispatch bci=184 --[invokeinterface]--> AbstractHandlerMethodAdapter.handle
  AbstractHandlerMethodAdapter.handle bci=7 --[invokevirtual]--> RequestMappingHandlerAdapter.handleInternal
    ... Spring internals ...
      InvocableHandlerMethod.doInvoke bci=55 --[invokevirtual]--> java/lang/reflect/Method.invoke
      InvocableHandlerMethod.doInvoke bci=55 --[reflection_method_invoke]--> ??? ← MISSING
```

**Root cause** (confirmed, 2026-05-30): `MemberName.resolve` (the cold-path hook) fires once per method target — a single-fire event. Startup validation code resolved `HelloController.index` before HTTP requests arrived, attributing that target to `Application.lambda$validationRunner$3`. All subsequent `Method.invoke` calls found a pre-resolved `MemberName`, firing no hook. `-Xint` produced identical missing-edge behavior — the issue was MemberName caching, not JIT compilation.

**Fix applied (Phase 2E, 2026-05-30)**:
- `soroush_trace_iv_dispatch` in `interpreterRuntime.cpp` now receives `recv_oop` (r2 = the `java.lang.reflect.Method` object at the `invokevirtual` call site).
- When `concrete_method == java/lang/reflect/Method.invoke`, the code decodes `recv_oop` via `java_lang_reflect_Method::clazz()` + `java_lang_reflect_Method::slot()` + `InstanceKlass::method_with_idnum()` to find the actual reflected target.
- Emits a `callsite_target` record with `category=reflection_method_invoke`, deduplicated per `(src_class, src_loader, src_method, src_desc, src_bci, tgt_class, tgt_loader, tgt_method, tgt_desc)` via `soroush_graph_poly_callsite`.
- Template table passes `recv` (= r2) as arg2 to `call_VM` in `invokevirtual_helper` non-final path (`templateTable_aarch64.cpp`).

**Validation results**:
| Run | `doInvoke bci=55 → HelloController.index` | `validationRunner bci=59 → HelloController.index` |
|-----|:---:|:---:|
| startup-only | **0** (correct: no HTTP requests) | 1 |
| mixed+HTTP (5 requests) | **1** (Gap #15 fixed) | 1 |
| -Xint+HTTP (5 requests) | **1** (consistent) | 1 |

**Source files changed**:
- `src/hotspot/share/interpreter/interpreterRuntime.cpp` — `soroush_trace_iv_dispatch` Phase 2E block
- `src/hotspot/share/interpreter/interpreterRuntime.hpp` — `recv_oop` parameter added
- `src/hotspot/cpu/aarch64/templateTable_aarch64.cpp` — `recv` passed as arg2 in `call_VM`

---

## ~~16. Dynamic Agent Attachment Starvation~~ — WORKAROUND CONFIRMED (Gap #16, 2026-05-30)

**Discovered**: Breadth validation pass, 2026-05-30.
**Workaround confirmed**: Gap #16 investigation, 2026-05-30.

**Status**: This is a **validation environment issue, not a capture architecture bug**. Adding `-javaagent:byte-buddy-agent.jar` to the JVM startup command completely eliminates the deadlock. With the preloaded agent, Mockito mocks are created successfully and all provenance records are captured correctly with `heuristic_edges_created=0`, `orphans=0`, and `validation fail=0`.

**Root cause**: Mockito 5 with the default `InlineByteBuddyMockMaker` uses `VirtualMachine.attach()` to dynamically load a Java agent at runtime. This requires the JVM to reach a safepoint. With `SOROUSH_PROVENANCE_GRAPH=1` enabled, the warm-path hooks (`soroush_trace_iv_dispatch`, `soroush_trace_ii_dispatch`) fire on every `invokevirtual`/`invokeinterface`, performing `vframeStream` stack walks. Under the fastdebug JVM, assertion-check overhead per invocation is high enough that the JVM cannot service the safepoint request within any observable timeout — deadlock ensues.

**Classification**: This is a **validation/agent-attachment issue**, not a capture architecture bug. The JVM hooks do not interfere with Mockito's class redefinition or bytecode generation; only the dynamic-attach safepoint path is affected. The capture architecture itself is sound.

**Investigation matrix results (2026-05-30)**:

| Run | Provenance | Agent loading | Result | Duration |
|-----|:---:|:---:|:---:|:---:|
| 1 | ON | dynamic `VirtualMachine.attach()` | **HUNG (deadlock at 45s)** | ∞ |
| 2 | ON | preloaded `-javaagent` | **PASS** | 3s |
| 3 | OFF | dynamic `VirtualMachine.attach()` | PASS | 2s |
| 4 | OFF | preloaded `-javaagent` | PASS | 1s |

**What is captured with the preloaded agent (Run 2)**:
- `Area3_Mockito$Calculator$MockitoMock$3UzkTlCQ` bytecode artifact captured (CRC `d48db186`, size 3580 bytes, kind=original)
- Two auxiliary mock classes captured (`$auxiliary$huxOfIIw`, `$auxiliary$MXIc59QN`)
- All 5 mock interface dispatch edges captured (`invokeinterface` from `Area3_Mockito.run` to mock class at BCIs 12, 35, 54, 63, 70)
- Spy dispatch edges captured (`Area3_Mockito.run → RealCalculator.add/multiply`)
- `heuristic_edges_created=0`, `orphan runtime_targets=0`, `validation fail=0`
- 12,563 callsite_target edges, complete=true

**Workaround** (no JVM code change required):
```bash
BBA=$HOME/.m2/repository/net/bytebuddy/byte-buddy-agent/1.14.15/byte-buddy-agent-1.14.15.jar
SOROUSH_PROVENANCE_GRAPH=1 SOROUSH_EXPORT_RUNTIME_TARGETS=out.jsonl \
  $JAVA -javaagent:$BBA -cp "$CP" breadthval.RunArea 3
```

**No JVM code changes required.** The capture architecture correctly captures mock class bytecode, dispatch edges, and spy dispatch when the agent is preloaded.

---

## ~~17. Same-Class-Name Dedup Collision Across Classloaders~~ — RESOLVED (Gap #17 fix, 2026-05-30)

**Discovered**: Breadth validation pass, Area5 (Custom ClassLoader isolation), 2026-05-30.

**Status**: RESOLVED. `soroush_graph_poly_callsite` dedup key now includes both `src_loader_id` and `target_loader_id` in both the hash computation and the equality check. Two classes with the same binary name but different classloaders dispatched from the same BCI each produce a distinct `callsite_target` record and a distinct method node in the graph.

**What was happening**: `soroush_graph_poly_callsite` deduplicated by `(source_class, source_method, source_bci, target_class_name, target_method, target_descriptor)`. The dedup key used `target_class_name` as a C-string without `target_loader_id`. When two classes with the same binary name but different classloaders were dispatched from the same BCI, the first target's record was stored and subsequent records with the same class name were dropped.

**Fix applied** (`soroushProvenanceGraph.cpp`):
- Hash extended: `^ (uint32_t)(src_loader_id ^ (src_loader_id >> 32)) ^ (uint32_t)(target_loader_id ^ (target_loader_id >> 32))`
- Equality check extended: added `src_loader_id == c->src_loader_id && target_loader_id == c->target_loader_id`

**Validation — Area8 (Loader-Aware Dedup)**:
- `area8.jsonl` emits two `callsite_target` records at `source_bci=187`, both targeting `breadthval/PolyGreeter::greet::()Ljava/lang/String;` but with different `target_loader_id` values:
  - loaderA: `0x00000001056323d0`
  - loaderB: `0x0000000105629ee0`
- Graph: 1 callsite node, 2 distinct method nodes (differing only by `loader_id`), 2 `CALLSITE_TARGET` edges.
- `static_label = blocked_multi_target`, `heuristic_edges_created = 0`.
- 26/26 graph builder unit tests pass. 15/15 test cases pass.

---

## Summary Table

| # | Limitation | User code impact | Phase 2 fix? |
|---|---|---|---|
| 1 | Reflection category label (MH warm path uses `methodhandle_invokeExact`) | None (target and source are correct) | Yes — detect accessor frame and override category |
| 2 | `StaticAccessor` unknown shape | None (framework only) | Yes — model StaticAccessor in sg_walk_mh |
| 3 | Receiver from method return | Theoretical (warm-path recovers at runtime) | Operand stack tracking |
| 4 | Backward branch (loop) | None observed | Mini abstract interpreter |
| 5 | SP underflow (adapter construction) | None | Already suppressed for A2 case |
| 6 | fast_multiop bytecode | None | Model fast opcodes |
| 7 | Compiled frame unavailable | Under mixed-mode (JIT active). Manual commands do NOT use -Xint. | Add -Xint to manual commands; or accept 1 compiled-frame diagnostic in fastdebug short runs |
| 8 | `reconstructable=false` for captures | Does not block target capture | Phase 2 reconstruction analysis |
| 9 | ~~Megamorphic invokeinterface — first target wins~~ | **RESOLVED** — Phase 2D warm-path hook | **DONE** (2026-05-30) |
| 10 | ~~Multi-classloader — loader-id dedup collision~~ | **RESOLVED** — Gap #17 fix: dedup key now includes both src_loader_id and target_loader_id | **DONE** (2026-05-30) |
| 11 | JDK-internal dispatch filtered | By design — not a gap | Relax filter if needed |
| 12 | Dedup semantics differ by table | `g_gen_buckets`: first-target-wins (invokehandle). `g_poly_buckets`: per-(BCI,target) (invokevirtual Phase 2C + invokeinterface Phase 2D). | Pre-emptive diagnostics risk applies only to `g_gen_buckets` |
| 13 | `runtime_target` has no source BCI | **RESOLVED** — 0 orphans after Phase 2B | vframeStream walk in `methodHandles.cpp` adds `source_capture=exact/missing` fields |
| 14 | ~~Polymorphic `invokevirtual` — cold path captures only first-resolved target~~ | **RESOLVED** — Phase 2C warm-path hook | **DONE** (2026-05-30) |
| 15 | ~~HTTP-path controller dispatch — missing reflection attribution~~ | **RESOLVED** — Phase 2E warm-path reflection hook: `soroush_trace_iv_dispatch` decodes `java.lang.reflect.Method` recv_oop to emit `reflection_method_invoke` record per call. `InvocableHandlerMethod.doInvoke bci=55 → HelloController.index` now present in HTTP runs. | **DONE** (2026-05-30) |
| 16 | ~~Dynamic agent attachment starvation~~ | **WORKAROUND** — validation env issue (not capture bug). `-javaagent:byte-buddy-agent.jar` eliminates deadlock. Mockito PROVEN_COVERED with preloaded agent: mock bytecode + dispatch edges captured, 0 heuristics. | Pre-load agent with `-javaagent` (2026-05-30) |
| 17 | ~~Same-class-name dedup collision across loaders~~ | **RESOLVED** — Gap #17 fix (2026-05-30) | Area8 breadth-val + graph builder test 23 |
