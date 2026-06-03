# Runtime Truth Causality — Demo Bug Suite

**Purpose**: Prove the value of the Runtime Truth Runtime Causality platform relative to conventional debugging.
Each bug is a self-contained Spring Boot HTTP endpoint. Agent A (no causality) and Agent B (with causality API) are given the same symptom description and asked to identify the root cause.

**Location**: `tools/demo-buggy-app/`  
**Build**: `mvn package -DskipTests`  
**Run**: `./run_demo.sh --non-interactive` (exercises all 4 bugs and exits)  
**Port**: 8090 (configurable in `application.properties`)

---

## Validation results (2026-05-30)

Run under `custom-jvm` fastdebug build, `-Xint`, provenance mode:

```
JSONL total records:           50861
reflection_method_invoke:         34  (incl. 1 from NotificationService.dispatch)
callsite_target (invokeinterface): 3 for RequestRouter.route bci=40
CGLIB proxy callsites:             2  (BugController → OrderService$$SpringCGLIB$$0)
invokedynamic from ProductCatalog: 8
```

All four bugs are present in the JSONL and visible through the causality API.

---

## Bug 1 — Reflection: Silent Handler Misdispatch

**Endpoint**: `GET /bug/reflection?type=DELIVERY&payload=user@example.com`

**Symptom**: Delivery events return HTTP 200 and log "dispatched:DELIVERY:AuditHandler" — no error, no crash, but no delivery is ever sent.

**Root cause**: `NotificationService` maps `"DELIVERY"` to `AuditHandler.class` instead of `DeliveryHandler.class`. The `dispatch()` method uses `Method.invoke()`, so static analysis and normal stack traces do not reveal which class actually handled the event.

**Files**: `bug1/NotificationService.java` (line 30), `bug1/AuditHandler.java`, `bug1/DeliveryHandler.java`

**JSONL evidence**:
```
record=callsite_target
category=reflection_method_invoke
source_class=com/example/demo/bug1/NotificationService
source_method=dispatch
source_bci=81
target_class=com/example/demo/bug1/AuditHandler
target_method=handle
```

**Causality API call**:
```
GET /api/runs/{run_id}/causality/reflection
```
Returns the `reflection_method_invoke` site with `target=AuditHandler.handle`. One API call reveals the bug.

**Agent A difficulty**: Must read `NotificationService` constructor, notice `registry.put("DELIVERY", AuditHandler.class)`, and connect it to the reflection dispatch. The source is in front of them but requires careful reading across multiple files.

**Agent B advantage**: Direct API call returns `source=NotificationService.dispatch bci=81 → target=AuditHandler.handle`. Reveals the wrong handler in one step. Expected TTID reduction: 80–90%.

---

## Bug 2 — Proxy: Wrong Tax Rate Hidden Behind CGLIB Proxy

**Endpoint**: `GET /bug/proxy?orderId=ORD-001&amount=1000&type=INTERNATIONAL`

**Symptom**: An INTERNATIONAL order for $1000 returns total `1080.00`. Expected: `1150.00` (15% tax). The log says "Order processed: ORD-001 total=1080.00" — no error, number is plausible.

**Root cause**: `OrderService.processOrder()` has an inverted conditional — `INTERNATIONAL` orders receive `DOMESTIC_TAX_RATE` (0.08) and domestic orders receive `INTERNATIONAL_TAX_RATE` (0.15). The `AuditInterceptor` AOP advice wraps every call, making the CGLIB proxy class (`OrderService$$SpringCGLIB$$0`) the visible dispatch target.

**Files**: `bug2/OrderService.java` (lines 45–48), `bug2/AuditInterceptor.java`

**JSONL evidence**:
```
record=callsite_target
category=invokevirtual
source_class=com/example/demo/BugController
target_class=com/example/demo/bug2/OrderService$$SpringCGLIB$$0
target_method=processOrder
```
Proxy chain visible; further dispatch through `AuditInterceptor.timed` to real `OrderService.processOrder`.

**Causality API calls**:
```
GET /api/runs/{run_id}/causality/proxies      → reveals CGLIB proxy
GET /api/runs/{run_id}/causality/chain        → full chain to OrderService.processOrder
```

**Agent A difficulty**: Stack traces always show `OrderService$$SpringCGLIB$$0` and `AuditInterceptor.timed`. Agent A must mentally strip the proxy and interceptor layers to find the real implementation. Then must spot the inverted conditional in a method with only two branches.

**Agent B advantage**: `/causality/proxies` immediately names the CGLIB proxy, `/causality/chain` navigates through it. The target is `OrderService.processOrder` — one place to look.

---

## Bug 3 — Polymorphic: Guest Sessions Receive Admin Permissions

**Endpoint**: `GET /bug/polymorphic?type=GUEST&userId=u789`

**Symptom**: GUEST profile response contains `permissions=[read,write,delete,manage]`. This is the full admin permission set — a privilege escalation. No exception is thrown; the response is structurally valid JSON.

**Root cause**: `GuestHandler.handle()` was copy-pasted from `AdminHandler` without changing the permissions list. `RequestRouter.route()` dispatches via `invokeinterface` on `RequestHandler` — the interface call site reveals nothing about which concrete class executes.

**Files**: `bug3/GuestHandler.java` (line 19), `bug3/RequestRouter.java`

