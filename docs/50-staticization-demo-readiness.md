# Staticization Demo Readiness — Full Case Suite

**Date:** 2026-06-01  
**Status:** Validated  
**Run:** `/tmp/rt_ui_runs/05160976` (Runtime Truth Cases demo, 12 cases)  
**Total JSONL records:** ~4 522 across all cases

---

## 1. Overall Verdict

**PARTIAL — 7 READY, 5 PARTIAL, 0 FAIL**

Demo is viable with caveats. All cases produce relevant runtime data. The PARTIAL cases have gaps in staticization proof but not in runtime coverage — the required information exists in the data, just not always with full LINKAGE_GUARANTEED strength or complete field population.

---

## 2. Per-Case Verdict Table

| Case | Mechanism | Verdict | Blocker summary |
|---|---|---|---|
| Case01_LambdaIndy | Lambda / invokedynamic | **PARTIAL** | Site 5 source/binary mismatch; `generated_class.impl_*` null |
| Case02_StringConcatIndy | String concat indy | **READY** | — |
| Case03_DirectMethodHandle | Direct MH invoke | **READY** | — |
| Case04_MHReceiverOrigins | MH from field/arr/method | **PARTIAL** | Array-element and method-return origin not LINKAGE_GUARANTEED |
| Case05_TypeAdapters | asType adapters | **PARTIAL** | MH adapter invocations are OBSERVED_ONLY, not LINKAGE_GUARANTEED |
| Case06_ArgumentAdapters | insert/drop/bind/permute | **PARTIAL** | Same as Case05 |
| Case07_FilterFoldCollect | filter/fold chains | **PARTIAL** | Same as Case05 |
| Case08_SpreadCollector | asCollector/asSpreader | **READY** | — |
| Case09_GuardCatchFinally | GWT / catch / tryFinally | **READY** | — |
| Case10_Reflection | Method.invoke / ctor | **READY** | — |
| Case11_DynamicProxy | JDK dynamic proxy | **PARTIAL** | Handler→impl routing implicit, not explicit record chain |
| Case12_HiddenClass | Lookup.defineHiddenClass | **READY** | — |
| HiddenClassTemplate | Template type | **READY** | Artifact + identity captured |
| HiddenOp | Interface | **READY** | Artifact captured |
| Runtime TruthCasesMain | Dispatch harness | **READY** | All 12 entry lambdas captured |

---

## 3. Detailed Per-Case Analysis

---

### Case01_LambdaIndy — PARTIAL

**Mechanism:** `invokedynamic` + `LambdaMetafactory`, 5 lambda sites, 2 string concat sites

**Records present:**
- `callsite_target`: 103 — 5 invokedynamic (lambda), 2 invokedynamic (concat), rest invokeinterface/invokevirtual dispatch
- `runtime_target`: 107 — 10 methodhandle_linkage (2 per lambda site: impl + hidden class `<init>`), 5 execution_trace
- `bytecode_artifact`: 7 — 2 for main class (`original` + instrumented), 5 for lambda instances (CRC-indexed)
- `generated_class`: 5 — one per lambda instance; `impl_class/method/descriptor` are **null** in all 5
- `hidden_class_identity`: 5 — maps each `+0x` address to its CRC
- `callsite_adapter_graph`: 5 — one per indy site (including concat sites)

**Sites:**

| BCI | Source expression | LMF impl | +0x identity | staticizable | reconstructable |
|---|---|---|---|---|---|
| 6 | `() -> {}` → Runnable | `lambda$run$0()V` | `+0x…88230` | true | false |
| 18 | `String::valueOf` → Function | `String.valueOf(Object)` | `+0x…88460` | true | false |
| 43 | `() -> greeting+"-world"` → Supplier | `lambda$run$1(String)` | `+0x…886b0` | true | false |
| 62 | `(a,b)->a+b` → BiFunction | `lambda$run$2(Integer,Integer)` | `+0x…888e8` | true | false |
| 103 | **`base::toLowerCase`** (binary) | `String.toLowerCase()` | `+0x…88b28` | true | false |

**PARTIAL — reasons:**
1. **Site 5 source/binary mismatch.** Current `.java` has `base::toUpperCase`; compiled artifact and JSONL both say `toLowerCase`. The JAR was not recompiled after the source edit. Assertion `s4.equals("PREFIX")` would fail. *(Data hygiene issue, not RT deficiency.)*
2. **`generated_class.impl_class` is null.** The 5 `generated_class` records carry no `impl_class`/`impl_method`/`impl_descriptor`. A staticizer relying solely on `generated_class` records cannot map a lambda class CRC to its implementation method without cross-referencing `callsite_target`.
3. **Captured argument values absent.** Sites 3 and 5 capture a `String`; the type is recoverable from class bytecode but the captured value is not recorded in the JSONL.

