#!/usr/bin/env python3
"""
Multi-turn discovery benchmark harness — v2.

Agents start with minimal information (endpoint + symptom + log only).
They must discover the root cause via tool calls — no source pre-loaded.

Agent A: search_files, read_file, grep
Agent B: same tools + causality API (pre-captured real data)

Uses `claude -p` (Claude Code CLI OAuth session) in a ReAct loop.
Each turn builds the full conversation history in a single prompt.

Usage:
  python harness_v2.py bug1v2 bug2v2 bug3v2
  python harness_v2.py bug1v2 --agent A
  python harness_v2.py bug1v2 --agent B --model claude-sonnet-4-6
"""

import os, sys, json, subprocess, argparse, time, re
from pathlib import Path
from datetime import datetime, timezone

sys.path.insert(0, str(Path(__file__).parent))
from bugs_v2 import SCENARIOS_V2
from bugs_v3 import SCENARIOS_V3
from bugs_v4 import SCENARIOS_V4
from bugs_v5 import SCENARIOS_V5
from scorer import score_run, compare

SCENARIOS_ALL = {**SCENARIOS_V2, **SCENARIOS_V3, **SCENARIOS_V4, **SCENARIOS_V5}

# ─── Paths ──────────────────────────────────────────────────────────────────────

REPO_ROOT   = Path(__file__).resolve().parents[2]
APP_ROOT    = REPO_ROOT / "tools/demo-buggy-app"
APP_SRC     = APP_ROOT / "src/main/java/com/example/demo"
APP_RES     = APP_ROOT / "src/main/resources"

# Per-scenario path overrides — resolved in run_agent() from scenario["app_root"].
# Scenarios without app_root default to the demo-buggy-app paths above.
_DEFAULT_APP_ROOT = APP_ROOT
_DEFAULT_APP_SRC  = APP_SRC
_DEFAULT_APP_RES  = APP_RES
RESULTS_DIR = Path(__file__).parent / "results"

MAX_TURNS   = 25

# ─── Tool descriptions ──────────────────────────────────────────────────────────

TOOLS_A_DESC = """\
You have the following tools. Use them to investigate the bug:

  search_files
    Find files in the application repository.
    Args: {"pattern": "<glob>"}
    Example: {"pattern": "**/*.java"} or {"pattern": "bug2v2/*.java"}

  read_file
    Read a file from the repository (Java source, properties, YAML).
    Args: {"path": "<relative-path>"}
    Paths are relative to src/main/java/com/example/demo/ for Java files,
    or src/main/resources/ for config files.
    Example: {"path": "bug1v2/NotificationService2.java"}
    Example: {"path": "application.properties"}   (for config, no prefix needed)

  grep
    Search for text across all source and config files.
    Args: {"pattern": "<regex-or-text>", "path": "<optional-subdir>"}
    Example: {"pattern": "DELIVERY", "path": "bug1v2"}
    Example: {"pattern": "HandlerConfig"}

  submit_diagnosis
    Submit your final diagnosis. Call this when you are confident.
    Args: {
      "root_cause_file": "<path relative to src/main/java/com/example/demo/ or src/main/resources/>",
      "root_cause_line": <integer>,
      "root_cause_explanation": "<1-3 sentences>",
      "patch_code": "<before → after>",
      "confidence": "low|medium|high",
      "wrong_hypotheses": ["<any wrong paths you explored>"]
    }\
"""

