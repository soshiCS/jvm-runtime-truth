# doc 32 — Phase V9: Candidate Domain Evaluation

**Date:** 2026-05-31  
**Format:** For each domain: bug description, existing workflow, 4-tool comparison, differentiation score.

Score legend: 1 = RT not better than alternatives, 10 = RT uniquely necessary.

---

## Domain 1: Spring Proxy Chains (1–2 layers)

**Representative bug:** @Transactional self-invocation bypass (V7/V8). processOrder() calls this.saveAuditEntry() — the proxy is bypassed.

**Existing workflow:**
- Stack trace (with exception in saveAuditEntry): shows CGLIB present/absent. Works.
- Logs + TransactionSynchronizationManager: confirms effect. Works.
- APM (Datadog): shows missing tx span. Effect visible, cause not.
- Undo (Linux): step back, inspect call stack. Definitive.

**If Undo had REST API:** advantage largely disappears on Linux.

**RT adds:** zero-instrumentation, passive, cross-platform, AI-queryable. Not unique.

**Human usefulness:** Medium. Stack traces work.  
**Agent usefulness:** High. Agents can't set breakpoints.  
**Differentiation score:** 4/10  
**Verdict:** Good explanatory demo. Not a moat.

---

## Domain 2: Multi-Level Spring Proxy Chains (4+ interceptors)

**Representative bug:** A service bean has @Transactional + @Cacheable + @Retryable + @Secured + a custom AOP aspect. A bug manifests — wrong cache key, or the retry interceptor runs after the transaction commits (incorrect order). The developer needs to know which interceptor executed, in what order, and which one broke the chain.

**Example stack trace during an interceptor bug (5 layers):**
```
at OrderService.placeOrder(OrderService.java:45)
at OrderService$$SpringCGLIB$$0.placeOrder(<generated>)
at CglibAopProxy$DynamicAdvisedInterceptor.intercept(CglibAopProxy.java:...)
at OrderService$$SpringCGLIB$$0.placeOrder(<generated>)
at CglibAopProxy$DynamicAdvisedInterceptor.intercept(CglibAopProxy.java:...)
at OrderService$$SpringCGLIB$$0.placeOrder(<generated>)
at CglibAopProxy$DynamicAdvisedInterceptor.intercept(CglibAopProxy.java:...)
at OrderService$$SpringCGLIB$$0.placeOrder(<generated>)
... (10 more frames all saying the same thing)
```
Stack traces are unreadable. Every frame says `DynamicAdvisedInterceptor.intercept`. You cannot determine from the stack trace which interceptor did what.

**Existing workflow:**
- Stack trace: useless for identifying individual interceptor behavior when all frames look the same
- APM: shows a single span for the method — doesn't distinguish individual interceptor layers
- Debug logging: requires each interceptor to log at entry — usually not present in third-party Spring advisors
- Undo: can step through. But in a 5-layer chain, you need to manually walk through each interceptor level.

**RT adds:** callsite_target records show each `DynamicAdvisedInterceptor.intercept` call with its actual source (which CGLIB proxy called it) and allows tracing the chain. More importantly, it shows the interceptor execution ORDER from the sequence of callsite_target records. The `causality/proxies` API lists every proxy dispatch site in one call.

**If Undo had REST API:** Would help, but requires Linux + recording. RT is zero-setup.

**Human usefulness:** High. Multi-hop CGLIB chains are genuinely hard to read.  
**Agent usefulness:** Very high. An agent with callsite_target records can reconstruct the exact interceptor dispatch order.  
**Differentiation score:** 7/10  
**Verdict:** Real advantage. Harder to diagnose case. Scales with complexity.

---

## Domain 3: ServiceLoader / Plugin Dispatch

**Representative bug:** An application uses `ServiceLoader.load(Serializer.class)` and gets `FastJsonSerializer` in production but `JacksonSerializer` in development. A field is serialized differently. Which serializer ran? The call goes through `ServiceLoader` + `Iterator.next()` + a proxy, and the concrete class is invisible at the call site.

**Existing workflow:**
- Logging: add `log.info("serializer class: {}", serializer.getClass())` — but this requires the developer to already suspect the service loader. Works if you know where to look.
- Stack trace: shows `FastJsonSerializer.serialize(...)` if an exception is thrown there. Works then.
- APM: no span created by default for ServiceLoader dispatch.
- Undo (Linux): step through `ServiceLoader.iterator()` — works, but tedious.

