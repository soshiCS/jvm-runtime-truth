#!/usr/bin/env python3
"""
Runtime Truth Agent Benchmark Harness

Measures whether runtime causality API access improves LLM debugging performance.

Usage:
  python harness.py bug1          # run Bug 1 for both agents
  python harness.py bug2          # run Bug 2
  python harness.py bug3          # run Bug 3
  python harness.py bug1 bug2     # run multiple bugs sequentially
  python harness.py --agent A bug1  # run only one agent

Requires:
  ANTHROPIC_API_KEY env var
  Runtime Truth UI running on localhost:5002 (no auth) with demo run ingested
  OR --jsonl /path/to/runtime_targets.jsonl to start a fresh UI automatically

Output: tools/benchmark/results/{bug}_{agent}_{timestamp}.json
        tools/benchmark/results/benchmark_result.json  (latest combined)
"""

import os, sys, json, time, argparse, subprocess, signal, tempfile, threading
import httpx
from pathlib import Path
from datetime import datetime, timezone

try:
    import anthropic
except ImportError:
    sys.exit("pip install anthropic")

from bugs import SCENARIOS
from scorer import score_run, compare

# ─── Paths ─────────────────────────────────────────────────────────────────────

REPO_ROOT    = Path(__file__).resolve().parents[2]
APP_SRC      = REPO_ROOT / "tools/demo-buggy-app/src/main/java/com/example/demo"
MANYCORE_UI  = REPO_ROOT / "tools/manycore-ui"
RESULTS_DIR  = Path(__file__).parent / "results"
DEFAULT_JSONL = "/tmp/demo-buggy-app-export2/runtime_targets.jsonl"

# ─── Config ─────────────────────────────────────────────────────────────────────

MODEL       = "claude-sonnet-4-6"
MAX_TOKENS  = 4096
MAX_TURNS   = 25
UI_PORT     = 5002
UI_BASE     = f"http://localhost:{UI_PORT}/api"
USER_PREFIX = "com/example/demo"

# ─── Tool implementations ───────────────────────────────────────────────────────

def _read_file(path: str) -> str:
    full = (APP_SRC / path.lstrip("/")).resolve()
    if not str(full).startswith(str(APP_SRC)):
        return "Error: path outside allowed directory"
    if not full.exists():
        return f"File not found: {path}\nUse list_files to see available files."
    return full.read_text()

def _list_files(directory: str = "") -> str:
    base = (APP_SRC / directory.lstrip("/")).resolve() if directory else APP_SRC.resolve()
    if not str(base).startswith(str(APP_SRC)):
        return "Error: path outside allowed directory"
    if not base.is_dir():
        return f"Not a directory: {directory or '.'}"
    files = sorted(str(p.relative_to(APP_SRC)) for p in base.rglob("*.java"))
    return "\n".join(files) if files else "(no Java files found)"

def _causality(endpoint: str, run_id: str, params: dict | None = None) -> str:
    url = f"{UI_BASE}/runs/{run_id}/causality/{endpoint}"
    try:
        r = httpx.get(url, params=params or {}, timeout=30)
        raw = r.json()
    except Exception as e:
        return json.dumps({"error": str(e)})

    # For high-cardinality endpoints, filter to user prefix to keep context manageable
    if endpoint == "polymorphic" and "polymorphic_callsites" in raw:
        raw["polymorphic_callsites"] = [
            cs for cs in raw["polymorphic_callsites"]
            if USER_PREFIX in cs.get("source_class", "")
        ]
        raw["count_shown"] = len(raw["polymorphic_callsites"])
        raw["note"] = f"Filtered to {USER_PREFIX} source classes only"

    if endpoint == "proxies" and "proxy_sites" in raw:
        demo = [s for s in raw["proxy_sites"] if USER_PREFIX in s.get("source_class", "") or USER_PREFIX in s.get("target_class", "")]
        if demo:
            raw["proxy_sites"] = demo
            raw["count_shown"] = len(demo)
            raw["note"] = f"Filtered to {USER_PREFIX} classes only"

    if endpoint == "hidden" and "hidden_classes" in raw:
        demo = [h for h in raw["hidden_classes"] if USER_PREFIX in h.get("runtime_name", "")]
        if demo:
            raw["hidden_classes"] = demo
            raw["count_shown"] = len(demo)
            raw["note"] = f"Filtered to {USER_PREFIX} classes only"

    return json.dumps(raw, indent=2)

# ─── Tool schemas ───────────────────────────────────────────────────────────────

