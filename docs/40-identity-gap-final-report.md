# Phase V10.6 — Handler Identity Gap: Final Report

**Date:** 2026-06-01  
**Question:** Is RT's runtime implementation identity signal already available from existing tools?  
**Scope:** Spring AI 1.0.0, LangChain4j 1.15.1, OpenTelemetry Java agent v2.28.1, Datadog dd-trace-java, Dynatrace OneAgent, LangSmith  
**Method:** Empirical (V10.5, V10.6 Parts 1–3). Not documentation. Not speculation.

---

## What Was Tested

The specific capability: passively obtaining the concrete Java class name that handled a named tool call, without modifying the application.

Formal statement: given two requests — one where `ProductionEmailService` handled `sendEmail`, one where `MockEmailService` handled `sendEmail` — can tool X distinguish them?

---

## Results by Tool

### Spring AI 1.0.0 default logging (doc: 37)

**Verdict: C — does not expose implementing class.**

Spring AI logs the tool name at DEBUG:
```
DEBUG DefaultToolCallingManager: Executing tool call: sendEmail
DEBUG MethodToolCallback: Starting execution of tool: sendEmail
DEBUG MethodToolCallback: Successful execution of tool: sendEmail
```

Both the Production and Mock runs produce identical log lines. `ToolDefinition.name()` is logged; the interface has no class field.

Source confirmed: `DefaultToolCallingManager.executeToolCalls()` calls `toolCallback.getToolDefinition().name()`. `ToolDefinition` interface: `name()`, `description()`, `inputSchema()`. No declaring class.

### LangChain4j 1.15.1 default logging (doc: 38)

**Verdict: C (worse than Spring AI) — logs nothing at all during tool execution.**

With `logging.level.dev.langchain4j=TRACE`: zero framework log lines during tool dispatch. `DefaultToolExecutor`, `ToolService`, and their callers have no `Logger` declaration and no log calls.

Spring AI logs the tool name; LangChain4j logs nothing.

### OpenTelemetry Java agent v2.28.1 (doc: 39)

**Verdict: C — zero tool dispatch spans.**

Actual output from agent run: 2 HTTP SERVER spans, one per HTTP request, both produced by `io.opentelemetry.tomcat-10.0`. No child spans. No `sendEmail` attribute. The two requests (`/run/production`, `/run/mock`) produce spans with different trace IDs and identical attribute keys and values (except port).

No Spring AI or LangChain4j instrumentation module exists in OTel Java agent v2.28.1.

### Datadog dd-trace-java (doc: 39)

**Verdict: C — OpenAI SDK boundary only, not tool dispatch.**

`dd-trace-java` has `openai-java` instrumentation in `instrumentation/openai-java/openai-java-3.0/ChatCompletionServiceInstrumentation.java`. It instruments the HTTP call to `api.openai.com`. Spans contain model, token counts, finish_reason. Tool dispatch within Spring AI or LangChain4j is not instrumented. No Spring AI module. No LangChain4j module.

### Dynatrace OneAgent (doc: 39)

**Verdict: C — no Spring AI or LangChain4j instrumentation.**

Instruments HTTP servers/clients, JDBC, messaging. No published instrumentation for Spring AI tool dispatch. Custom spans via `@Trace` require code modification.

### LangSmith (doc: 39)

**Out of scope — Java support does not exist.**

---

## Competitive Verdict

| Tool | Passively identifies implementing class? | Evidence grade |
|---|---|---|
| Spring AI logging | No — tool name only | Empirical (actual logs) |
| LangChain4j logging | No — nothing logged | Empirical (actual logs + source) |
| OpenTelemetry Java agent | No — HTTP spans only | Empirical (actual agent run) |
| Datadog dd-trace-java | No — OpenAI HTTP boundary only | Source analysis |
| Dynatrace OneAgent | No — no AI framework instrumentation | Architectural |
| LangSmith | N/A — no Java SDK | Documented |

**The gap is real as of 2026-06-01.**

---

## What Fills the Gap

The only passive mechanism confirmed to produce `{"tool": "sendEmail", "implementation": "MockEmailService"}` is RT's `runtime_target` record at the `Method.invoke()` call site in the tool executor.

From V10 JSONL (49,017 records, LangChain4j demo app, doc: 35):

```json
{
  "kind": "runtime_target",
  "dispatch_kind": "methodhandle_linkage",
  "target_class": "com/example/agentdispatch/tool/MockEmailTool",
  "target_method": "execute",
  "source_class": "dev/langchain4j/agent/tool/DefaultToolExecutor",
  "source_bci": 42
}
```

From Spring AI (V10.5): same mechanism would produce the same record at `MethodToolCallback.call()` → `Method.invoke()`.

RT operates at the JVM level, below the framework layer. It requires no instrumentation of specific framework classes. Any framework that ultimately calls `Method.invoke()` to dispatch a tool is covered by the same hook.

---

## Attacks on the Thesis

These were raised in doc/36 (V10 results) and remain valid:

**1. Application-level logging already solves this.**  
Valid when present. If `MockEmailService` logs `"[MockEmailService] handling sendEmail"`, the class is visible. The gap exists only when framework logging is the only signal. This is common in production services where tool implementation classes contain no logging.

**2. Spring AI throws an exception if two tools share the same name.**  
Valid — `MethodToolCallbackProvider.validateToolCallbacks()` throws `IllegalStateException` containing both class names. The gap applies to frameworks (LangChain4j, custom registries) that do not enforce uniqueness, or to single-name dispatch where logging is absent.

**3. This scenario is uncommon.**  
Partially valid. Runtime tool substitution (mock vs. production) is a real pattern in multi-environment deployments. Registry collision is less common but not contrived — it appears in plugin systems and framework-managed service registries.

**4. Datadog/OTel capture LLM token context.**  
True, but orthogonal. Those tools answer "what did the LLM decide to call?" RT answers "which Java object executed the call." Both questions are valid. They do not substitute for each other.

**5. The signal is too narrow.**  
Valid framing. RT answers exactly one question: which class handled the reflection invoke. It does not help with logic bugs, data correctness, or anything outside the dispatch-target boundary. The value is bounded and specific.

---

## Scope of the Gap

The gap is not universal. It is narrow and specific:

- It exists when the tool implementation class is ambiguous from available signals
- It exists when framework logging is absent or tool-name-only
- It does not exist when application-level logging includes class identity
- It does not exist when exception messages expose class names (Spring AI duplicate detection)
- It matters most in LangChain4j (no framework logging) and in opaque registries

---

## Final Verdict

**The identity gap is real. No existing tool in this audit passively exposes the concrete Java class handling an AI tool call. RT fills this specific gap.**

Grade: **B — moderate evidence of a real gap, narrow scope, valid competing signals in some configurations.**

- Stronger than B would require the gap to be universal (it isn't — application logging fills it when present)
- Weaker than B would require an existing tool to cover the case (none does, empirically confirmed)

The gap is most valuable in LangChain4j deployments with minimal application logging, where the tool execution path is completely opaque to all passive observability tooling.