**RT adds:** callsite_target shows the concrete class that was invoked at the SerializerInterface.serialize() callsite. Zero instrumentation. The developer queries `causality/search?q=serialize` and immediately sees which implementation ran.

**If Undo had REST API:** advantage largely disappears.

**Real-world prevalence:** ServiceLoader is used extensively in Java EE, JDBC drivers, SLF4J, JCE providers, JAX-RS implementations. Common in enterprise middleware.

**Human usefulness:** Medium. The fix is usually "add a log," but the RT answer is immediate.  
**Agent usefulness:** High. Agents can't add logging and rerun.  
**Differentiation score:** 6/10  
**Verdict:** Solid. Especially valuable for diagnosing "why did I get a different implementation than expected?"

---

## Domain 4: OSGi Bundle Dispatch (WebSphere, JBoss, Felix)

**Representative bug:** In an OSGi container, `com.example.PaymentProcessor` exists in two bundles with different versions. Bundle A uses v1.2.0, Bundle B uses v1.3.0. A call goes to `PaymentProcessor.process()` and hits the wrong version — the one loaded by Bundle A's classloader — due to an incorrect export/import configuration.

**The insidious detail:** Both classes have the same name. The loader_id differs. You get a `ClassCastException` ("class X is not the class X") or silent wrong-version behavior.

**Existing workflow:**
- OSGi console (Felix gogo shell, WebSphere admin): can inspect bundle wiring. Expert-only.
- Stack trace: shows class name, not which classloader loaded it. Shows `com.example.PaymentProcessor` for both.
- APM: records class name in span, not classloader ID.
- Undo: records full state including classloader, but OSGi on Linux with Undo is complex.

**RT adds:** Every callsite_target record includes `target_loader_id`. This is the classloader that loaded the target class. If two implementations of the same class are on the classpath from different loaders, RT distinguishes them by loader_id. The `causality/explain` API shows which loader handled each dispatch.

This is genuinely unique: no other passive tool records per-callsite loader ID.

**If Undo had REST API:** Would need to know to query the right state variable (loader ID). Less automatic than RT's built-in loader tracking.

**Population:** OSGi is used in WebSphere, JBoss EAP, Eclipse IDE plugins, Apache Karaf, ServiceMix. Large enterprises.

**Human usefulness:** High. OSGi classloader bugs are notoriously hard.  
**Agent usefulness:** Very high. Classloader context is invisible to source code analysis.  
**Differentiation score:** 8/10  
**Verdict:** Strong. Classloader attribution is unique to RT among passive tools.

---

## Domain 5: AI Agent Tool Dispatch (Spring AI, LangChain4j)

**Representative bug:** An AI agent framework (Spring AI, LangChain4j) dispatches tool calls from LLM responses. The framework dynamically selects which `FunctionCallback` (tool implementation) to invoke based on the tool name from the LLM. A bug occurs: the wrong tool is selected (name collision, misconfigured registry), or the tool's method invocation fails silently and falls back to a stub.

The developer asks: "Which Java class actually handled the tool call named 'send_email'?"

**Framework internals (Spring AI example):**
```
FunctionCallingOptionsBuilder → FunctionCallback.resolve(toolName)
  → MyEmailTool.call(toolInput)    ← Which implementation?
```
The resolution goes through a map lookup and reflection or proxy dispatch. The concrete class is not logged by default.

**Existing workflow:**
- LangSmith / Spring AI observability: logs tool name and input/output at the LLM API level. Does NOT show which Java class implementation handled the call.
- Debug logging: requires adding `log.info("dispatching to {}", tool.getClass())` — works but requires code change.
- Stack trace: only appears on exception. No exception = no stack trace.
- Undo (Linux): records everything, but requires Linux + recording enabled.
- APM (Datadog/Dynatrace): auto-instrument `@Service` entry, but don't track which FunctionCallback implementation was resolved at runtime.

**RT adds:** callsite_target for the FunctionCallback.call() or reflection dispatch shows the exact implementation class. Zero instrumentation. Works on macOS. Queryable via causality/search.

