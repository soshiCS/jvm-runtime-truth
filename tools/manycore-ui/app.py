"""
Runtime Truth UI — Flask backend.
Local-only by default; set RT_DEMO_TOKEN for HTTP Basic Auth gating.
"""
import base64
import io
import json
import os
import subprocess
import zipfile
from pathlib import Path

from flask import Flask, Response, abort, jsonify, request, send_from_directory
from indexer import callsite_summary, find_best_artifact, load_and_index, validate_run
from runner import DEFAULT_JDK, RunManager
from graph_builder import (
    build_graph, query_chain,
    list_blocked_callsites, list_staticizable_callsites, list_orphan_runtime_targets,
    ET_CALLSITE_TARGET, ET_LAMBDA_BODY,
    SR_MULTI_TARGET, SR_DIRECT, SR_ADAPTER_MODELED,
)

# ---------------------------------------------------------------------------
DEFAULT_JAVAP = os.path.join(DEFAULT_JDK, "bin", "javap")

app      = Flask(__name__, static_folder="static", static_url_path="")
run_mgr  = RunManager()
_idx_cache: dict = {}

# Max upload: 512 MB (generous for large JARs)
app.config["MAX_CONTENT_LENGTH"] = 512 * 1024 * 1024

# Optional demo token — set RT_DEMO_TOKEN env var to require Basic Auth.
# Username is always "demo", password is the token value.
_DEMO_TOKEN = os.environ.get("RT_DEMO_TOKEN", "")


@app.before_request
def _check_auth():
    if not _DEMO_TOKEN:
        return  # local use, no auth required
    auth_header = request.headers.get("Authorization", "")
    if auth_header.startswith("Basic "):
        try:
            decoded = base64.b64decode(auth_header[6:]).decode("utf-8")
            _, password = decoded.split(":", 1)
            if password == _DEMO_TOKEN:
                return  # authorised
        except Exception:
            pass
    return Response(
        "Runtime Truth UI — authentication required",
        401,
        {"WWW-Authenticate": 'Basic realm="Runtime Truth Demo"'},
    )


# ---------------------------------------------------------------------------
# Static / root
# ---------------------------------------------------------------------------

@app.route("/")
def root():
    return send_from_directory("static", "index.html")


# ---------------------------------------------------------------------------
# Run management
# ---------------------------------------------------------------------------

@app.route("/api/run", methods=["POST"])
def start_run():
    if "jar" not in request.files:
        return jsonify({"error": "no jar file uploaded"}), 400
    jar_bytes = request.files["jar"].read()
    config    = json.loads(request.form.get("config", "{}"))
    run_id    = run_mgr.start_run(config, jar_bytes)
    return jsonify({"run_id": run_id})


@app.route("/api/runs")
def list_runs():
    return jsonify(run_mgr.list_runs())


@app.route("/api/runs/ingest", methods=["POST"])
def ingest_run():
    """Register a pre-captured run directory (with runtime_targets.jsonl) as a completed run."""
    body = request.get_json(silent=True) or {}
    label   = body.get("label", "ingested-run")
    run_dir = body.get("run_dir", "")
    if not run_dir or not os.path.isdir(run_dir):
        return jsonify({"error": f"run_dir not found: {run_dir}"}), 400
    jsonl = os.path.join(run_dir, "runtime_targets.jsonl")
    if not os.path.isfile(jsonl):
        return jsonify({"error": f"runtime_targets.jsonl not found in {run_dir}"}), 400
    run_id = run_mgr.ingest_run(label, run_dir)
    return jsonify({"run_id": run_id, "label": label, "run_dir": run_dir})


@app.route("/api/runs/<run_id>")
def get_run(run_id):
    run = run_mgr.get_run(run_id)
    if not run:
        return jsonify({"error": "not found"}), 404

    run_dir = run.get("run_dir", "")
    jsonl   = os.path.join(run_dir, "runtime_targets.jsonl") if run_dir else None
    run["has_jsonl"] = bool(jsonl and os.path.isfile(jsonl))

    summary_p = Path(run_dir) / "summary.json" if run_dir else None
    if summary_p and summary_p.exists():
        try:
            run["summary"] = json.loads(summary_p.read_text())
        except Exception:
            pass

    run["user_prefixes_list"] = _parse_prefixes(run.get("config", {}))

    # Lightweight JSONL stats if available
    if run.get("has_jsonl") and run.get("status") == "done":
        idx = _get_index(run_id)
        if idx:
            prefixes = _parse_prefixes(run.get("config", {}))
            all_cs = idx["all_callsites"]
            user_cs_count = sum(
                1 for cs in all_cs
                if cs["record"] != "diagnostic"
                and (not prefixes or any(cs["source_class"].startswith(p) for p in prefixes))
            ) if prefixes else None
            run["stats"] = {
                "total_records":       idx["total_records"],
                "class_count":         len(idx["classes"]),
                "callsite_count":      sum(1 for cs in all_cs if cs["record"] != "diagnostic"),
                "user_callsite_count": user_cs_count,
                "diagnostic_count":    len(idx["diagnostics"]),
                "export_summary":      idx.get("export_summary"),
            }
    return jsonify(run)


