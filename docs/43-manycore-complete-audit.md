# ManyCore Complete Eradication Audit

**Date:** 2026-06-01  
**Scope:** Every occurrence of `ManyCore` / `manycore` / `MANYCORE` remaining in the repository after the doc/42 branding pass.  
**Total occurrences audited:** 295 (across 50 files, excluding `docs/42-branding-refactor-audit.md` which is a record of previous work, and `tools/benchmark/results/*.json` which are frozen LLM conversation archives)  
**Status:** Inventory only — no changes made.

---

## How to Read This Document

Each occurrence is rated:
- **Risk: High** — touching this breaks imports, tests, or external interfaces
- **Risk: Medium** — touching this requires coordinated multi-file changes or affects external tooling
- **Risk: Low** — can be changed in isolation with no functional impact

The rename column answers: "If we were to do a complete rename, what would be needed?"

---

## Section 1: Code-Level Occurrences

### 1.1 Directory Name

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| *(filesystem)* | — | `tools/manycore-ui/` | directory name | **No** | All Python `import` calls from `harness.py` use `cwd=MANYCORE_UI`. `app.py` loads `indexer`, `runner`, `graph_builder` from this directory. Renaming requires: (1) `git mv`, (2) update `MANYCORE_UI` in `harness.py`, (3) update all `cd tools/manycore-ui` in docs and scripts, (4) update all path references in 12+ docs. **Risk: High** |

### 1.2 Environment Variable Name

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| `tools/manycore-ui/app.py` | 3 | `set MANYCORE_DEMO_TOKEN for HTTP Basic Auth gating` | docstring | Yes | Comment only — rename the mention here when renaming the var |
| `tools/manycore-ui/app.py` | 33 | `# Optional demo token — set MANYCORE_DEMO_TOKEN env var to require Basic Auth.` | comment | Yes | Same |
| `tools/manycore-ui/app.py` | 35 | `_DEMO_TOKEN = os.environ.get("MANYCORE_DEMO_TOKEN", "")` | env var read | **No** | Functional: reads from env. Renaming silently breaks any deployed instance that has the old name set. Must rename atomically in app.py + start_demo.sh + harness.py + docs. **Risk: Medium** |
| `tools/manycore-ui/start_demo.sh` | 7 | `TOKEN="${MANYCORE_DEMO_TOKEN:-}"` | env var read | **No** | Same concern — functional |
| `tools/manycore-ui/start_demo.sh` | 13 | `export MANYCORE_DEMO_TOKEN="$TOKEN"` | env var set | **No** | Same |
| `tools/benchmark/harness.py` | 424 | `env.pop("MANYCORE_DEMO_TOKEN", None)` | env var delete | **No** | Same — must rename together |
| `docs/09-running-tests-and-demos.md` | 190 | `$MANYCORE_DEMO_TOKEN if already set` | documentation | Yes | Doc reference; update when renaming var |
| `docs/09-running-tests-and-demos.md` | 213 | `MANYCORE_DEMO_TOKEN=mytoken ./start_demo.sh` | documentation | Yes | Command example; update when renaming var |

**To rename `MANYCORE_DEMO_TOKEN`:** Change in 3 code files atomically (`app.py:35`, `start_demo.sh:7,13`, `harness.py:424`), then update docs. Any user who has exported the old name in their shell profile must update it. **Risk: Medium**

### 1.3 Python Variable (Path Constant)

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| `tools/benchmark/harness.py` | 40 | `MANYCORE_UI = REPO_ROOT / "tools/manycore-ui"` | Python constant | Yes (paired) | Local variable. Can rename to `RT_UI` freely — but must also rename the directory it points to, or the path becomes wrong. **Risk: Low** (paired with directory rename) |
| `tools/benchmark/harness.py` | 427 | `cwd=MANYCORE_UI,` | Python variable use | Yes (paired) | Uses the constant above; rename together |
| `tools/benchmark/harness.py` | 403 | `# ─── manycore-ui lifecycle ──────────` | comment | Yes | Section comment; rename freely |
| `tools/benchmark/harness.py` | 408 | `"""Start a fresh manycore-ui on UI_PORT and ingest the demo run.` | docstring | Yes | Descriptive; rename freely |
| `tools/benchmark/harness.py` | 422 | `print(f"  Starting manycore-ui on port {UI_PORT}...")` | log message | Yes | Developer-facing print; rename freely |
| `tools/benchmark/harness.py` | 443 | `print("  ERROR: manycore-ui did not start in time")` | log message | Yes | Rename freely |
| `tools/benchmark/harness.py` | 479 | `help="Use existing manycore-ui run_id instead of starting a new server"` | CLI help text | Yes | argparse description; rename freely |
| `tools/benchmark/harness.py` | 487 | `# Start manycore-ui if needed` | comment | Yes | Rename freely |
| `tools/benchmark/harness.py` | 495 | `sys.exit("Failed to start manycore-ui")` | error message | Yes | Rename freely |