**If Undo had REST API:** Advantage shrinks on Linux. But:
- Undo requires recording from the start (must anticipate the bug)
- LangChain4j + Spring AI = typically macOS/Windows dev environments
- RT works where Undo doesn't

**Population:** Every Java team building AI-powered applications with Spring AI, LangChain4j, or LLM tool-use patterns. Growing rapidly in 2025-2026. No current tooling specifically addresses JVM-level tool dispatch attribution.

**Human usefulness:** High. Tool dispatch attribution is invisible without instrumentation.  
**Agent usefulness:** VERY HIGH. An AI agent debugging another AI agent's tool calls is a genuinely novel and compelling use case. RT gives the outer agent structured runtime facts about the inner agent's behavior.  
**Differentiation score:** 9/10  
**Verdict:** Strongest new category. Growing market. No competing tools. Maps exactly to RT's capability. Compelling narrative.

---

## Domain 6: Security Filter Chain Dispatch

**Representative bug:** Spring Security's filter chain has 10 filters. A request is blocked (403) but the developer doesn't know which filter rejected it. The filter chain calls each `Filter.doFilter()` in sequence until one short-circuits.

**Existing workflow:**
- Spring Security DEBUG logging: `logging.level.org.springframework.security=DEBUG` logs which filter was reached. Works, but verbose and requires restart.
- Stack trace: exception thrown from the rejecting filter shows the filter class. Works.
- APM: shows a 403 span but not which filter generated it.
- Undo: can trace back to the filter decision.

**RT adds:** callsite_target for each `FilterChain.doFilter()` call shows which filter class was invoked. Can determine the last filter that received the request.

**Human usefulness:** Medium. DEBUG logging is the standard answer and works fine.  
**Agent usefulness:** Medium. An agent could query RT to see which filter was last invoked.  
**Differentiation score:** 4/10  
**Verdict:** Works, but the existing workflow (DEBUG logging) is simpler and more commonly known.

---

## Domain 7: Mockito/ByteBuddy Instrumented Code in Tests

