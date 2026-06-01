# doc 33 — Phase V9: Strongest Signal Report

**Date:** 2026-05-31

---

## Ranked Top 10 Opportunities

### #1 — AI Agent Tool Dispatch (Score: 9/10)

**Why it matters:**  
In 2025-2026, every enterprise Java team is building AI-assisted features using Spring AI or LangChain4j. These frameworks dispatch LLM tool calls to Java implementations via reflection or proxy. The concrete question — "which Java class handled the tool named 'send_email'?" — has no current passive answer. LangSmith tracks API calls; it does not track JVM dispatch. Datadog does not instrument FunctionCallback resolution. JFR doesn't capture it.

**Who experiences it:**  
Java teams building AI agents, chatbots, or copilot features with Spring AI or LangChain4j. Growing from a small base in 2024 to mainstream adoption in 2025-2026. This is not a legacy problem — it is an emerging problem with no established tooling.

**Competing workflow:**  
Add `log.info("tool dispatched: {}", handler.getClass())` inside the framework. Requires modifying framework code or wrapping every tool with a logging proxy. Neither is zero-effort.

**Why RT is better:**  
RT records the callsite_target for every tool invocation passively. A developer (or AI agent) queries `causality/search?q=FunctionCallback` and immediately sees which implementations were dispatched to and in what order. No code change. No framework modification.

**The compelling narrative:**  
"Your AI agent dispatched 50 tool calls. Which ones hit the real implementation and which ones hit a mock? RT shows you in one query." — No other tool can answer this question without instrumentation.

**If Undo had REST API:** Shrinks advantage on Linux, but dev environments are typically macOS. Also: Undo requires recording from the start; RT is zero-setup.

---

### #2 — OSGi / Multi-Tenant Classloader Attribution (Score: 8/10)

**Why it matters:**  
`loader_id` is a first-class field in every RT callsite_target record. In OSGi containers (WebSphere, JBoss EAP, Karaf) and multi-tenant applications, the same class name can appear in multiple classloaders with different bytecode. APM tools, stack traces, and logs all show the class NAME — not the loader. RT shows both.

**Who experiences it:**  
Large enterprise Java teams on WebSphere or JBoss EAP, Eclipse plugin developers, teams with custom classloading for tenant isolation (common in SaaS platforms serving enterprise customers).

**Competing workflow:**  
Custom JVMTI agent, javaagent that logs classloader context, or OSGi expert walking the bundle dependency graph manually. All require significant setup.

**Why RT is better:**  
Loader attribution is automatic. Every record has it. No configuration required.

**If Undo had REST API:** Undo records classloader context but you'd need to know which variable to query. RT surfaces it for every callsite without asking.

---

### #3 — Multi-Level Spring Proxy Chain Tracing (Score: 7/10)

**Why it matters:**  
When 4+ interceptors are layered on the same Spring bean (@Transactional + @Cacheable + @Retryable + @Secured + custom), stack traces become unreadable — every frame says `CglibAopProxy$DynamicAdvisedInterceptor.intercept`. APM tools consolidate all interceptors into one span. RT preserves the distinct dispatch sequence.

**Who experiences it:**  
Any team with a complex Spring application using multiple AOP annotations. Common in enterprise Spring Boot 3.x/4.x applications.

**Competing workflow:**  
Enable individual advisor logging (requires knowing the logger names for each advisor). Undo trace (Linux only). Add breakpoints inside each interceptor (requires reproducing the bug interactively).

**Why RT is better:**  
The causality chain from `causality/proxies` lists every proxy dispatch site. The sequence of callsite_target records in the JSONL captures the interceptor execution order.

**Scales with complexity:** A 2-layer proxy is debuggable without RT. A 6-layer proxy is not.

---

### #4 — Hot-Reload / Class Version Attribution (Score: 7/10)

**Why it matters:**  
During development with JRebel or DCEVM, the same class name may refer to two different bytecode versions simultaneously (old CGLIB proxy cached, new class loaded). RT's CRC-stable generated_class records can distinguish "same bytecode" from "different bytecode" without comparing source.

**Who experiences it:**  
Java teams using hot-reload tools for fast development cycles (JRebel, Spring Boot DevTools with byte-buddy).

**Competing workflow:**  
Manual inspection via `ClassLoader.findLoadedClass()` plus `javap`. Complex, requires knowing which classloader to inspect.

**Why RT is better:**  
CRC comparison is automatic. Two entries with the same class name but different CRCs = different bytecode versions. This is information no other passive tool surfaces.

**If Undo had REST API:** Undo's recording model is incompatible with hot-reload (class replacement breaks the recording). RT is unaffected.