**JSONL evidence** (three separate `callsite_target` records at same BCI, merged by graph_builder into `blocked_multi_target`):
```
source_class=com/example/demo/bug3/RequestRouter  source_method=route  source_bci=40
  target_class=com/example/demo/bug3/GuestHandler   target_method=handle
  target_class=com/example/demo/bug3/AdminHandler   target_method=handle
  target_class=com/example/demo/bug3/UserHandler    target_method=handle
```

**Causality API call**:
```
GET /api/runs/{run_id}/causality/polymorphic
```
Returns `blocked_multi_target` callsite at `RequestRouter.route bci=40` with all 3 observed concrete targets. For a GUEST request, the observed target is `GuestHandler.handle` — immediately identifying the implementation to inspect.

**Agent A difficulty**: Must navigate the handler registry in `RequestRouter`, identify that `GUEST` maps to `GuestHandler`, open `GuestHandler.java`, and compare it with the correct implementation. None of this is visible from the stack trace alone.

**Agent B advantage**: `blocked_multi_target` lists exactly which concrete handler executes for each request type. For GUEST → `GuestHandler.handle`. One file to inspect.

---

## Bug 4 — Lambda/Hidden Class: CLEARANCE Price Computed from Base Price

**Endpoint**: `GET /bug/lambda?name=Widget&category=CLEARANCE&basePrice=100&salePrice=60`

**Symptom**: CLEARANCE item with `basePrice=100, salePrice=60` returns `calculatedPrice=50.00`. Expected: `30.00` (50% off the sale price of 60). The price is numerically plausible — it looks like a 50%-off sale, just calculated against the wrong base.

**Root cause**: The `CLEARANCE` lambda in `ProductCatalog` uses `p.basePrice() * 0.50` instead of `p.salePrice() * 0.50`. The `calculatePrice()` method dispatches to the lambda via `invokedynamic` — the hidden class holding the lambda body is the actual dispatch target and is not visible in any normal source listing.

**Files**: `bug4/ProductCatalog.java` (line 36)

**JSONL evidence**:
```
record=callsite_target
category=invokedynamic
source_class=com/example/demo/bug4/ProductCatalog
source_method=<init>
source_bci=55   (bootstrap for CLEARANCE lambda)
target_class=com/example/demo/bug4/ProductCatalog$$Lambda/...  (hidden class)
```

**Causality API call**:
```
GET /api/runs/{run_id}/causality/hidden
```
Returns the `invokedynamic` site with the resolved hidden class, linking it back to the `ProductCatalog` source BCI. Agent B can identify which lambda bootstrap corresponds to the CLEARANCE pricer (bci=55) and inspect that specific lambda expression.

**Agent A difficulty**: Must know that `Function<Product, Double>` stored in a map is a lambda, open `ProductCatalog.java`, read all four lambda bodies, and identify the one with `basePrice` vs `salePrice`. No stack trace or log points to a lambda specifically.

**Agent B advantage**: `/causality/hidden` links `source_bci=55` to the hidden class. Combined with bytecode inspection (bci 21, 38, 55, 72 correspond to lambdas in constructor order), narrows to the CLEARANCE lambda immediately.

---

## Running under the causality API

After exercising the endpoints, ingest the export into a running rt-ui:

```bash
# Start rt-ui (if not already running)
cd tools/rt-ui && python app.py 5001

# Ingest the demo run (requires rt-ui restart to pick up /api/runs/ingest)
curl -X POST http://localhost:5001/api/runs/ingest \
     -H 'Content-Type: application/json' \
     -d '{"label":"demo-buggy-app","run_dir":"/tmp/demo-buggy-app-export"}'

# Use the returned run_id for causality queries
RUN_ID=<returned run_id>
curl http://localhost:5001/api/runs/$RUN_ID/causality/reflection
curl http://localhost:5001/api/runs/$RUN_ID/causality/proxies
curl http://localhost:5001/api/runs/$RUN_ID/causality/polymorphic
curl http://localhost:5001/api/runs/$RUN_ID/causality/hidden
```

**Note**: The `/api/runs/ingest` endpoint was added in this session to `app.py` and `runner.py`. A rt-ui restart is required to activate it.

---

## A/B Benchmark Summary

| Bug | Mechanism | Agent A signal | Agent B signal | Expected TTID reduction |
|-----|-----------|----------------|----------------|------------------------|
| 1   | Reflection misdispatch | "DELIVERY dispatched to AuditHandler" (visible in response) | `/causality/reflection` → `AuditHandler.handle` | 80–90% |
| 2   | Proxy hidden tax rate  | Stack shows CGLIB proxy + interceptor; number is plausible | `/causality/proxies` + `/causality/chain` → `OrderService.processOrder` | 60–75% |
| 3   | Polymorphic wrong impl | Response JSON has wrong permissions; interface call site opaque | `/causality/polymorphic` → `blocked_multi_target` → `GuestHandler.handle` | 70–85% |
| 4   | Lambda wrong field     | Price is wrong; no stack trace; lambda body invisible | `/causality/hidden` → `source_bci=55` → CLEARANCE lambda | 75–90% |

All four bugs are confirmed present in the JSONL export generated by a single `./run_demo.sh --non-interactive` session.