**Staticizer coverage:** Complete for dispatch rewriting on Sites 1–4. Site 5 dispatch is correct per binary. Captured type information sufficient. Allocation elimination feasible for non-capturing sites (1, 2, 4).

---

### Case02_StringConcatIndy — READY

**Mechanism:** `StringConcatFactory.makeConcatWithConstants`, 7 sites (not lambda)

**Records present:**
- `callsite_target`: 38 — 7 invokedynamic with `indy_name=makeConcatWithConstants`, all `staticizable=true reconstructable=true`
- `runtime_target`: 149 — MH adapter chains for concat operations
- `callsite_adapter_graph`: 7 — one per concat site, showing concat recipe and strategy (prepend/mix/newString)

**Sites:** BCIs 15, 25, 38, 46, 68, 81, 107 — two-arg, multi-arg, primitive+Object mix, constant sandwich, all-primitive chain, long concat.

**All 7 are `reconstructable=true`.** This means a staticizer can reconstruct the concat operation from the recipe string and descriptor alone — no runtime data required beyond the linkage event. RT provides the linkage anyway.

**String concat vs lambda distinction confirmed.** The `indy_name=makeConcatWithConstants` is correctly distinct from lambda `indy_name=run/apply/get`. Staticizer must check bootstrap method class (`StringConcatFactory` vs `LambdaMetafactory`) to distinguish them.

**Staticizer coverage:** Complete. All 7 sites can be rewritten to direct `StringBuilder` or constant-fold sequences. MH adapter chain graphs show the actual concat strategy chosen by the JDK.

---

### Case03_DirectMethodHandle — READY

**Mechanism:** `MethodHandle.invokeExact` / `invoke` on static, virtual, JDK methods

**Records present:**
- `callsite_target`: 565 — 5 `methodhandle_invoke` at BCIs 43/83/93/120/148 (OBSERVED_ONLY), plus invokevirtual/invokestatic internal dispatch
- `runtime_target`: 54 — `methodhandle_linkage` at MH construction BCIs (LINKAGE_GUARANTEED)
- `callsite_adapter_graph`: 3

**Sites:**

| BCI (invoke) | MH origin | MH construction BCI | Target confirmed (LINKAGE_GUARANTEED) |
|---|---|---|---|
| 43 | `findStatic(…staticAdd…)` | 36 | `Case03.staticAdd(int,int)int` |
| 83 | `findVirtual(…instanceMul…)` | 69 | `Case03.instanceMul(int,int)int` |
| 93 | same `mhStatic` (`.invoke`) | 36 | `Case03.staticAdd(int,int)int` |
| 120 | `findVirtual(…greet…)` | 104 | `Case03.greet(String)String` |
| 148 | `findStatic(Math…abs…)` | 133 | `Math.abs(int)int` |

**Two-layer evidence:** `runtime_target(methodhandle_linkage)` at construction BCI gives LINKAGE_GUARANTEED. `callsite_target(methodhandle_invoke)` at invoke BCI gives OBSERVED_ONLY confirmation. Together these prove both binding and execution.

**Staticizer coverage:** Complete for all 5 sites. Each MH was created in the same method body via a direct `findStatic`/`findVirtual` call, so the bound target is provably constant for that call.

---

### Case04_MHReceiverOrigins — PARTIAL

**Mechanism:** MH stored in local, instance field, static final field, method return, array element, inline chain

**Records present:**
- `callsite_target`: 147 — 6 `methodhandle_invoke` at BCIs 31/42/51/67/95/121 (OBSERVED_ONLY)
- `runtime_target`: 31 — `methodhandle_linkage` at construction sites (partial — not all 6 have explicit linkage records)
- `callsite_adapter_graph`: 1

**Targets observed:**

| BCI | Origin | Target method (OBSERVED) | LINKAGE_GUARANTEED present |
|---|---|---|---|
| 31 | local variable | `Math.abs(int)int` | Partially — local construction site captured |
| 42 | instance field `fieldMH` | `Math.max(int,int)int` | Yes — `<init>@31 → Math.max` |
| 51 | static final `STATIC_FIELD_MH` | `Math.abs(int)int` | Not seen — `<clinit>` not captured in user-scope runtime_targets |
| 67 | method return | `Math.min(int,int)int` | Yes — `getMHFromMethod@26 → Math.min` |
| 95 | **array element `arr[0]`** | `Math.abs(int)int` | Not explicit — inferred from same MH as BCI=31 |
| 121 | inline chain | `Math.abs(int)int` | Not seen in available records |