TOOLS_B_EXTRA = """

  causality_reflection
    Show Method.invoke() and Constructor.newInstance() dispatch targets observed at runtime.
    Reveals which concrete class was actually called through reflection.
    Args: {"class_filter": "<optional class name prefix>"}
    Example: {"class_filter": "com/example/demo"}

  causality_polymorphic
    Show polymorphic (invokeinterface) dispatch targets observed at runtime.
    Reveals which concrete class was called at interface call sites.
    Args: {"class_filter": "<optional class name prefix>"}

  causality_proxies
    Show proxy/CGLIB callsites and their real implementation targets.
    Reveals which real class sits behind a proxy chain.
    Args: {"class_filter": "<optional class name prefix>"}

  causality_summary
    Overview of all runtime dispatch events.
    Args: {}

  causality_hidden
    List all hidden classes (lambda implementations, generated classes using defineHiddenClass).
    Returns runtime_name (+0x... suffix), stable artifact_crc (cross-run identity), loader_id,
    and has_artifact flag. Use when a stack trace shows Lambda$N/0x... and you need the source
    method or bytecode.
    Args: {}

  causality_chain
    Get the full causality chain for a specific callsite.
    Returns all observed targets, edge types, and adapter chain details from one dispatch site.
    Use when you know the callsite BCI and want to see every concrete class that executed there.
    Args: {"class": "<source class>", "method": "<source method>", "bci": <int>}
    Example: {"class": "com/example/demo/engine/DispatchEngine", "method": "dispatch", "bci": 73}

  artifact_lookup
    Look up the bytecode artifact record for a class captured during the run.
    Returns: class name, CRC (stable identity), size in bytes, kind (final/original),
             artifact_path on disk, and _exists flag.
    Use before artifact_javap to confirm the artifact is available.
    Args: {"class": "<internal class name>"}
    Example: {"class": "com/example/demo/engine/proc/Processor037"}

  artifact_javap
    Disassemble a captured class artifact using javap -c -p -verbose.
    Returns the full javap output including ConstantValue entries, per-instruction operands,
    and constant pool references. This is the only way to read a generated/lambda class body
    that has no .java source file. Also confirms exact constant values in regular classes.
    Args: {"class": "<internal class name>"}
    Example: {"class": "com/example/demo/engine/proc/Processor037"}\
"""

SYSTEM_PROMPT_A = f"""\
You are an expert Java debugging agent. A bug has been reported in a Spring Boot application.
You will investigate it by using the provided tools. Do NOT guess or assume — gather evidence first.

MANDATORY INVESTIGATION PROTOCOL
Step 1: Call search_files to list available source files. This is your required first action.
Step 2: Read the files that are likely involved in the bug path (controller, service, config).
Step 3: Use grep to search for specific class names, method names, or config keys.
Step 4: When you have read the actual source files and identified the exact line, call submit_diagnosis.

You MUST call search_files as your very first action before anything else.

{TOOLS_A_DESC}

FORMAT YOUR RESPONSE AS:
Thought: [your current reasoning and next step]
Action: [tool_name]
Args: [JSON object with arguments]

When you have read the actual source and are certain:
Thought: [final reasoning citing specific file and line you read]
Action: submit_diagnosis
Args: {{...}}
"""

SYSTEM_PROMPT_B = f"""\
You are an expert Java debugging agent with access to runtime causality data.
A bug has been reported in a Spring Boot application. Investigate using the provided tools.

The causality tools show what ACTUALLY executed at runtime — not what static analysis predicts.
They resolve reflection targets, proxy chains, and polymorphic dispatch that are invisible to grep.

MANDATORY INVESTIGATION PROTOCOL
Step 1: Call a causality tool (causality_reflection, causality_polymorphic, or causality_proxies).
        This is your required first action — it immediately shows the runtime execution path.
Step 2: Examine the causality result to identify the exact class that executed.
Step 3a: If the identified class has a .java source file: call read_file to read it and confirm the bug.
Step 3b: IF the identified class is GENERATED (indicated by any of: a $ in the class name,
         the words "generated", "ByteBuddy", "no .java source", or "no source file" in the
         causality output) — you MUST call artifact_javap on that class instead of read_file.
         Generated classes have NO .java source. artifact_javap is the ONLY tool that can read them.
         Skipping artifact_javap on a generated class will lead to a wrong diagnosis.
Step 4: Call submit_diagnosis with the exact file and line you confirmed via tool output.

CRITICAL: When causality reveals a generated class ($-in-name or "generated"/"ByteBuddy" note):
  - Do NOT call read_file — there is no source to read.
  - Do NOT guess or infer the constant value — call artifact_javap to get the ground truth.
  - artifact_javap shows the exact bytecode constant baked into the generated class.

You MUST call a causality tool as your very first action.

{TOOLS_A_DESC}{TOOLS_B_EXTRA}

FORMAT YOUR RESPONSE AS:
Thought: [your current reasoning and next step]
Action: [tool_name]
Args: [JSON object with arguments]

When you have confirmed the bug via tool output:
Thought: [final reasoning citing the specific tool output and file/line]
Action: submit_diagnosis
Args: {{...}}
"""

