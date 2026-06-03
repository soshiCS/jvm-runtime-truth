# Lambda / InvokeDynamic Staticization-Readiness Validation

**Date:** 2026-06-01  
**Status:** Validated  
**Run used:** `/tmp/rt_ui_runs/05160976` (Runtime Truth Cases, Case01_LambdaIndy)

---

## 1. Source of Truth

This report uses three independent sources, in priority order:

| Source | Used for |
|---|---|
| Compiled `.class` artifact (captured run) | Ground truth on what code ran |
| Runtime JSONL (`runtime_targets.jsonl`) | Dispatch evidence, linkage, staticizability flags |
| Current `.java` source file | Cross-reference only — see §2 |

---

## 2. Source / Binary Mismatch — Site 5

**The current `.java` source and the compiled artifact disagree on Site 5.**

- `Case01_LambdaIndy.java` line 27: `Supplier<String> lower = base::toUpperCase;`
- Compiled artifact `BootstrapMethods #4`: `REF_invokeVirtual java/lang/String.toLowerCase`
- Captured lambda class `trace28_recover64.class`: `invokevirtual java/lang/String.toLowerCase`
- JSONL `callsite_target` BCI=103: `lmf_impl_method=toLowerCase`

**The source was edited to `toUpperCase` after the JAR was compiled. The JAR was not recompiled.**  
All analysis in this report uses the binary as source of truth. The assertion `s4.equals("PREFIX")` would fail against the stale binary.

---

## 3. Complete Site Mapping

### Key mappings derived from data

**Trace → CRC → +0x identity** (from per-trace `.metadata.txt` files):

| trace | CRC | +0x runtime name |
|---|---|---|
| 23 | `5768c00e` | `…$$Lambda+0x000000c800088230` |
| 24 | `10d9d8c9` | `…$$Lambda+0x000000c800088460` |
| 25 | `3285256c` | `…$$Lambda+0x000000c8000886b0` |
| 27 | `1fe9e6e7` | `…$$Lambda+0x000000c8000888e8` |
| 28 | `9d6569dc` | `…$$Lambda+0x000000c800088b28` |

(Prefix: `testcases/Case01_LambdaIndy`. trace 26 = `makeConcatWithConstants` string concat, no lambda class.)

---

### Site 1 — Non-capturing lambda

| Field | Value |
|---|---|
| **Source expression** | `Runnable r = () -> {}` |
| **Source method** | `Case01_LambdaIndy.run()V` |
| **Source BCI** | 6 |
| **indy bootstrap** | `LambdaMetafactory.metafactory` |
| **indy name / descriptor** | `run / ()Ljava/lang/Runnable;` |
| **LMF target** | `Case01_LambdaIndy.lambda$run$0()V` (REF_invokeStatic) |
| **Generated class** | `…$$Lambda+0x000000c800088230` (crc=`5768c00e`, trace=23) |
| **FI implemented** | `java.lang.Runnable` |
| **FI method** | `run()V` |
| **Lambda class body** | `invokestatic Case01_LambdaIndy.lambda$run$0:()V` |
| **Captures** | none (no fields, no-arg constructor) |
| **Runtime dispatch** | BCI=13 `invokeinterface` → `…+0x…88230.run()` (OBSERVED_ONLY) |
| **RT linkage evidence** | BCI=6 `runtime_target` → `Case01_LambdaIndy.lambda$run$0()V` (LINKAGE_GUARANTEED) |
| **Artifact path** | `testcases_Case01_LambdaIndy$$Lambda_trace23_recover53.class` |
| **staticizable** | `true` |
| **reconstructable** | `false` |
| **Missing info** | none |
| **Staticizer rewrite** | `invokeinterface r.run()` → `invokestatic Case01_LambdaIndy.lambda$run$0()` — lambda object allocation eliminated |

---

### Site 2 — Static method reference

