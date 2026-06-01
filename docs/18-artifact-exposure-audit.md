# Artifact Exposure Audit — V4A

**Goal**: Determine exactly which artifact-related endpoints already exist, what they return,
and whether Agent B can reach them today.

**Method**: Source-verified. Every endpoint description is confirmed against `app.py`,
`indexer.py`, `runner.py`, and `harness_v2.py`. No planned endpoints are described.

---

## 1. Existing artifact-related endpoints

### 1.1 `/api/runs/<id>/bytecode` — javap disassembly

| Field | Value |
|-------|-------|
| **URL** | `GET /api/runs/<run_id>/bytecode?artifact_path=<path>` |
| **Source** | `tools/manycore-ui/app.py:207–234` |
| **Returns** | `javap -c -p -verbose` output for the `.class` file at `artifact_path` |
| **Required params** | `artifact_path` — absolute path to a `.class` file on disk |
| **Agent B today** | ✗ — not in benchmark tool list |
| **harness_v2 exposes** | ✗ |
| **manycore-ui exposes** | ✓ — UI calls it when user clicks "View Bytecode" on an artifact |

**Response schema**:
```json
{
  "output":        "<full javap -c -p -verbose stdout>",
  "stderr":        "<javap stderr if any>",
  "artifact_path": "<path passed in>",
  "exit_code":     0
}
```

**Example response** (for `Processor037.class`):
```json
{
  "output": "Compiled from \"Processor037.java\"\npublic class com.example.demo.engine.proc.Processor037 implements com.example.demo.engine.proc.Processor {\n\n  private static final double FEE_RATE;\n    descriptor: D\n    flags: (0x001a) ACC_PRIVATE, ACC_STATIC, ACC_FINAL\n    ConstantValue: double 0.05d\n\n  public com.example.demo.engine.model.ProcessResult apply(com.example.demo.engine.model.ProcessingContext);\n    Code:\n       0: aload_1\n       1: invokeinterface #7,  1  // InterfaceMethod ...amount:()D\n       6: ldc2_w  #13             // double 0.05d\n       9: dmul\n      ...\n}",
  "stderr": "",
  "artifact_path": "/tmp/rt_ui_runs/abc12345/artifacts/com_example_demo_engine_proc_Processor037.class",
  "exit_code": 0
}
```

**Key diagnostic value**: For the V3 bug, the javap output shows `ConstantValue: double 0.05d` and
`ldc2_w #13 // double 0.05d` in the `apply` method — the bug is directly visible in bytecode.
For generated/lambda classes with no source file, this is the only way to read the class body.

---

### 1.2 `/api/runs/<id>/artifact` — artifact metadata lookup

| Field | Value |
|-------|-------|
| **URL** | `GET /api/runs/<run_id>/artifact?class=<cls>&loader_id=<id>` |
| **Source** | `tools/manycore-ui/app.py:192–199` |
| **Returns** | Artifact record for the named class (CRC, size, kind, path, existence flag) |
| **Required params** | `class` — internal class name (`com/example/Foo`); `loader_id` optional |
| **Agent B today** | ✗ — not in benchmark tool list |
| **harness_v2 exposes** | ✗ |
| **manycore-ui exposes** | ✓ |

**Response schema** (from `find_best_artifact()` in `indexer.py:384–423`):
```json
{
  "class":         "com/example/demo/engine/proc/Processor037",
  "loader_id":     "0x0000000100000001",
  "crc":           "a3f2b1c8",
  "size":          892,
  "kind":          "final",
  "load_kind":     "standard",
  "hidden":        false,
  "artifact_path": "/tmp/rt_ui_runs/abc12345/artifacts/com_example_demo_engine_proc_Processor037.class",
  "_exists":       true
}
```

`_exists` is set by `indexer.py:422` via `os.path.isfile(artifact_path)` at index time.

---

### 1.3 `/api/runs/<id>/causality/hidden` — hidden class identities

| Field | Value |
|-------|-------|
| **URL** | `GET /api/runs/<run_id>/causality/hidden` |
| **Source** | `tools/manycore-ui/app.py:843–873` |
| **Returns** | All hidden classes: runtime_name (+0x…), stable CRC, loader_id, has_artifact flag |
| **Agent B today** | ✗ — not in benchmark tool list (endpoint exists, just not exposed) |
| **harness_v2 exposes** | ✗ |
| **manycore-ui exposes** | ✓ |

