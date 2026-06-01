"""
tests/test_graph_builder.py — Unit tests for graph_builder.py

Synthetic JSONL fixtures prove each causality chain rule deterministically.
Run with:  python3 -m pytest tools/manycore-ui/tests/ -v
       or: python3 tools/manycore-ui/tests/test_graph_builder.py
"""

import json
import sys
import os
import tempfile

# Make the parent package importable when running as a script
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from graph_builder import (
    build_graph, build_gap_report, validate_graph,
    NT_CALLSITE, NT_METHOD, NT_CLASS, NT_ADAPTER_GRAPH, NT_ADAPTER_NODE,
    NT_HIDDEN_CLASS, NT_BYTECODE_ART, NT_RUNTIME_TARGET,
    NT_TARGET_SET, NT_DIAGNOSTIC,
    ET_CALLSITE_TARGET, ET_LAMBDA_BODY, ET_HAS_ADAPTER_GRAPH,
    ET_HAS_ADAPTER_NODE, ET_ADAPTER_NODE_TARGET, ET_HAS_TARGET_SET,
    ET_TARGET_SET_MEMBER, ET_HAS_HIDDEN_IDENTITY, ET_ORPHAN_RT,
    ET_HAS_BYTECODE_ART, ET_DIAGNOSTIC_AT, ET_CALLSITE_RT_ATTRIBUTED,
    GAP_MISSING_SOURCE_BCI, GAP_NO_BYTECODE_ARTIFACT_MATCH,
    GAP_ADAPTER_UNKNOWN_SHAPE,
    SR_DIRECT, SR_OBSERVED_ONLY, SR_MULTI_TARGET, SR_NEEDS_HIDDEN,
    query_chain, list_orphan_runtime_targets,
)


# ---------------------------------------------------------------------------
# Fixture helpers
# ---------------------------------------------------------------------------

def _write_jsonl(records: list[dict]) -> str:
    """Write a list of dicts to a temp JSONL file; return the path."""
    fd, path = tempfile.mkstemp(suffix=".jsonl")
    with os.fdopen(fd, "w") as fh:
        for r in records:
            fh.write(json.dumps(r) + "\n")
    return path


def _build(records: list[dict]):
    """Convenience: write temp JSONL, build graph, return (graph, report)."""
    path = _write_jsonl(records)
    try:
        g, raw_summary = build_graph(path)
        report = build_gap_report(g, raw_summary)
        return g, report
    finally:
        os.unlink(path)


def _edges_of_type(g, edge_type: str):
    return [e for e in g.edges if e.edge_type == edge_type]


# ---------------------------------------------------------------------------
# Test 1: exact callsite_target creates CALLSITE_TARGET edge
# ---------------------------------------------------------------------------

def test_callsite_target_creates_edge():
    records = [
        {
            "record": "callsite_target",
            "category": "invokeinterface",
            "evidence": "LINKAGE_GUARANTEED",
            "source_class": "com/example/App",
            "source_method": "run",
            "source_descriptor": "()V",
            "source_bci": 10,
            "source_loader_id": "0xaabbcc",
            "target_class": "com/example/ServiceImpl",
            "target_method": "doWork",
            "target_descriptor": "()V",
            "target_loader_id": "0xaabbcc",
            "exact": True,
            "staticizable": True,
        }
    ]
    g, report = _build(records)

    # Callsite node exists
    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1, f"Expected 1 callsite node, got {len(cs_nodes)}"
    cs = cs_nodes[0]
    assert cs.attrs["src_class"] == "com/example/App"
    assert cs.attrs["src_bci"] == 10

    # Target method node exists
    m_nodes = [n for n in g.nodes.values() if n.node_type == NT_METHOD]
    assert len(m_nodes) == 1
    assert m_nodes[0].attrs["cls"] == "com/example/ServiceImpl"

    # CALLSITE_TARGET edge exists
    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 1
    assert ct_edges[0].attrs["category"] == "invokeinterface"
    assert ct_edges[0].attrs["exact"] == True

    # Connected count in report
    assert report["connected"]["callsite_target_edges"] == 1

    # Heuristic edges = 0
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 2: invokedynamic creates CALLSITE_TARGET + LAMBDA_BODY edges
# ---------------------------------------------------------------------------

def test_invokedynamic_creates_lambda_body_edge():
    records = [
        {
            "record": "callsite_target",
            "category": "invokedynamic",
            "trace_id": 1,
            "evidence": "LINKAGE_GUARANTEED",
            "source_class": "com/example/App",
            "source_method": "main",
            "source_descriptor": "([Ljava/lang/String;)V",
            "source_bci": 5,
            "source_loader_id": "0xaabb",
            "target_class": "com/example/App$$Lambda+0x00001234",
            "target_method": "run",
            "target_descriptor": "()V",
            "target_loader_id": "0xaabb",
            "lmf_impl_class": "com/example/App",
            "lmf_impl_method": "lambda$main$0",
            "lmf_impl_descriptor": "()V",
            "indy_name": "run",
            "staticizable": True,
            "reconstructable": True,
        }
    ]
    g, report = _build(records)

    # Both CALLSITE_TARGET and LAMBDA_BODY edges
    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    lb_edges = _edges_of_type(g, ET_LAMBDA_BODY)
    assert len(ct_edges) == 1, "Expected 1 CALLSITE_TARGET edge"
    assert len(lb_edges) == 1, "Expected 1 LAMBDA_BODY edge"

    # LAMBDA_BODY points to the impl method
    lb_dst = g.nodes.get(lb_edges[0].dst_id)
    assert lb_dst is not None
    assert lb_dst.attrs["cls"] == "com/example/App"
    assert lb_dst.attrs["method"] == "lambda$main$0"
    assert lb_dst.attrs.get("is_lambda_body") == True