# ---------------------------------------------------------------------------
# Index / classes
# ---------------------------------------------------------------------------

@app.route("/api/runs/<run_id>/classes")
def get_classes(run_id):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify([])
    return jsonify(idx["classes"])


@app.route("/api/runs/<run_id>/classes/<path:class_name>/methods")
def get_methods(run_id, class_name):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify([])
    meths = idx["methods_by_class"].get(class_name, [])
    return jsonify(meths)


@app.route("/api/runs/<run_id>/classes/<path:class_name>/callsites")
def get_callsites(run_id, class_name):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify([])

    result = []
    for key, css in idx["callsites_by"].items():
        cls, _ = key.split("||", 1)
        if cls == class_name:
            for cs in css:
                result.append(callsite_summary(cs))

    result.sort(key=lambda c: (c.get("source_method", ""), c.get("source_bci", 0)))
    return jsonify(result)


@app.route("/api/runs/<run_id>/callsite/<int:cs_idx>")
def get_callsite_detail(run_id, cs_idx):
    idx = _get_index(run_id)
    if idx is None or cs_idx >= len(idx["all_callsites"]):
        return jsonify({"error": "not found"}), 404
    return jsonify(idx["all_callsites"][cs_idx]["raw"])


# ---------------------------------------------------------------------------
# Artifacts
# ---------------------------------------------------------------------------

@app.route("/api/runs/<run_id>/artifact")
def get_artifact(run_id):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify(None)
    cls      = request.args.get("class", "")
    loader   = request.args.get("loader_id")
    artifact = find_best_artifact(idx, cls, loader)
    return jsonify(artifact)


# ---------------------------------------------------------------------------
# Bytecode (javap)
# ---------------------------------------------------------------------------

@app.route("/api/runs/<run_id>/bytecode")
def get_bytecode(run_id):
    run = run_mgr.get_run(run_id)
    if not run:
        return jsonify({"error": "run not found"}), 404

    artifact_path = request.args.get("artifact_path", "")
    if not artifact_path:
        return jsonify({"error": "artifact_path required"}), 400
    if not os.path.isfile(artifact_path):
        return jsonify({"error": f"file not found: {artifact_path}"}), 404

    javap = (run.get("config") or {}).get("javap_path", "").strip() or DEFAULT_JAVAP
    try:
        result = subprocess.run(
            [javap, "-c", "-p", "-verbose", artifact_path],
            capture_output=True, text=True, timeout=15,
        )
        return jsonify({
            "output":        result.stdout,
            "stderr":        result.stderr,
            "artifact_path": artifact_path,
            "exit_code":     result.returncode,
        })
    except subprocess.TimeoutExpired:
        return jsonify({"error": "javap timed out"}), 500
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


# ---------------------------------------------------------------------------
# Diagnostics
# ---------------------------------------------------------------------------

@app.route("/api/runs/<run_id>/diagnostics")
def get_diagnostics(run_id):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify([])
    return jsonify([callsite_summary(d) for d in idx["diagnostics"]])


# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

@app.route("/api/runs/<run_id>/validate")
def do_validate(run_id):
    run = run_mgr.get_run(run_id)
    if not run:
        return jsonify({"error": "not found"}), 404
    idx = _get_index(run_id)
    if idx is None:
        return jsonify({"error": "index not available — run may still be in progress"}), 404
    return jsonify(validate_run(run["run_dir"], idx))


# ---------------------------------------------------------------------------
# Raw output
# ---------------------------------------------------------------------------

@app.route("/api/runs/<run_id>/stdout")
def get_stdout(run_id):
    return _read_run_file(run_id, "stdout.txt")


