# Runtime Truth

Records every dynamic dispatch that executes at runtime — `invokedynamic`, `invokevirtual`, `invokeinterface`, MethodHandle chains, `Method.invoke`, lambdas, hidden classes, proxies — with exact source BCI → target method attribution. Browse results in a local web UI.

---

## Build

```bash
bash tools/build.sh
```

That runs `configure` (first time only), `make hotspot`, and copies the updated JVM library into the JDK image. Works on Linux and macOS. The custom `java` binary path is printed at the end.

---

## Capture a run

```bash
mkdir -p /tmp/myrun/artifacts

export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_RUNTIME_GRAPH=1
export SOROUSH_CAPTURE_FINAL_BYTECODE=1
export SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/myrun/runtime_targets.jsonl
export SOROUSH_BYTECODE_DUMP_DIR=/tmp/myrun/artifacts
export SOROUSH_USER_PREFIXES="com/myapp"   # slash-separated package prefix

build/macosx-aarch64-server-fastdebug/jdk/bin/java -Xint -jar myapp.jar
```

`-Xint` is required (interpreter-only mode). Output: `runtime_targets.jsonl` + `.class` files in `artifacts/`.

---

## Start the UI

```bash
cd tools/manycore-ui
pip install flask
python3 app.py 5000
```

Open `http://localhost:5000`.

---

## Load your run

```bash
curl -X POST http://localhost:5000/api/runs/ingest \
  -H "Content-Type: application/json" \
  -d '{"label": "my-run", "run_dir": "/tmp/myrun"}'
```

Then select the run in the UI sidebar.

---

## Run tests

```bash
cd tools/manycore-ui && python3 -m pytest tests/ -v
```

---

## More detail

- `docs/00-agent-handoff.md` — architecture overview
- `docs/56-final-spring-validation.md` — validated output examples
- `tools/demo-runtime-truth/capture_live.sh` — Spring Boot end-to-end capture script