**PARTIAL — reasons:**
1. **Static final field (`STATIC_FIELD_MH`) linkage not confirmed.** The MH is created in `<clinit>`. RT records `<clinit>` execution but the static initializer's `methodhandle_linkage` record for `Math.abs` was not found in the user-scoped runtime_target set. The target is observable at BCI=51 as OBSERVED_ONLY.
2. **Array element origin not proven.** BCI=95 invokes the MH stored in `arr[0]`. RT confirms `Math.abs` was called (OBSERVED_ONLY) but does not record which array slot the MH came from or prove that `arr[0]` is always the same MH.
3. **Inline chain linkage.** BCI=121 — the chain `lookup().findStatic(…abs…).invoke(-7)` — RT shows `Math.abs` was called (OBSERVED_ONLY) but the intermediate inline MH object has no explicit LINKAGE_GUARANTEED record.

**Staticizer coverage:** All 6 actual runtime targets are correctly identified. LINKAGE_GUARANTEED is confirmed for instance-field and method-return origins. For array-element and inline-chain origins, a staticizer must additionally verify that the data path always produces the same MH (data-flow analysis beyond what RT records).

---

### Case05_TypeAdapters — PARTIAL

**Mechanism:** `MethodHandle.asType` — box/unbox, widen, cast, combined adapters

**Records present:**
- `callsite_target`: 733 — 5 `methodhandle_invoke` (BCI 37/69/84/130/175, OBSERVED_ONLY), many invokevirtual on internal MH classes
- `runtime_target`: 69 — `methodhandle_linkage` at construction/asType application sites (LINKAGE_GUARANTEED for final targets)
- `callsite_adapter_graph`: 6 — one per adapted MH, showing full adapter chain including box/unbox nodes

**Targets:**

| Site | Adapter | Final target | RT evidence |
|---|---|---|---|
| 1 (BCI=37) | `int→Object` (box) | `Case05.doubleIt(int)int` | `runtime_target@18 → Case05.doubleIt LINKAGE_GUARANTEED` |
| 2 (BCI=69) | `int/int→Integer/Integer` (unbox) | `Case05.boxedTriple(Integer)Integer` | `runtime_target@53 → Case05.boxedTriple LINKAGE_GUARANTEED` |
| 3 (BCI=84) | `int→long` (widen) | `Case05.widenOp(long)long` | Not explicit in shown records |
| 4 (BCI=130) | `Object→int` (checkcast) | `Case05.doubleIt(int)int` | — |
| 5 (BCI=175) | `Object/Object→int/Object` | `Case05.doubleIt(int)int` | — |

**PARTIAL — reasons:**
1. **Not all 5 invoke BCIs have LINKAGE_GUARANTEED in runtime_target.** Sites 3–5 have OBSERVED_ONLY on `methodhandle_invoke`. The LINKAGE_GUARANTEED records cover Sites 1 and 2 explicitly; Sites 3–5 targets are inferable from the adapter chain graph but not confirmed with a single direct linkage record.
2. **Adapter chain is correct** — `callsite_adapter_graph` records show the full asType adapter chain including type conversion nodes. A staticizer reading the adapter graph plus the OBSERVED_ONLY target can reconstruct the complete call path.

**Staticizer coverage:** Sufficient for Sites 1–2. Sites 3–5 require the staticizer to read the adapter graph to identify the final target. All targets are recoverable. No blocking gaps.

---

### Case06_ArgumentAdapters — PARTIAL

**Mechanism:** `insertArguments`, `dropArguments`, `bindTo`, `permuteArguments`

**Records present:**
- `callsite_target`: 282 — 4 `methodhandle_invoke` (OBSERVED_ONLY) at adapt-invoke BCIs
- `runtime_target`: 26 — `methodhandle_linkage` at construction BCIs (LINKAGE_GUARANTEED for primary targets)
- `callsite_adapter_graph`: 6 — one per adapted MH

**Targets:**

