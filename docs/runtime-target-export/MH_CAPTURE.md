# MethodHandle Capture

## What gets captured

Every `MethodHandle.invoke*` callsite that fires `resolve_handle_call` is captured.
This covers all variants:

- `mh.invokeExact(args...)` — rewritten by HotSpot to `invokehandle`
- `mh.invoke(args...)` — same rewrite
- `mh.invokeBasic(args...)` — internal form, fires from within `java/lang/invoke/`
- Any adapter built from the above (asType, filterArguments, guardWithTest, etc.)

One record is emitted per callsite (one per BCI per method), not per invocation.
The record is captured at the first execution of the callsite (CP-cache resolution).

---

## Hook: `resolve_handle_call`

**File:** `src/hotspot/share/interpreter/linkResolver.cpp` (export copy)
**Build file:** `src/hotspot/share/classfile/linkResolver.cpp`

This is HotSpot's CP-cache resolution function for `invokehandle` bytecodes. It
fires exactly once per unique callsite (one per CP-cache entry per method). The
system performs all capture work inside this function.

For a complete explanation of the three-case framework:
- [CALLSITE_ATTRIBUTION.md](CALLSITE_ATTRIBUTION.md) — how source BCI and MH receiver are found
- [RUNTIME_RECEIVER_RECOVERY.md](RUNTIME_RECEIVER_RECOVERY.md) — live stack recovery details
- [ADAPTER_GRAPHS.md](ADAPTER_GRAPHS.md) — BMH structure extraction

---

## Direct DMH invoke — `callsite_target`

**Condition:** The MH receiver at resolution time is a `DirectMethodHandle`.

A DMH wraps exactly one Java method. The method is identified via the DMH's
`MemberName.vmtarget` field (a resolved `Method*`).

**What's emitted:**
```json
{
  "record": "callsite_target",
  "category": "methodhandle_invokeExact",
  "evidence": "OBSERVED_ONLY",
  "source_class": "...", "source_method": "...", "source_bci": 37,
  "target_class": "...", "target_method": "add", "target_descriptor": "(II)I",
  "target_loader_id": "0x..."
}
```

**Example:**
```java
MethodHandle mhAdd = lookup.findStatic(Foo.class, "add",
    MethodType.methodType(int.class, int.class, int.class));
int r = (int) mhAdd.invokeExact(10, 20);  // → callsite_target, target=add
```

---

## Adapter chains — `callsite_adapter_graph`

**Condition:** The MH receiver is a generic `BoundMethodHandle` (not a GWT/GWC BMH).

BMH adapters are produced by `MethodHandles.asType`, `filterArguments`,
`filterReturnValue`, `insertArguments`, `foldArguments`, `tryFinally`, `asCollector`,
and others. The adapter's species class name encodes how many bound values it holds.

**What's emitted:**
```json
{
  "record": "callsite_adapter_graph",
  "adapter_kind": "dual_target",
  "adapter_class": "BoundMethodHandle$Species_LL",
  "all_exact": true,
  "nodes": [
    {"id":0, "role":"primary_target", "exact":true, "method":"add", ...},
    {"id":1, "role":"secondary_component", "exact":true, "method":"negate", ...}
  ]
}
```

For the full extraction algorithm and `adapter_kind` semantics, see
[ADAPTER_GRAPHS.md](ADAPTER_GRAPHS.md).

---

## GuardWithTest / CatchException — `callsite_target_set`

**Condition:** The BMH's LambdaForm has `lf_kind == GUARD` or `GUARD_WITH_CATCH`.

These combinators bind multiple target methods (test + branches for GWT; try +
handler for GWC). Each target is extracted individually.

**What's emitted:**
```json
{
  "record": "callsite_target_set",
  "adapter_shape": "GWT",
  "targets": [
    {"role": "test",         "valid": true, "method": "isPositive", ...},
    {"role": "true_target",  "valid": true, "method": "negate",     ...},
    {"role": "false_target", "valid": true, "method": "fallback",   ...}
  ]
}
```

---

## `invokehandle` BCI — the +6 offset

HotSpot rewrites `invokevirtual MethodHandle.invoke*` to the 6-byte `invokehandle`
opcode. `interpreter_frame_bci()` returns the BCI AFTER the instruction
(the PC has already advanced by 6).

The symbolic analysis corrects for this by subtracting 6 before backward analysis.
The exported `source_bci` is the **actual** BCI of the invoke instruction.

The Step 2 runtime stack fallback is unaffected by the BCI value.

---

## `arg_slots`

The number of Java argument slots to the MH-invoked method (used to locate the
MH receiver on the expression stack in Step 2). Computed from the CP-cache entry's
method type. Long/double arguments occupy 2 slots each.

For `add(int, int)`: `arg_slots = 2`
For `concat(String, Object)`: `arg_slots = 2`
For `reduce(long, long)`: `arg_slots = 4`

---

## Record dedup

All three MH callsite record types dedup on
`(source_class, source_method, source_descriptor, source_bci)`.

`category` is excluded from the key. One record per BCI regardless of which invoke
variant triggers the first resolution.

**Prefer-exact upgrade:** If a diagnostic arrives first for a BCI (e.g., because
the first-ever resolution was a JVM-internal warmup call), and a later resolution
from user code has the live MH receiver, the stored record is upgraded in place.
