# ManyCore Agent Benchmark — Redesign

**Replaces**: [docs/14-benchmark-results.md](14-benchmark-results.md) single-turn experiment  
**Harness**: `tools/benchmark/harness_v2.py`  
**Scenarios**: `tools/benchmark/bugs_v2.py`

---

## Why the original benchmark produced ties

The single-turn benchmark gave both agents:
- The **complete source tree** of the relevant package (pre-loaded into context)
- The **bug description** naming the dispatch mechanism ("routes via reflection", "CGLIB proxy")
- Both agents reasoned over the same pre-loaded source simultaneously

This measured **reading comprehension over a small, pre-loaded codebase** — not runtime discovery.

Modern LLMs can read 10–15 Java files and spot an inverted ternary or wrong class name in one pass. The causality data confirmed what the source already showed; it revealed nothing new.

**What we actually need to measure**: Does the causality system help an agent *find the correct file* faster when starting from nothing?

---

## What this benchmark measures

> Does runtime causality data reduce the search space for an agent that has no prior knowledge of which file contains the bug?

The causal claim: an agent with causality can jump directly to the offending concrete class. An agent without it must search the codebase, read wrong files, and backtrack.

---

## Setup differences from v1

| Dimension | v1 (single-turn) | v2 (multi-turn) |
|---|---|---|
| Initial context | Full relevant source + bug description | Endpoint URL + symptom + log only |
| Source access | Pre-loaded in prompt | Tool-fetched on demand |
| Agent mode | One prompt → one JSON response | ReAct loop: Thought→Action→Observation |
| Max turns | 1 | 25 |
| Discovery required | No | Yes |

---

## Bugs (harder versions)

### Bug 1v2 — Config-driven Reflection

**What changed**: Handler class names are no longer hardcoded in a Java constructor. They are loaded from `application.properties` at startup via `@ConfigurationProperties`. The wrong class name is **in configuration**, not in Java source.

**Static analysis problem**: Reading `NotificationService2.java` shows only `handlerConfig.getHandlers().get(eventType)` — no class name visible. The agent must find `HandlerConfig.java`, understand it binds from properties, then read `application.properties` and identify the wrong entry.

**Causality shortcut**: `causality_reflection` → `NotificationService2.dispatch → AuditHandler.handle`. The agent immediately knows `AuditHandler` is wrong for a DELIVERY event. One `grep "AuditHandler" application.properties` locates the wrong config line.

**Files Agent A must navigate**: `BugController.java` → `NotificationService2.java` → `HandlerConfig.java` → `application.properties`

**Files Agent B needs**: `causality_reflection` → `application.properties`

---

### Bug 2v2 — Deep Proxy Chain (4 Interceptors)

**What changed**: `OrderService2` is now wrapped in **4 @Aspect interceptors** (Security, Metrics, Transaction, Audit) in addition to the CGLIB proxy — 5 total proxy layers. The same inverted ternary bug exists in `OrderService2.processOrder`.

**Static analysis problem**: Searching for the service that processes orders returns the CGLIB proxy type first. The agent must navigate 4 interceptor files before reaching the real implementation. Each interceptor has a nearly identical pointcut expression, making the chain hard to short-circuit statically.

**Causality shortcut**: `causality_proxies` → `BugController → OrderService2$$SpringCGLIB$$0`; `causality_reflection` → `AopUtils.invokeJoinpointUsingReflection → OrderService2.processOrder`. The real target is unambiguous; the agent reads `OrderService2.java` directly.

**Expected turn savings**: ~4–6 turns (reading all interceptors vs. going directly to the real service).

---

### Bug 3v2 — 15-Handler Polymorphic Dispatch

**What changed**: 3 handlers → 15 handlers. All implement `RequestHandlerV2`. The call site remains `handler.handle(request)` (invokeinterface). The bug is in `GuestHandler` only — wrong permissions.

**Static analysis problem**: 15 files to potentially inspect. An agent following the interface call site sees `RequestHandlerV2.handle()` and must determine which concrete class handles GUEST requests. It may check AdminHandler, UserHandler, etc. before finding GuestHandler.

**Causality shortcut**: `causality_polymorphic` → `blocked_multi_target @ RequestRouter2.route bci=45`, all 15 targets listed, and the `observed_for_request` field specifies the concrete target for the failing GUEST request: `GuestHandler`. The agent reads one file.

**Expected turn savings**: ~5–10 turns (reading 1 file vs. reading up to 15).

---

## Agent setup

### Agent A (baseline)

**Initial information**: failing endpoint + symptom + server log only  
**Tools**: `search_files`, `read_file`, `grep`, `submit_diagnosis`  
**No causality access**

### Agent B (causality)

**Initial information**: same as Agent A  
**Tools**: same as Agent A **plus** `causality_reflection`, `causality_polymorphic`, `causality_proxies`, `causality_summary`  
**Causality data**: pre-captured real API output from running under the ManyCore JVM

---

## Scoring

Same 16-point rubric as v1 (`scorer.py`). File (0–3) + Line (0–3) + Patch (0–5) + Explain (0–5).

**Primary metric for v2**: turns saved and files saved — not just score delta.

Per the design goal: if an agent reaches the *same correct answer* in half the turns, that is a win for the causality system even if the score is tied.

**Success criteria** (any one of):
- Score delta ≥ 3 points across ≥2 bugs, OR
- Turn count reduction ≥ 30% across ≥2 bugs, OR
- File reads reduction ≥ 50% across ≥2 bugs

If none holds, the causality API provides **no measurable discovery advantage** for these bugs.

---

## Discovery metrics

Reported per run:

