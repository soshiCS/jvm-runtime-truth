# Phase V10 — Agent Debugging Hypothesis

**Date:** 2026-05-31  
**Question being tested:** Is Runtime Truth meaningfully more valuable when the debugger is an AI agent rather than a human engineer?

---

## The Hypothesis

AI agents cannot add log statements, cannot recompile code, and cannot attach debuggers. They must debug from the information already present in the system.

Traditional debugging techniques assume a human who can:
1. Read a stack trace and infer what to add
2. Modify source code, rebuild, and rerun
3. Use IDE breakpoints interactively

An AI debugging agent has none of these options in production. It must query existing APIs and interpret existing signals.

**Claim:** For AI agent debugging, RT's REST-queryable passive dispatch record has no direct equivalent in any mainstream tool. For human debugging, it does (logging, APM spans, debuggers).

---

## Why This Might Be True

1. **Zero-instrumentation requirement** — An AI agent that encounters a production bug cannot add `log.info("dispatching to {}", implClass)`. RT captures this passively, with no code changes.

2. **REST-queryable structure** — RT's `/causality/` APIs return structured JSON. An AI agent can ask "which class handled this tool invocation?" and get a direct answer without parsing log files.

3. **Class-level vs. name-level granularity** — LangSmith, LangFuse, and most AI observability tools record the logical tool name (`send_email`) but not the JVM class that handled it (`MockEmailTool`). When multiple classes implement the same tool name, this distinction is invisible to those tools.

4. **No observe-perturb problem** — Adding logging to a registry's dispatch method changes code paths. In frameworks like Spring AI or LangChain4j, the dispatch is internal. You cannot add logging without forking the framework. RT requires no such fork.

---

## Why This Might Be False

1. **The app can return impl_class itself** — A well-designed registry endpoint returns which class is registered. If the app already exposes this (as our demo does), RT provides nothing additional.

2. **The registry bug is visible without dispatch tracking** — `GET /registry` shows `"send_email": "MockEmailTool"`. An AI agent that calls the right API doesn't need RT at all.

3. **LangSmith captures enough for most AI debugging** — LangSmith records tool name, input, output, and latency. For most AI agent bugs (wrong output, wrong tool selected, hallucinated parameters), this is sufficient. JVM class identity is rarely the issue.

4. **The collision scenario is rare** — Most production systems don't have three implementations of the same logical tool name competing for registry slots. The scenario that makes RT decisive — class-level collision invisible at the tool-name level — is a code smell that most teams would catch in code review.

5. **A single log statement closes the gap for humans** — `log.info("[dispatch] {} -> {}", toolName, tool.getClass().getSimpleName())` takes 30 seconds to write. RT provides zero-instrumentation capture of exactly this, but the human-with-logging workflow is nearly as fast once the bug is suspected.

---

## What Would Constitute Strong Evidence

**Evidence A (strong):** A scenario where the correct class handling an invocation is unknowable via any other passive mechanism, and an AI agent correctly identifies the bug using RT but fails without it.

**Evidence B (moderate):** A scenario where RT surfaces the answer significantly faster than the next-best approach, and the speedup matters in a production debugging context.

**Evidence C (weak):** A scenario where RT captures the information, but an alternative approach (logging, APM, REST endpoint) also captures it and is comparably accessible.

---

## Methodology

1. Build a real agent dispatch scenario with a genuine registry collision bug (Phase V10 app).
2. Define four debugging workflows (A: RT + AI, B: no-RT + AI, C: RT + human, D: traditional human).
3. Document what each workflow can determine passively — without code modification.
4. Evaluate which workflows reach the correct root cause, and at what cost.
5. Check whether any competitor tool already covers the gap.
6. Issue a final A/B/C verdict.

---

## Scope Limitations

This evaluation covers one specific class of bug: **dispatch target ambiguity** — multiple implementations of the same logical operation, with one silently overwriting others. This is not all AI agent bugs. It is the class of bugs most likely to favor RT.

Results should not be generalized to: wrong model parameters, prompt injection, hallucinated tool calls, rate limit errors, latency anomalies, or output quality issues. For all of those, LangSmith/LangFuse/Datadog APM are likely more relevant than RT.
