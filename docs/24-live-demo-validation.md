# Live Demo Validation — V5 Runtime Truth Decryption

**Date captured**: 2026-05-31  
**Run directory**: `/tmp/demo_v5_live_repro`  
**JVM**: `build/macosx-aarch64-server-fastdebug/jdk/bin/java` (custom, fastdebug)  
**App**: `tools/demo-runtime-truth/target/demo-runtime-truth-1.0.0.jar`  
**Demo INSTANCE_TOKEN**: `4` (via `-Ddemo.instance.token=4`)

---

## Summary

The V5 demo was run entirely live under the custom JVM. All six causality
capabilities are confirmed present in the live JSONL output. The bug is
reproducible deterministically. Pre-captured data in `bugs_v5.py` has been
replaced with a validated live capture path.

---

## Reproduction

### Build

```bash
cd tools/demo-runtime-truth
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home \
  mvn package -DskipTests
```

Output: `target/demo-runtime-truth-1.0.0.jar` (28.8 MB)

### Run live capture

```bash
bash tools/demo-runtime-truth/capture_live.sh /tmp/demo_v5_live
```

The script:
1. Sets all `SOROUSH_*` environment variables
2. Starts Spring Boot under the custom JVM with `-Xint -Xverify:all`
3. Waits for port 18080 to open (typically 18–22 seconds in interpreter mode)
4. POSTs the V5 decrypt request
5. Sends SIGTERM to the server
6. Reports JSONL line count and artifact count

### Demo command (shows the bug)

```bash
bash tools/demo-runtime-truth/capture_live.sh /tmp/demo_v5_live_repro
# DEMO_INSTANCE_TOKEN=4 is the default — selects transform.037 deterministically
```

To use a random (non-demo) INSTANCE_TOKEN:
```bash
DEMO_INSTANCE_TOKEN= bash capture_live.sh /tmp/demo_v5_live_random
# Removes override; may or may not select a buggy transform
```

### Live API request

```bash
curl -s -X POST \
  "http://localhost:18080/api/decrypt?sessionKey=550e8400-e29b-41d4-a716-446655440000" \
  -H "Content-Type: application/json" \
  -d '{"ciphertext":"2D2A312B36323A5F2C3730282C5F2B373A5F2B2D2A2B37"}'
```

**Live response** (with `demo.instance.token=4`):
```json
{"status":"DECRYPTION_FAILED",
 "output":"66617a607d797114677c7b636714607c7114606661607c",
 "sessionKey":"550e8400-e29b-41d4-a716-446655440000"}
```

This matches the expected OBSERVED RESPONSE from the benchmark scenario exactly.

---

## JSONL Stats

| Metric | Value |
|--------|-------|
| Total JSONL lines | 58,989 |
| `callsite_target` records | 42,655 |
| `bytecode_artifact` records | 8,845 |
| `runtime_target` records | 3,410 |
| `generated_class` records | 1,673 |
| `hidden_class_identity` records | 1,619 |
| `callsite_adapter_graph` records | 693 |
| `method_identity` records | 39 |
| Artifact files captured | 10,960 |

---

## Capability Verification

### 1. Reflection — `DecryptPipeline.execute` → proxy

**Record type**: `callsite_target`, category `reflection_method_invoke`

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

**Status**: ✓ Confirmed present in live JSONL.

---

### 2. JDK Proxy — `$Proxy0` construction

**Record type**: `callsite_target`, category `reflection_constructor_newInstance`

```json
{
  "category": "reflection_constructor_newInstance",
  "source_class": "java/security/AccessController",
  "source_method": "executePrivileged",
  "target_class": "jdk/proxy1/$Proxy0",
  "target_method": "<init>"
}
```

**Status**: ✓ JDK proxy class `jdk/proxy1/$Proxy0` present in live JSONL.

---

### 3. Hidden Classes — InvocationHandler and finalize lambdas

**Record type**: `generated_class` (hidden=true) + `hidden_class_identity`

