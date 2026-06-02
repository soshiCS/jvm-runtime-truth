# Runtime Truth

Records every dynamic dispatch that executes at runtime and lets you browse the results in a web UI.

## Build

```bash
bash tools/build.sh
```

## Capture a run

```bash
mkdir -p /tmp/myrun/artifacts

SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_CAPTURE_FINAL_BYTECODE=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/myrun/runtime_targets.jsonl \
SOROUSH_BYTECODE_DUMP_DIR=/tmp/myrun/artifacts \
SOROUSH_USER_PREFIXES="com/myapp" \
  <path-to-custom-java> -Xint -jar myapp.jar
```

> `SOROUSH_USER_PREFIXES` is a slash-format package prefix (e.g. `com/myapp`). Only dispatches from matching classes are recorded.  
> `-Xint` is required.

## Browse results

```bash
cd tools/manycore-ui && pip install flask && python3 app.py 5000
```

Then load your run:

```bash
curl -X POST http://localhost:5000/api/runs/ingest \
  -H "Content-Type: application/json" \
  -d '{"label":"my-run","run_dir":"/tmp/myrun"}'
```

Open `http://localhost:5000` and select the run from the sidebar.

## Tests

```bash
cd tools/manycore-ui && python3 -m pytest tests/ -v
```