@app.route("/api/runs/<run_id>/stderr")
def get_stderr(run_id):
    return _read_run_file(run_id, "stderr.txt")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _get_index(run_id: str) -> dict | None:
    if run_id in _idx_cache:
        return _idx_cache[run_id]
    run = run_mgr.get_run(run_id)
    if not run or not run.get("run_dir"):
        return None
    jsonl = os.path.join(run["run_dir"], "runtime_targets.jsonl")
    if not os.path.isfile(jsonl):
        return None
    try:
        user_prefixes = _parse_prefixes(run.get("config", {}))
        idx = load_and_index(jsonl, user_prefixes=user_prefixes)
        _idx_cache[run_id] = idx
        return idx
    except Exception as exc:
        print(f"[indexer] error for run {run_id}: {exc}")
        return None


def _read_run_file(run_id: str, filename: str):
    run = run_mgr.get_run(run_id)
    if not run:
        abort(404)
    p = Path(run["run_dir"]) / filename
    if not p.exists():
        return ("", 200)
    return Response(p.read_text(errors="replace"), mimetype="text/plain")


def _parse_prefixes(config: dict) -> list:
    raw = (config.get("user_prefixes", "") or "").strip()
    if raw:
        return [p.strip() for p in raw.replace(",", "\n").splitlines() if p.strip()]
    old = (config.get("rewriter_prefix", "") or "").strip()
    return [old] if old else []


