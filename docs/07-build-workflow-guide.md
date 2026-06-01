# Build and Workflow Guide

**Read this before making any code changes.**

---

## The Most Important Rule

```
jdk21u-export  =  THE ONLY AUTHORITATIVE REPOSITORY
jdk21u         =  HISTORICAL COPY — NEVER MODIFY
```

Every source edit, every build, every validation, every `libjvm.dylib` deployment happens in `jdk21u-export`. Never touch `jdk21u` unless explicitly instructed to do so.

---

## Repository Locations

| Path | Role |
|---|---|
| `/Users/soroushaghajani/custom-jvm/jdk21u-export/` | Active development repo (authoritative) |
| `/Users/soroushaghajani/custom-jvm/jdk21u/` | Historical copy — do not modify |
| `/Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/` | Build output |
| `/Users/soroushaghajani/gs-spring-boot/complete/` | Spring Boot validation app |
| `/tmp/manycore-cases-build/` | ManyCore test case build dir |

---

## Key File Paths (Canonical)

| File | Path in jdk21u-export |
|---|---|
| Provenance graph core | `src/hotspot/share/classfile/soroushProvenanceGraph.cpp` |
| Provenance graph header | `src/hotspot/share/classfile/soroushProvenanceGraph.hpp` |
| Bytecode rewriter | `src/hotspot/share/classfile/soroushClassfileRewriter.cpp` |
| Hidden class hook | `src/hotspot/share/classfile/klassFactory.cpp` |
| Cold-path capture | `src/hotspot/share/interpreter/linkResolver.cpp` |
| Warm-path trampoline | `src/hotspot/share/interpreter/interpreterRuntime.cpp` |
| Warm-path declarations | `src/hotspot/share/interpreter/interpreterRuntime.hpp` |
| Warm-path hook (asm) | `src/hotspot/cpu/aarch64/templateTable_aarch64.cpp` |

**`linkResolver.cpp` is at `interpreter/`, NOT `classfile/`.**  
In the historical `jdk21u` repo, linkResolver.cpp is in `classfile/`. In `jdk21u-export`, it is in `interpreter/`. The build system for `jdk21u-export` compiles from `interpreter/`. Editing `classfile/linkResolver.cpp` in `jdk21u-export` (if that file even existed) would have no effect.

---

## Standard Build Sequence

### After editing any HotSpot source

```bash
# Step 1: Build
cd /Users/soroushaghajani/custom-jvm/jdk21u-export
make hotspot

# Step 2: Copy libjvm.dylib (REQUIRED — make hotspot does NOT do this automatically)
cp build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
   build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib

# Step 3: Verify
build/macosx-aarch64-server-fastdebug/jdk/bin/java -version
```

Expected output from step 3:
```
openjdk version "21.0.12-internal" 2026-07-21
OpenJDK Runtime Environment (fastdebug build 21.0.12-internal-adhoc.soroushaghajani.jdk21u)
OpenJDK 64-Bit Server VM (fastdebug build 21.0.12-internal-adhoc.soroushaghajani.jdk21u-export, mixed mode)
```

The string `jdk21u-export` must appear in the VM line. If it says `jdk21u` (without `-export`), you have deployed the wrong dylib.

### Why two copy destinations are NOT needed

`jdk/lib/server/libjvm.dylib` is the path used by:
- `build/macosx-aarch64-server-fastdebug/jdk/bin/java`
- The UI runner (which uses the export JDK path)
- Manual validation commands

`support/modules_libs/java.base/server/libjvm.dylib` is where `make hotspot` writes its output. It is NOT directly used by the `java` binary.

The copy command `support/ → jdk/` is the deployment step. Without it, `java -version` shows the old build.

---

## Environment Variables

| Variable | Required for | Effect when absent |
|---|---|---|
| `SOROUSH_PROVENANCE_GRAPH=1` | All export operations | Collection disabled; `g_sg_enabled=0`; near-zero overhead |
| `SOROUSH_EXPORT_RUNTIME_TARGETS=/path/out.jsonl` | Writing JSONL | Records collected in memory but never written to disk |

Both must be set together. Setting only `SOROUSH_EXPORT_RUNTIME_TARGETS` produces a single `diagnostic` record with `reason=graph_disabled`.

Other environment variables used by specific subsystems:
```
SOROUSH_BYTECODE_DUMP_DIR=/path/    # Directory for .class file dumps
SOROUSH_REWRITER_PREFIX=pkg/name    # Slash-separated prefix for bytecode rewriter
```

---

## Standard Run Command

```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/out.jsonl \
  /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  [JVM args] \
  [main class or -jar ...] \
  [program args]
```

---

## ManyCore Cases

### Build

```bash
JAVAC=/Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/javac
mkdir -p /tmp/manycore-cases-build/classes
$JAVAC -d /tmp/manycore-cases-build/classes \
  /tmp/manycore-cases-build/src/manycorecases/*.java
```