### 1.4 Filesystem Paths in Code

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| `tools/manycore-ui/runner.py` | 12 | `RUN_BASE = Path("/tmp/manycore_ui_runs")` | path constant | **No** | This path is where ALL run data is written. If renamed here but not in `app.py:985` and `start_demo.sh:30`, the server creates one dir and reads from another. Must rename all three simultaneously. **Risk: Medium** |
| `tools/manycore-ui/app.py` | 985 | `os.makedirs("/tmp/manycore_ui_runs", exist_ok=True)` | path | **No** | Same — must rename together with `runner.py:12` |
| `tools/manycore-ui/start_demo.sh` | 30 | `mkdir -p /tmp/manycore_ui_runs` | path | **No** | Same |
| `tools/manycore-ui/start_demo.sh` | 33 | `/tmp/manycore_flask.log` | log file path | Yes | Internal log path; rename freely. Update mention on line 82 in same file. |
| `tools/manycore-ui/start_demo.sh` | 54 | `CF_LOG=/tmp/manycore_cloudflare.log` | log file path | Yes | Same — rename freely with line 75, 83 |
| `tools/manycore-ui/start_demo.sh` | 75 | `check /tmp/manycore_cloudflare.log` | path in echo | Yes | Rename freely with line 54 |
| `tools/manycore-ui/start_demo.sh` | 82 | `/tmp/manycore_flask.log` | path in echo | Yes | Rename freely with line 33 |
| `tools/manycore-ui/start_demo.sh` | 83 | `/tmp/manycore_cloudflare.log` | path in echo | Yes | Rename freely with lines 54, 75 |

### 1.5 Artifact Filename

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| `tools/manycore-ui/app.py` | 486 | `filename=manycore_run_{run_id}.zip` | download filename | Yes | The name of the ZIP file users download. No code reads it back by name — it's just the `Content-Disposition` header value. Safe to rename to `rt_run_{run_id}.zip`. **Risk: Low** |

### 1.6 Java Class and Package Names

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| *(Desktop, not in repo)* | — | `manycorecases/ManyCoreCasesMain.java` | Java class | **No** | Source lives at `/Users/soroushaghajani/Desktop/manycore-cases-src/src/manycorecases/`. This is **outside the repo**. Renaming requires: rewrite all 15 Java source files, change the package declaration, recompile, and rebuild the `/tmp/manycore-cases-build/` directory. Then update all validation commands in docs/00, docs/05, docs/07, docs/09. The program output `ManyCore cases demo complete — 15/15 passed` would also change. **Risk: High** |
| `docs/05-validation-guide.md` | 46 | `manycorecases.ManyCoreCasesMain` | Java class reference | **No** | Validation command — must match actual compiled class |
| `docs/05-validation-guide.md` | 93 | `.startswith('manycorecases')` | Python filter string | **No** | Matches against actual emitted `source_class` values (JVM internal class name format `manycorecases/Case...`). If Java package is renamed, this filter must change too |
| `docs/05-validation-guide.md` | 174 | `.startswith('manycorecases/Case0')` | Python filter string | **No** | Same |
| `docs/09-running-tests-and-demos.md` | 40 | `manycorecases.ManyCoreCasesMain` | Java class reference | **No** | Validation command |
| `docs/09-running-tests-and-demos.md` | 42 | `manycorecases` | user prefix filter | **No** | Entered in the UI — must match actual Java package |
| `docs/09-running-tests-and-demos.md` | 48 | `entries starting with \`manycorecases\`` | filter description | **No** | Describes actual class name prefix |
| `docs/09-running-tests-and-demos.md` | 126 | `.startswith('manycorecases')` | Python filter string | **No** | Same concern |
| `docs/00-agent-handoff.md` | 87 | `/tmp/manycore-cases-build/src/manycorecases/*.java` | path + package | **No** | Build command — must match actual source location |
| `docs/00-agent-handoff.md` | 93 | `manycorecases.ManyCoreCasesMain` | Java class reference | **No** | Run command |
| `docs/07-build-workflow-guide.md` | 125 | `/tmp/manycore-cases-build/src/manycorecases/*.java` | path + package | **No** | Build command |
| `docs/07-build-workflow-guide.md` | 136 | `manycorecases.ManyCoreCasesMain` | Java class reference | **No** | Run command |
| `docs/03-source-ownership-map.md` | 328 | `manycorecases.ManyCoreCasesMain` | Java class reference | **No** | Documentation of actual class name |
| `docs/10-phase2b-runtime-target-attribution-design.md` | 484 | `manycorecases.ManyCoreCasesMain` | Java class reference | **No** | Command |

