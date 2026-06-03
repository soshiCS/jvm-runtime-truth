# Runtime Target Export — Engineering Spec

## Branch goal

```
Run Java slices under a modified JVM (-Xint mode)
  → capture every dynamic dispatch target at first resolution
  → export executed bytecode artifacts
  → emit a semantic JSONL file enabling downstream staticization
```

This branch instruments HotSpot to reveal what would otherwise be invisible at
static analysis time: the actual runtime targets of `invokedynamic`, `MethodHandle`
invoke, and reflection dispatch — plus the bytecode of every runtime-generated or
runtime-transformed class.

The output is a self-describing JSONL file consumed by rt's staticization
pipeline. Every record is either **exact** (a real runtime target with verified
identity) or an **explicit diagnostic** (explaining why a site could not be
represented exactly). There is no fake precision, no guessed target, no silent
omission.

---

## What this branch covers

| Feature | Doc |
|---------|-----|
| JSONL record types and field semantics | [EXPORT_FORMAT.md](EXPORT_FORMAT.md) |
| System architecture and data flow | [ARCHITECTURE.md](ARCHITECTURE.md) |
| MethodHandle callsite attribution (Case A / A2 / B) | [CALLSITE_ATTRIBUTION.md](CALLSITE_ATTRIBUTION.md) |
| Runtime MH receiver recovery from live stack | [RUNTIME_RECEIVER_RECOVERY.md](RUNTIME_RECEIVER_RECOVERY.md) |
| BMH adapter graph extraction (asType, filterArguments, etc.) | [ADAPTER_GRAPHS.md](ADAPTER_GRAPHS.md) |
| Direct MH invoke capture | [MH_CAPTURE.md](MH_CAPTURE.md) |
| Reflection Method.invoke / Constructor.newInstance capture | [REFLECTION_CAPTURE.md](REFLECTION_CAPTURE.md) |
| invokedynamic / LambdaMetafactory capture | [INDY_CAPTURE.md](INDY_CAPTURE.md) |
| Runtime-generated class export (proxies, lambda classes) | [GENERATED_CLASSES.md](GENERATED_CLASSES.md) |
| Final executable bytecode artifact export | [BYTECODE_ARTIFACTS.md](BYTECODE_ARTIFACTS.md) |
| Known limitations and deferred work | [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) |
| Validation programs and test guarantees | [VALIDATION_AND_DEMOS.md](VALIDATION_AND_DEMOS.md) |
| Exact source files, function names, line ranges | [FILES_AND_PATCH_POINTS.md](FILES_AND_PATCH_POINTS.md) |

---

## What this branch does NOT cover

- General JVM observability product vision
- Async/cross-thread causality (implemented in the broader fork, not relevant here)
- Debugging UI or graph database backend
- Failure reconstruction UX
- Full MethodHandle/LambdaForm execution tracing (§4C in the broader spec — separate feature)
- Future startup optimization plans

---

## Runtime assumptions

**This branch requires `-Xint` (interpreter-only) mode.** Every MH invocation must
go through the template interpreter so that exact frame state is available when
`resolve_handle_call` fires. Compiled frames lack the required stack/local variable
metadata. See [KNOWN_LIMITATIONS.md § compiled frames](KNOWN_LIMITATIONS.md#compiled-frame-recovery-deferred).

---

## Quick start

```bash
# Minimum — callsite records + generated class export:
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/out.jsonl \
SOROUSH_RUNTIME_RECOVERY=1 \
  java -Xverify:all -Xint -cp <classpath> <MainClass>

# Full export (adds INDY trace, bytecode artifacts, rewriter-phase5):
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/out.jsonl \
SOROUSH_RUNTIME_RECOVERY=1 \
SOROUSH_CAPTURE_FINAL_BYTECODE=1 \
SOROUSH_TRACE_INDY=1 \
SOROUSH_RUNTIME_RECOVERY=1 \
SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1 \
SOROUSH_REWRITER_PHASE5_PREFIX=<slash/form/prefix> \
  java -Xverify:all -Xint -cp <classpath> <MainClass>
```

The JSONL file is written at VM shutdown by the `before_exit` hook in
`runtime/java.cpp:470`.

---

## Current state (as of 2026-05-25)

**Complete:**
- callsite_target records (direct DMH invoke — exact target, exact BCI, exact loader)
- callsite_target_set records (guardWithTest, catchException — exact role-tagged targets)
- callsite_adapter_graph records (BMH adapter chains — structural extraction)
- invokedynamic / LambdaMetafactory callsite records
- reflection Method.invoke and Constructor.newInstance target capture
- generated_class records (proxies, ByteBuddy, CGLIB, lambda classes)
- bytecode_artifact records (pre- and post-rewrite class bytes)
- Runtime MH receiver recovery (Case A / A2 / B)
- Sibling BCI scan (no silent omissions for shared CP-cache entries)
- 11/11 showcase sections passing (RuntimeTargetShowcaseDemo)
- 0 demo-related diagnostics
- Spring Boot validation (0 VerifyErrors)

**Deferred / known limitations:**
- 4 JDK-internal diagnostics (ByteArray, ModuleDescriptor$Builder, MethodHandleAccessorFactory)
- Compiled frame recovery (requires -Xint)
- asType secondary unboxing BMH node (exact=false — C++ accessor not available)
- tryFinally cleanup slots (exact=false)
- Virtual/interface dispatch target deferred
- CompletableFuture stage chaining (separate async subsystem)

See [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md) for complete detail.
