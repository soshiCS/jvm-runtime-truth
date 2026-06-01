# Runtime Truth Platform vs. Undo LiveRecorder — Capability Comparison

**Purpose**: Determine whether Undo can solve the V5 benchmark scenario, compare
workflows honestly, and identify where each platform has a genuine advantage.

**Method**: Feature analysis from public documentation + hands-on V5 data. Undo
was not run directly (Linux-only; development environment is macOS). Reasoning
is based on documented capabilities and known JVM instrumentation behavior.

---

## Part 1 — Undo LiveRecorder: Platform Facts

### Supported Environments

| Requirement | Undo LiveRecorder |
|-------------|-------------------|
| OS for recording | **Linux only** (x86_64 + ARM64) |
| OS for replay/IDE | Windows, macOS, Linux |
| JDK support | 8, 11, 17, 21, 25 (OpenJDK, Zulu, Oracle) |
| V5 dev environment | macOS aarch64 — **cannot record** |
| V5 app (Spring Boot) | Can record on Linux if ported |

**Critical limitation for this evaluation**: Undo cannot record Java programs
on macOS. The V5 demo runs on macOS. A direct head-to-head test is not
possible without first deploying V5 on a Linux VM.

### What Undo Is

Undo LiveRecorder is a **time-travel debugger**. It records the full execution
of a Java process (every instruction, memory write, and system call), then
lets the developer replay that recording forwards and backwards in an IDE or
via command-line (`udb`). The model is:

```
reproduce bug → record → share recording → replay in IntelliJ → step backwards → find cause
```

This is fundamentally different from the Runtime Truth model:

```
bug occurs → query runtime graph → get attributed causality → fix
```

---

## Part 2 — Could Undo Solve V5?

### V5 scenario recap

The decryption service returns `DECRYPTION_FAILED`. The bug is `bipush 75`
in `TransformBase$Transform037$VHz0cN3d.getKey()` — a ByteBuddy-generated
class with no `.java` source file. The active transform was selected by a
hash that includes `System.nanoTime()` at class load time.

### Undo workflow for V5 (hypothetical, Linux deployment)

**Step 1**: Record the failing request.

```bash
lr-java -- java -jar demo-runtime-truth.jar  # start with recording enabled
# send the decrypt request via curl
# stop recording
```

**Step 2**: Load the recording in IntelliJ. Find `DECRYPTION_FAILED`.

**Step 3**: Set a breakpoint at `DecryptController.decrypt()`. Navigate to it
in the recording.

**Step 4**: Step forward until the wrong output is produced. The XOR loop in
`TransformBase.applyTransform()` applies `data[i] ^= getKey()`. The return
value of `getKey()` is 75.

**Step 5**: Step backwards from `getKey()` return. The method body is a single
bytecode: `bipush 75; ireturn`. This constant is **baked in** — there is no
variable to trace back. The time-travel chain ends here.

**Step 6**: Find where the constant came from. The developer must now look at
when the class was loaded. Undo records class loading, so in principle the
developer could navigate backwards to the point where `TransformCompiler.doCompile()`
called `FixedValue.value(keyValue)` with `keyValue = 75`.

**Step 7**: From `doCompile`, step backwards to see that `keyValue` came from
`transformKeys.getProperty("transform.037")` returning `"75"`.

**Step 8**: Cross-reference the config file `transform-keys.properties` line 40.

### Assessment: Can Undo solve V5?

**Yes, in principle** — but with important caveats:

1. **Requires interactive debugging**. Undo requires the developer to replay
   the execution manually. There is no API that returns "which class was
   selected" as structured data.

2. **The ByteBuddy constant requires backwards navigation across class
   generation**. The developer must trace the recording back through ByteBuddy
   framework code (potentially hundreds of frames) to find where `keyValue=75`
   originated. This is doable but non-trivial.

3. **The generated class name (`Transform037`) IS visible** in the Undo
   debugger when `getKey()` executes. A developer could read the class name
   and directly look up `transform.037` in the config. This would be the fast
   path — essentially the same as the ManyCore path, achieved via different means.

4. **No AI agent integration**. Undo's interface is an IntelliJ plugin or the
   `udb` command-line debugger. It does not expose REST APIs for programmatic
   access. An AI agent cannot call Undo directly.

5. **INSTANCE_TOKEN is not a barrier for Undo**. In a recorded execution, the
   exact value of `System.nanoTime()` is preserved. Undo could replay the
   composite hash and determine the transform index directly. The barrier that
   blocks Agent A (in the ManyCore benchmark) does not block a time-travel
   debugger.

### Estimated Undo step count for V5

| Step | Action | Notes |
|------|--------|-------|
| 1 | Record the failing request on Linux | Requires Linux deployment, recording overhead |
| 2 | Load recording in IntelliJ | |
| 3 | Navigate to `getKey()` return | Find the 75 value |
| 4 | Read class name `TransformBase$Transform037$...` | Class name visible in debugger |
| 5 | Infer `transform.037` from class name | Optional: can also trace backwards through ByteBuddy |
| 6 | Open `transform-keys.properties`, find line 40 | |
| 7 | Change `75 → 127` | |

