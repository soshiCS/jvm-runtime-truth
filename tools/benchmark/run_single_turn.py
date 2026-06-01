#!/usr/bin/env python3
"""
Single-turn benchmark runner — uses `claude -p` (Claude Code CLI) as the LLM backend.

Provides each agent with a single comprehensive prompt and asks for a structured diagnosis.
Agent A gets: bug description + source code.
Agent B gets: bug description + source code + causality API output.

Usage:
  python run_single_turn.py bug1
  python run_single_turn.py bug1 bug2 bug3
  python run_single_turn.py bug1 --agent A   # only Agent A

Output: tools/benchmark/results/single_turn_{bug}_{agent}_{ts}.json
        tools/benchmark/results/benchmark_result_single_turn.json
"""

import os, sys, json, subprocess, argparse, time
from pathlib import Path
from datetime import datetime, timezone

sys.path.insert(0, str(Path(__file__).parent))
from bugs import SCENARIOS
from scorer import score_run, compare

# ─── Paths ─────────────────────────────────────────────────────────────────────

REPO_ROOT   = Path(__file__).resolve().parents[2]
APP_SRC     = REPO_ROOT / "tools/demo-buggy-app/src/main/java/com/example/demo"
RESULTS_DIR = Path(__file__).parent / "results"

# ─── Pre-captured causality data ───────────────────────────────────────────────
# These are real outputs from /api/runs/.../causality/* for the demo run.

CAUSALITY_BUG1 = """\
=== /causality/reflection (filtered to com/example/demo) ===
{
  "count": 34,
  "explanation": "Found 34 reflection callsite(s). Reflection hides the actual callee from normal
    stack traces and IDE search. The causality graph resolves each to the exact target method.",
  "reflection_callsites": [
    {
      "category": "reflection_method_invoke",
      "evidence": "OBSERVED_ONLY",
      "source_class": "com/example/demo/bug1/NotificationService",
      "source_method": "dispatch",
      "source_bci": 81,
      "source_opcode": "invokevirtual",
      "target_class": "com/example/demo/bug1/AuditHandler",
      "target_method": "handle",
      "target_descriptor": "(Ljava/lang/String;)V"
    },
    {
      "category": "reflection_method_invoke",
      "source_class": "org/springframework/aop/support/AopUtils",
      "source_method": "invokeJoinpointUsingReflection",
      "source_bci": 34,
      "target_class": "com/example/demo/bug2/OrderService",
      "target_method": "processOrder"
    }
  ]
}
"""

CAUSALITY_BUG2 = """\
=== /causality/proxies (filtered to com/example/demo) ===
{
  "count": 514,
  "explanation": "Found 514 callsite(s) targeting generated or proxy classes. Stack traces show
    the generated class name, not the real business class. Use /causality/chain to follow through.",
  "proxy_sites": [
    {
      "proxy_kind": "cglib",
      "note": "Spring CGLIB proxy wrapping com/example/demo/bug2/OrderService — real implementation is in the non-proxy class",
      "source_class": "com/example/demo/BugController",
      "source_method": "bug2",
      "source_bci": 21,
      "target_class": "com/example/demo/bug2/OrderService$$SpringCGLIB$$0",
      "target_method": "processOrder"
    }
  ]
}

=== /causality/reflection (AOP chain entries) ===
{
  "reflection_callsites": [
    {
      "category": "reflection_method_invoke",
      "source_class": "org/springframework/aop/aspectj/AbstractAspectJAdvice",
      "source_method": "invokeAdviceMethodWithGivenArgs",
      "source_bci": 76,
      "target_class": "com/example/demo/bug2/AuditInterceptor",
      "target_method": "timed"
    },
    {
      "category": "reflection_method_invoke",
      "source_class": "org/springframework/aop/support/AopUtils",
      "source_method": "invokeJoinpointUsingReflection",
      "source_bci": 34,
      "target_class": "com/example/demo/bug2/OrderService",
      "target_method": "processOrder"
    }
  ]
}
"""