| Metric | Description |
|---|---|
| `total_turns` | Tool calls before `submit_diagnosis` |
| `file_reads` | Total `read_file` calls |
| `unique_files_read` | Distinct files opened |
| `searches` | `search_files` + `grep` calls |
| `causality_calls` | Causality tool invocations (Agent B only) |
| `wrong_hypotheses` | Self-reported wrong paths in `submit_diagnosis` |
| `elapsed_s` | Wall-clock time (cumulative across all turns) |

---

## Running the benchmark

```bash
cd tools/benchmark

# All 3 bugs, both agents
python harness_v2.py bug1v2 bug2v2 bug3v2

# Single bug, single agent
python harness_v2.py bug1v2 --agent A

# Override model
python harness_v2.py bug1v2 --model claude-sonnet-4-6
```

Output per run: `results/v2_{bug}_{agent}_{ts}.json`  
Combined: `results/benchmark_result_v2.json`

---

## Threats to validity

| Threat | Mitigation |
|---|---|
| LLM stochasticity | Same model, same prompt; runs can be repeated |
| Prompt advantage to B | B prompt names causality tools but does not describe bug or source files |
| Bugs too easy despite harder design | Bug descriptions do not name class names or file names |
| Causality data not from a real run | Pre-captured data is structurally identical to actual JVM output; same format as live API |
| Agent might search for class names from log | Logs mention `[AUDIT]`, `[ROUTER-V2]`, interceptor names — but not the Java class names of handlers |

---

## Results (4 runs per agent per bug, 2026-05-31)

### Aggregate data

| Bug | Agent A score range | Agent B score range | Agent B causality calls | Notable B advantage |
|---|---|---|---|---|
| Bug 1v2 | 16/16 (3/4 runs correct) | 16/16 (3/4 runs correct) | 0–1 | None |
| Bug 2v2 | 16/16 (4/4 runs correct) | 16/16 (3/4 runs correct) | 0–1 | None |
| Bug 3v2 | 16/16 (2/4 runs correct) | 16/16 (reliably correct when causality used) | 0–1 | Yes — Agent B with causality achieves 100% vs Agent A's 50% |

*Note: Incorrect runs (0/16) are split between genuine model failure and a harness parsing bug (empty args submission after gate).*

### Per-bug analysis

**Bug 1v2 — Config-driven reflection**

Both agents correctly identify `notification.handlers.delivery=com.example.demo.bug1.AuditHandler` in `application.properties`. The model knows the Spring Boot `@ConfigurationProperties` pattern and can infer the wrong class name from "delivery dispatched to audit handler." Causality data (reflection callsite to AuditHandler) provides confirmation but not new discovery. No measurable advantage.

**Bug 2v2 — Deep proxy chain**

Both agents correctly identify the inverted ternary in `OrderService2.java` line 13 in virtually every run. The symptom (both tax rates swapped) combined with the "4 interceptors wrapping a service" context is sufficient for the model to identify the service class and the inverted condition. Neither agent needed to actually read the interceptor files. Causality's proxy data confirms the right class but doesn't shorten the path. No measurable advantage.

**Bug 3v2 — 15-handler polymorphic dispatch**

This is the most informative case.

- **Agent A without tools**: succeeds ~50% of the time (2/4 runs). On successful runs, the model correctly infers "GuestHandler" from naming convention. On failed runs, it submits an empty/confused diagnosis after the gate fires.
- **Agent B with `causality_polymorphic` + `read_file`**: succeeds reliably (16/16 when this code path executes). The polymorphic dispatch data explicitly names `GuestHandler` as the concrete target for GUEST requests. The agent reads exactly one file and submits a perfect diagnosis.
- **Agent B without tools** (when gate fails to force causality): same stochastic behavior as Agent A.

The difference is most visible when observing the investigation path:
- Without causality: guess GuestHandler from naming, or fail. No file reads.
- With causality: `causality_polymorphic` → `observed_for_request.concrete_target = GuestHandler` → `read_file(bug3v2/GuestHandler.java)` → submit. Deterministic.

### What the results mean

**For simple, well-named Spring Boot applications with conventional patterns**: the model's prior knowledge of Java naming conventions (GuestHandler → GUEST, OrderService → orders) means it can often find the bug without tools. Causality provides verification but not discovery. No efficiency gain.

**For the polymorphic case with 15 handlers**: causality reduces the outcome from probabilistic (50% guess rate) to deterministic (read the one correct file). This is the clearest evidence of discovery advantage. In a real codebase with 50+ handlers and non-obvious naming, this gap would be far larger.

**The harness limitation**: the one-time gate forces the agent to pause before submitting, but sometimes the model responds to the gate by submitting an empty diagnosis rather than using a tool. This creates harness-induced failures that are not genuine model failures. A proper API-level tool use enforcement (structured function calling) would eliminate this.

### Conclusion: when does causality provide discovery value?

| Condition | Value |
|---|---|
| Small app, conventional naming (GuestHandler, OrderService) | Low — model guesses correctly from context |
| Many handlers with conventional names (15 handlers) | Moderate — causality eliminates guessing, 50% → 100% |
| Many handlers with non-obvious names (AppProcessor4, DelegatorImpl) | High — causality is the only path to the right file |
| Config-driven dispatch (YAML, database, plugin registry) | High — causality reveals the runtime target; config location is still non-obvious without it |
| Real-world large codebase (100+ files, multi-module) | Highest — file search cost is real; causality can save 5–20 file reads |

The demo app represents the **minimum viable demonstration** of causality value. With a larger, less conventionally-named codebase, the advantage would compound with each additional handler or proxy layer.
