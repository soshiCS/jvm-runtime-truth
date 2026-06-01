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
