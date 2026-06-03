# Gap Fixes: Spring Boot Attribution Gaps (docs/54 → READY)

**Date:** 2026-06-02  
**Predecessor doc:** `docs/54-real-program-spring-validation.md`  
**Status:** RESOLVED — docs/54 moves from PARTIAL to READY

---

## 1. Gaps Identified in docs/54

`docs/54` validated Runtime Truth attribution on a real Spring Boot program (`demo-runtime-truth`) and found two BLOCKER attribution gaps:

> **Gap 1 — Reflection `Method.invoke` missing source BCI**  
> `DecryptPipeline.execute()` calls `executeMethod.invoke(executor, ctx)`. No `callsite_target(reflection_method_invoke)` was emitted despite this being the primary dispatch point of the pipeline. Expected: `source_class=com/example/truth/DecryptPipeline, source_method=execute, source_bci=22, category=reflection_method_invoke, target_class=com/example/truth/proxy/PipelineExecutor, target_method=execute`

> **Gap 2 — Plain `invokeinterface TransformHandler.applyTransform` missing callsite record**  
> `PipelineProxy.dispatch()` calls `compiled.applyTransform(...)` as an `invokeinterface` bytecode. The call executes but no `callsite_target` record was emitted, leaving the dispatch edge invisible to the staticizer.

---

## 2. Root Causes

### Gap 1 Root Cause: DMHA INDY #61 Pre-emption

JDK 21's default reflection accessor for single-argument methods is `DirectMethodHandleAccessor` (DMHA). Its `invoke` method ultimately calls `this.target.invokeExact(obj, args[0])` where `this.target` is a `MethodHandle` with a DIRECT shape. This `invokeExact` is backed by an `invokedynamic` callsite (INDY #61 in DMHA's bytecode): `invokeExact(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;`.

The existing attribution path (`resolve_handle_call` in `linkResolver.cpp`) fires on the **first CP-cache resolution** of each callsite. `docs/53` (the MH adapter-chain fix) already ensured that `resolve_handle_call` fires only once per BCI.

The problem: `Launcher.launch` calls `Method.invoke` at JVM startup (before any HTTP request) to bootstrap the Spring Boot application. This causes DMHA's INDY #61 to be resolved and cached during startup. When `DecryptPipeline.execute` later calls `executeMethod.invoke(executor, ctx)` on the first HTTP request, the `invokevirtual Method.invoke` instruction reaches DMHA, but INDY #61 in DMHA is already cached — `resolve_handle_call` does NOT fire a second time.

**Confirmed from `stderr.txt` (docs/54 run):**
```
[JVM INDY #61] linkMethod caller=jdk/internal/reflect/DirectMethodHandleAccessor
               methodhandle=invokeExact(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;
[JVM AG CALLSITE] cat=reflection_method_invoke ... src=org/springframework/boot/loader/launch/Launcher.launch bci=62
```
INDY #61 was consumed by `Launcher.launch:62`, not by `DecryptPipeline.execute`.

### Gap 2 Root Cause: `soroush_trace_runtime_dispatch` Was Stderr-Only

`soroush_trace_runtime_dispatch` is called from `runtime_resolve_virtual_method` (line 1806) and `runtime_resolve_interface_method` (line 1905) on the **first CP-cache resolution** of each virtual/interface callsite. Before this fix it only printed diagnostic output to stderr (gated by `SOROUSH_TRACE_REFLECTION`). It never emitted any JSONL record. Every `invokeinterface` in user-prefix classes was therefore invisible to the provenance graph.

---

## 3. Fix

Both fixes are implemented in `src/hotspot/share/classfile/linkResolver.cpp`. The `interpreter/linkResolver.cpp` received identical changes for consistency (not compiled by the fastdebug build).

### New Infrastructure: `SOROUSH_USER_PREFIXES` Filter (lines 281–310)

