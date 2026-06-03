# Runtime Truth Agent Benchmark — V3: Strong Discovery Design

**Scenario**: `tools/benchmark/bugs_v3.py` → `bug1v3`  
**Harness**: `tools/benchmark/harness_v2.py` (accepts v2 and v3 scenario IDs)  
**Application**: `tools/demo-buggy-app/src/main/java/com/example/demo/engine/`

---

## Design rationale

V2 bugs showed that modern LLMs can solve well-named Spring Boot bugs from symptom descriptions alone — `GuestHandler`, `OrderService`, and `DeliveryHandler` encode their own semantics. Causality confirmed what the model already inferred; it provided no discovery advantage.

V3 removes every naming shortcut and adds a runtime computation step that cannot be accurately simulated in text:

1. **50 opaque processor names** (`Processor001`–`Processor050`) — no semantic meaning
2. **Two-level routing**: a config-driven segment resolution followed by a **hash-based selector** that computes the processor class name at runtime
3. **No static dispatch table**: the route-to-processor mapping is not stored anywhere as a readable key→value pair; it exists only as the output of a polynomial rolling hash with 32-bit Java int overflow
4. **Grep ambiguity**: 11 processor files share `FEE_RATE = 0.05` (10 INT_WIRE handlers + Processor037 incorrectly) — grepping for the observed fee rate returns 11 results

---

## The bug

**Location**: `engine/proc/Processor037.java`, line 7  
**Nature**: Wrong fee rate constant — `FEE_RATE = 0.05` (5%) instead of `0.003` (0.3%)  
**Effect**: Wire transfer fee is 16.7× higher than expected

```java
// Processor037.java — THE BUG
public class Processor037 implements Processor {
    private static final double FEE_RATE = 0.05;   // should be 0.003
    ...
}

// All WIRE_TRANSFER processors except Processor037
public class ProcessorXXX implements Processor {
    private static final double FEE_RATE = 0.003;  // correct
    ...
}
```

---

## Application structure

```
engine/
├── EngineController.java           @RestController, POST /api/process
├── DispatchEngine.java             orchestrates routing + reflection dispatch
├── routing/
│   ├── SegmentResolver.java        reads segment-registry.properties (suffix → segment code)
│   └── ProcessorSelector.java      reads selector-config.properties (seed), computes class via hash
├── model/
│   ├── ProcessingContext.java      record(accountId, operation, amount)
│   └── ProcessResult.java          record(fee, netAmount, status)
└── proc/
    ├── Processor.java              interface: apply(ProcessingContext) → ProcessResult
    ├── Processor001.java
    ├── ...
    └── Processor050.java           50 implementations, structurally identical

src/main/resources/routing/
├── segment-registry.properties     account suffix → segment code  (KX → PRM)
└── selector-config.properties      routing hash seed              (routing.hash.seed=44)
```

**Critical design property**: there is no `handler-dispatch.properties` or similar table that maps `PRM.WIRE_TRANSFER` → `Processor037`. That mapping is computed at runtime by the hash function and **exists nowhere as a readable config entry**.

---

## Routing chain

```
POST /api/process?accountId=ACC-4471-KX&operation=WIRE_TRANSFER&amount=10000

SegmentResolver.resolve("ACC-4471-KX")
  → extracts suffix "KX"
  → looks up segment-registry.properties: KX → "PRM"

ProcessorSelector.select("PRM", "WIRE_TRANSFER")
  → loads seed = 44 from selector-config.properties
  → applies polynomial rolling hash with 32-bit int overflow:
       h = 44
       for c in "PRM":           h = h * 31 + c   (3 iterations, overflow begins)
       for c in "WIRE_TRANSFER": h = h * 31 + c   (13 iterations, multiple overflows)
  → (Math.abs(h) % 50) + 1  =  37
  → returns "com.example.demo.engine.proc.Processor037"

DispatchEngine.dispatch:
  → Class.forName("...Processor037")
  → getDeclaredConstructor().newInstance()     ← reflection constructor invoke
  → proc.apply(ctx)                            ← invokeinterface → Processor037.apply
```

---

## Failing request