### 1.7 Validation Paths (temp filesystem, referenced in code and docs)

These paths appear in docs as runnable commands. They match the directories created during actual validation runs.

| Path | Files Referencing It | Safe to Rename? | Risk |
|---|---|---|---|
| `/tmp/manycore-cases-build/` | docs/00, 05, 07, 09, 10 | Yes (docs only) | Low — just documentation; but must be consistent across all docs and the actual commands people run |
| `/tmp/manycore_val.jsonl` | docs/00, 05, 08, 10 | Yes (docs only) | Low — output filename in commands; rename all together |
| `/tmp/manycore_out.jsonl` | docs/07, 09 | Yes (docs only) | Low |
| `/tmp/manycore_phase2b.jsonl` | docs/10 | Yes (docs only) | Low |

### 1.8 Benchmark Test Data Paths (embedded in Python source)

These paths appear in `bugs_v*.py` as hardcoded test data that simulates what the UI would return. They embed the `/tmp/manycore_ui_runs/` path.

| File | Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|---|
| `tools/benchmark/bugs_v2.py` | 90 | `# This is what the ManyCore JVM would produce for /bug/reflection-v2` | comment | Yes | Update comment; no functional impact |
| `tools/benchmark/bugs_v3.py` | 62 | `# This is what the ManyCore JVM produces when the request runs under instrumentation.` | comment | Yes | Same |
| `tools/benchmark/bugs_v3.py` | 161 | `"artifact_path": "/tmp/manycore_ui_runs/demo/artifacts/...` | test data path | **No** | This string is sent to the LLM as simulated API output. If `manycore_ui_runs` is renamed in `runner.py`, this must match — otherwise the benchmark simulates paths that don't exist on disk. **Risk: Medium** |
| `tools/benchmark/bugs_v3.py` | 168 | same as above | test data path | **No** | Same |
| `tools/benchmark/bugs_v4.py` | 74 | `# This is what the ManyCore JVM produces when the request runs under instrumentation.` | comment | Yes | Rename freely |
| `tools/benchmark/bugs_v4.py` | 173 | `"artifact_path": "/tmp/manycore_ui_runs/demo/artifacts/...` | test data path | **No** | Same as bugs_v3 concern |
| `tools/benchmark/bugs_v4.py` | 180 | same | test data path | **No** | Same |
| `tools/benchmark/bugs_v5.py` | 5 | `public demo for the ManyCore platform.` | docstring | Yes | Rename freely |
| `tools/benchmark/bugs_v5.py` | 95 | `# This is what the ManyCore JVM produces when the request runs under instrumentation.` | comment | Yes | Rename freely |
| `tools/benchmark/bugs_v5.py` | 228 | `"artifact_path": "/tmp/manycore_ui_runs/demo/artifacts/...` | test data path | **No** | Same concern |
| `tools/benchmark/bugs_v5.py` | 235 | same | test data path | **No** | Same |

---

## Section 2: Documentation Occurrences

These are all in human-readable markdown. No code depends on their exact text. All are **safe to rename** unless they contain a command or path that must match actual filesystem state — those are flagged.

### 2.1 `docs/00-agent-handoff.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 13 | `"ManyCore JVM" because the original motivation was to model...` | historical reference | Yes | Intentional historical explanation. Changing to "Runtime Truth" here is accurate but loses the name-origin context. Low risk. |
| 21 | `All 12 synthetic ManyCore test cases pass` | documentation | Yes | Update to "RT test cases" |
| 23 | `ManyCore 11/11 checks` | documentation | Yes | Update to "RT workload" |
| 24 | `ManyCore 915→0 orphans` | documentation | Yes | "ManyCore" here labels the test workload; rename to "RT test suite" |
| 42 | `tools/manycore-ui/` (path reference in text) | path | No | Must stay accurate to actual directory name |
| 80 | `### ManyCore 12-case test suite` | doc heading | Yes | Rename to "RT test suite" |
| 85–87 | `/tmp/manycore-cases-build/...` paths | path in commands | No | Commands must match actual temp dir names |
| 87 | `manycorecases/*.java` | package path | No | Must match actual Java package |
| 91 | `/tmp/manycore_val.jsonl` | path in command | No | Output path in command; rename consistently |
| 93 | `manycorecases.ManyCoreCasesMain` | Java class | No | Must match actual class |
| 96 | `ManyCore cases demo complete — 12/12 passed` | expected output | No | Literal program output; can only change if Java source is renamed |
| 180 | `ManyCore 915 → 0 orphans` | doc metric | Yes | Rename label |