# ---------------------------------------------------------------------------
# Test 3: adapter graph creates adapter_graph node, adapter_node nodes, edges
# ---------------------------------------------------------------------------

def test_adapter_graph_creates_nodes_and_edges():
    records = [
        {
            "record": "callsite_target",
            "category": "invokedynamic",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/App",
            "source_method": "go",
            "source_descriptor": "()V",
            "source_bci": 20,
            "source_loader_id": "0x1",
            "target_class": "com/example/App$$Lambda+0xabcd",
            "target_method": "accept",
            "target_descriptor": "(Ljava/lang/Object;)V",
            "target_loader_id": "0x1",
        },
        {
            "record": "callsite_adapter_graph",
            "category": "invokedynamic",
            "adapter_class": "java/lang/invoke/BoundMethodHandle$Species_LL",
            "adapter_kind": "type_conversion",
            "evidence": "OBSERVED_ONLY",
            "all_exact": True,
            "source_class": "com/example/App",
            "source_method": "go",
            "source_descriptor": "()V",
            "source_bci": 20,
            "source_loader_id": "0x1",
            "nodes": [
                {
                    "id": 0, "role": "primary_target", "exact": True,
                    "classification": "user_target",
                    "class": "com/example/ServiceImpl",
                    "loader_id": "0x1",
                    "method": "process",
                    "descriptor": "(Ljava/lang/Object;)V"
                },
                {
                    "id": 1, "role": "secondary_component", "exact": False,
                    "classification": "bound_data",
                    "exact_false_reason": "non_mh_bound_value"
                }
            ],
            "staticizable": True,
        },
    ]
    g, report = _build(records)

    # adapter_graph virtual node
    ag_nodes = [n for n in g.nodes.values() if n.node_type == NT_ADAPTER_GRAPH]
    assert len(ag_nodes) == 1, f"Expected 1 adapter_graph node, got {len(ag_nodes)}"
    assert ag_nodes[0].attrs["adapter_kind"] == "type_conversion"

    # HAS_ADAPTER_GRAPH edge from callsite to adapter_graph
    hag_edges = _edges_of_type(g, ET_HAS_ADAPTER_GRAPH)
    assert len(hag_edges) == 1

    # adapter_node nodes (2 nodes)
    an_nodes = [n for n in g.nodes.values() if n.node_type == NT_ADAPTER_NODE]
    assert len(an_nodes) == 2

    # HAS_ADAPTER_NODE edges
    han_edges = _edges_of_type(g, ET_HAS_ADAPTER_NODE)
    assert len(han_edges) == 2

    # Primary-target adapter node links to a method via ADAPTER_NODE_TARGET
    ant_edges = _edges_of_type(g, ET_ADAPTER_NODE_TARGET)
    assert len(ant_edges) == 1
    tgt_method_node = g.nodes.get(ant_edges[0].dst_id)
    assert tgt_method_node is not None
    assert tgt_method_node.attrs["method"] == "process"

    # Connected count
    assert report["connected"]["adapter_graphs_connected"] == 1

    # No heuristic edges
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 4: target_set creates HAS_TARGET_SET + TARGET_SET_MEMBER edges
# ---------------------------------------------------------------------------

def test_target_set_creates_member_edges():
    records = [
        {
            "record": "callsite_target_set",
            "category": "methodhandle_invoke",
            "adapter_shape": "GWT",
            "semantic_op": "guard_with_test",
            "staticizable": False,
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/Foo",
            "source_method": "equals",
            "source_descriptor": "(Ljava/lang/Object;)Z",
            "source_bci": 2,
            "source_loader_id": "0x1",
            "targets": [
                {"role": "test",         "valid": True,  "class": "java/lang/runtime/ObjectMethods", "loader_id": "0x2", "method": "eq",     "descriptor": "(Ljava/lang/Object;Ljava/lang/Object;)Z"},
                {"role": "true_target",  "valid": True,  "class": "com/example/Foo",                 "loader_id": "0x1", "method": "fastEq", "descriptor": "(Ljava/lang/Object;)Z"},
                {"role": "false_target", "valid": True,  "class": "com/example/Foo",                 "loader_id": "0x1", "method": "slowEq", "descriptor": "(Ljava/lang/Object;)Z"},
            ],
        }
    ]
    g, report = _build(records)

    # HAS_TARGET_SET edge
    hts_edges = _edges_of_type(g, ET_HAS_TARGET_SET)
    assert len(hts_edges) == 1

    # TARGET_SET_MEMBER edges (one per target)
    tsm_edges = _edges_of_type(g, ET_TARGET_SET_MEMBER)
    assert len(tsm_edges) == 3
    roles = sorted(e.attrs["role"] for e in tsm_edges)
    assert roles == ["false_target", "test", "true_target"]

    # Static label = blocked_multi_target
    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1
    assert cs_nodes[0].attrs["static_label"] == SR_MULTI_TARGET

    # Connected count
    assert report["connected"]["target_sets_connected"] == 1


# ---------------------------------------------------------------------------
# Test 5: runtime_target without source BCI → orphan node, gap reported
# ---------------------------------------------------------------------------

