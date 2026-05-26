# Adapter Graphs

## The problem with non-DMH MethodHandles

A `DirectMethodHandle` (DMH) wraps a single concrete Java method. Any other
`MethodHandle` is built by composing adapters on top of a DMH. The JVM implements
these adapters as `BoundMethodHandle` (BMH) species — generated classes that bind
argument slots at construction time.

At `resolve_handle_call` time, the MH receiver is often not a DMH but a BMH
species whose species class name encodes how many and what kind of bound values it
holds. Examples:
- `BoundMethodHandle$Species_LL` — two bound object slots
- `BoundMethodHandle$Species_LI` — one bound object + one bound int
- `BoundMethodHandle$Species_LLLL` — four bound object slots (filterArguments with 3 filters)

The fields of a BMH species are generated at JVM startup and have no C++ accessors.
The only way to read them from C++ code is via their field offsets, determined
from the species class name at runtime.

---

## `sg_walk_mh` — top-level MH classifier

**Function:** `sg_walk_mh(oop mh_recv, int depth_limit)`
**Location:** `linkResolver.cpp:3279–3376`

Classifies the MH receiver into one of the shapes in `SgMhWalkResult::Shape`.

**Logic:**
1. If `mh_recv` is a `DirectMethodHandle`: call `sg_extract_dmh_target()` →
   return `DIRECT` with `targets[0]` populated.
2. If `mh_recv` is a `DelegatingMethodHandle` (like `MethodHandles.explicitCastArguments`):
   call `sg_unwrap_delegating()` to peel one layer and recurse.
3. If `mh_recv` is a `BoundMethodHandle`:
   - Read the LF (LambdaForm) from the BMH to determine `lf_kind`.
   - `lf_kind == GUARD` → `GWT` shape, extract 3 targets (test/true/false)
   - `lf_kind == GUARD_WITH_CATCH` → `GWC` shape, extract 2 targets + exception class
   - Otherwise → `ADAPTER_GRAPH`, call `sg_walk_generic_bmh()`
4. If unrecognized: return `MH_UNKNOWN`.

**`sg_extract_dmh_target`** (`linkResolver.cpp:2877–2890`): Reads the
`DirectMethodHandle.member` MemberName, then `MemberName.vmtarget` to get the
resolved `Method*`, and extracts class/method/descriptor/loader.

**`sg_unwrap_delegating`** (`linkResolver.cpp:2895–2901`): Reads
`DelegatingMethodHandle.target` field and returns the inner MH for recursive
classification.

---

## `sg_walk_generic_bmh` — BMH adapter graph extractor

**Function:** `sg_walk_generic_bmh(oop bmh, SgMhWalkResult* out, int depth_limit)`
**Location:** `linkResolver.cpp:3085–3215`

Walks the species fields of a generic (non-GWT, non-GWC) BMH and builds an array
of `SgAdapterNode` entries representing the adapter structure.

**Species field iteration:** The species class name encodes the type of each bound
field: `L` = object, `I` = int, `J` = long, etc. For `Species_LI`, there are two
fields: argL0 (object) and argI1 (int). The walker iterates over all `argL` slots
(object slots only — non-object slots are not MethodHandles and are ignored).

For each `argL` slot:
1. Read the object field value from the BMH.
2. If it's a `MethodHandle`, call `sg_walk_mh()` recursively (bounded by depth_limit).
3. If the recursive result is `DIRECT`: create a node with `has_target=true`,
   `exact=true`.
4. If the recursive result is `ADAPTER_GRAPH` or other non-DIRECT: create a node
   with `has_target=false`, `exact=false`, `node_adapter_class=<species class name>`.

**Node role assignment:** Based on the total number of MH slots and the adapter
structure, nodes receive roles:
- `"primary_target"` — the first (and often only) argL0 MH slot in a type_conversion adapter
- `"adapted_target"` — the underlying method in a `dual_target` structure
- `"secondary_component"` — the second MH slot in `dual_target`
- `"component_N"` — additional slots in `multi_target` (filterArguments with multiple filters)
- `"cleanup"` — cleanup handlers in `try_finally` adapters

**adapter_kind classification:**
- `"type_conversion"` — single argL slot with type descriptor info (asType, insertArguments, bindTo)
- `"dual_target"` — two MH argL slots (filterReturnValue, foldArguments, filterArguments with 1 filter)
- `"multi_target"` — three or more MH argL slots (filterArguments with multiple filters)
- `"try_finally"` — `TRY_FINALLY` lf_kind