### 2.2 `docs/README.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 49 | `All 12 ManyCore cases + Spring Boot` | documentation | Yes | Low |
| 53 | `ManyCore UI (local + remote demo)` | documentation | Yes | Low |
| 62 | `12/12 ManyCore synthetic cases` | documentation | Yes | Low |
| 65 | `ManyCore 915→0 orphans` | documentation | Yes | Low |
| 82 | `**Validate (ManyCore):**` | doc heading | Yes | Low |
| 85 | `/tmp/manycore_val.jsonl` | path in command | No | Must be consistent across all docs |
| 87 | `manycorecases.ManyCoreCasesMain` | Java class | No | Must match actual class |
| 90 | `ManyCore cases demo complete — 12/12 passed` | expected output | No | Literal program output |

### 2.3 `docs/AGENT_NAVIGATOR.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 18 | `tools/manycore-ui/graph_builder.py` | path | No | Must match directory |
| 18 | `ManyCore + Spring Boot validation` | documentation | Yes | Low |
| 21 | `ManyCore: 915→0 orphans` | documentation | Yes | Low |
| 31 | `14/14 ManyCore cases pass` | documentation | Yes | Low |
| 39 | `15/15 ManyCore cases pass` | documentation | Yes | Low |
| 66 | `ManyCore test suite` (table cell) | documentation | Yes | Low |
| 135 | `"ManyCore UI" section` | section reference | Yes | Low |
| 136 | `tools/manycore-ui/README.md` | path | No | Must match directory |
| 137 | `tools/manycore-ui/app.py`, `tools/manycore-ui/indexer.py` | paths | No | Must match directory |
| 144 | `tools/manycore-ui/app.py` | path | No | Must match directory |
| 146 | `requires manycore-ui restart` | documentation | Yes | Low — refers to tool by dir name; update with dir rename |
| 160–161 | `tools/manycore-ui/graph_builder.py`, `test_graph_builder.py` | paths | No | Must match directory |
| 204 | `ManyCore UI, and start_demo.sh` | documentation | Yes | Low |
| 238–240 | `tools/manycore-ui/app.py`, `indexer.py`, `index.html`, `style.css` | paths | No | Must match directory |
| 249 | `ManyCoreCasesMain` | Java class | No | Must match actual class |
| 258 | `ManyCore 15 cases` | documentation | Yes | Low |

### 2.4 `docs/02-phase-history.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 67 | `tools/manycore-ui/indexer.py` | path | No | Must match directory |
| 297 | `ManyCoreCasesMain cases` | Java class reference | No | Describes test infrastructure |
| 354 | `tools/manycore-ui/tests/test_graph_builder.py` | path | No | Must match directory |
| 360 | `ManyCore: 15/15 PASS` | documentation | Yes | Low |

### 2.5 `docs/03-source-ownership-map.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 153 | `ManyCore runs use the interpreter hooks` | documentation | Yes | Low |
| 197 | `## ManyCore UI` (section heading) | doc heading | Yes | Low |
| 200–203 | `tools/manycore-ui/app.py` etc. | paths | No | Must match directory |
| 223–224 | `tools/manycore-ui/graph_builder.py` etc. | paths | No | Must match directory |
| 307 | `**ManyCore**: 915 orphans → 0` | documentation | Yes | Low |
| 326–328 | `/tmp/manycore-cases-build/...`, `manycorecases.ManyCoreCasesMain` | paths + Java class | No | Must match actual |
| 350 | `ManyCoreCasesMain.java` | Java class file | No | Describes actual file |

### 2.6 `docs/05-validation-guide.md`

**High density of path + Java class references.**

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 28 | `## Part 1: ManyCore 14-Case Suite` | doc heading | Yes | Low |
| 34,36–37 | `/tmp/manycore-cases-build/...` paths | commands | No | Must match actual temp dirs |
| 44,46 | `/tmp/manycore_val.jsonl`, `manycorecases.ManyCoreCasesMain` | command | No | Must match actual |
| 53 | `ManyCore cases demo complete — 14/14 passed` | expected output | No | Literal program output |
| 62, 87, 142, 172, 188, 206, 224, 243, 269 | `/tmp/manycore_val.jsonl` | path in Python snippets | No | Must be consistent |
| 93 | `.startswith('manycorecases')` | class filter string | No | Matches JVM-emitted class names |
| 114 | `manycorecases.ManyCoreCasesMain` | Java class | No | Must match actual |
| 174 | `.startswith('manycorecases/Case0')` | class filter | No | Matches actual JVM names |
| 257 | `manycorecases/Case13_InvokevirtualMono$Circle.describe` | Java class path | No | Actual emitted class name |
| 273 | `.startswith('manycorecases')` | class filter | No | Same |
| 283–284 | `manycorecases/Case14_...` | Java class paths | No | Actual emitted class names |
| 319 | `MANYCORE` (stream label) | configuration value | No | Config value used as a filter; must match what's actually configured |
| 509, 537, 543, 568, 591, 603 | `tools/manycore-ui/...` paths | paths | No | Must match directory |

