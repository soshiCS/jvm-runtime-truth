# Soroush Custom JVM — Project Spec & Agent Start-Here Guide

> **New to this project? Read this file first.** It explains the goal, the
> architecture, what works today, and — most importantly — **where to look next**
> (source files and the cross-session memory `.md` files) for whatever task you
> are doing.

This is a fork of **OpenJDK jdk21u (HotSpot)** modified to expose runtime
provenance and causality information that ordinary JVMs discard. Working
directory: `/Users/soroushaghajani/custom-jvm/jdk21u`.

---

## 1. Project goal

Build a JVM that lets an AI (or human) **reconstruct how runtime-generated and
dynamically-transformed code actually executed** — the things normal stack
traces and current observability tools lose:

- `invokedynamic` / `LambdaMetafactory` linkage
- reflection / `MethodHandle` dispatch
- hidden classes, proxies, ByteBuddy/CGLIB runtime-generated classes
- runtime bytecode transforms (JVMTI, ClassFileLoadHook, redefine/retransform)
- execution causality (a persistent ENTER/EXIT call graph)
- eventually async / cross-thread causality

The north star: an observability/debugging platform where failures in
dynamically-generated code can be reconstructed end to end.

---

## 2. The prime directive (read before changing the rewriter)

**Verifier-safety over coverage.** The bytecode rewriter must *never* emit
bytecode that fails verification. If a transform is unsafe or untested for some
code shape, **fail safe** (skip that method/class, leaving the original
bytecode) rather than emit questionable bytecode. A missed instrumentation
point is fine; a broken classfile is not. See
[`feedback_verifier_safety_over_coverage.md`](#5-memory-files--read-map).

Every change is validated under `-Xverify:all`. The verifier is the backstop:
if the rewriter is wrong, the load fails loudly instead of silently corrupting.

---

## 3. The six subsystems & current state

| # | Subsystem | State | Primary source |
|---|-----------|-------|----------------|
| 1 | Runtime-generated class recovery (hidden/lambda/proxy/ByteBuddy/CGLIB) + provenance sidecars → `/tmp/soroush_jvm_dump` | working | `klassFactory.cpp`, `systemDictionary.cpp`, `java.cpp` |
| 2 | invokedynamic / LambdaMetafactory linkage tracing | working (linkage layer); MH/LambdaForm **execution** now traced too — see §4C | `methodHandles.cpp`, `linkResolver.cpp`, `systemDictionary.cpp` |
| 3 | reflection / MethodHandle linkage tracing | working (linkage layer); MH **adapter execution** now traced (§4C) | `reflection.cpp`, `methodHandles.cpp`, `linkResolver.cpp` |
| 4 | final executable bytecode capture (post-transform) | working | `klassFactory.cpp` |
| 5 | persistent runtime ENTER/EXIT execution graph | working (single-thread) | `jvm.cpp`/`jvm.h`, `System.java`/`System.c` |
| 6 | **verifier-safe bytecode rewriter** (the heart) | branch widening + constructor instrumentation + rare/custom Code-attribute hardening complete | `soroushClassfileRewriter.{cpp,hpp}` + `klassFactory.cpp` |
| 7 | **unified provenance graph (v1 + async + MH execution)** — ties subsystems 1-5 into one in-memory graph, incl. async/cross-thread causality and MethodHandle/LambdaForm execution | working (observational, fail-safe, env-gated); see §4A, §4B, §4C | `soroushProvenanceGraph.{cpp,hpp}` (integration calls in jvm/klassFactory/methodHandles/linkResolver/systemDictionary + JDK concurrent classes) |

For a precise per-subsystem "implemented / partial / missing" breakdown, read
[`project_jvm_subsystems_status.md`](#5-memory-files--read-map).

---

## 4. The rewriter (subsystem 6) — the part most work touches

The **verifier-safe** rewriter lives in
**`src/hotspot/share/classfile/soroushClassfileRewriter.cpp`** (public API in the
`.hpp`). It is invoked from `klassFactory.cpp` during class load, gated by env
vars (below). It is a self-contained classfile parser / emitter — it does NOT
depend on the rest of HotSpot's classfile machinery.

> **Legacy path removed.** An older hand-rolled rewriter (`rewrite_soroush_main`,
> `[JVM REWRITE]`, gated by `SOROUSH_REWRITE_PREFIX`) and the string trace ABI it
> injected (`System.soroushTrace(String)`) were **deleted** — they were a
> best-effort duplicate tracing architecture (no descriptor, stack-walked loader).
> The exact method-token ABI (below) is now the **single** execution-tracing
> mechanism. The class-load pipeline is simply Phase5 → Phase3 → Phase2.

### Phases (entry points in the `.cpp`)

| Phase | API | What it does | Enable with |
|-------|-----|--------------|-------------|
| 1 | `roundtrip_copy` | parse + byte-identical re-emit (sanity) | `SOROUSH_REWRITER_PHASE1=1` |
| 2 | `insert_entry_nops` | prepend NOPs (layout test) | `SOROUSH_REWRITER_PHASE2_ENTRY_NOPS=1` + `SOROUSH_REWRITER_PHASE2_PREFIX=<pkg>` |
| 3 | `insert_entry_trace` | ENTER only | `SOROUSH_REWRITER_PHASE3_ENTER=1` + `SOROUSH_REWRITER_PHASE3_PREFIX=<pkg>` |
| 5 | `insert_entry_exit_trace` | **ENTER + normal EXIT + exception EXIT** (the main path) | `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1` + `SOROUSH_REWRITER_PHASE5_PREFIX=<pkg>` |

`<pkg>` is an **internal-name prefix** using JVM slash-separated form (e.g.
`com/example/springboot`, `org/springframework`, or `*` for all).
**Critical:** the prefix is matched with `strncmp` directly against the JVM
internal class name (which always uses `/` separators). Dot-form prefixes
(`com.example`) will silently fail to match. Always use slash form.

**Trace ABI — exact method tokens (Phase 3/5).** Instrumentation no longer injects
a string; it injects the **exact method-token ABI**. At rewrite time each
instrumented method is assigned a stable 32-bit token and registered (in the VM's
method-token registry) with its *exact* identity — internal class name, method
name, **descriptor**, defining **loader** (`ClassLoaderData*`, passed in from
klassFactory), **hidden** flag, and the original-bytes **crc** (bytecode version).
The injected prologue is `ldc_w <Integer token>; invokestatic
System.soroushTraceEnter(int)`; each return / synthetic exception-EXIT handler gets
`ldc_w <Integer token>; invokestatic System.soroushTraceExit(int)` (eventKind is
encoded by *which* native is called — Enter = 0, Exit = 1). The token is one shared
`CONSTANT_Integer` per method, so the injected prologue/epilogue keep the **same
6-byte, 1-stack-slot shape** as the old string `ldc_w`+`invokestatic` — the
pc-map / layout-convergence / StackMapTable / branch-widening logic is unchanged.
At runtime the native resolves the token to that exact identity, so the graph keys
the Method node by `(class_node_id, name, descriptor)` with the loader-precise
Class node — **no `?` descriptor and no `loader_id=0` guessing**. This is the
**sole** execution-tracing path; the legacy `System.soroushTrace(String)` native
and string ENTER/EXIT instrumentation were removed. See §4A "Exact execution
identity".

### Rewriter pipeline (Phase 5), in order
0. **Per-method safety pre-scan** (in `soroush_transform_code_attribute_entry_code`):
   reject (safe-skip → copy the method unchanged) anything we can't prove safe
   *before* emitting a byte — currently a constructor whose delegation can't be
   located, and any method whose `Code` carries an **unknown/non-standard Code
   sub-attribute** (it may embed PCs we can't remap; `soroush_is_known_code_attr`).
1. **Plan** which methods to rewrite (`soroush_insert_trace`): ordinary methods +
   `<init>` constructors; `<clinit>` skipped. Allocates constant-pool entries.
2. **Build base layout** (`soroush_build_pc_map`): decode instructions, compute
   new PCs with ENTER/EXIT prefixes; ENTER prepends at pc 0 for normal methods,
   or is inserted **after the constructor delegation** for `<init>`.
3. **Branch-widening convergence** (`soroush_converge_layout`): iterative
   fixed-point relaxation. Short branches whose rewritten offset overflows s2
   are widened (`goto→goto_w`, `jsr→jsr_w`, conditional → inverted-if + `goto_w`).
   Writes the stable layout back into `instructions[]`/`pc_map[]`.
4. **Exception-EXIT handler synthesis** (`soroush_build_stack_map_exception_handlers`):
   synthetic catch-all handlers that trace on exceptional exit. Disabled for
   constructors. **Conservative safety guard**: skips methods where a local slot
   is overwritten with a different type inside a protected region.
5. **Emit** (`soroush_emit_rewritten_code`): consume the stabilized layout; emit
   widened branches from the `widened` flag; insert ENTER/EXIT.
6. **Metadata remapping** through the same `pc_map` — the six standard
   PC-bearing Code sub-attributes: StackMapTable (with synthesized fall-through
   frames for widened conditionals), exception table, LineNumberTable,
   LocalVariable(Type)Table, Runtime(In)VisibleTypeAnnotations. Any *other* Code
   sub-attribute was already rejected in step 0 (never copied stale).

### Diagnostic log prefixes (how to read the stderr output)
- `[JVM REWRITER PHASE5] class=… normal_exit=ok|failed …` — per-class outcome (+ `code_methods`, `ctor_methods_skipped` census).
- `[JVM REWRITER WIDEN] …` — Phase 6A base-layout branch-overflow detection.
- `[JVM REWRITER CONVERGE] iter=… / converged=… …` — fixed-point layout relaxation.
- `[JVM REWRITER SAFE-SKIP] method=… slot=… first_type=… conflicting_type=…` — exception-EXIT variable-locals guard.
- `[JVM REWRITER CTOR] method=… considered | delegation=super|this | instrumented | safe-skip reason=…` — constructor handling.
- `[JVM REWRITER CODEATTR] method=… attr=… len=… decision=safe-skip reason=unknown-code-sub-attribute-may-bear-pcs` — unknown/non-standard Code sub-attribute → method copied unchanged.
- `[JVM TRACE] id=… parent=… thread=… ENTER|EXIT <class>.<method>` — runtime execution-graph events (subsystem 5), now fed by the exact method-token ABI (the class/method are resolved from the token, not parsed from a string). Requires `SOROUSH_RUNTIME_GRAPH=1`.
- `[JVM TRACE METHOD] …` — exact method-token ABI (subsystem 5; the sole execution-tracing mechanism): `register token=… class=… method=… desc=… loader=… hidden=… crc=…` at rewrite time, and `ENTER|EXIT token=… <class>.<method><desc> loader=…` per event (`System.soroushTraceEnter/Exit`). Printed independent of `SOROUSH_RUNTIME_GRAPH`.
- `[JVM GRAPH IDENTITY] token=… class=… method=… desc=… loader=… hidden=… kind=0(ENTER) method_node=…` — exact execution→Method identity (once per distinct instrumented method); or `… identity-unresolved` when a token can't be resolved (fail-fast; no Method node linked). See §4A "Exact execution identity".
- `[JVM DUMP] …` — dumped runtime-generated/transformed class bytes to `/tmp/soroush_jvm_dump` (subsystem 1).
- `[JVM RECOVER] …` — recovered runtime-generated class + provenance sidecar fields (subsystem 1: `generated_by`, `source_trigger`, `provenance_kind`, `trace_id`, loader, crc, …).
- `[JVM INDY] hidden_class=… …` — invokedynamic-associated hidden class observed at load (subsystem 2).
- `[JVM REFLECT] …` — reflection / MethodHandle **linkage** tracing (subsystems 2/3): `membername_source/target`, `dispatch_kind`, `resolved_method`, `methodhandle_dispatch`, `mh_target`. Gated by `SOROUSH_TRACE_REFLECTION=1`.
- `[JVM FINAL BYTECODE] …` — final post-transform executable bytecode capture (subsystem 4): `transformed`, `hidden`, `load_kind`, `original_size`, `final_size`, `crc32`, `dumped`. Gated by `SOROUSH_CAPTURE_FINAL_BYTECODE=1`.
- `[JVM GRAPH NODE|CLASS|EXEC|INDY|BYTECODE] …`, `[JVM GRAPH EDGE] …`, `[JVM GRAPH NOTE] …`, `[JVM GRAPH SUMMARY] …` — unified provenance graph (§4A). `[JVM GRAPH CLASS]` covers loader-precise Class-node creation + per-Method `attached class-node=… loader=…` lines; `[JVM GRAPH BYTECODE]` shows each BytecodeArtifact's loader-specific Class attachment; `[JVM GRAPH NOTE] loader-divergence …` flags a class name first seen under a second loader.
- `[JVM GRAPH THREAD] …`, `[JVM GRAPH EXECUTOR] …`, `[JVM GRAPH ASYNC] …` — async / cross-thread causality (§4B).
- `[JVM MH EXEC] …`, `[JVM LAMBDAFORM EXEC] …`, `[JVM MH ADAPTER] …`, `[JVM GRAPH MH] …` — MethodHandle / LambdaForm execution tracing (§4C).

---

## 4A. Unified provenance graph (v1)

A separate, observational layer (`soroushProvenanceGraph.{cpp,hpp}`) that ties
the five runtime signals into one append-only in-memory graph. **Gated by
`SOROUSH_PROVENANCE_GRAPH=1`; a no-op otherwise. Never affects verification,
rewriting, or program behavior — on any internal failure it silently drops
data.** Thread-safe via one internal mutex. Capped (1M nodes / 2M edges); a
per-type count is printed at VM shutdown (`soroush_graph_dump_summary`, hooked
in `java.cpp before_exit`).

**Node types (1–9):** Class, Method, Execution, GeneratedClass, IndyCallSite,
BootstrapMethod, ReflectionInvoke, MethodHandleLinkage, BytecodeArtifact.
**(10–12, async, §4B):** AsyncTask, Thread, Executor.
**(13–14, MH execution, §4C):** MethodHandleAdapter, LambdaFormExecution.
**Edge types (1–7):** EXECUTES (Execution→Method), CALLS (Execution→Execution),
GENERATED_FROM (GeneratedClass→Class), CREATED_BY (GeneratedClass→IndyCallSite,
IndyCallSite→Bootstrap), LINKS_TO (Reflection/MHLinkage→Method), HAS_BYTECODE
(Class→BytecodeArtifact), REWRITTEN_FROM (final BytecodeArtifact→original).
**(8–11, async, §4B):** SCHEDULES, SUBMITTED_TO, EXECUTES_ASYNC, CONTINUES_ON.
**(12–16, MH execution, §4C):** MH_INVOKES, ADAPTS_TO, BINDS_TO, INVOKE_BASIC, RESOLVES_TO.

**Identity (interning) rules** — nodes are deduplicated by a type-prefixed key;
class names are normalized to internal (slash) form. **Class / GeneratedClass /
BytecodeArtifact identity is loader-precise** (see "Loader-precise identity"
below):
- Class `C|<name>|<loader_id>|<hidden>`; GeneratedClass `G|<name>|<loader_id>|<crc>`;
  Method `M|<class_node_id>|<name>|<desc>` (loader-precise *transitively* — it keys
  off the loader-specific Class node id, not the bare class name; `<desc>` is the
  **exact descriptor** for methods entered via the token ABI, so overloads with the
  same name but different descriptors never merge — see "Exact execution identity");
  BytecodeArtifact `A|<name>|<loader_id>|<crc>|<original|final>`;
  IndyCallSite `I|<indy_trace_id>` (the **join key** between an indy site and the
  generated lambda class — unchanged, loader-independent); BootstrapMethod `B|<bsm>`;
  Reflection/MHLinkage `R|`/`H|<class.name+desc>`; Execution `E|<exec_id>` (never
  deduped — unique).
- `loader_id` is the defining loader's `ClassLoaderData*` (a stable per-loader
  pointer; the same value at class-load time and at run time). The bootstrap
  loader has its own non-null CLD, so it gets a stable id too.

**Loader-precise identity (landed; supersedes v1 name-only keying).** Two runtime
classes with the **same internal name but different loaders are now DISTINCT
graph nodes** (previously v1 merged them and only logged a NOTE). The loader is
threaded to every Class-interning site: class-load capture and generated-class
recovery have the defining `ClassLoaderData*`; the **execution** path gets it
exactly from the method token (recorded at rewrite time — see "Exact execution
identity" below), with no stack walk and no fallback; reflection/MH linkage uses
the target method holder's CLD; the MH link-time walk uses the resolved target's
CLD. Because
every site computes the *same* `ClassLoaderData*` for a given class, the Class
nodes from different signals **merge correctly within a loader** and **split
correctly across loaders**. BytecodeArtifacts are loader-keyed too, so identical
bytes under two loaders become two artifacts, each `HAS_BYTECODE` from its own
Class node, and the `REWRITTEN_FROM` final→original lookup is scoped to the same
name+loader (never crosses loaders). Diagnostics: `[JVM GRAPH CLASS]` prints the
loader on each new Class node and a `method-node=… attached class-node=… loader=…`
line per new Method; `[JVM GRAPH BYTECODE]` shows each artifact's loader-specific
Class attachment; `[JVM GRAPH NOTE] loader-divergence name=… loader1=… loader2=…`
fires the first time a name is seen under a second loader (best-effort via a
fixed-size, fail-safe name→first-loader side table). **Limitations:** the
divergence-NOTE side table is direct-mapped (a hash collision can miss/duplicate a
purely diagnostic NOTE).

**Exact execution identity (token ABI; the SOLE execution-tracing mechanism).**
An earlier string ABI received only `"ENTER com/example/Foo.bar"` — no descriptor
(Method keyed `…|?`) and a loader recovered by a best-effort `vframeStream` walk
that could fall back to `loader_id=0`. **That string path (the
`System.soroushTrace(String)` native, its parser, the `vframeStream`
`soroush_caller_loader_id` helper, the best-effort `soroush_graph_execution`, and
the legacy `rewrite_soroush_main` rewriter) was removed.** Instrumented methods now
carry **exact identity end-to-end** via the method-token ABI (see §4 "Trace ABI"):
the rewriter registers each method's exact
class/name/**descriptor**/loader/hidden/crc at rewrite time and bakes a token into
`System.soroushTraceEnter/Exit(int)`. `JVM_SoroushTraceEnter/Exit`
(`soroush_trace_token`, jvm.cpp) resolve the token via the **method-token
registry** (`soroush_method_token_register`/`_lookup` in
`soroushProvenanceGraph.{cpp,hpp}`: append-only, immortal entries, own mutex,
indexed by token, always active when instrumentation runs) and call
`soroush_graph_execution_exact`, which keys the Method node `M|<class_node>|<name>|
<descriptor>` against the loader-precise Class node — so same-name overloads stay
distinct and there is **no `?` descriptor and no `loader_id=0` for instrumented
methods**. The token's loader is the defining `ClassLoaderData*` captured at rewrite
time (same pointer the bytecode-capture/Class paths use), so executions merge into
the correct loader-specific Class/Method without any stack walk. **Fail-fast, no
fake precision:** if a token can't be resolved (registry OOM/overflow → token 0),
the event is marked `[JVM GRAPH IDENTITY] … identity-unresolved` and **no** Method
node is linked (rather than fabricating a `?` node); if the *graph* is at capacity
(node cap / OOM) the identity is still known but the node is silently dropped
(fail-safe), distinct from "unresolved". Diagnostics: `[JVM TRACE METHOD]`
(per-event human line + token registrations) and `[JVM GRAPH IDENTITY] token=…
class=… method=… desc=… loader=… hidden=… kind=0(ENTER) method_node=…` (emitted
once per distinct instrumented method). **Invariant:** `soroush_graph_execution_exact`
is the only path that creates an execution Method node, and it requires a non-empty
descriptor, so no execution Method node can ever be keyed `…|?` and no execution
node uses a `loader_id=0` fallback. **Limits:** int tokens cap at 8M instrumented
methods/run (fail-safe beyond); the registry holds immortal entries for the process
lifetime; each redefine/retransform assigns fresh tokens (older tokens stay valid
but unused).

**Integration points & coupling** (each is one guarded call at an existing
signal site; structural edges are added once, when the keying node is new):
- Execution graph — token ABI only: `JVM_SoroushTraceEnter/Exit` (jvm.cpp `soroush_trace_token` → `soroush_runtime_record_event` → `soroush_graph_execution_exact`) resolve the per-method token to exact class/name/descriptor/loader/hidden. Requires `SOROUSH_RUNTIME_GRAPH=1`; maintains the call-id/parent stack + async/MH overlay. (The legacy `JVM_SoroushTrace` string path was removed.)
- GeneratedClass — `recover_runtime_generated_class` (klassFactory.cpp); requires `SOROUSH_RUNTIME_RECOVERY=1`. Passes the defining `ClassLoaderData*` as `loader_id`.
- BytecodeArtifact + REWRITTEN_FROM — class-load capture (klassFactory.cpp); fires for every loaded class when the graph is on. Passes the defining `ClassLoaderData*` (and `hidden`) so the artifact + its Class node are loader-precise.
- IndyCallSite/Bootstrap — bootstrap site (systemDictionary.cpp); enriched when `SOROUSH_TRACE_INDY=1` (sparse IndyCallSite still created from the generated-class side via trace_id).
- Reflection/MHLinkage — `soroush_trace_membername_resolution` (methodHandles.cpp); passes the target method holder's CLD as `loader_id`. (MH link-time walk in linkResolver's `soroush_trace_mh_execution` passes the resolved target's CLD.)

---

## 4B. Async / cross-thread causality (provenance graph async extension)

Extends the graph (§4A) so causality survives thread boundaries: a scheduling /
submitting thread's execution is linked to the worker thread's execution that
actually runs the work. Same invariants as §4A — **observational, fail-safe,
gated by `SOROUSH_PROVENANCE_GRAPH=1`**; populates fully only when the execution
graph is also on (`SOROUSH_RUNTIME_GRAPH=1`) and the relevant task bodies are
instrumented (so a worker `Execution` node exists to link to).

**New nodes:** `AsyncTask` (a submitted unit of work, keyed by the task object's
identity hash `T|<hash>`), `Thread` (keyed by stable OS tid `Th|<tid>` — *not*
name), `Executor` (keyed by executor identity hash `X|<hash>`).
**New edges:** `SCHEDULES` (Execution→AsyncTask, Execution→Thread, Thread→Thread),
`SUBMITTED_TO` (AsyncTask→Executor), `EXECUTES_ASYNC` (AsyncTask/Thread→worker
Execution), `CONTINUES_ON` (submitter Execution→worker first-frame Execution —
the cross-thread causal link).

**Propagation model.** A worker thread carries a *pending async context* (the
cause execution + owning AsyncTask/Thread node) set at the hand-off and consumed
at its next instrumented `ENTER` (in `JVM_SoroushTrace`), which emits the
`CONTINUES_ON` + `EXECUTES_ASYNC` edges to that first worker frame. One-shot.
The two hand-off sources:
- **Raw `Thread.start`** (native, robust, no bytecode/JDK edits): `JVM_StartThread`
  records the parent's `{tid, exec id}` keyed by the new child `JavaThread*` in a
  pthread-guarded side-table; `thread_entry` (runs on the child) consumes it,
  creates the parent/child `Thread` nodes + `SCHEDULES` edge, and sets the
  child's pending context. Because pools spawn workers via `Thread.start`, this
  also captures executor *worker-thread creation*.
- **Executor task submit→run** (task-object identity join): an `AsyncTask` is
  keyed by `System.identityHashCode`. At **submit** the submitter execution +
  executor are recorded (keyed by task hash in a direct-mapped side-table); at
  **run** the worker resolves that context and stashes it as its pending context.
  Fed by gated calls (`System.soroushAsyncHandoff`) added to JDK concurrent
  classes — see integration points below.

**Native pieces (jvm.cpp):** thread-start side-table; `thread_entry`/`JVM_StartThread`
hooks; pending-context thread-locals consumed in `JVM_SoroushTrace`; two bridges
`JVM_SoroushAsyncEnabled` (cached gate) and `JVM_SoroushAsyncHandoff(kind, task,
executor)` (kind 1=submit, 2=run; computes identity hashes via `FastHashCode`,
reads the current execution id). Declared on `java.lang.System`
(`soroushAsyncEnabled`, `soroushAsyncHandoff`), registered in `System.c`,
exported in `make/data/hotspot-symbols/symbols-unix`.

**JDK integration points** (each a one-line `if (SOROUSH_ASYNC) …` guarded by a
`static final boolean` read once via `System.soroushAsyncEnabled()`; zero cost
when disabled):
- `ThreadPoolExecutor.execute` → submit; `ThreadPoolExecutor.runWorker` (before
  `task.run()`) → run. Covers `Executor.execute`, `ExecutorService.submit`
  (FutureTask), and pool reuse.
- `ForkJoinPool.poolSubmit` → submit (the single chokepoint for all FJP
  submissions); `ForkJoinTask.doExec` → run. Covers `CompletableFuture.*Async`
  (default ForkJoinPool.commonPool) and direct FJP use.

**Safety.** The graph mutex is only ever held for in-memory work — never while
calling back into Java or taking VM locks — so no deadlock. The bridges never
change scheduling, never load classes (the gate is a native call, not
`getenv`/property lookup), and fail safe (a missing edge, never a crash).

**Verified (`AsyncDemo`, all -Xverify:all, rc=0, `async-demo total=64`):** all 8
cases produce async edges — Thread.start, Executor.execute, submit+Callable,
`CompletableFuture.supplyAsync` (flows through ForkJoinPool), nested scheduling
(a worker submitting a further task — captured with the worker as submitter),
multiple workers, pool reuse, and an exception-throwing task. Per-run summary:
AsyncTask=14, Thread=8, Executor=2; SCHEDULES=25, SUBMITTED_TO=14,
EXECUTES_ASYNC=15, CONTINUES_ON=15; 0 VerifyErrors. No-op when disabled (0 graph
lines). Spring graph+async run serves `GET /`, 0 VerifyErrors, 27 Thread nodes,
46 async edges.

**v1 async limitations / next:** worker linkage needs the task body instrumented
(an uninstrumented task yields the submit/thread edges but no worker `Execution`
to link, and a lingering pending context could mis-attribute to a later ENTER);
`AsyncTask` is keyed by 32-bit identity hash (rare collisions merge tasks);
`CompletableFuture` *dependent-stage* chaining (thenApplyAsync→…) is captured
only at the per-stage submit/run granularity, not as a logical future-chain;
`ForkJoinTask.doExec` fires the run bridge for every FJ task when enabled
(observational overhead, not optimized). Deferred: structured-concurrency /
virtual-thread carrier causality, `CONTINUES_ON` across non-instrumented
boundaries.

---

## 4C. MethodHandle / LambdaForm execution tracing

Closes the gap between MH *linkage* tracing (subsystems 2/3 — already done) and MH
*adapter execution*: how a dynamic invocation actually flows through the adapter
chain (asType, bindTo/bound species, reinvokers, invokeBasic) to the final target.

**The hard constraint (read this first).** In HotSpot, `invokeBasic`/`linkTo*`
execute as **generated assembly stubs** (`generate_method_handle_dispatch`,
`methodHandles_<arch>.cpp`) — there is **no safe C++ per-invocation hook**, so we
do *not* touch them (an unsafe hook, deliberately skipped). Also, at link time
(`resolve_handle_call`) the appendix is the *invoker* MemberName, **not** the
user's MethodHandle (that is a runtime receiver value), and `BoundMethodHandle`
species fields have no C++ accessor (bound-value flow not readable). So the deep
execution is captured by **instrumenting the LambdaForm bytecode** (the only
safe surface that runs with the real receiver), via the verifier-safe rewriter.

**Two mechanisms:**
1. **LambdaForm/MH-internal bytecode instrumentation (primary).** Env
   `SOROUSH_TRACE_MH_EXEC=1` *additively* makes the PHASE5 rewriter also
   instrument the JDK's MH-internal classes (`java/lang/invoke/{LambdaForm,
   DirectMethodHandle,BoundMethodHandle,DelegatingMethodHandle,Invokers}`),
   on top of whatever app prefix you chose. Their ENTER/EXIT become Execution
   nodes, and `JVM_SoroushTrace` additionally interns a **LambdaFormExecution**
   node per executing MH-internal method with an `INVOKE_BASIC` edge
   (Execution→LambdaFormExecution). The adapter chain is then the runtime
   `CALLS` chain through these executions to the final target method.
   - The rewriter normally skips **all hidden classes**; the generic `$Holder`
     dispatch forms + the construction/adapter-setup machinery (Specializer,
     `makeReinvoker`, `viewAsType`, Species, LambdaFormEditor) are instrumented,
     but the *runtime-customized hidden* dispatch LambdaForms
     (`LambdaForm$DMH/$MH/$BMH+0x…`) are not. To trace the actual per-invocation
     dispatch chain, set the **separate, explicit, experimental** opt-in
     `SOROUSH_TRACE_MH_EXEC_HIDDEN=1` — it relaxes the hidden-class skip *only*
     for MH-internal classes. With it, the final target's traced parent becomes
     the dispatch LambdaForm (e.g. `LambdaForm$DMH.invokeStatic`), so the chain
     caller→adapters→target is explicit. Verified safe (demo + Spring serve
     correctly, 0 VerifyErrors) but heavier; off by default.
2. **Native read-only structure walk (secondary).** At `resolve_handle_call`
   (linkResolver) a fail-safe read-only walk of the appendix emits `[JVM MH EXEC]`
   diagnostics + a `MethodHandleAdapter` node; for a `DirectMethodHandle` it adds
   `RESOLVES_TO` the final Method, and `MH_INVOKES` from the caller Execution.
   Best-effort: fires only when the site carries a MethodHandle/MemberName
   appendix (often absent for `invokeExact`/identity-`invoke`), so it is
   supplementary to mechanism 1. `ADAPTS_TO`/`BINDS_TO` are schema-reserved
   (the adapter inner-target / bound-arg fields aren't reachable from C++).

Final-target *resolution* is already captured by the existing `MemberName.resolve`
linkage hook (MethodHandleLinkage node + `LINKS_TO` Method, methodHandles.cpp).

**Gating:** `[JVM MH EXEC]`/`[JVM LAMBDAFORM EXEC]`/`[JVM MH ADAPTER]` logs need
`SOROUSH_TRACE_MH_EXEC=1`; graph nodes/edges need `SOROUSH_PROVENANCE_GRAPH=1`;
LambdaForm-execution nodes also need `SOROUSH_RUNTIME_GRAPH=1` + the rewriter on.
All observational, fail-safe, no-ops when disabled.

**Verified (`MHExecDemo`, 6 cases: invokeExact, asType, bindTo, lambda/indy,
reflection→MH, chained bindTo+asType):** `mh-exec total=2221`, 0 VerifyErrors.
Default mode: ~92 LambdaFormExecution nodes (incl. `viewAsType`, `Species_L.make`,
`makeReinvoker`, `Specializer`, `LambdaFormEditor` — adapter machinery executing)
+ thousands of INVOKE_BASIC edges. Hidden mode: the dispatch chain is explicit
(`MHExecDemo.add`'s parent is `LambdaForm$DMH.invokeStatic`). No-op when disabled.
Spring serves `GET /` with 0 VerifyErrors in all three configs (off / MH_EXEC /
MH_EXEC_HIDDEN).

**Limitations:** per-invocation invokeBasic/linkTo* interception is not done
(asm stubs, unsafe); bound receiver/argument *values* aren't read (no species
accessor) — only the bound species *kind* shows; the link-time structural walk
is best-effort (appendix is usually the invoker, not the user MH).

---

## 4D. Runtime target revelation export (semantic JSONL layer)

Invoked at VM shutdown from `before_exit` (runtime/java.cpp) when
`SOROUSH_EXPORT_RUNTIME_TARGETS` is set. Writes one JSON object per line
(JSONL) to the specified path. **Gated and fail-safe: never aborts the VM.
Errors are reported to stderr; the VM continues.** Implemented in
`soroush_graph_export_runtime_targets` (`soroushProvenanceGraph.cpp`), declared
in `.hpp`, wired in `java.cpp`.

This is **not** a raw graph dump. The graph is an internal implementation
detail. The export is a **semantic API** with stable human-readable fields.

### Philosophy: exact or explicitly unresolved

The exporter follows the same no-fake-precision rule as the rest of the engine:
- If a runtime target cannot be identified exactly → **omit the target record**
  and emit a `diagnostic` record explaining why.
- If the graph itself is disabled or empty → emit an informational `diagnostic`
  and still write the `export_summary`.
- On file/OOM errors → report to stderr; the VM continues; summary always written.

**No invented descriptors, no `loader_id=0` fabrications, no guessed targets,
no inferred confidence scores.** Exact or explicitly unresolved.

### Evidence classification

Evidence is determined by the **source node type** (which encodes which runtime
signal fired), not inferred from edge type alone at export time:

| Source node type | Evidence | Meaning |
|-----------------|----------|---------|
| `SG_NODE_REFLECTION_INVOKE` | `LINKAGE_GUARANTEED` | MemberName.resolve — a hard linkage event |
| `SG_NODE_METHODHANDLE_LINKAGE` | `LINKAGE_GUARANTEED` | MH link-time resolution |
| `SG_NODE_MH_ADAPTER` (RESOLVES_TO) | `LINKAGE_GUARANTEED` | DMH structure walk at link time |
| `SG_NODE_EXECUTION` (EXECUTES) | `OBSERVED_ONLY` | Instrumented ENTER event at runtime |

### Record types (stable semantic API)

#### `method_identity`
Every instrumented method registered in the method-token registry. Always
written, independent of `SOROUSH_PROVENANCE_GRAPH`. Fields:
- `token` — stable 32-bit token assigned at rewrite time (1-based)
- `class` — slash-form class name
- `method` — method name
- `descriptor` — JVM descriptor (never `?` — exact or record is omitted)
- `loader_id` — `ClassLoaderData*` as `"0x…"` hex string (loader-precise)
- `hidden` — boolean
- `artifact_crc` — original (pre-rewrite) classfile crc32 hex

#### `runtime_target`
A dynamic callsite resolved to an actual runtime target. Fields:
- `evidence` — `"LINKAGE_GUARANTEED"` or `"OBSERVED_ONLY"`
- `dispatch_kind` — one of `"reflection"`, `"methodhandle_linkage"`,
  `"direct_methodhandle"`, `"execution_trace"`
- `caller_context` — source label (e.g. `"MemberName.resolve"`) or MH kind;
  absent for `execution_trace`
- `target_class`, `target_method`, `target_descriptor` — exact target identity
- `target_loader_id` — `ClassLoaderData*` hex string
- `target_hidden` — boolean

EXECUTES records are deduplicated: one `runtime_target` per distinct Method
node, regardless of call count.

#### `generated_class`
A runtime-generated class from the provenance graph (recovery path). Fields:
- `class`, `loader_id`, `hidden`, `crc`
- `generated_by`, `source_trigger`, `provenance_kind`
- `indy_trace_id` — present when the class was created by an invokedynamic site

**Note:** CGLIB proxies and lambda classes that are loaded from JAR (not
generated by the recovery path) appear as `bytecode_artifact` records
(with `hidden=true` for lambda classes), not as `generated_class` records.
`generated_class` records require `SOROUSH_RUNTIME_RECOVERY=1` and
`soroush_graph_generated_class` to have been called.

#### `bytecode_artifact`
A final or original executable bytecode snapshot. Fields:
- `class`, `loader_id`, `crc`, `size`, `hidden`
- `kind` — `"final"` (post-rewrite) or `"original"` (pre-rewrite)
- `load_kind` — how the class was loaded
- `rewritten_from_crc` — present for `"final"` artifacts; the crc32 of the
  corresponding original artifact, allowing source-to-instrumented tracing

#### `diagnostic`
Emitted when a record cannot be constructed exactly. Always has `level`
(`"info"`, `"warn"`, `"error"`) and `message`. The VM never aborts due to a
diagnostic; the export continues.

#### `export_summary`
Always the last line. Counts: `method_identity_count`, `runtime_target_count`,
`generated_class_count`, `bytecode_artifact_count`, `diagnostic_count`.

### Env var and path

| Setting | Behavior |
|---------|----------|
| `SOROUSH_EXPORT_RUNTIME_TARGETS=1` | write to `/tmp/soroush_jvm_dump/runtime_targets.jsonl` |
| `SOROUSH_EXPORT_RUNTIME_TARGETS=/path/to/file.jsonl` | write to specified path |
| unset or empty | no export |

### Required env var combinations for complete export

```bash
# Full export (all record types, including execution_trace OBSERVED_ONLY):
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_RUNTIME_GRAPH=1 \
SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 \
SOROUSH_REWRITER_PHASE5_PREFIX=<slash/form/prefix> \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/soroush_jvm_dump/targets.jsonl \
  java -Xverify:all -jar app.jar

# LINKAGE_GUARANTEED only (no execution instrumentation needed):
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=1 \
  java -Xverify:all -jar app.jar

# method_identity only (no graph needed; minimal env):
SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 \
SOROUSH_REWRITER_PHASE5_PREFIX=<slash/form/prefix> \
SOROUSH_EXPORT_RUNTIME_TARGETS=1 \
  java -Xverify:all MyClass
```

**Note:** `SOROUSH_RUNTIME_RECOVERY=1` is needed for `generated_class` records.
`SOROUSH_PROVENANCE_GRAPH=1` is needed for `runtime_target`, `generated_class`,
and `bytecode_artifact` records. `method_identity` records are always written
when instrumentation ran (independent of the graph flag).

### Example JSONL records

```jsonl
{"record":"method_identity","token":1,"class":"com/example/springboot/Application","method":"main","descriptor":"([Ljava/lang/String;)V","loader_id":"0x0000000113a81990","hidden":false,"artifact_crc":"7613bd70"}
{"record":"runtime_target","evidence":"LINKAGE_GUARANTEED","dispatch_kind":"methodhandle_linkage","caller_context":"MemberName.resolve","target_class":"java/lang/invoke/LambdaMetafactory","target_method":"metafactory","target_descriptor":"(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite;","target_loader_id":"0x000000013063dd00","target_hidden":false}
{"record":"runtime_target","evidence":"OBSERVED_ONLY","dispatch_kind":"execution_trace","target_class":"com/example/springboot/HelloController","target_method":"index","target_descriptor":"()Ljava/lang/String;","target_loader_id":"0x0000000113a81990","target_hidden":false}
{"record":"generated_class","class":"GraphDemo$$Lambda/0x…","loader_id":"0x000000012fe2a240","hidden":true,"crc":"eb467e14","generated_by":"LambdaMetafactory","source_trigger":"invokedynamic","provenance_kind":"exact","indy_trace_id":1}
{"record":"bytecode_artifact","class":"com/example/springboot/HelloController","loader_id":"0x0000000113a81990","crc":"3ab215ee","size":845,"kind":"final","load_kind":"load","hidden":false,"rewritten_from_crc":"5b2fed5b"}
{"record":"export_summary","method_identity_count":31,"runtime_target_count":4037,"generated_class_count":0,"bytecode_artifact_count":8998,"diagnostic_count":0}
```

### Shutdown hook wiring

`runtime/java.cpp` `before_exit()`:
```cpp
soroush_graph_dump_summary();   // existing graph summary
{
  const char* export_path = ::getenv("SOROUSH_EXPORT_RUNTIME_TARGETS");
  if (export_path != nullptr && export_path[0] != '\0') {
    const char* out = (strcmp(export_path, "1") == 0)
                      ? "/tmp/soroush_jvm_dump/runtime_targets.jsonl"
                      : export_path;
    soroush_graph_export_runtime_targets(out);
  }
}
```

### Validated against

- **GraphDemo** (`method_identity=5`, `runtime_target=143`, `generated_class=16`,
  `bytecode_artifact=779`, `diagnostic=0`, 0 VerifyErrors)
- **OverloadIdentityDemo** (`overload-identity total=31`, 4 distinct `f` descriptor nodes)
- **LoaderIdentityDemo** (`loader-identity total=62`, 2 distinct loader IDs per class)
- **MHExecDemo** (`mh-exec total=2221`)
- **AsyncDemo** (`async-demo total=64`)
- **Spring Boot 4 (gs-spring-boot)** (`method_identity=31`, `runtime_target=4037`,
  `LINKAGE_GUARANTEED=4018`, `OBSERVED_ONLY=19`, `bytecode_artifact=8998`,
  `diagnostic=0`, 0 VerifyErrors)

### Known limitations

- `generated_class` records require the runtime recovery path
  (`SOROUSH_RUNTIME_RECOVERY=1`). CGLIB/lambda classes loaded from JAR appear
  as `bytecode_artifact` records (hidden lambda classes have `hidden=true`).
- `OBSERVED_ONLY` records require PHASE5 instrumentation
  (`SOROUSH_REWRITER_PHASE5_*`) + `SOROUSH_RUNTIME_GRAPH=1`. Without both,
  only `LINKAGE_GUARANTEED` records and `method_identity` records are written.
- The graph caps at 1M nodes / 2M edges; broad framework runs can hit the cap
  (Spring broad `org/springframework` prefix). Records are dropped silently
  beyond the cap (fail-safe), not causing diagnostics.
- `SOROUSH_REWRITER_PHASE5_PREFIX` must be in **JVM internal slash form**
  (e.g. `com/example`). Dot form (`com.example`) silently fails to match.
- `direct_methodhandle` (RESOLVES_TO from MHAdapter) records appear only when
  the native DMH structure walk at `resolve_handle_call` fires; this walk is
  best-effort (appendix is usually the invoker MemberName, not the user MH).

---

## 5. Memory files — read map

The cross-session memory lives in
`/Users/soroushaghajani/.claude/projects/-Users-soroushaghajani-custom-jvm-jdk21u/memory/`.
`MEMORY.md` there is the auto-loaded one-line index. Use this table to decide
what to open for a given task:

| File | Read it when you need… |
|------|------------------------|
| `project_custom_jvm.md` | the overall vision, the six subsystems, env-var list, key modified files |
| `project_jvm_subsystems_status.md` | exact per-subsystem implemented / partial / missing status |
| `project_jvm_remaining_work.md` | the roadmap and what's done vs open (branch widening, constructors, Code-attribute hardening, provenance graph v1, async causality, MH/LambdaForm execution tracing all DONE; graph backend still open) |
| `feedback_verifier_safety_over_coverage.md` | the fail-safe engineering rule — read before touching the rewriter |
| `project_exception_exit_frame_bug.md` | the exception-EXIT handler frame verifier-safety gap and its conservative fix (local-slot type reuse) |
| `project_provenance_graph.md` | the unified provenance graph quick-reference (schema/identity/integration/limitations, incl. the async/cross-thread extension); full detail in §4A + §4B here |

**Decision guide for a new agent:**
- Touching the rewriter / bytecode? → this file §4 + `feedback_verifier_safety_over_coverage.md` + `project_exception_exit_frame_bug.md`.
- Touching the provenance graph / provenance signals? → this file §4A (+ §4B async/cross-thread, + §4C MethodHandle/LambdaForm execution) + `project_provenance_graph.md`.
- Tracing MethodHandle/LambdaForm execution? → this file §4C; note the asm-stub constraint (no per-invocation C++ hook) before designing anything.
- "What's next / what's left?" → `project_jvm_remaining_work.md`.
- Understanding a subsystem's depth? → `project_jvm_subsystems_status.md`.
- Big-picture / onboarding? → `project_custom_jvm.md`.

---

## 6. Recent achievements (rewriter completed end-to-end)

Built on the existing foundation (runtime recovery, indy/reflection tracing,
ENTER/EXIT graph, the rewriter's roundtrip + basic ENTER/EXIT), recent work
brought the **verifier-safe rewriter (subsystem 6) to a complete, real-world-tested
state** — branch widening, an exception-EXIT verifier-safety fix, constructor
instrumentation, and rare/custom Code-attribute hardening — all verified under
`-Xverify:all`, including the gs-spring-boot app and broad-framework runs.

1. **Branch widening (Phases 6A→6C), complete:**
   - 6A: detection + classification of short branches that overflow s2 after instrumentation.
   - 6B-precondition: `soroush_converge_layout`, an iterative fixed-point layout that stabilizes PCs before emission.
   - 6B-emit: `goto→goto_w`, `jsr→jsr_w` from the stabilized layout (javap-verified).
   - 6C: conditional `if*` → inverted-if + `goto_w`, including **synthesizing the
     required fall-through StackMapTable frame** (copied from the branch target's
     frame, since both edges share the same locals/stack).
2. **Exception-EXIT verifier-safety fix:** real-world Spring testing surfaced a
   `VerifyError` (`TomcatWebServer.start`) where a synthetic handler frame used a
   single locals snapshot for a region in which a slot's type changed. Added a
   conservative guard that fails safe (see `project_exception_exit_frame_bug.md`).
3. **Real-world hardening pass:** ran the rewriter against gs-spring-boot (app +
   broad `org/springframework` prefix); added the per-class method census
   (`code_methods` / `ctor_methods_skipped`) to the PHASE5 log.
4. **Constructor (`<init>`) instrumentation:** detect the super()/this()
   delegation, insert ENTER *after* it (so `this` is initialized), normal EXIT
   before `return`, exception-EXIT disabled for constructors, per-method
   safe-skip for shapes we can't prove safe (`super(new Foo())` etc.).
5. **Rare/custom Code-attribute hardening:** the six standard PC-bearing Code
   sub-attributes are remapped; any *other* Code sub-attribute is detected in the
   pre-scan (`soroush_is_known_code_attr`) and the method is **per-method
   safe-skipped** rather than copied with stale PCs (`[JVM REWRITER CODEATTR]`).
   Class-level attributes (e.g. SourceDebugExtension) are copied wholesale and
   unaffected.
6. **Unified provenance graph v1** (subsystem 7, §4A): a new observational,
   fail-safe, env-gated (`SOROUSH_PROVENANCE_GRAPH=1`) in-memory graph
   (`soroushProvenanceGraph.{cpp,hpp}`) with 9 node + 7 edge types that connects
   ENTER/EXIT executions, runtime-generated classes, invokedynamic, reflection/MH
   linkage, and final bytecode into one model. Verified with `GraphDemo`
   (lambda→GeneratedClass, original→final REWRITTEN_FROM, constructor execution)
   and a graph-on Spring run (serves `GET /`, 0 verify errors).
7. **Async / cross-thread causality (provenance graph async extension, §4B):**
   3 new node types (AsyncTask, Thread, Executor) + 4 new edge types (SCHEDULES,
   SUBMITTED_TO, EXECUTES_ASYNC, CONTINUES_ON) so causality survives thread
   boundaries. Native `Thread.start` hooks (`JVM_StartThread`/`thread_entry`,
   side-table keyed by child `JavaThread*`) + gated `System.soroushAsyncHandoff`
   bridge calls in `ThreadPoolExecutor`, `ForkJoinPool.poolSubmit`, and
   `ForkJoinTask.doExec` join submit→run by task-object identity. A worker's
   *pending async context* is consumed at its first ENTER to emit the
   `CONTINUES_ON` link. Verified with `AsyncDemo` (8 cases incl. Thread.start,
   Executor.execute, submit+Callable, CompletableFuture, nested scheduling, pool
   reuse, exception): `async-demo total=64`, 15 CONTINUES_ON edges, 0 verify
   errors; no-op when disabled; graph+async Spring run serves `GET /` with 27
   Thread nodes + 46 async edges, 0 verify errors.
8. **MethodHandle / LambdaForm execution tracing (§4C):** closes the
   linkage→execution gap. 2 new node types (MethodHandleAdapter,
   LambdaFormExecution) + 5 edge types (MH_INVOKES, ADAPTS_TO, BINDS_TO,
   INVOKE_BASIC, RESOLVES_TO). Primary mechanism: `SOROUSH_TRACE_MH_EXEC=1`
   additively instruments the JDK MH-internal classes via the verifier-safe
   rewriter so the adapter chain *executes* as Execution/LambdaFormExecution
   nodes; a separate experimental `SOROUSH_TRACE_MH_EXEC_HIDDEN=1` also
   instruments the hidden runtime-customized dispatch LambdaForms so the
   per-invocation chain caller→adapters→final-target is explicit. Plus a
   fail-safe native read-only structure walk at `resolve_handle_call`. The asm
   stub `invokeBasic`/`linkTo*` path is deliberately *not* hooked (unsafe).
   Verified with `MHExecDemo` (6 cases): `mh-exec total=2221`, ~92
   LambdaFormExecution nodes (default) / explicit dispatch chain (hidden), 0
   verify errors; no-op when disabled; Spring serves `GET /` (0 verify errors)
   in all three configs.
9. **Loader-precise graph identity (§4A):** Class / GeneratedClass /
   BytecodeArtifact node identity now includes the defining loader
   (`ClassLoaderData*`), and Method is loader-precise transitively (it keys off
   the loader-specific Class node id). Two runtime classes with the same internal
   name under different loaders are now **distinct nodes** (v1 merged them). The
   execution path gets the loader exactly from the method token (achievement #10/#11;
   the original best-effort `vframeStream` walk was later removed); reflection/MH
   linkage and the MH link-time walk use the target holder's CLD. New diagnostics `[JVM GRAPH
   BYTECODE]` + loader-aware `[JVM GRAPH CLASS]`/`[JVM GRAPH NOTE]
   loader-divergence`. Verified with `LoaderIdentityDemo` (same name under two
   custom loaders → two distinct Class/Method/BytecodeArtifact nodes,
   `loader-identity total=62`, 0 verify errors) and full regression
   (GraphDemo=223, AsyncDemo=64, MHExecDemo=2221, ctor/recovery/stress demos,
   Spring `GET /` with 227 loader-divergence NOTEs all 0 verify errors).
10. **Exact execution identity — method-token trace ABI (§4 "Trace ABI" + §4A
    "Exact execution identity"):** eliminates the best-effort execution fallback.
    The rewriter (Phase 3/5) now assigns each instrumented method a stable token,
    registers its exact class/name/**descriptor**/loader/hidden/crc at rewrite
    time (method-token registry in `soroushProvenanceGraph.{cpp,hpp}`), and injects
    `System.soroushTraceEnter/Exit(int)` (one `CONSTANT_Integer` per method →
    same 6-byte/1-slot prologue, so pc-map/StackMapTable/widening logic unchanged).
    `JVM_SoroushTraceEnter/Exit` resolve the token and call
    `soroush_graph_execution_exact`, keying Method nodes by exact
    `(class_node, name, descriptor)` — **no `?` descriptor, no `loader_id=0`** for
    instrumented methods; overloads stay distinct. Fail-fast: unresolved tokens are
    marked `identity-unresolved` (no fake node); graph-capacity drops are
    distinguished from unresolved. Verified with `OverloadIdentityDemo` (`f` → 4 distinct
    descriptor-keyed Method nodes, `g`/`<init>` → 2 each, `overload-identity
    total=31`, 0 `?` nodes, 0 loader=0, 0 unresolved), `LoaderIdentityDemo` (exact
    descriptors per loader), and full regression (GraphDemo=223 incl. exact
    `lambda$main$0 (I)I`, AsyncDemo=64 worker methods exact, MHExecDemo=2221,
    ctor/stress demos, Spring `GET /` 19032 token registrations, **0
    identity-unresolved**, 0 verify errors). A crash fix was required: the token
    natives run on worker threads and call `as_utf8_string`, so `soroush_trace_token`
    needs a `ResourceMark`.
11. **Legacy string-tracing path removed — token ABI is the sole execution tracer.**
    Deleted the entire best-effort string path now that the exact token ABI is
    authoritative: the `System.soroushTrace(String)` native + its `jvm.h`/`System.java`/
    `System.c`/symbols-unix plumbing; the `soroush_parse_runtime_event` string parser;
    the `vframeStream` `soroush_caller_loader_id` best-effort loader walk; the
    `soroush_graph_execution` best-effort graph entry (the only code that ever created
    a `…|?` descriptor / `loader_id=0` execution node); and the legacy hand-rolled
    `rewrite_soroush_main` rewriter + `should_rewrite_soroush_class` + `SOROUSH_REWRITE_PREFIX`
    + its byte/emit helpers + `[JVM REWRITE]` log. `soroush_runtime_record_event` lost its
    `exact` branch (always exact). Stress runners that grepped the removed bare
    `ENTER <class>.<method>` echo (`run-fake-codeattr.sh`, `run-goto-widen-stress.sh`,
    `run-cond-widen-stress.sh`, `run-mh-exec-demo.sh`) were migrated to the
    `[JVM TRACE METHOD] ENTER token=… <class>.<method><desc>` line. Verified: full
    regression green (all demos + both Spring runs, app- and broad-prefix) under
    `-Xverify:all` — **0 VerifyErrors, 0 `?`-descriptor execution Method nodes
    (Spring previously had 1 from the legacy path → now 0), 0 `loader_id=0`,
    0 identity-unresolved, 0 `[JVM REWRITE]` lines**.

12. **Runtime target revelation export — semantic JSONL layer (§4D):** a new
    `soroush_graph_export_runtime_targets` function appended to
    `soroushProvenanceGraph.cpp` (~500 new lines) + declaration in `.hpp` +
    env-gated call in `java.cpp before_exit`. Exports semantic JSONL records at
    VM shutdown: `method_identity` (always; from the method-token registry,
    independent of the graph flag), `runtime_target` (LINKAGE_GUARANTEED from
    MH/reflection linkage + OBSERVED_ONLY from instrumented execution trace,
    deduplicated per distinct Method node), `generated_class` (recovery path),
    `bytecode_artifact` (original + final, with `rewritten_from_crc` linking
    final → original), `diagnostic` (emitted on parse failures instead of
    fabricating records), `export_summary` (always last). Evidence classification
    is determined by the source node type (LINKAGE_GUARANTEED for
    SG_NODE_REFLECTION_INVOKE / SG_NODE_METHODHANDLE_LINKAGE / SG_NODE_MH_ADAPTER,
    OBSERVED_ONLY for SG_NODE_EXECUTION) — not inferred from the edge type alone.
    No fake precision: an unresolvable Method node key emits a diagnostic and omits
    the record. Fixed a pre-existing bug: BytecodeArtifact nodes were stored with
    `flags=0` (hidden bit lost); one-character fix stores `hidden ? 1 : 0` so the
    export correctly reports `hidden`. Env var: `SOROUSH_EXPORT_RUNTIME_TARGETS=1`
    (uses `/tmp/soroush_jvm_dump/runtime_targets.jsonl`) or a custom path.
    Validated: 0 VerifyErrors, 0 diagnostics on GraphDemo, OverloadIdentityDemo,
    LoaderIdentityDemo, MHExecDemo, AsyncDemo, and Spring Boot 4 (31 method
    identities, 4037 runtime targets, 8998 bytecode artifacts).

### Files changed (working tree vs last commit `9ca2ecfe356`)
- `src/hotspot/share/classfile/soroushClassfileRewriter.cpp` — the bulk: convergence, widening emit, StackMapTable synthesis, exception-EXIT guard, constructor support, cp-index parsing, unknown-Code-attribute guard. **Exact method-token trace ABI:** per-method `CONSTANT_Integer` token + `soroushTraceEnter/Exit(I)V` methodrefs (replacing per-method ENTER/EXIT Strings + single `soroushTrace(String)`); captures the descriptor and registers each method via `soroush_method_token_register`.
- `src/hotspot/share/classfile/soroushClassfileRewriter.hpp` — `TransformResult` census fields; `insert_entry_trace`/`insert_entry_exit_trace` now take `loader_id`/`hidden`/`artifact_crc`.
- `src/hotspot/share/classfile/klassFactory.cpp` — PHASE5 census logging + graph generated-class/bytecode integration; passes the defining `ClassLoaderData*` + `hidden` to `soroush_graph_bytecode` (§4A) and `loader_id`/`hidden`/original-`crc` to the Phase 3/5 trace inserters (§4A exact identity). **Removed** the legacy hand-rolled `rewrite_soroush_main` rewriter, `should_rewrite_soroush_class`/`SOROUSH_REWRITE_PREFIX`, the legacy byte/emit helpers (`read_u2`/`read_u4`/`write_u2`/`write_u4`/`emit_*`/`SoroushMethodRewrite`/`[JVM REWRITE]`), and its pipeline call block.
- `src/hotspot/share/classfile/soroushProvenanceGraph.{cpp,hpp}` — **new** unified provenance graph module (subsystem 7, §4A; **loader-precise Class/GeneratedClass/BytecodeArtifact keys + name→first-loader divergence side-table + `[JVM GRAPH BYTECODE]` diagnostics**; **method-token registry `soroush_method_token_register`/`_lookup` + `soroush_graph_execution_exact` for exact descriptor-keyed Method nodes**) + async extension (§4B: AsyncTask/Thread/Executor nodes, SCHEDULES/SUBMITTED_TO/EXECUTES_ASYNC/CONTINUES_ON edges, submit/run side-table) + MH-execution extension (§4C: MethodHandleAdapter/LambdaFormExecution nodes, MH_INVOKES/ADAPTS_TO/BINDS_TO/INVOKE_BASIC/RESOLVES_TO edges, `soroush_graph_mh_chain`/`soroush_graph_lambdaform_exec`).
- Graph integration calls in `prims/jvm.cpp`: **exact execution identity (sole tracer)** — `JVM_SoroushTraceEnter/Exit` natives + `soroush_trace_token` (token lookup, `ResourceMark`) + `soroush_runtime_record_event` recorder → `soroush_graph_execution_exact`; async (thread-start side-table, `JVM_StartThread`/`thread_entry` hooks, pending-context consumption, `JVM_SoroushAsyncEnabled`/`JVM_SoroushAsyncHandoff`). **Removed** the legacy `JVM_SoroushTrace` native, `soroush_parse_runtime_event`, `soroush_caller_loader_id`, and the `exact=false` best-effort branch. Also `prims/methodHandles.cpp` (reflection/MH; passes target holder CLD), `interpreter/linkResolver.cpp` (MH link-time walk passes resolved target CLD), `classfile/systemDictionary.cpp` (indy), `runtime/java.cpp` (shutdown summary).
- `src/hotspot/share/classfile/soroushProvenanceGraph.{cpp,hpp}` — **removed** the best-effort `soroush_graph_execution` (the only `…|?`-descriptor / `loader_id=0` execution-node creator); `soroush_graph_execution_exact` is now the sole execution-node path.
- Native plumbing: `prims/jvm.h` (`JVM_SoroushTraceEnter`/`Exit` + 2 async prototypes; `JVM_SoroushTrace` removed), `java.base/.../System.java` (`soroushTraceEnter`/`soroushTraceExit` + 2 async native decls; `soroushTrace(String)` removed), `java.base/.../native/libjava/System.c` (registration; `soroushTrace` removed), `make/data/hotspot-symbols/symbols-unix` (exports for `JVM_SoroushTraceEnter`/`Exit` + 2 async; `JVM_SoroushTrace` removed).
- Async JDK integration (gated `if (SOROUSH_ASYNC)` + `static final` gate field): `java.base/.../util/concurrent/{ThreadPoolExecutor,ForkJoinPool,ForkJoinTask}.java`.
- MH/LambdaForm execution tracing (§4C): `interpreter/linkResolver.cpp` (read-only MH-structure walk + `[JVM MH EXEC]` diagnostics at `resolve_handle_call`); `prims/jvm.cpp` (LambdaFormExecution overlay in `JVM_SoroushTrace` + `soroush_runtime_current_exec_id`); `classfile/klassFactory.cpp` (additive `SOROUSH_TRACE_MH_EXEC` + hidden `SOROUSH_TRACE_MH_EXEC_HIDDEN` rewriter toggles for MH-internal classes).
- (Earlier subsystems also touched: `jvm.cpp`/`jvm.h`, `System.java`/`System.c`, `java.cpp`, `systemDictionary.cpp`, `methodHandles.cpp`, `linkResolver.cpp`, `runtime/reflection.cpp`.)
- **Not yet committed** — all of this is in the working tree on top of `9ca2ecfe356`.

---

## 7. Build, run, test

```bash
# Build (from repo root). Output JDK:
#   build/macosx-aarch64-server-fastdebug/jdk
cd /Users/soroushaghajani/custom-jvm/jdk21u && make images

# Run a class with full ENTER/EXIT instrumentation under the verifier:
JH=build/macosx-aarch64-server-fastdebug/jdk
SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 SOROUSH_REWRITER_PHASE5_PREFIX=MyClass \
  "$JH/bin/java" -Xverify:all -cp <dir> MyClass
```

### Demos & runners — `/Users/soroushaghajani/Downloads/bugged/jvm-dump-demo/`
- Widening: `run-branch-widen-stress.sh` (ifle/invert), `run-goto-widen-stress.sh` (goto_w), `run-cond-widen-stress.sh` (ifle / if_icmp* / ifnull, with javap proof). Generators: `*StressGen.java`.
- Constructors: `CtorDemo.java` (7 cases: simple, branch, this()-chain, throw-after-super, try/catch, pre-super arg, pre-super-`new` safe-skip, delegation-fails). Run: `SOROUSH_REWRITER_PHASE5_PREFIX=Ctor`.
- Custom Code attribute: `run-fake-codeattr.sh` (`FakeAttrTarget.java` + `FakeCodeAttrPatcher.java`, a direct classfile patcher that injects a fake `SoroushFakeCodeAttr` Code sub-attribute) — confirms the affected method is safe-skipped while the rest instruments.
- Provenance graph (§4A): `GraphDemo.java` exercises all signals (constructor + method executions, a lambda → invokedynamic → generated class, a reflective `Method.invoke`, rewritten bytecode). Run with `SOROUSH_PROVENANCE_GRAPH=1 SOROUSH_RUNTIME_GRAPH=1 SOROUSH_RUNTIME_RECOVERY=1 SOROUSH_TRACE_INDY=1 SOROUSH_TRACE_REFLECTION=1 SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 SOROUSH_REWRITER_PHASE5_PREFIX=GraphDemo`; expect `graph-demo=223` and a `[JVM GRAPH SUMMARY]`.
- Async causality (§4B): `AsyncDemo.java` + `run-async-demo.sh` — 8 cases (Thread.start, Executor.execute, submit+Callable, CompletableFuture.supplyAsync, nested scheduling, multiple workers, pool reuse, exception-in-task). Runner enables `SOROUSH_PROVENANCE_GRAPH=1 SOROUSH_RUNTIME_GRAPH=1 SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 SOROUSH_REWRITER_PHASE5_PREFIX=AsyncDemo` and prints the async diagnostics/edges/summary; expect `async-demo total=64`, CONTINUES_ON/EXECUTES_ASYNC edges, 0 VerifyErrors.
- MethodHandle/LambdaForm execution (§4C): `MHExecDemo.java` + `run-mh-exec-demo.sh` — 6 cases (invokeExact, asType, bindTo, lambda/indy, reflection→MH, chained bindTo+asType). Runner enables `SOROUSH_PROVENANCE_GRAPH=1 SOROUSH_RUNTIME_GRAPH=1 SOROUSH_TRACE_MH_EXEC=1 SOROUSH_REWRITER_PHASE5_*`; expect `mh-exec total=2221`, `[JVM GRAPH MH]` LambdaFormExecution nodes + INVOKE_BASIC edges, 0 VerifyErrors. Add `SOROUSH_TRACE_MH_EXEC_HIDDEN=1` to also trace the hidden dispatch LambdaForms (the per-invocation chain to the final target).
- Loader-precise identity (§4A): `LoaderIdentityDemo.java` + `SharedWidget.java` + `run-loader-identity-demo.sh` — loads the **same internal class name `SharedWidget` through two distinct custom ClassLoaders**, executes ctor+methods on each. Runner enables `SOROUSH_PROVENANCE_GRAPH=1 SOROUSH_RUNTIME_GRAPH=1 SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 SOROUSH_REWRITER_PHASE5_PREFIX=SharedWidget` under `-Xverify:all`. Expect `loader-identity total=62`; **two distinct `SharedWidget` Class nodes** (distinct `loader=` values), a `[JVM GRAPH NOTE] loader-divergence`, `<init>`/compute/helper Method nodes under **both** Class nodes (distinct method-node ids), an original+final BytecodeArtifact per loader (identical crc but distinct nodes, each attached to its own Class node), and 0 VerifyErrors. The runner prints a `PASS:` verdict.
- Exact execution identity (§4A): `OverloadIdentityDemo.java` + `run-exact-identity-demo.sh` — overloaded methods sharing a name but differing by descriptor (`f(I)I`/`f(J)I`/`f(II)I`/`f(String)String`, two `g`, two `<init>`). Runner enables `SOROUSH_PROVENANCE_GRAPH=1 SOROUSH_RUNTIME_GRAPH=1 SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 SOROUSH_REWRITER_PHASE5_PREFIX=OverloadIdentityDemo` under `-Xverify:all`. Expect `overload-identity total=31`; **`f` → 4 distinct descriptor-keyed Method nodes**, `g`→2, `<init>`→2; **0 `?`-descriptor Method nodes, 0 `loader=0`, 0 identity-unresolved**, 0 VerifyErrors; `[JVM TRACE METHOD] register …` + `[JVM GRAPH IDENTITY] token=… method_node=…` lines; runner prints a `PASS:` verdict.
- Originals: `DecoderStressDemo`, `TypeAnnotationStressDemo`, `ReturnTypesStressDemo`, `ExceptionExitStressDemo`, `ReflectionDispatchDemo`.

### Real-world app — gs-spring-boot
```bash
# Built fat jar: gs-spring-boot/complete/target/spring-boot-complete-0.0.1-SNAPSHOT.jar
gs-spring-boot/complete/run-rewriter-test.sh com/example/springboot apponly   # app classes
gs-spring-boot/complete/run-rewriter-test.sh org/springframework broad        # framework-wide
# Success criterion: app starts, GET / -> "Greetings from Spring Boot!", 0 VerifyErrors.

# Full export run (all record types):
JH="build/macosx-aarch64-server-fastdebug/jdk"
SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 \
SOROUSH_REWRITER_PHASE5_PREFIX="com/example" \
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_RUNTIME_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS="/tmp/soroush_jvm_dump/targets.jsonl" \
  "$JH/bin/java" -Xverify:all -jar spring-boot-complete-0.0.1-SNAPSHOT.jar
# Expected: [JVM EXPORT] done: … (method_identity=31 runtime_target=4037
#            generated_class=0 bytecode_artifact=8998 diagnostic=0)
# NOTE: prefix must be slash form (com/example), not dot form (com.example).
```

---

## 8. Runtime env-var reference

| Env var | Effect |
|---------|--------|
| `SOROUSH_RUNTIME_RECOVERY=1` | dump + recover runtime-generated classes (subsystem 1; `[JVM DUMP]` / `[JVM RECOVER]` / `[JVM INDY]`) |
| `SOROUSH_TRACE_INDY=1` | invokedynamic/lambda linkage trace (subsystem 2) |
| `SOROUSH_TRACE_REFLECTION=1` | reflection / MethodHandle **linkage** tracing (subsystems 2/3; `[JVM REFLECT]`) — MemberName resolution, runtime dispatch, invokehandle target, mh_target walk |
| `SOROUSH_CAPTURE_FINAL_BYTECODE=1` | dump final post-transform bytecode to `/tmp/soroush_jvm_dump` (subsystem 4; `[JVM FINAL BYTECODE]`) |
| `SOROUSH_RUNTIME_GRAPH=1` | persistent ENTER/EXIT graph + `[JVM TRACE]` events (subsystem 5) |
| `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1` + `SOROUSH_REWRITER_PHASE5_PREFIX=<pkg>` | the main verifier-safe ENTER+EXIT rewriter (subsystem 6) |
| `SOROUSH_REWRITER_PHASE3_ENTER` / `_PHASE2_ENTRY_NOPS` / `_PHASE1` (+ matching `_PHASE{2,3}_PREFIX`) | lighter verifier-safe rewriter phases; `SOROUSH_REWRITER_PHASE1_FAILURES_ONLY=1` logs only roundtrip mismatches |
| `SOROUSH_PROVENANCE_GRAPH=1` | unified provenance graph (§4A) **and async/cross-thread causality (§4B) and MH-execution graph nodes (§4C)** — all gated by this one flag. Combine with the signals above to populate it (e.g. `SOROUSH_RUNTIME_GRAPH=1` for executions — also required for async worker-execution linkage and MH LambdaFormExecution nodes, `SOROUSH_RUNTIME_RECOVERY=1` for generated classes, `SOROUSH_TRACE_INDY=1` to enrich indy nodes). Async edges need the relevant task bodies instrumented via the PHASE5 rewriter. |
| `SOROUSH_TRACE_MH_EXEC=1` | MethodHandle/LambdaForm execution tracing (§4C): `[JVM MH EXEC]`/`[JVM LAMBDAFORM EXEC]`/`[JVM MH ADAPTER]` logs + (when PHASE5 on) additively instruments the JDK MH-internal classes so the adapter chain executes visibly. |
| `SOROUSH_TRACE_MH_EXEC_HIDDEN=1` | experimental (§4C): also instrument the *hidden* runtime-customized dispatch LambdaForms (`LambdaForm$DMH/$MH/$BMH+0x…`) so the per-invocation chain to the final target is explicit. Heavier; demo+Spring-validated safe; off by default. Requires `SOROUSH_TRACE_MH_EXEC=1` + PHASE5. |
| `SOROUSH_EXPORT_RUNTIME_TARGETS=1` | write semantic JSONL runtime target revelation export to `/tmp/soroush_jvm_dump/runtime_targets.jsonl` at VM shutdown (§4D). Set to a path (e.g. `/tmp/targets.jsonl`) to override the output file. Requires `SOROUSH_PROVENANCE_GRAPH=1` for graph-derived records. `method_identity` records are written independent of the graph flag. Fail-safe: errors reported to stderr; never aborts the VM. |

All dumps go to `/tmp/soroush_jvm_dump`.

**Prefix slash-form rule:** `SOROUSH_REWRITER_PHASE5_PREFIX` (and Phase2/3
equivalents) must use JVM internal slash-form class names. The prefix is
matched with `strncmp` against the internal name (e.g.
`com/example/springboot/Application`). Dot form (`com.example`) will
silently not match. Simple unprefixed class names (`GraphDemo`,
`AsyncDemo`) work correctly because they have no separator character.

---

## 9. Where to go next (open work)

From `project_jvm_remaining_work.md`, still open: broader real-world benchmarks
and a persistent/queryable graph backend. The **unified provenance graph** exists
at v1 (§4A), has **async / cross-thread causality** (§4B: Executors,
CompletableFuture/ForkJoinPool, Thread.start), and now **MethodHandle/LambdaForm
execution tracing** (§4C). MH-execution follow-ups: per-invocation
invokeBasic/linkTo* interception is not done (asm stubs — unsafe); bound
receiver/argument *values* aren't read (no `BoundMethodHandle` species accessor);
the link-time structural walk is best-effort (the appendix is usually the invoker,
not the user MH). Async follow-ups: linkage when a task body is *not* instrumented
(no worker Execution to attach), task-identity-hash collisions, logical
CompletableFuture dependent-stage chains (vs. per-stage submit/run), and
virtual-thread / structured-concurrency carrier causality. Graph follow-ups:
**distinct-loader-aware class identity is now DONE** (Class/GeneratedClass/
BytecodeArtifact keyed by `ClassLoaderData*`, Method transitively; see §4A
"Loader-precise identity" + `LoaderIdentityDemo`), and **exact execution identity
is now DONE** for instrumented methods (method-token trace ABI: exact
descriptor + loader/hidden, no `?`/`loader_id=0` fallback; see §4A "Exact
execution identity" + `OverloadIdentityDemo`); the legacy best-effort string trace
path (`System.soroushTrace(String)` + `rewrite_soroush_main`) has been **removed**
— the token ABI is the sole execution tracer. Remaining graph follow-ups: the
divergence-NOTE side table is direct-mapped (diagnostic-only collisions);
scoping/filtering by prefix (it currently records all loaded classes — Spring broad
runs hit the 1M node cap); edge dedup; and a persistent/queryable backend
(the **semantic JSONL export layer is now DONE** — see §4D; in-memory + shutdown
export works; a persistent live-queryable backend remains open). Smaller rewriter
follow-ups: recovering the conservative over-skips — operand-stack ref-type
tracking / common-supertype frame merging for the exception-EXIT guard,
operand-stack-aware constructor delegation detection (to instrument
`super(new Foo())` shapes), and an env-configurable allowlist for known-harmless
custom Code sub-attributes. Note the Phase-2 NOP path (`soroush_transform_code_attribute`,
test-only) is *not* hardened (copies unknown attrs, lacks type-annotation
remapping); the real instrumentation runs through Phase 3/5 (`*_entry_code`).
