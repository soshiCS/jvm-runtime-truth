# Generated Classes

## What gets captured

Every class that is generated at runtime (as opposed to loaded from a JAR or
classpath) is captured with its bytes and provenance metadata. This includes:

- Lambda hidden classes (created by `LambdaMetafactory`)
- Dynamic proxies (created by `java.lang.reflect.Proxy`)
- ByteBuddy-generated classes
- CGLIB proxies
- Any class created via `Unsafe.defineAnonymousClass` or `MethodHandles.Lookup.defineHiddenClass`

**Requires:** `SOROUSH_RUNTIME_RECOVERY=1`

---

## Hook: `recover_runtime_generated_class` in klassFactory.cpp

**File:** `src/hotspot/share/classfile/klassFactory.cpp`
**Lines:** 509–605

This function fires at class load time (from `KlassFactory::create`) when a class
is identified as runtime-generated. Detection heuristics:
- Hidden class flag (`InstanceKlass::is_hidden()`)
- Class name pattern matching (`$Lambda/`, `$Proxy`, ByteBuddy naming conventions)
- Loaded from a synthetic ClassLoader (not a URL/file loader)

**What it does:**
1. Dumps the raw class bytes to `/tmp/soroush_jvm_dump/<classname>.class`
2. Writes a sidecar `.provenance` file with metadata (generated_by, source_trigger, etc.)
3. Calls `soroush_graph_generated_class()` to record the class in the in-memory table

---

## `soroush_graph_generated_class`

**File:** `src/hotspot/share/classfile/soroushProvenanceGraph.cpp`
**Lines:** 1017–1053

Records:
- Class name (internal form)
- Loader ID (`ClassLoaderData*` as pointer)
- CRC32 of the class bytes
- `generated_by` — the factory that created the class (see table below)
- `source_trigger` — the fully qualified class that triggered generation
- `provenance_kind` — classification of the generation mechanism
- `hidden` — true for hidden classes
- `indy_trace_id` — set when the class was created by an `invokedynamic` bootstrap

---

## `generated_by` values

| Value | Meaning |
|-------|---------|
| `"LambdaMetafactory"` | Lambda hidden class created by LambdaMetafactory.metafactory |
| `"ProxyGenerator"` | Dynamic proxy class created by Proxy.newProxyInstance |
| `"ByteBuddy"` | ByteBuddy-generated subclass or interceptor |
| `"CGLIB"` | CGLIB proxy or subclass |
| `"Unsafe"` | Class defined via Unsafe.defineAnonymousClass |
| `"LookupDefineHidden"` | Class defined via MethodHandles.Lookup.defineHiddenClass |

---

## `indy_trace_id` join key

Lambda classes created by `LambdaMetafactory` carry the same `indy_trace_id` as
the `callsite_target` record for the `invokedynamic` instruction that triggered
their creation. This allows downstream consumers to correlate:

```
callsite_target { source_bci=4, trace_id=1, lmf_impl_method="lambda$main$0" }
  ↕ trace_id = 1
generated_class { class="RuntimeTargetShowcaseDemo$$Lambda/...", indy_trace_id=1 }
```

---

## Dynamic proxy capture

Proxies are created by `sun.misc.ProxyGenerator` (or `java.lang.reflect.ProxyGenerator`
in JDK21). The proxy class is generated in memory and passed to a class loader's
`defineClass`. The recovery hook fires at `defineClass` time.

**`generated_by`:** `"ProxyGenerator"`
**`source_trigger`:** `"java.lang.reflect.Proxy"`
**`class`:** Starts with `"$Proxy"` followed by an integer index

Proxy invocations route through `InvocationHandler.invoke`, which is a
`Method.invoke` call. That is captured by the reflection capture path
(see [REFLECTION_CAPTURE.md](REFLECTION_CAPTURE.md)).

---

## Difference from `bytecode_artifact`

`generated_class` records and `bytecode_artifact` records are distinct:

| | `generated_class` | `bytecode_artifact` |
|--|---|---|
| Source | Runtime-generated class (not from classpath) | Any loaded class |
| Requires | `SOROUSH_RUNTIME_RECOVERY=1` | `SOROUSH_CAPTURE_FINAL_BYTECODE=1` |
| Bytes | Dumped to `/tmp/soroush_jvm_dump/` | Captured in-memory |
| Extra fields | `generated_by`, `source_trigger`, `indy_trace_id` | `kind` (original/final), `rewritten_from_crc` |

A lambda class will appear in BOTH tables:
- `generated_class` (provenance: who generated it, why, from which indy site)
- `bytecode_artifact` with `kind="original"` (what bytes it was loaded with)
- `bytecode_artifact` with `kind="final"` if it was instrumented by the Phase 5 rewriter

---

## Disk dump

Raw class bytes are dumped to `/tmp/soroush_jvm_dump/` by the recovery hook.
The directory is created if it does not exist. File names are the class name with
`/` replaced by `_` and `.class` appended.

The disk dump is separate from the JSONL export. The JSONL `generated_class`
record contains metadata only (not the bytes). To reconstruct the actual bytes of
a generated class, combine the `crc` field from the JSONL record with the file
from the dump directory.

---

## Known limitations

- Detection of ByteBuddy / CGLIB classes is heuristic (pattern matching on class
  names and loader identity). A custom agent that uses different naming conventions
  may not be classified correctly — it will still be captured but `generated_by`
  may be `"unknown"`.
- Hidden classes that are loaded from JAR (some frameworks pre-generate and ship
  lambda classes) are NOT captured by the recovery path — they appear only as
  `bytecode_artifact` records with `hidden=true`.
- The `/tmp/soroush_jvm_dump/` path is hardcoded. Class name collisions (same
  name under two loaders) will cause the second dump to overwrite the first.