### 2.7 `docs/06-known-limitations.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 105 | `12-case ManyCore test suite` | documentation | Yes | Low |
| 182 | `ManyCore: 915 orphans → 0` | documentation | Yes | Low |
| 298 | `15/15 ManyCore cases pass` | documentation | Yes | Low |

### 2.8 `docs/07-build-workflow-guide.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 26 | `/tmp/manycore-cases-build/` (table) | path | No | Must match actual |
| 117 | `## ManyCore Cases` | doc heading | Yes | Low |
| 123–125 | `/tmp/manycore-cases-build/...`, `manycorecases/*.java` | commands | No | Must match actual |
| 128 | `manycorecases/` | path/package | No | Must match actual |
| 134, 136 | `/tmp/manycore_out.jsonl`, `manycorecases.ManyCoreCasesMain` | command | No | Must match actual |
| 164 | `## ManyCore UI` | doc heading | Yes | Low |
| 169 | `tools/manycore-ui` | path in command | No | Must match directory |

### 2.9 `docs/08-phase2-causality-graph-design-review.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 11 | `tools/manycore-ui/graph_builder.py` | path | No | Must match directory |
| 17 | `ManyCore 12-case validation` | documentation | Yes | Low |
| 34 | `**ManyCore results (2026-05-30):**` | doc heading | Yes | Low |
| 48, 50–51 | `tools/manycore-ui/...` paths + `/tmp/manycore_val.jsonl` | paths | No | Must match |
| 406 | `tools/manycore-ui/graph_builder.py` | path | No | Must match |
| 414 | `12-case ManyCore workload` | documentation | Yes | Low |
| 432 | `ManyCore: 915 orphans → 0` | documentation | Yes | Low |
| 488 | `# tools/manycore-ui/graph_builder.py` | path comment | No | Must match directory |
| 497 | `12-case ManyCore:` | documentation | Yes | Low |

### 2.10 `docs/09-running-tests-and-demos.md`

**Highest density of user-facing commands.**

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 13 | `ManyCore UI (local)` | table cell | Yes | Low |
| 19 | `## Method 1: ManyCore UI (Local)` | doc heading | Yes | Low |
| 30 | `cd .../tools/manycore-ui` | command | No | Must match directory |
| 36 | `### Run the 12-case ManyCore suite via UI` | doc heading | Yes | Low |
| 40–42 | `manycorecases.ManyCoreCasesMain`, `/tmp/manycore-cases-build/classes`, `manycorecases` prefix | commands/config | No | Must match actual class + paths |
| 47 | `ManyCore cases demo complete — 12/12 passed` | expected output | No | Literal program output |
| 48 | `entries starting with \`manycorecases\`` | class filter | No | Matches actual class names |
| 51 | `/tmp/manycore-cases-build/classes` | path | No | Must match actual |
| 97–99 | `/tmp/manycore-cases-build/...`, `manycorecases/*.java` | commands | No | Must match actual |
| 102 | `manycorecases/` | path | No | Must match actual |
| 104 | `### CLI: Run the 12-case ManyCore suite` | doc heading | Yes | Low |
| 108–110 | `/tmp/manycore_out.jsonl`, `manycorecases.ManyCoreCasesMain` | command | No | Must match actual |
| 116 | `ManyCore cases demo complete — 12/12 passed` | expected output | No | Literal program output |
| 123, 126 | `/tmp/manycore_out.jsonl`, `.startswith('manycorecases')` | path + filter | No | Must match actual |
| 173 | `starts the ManyCore UI and exposes it` | documentation | Yes | Low |
| 185 | `cd .../tools/manycore-ui` | command | No | Must match directory |
| 190 | `$MANYCORE_DEMO_TOKEN if already set` | env var reference | No | Must match actual var name |
| 198 | `ManyCore UI — Demo Ready` | expected output | No | Literal banner output from start_demo.sh (now changed to "Runtime Truth UI") — this doc is now STALE |
| 213 | `MANYCORE_DEMO_TOKEN=mytoken ./start_demo.sh` | command example | No | Must match env var name |
| 218–219 | `/tmp/manycore_cloudflare.log`, `/tmp/manycore_flask.log` | paths | No | Must match actual log paths |
| 245 | `/tmp/manycore_out.jsonl` | path | No | Must match |
| 281 | `tools/manycore-ui/README.md` | path | No | Must match directory |