| Field | Value |
|---|---|
| **Source expression** | `Function<Integer,String> toStr = String::valueOf` |
| **Source method** | `Case01_LambdaIndy.run()V` |
| **Source BCI** | 18 |
| **indy bootstrap** | `LambdaMetafactory.metafactory` |
| **indy name / descriptor** | `apply / ()Ljava/util/function/Function;` |
| **LMF target** | `java/lang/String.valueOf(Ljava/lang/Object;)Ljava/lang/String;` (REF_invokeStatic) |
| **Generated class** | `…$$Lambda+0x000000c800088460` (crc=`10d9d8c9`, trace=24) |
| **FI implemented** | `java.util.function.Function` |
| **FI method** | `apply(Object)Object` |
| **Lambda class body** | `checkcast Integer` → `invokestatic String.valueOf:(Object)String` → `areturn` |
| **Captures** | none (no fields, no-arg constructor) |
| **Runtime dispatch** | BCI=30 `invokeinterface` → `…+0x…88460.apply()` (OBSERVED_ONLY) |
| **RT linkage evidence** | BCI=18 `runtime_target` → `String.valueOf(Object)String` (LINKAGE_GUARANTEED) |
| **Artifact path** | `testcases_Case01_LambdaIndy$$Lambda_trace24_recover54.class` |
| **staticizable** | `true` |
| **reconstructable** | `false` |
| **Missing info** | none |
| **Staticizer rewrite** | `invokeinterface toStr.apply(42)` → `invokestatic String.valueOf(42)` — lambda object allocation eliminated |

---

### Site 3 — Capturing lambda (closed over local variable)

| Field | Value |
|---|---|
| **Source expression** | `Supplier<String> sup = () -> greeting + "-world"` |
| **Source method** | `Case01_LambdaIndy.run()V` |
| **Source BCI** | 43 |
| **indy bootstrap** | `LambdaMetafactory.metafactory` |
| **indy name / descriptor** | `get / (Ljava/lang/String;)Ljava/util/function/Supplier;` |
| **LMF target** | `Case01_LambdaIndy.lambda$run$1(Ljava/lang/String;)Ljava/lang/String;` (REF_invokeStatic) |
| **Generated class** | `…$$Lambda+0x000000c8000886b0` (crc=`3285256c`, trace=25) |
| **FI implemented** | `java.util.function.Supplier` |
| **FI method** | `get()Object` |
| **Lambda class body** | `getfield arg$1:String` → `invokestatic Case01_LambdaIndy.lambda$run$1:(String)String` → `areturn` |
| **Captures** | `arg$1: java.lang.String` (the value of `greeting` at the indy site) |
| **Runtime dispatch** | BCI=52 `invokeinterface` → `…+0x…886b0.get()` (OBSERVED_ONLY) |
| **RT linkage evidence** | BCI=43 `runtime_target` → `Case01_LambdaIndy.lambda$run$1(String)String` (LINKAGE_GUARANTEED) |
| **Artifact path** | `testcases_Case01_LambdaIndy$$Lambda_trace25_recover55.class` |
| **staticizable** | `true` |
| **reconstructable** | `false` |
| **Missing info** | Captured value not in JSONL (only captured type is recoverable from class bytecode) |
| **Staticizer rewrite** | `invokeinterface sup.get()` → `invokestatic Case01_LambdaIndy.lambda$run$1(greeting)` — devirtualized; allocation still occurs unless caller is also staticized. Full elimination requires inlining across the indy site. |

---

### Site 4 — Two-argument lambda