CAUSALITY_BUG3 = """\
=== /causality/polymorphic (filtered to com/example/demo/bug3/RequestRouter) ===
{
  "explanation": "Found 2670 polymorphic callsite(s). Filtered to com/example/demo source classes.",
  "polymorphic_callsites": [
    {
      "category": "invokeinterface",
      "source_class": "com/example/demo/bug3/RequestRouter",
      "source_method": "route",
      "source_bci": 40,
      "source_opcode": "invokeinterface",
      "static_label": "blocked_multi_target",
      "target_count": 3,
      "targets": [
        {
          "class": "com/example/demo/bug3/GuestHandler",
          "method": "handle",
          "descriptor": "(Lcom/example/demo/bug3/Request;)Ljava/lang/String;"
        },
        {
          "class": "com/example/demo/bug3/AdminHandler",
          "method": "handle",
          "descriptor": "(Lcom/example/demo/bug3/Request;)Ljava/lang/String;"
        },
        {
          "class": "com/example/demo/bug3/UserHandler",
          "method": "handle",
          "descriptor": "(Lcom/example/demo/bug3/Request;)Ljava/lang/String;"
        }
      ]
    }
  ]
}
"""

CAUSALITY_DATA = {
    "bug1": CAUSALITY_BUG1,
    "bug2": CAUSALITY_BUG2,
    "bug3": CAUSALITY_BUG3,
}

# ─── Source file collection ─────────────────────────────────────────────────────

import re as _re

_BUG_COMMENT_PAT = _re.compile(
    r'//\s*(BUG IS HERE|← BUG|BUG:|bug is here)',
    _re.IGNORECASE
)
_BUG_JAVADOC_PAT = _re.compile(
    r'BUG:|BUG IS HERE|← BUG|In a real system this would come from'
    r'|Causality:|causality reveal|→ AuditHandler|AuditHandler instead of'
    r'|dispatched via reflection|CGLIB proxy|SpringCGLIB|AuditInterceptor chain'
    r'|blocked_multi_target|reflection_method_invoke'
    r'|copy-pasted from Admin|wrong data is the observable symptom'
    r'|BugController →',
    _re.IGNORECASE
)

def _sanitize(text: str) -> str:
    """Remove lines that point to the bug or reveal causality conclusions.
    Keeps the buggy code itself; removes only the narrative hints.
    """
    lines = []
    for line in text.split("\n"):
        stripped = line.strip()
        # Drop // comment lines with explicit bug markers
        if stripped.startswith("//") and _BUG_COMMENT_PAT.search(stripped):
            continue
        # Drop Javadoc * lines that name the bug or causality endpoint results
        if stripped.startswith("*") and _BUG_JAVADOC_PAT.search(stripped):
            continue
        # Strip inline "← BUG" suffixes from code lines
        line = _re.sub(r'\s*//\s*←\s*BUG[^\n]*', '', line)
        lines.append(line)
    return "\n".join(lines)


def collect_sources(bug_id: str) -> str:
    """Collect all Java source files relevant to the given bug, as a formatted block.
    Bug indicator comments are stripped so Agent A cannot trivially locate the defect."""
    parts = []
    # Always include shared files
    shared = ["BugController.java", "BugDemoApplication.java"]
    # Bug-specific package
    bug_dir = f"bug{bug_id[-1]}"  # "bug1", "bug2", "bug3"

    # Gather all relevant files
    candidates = list(APP_SRC.rglob("*.java"))
    candidates.sort()

    for p in candidates:
        rel = str(p.relative_to(APP_SRC))
        # Include if shared file or in the bug's own package
        is_shared = any(rel == s for s in shared)
        is_bug_pkg = rel.startswith(bug_dir + "/")
        if is_shared or is_bug_pkg:
            parts.append(f"=== {rel} ===\n{_sanitize(p.read_text())}")

    # Also note other packages exist
    other_pkgs = sorted({
        str(p.relative_to(APP_SRC)).split("/")[0]
        for p in candidates
        if not str(p.relative_to(APP_SRC)).startswith(bug_dir)
        and str(p.relative_to(APP_SRC)) not in shared
    })
    if other_pkgs:
        parts.append(f"=== (other packages available but not shown: {', '.join(other_pkgs)}) ===\n")

    return "\n\n".join(parts)

