# Runtime Target Export — High-Level Capture Overview

## Branch goal

```
Run Java slices under a modified JVM (interpreter-only mode)
  → intercept every dynamic dispatch at its first resolution
  → capture the exact runtime target, adapter structure, and source callsite
  → export executed class bytecode artifacts
  → emit a semantic JSONL file for downstream staticization / recompilation
```

This JVM fork adds read-only, fail-safe interception hooks at the points where
HotSpot resolves MethodHandle, `invokedynamic`, and reflection dispatch. It
recovers information that static bytecode analysis cannot see — the actual runtime
target behind an adapter chain, the lambda body method behind an `invokedynamic`,
the reflected method behind `Method.invoke`. Everything captured is either
**exact** (class/method/descriptor/loader verified from live JVM state) or
**explicitly unresolved** (a `diagnostic` record explaining why). There is no
fake precision and no silent omission.

---

## Architecture summary

```
Java program running under -Xint
         │
         ├─ invokedynamic bootstrap       → systemDictionary.cpp
         ├─ MethodHandle.invoke*          → linkResolver.cpp (resolve_handle_call)
         ├─ Method.invoke / ctor.newInstance → methodHandles.cpp + linkResolver.cpp
         └─ class load (any class)        → klassFactory.cpp
                  │
                  ▼
         in-memory record tables
         (soroushProvenanceGraph.cpp)
                  │
                  ▼ VM shutdown (java.cpp before_exit)
         JSONL export file
         (soroush_graph_export_runtime_targets)
```

The capture system adds no overhead when `SOROUSH_PROVENANCE_GRAPH` is unset.
All hooks are observational: they never alter program behavior, never load
additional classes during capture, and never hold VM locks during graph writes.

---

## 1. Executed Dynamic Callsites

### What we capture

For every `MethodHandle.invoke*` callsite in the program:

- **Source callsite**: class, method, descriptor, BCI, opcode, CP cache index,
  defining loader
- **Evidence**: `"OBSERVED_ONLY"` — captured at first execution, not linkage time
- **Record type**: `callsite_target`, `callsite_target_set`, or
  `callsite_adapter_graph` depending on the adapter shape

One record per unique BCI per method, regardless of how many times that callsite
is executed. Records are deduped by `(source_class, source_method, source_descriptor,
source_bci)`.

### Why it matters

A staticizer needs to know *exactly* which bytecode callsite invoked which target.
The BCI+method tuple allows direct bytecode patching or devirtualization — the
callsite can be replaced with a direct `invokestatic` / `invokevirtual` with the
known target. Without the exact BCI and source class, there is no way to map a
runtime observation back to a patchable bytecode location.

### High-level approach

HotSpot rewrites `invokevirtual MethodHandle.invoke*` to an internal `invokehandle`
bytecode. The first time any `invokehandle` fires it calls `resolve_handle_call`
(CP-cache resolution). This is the capture hook. At that moment the interpreter
frame chain is intact, the MH receiver oop is on the expression stack, and the
source method and BCI are readable from the frame.

Three cases are distinguished based on which frame is on top:
- **Case A** — top frame is the user method (MH invoked directly)
- **Case A2** — top frame is a `java/lang/invoke/` LambdaForm dispatch frame (MH
  invoked through an adapter); source frame recovered by walking `vframeStream`
  past LF internals
- **Case B** — top frame is a `jdk/internal/reflect/` accessor (reflection path);
  target recovered from the accessor's `target` field

### Main implementation locations

```
src/hotspot/share/interpreter/linkResolver.cpp  (export copy)
src/hotspot/share/classfile/linkResolver.cpp    (build copy — keep in sync)

Key functions:
  resolve_handle_call          main hook, three-case dispatch
  sg_analyze_mh_receiver       symbolic backward receiver analysis (lines 2228–2613)
  sg_recover_mh_recv_from_java_sp  live stack fallback, Case A (lines 3255–3275)
  is_lf_dispatch_case flag     Case A vs A2 distinction (declared ~line 3506)
  Case A2 vframeStream walk    lines 3560–3645
  Step 2a local[0] fallback    lines 3747–3760
  soroush_graph_generic_callsite   called at line 3932
  soroush_graph_target_set_callsite   called at line 3785
  soroush_graph_adapter_graph_callsite  called at line 3820
```

