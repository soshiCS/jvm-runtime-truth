# doc 30 — Phase V8: Demo Script + Reality Check

**Date:** 2026-05-31

---

## Part 5 — Human Explanation Test

### 2-minute explanation for a senior Spring engineer

---

You have a `@Service` bean. You annotated `saveAuditEntry()` with `@Transactional(REQUIRES_NEW)` — it's supposed to run in its own database transaction, separate from whatever the caller is doing.

But it's not working. The audit entry is getting rolled back with the outer transaction instead of being committed independently.

Here's why: `processOrder()` calls `this.saveAuditEntry()`. Inside a Spring bean, `this` is the raw object — not the proxy Spring created. Every `@Transactional` annotation on your methods works because Spring's proxy intercepts the call and wraps it in transaction management. When you call through `this`, there is no interception. The annotation is there, but Spring never sees the call.

The fix is one word: instead of `this`, inject the bean into itself (`@Autowired OrderService self`) and call `self.saveAuditEntry()`. Now the call goes through the proxy, the interceptor fires, REQUIRES_NEW suspends the outer transaction and starts a new one.

Runtime Truth showed us this without a debugger or log statement: it recorded that `processOrder` dispatched to `OrderService.saveAuditEntry` — the raw class. Not `OrderService$$SpringCGLIB$$0.saveAuditEntry` — which is what you'd see if the proxy handled it. Two different target class names for the same method call. That difference is the bug.

---

*Total words: 230. Read aloud: ~90 seconds.*

**Does this pass the test?**  
A senior Spring engineer will immediately recognize `$$SpringCGLIB$$0` as a CGLIB proxy class and understand what its absence means. The explanation requires no JVM internals knowledge — it describes the proxy bypass in Spring terms, and shows the RT signal in terms of class names that any Spring developer has seen in stack traces.

---

## Part 6 — Reality Check

### The most important section. No optimism.

---

### Q1. Is this a stronger real-world demonstration than V5?

**Yes, for developer tooling. No, for "information only RT can produce."**

V5 demonstrated that RT reveals which specific transform a cryptographic function applied — genuinely impossible to infer from source code alone. That was a stronger uniqueness claim.

V8 demonstrates that RT identifies a real Spring bug faster than alternative approaches. The bug is real and common. But the evidence (target class name in a callsite) is not unique to RT.

V8 is more relatable to developers. V5 was more technically impressive.

---

### Q2. Would a senior Spring engineer immediately understand the value?

**Yes, conditionally.**

The `$$SpringCGLIB$$0` class name in the callsite record is immediately recognizable to a Spring developer who has read production stack traces. They will understand: "if CGLIB is not in the call chain, the proxy was bypassed."

The value proposition is clear: "you ran the code once, and without touching anything, you can see which class every call actually hit." That is a real and understandable advantage.

The condition: they need to believe that a production issue was worth investigating with a custom JVM agent. That's a significant ask.

---

### Q3. Would Datadog likely expose the same information?

**Partially. Not at the same level of detail.**

Datadog APM instruments Java at the agent level. It creates spans for `@Transactional` method entries — specifically, it wraps the proxy's intercept call. When `saveAuditEntry` is called through `this` (no proxy), the `@Transactional` span is never started. In a Datadog trace:
- `processOrder` has a span.
- `saveAuditEntry` does NOT have a span (no proxy interception = no span).
- The developer sees `saveAuditEntry` running without a transaction span.

Datadog shows the **effect** (no transaction started) but not the **cause** (raw class instead of proxy).

To go from "no span for saveAuditEntry" to "this is a proxy bypass" requires a developer who knows that Spring's `@Transactional` works via AOP proxies and understands what the missing span means. That's non-trivial.

RT shows the cause directly: target class = raw class, not proxy.

**Verdict: Datadog is 70% of the way there. RT goes one level deeper.**

---

### Q4. Would Dynatrace likely expose the same information?

**Same answer as Datadog, different mechanism.**

Dynatrace uses byte-code injection at the agent level. It instruments `@Transactional` methods. The bypass would manifest as a missing "service call" span when `saveAuditEntry` is called internally.

Dynatrace does not (in standard configuration) record per-callsite dispatch target classes. It records method entry/exit with context. The proxy class vs raw class distinction would not be visible.

Same verdict as Datadog: effect visible, cause obscured.

---

### Q5. Would Undo make this bug trivial?

**Yes, on Linux.**

