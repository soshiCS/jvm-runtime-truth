"""
JSONL parser and index builder for runtime_targets.jsonl.

Produces a structured index consumed by the Flask API layer.
"""
import json
import os
from collections import defaultdict
from pathlib import Path


# ---------------------------------------------------------------------------
# Evidence levels for staticization readiness.
# Lower number = stronger/more direct proof.
# ---------------------------------------------------------------------------
EVIDENCE_LEVELS = {
    "LINKAGE_GUARANTEED":       1,   # JVM confirmed at linkage time
    "ADAPTER_GRAPH_EXACT":      2,   # full MH adapter chain present, all_exact=True
    "OBSERVED_RUNTIME_TARGET":  3,   # target observed at runtime (OBSERVED_ONLY)
    "BYTECODE_DERIVED":         4,   # derived by reading captured bytecode
    "PROXY_BYTECODE_DERIVED":   5,   # proxy dispatch chain from proxy class bytecode
    "UNKNOWN":                  99,  # no evidence captured
}


def _scan_sidecars(artifacts_dir: Path) -> dict:
    """Read *.metadata.txt sidecar files from the artifacts directory.

    Returns a dict mapping crc32 → {trace_id, generated_by, class_name}.
    These sidecar files are emitted alongside per-trace recover class files
    and carry the trace_id that links a hidden lambda class CRC to its
    source invokedynamic callsite.
    """
    result: dict = {}
    if not artifacts_dir.is_dir():
        return result
    for f in artifacts_dir.glob("*.metadata.txt"):
        data: dict = {}
        try:
            for line in f.read_text().splitlines():
                if "=" in line:
                    k, _, v = line.partition("=")
                    data[k.strip()] = v.strip()
        except OSError:
            continue
        crc = data.get("crc32")
        trace_id_raw = data.get("trace_id")
        if crc and trace_id_raw is not None:
            try:
                trace_id = int(trace_id_raw)
            except ValueError:
                continue
            result[crc] = {
                "trace_id":     trace_id,
                "generated_by": data.get("generated_by", ""),
                "class_name":   data.get("class_name", ""),
            }
    return result


def load_and_index(jsonl_path: str, user_prefixes: list | None = None) -> dict:
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
    # Scan for per-trace artifact sidecar files in the sibling artifacts/ directory.
    # These carry the trace_id → CRC link needed to map lambda class instances back
    # to their source invokedynamic callsites.
    artifacts_dir = Path(jsonl_path).parent / "artifacts"
    sidecar_map = _scan_sidecars(artifacts_dir)

    idx = _build_index(records, user_prefixes=user_prefixes or [], sidecar_map=sidecar_map)
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


