# Project Overview: Runtime Truth — JVM Provenance Graph

See also: [00-agent-handoff.md](00-agent-handoff.md) | [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md)

---

## What the Project Does

This project extends the HotSpot JVM interpreter (OpenJDK 21) with a **callsite attribution system**: for every dynamic dispatch site in a running Java program, it records which method was actually invoked, how the dispatch was routed, and what adapter chain was traversed to get there.

The result is a JSONL file with structured records covering:
- Direct `invokedynamic` / lambda bootstrap targets
- `invokehandle` (MethodHandle) targets and their adapter graphs
- `invokeinterface` targets (JDK proxy, CGLIB, stream pipeline, etc.)
- Reflection targets (`Method.invoke`, `Constructor.newInstance`)
- Hidden class identities (lambda stubs, `LambdaForm$DMH`, etc.)
- Generated class artifacts (CGLIB subclasses, proxy stubs)
- Bytecode snapshots for every loaded class

---

## Long-Term Vision

The long-term goal is **staticization**: given a Java workload, produce a fully static representation where all dynamic dispatch has been replaced by direct calls. This allows:

1. AOT compilation without profiling guesses
2. Whole-program inlining decisions based on observed, not speculated, targets
3. Dead code elimination across lambda/proxy/MH boundaries
4. Correctness-guaranteed class initialization ordering

The provenance graph is the **evidence layer** required to make staticization sound. You cannot safely eliminate `invokevirtual` polymorphism unless you can prove, with evidence from actual runtime, that exactly one target is ever selected at each call site.

---

## Runtime Target Revelation Goals

The system aims to satisfy these guarantees for every user-code call site:

| Property | Meaning |
|---|---|
| **Exact** | The recorded target is the actual method that was dispatched to. Never inferred, never guessed. |
| **Complete** | Every call site that executed is recorded. No silent omissions. |
| **Loader-correct** | The loader identity of source and target is always captured. |
| **Hidden-class-resolved** | Lambda stub and LambdaForm classes are identified by CRC, not by runtime address. |
| **Adapter-decomposed** | For MH chains, the full adapter graph is recorded, not just the final target. |

A call site that cannot satisfy these properties emits a `diagnostic` record with a machine-readable `reason` instead of silently failing. This ensures that gaps are observable and auditable.

---

## The Staticization Pipeline (Planned)

```
Phase 1 (done): Capture
  Running JVM → JSONL provenance export
  Callsite targets, adapter graphs, bytecode artifacts, hidden class identities

Phase 2 (next): Reconstruction
  JSONL → AOT-analyzable graph
  Resolve lambda bodies from CRC-identified bytecode
  Verify adapter graphs are reconstructable at build time

Phase 3: Staticization
  Replace invokedynamic → direct invokestatic where target is exact + reconstructable
  Replace invokehandle → direct call where adapter graph is fully modeled
  Replace invokeinterface (proxy/CGLIB) → direct call where single target observed

Phase 4: Verification
  Rerun original workload with staticized code
  Assert: same observable output, no dynamic dispatch at previously-static sites
```

---

## Relationship Between Record Types

```
bytecode_artifact ──────────────────────────────────────────────────┐
  The raw class bytes + CRC for every loaded class.                  │
  Used to look up source and target bytecode.                        │
  Hidden classes get a runtime_name → CRC mapping via:              │
                                                                      │
hidden_class_identity ──────────────────────────────────────────────┤
  Maps "com/example/App$$Lambda+0x00000003004..." → CRC             │
  Allows indexer to find the bytecode_artifact for a hidden class.   │
                                                                      │
callsite_target ────────────────────────────────────────────────────┤
  SOURCE: (class, method, bci, opcode, cp_index)                    │
  TARGET: (class, method, descriptor, loader)                        │
  category: invokedynamic / invokeinterface / methodhandle_invoke*  │
  source_capture: exact | inferred | diagnostic                      │
  For invokedynamic: also indy_name, bootstrap_method, lmf_*        │
  For invokedynamic: also reconstructable, staticizable              │
                                                                      │
callsite_adapter_graph ─────────────────────────────────────────────┤
  SOURCE: same as callsite_target                                    │
  GRAPH: adapter_class, adapter_kind, nodes[]                        │
  Each node: role, classification, class/method/descriptor, exact    │
  Captures: type_conversion, bound_data, multi_target, guard_with_test│
                                                                      │
callsite_target_set ────────────────────────────────────────────────┤
  For GWT (guard_with_test) sites: test + true_target + false_target  │
  Each sub-target has role + valid flag                               │
                                                                      │
runtime_target ─────────────────────────────────────────────────────┤
  Targets discovered through linkage (MemberName.resolve) or         │
  reflection (Constructor, Method), not from a user callsite.        │
  dispatch_kind: methodhandle_linkage | reflection                    │
                                                                      │
diagnostic ─────────────────────────────────────────────────────────┘
  Emitted when a callsite cannot be attributed exactly.
  reason: recv_from_method_result_or_field | recv_slot_oob |
          backward_goto | unsupported_fast_multiop | adapter_unknown_shape | ...
  Always has src_class, src_method, src_bci, reason.
```

---

## Staticization Model for invokedynamic

An `invokedynamic` callsite is **staticizable** if:
- Its bootstrap method is `LambdaMetafactory.metafactory` or `StringConcatFactory.makeConcatWithConstants`
- Its implementation target (`lmf_impl_class`, `lmf_impl_method`) is a concrete, named method
- `reconstructable=true` (for string concat, this is always true; for lambdas, only if no captured variables prevent reconstruction at build time)

**Not staticizable** examples:
- Lambda captures a mutable object reference (captured state not AOT-resolvable)
- GWT adapter with `inexact_target` (target depends on runtime guard)
- `invokedynamic` for `ObjectMethods.bootstrap` (record pattern matching)

The `staticizable` field on `callsite_target` and `callsite_adapter_graph` records encodes this assessment.
