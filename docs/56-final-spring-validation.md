# Final Spring Boot Validation

**Date:** 2026-06-02  
**Status:** PASS — all 9 validation checks pass, all attribution gaps resolved  
**Run output:** `/tmp/rt56/`

---

## Commands Run

```bash
# Start Spring Boot under custom JVM with full capture enabled
JAVA="<repo>/build/macosx-aarch64-server-fastdebug/jdk/bin/java"
JAR="tools/demo-runtime-truth/target/demo-runtime-truth-1.0.0.jar"

export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_RUNTIME_GRAPH=1
export SOROUSH_RUNTIME_RECOVERY=1
export SOROUSH_TRACE_INDY=1
export SOROUSH_TRACE_REFLECTION=1
export SOROUSH_CAPTURE_FINAL_BYTECODE=1
export SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1
export SOROUSH_BYTECODE_DUMP_DIR=/tmp/rt56/artifacts
export SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/rt56/runtime_targets.jsonl
export SOROUSH_REWRITER_PHASE5_PREFIX="com/example/truth"
export SOROUSH_USER_PREFIXES="com/example/truth"

$JAVA -Xverify:all -Xint -Ddemo.instance.token=4 -jar $JAR \
  --server.port=18082 --logging.level.root=WARN
```

---

## Requests Executed

| Request | sessionKey | Result |
|---|---|---|
| R1 | `550e8400-e29b-41d4-a716-446655440000` | `DECRYPTION_FAILED` (wrong key for token=4) |
| R2 | `aaaabbbb-cccc-dddd-eeee-ffffffffffff` | `OK` — `RUNTIME SHOWS THE TRUTH` |
| R3 | `11111111-2222-3333-4444-555555555555` | `OK` — `RUNTIME SHOWS THE TRUTH` |

All three requests cover the full dispatch path: Spring controller → pipeline → proxy → ByteBuddy-compiled transform.

---

## Output Locations

| File | Description |
|---|---|
| `/tmp/rt56/runtime_targets.jsonl` | 17,738 JSONL records |
| `/tmp/rt56/artifacts/` | 1,627 hidden class + 17 truth-package `.class` files |
| `/tmp/rt56/stdout.txt` | Application output |
| `/tmp/rt56/stderr.txt` | JVM diagnostic trace |

---

## Validation Result

### Built-in 9-point validation: PASS

| Check | Result |
|---|---|
| JSONL file exists | PASS |
| JSONL parses without errors | PASS (0 bad lines) |
| JSONL has records | PASS (17,738 records) |
| export_summary present | PASS |
| export_summary.complete = true | PASS |
| all artifact_path files exist on disk | PASS |
| no '?' in any descriptor | PASS |
| no loader_id=0 on user_target nodes | PASS |
| no exact=false on user_target nodes | PASS |

### Record type breakdown

| Record type | Count |
|---|---|
| bytecode_artifact | 8,862 |
| runtime_target | 3,426 |
| generated_class | 1,681 |
| hidden_class_identity | 1,627 |
| callsite_target | 1,348 |
| callsite_adapter_graph | 695 |
| diagnostic | 55 |
| method_identity | 43 |
| export_summary | 1 |

### callsite_target by category

| Category | Count |
|---|---|
| invokedynamic | 1,246 |
| invokevirtual | 42 |
| methodhandle_invoke | 31 |
| reflection_method_invoke | 14 |
| invokeinterface | 7 |
| methodhandle_invokeExact | 5 |
| reflection_constructor_newInstance | 3 |

---

## Attribution Checks

### reflection Method.invoke attribution (Gap 1)

**PASS** — `callsite_target(reflection_method_invoke)` from `DecryptPipeline.execute`:

```json
{
  "record": "callsite_target",
  "category": "reflection_method_invoke",
  "evidence": "OBSERVED_ONLY",
  "source_class": "com/example/truth/DecryptPipeline",
  "source_method": "execute",
  "source_bci": 22,
  "source_opcode": "invokevirtual",
  "target_class": "com/example/truth/proxy/PipelineExecutor",
  "target_method": "execute"
}
```

