# Phase V10 — Agent Dispatch Demo

**Date:** 2026-05-31  
**App:** `tools/demo-agent-dispatch/`  
**Bug type:** Tool registry collision — lower-priority implementation silently overwrites higher-priority one  
**Port:** 19100  
**Capture dir:** `/tmp/demo_agent_dispatch/`

---

## Bug Design

### The Setup

A Spring Boot app simulates an LLM agent framework. Three tool implementations compete for the same logical slot (`send_email`):

| Class | @Order | toolName() | Behavior |
|---|---|---|---|
| `SendgridEmailTool` | 1 (highest) | send_email | Calls Sendgrid API |
| `SESMailTool` | 2 | send_email | Calls AWS SES API |
| `MockEmailTool` | MAX_VALUE (lowest) | send_email | Discards silently — test only |

### The Registry Bug

```java
// ToolRegistry constructor — BUG IS HERE
public ToolRegistry(List<AgentTool> allTools) {
    for (AgentTool tool : allTools) {
        registry.put(tool.toolName(), tool);  // last-write-wins
    }
}
```

Spring delivers `List<AgentTool>` in `@Order` sequence: `SendgridEmailTool` (1) first, `MockEmailTool` (MAX_VALUE) last. Because `HashMap.put()` overwrites, `MockEmailTool` is the final writer and wins.

**Developer intent:** First-registered (highest-priority) implementation wins.  
**Actual behavior:** Last-registered (lowest-priority) implementation wins.  
**Consequence:** Emails are silently discarded in production.

The fix is one character: `putIfAbsent()` instead of `put()`.

### Why This Is Realistic

Spring AI's `FunctionCallback` and LangChain4j's tool registry both maintain maps of tool name → handler. Production deployments regularly have test/mock implementations left on the classpath. The @Order vs. Map.put() interaction is a real class of configuration bug.

---

## Application Structure

```
tools/demo-agent-dispatch/
├── pom.xml                                          (Spring Boot 4.0.6)
└── src/main/java/com/example/agentdemo/
    ├── AgentController.java                         (GET /run, GET /registry)
    ├── agent/AgentService.java                      (simulates LLM task execution)
    ├── registry/ToolRegistry.java                   (the buggy registry)
    └── tool/
        ├── AgentTool.java                           (interface: toolName, execute, description)
        ├── SendgridEmailTool.java                   (@Order 1)
        ├── SESMailTool.java                         (@Order 2)
        ├── MockEmailTool.java                       (@Order MAX_VALUE — the intruder)
        ├── DatabaseQueryTool.java                   (unambiguous)
        └── CalendarTool.java                        (unambiguous)
```

### Reflection Dispatch Path

`ToolRegistry.dispatch()` uses `Method.invoke()` — same pattern as real LLM frameworks:

```java
public DispatchResult dispatch(String toolName, Map<String, Object> input) {
    AgentTool tool = registry.get(toolName);
    Method method = tool.getClass().getMethod("execute", Map.class);
    String result = (String) method.invoke(tool, input);  // reflection invoke
    return new DispatchResult(toolName, tool.getClass().getName(), result, null);
}
```

This is intentional: Spring AI's `FunctionCallback` and LangChain4j's `ToolExecutor` both use reflection dispatch. RT records the concrete target class of every `Method.invoke()` call.

---

## Live Capture Results

### `/registry` endpoint response

```json
{
  "current_registry": {
    "schedule_meeting": "CalendarTool",
    "query_database":   "DatabaseQueryTool",
    "send_email":       "MockEmailTool"
  },
  "registration_log": [
    { "tool_name": "send_email", "registered": "SendgridEmailTool", "overwrote": null         },
    { "tool_name": "send_email", "registered": "SESMailTool",       "overwrote": "SendgridEmailTool" },
    { "tool_name": "schedule_meeting", "registered": "CalendarTool", "overwrote": null        },
    { "tool_name": "query_database",   "registered": "DatabaseQueryTool", "overwrote": null   },
    { "tool_name": "send_email", "registered": "MockEmailTool",     "overwrote": "SESMailTool" }
  ]
}
```

### `/run?task=onboard_user` endpoint response

```json
{
  "task": "onboard_user",
  "mock_tool_activated": true,
  "WARNING": "MockEmailTool is active — emails are being silently discarded.",
  "steps": [
    {
      "tool_name":  "query_database",
      "impl_class": "com.example.agentdemo.tool.DatabaseQueryTool",
      "result":     "DB_RESULT: [{\"row\":1,\"value\":\"example\"}]..."
    },
    {
      "tool_name":  "send_email",
      "impl_class": "com.example.agentdemo.tool.MockEmailTool",
      "result":     "MOCK_NOOP: email discarded (MockEmailTool active — no email sent to alice@example.com)"
    },
    {
      "tool_name":  "schedule_meeting",
      "impl_class": "com.example.agentdemo.tool.CalendarTool",
      "result":     "CALENDAR_OK: meeting 'Onboarding call with Alice' scheduled..."
    }
  ]
}
```

---

## RT Evidence in JSONL

Total records: **49,017** (34,146 callsite_target + 3,213 runtime_target + rest)

### Key dispatch records (ToolRegistry.dispatch → execute(), BCI 95)

**callsite_target** — first observed target at BCI 95:
```json
{
  "record":          "callsite_target",
  "category":        "reflection_method_invoke",
  "evidence":        "OBSERVED_ONLY",
  "source_class":    "com/example/agentdemo/registry/ToolRegistry",
  "source_method":   "dispatch",
  "source_bci":      95,
  "target_class":    "com/example/agentdemo/tool/DatabaseQueryTool",
  "target_method":   "execute"
}
```

**runtime_target** records — subsequent targets from the same BCI:
```json
{
  "record":          "runtime_target",
  "evidence":        "LINKAGE_GUARANTEED",
  "dispatch_kind":   "methodhandle_linkage",
  "source_class":    "com/example/agentdemo/registry/ToolRegistry",
  "source_method":   "dispatch",
  "source_bci":      95,
  "target_class":    "com/example/agentdemo/tool/MockEmailTool",
  "target_method":   "execute"
}
```

```json
{
  "record":          "runtime_target",
  "evidence":        "LINKAGE_GUARANTEED",
  "dispatch_kind":   "methodhandle_linkage",
  "source_class":    "com/example/agentdemo/registry/ToolRegistry",
  "source_method":   "dispatch",
  "source_bci":      95,
  "target_class":    "com/example/agentdemo/tool/CalendarTool",
  "target_method":   "execute"
}
```

### RT record semantics

RT emits one `callsite_target` per unique `(source_class, source_method, source_bci, target_class)` tuple. When the same BCI is called with three different target classes (DatabaseQueryTool, MockEmailTool, CalendarTool), the first becomes `callsite_target`. The others become `runtime_target` with `dispatch_kind: methodhandle_linkage`. All three are captured.

The `reflection_method_invoke` category on the callsite_target record marks this as a Java reflection call. The concrete target class (not the interface `AgentTool`) is recorded.

---

## What RT Knows Passively

From the JSONL alone, without any app-level logging:

1. `ToolRegistry.dispatch` at BCI 95 reached `MockEmailTool.execute` — RT knows MockEmailTool was invoked.
2. The same BCI reached three distinct classes — RT knows the registry dispatches polymorphically.
3. `BeanUtils.instantiateClass` reached `MockEmailTool.<init>` — RT knows MockEmailTool was instantiated by Spring (it's on the classpath and autowired).

RT does NOT know from JSONL alone: the tool name `send_email`, whether the dispatch was correct, or what the business consequence was. That context comes from combining RT with the app's `/run` response.
