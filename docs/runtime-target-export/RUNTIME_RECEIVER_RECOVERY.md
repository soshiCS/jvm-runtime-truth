# Runtime Receiver Recovery

## The problem

When `resolve_handle_call` fires, the JVM needs to know *which* MethodHandle is
being invoked. This is the MH **receiver** — the oop sitting on the expression
stack as the first argument of `invokevirtual MethodHandle.invoke*`.

The receiver is NOT:
- In the CP-cache entry (that holds the invoker MemberName, not the user's MH)
- In the appendix (same — the appendix is an invoker, not the user MH)
- Directly readable from any parameter to `resolve_handle_call`

It must be found from the interpreter frame state at the moment of resolution.

---

## Step 1 — Symbolic backward analysis

**Function:** `sg_analyze_mh_receiver(Method* m, int invoke_bci, int arg_slots)`
**Location:** `linkResolver.cpp:2228–2613`

The analyzer walks the bytecode of method `m` backward from `invoke_bci`,
maintaining a symbolic model of the expression stack. It is looking for the
instruction that pushed the MH receiver — the topmost argument to the invoke
(above the `arg_slots` arguments to the invoked method).

**Return value:**

```cpp
struct SgRecvResult {
  enum Mode {
    SG_RECV_LOCAL,   // MH is in local variable slot N → local_n is valid
    SG_RECV_FIELD,   // MH is in an object field → cannot recover at runtime
    SG_RECV_UNKNOWN  // analysis failed
  };
  Mode mode;
  int  local_n;    // valid when mode == SG_RECV_LOCAL
  char diag[128];  // human-readable reason when mode != SG_RECV_LOCAL
};
```

**BCI offset correction:** `interpreter_frame_bci()` returns `actual_bci + 6`
because `invokehandle` is 6 bytes (the JVM advances the BCI past the instruction
before calling the resolution function). The analyzer subtracts 6 to get the
actual instruction BCI.

**What the analyzer can handle:**
- `aload_N` / `aload N` — MH is in local variable N (most common)
- `getfield` / `getstatic` — MH is in an object field (classified as RECV_FIELD,
  not recoverable at analysis time)
- Simple stack idioms (dup, swap, checkcast) — tracked through

**What the analyzer cannot handle:**
- MH returned from a method call (e.g., `MethodHandles.insertArguments(...).invokeExact(...)`)
  → `recv_from_method_result_or_field` diagnostic
- Complex control flow before the invoke
- MH from an array element

When the analyzer returns `SG_RECV_LOCAL`, the system attempts to read
`frame.interpreter_frame_local_at(local_n)` to get the actual oop.

---

## Step 2 — Runtime stack fallback (Case A only)

**Function:** `sg_recover_mh_recv_from_java_sp(JavaThread* thread, int arg_slots)`
**Location:** `linkResolver.cpp:3255–3275`
**Guard:** `mh_recv == nullptr && !is_lf_dispatch_case`

When symbolic analysis fails AND the top frame is a user frame (Case A), the
actual MH oop can be read directly from the Java expression stack. The MH
receiver is the oop sitting `arg_slots` slots above the current stack top (since
`arg_slots` arguments are above it on the stack at the point of the invoke).

```cpp
static oop sg_recover_mh_recv_from_java_sp(JavaThread* thread, int arg_slots) {
    intptr_t* sp = thread->last_Java_sp();
    if (sp == nullptr) return nullptr;
    // The MH receiver is above the arg_slots arguments
    intptr_t* slot = sp + arg_slots;
    oop candidate = *(oop*)slot;
    if (!sg_oop_valid(candidate)) return nullptr;
    if (!java_lang_invoke_MethodHandle::is_instance(candidate)) return nullptr;
    return candidate;
}
```

**Why `last_Java_sp()` and not the frame's `last_sp`:** On aarch64 HotSpot, the
frame's `last_sp` field is NULL during a `call_VM` (the resolution path). The
`JavaThread::last_Java_sp()` anchor is the stable stack pointer set before entering
native code from Java, and is valid throughout the call.

**Why this works:** `resolve_handle_call` is called from the template interpreter
as part of the invokehandle slow path. At the time of the call, the Java arguments
are still on the expression stack in the expected layout. The MH receiver was
pushed first, then the method arguments — so after `arg_slots` arguments, the MH
receiver oop is at `sp + arg_slots`.

---

## Step 2a — Case A2: LF dispatch frame local[0]

**Guard:** `mh_recv == nullptr && is_lf_dispatch_case`
**Location:** `linkResolver.cpp:3747–3760`

When the top interpreter frame is a `java/lang/invoke/` LambdaForm dispatch
frame (Case A2), the MH being dispatched is the receiver of the `invoke` call —
which is `local[0]` of the LF dispatch frame by the JVM calling convention.

```cpp
intptr_t* local0 = top.interpreter_frame_local_at(0);
if (local0 != nullptr) {
    oop candidate = *(oop*)local0;
    if (sg_oop_valid(candidate) &&
        java_lang_invoke_MethodHandle::is_instance(candidate)) {
        mh_recv = candidate;
    }
}
```

**Why local[0] is the MH:** In the JVM's calling convention, instance method
invocations receive `this` in local slot 0. For an LF dispatch method invoked via
`invoke`/`invokeExact`, `this` is the MethodHandle being dispatched. The JVM
stores it there before transferring control to the LF's bytecode.

**Why fp-relative reads are safe on aarch64:** `interpreter_frame_local_at(N)` is
a frame-pointer-relative calculation: `fp + frame::interpreter_frame_local_offset + N`.
The frame pointer is stable during the resolution call (the LF frame is not being
unwound), so this read is safe.

---

## Case B — reflection accessor target field

When the callsite is inside `jdk/internal/reflect/DirectMethodHandleAccessor`
(Case B), the MH is not on the expression stack in the usual position. Instead it
is stored in the accessor object's `target` field.

**Recovery:**
1. Read `local[0]` of the accessor frame → the accessor `this` oop.
2. Look up the `target` field offset via `FieldDescriptor` resolution.
3. Read `accessor_obj->obj_field(fd.offset())` → get the target MH.
4. Pass the target MH to `sg_walk_mh()` to traverse any adapter chain.

**Complication:** In JDK21, `MethodHandleAccessorFactory.makeSpecializedTarget`
always wraps the DMH in `dropArguments` + `asType` BMH adapters. A raw DMH is
only returned for the degenerate 0-parameter case or when the type already
matches exactly. Therefore `java_lang_invoke_DirectMethodHandle::is_instance(target)`
will fail for the vast majority of reflection invocations.

**Solution:** `sg_walk_mh(target_mh, 6)` traverses whatever BMH adapter chain
exists and locates the first node with a valid `has_target=true` entry pointing
to the underlying DMH.

**Code location:** `linkResolver.cpp:3520–3558`.

---

## `sg_oop_valid` — oop sanity check

**Function:** `sg_oop_valid(oop o)`
**Location:** `linkResolver.cpp:2801–2803`

A minimal oop validity check used before dereferencing any candidate oop
recovered from the stack:

```cpp
static bool sg_oop_valid(oop o) {
    return o != nullptr && oopDesc::is_oop_or_null(o);
}
```

This prevents crashes when the stack slot contains garbage or a non-oop value.
The subsequent `is_instance` checks add type safety on top.

---

## Summary: which recovery path fires for which adapter form

| Adapter form | Analysis result | Recovery path |
|--------------|----------------|---------------|
| `mh.invokeExact(a, b)` | SG_RECV_LOCAL (aload_N) | Step 1 succeeds: local N |
| `MethodHandles.insertArguments(mhAdd,0,100).invokeExact(5)` | SG_RECV_UNKNOWN (method result) | Step 2 (Case A): last_Java_sp |
| `MethodHandles.filterArguments(mhAdd,0,mhNeg).invokeExact(x,y)` | SG_RECV_UNKNOWN | Step 2 (Case A): last_Java_sp |
| `foldArguments(...)` (invoked via LF) | N/A — Case A2 | Step 2a: LF local[0] |
| `method.invoke(null)` | N/A — Case B | accessor.target field + sg_walk_mh |
| `mhField.invokeExact(...)` (field-stored MH) | SG_RECV_FIELD | Diagnostic: recv_from_method_result_or_field |