| Field | Value |
|---|---|
| **Source expression** | `BiFunction<Integer,Integer,Integer> add = (a, b) -> a + b` |
| **Source method** | `Case01_LambdaIndy.run()V` |
| **Source BCI** | 62 |
| **indy bootstrap** | `LambdaMetafactory.metafactory` |
| **indy name / descriptor** | `apply / ()Ljava/util/function/BiFunction;` |
| **LMF target** | `Case01_LambdaIndy.lambda$run$2(Ljava/lang/Integer;Ljava/lang/Integer;)Ljava/lang/Integer;` (REF_invokeStatic) |
| **Generated class** | `…$$Lambda+0x000000c8000888e8` (crc=`1fe9e6e7`, trace=27) |
| **FI implemented** | `java.util.function.BiFunction` |
| **FI method** | `apply(Object,Object)Object` |
| **Lambda class body** | `checkcast Integer` × 2 → `invokestatic Case01_LambdaIndy.lambda$run$2:(Integer,Integer)Integer` → `areturn` |
| **Captures** | none (no fields, no-arg constructor) |
| **Runtime dispatch** | BCI=79 `invokeinterface` → `…+0x…888e8.apply()` (OBSERVED_ONLY) |
| **RT linkage evidence** | BCI=62 `runtime_target` → `Case01_LambdaIndy.lambda$run$2(Integer,Integer)Integer` (LINKAGE_GUARANTEED) |
| **Artifact path** | `testcases_Case01_LambdaIndy$$Lambda_trace27_recover63.class` |
| **staticizable** | `true` |
| **reconstructable** | `false` |
| **Missing info** | none |
| **Staticizer rewrite** | `invokeinterface add.apply(3,4)` → `invokestatic Case01_LambdaIndy.lambda$run$2(3,4)` — lambda object allocation eliminated |

---

### Site 5 — Instance method reference on captured receiver

| Field | Value |
|---|---|
| **Source expression** | `Supplier<String> lower = base::toLowerCase` **(binary)** / `base::toUpperCase` (source — stale) |
| **Source method** | `Case01_LambdaIndy.run()V` |
| **Source BCI** | 103 |
| **indy bootstrap** | `LambdaMetafactory.metafactory` |
| **indy name / descriptor** | `get / (Ljava/lang/String;)Ljava/util/function/Supplier;` |
| **LMF target** | `java/lang/String.toLowerCase()Ljava/lang/String;` (REF_invokeVirtual) |
| **Generated class** | `…$$Lambda+0x000000c800088b28` (crc=`9d6569dc`, trace=28) |
| **FI implemented** | `java.util.function.Supplier` |
| **FI method** | `get()Object` |
| **Lambda class body** | `getfield arg$1:String` → `invokevirtual String.toLowerCase:()String` → `areturn` |
| **Captures** | `arg$1: java.lang.String` (the receiver object — value of `base` at the indy site) |
| **Runtime dispatch** | BCI=112 `invokeinterface` → `…+0x…88b28.get()` (OBSERVED_ONLY) |
| **RT linkage evidence** | BCI=103 `runtime_target` → `String.toLowerCase()String` (LINKAGE_GUARANTEED) |
| **Artifact path** | `testcases_Case01_LambdaIndy$$Lambda_trace28_recover64.class` |
| **staticizable** | `true` |
| **reconstructable** | `false` |
| **Source/binary mismatch** | **Yes** — binary has `toLowerCase`, source has `toUpperCase`. RT correctly reports binary. |
| **Missing info** | Captured receiver value not in JSONL. Receiver type (`String`) is confirmed by class bytecode. |
| **Staticizer rewrite** | `invokeinterface lower.get()` → `invokevirtual arg$1.toLowerCase()` where `arg$1` is the captured `base`. If the static type of `base` is `String` (proven), `String.toLowerCase` is non-overridable — fully devirtualizable. |

---

## 4. String Concat Sites — Distinguished from Lambda

Two additional `invokedynamic` sites in `run()` use `StringConcatFactory`, not `LambdaMetafactory`:

| BCI | Category | indy name | Notes |
|---|---|---|---|
| 132 | `invokedynamic` | `makeConcatWithConstants` | Output string concat — `reconstructable=true, staticizable=true` |
| 7 (in `lambda$run$1`) | `invokedynamic` | `makeConcatWithConstants` | String concat inside the body of lambda body method |

RT correctly sets `category=invokedynamic` for both but they produce no lambda class and are not `LambdaMetafactory` bootstraps. A staticizer must check the bootstrap method name to distinguish them.

---

## 5. Evidence Quality Summary

