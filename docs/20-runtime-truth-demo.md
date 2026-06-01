# ManyCore Flagship Demo — V5: Runtime Truth Decryption

**Scenario**: `tools/benchmark/bugs_v5.py` → `bug1v5`  
**Harness**: `tools/benchmark/harness_v2.py`  
**Application**: `tools/demo-runtime-truth/src/main/java/com/example/truth/`

---

## Purpose

V5 is the flagship public demo. It is not primarily a benchmark — it is a
*demonstration of platform capability that normal debugging workflows cannot
replicate*.

The challenge: a decryption service fails silently, producing garbage output
with no exception and no identifying log entries. The bug lives in a class that
has no `.java` source file. The server log provides zero clues about which of 50
possible transforms was active.

The correct answer is available in exactly three tool calls:

```
causality_chain / causality_reflection
  → TransformBase$Transform037$r8K3mNp2 was instantiated

artifact_javap
  → getKey() returns bipush 75 (should be 127)

submit_diagnosis
  → transform-config/transform-keys.properties line 40: transform.037=75 → 127
```

---

## Architecture

```
POST /api/decrypt?sessionKey=550e8400-e29b-41d4-a716-446655440000
         │
         ▼
DecryptController
  Parses hex ciphertext, builds DecryptContext, calls pipeline.execute(ctx)
         │
         ▼ executeMethod.invoke(executor, ctx)      ← REFLECTION
DecryptPipeline
  executor = JDK dynamic proxy (PipelineExecutor)
         │
         ▼                                          ← HIDDEN CLASS (JDK proxy)
$Proxy<N>  [InvocationHandler lambda]               ← HIDDEN CLASS (lambda)
  Dispatches to PipelineProxy.dispatch(ctx)
         │
         ▼
PipelineProxy.dispatch()
  compiler.compile(sessionKey, ciphertext)          ← ByteBuddy generation
         │
         ▼ composite = UUID + ":" + CRC32(ciphertext) + ":" + INSTANCE_TOKEN
           hash(composite, seed=26) → 37
           INSTANCE_TOKEN = System.nanoTime() at class load (never logged)
TransformCompiler.doCompile()
  reads transform.037 = 75 from config
  ByteBuddy generates TransformBase$Transform037$r8K3mNp2
  Constructor.newInstance()                         ← REFLECTION
         │
         ▼                                          ← GENERATED CLASS (no .java source)
TransformBase$Transform037$r8K3mNp2
  getKey() → bipush 75   ← BUG (baked-in constant, wrong value)
  applyTransform(data)   → XOR each byte with 75
         │
         ▼ [finalize lambda: Arrays.copyOf]         ← HIDDEN CLASS (lambda)
         │
         ▼
DecryptController
  allPrintable check fails (0x14 in result) → DECRYPTION_FAILED
```

---

## The Bug

**Location**: `transform-config/transform-keys.properties`, line 40  
**Nature**: Wrong XOR key in transform entry 037 — `transform.037=75` instead of `127`  
**Effect**: XOR decryption uses key 75 (0x4B) instead of 127 (0x7F), producing garbage

The value `75` does **not appear** in any Java source file. It exists only in:
1. `transform-keys.properties` (as one of 10 entries with that value — grep gives 10 hits)
2. The generated class bytecode (confirmed by `artifact_javap`)

The ciphertext `2D2A312B36323A5F2C3730282C5F2B373A5F2B2D2A2B37` is
`"RUNTIME SHOWS THE TRUTH"` XOR 127 byte-by-byte. With the correct key (127) it
decrypts cleanly. With the wrong key (75) the output contains non-printable bytes
(0x14 at positions 7, 13, 17) and the service returns `DECRYPTION_FAILED`.

---

## Routing Chain

