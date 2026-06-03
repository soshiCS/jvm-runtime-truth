# Runtime Truth Complete Eradication Audit

**Date:** 2026-06-01  
**Scope:** Every occurrence of `Runtime Truth` / `rt` / `rt` remaining in the repository after the doc/42 branding pass.  
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
| *(filesystem)* | — | `tools/rt-ui/` | directory name | **No** | All Python `import` calls from `harness.py` use `cwd=RT_UI`. `app.py` loads `indexer`, `runner`, `graph_builder` from this directory. Renaming requires: (1) `git mv`, (2) update `RT_UI` in `harness.py`, (3) update all `cd tools/rt-ui` in docs and scripts, (4) update all path references in 12+ docs. **Risk: High** |

### 1.2 Environment Variable Name

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| `tools/rt-ui/app.py` | 3 | `set RT_DEMO_TOKEN for HTTP Basic Auth gating` | docstring | Yes | Comment only — rename the mention here when renaming the var |
| `tools/rt-ui/app.py` | 33 | `# Optional demo token — set RT_DEMO_TOKEN env var to require Basic Auth.` | comment | Yes | Same |
| `tools/rt-ui/app.py` | 35 | `_DEMO_TOKEN = os.environ.get("RT_DEMO_TOKEN", "")` | env var read | **No** | Functional: reads from env. Renaming silently breaks any deployed instance that has the old name set. Must rename atomically in app.py + start_demo.sh + harness.py + docs. **Risk: Medium** |
| `tools/rt-ui/start_demo.sh` | 7 | `TOKEN="${RT_DEMO_TOKEN:-}"` | env var read | **No** | Same concern — functional |
| `tools/rt-ui/start_demo.sh` | 13 | `export RT_DEMO_TOKEN="$TOKEN"` | env var set | **No** | Same |
| `tools/benchmark/harness.py` | 424 | `env.pop("RT_DEMO_TOKEN", None)` | env var delete | **No** | Same — must rename together |
| `docs/09-running-tests-and-demos.md` | 190 | `$RT_DEMO_TOKEN if already set` | documentation | Yes | Doc reference; update when renaming var |
| `docs/09-running-tests-and-demos.md` | 213 | `RT_DEMO_TOKEN=mytoken ./start_demo.sh` | documentation | Yes | Command example; update when renaming var |

**To rename `RT_DEMO_TOKEN`:** Change in 3 code files atomically (`app.py:35`, `start_demo.sh:7,13`, `harness.py:424`), then update docs. Any user who has exported the old name in their shell profile must update it. **Risk: Medium**

### 1.3 Python Variable (Path Constant)

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| `tools/benchmark/harness.py` | 40 | `RT_UI = REPO_ROOT / "tools/rt-ui"` | Python constant | Yes (paired) | Local variable. Can rename to `RT_UI` freely — but must also rename the directory it points to, or the path becomes wrong. **Risk: Low** (paired with directory rename) |
| `tools/benchmark/harness.py` | 427 | `cwd=RT_UI,` | Python variable use | Yes (paired) | Uses the constant above; rename together |
| `tools/benchmark/harness.py` | 403 | `# ─── rt-ui lifecycle ──────────` | comment | Yes | Section comment; rename freely |
| `tools/benchmark/harness.py` | 408 | `"""Start a fresh rt-ui on UI_PORT and ingest the demo run.` | docstring | Yes | Descriptive; rename freely |
| `tools/benchmark/harness.py` | 422 | `print(f"  Starting rt-ui on port {UI_PORT}...")` | log message | Yes | Developer-facing print; rename freely |
| `tools/benchmark/harness.py` | 443 | `print("  ERROR: rt-ui did not start in time")` | log message | Yes | Rename freely |
| `tools/benchmark/harness.py` | 479 | `help="Use existing rt-ui run_id instead of starting a new server"` | CLI help text | Yes | argparse description; rename freely |
| `tools/benchmark/harness.py` | 487 | `# Start rt-ui if needed` | comment | Yes | Rename freely |
| `tools/benchmark/harness.py` | 495 | `sys.exit("Failed to start rt-ui")` | error message | Yes | Rename freely |

