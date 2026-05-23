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
| 2 | invokedynamic / LambdaMetafactory linkage tracing | working (linkage layer) | `methodHandles.cpp`, `linkResolver.cpp`, `systemDictionary.cpp` |
| 3 | reflection / MethodHandle linkage tracing | working (linkage layer) | `reflection.cpp`, `methodHandles.cpp` |
| 4 | final executable bytecode capture (post-transform) | working | `klassFactory.cpp` |
| 5 | persistent runtime ENTER/EXIT execution graph | working (single-thread) | `jvm.cpp`/`jvm.h`, `System.java`/`System.c` |
| 6 | **verifier-safe bytecode rewriter** (the heart) | branch widening + constructor instrumentation + rare/custom Code-attribute hardening complete | `soroushClassfileRewriter.{cpp,hpp}` + `klassFactory.cpp` |

For a precise per-subsystem "implemented / partial / missing" breakdown, read
[`project_jvm_subsystems_status.md`](#5-memory-files--read-map).

---

## 4. The rewriter (subsystem 6) — the part most work touches

All rewriting lives in **`src/hotspot/share/classfile/soroushClassfileRewriter.cpp`**
(public API in the `.hpp`). It is invoked from `klassFactory.cpp` during class
load, gated by env vars (below). It is a self-contained classfile parser /
emitter — it does NOT depend on the rest of HotSpot's classfile machinery.

### Phases (entry points in the `.cpp`)

| Phase | API | What it does | Enable with |
|-------|-----|--------------|-------------|
| 1 | `roundtrip_copy` | parse + byte-identical re-emit (sanity) | `SOROUSH_REWRITER_PHASE1=1` |
| 2 | `insert_entry_nops` | prepend NOPs (layout test) | `SOROUSH_REWRITER_PHASE2_ENTRY_NOPS=1` + `SOROUSH_REWRITER_PHASE2_PREFIX=<pkg>` |
| 3 | `insert_entry_trace` | ENTER only | `SOROUSH_REWRITER_PHASE3_ENTER=1` + `SOROUSH_REWRITER_PHASE3_PREFIX=<pkg>` |
| 5 | `insert_entry_exit_trace` | **ENTER + normal EXIT + exception EXIT** (the main path) | `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1` + `SOROUSH_REWRITER_PHASE5_PREFIX=<pkg>` |

`<pkg>` is an internal-name prefix (`com/example/springboot`, `org/springframework`,
or `*` for all). Instrumentation injects calls to `System.soroushTrace(String)`
with `"ENTER <class>.<method>"` / `"EXIT <class>.<method>"`.

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
- `[JVM TRACE] …` / bare `ENTER …` / `EXIT …` — runtime execution-graph events (`System.soroushTrace`).
- `[JVM DUMP] …` — recovered runtime-generated classes (subsystem 1).

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
| `project_jvm_remaining_work.md` | the roadmap and what's done vs open (branch widening DONE; constructors DONE; rare/custom Code-attribute hardening DONE; async causality, MH execution tracing, unified graph still open) |
| `feedback_verifier_safety_over_coverage.md` | the fail-safe engineering rule — read before touching the rewriter |
| `project_exception_exit_frame_bug.md` | the exception-EXIT handler frame verifier-safety gap and its conservative fix (local-slot type reuse) |

**Decision guide for a new agent:**
- Touching the rewriter / bytecode? → this file §4 + `feedback_verifier_safety_over_coverage.md` + `project_exception_exit_frame_bug.md`.
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

### Files changed (working tree vs last commit `9ca2ecfe356`)
- `src/hotspot/share/classfile/soroushClassfileRewriter.cpp` — the bulk: convergence, widening emit, StackMapTable synthesis, exception-EXIT guard, constructor support, cp-index parsing, unknown-Code-attribute guard.
- `src/hotspot/share/classfile/soroushClassfileRewriter.hpp` — `TransformResult` census fields.
- `src/hotspot/share/classfile/klassFactory.cpp` — PHASE5 census logging.
- (Earlier subsystems also touched: `jvm.cpp`/`jvm.h`, `System.java`/`System.c`, `java.cpp`, `systemDictionary.cpp`, `methodHandles.cpp`, `linkResolver.cpp`, `reflection.cpp`.)
- **Not yet committed** — all rewriter work is in the working tree on top of `9ca2ecfe356`.

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
- Originals: `DecoderStressDemo`, `TypeAnnotationStressDemo`, `ReturnTypesStressDemo`, `ExceptionExitStressDemo`.

### Real-world app — gs-spring-boot
```bash
# Built fat jar: gs-spring-boot/complete/target/spring-boot-complete-0.0.1-SNAPSHOT.jar
gs-spring-boot/complete/run-rewriter-test.sh com/example/springboot apponly   # app classes
gs-spring-boot/complete/run-rewriter-test.sh org/springframework broad        # framework-wide
# Success criterion: app starts, GET / -> "Greetings from Spring Boot!", 0 VerifyErrors.
```

---

## 8. Runtime env-var reference

| Env var | Effect |
|---------|--------|
| `SOROUSH_RUNTIME_RECOVERY=1` | dump runtime-generated classes (subsystem 1) |
| `SOROUSH_TRACE_INDY=1` | invokedynamic/lambda linkage trace (subsystem 2) |
| `SOROUSH_CAPTURE_FINAL_BYTECODE=1` | dump final post-transform bytecode to `/tmp/soroush_jvm_dump` (subsystem 4) |
| `SOROUSH_RUNTIME_GRAPH=1` | persistent ENTER/EXIT graph + `[JVM TRACE]` events (subsystem 5) |
| `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1` + `SOROUSH_REWRITER_PHASE5_PREFIX=<pkg>` | the main ENTER+EXIT rewriter (subsystem 6) |
| `SOROUSH_REWRITER_PHASE3_ENTER` / `_PHASE2_ENTRY_NOPS` / `_PHASE1` (+ prefixes) | lighter rewriter phases |

All dumps go to `/tmp/soroush_jvm_dump`.

---

## 9. Where to go next (open work)

From `project_jvm_remaining_work.md`, still open: **async / cross-thread
causality** (Executors, CompletableFuture, callback lineage), **full
MethodHandle/LambdaForm execution tracing** (beyond linkage), a **unified
provenance graph** tying the five tracing subsystems + recovered classes + final
bytecode together, and broader real-world benchmarks. Smaller rewriter
follow-ups: recovering the conservative over-skips — operand-stack ref-type
tracking / common-supertype frame merging for the exception-EXIT guard,
operand-stack-aware constructor delegation detection (to instrument
`super(new Foo())` shapes), and an env-configurable allowlist for known-harmless
custom Code sub-attributes. Note the Phase-2 NOP path (`soroush_transform_code_attribute`,
test-only) is *not* hardened (copies unknown attrs, lacks type-annotation
remapping); the real instrumentation runs through Phase 3/5 (`*_entry_code`).
