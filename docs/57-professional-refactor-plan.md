# Professional Refactor Plan

**Date:** 2026-06-02  
**Status:** PLAN — executing safe changes in docs/58

---

## Audit Findings

### 1. Tracked generated files (must fix)

| File | Issue |
|---|---|
| `tools/rt-ui/__pycache__/indexer.cpython-314.pyc` | Compiled .pyc tracked in git |
| `tools/rt-ui/__pycache__/runner.cpython-314.pyc` | Compiled .pyc tracked in git |
| `src/utils/LogCompilation/target/classes/*.class` | Compiled Java .class files tracked in git |

**Fix:** Remove from git index, add to `.gitignore`.

---

### 2. Hardcoded personal paths (must fix)

| File | Location | Issue |
|---|---|---|
| `tools/demo-real-bugs/capture_bugs.sh` | Line 26 | `JAVA="/Users/soroushaghajani/..."` hardcoded |
| `tools/demo-agent-dispatch/capture.sh` | Line 9 | Same pattern |
| `tools/demo-runtime-truth/capture_live.sh` | Line 25 | Same pattern |
| `tools/demo-spring-proxy-bypass/capture.sh` | Line 11 | Same pattern |
| `tools/rt-ui/runner.py` | Line 15 | `DEFAULT_JDK = "/Users/soroushaghajani/..."` |
| `tools/rt-ui/static/index.html` | Line 169 | Placeholder with full personal path |
| `docs/README.md` | Multiple | Build command with hardcoded personal path |

**Fix:** Replace hardcoded paths with `$(dirname "$0")/../../build/...` or `AUTO_DETECT` logic.

---

### 3. Internal names leaking to users (should fix)

| File | Issue |
|---|---|
| `tools/rt-ui/static/app.js:1187` | C++ function `soroush_graph_hidden_class_id()` in user-facing error message |
| `tools/rt-ui/static/app.js:1205` | `SOROUSH_CAPTURE_FINAL_BYTECODE=1` in user-facing error (acceptable as env var) |
| `tools/rt-ui/start_demo.sh` | Comment "Share with Nico" — personal name |

**Fix:** Replace C++ function name with a user-readable description. Remove personal name comment.

---

### 4. Personal name in root-level files (should fix)

| File | Lines |
|---|---|
| `SOROUSH_JVM_SPEC.md` | 1,139 lines — internal spec, root level |
| `Soroush_JVM_READING_GUIDE.md` | 868 lines — internal reading guide, root level |

**Fix:** Move to `docs/internal/` or `docs/`. Do not delete — valuable reference.

---

### 5. docs/README.md quality issues (must fix)

- Leads with "If You Are an AI Agent" — wrong audience for public repo
- Hardcoded build path: `build/macosx-aarch64-server-fastdebug/...`
- Shows "12/12 passed" — outdated (should be 15/15)
- No clear navigation structure for human readers
- Does not reference `tools/build.sh`

**Fix:** Rewrite to be human-first: brief intro, point to main README, list docs by topic.

---

### 6. .gitignore gaps (should fix)

- `.DS_Store` not listed (16 `.DS_Store` files present, not tracked, but should be ignored)
- `__pycache__/` not listed
- `*.pyc` not listed
- `target/` (Maven) not listed
- `build/` not listed (though it's already ignored somehow — verify)

**Fix:** Add missing entries.

---

### 7. docs volume / organization (low priority)

53 docs files. Most are internal dev notes, benchmark notes, agent handoff docs. No public navigation structure. The docs are valuable history but not organized for a new reader.

**Fix (minimal):** Make `docs/README.md` the entry point with clear sections: Overview, Key Docs, Internal/Historical.

---

## Classification

### SAFE — execute now

| # | Change | Risk |
|---|---|---|
| S1 | Remove .pyc / .class from git tracking, add to .gitignore | None |
| S2 | Add `.DS_Store`, `__pycache__/`, `*.pyc`, `target/classes/` to `.gitignore` | None |
| S3 | Fix `DEFAULT_JDK` in `runner.py` to auto-detect from script location | Low |
| S4 | Fix hardcoded `JAVA=` in all 4 capture scripts to use `$REPO/build/...` pattern | Low |
| S5 | Fix JDK path placeholder in `index.html` | None |
| S6 | Remove "Share with Nico" comment from `start_demo.sh` | None |
| S7 | Replace C++ function name in `app.js` error with user-readable message | None |
| S8 | Rewrite `docs/README.md` — human-first, correct stats, no hardcoded paths | None |
| S9 | Move `SOROUSH_JVM_SPEC.md` and `Soroush_JVM_READING_GUIDE.md` to `docs/` | Low |

### RISKY — leave for later

| # | Change | Why risky |
|---|---|---|
| R1 | Rename `tools/rt-ui/` → `tools/ui/` | Breaks all imports, test references, scripts |
| R2 | Rename `SOROUSH_*` env vars | Requires C++ + Python + docs + test changes in sync |
| R3 | Delete/archive old docs (docs/01–54) | Destroys project history; unknown what downstream tools reference |
| R4 | Rename Java packages in demo apps | Not needed; packages are not user-facing |

### LEAVE ALONE

- All `SOROUSH_*` env vars (JVM C++ implementation; stable API)
- JSONL data format (tests + indexer depend on it)
- All `src/hotspot/` C++ files (JVM behavior)
- Test suite structure
- `tools/build.sh` (already correct)
- `README.md` (already correct)
