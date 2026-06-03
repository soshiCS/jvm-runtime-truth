# Runtime Truth UI Redesign

**Date:** 2026-06-01  
**Status:** Implemented (Phase 1)  
**Goal:** Surface the staticization verdict immediately, before raw runtime data.

---

## Problem Statement

The current UI has one critical layout flaw: when Nico clicks a callsite, the first thing he sees is:

```
┌─ Callsite Info ─────────────┐
│ Record:   callsite_target    │
│ Category: invokedynamic      │
│ Source:   MyClass.myMethod   │
│ BCI:      42                 │
│ Opcode:   invokedynamic      │
└──────────────────────────────┘
```

This is all preamble — metadata about the record format, not an answer to "can this callsite be staticized?"  
The `staticizable`, `staticization_blockers`, and `reconstructable` fields exist in the data model  
but were buried in a narrow code path (StringConcatFactory only) and invisible for all other callsite types.

**The primary question is: can this callsite be staticized?**  
The answer should appear first.

---

## Current Layout (Before)

```
┌─ Classes ──────┬─ Methods + Callsites ────────┬─ Detail (Targets / Bytecode) ─┐
│ class list     │ method-group                  │                               │
│ filter         │   BCI  opcode  summary  [CT]  │  Callsite Info                │
│                │   BCI  opcode  summary  [CAG] │    Record: callsite_target    │
│                │                               │    Category: invokedynamic    │
│                │ method-group                  │    Source: MyClass.foo        │
│                │   ...                         │    BCI: 42                    │
│                │                               │    Opcode: invokedynamic      │
│                │                               │                               │
│                │                               │  [then target info below]     │
└────────────────┴──────────────────────────────┴───────────────────────────────┘
```

Verdict only appears for StringConcatFactory (≈5% of callsites). For invokevirtual,
invokeinterface, LambdaMetafactory, and all other callsite types: no verdict is shown at all.

---

## Redesigned Layout (After)

```
┌─ Classes ──────┬─ Methods + Callsites ────────┬─ Detail (Targets / Bytecode) ─┐
│ class list     │ method-group                  │                               │
│ filter         │  ● BCI  opcode  summary  [CT] │ ╔══════════════════════════╗  │
│                │  ✗ BCI  opcode  summary  [CAG]│ ║ ✓ STATICIZABLE           ║  │
│                │  ● BCI  opcode  summary  [CT] │ ║ 1 target · evidence:exact║  │
│                │                               │ ╚══════════════════════════╝  │
│                │ method-group                  │                               │
│                │  ...                          │  Target                       │
│                │                               │    Class:  MyImpl            │
│                │                               │    Method: handleRequest     │
│                │                               │                               │
│                │                               │  ▸ Callsite Details  [+]     │
│                │                               │    (Record/BCI/Opcode/etc.)  │
└────────────────┴──────────────────────────────┴───────────────────────────────┘
```

Key changes:
1. **Verdict banner** at the top of the detail panel — green/orange/gray based on verdict
2. **Verdict dots** in callsite rows — instant scan of all callsites in a method
3. **Callsite metadata** collapsed into a `<details>` toggle — still accessible, no longer the first thing

---

## Verdict Banner Wireframes

### STATICIZABLE
```
┌─────────────────────────────────────────────────────────────┐
│  ✓  STATICIZABLE                                            │
│     1 target · evidence: exact · invokevirtual @ BCI 17    │
└─────────────────────────────────────────────────────────────┘
[green: #f0fdf4 bg, #86efac border, #15803d text]
```

### BLOCKED — explicit blockers
```
┌─────────────────────────────────────────────────────────────┐
│  ✗  BLOCKED                                                 │
│     1 target · invokedynamic @ BCI 42                      │
│     [multiple_observers] [dynamic_binding]                  │
└─────────────────────────────────────────────────────────────┘
[orange: #fff7ed bg, #fdba74 border, #c2410c text]
[blocker chips: #fef3c7 bg, #fcd34d border, #92400e text]
```

### BLOCKED — polymorphic target set
```
┌─────────────────────────────────────────────────────────────┐
│  ✗  BLOCKED                                                 │
│     3 observed targets · invokeinterface @ BCI 88          │
│     [3 dispatch targets — polymorphic]                      │
└─────────────────────────────────────────────────────────────┘
```