### Known limitations

- Requires `-Xint` (interpreter-only). JIT-compiled frames do not fire
  `resolve_handle_call` after the method is compiled; those callsites are not
  captured.
- A callsite fires `resolve_handle_call` only once per CP-cache entry. The first
  firing must have the live MH receiver accessible; if not (e.g., JVM-internal
  warmup call before user code runs the site), a diagnostic is emitted and
  upgraded when user code fires the site later.

---

## 2. Resolved Runtime Targets

### What we capture

The concrete Java method (class, method name, descriptor, defining loader) that a
dynamic callsite resolves to at runtime:

- For a `DirectMethodHandle`: the method wrapped by the DMH (from its
  `MemberName.vmtarget`)
- For a GWT/GWC BMH: all branch targets (test, true/false, try/handler)
- For a generic BMH: all DMH-bearing `argL` slots in the adapter species
- For reflection: the actual target method extracted via `sg_walk_mh` traversal

Loader identity is always `ClassLoaderData*` cast to `uint64_t` — the stable
defining loader pointer, not a name guess.

### Why it matters

This is the core deliverable. The staticizer needs to know `Foo.add(II)I` (with
loader) to generate a static dispatch. The loader matters for multi-classloader
environments where `com/example/Foo` under loader A and `com/example/Foo` under
loader B are different classes with potentially different implementations.

### High-level approach

Once the MH receiver oop is recovered (see §1), `sg_walk_mh` classifies it:

```
DMH  → sg_extract_dmh_target → reads Method* from MemberName.vmtarget
BMH (GWT) → extracts argL0/1/2 as test/true/false target DMHs
BMH (GWC) → extracts argL0/1 as try/handler DMHs + exception class
BMH (generic) → sg_walk_generic_bmh → iterates argL slots
```

All extraction is read-only (no allocation, no class loading) using JVM internal
accessors (`java_lang_invoke_DirectMethodHandle::member`,
`java_lang_invoke_MemberName::vmtarget`).

### Main implementation locations

```
src/hotspot/share/interpreter/linkResolver.cpp

Key functions:
  sg_extract_dmh_target    DMH → SgMhTarget  (lines 2877–2890)
  sg_unwrap_delegating     peel DelegatingMethodHandle wrapper (lines 2895–2901)
  sg_walk_mh               top-level classifier (lines 3279–3376)
  sg_walk_generic_bmh      BMH species field walk (lines 3085–3215)

Structs:
  SgMhTarget               (lines 2740–2746)
  SgAdapterNode            (lines 2749–2760)
  SgMhWalkResult           (lines 2769–2799)
```

### Known limitations

- Targets are captured at first CP-cache resolution, not per-invocation. If a
  `BoundMethodHandle` is mutated between first and second invocation (extremely
  unusual; BMHs are effectively immutable), only the first-seen target is recorded.
- `all_exact=false` is emitted when a BMH slot holds another BMH (adapter wrapping
  adapter). The inner layer is not extractable without unsafe generated-class
  field accessors. See §3.

---

## 3. MethodHandle Adapter Structures

### What we capture

When the MH at a callsite is an adapter (not a raw DMH), the structural topology
of the adapter chain is captured:

- Species class name (e.g. `BoundMethodHandle$Species_LL`)
- `adapter_kind`: `dual_target`, `multi_target`, `try_finally`, `type_conversion`
- `lf_kind`: the LambdaForm kind string from the BMH
- Per-node: role, `exact` flag, target class/method/descriptor/loader if exact,
  BMH species class if not exact
- `all_exact`: whether every node in the graph was fully resolved

### Why it matters