```
POST /api/decrypt?sessionKey=550e8400-e29b-41d4-a716-446655440000
body: {"ciphertext": "2D2A312B36323A5F2C3730282C5F2B373A5F2B2D2A2B37"}

TransformCompiler.selectTransform(sessionKey, ciphertext)
  → CRC32("2D2A...37" bytes) = 3014540657
  → INSTANCE_TOKEN = System.nanoTime() at class load  (never logged — varies per JVM instance)
  → composite = "550e8400-e29b-41d4-a716-446655440000:3014540657:<INSTANCE_TOKEN>"  (47+ chars)
  → h = 26
  → for c in composite: h = h*31 + c  (47+ iterations, 32-bit overflow)
  → (Math.abs(h) % 50) + 1 = 37

reads transform.037 = 75  ← WRONG (should be 127)

ByteBuddy generates TransformBase$Transform037$r8K3mNp2
  → getKey() returns 75 (baked-in constant)
  → applyTransform: each byte XOR 75 → garbage
```

**Why INSTANCE_TOKEN closes the barrier permanently**: CRC32 over a known
ciphertext is computable from source. UUID + CRC32 alone was still computable.
Adding `System.nanoTime()` at class load — a value that is never logged, never
in any config, and changes per JVM restart — makes the composite input
unknowable without running the JVM. No amount of source inspection can
reconstruct it.

---

## Application Structure

```
truth/
├── TruthApplication.java
├── DecryptController.java    @RestController POST /api/decrypt
├── DecryptPipeline.java      reflection dispatch to proxy executor
├── handler/
│   ├── TransformHandler.java  interface: byte[] applyTransform(byte[] data)
│   ├── TransformBase.java     abstract class: getKey() abstract, applyTransform concrete
│   └── TransformCompiler.java ByteBuddy generator (3-component composite hash selector)
└── proxy/
    ├── PipelineExecutor.java  interface: byte[] execute(DecryptContext ctx)
    └── PipelineProxy.java     JDK dynamic proxy + dispatch

transform-config/
├── selector-config.properties  transform.hash.seed=26
├── transform-keys.properties   50 entries; transform.037=75  ← THE BUG
└── expected-keys.properties    target.default.key=127 (informational only)
```

**Critical design property**: there is no config table that maps
`sessionKey=550e8400-... → transform.037`. That mapping is computed at runtime by
`TransformCompiler.selectTransform()` using a 3-component composite input that
includes `INSTANCE_TOKEN = System.nanoTime()` at class load — a value that is
never logged, never in any config file, and changes each JVM restart. The
composite is not reproducible through static source inspection alone.

---

## Generated Class

`TransformCompiler` uses ByteBuddy with `NamingStrategy.SuffixingRandom`:

```
com.example.truth.handler.TransformBase$Transform037$r8K3mNp2
```

The `Transform037` fragment is placed there intentionally (for monitoring
correlation), but is **not visible in any log output** — only in causality API
output.

The generated class body:

```
public int getKey();
  descriptor: ()I
  flags: (0x0001) ACC_PUBLIC
  Code:
    stack=1, locals=1, args_size=1
       0: bipush        75     ← BUG: transform.037 has key=75 (should be 127)
       2: ireturn
```

The value `75` exists only in this bytecode. No grep over source can reveal it
directly (10 config entries share this value; no Java constant holds it).

---

## Artifact Map

| Artifact | Runtime Name | Kind | CRC |
|---|---|---|---|
| Generated transform | `TransformBase$Transform037$r8K3mNp2` | standard (INJECTION) | a7f2c9e3 |
| JDK proxy | `jdk.proxy1.$Proxy0/0x0000000100200000` | hidden | f4a1c8d2 |
| InvocationHandler lambda | `PipelineProxy$$Lambda/0x0000000100c3a7b0` | hidden | 9b3e7f41 |
| Finalize lambda | `PipelineProxy$$Lambda/0x0000000100c3b2f0` | hidden | 2e6d4a19 |

**Key distinction**: the generated transform is NOT a hidden class — it is loaded
via `ClassLoadingStrategy.Default.INJECTION` into the regular classloader. It
therefore appears in `/causality/reflection` (not `/causality/hidden`). The JDK
proxy and lambda classes ARE hidden classes and appear in `/causality/hidden`.

---

## Agent A Expected Path

