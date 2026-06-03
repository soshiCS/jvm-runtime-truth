# Branding Refactor Audit

**Date:** 2026-06-01  
**Scope:** Replace product-facing "Runtime Truth" / "rt" / "rt" references with "Runtime Truth" / "Runtime Truth UI"  
**Validation:** 26/26 tests pass; all four Python modules import cleanly after changes.

---

## Occurrences Found

Total files containing a case-insensitive match for "rt": 55  
(including benchmark JSON result archives and `docs/runtime-target-export/` which are read-only historical data)

Actionable files reviewed: 35

---

## What Was Changed

### UI — product-facing labels

| File | Before | After |
|---|---|---|
| `tools/rt-ui/static/index.html` | `<title>Runtime Truth UI — Runtime Target Explorer</title>` | `<title>Runtime Truth UI — Runtime Target Explorer</title>` |
| `tools/rt-ui/static/index.html` | `🔬 Runtime Truth` (navbar logo) | `🔬 Runtime Truth` |
| `tools/rt-ui/static/app.js` | `/* Runtime Truth UI — frontend logic */` | `/* Runtime Truth UI — frontend logic */` |
| `tools/rt-ui/README.md` | `# Runtime Truth UI` | `# Runtime Truth UI` |

### Flask backend — product-facing strings

| File | Before | After |
|---|---|---|
| `tools/rt-ui/app.py` | docstring: `Runtime Truth UI — Flask backend.` | `Runtime Truth UI — Flask backend.` |
| `tools/rt-ui/app.py` | auth body: `Runtime Truth UI — authentication required` | `Runtime Truth UI — authentication required` |
| `tools/rt-ui/app.py` | auth realm: `Basic realm="Runtime Truth Demo"` | `Basic realm="Runtime Truth Demo"` |
| `tools/rt-ui/app.py` | export header: `# Runtime Truth JVM Run Export` | `# Runtime Truth Run Export` |
| `tools/rt-ui/app.py` | startup print: `Runtime Truth UI  →  http://localhost:{port}` | `Runtime Truth UI  →  http://localhost:{port}` |

### Shell scripts — user-visible output

| File | Before | After |
|---|---|---|
| `tools/rt-ui/start_demo.sh` | comment: `launch Runtime Truth UI + Cloudflare Tunnel` | `launch Runtime Truth UI + Cloudflare Tunnel` |
| `tools/rt-ui/start_demo.sh` | echo: `Starting Runtime Truth UI on port $PORT...` | `Starting Runtime Truth UI on port $PORT...` |
| `tools/rt-ui/start_demo.sh` | banner: `Runtime Truth UI — Demo Ready` | `Runtime Truth UI — Demo Ready` |
| `tools/demo-buggy-app/run_demo.sh` | comment: `Build and run the Runtime Truth Causality Demo` | `Build and run the Runtime Truth Causality Demo` |
| `tools/demo-buggy-app/run_demo.sh` | echo: `requires rt-ui running on port 5001` | `requires Runtime Truth UI running on port 5001` |

### Python backend — docstrings and module descriptions

| File | Before | After |
|---|---|---|
| `tools/rt-ui/graph_builder.py` | `Runtime Truth JVM instrumentation.` (docstring) | `Runtime Truth instrumentation.` |
| `tools/benchmark/bugs.py` | `Runtime Truth Agent Benchmark` (docstring) | `Runtime Truth Agent Benchmark` |
| `tools/benchmark/scorer.py` | `Runtime Truth Agent Benchmark` (docstring) | `Runtime Truth Agent Benchmark` |
| `tools/benchmark/harness.py` | `Runtime Truth Agent Benchmark Harness` (docstring) | `Runtime Truth Agent Benchmark Harness` |
| `tools/benchmark/harness.py` | `rt-ui running on localhost:5002` (docstring) | `Runtime Truth UI running on localhost:5002` |

### Maven POM descriptions

| File | Before | After |
|---|---|---|
| `tools/demo-buggy-app/pom.xml` | `Runtime Truth Causality Demo — ...` | `Runtime Truth Causality Demo — ...` |
| `tools/demo-runtime-truth/pom.xml` | `Runtime Truth Flagship Demo — ...` | `Runtime Truth Flagship Demo — ...` |
| `tools/demo-real-bugs/pom.xml` | `Runtime Truth Phase V7 — ...` | `Runtime Truth Phase V7 — ...` |