---

### #5 — Java Agent / Instrumentation Chains (Score: 7/10)

**Why it matters:**  
When multiple agents instrument the same class (Datadog agent + Spring AOP + custom javaagent), the result can be unexpected double-instrumentation. Diagnosing which generated class was actually invoked requires understanding the full transformation chain. RT records the complete generated_class provenance including which agent created each class.

**Who experiences it:**  
Teams running multiple monitoring agents alongside Spring AOP — common in enterprise observability stacks.

**Competing workflow:**  
Turn off agents one by one. Use JVM flag `-verbose:class`. Expert-level javaagent debugging.

**Why RT is better:**  
generated_class records show which agent created each transformed class. callsite_target records show which transformation was in the dispatch chain.

---

### #6 — ServiceLoader / SPI Plugin Dispatch (Score: 6/10)

**Why it matters:**  
`ServiceLoader.load(Interface.class)` returns whatever implementation is on the classpath. In complex deployments (web application servers, IDE plugins), the "wrong" implementation can appear silently. RT shows which concrete class was invoked at the ServiceLoader-dispatched callsite.

**Who experiences it:**  
Teams debugging "works on my machine" issues where classpath ordering determines which implementation is loaded. JDK providers (JDBC drivers, JCE providers, JAX-RS), enterprise middleware.

**If Undo had REST API:** Advantage mostly disappears on Linux.

---

### #7 — Feature Flag + Runtime Proxy Selection (Score: 6/10)

**Why it matters:**  
When a feature flag wraps an implementation in a proxy that dynamically selects between old/new behavior, the selection happens at runtime based on request context. If the wrong implementation is selected silently (no exception), RT records which implementation was actually invoked.

**Who experiences it:**  
Teams doing percentage-based rollouts, A/B testing at the implementation level.

---

### #8 — Dynamic Language Interop CRC Tracking (Score: 5/10)

**Why it matters:**  
Groovy scripts, JEXL expressions, and scripting engines compile to JVM classes at runtime. RT tracks these via generated_class records with stable CRCs. The same script evaluated twice with different source produces different CRCs — RT can detect script version mismatches silently.

---

### #9 — Event-Driven Dispatch Attribution (Score: 5/10)

**Why it matters:**  
Spring ApplicationEvents and Kafka consumer dispatch go to multiple listeners. RT records which listener implementation was invoked. Useful when multiple listeners have the same method signature and the wrong one fires without exception.

---

### #10 — Single-Layer Spring Proxy (Score: 4/10)

**Why it matters:**  
The V8 demo case. Real bug, clear RT signal, but stack traces and Undo also solve it. RT is faster and requires no code changes, but it is not uniquely necessary.

---

## What Is the Single Strongest Signal Discovered So Far?

### V5 (cryptographic transform selection) remains the technically strongest uniqueness claim.

V5 demonstrated that RT could identify which specific numbered transform (out of 100) was applied to a payload. This information:
- Cannot be inferred from source code (transforms are selected by INSTANCE_TOKEN at runtime)
- Cannot be inferred from the output (the encryption is one-way)
- Requires knowing which generated class was instantiated and which indy call site resolved it
- Is genuinely impossible to derive without RT or Undo

No APM tool, no debugger, no stack trace can tell you "transform #37 ran" — unless you already know where transform selection happens and instrument it. RT tells you without instrumentation.

However, V5 is a purpose-built synthetic scenario. No enterprise Java team encounters "cryptographic transform selection via invokedynamic" as a production bug.

### V9 identifies AI agent tool dispatch as the strongest real-world production opportunity.

It scores 9/10 because:
1. **No competing tool solves it passively** — LangSmith, Datadog, Dynatrace, JFR, stack traces all fail
2. **Growing market** — every enterprise Java team building AI features in 2025-2026
3. **Maps directly to RT's core capability** — "which class handled the call?"
4. **Agent-on-agent debugging** — the narrative "use RT (an AI-friendly structured log) to debug what your AI agent's tool dispatch actually did" is compelling and differentiated
5. **Undo doesn't help** — development is on macOS; tool dispatch happens at high frequency (50 calls per conversation); time-travel debugging is impractical at that scale

---

## Answers to V9 Questions

### 1. What problem are we actually solving?

**The dispatch attribution gap in dynamically-dispatched Java code.**

When Java code dispatches a call through a proxy, plugin system, ServiceLoader, dynamically loaded class, or AI framework, the concrete implementation that handled the call is invisible to all standard passive monitoring tools. RT makes it visible, queryable, and AI-agent-accessible without any code changes.

