# Phase V10.6 Part 2 — APM Handler Visibility (OTel, Datadog, Dynatrace)

**Date:** 2026-06-01  
**Question:** Do standard APM tools — OpenTelemetry Java agent, Datadog dd-trace-java, Dynatrace OneAgent — expose the concrete Java implementation class handling a tool call?  
**Method:** Empirical for OTel (actual agent run, actual output). Source analysis for Datadog (dd-java-agent repository). Architectural analysis for Dynatrace.

---

## Part A — OpenTelemetry Java Agent v2.28.1

### Test Setup

Spring AI demo app from V10.5 (`tools/demo-spring-ai/`, port 19201) launched with OTel Java agent attached:

```
java \
  -javaagent:/tmp/demo_spring_ai_otel/opentelemetry-javaagent-2.28.1.jar \
  -Dotel.traces.exporter=logging \
  -Dotel.metrics.exporter=none \
  -Dotel.logs.exporter=none \
  -jar target/demo-spring-ai-1.0.0.jar
```

Exporter: `logging` — all spans emitted to stderr, nothing buffered or dropped.

Two requests executed:
```
curl http://localhost:19201/run/production
curl http://localhost:19201/run/mock
```

### Actual OTel Output

Complete OTel span output (entire stderr, verbatim):

```
[otel.javaagent 2026-06-01 00:23:16:752 -0400] [main] INFO
  io.opentelemetry.javaagent.tooling.VersionLogger -
  opentelemetry-javaagent - version: 2.28.1

[otel.javaagent 2026-06-01 00:23:25:904 -0400] [http-nio-19201-exec-2] INFO
  io.opentelemetry.exporter.logging.LoggingSpanExporter -
  'GET /run/production' : 6bff644cfcf420998e8d553d3a69a4f8 8cb2cf846ecd2866 SERVER
  [tracer: io.opentelemetry.tomcat-10.0:2.28.1-alpha]
  AttributesMap{data={
    user_agent.original=curl/8.7.1,
    network.protocol.version=1.1,
    server.port=19201,
    url.scheme=http,
    thread.id=43,
    thread.name=http-nio-19201-exec-2,
    http.response.status_code=200,
    network.peer.address=0:0:0:0:0:0:0:1,
    server.address=localhost,
    client.address=0:0:0:0:0:0:0:1,
    url.path=/run/production,
    http.request.method=GET,
    http.route=/run/production,
    network.peer.port=56552
  }, capacity=128, totalAddedValues=14}

[otel.javaagent 2026-06-01 00:23:25:920 -0400] [http-nio-19201-exec-3] INFO
  io.opentelemetry.exporter.logging.LoggingSpanExporter -
  'GET /run/mock' : 6dfed0eae5f81aa7dd79802b7b65df58 a8415a87626f9e5f SERVER
  [tracer: io.opentelemetry.tomcat-10.0:2.28.1-alpha]
  AttributesMap{data={
    user_agent.original=curl/8.7.1,
    network.protocol.version=1.1,
    server.port=19201,
    url.scheme=http,
    thread.id=44,
    thread.name=http-nio-19201-exec-3,
    http.response.status_code=200,
    network.peer.address=0:0:0:0:0:0:0:1,
    server.address=localhost,
    client.address=0:0:0:0:0:0:0:1,
    url.path=/run/mock,
    http.request.method=GET,
    http.route=/run/mock,
    network.peer.port=56553
  }, capacity=128, totalAddedValues=14}
```

**Total spans emitted: 2. Both are HTTP SERVER spans. No child spans of any kind.**

### Analysis

| Question | Answer |
|---|---|
| Does OTel produce a span for tool dispatch? | No |
| Does any span name mention `sendEmail`, `MethodToolCallback`, etc.? | No |
| Does any span attribute contain the tool name? | No |
| Does any span attribute contain the implementing class? | No |
| Does `/run/production` and `/run/mock` produce different spans? | No — identical structure, different trace/span IDs only |
| Can a backend (Jaeger, Tempo, Datadog) distinguish the two requests by implementation? | No |

### Why OTel Doesn't Cover This

OTel Java agent instruments frameworks by bytecode transformation of known classes. The instrumentation catalog for v2.28.1 includes:

- HTTP servers: Servlet, Tomcat, Netty, Undertow
- HTTP clients: HttpClient, OkHttp, Apache HttpClient
- Databases: JDBC, MongoDB, Redis, Cassandra
- Messaging: Kafka, RabbitMQ, JMS
- RPC: gRPC
- Runtimes: Spring Web MVC (HTTP routing), Spring Webflux