def test_runtime_target_becomes_orphan():
    records = [
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "methodhandle_linkage",
            "caller_context": "MemberName.resolve",
            "target_class": "com/example/ServiceImpl",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
            "target_hidden": False,
        }
    ]
    g, report = _build(records)

    # runtime_target node exists and is marked orphan
    rt_nodes = [n for n in g.nodes.values() if n.node_type == NT_RUNTIME_TARGET]
    assert len(rt_nodes) == 1
    assert rt_nodes[0].attrs["is_orphan"] == True
    assert rt_nodes[0].attrs["gap_reason"] == GAP_MISSING_SOURCE_BCI

    # ORPHAN edge exists
    orphan_edges = _edges_of_type(g, ET_ORPHAN_RT)
    assert len(orphan_edges) == 1

    # Gap reported
    assert report["orphans"]["runtime_target_orphan_count"] == 1
    assert GAP_MISSING_SOURCE_BCI in report["gaps_by_reason"]

    # No CALLSITE_TARGET edge (no source)
    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 0

    # No heuristic edges
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 6: hidden_class_identity links to bytecode_artifact by CRC + loader
# ---------------------------------------------------------------------------

def test_hidden_class_links_to_artifact():
    records = [
        {
            "record": "hidden_class_identity",
            "runtime_name": "com/example/App$$Lambda+0x00007f1234",
            "artifact_crc": "deadbeef",
            "loader_id": "0xaabbcc",
        },
        {
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabbcc",
            "crc": "deadbeef",
            "size": 512,
            "kind": "original",
            "hidden": True,
            "artifact_path": "/tmp/App$$Lambda.class",
        },
    ]
    g, report = _build(records)

    # hidden_class node exists
    hc_nodes = [n for n in g.nodes.values() if n.node_type == NT_HIDDEN_CLASS]
    assert len(hc_nodes) == 1

    # bytecode_artifact node exists
    ba_nodes = [n for n in g.nodes.values() if n.node_type == NT_BYTECODE_ART]
    assert len(ba_nodes) == 1
    assert ba_nodes[0].attrs["crc"] == "deadbeef"

    # HAS_HIDDEN_IDENTITY edge
    hhi_edges = _edges_of_type(g, ET_HAS_HIDDEN_IDENTITY)
    assert len(hhi_edges) == 1
    assert hhi_edges[0].attrs["artifact_crc"] == "deadbeef"

    # Connected count
    assert report["connected"]["hidden_identities_to_artifact"] == 1


# ---------------------------------------------------------------------------
# Test 7: non-matching CRC → no HAS_HIDDEN_IDENTITY edge, gap reported
# ---------------------------------------------------------------------------

def test_hidden_class_no_match_creates_gap():
    records = [
        {
            "record": "hidden_class_identity",
            "runtime_name": "com/example/App$$Lambda+0x000099",
            "artifact_crc": "11111111",   # ← does not match the artifact below
            "loader_id": "0xaabbcc",
        },
        {
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabbcc",
            "crc": "22222222",            # ← different CRC
            "kind": "original",
            "hidden": True,
            "artifact_path": "/tmp/App$$Lambda.class",
        },
    ]
    g, report = _build(records)

    # HAS_HIDDEN_IDENTITY edge should NOT be created
    hhi_edges = _edges_of_type(g, ET_HAS_HIDDEN_IDENTITY)
    assert len(hhi_edges) == 0

    # Gap recorded
    assert report["gaps_by_reason"].get(GAP_NO_BYTECODE_ARTIFACT_MATCH, 0) >= 1


# ---------------------------------------------------------------------------
# Test 8: diagnostic record creates diagnostic node, NOT a CALLSITE_TARGET
# ---------------------------------------------------------------------------

def test_diagnostic_does_not_create_callsite_target():
    records = [
        {
            "record": "diagnostic",
            "level": "info",
            "category": "methodhandle_invoke",
            "reason": "recv_from_method_result_or_field",
            "src_class": "com/example/App",
            "src_method": "run",
            "src_descriptor": "()V",
            "src_bci": 30,
            "src_loader_id": "0x1",
        }
    ]
    g, report = _build(records)

    # Diagnostic node exists
    diag_nodes = [n for n in g.nodes.values() if n.node_type == NT_DIAGNOSTIC]
    assert len(diag_nodes) == 1
    assert diag_nodes[0].attrs["reason"] == "recv_from_method_result_or_field"

    # NO CALLSITE_TARGET edge
    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 0

    # Reported in diagnostics
    assert report["diagnostics"]["count"] == 1


# ---------------------------------------------------------------------------
# Test 9: non-matching records do not get connected
# ---------------------------------------------------------------------------

def test_non_matching_records_not_connected():
    """An adapter graph with a source_bci that has no callsite_target record
    should still create nodes (adapter graph provides its own source), but
    should NOT create a spurious CALLSITE_TARGET edge."""
    records = [
        {
            "record": "callsite_adapter_graph",
            "category": "invokedynamic",
            "adapter_class": "java/lang/invoke/BoundMethodHandle$Species_LL",
            "adapter_kind": "lambda",
            "evidence": "OBSERVED_ONLY",
            "all_exact": False,
            "source_class": "com/example/X",
            "source_method": "foo",
            "source_descriptor": "()V",
            "source_bci": 99,
            "source_loader_id": "0x1",
            "nodes": [],
        }
    ]
    g, report = _build(records)

    # NO CALLSITE_TARGET edge (no callsite_target record was provided)
    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 0

    # But the callsite node was created (adapter graph provides source attribution)
    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1

    # And the adapter_graph node was created
    ag_nodes = [n for n in g.nodes.values() if n.node_type == NT_ADAPTER_GRAPH]
    assert len(ag_nodes) == 1