# ─── Tool implementations ────────────────────────────────────────────────────────

def tool_search_files(args: dict,
                      app_src: Path = APP_SRC,
                      app_res: Path = APP_RES) -> str:
    pattern = args.get("pattern", "**/*.java")
    results = []

    # Derive the display prefix from the actual src path
    java_prefix = "src/main/java/" + "/".join(app_src.parts[
        next(i for i, p in enumerate(app_src.parts) if p == "src") :
    ]) + "/"

    # Search Java sources
    for p in sorted(app_src.rglob(pattern.replace("**/*.java", "**/*.java"))):
        rel = str(p.relative_to(app_src))
        results.append(f"{java_prefix}{rel}")

    # Search resources if pattern doesn't look Java-specific
    if "*.java" not in pattern:
        for p in sorted(app_res.rglob(pattern)):
            results.append(f"src/main/resources/{p.relative_to(app_res)}")

    # Always try both for generic globs
    if "**" in pattern or not results:
        try:
            for p in sorted(app_src.rglob("**/*.java")):
                rel = f"{java_prefix}{p.relative_to(app_src)}"
                if rel not in results:
                    results.append(rel)
            for p in sorted(app_res.rglob("*")):
                if p.is_file():
                    rel = f"src/main/resources/{p.relative_to(app_res)}"
                    if rel not in results:
                        results.append(rel)
        except Exception:
            pass

    if not results:
        return f"No files found matching pattern '{pattern}'"
    return "\n".join(results[:100])  # cap at 100


def _resolve_path(path: str,
                  app_src: Path = APP_SRC,
                  app_res: Path = APP_RES,
                  app_root: Path = APP_ROOT) -> Path | None:
    """Resolve a file path from agent input to an actual filesystem path."""
    path = path.strip().lstrip("/")

    # Strip any known src-relative prefix so agents can use either form
    for prefix in ("src/main/java/", "src/main/resources/"):
        if path.startswith(prefix):
            path = path[len(prefix):]
            break

    candidates = [
        app_src / path,
        app_res / path,
        app_root / path,
        # Handle fully-qualified paths like com/example/truth/Foo.java
        app_src.parent / path,
    ]

    for c in candidates:
        if c.exists() and c.is_file():
            return c

    # Fuzzy: basename match
    basename = Path(path).name
    for p in list(app_src.rglob("*.java")) + list(app_res.rglob("*")):
        if p.name == basename and p.is_file():
            return p

    return None


def tool_read_file(args: dict,
                   app_src: Path = APP_SRC,
                   app_res: Path = APP_RES,
                   app_root: Path = APP_ROOT) -> str:
    path = args.get("path", "")
    resolved = _resolve_path(path, app_src, app_res, app_root)
    if resolved is None:
        return f"ERROR: file not found: '{path}'. Use search_files to list available files."
    lines = resolved.read_text().split("\n")
    numbered = "\n".join(f"{i+1:4}: {line}" for i, line in enumerate(lines))
    return f"=== {path} ===\n{numbered}"


def tool_grep(args: dict,
              app_src: Path = APP_SRC,
              app_res: Path = APP_RES) -> str:
    pattern = args.get("pattern", "")
    subpath = args.get("path", "")
    if not pattern:
        return "ERROR: grep requires a 'pattern' argument"

    search_root = app_src / subpath if subpath else app_src
    cmd = ["grep", "-r", "-n", "--include=*.java", pattern, str(search_root)]

    # Also search resources
    res_root = app_res / subpath if subpath else app_res
    cmd2 = ["grep", "-r", "-n", pattern, str(res_root)]

    output = []
    for c in [cmd, cmd2]:
        try:
            r = subprocess.run(c, capture_output=True, text=True, timeout=10)
            if r.stdout:
                output.append(r.stdout)
        except Exception:
            pass

    result = "\n".join(output)
    if not result.strip():
        return f"No matches for '{pattern}'"

    # Make paths relative and readable
    result = result.replace(str(app_src) + "/", "")
    result = result.replace(str(app_res) + "/", "resources:")
    return result[:4000]  # cap output