Adapter structures reveal the full invocation topology. `filterArguments(add, 0,
negate)` — knowing that the callsite is `dual_target` with `add` as primary and
`negate` as secondary tells the staticizer that both methods are dispatched from
this site and both may need to be inlined or devirtualized.

### High-level approach

`sg_walk_generic_bmh` iterates the BMH's `argL` species fields (object-typed bound
slots). For each slot it recursively calls `sg_walk_mh` with a depth limit. If
the slot holds a DMH, the node gets `exact=true` and the full target identity. If
the slot holds another BMH, the node gets `exact=false` with the inner species
class name — the inner target is not further unwrapped in the current
implementation.

### Main implementation locations

```
src/hotspot/share/interpreter/linkResolver.cpp

Key functions:
  sg_walk_generic_bmh       species field walk and node array construction (lines 3085–3215)
  sg_walk_mh                routes to sg_walk_generic_bmh for ADAPTER_GRAPH shape (lines 3279–3376)

soroushProvenanceGraph.cpp / .hpp
  soroush_graph_adapter_graph_callsite   stores record (lines 606–708)
  SgAdapterNodeEntry struct              (hpp lines 318–333)
  callsite_adapter_graph export phase    (cpp lines 1957–2081)
```

### Known limitations

**`exact=false` nodes are a known structural limitation, not a bug.**

| Adapter form | Which node is exact=false | Why |
|---|---|---|
| `asType(mh, type)` | secondary unboxing component | The boxing/unboxing helper is itself a BMH; its inner DMH is not accessible via C++ read-only walk |
| `tryFinally(target, cleanup)` | all nodes | Each slot is a BMH wrapping another adapter |
| `asCollector(mh, t, n)` | collector secondary | Internal array-collector trampoline has no Java-method slot |

No user or business-logic methods are hidden behind these `exact=false` nodes.
The unresolved slots are JDK-internal helper machinery.

---

## 4. invokedynamic / LambdaMetafactory Linkage Facts

### What we capture

For every `invokedynamic` instruction that executes its bootstrap:

- Source callsite (class, method, BCI, CP index)
- Bootstrap method reference
- Interface method name and descriptor (`indy_name`, `indy_descriptor`)
- For `LambdaMetafactory`: the lambda body class and method (`lmf_impl_class`,
  `lmf_impl_method`, `lmf_impl_descriptor`)
- `indy_trace_id` — join key linking the callsite record to the generated lambda class

### Why it matters

`invokedynamic` is the JVM's mechanism for lambda expressions and string
concatenation. Without capturing the LMF linkage, the staticizer sees only an
`invokedynamic` instruction with no information about what method it will actually
dispatch to. With `lmf_impl_method`, it can resolve the dispatch directly to the
lambda body method.

### High-level approach

The hook fires inside the `invokedynamic` bootstrap resolution path in
`systemDictionary.cpp`. At bootstrap time the JVM has the full `InvokeDynamic`
CP entry, the bootstrap method reference, and the linked `CallSite`. The hook reads
these and calls into `soroushProvenanceGraph.cpp` to store the record.

The `indy_trace_id` is assigned at bootstrap time and is also stored on the
generated hidden class (the `$$Lambda/...` class). This creates the join key
between the callsite record and the generated-class record.

### Main implementation locations

```
src/hotspot/share/classfile/systemDictionary.cpp
  soroush_trace_indy_enabled gate   (lines 134–141)
  trace_id assignment               (~line 2556)
  graph calls                       (~lines 2626, 2695)

soroushProvenanceGraph.cpp
  callsite_target (invokedynamic) export phase   (lines 1610–1682)
```

### Known limitations

- `invokedynamic` bootstrap fires once per callsite. `MutableCallSite` target
  mutations after bootstrap are not tracked.
- Non-LMF bootstrap methods get the structural record (BSM reference, indy name)
  but no `lmf_impl_*` enrichment — the implementation does not attempt to reverse-
  engineer arbitrary BSM semantics.