### UNKNOWN — diagnostic
```
┌─────────────────────────────────────────────────────────────┐
│  ?  UNKNOWN                                                 │
│     invokedynamic @ BCI 55 · could not resolve target      │
└─────────────────────────────────────────────────────────────┘
[gray: #f8fafc bg, #cbd5e1 border, #64748b text]
```

---

## Callsite Row Dots

Each callsite row gains a small 7×7 colored dot as the first column:

```
● 17  invokevirtual    → ServiceImpl.process        [CT]
✗ 42  invokedynamic   → makeConcatWithConstants     [CT]
● 88  invokeinterface  → HandlerChain.handle         [CTS]
? 55  invokedynamic   ⚠ could not resolve           [DIAG]
```

Colors: green (#22c55e) = staticizable, orange (#f97316) = blocked, gray (#94a3b8) = unknown.

This lets Nico scan a method's callsites instantly and focus on the blocked/unknown ones.

---

## Verdict Computation Rules

| Record | Condition | Verdict |
|---|---|---|
| `callsite_target` | `staticizable=true` | STATICIZABLE |
| `callsite_target` | `staticizable=false` | BLOCKED |
| `callsite_target` | indy, no `staticizable` field | UNKNOWN |
| `callsite_target` | non-indy, `all_exact=true` | STATICIZABLE |
| `callsite_target` | non-indy, `all_exact=false` | BLOCKED |
| `callsite_target_set` | 1 target | STATICIZABLE |
| `callsite_target_set` | N>1 targets | BLOCKED |
| `callsite_adapter_graph` | `staticizable=true` | STATICIZABLE |
| `callsite_adapter_graph` | `staticizable=false` | BLOCKED |
| `callsite_adapter_graph` | no `staticizable`, `all_exact=true` | STATICIZABLE |
| `callsite_adapter_graph` | no `staticizable`, `all_exact=false` | UNKNOWN |
| `diagnostic` | any | UNKNOWN |

Evidence text shown in banner:
- For `callsite_target`: `cs.evidence` (e.g. "exact", "profile", "devirt")
- For `callsite_target_set`: "N observed targets"
- For `callsite_adapter_graph`: adapter_kind + node count
- For `diagnostic`: reason string

---

## Graph Ideas (Future / Phase 2)

The user asked about small focused graphs. The adapter graph data already contains `nodes[]` and
`edges[]` arrays. A small linear SVG showing the MH chain (root → adapter → user_target) would be
more visually clear than the current flat node card list.

**Proposed Phase 2: Adapter Chain SVG**

```
[call] ──→ [helper_invoker] ──→ [helper_adapter] ──→ [user_target: MyClass.myMethod]
                                       ↓
                               [bound_data: String]
```

Each box would be color-coded by classification (green for user_target, blue for internal_jdk, etc.)
Clicking a box opens the node detail panel.

Implementation note: edges data is already available as `cs.edges[]` with `from_node_id`,
`to_node_id`, `label`. A ~100-line inline SVG renderer would be sufficient — no library needed for
linear/DAG graphs of ≤20 nodes.

**Other small-graph ideas:**
- `callsite → target` arrow (single dispatch, shows call site + concrete target)
- Reflection chain: `Method.invoke` → proxy class → actual impl
- Proxy chain: CGLIB wrapper → Spring interceptor → user bean

All of these require Phase 2 work. Phase 1 (this implementation) focuses on the verdict banner and
row dots, which provide the highest value per line of change.

---

## What Was NOT Changed

Per the requirement to keep all existing capabilities without a major rewrite:

- The 3-panel layout is unchanged
- All existing tabs (Targets/Bytecode) are unchanged
- The "Callsite Info" KV grid still exists — moved into a `<details>` collapse
- All adapter graph node cards are unchanged
- All bytecode view functionality is unchanged
- The New Run modal, Validate, Output, Diagnostics modals are unchanged
- The export dropdown is unchanged
- The JAR class browser is unchanged

---

## Implementation Files Changed

| File | Change |
|---|---|
| `tools/rt-ui/static/style.css` | Verdict banner CSS + callsite row dot column |
| `tools/rt-ui/static/app.js` | `computeVerdict()`, `renderVerdictBanner()`, modified `renderTargetsView()` and `renderCallsiteRow()` |

No HTML changes required — all modifications are in JS and CSS.