**Note:** Line 198 is already stale — the banner was changed to "Runtime Truth UI" in doc/42 but this doc still shows the old text.

### 2.11 `docs/10-phase2b-runtime-target-attribution-design.md`

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 9 | `ManyCore 915 → 0 orphans` | documentation | Yes | Low |
| 22–23 | `tools/manycore-ui/graph_builder.py`, `test_graph_builder.py` | paths | No | Must match |
| 25 | `11/11 checks pass on ManyCore and Spring Boot` | documentation | Yes | Low |
| 35 | `ManyCore (12-case suite) \| **915** \| 0` | table | Yes | Low |
| 95 | `5 (ManyCore)` | documentation | Yes | Low |
| 102 | `7 (ManyCore)` | documentation | Yes | Low |
| 112 | `903 (ManyCore)` | documentation | Yes | Low |
| 351 | `~915 (ManyCore) / ~2,905 (Spring Boot)` | documentation | Yes | Low |
| 383 | `**ManyCore (915 orphans):**` | doc heading | Yes | Low |
| 388 | `in ManyCore cases` | documentation | Yes | Low |
| 409 | `in ManyCore` | documentation | Yes | Low |
| 471 | `ManyCore: 915 orphans → ~23` | documentation | Yes | Low |
| 481 | `Run ManyCore suite` | doc heading | Yes | Low |
| 484–485 | `manycorecases.ManyCoreCasesMain`, `/tmp/manycore_phase2b.jsonl` | command | No | Must match actual |
| 490 | `tools/manycore-ui/graph_builder.py /tmp/manycore_phase2b.jsonl` | command | No | Must match both |
| 500 | `/tmp/manycore_phase2b.jsonl` | path | No | Must be consistent |
| 514 | `tools/manycore-ui/tests/test_graph_builder.py` | path | No | Must match directory |
| 543 | `tools/manycore-ui/graph_builder.py` | path | No | Must match directory |

### 2.12 `docs/11-demo-platform-design.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 10 | `The ManyCore JVM instrumentation is interpreter-complete` | documentation | Yes | Low — rename to "Runtime Truth JVM" |
| 97 | `in the ManyCore UI` | documentation | Yes | Low |
| 473 | `expose graph_builder.py as REST endpoints in manycore-ui` | documentation | Yes | Low |
| 476 | `surface causality API results in the existing manycore-ui frontend` | documentation | Yes | Low |
| 484 | `implemented as 8 new endpoints in \`tools/manycore-ui/app.py\`` | path | No | Must match directory |
| 539 | `tools/manycore-ui/app.py` | path | No | Must match directory |
| 557 | `Load the JSONL into manycore-ui` | documentation | Yes | Low |

### 2.13 `docs/12-demo-bug-suite.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# ManyCore Causality — Demo Bug Suite` | doc title | Yes | Low |
| 3 | `ManyCore Runtime Causality platform` | documentation | Yes | Low |
| 158 | `ingest the export into a running manycore-ui` | documentation | Yes | Low |
| 161–162 | `# Start manycore-ui`, `cd tools/manycore-ui && python app.py 5001` | command | No | Must match directory |
| 164 | `requires manycore-ui restart` | documentation | Yes | Low |
| 177 | `A manycore-ui restart is required` | documentation | Yes | Low |

### 2.14 `docs/13-agent-benchmark-design.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# ManyCore Agent Benchmark Design` | doc title | Yes | Low |
| 11 | `ManyCore causality API` | documentation | Yes | Low |
| 200 | `manycore-ui instance on port 5002` | documentation | Yes | Low (describes directory by name) |

### 2.15 `docs/14-benchmark-results.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# ManyCore Agent Benchmark — Results` | doc title | Yes | Low |
| 45 | `captured from a live run under the ManyCore JVM` | documentation | Yes | Low |

### 2.16 `docs/15-benchmark-redesign.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# ManyCore Agent Benchmark — Redesign` | doc title | Yes | Low |
| 96 | `running under the ManyCore JVM` | documentation | Yes | Low |

### 2.17 `docs/16-v3-benchmark.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# ManyCore Agent Benchmark — V3: Strong Discovery Design` | doc title | Yes | Low |