### 1.4 Filesystem Paths in Code

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| `tools/rt-ui/runner.py` | 12 | `RUN_BASE = Path("/tmp/rt_ui_runs")` | path constant | **No** | This path is where ALL run data is written. If renamed here but not in `app.py:985` and `start_demo.sh:30`, the server creates one dir and reads from another. Must rename all three simultaneously. **Risk: Medium** |
| `tools/rt-ui/app.py` | 985 | `os.makedirs("/tmp/rt_ui_runs", exist_ok=True)` | path | **No** | Same — must rename together with `runner.py:12` |
| `tools/rt-ui/start_demo.sh` | 30 | `mkdir -p /tmp/rt_ui_runs` | path | **No** | Same |
| `tools/rt-ui/start_demo.sh` | 33 | `/tmp/rt_flask.log` | log file path | Yes | Internal log path; rename freely. Update mention on line 82 in same file. |
| `tools/rt-ui/start_demo.sh` | 54 | `CF_LOG=/tmp/rt_cloudflare.log` | log file path | Yes | Same — rename freely with line 75, 83 |
| `tools/rt-ui/start_demo.sh` | 75 | `check /tmp/rt_cloudflare.log` | path in echo | Yes | Rename freely with line 54 |
| `tools/rt-ui/start_demo.sh` | 82 | `/tmp/rt_flask.log` | path in echo | Yes | Rename freely with line 33 |
| `tools/rt-ui/start_demo.sh` | 83 | `/tmp/rt_cloudflare.log` | path in echo | Yes | Rename freely with lines 54, 75 |

### 1.5 Artifact Filename

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| `tools/rt-ui/app.py` | 486 | `filename=rt_run_{run_id}.zip` | download filename | Yes | The name of the ZIP file users download. No code reads it back by name — it's just the `Content-Disposition` header value. Safe to rename to `rt_run_{run_id}.zip`. **Risk: Low** |

### 1.6 Java Class and Package Names

| File | Line | Exact Text | Category | Safe? | Why / Risk |
|---|---|---|---|---|---|
| *(Desktop, not in repo)* | — | `testcases/Runtime TruthCasesMain.java` | Java class | **No** | Source lives at `/Users/soroushaghajani/Desktop/rt-cases-src/src/testcases/`. This is **outside the repo**. Renaming requires: rewrite all 15 Java source files, change the package declaration, recompile, and rebuild the `/tmp/cases-build/` directory. Then update all validation commands in docs/00, docs/05, docs/07, docs/09. The program output `test cases demo complete — 15/15 passed` would also change. **Risk: High** |
| `docs/05-validation-guide.md` | 46 | `testcases.TestCasesMain` | Java class reference | **No** | Validation command — must match actual compiled class |
| `docs/05-validation-guide.md` | 93 | `.startswith('testcases')` | Python filter string | **No** | Matches against actual emitted `source_class` values (JVM internal class name format `testcases/Case...`). If Java package is renamed, this filter must change too |
| `docs/05-validation-guide.md` | 174 | `.startswith('testcases/Case0')` | Python filter string | **No** | Same |
| `docs/09-running-tests-and-demos.md` | 40 | `testcases.TestCasesMain` | Java class reference | **No** | Validation command |
| `docs/09-running-tests-and-demos.md` | 42 | `testcases` | user prefix filter | **No** | Entered in the UI — must match actual Java package |
| `docs/09-running-tests-and-demos.md` | 48 | `entries starting with \`testcases\`` | filter description | **No** | Describes actual class name prefix |
| `docs/09-running-tests-and-demos.md` | 126 | `.startswith('testcases')` | Python filter string | **No** | Same concern |
| `docs/00-agent-handoff.md` | 87 | `/tmp/cases-build/src/testcases/*.java` | path + package | **No** | Build command — must match actual source location |
| `docs/00-agent-handoff.md` | 93 | `testcases.TestCasesMain` | Java class reference | **No** | Run command |
| `docs/07-build-workflow-guide.md` | 125 | `/tmp/cases-build/src/testcases/*.java` | path + package | **No** | Build command |
| `docs/07-build-workflow-guide.md` | 136 | `testcases.TestCasesMain` | Java class reference | **No** | Run command |
| `docs/03-source-ownership-map.md` | 328 | `testcases.TestCasesMain` | Java class reference | **No** | Documentation of actual class name |
| `docs/10-phase2b-runtime-target-attribution-design.md` | 484 | `testcases.TestCasesMain` | Java class reference | **No** | Command |

### 1.7 Validation Paths (temp filesystem, referenced in code and docs)

These paths appear in docs as runnable commands. They match the directories created during actual validation runs.