---

## 5. Reflection Target Facts

### What we capture

For `java.lang.reflect.Method.invoke` and `Constructor.newInstance`:

- The exact target method (class, method name, descriptor, loader)
- Source callsite (who called `method.invoke(...)`, with BCI)
- Evidence level: `LINKAGE_GUARANTEED` (from `MemberName.resolve` hook) or
  `OBSERVED_ONLY` (from `resolve_handle_call` Case B)

### Why it matters

Reflection is a common dynamic dispatch mechanism in frameworks. The staticizer
needs to know that `method.invoke(obj, args)` at BCI 47 in class `Foo` actually
dispatches to `Bar.process(Object)V`. Without this, the reflection call is
opaque.

### High-level approach

Two complementary mechanisms:

**Mechanism 1 — `MemberName.resolve` hook** (linkage time): fires when the JDK
reflection infrastructure resolves the target `MemberName`. Captures the
`Method*` from `MemberName.vmtarget`. Provides `LINKAGE_GUARANTEED` evidence.

**Mechanism 2 — Case B in `resolve_handle_call`** (invocation time): fires when
the internal `DirectMethodHandleAccessor` executes its MH invoke. The accessor's
`target` field holds the internal MH. This MH is passed to `sg_walk_mh` to
traverse whatever BMH adapter chain the JDK has built around the core DMH.

**JDK21 complication:** `MethodHandleAccessorFactory.makeSpecializedTarget`
always wraps the core DMH in `dropArguments` + `asType` BMH adapters. A raw
`DirectMethodHandle::is_instance(target)` check fails for any method with
parameters. The fix: `sg_walk_mh(target, 6)` traverses the full adapter chain
and locates the first `has_target=true` node pointing to the underlying DMH.
This eliminated all `reflection_target_adapter_mh_deferred` diagnostics.

### Main implementation locations

```
src/hotspot/share/prims/methodHandles.cpp
  soroush_trace_membername_resolution   (lines 95–121)
  call sites: lines 260, 271, 849, 873

src/hotspot/share/interpreter/linkResolver.cpp
  Case B block                          (lines 3520–3558)
  sg_walk_mh traversal for accessor.target
```

### Known limitations

- `MethodHandleAccessorFactory.makeSpecializedTarget` itself involves internal MH
  invocations that produce 4 JDK-internal `recv_from_method_result_or_field`
  diagnostics. These are expected and have zero impact on user-visible reflection
  capture (see §10).
- The accessor's `target` field is lazily initialized on the first call. A
  diagnostic may fire on the very first invocation if initialization hasn't
  completed; the prefer-exact upgrade mechanism handles this on the second call.

---

## 6. Generated / Runtime Classes

### What we capture

Every class generated at runtime (not loaded from a JAR or classpath):

- Class name, loader, CRC32 of bytes
- `generated_by`: `LambdaMetafactory`, `ProxyGenerator`, `ByteBuddy`, `CGLIB`,
  `Unsafe`, `LookupDefineHidden`
- `source_trigger`: the class that caused generation
- `indy_trace_id`: for lambda classes, the join key to the invokedynamic callsite
- Raw bytes dumped to `/tmp/soroush_jvm_dump/`

**Requires** `SOROUSH_RUNTIME_RECOVERY=1`.

### Why it matters

Staticization of a program that uses dynamic proxies or ByteBuddy interceptors
requires the bytecode of those generated classes. Without the generated-class
export, the staticizer cannot analyze or recompile the dynamically-created code.

### High-level approach

The hook fires at `KlassFactory::create` time. Detection is based on class name
patterns (hidden class flag, `$Lambda/`, `$Proxy`, ByteBuddy/CGLIB naming
conventions) and whether the class is loaded from a synthetic classloader rather
than a URL/file loader.

### Main implementation locations