# ---------------------------------------------------------------------------
# Test 10: adapter_unknown_shape adds gap record
# ---------------------------------------------------------------------------

def test_unknown_adapter_shape_adds_gap():
    records = [
        {
            "record": "callsite_adapter_graph",
            "category": "methodhandle_invoke",
            "adapter_class": "java/lang/invoke/DirectMethodHandle$StaticAccessor",
            "adapter_kind": "unknown",
            "evidence": "OBSERVED_ONLY",
            "all_exact": False,
            "source_class": "com/example/App",
            "source_method": "bar",
            "source_descriptor": "()V",
            "source_bci": 43,
            "source_loader_id": "0x1",
            "nodes": [
                {
                    "id": 0, "role": "primary_target", "exact": False,
                    "classification": "adapter_unknown_shape",
                    "adapter_class": "java/lang/invoke/DirectMethodHandle$StaticAccessor",
                    "exact_false_reason": "unknown_shape",
                }
            ],
        }
    ]
    g, report = _build(records)

    # Gap for unknown shape
    assert report["gaps_by_reason"].get(GAP_ADAPTER_UNKNOWN_SHAPE, 0) >= 1


# ---------------------------------------------------------------------------
# Test 11: bytecode_artifact creates class→artifact HAS_BYTECODE_ARTIFACT edge
# ---------------------------------------------------------------------------

def test_bytecode_artifact_creates_class_edge():
    records = [
        {
            "record": "bytecode_artifact",
            "class": "com/example/Svc",
            "loader_id": "0x1",
            "crc": "abcd1234",
            "size": 1024,
            "kind": "original",
            "hidden": False,
            "artifact_path": "/tmp/Svc.class",
        }
    ]
    g, report = _build(records)

    hba_edges = _edges_of_type(g, ET_HAS_BYTECODE_ART)
    assert len(hba_edges) == 1

    src_cls = g.nodes.get(hba_edges[0].src_id)
    assert src_cls is not None
    assert src_cls.node_type == NT_CLASS
    assert "com/example/Svc" in src_cls.node_id

    dst_art = g.nodes.get(hba_edges[0].dst_id)
    assert dst_art is not None
    assert dst_art.node_type == NT_BYTECODE_ART
    assert dst_art.attrs["crc"] == "abcd1234"


# ---------------------------------------------------------------------------
# Test 12: validate_graph passes on clean fixture
# ---------------------------------------------------------------------------

def test_validate_graph_passes_on_clean_fixture():
    records = [
        {
            "record": "callsite_target",
            "category": "invokeinterface",
            "evidence": "LINKAGE_GUARANTEED",
            "source_class": "com/example/App",
            "source_method": "run",
            "source_descriptor": "()V",
            "source_bci": 10,
            "source_loader_id": "0xaa",
            "target_class": "com/example/ServiceImpl",
            "target_method": "doWork",
            "target_descriptor": "()V",
            "target_loader_id": "0xaa",
            "exact": True,
            "staticizable": True,
        },
        {
            "record": "export_summary",
            "callsite_target_count": 1,
            "runtime_target_count": 0,
            "generated_class_count": 0,
            "bytecode_artifact_count": 0,
            "hidden_class_identity_count": 0,
            "diagnostic_count": 0,
            "complete": True,
        },
    ]
    g, report = _build(records)
    vr = validate_graph(g, report)

    failures = [c for c in vr["checks"] if not c["passed"]]
    assert vr["fail"] == 0, f"Validation failures: {failures}"


# ---------------------------------------------------------------------------
# Test 13: query_chain returns chain for known callsite
# ---------------------------------------------------------------------------

def test_query_chain():
    records = [
        {
            "record": "callsite_target",
            "category": "invokeinterface",
            "evidence": "LINKAGE_GUARANTEED",
            "source_class": "com/example/App",
            "source_method": "run",
            "source_descriptor": "()V",
            "source_bci": 10,
            "source_loader_id": "0xaa",
            "target_class": "com/example/ServiceImpl",
            "target_method": "doWork",
            "target_descriptor": "()V",
            "target_loader_id": "0xaa",
            "exact": True,
        }
    ]
    g, _ = _build(records)
    chain = query_chain(g, "com/example/App", "run", 10)
    assert len(chain) >= 2
    assert chain[0]["type"] == "callsite"
    edges_in_chain = [e["edge"] for e in chain if "edge" in e]
    assert ET_CALLSITE_TARGET in edges_in_chain


# ---------------------------------------------------------------------------
# Test 14: orphan list query
# ---------------------------------------------------------------------------

def test_list_orphan_runtime_targets():
    records = [
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "constructor_resolve",
            "caller_context": "MemberName.resolve",
            "target_class": "com/example/Bean",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
        },
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "methodhandle_linkage",
            "caller_context": "MemberName.resolve",
            "target_class": "com/example/OtherBean",
            "target_method": "create",
            "target_descriptor": "()Ljava/lang/Object;",
            "target_loader_id": "0x1",
        },
    ]
    g, _ = _build(records)
    orphans = list_orphan_runtime_targets(g)
    assert len(orphans) == 2
    classes = {o["target_class"] for o in orphans}
    assert "com/example/Bean" in classes
    assert "com/example/OtherBean" in classes


# ---------------------------------------------------------------------------
# Test 15 (Phase 2b): attributed runtime_target creates deterministic edge
# ---------------------------------------------------------------------------