| Path | Files Referencing It | Safe to Rename? | Risk |
|---|---|---|---|
| `/tmp/cases-build/` | docs/00, 05, 07, 09, 10 | Yes (docs only) | Low — just documentation; but must be consistent across all docs and the actual commands people run |
| `/tmp/rt_val.jsonl` | docs/00, 05, 08, 10 | Yes (docs only) | Low — output filename in commands; rename all together |
| `/tmp/rt_out.jsonl` | docs/07, 09 | Yes (docs only) | Low |
| `/tmp/rt_phase2b.jsonl` | docs/10 | Yes (docs only) | Low |

### 1.8 Benchmark Test Data Paths (embedded in Python source)

These paths appear in `bugs_v*.py` as hardcoded test data that simulates what the UI would return. They embed the `/tmp/rt_ui_runs/` path.

| File | Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|---|
| `tools/benchmark/bugs_v2.py` | 90 | `# This is what the Runtime Truth JVM would produce for /bug/reflection-v2` | comment | Yes | Update comment; no functional impact |
| `tools/benchmark/bugs_v3.py` | 62 | `# This is what the Runtime Truth JVM produces when the request runs under instrumentation.` | comment | Yes | Same |
| `tools/benchmark/bugs_v3.py` | 161 | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/...` | test data path | **No** | This string is sent to the LLM as simulated API output. If `rt_ui_runs` is renamed in `runner.py`, this must match — otherwise the benchmark simulates paths that don't exist on disk. **Risk: Medium** |
| `tools/benchmark/bugs_v3.py` | 168 | same as above | test data path | **No** | Same |
| `tools/benchmark/bugs_v4.py` | 74 | `# This is what the Runtime Truth JVM produces when the request runs under instrumentation.` | comment | Yes | Rename freely |
| `tools/benchmark/bugs_v4.py` | 173 | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/...` | test data path | **No** | Same as bugs_v3 concern |
| `tools/benchmark/bugs_v4.py` | 180 | same | test data path | **No** | Same |
| `tools/benchmark/bugs_v5.py` | 5 | `public demo for the Runtime Truth platform.` | docstring | Yes | Rename freely |
| `tools/benchmark/bugs_v5.py` | 95 | `# This is what the Runtime Truth JVM produces when the request runs under instrumentation.` | comment | Yes | Rename freely |
| `tools/benchmark/bugs_v5.py` | 228 | `"artifact_path": "/tmp/rt_ui_runs/demo/artifacts/...` | test data path | **No** | Same concern |
| `tools/benchmark/bugs_v5.py` | 235 | same | test data path | **No** | Same |

---

## Section 2: Documentation Occurrences

These are all in human-readable markdown. No code depends on their exact text. All are **safe to rename** unless they contain a command or path that must match actual filesystem state — those are flagged.

### 2.1 `docs/00-agent-handoff.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 13 | `"Runtime Truth JVM" because the original motivation was to model...` | historical reference | Yes | Intentional historical explanation. Changing to "Runtime Truth" here is accurate but loses the name-origin context. Low risk. |
| 21 | `All 12 synthetic Runtime Truth test cases pass` | documentation | Yes | Update to "RT test cases" |
| 23 | `Runtime Truth 11/11 checks` | documentation | Yes | Update to "RT workload" |
| 24 | `Runtime Truth 915→0 orphans` | documentation | Yes | "Runtime Truth" here labels the test workload; rename to "RT test suite" |
| 42 | `tools/rt-ui/` (path reference in text) | path | No | Must stay accurate to actual directory name |
| 80 | `### 12-case test suite` | doc heading | Yes | Rename to "RT test suite" |
| 85–87 | `/tmp/cases-build/...` paths | path in commands | No | Commands must match actual temp dir names |
| 87 | `testcases/*.java` | package path | No | Must match actual Java package |
| 91 | `/tmp/rt_val.jsonl` | path in command | No | Output path in command; rename consistently |
| 93 | `testcases.TestCasesMain` | Java class | No | Must match actual class |
| 96 | `test cases demo complete — 12/12 passed` | expected output | No | Literal program output; can only change if Java source is renamed |
| 180 | `Runtime Truth 915 → 0 orphans` | doc metric | Yes | Rename label |

### 2.2 `docs/README.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 49 | `All 12 test cases + Spring Boot` | documentation | Yes | Low |
| 53 | `Runtime Truth UI (local + remote demo)` | documentation | Yes | Low |
| 62 | `12/12 synthetic test cases` | documentation | Yes | Low |
| 65 | `Runtime Truth 915→0 orphans` | documentation | Yes | Low |
| 82 | `**Validate (Runtime Truth):**` | doc heading | Yes | Low |
| 85 | `/tmp/rt_val.jsonl` | path in command | No | Must be consistent across all docs |
| 87 | `testcases.TestCasesMain` | Java class | No | Must match actual class |
| 90 | `test cases demo complete — 12/12 passed` | expected output | No | Literal program output |