The problem is most acute when:
- Multiple implementations of the same interface exist at runtime (plugins, classloaders, feature flags)
- The dispatch mechanism is runtime-dynamic (proxy, reflection, invokedynamic)
- The bug is silent (no exception, just wrong behavior)
- The environment cannot be easily reproduced (production, complex classloader setup)

---

### 2. Who cares about it?

**Ranked by urgency in 2025-2026:**

1. **Java AI agent developers** (Spring AI, LangChain4j) — NEW, growing, no tooling
2. **Enterprise Spring Boot teams** — mature, large, proxy/AOP bugs are common
3. **OSGi / enterprise container teams** — WebSphere, JBoss EAP
4. **Multi-tenant SaaS platform teams** — custom classloader isolation
5. **AI debugging agent vendors** — Cursor, GitHub Copilot, Claude Code integrations

---

### 3. Do competitors already solve it?

**For specific sub-problems: partially.**

| Sub-problem | Best current tool | Gap |
|------------|-----------------|-----|
| Single-layer Spring proxy bypass | Stack traces, Undo | RT is faster/no-code |
| Multi-layer proxy chain (4+) | None (stack traces unusable) | RT uniquely useful |
| OSGi classloader attribution | OSGi console (expert only) | RT is automatic |
| AI agent tool dispatch | Nothing passive | RT is the only option |
| Hot-reload versioning | Nothing passive (CRC) | RT unique |
| ServiceLoader dispatch | Add logging + rerun | RT is zero-setup |

**No competing tool provides passive, per-callsite, queryable dispatch attribution.**  
The gap exists. The question is whether the gap is large enough to build a product.

---

### 4. Is there a real product opportunity?

**Narrow but real, with one potentially large entry point.**

The **narrow** opportunity: enterprise Spring/Hibernate debugging for teams that cannot reproduce production bugs locally and cannot afford Undo's Linux-only, recording-required model.

The **potentially large** entry point: **AI agent tool dispatch attribution** for the AI-native development teams building AI features in Java. This is a new problem with no existing tooling, a growing addressable market, and a narrative that sells itself.

If the AI agent development workflow becomes RT's primary use case:
- "Run your Spring AI app under RT, and the outer RT layer records every tool dispatch your inner AI agent made"
- Query via REST: "Which tool implementations did the agent call in this conversation?"
- AI debugging AI — uses the same causality graph model, produces structured data for further agent analysis

This is a 3-5 year platform play, not an immediate product. But the technical foundation is already built.

---

### 5. If the answer is "no opportunity": state that clearly.

This is NOT "no opportunity."

The dispatch attribution gap is real, confirmed by V9 research (no passive per-callsite tracking exists in any mainstream tool). The AI agent tool dispatch category has no current solution and a growing market.

However, the opportunity is **not** "general Java debugger." It is specifically: "structured, queryable, AI-agent-accessible runtime dispatch attribution for dynamically-dispatched Java code in environments where instrumentation and Undo are unavailable or impractical."

That is a narrower statement than "debugging tool," but it is a real and defensible position.

---

## Recommendation for Phase V10

**Build an AI agent tool dispatch demonstration.**

Specifically:
1. Build a Spring AI application with 4+ registered tools
2. Run a conversation where the LLM selects tools dynamically
3. Capture RT JSONL showing which FunctionCallback implementations were dispatched to
4. Show an outer AI agent (Claude) querying the causality APIs to determine "what tools did my agent call, and which Java classes handled them?"
5. Compare against LangSmith and Datadog — neither can answer the Java class question

This is V10's strongest candidate for a new demo that:
- Goes beyond the Spring proxy bypass (which Undo can also solve)
- Demonstrates RT in a growing and underserved market
- Has a compelling agent-on-agent debugging narrative
- Produces evidence that V5 was technically the strongest signal, but AI agent dispatch is the strongest product opportunity

---

## Final Status of Each Phase

| Phase | What it proved | Strength |
|-------|---------------|----------|
| V5 | RT reveals cryptographic transform selection — impossible without RT | Technical uniqueness: HIGH. Real-world applicability: LOW |
| V6 | Undo comparison: RT advantage is REST API + macOS + AI-agent query model | Confirmed RT positioning vs Undo |
| V7 | Spring proxy bypass signal real; Bug2/3 not reproduced | Validated methodology |
| V8 | Spring proxy bypass: clean demo, honest reality check | Strong explanatory demo. Not a moat alone. |
| V9 | AI agent tool dispatch = strongest new opportunity | Signal identified. V10 recommendation produced. |