def _build_index(records: list, user_prefixes: list | None = None,
                 sidecar_map: dict | None = None) -> dict:
    pfx = user_prefixes or []
    sidecar_map = sidecar_map or {}   # crc32 → {trace_id, generated_by, class_name}

    def _is_user_src(cls: str) -> bool:
        return bool(pfx) and bool(cls) and any(cls.startswith(p) for p in pfx)

    # class_name -> class entry dict
    classes: dict[str, dict] = {}

    def cls_entry(name: str) -> dict:
        n = _norm(name)
        if not n:
            return {}
        if n not in classes:
            classes[n] = {
                "name":                    n,
                "record_types":            set(),
                "generated":               False,
                "has_artifacts":           False,
                "has_callsites_src":       False,
                "has_callsites_tgt":       False,
                "has_user_callsites_src":  False,  # src matches user prefix
                "has_user_callsites_tgt":  False,  # tgt of a user-prefix src callsite
                "has_diagnostics":         False,
            }
        return classes[n]

    # (class, loader) -> {"final": record|None, "original": record|None}
    artifacts: dict[tuple, dict] = defaultdict(lambda: {"final": None, "original": None})

    # crc → generated_class record for hidden lambdas.
    # Populated instead of cls_entry() so _crc pseudo-names never appear as classes.
    crc_to_gen_cls: dict[str, dict] = {}

    # (class, method) -> [callsite entry]
    callsites_by: dict[str, list] = defaultdict(list)

    all_callsites: list[dict] = []   # indexed by position
    diagnostics:   list[dict] = []
    generated_cls: list[dict] = []
    export_summary: dict | None = None

    # runtime_name (+0x...) -> {"artifact_crc": str, "loader_id": str}
    hidden_id_map: dict[str, dict] = {}

    # artifact_path -> CRC of the last bytecode_artifact record that named this path.
    # Hidden lambda instances share a filename; the last-written CRC is what's on disk.
    path_to_last_crc: dict[str, str] = {}

    # trace_id (int) -> LambdaMetafactory impl fields, from invokedynamic callsite_target.
    # Used to back-populate lmf_impl_* on +0x hidden class entries.
    trace_to_lmf: dict[int, dict] = {}

    # All runtime_target raw records, stored for post-loop proxy/handler derivation.
    runtime_targets_raw: list[dict] = []

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
            crc    = r.get("crc", "")
            if cls:
                if r.get("hidden"):
                    # The lookup key used by find_best_artifact() and the back-fill loop is
                    # always f"{base}_crc{crc}" (base = name without +0x or _crc suffix).
                    # Normalize here: real JSONL already carries the _crc suffix in cls;
                    # test fixtures may not.  Either way produce the canonical key.
                    canonical = cls if (crc and cls.endswith(f"_crc{crc}")) else (
                        f"{cls}_crc{crc}" if crc else cls
                    )
                    artifacts[(canonical, loader)][kind] = r
                    # Track which CRC was written last to each export file.
                    # Hidden lambda instances share a filename; later writes overwrite earlier ones.
                    if crc and r.get("artifact_path"):
                        path_to_last_crc[r["artifact_path"]] = crc
                else:
                    e = cls_entry(cls)
                    e["record_types"].add("bytecode_artifact")
                    e["has_artifacts"] = True
                    artifacts[(cls, loader)][kind] = r
            continue

        # ------------------------------------------------------------------ generated_class
        if rtype == "generated_class":
            cls = _norm(r.get("class", ""))
            crc = r.get("crc", "")
            if cls:
                if r.get("hidden"):
                    # Hidden lambda: the _crc-suffixed name is NOT a stable runtime identity.
                    # The real per-instance +0x name comes from hidden_class_identity records.
                    # Stash metadata for later attachment to the +0x entry; do NOT create a
                    # class entry here so _crc pseudo-names never appear in the sidebar.
                    if crc:
                        crc_to_gen_cls[crc] = r
                    generated_cls.append(r)
                else:
                    e = cls_entry(cls)
                    e["record_types"].add("generated_class")
                    e["generated"] = True
                    generated_cls.append(r)
            continue

        # ------------------------------------------------------------------ callsite_target
        if rtype == "callsite_target":
            src_cls, src_meth = _src(r)
            tgt_cls = _norm(r.get("target_class", ""))
            is_user = _is_user_src(src_cls)
            if src_cls:
                e = cls_entry(src_cls)
                e["record_types"].add("callsite_target")
                e["has_callsites_src"] = True
                if is_user:
                    e["has_user_callsites_src"] = True
            if tgt_cls:
                e = cls_entry(tgt_cls)
                e["has_callsites_tgt"] = True
                if is_user:
                    e["has_user_callsites_tgt"] = True
            cs = {
                "record":            rtype,
                "source_class":      src_cls,
                "source_method":     src_meth,
                "source_descriptor": r.get("source_descriptor") or r.get("src_desc", ""),
                "source_bci":        r.get("source_bci", -1),
                "source_opcode":     r.get("source_opcode", ""),
                "source_cp_index":   r.get("source_cp_index", -1),
                "category":          r.get("category", ""),
                "evidence":          r.get("evidence", ""),
                "exact":             bool(r.get("exact", False)),
                "all_exact":         bool(r.get("exact", False)),
                "raw":               r,
            }
            push_callsite(cs)
            # For invokedynamic LambdaMetafactory callsites: record the impl method so we can
            # back-populate it on the +0x hidden class entry via sidecar trace_id resolution.
            if (r.get("category") == "invokedynamic"
                    and r.get("lmf_impl_method")
                    and r.get("trace_id") is not None):
                trace_to_lmf[int(r["trace_id"])] = {
                    "lmf_impl_class":      _norm(r.get("lmf_impl_class", "")),
                    "lmf_impl_method":     r.get("lmf_impl_method", ""),
                    "lmf_impl_descriptor": r.get("lmf_impl_descriptor", ""),
                }
            continue

        # ------------------------------------------------------------------ callsite_target_set
        if rtype == "callsite_target_set":
            src_cls, src_meth = _src(r)
            is_user = _is_user_src(src_cls)
            if src_cls:
                e = cls_entry(src_cls)
                e["record_types"].add("callsite_target_set")
                e["has_callsites_src"] = True
                if is_user:
                    e["has_user_callsites_src"] = True
                for t in r.get("targets", []):
                    tc = _norm(t.get("class", ""))
                    if tc:
                        te = cls_entry(tc)
                        te["has_callsites_tgt"] = True
                        if is_user:
                            te["has_user_callsites_tgt"] = True
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
            is_user = _is_user_src(src_cls)
            if src_cls:
                e = cls_entry(src_cls)
                e["record_types"].add("callsite_adapter_graph")
                e["has_callsites_src"] = True
                if is_user:
                    e["has_user_callsites_src"] = True
                for n in r.get("nodes", []):
                    nc = _norm(n.get("class", ""))
                    if nc:
                        ne = cls_entry(nc)
                        ne["has_callsites_tgt"] = True
                        if is_user:
                            ne["has_user_callsites_tgt"] = True
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

        # ------------------------------------------------------------------ hidden_class_identity
        if rtype == "hidden_class_identity":
            rname = r.get("runtime_name", "")
            crc   = r.get("artifact_crc", "")
            lid   = r.get("loader_id", "")
            if rname and crc:
                hidden_id_map[rname] = {"artifact_crc": crc, "loader_id": lid}
                e = cls_entry(rname)
                e["record_types"].add("hidden_class_identity")
                e["artifact_crc"]   = crc
                e["artifact_loader"] = lid
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
            runtime_targets_raw.append(r)
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
                "source_descriptor": r.get("source_descriptor") or r.get("src_descriptor") or r.get("src_desc", ""),
                "source_bci":        r.get("source_bci") if r.get("source_bci") is not None else r.get("src_bci", -1),
                "source_cp_index":   r.get("source_cp_index") if r.get("source_cp_index") is not None else r.get("src_cp_index", -1),
                "source_opcode":     r.get("source_opcode") or r.get("src_opcode", ""),
                "category":          r.get("category", ""),
                "reason":            r.get("reason", ""),
                "all_exact":         False,
                "raw":               r,
            }
            push_callsite(cs)
            diagnostics.append(cs)
            continue

    # -- Link diagnostic sibling BCIs to their primary resolved callsite ----
    # Build (src_class, src_method, cp_index) -> first exact callsite_target
    cp_to_primary: dict = {}
    for cs in all_callsites:
        if cs["record"] == "callsite_target":
            cp_idx = cs.get("source_cp_index", -1)
            if cp_idx >= 0:
                key = (cs["source_class"], cs["source_method"], cp_idx)
                if key not in cp_to_primary:
                    cp_to_primary[key] = cs
    for cs in diagnostics:
        cp_idx = cs.get("source_cp_index", -1)
        if cp_idx >= 0:
            key = (cs["source_class"], cs["source_method"], cp_idx)
            if key in cp_to_primary:
                primary = cp_to_primary[key]
                cs["linked_primary_bci"] = primary.get("source_bci", -1)
                praw = primary.get("raw", {})
                if praw.get("target_class"):
                    cs["linked_primary_target"] = {
                        "class":      _norm(praw.get("target_class", "")),
                        "method":     praw.get("target_method", ""),
                        "descriptor": praw.get("target_descriptor", ""),
                    }

    # -- Back-fill has_artifacts on hidden class instances ------------------
    # bytecode_artifact records for hidden classes use the base class name (no +0x),
    # so cls_entry() was skipped for them.  Now that hidden_id_map is fully populated,
    # walk each +0x class entry and check whether its CRC-indexed artifact was stored.
    # Also derive lmf_impl_* (LambdaMetafactory impl method) via the sidecar trace_id
    # link so a staticizer can find the final target without cross-referencing JSONL.
    for rname, hid_info in hidden_id_map.items():
        crc    = hid_info.get("artifact_crc", "")
        loader = hid_info.get("loader_id", "")
        if not crc or rname not in classes:
            continue
        # base name is the part before +0x
        base   = rname.split("+0x")[0]
        crc_cls = f"{base}_crc{crc}"
        e = classes[rname]
        if (crc_cls, loader) in artifacts:
            e["has_artifacts"] = True
            e["record_types"].add("bytecode_artifact")
        # Attach generated_class metadata (generated_by, source_trigger) from stash.
        gen = crc_to_gen_cls.get(crc)
        if gen:
            e["generated"]      = True
            e["generated_by"]   = gen.get("generated_by", "")
            e["source_trigger"] = gen.get("source_trigger", "")
            e["record_types"].add("generated_class")
        # Derive trace_id from sidecar, then look up LMF impl method.
        sidecar = sidecar_map.get(crc, {})
        trace_id = sidecar.get("trace_id")
        if trace_id is not None:
            e["artifact_trace_id"] = trace_id
            lmf = trace_to_lmf.get(trace_id)
            if lmf:
                e["lmf_impl_class"]      = lmf["lmf_impl_class"]
                e["lmf_impl_method"]     = lmf["lmf_impl_method"]
                e["lmf_impl_descriptor"] = lmf["lmf_impl_descriptor"]

    # -- Build lambda family grouping entries ---------------------------------
    # For each set of 2+ hidden class instances sharing the same base name,
    # add a virtual "lambda_family" class entry so the UI can show a summary
    # instead of presenting the base name as if it were a real class.
    lambda_by_base: dict[str, list] = {}
    for rname in hidden_id_map:
        if rname in classes:
            base = rname.split("+0x")[0]
            lambda_by_base.setdefault(base, []).append(rname)

    for base, instances in lambda_by_base.items():
        if len(instances) < 2:
            continue
        existing = classes.get(base)
        # Only skip if there's a real class entry (more than just generated_class records)
        if existing and (existing.get("record_types", set()) - {"generated_class"}):
            continue
        instances_sorted = sorted(instances)
        # Aggregate lmf_impl_* from member instances (all members share the same base name
        # so they all have the same functional interface, but may have different impls).
        # Collect the distinct set for the family summary.
        family_impls = []
        seen_impls: set = set()
        for inst in instances_sorted:
            ic = classes[inst]
            impl_key = (ic.get("lmf_impl_class",""), ic.get("lmf_impl_method",""), ic.get("lmf_impl_descriptor",""))
            if impl_key[1] and impl_key not in seen_impls:
                seen_impls.add(impl_key)
                family_impls.append({
                    "lmf_impl_class":      impl_key[0],
                    "lmf_impl_method":     impl_key[1],
                    "lmf_impl_descriptor": impl_key[2],
                })
        classes[base] = {
            "name":                    base,
            "record_types":            {"lambda_family"},
            "generated":               True,
            "has_artifacts":           False,
            "has_callsites_src":       any(classes[i].get("has_callsites_src") for i in instances),
            "has_callsites_tgt":       any(classes[i].get("has_callsites_tgt") for i in instances),
            "has_user_callsites_src":  any(classes[i].get("has_user_callsites_src") for i in instances),
            "has_user_callsites_tgt":  any(classes[i].get("has_user_callsites_tgt") for i in instances),
            "has_diagnostics":         False,
            "is_lambda_family":        True,
            "lambda_instances":        instances_sorted,
            "lambda_count":            len(instances),
            "lambda_impls":            family_impls,  # distinct impl methods across all members
        }

    # -- Proxy class detection and handler linkage --------------------------
    # JDK dynamic proxy classes ($Proxy*) are generated at runtime; the InvocationHandler
    # that was installed on each proxy can be identified from the runtime_target records:
    # the handler lambda's <init> BCI precedes the proxy's <init> BCI in the same source
    # method.  We derive this link here so the UI/staticizer can consume it directly
    # without reading $Proxy*.class bytecode.
    #
    # handler_events: (src_class, src_method, bci) -> hidden class name (the handler lambda)
    handler_events: dict[tuple, str] = {}
    for rt in runtime_targets_raw:
        tc = _norm(rt.get("target_class", ""))
        tm = rt.get("target_method", "")
        if tm == "<init>" and "+0x" in tc:
            src = _norm(rt.get("source_class", ""))
            sm  = rt.get("source_method", "")
            bci = rt.get("source_bci", -1)
            if bci >= 0:
                handler_events[(src, sm, bci)] = tc

    for rt in runtime_targets_raw:
        tc = _norm(rt.get("target_class", ""))
        tm = rt.get("target_method", "")
        # Proxy classes created via reflection have target names like jdk/proxy1/$Proxy0.
        # Match only on the final path segment starting with "$Proxy" (not builder classes).
        if tm != "<init>" or not tc:
            continue
        simple = tc.split("/")[-1]
        is_proxy = simple.startswith("$Proxy")
        if not is_proxy:
            continue
        src = _norm(rt.get("source_class", ""))
        sm  = rt.get("source_method", "")
        bci = rt.get("source_bci", -1)
        if tc in classes:
            classes[tc]["is_proxy_class"] = True
        else:
            # Proxy class may not have been seen in callsite_target records yet
            e = cls_entry(tc)
            e["is_proxy_class"] = True
        # Find the closest preceding handler-lambda creation in the same source method
        best_handler: str | None = None
        best_bci: int = -1
        for (h_src, h_sm, h_bci), handler_cls in handler_events.items():
            if h_src == src and h_sm == sm and 0 <= h_bci < bci and h_bci > best_bci:
                best_handler = handler_cls
                best_bci = h_bci
        if best_handler:
            classes[tc]["proxy_handler"] = best_handler
            # Back-annotate the handler class with which proxy it serves
            if best_handler in classes:
                classes[best_handler].setdefault("proxy_for", [])
                if tc not in classes[best_handler]["proxy_for"]:
                    classes[best_handler]["proxy_for"].append(tc)

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
        "classes":            sorted_classes,
        "classes_by_name":    classes,
        "methods_by_class":   methods_by_class,
        "callsites_by":       dict(callsites_by),
        "all_callsites":      all_callsites,
        "artifact_by_class":  dict(artifact_by_class),
        "hidden_id_map":      hidden_id_map,
        "path_to_last_crc":   path_to_last_crc,
        "diagnostics":        diagnostics,
        "generated_classes":  generated_cls,
        "export_summary":     export_summary,
        "total_records":      len(records),
    }


