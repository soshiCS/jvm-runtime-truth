# Runtime Truth UI

Local web UI for exploring the runtime target export produced by the custom JVM fork.
Runs entirely on localhost — no auth, no external services.

---

## Prerequisites

- Python 3.11+
- Flask 3.x (`python3 -m pip install flask`)
- The custom JVM built in `jdk21u-export` with a fresh `libjvm.dylib` (see below)

### Keeping libjvm.dylib in sync

`jdk21u-export` is the only authoritative repo. After every `make hotspot` in `jdk21u-export`, copy the build output to the path the `java` binary loads from:

```bash
cd ~/custom-jvm/jdk21u-export
make hotspot
cp build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
   build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib
```

Verify: `build/macosx-aarch64-server-fastdebug/jdk/bin/java -version` must show `jdk21u-export` in the VM line. Do **not** build or copy from `jdk21u` — that is a historical copy.

---

## Starting the server

```bash
cd tools/manycore-ui
python3 app.py              # port 5000
python3 app.py 5001         # custom port
```

Open `http://localhost:5000` (or whatever port you chose).

---

## Quickstart with RuntimeTargetShowcaseDemo

```bash
# 1. Build the showcase JAR
mkdir -p /tmp/showcase_ui_build
JAVAC=~/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/javac
cp Downloads/bugged/jvm-dump-demo/RuntimeTargetShowcaseDemo.java /tmp/showcase_ui_build/
$JAVAC -d /tmp/showcase_ui_build /tmp/showcase_ui_build/RuntimeTargetShowcaseDemo.java
jar cf /tmp/showcase_ui_build/showcase.jar \
    -C /tmp/showcase_ui_build RuntimeTargetShowcaseDemo.class \
    -C /tmp/showcase_ui_build 'RuntimeTargetShowcaseDemo$Greeter.class'

# 2. Start UI
cd tools/manycore-ui && python3 app.py 5001

# 3. Open http://localhost:5001 → click "▶ New Run"
#    - Upload showcase.jar
#    - Set Rewriter prefix: RuntimeTargetShowcaseDemo
#    - Leave everything else default
#    - Click Run
```

The run completes in ~10 seconds. You should see:
- ~1349 classes loaded
- 51 callsites recorded
- `RuntimeTargetShowcaseDemo` highlighted in the class list (has callsites + artifacts)

---

## UI layout

```
┌─ Navbar ────────────────────────────────────────────────────────────┐
│ run selector  status  stats  [Diagnostics] [Validate] [Output] [Run]│
└─────────────────────────────────────────────────────────────────────┘
┌─ Classes ─────┬─ Callsites ──────────────────┬─ Targets / Bytecode ─┐
│ filter box    │ method + bci + opcode rows    │ node cards with       │
│ class rows    │ (grouped by method)           │ classification colors │
│ with badges:  │                               │                       │
│  CS = has src │                               │ [Targets] [Bytecode]  │
│  ART = bytes  │                               │ tabs                  │
│  ⚠ = diags   │                               │                       │
└───────────────┴───────────────────────────────┴───────────────────────┘
```

Click a class → callsites populate. Click a callsite → targets/adapter graph appear.
Click "View Bytecode" on any target node to run `javap` and see the disassembly.

---

## REST API summary

| Method | Path | Description |
|--------|------|-------------|
| POST   | `/api/run` | Start a run (multipart: `jar` file + `config` JSON) |
| GET    | `/api/runs` | List all runs |
| GET    | `/api/runs/<id>` | Run status + stats |
| GET    | `/api/runs/<id>/classes` | All indexed class names |
| GET    | `/api/runs/<id>/classes/<name>/callsites` | Callsites sourced from class |
| GET    | `/api/runs/<id>/callsite/<idx>` | Full callsite record |
| GET    | `/api/runs/<id>/artifact?class=&loader_id=` | Best bytecode artifact |
| GET    | `/api/runs/<id>/bytecode?artifact_path=` | javap output |
| GET    | `/api/runs/<id>/validate` | 9-check validation report |
| GET    | `/api/runs/<id>/diagnostics` | Diagnostic records |
| GET    | `/api/runs/<id>/stdout` | Program stdout (plain text) |
| GET    | `/api/runs/<id>/stderr` | JVM stderr, last 500 lines (plain text) |