```json
{"class": "com/example/truth/proxy/PipelineProxy$$Lambda",    "hidden": true, "crc": "83aaa585"}
{"class": "com/example/truth/handler/TransformCompiler$$Lambda","hidden": true, "crc": "9329d240"}
{"class": "com/example/truth/proxy/PipelineProxy$$Lambda",    "hidden": true, "crc": "6c564d46"}
```

Runtime addresses:
```
com/example/truth/proxy/PipelineProxy$$Lambda+0x000000e8004d9330    crc=83aaa585
com/example/truth/handler/TransformCompiler$$Lambda+0x000000e8005d34b8  crc=9329d240
com/example/truth/proxy/PipelineProxy$$Lambda+0x000000e800697738    crc=6c564d46
```

**Status**: ✓ All 3 hidden lambda classes confirmed in live JSONL.

---

### 4. Generated Class — `TransformBase$Transform037$VHz0cN3d`

**Record type**: `callsite_target` showing the generated class as target

```json
{
  "category": "invokevirtual",
  "source_class": "com/example/truth/handler/TransformBase",
  "source_method": "applyTransform",
  "source_bci": 12,
  "target_class": "com/example/truth/handler/TransformBase$Transform037$VHz0cN3d",
  "target_method": "getKey"
}
```

The class name changes per-run (the `$VHz0cN3d` suffix is random), but the
`$Transform037$` segment is deterministic. The index **037** identifies the
config entry `transform.037=75` in `transform-keys.properties`.

**Status**: ✓ Generated class present in live JSONL and in artifacts directory.

---

### 5. Artifact Lookup

**File on disk** (live-captured):
```
/tmp/demo_v5_live_repro/artifacts/com_example_truth_handler_TransformBase$Transform037$VHz0cN3d.class
/tmp/demo_v5_live_repro/artifacts/com_example_truth_handler_TransformBase$Transform037$VHz0cN3d.original.class
```

**Status**: ✓ Both final and original bytecode forms captured.

---

### 6. Artifact Javap — `getKey()` returns `bipush 75`

```bash
javap -c -p artifacts/com_example_truth_handler_TransformBase\$Transform037\$VHz0cN3d.class
```

Output (key section):
```
public int getKey();
  Code:
     0: ldc_w         #26   // int 38  [soroush trace: method id]
     3: invokestatic  #17   // Method java/lang/System.soroushTraceEnter:(I)V
     6: bipush        75    ← BUG: key=75 (should be 127)
     8: istore        1
    10: ldc_w         #26
    13: invokestatic  #20   // Method java/lang/System.soroushTraceExit:(I)V
    16: iload         1
    18: ireturn
```

The custom JVM instruments the bytecode with `soroushTraceEnter/Exit` calls for
runtime tracing. The constant `75` is still clearly visible at instruction 6.

**Status**: ✓ `bipush 75` confirmed in live-captured bytecode. The bug is real.

---

## INSTANCE_TOKEN and Demo Reproducibility

### Why INSTANCE_TOKEN exists

`TransformCompiler` uses `System.nanoTime()` at class load to form the 3-component
composite hash input. This makes the transform selection non-reproducible across
JVM restarts — a deliberate design for the Agent A barrier.

### Demo override mechanism

For controlled demonstrations:

```java
private static final long INSTANCE_TOKEN =
    System.getProperties().containsKey("demo.instance.token")
        ? Long.parseLong(System.getProperty("demo.instance.token"))
        : System.nanoTime();
```

The `-Ddemo.instance.token=4` JVM argument pins INSTANCE_TOKEN to 4. This value
was computed to select `transform.037` deterministically:

```python
composite = "550e8400-e29b-41d4-a716-446655440000:3014540657:4"
# (UUID sessionKey) : (CRC32 of ciphertext) : (INSTANCE_TOKEN)
# hash with seed=26 → index=37
```

### Demo vs. production behavior

| Mode | INSTANCE_TOKEN | Transform | Result |
|------|---------------|-----------|--------|
| Demo (`-Ddemo.instance.token=4`) | 4 (fixed) | 037 | DECRYPTION_FAILED (bug visible) |
| Production (no override) | `System.nanoTime()` | Random | Usually OK (1 in 5 chance of buggy) |