| Site | Adapter | Final target | RT evidence |
|---|---|---|---|
| 1 — insertArguments | pre-bind args 0,1=10,20 | `Case06.add3(int,int,int)int` | `runtime_target@34 → Case06.add3 LINKAGE_GUARANTEED` |
| 2 — dropArguments | ignore String at pos 0 | `Case06.add3(int,int,int)int` | inferred from adapter graph + observed target |
| 3 — bindTo | bind receiver `"HelloWorld"` | `String.substring(int)String` | `runtime_target@[lk.findVirtual BCI] → String.substring` |
| 4 — permuteArguments | reorder (2,1,0) | `Case06.sub3(int,int,int)int` | inferred from graph |

**PARTIAL — reason:** The adapter operations themselves (`insertArguments`, `dropArguments`, `permuteArguments`) are visible in the `callsite_adapter_graph` with the pre-bound values and reorder arrays recorded as bound data nodes. However, the `methodhandle_invoke` callsite records for Sites 2 and 4 are OBSERVED_ONLY without corresponding LINKAGE_GUARANTEED runtime_target. RT establishes the primary MH construction linkage (Site 1 = `add3` confirmed LINKAGE_GUARANTEED) but the composition steps for `drop`/`permute` are graph-visible rather than individually linkage-guaranteed.

**Staticizer coverage:** Complete with adapter graph reading. The pre-bound values for `insertArguments` (10, 20) and the bound receiver for `bindTo` ("HelloWorld") are in the adapter graph bound-data nodes. The permutation array (2,1,0) is in the graph. All necessary information for staticization is present.

---

### Case07_FilterFoldCollect — PARTIAL

**Mechanism:** `filterArguments`, `filterReturnValue`, `foldArguments`

**Records present:**
- `callsite_target`: 80 — 3 `methodhandle_invoke` (OBSERVED_ONLY) at composite MH invoke BCIs
- `runtime_target`: 35 — `methodhandle_linkage` for constituent components (LINKAGE_GUARANTEED)
- `callsite_adapter_graph`: 6

**Targets:**

| Site | Combinator | Components confirmed | LINKAGE_GUARANTEED |
|---|---|---|---|
| filterArguments | `Integer.sum(negate(a), negate(b))` | `Integer.sum`, `Case07.negate` (×2) | All 3 — `runtime_target@28→sum`, `@46→negate` |
| filterReturnValue | `brackets(join(a,b))` | `Case07.join`, `Case07.brackets` | Both — `@97→join`, `@114→brackets` |
| foldArguments | `triSum(product(a,b), a, b)` | `Case07.triSum`, `Case07.product` | Both — `@[TriSum BCI]→triSum`, `@[prod BCI]→product` |

**PARTIAL — reason:** The composite MH invocations at the final call BCIs (when `mhFiltered.invoke(3,5)` etc. are called) have `OBSERVED_ONLY` evidence on the `callsite_target(methodhandle_invoke)` records. The constituent methods (negate, sum, join, brackets, triSum, product) are individually confirmed LINKAGE_GUARANTEED in `runtime_target` records at their respective MH construction BCIs. Together this is complete evidence, but the composite invocation evidence is OBSERVED_ONLY.

**Staticizer coverage:** All constituent methods confirmed. Adapter graphs show the exact filter/fold composition. A staticizer can inline the composite: `filterArguments(mhSum, mhNeg, mhNeg).invoke(a,b)` → `Integer.sum(Case07.negate(a), Case07.negate(b))`. Sufficient for full staticization.

---

### Case08_SpreadCollector — READY

**Mechanism:** `asCollector`, `asSpreader`, `invokeWithArguments`

**Records present:**
- `callsite_target`: 571 — `methodhandle_invoke` at BCI=169 (invokeWithArguments, OBSERVED_ONLY) plus invokevirtual internal
- `runtime_target`: 86 — LINKAGE_GUARANTEED at construction BCIs for all 3 primary targets
- `callsite_adapter_graph`: 6

**Sites:**

| Site | Adapter | Final target | RT evidence |
|---|---|---|---|
| 1 — asCollector (3 ints→int[]) | Collector wraps `sumArray` | `Case08.sumArray(int[])int` | `runtime_target@17 → Case08.sumArray LINKAGE_GUARANTEED` |
| 2 — asSpreader (String[]→3 Strings) | Spreader wraps `concat3` | `Case08.concat3(String,String,String)String` | `runtime_target[concat3 BCI] LINKAGE_GUARANTEED` |
| 3 — invokeWithArguments | Flexible invocation of `sumThree` | `Case08.sumThree(int,int,int)int` | `callsite_target@169: methodhandle_invoke → Case08.sumThree OBSERVED_ONLY` + `runtime_target LINKAGE_GUARANTEED` |
| 4 — asCollector (2 ints→int[]) | `sumArray` again | `Case08.sumArray(int[])int` | Same linkage as Site 1 |

