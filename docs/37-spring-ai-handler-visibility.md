# Phase V10.5 — Spring AI Handler Visibility

**Date:** 2026-05-31  
**Question:** Does Spring AI expose the concrete Java implementation class handling a tool call by default?  
**Method:** Empirical — run actual code, read actual logs. No documentation assumed.  
**App:** `tools/demo-spring-ai/` (Spring AI 1.0.0, Spring Boot 3.3.13)

---

## Test Setup

Three tool implementations:
- `ProductionEmailService` — `@Tool(name="sendEmail")` — Sendgrid path
- `MockEmailService` — `@Tool(name="sendEmail")` — Silent discard
- `DatabaseService` — `@Tool(name="queryDatabase")`
- `CalendarService` — `@Tool(name="scheduleMeeting")`

Dispatch path: `DefaultToolCallingManager.executeToolCalls()` → `MethodToolCallback.call()` → `Method.invoke()`.  
This is the same code path that `OpenAiChatModel`, `OllamaChatModel`, and all other Spring AI model implementations call internally after receiving tool call responses from an LLM.

Logging configuration: `logging.level.org.springframework.ai=TRACE` (all Spring AI packages at trace level).

---

## Actual Log Output

### Scenario A — Production tools (`sendEmail` → `ProductionEmailService`)

```
INFO  AgentService          - [AgentService] runWithProductionTools — dispatching via DefaultToolCallingManager
INFO  AgentService          - [AgentService]   callback: name=MethodToolCallback definition.name=sendEmail
INFO  AgentService          - [AgentService]   callback: name=MethodToolCallback definition.name=queryDatabase
INFO  AgentService          - [AgentService]   callback: name=MethodToolCallback definition.name=scheduleMeeting

DEBUG DefaultToolCallingManager  - Executing tool call: sendEmail
DEBUG MethodToolCallback         - Starting execution of tool: sendEmail
INFO  ProductionEmailService     - [ProductionEmailService] sending email to alice@example.com
DEBUG MethodToolCallback         - Successful execution of tool: sendEmail
DEBUG DefaultToolCallResultConverter - Converting tool result to JSON.

DEBUG DefaultToolCallingManager  - Executing tool call: queryDatabase
DEBUG MethodToolCallback         - Starting execution of tool: queryDatabase
INFO  DatabaseService            - [DatabaseService] executing: SELECT * FROM users WHERE id=1
DEBUG MethodToolCallback         - Successful execution of tool: queryDatabase
DEBUG DefaultToolCallResultConverter - Converting tool result to JSON.

DEBUG DefaultToolCallingManager  - Executing tool call: scheduleMeeting
DEBUG MethodToolCallback         - Starting execution of tool: scheduleMeeting
INFO  CalendarService            - [CalendarService] scheduling: Onboarding with alice@example.com
DEBUG MethodToolCallback         - Successful execution of tool: scheduleMeeting
DEBUG DefaultToolCallResultConverter - Converting tool result to JSON.
```

### Scenario B — Mock tools (`sendEmail` → `MockEmailService`)

```
DEBUG DefaultToolCallingManager  - Executing tool call: sendEmail
DEBUG MethodToolCallback         - Starting execution of tool: sendEmail
INFO  MockEmailService           - [MockEmailService] MOCK: discarding email to alice@example.com
DEBUG MethodToolCallback         - Successful execution of tool: sendEmail
DEBUG DefaultToolCallResultConverter - Converting tool result to JSON.
```

### Scenario C — Duplicate tool names (both registered)

Spring AI throws **before any execution**:
```
java.lang.IllegalStateException: Multiple tools with the same name (sendEmail) found in sources:
  com.example.springaidemo.tool.ProductionEmailService,
  com.example.springaidemo.tool.MockEmailService,
  com.example.springaidemo.tool.DatabaseService,
  com.example.springaidemo.tool.CalendarService
```

---

## Analysis: Which Information Is Present

### From Spring AI framework logs only (`DefaultToolCallingManager`, `MethodToolCallback`)

| Question | Answer | Evidence |
|---|---|---|
| Tool name visible? | **YES** | `Executing tool call: sendEmail` |
| Callback type visible? | **YES** | Logger is `MethodToolCallback` — confirms @Tool-method dispatch |
| Concrete Java class visible? | **NO** | Nothing in Spring AI logs names `ProductionEmailService` or `MockEmailService` |
| Proxy class visible? | **NO** | Not present |
| Reflection target class visible? | **NO** | `callMethod()` calls `toolMethod.invoke()` with no logging |
| Which implementation ran? | **NO** | Both scenarios produce identical Spring AI log lines |

### The critical line that is identical in both scenarios

```
DEBUG DefaultToolCallingManager - Executing tool call: sendEmail
DEBUG MethodToolCallback        - Starting execution of tool: sendEmail
DEBUG MethodToolCallback        - Successful execution of tool: sendEmail
```

