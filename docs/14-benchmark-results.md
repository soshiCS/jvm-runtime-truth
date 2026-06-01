# ManyCore Agent Benchmark — Results

**Date**: 2026-05-31  
**Design ref**: [docs/13-agent-benchmark-design.md](13-agent-benchmark-design.md)

---

## ⚠️ Why this benchmark produced ties — and what we changed

This was a **single-turn benchmark with pre-loaded source code**. Both agents received the complete relevant Java package before being asked to diagnose. This measured *reading comprehension*, not *runtime discovery*.

Modern LLMs can brute-force a 5–15 file Java package in one pass. The causality data confirmed what the source already revealed — it provided no search shortcut because there was no search phase. Both agents saw all the evidence simultaneously.

**This does not mean the causality API lacks value.** It means the experiment was not measuring the right thing. The causality API's value is in *reducing the search space* when an agent is navigating a large codebase iteratively, wasting turns on wrong hypotheses.

The redesigned multi-turn benchmark (`harness_v2.py`, `docs/15-benchmark-redesign.md`) corrects this:
- Agents start with *only* the endpoint URL, symptom, and log — no source files
- Agents must discover the relevant file through tool calls
- Bug variants are harder: config-driven dispatch, 15-handler polymorphic, 4-interceptor proxy chain

See [docs/15-benchmark-redesign.md](15-benchmark-redesign.md) for the redesigned experiment.

---

## Executive summary (v1 single-turn results)

**The causality API provided no measurable advantage on these three bugs.**

Both agents scored identically on Bug 1 (Reflection) and Bug 2 (Proxy). Agent B scored 1 point higher than Agent A on Bug 3 (Polymorphic) by pinpointing the exact line — a marginal improvement below the 3-point threshold defined as "material advantage" in the design doc. All runs completed in under 7 seconds with high confidence. Neither agent expressed wrong hypotheses on Bugs 1 or 3.

The primary explanation: the bug descriptions were specific enough (named log lines, named wrong values, named proxy classes in the symptom) that static source reading alone was sufficient to identify the root cause. The causality data confirmed what the source already revealed — it did not unlock new information.

---

## Setup

| Setting | Value |
|---|---|
| Mode | Single-turn (`claude -p --output-format json`) |
| Model | `claude-sonnet-4-6` |
| Bugs | Bug 1 (Reflection), Bug 2 (Proxy), Bug 3 (Polymorphic) |
| Source sanitization | Bug indicator comments stripped from all Java files before agent sees them |
| Agent A input | Bug description + sanitized source code |
| Agent B input | Bug description + sanitized source code + pre-captured causality API output |
| Causality data | Real output from `/causality/reflection`, `/causality/proxies`, `/causality/polymorphic` captured from a live run under the ManyCore JVM |
| Scoring | Automated, 16-point rubric (file 0–3, line 0–3, patch 0–5, explain 0–5) |

Commands used:

```bash
cd tools/benchmark
python run_single_turn.py bug1 bug2 bug3
```

---

## Score table

| Bug | Ground truth | A score | B score | Delta | Winner |
|---|---|---|---|---|---|
| Bug 1 — Reflection | line 30 | **15/16** (93.8%) | **15/16** (93.8%) | 0 | TIE |
| Bug 2 — Proxy | line 45 | **14/16** (87.5%) | **14/16** (87.5%) | 0 | TIE |
| Bug 3 — Polymorphic | line 19 | **15/16** (93.8%) | **16/16** (100%) | +1 | TIE* |

\* Score delta of 1 is below the 2-point threshold for declaring a winner (scorer.py `compare()`).

### Score breakdown

| Bug | Agent | File | Line | Patch | Explain | Total | Elapsed |
|---|---|---|---|---|---|---|---|
| Bug 1 | A | 3/3 | 2/3 | 5/5 | 5/5 | 15/16 | 5.6s |
| Bug 1 | B | 3/3 | 2/3 | 5/5 | 5/5 | 15/16 | 5.8s |
| Bug 2 | A | 3/3 | 1/3 | 5/5 | 5/5 | 14/16 | 6.4s |
| Bug 2 | B | 3/3 | 1/3 | 5/5 | 5/5 | 14/16 | 6.5s |
| Bug 3 | A | 3/3 | 2/3 | 5/5 | 5/5 | 15/16 | 5.3s |
| Bug 3 | B | 3/3 | 3/3 | 5/5 | 5/5 | 16/16 | 6.0s |

