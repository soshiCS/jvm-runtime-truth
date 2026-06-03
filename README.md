# Runtime Truth

Records every dynamic dispatch that executes at runtime — `invokedynamic`, `invokevirtual`, `invokeinterface`, MethodHandle chains, `Method.invoke`, lambdas, hidden classes, proxies — with exact source BCI → target method attribution. Browse results in a local web UI.

---

## Build

```bash
bash tools/build.sh
```

That runs `configure` (first time only), `make hotspot`, and copies the updated JVM library into the JDK image. Works on Linux and macOS. The custom `java` binary path is printed at the end.

---

## Start the UI

```bash
cd tools/manycore-ui
pip install flask
python3 app.py 5000
```

Open `http://localhost:5000`.

---

## Run a JAR

Click **New Run**, upload your JAR, select the class prefix to observe, and click run. Results appear automatically in the sidebar when the run completes.

---

## Advanced: capture a long-running app (e.g. Spring Boot)

For apps that need external traffic (HTTP requests, etc.), run them manually with capture enabled:

```bash
mkdir -p /tmp/myrun/artifacts

export SOROUSH_PROVENANCE_GRAPH=1
export SOROUSH_RUNTIME_GRAPH=1
export SOROUSH_CAPTURE_FINAL_BYTECODE=1
export SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/myrun/runtime_targets.jsonl
export SOROUSH_BYTECODE_DUMP_DIR=/tmp/myrun/artifacts
export SOROUSH_USER_PREFIXES="com/myapp"

<path-to-custom-java> -Xint -jar myapp.jar
```

Then load the results into the UI:

```bash
curl -X POST http://localhost:5000/api/runs/ingest \
  -H "Content-Type: application/json" \
  -d '{"label": "my-run", "run_dir": "/tmp/myrun"}'
```

> `-Xint` is required (interpreter-only mode).  
> `SOROUSH_USER_PREFIXES` is a slash-format package prefix (e.g. `com/myapp`).

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
