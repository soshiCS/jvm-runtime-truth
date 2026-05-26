# Export Format (JSONL)

## Overview

The JSONL file is written at VM shutdown by `soroush_graph_export_runtime_targets`
(`soroushProvenanceGraph.cpp:1557–2394`), triggered from `before_exit` in
`runtime/java.cpp:470`.

Each line is one JSON object. The last line is always `export_summary`. Within
a line, fields are written in a defined order. The format is stable.

**Env var:** `SOROUSH_EXPORT_RUNTIME_TARGETS=/path/to/file.jsonl`

---

## Record types

### `method_identity`

Every instrumented method registered in the method-token registry. Written
in Phase 1 (`soroushProvenanceGraph.cpp:1570–1608`). Independent of the
provenance graph — written whenever Phase 5 bytecode instrumentation ran.

| Field | Type | Description |
|-------|------|-------------|
| `record` | string | `"method_identity"` |
| `token` | integer | Stable 32-bit token assigned at rewrite time |
| `class` | string | Internal (slash-form) class name |
| `method` | string | Method name |
| `descriptor` | string | JVM method descriptor (never `?`) |
| `loader_id` | string | `ClassLoaderData*` as `"0x…"` hex |
| `hidden` | boolean | True for hidden/lambda classes |
| `artifact_crc` | string | CRC32 hex of the original (pre-rewrite) classfile |

**Example:**
```json
{"record":"method_identity","token":3,"class":"RuntimeTargetShowcaseDemo","method":"add","descriptor":"(II)I","loader_id":"0x0000000113a81990","hidden":false,"artifact_crc":"7613bd70"}
```

---

### `callsite_target`

A MH callsite resolved to a single exact target. Written in Phase 2 (invokedynamic
form) and Phase 3 (generic MH form).

| Field | Type | Description |
|-------|------|-------------|
| `record` | string | `"callsite_target"` |
| `category` | string | See categories below |
| `evidence` | string | Always `"OBSERVED_ONLY"` |
| `source_class` | string | User callsite class (internal name) |
| `source_loader_id` | string | User callsite class loader |
| `source_capture` | string | `"exact"` |
| `source_method` | string | User callsite method name |
| `source_descriptor` | string | User callsite method descriptor |
| `source_bci` | integer | Exact BCI of the invoke instruction |
| `source_opcode` | string | Bytecode at source_bci (e.g. `"invokehandle"`) |
| `source_cp_index` | integer | Constant pool index |
| `target_class` | string | Resolved target class |
| `target_loader_id` | string | Target class loader |
| `target_method` | string | Target method name |
| `target_descriptor` | string | Target method descriptor |

**Extra fields for invokedynamic:**

| Field | Type | Description |
|-------|------|-------------|
| `trace_id` | integer | Unique indy bootstrap trace ID |
| `indy_name` | string | Interface method name from the indy instruction |
| `indy_descriptor` | string | Interface method descriptor |
| `bootstrap_method` | string | Bootstrap method reference |
| `lmf_impl_class` | string | LambdaMetafactory implementation class |
| `lmf_impl_method` | string | Lambda body method name (e.g. `lambda$main$0`) |
| `lmf_impl_descriptor` | string | Lambda body method descriptor |

**Categories:**
- `"invokedynamic"` — invokedynamic with LambdaMetafactory bootstrap
- `"methodhandle_invokeExact"` — `MethodHandle.invokeExact`
- `"methodhandle_invoke"` — `MethodHandle.invoke`
- `"methodhandle_invokeBasic"` — internal `MethodHandle.invokeBasic`
- `"reflection_constructor_newInstance"` — `Constructor.newInstance`
- `"reflection_method_invoke"` — `Method.invoke`

**Example (direct MH):**
```json
{"record":"callsite_target","category":"methodhandle_invokeExact","evidence":"OBSERVED_ONLY","source_class":"RuntimeTargetShowcaseDemo","source_loader_id":"0x0000000113a81990","source_capture":"exact","source_method":"main","source_descriptor":"([Ljava/lang/String;)V","source_bci":37,"source_opcode":"invokehandle","source_cp_index":12,"target_class":"RuntimeTargetShowcaseDemo","target_loader_id":"0x0000000113a81990","target_method":"add","target_descriptor":"(II)I"}
```

**Example (invokedynamic / lambda):**
```json
{"record":"callsite_target","category":"invokedynamic","evidence":"OBSERVED_ONLY","source_class":"RuntimeTargetShowcaseDemo","source_loader_id":"0x0000000113a81990","source_capture":"exact","source_method":"main","source_descriptor":"([Ljava/lang/String;)V","source_bci":4,"source_opcode":"invokedynamic","source_cp_index":0,"trace_id":1,"indy_name":"get","indy_descriptor":"()Ljava/util/function/Supplier;","bootstrap_method":"java/lang/invoke/LambdaMetafactory.metafactory","lmf_impl_class":"RuntimeTargetShowcaseDemo","lmf_impl_method":"lambda$main$0","lmf_impl_descriptor":"()Ljava/lang/Integer;"}
```