def test_attributed_runtime_target_creates_edge():
    """source_capture=exact runtime_target must create a CALLSITE_RT_ATTRIBUTED edge
    and a callsite node. No NT_RUNTIME_TARGET orphan node is created."""
    records = [
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "membername_linkage",
            "caller_context": "MemberName.resolve",
            "source_capture": "exact",
            "source_class": "com/example/App",
            "source_method": "main",
            "source_descriptor": "([Ljava/lang/String;)V",
            "source_bci": 42,
            "source_loader_id": "0x0000000100000000",
            "target_class": "com/example/Bean",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_loader_id": "0x0000000100000000",
        }
    ]
    g, report = _build(records)

    # Must have a callsite node with correct source attribution
    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1, f"Expected 1 callsite node, got {len(cs_nodes)}"
    cs = cs_nodes[0]
    assert cs.attrs["src_class"] == "com/example/App"
    assert cs.attrs["src_method"] == "main"
    assert cs.attrs["src_bci"] == 42

    # Must have a CALLSITE_RT_ATTRIBUTED edge from callsite to target method
    attr_edges = _edges_of_type(g, ET_CALLSITE_RT_ATTRIBUTED)
    assert len(attr_edges) == 1, f"Expected 1 CALLSITE_RT_ATTRIBUTED edge, got {len(attr_edges)}"
    edge = attr_edges[0]
    assert edge.src_id == cs.node_id
    tgt_node = g.nodes.get(edge.dst_id)
    assert tgt_node is not None
    assert tgt_node.node_type == NT_METHOD
    assert tgt_node.attrs["method"] == "<init>"

    # Must NOT have any NT_RUNTIME_TARGET orphan nodes
    rt_nodes = [n for n in g.nodes.values() if n.node_type == NT_RUNTIME_TARGET]
    assert len(rt_nodes) == 0, f"No orphan NT_RUNTIME_TARGET expected, got {len(rt_nodes)}"

    # connected_runtime_targets must be 1
    assert report["connected"]["runtime_targets_connected"] == 1

    # orphan count must be 0
    assert report["orphans"]["runtime_target_orphan_count"] == 0

    # no heuristic edges
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 16 (Phase 2b): unattributed runtime_target stays orphan
# ---------------------------------------------------------------------------

def test_unattributed_runtime_target_stays_orphan():
    """source_capture=missing runtime_target must remain an orphan NT_RUNTIME_TARGET
    node with ET_ORPHAN_RT edge. No callsite node is created."""
    records = [
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "methodhandle_linkage",
            "caller_context": "MemberName.resolve",
            "source_capture": "missing",
            "source_missing_reason": "no_user_frame_on_stack",
            "target_class": "java/lang/Object",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_loader_id": "0x0",
        }
    ]
    g, report = _build(records)

    # Must have an NT_RUNTIME_TARGET orphan node
    rt_nodes = [n for n in g.nodes.values() if n.node_type == NT_RUNTIME_TARGET]
    assert len(rt_nodes) == 1
    assert rt_nodes[0].attrs["is_orphan"] is True
    assert rt_nodes[0].attrs["source_missing_reason"] == "no_user_frame_on_stack"

    # Must have ORPHAN_RT edge
    orphan_edges = _edges_of_type(g, ET_ORPHAN_RT)
    assert len(orphan_edges) == 1

    # Must NOT have any callsite nodes
    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 0

    # connected_runtime_targets must be 0
    assert report["connected"]["runtime_targets_connected"] == 0

    # orphan count must be 1
    assert report["orphans"]["runtime_target_orphan_count"] == 1

    # no heuristic edges
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 17 (Phase 2b): old-format runtime_target (no source_capture) stays orphan
# ---------------------------------------------------------------------------

def test_old_format_runtime_target_stays_orphan():
    """Older JSONL files have runtime_target records with no source_capture field.
    These must be handled as orphans (backward compatibility)."""
    records = [
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "reflection",
            "caller_context": "java.lang.reflect.Constructor",
            # No source_capture, source_class, source_method fields — old format
            "target_class": "com/example/OldBean",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
        }
    ]
    g, report = _build(records)

    # Must remain an orphan
    rt_nodes = [n for n in g.nodes.values() if n.node_type == NT_RUNTIME_TARGET]
    assert len(rt_nodes) == 1
    assert rt_nodes[0].attrs["is_orphan"] is True

    # No callsite node
    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 0

    assert report["connected"]["runtime_targets_connected"] == 0
    assert report["orphans"]["runtime_target_orphan_count"] == 1
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 18 (Phase 2b): mixed attributed and orphan runtime_target records
# ---------------------------------------------------------------------------