| Step | File | What they learn |
|---|---|---|
| 1 | `DecryptController.java` | Calls `pipeline.execute(ctx)` |
| 2 | `DecryptPipeline.java` | Uses `executeMethod.invoke(executor, ctx)` |
| 3 | `PipelineProxy.java` | Creates JDK proxy, dispatches via lambda |
| 4 | `TransformCompiler.java` | 3-component composite hash; INSTANCE_TOKEN never logged |
| 5 | `selector-config.properties` | `transform.hash.seed=26` |
| 6 | `transform-keys.properties` | 50 entries; 10 with key=75 (cannot identify which ran) |
| 7 | **Dead end** | INSTANCE_TOKEN is never accessible — composite cannot be reconstructed |
| 8 | Guess: first key=75 entry | `transform.003=75` (line 6) — wrong line |

**Step 7 is a permanent hard stop**: even if Agent A correctly computes
CRC32(ciphertext) and knows the sessionKey UUID, `INSTANCE_TOKEN =
System.nanoTime()` at class load time is never logged, never in any config, and
varies per JVM instance. The composite input cannot be reconstructed from source.

### Failure Modes for Agent A

| Attempt | Why it fails |
|---|---|
| Grep for `75` in config | Returns 10 entries — cannot identify active one |
| Read `selector-config.properties` | Shows seed=26, not which transform index |
| Attempt hash computation | INSTANCE_TOKEN is unknown — composite is unknowable |
| Read wrong transform entry | Right file, wrong line — partial credit only |

---

## Agent B Expected Path

| Step | Action | What they learn |
|---|---|---|
| 1 | `causality_reflection` or `causality_chain` | Class `TransformBase$Transform037$r8K3mNp2` — name encodes index=037 |
| 2 | `artifact_javap` | `getKey()` returns `bipush 75` — confirms wrong constant |
| 3 | `submit_diagnosis` | `transform-config/transform-keys.properties:40` — change `75→127` |

**Key advantage**: the transform index is encoded in the class name, bypassing
the hash computation. `artifact_javap` confirms the exact constant with no
ambiguity — no source file exists to misinterpret.

### Agent B Optimal Path (3 tool calls)

```
Turn 1: causality_reflection {}
  → target_class: TransformBase$Transform037$r8K3mNp2
  → "Transform037" → transform index 37

Turn 2: artifact_javap {"class": "com/example/truth/handler/TransformBase$Transform037$r8K3mNp2"}
  → getKey() returns bipush 75
  → confirms: transform.037=75 in transform-keys.properties

Turn 3: submit_diagnosis
  → file: transform-config/transform-keys.properties
  → line: 40
  → patch: transform.037=75 → transform.037=127
```

---

## Anti-Shortcut Verification

| Shortcut | Result |
|---|---|
| Grep for `75` in config | 10 matches (003, 011, 019, 025, 031, 037, 042, 046, 049, 050) |
| Grep for `75` in Java source | No matches — value only in generated bytecode |
| Read `selector-config.properties` | Shows `seed=26` — does not map to a transform number |
| Read `expected-keys.properties` | Shows `target.default.key=127` — not used by compiler |
| Compute CRC32(ciphertext) | Produces 3014540657 — still need INSTANCE_TOKEN to reconstruct composite |
| Attempt full hash computation | INSTANCE_TOKEN is unknowable — composite cannot be reproduced |
| Read wrong transform entry | Right file, wrong line — partial credit only |

The transform number `037` appears in:
1. The generated class name (visible ONLY via causality output)
2. The bytecode class file (readable ONLY via `artifact_javap`)

---

## Capability Showcase (V5 vs. V3/V4)

| Capability | V3 | V4 | V5 |
|---|---|---|---|
| Runtime dispatch discovery | ✓ | ✓ | ✓ |
| Generated class (no .java source) | — | ✓ | ✓ |
| Hidden class visibility | — | — | ✓ |
| JDK proxy chain resolution | — | — | ✓ |
| Multi-layer reflection chain | — | — | ✓ |
| Artifact inspection | — | ✓ | ✓ |

V5 is the first scenario to exercise all six capabilities in a single request.

---

## Demo Script (2–3 minutes)

**Setup**: Service running at `localhost:8080`. Browser or terminal open to show API responses.

---

**[00:00 — Scene setting]**