---

### `callsite_target_set`

A MH callsite resolved to a structured set of targets. Emitted for
`guardWithTest` and `catchException` combinators.

| Field | Type | Description |
|-------|------|-------------|
| `record` | string | `"callsite_target_set"` |
| `category` | string | See callsite_target categories |
| `adapter_shape` | string | `"GWT"` or `"GWC"` |
| `adapter_class` | string | BMH species class name |
| `lf_kind` | string | `"GUARD"` or `"GUARD_WITH_CATCH"` |
| `exception_class` | string | (GWC only) caught exception class |
| `source_*` | — | Same source attribution fields as callsite_target |
| `all_exact` | boolean | True iff every target slot was resolved |
| `targets` | array | Target records (see below) |

**Target record fields:**

| Field | Type | Description |
|-------|------|-------------|
| `role` | string | `"test"` / `"true_target"` / `"false_target"` (GWT) or `"try_target"` / `"handler"` (GWC) |
| `valid` | boolean | False if the slot was not extractable |
| `class` | string | Present when valid=true |
| `loader_id` | string | Present when valid=true |
| `method` | string | Present when valid=true |
| `descriptor` | string | Present when valid=true |

**Example (guardWithTest):**
```json
{"record":"callsite_target_set","category":"methodhandle_invoke","adapter_shape":"GWT","adapter_class":"BoundMethodHandle$Species_LLL","lf_kind":"GUARD","source_class":"RuntimeTargetShowcaseDemo","source_method":"main","source_bci":87,"all_exact":true,"targets":[{"role":"test","valid":true,"class":"RuntimeTargetShowcaseDemo","method":"isPositive","descriptor":"(I)Z"},{"role":"true_target","valid":true,"class":"RuntimeTargetShowcaseDemo","method":"negate","descriptor":"(I)I"},{"role":"false_target","valid":true,"class":"RuntimeTargetShowcaseDemo","method":"fallback","descriptor":"(I)I"}]}
```

---

### `callsite_adapter_graph`

A MH callsite whose receiver is a generic BMH adapter. The structure of the
adapter chain is extracted read-only.

| Field | Type | Description |
|-------|------|-------------|
| `record` | string | `"callsite_adapter_graph"` |
| `category` | string | See callsite_target categories |
| `adapter_class` | string | BMH species class name |
| `adapter_kind` | string | `"dual_target"`, `"multi_target"`, `"try_finally"`, `"type_conversion"` |
| `lf_kind` | string | LambdaForm kind string from the BMH |
| `outer_descriptor` | string | Erased type at the outer call boundary (when available) |
| `source_*` | — | Same source attribution fields |
| `all_exact` | boolean | True iff every node has exact=true |
| `nodes` | array | Adapter node records (see below) |

**Node record fields:**

| Field | Type | Description |
|-------|------|-------------|
| `id` | integer | Node index |
| `role` | string | `"primary_target"`, `"adapted_target"`, `"secondary_component"`, `"component_N"`, `"cleanup"` |
| `exact` | boolean | True iff this slot resolved to a concrete DMH |
| `class` | string | Present when exact=true |
| `loader_id` | string | Present when exact=true |
| `method` | string | Present when exact=true |
| `descriptor` | string | Present when exact=true |
| `classification` | string | `"user_target"`, `"internal_jdk"`, `"helper_boxing"`, `"unknown"` |
| `node_adapter_class` | string | BMH species class when exact=false |
| `exact_false_reason` | string | Why exact=false (e.g., `"slot_is_bmh"`, `"depth_limit"`) |
| `from_descriptor` | string | Outer type descriptor (type_conversion nodes) |
| `to_descriptor` | string | Inner type descriptor (type_conversion nodes) |

---

### `runtime_target`

A dynamic target resolved via reflection linkage or execution tracing.
Written in Phase 4 of the export.

| Field | Type | Description |
|-------|------|-------------|
| `record` | string | `"runtime_target"` |
| `evidence` | string | `"LINKAGE_GUARANTEED"` or `"OBSERVED_ONLY"` |
| `dispatch_kind` | string | `"reflection"`, `"methodhandle_linkage"`, `"direct_methodhandle"`, `"execution_trace"` |
| `caller_context` | string | Source label (e.g. `"MemberName.resolve"`) |
| `target_class` | string | Resolved target class |
| `target_loader_id` | string | Target loader |
| `target_method` | string | Target method name |
| `target_descriptor` | string | Target method descriptor |
| `target_hidden` | boolean | True for hidden classes |

