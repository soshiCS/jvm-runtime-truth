# ManyCore Agent Benchmark Design

**Purpose**: Measure whether runtime causality API access materially improves LLM debugging performance.

This is a product-value test, not a JVM-capture test.

---

## Question

> Does an LLM identify the root cause of a runtime dispatch bug faster and more accurately when it has access to the ManyCore causality API?

---

## Setup

### Controlled variables (identical for both agents)

| Variable | Value |
|---|---|
| Model | `claude-sonnet-4-6` |
| Max tokens per turn | 4096 |
| Max turns | 25 |
| System prompt length | ~same |
| Bug description | identical |
| Available source files | identical — all 17 Java files in `tools/demo-buggy-app/` |
| Log output provided | identical — exact server stdout from a live request |
| Causality JSONL | same captured run (`/tmp/demo-buggy-app-export2/runtime_targets.jsonl`) |

### Independent variable

**Agent B** receives 6 additional tools:

| Tool | What it returns |
|---|---|
| `causality_summary` | Total records, dispatch mechanism counts |
| `causality_reflection` | `reflection_method_invoke` records: actual `Method.invoke()` targets |
| `causality_polymorphic` | `blocked_multi_target` records: concrete classes at interface callsites |
| `causality_proxies` | CGLIB/JDK-proxy callsites with proxy kind and real target |
| `causality_hidden` | `invokedynamic` → hidden class mappings for lambda bodies |
| `causality_search` | Full-text search of the causality graph by class/method name |

**Agent A** has no causality tools.

---

## Agent tool inventories

### Agent A

```
list_files(directory?)     — list Java source files
read_file(path)            — read a source file
submit_diagnosis(...)      — submit final diagnosis (ends the run)
```

### Agent B

```
list_files(directory?)     — list Java source files
read_file(path)            — read a source file
submit_diagnosis(...)      — submit final diagnosis (ends the run)
causality_summary()
causality_reflection()
causality_polymorphic()
causality_proxies()
causality_hidden()
causality_search(query)
```

---

## System prompts

Both prompts instruct the agent to:
- Work methodically
- Read source files
- Submit a diagnosis with file, line, explanation, patch, and confidence

Agent B's prompt additionally mentions the causality tools and explains their value:
> "The causality tools expose what ACTUALLY executed at runtime — not just what source code implies. They resolve reflection targets, proxy chains, and lambda dispatch that are invisible to static analysis and normal stack traces."

---

## Bug scenarios

All bugs are in `tools/demo-buggy-app/`, a Spring Boot 4.0.6 application with 4 HTTP endpoints. The JSONL was captured by running the app under the custom fastdebug JVM with `-Xint` and provenance mode (`SOROUSH_PROVENANCE_GRAPH=1`).

### Bug 1 — Reflection: Silent Handler Misdispatch

**Endpoint**: `GET /bug/reflection?type=DELIVERY&payload=alice@example.com`

**Symptom**: HTTP 200, no error. Log shows `[AUDIT] Recorded event payload: alice@example.com` instead of `[DELIVERY] Sending confirmation to: alice@example.com`.

**Root cause**: `NotificationService.<init>` maps `"DELIVERY"` to `AuditHandler.class` (line 30) instead of `DeliveryHandler.class`. Dispatch is via `Method.invoke()` — invisible to static analysis.

**Causality signal**: `/causality/reflection` → `reflection_method_invoke @ NotificationService.dispatch bci=81 → AuditHandler.handle`

**Ground truth**:
- File: `bug1/NotificationService.java`
- Line: 30
- Patch: `registry.put("DELIVERY", DeliveryHandler.class);`

### Bug 2 — Proxy: Inverted Tax Rate Behind CGLIB Proxy

**Endpoint**: `GET /bug/proxy?orderId=ORD-001&amount=1000&type=INTERNATIONAL`

**Symptom**: HTTP 200, total=`1080.00` (expected `1150.00`). Log: `taxRate=0.08`. DOMESTIC orders also wrong (get 15% instead of 8%). Both directions silently swapped.

**Root cause**: `OrderService.processOrder()` lines 45–48: ternary condition is inverted. Service is wrapped in CGLIB proxy + `AuditInterceptor`. Stack traces show 4 proxy/framework frames before reaching the real method.

**Causality signal**: `/causality/proxies` → `BugController → OrderService$$SpringCGLIB$$0`; `/causality/reflection` → `AopUtils → OrderService.processOrder`

**Ground truth**:
- File: `bug2/OrderService.java`
- Line: 45
- Patch: swap `DOMESTIC_TAX_RATE` and `INTERNATIONAL_TAX_RATE` in the ternary

