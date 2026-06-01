"""
Scoring logic for the Runtime Truth Agent Benchmark.

Scoring rubric (total 16 points):
  file_score      0–3   exact file identified
  line_score      0–3   line proximity
  patch_score     0–5   patch correctness
  explain_score   0–5   explanation quality (keyword coverage)

Efficiency metrics (not scored, reported separately):
  total_turns, files_read, causality_calls, wrong_hypotheses
"""

from __future__ import annotations
from typing import Any


def score_run(result: dict, ground_truth: dict) -> dict:
    """Score one agent run against ground truth. Returns a score dict."""
    diag = result.get("diagnosis") or {}
    tool_calls = result.get("tool_calls", [])

    # ── File score ─────────────────────────────────────────────────────────────
    correct_file = ground_truth["file"]
    submitted_file = diag.get("root_cause_file", "") or ""
    # Accept exact match OR path ending with the correct file (handles src/main/resources/ prefix)
    if submitted_file == correct_file or submitted_file.endswith("/" + correct_file):
        file_score = 3
    elif ground_truth["package"] in submitted_file:
        file_score = 1
    else:
        file_score = 0

    # ── Line score ─────────────────────────────────────────────────────────────
    correct_line = ground_truth["line"]
    submitted_line = diag.get("root_cause_line", -999) or -999
    try:
        submitted_line = int(submitted_line)
    except (TypeError, ValueError):
        submitted_line = -999
    delta = abs(submitted_line - correct_line)
    if delta <= 2:
        line_score = 3
    elif delta <= 8:
        line_score = 2
    elif delta <= 20:
        line_score = 1
    else:
        line_score = 0

    # ── Patch score ────────────────────────────────────────────────────────────
    patch = diag.get("patch_code", "") or ""
    if ground_truth["patch_fragment"] in patch:
        patch_score = 5
    elif any(kw in patch for kw in ground_truth["patch_keywords"]):
        patch_score = 3
    elif ground_truth["package"] in patch:
        patch_score = 1
    else:
        patch_score = 0

    # ── Explanation score ──────────────────────────────────────────────────────
    explanation = (diag.get("root_cause_explanation", "") or "").lower()
    kw_hits = sum(1 for kw in ground_truth["explanation_keywords"] if kw.lower() in explanation)
    explain_score = min(5, kw_hits)

    # ── Efficiency metrics ─────────────────────────────────────────────────────
    file_reads    = [tc for tc in tool_calls if tc["tool"] == "read_file"]
    causality_ops = [tc for tc in tool_calls if tc["tool"].startswith("causality")]
    unique_files  = len(set(
        tc["input"].get("path", "") for tc in file_reads if isinstance(tc.get("input"), dict)
    ))
    wrong_hypos   = len(diag.get("wrong_hypotheses", []) or [])

    total = file_score + line_score + patch_score + explain_score
    max_score = 3 + 3 + 5 + 5  # 16

    return {
        # Score breakdown
        "file_score":     file_score,
        "line_score":     line_score,
        "patch_score":    patch_score,
        "explain_score":  explain_score,
        "total":          total,
        "max_score":      max_score,
        "pct":            round(100 * total / max_score, 1),

        # Efficiency
        "total_turns":       result.get("total_turns", 0),
        "files_read":        len(file_reads),
        "unique_files_read": unique_files,
        "causality_calls":   len(causality_ops),
        "wrong_hypotheses":  wrong_hypos,
        "confidence":        diag.get("confidence", "not_submitted"),
        "submitted":         result.get("diagnosis") is not None,
    }


def compare(score_a: dict, score_b: dict) -> dict:
    """Produce a head-to-head comparison of two scored runs."""
    delta_total  = score_b["total"]       - score_a["total"]
    delta_turns  = score_a["total_turns"] - score_b["total_turns"]   # positive = B faster
    delta_files  = score_a["unique_files_read"] - score_b["unique_files_read"]

    if delta_total > 1:
        winner = "B"
    elif delta_total < -1:
        winner = "A"
    else:
        # Tiebreak by turns (fewer = better)
        winner = "B" if delta_turns > 0 else ("A" if delta_turns < 0 else "TIE")

    return {
        "winner":              winner,
        "score_delta":         delta_total,   # positive favours B
        "turns_saved":         delta_turns,   # positive = B used fewer turns
        "files_saved":         delta_files,   # positive = B read fewer files
        "causality_calls_b":   score_b["causality_calls"],
        "explanation":         _explain_result(winner, delta_total, delta_turns, score_a, score_b),
    }


def _explain_result(winner, delta_total, delta_turns, sa, sb):
    if winner == "B":
        parts = [f"Agent B outscored Agent A by {delta_total} point(s) ({sa['total']}/{sa['max_score']} vs {sb['total']}/{sb['max_score']})."]
        if delta_turns > 0:
            parts.append(f"Agent B reached diagnosis in {delta_turns} fewer turn(s).")
        if sb["causality_calls"] > 0:
            parts.append(f"Agent B made {sb['causality_calls']} causality API call(s).")
        return " ".join(parts)
    elif winner == "A":
        return (f"Agent A outperformed Agent B ({sa['total']}/{sa['max_score']} vs "
                f"{sb['total']}/{sb['max_score']}). Causality API did not provide measurable advantage on this bug.")
    else:
        return (f"Tied score ({sa['total']}/{sa['max_score']}). "
                f"Turn difference: {delta_turns} (positive = B was faster).")