### Documentation headings

| File | Before | After |
|---|---|---|
| `docs/README.md` | `# Runtime Truth JVM Documentation` | `# Runtime Truth Documentation` |
| `docs/README.md` | `...Runtime Truth JVM provenance graph project` | `...Runtime Truth provenance graph project` |
| `docs/AGENT_NAVIGATOR.md` | `starting work on the Runtime Truth JVM project` | `starting work on the Runtime Truth project` |
| `docs/00-agent-handoff.md` | `# Agent Handoff: Runtime Truth JVM — Provenance Graph` | `# Agent Handoff: Runtime Truth — JVM Provenance Graph` |
| `docs/01-project-overview.md` | `# Project Overview: Runtime Truth JVM Provenance Graph` | `# Project Overview: Runtime Truth — JVM Provenance Graph` |

**Total changes: 30 occurrences across 18 files.**

---

## What Was Intentionally Kept

### Filesystem paths and env vars (functional — changing would break code)

| Reference | Reason kept |
|---|---|
| `tools/rt-ui/` directory name | All Python imports (`from indexer import ...`, `from runner import ...`) reference this path. Renaming requires a full directory move + import updates across harness.py and all callers. |
| `/tmp/rt_ui_runs/` | Filesystem path used in `runner.py`, `app.py`, `start_demo.sh`. Changing it would silently break run persistence for anyone with existing data there. |
| `/tmp/rt_flask.log`, `/tmp/rt_cloudflare.log` | Log file paths referenced in shell scripts and user instructions. |
| `RT_DEMO_TOKEN` env var | Functional env var name. Changing it is a breaking change for any deployed instance. |
| `RT_UI` Python variable in `harness.py` | Internal code variable pointing to the directory path. |
| `rt_run_{run_id}.zip` | Export artifact filename. Changing it would break existing download links. |
| `# ─── rt-ui lifecycle ───` comment in `harness.py` | Internal section divider comment, describes the directory. |

### Java test infrastructure (changing would break test harness)

| Reference | Reason kept |
|---|---|
| `testcases.TestCasesMain` | Java package and class name. Cannot rename without recompiling and updating all validation commands in docs/05. |
| `/tmp/cases-build/` | Build output directory for the Java test cases. |
| `test cases demo complete — 12/12 passed` | Literal output from `Runtime TruthCasesMain`. Not this project's text to change. |
| "12/12 test cases", "15/15 test cases" | References to the Java test suite by its actual name. |
| `harness.py` print: `Starting rt-ui on port {UI_PORT}...` | References directory name directly. Kept to avoid confusion between product label and directory. |

### Historical context (intentional narrative)

| Reference | Reason kept |
|---|---|
| `docs/00-agent-handoff.md` line 13: `"Runtime Truth JVM" because the original motivation was to model...` | Historical explanation of the name origin. This is accurate context, not branding. |
| Phase result counts in docs (e.g., `Runtime Truth 915→0 orphans`) | Measurement labels tied to specific test workloads. Changing them would make the numbers hard to correlate with validation output. |
| `tools/rt-ui/README.md` — path references to `tools/rt-ui` | The actual directory path. |

### Benchmark result files (immutable historical records)

All files under `tools/benchmark/results/` — JSON snapshots of past benchmark runs. Not changed.

### `docs/runtime-target-export/` docs

Design documents that were written when the project used the "Runtime Truth" name internally. Left unchanged as historical design records.

---

## Risky References Left Unchanged

The following references remain and warrant a future decision if the `tools/rt-ui/` directory is ever renamed:

1. **`tools/rt-ui/` directory** — the largest remaining footprint. All Python imports depend on the current directory name. A rename is safe to do in one pass but was out of scope for this cleanup.

2. **`RT_DEMO_TOKEN` env var** — if the demo is re-deployed with a new token name, this should be updated in `app.py`, `harness.py`, and `start_demo.sh` together.

3. **Phase history docs** (`docs/02-phase-history.md` and others) — contain "Runtime Truth" as a label for the test workload and project phase names. These are accurate historical records and were not changed.

---

## Validation

```
python3 -m pytest tools/rt-ui/tests/ -q
→ 26 passed in 0.01s

python3 -c "import app, graph_builder, indexer, runner; print('OK')"
→ All module imports OK
```

No import errors, no test regressions.