**Total**: 7 steps, interactive debugger session, Linux recording required.

---

## Part 3 — Capability Comparison

### Per-Capability Table

| Capability | Runtime Truth | Undo LiveRecorder | Notes |
|---|---|---|---|
| **Reflection target attribution** | ✓ Automatic — REST API returns callsite→target | ✓ Via recording — navigate to invocation, inspect frame | Both see it; ManyCore is structured/queryable |
| **Polymorphic dispatch attribution** | ✓ `callsite_target` records per invocation | ✓ Step to invocation, inspect type | Both see it |
| **Generated class identification** | ✓ Class name in `generated_class` record; `artifact_javap` API | ✓ Class name visible in debugger at invocation | Both see it; ManyCore delivers it as data |
| **Hidden class visibility** | ✓ Explicit `hidden_class_identity` records with CRC | ✓ Hidden classes appear in debugger call stack | Both see it; ManyCore attributes provenance (LambdaMetafactory, invokedynamic) |
| **Runtime target attribution** | ✓ Per-callsite attribution across full execution | ✓ Same, via replay | Both; ManyCore is queryable without replay |
| **Generated bytecode inspection** | ✓ `artifact_javap` REST API — baked constants visible | ✓ Debugger decompiler shows class; stepping reveals values | Both; ManyCore is API-accessible |
| **Artifact retrieval** | ✓ `artifact_path` in JSONL — `.class` file on disk | ✗ No explicit artifact export API | ManyCore saves artifacts explicitly |
| **Loader attribution** | ✓ `loader_id` per class in JSONL | Partial — visible in debugger, not as structured data | ManyCore is more explicit |
| **Causality chain reconstruction** | ✓ `causality_chain` REST endpoint | Manual — navigate recording step by step | ManyCore is automatic; Undo requires interactive navigation |
| **AI-agent friendliness** | ✓ Structured REST APIs; designed for programmatic access | ✗ Interactive debugger; not API-accessible | **ManyCore's primary differentiator** |
| **Time-travel debugging** | ✗ | ✓ Core feature — step forwards and backwards | **Undo's primary differentiator** |
| **Memory state debugging** | ✗ | ✓ Full memory history — see any variable at any point | Undo advantage |
| **Reverse execution** | ✗ | ✓ Step backwards instruction by instruction | Undo advantage |
| **Deterministic replay** | ✗ | ✓ Recorded execution is fully deterministic | Undo advantage |
| **Cross-request correlation** | ✓ JSONL annotated per-callsite across full process run | Partial — one recording per session | ManyCore advantage for multi-request analysis |
| **macOS support** | ✓ | ✗ Recording requires Linux | ManyCore advantage for dev environments |
| **Structured data output** | ✓ JSONL + REST | ✗ Interactive only | ManyCore advantage for tooling integration |
| **Recording overhead** | None — JVM instrumentation at load time | Significant — records every instruction | ManyCore advantage for production use |

---

## Part 4 — Capabilities Undo Has That We Do Not

| Capability | Why It Matters |
|---|---|
| **Time-travel / reverse execution** | Can step backwards from a wrong value to where it was set. Critical for bugs where the cause and effect are far apart in execution order. |
| **Full memory history** | At any point in the recording, Undo can show the exact heap state. ManyCore records class/method attribution, not data values. |
| **Variable value inspection** | Can see the value of `keyValue` at the moment `FixedValue.value(keyValue)` was called. ManyCore only sees that the call happened. |
| **Deterministic replay** | The same bug can be replayed identically for collaborative investigation. ManyCore's live capture is not replayable. |
| **Root-cause precision** | For bugs where the control path is not the question (e.g., "why did this variable get this value?"), Undo provides a complete answer. |

---

## Part 5 — Capabilities We Have That Undo Does Not

| Capability | Why It Matters |
|---|---|
| **API-accessible causality graph** | AI agents can call REST endpoints and receive structured JSON. Undo requires a human or an IntelliJ plugin — there is no API. |
| **Explicit artifact export** | The bytecode of every generated and hidden class is written to disk as a `.class` file. Undo does not export artifacts. |
| **Provenance attribution** | The JSONL records which `invokedynamic` site generated each hidden lambda class, including the exact `indy_trace_id`. Undo does not expose this. |
| **Zero recording overhead in production** | ManyCore instruments at class load time. There is no per-execution recording cost. Undo's recording imposes significant overhead. |
| **macOS recording** | Works on macOS. Undo cannot record on macOS. |
| **No replay required** | ManyCore captures all data during normal execution. No special reproduction step is needed to get attribution data. |
| **Per-request JSONL scoping** | Attribution is captured during the normal request lifecycle. Undo requires deliberate reproduction inside a recording session. |

---

## Part 6 — What Datadog and Dynatrace Can Do

For completeness: how do APM/observability platforms compare?