**Response schema**:
```json
{
  "hidden_classes": [
    {
      "runtime_name": "com/example/App$$Lambda$1/0x0000000123456789",
      "artifact_crc":  "a3f1b2d4",
      "loader_id":     "0x00000001234abcde",
      "has_artifact":  true
    }
  ],
  "count":       1,
  "explanation": "Found 1 hidden class(es). Hidden class names include a hex suffix (+0x...) that changes every JVM run. The artifact_crc is stable across runs for the same bytecode. Use /causality/search with the base class name to find which callsite created or invoked each hidden class."
}
```

**Key diagnostic value**: For a lambda bug, `runtime_name` maps unstable name (`Lambda$1/0x…`) to
stable `artifact_crc`. Paired with `/api/runs/<id>/artifact`, this gives the `.class` path.
Then `/api/runs/<id>/bytecode` disassembles it. This three-step chain solves the lambda debugging
problem that makes Bug 3 (lambda comparator) currently unsolvable for Agent B.

---

### 1.4 `/api/runs/<id>/causality/chain` — full dispatch chain for a callsite

| Field | Value |
|-------|-------|
| **URL** | `GET /api/runs/<run_id>/causality/chain?class=<cls>&method=<m>&bci=<n>` |
| **Source** | `tools/manycore-ui/app.py:876–907` |
| **Returns** | Full causality chain from one BCI: callsite node + all target edges + adapter chain |
| **Required params** | `class`, `method`; `bci` and `desc` optional |
| **Agent B today** | ✗ — `causality_chain` is in the harness tool dispatch (`app.py:450`) but NOT in `TOOLS_B_EXTRA` in `harness_v2.py` |
| **harness_v2 exposes** | Partial — tool name recognized in dispatch but missing from tool description string and gate message |
| **manycore-ui exposes** | ✓ |

**Response schema**:
```json
{
  "callsite": "com/example/demo/engine/DispatchEngine.dispatch@bci=73",
  "chain": [
    {
      "type":  "callsite",
      "class": "com/example/demo/engine/DispatchEngine",
      "method": "dispatch",
      "bci":   73,
      "opcode": "invokevirtual",
      "static_label": "observed_single_target"
    },
    {
      "edge": "CALLSITE_TARGET",
      "id":   "method::com/example/demo/engine/proc/Processor037::<init>::()V::...",
      "attrs": {
        "target_class":      "com/example/demo/engine/proc/Processor037",
        "target_method":     "<init>",
        "target_descriptor": "()V",
        "evidence":          "OBSERVED_ONLY"
      }
    }
  ],
  "explanation": "Causality chain for DispatchEngine.dispatch@bci=73: 1 direct target edge. The reflection constructor call instantiated Processor037."
}
```

**Note on partial harness exposure**: `causality_chain` appears in the `elif tool_name in (...)` dispatch block (harness_v2.py:450) but is absent from `TOOLS_B_EXTRA` (the description string). The agent never learns it exists. This is a description gap, not a routing gap.

---

### 1.5 `/api/runs/<id>/download/artifacts` — bulk artifact zip download

| Field | Value |
|-------|-------|
| **URL** | `GET /api/runs/<run_id>/download/artifacts` |
| **Source** | `tools/manycore-ui/app.py:419–438` |
| **Returns** | ZIP archive of all `.class` files captured during the run |
| **Agent B today** | ✗ |
| **harness_v2 exposes** | ✗ |
| **manycore-ui exposes** | ✓ (download button in UI) |

Not useful for agent benchmark — bulk download, not targeted query.

---

### 1.6 `/api/runs/<id>/classes/<cls>/callsites` — callsites in a class

| Field | Value |
|-------|-------|
| **URL** | `GET /api/runs/<run_id>/classes/<class_name>/callsites` |
| **Source** | `tools/manycore-ui/app.py:163–177` |
| **Returns** | All callsite_summary records where `source_class == class_name` |
| **Agent B today** | ✗ |
| **harness_v2 exposes** | ✗ |
| **manycore-ui exposes** | ✓ |

Useful for "what did this class dispatch to?" but redundant with `causality_search`.

---

### 1.7 `/api/runs/<id>/causality/search` — search by class/method fragment

| Field | Value |
|-------|-------|
| **URL** | `GET /api/runs/<run_id>/causality/search?q=<fragment>` |
| **Source** | `tools/manycore-ui/app.py:693–734` |
| **Returns** | All callsites where source or target class/method contains the fragment |
| **Agent B today** | ✗ — not in benchmark tool list |
| **harness_v2 exposes** | ✗ |
| **manycore-ui exposes** | ✓ |

**Response schema**: same as the spec in `docs/11-demo-platform-design.md` (search response).
Useful for "find all callsites involving class X" without knowing exact BCI.