```cpp
static const char* sg_rtd_pfx[16];
static int         sg_rtd_n_pfx    = 0;
static bool        sg_rtd_pfx_init = false;

static void sg_rtd_init_prefixes() { /* parse SOROUSH_USER_PREFIXES env, store up to 16 prefixes */ }
static bool sg_rtd_matches(const char* class_name) { /* prefix-match against sg_rtd_pfx[] */ }
```

Without this filter, every `invokevirtual` and `invokeinterface` in the JVM would emit a JSONL record — millions of records per second from JDK-internal code. Setting `SOROUSH_USER_PREFIXES=com/example/truth` limits emission to only the application package. The filter is lazy-initialized on the first call.

### Gap 1 Fix: Warm-Path `Method.invoke` Hook (lines 403–437)

In `soroush_trace_runtime_dispatch`, after extracting the source frame, check whether `selected_method` is `java/lang/reflect/Method.invoke`. If so, and if `recv_oop` is a `java.lang.reflect.Method` object:

1. Read the declared target class from `java_lang_reflect_Method::clazz(recv_oop)`
2. Read the method slot from `java_lang_reflect_Method::slot(recv_oop)`
3. Recover the `Method*` via `InstanceKlass::cast(tgt_k)->method_with_idnum(slot)`
4. Emit `callsite_target(reflection_method_invoke)` with the declared target

This fires at **CP-cache resolution of `invokevirtual Method.invoke` in the source class** (i.e., `DecryptPipeline.execute`), not at DMHA's internal INDY. The CP-cache for `DecryptPipeline.execute`'s `invokevirtual Method.invoke` instruction is resolved on the first HTTP request, which is the correct moment.

**Key design point:** The `Method` object receiver (`recv_oop`) is the specific `Method` instance that was stored in `DecryptPipeline.executeMethod`. Its `clazz` field is `PipelineExecutor.class` and its `slot` index identifies `PipelineExecutor.execute`. This is the programmer's declared intent — the correct target for staticization purposes.

### Gap 2 Fix: Plain Virtual/Interface JSONL Emission (lines 439–449)

After the Gap 1 check, emit a `callsite_target(kind)` record for all other dispatches where source class matches `SOROUSH_USER_PREFIXES`:

```cpp
soroush_graph_generic_callsite(kind,
    src_class, src_loader, src_method, src_desc,
    src_bci, src_opcode, src_cp,
    tgt_class, tgt_loader, tgt_method, tgt_desc,
    true, true, nullptr);
```

`kind` is `"invokevirtual"` or `"invokeinterface"` passed from the call site. The `soroush_graph_generic_callsite` dedup key `(src_class, src_method, src_desc, src_bci)` prevents duplicate records if the CP-cache resolution path is entered more than once (e.g., warm-path re-resolution edge cases).

### Updated Call Sites

```cpp
// runtime_resolve_virtual_method (line 1806):
soroush_trace_runtime_dispatch("invokevirtual",
    recv_klass, resolved_method, selected_method,
    recv.not_null() ? recv() : (oop)nullptr, THREAD);

// runtime_resolve_interface_method (line 1905):
soroush_trace_runtime_dispatch("invokeinterface",
    recv_klass, resolved_method, selected_method,
    recv.not_null() ? recv() : (oop)nullptr, THREAD);
```

---

## 4. Before vs. After

### Gap 1

**Before (docs/54 run):** No `callsite_target` record with `source_class=com/example/truth/DecryptPipeline`.

**After (docs/55 run, R1):**
```json
{
  "record": "callsite_target",
  "category": "reflection_method_invoke",
  "evidence": "OBSERVED_ONLY",
  "source_class": "com/example/truth/DecryptPipeline",
  "source_loader_id": "0x000000011de06060",
  "source_capture": "exact",
  "source_method": "execute",
  "source_descriptor": "(Lcom/example/truth/model/DecryptContext;)[B",
  "source_bci": 22,
  "source_opcode": "invokevirtual",
  "source_cp_index": 35,
  "target_class": "com/example/truth/proxy/PipelineExecutor",
  "target_loader_id": "0x000000011de06060",
  "target_method": "execute",
  "target_descriptor": "(Lcom/example/truth/model/DecryptContext;)[B"
}
```