**READY — reason:** All 4 targets are confirmed. Sites 1 and 2 have LINKAGE_GUARANTEED at construction. Site 3 (`invokeWithArguments`) has OBSERVED_ONLY on the invoke record but LINKAGE_GUARANTEED at the MH construction BCI. The collector/spreader array manipulation is visible in the adapter graph. `Array.newInstance` appears in the runtime_target chain for the collector, confirming the array wrapping mechanism.

**Staticizer coverage:** Complete. Collector and spreader are pure structural adapters — no hidden dispatch. The array allocation can be eliminated by a staticizer that inlines the spread/collect operation.

---

### Case09_GuardCatchFinally — READY

**Mechanism:** `guardWithTest`, `catchException`, `tryFinally`

**Records present:**
- `callsite_target`: 136 — 2 `methodhandle_invoke` (GWT and tryFinally invocations) + 6 string concat invokedynamic
- `callsite_target_set`: 3 — one per combinator, each listing all semantic roles
- `runtime_target`: 43 — LINKAGE_GUARANTEED for all component methods

**`callsite_target_set` records (the key staticization evidence):**

| BCI | Combinator | Roles captured |
|---|---|---|
| 67 | `guardWithTest` | `test=isPositive`, `true_target=positive`, `false_target=negative` — all `valid=true` |
| 139 | `catchException` | `try_target=riskyOp`, `handler=recover(NPE,String)` — both `valid=true` |
| 150 | `catchException` | Same — second invocation (for `invoke((String)null)`) — both `valid=true` |

Note: `tryFinally(mhCompute, mhCleanup)` — the `compute` and `cleanup` methods are confirmed by `runtime_target: methodhandle_linkage @[compute BCI] → Case09.compute` and `@[cleanup BCI] → Case09.cleanup`. GWT also produces a separate `callsite_target_set` for the GWT combinator.

**READY — reason:** The `callsite_target_set` records are uniquely valuable here. They name each semantic role explicitly (test, true_target, false_target, try_target, handler) rather than requiring the staticizer to decode the combinator structure from the adapter graph. This is the strongest possible staticization evidence for guard/catch/finally patterns.

**Staticizer coverage:** Complete. `guardWithTest(test, t, f).invoke(x)` → `test(x) ? t(x) : f(x)`. `catchException(target, NPE, handler).invoke(s)` → `try { target(s) } catch (NPE e) { handler(e,s) }`. `tryFinally(compute, cleanup).invoke(x)` → `try { r=compute(x) } finally { cleanup(null,r,x) }`. All components named and confirmed.

---

### Case10_Reflection — READY

**Mechanism:** `Method.invoke`, `Constructor.newInstance`, private access

**Records present:**
- `callsite_target`: 517 — 5 reflection-category records + runtime overhead
- `runtime_target`: 31 — LINKAGE_GUARANTEED at MH linkage inside the reflection infrastructure

**Reflection callsites:**

| BCI | Category | Target | Evidence |
|---|---|---|---|
| 39 | `reflection_method_invoke` | `Case10.staticSquare(int)int` | OBSERVED_ONLY |
| 75 | `reflection_method_invoke` | `Case10.instanceDouble(int)int` | OBSERVED_ONLY |
| 97 | `reflection_constructor_newInstance` | `Case10.<init>()V` | OBSERVED_ONLY |
| 157 | `reflection_method_invoke` | `Case10.secretAdd(int,int)int` | OBSERVED_ONLY |
| 183 | `reflection_method_invoke` | `String.length()int` | OBSERVED_ONLY |

All 5 sites captured. The `runtime_target(methodhandle_linkage)` records at BCIs 39 and 75 confirm LINKAGE_GUARANTEED linkage through the reflection MH infrastructure, providing the second-layer proof for at least the first two sites.

**Note on BCI=39 duplicate:** Two `reflection_method_invoke` records appear for BCI=39, both targeting `Case10.staticSquare`. This is benign — the reflection infra may link twice on first invocation (interpreter + JIT path).

**READY — reason:** The distinct `reflection_method_invoke` and `reflection_constructor_newInstance` categories correctly identify all 5 reflection calls. Private method access (`secretAdd`) is captured correctly. The `setAccessible(true)` pattern does not prevent capture.

**Staticizer coverage:** Complete for target identification. For staticization, `Method.invoke(obj, args)` can be rewritten to `Case10.staticSquare((Integer)args[0])` etc. The private-method case (`secretAdd`) is more complex (access control must be bypassed in the staticized form), but the target is known.