def tool_causality(tool_name: str, args: dict, causality_data: str) -> str:
    """Return the section of causality_data matching the requested tool.

    Sections are delimited by lines starting with '=== /<key>'.
    If no matching section is found, return the full data (backward-compatible fallback).
    """
    SECTION_KEYS = {
        "causality_reflection":  "/causality/reflection",
        "causality_polymorphic": "/causality/polymorphic",
        "causality_proxies":     "/causality/proxies",
        "causality_summary":     "/causality/summary",
        "causality_hidden":      "/causality/hidden",
        "causality_chain":       "/causality/chain",
        "artifact_lookup":       "/api/artifact",
        "artifact_javap":        "/javap",
    }
    key = SECTION_KEYS.get(tool_name)
    if key is None:
        return causality_data  # unknown tool — return everything

    lines = causality_data.split("\n")
    section: list[str] = []
    inside = False
    for line in lines:
        is_header = line.startswith("===")
        if is_header and key in line:
            inside = True
            section.append(line)
            continue
        if inside:
            if is_header:
                break  # next section begins
            section.append(line)

    if section:
        return "\n".join(section).strip()
    # Section not present — return full data so agent still gets something useful
    return causality_data

# ─── Conversation and prompt building ─────────────────────────────────────────

def build_prompt(system_prompt: str, history: list[dict], next_input: str) -> str:
    """Build the full prompt for one claude -p call."""
    parts = [f"SYSTEM INSTRUCTIONS:\n{system_prompt}\n"]

    if history:
        parts.append("CONVERSATION HISTORY:")
        for entry in history:
            role = entry["role"]
            content = entry["content"]
            if role == "user":
                parts.append(f"\n[User input]:\n{content}")
            elif role == "assistant":
                parts.append(f"\n[Your previous response]:\n{content}")
            elif role == "tool_result":
                parts.append(f"\n[Tool result for {entry.get('tool','?')}]:\n{content}")

    parts.append(f"\n[Current input]:\n{next_input}")
    parts.append(
        "\nRespond with your Thought and Action (tool call or submit_diagnosis). "
        "Use exactly the format: Thought: ... / Action: ... / Args: ..."
    )
    return "\n".join(parts)


def _raw_decode_json(text: str) -> dict | None:
    """Find and parse the first complete JSON object in text using the JSON decoder.
    Uses raw_decode so it correctly handles } inside string values."""
    decoder = json.JSONDecoder()
    idx = text.find("{")
    while idx != -1:
        try:
            obj, _ = decoder.raw_decode(text, idx)
            if isinstance(obj, dict):
                return obj
        except json.JSONDecodeError:
            pass
        idx = text.find("{", idx + 1)
    return None


def parse_action(response: str) -> dict | None:
    """Extract Action + Args from a ReAct-format response.
    Handles inline JSON (Args: {...}), multi-line JSON, and markdown code blocks.
    Uses json.JSONDecoder.raw_decode to correctly parse } inside string values.
    """
    action_m = re.search(r'Action:\s*(\w+)', response)
    if not action_m:
        return None

    tool = action_m.group(1).strip()
    tail = response[action_m.end():]   # everything after the Action: line

    # Strip markdown code fences around the JSON
    tail_clean = re.sub(r'```(?:json)?\s*', '', tail)
    tail_clean = re.sub(r'```', '', tail_clean)

    args = _raw_decode_json(tail_clean) or {}
    return {"tool": tool, "args": args}


