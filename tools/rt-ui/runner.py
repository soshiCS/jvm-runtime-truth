"""
Process runner: launches the custom JVM with all required SOROUSH_* env vars,
captures stdout/stderr, enforces timeout, writes per-run directory.
"""
import json
import os
import subprocess
import threading
import uuid
from pathlib import Path

RUN_BASE = Path("/tmp/rt_ui_runs")

def _auto_detect_jdk() -> str:
    """Find the first built JDK under the repo's build/ directory."""
    import glob
    script_dir = Path(__file__).parent
    repo_root = script_dir.parent.parent
    candidates = sorted(glob.glob(str(repo_root / "build" / "*" / "jdk")))
    return candidates[0] if candidates else ""

DEFAULT_JDK = _auto_detect_jdk()

# Always-on SOROUSH export flags (same set as run_showcase.sh)
REQUIRED_ENV = {
    "SOROUSH_PROVENANCE_GRAPH":         "1",
    "SOROUSH_RUNTIME_GRAPH":            "1",
    "SOROUSH_RUNTIME_RECOVERY":         "1",
    "SOROUSH_TRACE_INDY":               "1",
    "SOROUSH_TRACE_REFLECTION":         "1",
    "SOROUSH_CAPTURE_FINAL_BYTECODE":   "1",
    "SOROUSH_REWRITER_PHASE5_NORMAL_EXIT": "1",
}


class RunManager:
    def __init__(self):
        self._runs: dict = {}
        self._lock = threading.Lock()

    # ------------------------------------------------------------------
    def start_run(self, config: dict, jar_bytes: bytes) -> str:
        run_id = str(uuid.uuid4())[:8]
        run_dir = RUN_BASE / run_id
        run_dir.mkdir(parents=True, exist_ok=True)
        (run_dir / "artifacts").mkdir(exist_ok=True)

        jar_path = run_dir / "app.jar"
        jar_path.write_bytes(jar_bytes)

        state = {
            "run_id":    run_id,
            "run_dir":   str(run_dir),
            "status":    "running",
            "exit_code": None,
            "timed_out": False,
            "error":     None,
            "config":    config,
        }
        with self._lock:
            self._runs[run_id] = state

        t = threading.Thread(
            target=self._execute,
            args=(run_id, config, run_dir, jar_path),
            daemon=True,
        )
        t.start()
        return run_id

    def get_run(self, run_id: str) -> dict:
        with self._lock:
            return dict(self._runs.get(run_id, {}))

    def list_runs(self) -> list:
        with self._lock:
            return [dict(v) for v in self._runs.values()]

    def ingest_run(self, label: str, run_dir: str) -> str:
        """Register a pre-captured run_dir (with runtime_targets.jsonl) as a completed run."""
        run_id = str(uuid.uuid4())[:8]
        state = {
            "run_id":    run_id,
            "run_dir":   run_dir,
            "status":    "done",
            "exit_code": 0,
            "timed_out": False,
            "error":     None,
            "config":    {"label": label, "user_prefixes": "com/example/demo"},
        }
        with self._lock:
            self._runs[run_id] = state
        return run_id

    # ------------------------------------------------------------------
    def _execute(self, run_id: str, config: dict, run_dir: Path, jar_path: Path):
        summary: dict = {}
        try:
            jdk_path = config.get("jdk_path", "").strip() or DEFAULT_JDK
            java_bin = os.path.join(jdk_path, "bin", "java")

            cmd = [java_bin, "-Xverify:all", "-Xint"]

            # User-supplied JVM args (space-split; no shell expansion)
            for arg in (config.get("jvm_args", "") or "").split():
                cmd.append(arg)

            run_mode = config.get("run_mode", "java-jar")
            extra_cp  = (config.get("extra_classpath", "") or "").strip()

            if run_mode == "java-jar":
                cmd += ["-jar", str(jar_path)]
            else:
                cp = str(jar_path)
                if extra_cp:
                    cp += ":" + extra_cp
                main_cls = (config.get("main_class", "") or "").strip()
                if not main_cls:
                    raise ValueError("main_class is required for main-class run mode")
                cmd += ["-cp", cp, main_cls]

            for arg in (config.get("program_args", "") or "").split():
                cmd.append(arg)

            # Environment
            env = os.environ.copy()
            env.update(REQUIRED_ENV)
            env["SOROUSH_BYTECODE_DUMP_DIR"]        = str(run_dir / "artifacts")
            env["SOROUSH_EXPORT_RUNTIME_TARGETS"]   = str(run_dir / "runtime_targets.jsonl")

            # user_prefixes: multi-line or comma-separated list.
            # Falls back to legacy single rewriter_prefix field.
            user_pfx_raw = (config.get("user_prefixes", "") or "").strip()
            if user_pfx_raw:
                parts = [p.strip() for p in user_pfx_raw.replace(",", "\n").splitlines() if p.strip()]
            else:
                old = (config.get("rewriter_prefix", "") or "").strip()
                parts = [old] if old else []
            if parts:
                env["SOROUSH_REWRITER_PHASE5_PREFIX"] = parts[0]
                env["SOROUSH_USER_PREFIXES"]           = ",".join(parts)

            mh_depth = (config.get("mh_walk_depth", "") or "").strip()
            if mh_depth:
                env["SOROUSH_MH_WALK_DEPTH"] = mh_depth

            for line in (config.get("env_vars", "") or "").splitlines():
                line = line.strip()
                if "=" in line and not line.startswith("#"):
                    k, v = line.split("=", 1)
                    env[k.strip()] = v.strip()

            cwd = (config.get("working_dir", "") or "").strip() or str(run_dir)
            if not os.path.isdir(cwd):
                cwd = str(run_dir)

            timeout = int(config.get("timeout", 60) or 60)

            summary = {
                "run_id":  run_id,
                "command": cmd,
                "status":  "running",
                "timeout": timeout,
            }
            (run_dir / "summary.json").write_text(json.dumps(summary, indent=2))

            stdout_path = run_dir / "stdout.txt"
            stderr_path = run_dir / "stderr.txt"
            with open(stdout_path, "w") as fout, open(stderr_path, "w") as ferr:
                proc = subprocess.Popen(
                    cmd, stdout=fout, stderr=ferr, env=env, cwd=cwd
                )

            try:
                exit_code = proc.wait(timeout=timeout)
                timed_out = False
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                exit_code = -1
                timed_out = True

            summary.update({
                "status":        "done",
                "exit_code":     exit_code,
                "timed_out":     timed_out,
                "jsonl_path":    str(run_dir / "runtime_targets.jsonl"),
                "artifacts_dir": str(run_dir / "artifacts"),
            })
            (run_dir / "summary.json").write_text(json.dumps(summary, indent=2))

            with self._lock:
                self._runs[run_id].update({
                    "status":    "done",
                    "exit_code": exit_code,
                    "timed_out": timed_out,
                })

        except Exception as exc:
            with self._lock:
                self._runs[run_id].update({"status": "error", "error": str(exc)})
            try:
                summary.update({"status": "error", "error": str(exc)})
                (run_dir / "summary.json").write_text(json.dumps(summary, indent=2))
            except Exception:
                pass
