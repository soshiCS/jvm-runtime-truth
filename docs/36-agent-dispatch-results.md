# Phase V10 — Agent Dispatch Results

**Date:** 2026-05-31  
**Hypothesis being tested:** Runtime Truth is more valuable for AI-agent debugging than for traditional JVM debugging.  
**Verdict (preview):** Moderate evidence — B. True in specific conditions; those conditions are narrower than hoped.

---

## Debugging Workflow Comparison

Four workflows applied to the same bug: `MockEmailTool` silently handling all `send_email` dispatches.

### Workflow A — RT + AI Agent

The agent queries:
1. `/causality/search?q=MockEmailTool` → finds it was instantiated and dispatched to
2. `/causality/explain?class=ToolRegistry&method=dispatch&bci=95` → sees `runtime_target: MockEmailTool.execute`
3. `GET /registry` → sees `"send_email": "MockEmailTool"`

**Result:** Correct root cause identified. No code modification needed.  
**Time to answer:** 3 API calls.  
**Blocker:** Requires RT to be running. Requires causality REST server. Requires knowing to call `/causality/search`.

### Workflow B — No RT + AI Agent (production log access only)

The agent reads stdout/stderr for the running process. The app emits:
```
[ToolRegistry] OVERWRITE: 'send_email' replaced SendgridEmailTool with SESMailTool
[ToolRegistry] OVERWRITE: 'send_email' replaced SESMailTool with MockEmailTool
```

**Result:** Correct root cause identified from startup logs alone. No RT needed.  
**Time to answer:** 1 grep on log file.  
**Condition:** This demo app has explicit `log.warn("[ToolRegistry] OVERWRITE: ...")` in the constructor. A real framework would not log this by default.

### Workflow C — RT + Human Engineer

The human runs the app under RT, downloads the JSONL, searches for `MockEmailTool`:
```
grep MockEmailTool runtime_targets.jsonl
```
Finds `runtime_target` records showing `ToolRegistry.dispatch → MockEmailTool.execute`.

**Result:** Correct root cause identified.  
**Time to answer:** One grep + reading 2 JSON lines.  
**Advantage over Workflow D:** None significant for this bug. The log-based approach (Workflow D) is equally fast.

### Workflow D — Traditional Human Debugging

Human looks at startup logs (same as Workflow B) or adds one line to `ToolRegistry.dispatch()`:
```java
log.info("[dispatch] {} -> {}", toolName, tool.getClass().getSimpleName());
```

**Result:** Correct root cause after one code change + rerun.  
**Time to answer:** 5–10 minutes (add log, rebuild, rerun, read output).  
**Limitation:** Requires code modification. Not available to AI agents without a "modify and rerun" capability.

---

## Summary Table

