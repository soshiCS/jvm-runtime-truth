# Agent Handoff: Runtime Truth — JVM Provenance Graph

**Read this document first. It is designed for a future AI agent or developer with no prior context.**

---

## What This Project Is

This is a custom fork of OpenJDK 21 (`jdk21u`) that instruments the HotSpot JVM interpreter to capture **runtime callsite attribution records** — a complete, evidence-based map of which methods are actually invoked at every dynamic dispatch site in a running Java program.

The long-term goal is **staticization**: converting a dynamic Java workload into a representation that can be analyzed, optimized, or ahead-of-time compiled without requiring dynamic dispatch. The provenance graph is Phase 1: prove that every dispatch site can be attributed to an exact, verifiable runtime target before attempting to eliminate the dynamism.

The project is sometimes referred to as **"Runtime Truth JVM"** because the original motivation was to model how Java programs behave on massively parallel hardware where dynamic dispatch overhead at scale becomes a bottleneck.

---

## Current Status (2026-05-30)

**Phase 1 is complete. Phase 2A is complete. Phase 2B is complete.**

- All 12 synthetic Runtime Truth test cases pass with zero user-code diagnostic records.
- Spring Boot validation passes: exit 0, `=== validation complete ===`, 0 user-code diagnostics.
- Phase 2A (`graph_builder.py`): Runtime Causality Graph MVP — 19/19 unit tests pass; Runtime Truth 11/11 checks; Spring Boot 11/11 checks.
- Phase 2B (`runtime_target` source attribution): 0 orphans in both workloads after vframeStream walk in `methodHandles.cpp`. Runtime Truth 915→0 orphans; Spring Boot 2,905→0 orphans. All `runtime_target` records now carry `source_capture=exact` + source attribution fields.
- The instrumentation is stable: no JVM crashes, no `BootstrapMethodError`, no silent corruption.

---

## Repository Layout

```
jdk21u-export/                         ← THE ONLY AUTHORITATIVE REPOSITORY
  src/hotspot/share/classfile/
    soroushProvenanceGraph.cpp/.hpp    ← Core data structures, export pipeline, g_sg_enabled
    soroushClassfileRewriter.cpp/.hpp  ← Bytecode rewriter (instruments class loading)
    klassFactory.cpp                   ← Hidden class identity hook (post-parser)
  src/hotspot/share/interpreter/
    linkResolver.cpp                   ← Main cold-path capture, MH walk, invokeinterface hook
    interpreterRuntime.cpp/.hpp        ← JRT_ENTRY trampoline for warm-path hook
  src/hotspot/cpu/aarch64/
    templateTable_aarch64.cpp          ← Warm-path invokehandle hook (fires on every dispatch)
  tools/rt-ui/                   ← Local Flask web UI for visualizing export
    app.py                             ← Flask server + run management
    indexer.py                         ← JSONL → in-memory index
    static/                            ← HTML/CSS/JS frontend
  docs/                                ← You are here
```

**CRITICAL:** `/Users/soroushaghajani/custom-jvm/jdk21u` is a separate historical copy and must NEVER be modified. All source edits, builds, and validation happen inside `jdk21u-export` only.

---

## The Two Things You Must Know Before Touching Anything

### 1. linkResolver.cpp lives at `interpreter/`, NOT `classfile/`

In `jdk21u` (the historical copy), linkResolver.cpp is in `classfile/`. In `jdk21u-export` (the active repo), it is at:

```
src/hotspot/share/interpreter/linkResolver.cpp
```

Do not confuse these. The build system for `jdk21u-export` compiles from `interpreter/`. Editing the wrong copy means your changes never get compiled.

### 2. `make hotspot` does NOT update `jdk/lib/server/libjvm.dylib`

It updates `support/modules_libs/java.base/server/libjvm.dylib`. The `java` binary loads from `jdk/lib/server/`. After every build:

```bash
cd /Users/soroushaghajani/custom-jvm/jdk21u-export
make hotspot
cp build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
   build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib
```

---

## How to Run a Complete Validation

### 12-case test suite

```bash
# Build cases (if not already built)
JAVAC=/Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/javac
mkdir -p /tmp/cases-build/classes
$JAVAC -d /tmp/cases-build/classes \
  /tmp/cases-build/src/testcases/*.java

# Run
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/rt_val.jsonl \
  /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  -cp /tmp/cases-build/classes testcases.TestCasesMain
```

**Expected:** `test cases demo complete — 12/12 passed`

### Spring Boot validation

```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/spring_val.jsonl \
  /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  -jar /Users/soroushaghajani/gs-spring-boot/complete/target/spring-boot-complete-0.0.1-SNAPSHOT.jar \
  --spring.main.web-application-type=none
echo "Exit: $?"
```

**Expected:** Exit code 0. Stdout contains `=== validation complete ===` and `app class: Application$$SpringCGLIB$$0`.

**Verify zero user-code diagnostics:**
```bash
python3 -c "
import json
records = [json.loads(l) for l in open('/tmp/spring_val.jsonl') if l.strip()]
d = [r for r in records if r.get('record')=='diagnostic' and r.get('src_class','').startswith('com/example')]
print(f'User diagnostics: {len(d)}')
"
```

Expected: `User diagnostics: 0`

---

## Architecture in Two Sentences

The cold path (`linkResolver.cpp → resolve_handle_call`, `runtime_resolve_interface_method`) captures dispatch targets the first time each call site is resolved. The warm path (`templateTable_aarch64.cpp → TemplateTable::invokehandle`) fires on every `invokehandle` execution, catching sibling BCIs that share a constant pool cache entry and would otherwise be missed.