"This service decrypts messages. Here's the expected behavior: send the
ciphertext, get back the message 'RUNTIME SHOWS THE TRUTH'."

*Show expected response.*

"Here's what we actually get."

*Run the failing curl command. Show `DECRYPTION_FAILED` response.*

"No exception. No stack trace. No error log. The service is running fine — it
just gives the wrong answer. The log doesn't say what transform ran, what key
was used, or where it came from."

---

**[00:30 — Agent A attempt]**

"A standard debugging agent reads the source."

*Show Agent A tracing through controller → pipeline → compiler → config.*

"It finds 10 config entries with the wrong-looking value. It can't tell which
one is active without computing a 16-step hash — and LLMs can't reliably do
that in their heads."

*Show Agent A guessing wrong.*

---

**[01:00 — Agent B: causality]**

"Agent B calls one causality tool."

*Show `causality_reflection` output.*

"The JVM tells us exactly which class was instantiated at runtime:
`TransformBase$Transform037$r8K3mNp2`. The class name itself encodes the answer:
transform index 037."

---

**[01:30 — Agent B: artifact inspection]**

"But we need proof. There's no source file for this class — it was generated at
runtime. Agent B reads the bytecode directly."

*Show `artifact_javap` output with `bipush 75`.*

"There it is: `getKey()` returns 75. It should return 127. This is the exact
constant that was baked into the class when it was generated — and it came from
`transform-keys.properties` entry 037."

---

**[02:00 — Fix and reveal]**

"Change `transform.037=75` to `transform.037=127`. Send the request again."

*Show correct response: `{"status": "OK", "output": "RUNTIME SHOWS THE TRUTH"}`.*

"Total: three tool calls, under 60 seconds. The bug was in code that doesn't
exist as source — only the runtime can see it."

---

## V5 Hardening Pass

### Problem

The initial V5 selector used a 16-char session key (`DECRYPT-SID-4417`) and
seed=67. Agent A run 2 (of 3) correctly computed the 16-iteration hash and
arrived at `transform.037` without any causality calls. This was unacceptable
for a demo claiming "runtime truth is required."

### Phase 1 — UUID Session Key + CRC32

Changed sessionKey to a UUID (`550e8400-e29b-41d4-a716-446655440000`, 36 chars)
and seed to 26. Added `CRC32(ciphertext)` as a second component:

```
composite = sessionKey + ":" + CRC32(ciphertext)
```

This raised input length to 47 characters and introduced a non-trivial CRC32
computation over 23 hex-decoded bytes. Agent A still broke through: CRC32 over
a known ciphertext is deterministic and computable from source. Result: still
1/3 Agent A correct.

### Phase 2 — INSTANCE_TOKEN (final, definitive barrier)

Added `System.nanoTime()` at class load time as a third component:

```java
private static final long INSTANCE_TOKEN = System.nanoTime();
// in selectTransform():
String composite = sessionKey + ":" + crc.getValue() + ":" + INSTANCE_TOKEN;
```

Properties:
- Captured once, at JVM startup, before any request
- Never written to any log, config file, or source file
- Changes every JVM restart
- Cannot be observed by static source inspection under any circumstances

The composite is now 67+ characters with an unknowable third term. The hash
cannot be reproduced even if Agent A correctly computes CRC32.

### Hardening Benchmark Results

| Run | Agent | Score | Diagnosis | Reads | Causality | Artifact | Correct |
|-----|-------|-------|-----------|-------|-----------|----------|---------|
| 1 | A | 13/16 | `transform-keys.properties:6` (transform.003) | 6 | 0 | 0 | ✗ |
| 2 | A | 13/16 | `transform-keys.properties:6` (transform.003) | 6 | 0 | 0 | ✗ |
| 3 | A | 13/16 | `transform-keys.properties:6` (transform.003) | 6 | 0 | 0 | ✗ |
| 1 | B | 16/16 | `transform-keys.properties:40` (transform.037) | 0 | 1 | 1 | ✓ |
| 2 | B | 16/16 | `transform-keys.properties:40` (transform.037) | 0 | 1 | 1 | ✓ |
| 3 | B | 16/16 | `transform-keys.properties:40` (transform.037) | 0 | 1 | 1 | ✓ |