**Representative bug:** A Mockito mock is configured incorrectly. `when(service.findUser(anyString())).thenReturn(null)` — but the actual call goes to `findByExternalId` instead of `findUser`, and the test passes incorrectly (returns null for both, test doesn't catch the wrong method). Which method was actually called on the mock?

**Existing workflow:**
- Mockito `verify()`: `verify(service).findUser(...)` — this is the standard answer. Works perfectly.
- Mockito `.inOrder()`: verify call sequence.
- `@Captor`: capture arguments.

**RT adds:** callsite_target would show which mock method was dispatched. But Mockito's verify API already does this better, with more ergonomic assertions.

**Human usefulness:** Low. Mockito has this covered.  
**Agent usefulness:** Medium. But agents can read Mockito test failures directly.  
**Differentiation score:** 2/10  
**Verdict:** No advantage. Mockito already solves this better.

---

## Domain 8: Dynamic Language Interop (Groovy, JRuby, JavaScript via GraalVM)

**Representative bug:** A Java application calls into a Groovy DSL via `GroovyShell.evaluate()` or a `Script` interface. The wrong Groovy script version is loaded (classpath ordering issue). Which script class actually ran?

**Existing workflow:**
- Logging: add `log.info("script class: {}", script.getClass())`.
- Stack trace: shows `Script_abc123` (Groovy-generated class name) when exception occurs.
- JFR: can capture class loading events for Groovy-generated classes.
- Undo: records execution.

**RT adds:** generated_class records for Groovy-generated Script classes + callsite_target showing which script was invoked. The generated_class record includes a CRC that is stable across runs for the same script bytecode — allows comparing "did the same script run twice?" without knowing the class name.

**If Undo had REST API:** Advantage shrinks.

**Human usefulness:** Medium.  
**Agent usefulness:** High. Generated class names are opaque to static analysis.  
**Differentiation score:** 5/10  
**Verdict:** Interesting for Groovy-heavy shops. Niche.

---

## Domain 9: Hot-Reload Scenarios (JRebel, Spring Boot DevTools, DCEVM)

**Representative bug:** During local development with hot-reload, a class is reloaded. But some call sites still dispatch to the old class version (cached proxy, uninvalidated CGLIB subclass). A behavior change is expected but doesn't appear. Which version of the class is actually handling calls?

**Existing workflow:**
- Class.getClassLoader().loadClass() inspection: can check if the correct version is loaded.
- JRebel logs: show which classes were reloaded.
- Undo: unavailable (hot-reload breaks Undo's recording model).
- Stack trace: shows class name but not which version (same class name, different bytecode).

**RT adds:** generated_class records include CRC (stable hash of bytecode). If two versions of the same class exist with different CRCs, RT shows which CRC was loaded. callsite_target shows which loader_id handled each call.

**If Undo had REST API:** Undo doesn't handle hot-reload well regardless.

**Human usefulness:** High. Hot-reload ordering bugs are genuinely confusing.  
**Agent usefulness:** High. Bytecode CRC comparison is exactly what an agent needs to determine "same class or different version?"  
**Differentiation score:** 7/10  
**Verdict:** Real advantage. Especially the CRC comparison for version detection. Niche but genuine.

---

## Domain 10: Rule Engine Dispatch (Drools, jBPM, Easy Rules)

**Representative bug:** A Drools rule fires incorrectly. A `then` block executes unexpected logic. The rule is dynamically compiled and executed as a generated class. Which rule class handled the fact insertion?

**Existing workflow:**
- Drools audit logging: Drools has a built-in `KieRuntimeLogger` that logs rule firings. Works.
- Drools debug output: verbose logging shows all rule evaluations.
- Undo: can trace, but Drools-generated class names are complex.

**RT adds:** generated_class records for Drools-compiled rule classes + callsite_target showing which rule was evaluated. The provenance (which indy call site created the rule class) is tracked.

**Human usefulness:** Low. Drools has its own logging infrastructure.  
**Agent usefulness:** Medium. Would help an agent navigate Drools internals.  
**Differentiation score:** 3/10  
**Verdict:** Drools already has rule-level logging. RT adds little beyond what Drools provides.

---

## Domain 11: Reflection-Heavy Data Mapping (Jackson, MapStruct, ModelMapper)

**Representative bug:** Jackson deserializes a JSON field to the wrong type (null coercion, missing constructor). Which deserializer class was invoked? `StdDeserializer` subclasses are generated/registered dynamically.

**Existing workflow:**
- Jackson exception messages: `Could not deserialize value of type X` — includes class name.
- DEBUG logging for Jackson: `mapper.enable(DeserializationFeature....)` and Jackson debug output.
- Stack trace: usually throws an exception with the relevant class in the stack.

**RT adds:** callsite_target for `JsonDeserializer.deserialize()` showing which specific `StdDeserializer` subclass was invoked. But Jackson usually provides this in its exceptions.

**Human usefulness:** Low. Jackson's error messages are already good.  
**Agent usefulness:** Medium. Could help an agent navigate Jackson's 150+ deserializer classes.  
**Differentiation score:** 3/10  
**Verdict:** Jackson's own error handling covers most of this. Marginal.

---

## Domain 12: Event-Driven / Message Bus Dispatch (Spring Events, Kafka Consumer)

**Representative bug:** An `ApplicationEvent` is published and multiple `@EventListener` methods are registered. The wrong listener fires, or a listener fires twice. Which concrete class handled the event?

**Existing workflow:**
- Spring DEBUG logging: Spring logs `ApplicationEvent` dispatch.
- Stack trace: shows the listener class when an exception occurs.
- APM: Datadog traces Kafka consumers; Spring events not usually instrumented.
- Undo: works.

**RT adds:** callsite_target for `ApplicationEventMulticaster.invokeListener()` showing which listener implementation was called. Useful for the "which listener fired?" question without exception.

**Human usefulness:** Medium. Logging usually sufficient.  
**Agent usefulness:** High. An agent could determine exactly which listeners were dispatched to.  
**Differentiation score:** 5/10  
**Verdict:** Solid for event debugging but not a major moat since logging covers most cases.

---

## Domain 13: ClassLoader Isolation in Multi-Tenant Apps

**Representative bug:** A multi-tenant SaaS application loads tenant-specific code via isolated classloaders. Tenant A's request accidentally routes to Tenant B's implementation (classloader leak). Which tenant's classloader handled the call?

**Existing workflow:**
- Thread-local tenant context: if tenant ID is tracked in ThreadLocal, logging shows which tenant. Usually the fix, not the diagnosis.
- ClassLoader.getParent() inspection: expert-level debugging.
- Undo: records classloader state, but multi-tenant reproduction is complex.
- APM: records tenant context if injected via MDC, but not classloader ID.

**RT adds:** loader_id in every callsite_target record distinguishes which classloader's implementation handled each call. For multi-tenant apps this is precisely the isolation boundary.

**If Undo had REST API:** RT still has advantage because loader_id is a first-class field, not something to query from execution state.

**Human usefulness:** High. Classloader leaks in multi-tenant apps are extremely hard to diagnose.  
**Agent usefulness:** Very high. An agent can query "show me all calls where loader_id differs from the expected tenant loader."  
**Differentiation score:** 8/10  
**Verdict:** Strong. Classloader ID attribution is first-class in RT. Not available passively elsewhere.

---

## Domain 14: Version-Specific Implementation Selection (Feature Flags + Proxy)

**Representative bug:** A feature flag system wraps the current implementation in a proxy that dynamically selects between `OldPaymentService` and `NewPaymentService`. The wrong implementation was selected for this request. Which one actually ran?

**Existing workflow:**
- Feature flag system logs: LaunchDarkly/Split/Unleash log flag evaluations. Works for which flag fired, not which Java class ran.
- APM: may tag spans with feature flag context if instrumented.
- Stack trace: shows the class if an exception.
- Undo: works.

**RT adds:** callsite_target for `PaymentService.process()` shows whether `OldPaymentService` or `NewPaymentService` (or the proxy class) was the actual target. Zero instrumentation.

**Human usefulness:** Medium. Feature flag platforms usually have their own logging.  
**Agent usefulness:** High. An agent can correlate feature flag evaluation with actual implementation class from RT records.  
**Differentiation score:** 6/10  
**Verdict:** Solid. Especially for gradual rollouts where the wrong implementation selection is silent.

---

## Domain 15: Java Agent / Instrumentation Feedback

**Representative bug:** A Java instrumentation agent (Datadog, New Relic, AppDynamics, custom) instruments a class and adds its own advice. The advice interacts unexpectedly with a Spring AOP proxy, double-instrumenting a method. Which implementation chain actually executed?

**Existing workflow:**
- Agent debug logs: most agents have verbose debug modes.
- javaagent + ASM debugging: expert-level.
- Undo: works but very complex with multiple agents.
- Custom JVMTI hook: expert-level.

**RT adds:** The generated_class records and callsite_target records show exactly which instrumented class was loaded and which callsite targeted it. If Datadog's agent created a `$DD_Proxy$OriginalClass` and it was invoked unexpectedly, RT shows it.

**Human usefulness:** High. Multi-agent interactions are very hard to debug.  
**Agent usefulness:** Very high. An agent can identify unexpected instrumentation chains from RT data.  
**Differentiation score:** 7/10  
**Verdict:** Strong for tooling-on-tooling scenarios. Grows more important as instrumentation stacks become complex.

---

## Summary Table

| Domain | Human | Agent | Score | Verdict |
|--------|-------|-------|-------|---------|
| 1. Spring 1–2 proxy layers | Med | High | 4 | Explanatory demo, not moat |
| 2. Multi-level proxy chain (4+) | High | V.High | 7 | Real advantage, scales |
| 3. ServiceLoader / plugin dispatch | Med | High | 6 | Solid |
| 4. OSGi classloader attribution | High | V.High | 8 | Strong, unique loader_id |
| 5. AI agent tool dispatch | High | V.High | 9 | **Strongest new category** |
| 6. Security filter chain | Med | Med | 4 | DEBUG logging wins |
| 7. Mockito / test mocks | Low | Med | 2 | Mockito already solves this |
| 8. Dynamic language interop | Med | High | 5 | Niche |
| 9. Hot-reload / class versioning | High | High | 7 | CRC comparison is unique |
| 10. Rule engines (Drools) | Low | Med | 3 | Drools has its own logging |
| 11. Jackson / data mapping | Low | Med | 3 | Jackson errors are good |
| 12. Event-driven dispatch | Med | High | 5 | Logging usually sufficient |
| 13. Multi-tenant classloader isolation | High | V.High | 8 | loader_id is first-class |
| 14. Feature flag + proxy selection | Med | High | 6 | Solid |
| 15. Java agent instrumentation chains | High | V.High | 7 | Tooling-on-tooling |