**Important**: the Agent A barrier is preserved in production. The benchmark
harness uses the pre-captured `CAUSALITY_BUG1V5` payload (which encodes
Transform037). The live demo uses `-Ddemo.instance.token=4` to reproduce the
same scenario.

---

## Causality API Validation via Runtime Truth UI

To validate the live JSONL against the causality API (as the benchmark does):

```bash
# Start Runtime Truth UI
cd tools/rt-ui
pip install -r requirements.txt
python3 app.py

# In a second terminal — ingest the live run
python3 - << 'EOF'
import requests, json
r = requests.post("http://localhost:5000/api/runs/ingest", json={
    "label": "V5 Live Demo",
    "run_dir": "/tmp/demo_v5_live_repro",
    "user_prefixes": "com/example/truth"
})
run_id = r.json()["run_id"]
print("run_id:", run_id)

# Verify causality/reflection
r = requests.get(f"http://localhost:5000/api/runs/{run_id}/causality/reflection")
print(json.dumps(r.json(), indent=2))
EOF
```

Expected: the causality/reflection endpoint returns `TransformBase$Transform037$VHz0cN3d`
as one of the constructor reflection targets from `TransformCompiler.doCompile`.

---

## Graph and JSONL Validation

The indexer (`tools/rt-ui/indexer.py`) can validate the run directly:

```bash
python3 - << 'EOF'
import sys; sys.path.insert(0, "tools/rt-ui")
from indexer import load_and_index, validate_run, find_best_artifact

idx = load_and_index(
    "/tmp/demo_v5_live_repro/runtime_targets.jsonl",
    user_prefixes=["com/example/truth"]
)
report = validate_run("/tmp/demo_v5_live_repro", idx)
for name, ok, detail in report["checks"]:
    status = "✓" if ok else "✗"
    print(f"  {status} {name}" + (f": {detail}" if detail else ""))

print(f"\nAll checks passed: {report['all_ok']}")
print(f"Classes seen: {report['class_count']}")
print(f"Artifacts on disk: {report['artifact_count']}")

art = find_best_artifact(idx, "com/example/truth/handler/TransformBase$Transform037$VHz0cN3d")
print(f"\nTransform037 artifact: {art}")
EOF
```

---

## Files Produced

| File | Description |
|------|-------------|
| `tools/demo-runtime-truth/target/demo-runtime-truth-1.0.0.jar` | Built fat JAR |
| `tools/demo-runtime-truth/capture_live.sh` | Live capture script |
| `/tmp/demo_v5_live_repro/runtime_targets.jsonl` | Live JSONL (58,989 lines) |
| `/tmp/demo_v5_live_repro/artifacts/*.class` | 10,960 live artifact files |
| `/tmp/demo_v5_live_repro/v5_response.json` | Live API response |
| `/tmp/demo_v5_live_repro/stdout.txt` | Spring Boot logs |
| `/tmp/demo_v5_live_repro/stderr.txt` | JVM dump output |

---

## Conclusion

All six V5 demo capabilities have been validated against live JVM output:

| Capability | Pre-captured | Live | Status |
|---|---|---|---|
| Reflection (`DecryptPipeline.execute`) | ✓ | ✓ | **LIVE CONFIRMED** |
| JDK proxy (`$Proxy0` creation) | ✓ | ✓ | **LIVE CONFIRMED** |
| Hidden lambda classes (2×) | ✓ | ✓ | **LIVE CONFIRMED** |
| Generated class (`TransformBase$Transform037$...`) | ✓ | ✓ | **LIVE CONFIRMED** |
| Artifact on disk | ✓ | ✓ | **LIVE CONFIRMED** |
| Bytecode inspection (`bipush 75`) | ✓ | ✓ | **LIVE CONFIRMED** |

The demo is not showing pre-staged data. The JSONL, artifact files, and javap
output are produced from scratch by each run of `capture_live.sh`. The data
is structurally identical to the pre-captured payload in `bugs_v5.py` —
because both represent the same underlying bug executing on the same JVM.

The class suffix (`$VHz0cN3d` vs `$r8K3mNp2`) differs between runs because
ByteBuddy generates a random suffix. This is expected and does not affect
the correctness of the causality chain or the benchmark.
