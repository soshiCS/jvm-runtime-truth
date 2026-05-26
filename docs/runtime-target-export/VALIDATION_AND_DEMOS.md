# Validation and Demos

## No fake precision policy

The system enforces at export time:
- No `descriptor` field may contain `"?"`
- No `user_target`-classified node may have `loader_id=0` or null
- No target is guessed, fabricated, or confidence-scored
- Any unresolvable site produces an explicit `diagnostic` record, never a silent omission

These are checked by the validation script in `run_showcase.sh`:

```python
for r in records:
    for field in ("descriptor", "source_descriptor", "lmf_impl_descriptor"):
        if "?" in (r.get(field) or ""):
            # FAIL
    for n in r.get("nodes", []):
        if n.get("classification") == "user_target" and n.get("loader_id") in NULL_LOADERS:
            # FAIL
```

---

## RuntimeTargetShowcaseDemo (11/11 PASS)

**File:** `Downloads/bugged/jvm-dump-demo/RuntimeTargetShowcaseDemo.java`
**Runner:** `Downloads/bugged/jvm-dump-demo/run_showcase.sh`

The primary validation harness. Covers one clean example per capture category.

### Showcase sections

| # | Description | Expected record | Result |
|---|-------------|----------------|--------|
| 1 | `invokedynamic` / `LambdaMetafactory` | `callsite_target` (invokedynamic, lmf_impl=lambda$main$0) | PASS |
| 2 | Direct `MethodHandle.invokeExact` | `callsite_target` (methodhandle_invoke, target=add) | PASS |
| 3 | `asType` adapter (boxing/unboxing) | `callsite_adapter_graph` (type_conversion, negate[user_target]+unboxInteger[helper_boxing]) | PASS |
| 4 | `filterArguments` | `callsite_adapter_graph` (dual_target, add[user_target]+negate[user_target], all_exact=true) | PASS |
| 5 | `filterReturnValue` | `callsite_adapter_graph` (dual_target, makeString[user_target]+length[internal_jdk]) | PASS |
| 6 | `guardWithTest` | `callsite_target_set` (GWT: isPositive/negate/fallback) | PASS |
| 7 | `catchException` | `callsite_target_set` (GWC: risky/recover) | PASS |
| 8 | `insertArguments` (partial binding) | `callsite_adapter_graph` (type_conversion, add[user_target], all_exact=true) | PASS |
| 9 | `Constructor.newInstance` | `callsite_target` (reflection_constructor_newInstance, target=<init>) | PASS |
| 10 | `Method.invoke` | `runtime_target` / `callsite_target` (target=hello) | PASS |
| 11 | Dynamic proxy | `generated_class` ($Proxy0, generated_by=ProxyGenerator) | PASS |

### Run command

```bash
cd ~/Downloads/bugged/jvm-dump-demo
bash run_showcase.sh
```

### Validation checks performed

1. JSONL parse validation — all lines must be valid JSON
2. Record type counts
3. No `"?"` in any descriptor field
4. No `loader_id=0` on `user_target` nodes
5. Coverage checklist — each section predicate must match at least one record

### Expected output

```
=== Runtime Target Showcase ===
Compile: OK
Run:     showcase complete: 11/11 sections executed
Export:  /tmp/showcase_run/showcase.jsonl
JSONL:   OK, NNN records
...
Showcase coverage:
  [OK]                   lambda / invokedynamic
  [OK]                   direct MethodHandle
  ...
  11/11 sections covered — all sections covered
Diagnostics:
  demo-related: 0   total: 4
Result: PASS
```

The 4 non-demo diagnostics are the JDK-internal ones listed in
[KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md#4-jdk-internal-diagnostics).

---

## Four adapter demo programs (complete coverage matrix)

These four programs validate all known BMH adapter forms. See
[ARCHITECTURE.md § Coverage matrix](ARCHITECTURE.md) for the full table.

### MHGenericAdapterDemo

Covers 7 adapter sites: asType, filterArguments, filterReturnValue, foldArguments,
tryFinally, asCollector, insertArguments. All 7 attributed to `MHGenericAdapterDemo.main`.
Silent omissions: 0.

### MHAdapterDemo

Covers 4 sites: invokeExact on static DMH, invoke on same static DMH,
invoke on asType BMH, invokeExact on virtual DMH.

### MHCombinatorDemo

Covers guardWithTest, catchException, asType adapter, plain DMH with distinct
signature.

### MHCallsiteDemo

Covers invokeExact and invoke on static and virtual DMH.

---

## Spring Boot validation

**Test case:** gs-spring-boot (official Spring Boot getting-started example),
with `SOROUSH_REWRITER_PHASE5_PREFIX=com/example` (app classes only).

**Result:**
- 0 VerifyErrors
- Spring starts, serves `GET /` successfully
- `callsite_target`, `callsite_target_set`, `callsite_adapter_graph` records emitted
  for all app-level MH callsites

For broader `org/springframework` prefix:
- 0 VerifyErrors
- ~37 callsite_target records for Spring framework MH invocations
- No demo-related diagnostics

---

## Guaranteed exactness properties

When a `callsite_target` record is emitted (not a diagnostic):
- `source_class`, `source_method`, `source_descriptor` identify the exact user method
- `source_bci` is the actual BCI of the `invokehandle` instruction (not off-by-one)
- `source_loader_id` is the `ClassLoaderData*` of the user class — exact, not guessed
- `target_class`, `target_method`, `target_descriptor` come from `Method*→name()/signature()` — always exact
- `target_loader_id` comes from `Method*→method_holder()→class_loader_data()` — always exact

When a `callsite_adapter_graph` record is emitted with `all_exact=true`:
- Every node has a direct DMH as its slot value
- Every node's `class`, `method`, `descriptor`, `loader_id` are exact
- No node was a placeholder or guess

When a node has `exact=false`:
- `exact_false_reason` explains why
- `node_adapter_class` gives the BMH species class name
- The slot's existence and type are correct; only the inner target is unresolved

---

## Validation script API

`run_showcase.sh` accepts `SHOW_RAW_DIAGNOSTICS=1` to print raw diagnostic JSON:

```bash
SHOW_RAW_DIAGNOSTICS=1 bash run_showcase.sh
```

The script writes a filtered record file to `/tmp/showcase_run/showcase_records.txt`
containing the first matched record for each showcase section, formatted with
`json.dumps(indent=2)` for human review.