### Line accuracy detail

| Bug | Ground truth | A guess | A off-by | B guess | B off-by |
|---|---|---|---|---|---|
| Bug 1 | 30 | 33 | ±3 | 34 | ±4 |
| Bug 2 | 45 | 33 | ±12 | 30 | ±15 |
| Bug 3 | 19 | 22 | ±3 | 19 | exact |

---

## Per-bug analysis

### Bug 1 — Reflection: Silent Handler Misdispatch

Both agents correctly identified `bug1/NotificationService.java` and submitted the exact patch (`DeliveryHandler.class`). Both rated their confidence `high`. Neither explored wrong hypotheses.

**Agent A explanation**: "The handler registry maps 'DELIVERY' to `AuditHandler.class` instead of `DeliveryHandler.class`. When a DELIVERY event is dispatched, reflection instantiates and invokes `AuditHandler.handle()`, which logs an audit record and returns silently."

**Agent B explanation**: "The handler registry maps 'DELIVERY' to `AuditHandler.class` instead of `DeliveryHandler.class`. The runtime causality data confirms this: the reflection callsite in `NotificationService.dispatch` resolves to `AuditHandler.handle` at runtime."

**Observation**: The causality data confirmed what the source already showed. The registry `put()` call is visible directly in the constructor — Agent A needed no runtime evidence to find it.

---

### Bug 2 — Proxy: Inverted Tax Rate

Both agents correctly identified `bug2/OrderService.java` and the exact patch (swap `DOMESTIC_TAX_RATE` and `INTERNATIONAL_TAX_RATE` in the ternary). Both scored line=1/3, as the ternary starts at line 33 in the sanitized source view but ground truth is line 45.

**Agent A wrong hypotheses**: `AuditInterceptor` modifying the return value; `Order.type()` returning incorrect values.

**Agent B wrong hypotheses**: `AuditInterceptor` modifying the return value; CGLIB proxy intercepting and altering the result; `Order` record storing the wrong type value.

**Observation**: Agent B explicitly named "CGLIB proxy intercepting and altering the result" as a wrong hypothesis and discarded it — suggesting the causality/proxies data helped it rule out the proxy faster. However, Agent A also discarded the AuditInterceptor hypothesis without causality data, arriving at the same score. The proxy data reduced consideration of a red herring (1 fewer explicit wrong-hypothesis report for Agent A) but didn't change the diagnosis.

---

### Bug 3 — Polymorphic: Guest Privilege Escalation

Agent B scored the only perfect 16/16 run of the benchmark, hitting the exact line (19). Agent A was off by 3 (guessed line 22), earning line=2/3.

**Agent A explanation**: "`GuestHandler.handle` was copy-pasted from `AdminHandler` without updating the permissions list. The role field was changed to 'guest' but the permissions array still contains `[read,write,delete,manage]`."

**Agent B explanation**: "`GuestHandler.handle` was copy-pasted from `AdminHandler` without updating the permissions list. The runtime causality data confirms `GuestHandler` is the concrete dispatch target for GUEST requests at the `invokeinterface` call site in `RequestRouter.route`."

**Observation**: This is the only case where causality data produced a measurable improvement. The polymorphic dispatch data (`blocked_multi_target @ RequestRouter.route bci=40 → GuestHandler`) named the exact concrete class receiving GUEST requests, which guided Agent B to read `GuestHandler.java` first and identify the exact offending line. Agent A reached the same file but guessed a slightly wrong line number.

The improvement is real but small (+1 point) and confined to a single scoring dimension (line precision).

---

## Efficiency metrics

In single-turn mode, "turns" = 1 for all runs. File-read counts are not applicable (source is pre-provided in the prompt, not tool-fetched). Causality calls: Agent A = 0, Agent B = 0 (data is pre-embedded in prompt, not queried interactively).

---

## Assessment against success criteria

From `docs/13-agent-benchmark-design.md`:

> Agent B is declared to have a **material advantage** if, across ≥2 bugs:
> - Score delta ≥ 3 points, OR
> - Turn count reduction ≥ 30%, OR
> - Both agents reach correct diagnosis but B uses ≤50% as many file reads

