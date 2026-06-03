"""Tests for indexer._build_index: class identity, hidden class handling, scope flags."""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from indexer import _build_index


def _idx(records, user_prefixes=None):
    return _build_index(records, user_prefixes=user_prefixes or [])


def _cls(idx, name):
    """Return the class entry dict for a given name, or None."""
    return idx["classes_by_name"].get(name)


# ---------------------------------------------------------------------------
# Hidden class identity: no phantom base-name entry
# ---------------------------------------------------------------------------

def test_hidden_artifact_does_not_create_base_name_entry():
    """bytecode_artifact with hidden=True must NOT create a class entry for the
    base name.  Each runtime instance is keyed by its +0x name from
    hidden_class_identity."""
    records = [
        {
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabb",
            "crc": "deadbeef",
            "kind": "original",
            "hidden": True,
            "artifact_path": "/tmp/App$$Lambda.class",
        },
        {
            "record": "hidden_class_identity",
            "runtime_name": "com/example/App$$Lambda+0x00001234",
            "artifact_crc": "deadbeef",
            "loader_id": "0xaabb",
        },
    ]
    idx = _idx(records)

    # Base name must NOT appear as a class entry
    assert _cls(idx, "com/example/App$$Lambda") is None

    # +0x instance must appear
    entry = _cls(idx, "com/example/App$$Lambda+0x00001234")
    assert entry is not None


def test_hidden_artifact_marks_has_artifacts_on_runtime_instance():
    """+0x class entry gets has_artifacts=True when CRC matches the artifact."""
    records = [
        {
            "record": "hidden_class_identity",
            "runtime_name": "com/example/App$$Lambda+0x00001234",
            "artifact_crc": "deadbeef",
            "loader_id": "0xaabb",
        },
        {
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabb",
            "crc": "deadbeef",
            "kind": "original",
            "hidden": True,
        },
    ]
    idx = _idx(records)

    entry = _cls(idx, "com/example/App$$Lambda+0x00001234")
    assert entry is not None
    assert entry["has_artifacts"] is True
    assert "bytecode_artifact" in entry["record_types"]


def test_five_distinct_hidden_instances_remain_distinct():
    """Five bytecode_artifact records sharing a base name must produce FIVE
    independent +0x class entries plus ONE lambda family grouping entry."""
    crcs = ["aaa11111", "bbb22222", "ccc33333", "ddd44444", "eee55555"]
    addrs = ["0x0001", "0x0002", "0x0003", "0x0004", "0x0005"]
    records = []
    for crc, addr in zip(crcs, addrs):
        records.append({
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabb",
            "crc": crc,
            "kind": "original",
            "hidden": True,
        })
        records.append({
            "record": "hidden_class_identity",
            "runtime_name": f"com/example/App$$Lambda+{addr}",
            "artifact_crc": crc,
            "loader_id": "0xaabb",
        })

    idx = _idx(records)

    # Base name exists as a lambda family grouping entry (not a phantom real class)
    fam = _cls(idx, "com/example/App$$Lambda")
    assert fam is not None, "lambda family entry should exist for 5+ instances"
    assert fam.get("is_lambda_family") is True
    assert fam.get("lambda_count") == 5
    assert fam.get("has_artifacts") is False  # family itself has no artifact
    assert sorted(fam["lambda_instances"]) == sorted(
        f"com/example/App$$Lambda+{a}" for a in addrs
    )

    # All 5 instances are distinct and have artifacts
    for addr in addrs:
        e = _cls(idx, f"com/example/App$$Lambda+{addr}")
        assert e is not None, f"missing entry for +{addr}"
        assert e["has_artifacts"] is True, f"+{addr} should have has_artifacts"

    # Total lambda entries: 5 instances + 1 family = 6
    names = [c["name"] for c in idx["classes"]]
    lambda_entries = [n for n in names if "Lambda" in n]
    assert len(lambda_entries) == 6  # 5 +0x instances + 1 family