| Evidence type | Present | Quality | Notes |
|---|---|---|---|
| `callsite_target` with `lmf_impl_*` | ✅ all 5 sites | LINKAGE_GUARANTEED | Exact impl class/method/descriptor |
| `staticizable` flag | ✅ all 5 sites | — | True for all |
| `reconstructable` flag | ✅ all 5 sites | — | False for all (staticizer needs lmf data) |
| `hidden_class_identity` CRC→+0x map | ✅ all 5 instances | — | CRC uniquely identifies each class |
| `bytecode_artifact` per-instance | ✅ all 5 instances | CRC-indexed | File path shared; per-trace recover copies present |
| `runtime_target` at indy BCI | ✅ all 5 sites | LINKAGE_GUARANTEED | impl method confirmed at MH linkage time |
| `invokeinterface` dispatch→+0x | ✅ all 5 instances | OBSERVED_ONLY | Confirms which instance handled each call |
| `callsite_adapter_graph` for indy | ✅ all 5 sites | OBSERVED_ONLY | MH adapter chain, bound data |
| Captured argument type | ✅ from bytecode | class bytecode | Sites 3, 5 capture `String` — visible in `arg$1` field |
| Captured argument value | ❌ not in JSONL | — | Values ("hello", "prefix") not recorded — see §6 |
| `generated_class.impl_class` | ❌ absent | — | `impl_class/method/descriptor` are null in all 5 records |

---

## 6. What Is Missing and Why It Matters

### Missing: captured value at indy sites

Sites 3 and 5 create lambda instances that capture a `String` value at the indy site. The JSONL records the **type** of the capture (visible from the lambda class bytecode: `arg$1: java.lang.String`) but not the **value** ("hello" or "prefix").

For staticization, the value is generally not needed — the staticizer rewrites the dispatch, not the data. The indy site in the source code still passes the captured arg. So this is **not a blocker for dispatch staticization**.

It would matter only if the staticizer tried to constant-fold the captured value. That is beyond dispatch staticization.

### Missing: `generated_class` records have no impl linkage

All 5 `generated_class` records have `impl_class=null`, `impl_method=null`, `impl_descriptor=null`. The mapping from generated class → LMF impl method must be reconstructed from the corresponding `callsite_target` record using CRC matching via the per-trace metadata files. This is a **secondary data path** — it works but requires the per-trace recover files to be present.

If the recover files were absent, the link from CRC → trace → BCI → impl would be broken.

### Missing: `bytecode_artifact.trace_id`

`bytecode_artifact` records carry no `trace_id` field. The CRC→trace link exists only through the `.metadata.txt` sidecar files. These files are present in this run but are not guaranteed to be present in all environments. A staticizer relying on the JSONL alone cannot directly map CRC → source BCI without the sidecar files.

---

## 7. Indexer and UI Issues Found During Validation

### Bug (fixed): lambda family creation skipped when `generated_class` record precedes it

**Root cause:** The `generated_class` record handler inserted a stub entry for the base name into `classes` before the lambda family creation step. The family creation guard (`if base in classes: continue`) treated the stub as a real class entry and skipped family creation.

**Symptom:** `Case01_LambdaIndy$$Lambda` appeared in the class list with `is_lambda_family=None`, `has_artifacts=False`, no member list — the family view never rendered.

**Fix:** Changed the guard to skip only when the existing entry has record types beyond `generated_class`:
```python
existing = classes.get(base)
if existing and (existing.get("record_types", set()) - {"generated_class"}):
    continue
```

**Status:** Fixed in `indexer.py`. 35/35 tests pass.

---

## 8. Test Cases

### Test A — Same source class, same FI type, different implementations

**Case:** Sites 3 and 5 both produce `Supplier<String>` instances that capture a `String`. They share the same functional interface, the same indy descriptor shape `(String)Supplier`, and the same base display name `Case01_LambdaIndy$$Lambda`. Their implementations differ:

| Property | Site 3 | Site 5 |
|---|---|---|
| CRC | `3285256c` | `9d6569dc` |
| +0x address | `…886b0` | `…88b28` |
| Lambda body | `invokestatic lambda$run$1(String)` | `invokevirtual String.toLowerCase()` |
| Captured field | `arg$1: String` (closes over `greeting`) | `arg$1: String` (closes over `base` = the receiver) |
| Runtime dispatch BCI | 52 | 112 |