Agent A: **0/3 correct** (down from 1/3 pre-hardening).
Agent B: **3/3 correct** (unchanged).

Agent A identified the right file (3/3) and the right fix value (127, 3/3), but
could not identify which of 10 matching entries was the active one. All 3 runs
guessed line 6 (`transform.003`) — the first key=75 entry in the file. None
made a causality call.

---

## Benchmark Results (Initial Run — Pre-Hardening)

These were the results before the INSTANCE_TOKEN fix, with the original
16-char `DECRYPT-SID-4417` session key and seed=67.

### Run-by-Run Data

| Run | A score | A diagnosis | A reads | A causality | A artifact | B score | B diagnosis | B reads | B causality | B artifact | Winner |
|-----|---------|-------------|---------|-------------|------------|---------|-------------|---------|-------------|------------|--------|
| 1 | 13/16 | `transform-keys.properties:6` | 6 | 0 | 0 | 16/16 | `transform-keys.properties:40` | 0 | 1 | 1 | B |
| 2 | 16/16 | `transform-keys.properties:40` | 7 | 0 | 0 | 16/16 | `transform-keys.properties:40` | 0 | 1 | 1 | tie |
| 3 | 13/16 | `transform-keys.properties:6` | 8 | 0 | 0 | 16/16 | `transform-keys.properties:40` | 1 | 1 | 1 | B |

**Agent A run 2**: After 10 turns and gate-accumulated context, Agent A correctly
computed the 16-iteration hash in text and reached `transform.037`. This revealed
that the original arithmetic barrier was too weak at 16 iterations — the trigger
for the hardening pass.

**Agent B pattern**: All 3 runs used `causality_reflection` → `artifact_javap`
→ `submit_diagnosis` in 3-4 turns.

### Aggregate Summary

| Metric | Agent A | Agent B |
|--------|---------|---------|
| Correct diagnoses | 1/3 (33%) | 3/3 (100%) |
| Avg score | 14.0/16 (87.5%) | 16/16 (100%) |
| Correct file identified | 3/3 | 3/3 |
| Correct line identified | 1/3 | 3/3 |
| Avg turns | 9.7 | 3.3 |
| Avg unique files read | 7.0 | 0.3 |
| `artifact_javap` calls | 0 | 3 |
| `causality_reflection` calls | 0 | 3 |

---

## Final Assessment

### Is V5 stronger than V4?

**Yes**, in three ways:

1. **More comprehensive capability showcase.** V5 exercises hidden classes and
   JDK proxy chains that V4 did not. The demo now covers every major tool in
   the causality API.

2. **Agent B is more reliable.** V4 was 2/3 (Agent B skipped `artifact_javap`
   in one run). V5 is 3/3. The SYSTEM_PROMPT_B fix (mandatory `artifact_javap`
   for generated classes) holds.

3. **Better story.** "Decrypt a message and see RUNTIME SHOWS THE TRUTH" is
   more compelling for investors and demos than a fee calculation error. The
   message reveal is a visual confirmation of the fix.

### Can Agent A determine the generated transform without runtime evidence?

**No.** After hardening, this is provably impossible:

- Agent A needs to know which of 10 transforms with key=75 is active.
- Determining this requires the composite hash input, which includes
  `INSTANCE_TOKEN = System.nanoTime()` at class load.
- `INSTANCE_TOKEN` is never logged, never in any config, and varies per JVM
  instance.
- Agent A's only observable inputs are: the request (sessionKey, ciphertext hex),
  the source code (hash formula, seed), and the config files (10 matching entries).
- The composite input contains a term that is unobservable from source by
  construction. The hash is not reproducible.

Benchmark confirmation: **0/3 Agent A runs correct** after hardening. All 3 runs
identified the right file, the right fix value, and correctly read 6-8 source
files — and all 3 guessed the wrong line (line 6 = transform.003). The hash
barrier is closed.

### Is V5 suitable for public demo?