These three lines are **byte-for-byte identical** whether `ProductionEmailService` or `MockEmailService` handled the call. There is no information in any Spring AI framework log line that distinguishes the two.

### Where the class name DOES appear

**Source 1 — Application code logging (not Spring AI):**  
`INFO ProductionEmailService - [ProductionEmailService] sending email to alice@example.com`

This appears because `ProductionEmailService` calls `LoggerFactory.getLogger(ProductionEmailService.class)`. The class name is visible as the **logger name prefix**. This is application-level logging, not Spring AI.

If the tool implementation has no `log.info()` statement, this line disappears entirely and the class is invisible.

**Source 2 — Duplicate-name exception message (not normal operation):**  
Spring AI includes class names in the `IllegalStateException` when duplicate tool names are registered. But this prevents execution — it's an error, not a visibility mechanism.

---

## What Logs a Human Can Read

**With default Spring AI logging (no tool-internal logs):**
- A human sees: `Executing tool call: sendEmail` — knows the tool was invoked
- Cannot determine: which class handled it, whether it was production or mock
- Cannot determine: whether the tool is silently discarding emails or sending them

**With tool-internal application logging (typical in real apps):**
- The class name appears in the logger prefix: `ProductionEmailService` or `MockEmailService`
- A human CAN distinguish the two, because the logger name is the class name
- BUT: this requires the developer to have added logging. It is not provided by Spring AI.

**If MockEmailService logged `log.info("[EmailService] email sent to {}", to)` using a generic logger:**
```
INFO  EmailService - [EmailService] email sent to alice@example.com
```
Indistinguishable from production. The class is hidden.

---

## What an AI Agent Can Read

An AI agent reading logs faces the same constraints as a human, plus one more:

- It can grep for class names. If tool implementations use `getLogger(MyClass.class)`, class names appear in log lines.
- It cannot distinguish production from mock from Spring AI lines alone.
- It cannot query "which class handled `sendEmail`?" from Spring AI logs — there is no such field.

**RT comparison:**  
RT's `callsite_target` with `category: reflection_method_invoke` records `target_class: ProductionEmailService` or `target_class: MockEmailService` regardless of whether that class has any logging. This is the gap.

---

## Verdict

**C — Spring AI does not expose the implementing class in its default logging.**

Spring AI 1.0.0's tool dispatch infrastructure (`DefaultToolCallingManager`, `MethodToolCallback`) logs only the logical tool name at DEBUG level. The declaring class of the `@Tool` method — `ProductionEmailService`, `MockEmailService`, or any other — does not appear in any Spring AI framework log line during normal operation.

The only places class names appear are:
1. Application-level logging added by the developer (outside Spring AI's control)
2. An exception message when duplicate tool names are registered (prevents execution)

---

## Source Code Confirmation

Confirmed against Spring AI 1.0.0 source (`github.com/spring-projects/spring-ai` at tag `v1.0.0`):

`DefaultToolCallingManager.executeToolCall()` (line that logs):
```java
logger.debug("Executing tool call: {}", toolCall.name());
// toolCall.name() = logical tool name. No class reference.
```

`MethodToolCallback.call()` (lines that log):
```java
logger.debug("Starting execution of tool: {}", this.toolDefinition.name());
// ...
logger.debug("Successful execution of tool: {}", this.toolDefinition.name());
// this.toolDefinition.name() = logical tool name. No class reference.
```

`ToolDefinition` interface (what the tool definition contains):
```java
String name();          // logical name: "sendEmail"
String description();   // description string
String inputSchema();   // JSON schema of input parameters
// No declaring class field. No method reference. No bean name.
```

---

## Implication for Runtime Truth

RT records `target_class: com/example/springaidemo/tool/MockEmailService` at the reflection invoke site in `MethodToolCallback.callMethod()` — the exact same call that Spring AI itself makes no attempt to log.

This is a genuine gap, not a documentation claim:
- Spring AI 1.0.0 does not log the class
- The gap is confirmed in source code and in actual output
- RT fills it passively with zero code changes

The gap matters only when the implementing class has no internal logging (or uses a generic logger). When tool implementations use `LoggerFactory.getLogger(ConcreteClass.class)`, application logs provide the class name indirectly. In that case RT provides redundant information.

---

## Reproduction

```bash
# Build
cd tools/demo-spring-ai && mvn package -DskipTests -q

# Run with full TRACE logging
java -jar target/demo-spring-ai-1.0.0.jar --server.port=19200

# Test unambiguous dispatch (class appears in tool's own logs)
curl http://localhost:19200/run/production

# Test mock dispatch (class appears in mock's own logs)  
curl http://localhost:19200/run/mock

# Test duplicate tool name conflict (class names in exception)
curl http://localhost:19200/run/duplicate
```
