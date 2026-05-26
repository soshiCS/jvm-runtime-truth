# Soroush Custom JVM — Reading Guide

> A map for reading and understanding this project without getting lost.
> This is **not** a changelog and **not** an implementation dump — it explains
> *what* exists, *why* it exists, *how* we approached it, and *where the code lives*.
>
> For the authoritative deep-dive on any subsystem, read
> [`SOROUSH_JVM_SPEC.md`](SOROUSH_JVM_SPEC.md) and the cross-session memory `.md`
> files (paths listed in §"Deeper docs" entries below).
>
> **Line ranges are approximate as of the current working tree** (master, atop
> commit `9ca2ecfe356`). They will drift as the code changes — use them as a
> "look around here," then grep for the named function to confirm.

---

## 1. Project overview

This is a fork of **OpenJDK jdk21u (HotSpot)** modified to expose **runtime
provenance** — information about how dynamically-generated and
dynamically-transformed code actually ran, which ordinary JVMs discard.

**What "runtime provenance" means here:** the ability to answer, after the fact,
questions like *"this lambda/proxy/generated class — where did it come from, who
generated it, what bytecode actually executed, and what called it across which
threads?"* Normal stack traces and tools (JFR, debuggers) lose this for dynamic
behavior.

**Why dynamic Java behavior is hard to understand:**
- `invokedynamic` / `LambdaMetafactory` synthesize call sites and hidden lambda
  classes at runtime — the bytecode you wrote is not the bytecode that runs.
- Reflection and `MethodHandle` dispatch route calls through adapter chains and
  generated `LambdaForm`s that don't appear in source.
- Frameworks (ByteBuddy, CGLIB, Spring) generate/transform classes on the fly.
- JVMTI / `ClassFileLoadHook` / redefine/retransform rewrite bytecode after you
  compiled it.
- Causality crosses thread boundaries (executors, `CompletableFuture`), which
  stack traces never capture.

**What this engine can currently capture:**
- The exact final bytecode that executed (post all transforms).
- Recovered runtime-generated classes (hidden/lambda/proxy/ByteBuddy/CGLIB) with
  provenance sidecars, dumped to `/tmp/soroush_jvm_dump`.
- `invokedynamic`/`LambdaMetafactory` linkage and reflection/MethodHandle
  linkage + adapter execution.
- A persistent ENTER/EXIT execution call graph with **exact** method identity
  (class, name, descriptor, defining loader).
- Cross-thread / async causality (Thread.start, executors, CompletableFuture).
- All of the above unified into one in-memory **provenance graph**.
- A **verifier-safe bytecode rewriter** that instruments methods at load time
  without ever producing bytecode that fails verification.

---

## 2. How to read the project

Recommended reading order (depth-first newcomers, follow top to bottom):

1. **[`SOROUSH_JVM_SPEC.md`](SOROUSH_JVM_SPEC.md)** §1–§4 — goal, the prime
   directive (verifier-safety over coverage), the rewriter, the trace ABI.
2. **This guide §3** — the subsystem map (why each piece exists + where it lives).
3. **Provenance graph model** — SPEC §4A, then `soroushProvenanceGraph.hpp`
   (node/edge enums + function contracts) before the `.cpp`.
4. **Bytecode rewriter** — SPEC §4 pipeline, then `soroushClassfileRewriter.cpp`
   in pipeline order (decode → build_pc_map → converge → emit → metadata).
5. **Runtime-generated class recovery** — `klassFactory.cpp`
   `recover_runtime_generated_class`.
6. **indy / reflection / MH tracing** — `systemDictionary.cpp`,
   `methodHandles.cpp`, `linkResolver.cpp`.
7. **Async causality** — SPEC §4B, then the `jvm.cpp` thread hooks + the JDK
   `java.util.concurrent` integration points.
8. **Demos / tests** — §4 of this guide; run `GraphDemo` first (it exercises
   every signal at once).

If you only have 20 minutes: read SPEC §1–§4A and skim §3 of this guide.

---

## 3. Subsystem map

The project has **7 top-level subsystems** (SPEC §3). Items 1–15 below break the
big ones (especially the rewriter, subsystem 6, and the graph, subsystem 7) into
the distinct implementation areas you'll actually navigate. Cross-references
note which top-level subsystem each belongs to.

Primary source files (full paths under `src/hotspot/share/`):
- `classfile/soroushClassfileRewriter.{cpp,hpp}` — the rewriter (4501 / 54 lines)
- `classfile/soroushProvenanceGraph.{cpp,hpp}` — the graph + JSONL export (1383 / 237 lines)
- `classfile/klassFactory.cpp` — class-load hook, recovery, rewriter invocation, bytecode capture (1008 lines)
- `prims/jvm.cpp` — execution-graph natives, token resolver, async thread hooks (4499 lines)
- `classfile/systemDictionary.cpp` — indy linkage
- `prims/methodHandles.cpp`, `interpreter/linkResolver.cpp` — reflection/MH linkage + MH execution walk
- `runtime/reflection.cpp` — reflective invoke tracing
- `runtime/java.cpp` — shutdown summary hook

---

### 3.1 Verifier-safe bytecode rewriter  *(subsystem 6 — the heart)*

**Why it exists.** To instrument arbitrary already-compiled classes (insert
ENTER/EXIT trace calls) at load time. HotSpot's own classfile machinery is not
reusable for this, and a wrong rewrite corrupts the classfile.

**Goal.** Parse → transform → re-emit any classfile such that the result *always*
passes `-Xverify:all`. The prime directive: **fail safe** (skip a method/class,
leaving it unchanged) rather than emit questionable bytecode.

**High-level approach.** A self-contained classfile parser/emitter (no dependency
on the rest of HotSpot). It decodes instructions, plans insertion points,
computes a stable new layout (handling branch-offset growth), emits, and remaps
all PC-bearing metadata through one shared `pc_map`. Anything it can't *prove*
safe is copied unchanged.

