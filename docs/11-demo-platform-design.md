# Demo Platform Design — LLM Debugging Benchmark

**Status**: Design complete. API layer implemented (2026-05-30).  
**Goal**: Prove that an LLM/debugging agent finds bugs faster with runtime causality graph access than with logs + source code alone.

---

## Background

The ManyCore JVM instrumentation is interpreter-complete for runtime causality reconstruction as of Phase 2E. Every dynamic dispatch in user code — invokevirtual, invokeinterface, invokedynamic, Method.invoke, Constructor.newInstance, proxy chains, lambda bodies, hidden classes — is captured with exact source attribution.

This document designs the demo layer that turns that capability into a demonstrable, measurable claim.

**What we are NOT doing here:**
- No JIT work
- No staticization output
- No JVM capture logic changes
- No new graph builder features

**What we ARE doing:**
- Designing a benchmark that measures agent debugging speed and accuracy
- Specifying a causality API that LLM agents can query
- Designing 4 demo bugs that prove the system's value
- Implementing the API layer (first step, done in this session)

---

## Task 1 — Demo Benchmark Design

### Setup

Two agents, same model (Claude Sonnet 4.6 or equivalent), same system prompt preamble, diverging only on tool access:

| | Agent A (baseline) | Agent B (causality) |
|---|---|---|
| Source code | ✓ | ✓ |
| Application logs | ✓ | ✓ |
| Stack trace | ✓ | ✓ |
| Test suite | ✓ | ✓ |
| Causality API | ✗ | ✓ |

**Benchmark structure:**
- One Spring Boot demo app with 4 injected bugs (one per category)
- Each bug is run as an independent trial
- Both agents attempt each bug independently with a wall-clock time limit (suggest 5 minutes per bug)
- Agent B is given tool descriptions and examples for the causality API
- A human evaluator (or automated test suite) grades diagnosis and patch correctness

**Agent A system prompt addition:**
```
You have access to the following:
- Application source code (attached)
- Application logs from the failing run (attached)
- Stack trace from the exception (attached)
- A test suite you can run to verify fixes

Diagnose and fix the bug. Explain your reasoning.
```

