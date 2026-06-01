# Real Bug Candidates — Phase V7

**Goal**: Identify real Java bugs from real open-source projects where Runtime
Truth provides a genuine debugging advantage. All 10 candidates were sourced from
public GitHub issue trackers, not invented.

---

## Scoring Key

- **RT Lift**: How much does Runtime Truth reduce debugging effort?
  - **High**: RT immediately identifies the active class/proxy without source reading
  - **Medium**: RT confirms a hypothesis faster than alternative methods
  - **Low**: RT provides data but alternative approaches are equally fast
- **Reproducible**: Can it be reproduced with a small Spring Boot 4.x app?
- **Blocker for Agent A**: Would an AI without RT likely diagnose incorrectly?

---

## Candidate 1 — Spring @Transactional Self-Invocation Bypass

| Field | Value |
|---|---|
| **Project** | Spring Framework |
| **Issue** | [#27534](https://github.com/spring-projects/spring-framework/issues/27534) / [#8896](https://github.com/spring-projects/spring-framework/issues/8896) |
| **Summary** | `methodA()` calls `this.methodB()` on the same bean. `methodB()` is `@Transactional(propagation=REQUIRES_NEW)`. No new transaction is opened — the call bypasses the proxy. |
| **Versions** | All Spring versions, all Spring Boot versions |
| **Reflection?** | Yes — Spring AOP uses reflection or CGLIB to intercept methods |
| **Proxies?** | Yes — CGLIB proxy wraps the bean, but the internal `this` call bypasses it |
| **Generated classes?** | Yes — `OrderService$$SpringCGLIB$$0` is the proxy |
| **Dynamic dispatch?** | Yes — `@Transactional` depends on the invokevirtual going through the proxy |
| **Difficulty** | Low to reproduce; hard to diagnose without knowing Spring AOP internals |
| **RT Lift** | **High** — `callsite_target` record shows the self-call goes directly to `OrderService.methodB`, NOT to `OrderService$$SpringCGLIB$$0.methodB`. Bypass is confirmed structurally, not inferentially. |

**Why RT helps**: Without RT, a developer (or agent) reads `@Transactional` on both methods, sees the code looks correct, and spends time reading Spring AOP documentation to understand why transactions aren't working. With RT, `causality_chain` from `methodA`'s callsite for `methodB` shows a direct `invokevirtual` to the concrete class, not the proxy — confirming the bypass in one observation.

**Selected for reproduction**: ✓

---

## Candidate 2 — Spring @Retryable Gets JDK Proxy Instead of CGLIB

| Field | Value |
|---|---|
| **Project** | Spring Framework / Spring Retry |
| **Issue** | [#35286](https://github.com/spring-projects/spring-framework/issues/35286) |
| **Summary** | When `spring.aop.proxy-target-class=true` (Spring Boot default), `@Transactional` and `@Cacheable` beans get CGLIB proxies, but `@Retryable` beans get JDK proxies. Code that casts the bean to its concrete class fails with `ClassCastException`. |
| **Versions** | Spring Framework 6.x / 7.x; Spring Retry 2.x |
| **Reflection?** | Yes — both JDK proxy and CGLIB use reflection |
| **Proxies?** | Yes — two different proxy types for the same bean |
| **Generated classes?** | Yes — CGLIB generates `$$SpringCGLIB$$0` class; JDK generates `jdk.proxy1.$Proxy<N>` |
| **Dynamic dispatch?** | Yes — proxy dispatches differently depending on type |
| **Difficulty** | Medium — requires spring-retry dependency |
| **RT Lift** | **High** — `generated_class` records immediately reveal proxy type. `jdk/proxy1/$Proxy0` confirms JDK proxy; `com.example.Service$$SpringCGLIB$$0` confirms CGLIB. No config reading needed. |

**Why RT helps**: Agent A would read `spring.aop.proxy-target-class=true`, expect CGLIB, and not understand why a `ClassCastException` occurs when casting to the concrete class. RT's `causality/proxies` endpoint shows the actual proxy class instantiated — revealing the inconsistency between `@Retryable` (JDK proxy) and `@Transactional` (CGLIB).

**Selected for reproduction**: ✓

---

## Candidate 3 — Hibernate `findById()` Returns Proxy After `JOIN FETCH` Query

| Field | Value |
|---|---|
| **Project** | Spring Data JPA / Hibernate ORM |
| **Issue** | [spring-data-jpa #3362](https://github.com/spring-projects/spring-data-jpa/issues/3362) |
| **Summary** | Calling `userRepository.findById(id)` after a `JOIN FETCH` query returns a `User$HibernateProxy$...` instead of the actual `User` entity. Subsequent `instanceof User` checks pass but behavior is wrong; type-specific methods dispatch to the proxy instead of the real class. |
| **Versions** | Hibernate 6.4.1+, Spring Data JPA 3.2.x |
| **Reflection?** | Partial — Hibernate uses reflection for proxy generation |
| **Proxies?** | Yes — `User$HibernateProxy$...` (Hibernate bytecode-enhanced proxy) |
| **Generated classes?** | Yes — Hibernate generates `User$HibernateProxy$<hash>` at runtime |
| **Dynamic dispatch?** | Yes — methods dispatch to proxy wrapper instead of entity |
| **Difficulty** | Medium — requires JPA, H2, and specific query ordering |
| **RT Lift** | **High** — `callsite_target` (polymorphic) record shows the concrete target is `User$HibernateProxy$...`, not `User`. Agent B identifies the wrong target class in one causality call. Agent A reads entity code and queries without seeing the proxy substitution. |

**Why RT helps**: The developer sees correct `User` return type in the repository interface signature but gets wrong behavior. The proxy is invisible in code — Hibernate substitutes it transparently. RT's `causality/polymorphic` reveals which concrete class `findById()` dispatched to.

**Selected for reproduction**: ✓

---

## Candidate 4 — Spring #33113: Mockito Mock Wrapped by CGLIB Proxy (AspectJ Present)

| Field | Value |
|---|---|
| **Project** | Spring Framework |
| **Issue** | [#33113](https://github.com/spring-projects/spring-framework/issues/33113) |
| **Summary** | When a `@MockBean` targets a class that also has an `@Aspect` applied, Spring wraps the Mockito mock in a CGLIB proxy (`MyService$MockitoMock$<hash>$$SpringCGLIB$$0`). Method calls hit the CGLIB proxy first, then the mock. Mockito's `verify()` counts are off by 1 or more. |
| **Versions** | Spring Framework 6.1.9–6.1.10 (regression) |
| **Reflection?** | Yes — CGLIB proxy intercepts via reflection |
| **Proxies?** | Yes — nested: CGLIB(MockitoMock) |
| **Generated classes?** | Yes — both Mockito's ByteBuddy mock class AND Spring's CGLIB proxy |
| **Dynamic dispatch?** | Yes — method reaches mock via proxy intermediary |
| **Difficulty** | Medium — requires Spring Boot test infra + AspectJ aspect |
| **RT Lift** | **High** — `causality_chain` shows: test → `$$SpringCGLIB$$0` → `MockitoMock` → actual method. Full proxy nesting visible instantly. Without RT, developer sees wrong verify counts with no indication of why. |

---

## Candidate 5 — Spring Boot #30575: @SpyBean + @Transactional + Self-Injection Regression

| Field | Value |
|---|---|
| **Project** | Spring Boot |
| **Issue** | [#30575](https://github.com/spring-projects/spring-boot/issues/30575) |
| **Summary** | A `@SpyBean` on a `@Transactional` bean that self-injects via `@Autowired` broke in Spring Boot 2.6.4. The spy's method interception was bypassed because the self-injected reference pointed to the wrong proxy layer. |
| **Versions** | Spring Boot 2.6.3 → 2.6.4 regression |
| **Proxies?** | Yes — SpyBean CGLIB + Transaction CGLIB (two proxy layers) |
| **Generated classes?** | Yes — two CGLIB proxy classes |
| **Dynamic dispatch?** | Yes |
| **RT Lift** | **High** — RT shows which proxy class the self-injected field points to, and which is the outer vs. inner proxy. |

---

## Candidate 6 — Quarkus #11447: Arc Proxy Recursive Interception (StackOverflow)

| Field | Value |
|---|---|
| **Project** | Quarkus |
| **Issue** | [#11447](https://github.com/quarkusio/quarkus/issues/11447) |
| **Summary** | An `@AroundInvoke` interceptor calls a method on the bean via `invocationContext.getTarget()`. Because `getTarget()` returns the Arc proxy (not the raw bean), the interceptor fires again, causing infinite recursion and `StackOverflowError`. |
| **Versions** | Quarkus 1.7.0.Final |
| **Proxies?** | Yes — Arc CDI proxy |
| **Generated classes?** | Yes — Arc generates proxy subclasses |
| **Dynamic dispatch?** | Yes — the method call loops back through the proxy |
| **RT Lift** | **High** — RT's `callsite_target` records would show the circular chain: `AroundInvoke.invoke` → Arc proxy → `AroundInvoke.invoke` again. Without RT, you see an `StackOverflowError` stack with repeated frames but must manually trace the proxy generation. |

---

## Candidate 7 — ByteBuddy #1162: `@Advice.Origin` Returns Wrong Method (Bridge Methods)

| Field | Value |
|---|---|
| **Project** | ByteBuddy |
| **Issue** | [#1162](https://github.com/raphw/byte-buddy/issues/1162) |
| **Summary** | When Java generates a bridge method for generic type erasure (e.g., covariant return types), ByteBuddy's `@Advice.Origin Method` annotation returns the bridge method's metadata instead of the actual method. The advice logic uses the wrong method signature for logging, routing, or metric recording. |
| **Versions** | ByteBuddy 1.10.x–1.12.x |
| **Generated classes?** | Yes — ByteBuddy instruments and generates class with wrong metadata |
| **Proxies?** | No |
| **Dynamic dispatch?** | Yes — bridge method dispatches to real method |
| **RT Lift** | **Medium** — `artifact_javap` would show the bridge method constant in the generated bytecode. An agent could read the baked-in method reference directly. Without RT, the agent reads source code but can't see what method reference was baked into the instrumented class. |

---

## Candidate 8 — ByteBuddy #647: AmbiguityResolver Picks Wrong Method in Subclass

| Field | Value |
|---|---|
| **Project** | ByteBuddy |
| **Issue** | [#647](https://github.com/raphw/byte-buddy/issues/647) |
| **Summary** | When subclassing class B (which extends A) and both have `getUuid()`, ByteBuddy's `DeclaringTypeResolver` picks the wrong method. The generated class delegates `getUuid()` to `getSubUuid()` instead of the intended method. |
| **Versions** | ByteBuddy 1.x |
| **Generated classes?** | Yes — generated subclass has wrong delegation |
| **Dynamic dispatch?** | Yes |
| **RT Lift** | **High** — `artifact_javap` immediately shows which method the generated class delegates to. Without RT, the developer must generate the class and inspect its bytecode manually. |

---

## Candidate 9 — Spring Framework #31238: CGLIB Proxy Classes Not Cached (Native Image)

| Field | Value |
|---|---|
| **Project** | Spring Framework |
| **Issue** | [#31238](https://github.com/spring-projects/spring-framework/issues/31238) |
| **Summary** | After Spring 6.0.11, CGLIB proxy classes are no longer cached, causing multiple `$$SpringCGLIB$$` class instances for the same target class. Native image builds fail; test contexts with `@DirtiesContext` regenerate proxy classes. |
| **Versions** | Spring Framework 6.0.11+ / Spring Boot 3.1.3+ |
| **Generated classes?** | Yes — multiple `$$SpringCGLIB$$0` instances created |
| **Dynamic dispatch?** | Yes |
| **RT Lift** | **Medium** — `generated_class` records would show duplicate CGLIB proxy entries. But the symptom (build failure) is already explicit. RT helps confirm the root cause. |

---

## Candidate 10 — Spring Framework #26535: `@EnableAspectJAutoProxy(proxyTargetClass=true)` Ignored for `@Async`/`@Scheduled` with Interface

| Field | Value |
|---|---|
| **Project** | Spring Framework |
| **Issue** | [#26535](https://github.com/spring-projects/spring-framework/issues/26535) |
| **Summary** | When `proxyTargetClass=true` is set globally and a `@Scheduled` or `@Async` bean implements an interface, Spring creates a JDK proxy anyway. Calling a non-interface method on the bean fails with `IllegalStateException: Need to invoke method 'schedule' declared on target class 'TestBean', but not found in any interface(s) of the exposed proxy type`. |
| **Versions** | Spring Framework 5.3.x |
| **Proxies?** | Yes — JDK proxy created despite `proxyTargetClass=true` |
| **Generated classes?** | Yes — `jdk.proxy1.$ProxyN` class generated |
| **Dynamic dispatch?** | Yes |
| **RT Lift** | **High** — RT shows proxy class type immediately. `generated_class` reveals `jdk/proxy1/$Proxy0` even though developer expected CGLIB. Confirms proxy type mismatch without reading Spring source. |

---

## Summary Table

| # | Project | Issue | RT Lift | Reproducible | Selected |
|---|---------|-------|---------|--------------|----------|
| 1 | Spring | #27534 — @Transactional self-invocation | High | ✓ Easy | **✓** |
| 2 | Spring Retry | #35286 — @Retryable JDK proxy mismatch | High | ✓ Medium | **✓** |
| 3 | Spring Data JPA | #3362 — Hibernate proxy in findById | High | ✓ Medium | **✓** |
| 4 | Spring | #33113 — Mockito+AspectJ double proxy | High | ✗ Complex | — |
| 5 | Spring Boot | #30575 — SpyBean+Transactional regression | High | ✗ Boot 2.x only | — |
| 6 | Quarkus | #11447 — Arc recursive interception | High | ✗ Quarkus required | — |
| 7 | ByteBuddy | #1162 — wrong method in bridge advice | Medium | ✓ But niche | — |
| 8 | ByteBuddy | #647 — AmbiguityResolver wrong method | High | ✓ But niche | — |
| 9 | Spring | #31238 — CGLIB cache regression | Medium | ✗ Native only | — |
| 10 | Spring | #26535 — proxyTargetClass ignored | High | ✓ Medium | — |

---

## Top 3 Selected for Local Reproduction

**Candidate 1** — @Transactional self-invocation bypass
- Easy to reproduce; high RT value; affects every Spring developer
- RT shows direct dispatch proof (no proxy in path)

**Candidate 2** — @Retryable JDK proxy vs @Transactional CGLIB mismatch
- Medium complexity; high RT value; visually striking comparison
- RT shows proxy type mismatch directly in generated_class records

**Candidate 3** — Hibernate findById returns proxy after join fetch
- Medium complexity; high RT value; demonstrates polymorphic dispatch visibility
- RT shows concrete target class in polymorphic callsite records

These three cover the three main failure modes:
1. **Bypass** — proxy exists but call skips it
2. **Wrong type** — wrong proxy class generated
3. **Wrong target** — right interface, wrong concrete class dispatched to