**Important files and line ranges** (`soroushClassfileRewriter.cpp`):
| Area | Function(s) | Lines (approx.) |
|------|-------------|-----------------|
| Byte read/write helpers | `soroush_read_u2/u4`, `write_*` | 8–24 |
| Instruction decode | `soroush_instruction_length`, `soroush_decode_code` | 266–374 |
| Base layout | `soroush_build_pc_map` | 482–564 |
| PC remap | `soroush_remap_pc`, `soroush_append_s2_checked` | 565–645 |
| Emit | `soroush_emit_rewritten_code` | 1066–1222 |
| **Main Phase 3/5 driver** | `soroush_transform_code_attribute_entry_code` | 3102–3604 |
| Per-method cp + token registration | `soroush_transform_member_entry_trace` | 3691–3785 |
| Class-level driver | `transform_class_entry_code` (static) | 4121–4483 |
| Public entry points | `insert_entry_trace`, `insert_entry_exit_trace`, `roundtrip_copy`, `insert_entry_nops` | 3786–4495 |

Public API: `soroushClassfileRewriter.hpp` (whole file, 54 lines).
Invocation: `klassFactory.cpp` PHASE5 at ~808, PHASE3 at ~844, PHASE2 at ~878.

**Env flags / logs.** `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1` +
`SOROUSH_REWRITER_PHASE5_PREFIX=<pkg>` (main path); lighter `PHASE1/2/3`. Logs:
`[JVM REWRITER PHASE5] … normal_exit=ok|failed`.

**Demos/tests.** Every stress demo (below) exercises the rewriter; broadest
coverage is the Spring Boot run. See §4.

**Deeper docs.** SPEC §2 (prime directive) + §4 (pipeline); memory
`feedback_verifier_safety_over_coverage.md`.

---

### 3.2 ENTER/EXIT execution tracing  *(subsystem 5)*

**Why it exists.** To record a persistent call graph (who called whom, on which
thread, how deep) — the runtime causality a stack trace only shows at one instant.

**Goal.** On every instrumented method entry and exit, emit an event with a call
id, parent id, thread, depth, and **exact method identity**.

**High-level approach.** The rewriter injects, per method, `ldc_w <Integer token>;
invokestatic System.soroushTraceEnter(int)` at entry and `…soroushTraceExit(int)`
at each return / synthetic exception handler. The token resolves at runtime to
the method's exact identity (see §3.14). The native maintains a ring buffer +
thread-local call stack.

