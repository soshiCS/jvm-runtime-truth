# Runtime Truth Eradication — Phase 1

**Date:** 2026-06-01  
**Scope:** Rename four medium-risk items per the audit in docs/43  
**Validation:** 26/26 tests pass. All four Python modules import cleanly. `start_demo.sh` passes `bash -n` syntax check.

---

## Renames Performed

| Old Name | New Name | Type |
|---|---|---|
| `RT_DEMO_TOKEN` | `RT_DEMO_TOKEN` | Environment variable |
| `/tmp/rt_ui_runs` | `/tmp/rt_ui_runs` | Filesystem path |
| `/tmp/rt_flask.log` | `/tmp/rt_flask.log` | Log file path |
| `/tmp/rt_cloudflare.log` | `/tmp/rt_cloudflare.log` | Log file path |

---

## Files Changed

### `tools/rt-ui/app.py` — 4 changes

| Line | Before | After |
|---|---|---|
| 3 | `set RT_DEMO_TOKEN for HTTP Basic Auth gating` | `set RT_DEMO_TOKEN for HTTP Basic Auth gating` |
| 33 | `# Optional demo token — set RT_DEMO_TOKEN env var` | `# Optional demo token — set RT_DEMO_TOKEN env var` |
| 35 | `os.environ.get("RT_DEMO_TOKEN", "")` | `os.environ.get("RT_DEMO_TOKEN", "")` |
| 985 | `os.makedirs("/tmp/rt_ui_runs", exist_ok=True)` | `os.makedirs("/tmp/rt_ui_runs", exist_ok=True)` |

### `tools/rt-ui/runner.py` — 1 change

| Line | Before | After |
|---|---|---|
| 12 | `RUN_BASE = Path("/tmp/rt_ui_runs")` | `RUN_BASE = Path("/tmp/rt_ui_runs")` |

### `tools/rt-ui/start_demo.sh` — 7 changes

| Line | Before | After |
|---|---|---|
| 7 | `TOKEN="${RT_DEMO_TOKEN:-}"` | `TOKEN="${RT_DEMO_TOKEN:-}"` |
| 13 | `export RT_DEMO_TOKEN="$TOKEN"` | `export RT_DEMO_TOKEN="$TOKEN"` |
| 30 | `mkdir -p /tmp/rt_ui_runs` | `mkdir -p /tmp/rt_ui_runs` |
| 33 | `python3 app.py "$PORT" > /tmp/rt_flask.log 2>&1 &` | `python3 app.py "$PORT" > /tmp/rt_flask.log 2>&1 &` |
| 54 | `CF_LOG=/tmp/rt_cloudflare.log` | `CF_LOG=/tmp/rt_cloudflare.log` |
| 75 | `check /tmp/rt_cloudflare.log` | `check /tmp/rt_cloudflare.log` |
| 82–83 | `rt_flask.log` / `rt_cloudflare.log` | `rt_flask.log` / `rt_cloudflare.log` |

### `tools/benchmark/harness.py` — 1 change

| Line | Before | After |
|---|---|---|
| 424 | `env.pop("RT_DEMO_TOKEN", None)` | `env.pop("RT_DEMO_TOKEN", None)` |

### `tools/benchmark/bugs_v3.py` — 2 changes

| Lines | Before | After |
|---|---|---|
| 161, 168 | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/..."` | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/..."` |

### `tools/benchmark/bugs_v4.py` — 2 changes

| Lines | Before | After |
|---|---|---|
| 173, 180 | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/..."` | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/..."` |

### `tools/benchmark/bugs_v5.py` — 2 changes

| Lines | Before | After |
|---|---|---|
| 228, 235 | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/..."` | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/..."` |

### `tools/rt-ui/README.md` — 2 changes

| Lines | Before | After |
|---|---|---|
| 129, 178 | `/tmp/rt_ui_runs/` | `/tmp/rt_ui_runs/` |

### `docs/09-running-tests-and-demos.md` — 5 changes

| Line | Before | After |
|---|---|---|
| 190 | `` `$RT_DEMO_TOKEN` if already set `` | `` `$RT_DEMO_TOKEN` if already set `` |
| 198 | `Runtime Truth UI — Demo Ready` (banner) | `Runtime Truth UI — Demo Ready` (stale fix) |
| 213 | `RT_DEMO_TOKEN=mytoken ./start_demo.sh` | `RT_DEMO_TOKEN=mytoken ./start_demo.sh` |
| 218 | `/tmp/rt_cloudflare.log` | `/tmp/rt_cloudflare.log` |
| 219 | `/tmp/rt_flask.log` | `/tmp/rt_flask.log` |

### `docs/18-artifact-exposure-audit.md` — 2 changes

| Lines | Before | After |
|---|---|---|
| 40, 73 | `/tmp/rt_ui_runs/abc12345/artifacts/...` | `/tmp/rt_ui_runs/abc12345/artifacts/...` |

---

## Total

**28 line changes across 10 files.**

---

## Validation Results

```
python3 -m pytest tools/rt-ui/tests/ -q
→ 26 passed in 0.01s

python3 -c "import app, graph_builder, indexer, runner; print('imports OK')"
→ imports OK

bash -n tools/rt-ui/start_demo.sh
→ start_demo.sh syntax OK
```

Residual scan of all four renamed items across `*.py`, `*.sh`, `*.md`, `*.html`, `*.js`, `*.xml`:
→ **0 occurrences** (excluding frozen benchmark result archives and this document)

---

## Not Touched (Phase 1 Boundary)

Per the phase specification:

| Item | Status |
|---|---|
| `tools/rt-ui/` directory name | Deferred — Phase 2 |
| `testcases` Java package | Deferred — Phase 3 (requires out-of-repo Java source rename + recompile) |
| `Runtime TruthCasesMain` Java class | Deferred — Phase 3 |
| `tools/benchmark/results/*.json` frozen archives | Permanent — do not modify |

---

## Remaining Scope

After Phase 1, the remaining Runtime Truth references fall into three categories:

1. **`tools/rt-ui/` directory** — the largest footprint. Requires `git mv`, updating `RT_UI` constant in `harness.py`, and all `cd tools/rt-ui` commands in 10+ docs and scripts.

2. **Java test infrastructure** (`testcases`, `Runtime TruthCasesMain`) — requires modifying and recompiling sources outside the repo, then updating all validation commands.

3. **Documentation prose** — section headings, table labels, descriptive text that uses "Runtime Truth" as a label. ~200 occurrences spread across 25 docs. All Low risk; can be done in a single pass once the directory and Java renames are complete.