Undo records every bytecode instruction. With Undo, you can:
1. Step back to the moment `saveAuditEntry` was entered.
2. Inspect the call stack — you'd see directly whether CGLIB was in the chain.
3. Done in under 2 minutes.

Undo is **definitively better** for this specific investigation on a Linux production system. You can inspect the exact call stack at any point in history without any setup. RT requires running a custom JVM in advance.

The relevant constraint: Undo is Linux-only. If your production environment is Linux (common), Undo is a stronger tool for this class of bug. If you're debugging locally on macOS (this demo), Undo is not available.

**Undo is the honest winner for this class of bug on Linux. RT wins on macOS and Windows.**

---

### Q6. Is Runtime Truth genuinely helping here?

**Yes, with caveats.**

RT provides:
1. **Zero-instrumentation causality**: Run once, the callsite records are already there. No code change, no log addition, no exception engineering required.
2. **Structural cause, not just effect**: APM tools show you the missing transaction span (effect). RT shows you the wrong target class (cause).
3. **Queryable post-mortem**: The JSONL is a file. An AI agent or a developer can query it after the fact without reproducing the bug.
4. **No framework knowledge required for the API consumer**: `search?q=saveAuditEntry` returns two records with different target classes. The inference is: "different class = different behavior." No Spring AOP knowledge needed.

What RT does NOT do:
1. Replace a debugger for step-by-step investigation.
2. Work without running the custom JVM in advance.
3. Handle bugs that are about data values, not dispatch targets.

---

### Q7. Is this evidence of a real product opportunity or merely a neat debugging trick?

**Honest answer: it is a real signal in a narrow category of bugs, not a general-purpose debugger.**

The Spring proxy bypass is a well-known, frequently-occurring bug. The category — "which class actually handled this call" — covers:
- Spring AOP proxy bypass (self-invocation)
- Spring proxy type inconsistency (@Retryable JDK proxy vs @Transactional CGLIB)
- Hibernate proxy substitution (findById returning a proxy)
- Spring Security proxy interception gaps
- Any framework that uses generated proxy classes

That category is non-trivial. It is the source of many hard-to-diagnose production issues in the Spring ecosystem specifically, because proxy behavior is largely invisible at the source code level.

For this specific category, RT provides a faster, more reliable, lower-friction path to root cause than APM tools (which show effect, not cause) or debuggers (which require reproduction).

For bugs outside this category — logic errors, data value issues, configuration bugs, performance problems — RT has little to offer.

**The product opportunity, if it exists, is in the intersection of:**
- Enterprise Java applications (Spring, Hibernate, Jakarta EE)
- Teams that can't reproduce production bugs locally
- Environments where adding logging and re-deploying is costly
- AI-assisted debugging workflows where structured runtime data is more useful than log text

This is a narrower opportunity than "general Java debugger." It is a wider opportunity than "neat trick." Whether the market is large enough to build a product around is unknown. The technical advantage is real but not absolute.

---

### Summary Verdict

| Claim                                       | Verdict       |
|---------------------------------------------|---------------|
| Bug is real and reproducible                | CONFIRMED     |
| RT shows structural cause (target class)    | CONFIRMED     |
| This information is unique to RT            | NO — stack traces and Undo also show it |
| RT is faster than traditional logging       | YES           |
| RT is faster than Undo (Linux)              | NO            |
| A Spring engineer understands it in 2 min   | YES           |
| APM tools (Datadog/Dynatrace) also detect it | PARTIALLY — effect, not cause |
| This is a stronger demo than V5             | For developers, YES. For technical uniqueness, NO. |
| Real product opportunity                    | Narrow but real — enterprise Spring + AI agents |
| General-purpose debugger replacement        | NO            |

---

### What would make this a definitively stronger demonstration

1. **A bug that APM tools miss entirely** — not just "show effect but not cause." Find a proxy bypass that produces no APM span gap (e.g., a caching proxy that runs the method anyway but with wrong state).

2. **A multi-hop proxy chain** — three levels of wrapping where the wrong target is buried 3 classes deep. Stack traces become unreadable; RT's per-callsite records remain clear.

3. **An AI agent live demo** — run Agent B in a fresh repository (no prior knowledge), query the causality APIs, and show diagnosis in 2 API calls. Compare Agent A reading 50 source files to reach the same conclusion. Make the agent advantage visible.

4. **Production environment evidence** — a real bug from a real company's codebase, not a synthetic demo. The pattern is common; finding a confirmed instance with permission to publish would be the strongest possible evidence.