**Important files and line ranges** (`prims/jvm.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Current exec id / ptr hash | `soroush_current_exec_id` | 364–374 |
| Event recorder (→ graph) | `soroush_runtime_record_event` | 494–616 |
| Token resolver | `soroush_trace_token` | 617–643 |
| Natives | `JVM_SoroushTraceEnter/Exit` | 644–652 |

Java decls: `java/lang/System.java` (`soroushTraceEnter/Exit`); registration
`System.c`; export `make/data/hotspot-symbols/symbols-unix`.

**Env flags / logs.** `SOROUSH_RUNTIME_GRAPH=1` → `[JVM TRACE] id=… parent=…
thread=… ENTER|EXIT <class>.<method>`. `[JVM TRACE METHOD]` (token register +
per-event human line) prints independently of `SOROUSH_RUNTIME_GRAPH`.

**Demos/tests.** `GraphDemo`, and every PHASE5 run. See §4.

**Deeper docs.** SPEC §4 "Trace ABI" + §4A "Exact execution identity"; memory
`project_provenance_graph.md`.

---

### 3.3 Constructor (`<init>`) instrumentation  *(subsystem 6)*

**Why it exists.** A constructor can't run ENTER before `this` is initialized —
the verifier forbids using an uninitialized `this`. So ENTER must go *after* the
`super()`/`this()` delegation call.

**Goal.** Instrument constructors safely: ENTER after delegation, normal EXIT
before `return`, exception-EXIT disabled (too risky for ctors), safe-skip any
shape we can't prove.

**High-level approach.** Detect the delegation `invokespecial <init>` (the first
one with no preceding `new`), classify it `super` vs `this`, thread an
"ENTER-after-this-PC" insertion point through build_pc_map/converge/emit. `<clinit>`
is skipped entirely.

**Important files and line ranges** (`soroushClassfileRewriter.cpp`):
| Area | Lines (approx.) |
|------|-----------------|
| `soroush_methodref_is_init` (delegation detection helper) | 226–256 |
| `is_constructor` handling in build_pc_map | 487–564 |
| `is_constructor` handling in converge | 906–1065 |
| `is_constructor` handling in emit | 1066–1222 |
| Constructor planning + safe-skip in main driver | 3120–3200 |

**Env flags / logs.** `[JVM REWRITER CTOR] method=… considered | delegation=super|this
| instrumented | safe-skip reason=…`.

**Demos/tests.** `CtorDemo.java` (7 cases: simple, branch, this()-chain,
throw-after-super, try/catch, pre-super arg, pre-super-`new` safe-skip).

**Deeper docs.** SPEC §4 pipeline step 0–2; memory `project_jvm_remaining_work.md`
item 1.

---

### 3.4 Branch widening  *(subsystem 6)*

**Why it exists.** Inserting ENTER/EXIT bytes shifts PCs, so a short branch
(`goto`/`if*`, s2 offset) can overflow its 16-bit range. Without widening, those
methods would have to be skipped.

**Goal.** Convert overflowing short branches to wide form so any method can be
instrumented: `goto→goto_w`, `jsr→jsr_w` (unreachable on modern classfiles), and
conditional `if*` → inverted-if + `goto_w`.

**High-level approach.** A two-pass design: (6A) detect+classify overflowing
branches; (6B) an **iterative fixed-point relaxation** (`soroush_converge_layout`)
that models widening until PCs/sizes stop changing, then writes the stable layout
back; (6C) conditional inversion, which also **synthesizes the required
fall-through StackMapTable frame** (copied from the branch target's frame, since
both edges share locals/stack).

**Important files and line ranges** (`soroushClassfileRewriter.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Classify / detect / log | `soroush_classify_branch_widening`, `_detect_branch_widening`, `_log_branch_widening` | 674–802 |
| Widen delta / invert op table | `soroush_branch_widen_delta`, `soroush_invert_conditional_op` | 803–849 |
| Fixed-point convergence | `soroush_converge_layout` | 906–1065 |
| Widened emit | within `soroush_emit_rewritten_code` | 1066–1222 |
| Widened StackMapTable | `soroush_emit_widened_stack_map_table` | 2445–2640 |

**Env flags / logs.** `[JVM REWRITER WIDEN] …` (detection), `[JVM REWRITER
CONVERGE] iter=… converged=… …`.

**Demos/tests.** `run-goto-widen-stress.sh`, `run-branch-widen-stress.sh`,
`run-cond-widen-stress.sh` (generators `*StressGen.java`; javap-verified wide
opcodes).

**Deeper docs.** SPEC §4 pipeline step 3; memory `project_jvm_remaining_work.md`
item 2.

---

### 3.5 StackMapTable / metadata remapping  *(subsystem 6)*

**Why it exists.** Inserting bytes invalidates every PC stored in the classfile's
Code attributes. The verifier reads StackMapTable; debuggers read LineNumberTable;
all must be remapped or the load fails.

**Goal.** Remap the six standard PC-bearing Code sub-attributes through the same
`pc_map`: StackMapTable, exception table, LineNumberTable, LocalVariableTable,
LocalVariableTypeTable, Runtime(In)VisibleTypeAnnotations.

**High-level approach.** Most tables are simple PC rewrites. StackMapTable is the
hard one: for widened conditionals it re-emits from **absolute frame states** as
full_frames (synthesizing the fall-through frame). Fails safe on
UninitializedThis/Uninitialized verification types.

**Important files and line ranges** (`soroushClassfileRewriter.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Verification-type copy/remap | `soroush_copy_verification_type[_mapped]`, `read/append` | 1223–1491 |
| StackMapTable transform (mapped) | `soroush_transform_stack_map_table_mapped` | 1653–1749 |
| Absolute-frame decode | `soroush_decode_absolute_frames` | 2284–2429 |
| Widened table emit | `soroush_emit_widened_stack_map_table` | 2445–2640 |
| LineNumber / LocalVariable mapped | `soroush_transform_line_number_table_mapped`, `_local_variable_table_mapped` | 2641–2698 |
| Type annotations mapped | `soroush_transform_type_annotations_mapped` | 2813–2929 |

**Env flags / logs.** No dedicated flag — part of every PHASE5/PHASE3 rewrite.

**Demos/tests.** `TypeAnnotationStressDemo.java`, plus all widening demos (which
stress StackMapTable synthesis).

**Deeper docs.** SPEC §4 pipeline step 6.

---

### 3.6 Exception-EXIT instrumentation  *(subsystem 6)*

**Why it exists.** A method can exit by throwing, not just returning. To trace
*exceptional* exit, we add synthetic catch-all handlers that emit EXIT then
re-throw.

**Goal.** Insert exception-EXIT handlers for ordinary methods (disabled for
constructors), without producing a handler stack-map frame the verifier rejects.

**High-level approach.** Synthesize catch-all handler regions + their
StackMapTable frames. **Conservative safety guard:** if a local slot is
overwritten with a *different type* inside a protected region, a single
locals-snapshot frame would be wrong, so the method is safe-skipped.

**Important files and line ranges** (`soroushClassfileRewriter.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Safe-skip guard (slot type reuse) | `soroush_store_writes_slot` | 1910–1936 |
| Add handler regions | `soroush_add_exception_handler_regions` | 1937–2040 |
| Build handler stack-map frames | `soroush_build_stack_map_exception_handlers` | 2041–2254 |
| Exit-prefix / handler emit | `soroush_append_exit_prefix`, `_append_exception_exit_handler` | 423–481 |

**Env flags / logs.** `[JVM REWRITER SAFE-SKIP] method=… slot=… first_type=…
conflicting_type=…`.

**Demos/tests.** `ExceptionExitStressDemo.java`; the real-world surfacing was
`TomcatWebServer.start` during Spring testing.

**Deeper docs.** memory `project_exception_exit_frame_bug.md` (the bug + the
conservative fix); SPEC §4 pipeline step 4.

---

### 3.7 Runtime-generated class recovery  *(subsystem 1)*

**Why it exists.** Hidden classes, lambdas, proxies, ByteBuddy/CGLIB classes
exist only in memory and vanish — you can't inspect what actually ran.

**Goal.** Dump their bytes to disk + a provenance sidecar describing *what
generated them* and *how we know*.

**High-level approach.** At class load, detect generated classes (hidden flag,
`$$Lambda` name, ProxyGenerator/ByteBuddy/CGLIB stack evidence), dump bytes to
`/tmp/soroush_jvm_dump`, and write sidecar fields. Provenance is classified
`exact | heuristic | unknown` — **exact requires observed runtime evidence**
(active indy trace, defineHiddenClass call, etc.).

**Important files and line ranges** (`classfile/klassFactory.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Dump path / crc | `soroush_class_dump_path`, `soroush_crc32` | 243–340 |
| Provenance classification | `soroush_recovery_provenance` | 367–508 |
| Recovery + graph hand-off | `recover_runtime_generated_class` | 509–605 |

**Env flags / logs.** `SOROUSH_RUNTIME_RECOVERY=1` → `[JVM DUMP]`, `[JVM RECOVER]`,
`[JVM INDY]`.

**Demos/tests.** `RuntimeGenerationDemo.java`; ByteBuddy/CGLIB/proxy paths via
Spring.

**Deeper docs.** memory `project_jvm_subsystems_status.md` §1; SPEC §3 row 1.

---

### 3.8 Final executable bytecode capture  *(subsystem 4)*

**Why it exists.** After JVMTI/ClassFileLoadHook/redefine/retransform (and our own
rewriter), the bytes that run differ from the bytes on disk. We capture the
*final* bytes.

**Goal.** Record post-all-transform bytecode + metadata (transformed flag, hidden,
load kind, sizes, crc32) and dump it.

**High-level approach.** At the end of the class-load path, use
`actual_stream->buffer()`/`length()` and log + dump. Feeds the graph's
`BytecodeArtifact` nodes + `REWRITTEN_FROM` edges (original→final).

**Important files and line ranges** (`classfile/klassFactory.cpp`):
| Area | Function / site | Lines (approx.) |
|------|-----------------|-----------------|
| Final bytes capture | `capture_final_class_bytes` | 304–340 |
| Graph bytecode artifact hand-off | `soroush_graph_bytecode` calls | 932–942 |

**Env flags / logs.** `SOROUSH_CAPTURE_FINAL_BYTECODE=1` → `[JVM FINAL BYTECODE]
… transformed/hidden/load_kind/original_size/final_size/crc32/dumped`.

**Demos/tests.** Implicitly every rewrite; `GraphDemo` shows the REWRITTEN_FROM
edge.

**Deeper docs.** memory `project_jvm_subsystems_status.md` §4. **Known gap:**
full redefine/retransform version lineage is not captured.

---

### 3.9 invokedynamic / LambdaMetafactory tracing  *(subsystem 2)*

**Why it exists.** `invokedynamic` is where lambdas/string-concat/etc. get their
real implementation at runtime — the linkage that connects a call site to a
generated class.

**Goal.** Trace the caller, indy descriptor, bootstrap method + args, impl target,
the hidden lambda class, and the final CallSite target — and **join** the indy
site to the generated class via a trace id.

**High-level approach.** Hook the bootstrap path; assign a thread-local indy trace
id (scoped via an RAII `SoroushIndyTraceScope`); the recovery path (§3.7) reads
that id so the generated lambda class links back to its indy site.

**Important files and line ranges** (`classfile/systemDictionary.cpp`):
| Area | Lines (approx.) |
|------|-----------------|
| Indy trace-id counter + thread-local + `SoroushIndyTraceScope` | 126–172 |
| MethodType / MemberName / MethodHandle printers | 182–240 |
| Bootstrap site hook + graph indy node | (search `soroush_graph_indy`) |

Graph side: `soroush_graph_indy` (`soroushProvenanceGraph.cpp` 507–531).

**Env flags / logs.** `SOROUSH_TRACE_INDY=1` → `[JVM INDY] hidden_class=… …`.

**Demos/tests.** `GraphDemo.java` (lambda → indy → GeneratedClass chain).

**Deeper docs.** SPEC §3 row 2; memory `project_jvm_subsystems_status.md` §2.

---

### 3.10 Reflection / MethodHandle linkage tracing  *(subsystem 3)*

**Why it exists.** Reflective `Method.invoke` and `MethodHandle` dispatch resolve
to a target method through machinery invisible in source.

**Goal.** Make `MemberName` resolution, runtime dispatch, the resolved method, and
the MH target visible — and link them to graph Method nodes.

**High-level approach.** Hook `MemberName.resolve` and reflective invoke; emit
`[JVM REFLECT]` lines + a `MethodHandleLinkage`/`ReflectionInvoke` graph node with
a `LINKS_TO` edge to the target Method (loader from the target holder's CLD).

**Important files and line ranges:**
| File | Function | Lines (approx.) |
|------|----------|-----------------|
| `prims/methodHandles.cpp` | `soroush_trace_membername_resolution` | 95–121 (call sites 260, 271, 849, 873) |
| `interpreter/linkResolver.cpp` | `soroush_trace_runtime_dispatch` | 273–… (call site 1650) |
| `runtime/reflection.cpp` | `soroush_trace_reflection_invoke` | 92–… (call site 1124) |

Graph side: `soroush_graph_linkage` (`soroushProvenanceGraph.cpp` 532–567).

**Env flags / logs.** `SOROUSH_TRACE_REFLECTION=1` → `[JVM REFLECT]
membername_source/target, dispatch_kind, resolved_method, methodhandle_dispatch,
mh_target`.

**Demos/tests.** `ReflectionDispatchDemo.java`; `GraphDemo` (reflective invoke).

**Deeper docs.** SPEC §3 row 3; memory `project_jvm_subsystems_status.md` §3.

---

### 3.11 LambdaForm / MethodHandle execution tracing  *(subsystem 3/7, SPEC §4C)*

**Why it exists.** Linkage (§3.10) tells you the *target*, but not *how* a dynamic
invocation flows through the adapter chain (asType, bindTo, reinvokers,
invokeBasic) to get there.

**Goal.** Make the adapter chain *execute visibly* as graph nodes.

**High-level approach.** The hard constraint: `invokeBasic`/`linkTo*` are
generated **assembly stubs** with no safe C++ per-invocation hook — we do *not*
touch them. Instead we **instrument the LambdaForm bytecode** (the only safe
surface that runs with the real receiver) via the rewriter. `SOROUSH_TRACE_MH_EXEC=1`
additively instruments JDK MH-internal classes; experimental
`SOROUSH_TRACE_MH_EXEC_HIDDEN=1` also instruments the hidden customized dispatch
LambdaForms. A secondary fail-safe read-only structure walk runs at
`resolve_handle_call`.

**Important files and line ranges:**
| File | Function | Lines (approx.) |
|------|----------|-----------------|
| `interpreter/linkResolver.cpp` | `soroush_trace_mh_execution` (+ `_mh_type_desc`, `_mh_lf_name`) | 127–273 |
| `classfile/klassFactory.cpp` | MH-internal class gate `soroush_is_mh_internal_class` + toggles | 158–233 |
| `prims/jvm.cpp` | LambdaFormExecution overlay in `soroush_runtime_record_event` | ~590–600 |
| `classfile/soroushProvenanceGraph.cpp` | `soroush_graph_mh_chain`, `_lambdaform_exec` | 789–857 |

**Env flags / logs.** `SOROUSH_TRACE_MH_EXEC=1` (+ `_HIDDEN=1`) → `[JVM MH EXEC]`,
`[JVM LAMBDAFORM EXEC]`, `[JVM MH ADAPTER]`, `[JVM GRAPH MH]`.

**Demos/tests.** `MHExecDemo.java` + `run-mh-exec-demo.sh` (6 cases).

**Deeper docs.** **SPEC §4C** (read the asm-stub constraint first); memory
`project_provenance_graph.md`.

---

### 3.12 Unified provenance graph  *(subsystem 7, SPEC §4A)*

**Why it exists.** The five runtime signals (recovery, indy, reflection/MH, final
bytecode, execution) are individually useful but disconnected. The graph ties them
into one queryable model.

**Goal.** An append-only in-memory graph linking executions ↔ methods ↔ generated
classes ↔ indy sites ↔ reflection/MH linkage ↔ bytecode artifacts.

**High-level approach.** A separate, **observational, fail-safe, env-gated** module.
Nodes are interned by type-prefixed keys; on any internal failure it silently
drops data (never affects verification/rewriting/behavior). Capped (1M nodes /
2M edges); per-type counts printed at VM shutdown.

- **Node types (1–14):** Class, Method, Execution, GeneratedClass, IndyCallSite,
  BootstrapMethod, ReflectionInvoke, MethodHandleLinkage, BytecodeArtifact,
  AsyncTask, Thread, Executor, MethodHandleAdapter, LambdaFormExecution.
- **Edge types (1–16):** EXECUTES, CALLS, GENERATED_FROM, CREATED_BY, LINKS_TO,
  HAS_BYTECODE, REWRITTEN_FROM, SCHEDULES, SUBMITTED_TO, EXECUTES_ASYNC,
  CONTINUES_ON, MH_INVOKES, ADAPTS_TO, BINDS_TO, INVOKE_BASIC, RESOLVES_TO.

**Important files and line ranges** (`soroushProvenanceGraph.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Node/edge enums + API contracts | `soroushProvenanceGraph.hpp` | whole file (230 lines) |
| Enabled gate | `soroush_graph_enabled` | 162 |
| Node/edge interning internals | (between gate and exact) | ~162–410 |
| Exact execution node (SOLE creator) | `soroush_graph_execution_exact` | 411–468 |
| Generated class | `soroush_graph_generated_class` | 469–506 |
| Indy / linkage / bytecode | `soroush_graph_indy/linkage/bytecode` | 507–666 |
| Shutdown summary | `soroush_graph_dump_summary` | 858–885 |

Integration: one guarded call at each signal site (jvm.cpp, klassFactory.cpp,
methodHandles.cpp, linkResolver.cpp, systemDictionary.cpp); shutdown hook in
`java.cpp` (460–462).

**Env flags / logs.** `SOROUSH_PROVENANCE_GRAPH=1` → `[JVM GRAPH
NODE|CLASS|EXEC|INDY|BYTECODE|EDGE|NOTE|SUMMARY]`, `[JVM GRAPH IDENTITY]`.
`SOROUSH_EXPORT_RUNTIME_TARGETS=1` (or a path) → semantic JSONL export at
shutdown (see §3.16).

**Demos/tests.** `GraphDemo.java` (exercises all signals; expect `graph-demo=223`).

**Deeper docs.** **SPEC §4A + §4D**; memory `project_provenance_graph.md`.

---

### 3.13 Async / cross-thread causality  *(subsystem 7, SPEC §4B)*

**Why it exists.** Causality crosses thread boundaries (executors,
CompletableFuture, Thread.start) — a stack trace on the worker thread can't tell
you who scheduled the work.

**Goal.** Link a submitting/scheduling execution to the worker execution that
actually runs the work.

**High-level approach.** Two hand-off sources: (1) **native `Thread.start`**
(robust, no JDK edits) — a side-table keyed by child `JavaThread*`; (2)
**executor submit→run** joined by the task object's `identityHashCode`, fed by
gated `System.soroushAsyncHandoff` calls in JDK concurrent classes. A worker
carries a *pending async context* consumed at its next ENTER to emit
`CONTINUES_ON`.

**Important files and line ranges** (`prims/jvm.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Thread-start side-table | `soroush_tstart_put/take` | 375–421 |
| Child-thread consume | `thread_entry` hook | 3347–3382 |
| Parent record | `JVM_StartThread` hook | 3383–3490 |
| Async bridges | `JVM_SoroushAsyncEnabled`, `JVM_SoroushAsyncHandoff` | 682–720 |

Graph side: `soroush_graph_thread_start/async_submit/async_run/async_continue`
(`soroushProvenanceGraph.cpp` 683–788). JDK integration: `java.util.concurrent`
`ThreadPoolExecutor`, `ForkJoinPool`, `ForkJoinTask` (gated `if (SOROUSH_ASYNC)`).

**Env flags / logs.** `SOROUSH_PROVENANCE_GRAPH=1` (+ `SOROUSH_RUNTIME_GRAPH=1`
for worker executions) → `[JVM GRAPH THREAD|EXECUTOR|ASYNC]`.

**Demos/tests.** `AsyncDemo.java` + `run-async-demo.sh` (8 cases; expect
`async-demo total=64`).

**Deeper docs.** **SPEC §4B**; memory `project_provenance_graph.md`.

---

### 3.14 Loader-precise identity + exact token trace ABI  *(subsystem 7, SPEC §4A)*

**Why it exists.** Two classes with the same name under different loaders are
genuinely different; and the old string trace ABI lost the descriptor (`Method|…|?`)
and guessed the loader (`loader_id=0`). Identity must be exact.

**Goal.** Every Class/GeneratedClass/BytecodeArtifact node keyed by defining
loader; every instrumented execution keyed by exact `(class, name, descriptor,
loader)` — **no `?` descriptor, no `loader_id=0`, overloads distinct.**

**High-level approach.** `loader_id = (uint64_t)(uintptr_t)ClassLoaderData*`,
threaded to every interning site. For execution, the rewriter assigns each method
a 32-bit **token** and registers its exact identity at rewrite time in an
append-only **method-token registry**; the trace native resolves the token (no
stack walk). Fail-fast: an unresolvable token is `identity-unresolved` (no node),
never a fabricated `?`.

**Important files and line ranges** (`soroushProvenanceGraph.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Token register | `soroush_method_token_register` | 90–140 |
| Token lookup | `soroush_method_token_lookup` | 141–161 |
| Exact execution node | `soroush_graph_execution_exact` (loader-precise, descriptor-keyed) | 411–468 |

Rewriter side: `soroush_transform_member_entry_trace` registers the token
(`soroushClassfileRewriter.cpp` 3691–3785). Resolver: `soroush_trace_token`
(`jvm.cpp` 617–643, needs a `ResourceMark`).

**Env flags / logs.** `[JVM TRACE METHOD] register token=… class=… method=… desc=…
loader=… hidden=… crc=…`; `[JVM GRAPH IDENTITY] token=… … method_node=…` /
`identity-unresolved`; `[JVM GRAPH NOTE] loader-divergence name=… loader1=…
loader2=…`.

**Demos/tests.** `OverloadIdentityDemo.java` + `run-exact-identity-demo.sh`
(`f` → 4 distinct descriptor nodes); `LoaderIdentityDemo.java` +
`run-loader-identity-demo.sh` (same name, two loaders → two distinct nodes).

**Deeper docs.** **SPEC §4A** "Loader-precise identity" + "Exact execution
identity" + **§4D** (export); memory `project_provenance_graph.md`.

---

### 3.15 Rare/custom Code attribute hardening  *(subsystem 6)*

**Why it exists.** A `Code` attribute may carry a non-standard sub-attribute that
embeds PCs we don't know how to remap. Copying it stale after a PC shift would
corrupt the method.

**Goal.** Detect unknown Code sub-attributes and safe-skip the affected method
(copy unchanged) rather than emit stale PCs.

**High-level approach.** A pre-scan (`soroush_is_known_code_attr`) recognizes the
six standard PC-bearing sub-attributes; anything else → per-method safe-skip.
Class-level attributes (e.g. SourceDebugExtension) are copied wholesale and
unaffected.

**Important files and line ranges** (`soroushClassfileRewriter.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| Known-attr recognizer | `soroush_is_known_code_attr` | 257–264 |
| Pre-scan + safe-skip decision | within `soroush_transform_code_attribute_entry_code` | 3102–3604 |

**Env flags / logs.** `[JVM REWRITER CODEATTR] method=… attr=… len=…
decision=safe-skip reason=unknown-code-sub-attribute-may-bear-pcs`.

**Demos/tests.** `run-fake-codeattr.sh` (`FakeAttrTarget.java` +
`FakeCodeAttrPatcher.java`, which injects a fake `SoroushFakeCodeAttr` Code
sub-attribute).

**Deeper docs.** SPEC §4 pipeline step 0; memory `project_jvm_remaining_work.md`
item 3.

---

### 3.16 Runtime target revelation export — semantic JSONL layer  *(§4D)*

**Why it exists.** The provenance graph is an internal, in-memory structure.
This layer converts it into a stable, semantic, human-readable (and
machine-parseable) output format for downstream analysis.

**Goal.** At VM shutdown write one JSONL record per runtime observation:
exact method identities, runtime-resolved dynamic targets with evidence
classification, generated classes, and final/original bytecode artifacts.
**No fake precision.** Exact or explicitly unresolved.

**High-level approach.** A single function `soroush_graph_export_runtime_targets`
(called from `runtime/java.cpp before_exit`). Phase 1 walks the method-token
registry (always active). Phases 2–5 snapshot and walk graph nodes/edges at
shutdown (quiescent, lock-free). Three per-node helper arrays are allocated to
build cross-edge lookups (`rewritten_from`, `indy_for_gen`, `observed_methods`).
Evidence is set by source node type, not inferred from edge type. Parse failures
emit `diagnostic` records instead of fabricated data.

**Important files and line ranges** (`soroushProvenanceGraph.cpp`):
| Area | Function | Lines (approx.) |
|------|----------|-----------------|
| JSON-safe string output | `sg_json_str` | 926–935 |
| Label field extractor | `sg_export_label_field` | 940–953 |
| Method node key parser | `sg_export_parse_method_node` | 960–1007 |
| Main export driver | `soroush_graph_export_runtime_targets` | 1009–1383 |
| Declaration | `soroushProvenanceGraph.hpp` | line 232 |
| Shutdown hook | `runtime/java.cpp before_exit` | ~464–472 |

**Env flags / logs.** `SOROUSH_EXPORT_RUNTIME_TARGETS=1` or `=<path>` →
`[JVM EXPORT] writing runtime targets to …` + `[JVM EXPORT] done: … (method_identity=N …)`.
See SPEC §4D for full env var combination guide.

**Prefix rule.** `SOROUSH_REWRITER_PHASE5_PREFIX` must be in JVM internal
**slash form** (e.g. `com/example`). Dot form (`com.example`) silently fails
because the prefix is matched with `strncmp` against the JVM internal name
which uses `/` as package separator. Simple bare class names (`GraphDemo`,
`AsyncDemo`) work without issue since they contain no separator.

**Demos/tests.** All regression demos + Spring Boot (see §4, SPEC §4D "Validated
against"). Run GraphDemo with `SOROUSH_EXPORT_RUNTIME_TARGETS=1`; expect
`method_identity=5 runtime_target=143 generated_class=16 bytecode_artifact=779
diagnostic=0`.

**Deeper docs.** **SPEC §4D** (complete schema, examples, env combos,
limitations).

---

## 4. Demos and tests map

All under `/Users/soroushaghajani/Downloads/bugged/jvm-dump-demo/`. Most runners
set the env flags themselves and print a `PASS:` verdict; all run under
`-Xverify:all`. `JH=build/macosx-aarch64-server-fastdebug/jdk`.

| Demo / Test | What it validates | Subsystem(s) | How to run |
|-------------|-------------------|--------------|------------|
| `GraphDemo.java` | All signals at once → unified graph (`graph-demo=223`) | 3.2, 3.7–3.10, 3.12 | `SOROUSH_PROVENANCE_GRAPH=1 SOROUSH_RUNTIME_GRAPH=1 SOROUSH_RUNTIME_RECOVERY=1 SOROUSH_TRACE_INDY=1 SOROUSH_TRACE_REFLECTION=1 SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 SOROUSH_REWRITER_PHASE5_PREFIX=GraphDemo $JH/bin/java -Xverify:all …` |
| `AsyncDemo.java` | Cross-thread causality (`async-demo total=64`) | 3.13 | `run-async-demo.sh` |
| `MHExecDemo.java` | MH/LambdaForm adapter execution (`mh-exec total=2221`) | 3.11 | `run-mh-exec-demo.sh` (add `SOROUSH_TRACE_MH_EXEC_HIDDEN=1` for dispatch chain) |
| `LoaderIdentityDemo.java` (+ `SharedWidget.java`) | Same name, two loaders → distinct nodes (`loader-identity total=62`) | 3.14 | `run-loader-identity-demo.sh` |
| `OverloadIdentityDemo.java` | Exact descriptor identity (`overload-identity total=31`, 0 `?`/loader0) | 3.14 | `run-exact-identity-demo.sh` |
| `CtorDemo.java` | Constructor instrumentation (7 cases) | 3.3 | `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 SOROUSH_REWRITER_PHASE5_PREFIX=Ctor …` |
| `RuntimeGenerationDemo.java` | Hidden/lambda/proxy recovery + sidecars | 3.7 | `SOROUSH_RUNTIME_RECOVERY=1 …` |
| `ReflectionDispatchDemo.java` | Reflection / MH linkage tracing | 3.10 | `SOROUSH_TRACE_REFLECTION=1 …` |
| `run-goto-widen-stress.sh` (`GotoWidenStressGen.java`) | `goto→goto_w` widening | 3.4 | `run-goto-widen-stress.sh` |
| `run-branch-widen-stress.sh` (`BranchWidenStressGen.java`) | conditional invert widening | 3.4 | `run-branch-widen-stress.sh` |
| `run-cond-widen-stress.sh` (`Icmp/NullWidenStressGen.java`) | if_icmp* / ifnull widening (+ javap proof) | 3.4, 3.5 | `run-cond-widen-stress.sh` |
| `run-fake-codeattr.sh` (`FakeAttrTarget` + `FakeCodeAttrPatcher`) | unknown Code sub-attr safe-skip | 3.15 | `run-fake-codeattr.sh` |
| `TypeAnnotationStressDemo.java` | Type-annotation PC remapping | 3.5 | PHASE5 prefix run |
| `ExceptionExitStressDemo.java` | Exception-EXIT handlers + safe-skip guard | 3.6 | PHASE5 prefix run |
| `DecoderStressDemo.java`, `ReturnTypesStressDemo.java` | instruction decode / all return opcodes | 3.1 | PHASE5 prefix run |
| **Spring Boot** (`gs-spring-boot/complete/run-rewriter-test.sh`) | broad real-world: app starts, `GET /`, 0 VerifyErrors | all | `… com/example/springboot apponly` or `org/springframework broad` |

---

## 5. Important invariants

These rules must not be broken. Most trace back to the **prime directive**
(SPEC §2).

1. **Verifier-safety over coverage.** Never emit bytecode that fails
   `-Xverify:all`. A missed instrumentation point is fine; a broken classfile is
   not.
2. **Safe-skip instead of unsafe rewrite.** If a transform can't be *proven* safe
   for a code shape, copy the method/class unchanged (per-method where possible).
3. **The graph is observational.** It never affects verification, rewriting, or
   program behavior. On any internal failure it silently drops data.
4. **No hard asserts in the graph hot path.** A crashing `assert` would violate
   the fail-safe contract — invariants are enforced by guarded early-returns +
   diagnostics, not aborts.
5. **Exact token tracing is the only execution-identity path.**
   `soroush_graph_execution_exact` is the sole creator of execution Method nodes.
6. **No legacy string trace path.** `System.soroushTrace(String)`, its parser,
   the `vframeStream` loader walk, and `rewrite_soroush_main` were removed.
7. **No descriptor `?` execution Method nodes** and **no `loader_id=0`** for
   instrumented methods — `_exact` requires a non-empty descriptor and fail-fasts
   otherwise (marks `identity-unresolved`, links no node).
8. **No graph identity guessing.** An unresolvable token yields no node rather
   than a fabricated one; graph-capacity drops are *distinguished* from unresolved.
9. **Loader-aware class identity.** Class/GeneratedClass/BytecodeArtifact keyed by
   defining `ClassLoaderData*`; Method loader-precise transitively. Same name +
   different loader = distinct nodes.
10. **No fake precision in the JSONL export.** An unresolvable Method node key
    emits a `diagnostic` record and omits the target record — no invented
    descriptors, no `loader_id=0` fabrications, no guessed targets.
11. **Prefix slash-form.** `SOROUSH_REWRITER_PHASE5_PREFIX` must use JVM
    internal slash-form names (e.g. `com/example`). Dot form silently fails.

---

## 6. Known limitations and TODOs

Concise but complete (see SPEC §9 + memory `project_jvm_remaining_work.md` for the
authoritative roadmap):

**Graph:**
- Semantic JSONL export at shutdown is **DONE** (`SOROUSH_EXPORT_RUNTIME_TARGETS`;
  see §3.16 + SPEC §4D). A persistent/live-queryable backend remains open.
- 1M node / 2M edge cap (Spring broad runs hit it — no prefix scoping/filtering).
- No edge dedup.
- Loader-divergence NOTE side-table is direct-mapped (diagnostic-only collisions).

**Async:**
- Worker linkage needs the task body instrumented (uninstrumented task → no
  worker Execution to attach).
- `AsyncTask` keyed by 32-bit identity hash (rare collisions merge tasks).
- `CompletableFuture` dependent-stage chaining captured per-stage, not as a
  logical future-chain.
- Virtual-thread / structured-concurrency carrier causality deferred.

**MethodHandle execution:**
- Per-invocation `invokeBasic`/`linkTo*` interception not done (asm stubs —
  unsafe by design).
- Bound receiver/argument *values* not read (no `BoundMethodHandle` species
  accessor) — only the bound species *kind*.
- Link-time structural walk is best-effort (appendix is usually the invoker, not
  the user MH).

**Rewriter:**
- Constructor exception-EXIT intentionally disabled.
- Exception-EXIT guard conservatively safe-skips on local-slot type reuse (fuller
  fix = operand-stack ref-type tracking / common-supertype merging).
- Constructor shapes with pre-delegation `new` (`super(new Foo())`) safe-skip;
  `<clinit>` not instrumented.
- Unknown Code sub-attributes safe-skip (no env-configurable allowlist yet).
- The Phase-2 NOP path (`soroush_transform_code_attribute`, test-only) is *not*
  hardened (copies unknown attrs, lacks type-annotation remapping).

**Capture / coverage:**
- Final-bytecode capture lacks full redefine/retransform version lineage.
- Native/JNI internals, JIT/machine-code provenance, object/dataflow provenance,
  and distributed causality are all out of scope today.

---

## 7. File / module index

Practical "where does X live" index (paths under `src/hotspot/share/` unless
noted):

| File | Main responsibility | Related subsystem(s) |
|------|---------------------|----------------------|
| `classfile/soroushClassfileRewriter.cpp` | The verifier-safe rewriter: decode, layout, widening, emit, metadata remap, token injection | 3.1, 3.3–3.6, 3.15 |
| `classfile/soroushClassfileRewriter.hpp` | Rewriter public API (`insert_entry_*`, `roundtrip_copy`, `TransformResult`) | 3.1 |
| `classfile/soroushProvenanceGraph.cpp` | Graph nodes/edges, interning, token registry, async + MH nodes, summary, **semantic JSONL export** | 3.12–3.14, 3.11, 3.13, 3.16 |
| `classfile/soroushProvenanceGraph.hpp` | Graph node/edge enums + function contracts (incl. export declaration) | 3.12, 3.14, 3.16 |
| `classfile/klassFactory.cpp` | Class-load hook: recovery, rewriter invocation, final-bytecode capture, graph bytecode hand-off, MH-internal gate | 3.1, 3.7, 3.8, 3.11 |
| `prims/jvm.cpp` | Execution-graph natives, token resolver, ENTER/EXIT recorder, async thread hooks + bridges | 3.2, 3.13, 3.14 |
| `prims/jvm.h` | `JVM_SoroushTraceEnter/Exit` + async prototypes | 3.2, 3.13 |
| `classfile/systemDictionary.cpp` | invokedynamic linkage trace + indy trace-id scoping | 3.9 |
| `prims/methodHandles.cpp` | MemberName resolution / MH linkage trace | 3.10, 3.11 |
| `interpreter/linkResolver.cpp` | Runtime dispatch trace + read-only MH-structure walk | 3.10, 3.11 |
| `runtime/reflection.cpp` | Reflective `Method.invoke` trace | 3.10 |
| `runtime/java.cpp` | Shutdown summary hook (`before_exit`) | 3.12 |
| `java.base/.../java/lang/System.java` | Native decls (`soroushTraceEnter/Exit`, async) | 3.2, 3.13 |
| `java.base/.../native/libjava/System.c` | Native registration | 3.2, 3.13 |
| `make/data/hotspot-symbols/symbols-unix` | Native symbol exports | 3.2, 3.13 |
| `java.base/.../util/concurrent/{ThreadPoolExecutor,ForkJoinPool,ForkJoinTask}.java` | Gated async submit/run hand-off calls | 3.13 |

---

## 8. Recommended next reading path

Concrete, goal-driven plans. Read in the order given.

**To understand the rewriter:**
1. SPEC §2 (prime directive) + §4 (pipeline).
2. `soroushClassfileRewriter.hpp` (the public API contract).
3. `soroushClassfileRewriter.cpp` in pipeline order: `soroush_decode_code`
   (266) → `soroush_build_pc_map` (482) → `soroush_converge_layout` (906) →
   `soroush_emit_rewritten_code` (1066) → `soroush_transform_code_attribute_entry_code`
   (3102, the orchestrator) → `soroush_transform_member_entry_trace` (3691).
4. `klassFactory.cpp` ~640–950 (how it's invoked at class load).
5. memory `feedback_verifier_safety_over_coverage.md` +
   `project_exception_exit_frame_bug.md`.

**To understand the provenance graph:**
1. SPEC §4A.
2. `soroushProvenanceGraph.hpp` (node/edge enums + contracts).
3. `soroushProvenanceGraph.cpp`: `soroush_graph_enabled` (162) → interning
   internals (~162–410) → `soroush_graph_execution_exact` (411) →
   `_generated_class`/`_indy`/`_linkage`/`_bytecode` (469–666) →
   `_dump_summary` (858).
4. The integration call sites (one guarded call each) in jvm.cpp / klassFactory.cpp
   / methodHandles.cpp / systemDictionary.cpp.
5. memory `project_provenance_graph.md`.

**To understand async causality:**
1. SPEC §4B.
2. `jvm.cpp`: `soroush_tstart_put/take` (375) → `thread_entry` (3347) →
   `JVM_StartThread` (3383) → `JVM_SoroushAsyncHandoff` (688).
3. `soroushProvenanceGraph.cpp` 683–788 (thread/async nodes + edges).
4. The `java.util.concurrent` hooks (grep `SOROUSH_ASYNC` /
   `soroushAsyncHandoff`).

**To understand MethodHandle execution tracing:**
1. SPEC §4C — **read the asm-stub constraint first** (it explains why the
   approach is bytecode instrumentation, not a C++ hook).
2. `linkResolver.cpp` `soroush_trace_mh_execution` (198) + `_mh_lf_name` (162).
3. `klassFactory.cpp` `soroush_is_mh_internal_class` (188) + the
   `SOROUSH_TRACE_MH_EXEC` toggles (158–233).
4. `jvm.cpp` LambdaFormExecution overlay in `soroush_runtime_record_event`
   (~590–600).
5. Run `MHExecDemo` with and without `SOROUSH_TRACE_MH_EXEC_HIDDEN=1` and diff
   the graph output.

**To understand exact identity (tokens + loaders):**
1. SPEC §4A "Loader-precise identity" + "Exact execution identity".
2. `soroushProvenanceGraph.cpp` `soroush_method_token_register` (90) / `_lookup`
   (141) / `soroush_graph_execution_exact` (411).
3. `soroushClassfileRewriter.cpp` `soroush_transform_member_entry_trace` (3691).
4. `jvm.cpp` `soroush_trace_token` (617).
5. Run `OverloadIdentityDemo` + `LoaderIdentityDemo`.