### 2.3 `docs/AGENT_NAVIGATOR.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 18 | `tools/rt-ui/graph_builder.py` | path | No | Must match directory |
| 18 | `Runtime Truth + Spring Boot validation` | documentation | Yes | Low |
| 21 | `Runtime Truth: 915→0 orphans` | documentation | Yes | Low |
| 31 | `14/14 test cases pass` | documentation | Yes | Low |
| 39 | `15/15 test cases pass` | documentation | Yes | Low |
| 66 | `Runtime Truth test suite` (table cell) | documentation | Yes | Low |
| 135 | `"Runtime Truth UI" section` | section reference | Yes | Low |
| 136 | `tools/rt-ui/README.md` | path | No | Must match directory |
| 137 | `tools/rt-ui/app.py`, `tools/rt-ui/indexer.py` | paths | No | Must match directory |
| 144 | `tools/rt-ui/app.py` | path | No | Must match directory |
| 146 | `requires rt-ui restart` | documentation | Yes | Low — refers to tool by dir name; update with dir rename |
| 160–161 | `tools/rt-ui/graph_builder.py`, `test_graph_builder.py` | paths | No | Must match directory |
| 204 | `Runtime Truth UI, and start_demo.sh` | documentation | Yes | Low |
| 238–240 | `tools/rt-ui/app.py`, `indexer.py`, `index.html`, `style.css` | paths | No | Must match directory |
| 249 | `Runtime TruthCasesMain` | Java class | No | Must match actual class |
| 258 | `Runtime Truth 15 cases` | documentation | Yes | Low |

### 2.4 `docs/02-phase-history.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 67 | `tools/rt-ui/indexer.py` | path | No | Must match directory |
| 297 | `Runtime TruthCasesMain cases` | Java class reference | No | Describes test infrastructure |
| 354 | `tools/rt-ui/tests/test_graph_builder.py` | path | No | Must match directory |
| 360 | `Runtime Truth: 15/15 PASS` | documentation | Yes | Low |

### 2.5 `docs/03-source-ownership-map.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 153 | `Runtime Truth runs use the interpreter hooks` | documentation | Yes | Low |
| 197 | `## Runtime Truth UI` (section heading) | doc heading | Yes | Low |
| 200–203 | `tools/rt-ui/app.py` etc. | paths | No | Must match directory |
| 223–224 | `tools/rt-ui/graph_builder.py` etc. | paths | No | Must match directory |
| 307 | `**Runtime Truth**: 915 orphans → 0` | documentation | Yes | Low |
| 326–328 | `/tmp/cases-build/...`, `testcases.TestCasesMain` | paths + Java class | No | Must match actual |
| 350 | `Runtime TruthCasesMain.java` | Java class file | No | Describes actual file |

### 2.6 `docs/05-validation-guide.md`

**High density of path + Java class references.**

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 28 | `## Part 1: Runtime Truth 14-Case Suite` | doc heading | Yes | Low |
| 34,36–37 | `/tmp/cases-build/...` paths | commands | No | Must match actual temp dirs |
| 44,46 | `/tmp/rt_val.jsonl`, `testcases.TestCasesMain` | command | No | Must match actual |
| 53 | `test cases demo complete — 14/14 passed` | expected output | No | Literal program output |
| 62, 87, 142, 172, 188, 206, 224, 243, 269 | `/tmp/rt_val.jsonl` | path in Python snippets | No | Must be consistent |
| 93 | `.startswith('testcases')` | class filter string | No | Matches JVM-emitted class names |
| 114 | `testcases.TestCasesMain` | Java class | No | Must match actual |
| 174 | `.startswith('testcases/Case0')` | class filter | No | Matches actual JVM names |
| 257 | `testcases/Case13_InvokevirtualMono$Circle.describe` | Java class path | No | Actual emitted class name |
| 273 | `.startswith('testcases')` | class filter | No | Same |
| 283–284 | `testcases/Case14_...` | Java class paths | No | Actual emitted class names |
| 319 | `rt` (stream label) | configuration value | No | Config value used as a filter; must match what's actually configured |
| 509, 537, 543, 568, 591, 603 | `tools/rt-ui/...` paths | paths | No | Must match directory |