### 2.18 `docs/17-agent-capability-audit.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 3 | `ManyCore JVM platform's captured information` | documentation | Yes | Low |
| 11–13 | `tools/manycore-ui/app.py`, `indexer.py`, `runner.py` | paths | No | Must match directory |
| 20 | `Available in manycore-ui` (table header) | documentation | Yes | Low |
| 103, 174, 223 | `manycore-ui`, `ManyCore` references | documentation | Yes | Low |

### 2.19 `docs/18-artifact-exposure-audit.md`

All occurrences are either `tools/manycore-ui/app.py` path references or `/tmp/manycore_ui_runs/` example paths.

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 18, 23, 40, 56, 61, 73, 87, 91, 121, 126, 165, 169, 180, 184, 195, 199, 211, 215 | `tools/manycore-ui/app.py` | path | No | Must match directory |
| 40, 73 | `/tmp/manycore_ui_runs/abc12345/artifacts/...` | example path | No | Example values — must match the actual path scheme used by `runner.py` |
| `manycore-ui exposes` column label | table | Yes | Low |

### 2.20 `docs/19-benchmark-v4-runtime-artifacts.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# ManyCore Agent Benchmark — V4: Runtime Artifact Inspection` | doc title | Yes | Low |

### 2.21 `docs/20-runtime-truth-demo.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# ManyCore Flagship Demo — V5: Runtime Truth Decryption` | doc title | Yes | Low |
| 542 | `With the actual Spring Boot app running under ManyCore` | documentation | Yes | Low |
| 551 | `Deploy the Spring Boot app under ManyCore` | documentation | Yes | Low |

### 2.22 `docs/24-live-demo-validation.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 262 | `## Causality API Validation via ManyCore UI` | doc heading | Yes | Low |
| 267 | `# Start ManyCore UI` | comment in code block | Yes | Low |
| 268 | `cd tools/manycore-ui` | command | No | Must match directory |
| 296 | `tools/manycore-ui/indexer.py` | path | No | Must match directory |
| 300 | `sys.path.insert(0, "tools/manycore-ui")` | path in code | No | Must match directory |

### 2.23 `docs/25-runtime-truth-vs-undo.md`

This doc uses "ManyCore" extensively as the name for this project in comparison tables vs Undo.

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 105, 114 | `ManyCore path`, `ManyCore benchmark` | documentation | Yes | Low — rename to "RT path", "RT benchmark" |
| 139–156 | Multiple table cells: `Both see it; ManyCore is...`, `ManyCore advantage...`, `ManyCore's primary differentiator` | documentation | Yes | Low — all safe to rename to "RT" |
| 165–167 | `ManyCore records class/method...`, `ManyCore only sees...`, `ManyCore's live capture` | documentation | Yes | Low |
| 179, 181 | `ManyCore instruments at class load time`, `ManyCore captures all data` | documentation | Yes | Low |
| 271 | `designed to favor ManyCore` | documentation | Yes | Low |

### 2.24 `docs/29-spring-proxy-bypass-results.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 4 | `(ManyCore UI port 5002)` | documentation | Yes | Low — identifies which tool produced the run |

### 2.25 `docs/manycore-ui/README.md` (inside the tool directory)

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 32 | `cd tools/manycore-ui` | command | No | Must match directory |
| 54 | `cd tools/manycore-ui && python3 app.py 5001` | command | No | Must match directory |
| 129 | `/tmp/manycore_ui_runs/<run_id>/` | path | No | Must match `runner.py:12` |
| 178 | `/tmp/manycore_ui_runs/` | path | No | Must match `runner.py:12` |

### 2.26 `tools/manycore-ui/tests/test_graph_builder.py`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 5 | `python3 -m pytest tools/manycore-ui/tests/ -v` | path in comment | No | Must match directory |
| 6 | `python3 tools/manycore-ui/tests/test_graph_builder.py` | path in comment | No | Must match directory |

### 2.27 `docs/runtime-target-export/` (4 files)

These documents use "Manycore" (capitalized differently) to refer to the staticization research direction — a different concern from the UI product name.

| File | Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| `KNOWN_LIMITATIONS.md` | 25 | `Manycore staticization use cases` | documentation | Yes | Low — rename to "RT staticization" |
| `README.md` | 17 | `consumed by Manycore's staticization` | documentation | Yes | Low |
| `HIGH_LEVEL_CAPTURE_OVERVIEW.md` | 671 | `### What "done" means for Manycore / staticization scope` | doc heading | Yes | Low |
| `PRODUCTION_READINESS_AUDIT.md` | 4 | `Runtime target export / Manycore staticization branch` | documentation | Yes | Low |

---

## Section 3: Benchmark Result Archives (Immutable)