Source location: `/tmp/manycore-cases-build/src/manycorecases/`

### Run

```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/manycore_out.jsonl \
  /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  -cp /tmp/manycore-cases-build/classes manycorecases.ManyCoreCasesMain
```

---

## Spring Boot Validation

### Rebuild JAR

```bash
cd /Users/soroushaghajani/gs-spring-boot/complete
mvn package -DskipTests
```

### Run

```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/spring_out.jsonl \
  /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  -jar /Users/soroushaghajani/gs-spring-boot/complete/target/spring-boot-complete-0.0.1-SNAPSHOT.jar \
  --spring.main.web-application-type=none
```

The `--spring.main.web-application-type=none` flag disables Tomcat. Without it, the application blocks indefinitely.

---

## ManyCore UI

### Start

```bash
cd /Users/soroushaghajani/custom-jvm/jdk21u-export/tools/manycore-ui
python3 app.py 5001
# Open http://localhost:5001
```

Prerequisites:
```bash
python3 -m pip install flask
```

The UI automatically sets all required environment variables for each run. It uses the export JDK at:
```
/Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/
```

---

## Common Mistakes and How to Fix Them

### "My print/change never appears in output"

**Cause**: You edited a file and rebuilt, but the `libjvm.dylib` copy step was skipped.  
**Fix**: Always run the copy command after `make hotspot`.

Also verify: if you added a `printf` or `fprintf(stderr, ...)`, ensure the env var is set (some code paths are guarded by `soroush_graph_enabled()`).

### "Build succeeds but I get wrong behavior / my change has no effect"

**Cause A**: You edited `jdk21u/src/...` instead of `jdk21u-export/src/...`.  
**Diagnosis**: `diff jdk21u/src/hotspot/share/interpreter/linkResolver.cpp jdk21u-export/src/hotspot/share/interpreter/linkResolver.cpp`  
**Fix**: Redo the edit in `jdk21u-export`.

**Cause B**: The `.d` dependency file for the source did not pick up the change (rare, happens with unusual file timestamps).  
**Fix**: `touch` the file and rebuild: `touch src/hotspot/share/interpreter/linkResolver.cpp && make hotspot`

### "Compilation error: 'pre' was not declared"

**Cause**: Using bare `pre(sp, -n)` in `templateTable_aarch64.cpp`.  
**Fix**: Use `__ pre(sp, -n)` — the assembler is accessed via the `__` macro in template table code.

### "Compilation error: 'current' was not declared" inside a JRT_ENTRY function

**Cause**: JRT_ENTRY function parameter named `thread` instead of `current`.  
**Fix**: Rename the parameter to `current`:
```cpp
JRT_ENTRY(void, InterpreterRuntime::my_function(JavaThread* current, ...))
```

### "JVM crashes with BootstrapMethodError or NullPointerException in invokeBasic"

**Cause**: Missing `lr` save/restore around `call_VM` in `TemplateTable::invokehandle`.  
**Diagnosis**: Does the crash happen only for MH invocations? Does removing the warm-path hook make it go away?  
**Fix**: Ensure `stp(lr, zr, __ pre(sp, -2 * wordSize))` before `call_VM` and `ldp(lr, zr, __ post(sp, 2 * wordSize))` after it. See [02-phase-history.md](02-phase-history.md) Milestone 6.

### "All callsite_target counts are 0 / JSONL is empty except for diagnostic"

**Cause**: `SOROUSH_PROVENANCE_GRAPH=1` is not set.  
**Fix**: Prefix the java command with `SOROUSH_PROVENANCE_GRAPH=1`.

### "Spring Boot hangs"

**Cause**: `--spring.main.web-application-type=none` not passed.  
**Fix**: Add it as a program argument.

### "libjvm.dylib from wrong build"

**Symptom**: `java -version` shows `jdk21u` in the VM line, not `jdk21u-export`.  
**Fix**:
```bash
cp /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
   /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib
```

---

## Build Diagnostics

### Checking which file was actually compiled

```bash
# Find the dependency file for linkResolver
find /Users/soroushaghajani/custom-jvm/jdk21u-export/build -name "linkResolver.d" 2>/dev/null
# The path inside the .d file shows which source was used
cat <path_to_linkResolver.d> | head -5
```

### Checking the modification timestamp

```bash
ls -la /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib
ls -la /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib
```

Both timestamps should match after the copy step. If `jdk/lib/server/` is older, the copy was not done or did not succeed.

### Clean build (last resort)

```bash
cd /Users/soroushaghajani/custom-jvm/jdk21u-export
make clean hotspot
cp build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
   build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib
```

Warning: `make clean` rebuilds everything, which takes significantly longer than incremental `make hotspot`.