```
src/hotspot/share/classfile/klassFactory.cpp
  recover_runtime_generated_class    (lines 509–605)
  soroush_graph_generated_class call (~line 600)
  disk dump calls                    (~lines 246, 282, 315, 524)

soroushProvenanceGraph.cpp
  soroush_graph_generated_class implementation   (lines 1017–1053)
  generated_class export phase                   (~lines 2306–2324)
```

### Known limitations

- `generated_by` classification for ByteBuddy and CGLIB is heuristic (class name
  pattern matching). Non-standard naming conventions fall back to `"unknown"`;
  the bytes are still captured.
- Hidden classes loaded from JAR (pre-generated lambda classes) appear only as
  `bytecode_artifact` records, not `generated_class` records.
- Disk dump uses `/tmp/soroush_jvm_dump/` (hardcoded). Name collisions across
  classloaders overwrite; the JSONL record is still unique by `(class, loader_id)`.

---

## 7. Final Executable Bytecode Artifacts

### What we capture

For every class loaded during the run, two bytecode snapshots:

- `kind="original"` — bytes as received before any bytecode rewriting
- `kind="final"` — bytes after Phase 5 ENTER/EXIT instrumentation (if the class
  was rewritten)

Both carry class name, loader, CRC32, size, and `hidden` flag. The `kind="final"`
artifact additionally carries `rewritten_from_crc` pointing back to the original.

**Requires** `SOROUSH_CAPTURE_FINAL_BYTECODE=1`.

### Why it matters

The staticizer needs the actual bytecode that ran, not the on-disk classfile
(which may differ after runtime transformations by JVMTI agents or the rewriter
itself). The pre/post pair allows analysis of both the original semantics and the
instrumented form.

### High-level approach

The capture hook fires inside `KlassFactory::create` after class parsing. The
`ClassFileStream` provides the original bytes; if Phase 5 ran, the rewriter's
output buffer provides the final bytes. Both are stored in-memory in the
provenance graph and emitted as `bytecode_artifact` records at shutdown.

### Main implementation locations

```
src/hotspot/share/classfile/klassFactory.cpp
  bytecode artifact capture block   (~line 935)

src/hotspot/share/classfile/soroushClassfileRewriter.cpp
  insert_entry_exit_trace            Phase 5 main entry (lines 4491–4493)
  soroush_transform_code_attribute_entry_code  per-method transform (lines 3102+)
  soroush_method_token_register call  token registration at rewrite time (~line 4314)

soroushProvenanceGraph.cpp
  bytecode_artifact export phase    (~lines 2353–2375)
```

### Known limitations

- Memory overhead is proportional to the number of loaded classes and their
  bytecode sizes. A Spring Boot run with `SOROUSH_CAPTURE_FINAL_BYTECODE=1` can
  accumulate several hundred MB.
- "Original" bytes are post-JVMTI if a JVMTI agent transforms classes before
  `KlassFactory` runs. This is the earliest capture point within HotSpot's
  classloading pipeline.
- Class redefinition (JVMTI `redefineClasses`) is not tracked after first load.

---

## 8. Loader-Aware Identities

### What we capture

Every record in the JSONL output carries a `loader_id` field that is the
`ClassLoaderData*` pointer (as `"0x..."` hex) of the defining classloader. This
applies to:

- Source callsite classes
- Target classes
- Generated class records
- Bytecode artifact records
- Method identity records

### Why it matters

In multi-classloader environments (OSGi, application servers, hot-reload
frameworks), two classes can share the same internal name (`com/example/Foo`) but
be entirely different types loaded by different loaders. Using a name-only key
would merge them incorrectly. The loader pointer distinguishes them.

For staticization: the staticizer must know *which loader's* `Foo.bar()` was
called, because devirtualizing to the wrong loader's copy would produce
incorrect code.

### High-level approach

`ClassLoaderData*` is available at every capture point — it's stored on every
`InstanceKlass`. It is passed through all record functions. The export emits it
as a hex string. The bootstrap classloader has its own non-null `ClassLoaderData*`
(it is not zero).