Evidence levels:
- `"LINKAGE_GUARANTEED"` — the target was determined at MemberName.resolve time
  (a hard JVM linkage event that always fires exactly once per site)
- `"OBSERVED_ONLY"` — the target was observed at execution time via instrumented
  ENTER/EXIT trace

---

### `generated_class`

A runtime-generated class captured during class loading. Requires
`SOROUSH_RUNTIME_RECOVERY=1`.

| Field | Type | Description |
|-------|------|-------------|
| `record` | string | `"generated_class"` |
| `class` | string | Internal class name |
| `loader_id` | string | Defining class loader |
| `hidden` | boolean | True for hidden classes |
| `crc` | string | CRC32 hex of the class bytes |
| `generated_by` | string | `"LambdaMetafactory"`, `"ProxyGenerator"`, `"ByteBuddy"`, etc. |
| `source_trigger` | string | Fully-qualified trigger class name |
| `provenance_kind` | string | Classification of how the class was generated |
| `indy_trace_id` | integer | Present when created by an invokedynamic site |

---

### `bytecode_artifact`

A snapshot of class bytecode. Written for both pre-rewrite (original) and
post-rewrite (final) versions of every loaded class.

| Field | Type | Description |
|-------|------|-------------|
| `record` | string | `"bytecode_artifact"` |
| `class` | string | Internal class name |
| `loader_id` | string | Defining class loader |
| `crc` | string | CRC32 hex |
| `size` | integer | Byte size |
| `hidden` | boolean | True for hidden classes |
| `kind` | string | `"final"` (post-rewrite) or `"original"` (pre-rewrite) |
| `load_kind` | string | How the class was loaded |
| `rewritten_from_crc` | string | Present for `"final"` artifacts — CRC of the original |

---

### `diagnostic`

Emitted when a record cannot be constructed exactly. Never omitted — explicit
diagnostics are the mechanism by which the exact-or-diagnostic invariant is
maintained.

| Field | Type | Description |
|-------|------|-------------|
| `record` | string | `"diagnostic"` |
| `level` | string | `"info"`, `"warn"`, `"error"` |
| `src_class` | string | Class where the diagnostic originated |
| `src_method` | string | Method where the diagnostic originated |
| `src_descriptor` | string | Method descriptor |
| `src_bci` | integer | BCI of the unresolvable callsite |
| `reason` | string | Machine-readable reason code |
| `message` | string | Human-readable explanation |

**Common reason codes:**

| Reason | Meaning |
|--------|---------|
| `recv_analyzer_recv_from_method_result_or_field` | Symbolic analysis found that the MH is loaded from a method-return value or field — not recoverable by static backward analysis alone |
| `recv_analyzer_recv_slot_oob` | MH receiver slot index is out of bounds for this frame (different stack depth at a sibling BCI) |
| `recv_analyzer_unknown` | Symbolic analysis could not determine the receiver slot |
| `mh_walk_bmh_unknown` | BMH with unrecognized lf_kind — not extractable |
| `sibling_bci_same_local` | Sibling BCI that shares a CP cache entry with a primary site; receiver is the same local variable |
| `sibling_bci_distinct_receiver` | Sibling BCI with a different MH receiver than the primary site |
| `sibling_bci_unanalyzable` | Sibling BCI where receiver analysis failed |

---

### `export_summary`

Always the last record. Contains record counts for all types written.

```json
{"record":"export_summary","callsite_target_count":37,"callsite_target_set_count":2,"callsite_adapter_graph_count":11,"runtime_target_count":143,"generated_class_count":1,"bytecode_artifact_count":12,"diagnostic_count":4,"method_identity_count":28}
```

---

## Dedup behavior

All three callsite table types dedup by `(source_class, source_method, source_descriptor, source_bci)`.
The `category` field is NOT part of the dedup key. One record per BCI, regardless
of which invoke variant triggered the first resolution.

**Prefer-exact upgrade:** If a diagnostic is stored first for a BCI, and a later
resolution produces an exact result (exact MH receiver recovered), the entry is
upgraded in place. The exported record reflects the exact result, not the initial
diagnostic.

---

## No fake precision policy

The exporter enforces the following invariants at write time:
- No `descriptor` field may contain `"?"` — if the descriptor is unknown, the record is suppressed and a diagnostic is emitted instead
- No `user_target` classification node may have `loader_id` of `0` or `null`
- No target is guessed, inferred, or fabricated
- If a site cannot be represented exactly → explicit `diagnostic`, never a best-effort approximation