The following files are frozen LLM conversation archives. They contain `manycore_ui_runs` as embedded paths in API responses. **Do not modify these.**

| File | Occurrences | Content | Safe? |
|---|---|---|---|
| `tools/benchmark/results/benchmark_result_v2.json` | 1 | `/tmp/manycore_ui_runs/demo/artifacts/...` in LLM conversation | **No** — frozen archive |
| `tools/benchmark/results/v2_bug1v4_B_20260531_051830.json` | multiple | Same | **No** |
| `tools/benchmark/results/v2_bug1v4_B_20260531_060837.json` | multiple | Same | **No** |
| `tools/benchmark/results/v2_bug1v5_B_*.json` (7 files) | multiple each | Same | **No** |

---

## Section 4: Out-of-Repository Java Source

The Java test cases (`manycorecases` package) live at:
```
/Users/soroushaghajani/Desktop/manycore-cases-src/src/manycorecases/
```

This directory is **outside the repo** and not tracked by git. It contains 15 `.java` files. The package name (`manycorecases`) and main class name (`ManyCoreCasesMain`) are the root source of many downstream references throughout docs and validation scripts. The program banner `ManyCore cases demo complete — N/N passed` is emitted from `ManyCoreCasesMain.java`.

Renaming this package is the **highest-risk single operation** in the entire audit — it invalidates every validation command in 6 docs and requires recompiling the test suite.

---

## Section 5: Stale References Introduced by Doc/42 Changes

The branding pass (doc/42) changed some strings in code but left docs that reference those strings pointing at the old text.

| Location | Stale Reference | Current Code/Script Value |
|---|---|---|
| `docs/09-running-tests-and-demos.md:198` | `ManyCore UI — Demo Ready` (in expected banner output) | `start_demo.sh` now prints `Runtime Truth UI — Demo Ready` |

---

## Section 6: Risk Summary

### High Risk (do not rename without a full coordinated pass)

| Item | Why |
|---|---|
| `tools/manycore-ui/` directory | All Python imports, 12+ doc paths, test runner commands |
| `manycorecases` Java package | Outside repo; 6 docs depend on it; requires recompile; program output changes |
| `ManyCoreCasesMain` Java class | Same as above |
| `manycorecases/` filter strings in validation scripts | Hardcoded against actual JVM-emitted class name prefix |

### Medium Risk (rename requires multi-file coordination)

| Item | Files Requiring Simultaneous Update |
|---|---|
| `MANYCORE_DEMO_TOKEN` env var | `app.py:35`, `start_demo.sh:7,13`, `harness.py:424`, `docs/09:190,213` |
| `/tmp/manycore_ui_runs/` path | `runner.py:12`, `app.py:985`, `start_demo.sh:30`, `bugs_v*.py` artifact paths, 5+ docs |
| `/tmp/manycore_flask.log`, `/tmp/manycore_cloudflare.log` | `start_demo.sh:33,54,75,82,83`, `docs/09:218,219` |
| `bugs_v*.py` artifact paths | Must stay in sync with `runner.py:12` |

### Low Risk (safe to change in isolation)

All documentation prose labels, doc titles, section headings, developer-facing print/error messages in `harness.py`, the `MANYCORE_UI` Python variable (paired with directory rename), the `manycore_run_{run_id}.zip` artifact filename, and all `tools/manycore-ui/` path references in docs (change only after directory rename).

---

## Section 7: Complete Rename Execution Order

If a full eradication is desired, the safe order is:

1. **Rename the Java source** (out-of-repo): change `manycorecases` package to `rtcases`, rename `ManyCoreCasesMain` to `RTCasesMain`, rebuild `/tmp/rt-cases-build/`. Update all filter strings in docs.
2. **Rename the directory**: `git mv tools/manycore-ui tools/rt-ui`. Update `MANYCORE_UI` in `harness.py`, all `cd tools/manycore-ui` commands in scripts and docs.
3. **Rename the env var**: `MANYCORE_DEMO_TOKEN` → `RT_DEMO_TOKEN` in `app.py`, `start_demo.sh`, `harness.py`, and `docs/09`.
4. **Rename the run base path**: `/tmp/manycore_ui_runs` → `/tmp/rt_ui_runs` in `runner.py`, `app.py`, `start_demo.sh`, and all docs + `bugs_v*.py` artifact paths.
5. **Rename log paths**: `/tmp/manycore_flask.log`, `/tmp/manycore_cloudflare.log` in `start_demo.sh` and `docs/09`.
6. **Update all remaining doc prose**: section headings, table labels, descriptive text — all Low risk.
7. **Do not touch benchmark result archives** (`tools/benchmark/results/*.json`).