**Validation result:** Both are correctly indexed as distinct `+0x` entries with distinct CRCs. No identity collapse. The lambda family node `Case01_LambdaIndy$$Lambda` lists both as members along with the other 3. The indexer correctly preserves them as separate classes.

---

### Test B — Same base display name, five distinct runtime identities

**Case:** All 5 lambda instances share the display base name `Case01_LambdaIndy$$Lambda`. They must not be overwritten or collapsed by the indexer.

**Before fix:** The `generated_class` handler created a single entry for the base name. If that entry was treated as a real class, four of the five instances would have no UI representation.

**After fix:** The indexer creates 5 distinct `+0x` class entries and 1 lambda family entry (6 total). The family entry has `is_lambda_family=True`, `lambda_count=5`, `has_artifacts=False`. Each `+0x` entry has `has_artifacts=True`, a distinct CRC, and its own bytecode available through the per-trace recover file.

**Indexer test coverage:** `test_five_distinct_hidden_instances_remain_distinct` in `tests/test_indexer.py` verifies this invariant (35/35 passing).

---

### Test C — Captured receiver method reference

**Case:** Site 5 (`base::toLowerCase`). The captured value (`arg$1`) is the receiver object for the method call, not an argument to it. The lambda class captures a `String` and calls `invokevirtual String.toLowerCase()` on it.

**What RT captures:**
- LMF target: `String.toLowerCase()String` (REF_invokeVirtual) — confirmed in JSONL and in lambda class bytecode
- Captured type: `arg$1: String` — confirmed from class bytecode
- The receiver is typed `String` → `toLowerCase()` is non-virtual at this type (final class) → **fully devirtualizable**

**What RT does NOT capture:**
- The identity of the captured object at runtime (its value or address)

**Staticizer conclusion:** The dispatch `invokeinterface lower.get()` can be rewritten to `invokevirtual captured_string.toLowerCase()` and further to `invokestatic String.toLowerCase(captured_string)` if the method is treated as static. The lambda class allocation can be eliminated. Full correctness requires that the capture type `String` be proven at all call paths — which it is, since the `invokedynamic` descriptor is `(String)Supplier`.

---

## 9. Verdict

### Overall: **Partially ready — dispatch staticization ready, value capture not recorded**

| Requirement | Status |
|---|---|
| All 5 lambda sites accounted for | ✅ |
| Each site has LMF impl class/method/descriptor | ✅ (from `callsite_target`) |
| Each site has `staticizable=True` | ✅ |
| Each site has a distinct `+0x` class entry | ✅ (after indexer fix) |
| Each class has bytecode evidence | ✅ (per-trace recover files) |
| CRC → +0x identity mapping | ✅ (via `hidden_class_identity`) |
| invokeinterface dispatch → +0x target confirmed | ✅ |
| String concat indy sites distinguished | ✅ |
| Captured argument type recoverable | ✅ (from class bytecode) |
| No lambda class lost or collapsed | ✅ (after indexer fix) |
| `bytecode_artifact.trace_id` present | ❌ — BCI→CRC link requires sidecar files |
| `generated_class.impl_class` populated | ❌ — null for all 5 records |
| Captured argument values recorded | ❌ — not in JSONL |

### What a staticizer can do with this data

A dispatch staticizer can, for all 5 sites, replace `invokeinterface` calls on lambda objects with direct `invokestatic` or `invokevirtual` calls to the bound implementation method. For non-capturing lambdas (Sites 1, 2, 4) the lambda object allocation can also be eliminated. For capturing lambdas (Sites 3, 5) the allocation remains unless the site is also inlined.

### What requires additional work

1. `bytecode_artifact.trace_id` should be populated in the JSONL record itself, not only in the sidecar `.metadata.txt` file, to make the BCI→CRC linkage self-contained.
2. `generated_class` records should carry `impl_class`, `impl_method`, `impl_descriptor` to allow direct CRC → impl lookup without going through `callsite_target` records.
3. The source/binary mismatch on Site 5 is a data hygiene issue in the test corpus, not a deficiency in RT. Rebuild the JAR after source changes.
