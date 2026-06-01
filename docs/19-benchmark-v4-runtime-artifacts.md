# ManyCore Agent Benchmark — V4: Runtime Artifact Inspection

**Scenario**: `tools/benchmark/bugs_v4.py` → `bug1v4`  
**Harness**: `tools/benchmark/harness_v2.py` (accepts v2, v3, v4 scenario IDs)  
**Application**: `tools/demo-buggy-app/src/main/java/com/example/demo/fee/`

---

## Design Rationale

V3 proved that a polynomial rolling hash barrier (16 iterations of 32-bit overflow) is a
reliable blocker for Agent A — the model cannot simulate the arithmetic correctly in text.
V3's advantage for Agent B was a single causality call identifying which static class ran.

V4 introduces a structurally harder problem: **the executed code has no `.java` source file
at all**. The bug exists in a ByteBuddy-generated class. Even an Agent who correctly
identifies *which* class ran cannot read its source — they must read the bytecode directly
using `artifact_javap`.

Additionally, V4 combines two barriers:

1. **Hash barrier** (inherited from V3): determining which profile was selected for
   `premium + TRANSFER` requires simulating 15 iterations of `h = h*31 + c` with Java
   32-bit signed int overflow. LLMs reliably fail this computation in text.

2. **Generated class barrier** (new in V4): the active class has no `.java` source. Only
   `artifact_javap` can reveal the constant baked into the `getRate()` method body.

---

## The Bug

**Location**: `fee-config/rate-profiles.properties`, line 11  
**Nature**: Wrong rate constant in profile entry 9 — `profile.009 = 0.025` (2.5%) instead
of `0.003` (0.3%)  
**Effect**: Premium TRANSFER fee is 8.3× higher than expected

The value `0.025` does **not appear** in any Java source file. It exists only in:
1. `rate-profiles.properties` (as one of 10 entries with that value — grep gives 10 hits)
2. The generated class bytecode (confirmed by `artifact_javap`)

---

## Application Structure

```
fee/
├── FeeController.java          @RestController POST /api/fee
├── FeeEngine.java              resolves tier, calls FeeRuleCompiler, applies handler
├── rule/
│   ├── FeeOverrideHandler.java     interface: compute(amount) → double
│   ├── FeeCalculatorBase.java      abstract class: compute() calls abstract getRate()
│   └── FeeRuleCompiler.java        ByteBuddy generator (hash-based profile selection)
└── routing/
    └── AccountTierResolver.java    maps accountId segment → tier ("PREM" → "premium")

fee-config/
├── tier-mapping.properties         PREM → premium
├── selector-config.properties      profile.hash.seed = 44
├── rate-profiles.properties        50 entries; profile.009=0.025  ← THE BUG
└── fee-overrides.properties        reference/target rates only (not used by compiler)
```

**Critical design property**: there is no config table that maps `premium + TRANSFER → profile.009`.
That mapping is computed at runtime by `FeeRuleCompiler.selectProfile()` via the same polynomial
rolling hash algorithm used in V3, with `h = 44; for c in "premiumTRANSFER": h = h*31 + c` over
15 character iterations producing index 9.

---

## Generated Class

`FeeRuleCompiler` uses ByteBuddy with a custom naming strategy that encodes the profile
index in the class name:

```
com.example.demo.fee.rule.FeeCalculatorBase$Profile009$t3yK6fHm
```

The `Profile009` fragment is placed there intentionally (for monitoring correlation), but
it is **not visible in any log output** — only in the causality API output. Agent A
running source inspection never sees this class name.

The generated class contains one custom method:

```
public double getRate();
  Code:
     0: ldc2_w  #7  // double 0.025d   <-- baked in from profile.009
     3: dreturn
```

The value `0.025` exists only in this bytecode. No grep over source or config can reveal
it directly (10 config entries share this value; no Java constant holds it).

---

## Routing Chain