def test_non_hidden_artifact_creates_class_entry():
    """For non-hidden bytecode_artifact, the class entry is created normally."""
    records = [
        {
            "record": "bytecode_artifact",
            "class": "com/example/MyClass",
            "loader_id": "0xaabb",
            "crc": "12345678",
            "kind": "original",
            "hidden": False,
        },
    ]
    idx = _idx(records)
    e = _cls(idx, "com/example/MyClass")
    assert e is not None
    assert e["has_artifacts"] is True
    assert "bytecode_artifact" in e["record_types"]


def test_hidden_artifact_no_identity_record_leaves_no_entry():
    """If hidden_class_identity is absent for a hidden artifact, no phantom
    class entry should appear."""
    records = [
        {
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabb",
            "crc": "deadbeef",
            "kind": "original",
            "hidden": True,
        },
    ]
    idx = _idx(records)
    assert _cls(idx, "com/example/App$$Lambda") is None
    assert len(idx["classes"]) == 0


# ---------------------------------------------------------------------------
# Scope gate: related class inclusion
# ---------------------------------------------------------------------------

def test_scope_includes_lambda_instances_via_prefix_match():
    """+0x lambda instances whose base name starts with the user prefix are
    included in scope even if has_user_callsites_tgt is False."""
    records = [
        {
            "record": "hidden_class_identity",
            "runtime_name": "com/example/App$$Lambda+0x00001234",
            "artifact_crc": "deadbeef",
            "loader_id": "0xaabb",
        },
        {
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabb",
            "crc": "deadbeef",
            "kind": "original",
            "hidden": True,
        },
    ]
    idx = _idx(records, user_prefixes=["com/example/App"])
    e = _cls(idx, "com/example/App$$Lambda+0x00001234")
    assert e is not None
    # Prefix "com/example/App" is a prefix of "com/example/App$$Lambda+0x..."
    # (classInScope in the UI checks c.name.startsWith(prefix))
    assert e["name"].startswith("com/example/App")


def test_scope_excludes_unrelated_class():
    """Classes from a different source class are NOT marked as user-callsite
    targets/sources and do not start with the selected prefix."""
    records = [
        {
            "record": "callsite_target",
            "source_class": "com/example/Other",
            "source_method": "run",
            "target_class": "com/example/Other2",
            "category": "invokevirtual",
            "evidence": "OBSERVED_ONLY",
        },
    ]
    idx = _idx(records, user_prefixes=["com/example/App"])

    other2 = _cls(idx, "com/example/Other2")
    assert other2 is not None
    # has_user_callsites_tgt must be False because the src class doesn't match prefix
    assert other2["has_user_callsites_tgt"] is False
    assert other2["has_user_callsites_src"] is False


def test_scope_marks_lambda_target_via_callsite():
    """A +0x hidden class that appears as target_class in a user-prefix callsite
    gets has_user_callsites_tgt=True."""
    records = [
        {
            "record": "callsite_target",
            "source_class": "com/example/App",
            "source_method": "run",
            "target_class": "com/example/App$$Lambda+0x00001234",
            "category": "invokeinterface",
            "evidence": "OBSERVED_ONLY",
        },
    ]
    idx = _idx(records, user_prefixes=["com/example/App"])
    e = _cls(idx, "com/example/App$$Lambda+0x00001234")
    assert e is not None
    assert e["has_user_callsites_tgt"] is True


def test_invokedynamic_callsite_no_target_class_does_not_create_phantom():
    """invokedynamic callsite_target records have no target_class — the source
    class is created but no phantom target entry should appear."""
    records = [
        {
            "record": "callsite_target",
            "source_class": "com/example/App",
            "source_method": "run",
            "category": "invokedynamic",
            "evidence": "LINKAGE_GUARANTEED",
            # no target_class field
        },
    ]
    idx = _idx(records, user_prefixes=["com/example/App"])
    names = {c["name"] for c in idx["classes"]}
    # Only App itself
    assert names == {"com/example/App"}
    src = _cls(idx, "com/example/App")
    assert src["has_user_callsites_src"] is True


# ---------------------------------------------------------------------------
# Derived fields: lmf_impl_* from sidecar trace_id cross-reference
# ---------------------------------------------------------------------------