def test_mixed_attributed_and_orphan_runtime_targets():
    """Some runtime_target records may be attributed, others orphaned.
    Each must be handled independently; no cross-record heuristic matching."""
    records = [
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "membername_linkage",
            "caller_context": "MemberName.resolve",
            "source_capture": "exact",
            "source_class": "com/example/App",
            "source_method": "setup",
            "source_descriptor": "()V",
            "source_bci": 10,
            "source_loader_id": "0x1",
            "target_class": "com/example/Svc",
            "target_method": "init",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
        },
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "methodhandle_linkage",
            "caller_context": "MemberName.resolve",
            "source_capture": "missing",
            "source_missing_reason": "no_user_frame_on_stack",
            "target_class": "java/lang/String",
            "target_method": "<init>",
            "target_descriptor": "(Ljava/lang/String;)V",
            "target_loader_id": "0x0",
        },
    ]
    g, report = _build(records)

    # One attributed → one CALLSITE_RT_ATTRIBUTED edge + one callsite node
    attr_edges = _edges_of_type(g, ET_CALLSITE_RT_ATTRIBUTED)
    assert len(attr_edges) == 1

    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1
    assert cs_nodes[0].attrs["src_class"] == "com/example/App"

    # One orphan → one NT_RUNTIME_TARGET node
    rt_nodes = [n for n in g.nodes.values() if n.node_type == NT_RUNTIME_TARGET]
    assert len(rt_nodes) == 1
    assert rt_nodes[0].attrs["target_class"] == "java/lang/String"

    assert report["connected"]["runtime_targets_connected"] == 1
    assert report["orphans"]["runtime_target_orphan_count"] == 1
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 19 (Phase 2b): attributed runtime_target with partial source stays orphan
# ---------------------------------------------------------------------------

def test_incomplete_source_fields_stay_orphan():
    """A runtime_target with source_capture=exact but missing source_class must
    not be connected — incomplete attribution is treated as unattributed."""
    records = [
        {
            "record": "runtime_target",
            "evidence": "LINKAGE_GUARANTEED",
            "dispatch_kind": "membername_linkage",
            "caller_context": "MemberName.resolve",
            "source_capture": "exact",
            # source_class intentionally omitted — incomplete attribution
            "source_method": "run",
            "source_descriptor": "()V",
            "source_bci": 5,
            "source_loader_id": "0x1",
            "target_class": "com/example/T",
            "target_method": "go",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
        }
    ]
    g, report = _build(records)

    # Without source_class, must fall back to orphan
    attr_edges = _edges_of_type(g, ET_CALLSITE_RT_ATTRIBUTED)
    assert len(attr_edges) == 0

    rt_nodes = [n for n in g.nodes.values() if n.node_type == NT_RUNTIME_TARGET]
    assert len(rt_nodes) == 1

    assert report["connected"]["runtime_targets_connected"] == 0
    assert report["orphans"]["runtime_target_orphan_count"] == 1
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 20 (poly callsite): two callsite_target records, same BCI, different
# targets → SR_MULTI_TARGET; both edges stored.
# ---------------------------------------------------------------------------

def test_poly_two_targets_same_bci_becomes_multi_target():
    """Two callsite_target records for the same source BCI but different
    target classes must produce SR_MULTI_TARGET and both CALLSITE_TARGET edges.
    This is the correct representation for a polymorphic invokevirtual site."""
    records = [
        {
            "record": "callsite_target",
            "category": "invokevirtual",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/App",
            "source_method": "dispatch",
            "source_descriptor": "()V",
            "source_bci": 42,
            "source_loader_id": "0x1",
            "target_class": "com/example/ImplA",
            "target_method": "run",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
            "exact": True,
        },
        {
            "record": "callsite_target",
            "category": "invokevirtual",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/App",
            "source_method": "dispatch",
            "source_descriptor": "()V",
            "source_bci": 42,
            "source_loader_id": "0x1",
            "target_class": "com/example/ImplB",
            "target_method": "run",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
            "exact": True,
        },
    ]
    g, report = _build(records)

    # Exactly one callsite node (same source BCI, same loader)
    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1, f"Expected 1 callsite node, got {len(cs_nodes)}"
    cs = cs_nodes[0]
    assert cs.attrs["src_bci"] == 42

    # Two distinct method nodes
    m_nodes = [n for n in g.nodes.values() if n.node_type == NT_METHOD]
    assert len(m_nodes) == 2
    m_classes = {n.attrs["cls"] for n in m_nodes}
    assert "com/example/ImplA" in m_classes
    assert "com/example/ImplB" in m_classes

    # Two CALLSITE_TARGET edges
    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 2, f"Expected 2 CALLSITE_TARGET edges, got {len(ct_edges)}"
    assert all(e.src_id == cs.node_id for e in ct_edges)

    # Static label must be SR_MULTI_TARGET (blocked_multi_target)
    assert cs.attrs.get("static_label") == SR_MULTI_TARGET, \
        f"Expected SR_MULTI_TARGET, got {cs.attrs.get('static_label')!r}"

    # Two target edges → two connected callsite targets
    assert report["connected"]["callsite_target_edges"] == 2

    # No heuristic edges
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 21 (poly callsite): three targets for same BCI → SR_MULTI_TARGET
# ---------------------------------------------------------------------------

def test_poly_three_targets_same_bci():
    """Three distinct targets at the same source BCI → three CALLSITE_TARGET
    edges, one callsite node, SR_MULTI_TARGET."""
    records = [
        {
            "record": "callsite_target",
            "category": "invokevirtual",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/Router",
            "source_method": "handle",
            "source_descriptor": "(I)V",
            "source_bci": 10,
            "source_loader_id": "0x1",
            "target_class": f"com/example/Handler{i}",
            "target_method": "process",
            "target_descriptor": "(I)V",
            "target_loader_id": "0x1",
            "exact": True,
        }
        for i in range(3)
    ]
    g, report = _build(records)

    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1
    cs = cs_nodes[0]

    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 3, f"Expected 3 CALLSITE_TARGET edges, got {len(ct_edges)}"

    assert cs.attrs.get("static_label") == SR_MULTI_TARGET
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 22 (poly callsite): same BCI, same target twice — dedup at graph level
# keeps one edge; NOT SR_MULTI_TARGET.
# ---------------------------------------------------------------------------