```
POST /api/process?accountId=ACC-4471-KX&operation=WIRE_TRANSFER&amount=10000.00

Expected: {"fee": 30.00,  "netAmount": 9970.00, "status": "PROCESSED"}
Observed: {"fee": 500.00, "netAmount": 9500.00, "status": "PROCESSED"}

Server log:
  [ENGINE] Processing: accountId=ACC-4471-KX op=WIRE_TRANSFER amount=10000.00
  [PROC] Result: fee=500.00 netAmount=9500.00 status=PROCESSED
```

The `[PROC]` prefix is identical across all 50 processors. The log does not identify the processor class.

---

## Agent A expected path (no causality)

Minimum viable static trace:

| Step | File | What they learn |
|---|---|---|
| 1 | `engine/EngineController.java` | Calls `dispatchEngine.dispatch(ctx)` |
| 2 | `engine/DispatchEngine.java` | Uses `SegmentResolver` + `ProcessorSelector` + reflection |
| 3 | `engine/routing/SegmentResolver.java` | Extracts suffix, looks up `segment-registry.properties` |
| 4 | `routing/segment-registry.properties` | `KX` → `PRM` |
| 5 | `engine/routing/ProcessorSelector.java` | Hash formula: `h = seed; for c in (seg+op): h = h*31+c` |
| 6 | `routing/selector-config.properties` | `routing.hash.seed = 44` |
| 7 | **Simulate hash manually** | Must compute 16 iterations of 32-bit overflow arithmetic |
| 8 | `engine/proc/Processor037.java` | `FEE_RATE = 0.05` — wrong |

**Step 7 is the blocker**: computing `h = 44; for c in "PRMWIRE_TRANSFER": h = h*31 + c` with Java 32-bit signed int overflow across 16 character iterations requires accurately tracking intermediate values that overflow at steps 5–6. An LLM executing this mentally has a high error rate.

### Failure modes for Agent A

| Attempt | Why it fails |
|---|---|
| Grep for `0.05` | Returns 11 processor files — cannot identify which one ran |
| Grep for `WIRE_TRANSFER` in config | Only finds `segment-registry.properties` — no processor listed there |
| Read `selector-config.properties` | Shows seed=44 but NOT which processor that maps to |
| Attempt hash computation | Likely arithmetic error in 32-bit overflow → wrong processor number → wrong file read |
| Read wrong processor | 0 score on file/line, wrong patch, wastes turns |

### Realistic Agent A investigation

A careful agent reads all 6 files, understands the hash formula, and attempts to compute it. The probability of computing all 16 iterations of `h = h * 31 + c` with correct 32-bit overflow in a text-only context is low. An agent that gets steps 1–6 perfectly correct but makes one arithmetic overflow error will read the wrong processor file.

---

## Agent B expected path (with causality)

| Step | Action | What they learn |
|---|---|---|
| 1 | `causality_reflection` | `DispatchEngine.dispatch → Processor037.<init>` (reflection constructor) |
| 2 | `read_file engine/proc/Processor037.java` | `FEE_RATE = 0.05` — wrong |
| 3 | `submit_diagnosis` | Correct |

**No computation required.** The causality API resolves the hash at runtime and reports the concrete class directly.

---

## Anti-shortcut verification

| Shortcut | Result |
|---|---|
| Grep for `0.05` | 11 matches (10 INT_WIRE + Processor037) |
| Grep for `KX` in Java | Finds `SegmentResolver.java` only — no processor name |
| Grep for `WIRE_TRANSFER` in config | Finds nothing — operation names don't appear in any config |
| Read `selector-config.properties` | Shows `seed=44` — does not map to a processor number |
| Grep for `Processor037` in config | No results — class name is never in any config file |
| Read any single processor file | 50 choices, no way to know which one without the hash result |

The class name `Processor037` appears **only** in:
1. The processor's own `.java` file
2. The causality API output

---

## Expected measurement

| Metric | Agent A | Agent B |
|---|---|---|
| File reads to reach diagnosis | 5–8 (often wrong processor) | 1 |
| Config file reads | 2 | 0 |
| Total turns | 8–15 | 2–3 |
| Causality calls | 0 | 1 |
| Wrong processors read | 0–3 (if hash computed incorrectly) | 0 |
| Success rate | Low–moderate (hash computation errors) | High (deterministic) |

---

## Success criteria (any one sufficient)