---

### 1.8 `/api/runs/<id>/causality/explain` — LLM-ready single-callsite explanation

| Field | Value |
|-------|-------|
| **URL** | `GET /api/runs/<run_id>/causality/explain?class=&method=&bci=` |
| **Source** | `tools/manycore-ui/app.py:910–977` |
| **Returns** | dispatch mechanism, polymorphism, targets, static_label, natural language explanation, full chain |
| **Agent B today** | ✗ — not in benchmark tool list |
| **harness_v2 exposes** | ✗ |
| **manycore-ui exposes** | ✓ |

The highest-value single endpoint for a debugging agent — synthesizes everything. Not exposed.

---

### 1.9 Bytecode on disk — verified capture

**Source**: `runner.py:123` sets `SOROUSH_BYTECODE_DUMP_DIR = str(run_dir / "artifacts")`.
**Source**: `runner.py:26` always sets `SOROUSH_CAPTURE_FINAL_BYTECODE = 1`.
**Source**: `klassFactory.cpp:309–346` — `capture_final_class_bytes()` writes `.class` file to disk.

This means: **every class loaded during a run already has its bytecode written to
`<run_dir>/artifacts/<sanitized_name>.class`**. The files exist. The javap API
(`/api/runs/<id>/bytecode`) serves them. The benchmark harness does not expose them.

---

## 2. Summary table

| Endpoint | Implemented | In graph | Serves artifacts | Agent B today | harness_v2 exposed | Gap |
|----------|:-----------:|:--------:|:----------------:|:------------:|:-----------------:|-----|
| `/api/runs/<id>/bytecode` | ✓ | N/A | ✓ (javap) | ✗ | ✗ | Highest priority |
| `/api/runs/<id>/artifact` | ✓ | ✓ | ✓ (metadata) | ✗ | ✗ | High priority |
| `/causality/hidden` | ✓ | ✓ | ✓ (CRC) | ✗ | ✗ | High priority |
| `/causality/chain` | ✓ | ✓ | indirect | ✗ | Dispatch only | Description gap only |
| `/causality/search` | ✓ | ✓ | indirect | ✗ | ✗ | Medium priority |
| `/causality/explain` | ✓ | ✓ | indirect | ✗ | ✗ | Medium priority |
| `/download/artifacts` | ✓ | N/A | ✓ (bulk zip) | ✗ | ✗ | Not useful for agent |
| `/classes/<cls>/callsites` | ✓ | ✓ | indirect | ✗ | ✗ | Redundant with search |

**Verdict**: 0 of the 6 high/medium-value endpoints are exposed to Agent B in the benchmark.
The bytecode and hidden class endpoints are the most immediately valuable.

---

## Section A — Richest runtime artifact information currently available but inaccessible to Agent B

**Rank 1: javap output** (`/api/runs/<id>/bytecode`)

For any class loaded during the run — including every one of the 50 Processors, any CGLIB proxy,
any ByteBuddy-generated class, any lambda hidden class — the JVM has already written the `.class`
file to `<run_dir>/artifacts/`. The javap endpoint serves it on demand with full verbose output
including constant pool values, flags, and per-instruction operands.

For the V3 bug, this would show:
```
ConstantValue: double 0.05d
```
directly in `Processor037`'s class metadata — the bug is visible at a glance without reading
source code or simulating a hash.

For any generated class (which has no `.java` source), javap output is the **only** way to
read the class body. Agent A cannot read generated classes at all; Agent B with javap access can.

**Rank 2: hidden class identity** (`/causality/hidden`)

Maps runtime names (`Lambda$1/0x…`, `HiddenClassTemplate+0x…`) to stable CRCs, which then
resolve to `artifact_path` entries. Without this, a lambda or hidden-class bug is structurally
unsolvable: the class name changes every JVM run, grep finds nothing, and there is no source line.

**Rank 3: causality chain** (`/causality/chain`)

Returns the full dispatch chain including all targets, adapter nodes, and edge types for one
callsite BCI. For a proxy bug, Agent B can traverse the full chain in one call instead of
building up the proxy path from multiple `/causality/proxies` and `/causality/polymorphic` calls.

---

## Section B — If we modify the harness today, can Agent B inspect:

| Capability | Answer | Condition |
|-----------|--------|-----------|
| Hidden class bytecode | ✓ Yes | `/causality/hidden` maps name→CRC; `/api/artifact` maps CRC→path; `/bytecode` runs javap |
| Generated ByteBuddy bytecode | ✓ Yes | ByteBuddy classes are captured as standard `bytecode_artifact` records; direct `/api/artifact?class=<name>` works |
| Dumped class artifacts | ✓ Yes | All `.class` files are in `<run_dir>/artifacts/` by default; `/bytecode?artifact_path=<path>` serves any of them |
| javap output | ✓ Yes | `/api/runs/<id>/bytecode?artifact_path=<path>` already calls `javap -c -p -verbose`; endpoint exists and works |

**All four capabilities are functional today.** The only change needed is harness integration.

---

## Section C — Exact tool calls Agent B would make

### For a regular class bug (V3, Processor037):
```
artifact_lookup({"class": "com/example/demo/engine/proc/Processor037"})
  → {crc: "a3f2b1c8", artifact_path: ".../Processor037.class", kind: "final", _exists: true}

artifact_javap({"class": "com/example/demo/engine/proc/Processor037"})
  → {output: "...ConstantValue: double 0.05d\n...ldc2_w #13 // double 0.05d...", exit_code: 0}
```

### For a lambda bug (future V4 scenario):
```
causality_hidden({})
  → {hidden_classes: [{runtime_name: "...Lambda$1/0x123...", artifact_crc: "a3f1b2d4"}]}

artifact_lookup({"class": "com/example/ProductService$$Lambda$1/0x0000000123456789"})
  → {crc: "a3f1b2d4", artifact_path: ".../ProductService_Lambda_1_crc...class", _exists: true}

artifact_javap({"class": "com/example/ProductService$$Lambda$1/0x0000000123456789"})
  → {output: "...ldc2_w #5 // double -1.0 (sign bug in comparator)...", exit_code: 0}
```

### For a proxy chain bug:
```
causality_proxies({})
  → proxy site: OrderController.checkout → Application$$SpringCGLIB$$0.saveOrder

causality_chain({"class": "com/example/OrderController", "method": "checkout", "bci": 15})
  → chain: CGLIB proxy → OrderTransactionInterceptor.intercept → OrderService.saveOrder
```

### For a generated ByteBuddy class bug:
```
causality_proxies({"class_filter": "com/example"})
  → site targeting: com/example/Service$ByteBuddy$xxx.method

artifact_lookup({"class": "com/example/Service$ByteBuddy$xxx"})
  → {artifact_path: ".../Service_ByteBuddy_xxx.class", _exists: true}

artifact_javap({"class": "com/example/Service$ByteBuddy$xxx"})
  → {output: "...wrong constant..."}
```

---

## Section D — Smallest implementation to make Agent B artifact-aware

**Three files. Zero new JVM work. Zero new endpoints. No new data capture.**

### Change 1: `tools/benchmark/harness_v2.py`

**a) Add to `TOOLS_B_EXTRA`** (4 new tool descriptions):
- `causality_hidden` — hidden class identity lookup
- `causality_chain` — full dispatch chain for one callsite
- `artifact_lookup` — class → CRC, artifact_path, kind
- `artifact_javap` — class → full javap disassembly

**b) Make `tool_causality()` section-aware**: parse `=== /<section> ===` headers in the
pre-captured CAUSALITY string, return only the section matching the requested tool name.
This prevents returning a giant combined string when the agent asks for javap output.

**c) Update gate message** to include new tools in the required-first-action description.

**d) Update tool dispatch `elif`** to include new tool names.

### Change 2: `tools/benchmark/bugs_v3.py`

Add four new sections to `CAUSALITY_BUG1V3`:
```
=== /causality/hidden ===
{ ... }

=== /causality/chain (DispatchEngine.dispatch bci=73) ===
{ ... }

=== /api/artifact (com/example/demo/engine/proc/Processor037) ===
{ ... }

=== /javap (com/example/demo/engine/proc/Processor037) ===
{ ... }
```

These are pre-captured representative responses, consistent with actual JVM output format.

### What this enables

After the change, Agent B's V3 investigation becomes:
```
Turn 1: causality_reflection → Processor037 identified (existing)
Turn 2: artifact_javap(Processor037) → see 0.05d constant directly
Turn 3: submit_diagnosis (exact file + line + patch)
```

Or even more directly (skipping reflection if agent goes to artifact first):
```
Turn 1: causality_reflection → Processor037
Turn 2: artifact_javap → ConstantValue: double 0.05d at line 7 confirmed
Turn 3: submit_diagnosis
```

Agent A still cannot read class files programmatically. The capability gap becomes structural
rather than just informational.

---

## Implementation follows in harness_v2.py and bugs_v3.py changes below.
