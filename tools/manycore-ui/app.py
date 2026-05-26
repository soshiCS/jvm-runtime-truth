"""
ManyCore UI — Flask backend.
Local-only. No auth, no sandboxing, no public exposure assumed.
"""
import io
import json
import os
import subprocess
import zipfile
from pathlib import Path

from flask import Flask, Response, abort, jsonify, request, send_from_directory
from indexer import callsite_summary, find_best_artifact, load_and_index, validate_run
from runner import DEFAULT_JDK, RunManager

# ---------------------------------------------------------------------------
DEFAULT_JAVAP = os.path.join(DEFAULT_JDK, "bin", "javap")

app      = Flask(__name__, static_folder="static", static_url_path="")
run_mgr  = RunManager()
_idx_cache: dict = {}


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
            run["stats"] = {
                "total_records":  idx["total_records"],
                "class_count":    len(idx["classes"]),
                "callsite_count": sum(
                    1 for cs in idx["all_callsites"]
                    if cs["record"] != "diagnostic"
                ),
                "diagnostic_count": len(idx["diagnostics"]),
                "export_summary":   idx.get("export_summary"),
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
        idx = load_and_index(jsonl)
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
        "# ManyCore JVM Run Export",
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

if __name__ == "__main__":
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
    os.makedirs("/tmp/manycore_ui_runs", exist_ok=True)
    print(f"\n  ManyCore UI  →  http://localhost:{port}\n")
    app.run(debug=False, port=port, threaded=True)
