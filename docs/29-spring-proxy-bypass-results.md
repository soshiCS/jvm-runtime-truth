# doc 29 — Phase V8: Spring Proxy Bypass Results

**Date:** 2026-05-31  
**Run ID:** c921c24c (Runtime Truth UI port 5002)  
**JSONL:** `/tmp/demo_proxy_bypass/runtime_targets.jsonl`

---

## Live Evidence

### 1. HTTP Response — the observable bug

```
GET http://localhost:19090/compare
```

```json
{
  "buggy_path": {
    "call_expression":   "this.saveAuditEntry()",
    "outer_tx":          "com.example.proxydemo.OrderService.processOrder",
    "saveAuditEntry_tx": "com.example.proxydemo.OrderService.processOrder",
    "proxy_bypassed":    true,
    "conclusion": "saveAuditEntry ran in processOrder's transaction — REQUIRES_NEW ignored"
  },
  "correct_path": {
    "call_expression":   "self.saveAuditEntry()",
    "outer_tx":          "com.example.proxydemo.OrderService.processOrderCorrect",
    "saveAuditEntry_tx": "com.example.proxydemo.OrderService.saveAuditEntry",
    "proxy_bypassed":    false,
    "conclusion": "saveAuditEntry has its own transaction — REQUIRES_NEW honored"
  },
  "verdict": "BUG CONFIRMED: self-invocation bypasses Spring proxy"
}
```

The `saveAuditEntry_tx` field is the transaction name active *inside* `saveAuditEntry`.  
In the buggy path it equals `processOrder`'s name — REQUIRES_NEW was silently ignored.

---

### 2. Runtime Truth — the structural cause

JSONL record count: **73,288** total (51,245 callsite_target)

The two key records from `runtime_targets.jsonl`:

```json
{"record":"callsite_target",
 "source_class":"com/example/proxydemo/OrderService",
 "source_method":"processOrder",
 "source_bci":21,
 "source_opcode":"invokevirtual",
 "target_class":"com/example/proxydemo/OrderService",
 "target_method":"saveAuditEntry",
 "target_descriptor":"(Ljava/lang/String;)Ljava/lang/String;"}

{"record":"callsite_target",
 "source_class":"com/example/proxydemo/OrderService",
 "source_method":"processOrderCorrect",
 "source_bci":24,
 "source_opcode":"invokevirtual",
 "target_class":"com/example/proxydemo/OrderService$$SpringCGLIB$$0",
 "target_method":"saveAuditEntry",
 "target_descriptor":"(Ljava/lang/String;)Ljava/lang/String;"}
```

Side-by-side:

| Field          | Buggy (BCI 21)              | Correct (BCI 24)                          |
|----------------|-----------------------------|-------------------------------------------|
| source_method  | processOrder                | processOrderCorrect                       |
| target_class   | `OrderService`              | `OrderService$$SpringCGLIB$$0`            |
| target_method  | saveAuditEntry              | saveAuditEntry                            |
| proxy in chain | NO                          | YES                                       |
| REQUIRES_NEW   | IGNORED                     | HONORED                                   |

The difference is entirely in `target_class`. The CGLIB suffix `$$SpringCGLIB$$0` is the Spring proxy class. Its absence means the interceptor was never reached.

---

### 3. Causality API Responses

**`GET /api/runs/c921c24c/causality/search?q=saveAuditEntry`**

```json
{
  "count": 5,
  "explanation": "Found 5 callsite(s) matching 'saveauditentry' across 3 class(es):
    OrderController, OrderService, OrderService$$SpringCGLIB$$0.",
  "matches": [
    {
      "source_class":  "com/example/proxydemo/OrderService",
      "source_method": "processOrder",
      "source_bci":    21,
      "target_class":  "com/example/proxydemo/OrderService",
      "target_method": "saveAuditEntry",
      "category":      "invokevirtual"
    },
    {
      "source_class":  "com/example/proxydemo/OrderService",
      "source_method": "processOrderCorrect",
      "source_bci":    24,
      "target_class":  "com/example/proxydemo/OrderService$$SpringCGLIB$$0",
      "target_method": "saveAuditEntry",
      "category":      "invokevirtual"
    }
    ...
  ]
}
```

