# Running Tests and Demos

This document is the quick-start guide for running anything in this project — UI, CLI, or remote demo. It assumes the JVM is already built. If it is not, build it first: [07-build-workflow-guide.md](07-build-workflow-guide.md).

For in-depth per-case validation details and expected output, see [05-validation-guide.md](05-validation-guide.md).

---

## Three Ways to Run

| Method | When to use |
|---|---|
| **Runtime Truth UI** (local) | Exploring output interactively, uploading a JAR, browsing callsite records |
| **CLI** | Scripted validation, CI, running without Flask |
| **start_demo.sh** (remote) | Sharing a live demo with someone outside localhost via Cloudflare tunnel |

---

## Method 1: Runtime Truth UI (Local)

### Prerequisites

```bash
python3 -m pip install flask
```

### Start the server

```bash
cd /Users/soroushaghajani/custom-jvm/jdk21u-export/tools/rt-ui
python3 app.py 5001
```

Open `http://localhost:5001` in a browser.

### Run the 12-case suite via UI

1. Click **New Run**
2. Set **Run mode** to `main-class`
3. Set **Main class** to `testcases.TestCasesMain`
4. Set **Extra classpath** to `/tmp/cases-build/classes`
5. Set **User prefixes** to `testcases`
6. Click **Run**
7. Wait for status **Complete** (~10 seconds)

Expected results:
- Stdout tab: last line `test cases demo complete — 12/12 passed`
- Diagnostics tab: 0 user-code diagnostics (entries starting with `testcases`)
- Stats bar: callsite_target count > 0