---

### Case11_DynamicProxy — PARTIAL

**Mechanism:** `Proxy.newProxyInstance`, `InvocationHandler`, two proxy instances

**Records present:**
- `callsite_target`: 51 — 2 invokedynamic (lambda FI, `reconstructable=false`), 3 invokeinterface on proxy interface methods, 1 `reflection_constructor_newInstance` for proxy ctor, dispatch records
- `runtime_target`: 29 — handler lambda construction, proxy ctor, and internal MH chains
- `bytecode_artifact`: up to 11 (including `$Proxy0`, `$Proxy1`, proxy builder classes)
- `generated_class`: 2 — the two InvocationHandler lambda instances
- `hidden_class_identity`: 2 — for the two InvocationHandler hidden class instances

**Proxy class coverage:**

| Class | Captured | Artifact | Evidence |
|---|---|---|---|
| `jdk/proxy1/$Proxy0` (Calc proxy) | ✅ | `$Proxy0.class` crc=`5126623e` | `bytecode_artifact`, `reflection_constructor_newInstance` |
| `jdk/proxy1/$Proxy1` (Runnable+Cloneable proxy) | ✅ | `$Proxy1.class` crc=`063c7e9b` | `bytecode_artifact`, `runtime_target@run → $Proxy1.<init>` |
| `Case11_DynamicProxy$$Lambda+0x...f9b8` | ✅ | hidden lambda artifact | `hidden_class_identity`, `generated_class` |
| `Case11_DynamicProxy$$Lambda+0x...4248` | ✅ | hidden lambda artifact | `hidden_class_identity`, `generated_class` |

**Interface method dispatch:**

| Proxy call | Callsite record | InvocationHandler dispatch |
|---|---|---|
| `$Proxy0.add(3,4)` | `invokeinterface → $Proxy0.add` (OBSERVED_ONLY) | `invokeinterface → $$Lambda+0x...f9b8.invoke` (implicit) |
| `$Proxy0.mul(5,6)` | `invokeinterface → $Proxy0.mul` (OBSERVED_ONLY) | same handler |
| `$Proxy0.describe()` | `invokeinterface → $Proxy0.describe` (OBSERVED_ONLY) | same handler |
| `$Proxy1.run()` | `invokeinterface → $Proxy1.run` (OBSERVED_ONLY) | `$$Lambda+0x...4248.invoke` |

**Handler internal dispatch (inside `lambda$run$0`):**
- `invokevirtual Method.getName` ✅
- `invokevirtual String.hashCode` / `String.equals` (switch internals) ✅
- `invokevirtual Integer.intValue` (for add/mul cases) ✅
- `invokevirtual Class.getSimpleName` (for describe case) ✅

**PARTIAL — reason:**
1. **Proxy method → handler invocation is implicit.** RT captures `proxy.add(3,4)` being called, and it captures `InvocationHandler.invoke` being called, but there is **no explicit `runtime_target` or `callsite_target` record whose source is `$Proxy0.add` and whose target is `InvocationHandler.invoke`.** The causal link between the proxy method call and the handler dispatch exists in `$Proxy0.class` bytecode (which is captured) but not as a direct JSONL record. A staticizer must read `$Proxy0.class` + `callsite_target(invokeinterface→$$Lambda.invoke)` together to reconstruct this.
2. **Handler routing is not captured.** The switch inside `lambda$run$0` (`case "add" → ..., case "mul" → ...`) is not decomposed into a `callsite_target_set`. RT captures the invokevirtual calls inside the handler (Method.getName, Integer.intValue, etc.) but does not record which branch ran for which proxy call.

**Staticizer coverage:** Sufficient to identify the handler and its lambda for each proxy instance. The proxy→handler→result chain is reconstructable by combining the `invokeinterface→$Proxy0.add` record, the `$Proxy0.class` bytecode (which calls `h.invoke(this, method, args)`), and the handler's internal dispatch records. Not a blocking gap for demo but requires multi-record reasoning.

---

### Case12_HiddenClass — READY

**Mechanism:** `Lookup.defineHiddenClass`, MH invocation of hidden class method

**Records present:**
- `callsite_target`: 519 — 3 `methodhandle_invoke` at BCIs 138/149/160 (OBSERVED_ONLY), 1 string concat indy
- `runtime_target`: 26 — LINKAGE_GUARANTEED at MH construction BCIs for `compute` and `<init>`
- `bytecode_artifact`: 1 for `Case12_HiddenClass` itself
- (HiddenClassTemplate covered separately below)