### Gap 2

**Before (docs/54 run):** No `callsite_target` record for `invokeinterface TransformHandler.applyTransform`.

**After (docs/55 run, R1):**
```json
{
  "record": "callsite_target",
  "category": "invokeinterface",
  "evidence": "OBSERVED_ONLY",
  "source_class": "com/example/truth/proxy/PipelineProxy",
  "source_loader_id": "0x000000011de06060",
  "source_capture": "exact",
  "source_method": "dispatch",
  "source_descriptor": "(Lcom/example/truth/model/DecryptContext;)[B",
  "source_bci": 25,
  "source_opcode": "invokeinterface",
  "source_cp_index": 47,
  "target_class": "com/example/truth/handler/TransformBase",
  "target_loader_id": "0x000000011de06060",
  "target_method": "applyTransform",
  "target_descriptor": "([B)[B"
}
```

---

## 5. Validation

### Spring Boot Run (docs/55, `/tmp/rt55/`, 3 requests, `SOROUSH_USER_PREFIXES=com/example/truth`)

3 requests fired:
- R1: `sessionKey=550e8400-…` → `DECRYPTION_FAILED` (known: instance token=4 selects transform.037; this key doesn't match the INSTANCE_TOKEN barrier)
- R2: `sessionKey=aaaabbbb-…` → `RUNTIME SHOWS THE TRUTH` ✓
- R3: `sessionKey=12345678-…` → `RUNTIME SHOWS THE TRUTH` ✓

**JSONL record breakdown (17,693 total lines):**

| Record type | Count |
|---|---|
| bytecode_artifact | 8,851 |
| runtime_target | 3,416 |
| generated_class | 1,674 |
| hidden_class_identity | 1,620 |
| callsite_target | 1,342 |
| callsite_adapter_graph | 691 |
| diagnostic | 55 |
| method_identity | 43 |
| export_summary | 1 |

**`callsite_target` by category:**

| Category | Count |
|---|---|
| invokedynamic | 1,242 |
| invokevirtual | 42 |
| methodhandle_invoke | 31 |
| reflection_method_invoke | 12 |
| invokeinterface | 7 |
| methodhandle_invokeExact | 5 |
| reflection_constructor_newInstance | 3 |

**Gap verification:**

| Gap | Expected Record | Found | Status |
|---|---|---|---|
| Gap 1 | `callsite_target(reflection_method_invoke)` `source=DecryptPipeline.execute BCI=22` `target=PipelineExecutor.execute` | ✓ Exact match | **RESOLVED** |
| Gap 2 | `callsite_target(invokeinterface)` `source=PipelineProxy.dispatch BCI=25` `target=TransformBase.applyTransform` | ✓ Exact match | **RESOLVED** |

**Full `com/example/truth` dispatch map (all callsite_target records with source in truth package):**

| Category | Source class | BCI | Target |
|---|---|---|---|
| invokevirtual | DecryptController | 6 | ClassLoader::loadClass |
| invokevirtual | DecryptController | 10 | DecryptRequest::ciphertext |
| invokevirtual | DecryptController | 13 | HexFormat::parseHex |
| invokevirtual | DecryptController | 37 | PrintStream::printf |
| invokevirtual | DecryptController | 58 | DecryptPipeline::execute |
| invokevirtual | DecryptController | 68 | PrintStream::println |
| invokevirtual | DecryptController | 155 | HexFormat::formatHex |
| invokevirtual | DecryptPipeline | 12 | PipelineProxy::createExecutor |
| invokevirtual | DecryptPipeline | 32 | Class::getDeclaredMethod |
| **reflection_method_invoke** | **DecryptPipeline** | **22** | **PipelineExecutor::execute** ← Gap 1 |
| invokevirtual | PipelineProxy | 8 | Class::getClassLoader |
| invokevirtual | PipelineProxy | 11 | DecryptContext::sessionKey |
| invokevirtual | PipelineProxy | 13 | PipelineProxy::dispatch |
| invokevirtual | PipelineProxy | 15 | DecryptContext::ciphertext |
| invokevirtual | PipelineProxy | 18 | TransformCompiler::compile |
| **invokeinterface** | **PipelineProxy** | **25** | **TransformBase::applyTransform** ← Gap 2 |
| invokeinterface | PipelineProxy | 39 | PipelineProxy$$Lambda::apply |
| invokedynamic | PipelineProxy | 21 | PipelineProxy::lambda$createExecutor$0 |
| invokedynamic | PipelineProxy | 31 | (bootstrap) |

### Runtime Truth 15-Case Run (`/tmp/mc55/`, `SOROUSH_USER_PREFIXES=testcases`)

```
PASS Case01 — Lambda / invokedynamic
PASS Case02 — String concat indy
PASS Case03 — Direct MethodHandle
PASS Case04 — MH receiver origins
PASS Case05 — asType adapters
PASS Case06 — Argument adapters
PASS Case07 — Filter / fold
PASS Case08 — Spread / collector
PASS Case09 — Guard / catch / finally
PASS Case10 — Reflection
PASS Case11 — Dynamic proxy
PASS Case12 — Hidden class
PASS Case13 — invokevirtual mono
PASS Case14 — invokevirtual poly
PASS Case15 — invokeinterface poly

test cases demo complete — 15/15 passed
```

**JSONL record breakdown (mc55):**

| Record type | Count |
|---|---|
| bytecode_artifact | 2,012 |
| runtime_target | 921 |
| generated_class | 536 |
| hidden_class_identity | 532 |
| callsite_target | 258 |
| callsite_adapter_graph | 99 |
| diagnostic | 4 |
| callsite_target_set | 3 |
| export_summary | 1 |

**`callsite_target` by category (mc55):**

| Category | Count |
|---|---|
| invokevirtual | 104 |
| invokedynamic | 87 |
| methodhandle_invoke | 52 |
| invokeinterface | 11 |
| reflection_method_invoke | 3 |
| methodhandle_invokeExact | 1 |

`invokevirtual` + `invokeinterface` = **115** (same as previous validated run — no regression).

Gap 1 warm-path validation (Case10_Reflection):
```
Case10_Reflection::run BCI=157 → Case10_Reflection::secretAdd    (reflection_method_invoke)
Case10_Reflection::run BCI=183 → String::length                   (reflection_method_invoke)
Case10_Reflection::run BCI=75  → Case10_Reflection::instanceDouble (reflection_method_invoke)
```

---

## 6. Impact on docs/54

`docs/54` verdict: **PARTIAL** — attribution correct for all previously covered cases but two BLOCKERs:
- Gap 1 (reflection Method.invoke from application code): BLOCKER
- Gap 2 (invokeinterface not captured): BLOCKER

**Both blockers are now RESOLVED.** `docs/54` overall verdict changes from **PARTIAL** to **READY**.

The full dispatch graph for the `demo-runtime-truth` Spring Boot application is now complete: every executed `invokevirtual`, `invokeinterface`, `invokedynamic`, `methodhandle_invoke`, and `reflection_method_invoke` in the `com/example/truth` package has a direct `callsite_target` record. No adapter-graph traversal or manual inspection is required to answer "what does this BCI dispatch to?"

---

## 7. Files Changed

| File | Change |
|---|---|
| `src/hotspot/share/classfile/linkResolver.cpp` | Added `sg_rtd_pfx[]` / `sg_rtd_init_prefixes()` / `sg_rtd_matches()` prefix filter (lines 281–310); rewrote `soroush_trace_runtime_dispatch` to emit JSONL (lines 344–450): Gap 1 warm-path `reflection_method_invoke` hook + Gap 2 plain virtual/interface emission; updated two call sites at lines 1806 and 1905 to pass `recv_oop` |
| `src/hotspot/share/interpreter/linkResolver.cpp` | Identical parallel changes (not compiled by fastdebug build; mirrored per project convention from docs/53) |