### 2.7 `docs/06-known-limitations.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 105 | `12-case test suite` | documentation | Yes | Low |
| 182 | `Runtime Truth: 915 orphans → 0` | documentation | Yes | Low |
| 298 | `15/15 test cases pass` | documentation | Yes | Low |

### 2.8 `docs/07-build-workflow-guide.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 26 | `/tmp/cases-build/` (table) | path | No | Must match actual |
| 117 | `## Runtime Truth Cases` | doc heading | Yes | Low |
| 123–125 | `/tmp/cases-build/...`, `testcases/*.java` | commands | No | Must match actual |
| 128 | `testcases/` | path/package | No | Must match actual |
| 134, 136 | `/tmp/rt_out.jsonl`, `testcases.TestCasesMain` | command | No | Must match actual |
| 164 | `## Runtime Truth UI` | doc heading | Yes | Low |
| 169 | `tools/rt-ui` | path in command | No | Must match directory |

### 2.9 `docs/08-phase2-causality-graph-design-review.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 11 | `tools/rt-ui/graph_builder.py` | path | No | Must match directory |
| 17 | `12-case validation` | documentation | Yes | Low |
| 34 | `**Runtime Truth results (2026-05-30):**` | doc heading | Yes | Low |
| 48, 50–51 | `tools/rt-ui/...` paths + `/tmp/rt_val.jsonl` | paths | No | Must match |
| 406 | `tools/rt-ui/graph_builder.py` | path | No | Must match |
| 414 | `12-case workload` | documentation | Yes | Low |
| 432 | `Runtime Truth: 915 orphans → 0` | documentation | Yes | Low |
| 488 | `# tools/rt-ui/graph_builder.py` | path comment | No | Must match directory |
| 497 | `12-case:` | documentation | Yes | Low |

### 2.10 `docs/09-running-tests-and-demos.md`

**Highest density of user-facing commands.**

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 13 | `Runtime Truth UI (local)` | table cell | Yes | Low |
| 19 | `## Method 1: Runtime Truth UI (Local)` | doc heading | Yes | Low |
| 30 | `cd .../tools/rt-ui` | command | No | Must match directory |
| 36 | `### Run the 12-case suite via UI` | doc heading | Yes | Low |
| 40–42 | `testcases.TestCasesMain`, `/tmp/cases-build/classes`, `testcases` prefix | commands/config | No | Must match actual class + paths |
| 47 | `test cases demo complete — 12/12 passed` | expected output | No | Literal program output |
| 48 | `entries starting with \`testcases\`` | class filter | No | Matches actual class names |
| 51 | `/tmp/cases-build/classes` | path | No | Must match actual |
| 97–99 | `/tmp/cases-build/...`, `testcases/*.java` | commands | No | Must match actual |
| 102 | `testcases/` | path | No | Must match actual |
| 104 | `### CLI: Run the 12-case suite` | doc heading | Yes | Low |
| 108–110 | `/tmp/rt_out.jsonl`, `testcases.TestCasesMain` | command | No | Must match actual |
| 116 | `test cases demo complete — 12/12 passed` | expected output | No | Literal program output |
| 123, 126 | `/tmp/rt_out.jsonl`, `.startswith('testcases')` | path + filter | No | Must match actual |
| 173 | `starts the Runtime Truth UI and exposes it` | documentation | Yes | Low |
| 185 | `cd .../tools/rt-ui` | command | No | Must match directory |
| 190 | `$RT_DEMO_TOKEN if already set` | env var reference | No | Must match actual var name |
| 198 | `Runtime Truth UI — Demo Ready` | expected output | No | Literal banner output from start_demo.sh (now changed to "Runtime Truth UI") — this doc is now STALE |
| 213 | `RT_DEMO_TOKEN=mytoken ./start_demo.sh` | command example | No | Must match env var name |
| 218–219 | `/tmp/rt_cloudflare.log`, `/tmp/rt_flask.log` | paths | No | Must match actual log paths |
| 245 | `/tmp/rt_out.jsonl` | path | No | Must match |
| 281 | `tools/rt-ui/README.md` | path | No | Must match directory |

**Note:** Line 198 is already stale — the banner was changed to "Runtime Truth UI" in doc/42 but this doc still shows the old text.