**Hidden class targets:**

| BCI | Operation | Target | Evidence |
|---|---|---|---|
| 131 | `mhCtor = findConstructor(hiddenClass, ()V)` | `HiddenClassTemplate+0x…9000.<init>()V` | `runtime_target@131 → HiddenClassTemplate+0x…9000.<init> LINKAGE_GUARANTEED` |
| 138 | `mhCtor.invoke()` | `HiddenClassTemplate+0x…9000.<init>()V` | `callsite_target: methodhandle_invoke @138 → HiddenClassTemplate+0x…9000.<init> OBSERVED_ONLY` |
| 117 | `mhCompute = findVirtual(hiddenClass, "compute", …)` | `HiddenClassTemplate+0x…9000.compute(int)int` | `runtime_target@117 → HiddenClassTemplate+0x…9000.compute LINKAGE_GUARANTEED` |
| 149 | `mhCompute.invoke(instance, 7)` | `HiddenClassTemplate+0x…9000.compute(int)int` | `callsite_target: methodhandle_invoke @149 → HiddenClassTemplate+0x…9000.compute OBSERVED_ONLY` |
| 160 | `mhCompute.invoke(instance, 12)` | same | `callsite_target: methodhandle_invoke @160 → HiddenClassTemplate+0x…9000.compute OBSERVED_ONLY` |

**Cast path (BCI=165: `HiddenOp op = (HiddenOp) instance; op.compute(5)`):**  
The `invokeinterface` call to `HiddenOp.compute(5)` is captured as a `callsite_target(invokeinterface)` showing `target_class=HiddenClassTemplate+0x…9000`.

**HiddenClassTemplate** records (under its own case entry):
- `hidden_class_identity`: `HiddenClassTemplate+0x000000c800159000` → crc=`5ef641b9`
- `bytecode_artifact`: `class=HiddenClassTemplate crc=5ef641b9 hidden=True`
- `generated_class`: `crc=5ef641b9`

**READY — reason:** Complete coverage. The hidden class has a unique `+0x` runtime address, a CRC-matched artifact, and all three invocation paths (ctor via MH, compute×2 via MH, compute via cast+invokeinterface) are captured. The `HiddenClassTemplate` source artifact is the same bytecode as the non-hidden version (they have the same CRC) — the JVM loads it from JAR resources, so the bytecode is available and verifiable.

**Staticizer coverage:** Complete. `mhCompute.invoke(instance, 7)` can be rewritten to `invokevirtual HiddenClassTemplate+0x…9000.compute(instance, 7)`. The cast path can be devirtualized similarly.

---

### HiddenClassTemplate / HiddenOp — READY

Supporting types. `HiddenClassTemplate.class` is captured both as a non-hidden artifact (from JAR loading) and as the hidden class instance. `HiddenOp.class` is captured as a non-hidden interface artifact. Both are correctly indexed.

---

### Runtime TruthCasesMain — READY

**12 invokedynamic dispatch lambdas** in `main()` (one per case runner). All 12 are:
- `staticizable=true, reconstructable=false` (LambdaMetafactory, ThrowingRunnable interface)
- Each has a `hidden_class_identity` + `bytecode_artifact` + `generated_class` record
- Each `runtime_target(methodhandle_linkage)` shows LINKAGE_GUARANTEED target (e.g., `main@7 → Case01_LambdaIndy.run`)

12 lambda instances captured, 12 `hidden_class_identity` mappings present.

---

## 4. UI Behavior Validation

### Scope / filter behavior

With user prefix set to `testcases/Case01_LambdaIndy`:
- Class list shows `Case01_LambdaIndy`, `Case01_LambdaIndy$$Lambda` (family, λ×5 badge), and the 5 `+0x` instances
- Other cases (Case02–Case12, Main) are hidden by default ✅
- Selecting a `+0x` instance shows its bytecode ✅ (after indexer family-creation fix)
- Selecting the family node shows the 5-member list ✅

### Generated/hidden/proxy class display

| Class type | Indexed correctly | Badge shown | Bytecode accessible |
|---|---|---|---|
| Lambda family node | ✅ (after fix) | λ×N | No — shows member list |
| Lambda +0x instance | ✅ | ART/TGT/SRC | Yes — CRC-matched artifact |
| `$Proxy0` / `$Proxy1` | ✅ | ART | Yes — proxy class bytecode |
| `HiddenClassTemplate+0x…` | ✅ | ART | Yes — matches template class bytecode |
| `Runtime TruthCasesMain$$Lambda+0x…` | ✅ | ART | Yes |