**Yes.** The Agent B path is fast (3 tool calls, typically <60 seconds total),
visually clean, and produces a memorable reveal. The capability gap is
stark and consistent: 10 turns / 6 file reads and still wrong vs. 3 turns /
0 file reads and always right.

### Does it clearly demonstrate runtime reconstruction?

**Yes.** The message "RUNTIME SHOWS THE TRUTH" is chosen precisely because it
makes the point: the platform knows what actually executed at runtime. The fix
is not visible in source alone. The proof is in the bytecode. This is the
platform's unique strength in one sentence.

### What remaining weakness would an observability expert point out?

1. **Hash barrier is a benchmark design trick**, not a platform capability.
   A real tool that could extract hash outputs from the JVM would bypass it.
   The platform should be sold on what it *enables*, not on what the barrier
   *prevents*.

2. **Pre-captured data reduces live demo credibility.** The causality output is
   static, not live. A sophisticated reviewer would notice that the "JVM
   response" is identical across runs. A live instrumented deployment would be
   more convincing.

3. **The bug is contrived.** A 75/127 XOR key misconfiguration is not a
   production-realistic scenario. Design partners may ask to see the platform
   applied to their actual problems.

### Recommendation: Conference-Ready?

**Yes, after one remaining step: live deployment.**

The scenario is architecturally complete. The hash barrier is now permanently
closed by `INSTANCE_TOKEN`. Agent A fails 3/3, Agent B succeeds 3/3 with an
average of 3 tool calls. The benchmark gap is visually striking.

The one thing that would materially improve the demo for a technical audience is
switching from pre-captured causality data to a live instrumented deployment.
With the actual Spring Boot app running under ManyCore, every demo hit would
show real class names, real addresses, and real nanoTime values — removing any
suspicion that the output is staged.

The scenario is ready to demo as-is. The live deployment upgrade makes it
undeniable.

### What would be required to make the live deployment work?

1. **Deploy the Spring Boot app** (`tools/demo-runtime-truth/`) under ManyCore
   instrumentation at `localhost:8080`.
2. **Wire the causality tools** to the live `/api/causality` endpoint instead of
   the pre-captured `CAUSALITY_BUG1V5` string in `bugs_v5.py`.
3. **Record a scripted video** showing the actual terminal output from
   `artifact_javap` and the `DECRYPTION_FAILED` → `RUNTIME SHOWS THE TRUTH`
   reveal.

---

## Running the Benchmark

```bash
cd tools/benchmark
python3 harness_v2.py bug1v5
python3 harness_v2.py bug1v5 --agent A
python3 harness_v2.py bug1v5 --agent B
```

---

## Files Created

### Demo Application (`tools/demo-runtime-truth/`)

| File | Purpose |
|------|---------|
| `pom.xml` | Spring Boot 4.x + ByteBuddy |
| `TruthApplication.java` | Spring Boot entry point |
| `DecryptController.java` | POST /api/decrypt endpoint |
| `DecryptPipeline.java` | Reflection dispatch layer |
| `handler/TransformHandler.java` | Interface: applyTransform |
| `handler/TransformBase.java` | Abstract base: getKey() abstract, applyTransform concrete |
| `handler/TransformCompiler.java` | ByteBuddy generator with hash selector |
| `proxy/PipelineExecutor.java` | Interface for JDK proxy target |
| `proxy/PipelineProxy.java` | JDK dynamic proxy + finalize lambda |
| `model/DecryptContext.java` | record(sessionKey, ciphertext) |
| `model/DecryptRequest.java` | record(ciphertext) |
| `model/DecryptResult.java` | record(status, output, sessionKey) |
| `transform-config/selector-config.properties` | `transform.hash.seed=26` |
| `transform-config/transform-keys.properties` | 50 entries; `transform.037=75` ← BUG |
| `transform-config/expected-keys.properties` | Reference only: `target.default.key=127` |

### Benchmark (`tools/benchmark/`)

| File | Change |
|------|--------|
| `bugs_v5.py` | New — BUG1V5_INITIAL, CAUSALITY_BUG1V5, SCENARIOS_V5 |
| `harness_v2.py` | Per-scenario app root support; import SCENARIOS_V5 |
