# Mockito / ByteBuddy Dynamic Attach Investigation

**Date:** 2026-06-03  
**Status:** DIAGNOSIS COMPLETE — hang confirmed, root cause refined, workaround valid

---

## Summary

Dynamic `VirtualMachine.attach()` via `ByteBuddyAgent.installExternal()` still hangs when
Runtime Truth provenance is enabled. The hang occurs in both interpreter-only (`-Xint`) and
JIT mixed-mode. It does **not** occur when provenance is disabled, or when the ByteBuddy agent
is preloaded with `-javaagent`. The workaround from 2026-05-30 (docs/06, Gap #16) remains
fully valid. No recent changes fixed or worsened the issue.

---

## Run Matrix

| Run | JVM | Provenance | `-Xint` | Agent loading | Result | Duration |
|-----|-----|:---:|:---:|:---:|:---:|:---:|
| A  | Custom | ON  | yes | dynamic `ByteBuddyAgent.installExternal()` | **HANG** | 45s (watchdog) |
| A2 | Custom | ON  | no  | dynamic `ByteBuddyAgent.installExternal()` | **HANG** | 20s (watchdog) |
| B  | Custom | OFF | no  | dynamic `ByteBuddyAgent.installExternal()` | PASS | 1884ms |
| C  | Custom | ON  | yes | preloaded `-javaagent:byte-buddy-agent.jar` | PASS | 946ms |
| C2 | Custom | ON  | yes | preloaded + `SOROUSH_USER_PREFIXES=mockitotest` | PASS | 907ms |
| D  | Stock Zulu 21 | OFF | no | dynamic `ByteBuddyAgent.installExternal()` | PASS | 551ms |

---

## Commands

### Run A (hang reference)
```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/runA_out.jsonl \
  <custom-java> -Xint \
  -cp "classes:mockito-core-5.11.0.jar:byte-buddy-1.14.15.jar:byte-buddy-agent-1.14.15.jar:objenesis-3.3.jar" \
  -Dtest.timeout=45 mockitotest.MockitoTest
# → exit 42 (watchdog timeout)
```

### Run C (workaround)
```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/runC_out.jsonl \
  <custom-java> -Xint \
  -javaagent:byte-buddy-agent-1.14.15.jar \
  -cp "classes:mockito-core-5.11.0.jar:byte-buddy-1.14.15.jar:byte-buddy-agent-1.14.15.jar:objenesis-3.3.jar" \
  -Dtest.timeout=30 mockitotest.MockitoTest
# → PASS in 946ms
```

---

## Thread Dump at Hang (Runs A and A2 — identical)

Main thread stuck in:
```
java.base/java.lang.ProcessImpl.waitFor(ProcessImpl.java:425)
net.bytebuddy.agent.ByteBuddyAgent.installExternal(ByteBuddyAgent.java:705)
net.bytebuddy.agent.ByteBuddyAgent.install(ByteBuddyAgent.java:636)
net.bytebuddy.agent.ByteBuddyAgent.install(ByteBuddyAgent.java:616)
net.bytebuddy.agent.ByteBuddyAgent.install(ByteBuddyAgent.java:568)
net.bytebuddy.agent.ByteBuddyAgent.install(ByteBuddyAgent.java:545)
org.mockito.internal.creation.bytebuddy.InlineDelegateByteBuddyMockMaker.<clinit>(InlineDelegateByteBuddyMockMaker.java:133)
...
mockitotest.MockitoTest.main(MockitoTest.java:44)
```

There is also a live `process reaper (pid 90007)` thread:
```
Thread: process reaper (pid 90007) state=RUNNABLE
    java.base/java.lang.ProcessHandleImpl.waitForProcessExit0(Native Method)
```

This subprocess IS the external attach tool that ByteBuddy spawned. It is running but the
target JVM cannot service the attach request.

---

## Root Cause Analysis

### Attach mechanism

Mockito's `InlineByteBuddyMockMaker` calls `ByteBuddyAgent.install()`. When the ByteBuddy
agent JAR is not preloaded, `install()` falls through to `installExternal()`, which:
1. Spawns a subprocess (a second JVM) running `ByteBuddyAgent.processCommand`
2. The subprocess calls `VirtualMachine.attach(targetPid)` from the `com.sun.tools.attach` API
3. `VirtualMachine.attach()` sends a signal (SIGQUIT + `.attach_pid` file) to the target JVM
4. The target JVM must reach a **safepoint** to service the attach request
5. The subprocess waits for the target JVM to confirm attachment

### Why safepoints are not serviced

With provenance ON, the JVM emits a graph record for every class load. For a Mockito run,
~2,700 classes are loaded before `Mockito.mock()` is even called. Each class load triggers:
- Mutex lock/unlock on the provenance graph (C++ level)
- Memory allocation for graph nodes/edges
- CRC computation + string formatting
- stderr write (thousands of lines)

The aggregate overhead is enough that the JVM's safepoint polling window — which normally occurs
at backward branches and method calls — cannot be serviced within the attach subprocess timeout.

### Why `-Xint` vs JIT doesn't matter

The class load path always runs through the interpreter regardless of JIT settings. The
provenance class load hooks (`soroush_graph_class_load` et al.) are hooked at the VM level, not
at the bytecode interpreter level. `-Xint` mode adds *additional* overhead (warm-path dispatch
hooks on every invokevirtual/invokeinterface), but the class load overhead alone is sufficient to
cause the hang.

**Run A2 confirms this:** same hang, same stack trace, without `-Xint`.

### Why preloaded agent works

When `-javaagent:byte-buddy-agent.jar` is passed, the agent is installed during JVM startup
*before* any application class loading begins. No safepoint is required — the agent hooks in
through the standard JVMTI agent attach path, which is always-safe. When `InlineDelegateByteBuddyMockMaker`
initializes, `ByteBuddyAgent.isInstalled()` returns true and `installExternal()` is never called.

### Why provenance OFF works (Run B)

Without `SOROUSH_PROVENANCE_GRAPH=1`, the class load hooks are disabled. Class loads are fast,
the JVM services safepoints normally, and `VirtualMachine.attach()` completes in ~1s.

---

## Did Recent Changes Affect This?

**No.** The following recent changes are not relevant to the hang:

| Change | Relevant to attach? |
|---|---|
| Gap #1: warm-path reflection hook (`soroush_trace_iv_dispatch` Phase 2E) | No — only fires post-attach |
| Gap #2: `invokeinterface` hook in `LinkResolver::runtime_resolve_interface_method` | No — only fires post-attach |
| Lambda dedup fix (indexer.py) | No — Python only |
| docs/55–58, README, .gitignore cleanup | No |

The hang in Run A and A2 is mechanically identical to the 2026-05-30 investigation (docs/06, Gap #16).

---

## Capture Quality With Preloaded Agent (Run C2)

When the workaround is used (`-javaagent`), the capture is **complete and correct**:

| Artifact | Present |
|---|---|
| `mockitotest/Calculator$MockitoMock$IxJ4Vz1U` bytecode | ✓ (3485 bytes, `kind=original`) |
| `mockitotest/Calculator$MockitoMock$...$auxiliary$bxvrT1cK` bytecode | ✓ |
| `mockitotest/Calculator$MockitoMock$...$auxiliary$GMsPtbuy` bytecode | ✓ |
| `mockitotest/Calculator` retransform + final bytecode | ✓ |
| `mockitotest/RealCalculator` retransform + final bytecode | ✓ (grows from 854 → 2586 bytes post-spy instrumentation) |
| `invokeinterface` dispatch: `MockitoTest.main → Calculator$MockitoMock$...` | ✓ (3 call sites) |
| `invokevirtual` dispatch: spy `MockitoTest.main → RealCalculator.add/multiply` | ✓ |
| `export_summary.complete = true` | ✓ |
| Diagnostics | 13 (normal JDK internals, 0 from mockitotest classes) |

The `RealCalculator` bytecode expansion (854 → 2586 bytes) is visible via the retransform chain:
ByteBuddy rewrites `RealCalculator` to inline mock interception logic for the spy.

---

## Current Verdict

**This is a validation/setup issue, not a capture architecture bug.**

The hang is caused by the combination of:
1. ByteBuddy's external subprocess attach path (not `VirtualMachine.attach()` directly)
2. Provenance class load hooks creating enough overhead to delay safepoints
3. The attach subprocess timing out while waiting for the target JVM

The capture architecture itself is correct. With the preloaded agent, Mockito mocks and spies
are fully captured: bytecode transformations, mock class identity, dispatch edges to mock
methods, and spy-proxied real method calls.

---

## Is This a Blocker?

**No** — for any of the following contexts:

| Use case | Impact |
|---|---|
| Demo with capture_live.sh (demo-runtime-truth) | Not affected — no Mockito |
| ManyCore 15-case regression | Not affected — no Mockito |
| Spring Boot capture | Not affected — Spring Boot doesn't use Mockito inline mock in production |
| Test-suite validation with Mockito | Requires `-javaagent:byte-buddy-agent.jar` |
| Staticization demo | Not affected |

---

## Workaround (Unchanged From 2026-05-30)

```bash
BBA=$HOME/.m2/repository/net/bytebuddy/byte-buddy-agent/1.14.15/byte-buddy-agent-1.14.15.jar

SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=out.jsonl \
SOROUSH_USER_PREFIXES="com/example" \
  <custom-java> -Xint \
  -javaagent:"$BBA" \
  -cp "..." \
  com.example.Main
```

The `-javaagent` flag makes `ByteBuddyAgent.isInstalled()` return true at class-init time,
so `installExternal()` is never invoked and the hang cannot occur.

---

## What a Future Fix Would Require

The hang could be eliminated by reducing per-class-load overhead enough that safepoints are
serviced within the attach timeout (~10s). Options, in order of complexity:

1. **Async class load export** — move graph node/edge writes to a background thread, letting the
   main thread's class load path return quickly. Requires thread-safe queue between class load
   hook and exporter. Moderate risk: ordering guarantees, shutdown flush.

2. **Deferred bytecode record emission** — buffer class load records and flush at end-of-run
   rather than synchronously during load. Already partially done (bytecode dump is
   `artifact_bytes_dumped=false` by default). Extend to graph node writes.

3. **Safepoint polling exemption** — not possible from user code; would require JVM patch.

None of these are required for the current demo or validation suite. The `-javaagent` workaround
is stable and has zero cost on correctness.

---

## Evidence Files

| File | Contents |
|---|---|
| `/tmp/mockito-attach-test/runA_stderr.txt` | Run A: provenance + -Xint + dynamic attach hang, thread dump, graph summary |
| `/tmp/mockito-attach-test/runA2_stderr.txt` | Run A2: provenance + no -Xint + dynamic attach hang, thread dump |
| `/tmp/mockito-attach-test/runC_out.jsonl` | Run C: preloaded agent, complete JSONL export |
| `/tmp/mockito-attach-test/runC2_out.jsonl` | Run C2: preloaded agent + user prefix, dispatch edges present |