There is no `spring-ai` instrumentation module. There is no `langchain4j` instrumentation module. The tool dispatch path — `DefaultToolCallingManager` → `MethodToolCallback` → `Method.invoke()` — is not in the instrumentation catalog and generates no spans.

This is not a configuration issue. No configuration option in OTel Java agent enables Spring AI tool dispatch tracing because the bytecode transformation for those classes does not exist.

---

## Part B — Datadog dd-trace-java

### Method

Source analysis of `DataDog/dd-trace-java` repository, `instrumentation/` directory.

### Findings

Datadog dd-trace-java contains an `openai-java` instrumentation module:

```
dd-java-agent/instrumentation/openai-java/openai-java-3.0/
  ChatCompletionServiceInstrumentation.java
```

`ChatCompletionServiceInstrumentation` instruments `com.openai.services.blocking.chat.ChatCompletionServiceImpl`. This is the OpenAI Java SDK's HTTP client — the class that sends the HTTP request to `api.openai.com` and receives the response. It instruments the boundary between application code and the OpenAI API network call.

**What it captures:** HTTP request to OpenAI (model, prompt tokens, completion tokens, finish_reason). **What it does not capture:** Anything inside the application after the response is received — including tool dispatch.

The tool dispatch path in Spring AI (`DefaultToolCallingManager` → `MethodToolCallback.call()`) executes after the ChatModel receives the response from OpenAI. The Datadog instrumentation ends at the HTTP boundary. The Java classes that execute tool calls are never instrumented.

**Spring AI instrumentation module:** Does not exist in `dd-trace-java`.  
**LangChain4j instrumentation module:** Does not exist in `dd-trace-java`.

### What Datadog Would Show

For a Spring AI app that calls OpenAI and dispatches tools:

```
Span: POST https://api.openai.com/v1/chat/completions
  model: gpt-4o
  prompt_tokens: 142
  completion_tokens: 38
  finish_reason: tool_calls
```

No child span for tool execution. No `sendEmail` in any span. No implementing class in any attribute.

The tool_calls response from OpenAI (which names `sendEmail`) is visible in the Datadog span only if the raw response body is captured — which is not done by default and involves PII risk. Even if captured, it would show the tool name as a JSON string inside the response body, not as a structured span attribute.

---

## Part C — Dynatrace OneAgent

### Method

Architectural analysis based on Dynatrace OneAgent instrumentation scope and Spring AI/LangChain4j support documentation.

### Findings

Dynatrace OneAgent instruments Java applications at the bytecode level. Its Java instrumentation covers:

- HTTP servers and clients (Spring MVC, REST endpoints)
- Database calls (JDBC)
- Messaging (Kafka, JMS)
- Remote calls (gRPC, RMI)
- Spring Framework: dependency injection, `@Service`, `@Controller`, `@Repository` — these get process group / service detection, not method-level spans

**Spring AI and LangChain4j:** No instrumentation. Dynatrace has published no blog post, documentation, or GitHub issue announcing Spring AI integration for tool dispatch tracing. The tool execution path generates no Dynatrace PurePath nodes.

**Custom instrumentation option:** Dynatrace supports `@Trace` annotation and OneAgent SDK for custom spans. If a developer adds `@Trace` to `sendEmail()` in `ProductionEmailService`, that method appears in the PurePath. This requires code modification — it is not passive.

---

## Part D — LangSmith

LangSmith is LangChain's observability platform. It has deep integration with LangChain (Python) and LangGraph, including tool call tracing. **Java support: none.** LangSmith has no Java SDK and no integration with LangChain4j. This line of inquiry is closed.

---

## Summary

| Tool | Passively captures tool dispatch? | Passively exposes implementing class? |
|---|---|---|
| OpenTelemetry Java agent v2.28.1 | No | No |
| Datadog dd-trace-java | No | No |
| Dynatrace OneAgent | No | No |
| LangSmith | No Java support | No |

**None of these tools passively produce `{"tool": "sendEmail", "implementation": "MockEmailService"}`.**

The shared reason: all four tools instrument at framework or protocol boundaries (HTTP, JDBC, messaging). The Java-level object that handles a tool invocation is inside the application layer — after the HTTP response from the LLM is received, inside the tool dispatch loop that none of these tools instrument.

---

## Verdict

**C — No APM tool in this audit passively exposes the concrete Java class handling an AI tool call.**

This is not because the tools are inadequate in general. It is because tool dispatch in Spring AI and LangChain4j is pure Java reflection inside the application process, and none of these tools have instrumented those specific classes. The gap is a coverage gap, not a fundamental limitation — but it is currently a real gap.

RT fills it by instrumenting `Method.invoke()` at the JVM level, which is framework-agnostic and requires no code modification.