| Capability | Datadog APM | Dynatrace | Runtime Truth |
|---|---|---|---|
| Generated class visibility | ✗ Shows method names; generated class name may appear | ✗ Similar | ✓ |
| Hidden class visibility | ✗ | ✗ | ✓ |
| Bytecode inspection | ✗ | ✗ | ✓ |
| Causality chain | Partial — flame graph shows call stack | Partial — PurePath traces stack | ✓ Attributed per-callsite |
| AI-agent integration | ✓ REST APIs | ✓ REST APIs | ✓ REST APIs |
| Artifact export | ✗ | ✗ | ✓ |

**APM tools show WHAT ran** (via sampling or instrumentation). They do not
show WHICH hidden class was selected for a specific invokedynamic, or WHAT
constant was baked into a ByteBuddy class. The V5 scenario requires both.

---

## Part 7 — Final Verdict

### 1. Could Undo solve V5?

**Yes, for a human debugger with Linux access.** An experienced developer using
Undo on a Linux deployment of V5 would:
- See `Transform037` in the class name at the `getKey()` call
- Optionally trace backwards to confirm `keyValue=75` came from the config
- Identify line 40 of `transform-keys.properties`

Estimated time: 15–30 minutes for a developer familiar with Undo. The `Transform037`
class name is sufficient — a developer would not need to trace through ByteBuddy
generation once they see the class name.

**Undo CANNOT solve V5 for an AI agent** — there is no API. The benchmark
comparison (Agent A vs. Agent B) is irrelevant for Undo because Undo does not
have an agent interface.

### 2. Could Datadog/Dynatrace solve V5?

**Unlikely with standard tooling.** APM platforms would show that `applyTransform`
was called and returned incorrect bytes. They would not show which generated class
was selected or what constant was baked into it. The developer would still face the
"10 entries with key=75" problem with no way to identify the active one.

**With custom Datadog APM instrumentation** (manual spans + custom attributes),
a developer could log the selected class name. But this requires modifying the
application code — it is not automatic.

### 3. Is V5 demonstrating runtime reconstruction, time-travel, or both?

**Runtime reconstruction only.** V5 demonstrates the platform's ability to
attribute what class was instantiated for a specific request and inspect its
bytecode. It does not demonstrate time-travel capability — the platform does
not support reverse execution or full execution replay.

This is a different category from what Undo provides. The comparison should be:

- Undo: "When did the bug happen and what was the state?" → time-travel
- Runtime Truth: "What code actually ran for this request?" → causality attribution

These answer different questions and are complementary, not directly competing.

### 4. What is the strongest unique capability demonstrated by the platform today?

**Structured causality attribution for AI agents.** No other tool provides:
1. Per-request attribution of generated and hidden classes via REST API
2. Exported `.class` artifacts for any generated class, programmatically accessible
3. A graph structure linking callsites to concrete targets that an AI can query directly

Undo has richer execution context (values, memory, reverse execution) but provides
it only through an interactive debugger. The Runtime Truth platform provides less
execution context but delivers it as structured data that AI agents can consume.

The benchmark result — Agent B solves V5 in 3 REST calls while Agent A reads
7 source files and still fails — demonstrates this advantage directly.

### 5. What remains unproven?

| Claim | Status |
|-------|--------|
| Platform works on production traffic at scale | Unproven — tested on demo apps only |
| Recording overhead is acceptable in production | Unproven — -Xint flag significantly slows execution |
| AI agents reliably use the causality API | Partially proven — 3/3 for the V5 scenario |
| Advantage holds on real bugs (not constructed scenarios) | Unproven — all bugs are benchmarks |
| Causality attribution is complete (no missed callsites) | Partially proven — 15/15 on test cases |
| Advantage vs. Undo on non-generated-class bugs | Unproven — V5 was designed to favor ManyCore |

**The honest summary**: V5 demonstrates a real workflow advantage for the specific
case of AI-assisted debugging of runtime-generated code. Whether this generalizes
to production debugging workflows requires more evidence.

---

## Running Undo on V5 (When Linux Is Available)

If a Linux deployment is available:

```bash
# On Linux with Undo LiveRecorder installed:
lr-java --output=/tmp/v5_recording.undo \
  -- java -jar demo-runtime-truth.jar \
       --server.port=8080

# In parallel terminal:
curl -X POST "http://localhost:8080/api/decrypt?sessionKey=550e8400-..." \
     -H "Content-Type: application/json" \
     -d '{"ciphertext":"2D2A312B36323A5F2C3730282C5F2B373A5F2B2D2A2B37"}'

# Stop recording (Ctrl+C or kill)
# Load in IntelliJ via Undo plugin, or:
uload /tmp/v5_recording.undo
```

Suggested breakpoints:
1. `TransformBase.applyTransform` — see `getKey()` returning 75
2. `TransformCompiler.doCompile` bci=73 — see which class was instantiated
3. Read `com/example/truth/handler/TransformBase$Transform037$...` from debugger

Expected result: Undo would show the `Transform037` class name and the 75 value.
The developer would correlate to `transform-keys.properties:40` without needing
the `artifact_javap` tool.