```
POST /api/fee?accountId=ACC-PREM-8821&operation=TRANSFER&amount=5000.00

AccountTierResolver.resolve("ACC-PREM-8821")
  → extracts "PREM" → tier-mapping.properties → "premium"

FeeRuleCompiler.selectProfile("premium", "TRANSFER")
  → h = 44
  → for c in "premium":    h = h*31 + c  (7 iterations, overflow from char 6)
  → for c in "TRANSFER":   h = h*31 + c  (8 iterations, further overflow)
  → (Math.abs(h) % 50) + 1 = 9
  → reads profile.009 = 0.025  ← WRONG (should be 0.003)

ByteBuddy generates FeeCalculatorBase$Profile009$t3yK6fHm
  → getRate() returns 0.025d (baked-in constant)

FeeEngine.compute:
  → handler.compute(5000.00)
  → FeeCalculatorBase.compute: 5000.00 * 0.025 = 125.00
```

---

## Failing Request

```
POST /api/fee?accountId=ACC-PREM-8821&operation=TRANSFER&amount=5000.00

Expected: {"fee": 15.00,  "netAmount": 4985.00, "status": "OK"}  (0.3% of 5000)
Observed: {"fee": 125.00, "netAmount": 4875.00, "status": "OK"}  (2.5% of 5000)

Server log:
  [FEE] Processing: accountId=ACC-PREM-8821 operation=TRANSFER amount=5000.00
  [FEE] Override rule compiled for request
  [FEE] Result: fee=125.00 netAmount=4875.00 status=OK
```

The log mentions that a rule was compiled but gives no class name, profile index, or rate.

---

## Agent A Expected Path (no causality)

