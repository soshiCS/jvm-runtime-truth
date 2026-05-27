"""
JSONL parser and index builder for runtime_targets.jsonl.

Produces a structured index consumed by the Flask API layer.
"""
import json
import os
from collections import defaultdict
from pathlib import Path


def load_and_index(jsonl_path: str) -> dict:
    records = []
    bad = 0
    with open(jsonl_path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                bad += 1
    idx = _build_index(records)
    idx["parse_errors"] = bad
    return idx


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _norm(name: str | None) -> str:
    """Normalise class name to slash-form."""
    return (name or "").replace(".", "/")


def _src(r: dict) -> tuple[str, str]:
    """Return (source_class, source_method) from any record, trying both spellings."""
    cls  = _norm(r.get("source_class") or r.get("src_class") or "")
    meth = r.get("source_method") or r.get("src_method") or ""
    return cls, meth


def _build_index(records: list) -> dict:
    # class_name -> class entry dict
    classes: dict[str, dict] = {}

    def cls_entry(name: str) -> dict:
        n = _norm(name)
        if not n:
            return {}
        if n not in classes:
            classes[n] = {
                "name":               n,
                "record_types":       set(),
                "generated":          False,
                "has_artifacts":      False,
                "has_callsites_src":  False,
                "has_callsites_tgt":  False,
                "has_diagnostics":    False,
            }
        return classes[n]

    # (class, loader) -> {"final": record|None, "original": record|None}
    artifacts: dict[tuple, dict] = defaultdict(lambda: {"final": None, "original": None})

    # (class, method) -> [callsite entry]
    callsites_by: dict[str, list] = defaultdict(list)

    all_callsites: list[dict] = []   # indexed by position
    diagnostics:   list[dict] = []
    generated_cls: list[dict] = []
    export_summary: dict | None = None

    # method_identity: class -> {method -> [entry]}
    methods: dict[str, dict[str, list]] = defaultdict(lambda: defaultdict(list))

    def push_callsite(cs: dict) -> dict:
        cs["idx"] = len(all_callsites)
        all_callsites.append(cs)
        key = f"{cs['source_class']}||{cs['source_method']}"
        callsites_by[key].append(cs)
        return cs

    for r in records:
        rtype = r.get("record", "")

        # ------------------------------------------------------------------ export_summary
        if rtype == "export_summary":
            export_summary = r
            continue

        # ------------------------------------------------------------------ method_identity
        if rtype == "method_identity":
            cls  = _norm(r.get("class", ""))
            meth = r.get("method", "")
            if cls:
                e = cls_entry(cls)
                e["record_types"].add("method_identity")
                methods[cls][meth].append({
                    "method":       meth,
                    "descriptor":   r.get("descriptor", ""),
                    "loader_id":    r.get("loader_id", ""),
                    "token":        r.get("token", 0),
                    "artifact_crc": r.get("artifact_crc", ""),
                    "hidden":       r.get("hidden", False),
                    "source":       "method_identity",
                })
            continue

        # ------------------------------------------------------------------ bytecode_artifact
        if rtype == "bytecode_artifact":
            cls    = _norm(r.get("class", ""))
            loader = r.get("loader_id", "")
            kind   = r.get("kind", "original")
            if cls:
                e = cls_entry(cls)
                e["record_types"].add("bytecode_artifact")
                e["has_artifacts"] = True
                artifacts[(cls, loader)][kind] = r
            continue

        # ------------------------------------------------------------------ generated_class
        if rtype == "generated_class":
            cls = _norm(r.get("class", ""))
            if cls:
                e = cls_entry(cls)
                e["record_types"].add("generated_class")
                e["generated"] = True
                generated_cls.append(r)
            continue

        # ------------------------------------------------------------------ callsite_target
        if rtype == "callsite_target":
            src_cls, src_meth = _src(r)
            tgt_cls = _norm(r.get("target_class", ""))
            if src_cls:
                e = cls_entry(src_cls)
                e["record_types"].add("callsite_target")
                e["has_callsites_src"] = True
            if tgt_cls:
                e = cls_entry(tgt_cls)
                e["has_callsites_tgt"] = True
            cs = {
                "record":            rtype,
                "source_class":      src_cls,
                "source_method":     src_meth,
                "source_descriptor": r.get("source_descriptor") or r.get("src_desc", ""),
                "source_bci":        r.get("source_bci", -1),
                "source_opcode":     r.get("source_opcode", ""),
                "category":          r.get("category", ""),
                "evidence":          r.get("evidence", ""),
                "exact":             bool(r.get("exact", False)),
                "all_exact":         bool(r.get("exact", False)),
                "raw":               r,
            }
            push_callsite(cs)
            continue

        # ------------------------------------------------------------------ callsite_target_set
        if rtype == "callsite_target_set":
            src_cls, src_meth = _src(r)
            if src_cls:
                e = cls_entry(src_cls)
                e["record_types"].add("callsite_target_set")
                e["has_callsites_src"] = True
                for t in r.get("targets", []):
                    tc = _norm(t.get("class", ""))
                    if tc:
                        cls_entry(tc)["has_callsites_tgt"] = True
            cs = {
                "record":            rtype,
                "source_class":      src_cls,
                "source_method":     src_meth,
                "source_descriptor": r.get("source_descriptor", ""),
                "source_bci":        r.get("source_bci", -1),
                "source_opcode":     r.get("source_opcode", ""),
                "category":          r.get("category", ""),
                "adapter_shape":     r.get("adapter_shape", ""),
                "all_exact":         True,
                "raw":               r,
            }
            push_callsite(cs)
            continue

        # ------------------------------------------------------------------ callsite_adapter_graph
        if rtype == "callsite_adapter_graph":
            src_cls, src_meth = _src(r)
            if src_cls:
                e = cls_entry(src_cls)
                e["record_types"].add("callsite_adapter_graph")
                e["has_callsites_src"] = True
                for n in r.get("nodes", []):
                    nc = _norm(n.get("class", ""))
                    if nc:
                        cls_entry(nc)["has_callsites_tgt"] = True
            cs = {
                "record":            rtype,
                "source_class":      src_cls,
                "source_method":     src_meth,
                "source_descriptor": r.get("source_descriptor", ""),
                "source_bci":        r.get("source_bci", -1),
                "source_opcode":     r.get("source_opcode", ""),
                "category":          r.get("category", ""),
                "adapter_kind":      r.get("adapter_kind", ""),
                "adapter_class":     r.get("adapter_class", ""),
                "all_exact":         bool(r.get("all_exact", False)),
                "raw":               r,
            }
            push_callsite(cs)
            continue

        # ------------------------------------------------------------------ runtime_target
        if rtype == "runtime_target":
            src_cls, src_meth = _src(r)
            tgt_cls = _norm(r.get("target_class", ""))
            if src_cls:
                e = cls_entry(src_cls)
                e["record_types"].add("runtime_target")
            if tgt_cls:
                cls_entry(tgt_cls)["record_types"].add("runtime_target_ref")
            continue

        # ------------------------------------------------------------------ diagnostic
        if rtype == "diagnostic":
            src_cls, src_meth = _src(r)
            if src_cls:
                e = cls_entry(src_cls)
                e["record_types"].add("diagnostic")
                e["has_diagnostics"] = True
            cs = {
                "record":            rtype,
                "source_class":      src_cls,
                "source_method":     src_meth,
                "source_descriptor": r.get("source_descriptor") or r.get("src_desc", ""),
                "source_bci":        r.get("source_bci", -1),
                "source_opcode":     r.get("source_opcode", ""),
                "category":          r.get("category", ""),
                "reason":            r.get("reason", ""),
                "all_exact":         False,
                "raw":               r,
            }
            push_callsite(cs)
            diagnostics.append(cs)
            continue

    # -- Finalise classes ---------------------------------------------------
    for c in classes.values():
        c["record_types"] = sorted(c["record_types"])

    sorted_classes = sorted(classes.values(), key=lambda c: c["name"])

    # -- Build methods-per-class (union of method_identity + callsite sources) --
    methods_by_class: dict[str, list] = {}
    for cls, meth_map in methods.items():
        methods_by_class[cls] = []
        for meth_name, entries in meth_map.items():
            merged = entries[0].copy() if entries else {}
            merged["method"] = meth_name
            methods_by_class[cls].append(merged)

    # Also add source methods seen only in callsites
    cs_source_methods: dict[str, set] = defaultdict(set)
    for cs in all_callsites:
        cls  = cs["source_class"]
        meth = cs["source_method"]
        if cls and meth:
            cs_source_methods[cls].add(meth)

    for cls, meths in cs_source_methods.items():
        known = {m["method"] for m in methods_by_class.get(cls, [])}
        for meth in meths:
            if meth not in known:
                methods_by_class.setdefault(cls, []).append({
                    "method": meth,
                    "descriptor": "",
                    "source": "callsite_source_only",
                })

    # Sort methods per class
    for cls in methods_by_class:
        methods_by_class[cls].sort(key=lambda m: m["method"])

    # -- Build per-class best artifact table --------------------------------
    # artifact_by_class: class_name -> {loader_id -> {"final": r, "original": r}}
    artifact_by_class: dict[str, dict] = defaultdict(dict)
    for (cls, loader), kinds in artifacts.items():
        artifact_by_class[cls][loader] = kinds

    return {
        "classes":           sorted_classes,
        "classes_by_name":   classes,
        "methods_by_class":  methods_by_class,
        "callsites_by":      dict(callsites_by),
        "all_callsites":     all_callsites,
        "artifact_by_class": dict(artifact_by_class),
        "diagnostics":       diagnostics,
        "generated_classes": generated_cls,
        "export_summary":    export_summary,
        "total_records":     len(records),
    }


# ---------------------------------------------------------------------------
# Public helpers used by app.py
# ---------------------------------------------------------------------------

def find_best_artifact(index: dict, class_name: str, loader_id: str | None = None) -> dict | None:
    """Return the best artifact record for a class (prefer final, then original)."""
    by_class = index.get("artifact_by_class", {})
    cls = _norm(class_name)
    if cls not in by_class:
        return None

    loaders = by_class[cls]
    if loader_id and loader_id in loaders:
        entry = loaders[loader_id]
    else:
        entry = next(iter(loaders.values()), None)

    if not entry:
        return None

    artifact = entry.get("final") or entry.get("original")
    if artifact:
        ap = artifact.get("artifact_path")
        artifact = dict(artifact)
        artifact["_exists"] = bool(ap and os.path.isfile(ap))
    return artifact


def validate_run(run_dir: str, index: dict) -> dict:
    results = []
    passes = 0
    fails  = 0

    def chk(name: str, ok: bool, detail: str = ""):
        nonlocal passes, fails
        if ok:
            passes += 1
        else:
            fails += 1
        results.append({"name": name, "passed": ok, "detail": detail})

    jsonl = os.path.join(run_dir, "runtime_targets.jsonl")
    chk("JSONL file exists", os.path.isfile(jsonl))

    total = index.get("total_records", 0)
    chk("JSONL parses without errors",
        index.get("parse_errors", 0) == 0,
        f"{index.get('parse_errors',0)} bad line(s)")
    chk("JSONL has records", total > 0, f"{total} records")

    es = index.get("export_summary")
    chk("export_summary present", es is not None)
    if es:
        chk("export_summary.complete = true",
            es.get("complete", True) is True,
            "" if es.get("complete", True) else "INCOMPLETE export detected")

    # Artifact path existence
    missing_paths = []
    for _, loaders in index.get("artifact_by_class", {}).items():
        for _, kinds in loaders.items():
            for kind_rec in kinds.values():
                if kind_rec is None:
                    continue
                ap = kind_rec.get("artifact_path")
                if ap and not os.path.isfile(ap):
                    missing_paths.append(ap)
    chk("all artifact_path files exist on disk",
        len(missing_paths) == 0,
        f"{len(missing_paths)} missing" if missing_paths else "")

    # No ? in descriptors
    qmarks = []
    for cs in index.get("all_callsites", []):
        r = cs.get("raw", {})
        for field in ("descriptor", "source_descriptor", "lmf_impl_descriptor"):
            v = r.get(field) or ""
            if "?" in v:
                qmarks.append(f"{cs['source_class']}.{cs['source_method']} {field}={v}")
        for n in r.get("nodes", []):
            d = n.get("descriptor") or ""
            if "?" in d:
                qmarks.append(f"node.descriptor={d}")
    chk("no '?' in any descriptor", len(qmarks) == 0,
        f"{len(qmarks)} found: {'; '.join(qmarks[:3])}" if qmarks else "")

    # No loader_id=0 on user_target nodes
    NULL_LOADERS = {"0x0000000000000000", "0x0", "0", None}
    bad_loaders = []
    for cs in index.get("all_callsites", []):
        for n in cs.get("raw", {}).get("nodes", []):
            if n.get("classification") == "user_target" and n.get("loader_id") in NULL_LOADERS:
                bad_loaders.append(f"{n.get('class','?')}.{n.get('method','?')}")
    chk("no loader_id=0 on user_target nodes",
        len(bad_loaders) == 0,
        f"{len(bad_loaders)} found" if bad_loaders else "")

    # No exact=false on user_target nodes
    exact_false = []
    for cs in index.get("all_callsites", []):
        for n in cs.get("raw", {}).get("nodes", []):
            if n.get("classification") == "user_target" and not n.get("exact", True):
                exact_false.append(f"{n.get('class','?')}.{n.get('method','?')}")
    chk("no exact=false on user_target nodes",
        len(exact_false) == 0,
        f"{len(exact_false)} found" if exact_false else "")

    return {
        "pass":    passes,
        "fail":    fails,
        "results": results,
        "overall": fails == 0,
    }


def callsite_summary(cs: dict) -> dict:
    """Return a lightweight summary of a callsite entry (no full raw record)."""
    r = cs.get("raw", {})
    s = {
        "idx":               cs["idx"],
        "record":            cs["record"],
        "source_class":      cs["source_class"],
        "source_method":     cs["source_method"],
        "source_descriptor": cs.get("source_descriptor", ""),
        "source_bci":        cs.get("source_bci", -1),
        "source_opcode":     cs.get("source_opcode", ""),
        "category":          cs.get("category", ""),
        "all_exact":         cs.get("all_exact", False),
    }
    rec = cs["record"]
    if rec == "callsite_target":
        s["target"] = {
            "class":      _norm(r.get("target_class", "")),
            "method":     r.get("target_method", ""),
            "descriptor": r.get("target_descriptor", ""),
            "loader_id":  r.get("target_loader_id", ""),
        }
        s["evidence"] = r.get("evidence", "")
        # invokedynamic fields: pass through all indy-specific data for UI rendering
        if r.get("category") == "invokedynamic":
            s["indy_name"]       = r.get("indy_name", "")
            s["indy_descriptor"] = r.get("indy_descriptor", r.get("indy_sig", ""))
            s["bootstrap_method"] = r.get("bootstrap_method", "")
            # LambdaMetafactory impl method
            if r.get("lmf_impl_method"):
                s["lmf_impl"] = {
                    "class":      _norm(r.get("lmf_impl_class", "")),
                    "method":     r.get("lmf_impl_method", ""),
                    "descriptor": r.get("lmf_impl_descriptor", ""),
                }
            # StringConcatFactory fields
            if r.get("semantic_op"):
                s["semantic_op"] = r.get("semantic_op")
            if r.get("string_concat_recipe") is not None:
                s["string_concat_recipe"] = r.get("string_concat_recipe")
            if r.get("string_concat_constants") is not None:
                s["string_concat_constants"] = r.get("string_concat_constants")
            s["reconstructable"] = r.get("reconstructable", False)
            s["staticizable"] = r.get("staticizable", False)
            if r.get("staticization_blockers"):
                s["staticization_blockers"] = r.get("staticization_blockers")
        elif r.get("lmf_impl_method"):
            # non-indy record with LMF fields (defensive)
            s["lmf_impl"] = {
                "class":      _norm(r.get("lmf_impl_class", "")),
                "method":     r.get("lmf_impl_method", ""),
                "descriptor": r.get("lmf_impl_descriptor", ""),
            }
    elif rec == "callsite_target_set":
        s["targets"]       = r.get("targets", [])
        s["adapter_shape"] = r.get("adapter_shape", "")
    elif rec == "callsite_adapter_graph":
        s["nodes"]         = r.get("nodes", [])
        s["edges"]         = r.get("edges", [])
        s["adapter_kind"]  = r.get("adapter_kind", "")
        s["adapter_class"] = r.get("adapter_class", "")
    elif rec == "diagnostic":
        s["reason"] = cs.get("reason", "")
    return s