BASE_TOOLS = [
    {
        "name": "list_files",
        "description": "List all Java source files in the demo application.",
        "input_schema": {
            "type": "object",
            "properties": {
                "directory": {
                    "type": "string",
                    "description": "Optional subdirectory, e.g. 'bug1'. Omit for all files."
                }
            }
        }
    },
    {
        "name": "read_file",
        "description": (
            "Read a Java source file. Paths are relative to com/example/demo/. "
            "Example: 'bug1/NotificationService.java' or 'BugController.java'."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "path": {"type": "string", "description": "Path relative to com/example/demo/"}
            },
            "required": ["path"]
        }
    },
    {
        "name": "submit_diagnosis",
        "description": (
            "Submit your final diagnosis. Call this exactly once when you are confident "
            "you have identified the root cause and have a patch. This ends the benchmark run."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "root_cause_file":        {
                    "type": "string",
                    "description": "File containing the bug, relative to com/example/demo/"
                },
                "root_cause_line":        {
                    "type": "integer",
                    "description": "Line number of the defect (approximate is fine)"
                },
                "root_cause_explanation": {
                    "type": "string",
                    "description": "Concise explanation of the root cause"
                },
                "patch_code":             {
                    "type": "string",
                    "description": "Exact code change, showing before → after"
                },
                "confidence":             {
                    "type": "string",
                    "enum": ["low", "medium", "high"]
                },
                "wrong_hypotheses":       {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Any wrong hypotheses you explored (self-report, may be empty)"
                }
            },
            "required": [
                "root_cause_file", "root_cause_line",
                "root_cause_explanation", "patch_code", "confidence"
            ]
        }
    }
]

CAUSALITY_TOOLS = [
    {
        "name": "causality_summary",
        "description": (
            "High-level summary of the runtime causality graph: total records captured, "
            "dispatch mechanisms observed, and class/callsite counts."
        ),
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "causality_reflection",
        "description": (
            "Show all Method.invoke() and Constructor.newInstance() callsites with the "
            "actual target class and method resolved at runtime. "
            "Reveals what static analysis cannot: the concrete callee behind reflection."
        ),
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "causality_polymorphic",
        "description": (
            "Show polymorphic callsites (invokeinterface / invokevirtual) where the JVM "
            "observed multiple concrete implementations. Lists every concrete class that "
            "executed at each call site during the captured run."
        ),
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "causality_proxies",
        "description": (
            "Show callsites targeting CGLIB/JDK-proxy/ByteBuddy generated classes. "
            "Reveals the real business class hidden behind a proxy and the interceptor chain."
        ),
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "causality_hidden",
        "description": (
            "Show lambda / hidden-class dispatch sites. Maps each invokedynamic bootstrap "
            "to the hidden class holding the lambda body, with a stable artifact_crc."
        ),
        "input_schema": {"type": "object", "properties": {}}
    },
    {
        "name": "causality_search",
        "description": "Search the causality graph by class or method name substring.",
        "input_schema": {
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "Class name, method name, or substring to search"
                }
            },
            "required": ["query"]
        }
    }
]

# ─── System prompts ─────────────────────────────────────────────────────────────

SYSTEM_A = """\
You are a senior Java engineer debugging a Spring Boot application.

Available tools:
- list_files   — list application source files
- read_file    — read a source file
- submit_diagnosis — record your final diagnosis (call once when done)

Work methodically. Read the relevant source files. Form hypotheses. When confident, \
submit your diagnosis with the exact file, line, explanation, and patch.\
"""

SYSTEM_B = """\
You are a senior Java engineer debugging a Spring Boot application.

Available tools:
- list_files         — list application source files
- read_file          — read a source file
- submit_diagnosis   — record your final diagnosis (call once when done)
- causality_summary      — runtime graph: record counts, dispatch mechanisms
- causality_reflection   — actual Method.invoke() targets observed at runtime
- causality_polymorphic  — concrete implementations at each interface/polymorphic callsite
- causality_proxies      — proxy class identification and their real targets
- causality_hidden       — lambda/hidden-class dispatch targets
- causality_search       — search by class or method name

The causality tools expose what ACTUALLY executed at runtime — not just what source code \
implies. They resolve reflection targets, proxy chains, and lambda dispatch that are \
invisible to static analysis and normal stack traces.

Work methodically. Consider querying the causality graph early to narrow the search space. \
When confident, submit your diagnosis.\
"""

# ─── Agent runner ───────────────────────────────────────────────────────────────