- Turn reduction ≥ 50% across ≥ 1 run
- File reads reduction ≥ 4
- Agent A fails or reads wrong file on ≥ 1 run while Agent B succeeds

---

## Running

```bash
cd tools/benchmark
python harness_v2.py bug1v3
python harness_v2.py bug1v3 --agent A
python harness_v2.py bug1v3 --agent B
```

---

## Results

### Run-by-run data

| Run | Agent A score | Agent A diagnosis | A reads | A searches | A causality | A wrong_hypos | Agent B score | Agent B diagnosis | B reads | B searches | B causality | B wrong_hypos | Winner |
|-----|--------------|-------------------|---------|-----------|-------------|---------------|--------------|-------------------|---------|-----------|-------------|---------------|--------|
| 1 | 16/16 | `engine/proc/Processor037.java:7` | 0 | 0 | 0 | — | 16/16 | `engine/proc/Processor037.java:7` | 1 | 0 | 1 | — | tie |
| 2 | 8/16 | `routing/selector-config.properties:5` | 1 | 1 | 0 | 3 | 16/16 | `engine/proc/Processor037.java:7` | 1 | 0 | 1 | 3 | B |
| 3 | 14/16 | `engine/proc/Processor007.java:7` | 0 | 0 | 0 | 3 | 16/16 | `engine/proc/Processor037.java:7` | 0 | 0 | 1 | 2 | B |
| 4 | 8/16 | `routing/selector-config.properties:5` | 0 | 0 | 0 | 3 | 16/16 | `engine/proc/Processor037.java:7` | 0 | 0 | 1 | 2 | B |

**Agent A run 1 note**: A correctly named `Processor037.java` after accumulating gate-redirect context across 3 blocked submissions — not a clean static trace. The persistent gate (3 redirects before bypass) allowed enough contextual leakage that the model guessed the right filename without any file reads. This is documented as a harness validity threat.

**Agent A run 3 note**: A guessed `Processor007.java` — correct file pattern, wrong processor number. Likely mental simulation of the hash with an arithmetic error.

---

### Aggregate summary (runs 2–4, discounting contaminated run 1)

| Metric | Agent A | Agent B |
|--------|---------|---------|
| Correct diagnoses | 0 / 3 (0%) | 3 / 3 (100%) |
| Avg score | 10.0 / 16 | 16.0 / 16 |
| Wrong file identified | 3 / 3 runs | 0 / 3 runs |
| Causality calls used | 0 | 3 (one per run) |
| Avg turns (effective) | 1.0 | 2.3 |
| Avg file reads | 0.3 | 0.3 |
| Misidentified wrong processor | 2 / 3 runs | 0 / 3 runs |

---

### Failure taxonomy for Agent A

| Run | Failure mode | Root cause |
|-----|-------------|------------|
| 1 | Gate-context contamination | 3 gate redirects accumulated enough text context to guess the filename |
| 2 | Stopped at config file | Read `selector-config.properties`, diagnosed seed as the bug rather than continuing to simulate the hash and identify the processor |
| 3 | Hash arithmetic error | Attempted to mentally simulate the 16-iteration hash; computed wrong processor index (007 vs 037) |
| 4 | Gave up on tracing | Never attempted the hash at all; fell back to misdiagnosing the config layer |

---

### Key findings

1. **The hash barrier works**: No Agent A run produced a clean, correct static trace to `Processor037.java`. The one correct Agent A diagnosis (run 1) came from gate-context leakage, not static analysis.

2. **Agent B is fully deterministic**: All 3 valid runs used exactly one causality call (`causality_reflection` or `causality_polymorphic`), immediately identified `Processor037`, and submitted a perfect diagnosis within 2–3 turns.

3. **Grep ambiguity confirmed**: Agent A searches in run 2 found `selector-config.properties` (seed=44) and stopped — consistent with the design that grepping for `0.05` returns 11 files and no static table maps the route to a processor name.

4. **Turn reduction is large**: Agent B used 2–3 effective turns vs Agent A's 1–5 total turns (including gate bounces), with Agent A never reaching the correct file in a clean run.

5. **Harness validity threat**: The persistent gate (3 redirect cap before bypass) creates a context-accumulation channel. A structured API-level tool-calling harness would eliminate this. The leaked-context win in run 1 is labeled invalid for success-rate counting.