If `/tmp/cases-build/classes` does not exist yet, build it first — see [CLI: Build the test cases](#cli-build-the-test-cases) below.

### Run Spring Boot via UI

1. Click **New Run**
2. Set **Run mode** to `java-jar`
3. Upload `/Users/soroushaghajani/gs-spring-boot/complete/target/spring-boot-complete-0.0.1-SNAPSHOT.jar`
4. Set **Program args** to `--spring.main.web-application-type=none`
5. Set **User prefixes** to `com/example/springboot`
6. Click **Run**
7. Wait for status **Complete** (~20–30 seconds)

Expected results:
- Stdout tab: contains `=== validation complete ===` and `app class: Application$$SpringCGLIB$$0`
- Diagnostics tab: 0 entries starting with `com/example`
- Stats bar: callsite_target ≥ 4000, with 26 user-code records

### UI layout overview

```
┌─ Navbar ─────────────────────────────────────────────────────────────┐
│ run selector  status  stats  [Diagnostics] [Validate] [Output] [Run] │
└──────────────────────────────────────────────────────────────────────┘
┌─ Classes ──────┬─ Callsites ─────────────────┬─ Targets / Bytecode ─┐
│ filter box     │ method + bci + opcode rows   │ node cards           │
│ class rows     │ (grouped by method)          │                      │
│ CS = callsites │                              │ [Targets] [Bytecode] │
│ ART = bytes    │                              │ tabs                 │
│ ⚠ = diags     │                              │                      │
└────────────────┴─────────────────────────────┴──────────────────────┘
```

Click a class → callsites populate. Click a callsite row → targets and adapter graph appear on the right. Click **View Bytecode** on any target node to see `javap` disassembly.

The **Validate** button runs 9 integrity checks and shows pass/fail for each.

---

## Method 2: CLI

### CLI: Build the test cases

Only needed once (or after editing the Java source files):

```bash
JAVAC=/Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/javac
mkdir -p /tmp/cases-build/classes
$JAVAC -d /tmp/cases-build/classes \
  /tmp/cases-build/src/testcases/*.java
```

Source lives at `/tmp/cases-build/src/testcases/`.

### CLI: Run the 12-case suite

```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/rt_out.jsonl \
  /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  -cp /tmp/cases-build/classes testcases.TestCasesMain
```

Expected last two lines of stdout:
```
PASS Case12 — Hidden class
test cases demo complete — 12/12 passed
```

Quick sanity check on the JSONL:
```bash
python3 -c "
import json
records = [json.loads(l) for l in open('/tmp/rt_out.jsonl') if l.strip()]
es = [r for r in records if r.get('record')=='export_summary'][0]
ud = [r for r in records if r.get('record')=='diagnostic'
      and r.get('src_class','').startswith('testcases')]
print('export complete :', es.get('complete'))
print('user diagnostics:', len(ud))
print('callsite_target :', es.get('callsite_target_count'))
"
```

Expected:
```
export complete : True
user diagnostics: 0
callsite_target : (some positive number)
```

### CLI: Run Spring Boot

```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/spring_out.jsonl \
  /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  -jar /Users/soroushaghajani/gs-spring-boot/complete/target/spring-boot-complete-0.0.1-SNAPSHOT.jar \
  --spring.main.web-application-type=none
echo "Exit: $?"
```

Expected:
- Stdout contains `=== validation complete ===` and `app class: Application$$SpringCGLIB$$0`
- Exit code: 0
- `/tmp/spring_out.jsonl` exists with ~15,000 records

Zero user-code diagnostics check:
```bash
python3 -c "
import json
records = [json.loads(l) for l in open('/tmp/spring_out.jsonl') if l.strip()]
ud = [r for r in records if r.get('record')=='diagnostic'
      and r.get('src_class','').startswith('com/example')]
print('User diagnostics:', len(ud))
"
```

Expected: `User diagnostics: 0`

---

## Method 3: Remote Demo via start_demo.sh

`start_demo.sh` starts the Runtime Truth UI and exposes it publicly through a Cloudflare Tunnel with single-use token auth. Use this when showing a live demo to someone outside localhost.

### Prerequisites

```bash
brew install cloudflared
python3 -m pip install flask
```

### Start

```bash
cd /Users/soroushaghajani/custom-jvm/jdk21u-export/tools/rt-ui
./start_demo.sh
```

The script:
1. Generates a random auth token (or uses `$RT_DEMO_TOKEN` if already set)
2. Starts Flask on port 5001
3. Starts a Cloudflare quick tunnel pointing to localhost:5001
4. Prints a summary with the public URL, username (`demo`), and password

Expected output:
```
╔══════════════════════════════════════════════════════════════╗
║           Runtime Truth UI — Demo Ready                     ║
╠══════════════════════════════════════════════════════════════╣
║  Public URL : https://xxxx-xxxx.trycloudflare.com
║  Username   : demo
║  Password   : <generated token>
║
║  Local URL  : http://localhost:5001
╚══════════════════════════════════════════════════════════════╝
```

Share the public URL, username, and password. The tunnel and Flask process stay alive until you close the terminal or press Ctrl-C.

### Using a fixed token

```bash
RT_DEMO_TOKEN=mytoken ./start_demo.sh
```

### Troubleshooting

- **Tunnel URL doesn't appear**: Check `/tmp/rt_cloudflare.log` for errors. The tunnel usually connects within 10 seconds.
- **Flask doesn't start**: Check `/tmp/rt_flask.log`. Most common cause: another process is using port 5001. The script kills it automatically, but if the kill fails, run `lsof -i :5001` to identify the process.
- **Auth rejected**: The password is the full generated token (URL-safe base64, 16 bytes). Confirm you're using the password printed by the script, not a cached browser credential.

---

## What the Output Means

### In the UI

| Tab | What to look for |
|---|---|
| **Diagnostics** | Should be empty for user-code prefixes. Any entry here means a callsite could not be resolved. |
| **Validate** | All 9 checks should be green. `export_summary.complete = true` is the most important. |
| **Output** (stdout/stderr) | Stdout: application output. Stderr: JVM debug output (verbose, ignore unless debugging). |

In the class list on the left:
- `CS` badge = class has captured callsite records (it was a source of dispatches)
- `ART` badge = class has a bytecode artifact (its .class was captured)
- `⚠` badge = class has diagnostic records

### In the JSONL (CLI runs)

```bash
python3 -c "
import json
from collections import Counter
records = [json.loads(l) for l in open('/tmp/rt_out.jsonl') if l.strip()]
print('Record type counts:')
for t, c in sorted(Counter(r.get('record') for r in records).items(), key=lambda x: -x[1]):
    print(f'  {c:>6}  {t}')
"
```

Key record types:
- `callsite_target` — the primary output; one record per resolved dispatch site
- `callsite_adapter_graph` — MethodHandle adapter chain decomposition
- `runtime_target` — methods resolved through JVM linkage machinery (no source BCI)
- `diagnostic` — callsites that could not be fully resolved (should be 0 for user code)
- `export_summary` — final counts; `complete: true` means no write errors

---

## Common Problems

| Symptom | Cause | Fix |
|---|---|---|
| Spring Boot run hangs forever | `--spring.main.web-application-type=none` not passed | Add it as program arg in UI, or to the CLI command |
| JSONL is empty / single `graph disabled` record | `SOROUSH_PROVENANCE_GRAPH=1` not set | The UI always sets it; for CLI, prefix the command with it |
| `callsite_target_count = 0` despite graph enabled | Wrong `libjvm.dylib` deployed | Run `java -version`; VM line must say `jdk21u-export`. If it says `jdk21u`, re-copy the dylib |
| UI shows runs but no callsites | User prefix doesn't match | The prefix filter in the UI controls which classes appear in the class list |
| `12/12 passed` but some Per-case checks fail | Cases pass at runtime but BCI details wrong | Run the per-case checks in [05-validation-guide.md](05-validation-guide.md) Part 2 |

---

## Cross-References

| Need | Document |
|---|---|
| Per-case validation with expected JSONL content | [05-validation-guide.md](05-validation-guide.md) |
| Build the JVM from scratch | [07-build-workflow-guide.md](07-build-workflow-guide.md) |
| Understanding the JSONL schema | [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) — "Export Pipeline" |
| Why a diagnostic was emitted | [06-known-limitations.md](06-known-limitations.md) |
| UI REST API reference | `tools/rt-ui/README.md` |