No name-based loader inference, no heuristic. The pointer is read directly from
the defining `InstanceKlass`.

### Known limitations

- `ClassLoaderData*` is a process-lifetime stable pointer for non-unloaded
  loaders. If a loader is collected (class unloading), the pointer value may be
  reused by a future loader. For the single-run snapshot model of this branch,
  loader unloading during the run is an edge case; records emitted before
  unloading retain the original pointer value.

---

## 9. Runtime Execution Observations

### What we capture

When the Phase 5 bytecode rewriter is enabled (`SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1`
+ `SOROUSH_REWRITER_PHASE5_PREFIX=<prefix>`), every instrumented method generates
a `method_identity` record:

- 32-bit stable token assigned at rewrite time
- Exact class, method, descriptor, loader, hidden flag
- CRC32 of the original (pre-rewrite) classfile

At runtime, ENTER/EXIT events are emitted to the provenance graph via the token.
These produce `runtime_target` records with `evidence="OBSERVED_ONLY"`.

### Why it matters

`method_identity` records tell the staticizer exactly which methods were
instrumented and provide the stable method token → exact identity mapping. An
`OBSERVED_ONLY` `runtime_target` confirms that a given method was actually
executed during the slice run.

### High-level approach

The rewriter assigns each instrumented method a 32-bit token and injects
`ldc_w <token>; invokestatic System.soroushTraceEnter(int)` at method entry
and `ldc_w <token>; invokestatic System.soroushTraceExit(int)` at each return.
At runtime, `JVM_SoroushTraceEnter` resolves the token via the method-token
registry to the exact (class, method, descriptor, loader) tuple.

The registry is append-only and immortal for the process lifetime.

### Main implementation locations

```
src/hotspot/share/classfile/soroushClassfileRewriter.cpp
  soroush_method_token_register call   (~line 4314)

soroushProvenanceGraph.cpp / .hpp
  soroush_method_token_register    (cpp lines 251–300)
  soroush_method_token_lookup      (cpp lines 302–321)
  method_identity export phase     (cpp lines 1570–1608)

src/java.base/share/classes/java/lang/System.java
  soroushTraceEnter(int) / soroushTraceExit(int)  native declarations

src/hotspot/share/prims/jvm.cpp
  JVM_SoroushTraceEnter / JVM_SoroushTraceExit
```

### Known limitations

- Phase 5 instrumentation only applies to classes whose names match
  `SOROUSH_REWRITER_PHASE5_PREFIX` (slash form). Classes outside the prefix
  produce no `method_identity` records.
- Token integers cap at 8M per run (fail-safe; beyond that, tokens are not
  registered and the event is logged as `identity-unresolved`).

---

## 10. Diagnostics — Explicitly Unresolved Runtime Facts

### What we capture

When a dynamic callsite cannot be resolved exactly, a `diagnostic` record is
emitted instead of a callsite record. The diagnostic includes:

- Source class, method, descriptor, BCI
- A machine-readable `reason` code
- A human-readable `message`

This is not an error state — it is the contract. The exact-or-diagnostic
invariant guarantees that **every executed MH callsite** in the program produces
exactly one record: either an exact callsite record or an explicit diagnostic.

### Why it matters

Silent omissions would leave the staticizer with an incomplete picture. An
explicit diagnostic tells downstream tools: "this callsite exists and was
executed, but could not be resolved — here is why." The staticizer can then
decide whether to leave the site dynamic or flag it for manual review.

### High-level approach

Diagnostic reasons arise at four layers:
1. **Symbolic analysis failure** (`sg_analyze_mh_receiver`): MH receiver comes
   from a method-return value or unanalyzable expression
2. **Runtime stack recovery failure**: Step 2/2a could not find a valid MH oop
3. **BMH walk failure**: unrecognized `lf_kind`, depth limit exceeded
4. **Sibling BCI scan** (`sg_emit_sibling_bcis`): BCIs that share a CP-cache entry
   with a primary site and can never fire `resolve_handle_call` independently