**`GET /api/runs/c921c24c/causality/explain?class=...OrderService&method=processOrder&bci=21`**

```json
{
  "callsite":           "...OrderService.processOrder@bci=21",
  "dispatch_mechanism": "invokevirtual",
  "polymorphic":        false,
  "targets": [
    { "class": "com/example/proxydemo/OrderService",
      "method": "saveAuditEntry" }
  ],
  "explanation": "The invokevirtual callsite at ...processOrder@bci=21 dispatches to
    exactly one target: OrderService.saveAuditEntry."
}
```

**`GET /api/runs/c921c24c/causality/explain?class=...OrderService&method=processOrderCorrect&bci=24`**

```json
{
  "callsite":           "...OrderService.processOrderCorrect@bci=24",
  "dispatch_mechanism": "invokevirtual",
  "polymorphic":        false,
  "targets": [
    { "class": "com/example/proxydemo/OrderService$$SpringCGLIB$$0",
      "method": "saveAuditEntry" }
  ],
  "explanation": "The invokevirtual callsite at ...processOrderCorrect@bci=24 dispatches
    to exactly one target: OrderService$$SpringCGLIB$$0.saveAuditEntry."
}
```

**`GET /api/runs/c921c24c/causality/proxies`** (proxydemo entries only)

```
OrderController.compare@12     -> OrderService$$SpringCGLIB$$0   [cglib]
OrderController.compare@22     -> OrderService$$SpringCGLIB$$0   [cglib]
OrderService.processOrderCorrect@24 -> OrderService$$SpringCGLIB$$0   [cglib]
```

Note: `OrderService.processOrder@21` does **not** appear in `/causality/proxies` — because that call goes to the raw class, not a proxy. The absence of an entry here is itself the signal.

---

## Part 3 — Agent Evaluation

### Agent A: Normal repository analysis (no RT)

**What Agent A does:**
1. Reads `OrderService.java` — finds `this.saveAuditEntry()` call.
2. Knows the Spring self-invocation rule (if trained on Spring knowledge).
3. Produces hypothesis: "The call to saveAuditEntry at line 34 uses `this`, which bypasses the Spring proxy. REQUIRES_NEW will not fire."

**Turns to diagnosis:** 1–3 (if it knows Spring AOP internals)  
**Uncertainty:** High — the agent is reasoning from source code + static knowledge. It cannot confirm the proxy was actually bypassed at runtime. It may miss the bug if it doesn't know the self-invocation rule.  
**Files read:** `OrderService.java`, possibly `pom.xml` for Spring version.

**Can Agent A identify the bug?** Yes — if it knows Spring. No — if it only reads the code without AOP knowledge. The annotation is present and syntactically correct; nothing in the source says "this is wrong."

---

### Agent B: RT APIs available (`causality/search`, `causality/explain`, `causality/proxies`)

**What Agent B does:**
1. Queries `causality/search?q=saveAuditEntry`.
2. Sees two entries: one targeting `OrderService` (raw), one targeting `OrderService$$SpringCGLIB$$0`.
3. Queries `causality/explain` for each.
4. Reads: processOrder → raw class. processOrderCorrect → CGLIB proxy.
5. Conclusion: "processOrder called saveAuditEntry directly, bypassing the proxy. The transaction interceptor was never reached."

**Turns to diagnosis:** 2 (search + explain)  
**Uncertainty:** Low — the evidence is structural, not hypothetical. The proxy bypass is a measured fact, not an inference from source code.  
**Files read:** none required (all evidence is in the API response)

**Can Agent B identify the bug?** Yes, immediately. No Spring knowledge required. The CGLIB suffix difference is self-explanatory from the API response.

---

### Measurement comparison