### 2.11 `docs/10-phase2b-runtime-target-attribution-design.md`

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 9 | `Runtime Truth 915 → 0 orphans` | documentation | Yes | Low |
| 22–23 | `tools/rt-ui/graph_builder.py`, `test_graph_builder.py` | paths | No | Must match |
| 25 | `11/11 checks pass on Runtime Truth and Spring Boot` | documentation | Yes | Low |
| 35 | `Runtime Truth (12-case suite) \| **915** \| 0` | table | Yes | Low |
| 95 | `5 (Runtime Truth)` | documentation | Yes | Low |
| 102 | `7 (Runtime Truth)` | documentation | Yes | Low |
| 112 | `903 (Runtime Truth)` | documentation | Yes | Low |
| 351 | `~915 (Runtime Truth) / ~2,905 (Spring Boot)` | documentation | Yes | Low |
| 383 | `**Runtime Truth (915 orphans):**` | doc heading | Yes | Low |
| 388 | `in test cases` | documentation | Yes | Low |
| 409 | `in Runtime Truth` | documentation | Yes | Low |
| 471 | `Runtime Truth: 915 orphans → ~23` | documentation | Yes | Low |
| 481 | `Run Runtime Truth suite` | doc heading | Yes | Low |
| 484–485 | `testcases.TestCasesMain`, `/tmp/rt_phase2b.jsonl` | command | No | Must match actual |
| 490 | `tools/rt-ui/graph_builder.py /tmp/rt_phase2b.jsonl` | command | No | Must match both |
| 500 | `/tmp/rt_phase2b.jsonl` | path | No | Must be consistent |
| 514 | `tools/rt-ui/tests/test_graph_builder.py` | path | No | Must match directory |
| 543 | `tools/rt-ui/graph_builder.py` | path | No | Must match directory |

### 2.12 `docs/11-demo-platform-design.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 10 | `The Runtime Truth JVM instrumentation is interpreter-complete` | documentation | Yes | Low — rename to "Runtime Truth JVM" |
| 97 | `in the Runtime Truth UI` | documentation | Yes | Low |
| 473 | `expose graph_builder.py as REST endpoints in rt-ui` | documentation | Yes | Low |
| 476 | `surface causality API results in the existing rt-ui frontend` | documentation | Yes | Low |
| 484 | `implemented as 8 new endpoints in \`tools/rt-ui/app.py\`` | path | No | Must match directory |
| 539 | `tools/rt-ui/app.py` | path | No | Must match directory |
| 557 | `Load the JSONL into rt-ui` | documentation | Yes | Low |

### 2.13 `docs/12-demo-bug-suite.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# Runtime Truth Causality — Demo Bug Suite` | doc title | Yes | Low |
| 3 | `Runtime Truth Runtime Causality platform` | documentation | Yes | Low |
| 158 | `ingest the export into a running rt-ui` | documentation | Yes | Low |
| 161–162 | `# Start rt-ui`, `cd tools/rt-ui && python app.py 5001` | command | No | Must match directory |
| 164 | `requires rt-ui restart` | documentation | Yes | Low |
| 177 | `A rt-ui restart is required` | documentation | Yes | Low |

### 2.14 `docs/13-agent-benchmark-design.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# Runtime Truth Agent Benchmark Design` | doc title | Yes | Low |
| 11 | `Runtime Truth causality API` | documentation | Yes | Low |
| 200 | `rt-ui instance on port 5002` | documentation | Yes | Low (describes directory by name) |

### 2.15 `docs/14-benchmark-results.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# Runtime Truth Agent Benchmark — Results` | doc title | Yes | Low |
| 45 | `captured from a live run under the Runtime Truth JVM` | documentation | Yes | Low |

### 2.16 `docs/15-benchmark-redesign.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# Runtime Truth Agent Benchmark — Redesign` | doc title | Yes | Low |
| 96 | `running under the Runtime Truth JVM` | documentation | Yes | Low |

### 2.17 `docs/16-v3-benchmark.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# Runtime Truth Agent Benchmark — V3: Strong Discovery Design` | doc title | Yes | Low |

### 2.18 `docs/17-agent-capability-audit.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 3 | `Runtime Truth JVM platform's captured information` | documentation | Yes | Low |
| 11–13 | `tools/rt-ui/app.py`, `indexer.py`, `runner.py` | paths | No | Must match directory |
| 20 | `Available in rt-ui` (table header) | documentation | Yes | Low |
| 103, 174, 223 | `rt-ui`, `Runtime Truth` references | documentation | Yes | Low |

### 2.19 `docs/18-artifact-exposure-audit.md`

