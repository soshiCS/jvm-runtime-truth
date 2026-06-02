# Runtime Truth — JVM Dynamic Dispatch Analyzer

Runtime Truth is a modified OpenJDK 21 JVM that records every dynamic dispatch that actually executes at runtime: `invokedynamic`, `invokevirtual`, `invokeinterface`, MethodHandle adapter chains, reflection (`Method.invoke`), lambda bodies, hidden classes, and JDK dynamic proxies.

Every captured dispatch is written to a JSONL file with exact source class/method/BCI, dispatch kind, and resolved target. A local web UI lets you browse classes, inspect callsites, view captured bytecode, and follow dispatch chains.

---

## 1. Build

### Prerequisites

- macOS (aarch64) — the build has been validated on `macosx-aarch64-server-fastdebug`
- Xcode command line tools
- Java 21 boot JDK (used only for bootstrapping the build)
- `autoconf`, `make`

### Build the custom JVM

```bash
# from repo root (jdk21u-export/)
bash configure --with-debug-level=fastdebug --with-jvm-variants=server
make hotspot

# Copy the new libjvm into the jdk image (two-step required on macOS)
cp build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
   build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib
```

The custom `java` binary is at:
```
build/macosx-aarch64-server-fastdebug/jdk/bin/java
```

---

## 2. Run a Java Application with Runtime Truth

Set the required environment variables and run your application with the custom JVM.

### Minimum environment

```bash
export SOROUSH_PROVENANCE_GRAPH=1          # enable graph export
export SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/myrun/runtime_targets.jsonl
export SOROUSH_BYTECODE_DUMP_DIR=/tmp/myrun/artifacts
export SOROUSH_USER_PREFIXES="com/myapp"   # JVM-format prefix; only these classes emit dispatch records
mkdir -p /tmp/myrun/artifacts

java -Xint -jar myapp.jar
```

`-Xint` forces interpreter mode. This is required — JIT-compiled frames do not pass through the resolution hooks.

### Full environment (all capture features enabled)

```bash
export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_RUNTIME_GRAPH=1
export SOROUSH_RUNTIME_RECOVERY=1
export SOROUSH_TRACE_INDY=1
export SOROUSH_TRACE_REFLECTION=1
export SOROUSH_CAPTURE_FINAL_BYTECODE=1
export SOROUSH_REWRITER_PHASE5_NORMAL_EXIT=1
export SOROUSH_BYTECODE_DUMP_DIR=/tmp/myrun/artifacts
export SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/myrun/runtime_targets.jsonl
export SOROUSH_REWRITER_PHASE5_PREFIX="com/myapp"
export SOROUSH_USER_PREFIXES="com/myapp"
```

### Spring Boot example

```bash
mkdir -p /tmp/myrun/artifacts
# ... set exports above ...
java -Xverify:all -Xint \
  -jar tools/demo-runtime-truth/target/demo-runtime-truth-1.0.0.jar \
  --server.port=8080
```

Build the demo Spring app first if needed:
```bash
cd tools/demo-runtime-truth && mvn package -DskipTests
```

---

## 3. Output Files

| File | Description |
|---|---|
| `runtime_targets.jsonl` | One JSON record per line; the complete dispatch graph |
| `artifacts/*.class` | Captured `.class` files for all loaded classes |
| `stdout.txt` / `stderr.txt` | Application output / JVM trace |

### JSONL record types

| `record` value | Meaning |
|---|---|
| `callsite_target` | Exact target for an executed callsite (invokedynamic, MH invoke, invokevirtual, invokeinterface, reflection) |
| `callsite_adapter_graph` | MethodHandle adapter chain with classified nodes |
| `callsite_target_set` | Multi-target combinator (guardWithTest, catchException, tryFinally) |
| `diagnostic` | Callsite where exact resolution was not possible |
| `bytecode_artifact` | A captured `.class` file |
| `generated_class` | Class created at runtime (LambdaMetafactory, ByteBuddy, Proxy.newProxyInstance) |
| `hidden_class_identity` | Maps a stable CRC to a run-specific `+0x` hidden class name |
| `runtime_target` | Target observed via vtable/itable dispatch |
| `export_summary` | Final record; `complete=true` means a clean JVM shutdown |

### Reading a callsite_target record

```json
{
  "record": "callsite_target",
  "category": "invokedynamic",
  "evidence": "LINKAGE_GUARANTEED",
  "source_class": "com/example/App",
  "source_method": "run",
  "source_bci": 42,
  "source_opcode": "invokedynamic",
  "target_class": "com/example/App$$Lambda+0x000000f000088230",
  "target_method": "accept",
  "lmf_impl_class": "com/example/App",
  "lmf_impl_method": "lambda$run$0"
}
```

Key fields: `source_class` + `source_method` + `source_bci` = the exact bytecode instruction that dispatches. `target_class` + `target_method` = what actually ran.

---

## 4. Start the UI

```bash
cd tools/manycore-ui
pip install -r requirements.txt   # only needed once; requires flask

# local-only (no auth)
python3 app.py 5000

# remote demo with Cloudflare Tunnel + token auth
./start_demo.sh
```

The UI runs at `http://localhost:5000` (or the port you pass).

---

## 5. Load a Run in the UI

### Option A: Ingest a pre-captured run directory

```bash
curl -s -X POST http://localhost:5000/api/runs/ingest \
  -H "Content-Type: application/json" \
  -d '{"label": "my-spring-run", "run_dir": "/tmp/myrun"}'
```