def run_agent(scenario: dict, agent_id: str, causality_run_id: str | None) -> dict:
    """Run one agent against one bug scenario. Returns a full trace dict."""
    client  = anthropic.Anthropic()
    tools   = BASE_TOOLS + (CAUSALITY_TOOLS if agent_id == "B" else [])
    system  = SYSTEM_B  if agent_id == "B" else SYSTEM_A
    messages = [{"role": "user", "content": scenario["prompt"]}]

    trace = {
        "agent":       agent_id,
        "bug":         scenario["id"],
        "model":       MODEL,
        "start_ts":    datetime.now(timezone.utc).isoformat(),
        "end_ts":      None,
        "total_turns": 0,
        "tool_calls":  [],
        "turns":       [],
        "diagnosis":   None,
        "completed":   False,
    }

    print(f"\n{'='*60}")
    print(f"  Agent {agent_id} | {scenario['title']}")
    print(f"{'='*60}")

    for turn_n in range(MAX_TURNS):
        print(f"  [turn {turn_n+1}] calling API...", end=" ", flush=True)
        resp = client.messages.create(
            model=MODEL,
            max_tokens=MAX_TOKENS,
            system=system,
            tools=tools,
            messages=messages,
        )
        print(f"stop={resp.stop_reason}", flush=True)

        turn_record = {
            "n":           turn_n,
            "stop_reason": resp.stop_reason,
            "text":        "",
            "tool_uses":   [],
        }
        tool_results = []

        for block in resp.content:
            if hasattr(block, "text"):
                turn_record["text"] += block.text
                if block.text.strip():
                    # Print first 120 chars of reasoning
                    preview = block.text.strip()[:120].replace("\n", " ")
                    print(f"    [text] {preview}{'...' if len(block.text) > 120 else ''}")

            elif block.type == "tool_use":
                tool_name  = block.name
                tool_input = block.input
                print(f"    [tool] {tool_name}({_fmt_input(tool_input)})", end=" ", flush=True)

                result = _dispatch(tool_name, tool_input, causality_run_id)
                preview_len = min(200, len(result))
                print(f"→ {result[:preview_len].replace(chr(10),' ')}{'...' if len(result) > preview_len else ''}")

                tu = {
                    "tool":   tool_name,
                    "input":  tool_input,
                    "result": result,       # full result stored
                }
                turn_record["tool_uses"].append(tu)
                trace["tool_calls"].append({
                    "tool":   tool_name,
                    "input":  tool_input,
                    "result": result[:1000],  # truncate for readability
                })
                tool_results.append({
                    "type":        "tool_result",
                    "tool_use_id": block.id,
                    "content":     result,
                })

                if tool_name == "submit_diagnosis":
                    trace["diagnosis"]   = tool_input
                    trace["completed"]   = True
                    trace["total_turns"] = turn_n + 1
                    trace["end_ts"]      = datetime.now(timezone.utc).isoformat()
                    trace["turns"].append(turn_record)
                    print(f"\n  -> Diagnosis submitted on turn {turn_n + 1}")
                    return trace

        trace["turns"].append(turn_record)
        messages.append({"role": "assistant", "content": resp.content})

        if tool_results:
            messages.append({"role": "user", "content": tool_results})
        elif resp.stop_reason == "end_turn":
            print("  -> Agent stopped without submitting diagnosis.")
            break

    trace["total_turns"] = MAX_TURNS
    trace["end_ts"]      = datetime.now(timezone.utc).isoformat()
    return trace


def _dispatch(name: str, inp: dict, run_id: str | None) -> str:
    if name == "read_file":
        return _read_file(inp.get("path", ""))
    if name == "list_files":
        return _list_files(inp.get("directory", ""))
    if name == "submit_diagnosis":
        return "Diagnosis recorded."
    if name == "causality_summary":
        return _causality("summary",     run_id)
    if name == "causality_reflection":
        return _causality("reflection",  run_id)
    if name == "causality_polymorphic":
        return _causality("polymorphic", run_id)
    if name == "causality_proxies":
        return _causality("proxies",     run_id)
    if name == "causality_hidden":
        return _causality("hidden",      run_id)
    if name == "causality_search":
        return _causality("search",      run_id, params={"q": inp.get("query", "")})
    return f"Unknown tool: {name}"


def _fmt_input(inp: dict) -> str:
    if not inp:
        return ""
    items = [f"{k}={repr(v)[:40]}" for k, v in inp.items() if k != "wrong_hypotheses"]
    return ", ".join(items)

# ─── manycore-ui lifecycle ──────────────────────────────────────────────────────