All occurrences are either `tools/rt-ui/app.py` path references or `/tmp/rt_ui_runs/` example paths.

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 18, 23, 40, 56, 61, 73, 87, 91, 121, 126, 165, 169, 180, 184, 195, 199, 211, 215 | `tools/rt-ui/app.py` | path | No | Must match directory |
| 40, 73 | `/tmp/rt_ui_runs/abc12345/artifacts/...` | example path | No | Example values — must match the actual path scheme used by `runner.py` |
| `rt-ui exposes` column label | table | Yes | Low |

### 2.20 `docs/19-benchmark-v4-runtime-artifacts.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# Runtime Truth Agent Benchmark — V4: Runtime Artifact Inspection` | doc title | Yes | Low |

### 2.21 `docs/20-runtime-truth-demo.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 1 | `# Runtime Truth Flagship Demo — V5: Runtime Truth Decryption` | doc title | Yes | Low |
| 542 | `With the actual Spring Boot app running under Runtime Truth` | documentation | Yes | Low |
| 551 | `Deploy the Spring Boot app under Runtime Truth` | documentation | Yes | Low |

### 2.22 `docs/24-live-demo-validation.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 262 | `## Causality API Validation via Runtime Truth UI` | doc heading | Yes | Low |
| 267 | `# Start Runtime Truth UI` | comment in code block | Yes | Low |
| 268 | `cd tools/rt-ui` | command | No | Must match directory |
| 296 | `tools/rt-ui/indexer.py` | path | No | Must match directory |
| 300 | `sys.path.insert(0, "tools/rt-ui")` | path in code | No | Must match directory |

### 2.23 `docs/25-runtime-truth-vs-undo.md`

This doc uses "Runtime Truth" extensively as the name for this project in comparison tables vs Undo.

| Lines | Content | Category | Safe? | Risk |
|---|---|---|---|---|
| 105, 114 | `Runtime Truth path`, `Runtime Truth benchmark` | documentation | Yes | Low — rename to "RT path", "RT benchmark" |
| 139–156 | Multiple table cells: `Both see it; Runtime Truth is...`, `Runtime Truth advantage...`, `Runtime Truth's primary differentiator` | documentation | Yes | Low — all safe to rename to "RT" |
| 165–167 | `Runtime Truth records class/method...`, `Runtime Truth only sees...`, `Runtime Truth's live capture` | documentation | Yes | Low |
| 179, 181 | `Runtime Truth instruments at class load time`, `Runtime Truth captures all data` | documentation | Yes | Low |
| 271 | `designed to favor Runtime Truth` | documentation | Yes | Low |

### 2.24 `docs/29-spring-proxy-bypass-results.md`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 4 | `(Runtime Truth UI port 5002)` | documentation | Yes | Low — identifies which tool produced the run |

### 2.25 `docs/rt-ui/README.md` (inside the tool directory)

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 32 | `cd tools/rt-ui` | command | No | Must match directory |
| 54 | `cd tools/rt-ui && python3 app.py 5001` | command | No | Must match directory |
| 129 | `/tmp/rt_ui_runs/<run_id>/` | path | No | Must match `runner.py:12` |
| 178 | `/tmp/rt_ui_runs/` | path | No | Must match `runner.py:12` |

### 2.26 `tools/rt-ui/tests/test_graph_builder.py`

| Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| 5 | `python3 -m pytest tools/rt-ui/tests/ -v` | path in comment | No | Must match directory |
| 6 | `python3 tools/rt-ui/tests/test_graph_builder.py` | path in comment | No | Must match directory |

### 2.27 `docs/runtime-target-export/` (4 files)

These documents use "rt" (capitalized differently) to refer to the staticization research direction — a different concern from the UI product name.

| File | Line | Exact Text | Category | Safe? | Risk |
|---|---|---|---|---|
| `KNOWN_LIMITATIONS.md` | 25 | `rt staticization use cases` | documentation | Yes | Low — rename to "RT staticization" |
| `README.md` | 17 | `consumed by rt's staticization` | documentation | Yes | Low |
| `HIGH_LEVEL_CAPTURE_OVERVIEW.md` | 671 | `### What "done" means for rt / staticization scope` | doc heading | Yes | Low |
| `PRODUCTION_READINESS_AUDIT.md` | 4 | `Runtime target export / rt staticization branch` | documentation | Yes | Low |

---

## Section 3: Benchmark Result Archives (Immutable)

The following files are frozen LLM conversation archives. They contain `rt_ui_runs` as embedded paths in API responses. **Do not modify these.**