# ─── Prompt builders ────────────────────────────────────────────────────────────

DIAGNOSIS_FORMAT = """\
Respond with ONLY a JSON object in this exact format (no markdown, no explanation outside the JSON):
{
  "root_cause_file": "relative/path/to/File.java",
  "root_cause_line": <integer>,
  "root_cause_explanation": "one to three sentence explanation",
  "patch_code": "exact before → after change",
  "confidence": "low|medium|high",
  "wrong_hypotheses": ["any wrong paths you considered (may be empty)"]
}\
"""

def build_prompt_a(scenario: dict, sources: str) -> str:
    return f"""\
You are a senior Java engineer. Diagnose the following bug in a Spring Boot application.

{scenario['prompt']}

SOURCE CODE
{sources}

INSTRUCTIONS
{DIAGNOSIS_FORMAT}
"""

def build_prompt_b(scenario: dict, sources: str, causality: str) -> str:
    return f"""\
You are a senior Java engineer. Diagnose the following bug in a Spring Boot application.
You have access to runtime causality data that shows what actually executed at runtime.

{scenario['prompt']}

SOURCE CODE
{sources}

RUNTIME CAUSALITY DATA
The following was captured by running the application under a custom JVM with instrumentation.
It shows the actual dispatch targets observed at runtime — including targets of reflection calls,
proxy chains, and polymorphic interface calls that are invisible to static analysis.

{causality}

INSTRUCTIONS
{DIAGNOSIS_FORMAT}
"""

# ─── Runner ─────────────────────────────────────────────────────────────────────

def run_claude_p(prompt: str, model: str = "claude-sonnet-4-6") -> tuple[str, dict]:
    """Run `claude -p` with the given prompt. Returns (raw_output, metadata)."""
    start = time.monotonic()
    result = subprocess.run(
        ["claude", "-p", "--model", model, "--output-format", "json", prompt],
        capture_output=True, text=True, timeout=300
    )
    elapsed = time.monotonic() - start

    if result.returncode != 0:
        raise RuntimeError(f"claude -p failed: {result.stderr[:500]}")

    try:
        meta = json.loads(result.stdout)
        text = meta.get("result", "")
    except json.JSONDecodeError:
        text = result.stdout
        meta = {}

    meta["elapsed_s"] = round(elapsed, 1)
    return text, meta

def parse_diagnosis(text: str) -> dict | None:
    """Extract JSON diagnosis from claude output."""
    text = text.strip()
    # Strip markdown fences if present
    if text.startswith("```"):
        lines = text.split("\n")
        text = "\n".join(lines[1:-1] if lines[-1].strip() == "```" else lines[1:])
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        # Try to find JSON object in text
        import re
        m = re.search(r'\{.*\}', text, re.DOTALL)
        if m:
            try:
                return json.loads(m.group())
            except Exception:
                pass
    return None