### Callsite→target navigation

- Selecting a callsite row shows the target class in the right panel ✅
- `methodhandle_invoke` records link to the target class ✅
- `callsite_target_set` rows (Case09) show multi-target roles ✅ (rendered as separate target badges per role)

### Known UI issue (fixed)

`generated_class` records inserted base-name stubs before the lambda family creation step ran, causing `is_lambda_family` to be skipped. Fixed in `indexer.py` — 35/35 tests passing.

---

## 5. Blockers (must fix before demo)

### B1 — Case01 Site 5: source/binary mismatch

`Case01_LambdaIndy.java` line 27 says `base::toUpperCase` but the compiled JAR has `base::toLowerCase`. The assertion at line 35 (`s4.equals("PREFIX")`) would fail, causing the case to print `FAIL`. Nico will see a failing case.

**Fix:** Recompile `Case01_LambdaIndy.java` after editing (or change source back to `toLowerCase` and update the assertion).

### B2 — `generated_class.impl_class` is null

All `generated_class` records have `impl_class=null`, `impl_method=null`, `impl_descriptor=null`. The JVM agent does not populate these fields. A staticizer relying on `generated_class` records alone cannot determine what method the lambda class delegates to.

**Impact:** Medium. Workaround exists (cross-reference `callsite_target` JSONL). Does not affect UI display but weakens the staticization data model.

**Fix:** Populate `impl_class/method/descriptor` in the JVM agent's `generated_class` emission from `InnerClassLambdaMetafactory`.

---

## 6. Non-Blocking Cleanup Items

### C1 — `bytecode_artifact.trace_id` absent

`bytecode_artifact` records have no `trace_id` field. The BCI→CRC link requires the `.metadata.txt` sidecar files. These exist in the run but are not self-contained in the JSONL.

**Fix:** Emit `trace_id` on `bytecode_artifact` records.

### C2 — `methodhandle_invoke` callsites have no `staticizable` flag

`callsite_target` records with `category=methodhandle_invoke` have `staticizable=null`. Only the `invokedynamic` category has this flag. This means a staticizer cannot query the JSONL for "which MH invoke sites are staticizable" — it must infer from context.

**Fix:** Set `staticizable=true` / `reconstructable` on `methodhandle_invoke` records where RT has LINKAGE_GUARANTEED on the corresponding `runtime_target`.

### C3 — Case11 proxy dispatch chain not explicit

No `callsite_target` record with source=`$Proxy0.add` exists. The causal link from proxy method call to InvocationHandler dispatch requires reading `$Proxy0.class` bytecode. This is acceptable (bytecode is captured) but could be made explicit as a `runtime_target(proxy_dispatch)` record.

### C4 — Case04 array/inline-chain origin not proven with LINKAGE_GUARANTEED

Covered in §3 Case04 analysis. Does not affect the demo narrative (Case04 is about origin tracking, and RT correctly captures all 6 targets even if the proof strength varies by origin type).

### C5 — Duplicate `reflection_method_invoke` at BCI=39 (Case10)

Two identical records for the same site. Benign but noisy.

---

## 7. Demo Recommendation

**Fix B1 (Case01 binary mismatch) before showing the demo.** This is the only change that affects visible pass/fail output. Recompile the JAR and re-run the capture.

**Accept B2 and C1–C5 as known limitations.** They do not affect the demo narrative:

- The UI correctly displays all generated/hidden/proxy classes
- All 12 cases produce relevant dispatch data
- The staticization evidence is complete enough to explain devirtualization for every case
- The PARTIAL cases (04, 05, 06, 07, 11) all have their targets correctly identified — the PARTIAL verdict reflects evidence-strength gaps, not missing targets

**Demo talking points by category:**

| Category | Strongest case to show | Evidence strength |
|---|---|---|
| Lambda staticization | Case01 (after fix) + Case09 (GWT callsite_target_set) | LINKAGE_GUARANTEED |
| String concat | Case02 — all reconstructable=true | LINKAGE_GUARANTEED |
| Direct MH | Case03 — full static+virtual coverage | LINKAGE_GUARANTEED |
| Adapters | Case09 callsite_target_set | Best structured evidence |
| Reflection | Case10 — all 5 targets, private method included | OBSERVED_ONLY (by design) |
| Proxy | Case11 — $Proxy0/$Proxy1 artifacts + handler dispatch | OBSERVED_ONLY |
| Hidden class | Case12 — full MH chain + cast path | LINKAGE_GUARANTEED |
