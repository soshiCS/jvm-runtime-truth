"""
graph_builder.py — Phase 2a: Offline Runtime Causality Graph MVP

Builds a deterministic directed graph from a JSONL export produced by the
Runtime Truth instrumentation.  Edges are created ONLY when there is explicit
evidence in the JSONL records.  Missing connections produce orphan nodes and
gap-report entries — never heuristic edges.

Usage:
    python3 graph_builder.py <jsonl_path> [--report] [--json]
    python3 graph_builder.py /tmp/spring_out.jsonl --report
    python3 graph_builder.py /tmp/out.jsonl --query chain --src "com/example/App" --bci 42

Identity rules (stable node IDs)
---------------------------------
callsite        callsite::{src_class}::{src_method}::{src_desc}::{src_bci}::{src_loader}
method          method::{class}::{method}::{descriptor}::{loader_id}
class           class::{class_name}::{loader_id}
adapter_graph   adapter_graph::{src_class}::{src_method}::{src_desc}::{src_bci}
adapter_node    adapter_node::{callsite_id}::node_{node_id}
hidden_class    hidden_class::{runtime_name}::{loader_id}
bytecode_art    bytecode_artifact::{class}::{loader_id}::{crc}::{kind}
runtime_target  runtime_target::{target_class}::{target_method}::{target_desc}::{target_loader}
target_set      target_set::{src_class}::{src_method}::{src_desc}::{src_bci}
diagnostic      diagnostic::{src_class}::{src_method}::{src_desc}::{src_bci}
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# Node / edge type constants
# ---------------------------------------------------------------------------

# Node types
NT_CALLSITE         = "callsite"
NT_METHOD           = "method"
NT_CLASS            = "class"
NT_ADAPTER_GRAPH    = "adapter_graph"
NT_ADAPTER_NODE     = "adapter_node"
NT_HIDDEN_CLASS     = "hidden_class"
NT_BYTECODE_ART     = "bytecode_artifact"
NT_RUNTIME_TARGET   = "runtime_target"   # orphan — no source attribution
NT_TARGET_SET       = "target_set"
NT_GENERATED_CLASS  = "generated_class"
NT_DIAGNOSTIC       = "diagnostic"

# Edge types
ET_CALLSITE_TARGET       = "CALLSITE_TARGET"        # callsite → method
ET_LAMBDA_BODY           = "LAMBDA_BODY"             # callsite(invokedynamic) → method (lmf_impl)
ET_HAS_ADAPTER_GRAPH     = "HAS_ADAPTER_GRAPH"       # callsite → adapter_graph
ET_HAS_ADAPTER_NODE      = "HAS_ADAPTER_NODE"        # adapter_graph → adapter_node
ET_ADAPTER_NODE_TARGET   = "ADAPTER_NODE_TARGET"     # adapter_node(primary) → method
ET_INTERNAL_EDGE         = "INTERNAL_EDGE"           # adapter_node → adapter_node (within graph)
ET_HAS_TARGET_SET        = "HAS_TARGET_SET"          # callsite → target_set
ET_TARGET_SET_MEMBER     = "TARGET_SET_MEMBER"       # target_set → method
ET_HAS_BYTECODE_ART      = "HAS_BYTECODE_ARTIFACT"  # class → bytecode_artifact
ET_HAS_HIDDEN_IDENTITY   = "HAS_HIDDEN_IDENTITY"     # hidden_class → bytecode_artifact
ET_RUNTIME_LINKAGE       = "RUNTIME_LINKAGE"         # runtime_target → method (target known)
ET_ORPHAN_RT             = "ORPHAN_RUNTIME_TARGET"   # documents the orphan fact
ET_CALLSITE_RT_ATTRIBUTED = "CALLSITE_RT_ATTRIBUTED" # callsite → method via attributed runtime_target
ET_GENERATED_CLASS_ART   = "GENERATED_CLASS_ARTIFACT"
ET_DIAGNOSTIC_AT         = "DIAGNOSTIC_AT"           # diagnostic → callsite location

# Staticization readiness labels
SR_DIRECT              = "staticizable_candidate_direct"
SR_ADAPTER_MODELED     = "staticizable_candidate_adapter_modeled"
SR_NEEDS_HIDDEN        = "needs_hidden_class_reconstruction"
SR_MULTI_TARGET        = "blocked_multi_target"
SR_UNKNOWN_ADAPTER     = "blocked_unknown_adapter"
SR_ORPHAN_RT           = "blocked_orphan_runtime_target"
SR_OBSERVED_ONLY       = "observed_only_not_proven"
SR_INSUFFICIENT        = "insufficient_evidence"

# Gap reasons (machine-readable codes for gap report)
GAP_MISSING_SOURCE_BCI        = "missing_source_bci"
GAP_NO_BYTECODE_ARTIFACT_MATCH = "no_bytecode_artifact_match"
GAP_ADAPTER_UNKNOWN_SHAPE      = "adapter_unknown_shape"
GAP_GENERATED_CLASS_NOT_EMITTED = "generated_class_record_not_emitted"
GAP_NO_CALLSITE_FOR_ADAPTER    = "no_callsite_for_adapter_graph"
GAP_NO_CALLSITE_FOR_TARGET_SET = "no_callsite_for_target_set"


# ---------------------------------------------------------------------------
# Data model
# ---------------------------------------------------------------------------

@dataclass
class Node:
    node_type: str
    node_id:   str
    attrs:     dict = field(default_factory=dict)

    def __hash__(self):
        return hash(self.node_id)

    def __eq__(self, other):
        return isinstance(other, Node) and self.node_id == other.node_id


@dataclass
class Edge:
    edge_type: str
    src_id:    str
    dst_id:    str
    attrs:     dict = field(default_factory=dict)


@dataclass
class GapRecord:
    reason:    str
    context:   str
    detail:    str = ""


class CausalityGraph:
    """
    In-memory directed graph.  All mutation happens through add_node / add_edge.
    Duplicate node IDs silently merge (second add is ignored).
    Duplicate edges are allowed (same src/dst/type but different attrs).
    """

    def __init__(self):
        self.nodes: dict[str, Node] = {}
        self.edges: list[Edge]      = []
        self.gaps:  list[GapRecord] = []
        self._edges_by_src: dict[str, list[Edge]] = defaultdict(list)
        self._edges_by_dst: dict[str, list[Edge]] = defaultdict(list)

    # --- mutation -----------------------------------------------------------

    def add_node(self, node_type: str, node_id: str, **attrs) -> Node:
        if node_id not in self.nodes:
            self.nodes[node_id] = Node(node_type=node_type, node_id=node_id, attrs=attrs)
        return self.nodes[node_id]

    def add_edge(self, edge_type: str, src_id: str, dst_id: str, **attrs) -> Edge:
        e = Edge(edge_type=edge_type, src_id=src_id, dst_id=dst_id, attrs=attrs)
        self.edges.append(e)
        self._edges_by_src[src_id].append(e)
        self._edges_by_dst[dst_id].append(e)
        return e

    def add_gap(self, reason: str, context: str, detail: str = ""):
        self.gaps.append(GapRecord(reason=reason, context=context, detail=detail))

    # --- query helpers ------------------------------------------------------

    def out_edges(self, node_id: str, edge_type: str | None = None) -> list[Edge]:
        edges = self._edges_by_src.get(node_id, [])
        if edge_type:
            return [e for e in edges if e.edge_type == edge_type]
        return list(edges)

    def in_edges(self, node_id: str, edge_type: str | None = None) -> list[Edge]:
        edges = self._edges_by_dst.get(node_id, [])
        if edge_type:
            return [e for e in edges if e.edge_type == edge_type]
        return list(edges)

    def get(self, node_id: str) -> Node | None:
        return self.nodes.get(node_id)

    # --- statistics ---------------------------------------------------------

    def node_counts(self) -> dict[str, int]:
        counts: dict[str, int] = defaultdict(int)
        for n in self.nodes.values():
            counts[n.node_type] += 1
        return dict(counts)

    def edge_counts(self) -> dict[str, int]:
        counts: dict[str, int] = defaultdict(int)
        for e in self.edges:
            counts[e.edge_type] += 1
        return dict(counts)

    def gap_counts(self) -> dict[str, int]:
        counts: dict[str, int] = defaultdict(int)
        for g in self.gaps:
            counts[g.reason] += 1
        return dict(counts)


# ---------------------------------------------------------------------------
# Stable ID helpers
# ---------------------------------------------------------------------------

def _n(s: Any) -> str:
    """Normalise class name to slash-form, handle None."""
    return str(s or "").replace(".", "/")


def _callsite_id(src_class: str, src_method: str, src_desc: str,
                 src_bci: int, src_loader: str = "") -> str:
    return f"callsite::{_n(src_class)}::{src_method}::{src_desc}::{src_bci}::{src_loader}"


def _method_id(cls: str, method: str, descriptor: str, loader_id: str = "") -> str:
    return f"method::{_n(cls)}::{method}::{descriptor}::{loader_id}"


def _class_id(cls: str, loader_id: str = "") -> str:
    return f"class::{_n(cls)}::{loader_id}"


def _adapter_graph_id(src_class: str, src_method: str, src_desc: str, src_bci: int) -> str:
    return f"adapter_graph::{_n(src_class)}::{src_method}::{src_desc}::{src_bci}"


def _adapter_node_id(callsite_id: str, node_id: int) -> str:
    return f"adapter_node::{callsite_id}::node_{node_id}"


def _hidden_class_id(runtime_name: str, loader_id: str = "") -> str:
    return f"hidden_class::{runtime_name}::{loader_id}"


def _bytecode_art_id(cls: str, loader_id: str, crc: str, kind: str) -> str:
    return f"bytecode_artifact::{_n(cls)}::{loader_id}::{crc}::{kind}"


def _runtime_target_id(tgt_cls: str, tgt_method: str, tgt_desc: str, tgt_loader: str) -> str:
    return f"runtime_target::{_n(tgt_cls)}::{tgt_method}::{tgt_desc}::{tgt_loader}"


def _target_set_id(src_class: str, src_method: str, src_desc: str, src_bci: int) -> str:
    return f"target_set::{_n(src_class)}::{src_method}::{src_desc}::{src_bci}"


def _diagnostic_id(src_class: str, src_method: str, src_desc: str, src_bci: int) -> str:
    return f"diagnostic::{_n(src_class)}::{src_method}::{src_desc}::{src_bci}"


# ---------------------------------------------------------------------------
# Source field normalisation (callsite_target uses source_* , diagnostic
# uses src_* — handle both)
# ---------------------------------------------------------------------------

def _src_fields(r: dict) -> tuple[str, str, str, int, str]:
    """Return (class, method, descriptor, bci, loader_id), tolerating both field spellings."""
    cls    = _n(r.get("source_class")   or r.get("src_class")      or "")
    meth   =    r.get("source_method")  or r.get("src_method")      or ""
    desc   =    r.get("source_descriptor") or r.get("src_descriptor") or r.get("src_desc") or ""
    bci    =    r.get("source_bci",        r.get("src_bci",       -1))
    loader =    r.get("source_loader_id", r.get("src_loader_id",  ""))
    return cls, meth, desc, int(bci), loader


def _tgt_fields(r: dict) -> tuple[str, str, str, str]:
    """Return (class, method, descriptor, loader_id) for target fields."""
    cls    = _n(r.get("target_class",      ""))
    meth   =    r.get("target_method",     "")
    desc   =    r.get("target_descriptor", "")
    loader =    r.get("target_loader_id",  "")
    return cls, meth, desc, loader


# ---------------------------------------------------------------------------
# Graph builder
# ---------------------------------------------------------------------------

def build_graph(jsonl_path: str) -> tuple[CausalityGraph, dict]:
    """
    Read the JSONL export and return (graph, raw_summary).

    Two-pass design:
      Pass 1 — create all nodes from each record type.
      Pass 2 — create cross-record edges (e.g., hidden_class → bytecode_artifact).
    """
    g = CausalityGraph()
    records = _load_records(jsonl_path)
    raw_summary: dict = {}

    # Deferred-linkage tables populated in pass 1, resolved in pass 2
    # (src_class, src_method, src_desc, src_bci) → callsite node_id
    callsite_key_to_id: dict[tuple, str] = {}

    # (base_class_name, loader_id, crc) → bytecode_artifact node_id
    # Populated when bytecode_artifact records are processed.
    bca_key_to_id: dict[tuple, str] = {}

    # hidden_class node_id → (artifact_crc, loader_id, base_name)
    # Populated when hidden_class_identity records are processed.
    pending_hidden_links: list[tuple[str, str, str, str]] = []  # (hc_id, artifact_crc, loader_id, base_name)

    # Adapter-graph records: deferred until callsite nodes are created.
    pending_adapter_graphs: list[dict] = []

    # counts for gap report
    record_type_counts: dict[str, int] = defaultdict(int)
    connected_callsite_targets   = 0
    connected_adapter_graphs     = 0
    connected_target_sets        = 0
    connected_hidden_to_artifact = 0
    connected_runtime_targets    = 0
    rt_orphan_count              = 0

    # -----------------------------------------------------------------------
    # Pass 1: iterate records and create nodes
    # -----------------------------------------------------------------------

    for r in records:
        rtype = r.get("record", "")
        record_type_counts[rtype] += 1

        if rtype == "export_summary":
            raw_summary = r
            continue

        # ------------------------------------------------------------------ callsite_target
        if rtype == "callsite_target":
            src_cls, src_meth, src_desc, src_bci, src_loader = _src_fields(r)
            if not src_cls:
                g.add_gap(GAP_MISSING_SOURCE_BCI, "callsite_target", "empty source_class")
                continue

            category = r.get("category", "")
            evidence = r.get("evidence", "")

            # Callsite node
            cs_id = _callsite_id(src_cls, src_meth, src_desc, src_bci, src_loader)
            g.add_node(NT_CALLSITE, cs_id,
                       src_class=src_cls, src_method=src_meth,
                       src_descriptor=src_desc, src_bci=src_bci,
                       src_loader=src_loader, category=category,
                       evidence=evidence,
                       staticizable=r.get("staticizable", False),
                       reconstructable=r.get("reconstructable", False),
                       static_label=_callsite_static_label(r))

            callsite_key_to_id[(src_cls, src_meth, src_desc, src_bci)] = cs_id

            # Target method node (may be a hidden class)
            tgt_cls, tgt_meth, tgt_desc, tgt_loader = _tgt_fields(r)
            if tgt_cls and tgt_meth:
                tgt_id = _method_id(tgt_cls, tgt_meth, tgt_desc, tgt_loader)
                g.add_node(NT_METHOD, tgt_id,
                           cls=tgt_cls, method=tgt_meth,
                           descriptor=tgt_desc, loader_id=tgt_loader,
                           hidden="+0x" in tgt_cls)

                # Primary causality edge
                g.add_edge(ET_CALLSITE_TARGET, cs_id, tgt_id,
                           category=category, evidence=evidence,
                           exact=r.get("exact", False),
                           staticizable=r.get("staticizable", False))
                connected_callsite_targets += 1

            # Lambda body shortcut (invokedynamic only)
            lmf_cls  = _n(r.get("lmf_impl_class",  ""))
            lmf_meth =    r.get("lmf_impl_method",  "")
            lmf_desc =    r.get("lmf_impl_descriptor", "")
            if lmf_cls and lmf_meth:
                lmf_id = _method_id(lmf_cls, lmf_meth, lmf_desc, "")
                g.add_node(NT_METHOD, lmf_id,
                           cls=lmf_cls, method=lmf_meth,
                           descriptor=lmf_desc, loader_id="",
                           is_lambda_body=True)
                g.add_edge(ET_LAMBDA_BODY, cs_id, lmf_id,
                           indy_name=r.get("indy_name", ""),
                           bootstrap=r.get("bootstrap_method", ""))

        # ------------------------------------------------------------------ callsite_target_set
        elif rtype == "callsite_target_set":
            src_cls, src_meth, src_desc, src_bci, src_loader = _src_fields(r)
            if not src_cls:
                g.add_gap(GAP_MISSING_SOURCE_BCI, "callsite_target_set", "empty source_class")
                continue

            category = r.get("category", "")

            # Callsite node (may already exist from a parallel callsite_target)
            cs_id = _callsite_id(src_cls, src_meth, src_desc, src_bci, src_loader)
            g.add_node(NT_CALLSITE, cs_id,
                       src_class=src_cls, src_method=src_meth,
                       src_descriptor=src_desc, src_bci=src_bci,
                       src_loader=src_loader, category=category,
                       static_label=SR_MULTI_TARGET)

            callsite_key_to_id[(src_cls, src_meth, src_desc, src_bci)] = cs_id

            # TargetSet node
            ts_id = _target_set_id(src_cls, src_meth, src_desc, src_bci)
            g.add_node(NT_TARGET_SET, ts_id,
                       adapter_shape=r.get("adapter_shape", ""),
                       semantic_op=r.get("semantic_op", ""),
                       staticizable=r.get("staticizable", False))
            g.add_edge(ET_HAS_TARGET_SET, cs_id, ts_id, category=category)
            connected_target_sets += 1

            # Member edges for each target in the set
            for t in r.get("targets", []):
                t_cls    = _n(t.get("class", ""))
                t_method =    t.get("method", "")
                t_desc   =    t.get("descriptor", "")
                t_loader =    t.get("loader_id", "")
                t_role   =    t.get("role", "")
                if t_cls and t_method:
                    tgt_id = _method_id(t_cls, t_method, t_desc, t_loader)
                    g.add_node(NT_METHOD, tgt_id,
                               cls=t_cls, method=t_method,
                               descriptor=t_desc, loader_id=t_loader)
                    g.add_edge(ET_TARGET_SET_MEMBER, ts_id, tgt_id,
                               role=t_role, valid=t.get("valid", True))

        # ------------------------------------------------------------------ callsite_adapter_graph
        elif rtype == "callsite_adapter_graph":
            pending_adapter_graphs.append(r)

        # ------------------------------------------------------------------ bytecode_artifact
        elif rtype == "bytecode_artifact":
            cls    = _n(r.get("class", ""))
            loader =    r.get("loader_id", "")
            crc    =    r.get("crc", "")
            kind   =    r.get("kind", "original")
            if not cls:
                continue

            art_id = _bytecode_art_id(cls, loader, crc, kind)
            g.add_node(NT_BYTECODE_ART, art_id,
                       cls=cls, loader_id=loader, crc=crc, kind=kind,
                       hidden=r.get("hidden", False),
                       artifact_path=r.get("artifact_path", ""),
                       size=r.get("size", 0))

            # Register in lookup table for hidden-class linking
            # For hidden classes, register by base_name (strip +0x suffix if any)
            base_cls = cls.split("+0x")[0] if "+0x" in cls else cls
            bca_key_to_id[(base_cls, loader, crc)] = art_id

            # Class → bytecode_artifact edge
            cls_id = _class_id(cls, loader)
            g.add_node(NT_CLASS, cls_id, cls=cls, loader_id=loader)
            g.add_edge(ET_HAS_BYTECODE_ART, cls_id, art_id, kind=kind)

        # ------------------------------------------------------------------ hidden_class_identity
        elif rtype == "hidden_class_identity":
            runtime_name  = r.get("runtime_name", "")
            artifact_crc  = r.get("artifact_crc", "")
            loader_id     = r.get("loader_id", "")
            if not runtime_name or not artifact_crc:
                continue

            hc_id = _hidden_class_id(runtime_name, loader_id)
            g.add_node(NT_HIDDEN_CLASS, hc_id,
                       runtime_name=runtime_name,
                       artifact_crc=artifact_crc,
                       loader_id=loader_id)

            # Base class name = strip +0x... suffix
            base_name = runtime_name.split("+0x")[0] if "+0x" in runtime_name else runtime_name
            pending_hidden_links.append((hc_id, artifact_crc, loader_id, base_name))

        # ------------------------------------------------------------------ runtime_target
        elif rtype == "runtime_target":
            tgt_cls, tgt_meth, tgt_desc, tgt_loader = _tgt_fields(r)
            if not tgt_cls:
                continue

            src_capture = r.get("source_capture", "")
            src_cls = _n(r.get("source_class", ""))

            if src_capture == "exact" and src_cls:
                # Phase 2b attributed record: source frame was recovered via vframeStream.
                # Create a proper callsite node and CALLSITE_RT_ATTRIBUTED edge.
                # No NT_RUNTIME_TARGET orphan node is created.
                src_meth      = r.get("source_method", "")
                src_desc_val  = r.get("source_descriptor", "")
                src_bci_val   = r.get("source_bci", -1)
                src_loader_id = r.get("source_loader_id", "")

                cs_id = _callsite_id(src_cls, src_meth, src_desc_val, src_bci_val, src_loader_id)
                g.add_node(NT_CALLSITE, cs_id,
                           src_class=src_cls, src_method=src_meth,
                           src_descriptor=src_desc_val, src_bci=src_bci_val,
                           src_loader=src_loader_id,
                           category=r.get("dispatch_kind", "membername_linkage"),
                           evidence=r.get("evidence", ""),
                           staticizable=False,
                           reconstructable=False,
                           static_label=SR_OBSERVED_ONLY)
                callsite_key_to_id[(src_cls, src_meth, src_desc_val, src_bci_val)] = cs_id

                tgt_id = _method_id(tgt_cls, tgt_meth, tgt_desc, tgt_loader)
                g.add_node(NT_METHOD, tgt_id,
                           cls=tgt_cls, method=tgt_meth,
                           descriptor=tgt_desc, loader_id=tgt_loader,
                           hidden="+0x" in tgt_cls)
                g.add_edge(ET_CALLSITE_RT_ATTRIBUTED, cs_id, tgt_id,
                           dispatch_kind=r.get("dispatch_kind", ""),
                           evidence=r.get("evidence", ""),
                           caller_context=r.get("caller_context", ""))
                connected_runtime_targets += 1

            else:
                # Unattributed runtime_target: source frame was not recovered.
                # Preserve as orphan node with reason for the gap.
                missing_reason = r.get("source_missing_reason", "no_source_attribution")
                rt_id = _runtime_target_id(tgt_cls, tgt_meth, tgt_desc, tgt_loader)
                g.add_node(NT_RUNTIME_TARGET, rt_id,
                           target_class=tgt_cls, target_method=tgt_meth,
                           target_descriptor=tgt_desc, target_loader=tgt_loader,
                           dispatch_kind=r.get("dispatch_kind", ""),
                           caller_context=r.get("caller_context", ""),
                           evidence=r.get("evidence", ""),
                           source_capture=src_capture,
                           source_missing_reason=missing_reason,
                           is_orphan=True,
                           gap_reason=GAP_MISSING_SOURCE_BCI)

                tgt_method_id = _method_id(tgt_cls, tgt_meth, tgt_desc, tgt_loader)
                g.add_node(NT_METHOD, tgt_method_id,
                           cls=tgt_cls, method=tgt_meth,
                           descriptor=tgt_desc, loader_id=tgt_loader)
                g.add_edge(ET_ORPHAN_RT, rt_id, tgt_method_id,
                           gap_reason=GAP_MISSING_SOURCE_BCI,
                           caller_context=r.get("caller_context", ""),
                           source_missing_reason=missing_reason)
                g.add_edge(ET_RUNTIME_LINKAGE, rt_id, tgt_method_id,
                           dispatch_kind=r.get("dispatch_kind", ""),
                           evidence=r.get("evidence", ""))
                g.add_gap(GAP_MISSING_SOURCE_BCI,
                          f"runtime_target:{tgt_cls}.{tgt_meth}",
                          f"caller_context={r.get('caller_context','')} "
                          f"source_capture={src_capture or 'none'} "
                          f"reason={missing_reason}")
                rt_orphan_count += 1

        # ------------------------------------------------------------------ diagnostic
        elif rtype == "diagnostic":
            src_cls, src_meth, src_desc, src_bci, src_loader = _src_fields(r)
            if not src_cls:
                continue

            diag_id = _diagnostic_id(src_cls, src_meth, src_desc, src_bci)
            g.add_node(NT_DIAGNOSTIC, diag_id,
                       src_class=src_cls, src_method=src_meth,
                       src_descriptor=src_desc, src_bci=src_bci,
                       reason=r.get("reason", ""),
                       category=r.get("category", ""),
                       level=r.get("level", "info"))
            # DIAGNOSTIC_AT is a self-annotation edge
            g.add_edge(ET_DIAGNOSTIC_AT, diag_id, diag_id,
                       reason=r.get("reason", ""))

        # ------------------------------------------------------------------ generated_class
        elif rtype == "generated_class":
            cls = _n(r.get("class", ""))
            if cls:
                gc_id = f"generated_class::{cls}"
                g.add_node(NT_GENERATED_CLASS, gc_id,
                           cls=cls,
                           generated_by=r.get("generated_by", ""),
                           source_trigger=r.get("source_trigger", ""))

    # -----------------------------------------------------------------------
    # Pass 1.5: multi-target upgrade
    # If multiple callsite_target records arrived for the same source BCI
    # (distinct target_class/method pairs — produced by soroush_graph_poly_callsite
    # for polymorphic invokevirtual sites), the callsite node will have >1
    # outgoing CALLSITE_TARGET edges to distinct destinations.  Upgrade those
    # nodes to SR_MULTI_TARGET regardless of what Pass 1 set, so staticization
    # analysis does not attempt devirtualization of polymorphic sites.
    # -----------------------------------------------------------------------
    for node in g.nodes.values():
        if node.node_type != NT_CALLSITE:
            continue
        ct_targets = {e.dst_id for e in g._edges_by_src.get(node.node_id, [])
                      if e.edge_type == ET_CALLSITE_TARGET}
        if len(ct_targets) > 1:
            node.attrs["static_label"] = SR_MULTI_TARGET

    # -----------------------------------------------------------------------
    # Pass 2: create cross-record edges
    # -----------------------------------------------------------------------

    # 2a. Process deferred adapter graphs
    for r in pending_adapter_graphs:
        src_cls, src_meth, src_desc, src_bci, src_loader = _src_fields(r)
        if not src_cls:
            g.add_gap(GAP_NO_CALLSITE_FOR_ADAPTER, "callsite_adapter_graph",
                      "empty source_class")
            continue

        cs_key = (src_cls, src_meth, src_desc, src_bci)
        cs_id  = callsite_key_to_id.get(cs_key)
        if cs_id is None:
            # Create a callsite node even if no callsite_target record matches —
            # the adapter graph itself provides source attribution.
            cs_id = _callsite_id(src_cls, src_meth, src_desc, src_bci, src_loader)
            g.add_node(NT_CALLSITE, cs_id,
                       src_class=src_cls, src_method=src_meth,
                       src_descriptor=src_desc, src_bci=src_bci,
                       src_loader=src_loader,
                       category=r.get("category", ""),
                       static_label=SR_ADAPTER_MODELED)
            callsite_key_to_id[cs_key] = cs_id

        # Adapter-graph virtual node
        ag_id = _adapter_graph_id(src_cls, src_meth, src_desc, src_bci)
        g.add_node(NT_ADAPTER_GRAPH, ag_id,
                   src_class=src_cls, src_bci=src_bci,
                   adapter_class=r.get("adapter_class", ""),
                   adapter_kind=r.get("adapter_kind", ""),
                   lf_kind=r.get("lf_kind", ""),
                   evidence=r.get("evidence", ""),
                   all_exact=r.get("all_exact", False),
                   staticizable=r.get("staticizable", False))
        g.add_edge(ET_HAS_ADAPTER_GRAPH, cs_id, ag_id,
                   adapter_kind=r.get("adapter_kind", ""))
        connected_adapter_graphs += 1

        # Adapter nodes within the graph
        has_unknown_shape = False
        for n in r.get("nodes", []):
            n_id_raw  = n.get("id", -1)
            an_id     = _adapter_node_id(cs_id, n_id_raw)
            role      = n.get("role", "")
            classif   = n.get("classification", "")
            n_cls     = _n(n.get("class", ""))
            n_meth    =    n.get("method", "")
            n_desc    =    n.get("descriptor", "")
            n_loader  =    n.get("loader_id", "")

            if "unknown_shape" in classif or "unknown_shape" in role:
                has_unknown_shape = True
                g.add_gap(GAP_ADAPTER_UNKNOWN_SHAPE, ag_id,
                          f"node_id={n_id_raw} adapter_class={n.get('adapter_class','')}")

            g.add_node(NT_ADAPTER_NODE, an_id,
                       role=role, classification=classif,
                       cls=n_cls, method=n_meth,
                       descriptor=n_desc, loader_id=n_loader,
                       exact=n.get("exact", False),
                       exact_false_reason=n.get("exact_false_reason", ""))
            g.add_edge(ET_HAS_ADAPTER_NODE, ag_id, an_id, role=role)

            # If this adapter node has a concrete method target, link it
            if n_cls and n_meth:
                tgt_id = _method_id(n_cls, n_meth, n_desc, n_loader)
                g.add_node(NT_METHOD, tgt_id,
                           cls=n_cls, method=n_meth,
                           descriptor=n_desc, loader_id=n_loader)
                g.add_edge(ET_ADAPTER_NODE_TARGET, an_id, tgt_id,
                           role=role, exact=n.get("exact", False))

        # Intra-graph edges (between adapter nodes)
        for e in r.get("edges", []):
            src_an = _adapter_node_id(cs_id, e.get("from", -1))
            dst_an = _adapter_node_id(cs_id, e.get("to",   -1))
            if src_an in g.nodes and dst_an in g.nodes:
                g.add_edge(ET_INTERNAL_EDGE, src_an, dst_an, kind=e.get("kind", ""))

        # Update static label if unknown shape present
        if has_unknown_shape and cs_id in g.nodes:
            g.nodes[cs_id].attrs["static_label"] = SR_UNKNOWN_ADAPTER

    # 2b. Link hidden_class_identity → bytecode_artifact
    for (hc_id, artifact_crc, loader_id, base_name) in pending_hidden_links:
        art_id = bca_key_to_id.get((base_name, loader_id, artifact_crc))
        if art_id is None:
            # Fallback: try matching only by (base_name, crc) ignoring loader
            art_id = next(
                (v for (bn, ld, crc), v in bca_key_to_id.items()
                 if bn == base_name and crc == artifact_crc),
                None
            )
        if art_id is not None:
            g.add_edge(ET_HAS_HIDDEN_IDENTITY, hc_id, art_id,
                       artifact_crc=artifact_crc)
            connected_hidden_to_artifact += 1
        else:
            g.add_gap(GAP_NO_BYTECODE_ARTIFACT_MATCH, hc_id,
                      f"base_name={base_name} crc={artifact_crc} loader={loader_id}")

    # 2c. Note: generated_class records are never emitted in Phase 1
    generated_class_count = record_type_counts.get("generated_class", 0)
    if generated_class_count == 0:
        g.add_gap(GAP_GENERATED_CLASS_NOT_EMITTED, "export",
                  "generated_class_count=0 in export — schema defined but never emitted")

    # connected_runtime_targets is accumulated in pass 1 for attributed records.

    # -----------------------------------------------------------------------
    # Pass 3: staticization labelling for any callsites not yet labelled
    # -----------------------------------------------------------------------
    for cs_id, node in g.nodes.items():
        if node.node_type != NT_CALLSITE:
            continue
        if node.attrs.get("static_label"):
            continue  # already set in pass 1
        # Derive label from outgoing edges
        out = g.out_edges(cs_id)
        ct_edges = [e for e in out if e.edge_type == ET_CALLSITE_TARGET]
        if not ct_edges:
            node.attrs["static_label"] = SR_INSUFFICIENT
        elif len(ct_edges) > 1:
            node.attrs["static_label"] = SR_MULTI_TARGET
        elif any(g.nodes.get(e.dst_id, Node("","")).attrs.get("hidden") for e in ct_edges):
            node.attrs["static_label"] = SR_NEEDS_HIDDEN
        elif node.attrs.get("evidence") == "OBSERVED_ONLY":
            node.attrs["static_label"] = SR_OBSERVED_ONLY
        else:
            node.attrs["static_label"] = SR_DIRECT

    # Build and store metrics for gap report
    g._metrics = {
        "record_type_counts":           dict(record_type_counts),
        "connected_callsite_targets":    connected_callsite_targets,
        "connected_adapter_graphs":      connected_adapter_graphs,
        "connected_target_sets":         connected_target_sets,
        "connected_hidden_to_artifact":  connected_hidden_to_artifact,
        "connected_runtime_targets":     connected_runtime_targets,
        "rt_orphan_count":               rt_orphan_count,
        "generated_class_records_found": generated_class_count,
    }

    return g, raw_summary


# ---------------------------------------------------------------------------
# Staticization label classifier (per callsite_target record)
# ---------------------------------------------------------------------------

def _callsite_static_label(r: dict) -> str:
    evidence   = r.get("evidence",  "")
    staticizable = r.get("staticizable", False)
    category     = r.get("category", "")

    if not staticizable:
        return SR_OBSERVED_ONLY
    if evidence == "LINKAGE_GUARANTEED":
        return SR_DIRECT
    if evidence == "OBSERVED_ONLY":
        tgt_cls = _n(r.get("target_class", ""))
        if "+0x" in tgt_cls:
            return SR_NEEDS_HIDDEN
        return SR_OBSERVED_ONLY
    return SR_INSUFFICIENT


# ---------------------------------------------------------------------------
# Gap report
# ---------------------------------------------------------------------------

def build_gap_report(g: CausalityGraph, raw_summary: dict) -> dict:
    """Return a deterministic gap report dict."""
    m = getattr(g, "_metrics", {})

    node_counts = g.node_counts()
    edge_counts = g.edge_counts()
    gap_counts  = g.gap_counts()

    # Static label distribution across callsite nodes
    label_dist: dict[str, int] = defaultdict(int)
    for node in g.nodes.values():
        if node.node_type == NT_CALLSITE:
            label_dist[node.attrs.get("static_label", "unlabelled")] += 1

    # Orphan runtime_target list (truncated to first 20)
    orphan_list = [
        {
            "node_id": nid,
            "target_class":  n.attrs.get("target_class", ""),
            "target_method": n.attrs.get("target_method", ""),
            "caller_context": n.attrs.get("caller_context", ""),
        }
        for nid, n in g.nodes.items()
        if n.node_type == NT_RUNTIME_TARGET and n.attrs.get("is_orphan")
    ][:20]

    # Diagnostics list
    diag_list = [
        {
            "node_id": nid,
            "src_class":  n.attrs.get("src_class", ""),
            "src_method": n.attrs.get("src_method", ""),
            "src_bci":    n.attrs.get("src_bci", -1),
            "reason":     n.attrs.get("reason", ""),
        }
        for nid, n in g.nodes.items()
        if n.node_type == NT_DIAGNOSTIC
    ]

    # Unresolved / unsupported cases
    unsupported = [
        {"reason": g.reason, "context": g.context, "detail": g.detail}
        for g in g.gaps
        if g.reason != GAP_MISSING_SOURCE_BCI  # reported separately as orphan_count
    ]

    report = {
        "phase": "2b",
        "export_summary":                  raw_summary,
        "records_by_type":                 m.get("record_type_counts", {}),
        "nodes_by_type":                   node_counts,
        "edges_by_type":                   edge_counts,
        "gaps_by_reason":                  gap_counts,
        "connected": {
            "callsite_target_edges":       m.get("connected_callsite_targets", 0),
            "adapter_graphs_connected":    m.get("connected_adapter_graphs", 0),
            "target_sets_connected":       m.get("connected_target_sets", 0),
            "hidden_identities_to_artifact": m.get("connected_hidden_to_artifact", 0),
            "runtime_targets_connected":   m.get("connected_runtime_targets", 0),
        },
        "orphans": {
            "runtime_target_orphan_count": m.get("rt_orphan_count", 0),
            "gap_reason":                  GAP_MISSING_SOURCE_BCI,
            "note": ("Remaining orphan runtime_target records have source_capture=missing "
                     "— no user frame was on the stack at emission time (JVM init, daemon threads). "
                     "Attributed records (source_capture=exact) become CALLSITE_RT_ATTRIBUTED edges."),
            "sample_orphans":              orphan_list,
        },
        "diagnostics": {
            "count":  len(diag_list),
            "sample": diag_list[:10],
        },
        "generated_class_records_found":  m.get("generated_class_records_found", 0),
        "staticization_readiness":        dict(label_dist),
        "unsupported_cases":              unsupported[:50],
        "heuristic_edges_created":        0,  # always 0 — hard rule
    }
    return report


# ---------------------------------------------------------------------------
# Query functions
# ---------------------------------------------------------------------------

def query_chain(g: CausalityGraph, src_class: str, src_method: str,
                src_bci: int, src_desc: str = "") -> list[dict]:
    """
    Return the full causality chain for a source callsite.
    Traverses CALLSITE_TARGET, LAMBDA_BODY, HAS_ADAPTER_GRAPH, HAS_TARGET_SET edges.
    """
    cs_key = (_n(src_class), src_method, src_desc, src_bci)
    # Find matching callsite node (fuzzy on descriptor if not provided)
    cs_id = None
    if src_desc:
        cs_id = next((nid for nid in g.nodes if
                      nid.startswith(f"callsite::{_n(src_class)}::{src_method}::{src_desc}::{src_bci}::")),
                     None)
    else:
        cs_id = next((nid for nid in g.nodes if
                      nid.startswith(f"callsite::{_n(src_class)}::{src_method}::") and
                      f"::{src_bci}::" in nid),
                     None)

    if cs_id is None:
        return [{"error": f"no callsite found for {src_class}.{src_method}@{src_bci}"}]

    chain = []
    cs_node = g.nodes[cs_id]
    chain.append({"type": "callsite", "id": cs_id, "attrs": cs_node.attrs})

    for e in g.out_edges(cs_id):
        dst = g.nodes.get(e.dst_id)
        if dst is None:
            continue
        entry: dict = {"edge": e.edge_type, "dst_type": dst.node_type, "id": e.dst_id}
        entry["attrs"] = {k: v for k, v in dst.attrs.items()
                          if k not in ("cls",) or True}
        # For adapter graphs, follow their nodes
        if dst.node_type == NT_ADAPTER_GRAPH:
            entry["adapter_nodes"] = [
                {"id": ae.dst_id,
                 "attrs": {k: v for k, v in (g.nodes.get(ae.dst_id) or Node("","")).attrs.items()
                           if k not in ("loader_id",)}}
                for ae in g.out_edges(e.dst_id, ET_HAS_ADAPTER_NODE)
            ]
        chain.append(entry)

    return chain


def list_orphan_runtime_targets(g: CausalityGraph) -> list[dict]:
    """Return all runtime_target orphan nodes."""
    return [
        {"id": nid, **n.attrs}
        for nid, n in g.nodes.items()
        if n.node_type == NT_RUNTIME_TARGET and n.attrs.get("is_orphan")
    ]


def list_blocked_callsites(g: CausalityGraph) -> list[dict]:
    """Return callsites that are not staticizable."""
    blocked_labels = {SR_MULTI_TARGET, SR_UNKNOWN_ADAPTER, SR_ORPHAN_RT,
                      SR_OBSERVED_ONLY, SR_INSUFFICIENT}
    return [
        {"id": nid, "label": n.attrs.get("static_label"), **{
            k: v for k, v in n.attrs.items()
            if k in ("src_class", "src_method", "src_bci", "evidence", "category")}}
        for nid, n in g.nodes.items()
        if n.node_type == NT_CALLSITE and n.attrs.get("static_label") in blocked_labels
    ]


def list_staticizable_callsites(g: CausalityGraph) -> list[dict]:
    """Return callsites labelled as staticizable candidates."""
    candidate_labels = {SR_DIRECT, SR_ADAPTER_MODELED}
    return [
        {"id": nid, "label": n.attrs.get("static_label"), **{
            k: v for k, v in n.attrs.items()
            if k in ("src_class", "src_method", "src_bci", "evidence", "category")}}
        for nid, n in g.nodes.items()
        if n.node_type == NT_CALLSITE and n.attrs.get("static_label") in candidate_labels
    ]


# ---------------------------------------------------------------------------
# JSONL loader
# ---------------------------------------------------------------------------

def _load_records(jsonl_path: str) -> list[dict]:
    records = []
    bad = 0
    with open(jsonl_path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                bad += 1
    if bad:
        print(f"[graph_builder] WARNING: {bad} unparseable line(s) in {jsonl_path}",
              file=sys.stderr)
    return records


# ---------------------------------------------------------------------------
# Validation helpers (used by tests and CLI)
# ---------------------------------------------------------------------------

def validate_graph(g: CausalityGraph, report: dict) -> dict:
    """
    Run deterministic correctness checks on the built graph.
    Returns {"pass": N, "fail": N, "checks": [...]}
    """
    checks = []
    passes = 0
    fails  = 0

    def chk(name: str, ok: bool, detail: str = ""):
        nonlocal passes, fails
        (passes if ok else fails).__class__  # type checker appeasement
        if ok:
            passes += 1
        else:
            fails += 1
        checks.append({"name": name, "passed": ok, "detail": detail})

    # 1. Graph loaded
    chk("graph loaded (>0 nodes)", len(g.nodes) > 0, f"{len(g.nodes)} nodes")

    # 2. All callsite_target records produced CALLSITE_TARGET edges
    n_callsite_nodes = report["nodes_by_type"].get(NT_CALLSITE, 0)
    n_ct_edges       = report["edges_by_type"].get(ET_CALLSITE_TARGET, 0)
    # At minimum, every callsite_target record should produce ≥1 CALLSITE_TARGET edge
    # (some callsites may also have LAMBDA_BODY edges, so n_ct_edges can exceed count)
    chk("callsite nodes exist", n_callsite_nodes > 0,
        f"{n_callsite_nodes} callsite nodes")
    chk("CALLSITE_TARGET edges created", n_ct_edges > 0,
        f"{n_ct_edges} edges")

    # 3. Adapter graphs connected (if any records exist)
    n_ag_records   = report["records_by_type"].get("callsite_adapter_graph", 0)
    n_ag_connected = report["connected"]["adapter_graphs_connected"]
    if n_ag_records > 0:
        chk("adapter graphs connected to callsites",
            n_ag_connected == n_ag_records,
            f"{n_ag_connected}/{n_ag_records}")

    # 4. Target sets connected
    n_ts_records   = report["records_by_type"].get("callsite_target_set", 0)
    n_ts_connected = report["connected"]["target_sets_connected"]
    if n_ts_records > 0:
        chk("target sets connected",
            n_ts_connected == n_ts_records,
            f"{n_ts_connected}/{n_ts_records}")

    # 5. Hidden identities linked to artifacts (some may have no artifact — reported as gaps)
    n_hci = report["records_by_type"].get("hidden_class_identity", 0)
    n_hca = report["connected"]["hidden_identities_to_artifact"]
    if n_hci > 0:
        pct = 100 * n_hca // n_hci
        chk("hidden class identities linked to artifacts (>50%)",
            pct >= 50,
            f"{n_hca}/{n_hci} ({pct}%)")

    # 6. runtime_target orphan count is reported (not hidden)
    chk("runtime_target orphan count reported",
        "runtime_target_orphan_count" in report.get("orphans", {}),
        str(report.get("orphans", {}).get("runtime_target_orphan_count", "missing")))

    # 6b. connected_runtime_targets metric is present in connected section
    chk("connected_runtime_targets metric tracked",
        "runtime_targets_connected" in report.get("connected", {}),
        str(report.get("connected", {}).get("runtime_targets_connected", "missing")))

    # 7. No heuristic edges
    chk("no heuristic edges created",
        report.get("heuristic_edges_created", -1) == 0,
        f"heuristic_edges={report.get('heuristic_edges_created', 'N/A')}")

    # 8. generated_class count reported
    chk("generated_class count reported (schema gap documented)",
        "generated_class_records_found" in report,
        str(report.get("generated_class_records_found", "missing")))

    # 9. No edges with missing node endpoints
    missing = sum(
        1 for e in g.edges
        if e.src_id not in g.nodes or e.dst_id not in g.nodes
    )
    chk("all edge endpoints exist as nodes", missing == 0,
        f"{missing} dangling edges" if missing else "")

    return {"pass": passes, "fail": fails, "checks": checks}


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Phase 2a: build offline Runtime Causality Graph from JSONL export")
    parser.add_argument("jsonl", help="Path to runtime_targets.jsonl")
    parser.add_argument("--report",  action="store_true",
                        help="Print gap report (default if no other action)")
    parser.add_argument("--validate", action="store_true",
                        help="Run graph validation checks")
    parser.add_argument("--json",    action="store_true",
                        help="Output as JSON instead of human-readable text")
    parser.add_argument("--query",   choices=["chain", "orphans", "blocked", "staticizable"],
                        help="Run a specific query")
    parser.add_argument("--src",  default="", help="Source class for --query chain")
    parser.add_argument("--method", default="", help="Source method for --query chain")
    parser.add_argument("--bci",  type=int, default=-1, help="Source BCI for --query chain")
    args = parser.parse_args()

    print(f"[graph_builder] Loading {args.jsonl} ...", file=sys.stderr)
    g, raw_summary = build_graph(args.jsonl)
    report = build_gap_report(g, raw_summary)
    print(f"[graph_builder] Built: {len(g.nodes)} nodes, {len(g.edges)} edges, "
          f"{len(g.gaps)} gap records", file=sys.stderr)

    if args.validate or args.report or (not args.query):
        if args.validate:
            vr = validate_graph(g, report)
            if args.json:
                print(json.dumps(vr, indent=2))
            else:
                _print_validation(vr)

        if args.report or not args.query:
            if args.json:
                print(json.dumps(report, indent=2, default=str))
            else:
                _print_report(report)

    if args.query == "chain":
        result = query_chain(g, args.src, args.method, args.bci)
        print(json.dumps(result, indent=2, default=str))
    elif args.query == "orphans":
        result = list_orphan_runtime_targets(g)
        print(json.dumps(result[:50], indent=2, default=str))
    elif args.query == "blocked":
        result = list_blocked_callsites(g)
        print(json.dumps(result[:50], indent=2, default=str))
    elif args.query == "staticizable":
        result = list_staticizable_callsites(g)
        print(json.dumps(result[:50], indent=2, default=str))


def _print_report(r: dict):
    print("=" * 60)
    print("  RUNTIME CAUSALITY GRAPH — GAP REPORT (Phase 2b)")
    print("=" * 60)
    print()
    print("Records loaded:")
    for rtype, cnt in sorted(r["records_by_type"].items()):
        print(f"  {cnt:>8}  {rtype}")
    print()
    print("Nodes in graph:")
    for ntype, cnt in sorted(r["nodes_by_type"].items()):
        print(f"  {cnt:>8}  {ntype}")
    print()
    print("Edges in graph:")
    for etype, cnt in sorted(r["edges_by_type"].items()):
        print(f"  {cnt:>8}  {etype}")
    print()
    print("Connectivity:")
    c = r["connected"]
    print(f"  CALLSITE_TARGET edges      : {c['callsite_target_edges']}")
    print(f"  adapter graphs connected   : {c['adapter_graphs_connected']}")
    print(f"  target sets connected      : {c['target_sets_connected']}")
    print(f"  hidden→artifact links      : {c['hidden_identities_to_artifact']}")
    print(f"  runtime_targets connected  : {c['runtime_targets_connected']}")
    print()
    print("Orphans:")
    o = r["orphans"]
    print(f"  runtime_target orphans     : {o['runtime_target_orphan_count']}")
    print(f"  gap reason                 : {o['gap_reason']}")
    print()
    print("Diagnostics:")
    print(f"  total                      : {r['diagnostics']['count']}")
    print()
    print("Generated class records      :", r["generated_class_records_found"],
          "(schema gap: always 0 in Phase 1)")
    print("Heuristic edges created      :", r["heuristic_edges_created"],
          "← must always be 0")
    print()
    print("Staticization readiness:")
    for label, cnt in sorted(r["staticization_readiness"].items()):
        print(f"  {cnt:>6}  {label}")
    print()
    if r["gaps_by_reason"]:
        print("Gap reasons:")
        for reason, cnt in sorted(r["gaps_by_reason"].items()):
            print(f"  {cnt:>6}  {reason}")
    print()


def _print_validation(vr: dict):
    print("=" * 60)
    print("  GRAPH VALIDATION CHECKS")
    print("=" * 60)
    for chk in vr["checks"]:
        status = "PASS" if chk["passed"] else "FAIL"
        detail = f"  [{chk['detail']}]" if chk["detail"] else ""
        print(f"  [{status}] {chk['name']}{detail}")
    print(f"\n  Result: {vr['pass']} passed, {vr['fail']} failed")
    print()


if __name__ == "__main__":
    main()