def run_agent(scenario: dict, agent_id: str, model: str) -> dict:
    bug_id  = scenario["id"]
    sources = collect_sources(bug_id)

    if agent_id == "A":
        prompt = build_prompt_a(scenario, sources)
    else:
        causality = CAUSALITY_DATA.get(bug_id, "(no causality data for this bug)")
        prompt = build_prompt_b(scenario, sources, causality)

    print(f"\n{'='*60}")
    print(f"  Agent {agent_id} | {scenario['title']} | model={model}")
    print(f"  Prompt length: {len(prompt)} chars")
    print(f"{'='*60}")

    start_ts = datetime.now(timezone.utc).isoformat()
    text, meta = run_claude_p(prompt, model)
    end_ts   = datetime.now(timezone.utc).isoformat()

    diag = parse_diagnosis(text)
    print(f"  Elapsed: {meta.get('elapsed_s','?')}s")
    if diag:
        print(f"  File:    {diag.get('root_cause_file','?')}")
        print(f"  Line:    {diag.get('root_cause_line','?')}")
        print(f"  Conf:    {diag.get('confidence','?')}")
        print(f"  Patch:   {(diag.get('patch_code','') or '')[:80]}")
    else:
        print(f"  WARNING: could not parse JSON diagnosis from output")
        print(f"  Raw (first 300): {text[:300]}")

    trace = {
        "agent":       agent_id,
        "bug":         bug_id,
        "model":       model,
        "mode":        "single_turn",
        "start_ts":    start_ts,
        "end_ts":      end_ts,
        "elapsed_s":   meta.get("elapsed_s"),
        "input_tokens":  meta.get("usage", {}).get("input_tokens"),
        "output_tokens": meta.get("usage", {}).get("output_tokens"),
        "cost_usd":      meta.get("total_cost_usd"),
        "num_turns":     meta.get("num_turns", 1),
        "tool_calls":    [],   # no tool loop in single-turn mode
        "diagnosis":     diag,
        "raw_output":    text,
        "completed":     diag is not None,
    }
    return trace

# ─── Main ───────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bugs", nargs="+", choices=list(SCENARIOS.keys()))
    ap.add_argument("--agent", choices=["A", "B", "both"], default="both")
    ap.add_argument("--model", default="claude-sonnet-4-6")
    args = ap.parse_args()

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    agents = (["A", "B"] if args.agent == "both" else [args.agent])
    all_results = {}

    for bug_id in args.bugs:
        scenario    = SCENARIOS[bug_id]
        bug_results = {}

        for agent_id in agents:
            ts       = datetime.now().strftime("%Y%m%d_%H%M%S")
            out_path = RESULTS_DIR / f"single_turn_{bug_id}_{agent_id}_{ts}.json"

            trace  = run_agent(scenario, agent_id, args.model)
            scored = score_run(trace, scenario["ground_truth"])

            result = {"trace": trace, "score": scored}
            out_path.write_text(json.dumps(result, indent=2))

            print(f"\n  Score: {scored['total']}/{scored['max_score']} ({scored['pct']}%)")
            print(f"    file={scored['file_score']}/3  "
                  f"line={scored['line_score']}/3  "
                  f"patch={scored['patch_score']}/5  "
                  f"explain={scored['explain_score']}/5")

            bug_results[agent_id] = result

        if "A" in bug_results and "B" in bug_results:
            comparison = compare(bug_results["A"]["score"], bug_results["B"]["score"])
            bug_results["comparison"] = comparison
            print(f"\n  ── Head-to-head ──")
            print(f"  Winner:      {comparison['winner']}")
            print(f"  Score delta: {comparison['score_delta']:+d} (B minus A)")
            print(f"  {comparison['explanation']}")

        all_results[bug_id] = bug_results

    # Write combined result
    combined = RESULTS_DIR / "benchmark_result_single_turn.json"
    combined.write_text(json.dumps(all_results, indent=2))
    print(f"\nAll results: {combined}")

    # Summary table
    print("\n" + "="*60)
    print("  BENCHMARK SUMMARY")
    print("="*60)
    print(f"  {'Bug':<8} {'A score':>8} {'B score':>8} {'Delta':>8} {'Winner':>8}")
    print(f"  {'-'*8} {'-'*8} {'-'*8} {'-'*8} {'-'*8}")
    for bug_id, br in all_results.items():
        sa = br.get("A", {}).get("score", {})
        sb = br.get("B", {}).get("score", {})
        cmp = br.get("comparison", {})
        a_str = f"{sa.get('total','?')}/{sa.get('max_score','?')}" if sa else "—"
        b_str = f"{sb.get('total','?')}/{sb.get('max_score','?')}" if sb else "—"
        delta = cmp.get("score_delta", "?") if cmp else "?"
        winner = cmp.get("winner", "?") if cmp else "?"
        print(f"  {bug_id:<8} {a_str:>8} {b_str:>8} {str(delta):>8} {winner:>8}")
    print("="*60)

    return all_results


if __name__ == "__main__":
    main()
