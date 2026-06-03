# Runtime Truth — Documentation

See the top-level [README](../README.md) for build and usage instructions.

---

## Key Reference Docs

| Document | Contents |
|---|---|
| [00-agent-handoff.md](00-agent-handoff.md) | Architecture overview and project status |
| [03-source-ownership-map.md](03-source-ownership-map.md) | C++ source files → capabilities map |
| [04-runtime-capture-architecture.md](04-runtime-capture-architecture.md) | Cold path, warm path, MH walk, export pipeline |
| [05-validation-guide.md](05-validation-guide.md) | 15-case test suite + Spring Boot reproduction steps |
| [06-known-limitations.md](06-known-limitations.md) | Known gaps with impact and workarounds |
| [07-build-workflow-guide.md](07-build-workflow-guide.md) | Build, libjvm copy, env vars, troubleshooting |
| [09-running-tests-and-demos.md](09-running-tests-and-demos.md) | How to run the test suite and capture demos |

---

## Current State

**Phase 1: COMPLETE.**

- 15/15 synthetic test cases pass
- Spring Boot validation (docs/56): 17,738 records, 9/9 validation checks pass
- 41/41 indexer + graph builder unit tests pass
- All attribution gaps resolved: reflection_method_invoke, invokeinterface, invokevirtual

---

## Build Reference

```bash
bash tools/build.sh
```

Validate: build the 15-case test JAR (see [09-running-tests-and-demos.md](09-running-tests-and-demos.md)), then:
```bash
SOROUSH_PROVENANCE_GRAPH=1 \
SOROUSH_EXPORT_RUNTIME_TARGETS=/tmp/val.jsonl \
  <path-to-custom-java> -jar /tmp/cases-build/cases.jar
```

Expected output ends with: `15/15 passed`

---

## Historical / Internal Docs

Docs 10–56 are development and validation notes written during implementation. They are preserved as an accurate record of design decisions, experiments, and benchmark results.