### Config JSON fields (all optional except noted)

| Field | Type | Default | Notes |
|-------|------|---------|-------|
| `run_mode` | `"java-jar"` \| `"main-class"` | `"java-jar"` | |
| `main_class` | string | — | required when `run_mode=main-class` |
| `prog_args` | string | — | space-separated |
| `jvm_args` | string | — | space-separated; `-Xint` and `-Xverify:all` always added |
| `timeout` | int | 60 | seconds |
| `extra_cp` | string | — | colon-separated extra classpath entries |
| `env_vars` | object | — | extra env vars as `{"KEY":"VALUE"}` |
| `rewriter_prefix` | string | — | slash-separated class prefix to instrument |
| `working_dir` | string | — | override cwd for the JVM process |
| `jdk_path` | string | export JDK | override full JDK directory |
| `javap_path` | string | export javap | override javap binary |

---

## Per-run directory layout

```
/tmp/rt_ui_runs/<run_id>/
  app.jar                    copy of the uploaded JAR
  runtime_targets.jsonl      full export (all 9 record types)
  artifacts/                 .class files (SOROUSH_BYTECODE_DUMP_DIR)
    Foo.class                final (transformed) bytecode
    Foo.original.class       pre-transformation bytecode (when rewritten)
  stdout.txt
  stderr.txt
  summary.json               run metadata
```

---

## Environment variables set on every run

The backend always injects:

| Variable | Value |
|----------|-------|
| `SOROUSH_PROVENANCE_GRAPH` | `1` |
| `SOROUSH_RUNTIME_GRAPH` | `1` |
| `SOROUSH_RUNTIME_RECOVERY` | `1` |
| `SOROUSH_TRACE_INDY` | `1` |
| `SOROUSH_TRACE_REFLECTION` | `1` |
| `SOROUSH_CAPTURE_FINAL_BYTECODE` | `1` |
| `SOROUSH_REWRITER_PHASE5_NORMAL_EXIT` | `1` |
| `SOROUSH_BYTECODE_DUMP_DIR` | `<run_dir>/artifacts/` |
| `SOROUSH_EXPORT_RUNTIME_TARGETS` | `<run_dir>/runtime_targets.jsonl` |

---

## Validation checks

`GET /api/runs/<id>/validate` runs these checks in order:

1. JSONL file exists  
2. JSONL parses without error  
3. At least one record present  
4. `export_summary` record present  
5. `export_summary.complete = true` (no write errors during export)  
6. All `bytecode_artifact` records with `artifact_path` point to files that exist on disk  
7. No callsite records with descriptor `?` (unknown descriptor — indicates missing recovery)  
8. No `user_target` callsite nodes with `loader_id = 0` (bootstrap loader confusion)  
9. No `user_target` callsite nodes with `exact = false`  

---

## Known limitations

- **In-memory run store:** Runs are kept in a Python dict; they are lost if the Flask process restarts. The per-run directories under `/tmp/rt_ui_runs/` persist across restarts, but are not automatically re-indexed on startup.
- **Index cache not invalidated:** If the JSONL for a completed run is somehow modified after the first load, the old index is served. Restart the server to clear.
- **`-Xint` only:** All runs use interpreter mode. JIT-compiled frames do not fire `resolve_handle_call`, so no JIT-generated callsite records will appear.
- **Single JAR upload:** Only one JAR per run. Multi-module projects need a fat JAR or use `extra_cp` to supply dependencies.
- **No streaming progress:** The callsite/class counts only appear after the run finishes; there is no line-by-line live tail.
- **`libjvm.dylib` drift:** If `jdk21u` is rebuilt without copying the dylib to `jdk21u-export`, runs will silently produce no JSONL. Always check `has_jsonl` in the run status response.