See [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) for the full picture.

---

## JSONL Record Types

The export produces these record types (in `/tmp/spring_out.jsonl` or the UI run's `runtime_targets.jsonl`):

| Record | What it contains |
|---|---|
| `callsite_target` | One resolved dispatch target for a source BCI. The primary output. |
| `callsite_target_set` | Multi-target callsite (GWT: test + true branch + false branch). |
| `callsite_adapter_graph` | Adapter chain decomposition for an invokedynamic or invokehandle site. |
| `runtime_target` | Reflection/MH linkage target discovered without a user callsite (constructor via Spring, etc.). |
| `bytecode_artifact` | A class's bytecode snapshot (original or rewritten), with CRC and dump path. |
| `hidden_class_identity` | Maps `runtime_name+0x<addr>` → `artifact_crc` for hidden/lambda classes. |
| `diagnostic` | A call site that could not be fully resolved. Always has a `reason` field. |
| `export_summary` | Final counts. `complete: true` means no I/O errors during export. |

---

## What SOROUSH_PROVENANCE_GRAPH=1 Does

This env var is the master switch. Without it, `soroush_graph_enabled()` returns false and every instrumentation hook exits immediately (3 instructions: check, branch, return). The JVM runs at near-normal speed when the flag is off.

`SOROUSH_EXPORT_RUNTIME_TARGETS=/path/to/output.jsonl` sets the export destination. Without this, records are collected in memory but never written to disk.

Both must be set. Setting only `SOROUSH_EXPORT_RUNTIME_TARGETS` produces a single `diagnostic` record: `graph disabled`.

---

## Known Traps for Future Agents

1. **`lr` corruption**: The AArch64 link register is set by `prepare_invoke` to the invoke return-entry address. Any `call_VM` after that (including `blr` inside `call_VM_leaf_base`) overwrites `lr`. The warm-path hook saves/restores `lr` via `stp/ldp` on the native stack. Do not add `call_VM` calls in `TemplateTable::invokehandle` without this save/restore.

2. **`JRT_ENTRY` parameter naming**: The JRT_ENTRY macro uses `current` as the thread variable name internally. The JRT function's first parameter must be named `current`, not `thread`. A mismatch causes assertion failures.

3. **CP cache index byte order**: The bytecode rewriter stores CP cache indices with `Bytes::put_native_u2` (native/little-endian on AArch64). Reading them big-endian gives wrong results. Use `sg_u2at(bcp)` which calls `Bytes::get_native_u2(bcp+1)`.

4. **`pre()`/`post()` are assembler members**: In `templateTable_aarch64.cpp`, use `__ pre(sp, -n)` and `__ post(sp, n)`. The bare form `pre(sp, -n)` compiles in `MacroAssembler` member functions but NOT in template table code.

5. **Hidden class name timing**: `ClassFileParser::mangle_hidden_class_name()` appends `+0x<addr>` INSIDE `create_instance_klass()`. Calling `soroush_graph_hidden_identity()` before that returns completes gives the wrong name (no `+0x` suffix). The hook in `klassFactory.cpp` runs after `create_instance_klass()` returns, which is correct.

6. **Dedup is category-agnostic**: The side tables dedup by `(source_class, source_method, source_descriptor, source_bci)`. Category is excluded. The first record for a given (class, method, bci) wins. This means a warm-path exact record can only be emitted if no cold-path diagnostic was already stored for that BCI.

---

## Recommended Next Tasks (Phase 2)

1. **`DirectMethodHandle$StaticAccessor` adapter shape**: Currently produces `adapter_unknown_shape` diagnostics for `Enhancer.wrapCachedClass` and `MethodHandleObjectFieldAccessorImpl.set`. Adding StaticAccessor modeling would eliminate these.

2. **`runtime_target` source attribution — COMPLETE (Phase 2B)**: All `runtime_target` records now carry `source_capture=exact` + `source_class`/`source_method`/`source_bci`/`source_loader_id` fields recovered from a vframeStream walk in `soroush_trace_membername_resolution()` (`methodHandles.cpp`). Result: Runtime Truth 915 → 0 orphans, Spring Boot 2,905 → 0 orphans. `graph_builder.py` connects attributed records via `ET_CALLSITE_RT_ATTRIBUTED` edges. See [10-phase2b-runtime-target-attribution-design.md](10-phase2b-runtime-target-attribution-design.md) and `06-known-limitations.md` Limitation #13.

3. **Web request path validation**: Run Spring Boot in web mode (`web-application-type=servlet`) for a fixed number of HTTP requests, then trigger a graceful shutdown. This exercises `@RequestMapping` dispatch chains not exercised in Phase 1.

4. **Warm-path hook overhead measurement**: Benchmark the 3-instruction `g_sg_enabled` check in `TemplateTable::invokehandle` at JMH scale to confirm it is negligible when the flag is off.

---

## Cross-References

- [01-project-overview.md](01-project-overview.md) — Goals, vision, staticization model
- [02-phase-history.md](02-phase-history.md) — Every milestone with files changed and lessons learned
- [03-source-ownership-map.md](03-source-ownership-map.md) — Which files own which capability
- [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — Cold path, warm path, MH walk, export
- [05-validation-guide.md](05-validation-guide.md) — Exact commands for all 12 cases + Spring Boot
- [06-known-limitations.md](06-known-limitations.md) — Diagnostics, limitations, Phase 2 scope
- [07-build-workflow-guide.md](07-build-workflow-guide.md) — Build, copy, deploy, common mistakes