### Bug 3 — Polymorphic: Guest Privilege Escalation

**Endpoint**: `GET /bug/polymorphic?type=GUEST&userId=u789`

**Symptom**: HTTP 200, profile response contains `permissions=[read,write,delete,manage]` for a GUEST user. Expected: `permissions=[read:public]`.

**Root cause**: `GuestHandler.handle()` was copy-pasted from `AdminHandler` without adjusting the permissions. The router dispatches via `invokeinterface` on `RequestHandler` — static analysis sees only the interface.

**Causality signal**: `/causality/polymorphic` → `blocked_multi_target @ RequestRouter.route bci=40` with all 3 targets: `GuestHandler`, `AdminHandler`, `UserHandler`

**Ground truth**:
- File: `bug3/GuestHandler.java`
- Line: 19
- Patch: change permissions string to `read:public`

---

## Scoring rubric (16 points total)

### File score (0–3)

| Points | Criterion |
|---|---|
| 3 | Exact file identified |
| 1 | Correct package (wrong file within package) |
| 0 | Wrong package or no answer |

### Line score (0–3)

| Points | Criterion |
|---|---|
| 3 | Within ±2 lines of ground truth |
| 2 | Within ±8 lines |
| 1 | Within ±20 lines |
| 0 | Wrong method / no answer |

### Patch score (0–5)

| Points | Criterion |
|---|---|
| 5 | Patch contains the exact fix fragment |
| 3 | Patch references the right keywords but is verbose/imprecise |
| 1 | Patch targets the right file/package but wrong change |
| 0 | Wrong file or no patch |

### Explanation score (0–5)

One point per ground-truth keyword present in the explanation (capped at 5). Keywords per bug:

| Bug | Keywords |
|---|---|
| Bug 1 | AuditHandler, DELIVERY, registry, reflection, dispatch, handler, misdispatch |
| Bug 2 | inverted, swapped, ternary, DOMESTIC, INTERNATIONAL, tax, rate, condition |
| Bug 3 | GuestHandler, copy, AdminHandler, permissions, privilege, guest, role |

### Efficiency metrics (reported separately, not scored)

| Metric | Description |
|---|---|
| `total_turns` | API calls to the LLM before diagnosis |
| `unique_files_read` | Distinct source files opened |
| `causality_calls` | Number of causality API tool invocations |
| `wrong_hypotheses` | Self-reported wrong paths (from `submit_diagnosis.wrong_hypotheses`) |
| `confidence` | Agent's self-reported confidence: low / medium / high |

---

## Harness

**Location**: `tools/benchmark/harness.py`

**Usage**:
```bash
cd tools/benchmark
export ANTHROPIC_API_KEY=...
python harness.py bug1               # run Bug 1, both agents
python harness.py bug1 bug2 bug3     # run all three bugs
python harness.py bug1 --agent A     # run only Agent A
```

The harness:
1. Starts a fresh manycore-ui instance on port 5002 (no auth)
2. Ingests the captured JSONL via `POST /api/runs/ingest`
3. Runs Agent A then Agent B against the specified bug(s)
4. Scores each agent using the rubric above
5. Writes per-run JSONs to `tools/benchmark/results/`
6. Writes a combined `benchmark_result.json`

**Output format** (`benchmark_result.json`):
```json
{
  "bug1": {
    "A": { "trace": {...}, "score": {...} },
    "B": { "trace": {...}, "score": {...} },
    "comparison": { "winner": "B", "score_delta": 4, "turns_saved": 3, ... }
  }
}
```

---

## Threats to validity

| Threat | Mitigation |
|---|---|
| LLM stochasticity | Same model, same prompt; repeat runs possible with `--repeat N` |
| Prompt advantage to B | Both prompts instruct same goal; B prompt only names available tools |
| Easy bugs | All 4 bugs were designed so source code alone is insufficient (reflection, proxy, interface, lambda) |
| Scoring bias | Scoring is automated against objective ground truth; no human judgment in the loop |
| Causality noise | High-cardinality endpoints (polymorphic) are filtered to `com/example/demo` prefix before being returned to the agent |

---

## What counts as success

Agent B is declared to have a **material advantage** if, across ≥2 bugs:
- Score delta ≥ 3 points (out of 16), OR
- Turn count reduction ≥ 30%, OR
- Both agents reach correct diagnosis but B uses ≤50% as many file reads

If neither condition holds, the causality API provides **no measurable advantage** for these bugs and the result is documented as such.
