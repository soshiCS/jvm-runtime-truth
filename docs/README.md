# Runtime Truth Documentation

This directory contains the complete knowledge base for the Runtime Truth provenance graph project.

---

## If You Are an AI Agent: Read This First

**[→ AGENT_NAVIGATOR.md](AGENT_NAVIGATOR.md)**

The navigator is the single entry point for future agents. It contains:
- Current project status (one page)
- The core authoritativity rule (`jdk21u-export` only)
- "Where should I go if…" routing for every common task
- Feature-to-file and task-to-validation routing tables
- All known traps and which document explains each one
- Five recommended reading paths (10-min, 30-min, deep, build-only, Phase 2)

**Do not open any other document until you have read AGENT_NAVIGATOR.md.**

---

## Start Here (Humans)

| You are... | Start with |
|---|---|
| A new AI agent taking over this project | **[AGENT_NAVIGATOR.md](AGENT_NAVIGATOR.md)** |
| A developer wanting to understand the goals | [01-project-overview.md](01-project-overview.md) |
| Trying to reproduce a past result or understand a past decision | [02-phase-history.md](02-phase-history.md) |
| Looking for which file to edit | [03-source-ownership-map.md](03-source-ownership-map.md) |
| Trying to understand how the instrumentation works | [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) |
| Trying to validate the system from scratch | [05-validation-guide.md](05-validation-guide.md) |
| Investigating a diagnostic or limitation | [06-known-limitations.md](06-known-limitations.md) |
| Building the JVM or running into build problems | [07-build-workflow-guide.md](07-build-workflow-guide.md) |
| Wanting to run a test or demo right now | [09-running-tests-and-demos.md](09-running-tests-and-demos.md) |

---

## Document Index

| Document | Contents |
|---|---|
| [AGENT_NAVIGATOR.md](AGENT_NAVIGATOR.md) | **Navigation map for future agents** — routing tables, reading paths, traps |
| [00-agent-handoff.md](00-agent-handoff.md) | Project summary, current status, critical traps, one-stop reference for new agents |
| [01-project-overview.md](01-project-overview.md) | Goals, long-term vision, staticization model, record type relationships |
| [02-phase-history.md](02-phase-history.md) | Every milestone: goal, problem, solution, files changed, lessons learned |
| [03-source-ownership-map.md](03-source-ownership-map.md) | Files → capabilities map with key functions and extension guidance |
| [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) | Cold path, warm path, receiver discovery, MH walk, adapter graphs, export pipeline |
| [05-validation-guide.md](05-validation-guide.md) | All 12 ManyCore cases + Spring Boot: exact reproduction commands and expected outputs |
| [06-known-limitations.md](06-known-limitations.md) | All known gaps with impact assessment and Phase 2 fix guidance |
| [07-build-workflow-guide.md](07-build-workflow-guide.md) | Build commands, libjvm copy, env vars, common mistakes, troubleshooting |
| [08-phase2-causality-graph-design-review.md](08-phase2-causality-graph-design-review.md) | Phase 2 architectural review: causality graph MVP feasibility, gaps, new record type proposals, roadmap |
| [09-running-tests-and-demos.md](09-running-tests-and-demos.md) | Quick-start guide: how to run the 12-case suite, Spring Boot, and ManyCore UI (local + remote demo) |
| [10-phase2b-runtime-target-attribution-design.md](10-phase2b-runtime-target-attribution-design.md) | Phase 2b design review: root-cause investigation of `runtime_target` orphans, emission-site inventory, option analysis, recommended Option B implementation |

---

## Current State (2026-05-30)

**Phase 1: COMPLETE. Phase 2A: COMPLETE. Phase 2B: COMPLETE.**

- 12/12 ManyCore synthetic cases pass with zero user-code diagnostics
- Spring Boot 4.0.6 validation: exit 0, `=== validation complete ===`, 0 user-code diagnostics
- Phase 2A: `graph_builder.py` Runtime Causality Graph MVP — 19/19 unit tests; 11/11 validation checks on both workloads
- Phase 2B: `runtime_target` source attribution — vframeStream walk in `methodHandles.cpp`. ManyCore 915→0 orphans; Spring Boot 2,905→0 orphans. `source_capture=exact/missing` fields in schema.
- `heuristic_edges_created` = 0 (hard invariant preserved)
- Phase 2C–D (invocation frequency, web request validation): not started

See [00-agent-handoff.md](00-agent-handoff.md) — Recommended Next Tasks.

---

## Quick Reference: The Two Commands You Always Need

**Build:**
```bash
cd /Users/soroushaghajani/custom-jvm/jdk21u-export && make hotspot && \
cp build/macosx-aarch64-server-fastdebug/support/modules_libs/java.base/server/libjvm.dylib \
   build/macosx-aarch64-server-fastdebug/jdk/lib/server/libjvm.dylib
```

**Validate (ManyCore):**
```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/manycore_val.jsonl \
  /Users/soroushaghajani/custom-jvm/jdk21u-export/build/macosx-aarch64-server-fastdebug/jdk/bin/java \
  -cp /tmp/manycore-cases-build/classes manycorecases.ManyCoreCasesMain
```

Expected: `ManyCore cases demo complete — 12/12 passed`