def _filter_user_records(jsonl_path: str, prefixes: list) -> str:
    if not prefixes:
        return ""
    out = []
    try:
        with open(jsonl_path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                    cls = rec.get("source_class", "")
                    if any(cls.startswith(p) for p in prefixes):
                        out.append(line)
                except Exception:
                    pass
    except Exception:
        pass
    return "\n".join(out) + ("\n" if out else "")


def _make_llm_readme(run: dict, idx: dict | None, prefixes: list, validation: dict | None) -> str:
    lines = [
        "# Runtime Truth Run Export",
        "",
        f"Run ID: {run.get('run_id', '?')}",
        f"Status: {run.get('status', '?')}",
        f"Exit code: {run.get('exit_code', '?')}",
        "",
        "## Files in this archive",
        "",
        "| File | Description |",
        "|------|-------------|",
        "| runtime_targets.jsonl | Full JVM runtime analysis — callsites, targets, adapter graphs, diagnostics |",
        "| summary.json | Run metadata (command, exit code, timeout, paths) |",
        "| stdout.txt | Program standard output |",
        "| stderr.txt | Program standard error (includes JVM diagnostics) |",
        "| artifacts/ | Bytecode dumps (.class files) captured during run |",
    ]
    if prefixes:
        lines.append("| filtered_user_records.jsonl | JSONL records filtered to user/app classes |")
    lines.append("| validation_report.json | 9-point validation results |")
    if prefixes:
        lines.extend(["", "## User/App prefixes"])
        for p in prefixes:
            lines.append(f"- `{p}`")
    lines.extend(["", "## Stats"])
    if idx:
        lines.extend([
            f"- total_records: {idx['total_records']}",
            f"- class_count: {len(idx['classes'])}",
            f"- callsite_count: {sum(1 for cs in idx['all_callsites'] if cs['record'] != 'diagnostic')}",
            f"- diagnostic_count: {len(idx['diagnostics'])}",
        ])
    else:
        lines.append("- (index not available)")
    if validation:
        overall = "PASS" if validation.get("overall") else "FAIL"
        lines.extend([
            "",
            "## Validation",
            f"Overall: {overall} ({validation.get('pass', 0)} pass, {validation.get('fail', 0)} fail)",
        ])
    lines.extend([
        "",
        "## JSONL record types",
        "",
        "Each line is a JSON object with a `record` field:",
        "- `callsite_target` — exact method target for a callsite (invokedynamic, MH invoke, reflection)",
        "- `callsite_adapter_graph` — MethodHandle adapter chain with classified nodes",
        "- `callsite_target_set` — multi-target combinator (guardWithTest, catchException, tryFinally)",
        "- `diagnostic` — callsite where exact resolution was not possible",
        "- `bytecode_artifact` — a captured .class file (path relative to artifacts/)",
        "- `export_summary` — overall export metadata (last record)",
    ])
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# Downloads
# ---------------------------------------------------------------------------

@app.route("/api/runs/<run_id>/download/jsonl")
def download_jsonl(run_id):
    run = run_mgr.get_run(run_id)
    if not run:
        abort(404)
    jsonl_path = os.path.join(run["run_dir"], "runtime_targets.jsonl")
    if not os.path.isfile(jsonl_path):
        abort(404)
    with open(jsonl_path, "rb") as f:
        content = f.read()
    return Response(
        content,
        mimetype="application/x-ndjson",
        headers={"Content-Disposition": f"attachment; filename=runtime_targets_{run_id}.jsonl"},
    )


@app.route("/api/runs/<run_id>/download/artifacts")
def download_artifacts(run_id):
    run = run_mgr.get_run(run_id)
    if not run:
        abort(404)
    artifacts_dir = os.path.join(run["run_dir"], "artifacts")
    if not os.path.isdir(artifacts_dir):
        abort(404)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, _, files in os.walk(artifacts_dir):
            for fname in files:
                fpath = os.path.join(root, fname)
                zf.write(fpath, os.path.relpath(fpath, artifacts_dir))
    buf.seek(0)
    return Response(
        buf.read(),
        mimetype="application/zip",
        headers={"Content-Disposition": f"attachment; filename=artifacts_{run_id}.zip"},
    )


@app.route("/api/runs/<run_id>/download/full")
def download_full(run_id):
    run = run_mgr.get_run(run_id)
    if not run:
        abort(404)
    run_dir  = Path(run["run_dir"])
    idx      = _get_index(run_id)
    prefixes = _parse_prefixes(run.get("config", {}))

    validation = None
    if idx:
        try:
            validation = validate_run(run["run_dir"], idx)
        except Exception:
            pass

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as zf:
        for fname in ("runtime_targets.jsonl", "summary.json", "stdout.txt", "stderr.txt"):
            fpath = run_dir / fname
            if fpath.exists():
                zf.write(str(fpath), fname)

        artifacts_dir = run_dir / "artifacts"
        if artifacts_dir.is_dir():
            for root, _, files in os.walk(str(artifacts_dir)):
                for fname in files:
                    fpath_str = os.path.join(root, fname)
                    zf.write(fpath_str, "artifacts/" + os.path.relpath(fpath_str, str(artifacts_dir)))

        if validation:
            zf.writestr("validation_report.json", json.dumps(validation, indent=2))

        if prefixes:
            jsonl_path = str(run_dir / "runtime_targets.jsonl")
            filtered = _filter_user_records(jsonl_path, prefixes)
            if filtered:
                zf.writestr("filtered_user_records.jsonl", filtered)

        zf.writestr("llm_readme.md", _make_llm_readme(run, idx, prefixes, validation))

    buf.seek(0)
    return Response(
        buf.read(),
        mimetype="application/zip",
        headers={"Content-Disposition": f"attachment; filename=manycore_run_{run_id}.zip"},
    )


# ---------------------------------------------------------------------------
# JAR inspection (class list — no execution)
# ---------------------------------------------------------------------------

@app.route("/api/jar/classes", methods=["POST"])
def jar_classes():
    if "jar" not in request.files:
        return jsonify({"error": "no jar file"}), 400
    jar_bytes = request.files["jar"].read()
    try:
        with zipfile.ZipFile(io.BytesIO(jar_bytes)) as zf:
            classes = sorted(
                name[:-6]   # strip .class
                for name in zf.namelist()
                if name.endswith(".class")
                and name        != "module-info.class"
                and not name.endswith("/module-info.class")
                and not name.endswith("/package-info.class")
                and name        != "package-info.class"
            )
        return jsonify({"classes": classes, "count": len(classes)})
    except zipfile.BadZipFile:
        return jsonify({"error": "not a valid JAR/ZIP file"}), 400
    except Exception as exc:
        return jsonify({"error": str(exc)}), 400


# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Causality API — LLM agent endpoints
# ---------------------------------------------------------------------------

_graph_cache: dict = {}   # run_id → (CausalityGraph, raw_summary)


def _get_graph(run_id: str):
    if run_id in _graph_cache:
        return _graph_cache[run_id]
    run = run_mgr.get_run(run_id)
    if not run or not run.get("run_dir"):
        return None
    jsonl = os.path.join(run["run_dir"], "runtime_targets.jsonl")
    if not os.path.isfile(jsonl):
        return None
    try:
        result = build_graph(jsonl)
        _graph_cache[run_id] = result
        return result
    except Exception as exc:
        print(f"[causality] graph build error for run {run_id}: {exc}")
        return None


_PROXY_INDICATORS = (
    "$Proxy", "$$SpringCGLIB$$", "$ByteBuddy$", "MockitoMock",
    "$$EnhancerBy", "$Subclass$", "HibernateProxy", "$auxiliary$",
)


def _proxy_kind(cls: str) -> str | None:
    if "$$SpringCGLIB$$" in cls or "$$EnhancerBy" in cls:
        return "cglib"
    if "$Proxy" in cls:
        return "jdk_proxy"
    if "$ByteBuddy$" in cls or "$auxiliary$" in cls:
        return "bytebuddy"
    if "MockitoMock" in cls:
        return "mockito"
    if "HibernateProxy" in cls:
        return "hibernate"
    return None


def _proxy_note(kind: str, cls: str) -> str:
    if kind == "cglib":
        base = cls.split("$$SpringCGLIB$$")[0].split("$$EnhancerBy")[0]
        return f"Spring CGLIB proxy wrapping {base} — real implementation is in the non-proxy class"
    if kind == "jdk_proxy":
        return "JDK dynamic proxy — dispatches to InvocationHandler.invoke; real target determined by handler"
    if kind == "bytebuddy":
        base = cls.split("$ByteBuddy$")[0].split("$auxiliary$")[0]
        return f"ByteBuddy-generated class (may be {base} subclass, Mockito mock, or Hibernate proxy)"
    if kind == "mockito":
        return "Mockito mock — all methods intercepted; configure stubs to set expected return values"
    if kind == "hibernate":
        return "Hibernate lazy-loading proxy — real entity class is the base type without $HibernateProxy"
    return "Generated proxy class"


def _fmt_callsite(cs: dict) -> dict:
    r = cs.get("raw", {})
    out: dict = {
        "source_class":  cs.get("source_class", ""),
        "source_method": cs.get("source_method", ""),
        "source_bci":    cs.get("source_bci", -1),
        "source_opcode": cs.get("source_opcode", ""),
        "category":      cs.get("category", ""),
        "evidence":      r.get("evidence", ""),
    }
    if cs.get("record") == "callsite_target":
        out["target_class"]      = r.get("target_class", "")
        out["target_method"]     = r.get("target_method", "")
        out["target_descriptor"] = r.get("target_descriptor", "")
        out["target_loader_id"]  = r.get("target_loader_id", "")
        if r.get("lmf_impl_method"):
            out["lmf_impl_class"]       = r.get("lmf_impl_class", "")
            out["lmf_impl_method"]      = r.get("lmf_impl_method", "")
            out["lmf_impl_descriptor"]  = r.get("lmf_impl_descriptor", "")
    return out


def _explain_callsite(callsite: str, category: str, static_label: str,
                      targets: list, is_lambda: bool, staticizable: bool) -> str:
    n    = len(targets)
    mech = category or "dynamic dispatch"

    if n == 0:
        return (f"No dispatch targets recorded for {callsite}. "
                f"The callsite may not have been reached during the run, or a diagnostic "
                f"record was emitted instead of an exact target.")

    def _short(t: dict) -> str:
        return f"{t.get('class','?').split('/')[-1]}.{t.get('method','?')}"

    names = [_short(t) for t in targets]

    if is_lambda and n == 1:
        return (f"The invokedynamic callsite at {callsite} creates a lambda. "
                f"The lambda body is implemented by {names[0]}. "
                f"If unexpected behavior occurs in this lambda, inspect that method.")

    if n == 1:
        ev = targets[0].get("evidence", "")
        ev_note = (" (target guaranteed by JVM linkage)" if "LINKAGE_GUARANTEED" in ev
                   else " (observed at runtime — not statically guaranteed)" if "OBSERVED" in ev
                   else "")
        refl_note = ""
        if "reflection" in category:
            refl_note = (" Discovered through reflection (Method.invoke) — "
                         "would not appear in a normal stack trace at this callsite. ")
        static_note = " This callsite is staticizable." if staticizable else ""
        return (f"The {mech} callsite at {callsite} dispatches to exactly one target: "
                f"{names[0]}{ev_note}.{refl_note}{static_note}")

    if static_label == SR_MULTI_TARGET:
        return (f"The {mech} callsite at {callsite} is polymorphic: "
                f"{n} distinct concrete implementations were observed during this run: "
                f"{', '.join(names)}. "
                f"This is a blocked_multi_target site — the JVM cannot statically predict "
                f"which implementation executes. If a bug manifests only for certain inputs, "
                f"inspect each listed implementation to find which one is responsible.")

    return (f"The {mech} callsite at {callsite} has {n} observed targets: "
            f"{', '.join(names)}. Static label: {static_label}.")


# --- endpoints ---------------------------------------------------------------

@app.route("/api/runs/<run_id>/causality/summary")
def causality_summary(run_id):
    result = _get_graph(run_id)
    if result is None:
        return jsonify({"error": "graph not available — run missing or JSONL not ready"}), 404
    g, raw_summary = result

    nc      = g.node_counts()
    ec      = g.edge_counts()
    blocked = list_blocked_callsites(g)
    static_ = list_staticizable_callsites(g)
    orphans = list_orphan_runtime_targets(g)
    multi   = [b for b in blocked if b.get("label") == SR_MULTI_TARGET]

    n_b = len(blocked)
    n_s = len(static_)
    n_o = len(orphans)
    n_m = len(multi)

    exp = (
        f"The graph captured {nc.get('callsite', 0)} callsite dispatch records across "
        f"{nc.get('method', 0)} distinct methods in {nc.get('class', 0)} classes. "
        f"{n_b} callsite(s) are blocked: {n_m} polymorphic (multiple observed targets), "
        f"{n_b - n_m} other reasons. "
        f"{n_s} callsite(s) are monomorphic and staticizable. "
        f"{n_o} orphan runtime_target(s) — should be 0. "
        f"Heuristic edges: 0 (hard invariant)."
    )
    return jsonify({
        "run_id":                run_id,
        "node_counts":           nc,
        "edge_counts":           ec,
        "gap_counts":            g.gap_counts(),
        "blocked_callsites":     n_b,
        "polymorphic_callsites": n_m,
        "staticizable_callsites": n_s,
        "orphan_runtime_targets": n_o,
        "heuristic_edges_created": 0,
        "export_complete":       raw_summary.get("complete", False),
        "export_summary":        raw_summary,
        "explanation":           exp,
    })


@app.route("/api/runs/<run_id>/causality/search")
def causality_search(run_id):
    q = request.args.get("q", "").strip().lower()
    if not q:
        return jsonify({"error": "q parameter required"}), 400
    idx = _get_index(run_id)
    if idx is None:
        return jsonify({"error": "index not available"}), 404

    matches = []
    seen: set = set()
    for cs in idx["all_callsites"]:
        r        = cs.get("raw", {})
        src_cls  = cs.get("source_class", "")
        src_meth = cs.get("source_method", "")
        tgt_cls  = r.get("target_class", "")
        tgt_meth = r.get("target_method", "")
        if not (q in src_cls.lower() or q in src_meth.lower() or
                q in tgt_cls.lower() or q in tgt_meth.lower()):
            continue
        key = (src_cls, src_meth, cs.get("source_bci", -1),
               tgt_cls, tgt_meth, cs.get("category", ""))
        if key in seen:
            continue
        seen.add(key)
        matches.append(_fmt_callsite(cs))

    matches.sort(key=lambda m: (m["source_class"], m["source_method"], m["source_bci"]))
    n = len(matches)
    if n == 0:
        exp = f"No callsites found matching '{q}'."
    elif n == 1:
        m = matches[0]
        exp = (f"Found 1 callsite matching '{q}': "
               f"{m['source_class']}.{m['source_method']}@bci={m['source_bci']} "
               f"[{m['category']}] → {m.get('target_class','?')}.{m.get('target_method','?')}.")
    else:
        classes = sorted({m["source_class"] for m in matches})
        exp = (f"Found {n} callsite(s) matching '{q}' across "
               f"{len(classes)} class(es): " + ", ".join(classes[:4]) +
               ("..." if len(classes) > 4 else "") + ".")
    return jsonify({"query": q, "matches": matches, "count": n, "explanation": exp})


@app.route("/api/runs/<run_id>/causality/polymorphic")
def causality_polymorphic(run_id):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify({"error": "index not available"}), 404

    from collections import defaultdict as _dd
    groups: dict = _dd(list)
    for cs in idx["all_callsites"]:
        if cs.get("record") != "callsite_target":
            continue
        key = (cs["source_class"], cs["source_method"], cs.get("source_bci", -1))
        groups[key].append(cs)

    poly = []
    for (src_cls, src_meth, src_bci), css in groups.items():
        seen_tgt: set = set()
        targets = []
        for cs in css:
            r = cs.get("raw", {})
            t = (r.get("target_class", ""), r.get("target_method", ""))
            if t in seen_tgt:
                continue
            seen_tgt.add(t)
            targets.append({
                "class":      r.get("target_class", ""),
                "method":     r.get("target_method", ""),
                "descriptor": r.get("target_descriptor", ""),
                "loader_id":  r.get("target_loader_id", ""),
            })
        if len(targets) < 2:
            continue
        poly.append({
            "source_class":  src_cls,
            "source_method": src_meth,
            "source_bci":    src_bci,
            "source_opcode": css[0].get("source_opcode", ""),
            "category":      css[0].get("category", ""),
            "static_label":  SR_MULTI_TARGET,
            "target_count":  len(targets),
            "targets":       targets,
        })

    poly.sort(key=lambda p: (p["source_class"], p["source_method"], p["source_bci"]))
    n = len(poly)
    exp = ("No polymorphic callsites found — all observed dispatch sites are monomorphic."
           if n == 0 else
           f"Found {n} polymorphic callsite(s) where the JVM observed multiple concrete "
           f"implementations during this run. These are the most likely locations for bugs "
           f"that manifest only for specific input types. Use /causality/explain for each "
           f"site to see all observed targets.")
    return jsonify({"polymorphic_callsites": poly, "count": n, "explanation": exp})


@app.route("/api/runs/<run_id>/causality/reflection")
def causality_reflection(run_id):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify({"error": "index not available"}), 404

    REFL = {"reflection_method_invoke", "reflection_constructor_newInstance"}
    sites = sorted(
        [_fmt_callsite(cs) for cs in idx["all_callsites"] if cs.get("category") in REFL],
        key=lambda s: (s["source_class"], s["source_method"], s["source_bci"]),
    )
    n = len(sites)
    exp = ("No reflection callsites (Method.invoke or Constructor.newInstance) found."
           if n == 0 else
           f"Found {n} reflection callsite(s). Reflection hides the actual callee from "
           f"normal stack traces and IDE search — the causality graph resolves each to the "
           f"exact target method. If an InvocationTargetException or unexpected behavior "
           f"is occurring through reflection, check the targets listed here.")
    return jsonify({"reflection_callsites": sites, "count": n, "explanation": exp})


@app.route("/api/runs/<run_id>/causality/proxies")
def causality_proxies(run_id):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify({"error": "index not available"}), 404

    sites = []
    for cs in idx["all_callsites"]:
        if cs.get("record") not in ("callsite_target", "callsite_adapter_graph"):
            continue
        tgt_cls = cs.get("raw", {}).get("target_class", "") or ""
        kind    = _proxy_kind(tgt_cls)
        if kind is None:
            continue
        entry = _fmt_callsite(cs)
        entry["proxy_kind"] = kind
        entry["note"]       = _proxy_note(kind, tgt_cls)
        sites.append(entry)

    sites.sort(key=lambda s: (s["source_class"], s["source_method"], s["source_bci"]))
    n    = len(sites)
    kinds = sorted({s["proxy_kind"] for s in sites}) if sites else []
    exp  = ("No proxy/generated-class dispatch sites found."
            if n == 0 else
            f"Found {n} callsite(s) targeting generated or proxy classes "
            f"({', '.join(kinds)}). Stack traces show the generated class name, not the "
            f"real business class. Use /causality/chain to follow the full dispatch through "
            f"the proxy to the actual target.")
    return jsonify({"proxy_sites": sites, "count": n, "explanation": exp})


@app.route("/api/runs/<run_id>/causality/hidden")
def causality_hidden(run_id):
    idx = _get_index(run_id)
    if idx is None:
        return jsonify({"error": "index not available"}), 404

    hidden_map     = idx.get("hidden_id_map", {})
    artifact_by_cls = idx.get("artifact_by_class", {})
    entries = []
    for runtime_name, info in hidden_map.items():
        crc    = info.get("artifact_crc", "")
        loader = info.get("loader_id", "")
        base   = runtime_name.split("+0x")[0] if "+0x" in runtime_name else runtime_name
        has_art = f"{base}_crc{crc}" in artifact_by_cls or base in artifact_by_cls
        entries.append({
            "runtime_name": runtime_name,
            "artifact_crc": crc,
            "loader_id":    loader,
            "has_artifact": has_art,
        })

    entries.sort(key=lambda e: e["runtime_name"])
    n = len(entries)
    exp = ("No hidden classes found. Lambda implementations and defineHiddenClass-created "
           "classes would appear here."
           if n == 0 else
           f"Found {n} hidden class(es). Hidden class names include a hex suffix (+0x...) "
           f"that changes every JVM run. The artifact_crc is stable across runs for the same "
           f"bytecode. Use /causality/search with the base class name (without +0x...) to "
           f"find which callsite created or invoked each hidden class.")
    return jsonify({"hidden_classes": entries, "count": n, "explanation": exp})


@app.route("/api/runs/<run_id>/causality/chain")
def causality_chain(run_id):
    src_class  = request.args.get("class",  "").strip()
    src_method = request.args.get("method", "").strip()
    src_bci    = request.args.get("bci",    "").strip()
    src_desc   = request.args.get("desc",   "").strip()
    if not src_class or not src_method:
        return jsonify({"error": "class and method parameters required"}), 400
    try:
        bci = int(src_bci) if src_bci else -1
    except ValueError:
        return jsonify({"error": "bci must be an integer"}), 400

    result = _get_graph(run_id)
    if result is None:
        return jsonify({"error": "graph not available"}), 404
    g, _ = result

    chain      = query_chain(g, src_class, src_method, bci, src_desc)
    label      = src_bci or "?"
    cs_str     = f"{src_class}.{src_method}@bci={label}"

    if chain and "error" in chain[0]:
        exp = (f"No callsite found for {cs_str}. "
               f"Try /causality/search?q={src_class.split('/')[-1]} to find the correct BCI.")
        return jsonify({"callsite": cs_str, "chain": chain, "explanation": exp})

    n_tgt = sum(1 for e in chain if e.get("edge") in (ET_CALLSITE_TARGET, ET_LAMBDA_BODY))
    exp   = (f"Causality chain for {cs_str}: {n_tgt} direct target edge(s). "
             f"Each CALLSITE_TARGET entry is a concrete method observed at this dispatch site. "
             f"LAMBDA_BODY edges point to lambda implementation methods.")
    return jsonify({"callsite": cs_str, "chain": chain, "explanation": exp})


@app.route("/api/runs/<run_id>/causality/explain")
def causality_explain(run_id):
    src_class  = request.args.get("class",  "").strip()
    src_method = request.args.get("method", "").strip()
    src_bci    = request.args.get("bci",    "").strip()
    src_desc   = request.args.get("desc",   "").strip()
    if not src_class or not src_method:
        return jsonify({"error": "class and method parameters required"}), 400
    try:
        bci = int(src_bci) if src_bci else -1
    except ValueError:
        return jsonify({"error": "bci must be an integer"}), 400

    result = _get_graph(run_id)
    if result is None:
        return jsonify({"error": "graph not available"}), 404
    g, _ = result

    chain  = query_chain(g, src_class, src_method, bci, src_desc)
    label  = src_bci or "?"
    cs_str = f"{src_class}.{src_method}@bci={label}"

    if chain and "error" in chain[0]:
        exp = (f"No callsite found for {cs_str}. "
               f"Use /causality/search?q={src_class.split('/')[-1]} to find the exact BCI.")
        return jsonify({"callsite": cs_str, "error": chain[0]["error"],
                        "explanation": exp}), 404

    cs_attrs    = chain[0].get("attrs", {}) if chain else {}
    static_lbl  = cs_attrs.get("static_label", "")
    category    = cs_attrs.get("category", cs_attrs.get("opcode", ""))

    targets   = []
    is_lambda = False
    for entry in chain[1:]:
        edge = entry.get("edge", "")
        if edge not in (ET_CALLSITE_TARGET, ET_LAMBDA_BODY):
            continue
        attrs = entry.get("attrs", {})
        node_id = entry.get("id", "")
        parts   = node_id.split("::") if "::" in node_id else []
        t: dict = {
            "class":      attrs.get("target_class", parts[-4] if len(parts) >= 4 else ""),
            "method":     attrs.get("target_method", ""),
            "descriptor": attrs.get("target_descriptor", attrs.get("descriptor", "")),
            "evidence":   attrs.get("evidence", ""),
            "edge_type":  edge,
        }
        if edge == ET_LAMBDA_BODY:
            is_lambda = True
            t["note"] = "lambda body implementation"
        targets.append(t)

    polymorphic  = len(targets) > 1
    staticizable = static_lbl in (SR_DIRECT, SR_ADAPTER_MODELED)
    exp = _explain_callsite(cs_str, category, static_lbl, targets, is_lambda, staticizable)

    return jsonify({
        "callsite":           cs_str,
        "dispatch_mechanism": category,
        "static_label":       static_lbl,
        "polymorphic":        polymorphic,
        "staticizable":       staticizable,
        "target_count":       len(targets),
        "targets":            targets,
        "explanation":        exp,
        "chain":              chain,
    })


# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
    os.makedirs("/tmp/rt_ui_runs", exist_ok=True)
    print(f"\n  Runtime Truth UI  →  http://localhost:{port}\n")
    app.run(debug=False, port=port, threaded=True)