**None of these criteria are met.**

- Largest score delta is +1 (Bug 3 only)
- Turns are identical (1 turn each, single-turn mode)
- File access is identical (both receive full source in prompt)

**Conclusion: For these three bugs, the causality API provides no measurable advantage in single-turn mode.**

---

## Interpretation

### Why didn't causality help more?

Three factors combined to minimize the causality advantage:

1. **Bug descriptions named the mechanism.** Bug 1 says "routes via reflection." Bug 2 says "wrapped in AOP interceptor... CGLIB proxy." Bug 3 says "dispatched through an interface." The agent did not need runtime evidence to know *where* to look.

2. **Source files are small and directly inspectable.** The demo app has ~17 Java files. A single-turn agent reading them all in one pass can find a registry `put()` with the wrong class in O(1) time.

3. **Single-turn mode collapses the search process.** In a multi-turn investigation, the agent might waste 3–5 tool calls chasing the CGLIB proxy or inspecting interceptor logic before finding the real bug. Causality data could skip those detours. But in single-turn mode, all source is present — the agent reasons over the full codebase at once, which effectively eliminates the exploration problem.

### When would causality matter?

The causality API is designed for scenarios where:
- The codebase is large and the agent cannot read every file
- The dispatch mechanism is genuinely opaque (runtime-generated classes, multi-layer proxies with 6+ frames)
- The agent is making tool calls to explore the code iteratively, burning turns on wrong paths

The multi-turn harness (`tools/benchmark/harness.py`) tests this scenario. In that setup, Agent A must navigate the source tree using tool calls — and causality data could save 5–10 turns by immediately pointing to the concrete dispatch target. That experiment has not yet been run.

---

## Raw output samples

### Bug 1, Agent A (15/16)
```json
{
  "root_cause_file": "bug1/NotificationService.java",
  "root_cause_line": 33,
  "root_cause_explanation": "The handler registry maps 'DELIVERY' to AuditHandler.class instead of DeliveryHandler.class. When a DELIVERY event is dispatched, reflection instantiates and invokes AuditHandler.handle(), which logs an audit record and returns silently — no delivery confirmation is ever sent.",
  "patch_code": "registry.put(\"DELIVERY\", AuditHandler.class); → registry.put(\"DELIVERY\", DeliveryHandler.class);",
  "confidence": "high",
  "wrong_hypotheses": []
}
```

### Bug 3, Agent B (16/16 — only perfect score)
```json
{
  "root_cause_file": "bug3/GuestHandler.java",
  "root_cause_line": 19,
  "root_cause_explanation": "GuestHandler.handle was copy-pasted from AdminHandler without updating the permissions list. The method returns the string literal '[read,write,delete,manage]' instead of '[read:public]', granting guest sessions full administrator permissions. The runtime causality data confirms GuestHandler is the concrete dispatch target for GUEST requests at the invokeinterface call site in RequestRouter.route.",
  "patch_code": "return \"GUEST_PROFILE:{userId=\" + request.userId() + \",role=guest,permissions=[read,write,delete,manage]}\"; → return \"GUEST_PROFILE:{userId=\" + request.userId() + \",role=guest,permissions=[read:public]}\";",
  "confidence": "high",
  "wrong_hypotheses": []
}
```

---

## Files

| File | Description |
|---|---|
| `tools/benchmark/run_single_turn.py` | Single-turn harness (`claude -p`) |
| `tools/benchmark/bugs.py` | Bug scenario definitions and ground truth |
| `tools/benchmark/scorer.py` | Automated scoring logic |
| `tools/benchmark/results/benchmark_result_single_turn.json` | Raw results |
| `tools/benchmark/harness.py` | Multi-turn harness (not yet run) |

---

## Next steps

1. **Run multi-turn harness** (`harness.py`) — this is the higher-fidelity test where agents explore interactively. Causality data should show a larger advantage when agents must search rather than reason over a pre-loaded codebase.

2. **Harder bug variants** — bugs where the symptom does not name the dispatch mechanism (no "via reflection" in the prompt) would better stress-test causality's advantage.

3. **Larger codebase** — scale the demo app to 50+ classes so source-scan-in-one-pass is no longer feasible, making iterative exploration (and causality shortcuts) more realistic.
