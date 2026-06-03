# Professional Refactor Results

**Date:** 2026-06-02  
**Status:** COMPLETE — all safe changes applied, 41/41 tests pass

---

## Changes Applied

### Tracked generated files removed from git

- `tools/manycore-ui/__pycache__/indexer.cpython-314.pyc` — removed from index
- `tools/manycore-ui/__pycache__/runner.cpython-314.pyc` — removed from index
- `src/utils/LogCompilation/target/classes/*.class` (21 files) — removed from index
- `src/utils/LogCompilation/target/test-classes/*.class` (5 files) — removed from index

`.gitignore` updated: added `.DS_Store`, `*.class`.

---

### Hardcoded personal paths fixed

All capture scripts and `runner.py` now auto-detect the JDK from the repo root at runtime.

| File | Change |
|---|---|
| `tools/demo-runtime-truth/capture_live.sh` | `JAVA=` auto-detected via `ls -d "$REPO_ROOT/build"/*/jdk` |
| `tools/demo-agent-dispatch/capture.sh` | Same pattern |
| `tools/demo-real-bugs/capture_bugs.sh` | Same pattern |
| `tools/demo-spring-proxy-bypass/capture.sh` | Same pattern |
| `tools/manycore-ui/runner.py` | `DEFAULT_JDK` replaced with `_auto_detect_jdk()` (glob-based) |
| `tools/manycore-ui/static/index.html` | Placeholder text updated to `…/build/<variant>/jdk  (auto-detected if blank)` |

---

### Internal names removed from user-facing output

| File | Change |
|---|---|
| `tools/manycore-ui/static/app.js:1187` | Replaced `soroush_graph_hidden_class_id()` C++ function name with "hidden class identity hook must fire at load time" |
| `tools/manycore-ui/start_demo.sh` | "Share with Nico" → "Remote access credentials" |

---

### Personal-name files moved

| Before | After |
|---|---|
| `SOROUSH_JVM_SPEC.md` (root) | `docs/jvm-spec.md` |
| `Soroush_JVM_READING_GUIDE.md` (root) | `docs/jvm-reading-guide.md` |

---

### docs/README.md rewritten

- Removed "If You Are an AI Agent" lead section
- Removed hardcoded `build/macosx-aarch64-server-fastdebug/...` build command
- Updated stats: 12/12 → 15/15 ManyCore cases, 9/9 Spring Boot checks, 41/41 tests
- Added clear human navigation table pointing to key docs
- Demoted "Historical / Internal Docs" to its own section at the bottom

---

## Unchanged (by design)

- `SOROUSH_*` env vars — stable C++ API, documented in README
- JSONL data format — tests and indexer depend on it
- All `src/hotspot/` C++ source — JVM behavior, not refactored
- `tools/manycore-ui/` directory name — risky rename deferred
- Old docs/01–56 — preserved as historical record

---

## Validation

```
41 passed in 0.03s
```

All 41 indexer + graph builder unit tests pass after all changes.