**Agent B system prompt addition (on top of A's):**
```
You also have access to a runtime causality API that recorded every method dispatch 
during the failing run. The API returns structured JSON. Use it to identify which 
method was actually invoked at each call site, including through proxies, reflection, 
and dynamic dispatch. See the API reference below.
```

### Metrics

| Metric | How measured |
|---|---|
| `time_to_diagnosis_s` | Wall-clock seconds from first token to agent stating the correct root cause |
| `diagnosis_correctness` | 1.0 = exact method + class named; 0.5 = correct class only; 0.0 = wrong |
| `patch_correctness` | 1.0 = patch makes all tests pass; 0.5 = partially fixes; 0.0 = wrong/breaks |
| `wrong_hypotheses` | Count of incorrect root causes stated before correct one |
| `files_inspected` | Distinct source file paths the agent read during diagnosis |
| `api_queries_made` | (Agent B only) Number of causality API calls made |
| `explanation_quality` | Human-rated 1–5: precision, causal correctness, conciseness |

### Expected outcome hypothesis

For bugs involving proxy chains, reflection, and polymorphic dispatch:
- Agent A will read more files, form more wrong hypotheses, and take longer
- Agent B will identify the actual dispatch target in 1–2 API calls and go directly to the fix

For Agent B, `files_inspected` should be lower (fewer red herrings from proxy class names, lambda hidden class names) and `time_to_diagnosis_s` should be 2–5× lower.

---

## Task 2 — Causality API Design

### Design principles

1. Every endpoint returns structured JSON — no free-text explanation required to use it
2. All class names in slash-form (`com/example/Foo`) consistent with JSONL
3. Every response includes an `explanation` field: a one-paragraph human-readable description of what the data means — for direct consumption by the LLM agent without requiring it to understand the full graph schema
4. Endpoints are scoped to a `run_id` — the ID of a completed provenance export run in the ManyCore UI
5. All endpoints are read-only GET requests

### Endpoint specifications

#### `GET /api/runs/{run_id}/causality/summary`

Returns overall graph statistics for the run.

**Response:**
```json
{
  "run_id": "...",
  "node_counts": {
    "callsite": 4322,
    "method": 1234,
    "class": 567,
    "hidden_class": 5,
    "bytecode_artifact": 42,
    "diagnostic": 39
  },
  "edge_counts": {
    "CALLSITE_TARGET": 4322,
    "LAMBDA_BODY": 412,
    "CALLSITE_RT_ATTRIBUTED": 2905
  },
  "blocked_callsites": 12,
  "staticizable_callsites": 4310,
  "orphan_runtime_targets": 0,
  "heuristic_edges_created": 0,
  "export_complete": true,
  "explanation": "The graph captured 4322 callsite dispatch records. 12 callsites are polymorphic (multiple observed targets — potential sources of dispatch surprises). 4310 are monomorphic and staticizable. 0 orphans — every dispatch has an attributed source."
}
```

#### `GET /api/runs/{run_id}/causality/search?q={fragment}`

Search callsites by class name or method name fragment.

**Response:**
```json
{
  "query": "PaymentService",
  "matches": [
    {
      "source_class": "com/example/PaymentService",
      "source_method": "processPayment",
      "source_bci": 42,
      "source_opcode": "invokevirtual",
      "category": "reflection_method_invoke",
      "target_class": "com/example/StripeProcessor",
      "target_method": "charge",
      "target_descriptor": "(Lcom/example/Order;)Z",
      "static_label": "staticizable_candidate_direct",
      "evidence": "LINKAGE_GUARANTEED"
    }
  ],
  "count": 1,
  "explanation": "Found 1 callsite in/from PaymentService. The callsite at processPayment bci=42 uses reflection (Method.invoke) and dispatches to StripeProcessor.charge."
}
```

#### `GET /api/runs/{run_id}/causality/polymorphic`

List all callsites where more than one distinct target was observed. These are the sites where bugs involving the wrong implementation being called are most likely.

**Response:**
```json
{
  "polymorphic_callsites": [
    {
      "source_class": "com/example/Router",
      "source_method": "route",
      "source_bci": 55,
      "source_opcode": "invokeinterface",
      "category": "invokeinterface",
      "static_label": "blocked_multi_target",
      "target_count": 3,
      "targets": [
        {"class": "com/example/UserHandler", "method": "handle", "descriptor": "(Lcom/example/Request;)V"},
        {"class": "com/example/AdminHandler", "method": "handle", "descriptor": "(Lcom/example/Request;)V"},
        {"class": "com/example/ErrorHandler", "method": "handle", "descriptor": "(Lcom/example/Request;)V"}
      ]
    }
  ],
  "count": 1,
  "explanation": "Found 1 polymorphic callsite — a dispatch point where multiple concrete implementations were observed during the run. If a bug manifests only for some inputs, check each target implementation listed. blocked_multi_target means the JVM cannot statically predict which implementation will be called."
}
```

#### `GET /api/runs/{run_id}/causality/reflection`

List all callsites where reflection (`Method.invoke` or `Constructor.newInstance`) was used.

**Response:**
```json
{
  "reflection_callsites": [
    {
      "source_class": "com/example/PaymentService",
      "source_method": "processPayment",
      "source_bci": 42,
      "category": "reflection_method_invoke",
      "target_class": "com/example/StripeProcessor",
      "target_method": "charge",
      "target_descriptor": "(Lcom/example/Order;)Z",
      "evidence": "LINKAGE_GUARANTEED"
    }
  ],
  "count": 1,
  "explanation": "Found 1 reflection callsite. At processPayment bci=42, Method.invoke was used to call StripeProcessor.charge. Reflection hides the actual target from normal stack traces and IDE search — the causality graph resolves it directly."
}
```

#### `GET /api/runs/{run_id}/causality/proxies`

List all callsites targeting generated or proxy classes (Spring CGLIB, JDK Proxy, ByteBuddy, Mockito mocks).

**Response:**
```json
{
  "proxy_sites": [
    {
      "source_class": "com/example/OrderController",
      "source_method": "checkout",
      "source_bci": 15,
      "source_opcode": "invokevirtual",
      "target_class": "com/example/Application$$SpringCGLIB$$0",
      "target_method": "saveOrder",
      "proxy_kind": "cglib",
      "note": "CGLIB-generated proxy — the real implementation is likely OrderService.saveOrder"
    }
  ],
  "count": 1,
  "explanation": "Found 1 callsite targeting a generated/proxy class. Proxy classes (CGLIB, JDK Proxy, ByteBuddy) intercept calls before forwarding to the real implementation. The class name in a stack trace will be the generated name, not the business class. Use /causality/chain to follow the full dispatch through the proxy."
}
```

#### `GET /api/runs/{run_id}/causality/hidden`

List all hidden classes (lambda implementations, generated classes using `defineHiddenClass`).

**Response:**
```json
{
  "hidden_classes": [
    {
      "runtime_name": "com/example/App$$Lambda$1/0x0000000123456789",
      "artifact_crc": "a1b2c3d4",
      "loader_id": "0x00000001234abcde",
      "has_artifact": true
    }
  ],
  "count": 5,
  "explanation": "Found 5 hidden classes. Hidden classes are generated at runtime for lambda expressions and some reflection operations. Their names include a hex suffix (+0x...) that changes between JVM runs. The CRC field is stable across runs for the same bytecode. Use /causality/search to find which callsite created each hidden class."
}
```

#### `GET /api/runs/{run_id}/causality/chain?class={cls}&method={method}&bci={bci}`

Get the full causality chain for a specific callsite.

**Response:**
```json
{
  "callsite": "com/example/Router.route@bci=55",
  "chain": [
    {
      "type": "callsite",
      "class": "com/example/Router",
      "method": "route",
      "bci": 55,
      "opcode": "invokeinterface",
      "static_label": "blocked_multi_target"
    },
    {
      "edge": "CALLSITE_TARGET",
      "target_class": "com/example/UserHandler",
      "target_method": "handle"
    },
    {
      "edge": "CALLSITE_TARGET",
      "target_class": "com/example/AdminHandler",
      "target_method": "handle"
    },
    {
      "edge": "CALLSITE_TARGET",
      "target_class": "com/example/ErrorHandler",
      "target_method": "handle"
    }
  ],
  "explanation": "The callsite Router.route@bci=55 dispatched to 3 distinct targets during this run. Each CALLSITE_TARGET edge represents one concrete class observed at this dispatch point."
}
```

#### `GET /api/runs/{run_id}/causality/explain?class={cls}&method={method}&bci={bci}`

Get a fully self-contained LLM-readable explanation of a callsite. This is the highest-value endpoint for a debugging agent — it combines chain data, static label, dispatch mechanism, and a synthesized explanation in one call.

**Response:**
```json
{
  "callsite": "com/example/PaymentService.processPayment@bci=42",
  "dispatch_mechanism": "reflection_method_invoke",
  "polymorphic": false,
  "target_count": 1,
  "targets": [
    {
      "class": "com/example/StripeProcessor",
      "method": "charge",
      "descriptor": "(Lcom/example/Order;)Z",
      "evidence": "LINKAGE_GUARANTEED"
    }
  ],
  "static_label": "staticizable_candidate_direct",
  "staticizable": true,
  "explanation": "At com/example/PaymentService.processPayment (bytecode index 42), a reflective Method.invoke call dispatches to com/example/StripeProcessor.charge. This is a monomorphic callsite — only one concrete target was observed during this run. The method was resolved exactly (LINKAGE_GUARANTEED). If this call site is producing unexpected behavior, the bug is in StripeProcessor.charge.",
  "chain": [...]
}
```

---

## Task 3 — Demo Bug Specifications

All 4 bugs live in the same Spring Boot demo app at:
`tools/demo-buggy-app/` (to be created)

The app is a minimal e-commerce backend:
- `PaymentService` — processes orders by dispatching to a payment processor
- `OrderService` — saves orders, annotated `@Transactional` (Spring proxy)
- `ProductService` — sorts/filters product lists using stream lambdas
- `RequestRouter` — routes incoming requests to handlers by type

### Bug 1 — Reflection: Wrong Payment Processor

**Scenario:** `PaymentService.processPayment(Order)` selects a payment processor class by name from a config map and invokes its `charge()` method via `Method.invoke`. A config key collision means `STRIPE` requests are routed to `PaypalProcessor.charge` instead of `StripeProcessor.charge`. Both processors implement the same `PaymentProcessor` interface.

**Failure symptom:**
```
InvocationTargetException at PaymentService.processPayment(PaymentService.java:42)
  Caused by: com.example.PaypalException: Currency EUR not supported by PayPal
```
No class name in the exception tells you which processor was called.

**Why logs are confusing:** `InvocationTargetException` wraps the real cause. The stack trace shows `PaymentService.java:42` but the line is just `method.invoke(processor, order)` — no indication of which `method` it is.

**What the causality graph reveals:**
```
Query: GET /causality/reflection
→ reflection_method_invoke: PaymentService.processPayment bci=42 → PaypalProcessor.charge
```
The agent immediately knows it's `PaypalProcessor`, not `StripeProcessor`, and looks at the config map.

**API query Agent B uses:** `/causality/reflection` then `/causality/explain?class=com/example/PaymentService&method=processPayment&bci=42`

**Expected fix:** Fix the key in the config map from `"STRIPE"` to a key that maps to `StripeProcessor`.

**Success criteria:** `PaymentServiceTest.testStripePayment()` passes.

---

### Bug 2 — Proxy Chain: Silently Swallowed Exception in @Transactional Proxy

**Scenario:** `OrderService.saveOrder(Order)` is annotated `@Transactional`. Spring wraps it in a CGLIB proxy. Inside `saveOrder`, a validation step throws a custom `ValidationException` — but the CGLIB interceptor catches all `RuntimeException`s and swallows them to "ensure transaction cleanup," due to an overly broad catch block added as a workaround during a previous incident.

**Failure symptom:**
```
Order saved successfully
Order not found in database
Test assertion failed: expected order ID 42, got null
```
No exception is thrown. The order silently disappears.

**Why logs are confusing:** The order service returns normally from the CGLIB proxy's perspective. Stack traces don't show the swallowed exception. The developer reads `OrderService.saveOrder` source, sees the validation, but the bug is in the interceptor class `OrderTransactionInterceptor` which is framework-wired.

**What the causality graph reveals:**
```
Query: GET /causality/proxies
→ OrderController.checkout bci=15 → Application$$SpringCGLIB$$0.saveOrder [cglib]

Query: GET /causality/chain?class=com/example/OrderController&method=checkout&bci=15
→ CALLSITE_TARGET: OrderController.checkout → Application$$SpringCGLIB$$0.saveOrder
→ (via invokeinterface) → OrderTransactionInterceptor.intercept
→ (via invokeinterface in interceptor) → OrderService.saveOrder
```
The agent sees `OrderTransactionInterceptor` in the chain and reads its `intercept()` method.

**API query Agent B uses:** `/causality/proxies` then `/causality/chain?class=...`

**Expected fix:** Narrow the catch block in `OrderTransactionInterceptor.intercept` from `RuntimeException` to `PersistenceException`.

**Success criteria:** `OrderServiceTest.testSaveOrderValidation()` passes.

---

### Bug 3 — Lambda / Hidden Class: Wrong Comparator in Stream Sort

**Scenario:** `ProductService.sortByPrice(List<Product>)` uses a stream with a lambda comparator. The lambda has a sign bug: it returns `b.price - a.price` instead of `a.price - b.price`, producing reverse-descending order when ascending is expected. The lambda body is stored as a hidden class.

**Failure symptom:**
```
NullPointerException in com/example/ProductService$$Lambda$1/0x00007f1234abcd.apply at line -1
  at java.util.stream.SortedOps.lambda$ofRef$0
  at java.util.Arrays.sort
```
No source line. The hidden class name changes every JVM run.

**Why logs are confusing:** Lambda hidden class names are unstable. The stack trace line number is -1 (no debug info in the hidden class). Developer searches codebase for `Lambda$1` and finds nothing. `ProductService.java` has three lambdas — which one?

**What the causality graph reveals:**
```
Query: GET /causality/search?q=ProductService
→ callsite_target[invokedynamic]: ProductService.sortByPrice bci=22
    lmf_impl_class: com/example/ProductService
    lmf_impl_method: lambda$sortByPrice$0
    lmf_impl_descriptor: (Lcom/example/Product;Lcom/example/Product;)I

Query: GET /causality/hidden
→ hidden_class: com/example/ProductService$$Lambda$1/0x...
    CRC: a3f1b2d8 (stable across runs, maps to the sort lambda)
```
The agent knows the exact source method `lambda$sortByPrice$0` in `ProductService.java`.

**API query Agent B uses:** `/causality/search?q=ProductService` then `/causality/explain?class=...&method=sortByPrice&bci=22`

**Expected fix:** Flip comparator in `ProductService.sortByPrice`: `a.price - b.price` → `b.price - a.price` (intentionally reversed — fix is to put it back).

**Success criteria:** `ProductServiceTest.testSortAscending()` passes.

---

### Bug 4 — Polymorphic Dispatch: Off-by-One in One Handler

**Scenario:** `RequestRouter.route(Request)` dispatches to one of three handlers via an `invokeinterface` on the `RequestHandler` interface: `UserHandler`, `AdminHandler`, `ErrorHandler`. `ErrorHandler.handle(Request)` has an off-by-one error in its response code array lookup (`codes[type]` vs `codes[type - 1]`), causing `ArrayIndexOutOfBoundsException` only for error-type requests.

**Failure symptom:**
```
ArrayIndexOutOfBoundsException: Index 3 out of bounds for length 3
  at com.example.RequestRouter.route(RequestRouter.java:55)
```
The exception is caught and re-thrown by `route()` — the stack frame points to the dispatch site, not the handler.

**Why logs are confusing:** The exception shows `RequestRouter.route:55` — the dispatch site — not the handler. Developer reads `route()` source; the method just calls `handler.handle(request)`. Three handlers exist. Without knowing which one executed, the developer reads all three.

**What the causality graph reveals:**
```
Query: GET /causality/polymorphic
→ blocked_multi_target: RequestRouter.route bci=55
    targets: [UserHandler.handle, AdminHandler.handle, ErrorHandler.handle]

Query: GET /causality/explain?class=com/example/RequestRouter&method=route&bci=55
→ "3 concrete implementations observed. For error-type requests, ErrorHandler.handle 
   is invoked. Check ErrorHandler.handle for array bounds issues."
```
Agent B immediately narrows to `ErrorHandler` and finds the off-by-one.

**API query Agent B uses:** `/causality/polymorphic` then `/causality/explain?class=...&method=route&bci=55`

**Expected fix:** Change `codes[type]` to `codes[type - 1]` in `ErrorHandler.handle` (or vice versa depending on design intent).

**Success criteria:** `RequestRouterTest.testErrorRequest()` passes.

---

## Task 4 — Implementation Order

**Recommendation: A — API first.**

Reasoning:
1. The causality API is the dependency for both the demo bugs and the benchmark harness. Without the API, Agent B has no advantage and the benchmark cannot run.
2. The API can be tested immediately against existing JSONL files (Spring Boot run, breadth validation areas). No new JVM runs needed.
3. Building the demo app without the API wastes time — the bug scenarios need to be calibrated against what the API can actually reveal.
4. The benchmark harness is just a wrapper around two API-equipped conversations — it's the last layer to build.

**Correct order:**
1. **API** (this session) — expose graph_builder.py as REST endpoints in manycore-ui
2. **Demo app** (next session) — Spring Boot app with 4 injected bugs, test suite, run script
3. **Benchmark harness** (after demo app) — orchestrator that runs both agents, collects metrics
4. **UI integration** (optional/last) — surface causality API results in the existing manycore-ui frontend

---

## Task 5 — Implementation: Causality API

**Status: IMPLEMENTED (2026-05-30)**

The causality API is implemented as 8 new endpoints in `tools/manycore-ui/app.py`, all prefixed `/api/runs/<run_id>/causality/`.

### Endpoints implemented

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/runs/{id}/causality/summary` | Graph statistics, coverage metrics |
| GET | `/api/runs/{id}/causality/search?q=` | Search callsites by class/method fragment |
| GET | `/api/runs/{id}/causality/polymorphic` | List multi-target callsites (blocked_multi_target) |
| GET | `/api/runs/{id}/causality/reflection` | List reflection callsites (Method.invoke, Constructor.newInstance) |
| GET | `/api/runs/{id}/causality/proxies` | List proxy/generated-class dispatch sites |
| GET | `/api/runs/{id}/causality/hidden` | List hidden class identities |
| GET | `/api/runs/{id}/causality/chain?class=&method=&bci=` | Full causality chain for one callsite |
| GET | `/api/runs/{id}/causality/explain?class=&method=&bci=` | LLM-ready explanation of one callsite |

### Architecture notes

- `graph_builder.build_graph()` is called on first causality request per run, result cached in `_graph_cache`
- Endpoints that only need the flat index (reflection, proxies, hidden, search) use `_get_index()` for speed
- Endpoints that need graph structure (chain, explain, summary, polymorphic) use `_get_graph()`
- All responses include an `explanation` field — self-contained natural language for the LLM agent

### Example usage (curl)

```bash
BASE=http://localhost:5000
RUN_ID=run_20260530_123456

# Graph summary
curl "$BASE/api/runs/$RUN_ID/causality/summary"

# Find callsites in/from PaymentService
curl "$BASE/api/runs/$RUN_ID/causality/search?q=PaymentService"

# List all reflection sites
curl "$BASE/api/runs/$RUN_ID/causality/reflection"

# List all polymorphic dispatch sites
curl "$BASE/api/runs/$RUN_ID/causality/polymorphic"

# Get full chain for a specific callsite
curl "$BASE/api/runs/$RUN_ID/causality/chain?class=com/example/Router&method=route&bci=55"

# Get LLM-ready explanation
curl "$BASE/api/runs/$RUN_ID/causality/explain?class=com/example/Router&method=route&bci=55"
```

---

## Task 6 — Documentation

| Document | Status |
|---|---|
| `docs/11-demo-platform-design.md` | Created (this file) |
| `docs/AGENT_NAVIGATOR.md` | Updated — pointer to this doc added |
| `tools/manycore-ui/app.py` | Updated — 8 causality endpoints added |

---

## Next Tasks (for next session)

### Task A — Build the demo buggy Spring Boot app
Location: `tools/demo-buggy-app/`

Requirements:
- Maven or Gradle project, buildable to a fat JAR
- 4 classes with injected bugs as specified in Task 3
- A test suite (`JUnit 5`) with one test per bug (green when bug is fixed, red when injected)
- A `run_with_provenance.sh` script that runs the app under the custom JVM with `SOROUSH_PROVENANCE_GRAPH=1` and exports to `/tmp/demo_run.jsonl`
- A `README.md` explaining the setup

### Task B — Run the demo app and verify the API reveals the bugs
- Run `run_with_provenance.sh`
- Load the JSONL into manycore-ui
- Call each causality endpoint and verify it returns the expected data for each bug
- Document the exact API query sequences Agent B should use

### Task C — Build the benchmark harness
Location: `tools/benchmark/`

Requirements:
- `run_benchmark.py` — takes a JSONL run_id and a bug scenario ID, starts two agent conversations in parallel
- Agent A: Claude API call with source code + logs context
- Agent B: Same + causality tool definitions
- Scoring: compare `time_to_diagnosis_s`, `diagnosis_correctness`, `patch_correctness`
- Output: `benchmark_results.json` with per-bug per-agent metrics

### Task D — Produce benchmark results
- Run Task C against all 4 bugs
- Compute summary statistics
- Write a brief writeup at `docs/12-benchmark-results.md`

---

## Appendix: API Reference for Agent B

This is the tool description to include in Agent B's system prompt.

```
## Causality API Tools

You can query the runtime causality graph to discover which methods were actually 
called during the failing run, even through reflection, proxy layers, and dynamic dispatch.

All endpoints: GET /api/runs/{RUN_ID}/causality/<endpoint>
Base URL: http://localhost:5000
Run ID: provided by the benchmark harness as $RUN_ID

### Tools

1. GET /causality/summary
   Returns: overall graph stats. Use first to understand the run.

2. GET /causality/search?q={fragment}
   Returns: all callsites in/from classes or methods matching the fragment.
   Use when: you know the class name but not which BCI is involved.

3. GET /causality/reflection
   Returns: all Method.invoke and Constructor.newInstance callsites with their actual targets.
   Use when: you see InvocationTargetException or suspect reflection is hiding the real callee.

4. GET /causality/polymorphic
   Returns: all callsites with multiple observed targets. 
   Use when: the bug only manifests for some inputs — the wrong implementation might be selected.

5. GET /causality/proxies
   Returns: all callsites targeting generated/proxy classes.
   Use when: stack traces show $$SpringCGLIB$$, $Proxy, $ByteBuddy$, or MockitoMock.

6. GET /causality/hidden
   Returns: all lambda hidden class identities with their source locations.
   Use when: stack trace shows Lambda$N/0x... and you need the source method.

7. GET /causality/chain?class={cls}&method={method}&bci={bci}
   Returns: full causality chain from a specific callsite.
   Use when: you know the callsite and want all targets and adapter chain details.

8. GET /causality/explain?class={cls}&method={method}&bci={bci}
   Returns: a complete self-contained explanation of the callsite.
   Use this as your primary diagnostic tool — it synthesizes all available evidence.
```
