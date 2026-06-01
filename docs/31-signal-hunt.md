# doc 31 — Phase V9: Signal Hunt

**Date:** 2026-05-31  
**Objective:** Find where Runtime Truth is genuinely differentiated. Not merely working — differentiated.

---

## Framing

Runtime Truth records one thing: at every call site in user code, which concrete class actually handled the call. This is a per-callsite dispatch target log, queryable after execution.

The question for V9 is: **in which bug categories is this specific fact both hard to get and worth having?**

Three conditions must all be true for a strong signal:

1. **The bug involves dispatch ambiguity** — the "which class handled the call" question is the actual root cause, not an incidental fact.
2. **Competing tools don't answer this passively** — the developer cannot get the same answer from logs, stack traces, APM spans, or Undo without prior setup.
3. **The population with this problem is large enough to matter** — niche bugs in rare frameworks are not a product opportunity.

---

## Research Findings (V9 Competitive Survey)

Before evaluating domains, the following was confirmed about competing tools:

**Undo / LiveRecorder:**  
Records all execution. On Linux only. Query model is a GDB extension Python API — time-travel debugging, not queryable JSONL. No REST API. Requires recording to have started before the event. Even if Undo added a REST API, the query model is "what was the state at time T?" not "what was the target of every callsite invocation?" The data models differ.

**Java Flight Recorder (JFR):**  
Sampling-based profiler. Records thread samples, lock contention, GC events. Does NOT record per-callsite dispatch target classes. Sampling means most invocations are not captured. Not useful for "which class handled this specific call?"

**Datadog APM / Dynatrace OneAgent:**  
Instruments method entry/exit via agent. Creates spans for `@Transactional`, `@Async`, etc. intercept points. Records timing and context. Does NOT record which concrete class was the dispatch target of a call. Shows "processOrder started a transaction span" but not "processOrder dispatched to OrderService$$SpringCGLIB$$0 vs OrderService."

**LangSmith / Spring AI Observability:**  
Captures LLM API calls, tool invocation sequences, prompt/completion pairs. Records at the framework event level (tool name, parameters). Does NOT record JVM-level dispatch — which implementation class handled the tool call.

**Custom bytecode instrumentation (ASM/ByteBuddy agents):**  
Could be built to record what RT records, but requires per-project configuration, separate development, and maintenance. Not a zero-configuration tool.

**Conclusion:** No existing mainstream tool provides passive, per-callsite, queryable dispatch target attribution for Java applications.

---

## Methodology

15 categories were evaluated (see doc 32). For each:
- Described a representative real-world bug
- Assessed whether the "which class handled the call" question is the root cause
- Scored competing workflows (stack trace, Undo, APM tools, RT)
- Produced a differentiation score (1–10) reflecting how much better RT is than alternatives

---

## Key Insight: Human vs. Agent Usefulness Differ

The most important finding of V9 is that human usefulness and AI-agent usefulness are **substantially different** for the same bugs.

For a **human engineer**:
- Stack traces + debugger can solve most proxy-related bugs, given time and expertise
- The question is: how much expertise is required, and how long does it take?
- RT reduces debugging time and expertise requirements, but rarely makes the bug impossible without it

For an **AI agent (Claude, GPT, Cursor, Copilot)**:
- An agent cannot set breakpoints, cannot attach a debugger, cannot trigger exceptions at will
- An agent CAN query structured JSONL (RT output) programmatically
- An agent CAN call REST APIs (causality/search, causality/explain)
- For an agent, RT is the difference between guessing and knowing

This asymmetry is significant. RT's strongest case is not "helps human engineers" — it's "enables AI agents to debug dynamically-dispatched Java code without executing it again."

---

## The Undo Stress Test

For each candidate, the question was asked: **"If Undo added a REST API tomorrow, would our advantage disappear?"**

This revealed a key structural difference:

Undo's model: "Given a specific point in recorded execution, what was the state?"  
RT's model: "Given this callsite (class, method, BCI), what target classes were observed across all executions?"

Even with a Undo REST API:
- You'd query: "What was the call stack at line X when method Y executed?"
- You'd get: the stack at that specific moment
- You'd need to have started recording before the event
- You'd need to reproduce the bug with recording enabled (usually requires Linux, significant overhead)

RT provides:
- All callsite targets across all executions, in a single JSONL
- Zero overhead (written on JVM shutdown)
- No reproduction required — run once, analyze indefinitely
- Cross-platform (macOS, Linux, Windows)
- Queryable by AI agents via REST

The Undo REST API would not eliminate RT's advantage for post-mortem analysis, AI-agent consumption, and cross-platform support.

---

## What V9 Found

Three categories emerged as having defensible RT advantages even against a hypothetical Undo REST API:

1. **AI agent tool dispatch** — growing market, no tooling, maps directly to RT's capability
2. **Multi-level proxy chains (4+ layers)** — stack traces become illegible, APM doesn't distinguish layers
3. **Runtime-loaded implementations (ServiceLoader, OSGi, plugin systems)** — static analysis blind spots

These are documented in detail in doc 32 and ranked in doc 33.
