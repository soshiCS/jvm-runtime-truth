# Phase V10.6 — LangChain4j Handler Visibility

**Date:** 2026-06-01  
**Question:** Does LangChain4j expose the concrete Java implementation class handling a tool call by default?  
**App:** `tools/demo-langchain4j/` (LangChain4j 1.15.1, Spring Boot 3.3.13)  
**Method:** Empirical — actual code, actual logs. Source confirmed.

---

## Test Setup

Three distinct `@Tool`-annotated service classes, same email collision scenario as V10:

| Class | Tool method name | Behavior |
|---|---|---|
| `ProductionEmailService` | `sendEmail` | Sends via Sendgrid (simulated) |
| `MockEmailService` | `sendEmail` | Discards silently |
| `DatabaseService` | `queryDatabase` | DB query |
| `CalendarService` | `scheduleMeeting` | Calendar invite |

Dispatch path exercised: `DefaultToolExecutor.execute()` and `ToolService.executeWithErrorHandling()` — the same path that `AiServices` calls internally after receiving tool call responses from an LLM.

Logging: `logging.level.dev.langchain4j=TRACE` (all LangChain4j packages at trace).

---

## Actual Log Output

### Scenario A — ProductionEmailService handles `sendEmail`

```
INFO  AgentService         - [AgentService] runWithProductionTools
INFO  ProductionEmailService - [ProductionEmailService] sending email to alice@example.com
INFO  DatabaseService      - [DatabaseService] executing: SELECT * FROM users WHERE id=1
INFO  CalendarService      - [CalendarService] scheduling: Onboarding with alice@example.com
```

### Scenario B — MockEmailService handles `sendEmail`

```
INFO  AgentService         - [AgentService] runWithMockTools
INFO  MockEmailService     - [MockEmailService] MOCK: discarding email to alice@example.com
INFO  DatabaseService      - [DatabaseService] executing: SELECT * FROM users WHERE id=1
INFO  CalendarService      - [CalendarService] scheduling: Onboarding with alice@example.com
```

**The total number of log lines from `dev.langchain4j.*` classes during tool execution: ZERO.**

Even with `logging.level.dev.langchain4j=TRACE`, not a single log line from the LangChain4j framework appeared.

---

## Analysis

### What Spring AI logs vs what LangChain4j logs

| Moment | Spring AI 1.0.0 | LangChain4j 1.15.1 |
|---|---|---|
| Before tool execution | `DEBUG DefaultToolCallingManager: Executing tool call: sendEmail` | Nothing |
| During execution | `DEBUG MethodToolCallback: Starting execution of tool: sendEmail` | Nothing |
| After execution | `DEBUG MethodToolCallback: Successful execution of tool: sendEmail` | Nothing |
| Class name anywhere | Never | Never |

LangChain4j is strictly worse than Spring AI for observability. It logs zero information about tool dispatch.

### Source code confirmation

`DefaultToolExecutor` (the class that calls `Method.invoke()`): no `Logger` declaration, no log calls anywhere.  
`ToolService.executeInferenceAndToolsLoop()`: no `Logger` declaration, no log calls.  
`ToolService.internalExecuteTool()`: no `Logger` declaration, no log calls.

The entire tool execution pipeline has zero logging at any log level.

### What `BeforeToolExecution` / `AfterToolExecution` callbacks expose

LangChain4j provides `beforeToolExecution(Consumer<BeforeToolExecution>)` and `afterToolExecution(Consumer<ToolExecution>)` hooks. These receive:
- `ToolExecutionRequest` — contains tool name and JSON arguments
- `InvocationContext` — session context

Neither contains the implementing object, the implementing class, or a method reference. The hooks tell you the tool was called by name; they do not tell you which class handled it.

### Where class names DO appear

Only in the tool implementation's own application-level logging (if the developer added it) — identical to Spring AI. If `MockEmailService` has no `log.info()` statement, the class name is completely invisible.

---

## Verdict

**C (stronger than Spring AI) — LangChain4j does not expose the implementing class in any logging at any level.**

Spring AI 1.0.0 at least logs the tool name at DEBUG. LangChain4j 1.15.1 logs nothing at all from its tool execution path. Both frameworks fail to surface the implementing class.

---

## Structured Data Question

Can another AI agent get `{"tool": "sendEmail", "implementation": "MockEmailService"}` from LangChain4j?

**No.** There is no structured mechanism — no log field, no callback parameter, no runtime inspection API — that exposes which Java class handled a tool invocation in LangChain4j 1.15.1.

RT fills this gap by recording `target_class: com/example/lc4jdemo/tool/MockEmailService` at the reflection invoke site in `DefaultToolExecutor.execute()`. This is the only passive mechanism.