def _idx_with_sidecars(records, sidecar_map, user_prefixes=None):
    return _build_index(records, user_prefixes=user_prefixes or [], sidecar_map=sidecar_map)


def test_lmf_impl_derived_on_hidden_instance():
    """When a sidecar_map maps a CRC to a trace_id, and a callsite_target record
    carries lmf_impl_* for that trace_id, the +0x class entry should expose those
    fields directly so a staticizer does not need to cross-reference the JSONL."""
    records = [
        {
            "record": "callsite_target",
            "category": "invokedynamic",
            "trace_id": 7,
            "source_class": "com/example/App",
            "source_method": "run",
            "source_bci": 10,
            "evidence": "LINKAGE_GUARANTEED",
            "indy_name": "run",
            "indy_descriptor": "()Ljava/lang/Runnable;",
            "lmf_impl_class": "com/example/App",
            "lmf_impl_method": "lambda$run$0",
            "lmf_impl_descriptor": "()V",
            "staticizable": True,
            "reconstructable": False,
        },
        {
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabb",
            "crc": "cafe1234",
            "kind": "original",
            "hidden": True,
        },
        {
            "record": "hidden_class_identity",
            "runtime_name": "com/example/App$$Lambda+0x00001111",
            "artifact_crc": "cafe1234",
            "loader_id": "0xaabb",
        },
    ]
    sidecar_map = {"cafe1234": {"trace_id": 7, "generated_by": "LambdaMetafactory", "class_name": "com/example/App$$Lambda"}}
    idx = _idx_with_sidecars(records, sidecar_map)

    entry = idx["classes_by_name"].get("com/example/App$$Lambda+0x00001111")
    assert entry is not None
    assert entry.get("artifact_trace_id") == 7
    assert entry.get("lmf_impl_method") == "lambda$run$0"
    assert entry.get("lmf_impl_class") == "com/example/App"
    assert entry.get("lmf_impl_descriptor") == "()V"


def test_lmf_impl_absent_when_no_sidecar():
    """Without sidecar_map, lmf_impl_* fields must not appear on the +0x entry."""
    records = [
        {
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabb",
            "crc": "cafe1234",
            "kind": "original",
            "hidden": True,
        },
        {
            "record": "hidden_class_identity",
            "runtime_name": "com/example/App$$Lambda+0x00001111",
            "artifact_crc": "cafe1234",
            "loader_id": "0xaabb",
        },
    ]
    idx = _idx(records)
    entry = idx["classes_by_name"].get("com/example/App$$Lambda+0x00001111")
    assert entry is not None
    assert "lmf_impl_method" not in entry
    assert "artifact_trace_id" not in entry


def test_lambda_family_aggregates_lmf_impls():
    """A lambda family entry should expose lambda_impls listing distinct impl methods
    from all member +0x instances."""
    crcs  = ["aaa11111", "bbb22222"]
    addrs = ["0x0001", "0x0002"]
    trace_ids = [10, 11]
    records = []
    for crc, addr in zip(crcs, addrs):
        records.append({
            "record": "bytecode_artifact",
            "class": "com/example/App$$Lambda",
            "loader_id": "0xaabb",
            "crc": crc,
            "kind": "original",
            "hidden": True,
        })
        records.append({
            "record": "hidden_class_identity",
            "runtime_name": f"com/example/App$$Lambda+{addr}",
            "artifact_crc": crc,
            "loader_id": "0xaabb",
        })
    for trace_id, crc, method in zip(trace_ids, crcs, ["lambda$0", "lambda$1"]):
        records.append({
            "record": "callsite_target",
            "category": "invokedynamic",
            "trace_id": trace_id,
            "source_class": "com/example/App",
            "source_method": "run",
            "source_bci": 10 * trace_id,
            "evidence": "LINKAGE_GUARANTEED",
            "indy_name": "run",
            "indy_descriptor": "()Ljava/lang/Runnable;",
            "lmf_impl_class": "com/example/App",
            "lmf_impl_method": method,
            "lmf_impl_descriptor": "()V",
            "staticizable": True,
            "reconstructable": False,
        })
    sidecar_map = {
        "aaa11111": {"trace_id": 10, "generated_by": "LambdaMetafactory", "class_name": "com/example/App$$Lambda"},
        "bbb22222": {"trace_id": 11, "generated_by": "LambdaMetafactory", "class_name": "com/example/App$$Lambda"},
    }
    idx = _idx_with_sidecars(records, sidecar_map)

    fam = idx["classes_by_name"].get("com/example/App$$Lambda")
    assert fam is not None
    assert fam.get("is_lambda_family") is True
    impls = {i["lmf_impl_method"] for i in fam.get("lambda_impls", [])}
    assert impls == {"lambda$0", "lambda$1"}


