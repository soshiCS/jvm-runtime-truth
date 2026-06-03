# Class List UX Redesign

**Date:** 2026-06-01  
**Status:** Implemented  
**Goal:** Default to application/user classes only; make the class list scannable and non-noisy.

---

## Problem

The class list showed hundreds of JDK internal and framework classes by default. A Spring Boot run
produces ~3,000 loaded classes; fewer than 30 are user application code. Nico had to scroll past
thousands of rows to find the classes he cared about.

The old "Show all loaded classes" toggle was coarse — it was either everything or a heuristic
dispatch-relevant subset, with no control over which types of noise to reveal.

---

## Changes

### 1. Category-based filter chips

The `panel-subheader` now contains toggle chips instead of a checkbox:

```
[APP 12] [GEN 48] [HID 3] [PROXY 15] [FW 320] [JDK 1200] [Other 8]   [All]
```

Each chip:
- Shows the count of classes in that category
- Is toggled on/off by clicking (active = colored, inactive = gray)
- Empty categories are hidden (no JDK chip if no JDK classes loaded)
- `All` button appears only when not all categories are active

**Default active:** `APP` + `Other`

This means:
- With user prefixes set: only the user's classes appear by default
- Without user prefixes: "Other" catches everything that isn't JDK/framework/generated/proxy — i.e.,
  the classes most likely to be user code

### 2. Class classification logic

`classifyClass(c)` assigns every loaded class to one category:

| Category | Rule |
|---|---|
| `hidden` | Name contains `+0x…` (runtime hidden class instance) |
| `generated` | `c.generated === true` (lambda, CGLIB class, etc.) |
| `app` | Matches a user-supplied prefix |
| `jdk` | Starts with `java/`, `jdk/`, `sun/`, `com/sun/`, `javax/` |
| `framework` | Spring, Hibernate, Netty, SLF4J, Guava, Micrometer, ByteBuddy, etc. |
| `proxy` | Contains CGLIB enhancer / Spring CGLIB / JDK `$Proxy` / HibernateProxy patterns |
| `other` | Everything else |

The order matters — `hidden` and `generated` are checked first so a runtime-generated Spring proxy
is classified as `generated`, not `proxy`.

### 3. Two-line class item layout

Each class row now shows:
```
OrderService       [APP][SRC]
com/example/order
```

- Short class name (last path segment) on the primary line — bold, ellipsis if overflows
- Parent package path on the secondary line — small, muted, ellipsis if overflows
- Full slash-form path as `title` attribute (hover to see)
- No more `word-break: break-all` which made long names wrap mid-word

### 4. Scroll-into-view

When a class is selected (either by click or programmatic selection), the class list scrolls to
keep it visible — `scrollIntoView({ block: 'nearest' })`.

### 5. Diagnostics badge

Class rows now show a `⚠` badge if the class has unresolved callsites (`has_diagnostics`). Previously
this was only visible in the Diagnostics modal.

---

## Files Changed

| File | Change |
|---|---|
| `tools/rt-ui/static/index.html` | Replace checkbox `panel-subheader` with empty `#class-category-bar` div |
| `tools/rt-ui/static/style.css` | Replace `.sidebar-toggle` with chip styles; two-line `.class-item` layout |
| `tools/rt-ui/static/app.js` | Add `classifyClass()`, `renderCategoryBar()`; rewrite `renderClassList()`; remove `showAllClasses` state |

---

## What Was NOT Changed

- Callsite list, target detail, bytecode view — unchanged
- Run modal, validate, output, diagnostics modals — unchanged
- The badge set per class row (APP/SRC/TGT/ART + new DIAG) — same data, cleaned up display
- All existing capabilities preserved

---

## Validation

```
node --check tools/rt-ui/static/app.js  → OK
python3 -m pytest tools/rt-ui/tests/ -q → 26 passed
```