def run_claude_p(prompt: str, model: str) -> tuple[str, dict]:
    start = time.monotonic()
    # Run from APP_ROOT so the agent's file access is scoped to the demo app,
    # not the benchmark directory (which contains ground truth in bugs_v2.py).
    result = subprocess.run(
        ["claude", "-p", "--model", model, "--output-format", "json", prompt],
        capture_output=True, text=True, timeout=300,
        cwd=str(APP_ROOT)
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


# ─── Agent runner ────────────────────────────────────────────────────────────────

def _resolve_scenario_paths(scenario: dict) -> tuple[Path, Path, Path]:
    """Return (app_root, app_src, app_res) for a scenario."""
    app_root_rel = scenario.get("app_root", "tools/demo-buggy-app")
    java_pkg     = scenario.get("java_package", "com/example/demo")
    app_root = REPO_ROOT / app_root_rel
    app_src  = app_root / "src/main/java" / java_pkg
    app_res  = app_root / "src/main/resources"
    return app_root, app_src, app_res


def run_agent(scenario: dict, agent_id: str, model: str) -> dict:
    bug_id = scenario["id"]
    system = SYSTEM_PROMPT_A if agent_id == "A" else SYSTEM_PROMPT_B
    causality = scenario.get("causality", "")

    # Resolve app paths for this scenario (supports multi-app harness)
    scenario_app_root, scenario_app_src, scenario_app_res = _resolve_scenario_paths(scenario)

    history: list[dict] = []
    tool_calls_log: list[dict] = []
    diagnosis = None
    gate_used = False   # Whether the one-time gate has already fired
    total_tokens_in = 0
    total_tokens_out = 0
    total_elapsed = 0.0
    start_ts = datetime.now(timezone.utc).isoformat()

    print(f"\n{'='*60}")
    print(f"  Agent {agent_id} | {scenario['title']} | model={model}")
    print(f"{'='*60}")

    current_input = scenario["initial_msg"]

    for turn in range(MAX_TURNS):
        prompt = build_prompt(system, history, current_input)
        print(f"  [Turn {turn+1}] prompt={len(prompt)} chars ... ", end="", flush=True)

        try:
            response, meta = run_claude_p(prompt, model)
        except RuntimeError as e:
            print(f"ERROR: {e}")
            break

        elapsed = meta.get("elapsed_s", 0)
        total_elapsed += elapsed
        total_tokens_in  += (meta.get("usage") or {}).get("input_tokens", 0)
        total_tokens_out += (meta.get("usage") or {}).get("output_tokens", 0)

        history.append({"role": "user", "content": current_input})
        history.append({"role": "assistant", "content": response})

        action = parse_action(response)

        if action is None:
            print(f"{elapsed}s [no action parsed, prompting to continue]")
            current_input = "Please continue. Use a tool (Action: ...) to gather more information."
            continue

        tool_name = action["tool"]
        tool_args = action["args"]
        print(f"{elapsed}s  Action={tool_name}")

        # Persistent gate: block submit_diagnosis until at least one investigation
        # tool has been called. Re-fires every attempt until the requirement is met.
        # After 3 redirections with no tool use, allow through to avoid infinite loops.
        gate_redirects = sum(
            1 for h in history if h.get("tool") == "submit_diagnosis_deferred"
        )
        if tool_name == "submit_diagnosis" and not tool_calls_log and gate_redirects < 3:
            if agent_id == "B":
                first_tool = (
                    "causality_reflection, causality_polymorphic, causality_proxies, "
                    "causality_hidden, causality_chain, artifact_lookup, or artifact_javap"
                )
                gate_detail = (
                    "Call a causality or artifact tool RIGHT NOW. "
                    "It returns the exact class that executed at runtime or its bytecode. "
                    "Do NOT submit a diagnosis until you have called at least one causality/artifact tool."
                )
            else:
                first_tool = "search_files or read_file or grep"
                gate_detail = (
                    "Call search_files, read_file, or grep RIGHT NOW to examine the source code. "
                    "Do NOT submit a diagnosis until you have read at least one file."
                )
            gate_msg = (
                f"BLOCKED: You have not investigated yet. "
                f"You MUST call {first_tool} before submitting a diagnosis. "
                f"{gate_detail}"
            )
            print(f"{elapsed}s  [gate redirect #{gate_redirects+1}: diagnosis blocked, no tools used yet]")
            history.append({"role": "tool_result", "content": gate_msg, "tool": "submit_diagnosis_deferred"})
            current_input = gate_msg
            continue

        tool_calls_log.append({
            "tool":  tool_name,
            "input": tool_args,
            "turn":  turn + 1,
        })

        if tool_name == "submit_diagnosis":
            diagnosis = tool_args
            print(f"  DIAGNOSIS: file={tool_args.get('root_cause_file','?')} "
                  f"line={tool_args.get('root_cause_line','?')} "
                  f"conf={tool_args.get('confidence','?')}")
            break

        # Execute tool
        if tool_name == "search_files":
            tool_result = tool_search_files(tool_args, scenario_app_src, scenario_app_res)
        elif tool_name == "read_file":
            tool_result = tool_read_file(tool_args, scenario_app_src, scenario_app_res, scenario_app_root)
        elif tool_name == "grep":
            tool_result = tool_grep(tool_args, scenario_app_src, scenario_app_res)
        elif tool_name in (
            "causality_reflection", "causality_polymorphic", "causality_proxies",
            "causality_summary", "causality_chain", "causality_hidden",
            "artifact_lookup", "artifact_javap",
        ):
            if agent_id == "A":
                tool_result = "ERROR: causality and artifact tools are not available for this agent."
            else:
                tool_result = tool_causality(tool_name, tool_args, causality)
        else:
            available = "search_files, read_file, grep, submit_diagnosis"
            if agent_id == "B":
                available += (
                    ", causality_reflection, causality_polymorphic, causality_proxies, "
                    "causality_summary, causality_hidden, causality_chain, "
                    "artifact_lookup, artifact_javap"
                )
            tool_result = f"ERROR: unknown tool '{tool_name}'. Available: {available}"

        history.append({"role": "tool_result", "content": tool_result, "tool": tool_name})
        current_input = f"Tool '{tool_name}' returned the above result. Continue your investigation."

    end_ts = datetime.now(timezone.utc).isoformat()

    if diagnosis is None:
        print(f"  WARNING: agent did not submit a diagnosis after {MAX_TURNS} turns")

    # Compute efficiency metrics
    file_reads    = [tc for tc in tool_calls_log if tc["tool"] == "read_file"]
    searches      = [tc for tc in tool_calls_log if tc["tool"] in ("search_files", "grep")]
    causality_ops = [tc for tc in tool_calls_log if tc["tool"].startswith("causality")]
    artifact_ops  = [tc for tc in tool_calls_log if tc["tool"].startswith("artifact_")]
    unique_files  = len(set(
        tc["input"].get("path", "") for tc in file_reads if isinstance(tc.get("input"), dict)
    ))

    return {
        "agent":          agent_id,
        "bug":            bug_id,
        "model":          model,
        "mode":           "multi_turn",
        "start_ts":       start_ts,
        "end_ts":         end_ts,
        "elapsed_s":      round(total_elapsed, 1),
        "total_turns":    len(tool_calls_log),
        "input_tokens":   total_tokens_in,
        "output_tokens":  total_tokens_out,
        "tool_calls":     tool_calls_log,
        "diagnosis":      diagnosis,
        "raw_history":    history,
        "completed":      diagnosis is not None,
        # Convenience summaries for reporting
        "summary": {
            "file_reads":        len(file_reads),
            "unique_files_read": unique_files,
            "searches":          len(searches),
            "causality_calls":   len(causality_ops),
            "artifact_calls":    len(artifact_ops),
            "wrong_hypotheses":  len((diagnosis or {}).get("wrong_hypotheses", []) or []),
        },
    }

# ─── Main ────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bugs", nargs="+", choices=list(SCENARIOS_ALL.keys()))
    ap.add_argument("--agent", choices=["A", "B", "both"], default="both")
    ap.add_argument("--model", default="claude-sonnet-4-6")
    args = ap.parse_args()

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    agents = ["A", "B"] if args.agent == "both" else [args.agent]
    all_results = {}

    for bug_id in args.bugs:
        scenario    = SCENARIOS_ALL[bug_id]
        bug_results = {}

        for agent_id in agents:
            ts       = datetime.now().strftime("%Y%m%d_%H%M%S")
            out_path = RESULTS_DIR / f"v2_{bug_id}_{agent_id}_{ts}.json"

            trace  = run_agent(scenario, agent_id, args.model)
            scored = score_run(trace, scenario["ground_truth"])
            result = {"trace": trace, "score": scored}
            out_path.write_text(json.dumps(result, indent=2))

            s = trace["summary"]
            print(f"\n  Score: {scored['total']}/{scored['max_score']} ({scored['pct']}%)")
            print(f"    file={scored['file_score']}/3  line={scored['line_score']}/3  "
                  f"patch={scored['patch_score']}/5  explain={scored['explain_score']}/5")
            print(f"  Turns:  {trace['total_turns']}  "
                  f"reads={s['file_reads']} unique={s['unique_files_read']}  "
                  f"searches={s['searches']}  "
                  f"causality={s['causality_calls']}  "
                  f"artifact={s['artifact_calls']}  "
                  f"wrong_hypos={s['wrong_hypotheses']}")

            bug_results[agent_id] = result

        if "A" in bug_results and "B" in bug_results:
            sa = bug_results["A"]["score"]
            sb = bug_results["B"]["score"]
            ta = bug_results["A"]["trace"]
            tb = bug_results["B"]["trace"]
            comparison = compare(sa, sb)
            bug_results["comparison"] = comparison

            print(f"\n  ── Head-to-head ──")
            print(f"  Winner:       {comparison['winner']}")
            print(f"  Score delta:  {comparison['score_delta']:+d} (B minus A)")
            print(f"  Turns:        A={ta['total_turns']}  B={tb['total_turns']}  saved={comparison['turns_saved']}")
            print(f"  Files read:   A={ta['summary']['unique_files_read']}  B={tb['summary']['unique_files_read']}  saved={comparison['files_saved']}")
            print(f"  Causality:    B made {tb['summary']['causality_calls']} causality call(s), "
                  f"{tb['summary']['artifact_calls']} artifact call(s)")
            print(f"  {comparison['explanation']}")

        all_results[bug_id] = bug_results

    combined = RESULTS_DIR / "benchmark_result_v2.json"
    combined.write_text(json.dumps(all_results, indent=2))
    print(f"\nAll results: {combined}")

    # Summary table
    print("\n" + "="*70)
    print("  BENCHMARK SUMMARY (v2 — multi-turn discovery + artifact access)")
    print("="*70)
    print(f"  {'Bug':<10} {'A score':>8} {'B score':>8} {'Delta':>6} "
          f"{'A turns':>8} {'B turns':>8} {'A files':>8} {'B files':>8} {'Winner':>8}")
    print(f"  {'-'*10} {'-'*8} {'-'*8} {'-'*6} {'-'*8} {'-'*8} {'-'*8} {'-'*8} {'-'*8}")
    for bug_id, br in all_results.items():
        sa    = br.get("A", {}).get("score", {})
        sb    = br.get("B", {}).get("score", {})
        ta    = br.get("A", {}).get("trace", {})
        tb    = br.get("B", {}).get("trace", {})
        cmp   = br.get("comparison", {})
        a_sc  = f"{sa.get('total','?')}/{sa.get('max_score','?')}" if sa else "—"
        b_sc  = f"{sb.get('total','?')}/{sb.get('max_score','?')}" if sb else "—"
        delta = cmp.get("score_delta", "?") if cmp else "?"
        winner = cmp.get("winner", "?") if cmp else "?"
        a_t   = ta.get("total_turns", "?")
        b_t   = tb.get("total_turns", "?")
        a_f   = ta.get("summary", {}).get("unique_files_read", "?")
        b_f   = tb.get("summary", {}).get("unique_files_read", "?")
        print(f"  {bug_id:<10} {a_sc:>8} {b_sc:>8} {str(delta):>6} "
              f"{str(a_t):>8} {str(b_t):>8} {str(a_f):>8} {str(b_f):>8} {winner:>8}")
    print("="*70)

    return all_results


if __name__ == "__main__":
    main()
