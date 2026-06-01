# doc 28 — Phase V8: Spring Proxy Bypass Demo

**Date:** 2026-05-31

---

## The Bug

Spring `@Transactional` works by wrapping your service bean in a proxy. When a caller invokes `orderService.processOrder()`, the call goes through the proxy, which starts a transaction before delegating to your real code.

The trap: inside the bean, `this` refers to the raw object — not the proxy. Any call `this.someMethod()` bypasses every Spring interceptor on `someMethod`, silently.

This is one of the most common Spring production bugs. The annotation is present. The code compiles. Tests often pass. The transaction simply never starts when it should.

---

## Codebase

**Location:** `tools/demo-spring-proxy-bypass/`  
**Stack:** Spring Boot 4.0.6 + H2 (transaction manager only — no entities)  
**Port:** 19090

### Files

```
src/main/java/com/example/proxydemo/
  ProxyBypassApplication.java   @SpringBootApplication entry
  OrderService.java             The bug + the fix, side by side
  CallRecord.java               Return type (Java record)
  OrderController.java          GET /compare — shows both paths
src/main/resources/
  application.properties        port=19090, allow-circular-references=true
```

### OrderService.java — the whole story

```java
@Service
public class OrderService {

    @Autowired
    private OrderService self;    // self-injection = Spring proxy, the fix

    // BUGGY: 'this' is the raw object. Spring never sees this call.
    @Transactional
    public CallRecord processOrder(String orderId) {
        String auditTx = this.saveAuditEntry(orderId);   // ← BUG
        return new CallRecord("processOrder", txName(), auditTx, "this.saveAuditEntry()");
    }

    // CORRECT: 'self' is the Spring proxy. Interceptors fire normally.
    @Transactional
    public CallRecord processOrderCorrect(String orderId) {
        String auditTx = self.saveAuditEntry(orderId);   // ← CORRECT
        return new CallRecord("processOrderCorrect", txName(), auditTx, "self.saveAuditEntry()");
    }

    // Intended to run in a NEW transaction. Only works if called through the proxy.
    @Transactional(propagation = Propagation.REQUIRES_NEW)
    public String saveAuditEntry(String orderId) {
        return txName();    // reports which transaction is currently active
    }
}
```

`txName()` returns `TransactionSynchronizationManager.getCurrentTransactionName()`.

---

## What GET /compare Returns

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

The bug is directly readable: both `outer_tx` and `saveAuditEntry_tx` are identical in the buggy path. `saveAuditEntry` never got its own transaction name — it ran inside `processOrder`'s transaction.

---

## The Runtime Truth Signal

When run under the custom JVM with `SOROUSH_USER_PREFIXES=com/example/proxydemo`, the `runtime_targets.jsonl` contains the structural cause:

```
callsite_target:
  source: OrderService.processOrder          @ bci=21
  target: OrderService.saveAuditEntry                 ← RAW CLASS
  opcode: invokevirtual

callsite_target:
  source: OrderService.processOrderCorrect   @ bci=24
  target: OrderService$$SpringCGLIB$$0.saveAuditEntry ← CGLIB PROXY
  opcode: invokevirtual
```

Same method name. Same opcode. Different target class.

The CGLIB suffix `$$SpringCGLIB$$0` means the call was routed through the Spring proxy, where the `@Transactional` interceptor fires. The absent suffix means it went directly to the raw object — the interceptor was never reached.

---

## How to Reproduce

```bash
# Build
cd tools/demo-spring-proxy-bypass
JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-25.jdk/Contents/Home mvn package -DskipTests

# Run + capture
bash capture.sh /tmp/demo_proxy_bypass

# View evidence
cat /tmp/demo_proxy_bypass/compare_response.json
python3 -c "
import json
with open('/tmp/demo_proxy_bypass/runtime_targets.jsonl') as f:
    for line in f:
        r = json.loads(line)
        if r.get('record') == 'callsite_target' and r.get('target_method') == 'saveAuditEntry':
            print(r['source_method'], '->', r['target_class'].split('/')[-1] + '.' + r['target_method'])
"
```

Output:
```
processOrder -> OrderService.saveAuditEntry
processOrderCorrect -> OrderService$$SpringCGLIB$$0.saveAuditEntry
```

---

## The One Thing This Demo Shows

> **Which class actually handled the call?**

Not "which method signature was called" — both are `saveAuditEntry(String)`.  
Not "what transaction context existed" — that's an effect.

The cause: `processOrder` called `OrderService.saveAuditEntry` directly. The proxy was not in the call chain.

That single fact, readable from a JSONL file without a debugger, without log changes, without knowing where to look — is the point of Runtime Truth.