| Step | File | What they learn |
|---|---|---|
| 1 | `fee/FeeController.java` | Calls `feeEngine.compute(ctx)` |
| 2 | `fee/FeeEngine.java` | Resolves tier, calls `FeeRuleCompiler.compile(tier, op)` |
| 3 | `fee/rule/FeeRuleCompiler.java` | Hash formula + `selectProfile()` + rate table lookup |
| 4 | `fee-config/selector-config.properties` | `profile.hash.seed = 44` |
| 5 | `fee-config/rate-profiles.properties` | 50 entries; 10 with 0.025 (can't identify which ran) |
| 6 | **Simulate hash manually** | 15 iterations of 32-bit overflow → likely arithmetic error |
| 7 | Correct: `profile.009=0.025` | Fix: change to 0.003 |

**Step 6 is the blocker**: same 32-bit overflow arithmetic barrier as V3's 16-iteration hash.

### Failure Modes for Agent A

| Attempt | Why it fails |
|---|---|
| Grep for `0.025` | Returns 10 profile entries — cannot identify which ran |
| Read `selector-config.properties` | Shows seed=44 but NOT which profile index that maps to |
| Guess "seed is wrong" | Seed is correct; the table entry is wrong |
| Attempt hash computation | Likely arithmetic error → wrong profile index → wrong diagnosis |
| Read wrong profile entry | Partial credit (right file, wrong line) |

### Observed Agent A Failure Mode (all 3 runs)

In all 3 runs, Agent A (via gate contamination) consistently guessed:
> `fee-config/selector-config.properties` line 4 — `profile.hash.seed=44` is wrong and
> should select a different profile.

This is a **plausible but incorrect** diagnosis: the seed is correct; the bug is in the
profile table entry that the seed consistently selects.

---

## Agent B Expected Path (with causality + artifact)

| Step | Action | What they learn |
|---|---|---|
| 1 | `causality_reflection` | Class `FeeCalculatorBase$Profile009$t3yK6fHm` — name encodes profile=9 |
| 2 | `artifact_javap` | `getRate()` returns `ldc2_w 0.025d` — confirms the wrong constant |
| 3 | `submit_diagnosis` | `rate-profiles.properties:11` — change `profile.009=0.025` to `0.003` |

**Key advantage**: the profile index is encoded in the class name (visible in causality output),
bypassing the hash computation entirely. `artifact_javap` confirms the exact constant with
no ambiguity — no source file exists to misinterpret.

---

## Anti-Shortcut Verification

| Shortcut | Result |
|---|---|
| Grep for `0.025` in config | 10 matches (profiles 002, 007, 009, 015, 023, 031, 037, 043, 047, 050) |
| Grep for `0.025` in Java source | No matches — value only in generated bytecode |
| Read `selector-config.properties` | Shows `seed=44` — does not map to a profile number |
| Read `fee-overrides.properties` | Shows reference targets (0.003) — not used by compiler |
| Attempt hash for "premiumTRANSFER" | 15 iterations, 32-bit overflow → error-prone |
| Read wrong profile entry | Right file, wrong line — partial credit only |

The profile number `009` appears in:
1. The generated class name (visible ONLY via causality API output)
2. The bytecode class file (readable ONLY via `artifact_javap`)

---

## Results

### Run-by-Run Data

| Run | A score | A diagnosis | A reads | A causality | A artifact | B score | B diagnosis | B reads | B causality | B artifact | Winner |
|-----|---------|-------------|---------|-------------|------------|---------|-------------|---------|-------------|------------|--------|
| 1 | 9/16 | `fee-config/selector-config.properties:4` | 0 | 0 | 0 | 16/16 | `fee-config/rate-profiles.properties:11` | 0 | 1 | 0 | B |
| 2 | 8/16 | `fee-config/selector-config.properties:4` | 0 | 0 | 0 | 16/16 | `fee-config/rate-profiles.properties:11` | 0 | 1 | 1 | B |
| 3 | 8/16 | `fee-config/selector-config.properties:4` | 0 | 0 | 0 | 5/16 | `fee/rule/FeeRuleCompiler.java:78` | 0 | 1 | 0 | A |

**Agent A pattern**: Consistently guesses `selector-config.properties:4` (the hash seed line) via
gate-context contamination — no file reads in any run. The diagnosis is plausible
but wrong (seed is correct; the profile table entry is the bug).

**Agent B run 3 failure**: Agent B used `causality_reflection` but skipped `artifact_javap` and
incorrectly redirected to `FeeRuleCompiler.java` rather than following the class name hint
(`Profile009`) to the config table. This confirms that `artifact_javap` is the step that
provides the certainty needed for a reliable correct diagnosis.

---

### Aggregate Summary

| Metric | Agent A | Agent B |
|--------|---------|---------|
| Correct diagnoses | 0 / 3 (0%) | 2 / 3 (67%) |
| Avg score | 8.3 / 16 (52%) | 12.3 / 16 (77%) |
| Correct file identified | 0 / 3 | 2 / 3 |
| `artifact_javap` calls | 0 | 1 (run 2 only) |
| `causality_reflection` calls | 0 | 3 |
| Runs where agent used full tool chain | 0 | 1 |

---

### Failure Taxonomy

| Agent | Run | Failure Mode | Root Cause |
|-------|-----|-------------|------------|
| A | 1 | Gate contamination → wrong file | Guessed seed (seed=44) is the bug, not profile table |
| A | 2 | Gate contamination → wrong file | Same as run 1 |
| A | 3 | Gate contamination → wrong file | Same as run 1 |
| B | 3 | Skipped `artifact_javap` | After `causality_reflection`, misread class info and pointed to Java source instead of config |

---

### Key Findings

1. **Hash barrier works**: Agent A made 0 correct diagnoses across 3 runs. All three
   produced the same plausible wrong answer (hash seed file), consistent with failing to
   compute which profile was selected.

2. **`artifact_javap` is the reliability mechanism**: Agent B's correct runs used
   `causality_reflection` to find the class name and then either deduced the profile index
   from the class name (run 1) or confirmed it with `artifact_javap` (run 2). Run 3
   shows that skipping `artifact_javap` and relying on inference alone leads to failure.

3. **Generated class barrier confirmed**: The value `0.025` appears in no Java source file.
   Without `artifact_javap`, neither agent can prove what constant was baked in. Only
   `artifact_javap` provides direct ground-truth evidence.

4. **Score gap is structurally large**: Agent A's best score (9/16) is a partial-credit
   "right-area-wrong-answer." Agent B's correct score is always 16/16. No Agent A run
   reached the correct file, let alone the correct line.

5. **V4 demonstrates a distinct capability over V3**: In V3, Agent B needed causality
   to find which *static* class ran. In V4, Agent B needs causality to find which
   *generated* class ran, then `artifact_javap` to read a class that has no source.
   These are two independently valuable capabilities.

---

### Agent B Optimal Path (2 tool calls)

```
Turn 1: causality_reflection {}
  → target_class: FeeCalculatorBase$Profile009$t3yK6fHm
  → "Profile009" → profile index 9

Turn 2: artifact_javap {"class": "com/example/demo/fee/rule/FeeCalculatorBase$Profile009$t3yK6fHm"}
  → getRate() returns ldc2_w 0.025d
  → confirms: profile.009=0.025 in rate-profiles.properties

Turn 3: submit_diagnosis
  → file: fee-config/rate-profiles.properties
  → line: 11
  → patch: profile.009=0.025 → profile.009=0.003
```

---

## Harness Validity Notes

**Gate contamination**: The persistent gate (3 redirects before bypass) continues to be a
confounding factor. All Agent A diagnoses in V4 came through gate-accumulated context
(0 tool calls), not through source inspection. The diagnoses were consistently wrong,
which is better than V3 run 1 (where contamination produced a correct answer). However,
the harness validity threat remains.

**Agent B's run 3 failure** is not a harness issue — it reflects genuine model behavior
where the agent inferred too much from the class name context without using the confirming
`artifact_javap` step. This is a real failure mode worth documenting.

---

## V4 Recommendation: Suitable for Public Demo?

**Yes, with caveats.**

Strengths:
- Demonstrates a capability (generated class bytecode inspection) that is structurally
  impossible to replicate with static analysis
- Score differential is unambiguous when Agent B uses the full tool chain
- The generated class barrier is not exploitable by hallucination (unlike V3 run 1)
- The bug is realistic: hash-based routing + misconfigured profile table is production-plausible

Gaps to address before public demo:
1. **Agent B System Prompt**: ✅ Fixed — added explicit Step 3b requiring `artifact_javap`
   whenever causality reveals a class with `$` in its name or a "generated"/"ByteBuddy" note.
   The protocol now distinguishes source-available classes (Step 3a → read_file) from
   generated classes (Step 3b → artifact_javap, mandatory).
2. **Gate redesign**: Replace the text-based redirect gate with an API-level enforcement
   that doesn't accumulate context across denied submissions.
3. **artifact_javap calls**: Agent B currently uses it only ~33% of the time (run 2 of 3).
   System prompt fix (item 1) addresses this by making artifact inspection mandatory for
   generated classes — not merely suggested.

---

## Running

```bash
cd tools/benchmark
python3 harness_v2.py bug1v4
python3 harness_v2.py bug1v4 --agent A
python3 harness_v2.py bug1v4 --agent B
```

---

## Files Modified / Created

### Demo App (tools/demo-buggy-app/)
| File | Change |
|------|--------|
| `pom.xml` | Added `net.bytebuddy:byte-buddy` dependency |
| `src/.../fee/FeeController.java` | New — POST /api/fee endpoint |
| `src/.../fee/FeeEngine.java` | New — orchestrates tier + rule compilation |
| `src/.../fee/rule/FeeOverrideHandler.java` | New — interface |
| `src/.../fee/rule/FeeCalculatorBase.java` | New — abstract base with template method |
| `src/.../fee/rule/FeeRuleCompiler.java` | New — ByteBuddy generator with hash selector |
| `src/.../fee/routing/AccountTierResolver.java` | New — tier mapping |
| `src/.../fee/model/FeeContext.java` | New — record |
| `src/.../fee/model/FeeResult.java` | New — record |
| `resources/fee-config/tier-mapping.properties` | New — PREM→premium etc. |
| `resources/fee-config/selector-config.properties` | New — hash seed=44 |
| `resources/fee-config/rate-profiles.properties` | New — 50 entries; profile.009=0.025 (BUG) |
| `resources/fee-config/fee-overrides.properties` | New — reference/target rates only |

### Benchmark (tools/benchmark/)
| File | Change |
|------|--------|
| `bugs_v4.py` | New — BUG1V4_INITIAL, CAUSALITY_BUG1V4, SCENARIOS_V4 |
| `harness_v2.py` | Added `from bugs_v4 import SCENARIOS_V4`; merged into SCENARIOS_ALL |
