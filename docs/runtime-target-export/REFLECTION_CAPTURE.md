# Reflection Capture

## What gets captured

- `java.lang.reflect.Method.invoke(Object obj, Object... args)` — resolves to the
  actual target method
- `java.lang.reflect.Constructor.newInstance(Object... args)` — resolves to the
  actual constructor

Both are captured with exact class, method, descriptor, and loader for the
resolved target.

---

## Two capture mechanisms

### Mechanism 1 — MemberName.resolve linkage hook

**File:** `src/hotspot/share/prims/methodHandles.cpp`
**Function:** `soroush_trace_membername_resolution` (lines 95–121)

This function is called whenever `MemberName.resolve` completes — the JDK's
internal resolution for reflection invocations. It fires at linkage time, before
any invocation. This provides `LINKAGE_GUARANTEED` evidence.

**Call sites:**
- Line 260: `java.lang.reflect.Method` resolution path
- Line 271: `java.lang.reflect.Constructor` resolution path
- Lines 849, 873: `MemberName.resolve` (broader resolution path)

The function calls `soroush_graph_linkage` (or equivalent) to record the
`runtime_target` record with `evidence="LINKAGE_GUARANTEED"`.

**What's captured:** The resolved `Method*` from the `MemberName.vmtarget` field —
exact class name, method name, descriptor, and loader.

---

### Mechanism 2 — Case B in `resolve_handle_call`

**File:** `src/hotspot/share/interpreter/linkResolver.cpp` (export copy)
**Lines:** 3520–3558

When `Method.invoke` is called, the JDK reflection path eventually invokes the
internal `DirectMethodHandleAccessor.invoke`, which triggers `resolve_handle_call`
from inside `jdk/internal/reflect/DirectMethodHandleAccessor`. The system detects
this as Case B (top frame class has `jdk/internal/reflect/` prefix and the frame's
method is an accessor).

This mechanism captures the same target as Mechanism 1, but at invocation time
rather than linkage time. It produces a `callsite_target` record with
`category="reflection_method_invoke"` or `category="reflection_constructor_newInstance"`.

---

## The JDK21 BMH adapter complication

**Problem statement:**

In JDK21, `jdk/internal/reflect/MethodHandleAccessorFactory.makeSpecializedTarget`
creates the internal MethodHandle for each reflected method. The factory always
wraps the core `DirectMethodHandle` in additional adapter MHs:

```
For a static method (say Foo.hello()):
  core DMH: DirectMethodHandle for Foo.hello
  wrapped in: dropArguments(core, 0, Object.class)   [add null receiver slot]
  wrapped in: asType(...) [type erasure to (Object,Object[])Object]
  result: BoundMethodHandle$Species_L or similar
```

For a method with parameters, the factory additionally adds `asSpreader` to unpack
the `Object[]` varargs.

The old code to extract the reflection target:

```cpp
// BROKEN for JDK21:
if (java_lang_invoke_DirectMethodHandle::is_instance(target_mh)) {
    // Only works when target is a raw DMH — NOT true for JDK21
    oop mn = java_lang_invoke_DirectMethodHandle::member(target_mh);
    Method* tm = (Method*)java_lang_invoke_MemberName::vmtarget(mn);
    ...
}
```

This check fails for every reflected method that has parameters or is static,
because the `target` field of the accessor is a BMH (an adapter), not a DMH.
The fallback was a `reflection_target_adapter_mh_deferred` diagnostic.

**The fix:**

Use `sg_walk_mh(target_mh, 6)` to traverse the adapter chain. The walk returns
either `DIRECT` (if the target is already a DMH — rare in JDK21) or
`ADAPTER_GRAPH` (the common case), and in both cases the first node with
`has_target=true` points to the underlying DMH for the reflected method.

```cpp
// CORRECT for JDK21:
SgMhWalkResult walk = sg_walk_mh(target_mh, 6);
if (walk.shape == SgMhWalkResult::DIRECT && walk.n_targets > 0) {
    // Fast path (0-param methods, or already-adapted trivial cases)
    tgt_class  = walk.targets[0].klass;
    tgt_method = walk.targets[0].method;
    tgt_desc   = walk.targets[0].descriptor;
    tgt_loader = walk.targets[0].loader_id;
    tgt_ok = true;
} else if (walk.shape == SgMhWalkResult::ADAPTER_GRAPH) {
    // Adapter path — find first DMH node
    for (int ni = 0; ni < walk.n_graph_nodes && !tgt_ok; ni++) {
        const SgAdapterNode& gn = walk.graph_nodes[ni];
        if (gn.has_target && gn.target.valid) {
            tgt_class  = gn.target.klass;
            ...
            tgt_ok = true;
        }
    }
}
```

This eliminated all `reflection_target_adapter_mh_deferred` diagnostics.

---

## Dynamic proxy capture

Dynamic proxies (`java.lang.reflect.Proxy.newProxyInstance`) are captured via the
generated-class recovery path (see [GENERATED_CLASSES.md](GENERATED_CLASSES.md)),
not the reflection capture path. The proxy class itself is emitted as a
`generated_class` record with `generated_by="ProxyGenerator"`. The invocation
handler call inside the proxy dispatches through `java/lang/reflect/Method.invoke`
which is then captured by the reflection capture path described above.

---

## Exact sources for reflection targets

The accessor's `target` field chain always leads to a `DirectMethodHandle` whose
`MemberName.vmtarget` is a resolved `Method*`. All fields are read from this
`Method*`:

| Field | Source |
|-------|--------|
| `target_class` | `Method*→method_holder()→name()→as_C_string()` |
| `target_method` | `Method*→name()→as_C_string()` |
| `target_descriptor` | `Method*→signature()→as_C_string()` |
| `target_loader_id` | `Method*→method_holder()→class_loader_data()` as pointer |

This is the same source used for all DMH target extraction in the system — there
is no special-casing or alternative path for reflection.

---

## Output records

Both mechanisms can produce records. They are complementary:

| Mechanism | Record type | Evidence | When fires |
|-----------|-------------|----------|------------|
| MemberName.resolve hook | `runtime_target` | `LINKAGE_GUARANTEED` | At class load / first access |
| Case B in resolve_handle_call | `callsite_target` | `OBSERVED_ONLY` | At first invocation |

The `callsite_target` record from Case B is more useful for staticization because
it includes the exact source BCI and the loader of the calling class.

---

## Known limitations

- The `target` field read requires the accessor object to be initialized before
  `resolve_handle_call` fires. JDK21's lazy initialization means the target field
  may be null on the very first call if the internal specialization hasn't
  completed. In that case, a diagnostic is emitted and the prefer-exact upgrade
  mechanism handles it when the next resolution fires.
- `MethodHandleAccessorFactory.makeSpecializedTarget` itself involves MH invocations
  that fire `resolve_handle_call` from inside `jdk/internal/reflect/`. These
  produce 4 JDK-internal diagnostics (`recv_from_method_result_or_field`) — they
  are expected and do not affect user-visible reflection target capture.