# ---------------------------------------------------------------------------
# Public helpers used by app.py
# ---------------------------------------------------------------------------

def find_best_artifact(index: dict, class_name: str, loader_id: str | None = None) -> dict | None:
    """Return the best artifact record for a class (prefer final, then original).

    For hidden lambda classes (containing '+0x'), resolves the exact artifact via
    the hidden_class_identity mapping to get the CRC-suffixed artifact name.
    No fallback guessing — if the mapping is absent, returns None.
    """
    by_class = index.get("artifact_by_class", {})
    cls = _norm(class_name)

    # For hidden class runtime names (contain +0x), use hidden_id_map for exact resolution
    if "+0x" in cls:
        hid = index.get("hidden_id_map", {}).get(cls)
        if hid is None:
            return None  # no identity record — capture/export bug
        crc = hid["artifact_crc"]
        # Derive the base name (strip +0x... suffix) and form the CRC-suffixed artifact name
        base = cls.split("+0x")[0]
        crc_cls = f"{base}_crc{crc}"
        cls = crc_cls
        loader_id = loader_id or hid.get("loader_id")

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
        ap  = artifact.get("artifact_path")
        artifact = dict(artifact)
        artifact["_exists"] = bool(ap and os.path.isfile(ap))
        # For hidden class artifacts: note whether the export file was overwritten
        # by a later lambda instance (last-write-wins on a shared filename).
        if ap and artifact.get("hidden"):
            last_crc = index.get("path_to_last_crc", {}).get(ap)
            if last_crc and last_crc != artifact.get("crc"):
                artifact["_file_crc"] = last_crc   # what's actually in the file
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
        "source_cp_index":   cs.get("source_cp_index", r.get("source_cp_index", r.get("src_cp_index", -1))),
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
        s["staticizable"]  = r.get("staticizable", False)
        if r.get("staticization_blockers"):
            s["staticization_blockers"] = r.get("staticization_blockers")
    elif rec == "diagnostic":
        s["reason"] = cs.get("reason", "")
        if cs.get("linked_primary_bci") is not None:
            s["linked_primary_bci"] = cs["linked_primary_bci"]
        if cs.get("linked_primary_target"):
            s["linked_primary_target"] = cs["linked_primary_target"]
    return s