def test_poly_same_target_twice_not_multi_target():
    """Two callsite_target records with the same source BCI AND the same target
    class/method produce only one distinct CALLSITE_TARGET destination.  The
    callsite must NOT be labelled SR_MULTI_TARGET."""
    records = [
        {
            "record": "callsite_target",
            "category": "invokevirtual",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/App",
            "source_method": "go",
            "source_descriptor": "()V",
            "source_bci": 7,
            "source_loader_id": "0x1",
            "target_class": "com/example/Only",
            "target_method": "act",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
            "exact": True,
        },
        # Identical record — the C++ poly dedup prevents this in production,
        # but the graph builder must handle it defensively: one distinct target.
        {
            "record": "callsite_target",
            "category": "invokevirtual",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/App",
            "source_method": "go",
            "source_descriptor": "()V",
            "source_bci": 7,
            "source_loader_id": "0x1",
            "target_class": "com/example/Only",
            "target_method": "act",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
            "exact": True,
        },
    ]
    g, report = _build(records)

    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1

    # One distinct target destination — NOT SR_MULTI_TARGET
    cs = cs_nodes[0]
    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    distinct_targets = {e.dst_id for e in ct_edges}
    assert len(distinct_targets) == 1, \
        f"Expected 1 distinct target, got {len(distinct_targets)}"
    assert cs.attrs.get("static_label") != SR_MULTI_TARGET, \
        "Single-target callsite must not be SR_MULTI_TARGET"

    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Test 23 (Gap #17 fix): same class name, same method, different loader IDs →
# two distinct CALLSITE_TARGET edges, SR_MULTI_TARGET
# ---------------------------------------------------------------------------

def test_poly_same_name_different_loader_is_multi_target():
    """Two callsite_target records at the same BCI where target_class and
    target_method are identical but target_loader_id differs.
    This is the Gap #17 scenario: two classloaders both defining 'breadthval/PolyGreeter'.
    The dedup key must include target_loader_id so both records are preserved.
    Expected: one callsite node, two distinct method nodes, two CALLSITE_TARGET
    edges, static_label=SR_MULTI_TARGET."""
    records = [
        {
            "record": "callsite_target",
            "category": "invokeinterface",
            "evidence": "OBSERVED_ONLY",
            "source_class": "breadthval/Area8_LoaderAwareDedup",
            "source_method": "run",
            "source_descriptor": "()V",
            "source_bci": 55,
            "source_loader_id": "0x0000000100f05b60",
            "source_capture": "exact",
            "source_opcode": "invokeinterface",
            "target_class": "breadthval/PolyGreeter",
            "target_loader_id": "0x0000000100d14800",   # loaderA
            "target_method": "greet",
            "target_descriptor": "()Ljava/lang/String;",
        },
        {
            "record": "callsite_target",
            "category": "invokeinterface",
            "evidence": "OBSERVED_ONLY",
            "source_class": "breadthval/Area8_LoaderAwareDedup",
            "source_method": "run",
            "source_descriptor": "()V",
            "source_bci": 55,
            "source_loader_id": "0x0000000100f05b60",
            "source_capture": "exact",
            "source_opcode": "invokeinterface",
            "target_class": "breadthval/PolyGreeter",
            "target_loader_id": "0x0000000100d168b0",   # loaderB — same class name!
            "target_method": "greet",
            "target_descriptor": "()Ljava/lang/String;",
        },
    ]
    g, report = _build(records)

    # One callsite node (same source)
    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1, f"Expected 1 callsite node, got {len(cs_nodes)}"
    cs = cs_nodes[0]
    assert cs.attrs["src_bci"] == 55

    # Two distinct method nodes — one per loader
    m_nodes = [n for n in g.nodes.values() if n.node_type == NT_METHOD]
    assert len(m_nodes) == 2, f"Expected 2 distinct method nodes (one per loader), got {len(m_nodes)}"
    loader_ids = {n.attrs.get("loader_id", "") for n in m_nodes}
    assert "0x0000000100d14800" in loader_ids, "loaderA method node missing"
    assert "0x0000000100d168b0" in loader_ids, "loaderB method node missing"

    # Two CALLSITE_TARGET edges
    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 2, f"Expected 2 CALLSITE_TARGET edges, got {len(ct_edges)}"
    assert all(e.src_id == cs.node_id for e in ct_edges)

    # Both targets are the same class/method/desc — only loader differs
    for n in m_nodes:
        assert n.attrs["cls"] == "breadthval/PolyGreeter"
        assert n.attrs["method"] == "greet"

    # SR_MULTI_TARGET because two distinct target method nodes
    assert cs.attrs.get("static_label") == SR_MULTI_TARGET, \
        f"Expected SR_MULTI_TARGET, got {cs.attrs.get('static_label')!r}"

    # No heuristic edges
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Tests 24–26 (Phase 2E): reflection_method_invoke warm-path callsite records
# ---------------------------------------------------------------------------

def test_reflection_method_invoke_creates_edge():
    """A callsite_target with category=reflection_method_invoke must produce
    one callsite node and one CALLSITE_TARGET edge to the reflected method.
    No heuristic edges."""
    records = [
        {
            "record": "callsite_target",
            "category": "reflection_method_invoke",
            "evidence": "OBSERVED_ONLY",
            "source_class": "org/springframework/web/method/support/InvocableHandlerMethod",
            "source_method": "doInvoke",
            "source_descriptor": "([Ljava/lang/Object;)Ljava/lang/Object;",
            "source_bci": 55,
            "source_loader_id": "0x1",
            "target_class": "com/example/springboot/HelloController",
            "target_method": "index",
            "target_descriptor": "()Ljava/lang/String;",
            "target_loader_id": "0x1",
        }
    ]
    g, report = _build(records)

    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1
    cs = cs_nodes[0]
    assert cs.attrs["src_method"] == "doInvoke"
    assert cs.attrs["src_bci"] == 55

    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 1
    tgt = g.nodes.get(ct_edges[0].dst_id)
    assert tgt is not None
    assert tgt.node_type == NT_METHOD
    assert tgt.attrs["cls"] == "com/example/springboot/HelloController"
    assert tgt.attrs["method"] == "index"

    assert report["heuristic_edges_created"] == 0
    assert report["connected"]["callsite_target_edges"] == 1


def test_reflection_method_invoke_multi_target_same_bci():
    """Two reflection_method_invoke records for the same source BCI but different
    reflected targets must produce SR_MULTI_TARGET and two CALLSITE_TARGET edges."""
    records = [
        {
            "record": "callsite_target",
            "category": "reflection_method_invoke",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/Dispatcher",
            "source_method": "dispatch",
            "source_descriptor": "(Ljava/lang/Object;)V",
            "source_bci": 10,
            "source_loader_id": "0x1",
            "target_class": "com/example/HandlerA",
            "target_method": "handle",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
        },
        {
            "record": "callsite_target",
            "category": "reflection_method_invoke",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/Dispatcher",
            "source_method": "dispatch",
            "source_descriptor": "(Ljava/lang/Object;)V",
            "source_bci": 10,
            "source_loader_id": "0x1",
            "target_class": "com/example/HandlerB",
            "target_method": "handle",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
        },
    ]
    g, report = _build(records)

    cs_nodes = [n for n in g.nodes.values() if n.node_type == NT_CALLSITE]
    assert len(cs_nodes) == 1
    cs = cs_nodes[0]

    ct_edges = _edges_of_type(g, ET_CALLSITE_TARGET)
    assert len(ct_edges) == 2, f"Expected 2 CALLSITE_TARGET edges, got {len(ct_edges)}"
    assert all(e.src_id == cs.node_id for e in ct_edges)

    tgt_classes = {g.nodes[e.dst_id].attrs["cls"] for e in ct_edges}
    assert "com/example/HandlerA" in tgt_classes
    assert "com/example/HandlerB" in tgt_classes

    assert cs.attrs.get("static_label") == SR_MULTI_TARGET
    assert report["heuristic_edges_created"] == 0


def test_reflection_method_invoke_no_heuristic_edges():
    """A reflection_method_invoke record with no target info (target_class=None)
    must not produce heuristic edges or fabricated method nodes."""
    records = [
        {
            "record": "callsite_target",
            "category": "reflection_method_invoke",
            "evidence": "OBSERVED_ONLY",
            "source_class": "com/example/Runner",
            "source_method": "run",
            "source_descriptor": "()V",
            "source_bci": 20,
            "source_loader_id": "0x1",
            "target_class": "com/example/TargetService",
            "target_method": "execute",
            "target_descriptor": "()V",
            "target_loader_id": "0x1",
        }
    ]
    g, report = _build(records)

    # Must have exactly one method node (the reflected target)
    m_nodes = [n for n in g.nodes.values() if n.node_type == NT_METHOD]
    assert len(m_nodes) == 1
    assert m_nodes[0].attrs["cls"] == "com/example/TargetService"

    # No heuristic edges under any circumstances
    assert report["heuristic_edges_created"] == 0


# ---------------------------------------------------------------------------
# Runner (allows `python3 test_graph_builder.py` without pytest)
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import traceback

    tests = [
        test_callsite_target_creates_edge,
        test_invokedynamic_creates_lambda_body_edge,
        test_adapter_graph_creates_nodes_and_edges,
        test_target_set_creates_member_edges,
        test_runtime_target_becomes_orphan,
        test_hidden_class_links_to_artifact,
        test_hidden_class_no_match_creates_gap,
        test_diagnostic_does_not_create_callsite_target,
        test_non_matching_records_not_connected,
        test_unknown_adapter_shape_adds_gap,
        test_bytecode_artifact_creates_class_edge,
        test_validate_graph_passes_on_clean_fixture,
        test_query_chain,
        test_list_orphan_runtime_targets,
        # Phase 2b tests
        test_attributed_runtime_target_creates_edge,
        test_unattributed_runtime_target_stays_orphan,
        test_old_format_runtime_target_stays_orphan,
        test_mixed_attributed_and_orphan_runtime_targets,
        test_incomplete_source_fields_stay_orphan,
        # Poly callsite model tests
        test_poly_two_targets_same_bci_becomes_multi_target,
        test_poly_three_targets_same_bci,
        test_poly_same_target_twice_not_multi_target,
        test_poly_same_name_different_loader_is_multi_target,
        # Phase 2E: warm-path reflection attribution
        test_reflection_method_invoke_creates_edge,
        test_reflection_method_invoke_multi_target_same_bci,
        test_reflection_method_invoke_no_heuristic_edges,
    ]

    passed = 0
    failed = 0
    for t in tests:
        try:
            t()
            print(f"  PASS  {t.__name__}")
            passed += 1
        except Exception as e:
            print(f"  FAIL  {t.__name__}: {e}")
            traceback.print_exc()
            failed += 1

    print(f"\n{passed} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)