The sibling scan fires after every primary site emission to ensure zero silent
omissions for shared CP-cache entries.

### Main implementation locations

```
src/hotspot/share/interpreter/linkResolver.cpp
  sg_emit_sibling_bcis    sibling scan (lines 2634–2710)
  diagnostic emission paths throughout resolve_handle_call

soroushProvenanceGraph.cpp
  soroush_graph_generic_callsite   stores diagnostics when tgt_ok=false (lines 419–525)
  diagnostic export in Phases 3, 3.5, 3.6
```

### Remaining acceptable diagnostics (as of 2026-05-25)

| Count | Source | Reason | Status |
|-------|--------|--------|--------|
| 4 | JDK internal (`ByteArray`, `ModuleDescriptor$Builder`, `MethodHandleAccessorFactory`) | `recv_from_method_result_or_field` | Acceptable — JDK-internal only, zero user/business impact |
| 0 | User / demo code | any | Clean |

No demo-related or user-code diagnostics remain.

---

## Current Branch Status

### Complete

| Feature | Evidence |
|---------|----------|
| Direct MH invoke capture (`callsite_target`) | 11/11 showcase PASS, 4 adapter demos |
| GWT / GWC target-set capture | Showcase §6, §7 PASS |
| BMH adapter graph capture | Showcase §3–§5, §8 PASS; MHGenericAdapterDemo 7/7 sites |
| insertArguments exact resolution | Showcase §8 PASS (was diagnostic before 2026-05-25 fix) |
| Reflection Method.invoke / Constructor.newInstance | Showcase §9, §10 PASS; 0 `reflection_target_adapter_mh_deferred` |
| invokedynamic / LambdaMetafactory capture | Showcase §1 PASS |
| Dynamic proxy generated_class | Showcase §11 PASS |
| Bytecode artifact export | Spring Boot validated (0 VerifyErrors, 8998 artifacts) |
| Loader-precise identity on all records | OverloadIdentityDemo, LoaderIdentityDemo validated |
| No `?` descriptors, no `loader_id=0` on user_target nodes | Enforced at export, validated by showcase script |
| Sibling BCI scan (no silent omissions) | Verified across all 4 adapter demos |

### Deferred (out of scope for current branch)

| Item | Reason |
|------|--------|
| Compiled-frame (-Xjit) recovery | Requires JIT-specific deoptimization hooks; large separate task |
| Virtual / interface dispatch | Focus is on MH/indy/reflection; virtual dispatch is a different mechanism |
| `asType` secondary unboxing node exact resolution | Requires unsafe BMH species field accessor; no user target hidden behind it |
| `tryFinally` cleanup slot exact resolution | Same root cause |
| `MutableCallSite` / `VolatileCallSite` target mutations | Requires per-invocation hook; out of scope for first-resolution model |
| Async / cross-thread causality | Separate subsystem (§4B in broader spec); not relevant to staticization |
| LambdaForm execution tracing | Separate subsystem (§4C); not needed for callsite target identification |
| Graph database / persistent backend | Infrastructure concern; not part of capture branch |

### Remaining acceptable diagnostics

4 diagnostics from JDK-internal classes in `ByteArray`, `ModuleDescriptor$Builder`,
and `MethodHandleAccessorFactory`. These are MH invocations where the receiver is
a method-return value inside JDK initialization code. They are not user code, not
business logic, and have zero impact on staticization coverage.

### What "done" means for Manycore / staticization scope

The branch is complete for its stated goal when:
- Every user-code MH callsite in a `-Xint` slice run produces an exact record
  (`callsite_target`, `callsite_target_set`, or `callsite_adapter_graph`)
- Zero user-code callsites produce diagnostics
- Generated lambda/proxy classes are captured with provenance metadata
- Executed class bytecode is captured pre- and post-rewrite
- All records carry loader-precise identity (no name-only keys)
- No record contains a fabricated or guessed target

**All of the above is true as of commit `775a935648b` (2026-05-25).**