_ui_proc = None

def start_ui(jsonl_path: str) -> str | None:
    """Start a fresh manycore-ui on UI_PORT and ingest the demo run. Returns run_id."""
    global _ui_proc

    # Kill any existing process on this port
    subprocess.run(["lsof", "-ti", f":{UI_PORT}"], capture_output=True).stdout.decode()
    existing = subprocess.run(["lsof", "-ti", f":{UI_PORT}"], capture_output=True).stdout.strip()
    if existing:
        for pid in existing.split():
            try:
                os.kill(int(pid), signal.SIGTERM)
            except Exception:
                pass
        time.sleep(1)

    print(f"  Starting manycore-ui on port {UI_PORT}...")
    env = os.environ.copy()
    env.pop("RT_DEMO_TOKEN", None)   # ensure no auth required
    _ui_proc = subprocess.Popen(
        ["python3", "app.py", str(UI_PORT)],
        cwd=MANYCORE_UI,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # Wait for server to be ready
    for _ in range(20):
        try:
            r = httpx.get(f"{UI_BASE}/runs", timeout=2)
            if r.status_code == 200:
                break
        except Exception:
            pass
        time.sleep(0.5)
    else:
        print("  ERROR: manycore-ui did not start in time")
        return None

    # Ingest the demo JSONL
    run_dir = str(Path(jsonl_path).parent)
    try:
        r = httpx.post(
            f"{UI_BASE}/runs/ingest",
            json={"label": "demo-buggy-app", "run_dir": run_dir},
            timeout=10,
        )
        data = r.json()
        run_id = data.get("run_id")
        print(f"  Ingested run: {run_id}  (jsonl: {jsonl_path})")
        return run_id
    except Exception as e:
        print(f"  ERROR ingesting run: {e}")
        return None


def stop_ui():
    global _ui_proc
    if _ui_proc:
        _ui_proc.terminate()
        _ui_proc = None

# ─── Main ───────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bugs", nargs="+", choices=list(SCENARIOS.keys()),
                    help="Which bugs to benchmark")
    ap.add_argument("--agent", choices=["A", "B", "both"], default="both")
    ap.add_argument("--jsonl", default=DEFAULT_JSONL,
                    help="Path to runtime_targets.jsonl")
    ap.add_argument("--run-id",
                    help="Use existing manycore-ui run_id instead of starting a new server")
    args = ap.parse_args()

    if not os.environ.get("ANTHROPIC_API_KEY"):
        sys.exit("Error: ANTHROPIC_API_KEY not set")

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)

    # Start manycore-ui if needed
    causality_run_id = args.run_id
    if not causality_run_id:
        if not Path(args.jsonl).exists():
            sys.exit(f"Error: JSONL not found: {args.jsonl}\n"
                     f"Run the demo app first: tools/demo-buggy-app/run_demo.sh --non-interactive")
        causality_run_id = start_ui(args.jsonl)
        if not causality_run_id:
            sys.exit("Failed to start manycore-ui")

    agents   = (["A", "B"] if args.agent == "both" else [args.agent])
    all_results = {}

    try:
        for bug_id in args.bugs:
            scenario = SCENARIOS[bug_id]
            bug_results = {}

            for agent_id in agents:
                ts = datetime.now().strftime("%Y%m%d_%H%M%S")
                out_path = RESULTS_DIR / f"{bug_id}_{agent_id}_{ts}.json"

                trace  = run_agent(scenario, agent_id, causality_run_id)
                scored = score_run(trace, scenario["ground_truth"])
                result = {"trace": trace, "score": scored}

                out_path.write_text(json.dumps(result, indent=2))
                print(f"\n  Score: {scored['total']}/{scored['max_score']} ({scored['pct']}%)"
                      f"  turns={scored['total_turns']}"
                      f"  files_read={scored['unique_files_read']}"
                      f"  causality={scored['causality_calls']}")

                bug_results[agent_id] = result

            if "A" in bug_results and "B" in bug_results:
                comparison = compare(bug_results["A"]["score"], bug_results["B"]["score"])
                bug_results["comparison"] = comparison
                print(f"\n  Winner: {comparison['winner']}  |  {comparison['explanation']}")

            all_results[bug_id] = bug_results

    finally:
        stop_ui()

    # Write combined result
    combined_path = RESULTS_DIR / "benchmark_result.json"
    combined_path.write_text(json.dumps(all_results, indent=2))
    print(f"\nResults written to: {combined_path}")

    return all_results


if __name__ == "__main__":
    main()