| Workflow | Finds Root Cause? | Code Change Needed? | Time |
|---|---|---|---|
| A: RT + AI Agent | Yes | No | 3 API calls |
| B: No RT + AI Agent (if logs exist) | Yes | No | 1 grep |
| B′: No RT + AI Agent (no logs) | No | Yes (can't do it) | Stuck |
| C: RT + Human | Yes | No | 1 grep on JSONL |
| D: Traditional Human | Yes | Yes (add log) | 5–10 min |

**The gap RT fills:** Workflow B′ — the AI agent scenario where the framework does not log overwrite events. In this case, RT is the only passive mechanism that answers "which class handled this dispatch."

---

## Competitor Reality Check

### LangSmith / LangFuse / Braintrust
Track tool name, input, output, and latency at the framework invocation boundary. They would show `send_email` was called with `to=alice@example.com` and returned `"MOCK_NOOP: email discarded"`. They would NOT show `impl_class=MockEmailTool` unless the framework exposes it.

**Verdict:** Sufficient for most AI agent bugs. Insufficient for class-level collision (MockEmailTool vs SendgridEmailTool). The gap is real but narrow.

### Datadog APM / Dynatrace
Instrument method entry/exit. They would show a span for `ToolRegistry.dispatch` and one for `MockEmailTool.execute`. The target class IS visible in the span tree — but only if auto-instrumentation traces into the reflect-invoked method, which is not guaranteed for reflection targets.

**Verdict:** Likely catches this in practice via span visibility. Requires agent deployed, not zero-instrumentation. Advantage: existing enterprise deployments already have this.

### OpenTelemetry Java Agent
Similar to Datadog. The reflection target class would appear as a child span if OTel auto-instrumentation descends into it. In practice, OTel does instrument `Method.invoke()` in some configurations.

**Verdict:** Comparable to Datadog APM. Not zero-instrumentation (requires agent configuration).

### JFR (Java Flight Recorder)
Sampling profiler. Would show `MockEmailTool.execute` in a flame graph if sampled during execution. Does not produce per-callsite dispatch records. Cannot answer "which class handled the second invocation of `send_email`" at the BCI level.

**Verdict:** Insufficient for this class of bug.

### Undo (Linux only)
Full execution recording with GDB extension. Would allow replaying the exact `HashMap.put()` sequence and observing the overwrite. Definitive answer, but Linux-only, no REST API, not queryable by an AI agent without custom GDB scripting.

**Verdict:** Strongest human debugging tool for this bug class on Linux. Not relevant on macOS. Not AI-agent-queryable.

---

## Kill-the-Thesis Analysis

### Attack 1: The demo app already surfaces impl_class

**Observation:** The `/run` endpoint explicitly returns `"impl_class": "com.example.agentdemo.tool.MockEmailTool"` in every step. An AI agent calling `/run` already has the answer. RT adds nothing.

**Assessment: Valid attack.** This demo over-exposes the answer in the app API. In production, most frameworks do not return the JVM class name of the concrete handler — they return the logical tool name. But a well-instrumented app can make RT unnecessary.

**Implication:** RT's value is inversely proportional to how well the app itself exposes dispatch internals. In heavily logged/instrumented applications, RT is redundant.

### Attack 2: The startup logs already show the overwrite

**Observation:** The startup log sequence (`OVERWRITE: 'send_email' replaced SESMailTool with MockEmailTool`) fully explains the bug without any RT signal.

**Assessment: Valid attack for this demo.** This logging was added deliberately. In real-world frameworks (Spring AI's `ToolCallbackProvider`, LangChain4j's `DefaultToolExecutionService`), there is no equivalent overwrite warning.

**Implication:** The demo app is too cooperative. It warns about its own bug. A real-world scenario would be harder.

### Attack 3: This scenario is rare

**Observation:** Three competing implementations of the same tool name is a code smell. Most production systems have one implementation per tool. The collision scenario that makes RT decisive is a contrived configuration error, not a common bug category.

**Assessment: Partially valid.** The specific three-way collision is contrived. But the underlying pattern — test/mock tool left on classpath, overwriting production implementation due to registration ordering — is a real class of bug. It's less common than, say, wrong parameter values, but it does occur.

**Implication:** RT excels at a narrow bug class. It's not a general-purpose AI debugging advantage.

### Attack 4: LangSmith already covers this in real LLM frameworks

**Observation:** Spring AI and LangChain4j emit structured tool invocation events to traces. LangSmith would capture which function was called and what it returned. If MockEmailTool returns `"MOCK_NOOP"`, an AI agent reading LangSmith traces sees the wrong output immediately, regardless of JVM class identity.

**Assessment: Valid for output-level bugs.** If the mock tool produces visibly wrong output, the bug is caught by output monitoring. RT's class-level attribution only adds value when the wrong implementation produces output that looks correct — or produces no output (silent discard). Silent discard is the realistic case, but it's the output that's being monitored, not the class.

**Implication:** RT's advantage narrows further to cases where output is absent/ambiguous AND no existing tracing captures class identity.

### Attack 5: An AI agent cannot currently use /causality APIs effectively

**Observation:** Calling `/causality/explain?class=ToolRegistry&method=dispatch&bci=95` requires knowing the exact class name, method name, and BCI. An AI agent would need to either know the code or discover the BCI from a prior call. The causality API surface is not self-describing enough for autonomous agent discovery.

**Assessment: Valid.** The current causality API requires source-code-level knowledge as input. It is not an exploration API — it is a structured query API. An AI agent would need to first enumerate candidate classes, then formulate queries. This is tractable but non-trivial.

**Implication:** The causality API needs an exploration endpoint (e.g., `/causality/classes-involved-in?target_method=execute`) for AI agents to use it without prior code knowledge.

---

## What Survives the Attack

After all attacks, the residual RT advantage for AI agent debugging is:

**Scenario:** Production system where:
1. The LLM framework does not log which concrete handler class was invoked
2. The framework's tool execution produces silent wrong behavior (no visible error)
3. No APM agent is deployed
4. The debugging agent cannot modify code and rerun
5. The registry/framework API does not expose handler class identity

In this specific scenario, RT is the only passive mechanism that can answer "which class handled this invocation."

**How common is this scenario?** Uncommon in well-instrumented enterprises. Plausible in early-stage teams running AI agents without full observability stacks.

---

## Final Verdict

**A: Strong — RT is categorically better for AI agent debugging.**  
Not supported. Competitors (LangSmith, Datadog APM) cover the common cases. The app itself can expose impl_class if designed to.

**B: Moderate — RT provides a real but narrow advantage in specific conditions.**  
Supported. When the framework is opaque, no APM is deployed, and the bug is a silent dispatch-target mismatch, RT is the only passive zero-instrumentation answer. The conditions are real but not common.

**C: Weak — RT provides marginal benefit; the problem is already solved.**  
Not fully supported. For zero-instrumentation macOS deployments debugging class-level dispatch collisions, no current tool provides an equivalent passive capture. The gap exists. It's just smaller than the AI agent dispatch framing suggested.

**Final answer: B.**

RT is more valuable for AI-agent debugging than traditional debugging in the specific class of dispatch-target-ambiguity bugs. That class is real. The advantage is not general. The honest position is that RT should be evaluated as a complement to LangSmith/Datadog tracing, not a replacement — filling in the JVM class identity layer that observability tools currently skip.

---

## What Would Strengthen the Hypothesis

1. A production LLM framework (Spring AI, LangChain4j) where `FunctionCallback` dispatch does NOT log the concrete handler class by default — verify empirically.
2. An AI agent demonstration that autonomously discovers the bug via `/causality/search` without human-provided class names.
3. An exploration API on the causality server: `GET /causality/callsites?target_method=execute` — returns all call sites that reached any `execute()` method, without requiring the caller to know class names.

Without (1), the claim that "real frameworks don't log this" is an assertion, not a demonstration. That verification is the most important next step.