### invokeinterface attribution (Gap 2)

**PASS** — `callsite_target(invokeinterface)` from `PipelineProxy.dispatch`:

```json
{
  "record": "callsite_target",
  "category": "invokeinterface",
  "evidence": "OBSERVED_ONLY",
  "source_class": "com/example/truth/proxy/PipelineProxy",
  "source_method": "dispatch",
  "source_bci": 25,
  "source_opcode": "invokeinterface",
  "target_class": "com/example/truth/handler/TransformBase",
  "target_method": "applyTransform"
}
```

### invokevirtual attribution

**PASS** — 42 `invokevirtual` records, including all truth-package callsites:
`DecryptController → DecryptPipeline::execute`, `PipelineProxy → TransformCompiler::compile`, etc.

### invokedynamic / LambdaMetafactory records

**PASS** — 8 `invokedynamic` records from truth-package, all with `lmf_impl_method` set and stable `trace_id` for artifact lookup.

### MethodHandle adapter graph records

**PASS** — 3 `callsite_adapter_graph` records from truth-package (ByteBuddy-generated transform compilation).

### Generated class artifacts

**PASS** — 1,681 `generated_class` records; all hidden lambda classes stored with CRC-keyed identity; no `_crc` pseudo-names in class sidebar (0 phantom entries).

### Hidden class artifacts

**PASS** — 1,627 `hidden_class_identity` records; all `+0x` entries have `has_artifacts=True`; `find_best_artifact()` resolves correctly via CRC link.

### Proxy records

**PASS** — 98 JDK dynamic proxy classes detected (`$Proxy*`); Spring's `jdk/proxy1/$Proxy0` (the `PipelineExecutor` proxy) is present.

### Bytecode artifact lookup

**PASS** — 17 `com/example/truth` classes have artifacts on disk; `DecryptPipeline`, `DecryptController`, `TruthApplication` all have `artifact_exists=True`.

### UI/indexer visibility

**PASS** — `_crc` pseudo-class entries: **0** (down from 530 before the `generated_class` hidden-lambda fix).

---

## Runtime Truth 15-Case Regression Check

All 15 test cases still pass with the same binary and indexer:

```
PASS Case01 — Lambda / invokedynamic
PASS Case02 — String concat indy
PASS Case03 — Direct MethodHandle
PASS Case04 — MH receiver origins
PASS Case05 — asType adapters
PASS Case06 — Argument adapters
PASS Case07 — Filter / fold
PASS Case08 — Spread / collector
PASS Case09 — Guard / catch / finally
PASS Case10 — Reflection
PASS Case11 — Dynamic proxy
PASS Case12 — Hidden class
PASS Case13 — invokevirtual mono
PASS Case14 — invokevirtual poly
PASS Case15 — invokeinterface poly

test cases demo complete — 15/15 passed
```

---

## Test Suite

41/41 indexer + graph builder unit tests pass:

```
41 passed in 0.03s
```

---

## Known Non-Blocking Limitations

1. **Proxy handler linkage** — JDK dynamic proxy `$Proxy*` classes show `is_proxy_class=True` but `proxy_handler=None` in the Spring Boot run. Handler linkage requires the handler lambda's `<init>` to appear in the same source method immediately before the proxy `<init>`. Spring Boot uses factory methods (e.g., `Proxy.newProxyInstance`) that don't follow this pattern. The attribution is still correct at the callsite level — this only affects the reverse link from proxy class to its handler.

2. **Interpreter-only (`-Xint`)** — capture requires `-Xint` to ensure all callsites pass through the interpreter resolution path. JIT-compiled methods after warm-up are not captured. For production profiling with warm JIT, a follow-up phase is needed.

3. **CP-cache dedup** — each callsite BCI emits at most one `callsite_target` record (first resolution wins). Polymorphic callsites that dispatch to different targets are represented by multiple records only when a different CP-cache entry resolves (different source method × BCI).

4. **macOS/aarch64 only** — the build and two-step `libjvm.dylib` copy workflow is specific to the `macosx-aarch64-server-fastdebug` variant.