| File | Occurrences | Content | Safe? |
|---|---|---|---|
| `tools/benchmark/results/benchmark_result_v2.json` | 1 | `/tmp/rt_ui_runs/demo/artifacts/...` in LLM conversation | **No** — frozen archive |
| `tools/benchmark/results/v2_bug1v4_B_20260531_051830.json` | multiple | Same | **No** |
| `tools/benchmark/results/v2_bug1v4_B_20260531_060837.json` | multiple | Same | **No** |
| `tools/benchmark/results/v2_bug1v5_B_*.json` (7 files) | multiple each | Same | **No** |

---

## Section 4: Out-of-Repository Java Source

The Java test cases (`testcases` package) live at:
```
/Users/soroushaghajani/Desktop/rt-cases-src/src/testcases/
```

This directory is **outside the repo** and not tracked by git. It contains 15 `.java` files. The package name (`testcases`) and main class name (`Runtime TruthCasesMain`) are the root source of many downstream references throughout docs and validation scripts. The program banner `test cases demo complete — N/N passed` is emitted from `Runtime TruthCasesMain.java`.

Renaming this package is the **highest-risk single operation** in the entire audit — it invalidates every validation command in 6 docs and requires recompiling the test suite.

---

## Section 5: Stale References Introduced by Doc/42 Changes

The branding pass (doc/42) changed some strings in code but left docs that reference those strings pointing at the old text.

| Location | Stale Reference | Current Code/Script Value |
|---|---|---|
| `docs/09-running-tests-and-demos.md:198` | `Runtime Truth UI — Demo Ready` (in expected banner output) | `start_demo.sh` now prints `Runtime Truth UI — Demo Ready` |

---

## Section 6: Risk Summary

### High Risk (do not rename without a full coordinated pass)

| Item | Why |
|---|---|
| `tools/rt-ui/` directory | All Python imports, 12+ doc paths, test runner commands |
| `testcases` Java package | Outside repo; 6 docs depend on it; requires recompile; program output changes |
| `Runtime TruthCasesMain` Java class | Same as above |
| `testcases/` filter strings in validation scripts | Hardcoded against actual JVM-emitted class name prefix |

### Medium Risk (rename requires multi-file coordination)

| Item | Files Requiring Simultaneous Update |
|---|---|
| `RT_DEMO_TOKEN` env var | `app.py:35`, `start_demo.sh:7,13`, `harness.py:424`, `docs/09:190,213` |
| `/tmp/rt_ui_runs/` path | `runner.py:12`, `app.py:985`, `start_demo.sh:30`, `bugs_v*.py` artifact paths, 5+ docs |
| `/tmp/rt_flask.log`, `/tmp/rt_cloudflare.log` | `start_demo.sh:33,54,75,82,83`, `docs/09:218,219` |
| `bugs_v*.py` artifact paths | Must stay in sync with `runner.py:12` |

### Low Risk (safe to change in isolation)

All documentation prose labels, doc titles, section headings, developer-facing print/error messages in `harness.py`, the `RT_UI` Python variable (paired with directory rename), the `rt_run_{run_id}.zip` artifact filename, and all `tools/rt-ui/` path references in docs (change only after directory rename).

---

## Section 7: Complete Rename Execution Order

If a full eradication is desired, the safe order is:

1. **Rename the Java source** (out-of-repo): change `testcases` package to `rtcases`, rename `Runtime TruthCasesMain` to `RTCasesMain`, rebuild `/tmp/rt-cases-build/`. Update all filter strings in docs.
2. **Rename the directory**: `git mv tools/rt-ui tools/rt-ui`. Update `RT_UI` in `harness.py`, all `cd tools/rt-ui` commands in scripts and docs.
3. **Rename the env var**: `RT_DEMO_TOKEN` → `RT_DEMO_TOKEN` in `app.py`, `start_demo.sh`, `harness.py`, and `docs/09`.
4. **Rename the run base path**: `/tmp/rt_ui_runs` → `/tmp/rt_ui_runs` in `runner.py`, `app.py`, `start_demo.sh`, and all docs + `bugs_v*.py` artifact paths.
5. **Rename log paths**: `/tmp/rt_flask.log`, `/tmp/rt_cloudflare.log` in `start_demo.sh` and `docs/09`.
6. **Update all remaining doc prose**: section headings, table labels, descriptive text — all Low risk.
7. **Do not touch benchmark result archives** (`tools/benchmark/results/*.json`).