# ---------------------------------------------------------------------------
# Proxy detection and handler linkage
# ---------------------------------------------------------------------------

def test_proxy_class_is_detected():
    """A class whose simple name starts with $Proxy and is created via runtime_target
    should be marked is_proxy_class=True."""
    records = [
        {
            "record": "runtime_target",
            "dispatch_kind": "reflection",
            "evidence": "LINKAGE_GUARANTEED",
            "source_class": "com/example/App",
            "source_method": "run",
            "source_bci": 50,
            "target_class": "jdk/proxy1/$Proxy0",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_hidden": False,
        },
    ]
    idx = _idx(records)
    proxy = idx["classes_by_name"].get("jdk/proxy1/$Proxy0")
    assert proxy is not None
    assert proxy.get("is_proxy_class") is True


def test_proxy_handler_linked_to_preceding_lambda():
    """When a lambda-class <init> is observed just before a proxy <init> in the same
    source method, the proxy entry should expose proxy_handler pointing to that lambda."""
    records = [
        # BCI=10: handler lambda created
        {
            "record": "runtime_target",
            "dispatch_kind": "methodhandle_linkage",
            "evidence": "LINKAGE_GUARANTEED",
            "source_class": "com/example/App",
            "source_method": "run",
            "source_bci": 10,
            "target_class": "com/example/App$$Lambda+0x00001234",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_hidden": False,
        },
        # BCI=30: proxy created with that handler
        {
            "record": "runtime_target",
            "dispatch_kind": "reflection",
            "evidence": "LINKAGE_GUARANTEED",
            "source_class": "com/example/App",
            "source_method": "run",
            "source_bci": 30,
            "target_class": "jdk/proxy1/$Proxy0",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_hidden": False,
        },
        # identity record for the handler lambda
        {
            "record": "hidden_class_identity",
            "runtime_name": "com/example/App$$Lambda+0x00001234",
            "artifact_crc": "deadbeef",
            "loader_id": "0xaabb",
        },
    ]
    idx = _idx(records)
    proxy = idx["classes_by_name"].get("jdk/proxy1/$Proxy0")
    assert proxy is not None
    assert proxy.get("is_proxy_class") is True
    assert proxy.get("proxy_handler") == "com/example/App$$Lambda+0x00001234"

    handler = idx["classes_by_name"].get("com/example/App$$Lambda+0x00001234")
    assert handler is not None
    assert "jdk/proxy1/$Proxy0" in handler.get("proxy_for", [])


def test_non_proxy_builder_lambda_not_marked_as_proxy():
    """Proxy$ProxyBuilder$$Lambda classes contain '$Proxy' but their simple name
    does NOT start with '$Proxy', so they must NOT be flagged is_proxy_class."""
    records = [
        {
            "record": "runtime_target",
            "dispatch_kind": "methodhandle_linkage",
            "evidence": "LINKAGE_GUARANTEED",
            "source_class": "java/lang/reflect/Proxy",
            "source_method": "newProxyInstance",
            "source_bci": 5,
            "target_class": "java/lang/reflect/Proxy$ProxyBuilder$$Lambda+0x0000aabb",
            "target_method": "<init>",
            "target_descriptor": "()V",
            "target_hidden": False,
        },
    ]
    idx = _idx(records)
    cls = idx["classes_by_name"].get("java/lang/reflect/Proxy$ProxyBuilder$$Lambda+0x0000aabb")
    # If entry exists (via hidden_class_identity it might not), it must NOT be is_proxy_class
    if cls:
        assert not cls.get("is_proxy_class", False)