| Metric                 | Agent A (no RT)        | Agent B (with RT)            |
|------------------------|------------------------|------------------------------|
| API calls              | 0 (file reads only)    | 2 (search + explain)         |
| Files read             | 2–5                    | 0                            |
| Turns to diagnosis     | 2–5                    | 2                            |
| Requires framework knowledge | YES             | NO                           |
| Confidence in diagnosis | Medium (inference)    | High (measured)              |
| Can confirm at runtime | No                     | Yes (measured, not inferred) |

The advantage is not turn count — both converge in 2–3 turns for this simple case. The advantage is **confidence and generalizability**: Agent B's answer is grounded in runtime measurement, not static source analysis. In complex real-world codebases where proxies are nested 3+ levels deep, Agent A may miss the bug entirely. Agent B reads the same evidence regardless of complexity.

---

## Part 4 — Traditional Debugging Comparison

### Method 1: Logs only

**Steps:**
1. Know the bug exists (symptom: transaction not committed, audit record missing, etc.)
2. Add `log.info("tx={}", TransactionSynchronizationManager.getCurrentTransactionName())` inside `saveAuditEntry`.
3. Re-deploy.
4. Observe: tx name is `processOrder`, not `saveAuditEntry`.
5. Search for all calls to `saveAuditEntry` in the codebase.
6. Find the `this.saveAuditEntry()` call.
7. Apply the fix.

**Time:** 15–45 min (requires code change, re-deploy, and knowing what to log)  
**Information available:** Transaction name — confirms the effect (wrong tx) but not the structural cause.  
**Clarity of root cause:** Medium. You see the wrong transaction, not why.

---

### Method 2: Stack trace only

**Steps:**
1. Throw an exception (or add a Thread.dumpStack()) inside `saveAuditEntry`.
2. Read the stack trace.
3. Buggy path: `... OrderService.saveAuditEntry ← OrderService.processOrder`. No CGLIB in the chain.
4. Correct path: `... OrderService.saveAuditEntry ← CglibAopProxy$DynamicAdvisedInterceptor.intercept ← OrderService$$SpringCGLIB$$0.saveAuditEntry`.
5. The absence of CGLIB in the buggy stack trace reveals the bypass.

**Time:** 10–30 min (requires reproducing the bug with a controllable exception or Thread.dumpStack())  
**Information available:** Full call chain — directly shows CGLIB present/absent.  
**Clarity of root cause:** High — IF you know how to read CGLIB-annotated stack traces.

Stack traces are actually effective here. The key weakness: you must reproduce the bug, know to throw an exception inside the affected method, and understand what `$$SpringCGLIB$$0` means in a call stack.

---

### Method 3: Runtime Truth

**Steps:**
1. Run the application once under RT-enabled JVM.
2. Query: `causality/search?q=saveAuditEntry`.
3. Read two entries: `processOrder → OrderService.saveAuditEntry` (raw), `processOrderCorrect → OrderService$$SpringCGLIB$$0.saveAuditEntry` (proxy).
4. Done.

**Time:** < 5 min (no code changes, no re-deploy, no exception engineering)  
**Information available:** Per-callsite dispatch target — structural cause directly.  
**Clarity of root cause:** High — CGLIB suffix absent/present is immediately readable.

---

### Comparison table

| Approach     | Code change? | Re-deploy? | CGLIB knowledge needed? | Time    | Certainty |
|--------------|-------------|-----------|------------------------|---------|-----------|
| Logs only    | YES         | YES       | No (tx name sufficient)| 15–45m  | Effect only |
| Stack traces | YES         | YES       | Helps                  | 10–30m  | High (cause) |
| Runtime Truth | NO         | NO        | No                     | < 5m    | High (cause) |

RT's practical advantage is **no code change and no re-deploy**. In a production environment where re-deploying requires a PR, CI run, and deployment pipeline, RT's zero-instrumentation model is a genuine time savings. In a local dev environment with hot reload, the advantage narrows.