The run directory must contain `runtime_targets.jsonl`. The `artifacts/` subdirectory is optional but needed for bytecode viewing.

### Option B: Upload a JAR via the UI

Use the **New Run** button in the UI to upload a JAR file. The UI will run it under the custom JVM using the configured environment.

### Selecting a run

After ingesting or completing a run, it appears in the sidebar run list. Click it to activate. The class list, callsites, and artifact viewer all update to reflect the selected run.

---

## 6. Inspect Dispatches in the UI

### Classes panel

The left sidebar lists every class seen in the run. Classes are filtered by their role:
- **user** — source of a callsite matching `SOROUSH_USER_PREFIXES`
- **target** — target of a user-source callsite
- **generated** — LambdaMetafactory lambdas, ByteBuddy subclasses, dynamic proxies
- **hidden** — hidden classes (`+0x` suffix); grouped into lambda families when multiple instances share a base name

Clicking a class shows its methods and callsites.

### Callsites panel

For each method in a class, the callsites panel shows:
- Source BCI and opcode
- Dispatch category (`invokedynamic`, `invokevirtual`, `invokeinterface`, `methodhandle_invoke`, `reflection_method_invoke`, etc.)
- Resolved target class and method
- Evidence level (`LINKAGE_GUARANTEED`, `OBSERVED_ONLY`)
- For `invokedynamic` with LambdaMetafactory: the lambda body implementation method
- For MethodHandle adapter chains: the full adapter node graph

### Bytecode viewer

Click **View Bytecode** on any class with artifacts. The UI calls `javap -c -p -verbose` on the captured `.class` file and displays the result. For hidden lambda classes, the correct artifact is resolved via the CRC link in `hidden_class_identity` records.

### Adapter graph

For `callsite_adapter_graph` records, the UI renders the adapter chain nodes with their classification (primary_target, adapted_target, jdk_adapter, etc.) and exact/non-exact flags.

### Proxy and reflection evidence

- JDK dynamic proxy classes (`$Proxy*`) are tagged `is_proxy_class=True`. The UI shows which handler lambda was installed.
- Reflection callsites (`reflection_method_invoke`) show the declared target from the `Method` object, not just the `Method.invoke` dispatch.

---

## 7. Run Validation Tests

```bash
cd tools/manycore-ui
python3 -m pytest tests/ -v
# expected: 41 passed
```

### Run the ManyCore demo (15 dispatch categories)

```bash
cd /tmp/manycore-cases-build   # or wherever ManyCoreCasesDemo.jar is built

export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/mc/runtime_targets.jsonl
export SOROUSH_BYTECODE_DUMP_DIR=/tmp/mc/artifacts
export SOROUSH_USER_PREFIXES="manycorecases"
mkdir -p /tmp/mc/artifacts

<repo>/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  -Xverify:all -Xint \
  -cp ManyCoreCasesDemo.jar manycorecases.ManyCoreCasesMain
# expected: ManyCore cases demo complete — 15/15 passed
```

---

## 8. Common Commands

```bash
# Build JVM only (after editing C++ sources)
make hotspot && cp build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
                   build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib

# Run unit tests
cd tools/manycore-ui && python3 -m pytest tests/ -v

# Quick dispatch check: count records by type
python3 -c "
import json, collections
counts = collections.Counter()
with open('/tmp/myrun/runtime_targets.jsonl') as f:
    for line in f: counts[json.loads(line).get('record','')] += 1
for k,v in sorted(counts.items(), key=lambda x:-x[1]): print(f'{v:6d}  {k}')
"

# Ingest a run into the UI
curl -X POST http://localhost:5000/api/runs/ingest \
  -H "Content-Type: application/json" \
  -d '{"label": "my-run", "run_dir": "/tmp/myrun"}'

# Download full run package (JSONL + artifacts + validation report)
curl http://localhost:5000/api/runs/<run_id>/download/full -o run.zip
```

---

## 9. Project Structure

```
src/hotspot/share/classfile/linkResolver.cpp  — Core dispatch hooks (the only compiled file)
src/hotspot/share/interpreter/linkResolver.cpp — Mirror (not compiled; kept in sync)

tools/manycore-ui/
  app.py          — Flask REST API + UI backend
  indexer.py      — JSONL parser and class index builder
  graph_builder.py — Causality graph (staticization readiness)
  runner.py       — JAR execution under custom JVM
  static/         — Single-page UI (HTML/CSS/JS)
  tests/          — 41 unit tests

tools/demo-runtime-truth/  — Spring Boot demo app
  capture_live.sh          — End-to-end capture script

docs/                      — Design docs, validation reports
  00-agent-handoff.md      — Quick-start for new contributors
  52–56-*.md               — Recent validation and fix history
```

---

## 10. Known Limitations

- **Interpreter-only**: `-Xint` is required. JIT-compiled frames after warmup are not captured.
- **macOS/aarch64**: the two-step `libjvm.dylib` copy workflow is platform-specific.
- **Proxy handler linkage**: works when the handler lambda is created in the same source method immediately before `Proxy.newProxyInstance`. Spring's factory patterns (`Proxy.newProxyInstance` called deep inside framework code) show `is_proxy_class=True` but `proxy_handler=None`.
- **CP-cache dedup**: one `callsite_target` per BCI. Polymorphic callsites where different types arrive at the same BCI across separate warm-up phases may emit multiple records only if CP-cache resolution is re-triggered.