---

## GWT / GWC target extraction

For `guardWithTest` BMH (`lf_kind == GUARD`):

```
BMH Species_LLL:
  argL0 = test    MH (e.g., isPositive)
  argL1 = target  MH (e.g., negate)
  argL2 = fallback MH (e.g., fallback)
```

Each slot is read and extracted via `sg_extract_dmh_target()` if it is a DMH.
The three targets get roles `"test"`, `"true_target"`, `"false_target"`.

For `catchException` BMH (`lf_kind == GUARD_WITH_CATCH`):

```
BMH Species_LL:
  argL0 = try MH
  argL1 = handler MH
```

Plus an additional field for the caught exception class. Targets get roles
`"try_target"`, `"handler"`.

**Code location:** `linkResolver.cpp:3380–3450` (GWT/GWC extraction, within `sg_walk_mh`).

---

## Why some nodes are exact=false

The C++ read-only walk can only read *direct* field values. If a BMH species field
holds another BMH (an adapter wrapping an adapter), that inner BMH's contents are
not readable in the same pass without unsafe accessors.

| Adapter form | Not-exact slot | Reason |
|---|---|---|
| `asType(mh, type)` | secondary_component | The unboxing/boxing BMH wrapping the type conversion is itself a BMH; its inner DMH is not extractable |
| `tryFinally(target, cleanup)` | all 4 slots | Each slot is a BMH; inner targets not extractable without recursion into unexposed species fields |
| `asCollector(mh, type, count)` | secondary_component | The array-collector trampoline is an internal BMH with no DMH slot |

These are emitted as `exact=false` nodes with `exact_false_reason` populated.
They are never silently omitted.

The `all_exact` boolean at the record level is `true` only when every node in
`nodes` has `exact=true`. This is a fast indicator for downstream consumers of
whether the full adapter structure was resolved.

---

## adapter_graph examples

### `filterArguments(mhAdd, 0, mhNegate)` — dual_target, all_exact=true

```json
{
  "record": "callsite_adapter_graph",
  "adapter_kind": "dual_target",
  "lf_kind": "GENERIC",
  "all_exact": true,
  "nodes": [
    {
      "id": 0, "role": "primary_target", "exact": true,
      "class": "RuntimeTargetShowcaseDemo", "method": "add",
      "descriptor": "(II)I", "classification": "user_target"
    },
    {
      "id": 1, "role": "secondary_component", "exact": true,
      "class": "RuntimeTargetShowcaseDemo", "method": "negate",
      "descriptor": "(I)I", "classification": "user_target"
    }
  ]
}
```

### `asType(mhNegate, (Integer)I)` — type_conversion, all_exact=false

```json
{
  "record": "callsite_adapter_graph",
  "adapter_kind": "type_conversion",
  "lf_kind": "GENERIC",
  "all_exact": false,
  "nodes": [
    {
      "id": 0, "role": "adapted_target", "exact": true,
      "class": "RuntimeTargetShowcaseDemo", "method": "negate",
      "descriptor": "(I)I", "classification": "user_target"
    },
    {
      "id": 1, "role": "secondary_component", "exact": false,
      "node_adapter_class": "BoundMethodHandle$Species_L",
      "exact_false_reason": "slot_is_bmh",
      "classification": "helper_boxing"
    }
  ]
}
```

### `insertArguments(mhAdd, 0, 100)` — type_conversion, all_exact=true

The bound constant `100` is stored in an `I`-typed species slot, not an `argL`
slot. The `argL0` slot holds the target DMH directly.

```json
{
  "record": "callsite_adapter_graph",
  "adapter_kind": "type_conversion",
  "lf_kind": "GENERIC",
  "all_exact": true,
  "nodes": [
    {
      "id": 0, "role": "adapted_target", "exact": true,
      "class": "RuntimeTargetShowcaseDemo", "method": "add",
      "descriptor": "(II)I", "classification": "user_target"
    }
  ]
}
```

---

## Depth limit

`sg_walk_mh` and `sg_walk_generic_bmh` both accept a `depth_limit` parameter.
The current call passes `depth_limit=6`. Exceeding the depth limit causes the
walk to terminate and the innermost node to be classified as `exact=false` with
`exact_false_reason="depth_limit"`. This prevents infinite loops on pathological
self-referential MH structures (which should not exist in practice, but are
guarded against for safety).
